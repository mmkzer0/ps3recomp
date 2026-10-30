cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(host_fiber LANGUAGES C CXX ASM)
    enable_testing()
    include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/PS3RecompSanitizers.cmake")
else()
    enable_language(ASM)
endif()
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
get_filename_component(HF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)
set(HF_BACKEND "${HF_ROOT}/runtime/platform/host_fiber_aarch64.c"
    "${HF_ROOT}/runtime/platform/host_fiber_aarch64.S" "${HF_ROOT}/runtime/diag/stop.cpp")
add_executable(test_host_fiber_aarch64 main.c abi_probe.S ${HF_BACKEND})
target_include_directories(test_host_fiber_aarch64 PRIVATE
    "${HF_ROOT}/runtime/platform" "${HF_ROOT}/include")
target_link_libraries(test_host_fiber_aarch64 PRIVATE Threads::Threads)
target_compile_options(test_host_fiber_aarch64 PRIVATE
    "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic;-fno-omit-frame-pointer>")
foreach(mode abi frame lifecycle many deep enomem enomem_mprotect annot)
    add_test(NAME host_fiber_${mode} COMMAND test_host_fiber_aarch64 ${mode})
    set_tests_properties(host_fiber_${mode} PROPERTIES LABELS "runtime;fiber;t0;tsan" TIMEOUT 120)
endforeach()
# A no-return call from a fiber must not make ASan print that it ignores the
# unwind: that text means the fiber stack bounds it was given are wrong.
add_test(NAME host_fiber_noret COMMAND test_host_fiber_aarch64 noret)
set_tests_properties(host_fiber_noret PROPERTIES LABELS "runtime;fiber;t0;tsan" TIMEOUT 30
    FAIL_REGULAR_EXPRESSION "ignoring requested __asan_handle_no_return")
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_test(NAME host_fiber_guard COMMAND test_host_fiber_aarch64 guard)
    set_tests_properties(host_fiber_guard PROPERTIES LABELS "runtime;fiber;stress" TIMEOUT 30)
endif()
set(HF_STOPS entry_returned resume_exited resume_running resume_destroyed uncreated
    yield_foreign_stack destroy_suspended foreign_thread size mprotect)
# The expected op, reason and ea of each stop live in main.c (stops[]). The
# parent mode demands exit 70 and exactly one machine line; a
# PASS_REGULAR_EXPRESSION would stand in for the exit status check instead.
foreach(mode ${HF_STOPS})
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # An iOS app cannot spawn itself; run the stop_ modes directly for the
        # exit status and the exact line.
        add_test(NAME host_fiber_stop_${mode} COMMAND test_host_fiber_aarch64 stop_${mode})
        set_tests_properties(host_fiber_stop_${mode} PROPERTIES
            PASS_REGULAR_EXPRESSION "PS3STOP v=1 kind=fiber-host-context")
    else()
        add_test(NAME host_fiber_stop_${mode} COMMAND test_host_fiber_aarch64 expect_stop_${mode})
    endif()
    set_tests_properties(host_fiber_stop_${mode} PROPERTIES LABELS "runtime;fiber;t0;tsan" TIMEOUT 30)
endforeach()
if(PS3RECOMP_SANITIZE_THREAD)
    add_test(NAME host_fiber_tsan_handoff COMMAND test_host_fiber_aarch64 tsan_handoff)
    add_test(NAME host_fiber_tsan_destroy COMMAND test_host_fiber_aarch64 tsan_destroy)
    set_tests_properties(host_fiber_tsan_destroy PROPERTIES LABELS "runtime;fiber;t0;tsan" TIMEOUT 60)
    set_tests_properties(host_fiber_tsan_handoff PROPERTIES LABELS "runtime;fiber;t0;tsan" TIMEOUT 30)
endif()
if(PS3RECOMP_SANITIZE)
    get_property(HF_TESTS DIRECTORY PROPERTY TESTS)
    set_tests_properties(${HF_TESTS} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=1")
endif()
add_executable(bench_host_fiber EXCLUDE_FROM_ALL bench.c ${HF_BACKEND})
target_include_directories(bench_host_fiber PRIVATE "${HF_ROOT}/runtime/platform" "${HF_ROOT}/include")
target_link_libraries(bench_host_fiber PRIVATE Threads::Threads)
