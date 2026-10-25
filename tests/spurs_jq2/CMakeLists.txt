# ps3recomp - tests/spurs_jq2
#
# The JobQueue2 ring-entry codec and the reference scheduler are the standalone
# target ps3recomp_spurs_jq2_model, so these tests link it alone (no HLE runtime)
# and prove the bit layout and the scheduling contract on synthetic entries.
# Guest and trace bytes are out of scope for this repository; the Odin-backed
# check lives in leifthrasir-recomp.
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
# The CopyPush descriptor pool owns guest memory and therefore links the
# reservation model and the named-stop implementation it calls; its test is
# label t0 and tsan because it exercises the pool from several threads.
#
#   ctest --test-dir build/dev -L spurs
#   ctest --preset t0 -R jq2
#   ctest --preset tsan -R jq2_port

add_executable(test_jq2_ring_codec test_jq2_ring_codec.cpp)
target_link_libraries(test_jq2_ring_codec PRIVATE ps3recomp_spurs_jq2_model)
add_test(NAME jq2_ring_codec COMMAND test_jq2_ring_codec)
set_tests_properties(jq2_ring_codec PROPERTIES LABELS "spurs;t0")

add_executable(test_jq2_sched test_jq2_sched.cpp)
target_link_libraries(test_jq2_sched PRIVATE ps3recomp_spurs_jq2_model)
add_test(NAME jq2_sched COMMAND test_jq2_sched)
set_tests_properties(jq2_sched PROPERTIES LABELS "spurs;t0")

set(JQ2_SPU_DIR "${PROJECT_SOURCE_DIR}/runtime/spu")
add_executable(test_jq2_port
    test_jq2_port.cpp
    jq2_port_harness.c
    "${PROJECT_SOURCE_DIR}/libs/spurs/jq2/port.cpp"
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
    "${PROJECT_SOURCE_DIR}/runtime"
    "${PROJECT_SOURCE_DIR}/runtime/memory"
    "${JQ2_SPU_DIR}"
    "${PROJECT_SOURCE_DIR}/runtime/ppu"
    "${PROJECT_SOURCE_DIR}/runtime/platform"
    "${PROJECT_SOURCE_DIR}/include"
)
find_package(Threads REQUIRED)
target_link_libraries(test_jq2_port PRIVATE ps3recomp_spurs_jq2_model Threads::Threads m)
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

