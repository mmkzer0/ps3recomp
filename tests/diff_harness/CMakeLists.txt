# Call-level differential harness: the HLE implementation of a library function
# versus the same function lifted from the real firmware module.
#
# Two independent things live here:
#
# 1. Firmware-backed modules (only when PS3RECOMP_DIFF_FW_DIR names a lifted
#    module kept OUTSIDE this repository):
#
#      cmake -S . -B build -DPS3RECOMP_BUILD_TESTS=ON \
#            -DPS3RECOMP_DIFF_FW_DIR=/path/to/fw
#
#    Expected layout under PS3RECOMP_DIFF_FW_DIR (tools/lift_prx.py +
#    tools/ppu_lifter.py output):
#      lift/<mod>/<mod>_{image.bin,exports.json,imports.json,functions.json}
#      lift/<mod>_c/<stem>.c, <stem>.h, <prefix>harness_{tables,layout}.cpp
#    (ppu_lifter.py --raw --base <base> --toc <toc> --single-file
#     --symbol-prefix <prefix> --header-name <stem>.h --source-name <stem>.c)
#    A configured module whose directory is incomplete is a visible ctest SKIP,
#    never a silent pass. tools/diff_harness/run_diff.py does the lift,
#    configure, build and run in one go.
#
# 2. The synthetic fixture (always, when a PPC-capable clang and ld.lld are
#    found): a checked-in PPC64 module built from tests/diff_harness/synth,
#    lifted by the toolkit's own lifter and compared against a test-only HLE.
#    It proves every comparator dimension with a deliberate mutation; without
#    the toolchain it registers a visible ctest SKIP (77).
if(NOT PS3RECOMP_DIFF_FW_DIR)
    message(STATUS "diff_harness: firmware modules not configured (PS3RECOMP_DIFF_FW_DIR unset)")
endif()

find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "diff_harness: no python3; the harness targets are not built")
    return()
endif()

execute_process(
    COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE DH_TOOLKIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE DH_GIT_RC)
if(NOT DH_GIT_RC EQUAL 0 OR NOT DH_TOOLKIT_SHA)
    set(DH_TOOLKIT_SHA "unknown")
endif()

if(PS3RECOMP_DIFF_FW_DIR)
    # Canonicalize before any skip path (and before the GCC-on-macOS skip): a
    # symlinked checkout or firmware path must not smuggle firmware-derived
    # material into the repository, whatever the host compiler is.
    file(REAL_PATH "${PS3RECOMP_DIFF_FW_DIR}" DH_FW_REAL)
    file(REAL_PATH "${PROJECT_SOURCE_DIR}" DH_REPO_REAL)
    string(FIND "${DH_FW_REAL}" "${DH_REPO_REAL}/" DH_FW_INSIDE)
    if(DH_FW_REAL STREQUAL DH_REPO_REAL OR DH_FW_INSIDE EQUAL 0)
        message(FATAL_ERROR "refusing PS3RECOMP_DIFF_FW_DIR inside the repository: ${DH_FW_REAL}")
    endif()
endif()

set(DH_CORE dh_main.cpp dh_engine.cpp dh_runtime.cpp)

# Common include/link/define setup. The runner needs the repo root at compile
# time so it can refuse an output or image path inside the repository.
function(dh_common tgt)
    target_include_directories(${tgt} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/runtime" "${PROJECT_SOURCE_DIR}/runtime/ppu"
        "${PROJECT_SOURCE_DIR}/libs" "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/tests/spurs_jq2")   # its ppu_recomp.h shim: ppu_hle.cpp wants the lifter's header for ppu_context
    target_compile_definitions(${tgt} PRIVATE
        PS3RECOMP_TOOLKIT_SHA="${DH_TOOLKIT_SHA}"
        PS3RECOMP_REPO_ROOT="${PROJECT_SOURCE_DIR}")
    target_link_libraries(${tgt} PRIVATE ps3recomp_runtime Threads::Threads)
endfunction()

# The real HLE dispatcher supplies ps3_hle_register_ctx in this fixture.
function(dh_host_stubs tgt)
    set_source_files_properties("${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
        PROPERTIES COMPILE_DEFINITIONS "ps3_hle_register_ctx=dh_unused_host_register_ctx")
endfunction()

