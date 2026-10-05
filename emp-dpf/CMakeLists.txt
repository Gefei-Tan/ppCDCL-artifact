cmake_minimum_required (VERSION 3.0)
project (emp-dpf)
set(NAME "emp-dpf")

find_path(CMAKE_FOLDER NAMES cmake/emp-tool-config.cmake)
include(${CMAKE_FOLDER}/cmake/emp-base.cmake)

find_package(emp-tool REQUIRED)
include_directories(${EMP-TOOL_INCLUDE_DIRS})

# Installation
install(FILES cmake/emp-dpf-config.cmake DESTINATION cmake/)
install(DIRECTORY emp-dpf DESTINATION include/)

ENABLE_TESTING()
ADD_SUBDIRECTORY(test)
