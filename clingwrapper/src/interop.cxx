#ifndef WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
// silence warnings about getenv, strncpy, etc.
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

// Bindings
#include "capi.h"
#include "cpp_cppyy.h"
#include "callcontext.h"

// Cling
#include "cling/Utils/AST.h"
#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/LookupHelper.h"

// Clang
#include "clang/AST/DeclBase.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/Lookup.h"

// LLVM
#include "llvm/Support/Casting.h"

#include <dlfcn.h>

// Standard
#include <assert.h>
#include <algorithm>     // for std::count, std::remove
#include <stdexcept>
#include <map>
#include <new>
#include <regex>
#include <set>
#include <sstream>
#include <signal.h>
#include <stdlib.h>      // for getenv
#include <string.h>
#include <typeinfo>
#include <iostream>


// temp
#include <iostream>
// --temp

static std::set<std::string> gInitialNames;

static inline
bool match_name(const std::string& tname, const std::string fname)
{
// either match exactly, or match the name as template
    if (fname.rfind(tname, 0) == 0) {
        if ((tname.size() == fname.size()) ||
              (tname.size() < fname.size() && fname[tname.size()] == '<'))
           return true;
    }
    return false;
}

bool Cppyy::NewIsNamespace(TCppScope_t handle)
{
    auto *D = (clang::Decl *)handle;
    return llvm::isa_and_nonnull<clang::NamespaceDecl>(D);
}

bool Cppyy::NewIsTemplate(TCppScope_t handle)
{
    auto *D = (clang::Decl *)handle;
    return llvm::isa_and_nonnull<clang::TemplateDecl>(D);
}

bool Cppyy::NewIsAbstract(TCppType_t klass)
{
// Test if this type may not be instantiated.
    auto *D = (clang::Decl *)klass;
    if (auto *CXXRD = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(D))
        CXXRD->isAbstract();

    return false;
}

bool Cppyy::NewIsEnum(TCppScope_t handle)
{
    auto *D = (clang::Decl *)handle;
    return llvm::isa_and_nonnull<clang::EnumDecl>(D);
}

bool Cppyy::NewIsAggregate(TCppType_t type)
{
    if (type == 0) return false;
    // XXX: What todo here?
    return false;
}

bool Cppyy::NewIsVariable(TCppScope_t scope)
{
    auto *D = (clang::Decl *)scope;
    return llvm::isa_and_nonnull<clang::VarDecl>(D);
}

// helpers for stripping scope names
static
std::string outer_with_template(const std::string& name)
{
// Cut down to the outer-most scope from <name>, taking proper care of templates.
    int tpl_open = 0;
    for (std::string::size_type pos = 0; pos < name.size(); ++pos) {
        std::string::value_type c = name[pos];

    // count '<' and '>' to be able to skip template contents
        if (c == '<')
            ++tpl_open;
        else if (c == '>')
            --tpl_open;

    // collect name up to "::"
        else if (tpl_open == 0 && \
                 c == ':' && pos+1 < name.size() && name[pos+1] == ':') {
        // found the extend of the scope ... done
            return name.substr(0, pos-1);
        }
    }

// whole name is apparently a single scope
    return name;
}

static
std::string outer_no_template(const std::string& name)
{
// Cut down to the outer-most scope from <name>, drop templates
    std::string::size_type first_scope = name.find(':');
    if (first_scope == std::string::npos)
        return name.substr(0, name.find('<'));
    std::string::size_type first_templ = name.find('<');
    if (first_templ == std::string::npos)
        return name.substr(0, first_scope);
    return name.substr(0, std::min(first_templ, first_scope));
}

#define FILL_COLL(type, filter) {                                             \
    TIter itr{coll};                                                          \
    type* obj = nullptr;                                                      \
    while ((obj = (type*)itr.Next())) {                                       \
        const char* nm = obj->GetName();                                      \
        if (nm && nm[0] != '_' && !(obj->Property() & (filter))) {            \
            if (gInitialNames.find(nm) == gInitialNames.end())                \
                cppnames.insert(nm);                                          \
    }}}

