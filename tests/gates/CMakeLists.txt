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
#   ctest --test-dir build/dev -L config --output-on-failure

find_package(Python3 3.10 COMPONENTS Interpreter QUIET)

function(gates_probe name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ps3recomp_runtime)
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/include")
    # The runtime carries C++ and Objective-C, so the C++ driver links.
    set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
endfunction()

# gates_py_test(<name> <area label> <script> <probe args...>)
function(gates_py_test name area script)
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS "  gates:        no python3; ${name} not registered")
        return()
    endif()
    add_test(NAME ${name}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/${script}" ${ARGN}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(${name} PROPERTIES LABELS "${area};t0" TIMEOUT 120)
endfunction()

gates_probe(probe_stop probe_stop.cpp)
gates_py_test(stop "runtime;tsan" test_stop.py $<TARGET_FILE:probe_stop>)

gates_probe(probe_sha256 probe_sha256.cpp)
gates_py_test(sha256 runtime test_sha256.py $<TARGET_FILE:probe_sha256>)

gates_probe(probe_config probe_config.cpp)
gates_py_test(config "config;tsan" test_config.py $<TARGET_FILE:probe_config>)

# The lifted-code ABI gate: the runtime's check against a lift stamped with its
# ABI (runs), with another (stops lifter-abi), and with no stamp at all (link
# error). One source, two stamps; the driver also builds the unstamped pair.
gates_probe(probe_lifter_abi_match probe_lifter_abi.c stamp_lifter_abi.c)
target_compile_definitions(probe_lifter_abi_match PRIVATE
    PPU_PROBE_ABI=PPU_RUNTIME_LIFTER_ABI)

gates_probe(probe_lifter_abi_mismatch probe_lifter_abi.c stamp_lifter_abi.c)
target_compile_definitions(probe_lifter_abi_mismatch PRIVATE
    "PPU_PROBE_ABI=(PPU_RUNTIME_LIFTER_ABI+1)")

gates_py_test(lifter_abi runtime test_lifter_abi.py
    $<TARGET_FILE:probe_lifter_abi_match>
    $<TARGET_FILE:probe_lifter_abi_mismatch>
    ${PROJECT_SOURCE_DIR})
