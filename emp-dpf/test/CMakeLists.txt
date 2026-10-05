#Testing macro
macro(add_test_executable_with_lib _name libs)
	add_executable(test_${_name} "${_name}.cpp")
	target_link_libraries(test_${_name} ${EMP-TOOL_LIBRARIES})

	if("${_name}" STREQUAL "breakdown")
		target_compile_definitions(test_${_name} PRIVATE BREAKDOWN)
	endif()
endmacro()


macro (add_test_case _name)
	add_test_executable_with_lib(${_name} "")
  	add_test(NAME ${_name} COMMAND "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_${_name}" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
endmacro()

macro (add_test_case_with_run _name)
	add_test_executable_with_lib(${_name} "")
	add_test(NAME ${_name} COMMAND "./run" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_${_name}" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")

endmacro()

# Test cases
add_test_case(oram)
add_test_case(floram)
add_test_case(circuit)
add_test_case(benchmark)
add_test_case(benchfl)
add_test_case(breakdown)
add_test_case(matching)
add_test_case(scrypt)
add_test_case(binary)