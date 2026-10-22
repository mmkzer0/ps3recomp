# ps3recomp - tests/spurs_jq2
#
# The JobQueue2 ring-entry codec is a standalone target, so this test links it
# alone (no HLE runtime) and proves the bit layout on synthetic bytes. Guest
# and trace bytes are out of scope for this repository; the Odin-backed check
# lives in leifthrasir-recomp.
#
#   ctest --test-dir build/dev -L spurs

add_executable(test_jq2_ring_codec test_jq2_ring_codec.cpp)
target_link_libraries(test_jq2_ring_codec PRIVATE ps3recomp_spurs_jq2)
add_test(NAME jq2_ring_codec COMMAND test_jq2_ring_codec)
set_tests_properties(jq2_ring_codec PROPERTIES LABELS "spurs;t0")
