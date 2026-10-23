# ps3recomp - tests/lwsync
#
# sys_lwmutex_* and sys_lwcond_* through ps3_hle_call, the dispatch a title
# uses, on the runtime library's own handlers (runtime/syscalls/sys_lwsync.cpp):
#
#   lwsync_contract   the guest-visible contract: recursion, error codes,
#                     destroy with a waiter, timeouts, signal one / all / to
#   lwsync_mp         litmus, message passing through an lwmutex-protected slot
#   lwsync_lostwake   litmus, a signal racing a waiter that is entering the wait
#
# The short litmus runs are label t0 and tsan. lwsync_*_stress is the same
# program at 10^8 iterations, label stress and never t0.
#
#   ctest --preset t0 -R lwsync
#   ctest --preset tsan -R lwsync
#   ctest --test-dir build/dev -L stress -R lwsync

add_executable(test_lwsync
    test_lwsync.cpp
    "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_hle.cpp"
)
# ppu_hle.cpp includes ppu_recomp.h for ppu_context: the one beside this file
# gives it the toolkit's own definition, there being no lift here.
target_include_directories(test_lwsync PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/runtime/ppu"
    "${PROJECT_SOURCE_DIR}/include"
)
set_source_files_properties("${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_hle.cpp" PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(test_lwsync PRIVATE ps3recomp_runtime)
# The runtime carries C++ and Objective-C, so the C++ driver links.
set_target_properties(test_lwsync PROPERTIES LINKER_LANGUAGE CXX)

add_test(NAME lwsync_contract COMMAND test_lwsync contract)
set_tests_properties(lwsync_contract PROPERTIES LABELS "sync;t0;tsan" TIMEOUT 120)

add_test(NAME lwsync_mp COMMAND test_lwsync mp 2000000)
add_test(NAME lwsync_lostwake COMMAND test_lwsync lostwake 500000)
set_tests_properties(lwsync_mp lwsync_lostwake PROPERTIES LABELS "sync;t0;tsan" TIMEOUT 300)

add_test(NAME lwsync_mp_stress COMMAND test_lwsync mp 100000000)
add_test(NAME lwsync_lostwake_stress COMMAND test_lwsync lostwake 100000000)
set_tests_properties(lwsync_mp_stress lwsync_lostwake_stress PROPERTIES LABELS "sync;stress" TIMEOUT 7200)
