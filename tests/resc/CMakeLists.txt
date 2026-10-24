# ps3recomp - tests/resc
#
# cellResc's error numbering and cellRescVideoOutResolutionId2RescBufferMode,
# called directly with guest effective addresses into a calloc'd guest memory.
# Built with the runtime (PS3RECOMP_BUILD_TESTS).
#
#   ctest --test-dir build/dev -R resc --output-on-failure

add_executable(test_resc
    main.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_resc PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_resc PRIVATE m)
endif()
target_include_directories(test_resc PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
# The runtime carries C++, so the C++ driver does the final link.
set_target_properties(test_resc PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME resc COMMAND test_resc)
set_tests_properties(resc PROPERTIES LABELS "resc;t0")

# cellRescGcmSurface2RescSrc (NID 0x01220224): the CellGcmSurface -> CellRescSrc
# conversion, including the init-config-dependent anti-aliasing scale.
add_executable(test_resc_surface2src
    test_surface2src.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_resc_surface2src PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_resc_surface2src PRIVATE m)
endif()
target_include_directories(test_resc_surface2src PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
set_target_properties(test_resc_surface2src PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME resc_surface2src COMMAND test_resc_surface2src)
set_tests_properties(resc_surface2src PROPERTIES LABELS "resc;t0")

# cellRescSet/GetRegisterCount and the SetDisplayMode rules that pick the
# current convert program; the before-Init named stop runs in a subprocess.
add_executable(test_resc_register_count
    test_register_count.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_resc_register_count PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_resc_register_count PRIVATE m)
endif()
target_include_directories(test_resc_register_count PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
set_target_properties(test_resc_register_count PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME resc_register_count COMMAND test_resc_register_count)
set_tests_properties(resc_register_count PROPERTIES LABELS "resc;t0")

# cellRescGetBufferSize (firmware 0x02004B48 / 0x020022A0) and the Dsts records and SDK
# version it reads.
add_executable(test_resc_buffer_size
    test_buffer_size.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_resc_buffer_size PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_resc_buffer_size PRIVATE m)
endif()
target_include_directories(test_resc_buffer_size PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
set_target_properties(test_resc_buffer_size PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME resc_buffer_size COMMAND test_resc_buffer_size)
set_tests_properties(resc_buffer_size PROPERTIES LABELS "resc;t0")

# cellRescSetConvertAndFlip (libresc 0x02004D4C) and the libgcm_sys flip command: the 185-word
# stream for a synthetic 1920x1080 configuration, the frame toggle, the buffer-full callback's
# reservation points, the quad and vertex program, and the named stops for the paths not modelled.
add_executable(test_resc_convert
    test_convert.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_resc_convert PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_resc_convert PRIVATE m)
endif()
target_include_directories(test_resc_convert PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
set_target_properties(test_resc_convert PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME resc_convert COMMAND test_resc_convert)
set_tests_properties(resc_convert PROPERTIES LABELS "resc;gcm;t0")

# cellRescSetWaitFlip (libresc 0x020045E8): the 4-word ACQUIRE group on the flip-done label (0x10, or
# 18 << 4 for 720x576 with a PAL temporal mode), its reservation, and the walker side: held at the ACQUIRE
# until the vblank after the flip head completes the flip, presentable meanwhile, and a flip nobody
# completes is the gcm-wait-timeout stop.
add_executable(test_resc_waitflip
    test_waitflip.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_resc_waitflip PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_resc_waitflip PRIVATE m)
endif()
target_include_directories(test_resc_waitflip PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
set_target_properties(test_resc_waitflip PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME resc_waitflip COMMAND test_resc_waitflip)
set_tests_properties(resc_waitflip PROPERTIES LABELS "resc;gcm;t0")

# cellRescResetFlipStatus / GetFlipStatus / SetFlipHandler / SetVBlankHandler / GetLastFlipTime (libresc 02004534,
# 020044D0, 02003704, 02003794, 02004590): libgcm's status, not a library flag that completes on a poll; the
# 720x576 PAL-temporal configuration that keeps its own flag is a named stop.
add_executable(test_resc_flipstatus
    test_flipstatus.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_resc_flipstatus PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_resc_flipstatus PRIVATE m)
endif()
target_include_directories(test_resc_flipstatus PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
set_target_properties(test_resc_flipstatus PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME resc_flipstatus COMMAND test_resc_flipstatus)
set_tests_properties(resc_flipstatus PROPERTIES LABELS "resc;gcm;t0")