static inline
void cond_add(Cppyy::TCppScope_t scope, const std::string& ns_scope,
    std::set<std::string>& cppnames, const char* name, bool nofilter = false)
{
    if (!name || name[0] == '_' || strstr(name, ".h") != 0 || strncmp(name, "operator", 8) == 0)
        return;

    if (scope == Cppyy::NewGetGlobalScope()) {
        std::string to_add = outer_no_template(name);
        if (nofilter || gInitialNames.find(to_add) == gInitialNames.end())
            cppnames.insert(outer_no_template(name));
    } else if (scope == Cppyy::NewGetScope("std")) {
        if (strncmp(name, "std::", 5) == 0) {
            name += 5;
#ifdef __APPLE__
            if (strncmp(name, "__1::", 5) == 0) name += 5;
#endif
        }
        cppnames.insert(outer_no_template(name));
    } else {
        if (strncmp(name, ns_scope.c_str(), ns_scope.size()) == 0)
            cppnames.insert(outer_with_template(name + ns_scope.size()));
    }
}

std::vector<Cppyy::TCppScope_t> Cppyy::NewGetUsingNamespaces(TCppScope_t scope)
{
    auto *D = (clang::Decl *) scope;

    if (auto *DC = llvm::dyn_cast_or_null<clang::DeclContext>(D)) {
        std::vector<TCppScope_t> namespaces;
        for (auto UD : DC->using_directives()) {
            namespaces.push_back((TCppScope_t) UD->getNominatedNamespace());
        }
        return namespaces;
    }

    return {};
}

// class reflection information ----------------------------------------------
std::string Cppyy::NewGetFinalName(TCppType_t klass)
{
    if (klass == Cppyy::NewGetGlobalScope())
        return "";

    auto *D = (clang::NamedDecl *) klass;
    return D->getNameAsString();
}

std::string Cppyy::NewGetScopedFinalName(TCppType_t klass)
{
    if (klass == Cppyy::NewGetGlobalScope() || klass == 0)
        return "";

    auto *D = (clang::NamedDecl *) klass;
    return D->getQualifiedNameAsString();
}

Cppyy::TCppScope_t Cppyy::NewGetScope(const std::string &name, TCppScope_t parent)
{
    clang::DeclContext *Within = 0;
    if (parent) {
        auto *D = (clang::Decl *)parent;
        Within = llvm::dyn_cast<clang::DeclContext>(D);
    }

    cling::Interpreter::PushTransactionRAII RAII(cling::cppyy::gCling);
    auto *ND = cling::utils::Lookup::Named(&cling::cppyy::gCling->getCI()->getSema(), name, Within);
    if (!(ND == (clang::NamedDecl *) -1) && (llvm::isa_and_nonnull<clang::NamespaceDecl>(ND) || llvm::isa_and_nonnull<clang::RecordDecl>(ND)))
        return (TCppScope_t)(ND->getCanonicalDecl());

    return 0;
}

Cppyy::TCppScope_t Cppyy::NewGetFullScope(const std::string &name)
{
    std::string delim = "::";
    size_t start = 0;
    size_t end = name.find(delim);
    TCppScope_t curr_scope = 0;
    while (end != std::string::npos)
    {
        curr_scope = Cppyy::NewGetScope(name.substr(start, end - start), curr_scope);
        start = end + delim.length();
        end = name.find(delim, start);
    }
    return Cppyy::NewGetScope(name.substr(start, end), curr_scope);
}

