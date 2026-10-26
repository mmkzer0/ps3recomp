# ps3recomp - tests/reservation
#
# The reservation model (runtime/memory/reservation.h): PPU lwarx/ldarx +
# stwcx./stdcx. against SPU GETLLAR + PUTLLC/PUTLLUC/PUT, on the real SPU
# channel path (spu_channels.c, spu_dma.h) and the real PPU atomics.
#
#   reservation_contract   deterministic, who kills whose reservation: both
#                          directions, same value and different value, the
#                          neighbouring line, and the documented limits
#   reservation_jq2        litmus, the JQ2 port line: an SPU PUTLLC counts
#                          credit at +0x00 while PPU lwarx/stwcx. and
#                          ldarx/stdcx. loops count at +0x10 and +0x18 on the
#                          same 128-byte line; no update may be lost
#   reservation_aba_ppu    litmus, a PPU reservation across an SPU PUTLLC of the
#                          same bytes must die, N rounds
#   reservation_aba_spu    litmus, an SPU reservation across a PPU stwcx. of the
#                          same value must die, N rounds
#
# The short runs are label t0 and tsan. *_stress is the same program at 10^8
# iterations, label stress and never t0.
#
#   ctest --preset t0 -R reservation
#   ctest --preset tsan -R reservation
#   ctest --test-dir build/dev -L stress -R reservation

# The SPU channel layer is compiled in from source, not linked through the
# runtime library: the library carries every HLE module, and the fixture would
# have to stub half a console to say nothing about reservations. The set is the
# one runtime/spu/tests/test_spu_lockline_peer.c links.
set(RESV_SPU_DIR "${PROJECT_SOURCE_DIR}/runtime/spu")
add_executable(test_reservation
    test_reservation.c
    "${PROJECT_SOURCE_DIR}/runtime/memory/reservation.c"
    "${PROJECT_SOURCE_DIR}/runtime/memory/label_window.c"
    "${RESV_SPU_DIR}/spu_channels.c" "${RESV_SPU_DIR}/spu_drain.c" "${RESV_SPU_DIR}/spu_lockstep.c"
    "${RESV_SPU_DIR}/spu_coherency.c" "${RESV_SPU_DIR}/spu_workload.c" "${RESV_SPU_DIR}/spurs_policy.c"
    "${RESV_SPU_DIR}/spurs_job.c" "${RESV_SPU_DIR}/spu_tsp_weak.c" "${RESV_SPU_DIR}/spu_vm_pagemap.c"
    "${RESV_SPU_DIR}/spurs_policy_blob_weak.c" "${RESV_SPU_DIR}/spu_interp.c"
    "${PROJECT_SOURCE_DIR}/runtime/platform/win32_compat.c"
    # spu_dma.h calls the typed configuration and the named-stop runtime directly.
    "${PROJECT_SOURCE_DIR}/runtime/config/config.cpp"
    "${PROJECT_SOURCE_DIR}/runtime/diag/stop.cpp"
)
target_include_directories(test_reservation PRIVATE
    "${PROJECT_SOURCE_DIR}/runtime/memory"
    "${RESV_SPU_DIR}"
    "${PROJECT_SOURCE_DIR}/runtime/ppu"
    "${PROJECT_SOURCE_DIR}/runtime/platform"
    "${PROJECT_SOURCE_DIR}/include"
)
find_package(Threads REQUIRED)
target_link_libraries(test_reservation PRIVATE Threads::Threads m)

add_test(NAME reservation_contract COMMAND test_reservation contract)
set_tests_properties(reservation_contract PROPERTIES LABELS "memory;t0;tsan" TIMEOUT 120)

# jq2 is three retry loops on one line: at 10^6 its TSan wall time on a 3-vCPU
# CI runner went from 130 s to a 300 s timeout (dev-ci 40037474605,
# 40038567537; 20 s locally, 2 s without TSan), from contention, not a lost
# update. The short run is 2 * 10^5; the stress tier keeps 10^8.
add_test(NAME reservation_jq2 COMMAND test_reservation jq2 200000)
add_test(NAME reservation_aba_ppu COMMAND test_reservation aba_ppu 1000000)
add_test(NAME reservation_aba_spu COMMAND test_reservation aba_spu 1000000)
set_tests_properties(reservation_jq2 reservation_aba_ppu reservation_aba_spu
    PROPERTIES LABELS "memory;t0;tsan" TIMEOUT 300)

add_test(NAME reservation_jq2_stress COMMAND test_reservation jq2 100000000)
add_test(NAME reservation_aba_ppu_stress COMMAND test_reservation aba_ppu 100000000)
add_test(NAME reservation_aba_spu_stress COMMAND test_reservation aba_spu 100000000)
set_tests_properties(reservation_jq2_stress reservation_aba_ppu_stress reservation_aba_spu_stress
    PROPERTIES LABELS "memory;stress" TIMEOUT 7200)
