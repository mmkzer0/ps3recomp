# ps3recomp - tests/spurs_jq2
#
# The JobQueue2 ring-entry codec and the reference scheduler are standalone
# targets (one library), so these tests link it alone (no HLE runtime) and prove
# the bit layout and the scheduling contract on synthetic entries. Guest and
# trace bytes are out of scope for this repository; the Odin-backed check lives
# in leifthrasir-recomp.
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
