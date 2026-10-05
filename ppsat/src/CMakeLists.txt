file(GLOB ppsat_SRC "*.cpp")
list(REMOVE_ITEM ppsat_SRC "${CMAKE_CURRENT_SOURCE_DIR}/ppsat.cpp")
# Use a name that does not differ from the executable only by case; HFS/APFS
# is usually case-insensitive and CMake will generate clashing folders.
add_library(ppsat_lib ${ppsat_SRC})
add_executable(ppsat ppsat.cpp)
target_link_libraries(ppsat
        ${EMP_TOOL_LIBRARY}
        OpenSSL::SSL
        OpenSSL::Crypto
        ppsat_lib
)
ppsat_fix_emp_tool_runtime(ppsat)
