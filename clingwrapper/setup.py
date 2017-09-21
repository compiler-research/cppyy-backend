#!/usr/bin/env python

import os, glob, subprocess
from setuptools import setup, Extension
from distutils import log
from distutils.command.build_ext import build_ext as _build_ext
from wheel.bdist_wheel import bdist_wheel as _bdist_wheel
from codecs import open


here = os.path.abspath(os.path.dirname(__file__))
with open(os.path.join(here, 'README.rst'), encoding='utf-8') as f:
    long_description = f.read()


def get_include_path():
    cli_arg = subprocess.check_output(['cling-config', '--cppflags'])
    return cli_arg[2:-1]

class my_build_cpplib(_build_ext):
    def build_extension(self, ext):
        objects = self.compiler.compile(
            ext.sources,
            output_dir=self.build_temp,
            include_dirs=ext.include_dirs,
            debug=self.debug,
            extra_postargs=['-std=c++11', '-O2'])

        ext_path = self.get_ext_fullpath(ext.name)
        output_dir = os.path.dirname(ext_path)
        full_libname = os.path.basename(ext_path)

        log.info("Now building %s", full_libname)
        self.compiler.link_shared_object(
            objects, full_libname,
            build_temp=self.build_temp,
            output_dir=output_dir,
            debug=self.debug,
            target_lang='c++')

class my_bdist_wheel(_bdist_wheel):
    def finalize_options(self):
     # this is a universal, but platform-specific package; a combination
     # that wheel does not recognize, thus simply fool it
        from distutils.util import get_platform
        self.plat_name = get_platform()
        self.universal = True
        _bdist_wheel.finalize_options(self)
        self.root_is_pure = True


setup(
    name='clingwrapper',
    description='C/C++ wrapper for Cling',
    long_description=long_description,
    url='http://pypy.org',

    # Author details
    author='PyPy Developers',
    author_email='pypy-dev@python.org',

    use_scm_version=True,
    setup_requires=['setuptools_scm', 'cppyy_backend'],

    license='LBNL BSD',

    classifiers=[
        'Development Status :: 4 - Beta',

        'Intended Audience :: Developers',
        'Intended Audience :: Science/Research',

        'Topic :: Software Development',
        'Topic :: Software Development :: Interpreters',

        'License :: OSI Approved :: LBNL BSD License',

        'Operating System :: POSIX',
        'Operating System :: POSIX :: Linux',
        'Operating System :: MacOS :: MacOS X',

        'Programming Language :: C',
        'Programming Language :: C++',

        'Natural Language :: English'
    ],

    keywords='C++ bindings',

    install_requires=['cppyy-backend'],

    ext_modules=[Extension('cppyy_backend/lib/libcppyy_backend',
        sources=glob.glob('src/clingwrapper.cxx'),
        include_dirs=[get_include_path()])],

    cmdclass = {
        'build_ext': my_build_cpplib,
        'bdist_wheel': my_bdist_wheel
    }
)
