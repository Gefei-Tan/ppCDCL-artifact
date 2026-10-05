cmake_minimum_required(VERSION 3.16)
project(ppCDCL)
set(NAME "ppCDCL")
option(PPCDCL_BUILD_PPSAT "Build the bundled ppSAT comparison project" ON)
option(PPCDCL_ALLOW_DEBUG_BUILD "Allow Debug builds in this release tree" OFF)

if(NOT CMAKE_CONFIGURATION_TYPES)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    if(NOT CMAKE_BUILD_TYPE OR (CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT PPCDCL_ALLOW_DEBUG_BUILD))
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
endif()

find_path(CMAKE_FOLDER NAMES cmake/emp-tool-config.cmake)
include(${CMAKE_FOLDER}/cmake/emp-base.cmake)

find_package(emp-tool REQUIRED)

include_directories(${EMP-TOOL_INCLUDE_DIRS})

find_library(PPCDCL_EMP_TOOL_LIBRARY emp-tool
    PATHS "${CMAKE_FOLDER}/lib" "/usr/local/lib"
    NO_DEFAULT_PATH)
if(NOT PPCDCL_EMP_TOOL_LIBRARY)
    message(FATAL_ERROR "emp-tool library not found under ${CMAKE_FOLDER}/lib or /usr/local/lib")
endif()

function(ppcdcl_fix_dylib_runtime target_name dylib_path)
    if(NOT TARGET "${target_name}" OR NOT dylib_path)
        return()
    endif()
    get_filename_component(runtime_lib "${dylib_path}" ABSOLUTE)
    get_filename_component(runtime_lib_dir "${runtime_lib}" DIRECTORY)
    set_property(TARGET "${target_name}" APPEND PROPERTY BUILD_RPATH "${runtime_lib_dir}")
    set_property(TARGET "${target_name}" APPEND PROPERTY INSTALL_RPATH "${runtime_lib_dir}")
    if(APPLE)
        add_custom_command(TARGET "${target_name}" POST_BUILD
            COMMAND ${CMAKE_INSTALL_NAME_TOOL}
                    -change libemp-tool.dylib
                    "${runtime_lib}"
                    "$<TARGET_FILE:${target_name}>"
            COMMAND ${CMAKE_INSTALL_NAME_TOOL}
                    -change @rpath/libemp-tool.dylib
                    "${runtime_lib}"
                    "$<TARGET_FILE:${target_name}>"
            VERBATIM
        )
    endif()
endfunction()

ENABLE_TESTING()
ADD_SUBDIRECTORY(test)
ADD_SUBDIRECTORY(plaintext_cdcl)

if(PPCDCL_BUILD_PPSAT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ppsat/CMakeLists.txt")
        ADD_SUBDIRECTORY(ppsat)
    else()
        message(WARNING "PPCDCL_BUILD_PPSAT is ON, but ./ppsat/CMakeLists.txt was not found; skipping ppSAT.")
    endif()
endif()
