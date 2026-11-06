# ps3recomp - tests/spu_kernel_range
#
# The SPURS kernel's selectWorkload service copies 0x80 bytes from an EA it reads out of the local store: an EA
# whose line is not inside guest memory is the named stop hle-guest-range, not a host read past vm_base
# (runtime/spu/spu_channels.c spu_indirect_branch).
#
#   ctest --test-dir build/dev -R spu_kernel_range --output-on-failure

# The SPU channel layer is compiled in from source, as tests/reservation does: the library carries every HLE
# module, and the fixture would have to stub half a console to say nothing about this copy.
set(KR_SPU_DIR "${PROJECT_SOURCE_DIR}/runtime/spu")
add_executable(test_spu_kernel_range
    main.c
    "${PROJECT_SOURCE_DIR}/runtime/memory/reservation.c"
    "${PROJECT_SOURCE_DIR}/runtime/memory/label_window.c"
    "${KR_SPU_DIR}/spu_channels.c" "${KR_SPU_DIR}/spu_drain.c" "${KR_SPU_DIR}/spu_lockstep.c"
    "${KR_SPU_DIR}/spu_coherency.c" "${KR_SPU_DIR}/spu_workload.c" "${KR_SPU_DIR}/spurs_policy.c"
    "${KR_SPU_DIR}/spurs_job.c" "${KR_SPU_DIR}/spu_tsp_weak.c" "${KR_SPU_DIR}/spu_vm_pagemap.c"
    "${KR_SPU_DIR}/spurs_policy_blob_weak.c" "${KR_SPU_DIR}/spu_interp.c"
    "${PROJECT_SOURCE_DIR}/runtime/platform/win32_compat.c"
    # spu_dma.h calls the typed configuration and the named-stop runtime directly.
    "${PROJECT_SOURCE_DIR}/runtime/config/config.cpp"
    "${PROJECT_SOURCE_DIR}/runtime/diag/stop.cpp"
)
target_include_directories(test_spu_kernel_range PRIVATE
    "${PROJECT_SOURCE_DIR}/runtime/memory"
    "${KR_SPU_DIR}"
    "${PROJECT_SOURCE_DIR}/runtime/ppu"
    "${PROJECT_SOURCE_DIR}/runtime/platform"
    "${PROJECT_SOURCE_DIR}/include"
)
find_package(Threads REQUIRED)
target_link_libraries(test_spu_kernel_range PRIVATE Threads::Threads m)
set_target_properties(test_spu_kernel_range PROPERTIES LINKER_LANGUAGE CXX)

add_test(NAME spu_kernel_range_copy COMMAND test_spu_kernel_range copy)
foreach(_case straddle far)
    add_test(NAME spu_kernel_range_${_case} COMMAND test_spu_kernel_range ${_case})
    set_tests_properties(spu_kernel_range_${_case} PROPERTIES
        PASS_REGULAR_EXPRESSION "PS3STOP v=1 kind=hle-guest-range func=spu_spurs_select_workload arg=spurs( |\n)")
endforeach()
set_tests_properties(spu_kernel_range_copy spu_kernel_range_straddle spu_kernel_range_far
    PROPERTIES LABELS "spu;t0" TIMEOUT 60)
