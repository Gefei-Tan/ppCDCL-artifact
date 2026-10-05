 add_executable(microtest microtest.cpp)
 set_target_properties(microtest PROPERTIES
         RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
         OUTPUT_NAME "microtest"
 )
 target_link_libraries(microtest
         ppsat_lib
         pthread
         ${EMP_TOOL_LIBRARY}
         ${GMP_LIBRARY}
         OpenSSL::SSL
         OpenSSL::Crypto
 )
 ppsat_fix_emp_tool_runtime(microtest)
