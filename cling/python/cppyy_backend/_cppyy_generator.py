#!/usr/bin/env python

"""Cppyy binding generator."""
from __future__ import print_function
import argparse
import gettext
import inspect
import json
import logging
import os
import re
import sys
import traceback

from clang.cindex import AccessSpecifier, Config, CursorKind, Diagnostic, Index, SourceRange, TokenKind, TypeKind


class HelpFormatter(argparse.ArgumentDefaultsHelpFormatter, argparse.RawDescriptionHelpFormatter):
    pass


logger = logging.getLogger(__name__)
gettext.install(__name__)

# Keep PyCharm happy.
_ = _

EXPR_KINDS = [
    CursorKind.UNEXPOSED_EXPR,
    CursorKind.CONDITIONAL_OPERATOR, CursorKind.UNARY_OPERATOR, CursorKind.BINARY_OPERATOR,
    CursorKind.INTEGER_LITERAL, CursorKind.FLOATING_LITERAL, CursorKind.STRING_LITERAL,
    CursorKind.CXX_BOOL_LITERAL_EXPR, CursorKind.CXX_STATIC_CAST_EXPR, CursorKind.DECL_REF_EXPR
]
TEMPLATE_KINDS = [
                     CursorKind.TYPE_REF, CursorKind.TEMPLATE_REF, CursorKind.NAMESPACE_REF
                 ] + EXPR_KINDS
VARIABLE_KINDS = [CursorKind.VAR_DECL, CursorKind.FIELD_DECL]
FN_KINDS = [CursorKind.CXX_METHOD, CursorKind.FUNCTION_DECL, CursorKind.FUNCTION_TEMPLATE,
            CursorKind.CONSTRUCTOR, CursorKind.DESTRUCTOR, CursorKind.CONVERSION_FUNCTION]
#
# All Qt-specific logic is driven from these identifiers. Setting them to
# nonsense values would effectively disable all Qt-specific logic.
#
Q_SIGNALS = "Q_SIGNALS"


class SourceProcessor(object):
    """
    Centralise all processing of the source.
    Ideally, we'd use Clang for everything, but on occasion, we'll need access
    to the source, without pre-processing.
    """
    def __init__(self, compile_flags, verbose):
        super(SourceProcessor, self).__init__()
        self.compile_flags = compile_flags + ["-x", "c++"]
        self.verbose = verbose
        self.unpreprocessed_source = []
        self.source = None

    def compile(self, source):
        """
        Use Clang to parse the source and return its AST.
        :param source:              The source file.
        """
        if source != self.source:
            self.unpreprocessed_source = []
            self.source = source
        if self.verbose:
            logger.info(" ".join(self.compile_flags + [self.source]))
        tu = Index.create().parse(self.source, self.compile_flags)
        #
        # Stash ourselves on the tu for later use.
        #
        tu.source_processor = self
        return tu

    def unpreprocessed(self, extent, nl=" "):
        """
        Read the given range from the raw source.
        :param extent:              The range of text required.
        """
        assert self.source, "Must call compile() first!"
        if not self.unpreprocessed_source:
            self.unpreprocessed_source = self._read(self.source)
        text = self._extract(self.unpreprocessed_source, extent)
        if nl != "\n":
            text = text.replace("\n", nl)
        return text

    def _read(self, source):
        lines = []
        with open(source, "rU") as f:
            for line in f:
                lines.append(line)
        return lines

    def _extract(self, lines, extent):
        extract = lines[extent.start.line - 1:extent.end.line]
        if extent.start.line == extent.end.line:
            extract[0] = extract[0][extent.start.column - 1:extent.end.column - 1]
        else:
            extract[0] = extract[0][extent.start.column - 1:]
            extract[-1] = extract[-1][:extent.end.column - 1]
        #
        # Return a single buffer of text.
        #
        return "".join(extract)


class Info(dict):
    def __init__(self, kind, cursor):
        super(Info, self).__init__()
        logger.debug(_("Processing object {}").format(item_describe(cursor)))
        self["kind"] = kind
        self["name"] = cursor.spelling
        location = cursor.extent.start
        self["row:col"] = "{}:{}".format(location.line, location.column)
        if cursor.brief_comment:
            self["brief_comment"] = cursor.brief_comment
        if cursor.raw_comment:
            self["raw_comment"] = cursor.raw_comment


