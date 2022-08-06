project(cppyy-backend)
cmake_minimum_required(VERSION 3.10)

set(CMAKE_FIND_PACKAGE_SORT_ORDER NATURAL)
set(CMAKE_FIND_PACKAGE_SORT_DIRECTION DEC)

add_library(cppyy-backend SHARED
    src/clingwrapper.cxx
)

# set_target_properties(cppyy-backend PROPERTIES VERSION ${PROJECT_VERSION})

# set_target_properties(cppyy-backend PROPERTIES PUBLIC_HEADER include/cpp_cppyy.h)

target_include_directories(cppyy-backend PRIVATE include)

link_directories(${PROJECT_SOURCE_DIR}/../cling/builddir/lib)

set(CLING_LIBS
    ${PROJECT_SOURCE_DIR}/../cling/builddir/lib/libclingInterpreter.a
    # clangAST
    # clangBasic
    # clangLex
)

include_directories(${PROJECT_SOURCE_DIR}/../cling/cling-obj/include)

target_link_libraries(cppyy-backend ${CLING_LIBS})
# target_link_libraries(cppyy-backend ${PROJECT_SOURCE_DIR}/../cling/builddir/lib/libcling.so)

# get_cmake_property(_variableNames VARIABLES)
# list (SORT _variableNames)
# foreach (_variableName ${_variableNames})
#     message(STATUS "${_variableName}=${${_variableName}}")
# endforeach()