Cppyy::TCppScope_t Cppyy::NewGetTypeScope(TCppScope_t klass) {
    auto *D = (clang::Decl *)klass;
    if (auto *VD = llvm::dyn_cast_or_null<clang::ValueDecl>(D)) {
        if (auto *Type = VD->getType().getTypePtrOrNull()) {
            Type = Type->getPointeeOrArrayElementType()->getUnqualifiedDesugaredType();
            return (TCppScope_t) Type->getAsCXXRecordDecl();
        }
    }
    return 0;
}


Cppyy::TCppScope_t Cppyy::NewGetNamed(const std::string &name, TCppScope_t parent)
{
    clang::DeclContext *Within = 0;
    std::string par = "";
    if (parent) {
        auto *D = (clang::Decl *)parent;
        Within = llvm::dyn_cast<clang::DeclContext>(D);
        par = Cppyy::NewGetScopedFinalName(parent);
    }

    cling::Interpreter::PushTransactionRAII RAII(cling::cppyy::gCling);
    auto *ND = cling::utils::Lookup::Named(&cling::cppyy::gCling->getCI()->getSema(), name, Within);
    if (ND && ND != (clang::NamedDecl*) -1) {
        printf("  Lookup: Found => %s : %s\n", par.c_str(), name.c_str());
        return (TCppScope_t)(ND->getCanonicalDecl());
    }

    printf("  Lookup: Not Found => %s : %s\n", par.c_str(), name.c_str());
    return 0;
}

Cppyy::TCppScope_t Cppyy::NewGetParentScope(TCppScope_t scope)
{
    if (scope == Cppyy::NewGetGlobalScope())
        return 0;

    auto *D = (clang::Decl *) scope;
    auto *ParentDC = D->getDeclContext()->getParent();

    if (!ParentDC)
        return 0;

    return (TCppScope_t) clang::Decl::castFromDeclContext(
            ParentDC)->getCanonicalDecl(); 
}

Cppyy::TCppScope_t Cppyy::NewGetScopeFromType(Cppyy::TCppType_t parent)
{
    auto *RD = ((clang::QualType *) parent)->getTypePtr()->getAsRecordDecl();
    if (RD)
        return (TCppScope_t) (RD->getCanonicalDecl());

    return 0;
}

Cppyy::TCppScope_t Cppyy::NewGetGlobalScope()
{
    return (TCppScope_t) cling::cppyy::gCling->getCI()->getSema().getASTContext().getTranslationUnitDecl();
}

Cppyy::TCppIndex_t Cppyy::NewGetNumBases(TCppType_t klass)
{
    if (!klass)
        return 0;

    auto *D = (clang::Decl *) klass;

    if (auto *CRD = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(D))
        return CRD->getNumBases();

    return 0;
}

Cppyy::TCppScope_t Cppyy::NewGetBaseScope(TCppType_t klass, TCppIndex_t ibase)
{
    auto *CXXRD = (clang::CXXRecordDecl *) klass;
    auto type = (CXXRD->bases_begin() + ibase)->getType();
    if (auto RT = llvm::dyn_cast<clang::RecordType>(type)) {
        return (TCppScope_t) RT->getDecl()->getCanonicalDecl();
    } else if (auto TST = llvm::dyn_cast<clang::TemplateSpecializationType>(type)) {
        return (TCppScope_t) TST->getTemplateName()
            .getAsTemplateDecl()->getCanonicalDecl();
    }

    return 0;
}

bool Cppyy::NewIsSubclass(TCppScope_t derived, TCppScope_t base)
{
    if (derived == base)
        return true;
    auto global_scope = NewGetGlobalScope();
    if (derived == global_scope || base == global_scope)
        return false;
    auto *derived_D = (clang::Decl *) derived;
    auto *base_D = (clang::Decl *) base;
    if (auto derived_CXXRD = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(derived_D))
        if (auto base_CXXRD = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(base_D))
            return derived_CXXRD->isDerivedFrom(base_CXXRD);
    return false;
}