function(dh_skip_test name module reason)
    add_test(NAME ${name}
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/diff_harness/skip.py" ${module} "${reason}")
    set_tests_properties(${name} PROPERTIES LABELS "fw;stress" SKIP_RETURN_CODE 77)
endfunction()

# ---------------------------------------------------------------------------
# Firmware-backed modules
# ---------------------------------------------------------------------------
if(PS3RECOMP_DIFF_FW_DIR)
# dh_add_module(<mod> <prefix> <lifted-c-name> <cases.cpp> <hle-lib>...)
function(dh_add_module MOD PREFIX CNAME CASES)
    set(META "${DH_FW_REAL}/lift/${MOD}")
    set(LIFT "${DH_FW_REAL}/lift/${MOD}_c")
    set(missing "")
    foreach(f "${META}/${MOD}_image.bin" "${META}/${MOD}_exports.json" "${META}/${MOD}_imports.json"
              "${META}/${MOD}_functions.json" "${LIFT}/${CNAME}.c" "${LIFT}/${CNAME}.h")
        if(NOT EXISTS "${f}")
            list(APPEND missing "${f}")
        endif()
    endforeach()
    if(missing)
        message(STATUS "diff_harness: SKIP ${MOD}: missing ${missing}")
        dh_skip_test(diff_fw_${MOD} ${MOD} "missing: ${missing}")
        return()
    endif()

    set(GEN_TABLES "${LIFT}/${PREFIX}harness_tables.cpp")
    set(GEN_LAYOUT "${LIFT}/${PREFIX}harness_layout.cpp")
    add_custom_command(
        OUTPUT "${GEN_TABLES}" "${GEN_LAYOUT}"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/diff_harness/gen_tables.py"
                --name ${MOD} --prefix ${PREFIX} --header ${CNAME}.h
                --exports "${META}/${MOD}_exports.json" --imports "${META}/${MOD}_imports.json"
                --image "${META}/${MOD}_image.bin" --out "${LIFT}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tools/diff_harness/gen_tables.py"
                "${META}/${MOD}_image.bin" "${META}/${MOD}_exports.json"
                "${META}/${MOD}_imports.json" "${META}/${MOD}_functions.json"
        VERBATIM)

    # The HLE registrations, generated exactly as a port generates them.
    set(HLE_GEN "${CMAKE_CURRENT_BINARY_DIR}/${MOD}_ppu_hle_nids.cpp")
    set(HLE_DEPS "")
    foreach(l ${ARGN})
        file(GLOB_RECURSE found "${PROJECT_SOURCE_DIR}/libs/${l}.c")
        list(APPEND HLE_DEPS ${found})
    endforeach()
    add_custom_command(OUTPUT "${HLE_GEN}"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gen_hle_nids.py" ${ARGN} --out "${HLE_GEN}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tools/gen_hle_nids.py" ${HLE_DEPS}
        VERBATIM)
    set_source_files_properties("${HLE_GEN}" PROPERTIES INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}")

    # The lifted C uses C++ constructs (extern "C", thread-local g_trampoline_fn).
    set_source_files_properties("${LIFT}/${CNAME}.c" PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-w")

    add_executable(dh_diff_${MOD}
        ${DH_CORE} ${CASES}
        "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
        "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_hle.cpp" "${HLE_GEN}"
        "${LIFT}/${CNAME}.c" "${GEN_TABLES}" "${GEN_LAYOUT}")
    dh_common(dh_diff_${MOD})
    target_include_directories(dh_diff_${MOD} PRIVATE "${LIFT}")
    dh_host_stubs(dh_diff_${MOD})

    # The report is firmware-derived, so it goes into the external firmware
    # tree (the same directory run_diff.py uses), never the in-repo build tree.
    file(MAKE_DIRECTORY "${DH_FW_REAL}/build-diff")
    add_test(NAME diff_fw_${MOD}
        COMMAND $<TARGET_FILE:dh_diff_${MOD}> --out "${DH_FW_REAL}/build-diff/diff_results_${MOD}.json")
    set_tests_properties(diff_fw_${MOD} PROPERTIES LABELS "fw;stress" TIMEOUT 1800)
endfunction()

dh_add_module(libsync2 fwsync2_ fwsync2 cases_libsync2.cpp cellSync2 sysPrxForUser)
dh_add_module(libfiber fwfiber_ fwfiber cases_libfiber.cpp cellFiber cellSync2 sysPrxForUser)
endif()

# The path-hygiene/skip contract of the tools needs no toolchain.
add_test(NAME diff_harness_tools
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_diff_harness.py")
set_tests_properties(diff_harness_tools PROPERTIES LABELS "diff;t0" TIMEOUT 120)
