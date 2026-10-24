# Real PPU fibers blocking in the JobQueue2 and Sync2 paths: the real loader
# dispatches synthetic fiber bodies, the generated HLE table dispatches the
# imports, and the executor runs synthetic SPU bodies. Synthetic bytes only.
#
#   jq2_fiber_kind0      a fiber's mode 0 Sync registers kind 0 (+4 = its EA, no bitmap bit)
#                        and parks; another fiber runs; the credit signals it
#   jq2_fiber_spurious   a stale signal wakes WaitSignal early; Sync parks again
#   jq2_fiber_mixed      fiber and thread waiters on one port: +4 stays stale, bitmap untouched by fibers
#   jq2_fiber_race       fibers race job completion against registration, no lost wake
#   jq2_fiber_shutdown   Shutdown / Destroy while a fiber is blocked in Sync: BUSY, nothing faked
#   jq2_fiber_c4         B5's producer property: job output, Sync, GetCount/Release, Acquire
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(JQ2_FIBER_GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
add_custom_command(
    OUTPUT "${JQ2_FIBER_GEN}/ppu_recomp.h" "${JQ2_FIBER_GEN}/fixture.elf" "${JQ2_FIBER_GEN}/identity.c"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${JQ2_FIBER_GEN}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/make_smoke_elf.py"
            --out "${JQ2_FIBER_GEN}/fixture.elf" --header "${JQ2_FIBER_GEN}/ppu_recomp.h"
            --identity "${JQ2_FIBER_GEN}/identity.c"
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/make_smoke_elf.py" "${PROJECT_SOURCE_DIR}/tools/ppu_lifter.py"
    VERBATIM)
add_custom_command(
    OUTPUT "${JQ2_FIBER_GEN}/ppu_hle_nids.cpp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${JQ2_FIBER_GEN}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gen_hle_nids.py"
            cellFiber cellSync2 cellSpursJq --out "${JQ2_FIBER_GEN}/ppu_hle_nids.cpp"
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/gen_hle_nids.py"
            "${PROJECT_SOURCE_DIR}/libs/spurs/cellFiber.c" "${PROJECT_SOURCE_DIR}/libs/spurs/cellFiber.h"
            "${PROJECT_SOURCE_DIR}/libs/spurs/cellSpursJq.c" "${PROJECT_SOURCE_DIR}/libs/spurs/cellSpursJq.h"
            "${PROJECT_SOURCE_DIR}/libs/sync/cellSync2.c" "${PROJECT_SOURCE_DIR}/libs/sync/cellSync2.h"
    VERBATIM)
add_executable(jq2_fiber main.cpp "${JQ2_FIBER_GEN}/ppu_recomp.h" "${JQ2_FIBER_GEN}/ppu_hle_nids.cpp"
    "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_loader.cpp" "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_sysprx.cpp" "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_fs.cpp")
target_include_directories(jq2_fiber PRIVATE "${JQ2_FIBER_GEN}" "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/libs/spurs" "${PROJECT_SOURCE_DIR}/runtime" "${PROJECT_SOURCE_DIR}/runtime/ppu"
    "${PROJECT_SOURCE_DIR}/runtime/spu" "${PROJECT_SOURCE_DIR}/runtime/platform"
    "${PROJECT_SOURCE_DIR}/runtime/memory" "${PROJECT_SOURCE_DIR}/runtime/syscalls"
    "${PROJECT_SOURCE_DIR}/libs/video" "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(jq2_fiber PRIVATE ps3recomp_runtime Threads::Threads)
set_target_properties(jq2_fiber PROPERTIES LINKER_LANGUAGE CXX)
foreach(mode kind0 spurious mixed race shutdown c4 stop-wait)
    string(REPLACE "-" "_" name "${mode}")
    add_test(NAME jq2_fiber_${name} COMMAND jq2_fiber "${mode}")
    set_tests_properties(jq2_fiber_${name} PROPERTIES LABELS "spurs;fiber;t0;tsan" TIMEOUT 120)
endforeach()
