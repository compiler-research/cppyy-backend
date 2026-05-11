cmake_minimum_required(VERSION 3.10)

project(cppyy-backend)
include(ExternalProject)
include(CMakeSystemSpecificInformation)

set(CMAKE_FIND_PACKAGE_SORT_ORDER NATURAL)
set(CMAKE_FIND_PACKAGE_SORT_DIRECTION DEC)

if (EMSCRIPTEN)
    set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
    set(CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS "-s SIDE_MODULE=1")
    set(CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS "-s SIDE_MODULE=1")
    set(CMAKE_STRIP FALSE)
endif()

add_library(cppyy-backend SHARED
    clingwrapper/src/clingwrapper.cxx
    clingwrapper/src/cppinterop_dispatch.cxx
)

if (EMSCRIPTEN)
    target_link_options(cppyy-backend
        PUBLIC "SHELL: -s WASM_BIGINT"
    )
endif()

# set_target_properties(cppyy-backend PROPERTIES VERSION ${PROJECT_VERSION})

# set_target_properties(cppyy-backend PROPERTIES PUBLIC_HEADER include/cpp_cppyy.h)

target_include_directories(cppyy-backend PRIVATE clingwrapper/src)


if(DEFINED CppInterOp_DIR)
    set(_interop_install_dir ${CppInterOp_DIR})
else()
    set(_interop_install_dir ${CMAKE_BINARY_DIR}/CppInterOp/install)

    if(DEFINED CPPINTEROP_USE_CLING)
        list(APPEND _interop_cmake_args "-DCPPINTEROP_USE_CLING=${CPPINTEROP_USE_CLING}")
    endif()

    if(DEFINED Cling_DIR)
        list(APPEND _interop_cmake_args "-DCling_DIR=${Cling_DIR}")
    endif()

    if(DEFINED LLVM_DIR)
        list(APPEND _interop_cmake_args "-DLLVM_DIR=${LLVM_DIR}")
    endif()

    if(DEFINED Clang_DIR)
        list(APPEND _interop_cmake_args "-DClang_DIR=${Clang_DIR}")
    endif()

    list(APPEND _interop_cmake_args "-DCMAKE_INSTALL_PREFIX=${_interop_install_dir}")

    set(_interop_build_type ${CMAKE_CFG_INTDIR})
    set(_interop_cmake_logging_settings
          LOG_DOWNLOAD ON
          LOG_CONFIGURE ON
          LOG_BUILD ON
          LOG_INSTALL ON
          LOG_OUTPUT_ON_FAILURE ON
        )

    ExternalProject_Add(CppInterOp 
        GIT_REPOSITORY https://github.com/compiler-research/CppInterOp.git
        GIT_TAG main
        PREFIX "CppInterOp"
        CMAKE_ARGS ${_interop_cmake_args}
        BUILD_BYPRODUCTS ${_interop_byproducts}
        ${_interop_cmake_logging_settings}
    )

    add_dependencies(libCppInterOp CppInterOp)
endif()

target_compile_definitions(cppyy-backend PRIVATE
  CPPINTEROP_DIR="${_interop_install_dir}"
  CMAKE_SHARED_LIBRARY_SUFFIX="${CMAKE_SHARED_LIBRARY_SUFFIX}"
)


target_include_directories(cppyy-backend PUBLIC ${_interop_install_dir}/include)
