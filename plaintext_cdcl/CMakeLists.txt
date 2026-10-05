cmake_minimum_required(VERSION 3.16)
set(NAME "ppCDCL")

# Use the regular emp-tool installation for the plaintext build.
set(PLAINTEXT_EMP_TOOL_PREFIX "/usr/local" CACHE PATH
    "Prefix for the normal/update-backend emp-tool install used by ppCDCL and plaintext_cdcl")


set(CMAKE_FOLDER ${PLAINTEXT_EMP_TOOL_PREFIX})
include(${CMAKE_FOLDER}/cmake/emp-base.cmake)

find_package(OpenSSL REQUIRED)
find_library(PLAINTEXT_EMP_TOOL_LIBRARY emp-tool
    PATHS ${PLAINTEXT_EMP_TOOL_PREFIX}/lib
    NO_DEFAULT_PATH)
find_path(PLAINTEXT_EMP_TOOL_INCLUDE emp-tool/emp-tool.h
    PATHS ${PLAINTEXT_EMP_TOOL_PREFIX}/include
    NO_DEFAULT_PATH)

if(NOT PLAINTEXT_EMP_TOOL_LIBRARY OR NOT PLAINTEXT_EMP_TOOL_INCLUDE)
    message(FATAL_ERROR "Regular emp-tool install not found under ${PLAINTEXT_EMP_TOOL_PREFIX}")
endif()

set(PLAINTEXT_EMP_TOOL_LIBRARIES ${PLAINTEXT_EMP_TOOL_LIBRARY} ${OPENSSL_LIBRARIES})
set(PLAINTEXT_EMP_TOOL_INCLUDE_DIRS ${OPENSSL_INCLUDE_DIR} ${PLAINTEXT_EMP_TOOL_INCLUDE})
include_directories(${PLAINTEXT_EMP_TOOL_INCLUDE_DIRS})

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

ENABLE_TESTING()
ADD_SUBDIRECTORY(test)