def cursor_parents(cursor):
    """
    A helper function which returns the parents of a cursor in the forms:
        - A::B::C::...N for non-top level entities.
        - filename.h    for top level entities.
        - ""            in exceptional cases of having no parents.
    """
    parents = ""
    parent = cursor.semantic_parent
    while parent and parent.kind != CursorKind.TRANSLATION_UNIT:
        parents = parent.spelling + "::" + parents
        parent = parent.semantic_parent
    if parent and not parents:
        return os.path.basename(parent.spelling)
    return parents[:-2]


def item_describe(item, alternate_spelling=None):
    """
    A helper function providing a standardised description for an item,
    which may be a cursor.
    """
    if isinstance(item, str):
        return item
    if alternate_spelling is None:
        text = item.spelling
    else:
        text = alternate_spelling
    return "{} on line {} '{}::{}'".format(item.kind.name, item.extent.start.line, cursor_parents(item), text)


def parameters_fixup(level, sip, key):
    """
    Clang seems to replace template parameter N of the form "T" with
    "type-parameter-<depth>-N"...so we need to put "T" back.
    :param sip:                 The sip.
    :param key:                 The key in the sip which may need
                                fixing up.
    :return:
    """
    for parent in reversed(level):
        if parent.kind in [CursorKind.CLASS_TEMPLATE, CursorKind.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION]:
            for clang_parameter, real_parameter in enumerate(parent.template_params):
                clang_parameter = "type-parameter-{}-{}".format(parent.template_level, clang_parameter)
                real_parameter = real_parameter["name"]
                #
                # Depending on the type of the SIP entry, replace the Clang
                # version of the value with the actual version.
                #
                value = sip[key]
                if isinstance(value, str):
                    sip[key] = value.replace(clang_parameter, real_parameter)
                elif isinstance(value, list):
                    for j, item in enumerate(value):
                        sip[key][j] = item.replace(clang_parameter, real_parameter)
                elif isinstance(value, dict):
                    for j, item in value.items():
                        sip[key][j] = item.replace(clang_parameter, real_parameter)


