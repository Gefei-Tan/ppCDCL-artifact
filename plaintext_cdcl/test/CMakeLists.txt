# Testing macros
macro(add_test_executable_with_lib _name libs)
    add_executable(test_${_name} "${_name}.cpp")
    target_link_libraries(test_${_name} ${libs})
    if(COMMAND ppcdcl_fix_dylib_runtime)
        ppcdcl_fix_dylib_runtime(test_${_name} "${PLAINTEXT_EMP_TOOL_LIBRARY}")
    endif()
endmacro()

macro(add_test_case _name)
    add_test_executable_with_lib(${_name} ${PLAINTEXT_EMP_TOOL_LIBRARIES})
    add_test(NAME ${_name} COMMAND "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_${_name}" ${ARGN} WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
endmacro()

macro(add_test_case_with_run _name)
    add_test_executable_with_lib(${_name} ${PLAINTEXT_EMP_TOOL_LIBRARIES})
    add_test(NAME ${_name} COMMAND "./run" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_${_name}" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
endmacro()

# Test cases
add_test_executable_with_lib(param_probe ${PLAINTEXT_EMP_TOOL_LIBRARIES})

add_test_executable_with_lib(sat ${PLAINTEXT_EMP_TOOL_LIBRARIES})
add_test_case(unsat ./test/cnfs/prop_chain_unsat.cnf)
target_compile_features(test_unsat PRIVATE cxx_std_17)
add_test_case(preprocessing)
add_test_executable_with_lib(correctness ${PLAINTEXT_EMP_TOOL_LIBRARIES})
add_test_executable_with_lib(big ${PLAINTEXT_EMP_TOOL_LIBRARIES})
add_test_case(cdcl_dump ./uf20-01.cnf 100)
