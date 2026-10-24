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