std::vector<Cppyy::TCppMethod_t> Cppyy::NewGetClassMethods(TCppScope_t scope) {
    auto *D = (clang::Decl *) scope;

    if (auto *CXXRD = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(D)) {
        std::vector<TCppMethod_t> methods;
        for (auto it = CXXRD->method_begin(), end = CXXRD->method_end(); it != end; it++) {
            methods.push_back((TCppMethod_t) *it);
        }
        return methods;
    }
    return {};
}

std::vector<Cppyy::TCppScope_t> Cppyy::NewGetMethodsFromName(
        TCppScope_t scope, const std::string& name)
{
    auto *D = (clang::Decl *) scope;
    std::vector<TCppScope_t> methods;
    llvm::StringRef Name(name);
    clang::DeclarationName DName = &(&cling::cppyy::gCling->getCI()->getSema())->Context.Idents.get(name);
    clang::LookupResult R(cling::cppyy::gCling->getCI()->getSema(),
                          DName,
                          clang::SourceLocation(),
                          clang::Sema::LookupOrdinaryName,
                          clang::Sema::ForVisibleRedeclaration);

    cling::Interpreter::PushTransactionRAII RAII(cling::cppyy::gCling);
    cling::utils::Lookup::Named(&cling::cppyy::gCling->getCI()->getSema(), R,
            clang::Decl::castToDeclContext(D));

    if (R.empty())
        return methods;

    R.resolveKind();

    for (clang::LookupResult::iterator Res = R.begin(), ResEnd = R.end();
         Res != ResEnd;
         ++Res) {
        if (llvm::isa<clang::CXXMethodDecl>(*Res)) {
            methods.push_back((TCppScope_t) *Res);
        }
    }

    return methods;
}

std::string Cppyy::NewGetMethodName(TCppMethod_t method)
{
    auto *D = (clang::Decl *) method;
    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(D)) {
        return CXXMD->getNameAsString();
    }
    return "";
}

std::string Cppyy::NewGetMethodFullName(TCppMethod_t method)
{
    auto *D = (clang::Decl *) method;
    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(D)) {
        return CXXMD->getQualifiedNameAsString();
    }
    return "";
}

std::string Cppyy::NewGetMethodReturnTypeAsString(TCppMethod_t method)
{
    auto *D = (clang::Decl *) method;
    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(D)) {
        return CXXMD->getReturnType().getAsString();
    }
    return "";
}

Cppyy::TCppIndex_t Cppyy::NewGetMethodNumArgs(TCppMethod_t method)
{
    auto *D = (clang::Decl *) method;
    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(D)) {
        return CXXMD->getNumParams();
    }
    return 0;
}

Cppyy::TCppIndex_t Cppyy::NewGetMethodReqArgs(TCppMethod_t method)
{
    auto *D = (clang::Decl *) method;
    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl> (D)) {
        return CXXMD->getMinRequiredArguments();
    }
    return 0;
}

std::string Cppyy::NewGetMethodArgTypeAsString(TCppMethod_t method, TCppIndex_t iarg)
{
    auto *D = (clang::Decl *) method;

    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(D)) {
        if (iarg < CXXMD->getNumParams()) {
            auto *PVD = CXXMD->getParamDecl(iarg);
            return PVD->getOriginalType().getAsString();
        }
    }
    return "";
}

std::string Cppyy::NewGetMethodSignature(TCppMethod_t method, bool show_formalargs, TCppIndex_t maxargs)
{
    auto *D = (clang::Decl *) method;
    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(D)) {
        printf("%s\n", CXXMD->getFunctionType()->getTypeClassName());
    }
    return "<unknown>";
}

bool Cppyy::NewExistsMethodTemplate(TCppScope_t scope, const std::string& name)
{
    auto *D = (clang::Decl *) scope;
    if (llvm::isa_and_nonnull<clang::DeclContext>(D)) {
        auto *ND = cling::utils::Lookup::Named(&cling::cppyy::gCling->getCI()->getSema(), name,
                clang::Decl::castToDeclContext(D));
        if ((bool) ND)
            return true;
    }

    return false;
}

