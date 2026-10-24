# ps3recomp - tests/fiber_switch
#
# cellFiber's scheduler API on the host's own switching mechanism: Windows
# fibers on Windows, ucontext elsewhere. Standalone project with its own build dir,
# and also added from the top-level CMakeLists under PS3RECOMP_BUILD_TESTS.
#
#   cmake -S tests/fiber_switch -B build-fiber -G Ninja
#   cmake --build build-fiber && ./build-fiber/fiber_switch
#
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(fiber_switch LANGUAGES C CXX)
    enable_testing()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT DEFINED PS3RECOMP_TREE_ROOT)
    set(PS3RECOMP_TREE_ROOT "${PS3RECOMP_ROOT}")
endif()

# The library now owns firmware guest state, reservations and LV2 flags.
if(NOT TARGET ps3recomp_runtime)
    set(PS3RECOMP_BUILD_TESTS OFF CACHE BOOL "Build runtime tests" FORCE)
    add_subdirectory("${PS3RECOMP_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/runtime")
endif()
add_executable(fiber_switch main.c "${PS3RECOMP_ROOT}/runtime/host/host_stubs.c")
target_link_libraries(fiber_switch PRIVATE ps3recomp_runtime)
set_target_properties(fiber_switch PROPERTIES LINKER_LANGUAGE CXX)

# The case that signals a fiber from another host thread.
find_package(Threads REQUIRED)
target_link_libraries(fiber_switch PRIVATE Threads::Threads)

target_include_directories(fiber_switch PRIVATE
    "${PS3RECOMP_TREE_ROOT}/libs/spurs"
    "${PS3RECOMP_TREE_ROOT}/include"
)

if(MSVC)
    target_compile_options(fiber_switch PRIVATE /W3 /wd4100)
    target_compile_definitions(fiber_switch PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    # Apple deprecated getcontext/makecontext/swapcontext in 10.6 and never
    # removed them; this suite is the evidence that they still work, so the
    # warning is noise rather than news.
    target_compile_options(fiber_switch PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
endif()

add_test(NAME fiber_switch COMMAND fiber_switch)
set_tests_properties(fiber_switch PROPERTIES
    LABELS "runtime;fiber;t0"
    TIMEOUT 60
)

if(UNIX AND NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND TARGET ps3recomp_runtime)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(FIBER_GEN "${CMAKE_CURRENT_BINARY_DIR}/worker")
    add_custom_command(
        OUTPUT "${FIBER_GEN}/ppu_recomp.h" "${FIBER_GEN}/fixture.elf" "${FIBER_GEN}/identity.c"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/make_smoke_elf.py"
                --out "${FIBER_GEN}/fixture.elf" --header "${FIBER_GEN}/ppu_recomp.h"
                --identity "${FIBER_GEN}/identity.c"
        DEPENDS "${PROJECT_SOURCE_DIR}/tools/make_smoke_elf.py" "${PROJECT_SOURCE_DIR}/tools/ppu_lifter.py"
        VERBATIM)
    add_custom_command(
        OUTPUT "${FIBER_GEN}/ppu_hle_nids.cpp"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gen_hle_nids.py"
                cellFiber cellSync2 --out "${FIBER_GEN}/ppu_hle_nids.cpp"
        DEPENDS "${PROJECT_SOURCE_DIR}/tools/gen_hle_nids.py"
                "${PROJECT_SOURCE_DIR}/libs/spurs/cellFiber.c" "${PROJECT_SOURCE_DIR}/libs/spurs/cellFiber.h"
                "${PROJECT_SOURCE_DIR}/libs/sync/cellSync2.c" "${PROJECT_SOURCE_DIR}/libs/sync/cellSync2.h"
        VERBATIM)
    add_executable(fiber_worker worker.cpp "${FIBER_GEN}/ppu_recomp.h" "${FIBER_GEN}/ppu_hle_nids.cpp"
        "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_loader.cpp" "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_hle.cpp"
        "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_sysprx.cpp" "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_fs.cpp")
    target_include_directories(fiber_worker PRIVATE "${FIBER_GEN}" "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/runtime" "${PROJECT_SOURCE_DIR}/runtime/ppu"
        "${PROJECT_SOURCE_DIR}/runtime/spu" "${PROJECT_SOURCE_DIR}/runtime/platform"
        "${PROJECT_SOURCE_DIR}/runtime/memory" "${PROJECT_SOURCE_DIR}/runtime/syscalls"
        "${PROJECT_SOURCE_DIR}/libs/video" "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(fiber_worker PRIVATE ps3recomp_runtime Threads::Threads)
    foreach(mode init attributes lifecycle dispatch)
        add_test(NAME fiber_switch_worker_${mode} COMMAND fiber_worker "${mode}")
        set_tests_properties(fiber_switch_worker_${mode} PROPERTIES LABELS "fiber;t0;tsan" TIMEOUT 30)
    endforeach()
endif()
