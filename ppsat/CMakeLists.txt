cmake_minimum_required(VERSION 3.10)

project(ppsat)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(CheckCXXCompilerFlag)
set(PPSAT_CXX_FLAGS -O3)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64|AARCH64")
  set(PPSAT_OPTIONAL_CXX_FLAGS -march=armv8-a+simd+crypto+crc)
else()
  set(PPSAT_OPTIONAL_CXX_FLAGS -march=native -maes -mrdseed)
endif()
foreach(PPSAT_FLAG ${PPSAT_OPTIONAL_CXX_FLAGS})
  string(MAKE_C_IDENTIFIER "PPSAT_HAS_${PPSAT_FLAG}" PPSAT_FLAG_VAR)
  check_cxx_compiler_flag("${PPSAT_FLAG}" "${PPSAT_FLAG_VAR}")
  if(${PPSAT_FLAG_VAR})
    list(APPEND PPSAT_CXX_FLAGS "${PPSAT_FLAG}")
  endif()
endforeach()
add_compile_options(${PPSAT_CXX_FLAGS})

include_directories(include)


set(PPSAT_EMP_TOOL_PREFIX_DEFAULT "/usr/local/emp-tool-master")
set(PPSAT_EMP_TOOL_PREFIX "${PPSAT_EMP_TOOL_PREFIX_DEFAULT}" CACHE PATH
    "Prefix where the emp-tool master branch used by ppSAT is installed")
set(PPSAT_EMP_DEPS_PREFIX "/usr/local" CACHE PATH
    "Prefix where ppSAT's emp-ot/emp-sh2pc dependencies are installed")

find_package(emp-ot REQUIRED
  PATHS
    "${PPSAT_EMP_DEPS_PREFIX}/cmake"
    "${PPSAT_EMP_DEPS_PREFIX}/lib/cmake/emp-ot"
  NO_DEFAULT_PATH
)

find_path(PPSAT_EMP_SH2PC_INCLUDE_DIR
  NAMES emp-sh2pc/emp-sh2pc.h
  PATHS "${PPSAT_EMP_DEPS_PREFIX}/include"
  NO_DEFAULT_PATH
)
if(NOT PPSAT_EMP_SH2PC_INCLUDE_DIR)
  message(FATAL_ERROR
    "emp-sh2pc headers not found under ${PPSAT_EMP_DEPS_PREFIX}/include. "
    "Set PPSAT_EMP_DEPS_PREFIX to the emp-sh2pc install prefix.")
endif()

# Keep ppSAT's emp-tool headers ahead of inherited include paths
# (e.g. update-backend).
find_path(PPSAT_EMP_TOOL_INCLUDE_DIR
  NAMES emp-tool/emp-tool.h
  PATHS "${PPSAT_EMP_TOOL_PREFIX}/include"
  NO_DEFAULT_PATH
)
if(NOT PPSAT_EMP_TOOL_INCLUDE_DIR)
  message(FATAL_ERROR
    "emp-tool master headers not found under ${PPSAT_EMP_TOOL_PREFIX}/include. "
    "Install emp-tool master there or configure -DPPSAT_EMP_TOOL_PREFIX=/path/to/emp-tool-master.")
endif()

include_directories(BEFORE "${PPSAT_EMP_TOOL_INCLUDE_DIR}")
include_directories("${PPSAT_EMP_DEPS_PREFIX}/include")
include_directories(${EMP-OT_INCLUDE_DIRS})
include_directories("${PPSAT_EMP_SH2PC_INCLUDE_DIR}")

find_package(OpenSSL REQUIRED)

unset(PPSAT_EMP_TOOL_LIBRARY_FROM_PREFIX CACHE)
find_library(PPSAT_EMP_TOOL_LIBRARY_FROM_PREFIX NAMES "emp-tool"
  PATHS "${PPSAT_EMP_TOOL_PREFIX}/lib"
  NO_DEFAULT_PATH
)
if(NOT PPSAT_EMP_TOOL_LIBRARY_FROM_PREFIX)
  message(FATAL_ERROR
    "emp-tool library not found under ${PPSAT_EMP_TOOL_PREFIX}/lib. "
    "Install emp-tool master there or configure -DPPSAT_EMP_TOOL_PREFIX=/path/to/emp-tool-master.")
endif()
# Avoid using the top-level cached EMP_TOOL_LIBRARY that points to update-backend.
set(EMP_TOOL_LIBRARY "${PPSAT_EMP_TOOL_LIBRARY_FROM_PREFIX}")

# Make build-tree ppSAT binaries runnable without manually setting
# DYLD_LIBRARY_PATH/LD_LIBRARY_PATH.
list(PREPEND CMAKE_BUILD_RPATH "${PPSAT_EMP_TOOL_PREFIX}/lib")
list(PREPEND CMAKE_INSTALL_RPATH "${PPSAT_EMP_TOOL_PREFIX}/lib")

function(ppsat_fix_emp_tool_runtime target_name)
  if(APPLE)
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_INSTALL_NAME_TOOL}
              -change @rpath/libemp-tool.dylib
              "${PPSAT_EMP_TOOL_PREFIX}/lib/libemp-tool.dylib"
              "$<TARGET_FILE:${target_name}>"
      VERBATIM
    )
  endif()
endfunction()

enable_testing()

add_subdirectory(src)
add_subdirectory(Eval)