# The HLE executor (libs/spurs/jq2/executor.cpp) through jq2.h, on the runtime
# library it links (SPU interpreter, SPURS job loader, LV2 event flags, named
# stops). One mode per process. The stop modes fork and expect exit status 70.
#
#   jq2_exec_premature  S2: a store before admission reaches the job, after it does not
#   jq2_exec_copy       S3 and the descriptor pool: snapshot at push, slots return
#   jq2_exec_credit     1000 jobs, 2 workers, 3 tags: dispatched == executed == credited
#   jq2_exec_wrap       a depth-4 ring wraps under two workers
#   jq2_exec_barrier    SYNC order, overtaking, the out-of-order mark in guest memory
#   jq2_exec_sync       Port2Sync modes 0 and 4, BUSY, INVAL for every other mode
#   jq2_exec_sync_concurrent  several PPU threads blocked in mode 0 at once, each woken by its own credits
#   jq2_exec_join       Shutdown / Join with outstanding jobs (L1), caller-owned buffers (L2)
#   jq2_exec_stop_waiter_id, _waiter_alloc, _wait, _fiber_notify  jq2-wake: a waiter id above 63, a 64th blocked Sync, a failed wait, a kind-0 waiter that is no fiber
#   jq2_exec_stop_push_shutdown, _copy_push_shutdown  jq2-state: a push that lands after Shutdown
#   jq2_exec_desc_sizes  every descriptor size the firmware accepts (64, 128..896) through ring, admission and loader on a maxDescriptor 896 queue
#   jq2_exec_stop_push_size0, _copy_push_size0  jq2-unmodelled: the firmware accepts size 0, the HLE models no 0-byte descriptor
#   jq2_exec_observer   the job observer: layout at start, r3 at end, off when removed
#   jq2_exec_validate   every guard and status code of jq2.h
#   jq2_exec_lifted     a lifted-style body returns normally: executed and credited once
#   jq2_exec_stop_*     a job that stops or halts (interpreted or lifted), another CRT, a ring entry nobody models
# host_stubs.c satisfies the PPU-loader symbols the runtime's HLE modules name,
# as a game-less host does.
add_executable(test_jq2_executor test_jq2_executor.cpp
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c")
target_include_directories(test_jq2_executor PRIVATE "${PROJECT_SOURCE_DIR}/runtime/spu"
    "${PROJECT_SOURCE_DIR}/runtime" "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(test_jq2_executor PRIVATE ps3recomp_spurs_jq2 Threads::Threads)
# The runtime carries C++ and Objective-C, so the C++ driver links.
set_target_properties(test_jq2_executor PROPERTIES LINKER_LANGUAGE CXX)
foreach(mode premature copy credit wrap barrier sync join observer validate lifted layout layout-896 jm-wait heap-fit
              stop-job stop-halt stop-jm stop-jm-lifted stop-heap stop-unmodelled stop-crt stop-ring
              sync-concurrent waiter-fresh waiter-63 stop-waiter-id waiter-64-nowake stop-waiter-64-wake stop-wait stop-fiber-notify
              stop-push-shutdown stop-copy-push-shutdown desc-sizes stop-push-size0 stop-copy-push-size0)
    string(REPLACE "-" "_" name "${mode}")
    add_test(NAME jq2_exec_${name} COMMAND test_jq2_executor ${mode})
    set_tests_properties(jq2_exec_${name} PROPERTIES LABELS "spurs;t0;tsan" TIMEOUT 300)
endforeach()

# cellSpursJq HLE surface: the 17 firmware imports through ps3_hle_call.
# The table is generated from cellSpursJq.c, so the test's dispatch is exactly
# the build's: 17 ps3_hle_register lines, typed adapters, save-area args.
find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(JQ2_HLE_GEN "${CMAKE_CURRENT_BINARY_DIR}/jq2_hle")
    add_custom_command(
        OUTPUT "${JQ2_HLE_GEN}/ppu_hle_nids.cpp"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${JQ2_HLE_GEN}"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gen_hle_nids.py"
                cellSpursJq --out "${JQ2_HLE_GEN}/ppu_hle_nids.cpp"
        DEPENDS "${PROJECT_SOURCE_DIR}/tools/gen_hle_nids.py"
                "${PROJECT_SOURCE_DIR}/tools/nid_database.py"
                "${PROJECT_SOURCE_DIR}/libs/spurs/cellSpursJq.c"
                "${PROJECT_SOURCE_DIR}/libs/spurs/cellSpursJq.h"
        COMMENT "Generating the cellSpursJq HLE NID table"
        VERBATIM)
    add_executable(test_jq2_exports
        test_jq2_exports.cpp
        "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_hle.cpp"
        "${JQ2_HLE_GEN}/ppu_hle_nids.cpp")
    target_include_directories(test_jq2_exports PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/libs/spurs"
        "${PROJECT_SOURCE_DIR}/runtime/ppu"
        "${PROJECT_SOURCE_DIR}/include")
    set_source_files_properties("${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_hle.cpp"
        PROPERTIES COMPILE_OPTIONS "-w")
    set_source_files_properties("${JQ2_HLE_GEN}/ppu_hle_nids.cpp"
        PROPERTIES COMPILE_OPTIONS "-w")
    target_link_libraries(test_jq2_exports PRIVATE ps3recomp_runtime)
    set_target_properties(test_jq2_exports PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME jq2_exports COMMAND test_jq2_exports)
    set_tests_properties(jq2_exports PROPERTIES LABELS "spurs;t0" TIMEOUT 120)
else()
    message(STATUS "  spurs_jq2:    no python3; jq2_exports not registered")
endif()

# The CopyPush descriptor pool (libs/spurs/jq2/pool.cpp) owns guest memory; it
# is exercised from several threads, so the test is also labelled tsan.
add_executable(test_jq2_pool
    test_jq2_pool.cpp
    "${PROJECT_SOURCE_DIR}/runtime/memory/reservation.c"
    "${PROJECT_SOURCE_DIR}/runtime/diag/stop.cpp"
)
target_include_directories(test_jq2_pool PRIVATE
    "${PROJECT_SOURCE_DIR}/runtime/memory"
    "${PROJECT_SOURCE_DIR}/runtime/ppu"
    "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(test_jq2_pool PRIVATE ps3recomp_spurs_jq2 Threads::Threads)
add_test(NAME jq2_pool COMMAND test_jq2_pool)
set_tests_properties(jq2_pool PROPERTIES LABELS "spurs;t0;tsan" TIMEOUT 120)

add_executable(test_jq2_range test_jq2_range.cpp
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c")
target_include_directories(test_jq2_range PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/libs/spurs"
    "${PROJECT_SOURCE_DIR}/runtime/spu"
    "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(test_jq2_range PRIVATE ps3recomp_spurs_jq2 ps3recomp_runtime Threads::Threads)
set_target_properties(test_jq2_range PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME jq2_range COMMAND test_jq2_range)
set_tests_properties(jq2_range PROPERTIES LABELS "spurs;t0" TIMEOUT 120)
