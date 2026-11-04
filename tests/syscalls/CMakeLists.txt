# ps3recomp - tests/syscalls
#
# Focused unit tests for the pure logic of individual lv2 syscall handlers.
# The first one is syscall 14's address/flags classification: split into
# runtime/syscalls/lv2_lock_line.c precisely so this test can link one
# translation unit instead of the whole syscall layer.
#
# Standalone project: its OWN build dir, never the runtime's build tree.
#
# Build on Linux / macOS:
#
#   cmake -S tests/syscalls -B tests/syscalls/build -G Ninja && ninja -C tests/syscalls/build
#
# Run:
#
#   ./tests/syscalls/build/test_lock_line
#
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(syscall_tests LANGUAGES C CXX)
    enable_testing()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(test_lock_line
    test_lock_line.c
    "${PS3RECOMP_ROOT}/runtime/syscalls/lv2_lock_line.c"
)

target_include_directories(test_lock_line PRIVATE
    "${PS3RECOMP_ROOT}/runtime/syscalls"
    "${PS3RECOMP_ROOT}/runtime"
    "${PS3RECOMP_ROOT}/include"
)

if(MSVC)
    target_compile_definitions(test_lock_line PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_lock_line PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_test(NAME lock_line COMMAND test_lock_line)
set_tests_properties(lock_line PROPERTIES
    LABELS "runtime;syscalls;t0"
    TIMEOUT 30
)
add_executable(test_event_flag
    test_event_flag.c
    "${PS3RECOMP_ROOT}/runtime/syscalls/sys_event.c"
    "${PS3RECOMP_ROOT}/runtime/diag/stop.cpp"
    "${PS3RECOMP_ROOT}/tests/sync_stress/test_shims.c"
)
target_include_directories(test_event_flag PRIVATE
    "${PS3RECOMP_ROOT}/runtime/syscalls"
    "${PS3RECOMP_ROOT}/runtime"
    "${PS3RECOMP_ROOT}/runtime/ppu"
    "${PS3RECOMP_ROOT}/include"
)
if(MSVC)
    target_compile_definitions(test_event_flag PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_event_flag PRIVATE -Wall -Wextra -Wno-unused-parameter)
    find_package(Threads REQUIRED)
    target_link_libraries(test_event_flag PRIVATE Threads::Threads)
endif()
# sys_event.c now reaches ps3_stop, whose implementation is C++.
set_target_properties(test_event_flag PROPERTIES LINKER_LANGUAGE CXX)

add_test(NAME event_flag COMMAND test_event_flag)
set_tests_properties(event_flag PROPERTIES
    LABELS "runtime;t0;tsan"
    TIMEOUT 30
)

# A push_by_id failure with no sender to report it to (SPU completion, mailbox,
# group-stop) is a named stop, not a dropped event. The stop ends the probe, so
# the Python runner drives one scenario per invocation and checks the PS3STOP
# line and exit status 70.
add_executable(test_event_delivery
    test_event_delivery.c
    "${PS3RECOMP_ROOT}/runtime/syscalls/sys_event.c"
    "${PS3RECOMP_ROOT}/runtime/diag/stop.cpp"
    "${PS3RECOMP_ROOT}/tests/sync_stress/test_shims.c"
)
target_include_directories(test_event_delivery PRIVATE
    "${PS3RECOMP_ROOT}/runtime/syscalls"
    "${PS3RECOMP_ROOT}/runtime"
    "${PS3RECOMP_ROOT}/runtime/ppu"
    "${PS3RECOMP_ROOT}/include"
)
if(MSVC)
    target_compile_definitions(test_event_delivery PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_event_delivery PRIVATE -Wall -Wextra -Wno-unused-parameter)
    find_package(Threads REQUIRED)
    target_link_libraries(test_event_delivery PRIVATE Threads::Threads)
endif()
set_target_properties(test_event_delivery PROPERTIES LINKER_LANGUAGE CXX)

find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME event_delivery
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_event_delivery.py"
                $<TARGET_FILE:test_event_delivery>
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(event_delivery PROPERTIES
        LABELS "runtime;t0"
        TIMEOUT 30
    )
else()
    message(STATUS "  syscalls:     no python3; event_delivery not registered")
endif()

# lv2conf: the conformance cases of tools/hwprobe/probes/lv2conf against the toolkit's HLE. The same
# case sources run on the console as a hwprobe SELF; the hardware result is the golden file
# (lv2conf/golden.csv), and compare_lv2conf.py holds the HLE result to it. test_lv2conf only runs the
# cases, each in a forked child, so a named stop, a crash or a hang in one slot is recorded as that
# case's result. fork, alarm and waitpid make it POSIX-only: the toolkit also builds on Windows, where
# this target and test are not defined.
#
# LV2CONF_DIR names the directory holding conf.h and cases/. It is a cache variable so a build can point
# at cases that are not merged yet.
if(NOT WIN32 AND TARGET ps3recomp_runtime)
    set(LV2CONF_DIR "${PS3RECOMP_ROOT}/tools/hwprobe/probes/lv2conf" CACHE PATH
        "Directory with conf.h and cases/*.c of the lv2conf conformance suite")
    if(EXISTS "${LV2CONF_DIR}/conf.h")
        file(GLOB LV2CONF_CASE_SOURCES CONFIGURE_DEPENDS "${LV2CONF_DIR}/cases/*.c")
        add_executable(test_lv2conf
            test_lv2conf.c
            lv2conf_host.c
            "${PS3RECOMP_ROOT}/runtime/host/host_stubs.c"
            ${LV2CONF_CASE_SOURCES}
        )
        target_include_directories(test_lv2conf PRIVATE
            "${LV2CONF_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${PS3RECOMP_ROOT}/runtime"
            "${PS3RECOMP_ROOT}/runtime/ppu"
            "${PS3RECOMP_ROOT}/runtime/syscalls"
            "${PS3RECOMP_ROOT}/include"
        )
        target_compile_options(test_lv2conf PRIVATE -Wall -Wextra -Wno-unused-parameter)
        if(PS3RECOMP_TOOLKIT_SHA)
            target_compile_definitions(test_lv2conf PRIVATE "LV2CONF_TOOLKIT_SHA=\"${PS3RECOMP_TOOLKIT_SHA}\"")
        endif()
        target_link_libraries(test_lv2conf PRIVATE ps3recomp_runtime)
        # The runtime carries C++, so the C++ driver does the final link.
        set_target_properties(test_lv2conf PROPERTIES LINKER_LANGUAGE CXX)

        # The golden file is added after the hardware run; until it exists the driver only requires that
        # every case ran, or was skipped with a reason.
        if(Python3_Interpreter_FOUND)
            set(LV2CONF_GOLDEN "${CMAKE_CURRENT_SOURCE_DIR}/lv2conf/golden.csv")
            set(LV2CONF_ARGS $<TARGET_FILE:test_lv2conf> "${CMAKE_CURRENT_BINARY_DIR}/lv2conf_host.csv")
            if(EXISTS "${LV2CONF_GOLDEN}")
                list(APPEND LV2CONF_ARGS --golden "${LV2CONF_GOLDEN}"
                     --divergences "${CMAKE_CURRENT_SOURCE_DIR}/lv2conf/divergences.txt")
            endif()
            add_test(NAME lv2conf_conformance
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_lv2conf.py" ${LV2CONF_ARGS})
            set_tests_properties(lv2conf_conformance PROPERTIES
                LABELS "syscalls;t0"
                TIMEOUT 900
            )
        else()
            message(STATUS "  syscalls:     no python3; lv2conf_conformance not registered")
        endif()
    else()
        message(WARNING "lv2conf: ${LV2CONF_DIR}/conf.h not found; lv2conf_conformance not registered")
    endif()
endif()
