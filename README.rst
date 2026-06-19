cppyy-backend
=============

A repackaging of Cling, the interactive C++ interpreter, including a version
of LLVM patched for interactive use, and C/C++ wrappers that expose no further
external headers or types.


Cling documentation is here:
  https://root.cern.ch/cling

----

Find the cppyy documentation here:
  http://cppyy.readthedocs.io/

Please report bugs in the `cppyy issue tracker <https://bitbucket.org/wlav/cppyy/issues>`_.

----

Building with Bazel (experimental)
----------------------------------

CMake is the supported build for cppyy-backend. A best-effort Bazel build is
also provided and is **not** a replacement for CMake; it may lag behind. A
non-gating CI job builds it against an upstream LLVM 22 release (via
``compiler-research/ci-workflows``'s ``setup-llvm``), so a failure warns rather
than blocking a PR.

It builds the ``libcppyy-backend`` shared library and the ``cppyy_backend``
Python package against a local LLVM/Clang tree and a sibling CppInterOp.

Requirements:

- An LLVM/Clang build or release install tree (LLVM 20-22), pointed at by the
  ``LLVM_DIR`` env var.
- Sibling checkouts laid out next to this repo (the bzlmod
  ``local_path_override``\ s expect them)::

    <workspace>/
      cppyy-backend/   (this repo)
      cppyy/           (provides cppyy/bazel, the shared Bazel module)
      CppInterOp/

Build::

    LLVM_DIR=/path/to/llvm bazelisk build //:solib //:pylib

Build the wheel::

    LLVM_DIR=/path/to/llvm bazelisk build //:cppyy_backend_wheel
