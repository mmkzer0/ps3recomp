# ps3recomp - tests/spurs_jq2
#
# The JobQueue2 ring-entry codec, the reference scheduler and the CopyPush
# descriptor pool are standalone targets (one library). The codec and scheduler
# tests link it alone (no HLE runtime) and prove the bit layout and scheduling
# contract on synthetic entries. The pool owns guest memory and therefore links
# the reservation model and the named-stop implementation it calls; its test is
# label t0 and tsan because it exercises the pool from several threads.
#
#   ctest --test-dir build/dev -L spurs
#   ctest --preset t0 -R jq2

add_executable(test_jq2_ring_codec test_jq2_ring_codec.cpp)
target_link_libraries(test_jq2_ring_codec PRIVATE ps3recomp_spurs_jq2)
add_test(NAME jq2_ring_codec COMMAND test_jq2_ring_codec)
set_tests_properties(jq2_ring_codec PROPERTIES LABELS "spurs;t0")

add_executable(test_jq2_sched test_jq2_sched.cpp)
target_link_libraries(test_jq2_sched PRIVATE ps3recomp_spurs_jq2)
add_test(NAME jq2_sched COMMAND test_jq2_sched)
set_tests_properties(jq2_sched PROPERTIES LABELS "spurs;t0")

find_package(Threads REQUIRED)
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