bool Cppyy::NewIsTemplatedMethod(TCppMethod_t method)
{
    auto *D = (clang::Decl *) method;
    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(D)) {
        auto TK = CXXMD->getTemplatedKind();
        return TK == clang::FunctionDecl::TemplatedKind::TK_FunctionTemplateSpecialization
            || TK == clang::FunctionDecl::TemplatedKind::TK_DependentFunctionTemplateSpecialization
            || TK == clang::FunctionDecl::TemplatedKind::TK_FunctionTemplate ;
    }

    return false;
}

inline
CallWrapper* new_CallWrapper(CallWrapper::DeclId_t fid, const std::string& n)
{
    CallWrapper* wrap = new CallWrapper(fid, n);
    gWrapperHolder.push_back(wrap);
    return wrap;
}

// method/function dispatching -----------------------------------------------
static cling::Interpreter::CallFuncIFacePtr_t GetCallFunc(Cppyy::TCppMethod_t method, bool as_iface)
{
// TODO: method should be a callfunc, so that no mapping would be needed.
    auto *D = (clang::Decl *) method;
    CallWrapper *wrap;
    if (auto *FD = llvm::dyn_cast_or_null<clang::FunctionDecl>(D)) {
        wrap = new_CallWrapper(FD, FD->getNameAsString());
    } else {
        printf("Function Decl invalid\n");
    }


    cling::CallFunc_t* callf = cling::cppyy::gCling->CallFunc_Factory();
    cling::cppyy::gCling->CallFunc_SetFunc(callf, (cling::Method_t *)wrap->fDecl);

    if (!(callf && cling::cppyy::gCling->CallFunc_IsValid(callf))) {
    // TODO: propagate this error to caller w/o use of Python C-API
    /*
        PyErr_Format(PyExc_RuntimeError, "could not resolve %s::%s(%s)",
            const_cast<TClassRef&>(klass).GetClassName(),
            wrap.fName, callString.c_str()); */
        std::cerr << "TODO: report unresolved function error to Python\n";
        if (callf) cling::cppyy::gCling->CallFunc_Delete(callf);
        return cling::Interpreter::CallFuncIFacePtr_t{};
    }

// generate the wrapper and JIT it; ignore wrapper generation errors (will simply
// result in a nullptr that is reported upstream if necessary; often, however,
// there is a different overload available that will do)
    // auto oldErrLvl = gErrorIgnoreLevel;
    // gErrorIgnoreLevel = kFatal;
    wrap->fFaceptr = cling::cppyy::gCling->CallFunc_IFacePtr(callf, as_iface);
    // gErrorIgnoreLevel = oldErrLvl;

    cling::cppyy::gCling->CallFunc_Delete(callf);   // does not touch IFacePtr
    return wrap->fFaceptr;
}

static inline
bool copy_args(CPyCppyy::Parameter* args, size_t nargs, void** vargs)
{
    bool runRelease = false;
    for (size_t i = 0; i < nargs; ++i) {
        switch (args[i].fTypeCode) {
        case 'X':       /* (void*)type& with free */
            runRelease = true;
        case 'V':       /* (void*)type& */
            vargs[i] = args[i].fValue.fVoidp;
            break;
        case 'r':       /* const type& */
            vargs[i] = args[i].fRef;
            break;
        default:        /* all other types in union */
            vargs[i] = (void*)&args[i].fValue.fVoidp;
            break;
        }
    }
    return runRelease;
}

static inline
void release_args(CPyCppyy::Parameter* args, size_t nargs) {
    for (size_t i = 0; i < nargs; ++i) {
        if (args[i].fTypeCode == 'X')
            free(args[i].fValue.fVoidp);
    }
}

static inline
bool is_ready(CallWrapper* wrap, bool is_direct) {
    return (!is_direct && wrap->fFaceptr.fGeneric) || (is_direct && wrap->fFaceptr.fDirect);
}

