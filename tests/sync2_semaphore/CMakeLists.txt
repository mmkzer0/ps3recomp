# Synthetic firmware-ABI calls and real PPU/fiber blocking paths.
find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
set(SYNC2_HLE_GEN "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
add_custom_command(OUTPUT "${SYNC2_HLE_GEN}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gen_hle_nids.py"
            cellSync2 cellFiber --out "${SYNC2_HLE_GEN}"
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/gen_hle_nids.py"
            "${PROJECT_SOURCE_DIR}/libs/sync/cellSync2.c"
            "${PROJECT_SOURCE_DIR}/libs/sync/cellSync2.h"
            "${PROJECT_SOURCE_DIR}/libs/spurs/cellFiber.c"
            "${PROJECT_SOURCE_DIR}/libs/spurs/cellFiber.h"
    VERBATIM)
add_executable(sync2_semaphore main.cpp
    host_stubs.c
    "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_hle.cpp" "${SYNC2_HLE_GEN}")
target_include_directories(sync2_semaphore PRIVATE "${PROJECT_SOURCE_DIR}/runtime"
    "${PROJECT_SOURCE_DIR}/runtime/ppu" "${PROJECT_SOURCE_DIR}/libs"
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/tests/spurs_jq2")
target_link_libraries(sync2_semaphore PRIVATE ps3recomp_runtime Threads::Threads)
foreach(mode layout counts debt multi fiber fullvm estimate config-acquire config-release range)
    string(REPLACE "-" "_" name "${mode}")
    add_test(NAME sync2_semaphore_${name} COMMAND sync2_semaphore "${mode}")
    set_tests_properties(sync2_semaphore_${name} PROPERTIES LABELS "sync2;t0;tsan" TIMEOUT 30)
endforeach()