class CppyyGenerator(object):
    def __init__(self, compile_flags, dump_modules=False, dump_items=False, dump_includes=False,
                 dump_privates=False, verbose=False):
        """
        Constructor.

        :param compile_flags:       The compile flags for the file.
        :param dump_modules:        Turn on tracing for modules.
        :param dump_items:          Turn on tracing for container members.
        :param dump_includes:       Turn on diagnostics for include files.
        :param dump_privates:       Turn on diagnostics for omitted private items.
        :param verbose:             Turn on diagnostics for command lines.
        """
        self.dump_modules = dump_modules
        self.dump_items = dump_items
        self.dump_includes = dump_includes
        self.dump_privates = dump_privates
        self.verbose = verbose
        self.diagnostics = set()
        self.tu = None
        self.source_processor = SourceProcessor(compile_flags, verbose)

    def create_mapping(self, h_files):
        info = []
        for h_file in h_files:
            logger.debug(_("Processing {}").format(h_file))
            file_info = self.create_file_mapping(h_file)
            info.append(file_info)
        return info

    def create_file_mapping(self, h_file):
        """
        Actually convert the given source header file into its SIP equivalent.
        This is the main entry point for this class.

        :param h_file:              The source header file of interest.
        :returns: A (body, modulecode, includes). The body is the SIP text
                corresponding to the h_file, it can be a null string indicating
                there was nothing that could be generated. The modulecode is
                a dictionary of fragments of code that are to be emitted at
                module scope. The includes is a iterator over the files which
                clang #included while processing h_file.
        """
        #
        # Use Clang to parse the source and return its AST.
        #
        self.tu = self.source_processor.compile(h_file)
        m = (logging.ERROR - logging.WARNING)/(Diagnostic.Error - Diagnostic.Warning)
        c = logging.ERROR - (Diagnostic.Error * m)
        for diag in self.tu.diagnostics:
            #
            # We expect to be run over hundreds of files. Any parsing issues are likely to be very repetitive.
            # So, to avoid bothering the user, we suppress duplicates.
            #
            loc = diag.location
            msg = "{}:{}[{}] {}".format(loc.file, loc.line, loc.column, diag.spelling)
            if diag.spelling == "#pragma once in main file":
                continue
            if msg in self.diagnostics:
                continue
            self.diagnostics.add(msg)
            logger.log(m * diag.severity + c, "While parsing: {}".format(msg))
        if self.dump_includes:
            logger.info(_("File {}").format(h_file))
            for include in sorted(set(self.tu.get_includes())):
                logger.info(_("    #includes {}").format(include.include.name))
        #
        # Run through the top level children in the translation unit.
        #
        info = self._container_get(self.tu.cursor, [], h_file)
        return info

    def _container_get(self, container, level, h_file):
        """
        Generate the (recursive) translation for a class or namespace.

        :param container:           A class or namespace.
        :param level:               Recursion level controls indentation.
        :param h_file:              The source header file of interest.
        :return:                    Info().
        """
        def mark_forward_kinds(kind, definition):
            has_body = False
            #
            # Could this be a forward declaration?
            #
            for token in definition.get_tokens():
                if token.kind == TokenKind.PUNCTUATION and token.spelling == "{":
                    has_body = True
            if not has_body:
                kind = "forward " + kind
            return kind

        if container.kind == CursorKind.CLASS_DECL:
            kind = mark_forward_kinds("class", container)
        elif container.kind in [CursorKind.CLASS_TEMPLATE, CursorKind.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION]:
            kind = mark_forward_kinds("template class", container)
            #
            # What level of template parameter is on this container?
            #
            container.template_level = 0
            for parent in reversed(level):
                if parent.kind in [CursorKind.CLASS_TEMPLATE, CursorKind.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION]:
                    container.template_level = parent.template_level + 1
                    break
            container.template_params = []
        elif container.kind == CursorKind.STRUCT_DECL:
            kind = mark_forward_kinds("struct", container)
        elif container.kind == CursorKind.UNION_DECL:
            kind = mark_forward_kinds("union", container)
        elif container.kind == CursorKind.NAMESPACE:
            kind = "namespace"
        elif container.kind == CursorKind.TRANSLATION_UNIT:
            kind = "file"
        else:
            logger.error(_("Unknown container kind {}").format(container.kind))
            kind = container.kind
        info = Info(kind, container)
        children = []
        info["children"] = children
        template_count_stack = []
        template_info_stack = []
        template_stack_index = -1
        is_signal = False
        for child in container.get_children():
            #
            # Only emit items in the translation unit.
            #
            if child.location.file.name != self.tu.spelling:
                continue
            if child.access_specifier == AccessSpecifier.PRIVATE:
                continue
            if child.kind in FN_KINDS:
                child_info = self._fn_get(container, child, level + [container], is_signal)
                children.append(child_info)
            elif child.kind == CursorKind.ENUM_DECL:
                child_info = self._enum_get(container, child, level.append(container))
                children.append(child_info)
            elif child.kind == CursorKind.CXX_ACCESS_SPEC_DECL:
                is_signal = self._get_access_specifier(child, level.append(container))
            elif child.kind == CursorKind.TYPEDEF_DECL:
                child_info = self.typedef_decl(container, child, level + [container], h_file)
                #
                # Structs and unions are emitted twice:
                #
                #   - first as just the bare fields
                #   - then as children of the typedef
                #
                if "type" in child_info and child_info["type"]["kind"] in ("struct", "union"):
                    assert children[-1] == child_info["type"]
                    children.pop()
                children.append(child_info)
            elif child.kind in [CursorKind.NAMESPACE, CursorKind.CLASS_DECL,
                                 CursorKind.CLASS_TEMPLATE, CursorKind.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION,
                                 CursorKind.STRUCT_DECL, CursorKind.UNION_DECL]:
                child_info = self._container_get(child, level + [container], h_file)
                children.append(child_info)
            elif child.kind in [CursorKind.FIELD_DECL]:
                child_info = self._var_get("field", child, level + [container], h_file)
                children.append(child_info)
            elif child.kind in [CursorKind.VAR_DECL]:
                child_info = self._var_get("variable", child, level + [container], h_file)
                children.append(child_info)
            elif child.kind in [CursorKind.TEMPLATE_TYPE_PARAMETER, CursorKind.TEMPLATE_NON_TYPE_PARAMETER,
                                CursorKind.TEMPLATE_TEMPLATE_PARAMETER]:
                info["parameters"] = container.template_params
                param_info = Info("parameter", child)
                container.template_params.append(param_info)
            elif child.kind == CursorKind.TEMPLATE_REF:
                #
                # Create an entry to collect information for this level of template arguments.
                #
                tmp = Config().lib.clang_Type_getNumTemplateArguments(container.type)
                template_count_stack.append(tmp)
                template_info = Info("template", child)
                template_info["parameters"] = []
                if template_stack_index == -1:
                    template_info_stack.append(template_info)
                    info["type"] = template_info
                else:
                    #
                    # Non-first template_infos are just parameters.
                    #
                    template_info_stack[template_stack_index]["parameters"].append(template_info)
                template_stack_index += 1
            elif child.kind == CursorKind.TYPE_REF:
                if template_stack_index > -1:
                    #
                    # This is a template parameter.
                    #
                    template_parameters = template_info_stack[0]["parameters"]
                    for i in range(template_stack_index):
                        template_parameters = template_parameters[-1]["parameters"]
                    template_parameters.append(child.spelling)
                    template_count_stack[template_stack_index] -= 1
                    if template_count_stack[template_stack_index] == 0:
                        template_stack_index -= 1
                else:
                    #
                    # Not a template.
                    #
                    child_info = Info("type", child)
                    info["type"] = child_info
            else:
                CppyyGenerator._report_ignoring(child, "unusable")
            if self.dump_items:
                logger.info(_("Processing {}").format(item_describe(child)))
        return info

    def _get_access_specifier(self, member, level):
        """
        In principle, we just want member.access_specifier.name.lower(), except that we need to handle:

          Q_SIGNALS:|signals:

        which are converted by the preprocessor...so read the original text.

        :param member:                  The access_specifier.
        :return:
        """
        access_specifier_text = self.source_processor.unpreprocessed(member.extent)
        if access_specifier_text in (Q_SIGNALS + ":", "signals:"):
            return True
        return False

    def _enum_get(self, container, enum, level):
        info = Info("enum", enum)
        enumerations = []
        info["enumerations"] = enumerations
        for enumeration in enum.get_children():
            #
            # Skip visibility attributes and the like.
            #
            child_info = {}
            if enumeration.kind == CursorKind.ENUM_CONSTANT_DECL:
                child_info["name"] = enumeration.spelling
                child_info["value"] = enumeration.enum_value
                enumerations.append(child_info)
            else:
                CppyyGenerator._report_ignoring(enumeration, "unusable")
        return info

    def _fn_get(self, container, fn, level, is_signal):
        """
        Generate the translation for a function.

        :param container:           A class or namespace.
        :param fn:                  The function object.
        :param level:               Recursion level controls indentation.
        :param is_signal:           Is this a Qt signal?
        :return:                    A string.
        """
        if fn.spelling.startswith("operator"):
            pass
        info = Info("function", fn)
        parameters_fixup(level, info, "name")
        if fn.kind not in [CursorKind.CONSTRUCTOR, CursorKind.DESTRUCTOR]:
            info["type"] = fn.result_type.spelling
            parameters_fixup(level, info, "type")
        parameters = []
        info["parameters"] = parameters
        for child in fn.get_children():
            if child.kind == CursorKind.PARM_DECL:
                #
                # So far so good, but we need any default value.
                #
                child_info = self._fn_get_parameter(fn, child)
                parameters_fixup(level, child_info, "type")
                parameters.append(child_info)
            elif child.kind in [CursorKind.COMPOUND_STMT, CursorKind.CXX_OVERRIDE_ATTR,
                                CursorKind.MEMBER_REF, CursorKind.DECL_REF_EXPR, CursorKind.CALL_EXPR,
                                CursorKind.UNEXPOSED_ATTR, CursorKind.VISIBILITY_ATTR] + TEMPLATE_KINDS:
                #
                # Ignore:
                #
                #   CursorKind.COMPOUND_STMT: Function body.
                #   CursorKind.CXX_OVERRIDE_ATTR: The "override" keyword.
                #   CursorKind.MEMBER_REF, CursorKind.DECL_REF_EXPR, CursorKind.CALL_EXPR: Constructor initialisers.
                #   TEMPLATE_KINDS: The result type.
                #
                pass
            else:
                CppyyGenerator._report_ignoring(child, "unusable")
        return info

    QUALIFIED_ID = re.compile("(?:[a-z_][a-z_0-9]*::)*([a-z_][a-z_0-9]*)", re.I)

    def _fn_get_parameter(self, function, parameter):
        """
        The parser does not seem to provide access to the complete text of a parameter.
        This makes it hard to find any default values, so we:

            1. Run the lexer from "here" to the end of the file, bailing out when we see the ","
            or a ")" marking the end.
            2. Watch for the assignment.
        """
        def decompose_arg(arg, spellings):
            template, args = utils.decompose_template(arg)
            spellings.append(template)
            if args is not None:
                for arg in args:
                    decompose_arg(arg, spellings)

        def _get_param_type(parameter):
            q_flags_enum = None
            canonical = underlying = parameter.type
            spellings = []
            while underlying:
                prefixes, name, operators, suffixes, next = underlying.decomposition()
                name, args = utils.decompose_template(name)
                #
                # We want the name (or name part of the template), plus any template parameters to deal with:
                #
                #  QList<KDGantt::Constraint> &constraints = QList<Constraint>()
                #
                spellings.append(name)
                if args is not None:
                    for arg in args:
                        decompose_arg(arg, spellings)
                    if name == QFLAGS:
                        #
                        # The name of the enum.
                        #
                        q_flags_enum = args[0]
                        return spellings, q_flags_enum, underlying
                canonical = underlying
                underlying = next
            return spellings, q_flags_enum, canonical.get_canonical()

        def _get_param_value(text, parameter):

            def mangler_enum(spellings, rhs, fqn):
                #
                # Is rhs the suffix of any of the typedefs?
                #
                for spelling in spellings[:-1]:
                    name, args = utils.decompose_template(spelling)
                    if name.endswith(rhs):
                        return name
                prefix = spellings[-1].rsplit("::", 1)[0] + "::"
                return prefix + rhs

            def mangler_other(spellings, rhs, fqn):
                #
                # Is rhs the suffix of any of the typedefs?
                #
                for spelling in spellings:
                    name, args = utils.decompose_template(spelling)
                    if name.endswith(rhs):
                        return name
                return fqn

            if text in ["", "0", "nullptr", Q_NULLPTR]:
                return text
            spellings, q_flags_enum, canonical_t = _get_param_type(parameter)
            if text == "{}":
                if q_flags_enum or canonical_t.kind == TypeKind.ENUM:
                    return "0"
                if canonical_t.kind == TypeKind.POINTER:
                    return "nullptr"
                #
                # TODO: return the lowest or highest type?
                #
                return spellings[-1] + "()"
            #
            # SIP wants things fully qualified. Without creating a full AST, we can only hope to cover the common
            # cases:
            #
            #   - Enums may come as a single value or an or'd list:
            #
            #       Input                       Output
            #       -----                       ------
            #       Option1                     parents::Option1
            #       Flag1|Flag3                 parents::Flag1|parents::Flag3
            #       FlagsType(Flag1|Flag3)      parents::FlagsType(parents::Flag1|parents::Flag3)
            #       LookUpMode(exactOnly) | defaultOnly
            #                                   parents::LookUpMode(parents::exactOnly) | parents::defaultOnly
            #
            #     So, prefix any identifier with the prefix of the enum.
            #
            #   - For other cases, if any (qualified) id in the default value matches the RHS of the parameter
            #     type, use the parameter type.
            #
            if q_flags_enum or canonical_t.kind == TypeKind.ENUM:
                mangler = mangler_enum
            else:
                mangler = mangler_other
            tmp = ""
            match = CppyyGenerator.QUALIFIED_ID.search(text)
            while match:
                tmp += match.string[:match.start()]
                rhs = match.group(1)
                fqn = match.group()
                tmp += mangler(spellings, rhs, fqn)
                text = text[match.end():]
                match = CppyyGenerator.QUALIFIED_ID.search(text)
            tmp += text
            return tmp

        info = Info("parameter", parameter)
        info["type"] = parameter.type.spelling
        for member in parameter.get_children():
            if member.kind.is_expression():
                #
                # Get the text after the "=". Macro expansion can make relying on tokens fraught...and
                # member.get_tokens() simply does not always return anything.
                #
                possible_extent = SourceRange.from_locations(parameter.extent.start, function.extent.end)
                text = ""
                bracket_level = 0
                found_start = False
                found_end = False
                was_punctuated = True
                for token in self.tu.get_tokens(extent=possible_extent):
                    #
                    # Now count balanced anything-which-can-contain-a-comma till we get to the end.
                    #
                    if bracket_level == 0 and token.spelling == "=" and not found_start:
                        found_start = True
                    elif bracket_level == 0 and token.spelling in ",)":
                        found_end = True
                        text = text[1:]
                        break
                    elif token.spelling in "<(":
                        bracket_level += 1
                    elif token.spelling in ")>":
                        bracket_level -= 1
                    if found_start:
                        if (token.kind != TokenKind.PUNCTUATION and not was_punctuated) or (token.spelling in "*&"):
                            text += " "
                        text += token.spelling
                        was_punctuated = token.kind == TokenKind.PUNCTUATION
                if not found_end and text:
                    raise RuntimeError(_("No end found for {}::{}, '{}'").format(function.spelling, parameter.spelling,
                                                                                 text))
                info["default"] = text
        return info

    def typedef_decl(self, container, typedef, level, h_file):
        """
        Generate the translation for a typedef.

        :param container:           A class or namespace.
        :param typedef:             The typedef object.
        :param level:               Recursion level controls indentation.
        :param h_file:              The source header file of interest.
        :return:                    Info().
        """
        info = self._type_get("typedef", typedef, level, h_file)
        return info

    def _type_get(self, tag, parent, level, h_file):
        """
        Generate the translation for a type.

        :param tag:                 "typedef", "variable" etc.
        :param parent:              The typed object.
        :param level:               Recursion level controls indentation.
        :param h_file:              The source header file of interest.
        :return:                    Info().
        """
        info = Info(tag, parent)
        template_count_stack = []
        template_info_stack = []
        template_stack_index = -1
        parameters = None
        for child in parent.get_children():
            if child.kind in [CursorKind.STRUCT_DECL, CursorKind.UNION_DECL]:
                child_info = self._container_get(child, level, h_file)
                info["type"] = child_info
            elif child.kind == CursorKind.TEMPLATE_REF:
                #
                # Create an entry to collect information for this level of template arguments.
                #
                tmp = Config().lib.clang_Type_getNumTemplateArguments(parent.type)
                if tmp == -1:
                    logger.error(_("Unexpected template_arg_count={}").format(tmp))
                    #
                    # Just a WAG...
                    #
                    tmp = 1
                template_count_stack.append(tmp)
                template_info = Info("template", child)
                template_info["parameters"] = []
                if template_stack_index == -1:
                    template_info_stack.append(template_info)
                    info["type"] = template_info
                else:
                    #
                    # Non-first template_infos are just parameters.
                    #
                    template_info_stack[template_stack_index]["parameters"].append(template_info)
                template_stack_index += 1
            elif child.kind == CursorKind.TYPE_REF:
                if template_stack_index > -1:
                    #
                    # This is a template parameter.
                    #
                    template_parameters = template_info_stack[0]["parameters"]
                    for i in range(template_stack_index):
                        template_parameters = template_parameters[-1]["parameters"]
                    template_parameters.append(child.spelling)
                    template_count_stack[template_stack_index] -= 1
                    if template_count_stack[template_stack_index] == 0:
                        template_stack_index -= 1
                else:
                    #
                    # Not a template.
                    #
                    child_info = Info("type", child)
                    info["type"] = child_info
            elif child.kind == CursorKind.PARM_DECL:
                #
                # This must be a function type.
                #
                if parameters is None:
                    child_info = Info("function", parent)
                    info["type"] = child_info
                    child_info["type"] = parent.underlying_typedef_type.spelling
                    parameters = []
                    child_info["parameters"] = parameters
                child_info = self._fn_get_parameter(parent, child)
                parameters.append(child_info)
            else:
                CppyyGenerator._report_ignoring(child, "unusable")
        return info

    def _var_get(self, tag, parent, level, h_file):
        """
        Generate the translation for a type.

        :param tag:                 "typedef", "variable" etc.
        :param parent:              The typed object.
        :param level:               Recursion level controls indentation.
        :param h_file:              The source header file of interest.
        :return:                    Info().
        """
        info = Info(tag, parent)
        for child in parent.get_children():
            if child.kind == CursorKind.TYPE_REF:
                info["type"] = child.spelling
                parameters_fixup(level, info, "type")
            else:
                CppyyGenerator._report_ignoring(child, "unusable")
        return info

    @staticmethod
    def _report_ignoring(child, reason):
        logger.debug(_("Ignoring {} {}").format(reason, item_describe(child)))