inline
bool WrapperCall(Cppyy::TCppMethod_t method, size_t nargs, void* args_, void* self, void* result)
{
    CPyCppyy::Parameter* args = (CPyCppyy::Parameter*)args_;
    bool is_direct = nargs & DIRECT_CALL;
    nargs = CALL_NARGS(nargs);

    auto *D = (clang::Decl *) method;
    CallWrapper *wrap;
    if (auto *FD = llvm::dyn_cast_or_null<clang::FunctionDecl>(D)) {
        wrap = new_CallWrapper(FD, FD->getNameAsString());
    } else {
        printf("Function Decl invalid\n");
    }
    const cling::Interpreter::CallFuncIFacePtr_t& faceptr = \
        is_ready(wrap, is_direct) ? wrap->fFaceptr : GetCallFunc(method, !is_direct);
    if (!is_ready(wrap, is_direct))
        return false;        // happens with compilation error

    nargs = CALL_NARGS(nargs);
    if (faceptr.fKind == cling::Interpreter::CallFuncIFacePtr_t::kGeneric) {
        bool runRelease = false;
        const auto& fgen = is_direct ? faceptr.fDirect : faceptr.fGeneric;
        if (nargs <= SMALL_ARGS_N) {
            void* smallbuf[SMALL_ARGS_N];
            if (nargs) runRelease = copy_args(args, nargs, smallbuf);
            fgen(self, (int)nargs, smallbuf, result);
        } else {
            std::vector<void*> buf(nargs);
            runRelease = copy_args(args, nargs, buf.data());
            fgen(self, (int)nargs, buf.data(), result);
        }
        if (runRelease) release_args(args, nargs);
        return true;
    }

    if (faceptr.fKind == cling::Interpreter::CallFuncIFacePtr_t::kCtor) {
        bool runRelease = false;
        if (nargs <= SMALL_ARGS_N) {
            void* smallbuf[SMALL_ARGS_N];
            if (nargs) runRelease = copy_args(args, nargs, (void**)smallbuf);
            faceptr.fCtor((void**)smallbuf, result, (unsigned long)nargs);
        } else {
            std::vector<void*> buf(nargs);
            runRelease = copy_args(args, nargs, buf.data());
            faceptr.fCtor(buf.data(), result, (unsigned long)nargs);
        }
        if (runRelease) release_args(args, nargs);
        return true;
    }

    if (faceptr.fKind == cling::Interpreter::CallFuncIFacePtr_t::kDtor) {
        std::cerr << " DESTRUCTOR NOT IMPLEMENTED YET! " << std::endl;
        return false;
    }

    return false;
}


template<typename T>
static inline
T CallT(Cppyy::TCppMethod_t method, Cppyy::TCppObject_t self, size_t nargs, void* args)
{
    T t{};
    if (WrapperCall(method, nargs, args, (void*)self, &t))
        return t;
    return (T)-1;
}

#define CPPYY_IMP_CALL(typecode, rtype)                                      \
rtype Cppyy::Call##typecode(TCppMethod_t method, TCppObject_t self, size_t nargs, void* args)\
{                                                                            \
    return CallT<rtype>(method, self, nargs, args);                          \
}

void Cppyy::CallV(TCppMethod_t method, TCppObject_t self, size_t nargs, void* args)
{
    if (!WrapperCall(method, nargs, args, (void*)self, nullptr))
        return /* TODO ... report error */;
}

CPPYY_IMP_CALL(B,  unsigned char)
CPPYY_IMP_CALL(C,  char         )
CPPYY_IMP_CALL(H,  short        )
CPPYY_IMP_CALL(I,  int          )
CPPYY_IMP_CALL(L,  long         )
CPPYY_IMP_CALL(LL, long long    )
CPPYY_IMP_CALL(F,  float        )
CPPYY_IMP_CALL(D,  double       )
CPPYY_IMP_CALL(LD, long double  )

