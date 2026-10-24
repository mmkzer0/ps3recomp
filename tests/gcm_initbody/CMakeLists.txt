# ps3recomp - tests/gcm_initbody
#
# The command-buffer layout _cellGcmInitBody builds (default-state subroutine at io 0, context at
# io + 0x1000 over 32 KiB segments), the buffer-full callback's segment switch and wait, NV406E
# semaphore acquire and its named stop, the label window, ordered user commands and the driver
# events. Synthetic input only; no graphics backend, no lifted game.
#
#   ctest --test-dir build/dev -R gcm_initbody --output-on-failure

add_executable(test_gcm_initbody
    test_initbody.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_gcm_initbody PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_gcm_initbody PRIVATE m)
endif()
target_include_directories(test_gcm_initbody PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
set_target_properties(test_gcm_initbody PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME gcm_initbody COMMAND test_gcm_initbody)
set_tests_properties(gcm_initbody PROPERTIES LABELS "gcm;t0")
