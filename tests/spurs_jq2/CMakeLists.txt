# ps3recomp - tests/spurs_jq2
#
# The JobQueue2 ring-entry codec and the reference scheduler are standalone
# targets (one library), so these tests link it alone (no HLE runtime) and prove
# the bit layout and the scheduling contract on synthetic entries. Guest and
# trace bytes are out of scope for this repository; the Odin-backed check lives
# in leifthrasir-recomp.
#
# The port line (libs/spurs/jq2/port.hpp) adds one test program with four
# modes. It needs guest memory, the reservation model and the SPU channel path,
# so it compiles those from source (the set tests/reservation/CMakeLists.txt
# links) rather than the HLE runtime:
#
#   jq2_port_model   every transition and error code on the pure model
#   jq2_port_guest   guest byte layout, guards, the reservation model's rule
#   jq2_port_claim   a Port2Sync's claim makes a concurrent push return BUSY
#   jq2_port_race    SPU PUTLLC credits, host pushes and Syncs, PPU stwcx. on
#                    one line; final A / B exact
#
# jq2_port_race_stress is the same program at 10^7 pushes, label stress and
# never t0.
#
#   ctest --test-dir build/dev -L spurs
#   ctest --preset t0 -R jq2
#   ctest --preset tsan -R jq2_port

add_executable(test_jq2_ring_codec test_jq2_ring_codec.cpp)
target_link_libraries(test_jq2_ring_codec PRIVATE ps3recomp_spurs_jq2)
add_test(NAME jq2_ring_codec COMMAND test_jq2_ring_codec)
set_tests_properties(jq2_ring_codec PROPERTIES LABELS "spurs;t0")

add_executable(test_jq2_sched test_jq2_sched.cpp)
target_link_libraries(test_jq2_sched PRIVATE ps3recomp_spurs_jq2)
add_test(NAME jq2_sched COMMAND test_jq2_sched)
set_tests_properties(jq2_sched PROPERTIES LABELS "spurs;t0")

set(JQ2_SPU_DIR "${PROJECT_SOURCE_DIR}/runtime/spu")
add_executable(test_jq2_port
    test_jq2_port.cpp
    jq2_port_harness.c
    "${PROJECT_SOURCE_DIR}/runtime/memory/reservation.c"
    "${JQ2_SPU_DIR}/spu_channels.c" "${JQ2_SPU_DIR}/spu_drain.c" "${JQ2_SPU_DIR}/spu_lockstep.c"
    "${JQ2_SPU_DIR}/spu_coherency.c" "${JQ2_SPU_DIR}/spu_workload.c" "${JQ2_SPU_DIR}/spurs_policy.c"
    "${JQ2_SPU_DIR}/spurs_job.c" "${JQ2_SPU_DIR}/spu_tsp_weak.c" "${JQ2_SPU_DIR}/spu_vm_pagemap.c"
    "${JQ2_SPU_DIR}/spurs_policy_blob_weak.c" "${JQ2_SPU_DIR}/spu_interp.c"
    "${PROJECT_SOURCE_DIR}/runtime/platform/win32_compat.c"
    # spu_dma.h calls the typed configuration and the named-stop runtime directly.
    "${PROJECT_SOURCE_DIR}/runtime/config/config.cpp"
    "${PROJECT_SOURCE_DIR}/runtime/diag/stop.cpp"
)
target_include_directories(test_jq2_port PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/runtime/memory"
    "${JQ2_SPU_DIR}"
    "${PROJECT_SOURCE_DIR}/runtime/ppu"
    "${PROJECT_SOURCE_DIR}/runtime/platform"
    "${PROJECT_SOURCE_DIR}/include"
)
find_package(Threads REQUIRED)
target_link_libraries(test_jq2_port PRIVATE ps3recomp_spurs_jq2 Threads::Threads m)
# The runtime sources include C++ (config, stop): link with the C++ driver.
set_target_properties(test_jq2_port PROPERTIES LINKER_LANGUAGE CXX)

add_test(NAME jq2_port_model COMMAND test_jq2_port model)
add_test(NAME jq2_port_guest COMMAND test_jq2_port guest)
add_test(NAME jq2_port_claim COMMAND test_jq2_port claim)
add_test(NAME jq2_port_race COMMAND test_jq2_port race 200000)
set_tests_properties(jq2_port_model jq2_port_guest jq2_port_claim jq2_port_race
    PROPERTIES LABELS "spurs;t0;tsan" TIMEOUT 300)

add_test(NAME jq2_port_race_stress COMMAND test_jq2_port race 10000000)
set_tests_properties(jq2_port_race_stress PROPERTIES LABELS "spurs;stress" TIMEOUT 7200)