void* Cppyy::CallR(TCppMethod_t method, TCppObject_t self, size_t nargs, void* args)
{
    void* r = nullptr;
    if (WrapperCall(method, nargs, args, (void*)self, &r))
        return r;
    return nullptr;
}

static inline
char* cppstring_to_cstring(const std::string& cppstr)
{
    char* cstr = (char*)malloc(cppstr.size()+1);
    memcpy(cstr, cppstr.c_str(), cppstr.size()+1);
    return cstr;
}



Cppyy::TCppObject_t Cppyy::CallConstructor(
    TCppMethod_t method, TCppType_t /* klass */, size_t nargs, void* args)
{
    void* obj = nullptr;
    if (WrapperCall(method, nargs, args, nullptr, &obj))
        return (TCppObject_t)obj;
    return (TCppObject_t)0;
}
// helpers for Cppyy::GetMethodTemplate()

static inline
void remove_space(std::string& n) {
   std::string::iterator pos = std::remove_if(n.begin(), n.end(), isspace);
   n.erase(pos, n.end());
}

static inline
bool template_compare(std::string n1, std::string n2) {
    if (n1.back() == '>') n1 = n1.substr(0, n1.size()-1);
    remove_space(n1);
    remove_space(n2);
    return n2.compare(0, n1.size(), n1) == 0;
}

static inline
std::string type_remap(const std::string& n1, const std::string& n2)
{
// Operator lookups of (C++ string, Python str) should succeed for the combos of
// string/str, wstring/str, string/unicode and wstring/unicode; since C++ does not have a
// operator+(std::string, std::wstring), we'll have to look up the same type and rely on
// the converters in CPyCppyy/_cppyy.
    if (n1 == "str" || n1 == "unicode") {
        if (n2 == "std::basic_string<wchar_t,std::char_traits<wchar_t>,std::allocator<wchar_t> >")
            return n2;                      // match like for like
        return "std::string";               // probably best bet
    } else if (n1 == "float") {
        return "double";                    // debatable, but probably intended
    } else if (n1 == "complex") {
        return "std::complex<double>";
    }
    return n1;
}

// method properties ---------------------------------------------------------
bool Cppyy::NewIsPublicMethod(TCppMethod_t method)
{
    auto *D = (clang::Decl *) method;
    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(D)) {
        return CXXMD->getAccess() == clang::AccessSpecifier::AS_public;
    }

    return false;
}

bool Cppyy::NewIsProtectedMethod(TCppMethod_t method)
{
    auto *D = (clang::Decl *) method;
    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(D)) {
        return CXXMD->getAccess() == clang::AccessSpecifier::AS_protected;
    }

    return false;
}

bool Cppyy::NewIsConstructor(TCppMethod_t method)
{
    auto *D = (clang::Decl *) method;
    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(D)) {
        return llvm::isa_and_nonnull<clang::CXXConstructorDecl>(CXXMD);
    }

    return false;
}

bool Cppyy::NewIsDestructor(TCppMethod_t method)
{
    auto *D = (clang::Decl *) method;
    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(D)) {
        return llvm::isa_and_nonnull<clang::CXXDestructorDecl>(CXXMD);
    }

    return false;
}

bool Cppyy::NewIsStaticMethod(TCppMethod_t method)
{
    auto *D = (clang::Decl *) method;
    if (auto *CXXMD = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(D)) {
        return CXXMD->isStatic();
    }

    return false;
}

// data member reflection information ----------------------------------------
std::vector<Cppyy::TCppScope_t> Cppyy::NewGetDatamembers(TCppScope_t scope)
{
    auto *D = (clang::Decl *) scope;

    if (auto *CXXRD = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(D)) {
        std::vector<TCppScope_t> datamembers;
        for (auto it = CXXRD->field_begin(), end = CXXRD->field_end(); it != end ; it++) {
            datamembers.push_back((TCppScope_t) *it);
        }
        return datamembers;
    }
    return {};
}

