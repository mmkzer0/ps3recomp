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
