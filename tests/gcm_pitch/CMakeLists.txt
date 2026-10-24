# ps3recomp - tests/gcm_pitch
#
# cellGcmGetTiledPitchSize against the pitch table of libgcm_sys. Built with
# the runtime (PS3RECOMP_BUILD_TESTS).
#
#   ctest --test-dir build/dev -R gcm_pitch --output-on-failure

add_executable(test_gcm_pitch
    main.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_gcm_pitch PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_gcm_pitch PRIVATE m)
endif()
target_include_directories(test_gcm_pitch PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
# The runtime carries C++, so the C++ driver does the final link.
set_target_properties(test_gcm_pitch PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME gcm_pitch COMMAND test_gcm_pitch)
set_tests_properties(gcm_pitch PROPERTIES LABELS "gcm;t0")
