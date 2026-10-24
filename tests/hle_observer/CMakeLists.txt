# ps3recomp - tests/hle_observer
#
# The observer ps3_hle_call reports every import to (include/ps3emu/hle_observer.h): order, arguments,
# return value, threads, removal. The real runtime/ppu/ppu_hle.cpp is compiled into the test, as in
# tests/lwsync.
#
#   ctest --test-dir build/dev -R hle_observer --output-on-failure

add_executable(test_hle_observer
    test_hle_observer.cpp
    "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_hle.cpp"
)
target_include_directories(test_hle_observer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/runtime/ppu"
    "${PROJECT_SOURCE_DIR}/include"
)
set_source_files_properties("${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_hle.cpp" PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(test_hle_observer PRIVATE ps3recomp_runtime)
set_target_properties(test_hle_observer PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME hle_observer COMMAND test_hle_observer)
set_tests_properties(hle_observer PROPERTIES LABELS "ppu;t0;tsan" TIMEOUT 60)
