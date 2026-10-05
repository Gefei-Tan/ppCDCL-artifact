# Testing macros
macro(add_test_executable_with_lib _name libs)
    add_executable(test_${_name} "${_name}.cpp")
    target_link_libraries(test_${_name} ${EMP-TOOL_LIBRARIES})
    if(COMMAND ppcdcl_fix_dylib_runtime)
        ppcdcl_fix_dylib_runtime(test_${_name} "${PPCDCL_EMP_TOOL_LIBRARY}")
    endif()
endmacro()

macro(add_test_case _name)
    add_test_executable_with_lib(${_name} "")
    add_test(NAME ${_name} COMMAND "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_${_name}" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
endmacro()

macro(add_test_case_with_run _name)
    add_test_executable_with_lib(${_name} "")
    add_test(NAME ${_name} COMMAND "./run" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_${_name}" ${ARGN} WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")

endmacro()

# Test cases

add_test_case_with_run(floram)
add_test_case_with_run(states)
add_test_case_with_run(wl)
add_test_case_with_run(clause_list)
add_test_executable_with_lib(bench "")
add_test_case_with_run(solver 16 8)
add_test_executable_with_lib(microbench "")
add_test_executable_with_lib(good_bench "")
add_test(NAME good_bench_flat_geometry_smoke
         COMMAND "./run" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_good_bench"
                 300 1117 1 3 10 10 1 1 1 1 1 1 1 1
                 --no-oramV --no-shortest-wl --threads 2
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
set_tests_properties(good_bench_flat_geometry_smoke PROPERTIES
    LABELS "correctness;benchmark"
    PASS_REGULAR_EXPRESSION "SUMMARY"
    RUN_SERIAL TRUE
    TIMEOUT 30)
add_test_case_with_run(heuristics)
add_test_case(public_schedule)
add_test_case(dimacs)
add_test_executable_with_lib(production_step_estimator "")
add_test(NAME production_step_projection
         COMMAND "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_production_step_estimator" --self-test
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
add_test_case_with_run(ppcdcl_unit --no-oramV)
add_test(NAME ppcdcl_watch_frame_cache_coherence
         COMMAND "./run" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_ppcdcl_unit" --no-oramV
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
set_tests_properties(ppcdcl_watch_frame_cache_coherence PROPERTIES
    ENVIRONMENT "PPCDCL_UNIT_ONLY=watch_frame_cache"
    LABELS "correctness;watch-cache;watch-indexed"
    RUN_SERIAL TRUE
    TIMEOUT 120)
add_test(NAME ppcdcl_clause_address_width
         COMMAND "./run" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_ppcdcl_unit" --no-oramV
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
set_tests_properties(ppcdcl_clause_address_width PROPERTIES
    ENVIRONMENT "PPCDCL_UNIT_ONLY=physical_clause_address"
    LABELS "correctness;oram"
    RUN_SERIAL TRUE
    TIMEOUT 120)
add_test_case_with_run(ppcdcl_solver_e2e
    ./uf20-01.cnf sat 1000000
    --public-early-exit --no-oramV
    --max-watchlist 14 --max-phi-literals 3
    --max-conflict-literals 10 --max-conflict-clauses 4
    --uip-cap 30 --decision-delay 10 --conflict-delay 10
    --threads 2 --no-shortest-wl --base-port 12480)
set_tests_properties(ppcdcl_solver_e2e PROPERTIES
    LABELS "correctness;integration"
    RUN_SERIAL TRUE
    TIMEOUT 90)
add_test(NAME ppcdcl_source_gate_e2e
         COMMAND "./run" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_ppcdcl_solver_e2e"
                 ./test/cnfs/conflict_simple.cnf sat 60
                 --no-oramV --max-watchlist 8 --decision-delay 3 --conflict-delay 5
                 --threads 2 --base-port 12880
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
set_tests_properties(ppcdcl_source_gate_e2e PROPERTIES
    LABELS "correctness;integration;source-gate"
    PASS_REGULAR_EXPRESSION "source_attempt_steps=50 source_refresh_steps=54 source_skipped_steps=6"
    RUN_SERIAL TRUE
    TIMEOUT 60)
add_test(NAME ppcdcl_indexed_watch_e2e
         COMMAND "./run" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_ppcdcl_solver_e2e"
                 ./test/cnfs/conflict_simple.cnf sat 60
                 --no-oramV --max-watchlist 8 --decision-delay 3 --conflict-delay 5
                 --threads 2 --base-port 12890 --watch-backend indexed
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
set_tests_properties(ppcdcl_indexed_watch_e2e PROPERTIES
    LABELS "correctness;integration;watch-indexed"
    PASS_REGULAR_EXPRESSION "effective=indexed"
    RUN_SERIAL TRUE
    TIMEOUT 60)
