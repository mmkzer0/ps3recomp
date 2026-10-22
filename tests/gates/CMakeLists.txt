# ps3recomp - tests/gates
#
# The runtime's startup gates and the named stop they end in: typed config,
# the executable identity check, VSCR policy. Every gate that refuses to
# continue ends the process, so those tests run a small probe executable per
# scenario from a Python runner and assert on its exit status and its
# PS3STOP line (see include/ps3emu/stop.h).
#
# The probes link ps3recomp_runtime rather than recompiling the modules under
# test, so they exercise exactly what a title links.
#
#   ctest --test-dir build/dev -L gates --output-on-failure

find_package(Python3 3.10 COMPONENTS Interpreter QUIET)

function(gates_probe name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ps3recomp_runtime)
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/include")
    # The runtime carries C++ and Objective-C, so the C++ driver links.
    set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
endfunction()

function(gates_py_test name script)
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS "  gates:        no python3; ${name} not registered")
        return()
    endif()
    add_test(NAME ${name}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/${script}" ${ARGN}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(${name} PROPERTIES LABELS "gates;t0" TIMEOUT 120)
endfunction()

gates_probe(probe_stop probe_stop.cpp)
gates_py_test(stop test_stop.py $<TARGET_FILE:probe_stop>)