def main(argv=None):
    """
    Take set of C++ header files and generate the corresponding SIP file.
    Beyond simple generation of the SIP file from the corresponding C++
    header file, a set of rules can be used to customise the generated
    SIP file.

    Examples:

        INC=/usr/include
        QT5=$INC/x86_64-linux-gnu/qt5
        KF5=$INC/KF5
        FLAGS="\\\\-I$QT5;\\\\-I$QT5/QtCore;\\\\-I$KF5/kjs;\\\\-I$KF5/wtf"
        cppyy-generator --flags="$FLAGS" /usr/lib/x86_64-linux-gnu/libclang-3.9.so PyKF5 $KF5/kjs/kjsinterpreter.h tmp.sip
    """
    if argv is None:
        argv = sys.argv
    parser = argparse.ArgumentParser(epilog=inspect.getdoc(main),
                                     formatter_class=HelpFormatter)
    parser.add_argument("-v", "--verbose", action="store_true", default=False, help=_("Enable verbose output"))
    parser.add_argument("--flags", default="",
                        help=_("Semicolon-separated C++ compile flags to use, escape leading - or -- with \\"))
    parser.add_argument("--libclang", help=_("libclang library to use for parsing"))
    parser.add_argument("output", help=_("Output filename to write"))
    parser.add_argument("sources", nargs="+", help=_("Semicolon-separated C++ headers to process"))
    try:
        args = parser.parse_args(argv[1:])
        if args.verbose:
            logging.basicConfig(level=logging.DEBUG, format='%(asctime)s %(name)s %(levelname)s: %(message)s')
        else:
            logging.basicConfig(level=logging.INFO, format='%(levelname)s: %(message)s')
        flags = []
        for f in args.flags.lstrip().split(";"):
            if f.startswith("\\-\\-"):
                flags.append("--" + f[4:])
            elif f.startswith("\\-"):
                flags.append("-" + f[2:])
            elif f:
                flags.append(f)
        #
        # Load the given libclang.
        #
        if args.libclang:
            Config.set_library_file(args.libclang)
        lib = Config().lib
        import ctypes
        from clang.cindex import Type
        items = [
            ("clang_Type_getNumTemplateArguments", [Type], ctypes.c_size_t),
        ]
        for item in items:
            func = getattr(lib, item[0])
            if len(item) >= 2:
                func.argtypes = item[1]

            if len(item) >= 3:
                func.restype = item[2]

            if len(item) == 4:
                func.errcheck = item[3]
        #
        # Generate!
        #
        g = CppyyGenerator(flags, verbose=args.verbose)
        mapping = g.create_mapping(args.sources)
        with open(args.output, "w") as f:
            json.dump(mapping, f, indent=4, sort_keys=True)
        return 0
    except Exception as e:
        tbk = traceback.format_exc()
        print(tbk)
        return 1


if __name__ == "__main__":
    sys.exit(main())