static inline
int count_scopes(const std::string& tpname)
{
    int count = 0;
    std::string::size_type pos = tpname.find("::", 0);
    while (pos != std::string::npos) {
        count++;
        pos = tpname.find("::", pos+1);
    }
    return count;
}

std::string Cppyy::NewGetDatamemberTypeAsString(TCppScope_t scope)
{
    auto D = (clang::Decl *) scope;

    if (auto VD = llvm::dyn_cast_or_null<clang::ValueDecl>(D)) {
        return VD->getType().getAsString();
    }

    return "";
}

intptr_t Cppyy::NewGetDatamemberOffset(TCppScope_t scope, TCppScope_t idata)
{
    auto *D = (clang::Decl *) idata;
    printf("hoop\n");
    if (scope == NewGetGlobalScope() || Cppyy::NewIsNamespace(scope)) {
        if (auto *VD = llvm::dyn_cast_or_null<clang::VarDecl>(D)) {
            auto GD = clang::GlobalDecl(VD);
            std::string mangledName;
            cling::utils::Analyze::maybeMangleDeclName(GD, mangledName);
            auto address = dlsym(/*whole_process=*/0, mangledName.c_str());
            if (!address)
                address = cling::cppyy::gCling->getAddressOfGlobal(GD);
            printf("here\n");
            return (intptr_t) address;
        }
    }

    if (auto *CXXRD = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(D)) {
        if (llvm::isa_and_nonnull<clang::VarDecl>(D)) {
            return (intptr_t) cling::cppyy::gCling->process((std::string("&")+NewGetScopedFinalName(idata)+";").c_str());
        }
        if (auto *FD = llvm::dyn_cast_or_null<clang::FieldDecl>(D)) {
            auto &ASTCxt = cling::cppyy::gCling->getCI()->getSema().getASTContext();
            return (intptr_t) ASTCxt.toCharUnitsFromBits(
                    ASTCxt.getASTRecordLayout(CXXRD)
                          .getFieldOffset(FD->getFieldIndex())).getQuantity();
        }
    }
    return 0;
}

bool Cppyy::NewCheckDatamember(TCppScope_t scope, const std::string& name)
{
    auto *D = (clang::Decl *) scope;
    cling::Interpreter::PushTransactionRAII RAII(cling::cppyy::gCling);
    auto *ND = cling::utils::Lookup::Named(&cling::cppyy::gCling->getCI()->getSema(),
                                           name,
                                           clang::Decl::castToDeclContext(D));
    return (bool) ND;
}

// data member properties ----------------------------------------------------
bool Cppyy::NewIsPublicData(TCppScope_t data)
{
    auto *D = (clang::Decl *) data;

    if (auto *FD = llvm::dyn_cast_or_null<clang::FieldDecl>(D)) {
        return FD->getAccess() == clang::AS_public;
    }

    return false;
}

bool Cppyy::NewIsProtectedData(TCppScope_t data)
{
    auto *D = (clang::Decl *) data;

    if (auto *FD = llvm::dyn_cast_or_null<clang::FieldDecl>(D)) {
        return FD->getAccess() == clang::AS_protected;
    }

    return false;
}

bool Cppyy::NewIsStaticDatamember(TCppScope_t var)
{
    auto *D = (clang::Decl *) var;
    if (auto *VD = llvm::dyn_cast_or_null<clang::VarDecl>(D)) {
        return true;
        // return VD->isStaticDataMember();
    }

    return false;
}

bool Cppyy::NewIsConstVar(TCppScope_t var)
{
    auto *D = (clang::Decl *) var;

    if (auto *VD = llvm::dyn_cast_or_null<clang::ValueDecl>(D)) {
        return VD->getType().isConstQualified();
    }

    return false;
}

