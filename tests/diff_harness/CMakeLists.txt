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

# The harness itself is portable C++20, but its runtime library carries the
# Metal/Objective-C backend, which GCC on macOS cannot compile
# (-fobjc-arc is clang-only). That is a pre-existing platform constraint, not
# a harness one: on Linux the GCC/libstdc++ CI job builds and runs this fixture.
# Say so at configure time and register the visible skips instead of a target
# that cannot link.
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS "diff_harness: SKIPPED: GCC on macOS cannot build the runtime's Metal backend; "
                   "build the differential harness with clang (GCC is supported on Linux)")
    add_test(NAME diff_harness_tools
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_diff_harness.py")
    set_tests_properties(diff_harness_tools PROPERTIES LABELS "diff;t0" TIMEOUT 120)
    add_test(NAME diff_synth
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/diff_harness/skip.py"
                diff_synth "GCC on macOS cannot build the Metal backend; use clang")
    set_tests_properties(diff_synth PROPERTIES LABELS "diff;t0" SKIP_RETURN_CODE 77)
    if(PS3RECOMP_DIFF_FW_DIR)
        foreach(mod libsync2 libfiber)
            add_test(NAME diff_fw_${mod}
                COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/diff_harness/skip.py"
                        ${mod} "GCC on macOS cannot build the Metal backend; use clang")
            set_tests_properties(diff_fw_${mod} PROPERTIES LABELS "fw;stress" SKIP_RETURN_CODE 77)
        endforeach()
    endif()
    return()
endif()

set(DH_CORE dh_main.cpp dh_engine.cpp dh_runtime.cpp)

# Common include/link/define setup. The runner needs the repo root at compile
# time so it can refuse an output or image path inside the repository.
function(dh_common tgt)
    target_include_directories(${tgt} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/runtime" "${PROJECT_SOURCE_DIR}/runtime/ppu"
        "${PROJECT_SOURCE_DIR}/libs" "${PROJECT_SOURCE_DIR}/libs/spurs"
        "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}"
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
# JQ2: the JQ HLE plus the HLEs its lifted code forwards through its imports
# (cellFiberPpuSelf, _sys_memset/_sys_strncmp/_sys_printf, sys_process_get_paramsfo).
dh_add_module(libspurs_jq fwjq_ fwjq cases_libspurs_jq.cpp cellSpursJq cellFiber sysPrxForUser)
endif()

# The path-hygiene/skip contract of the tools needs no toolchain.
add_test(NAME diff_harness_tools
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_diff_harness.py")
set_tests_properties(diff_harness_tools PROPERTIES LABELS "diff;t0" TIMEOUT 120)

# ---------------------------------------------------------------------------
# Synthetic fixture (no firmware)
# ---------------------------------------------------------------------------
set(DH_PPC_CLANG "" CACHE FILEPATH "PPC64-capable clang for the synthetic differential fixture")
set(DH_LD_LLD "" CACHE FILEPATH "ld.lld for the synthetic differential fixture")

function(dh_try_ppc_compile clang out_ok out_flags)
    set(${out_ok} FALSE PARENT_SCOPE)
    set(${out_flags} "" PARENT_SCOPE)
    if(NOT clang)
        return()
    endif()
    set(probe_dir "${CMAKE_CURRENT_BINARY_DIR}/ppc-probe")
    file(MAKE_DIRECTORY "${probe_dir}")
    file(WRITE "${probe_dir}/probe.c" "unsigned probe(unsigned a) { return a + 1u; }\n")
    foreach(cpu "-mcpu=ppc-cell-be" "-mcpu=cell" "")
        file(REMOVE "${probe_dir}/probe.o")
        execute_process(
            COMMAND "${clang}" --target=powerpc64-unknown-linux-gnu ${cpu} -ffreestanding -fno-builtin -O2
                    -c "${probe_dir}/probe.c" -o "${probe_dir}/probe.o"
            RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
        if(rc EQUAL 0 AND EXISTS "${probe_dir}/probe.o")
            file(READ "${probe_dir}/probe.o" magic HEX LIMIT 20)
            # 7f 45 4c 46 | class 02 (ELF64) | data 02 (big endian) | machine 0015 (PPC64)
            if(magic MATCHES "^7f454c46" AND magic MATCHES "^7f454c460202" AND magic MATCHES "0015$")
                set(${out_ok} TRUE PARENT_SCOPE)
                set(${out_flags} "${cpu}" PARENT_SCOPE)
                return()
            endif()
        endif()
    endforeach()
endfunction()

set(DH_PPC_CANDIDATES "")
if(DH_PPC_CLANG)
    list(APPEND DH_PPC_CANDIDATES "${DH_PPC_CLANG}")
endif()
if(DEFINED ENV{PS3RECOMP_PPC_CLANG})
    list(APPEND DH_PPC_CANDIDATES "$ENV{PS3RECOMP_PPC_CLANG}")
endif()
list(APPEND DH_PPC_CANDIDATES
    /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang
    clang clang-20 clang-19 clang-18 clang-17 clang-16 clang-15)

set(DH_PPC_CLANG_FOUND "")
set(DH_PPC_CPU_FLAG "")
foreach(cand IN LISTS DH_PPC_CANDIDATES)
    set(resolved "${cand}")
    if(NOT IS_ABSOLUTE "${cand}")
        unset(_found CACHE)
        find_program(_found NAMES "${cand}")
        set(resolved "${_found}")
    endif()
    if(NOT resolved OR NOT EXISTS "${resolved}")
        continue()
    endif()
    dh_try_ppc_compile("${resolved}" ok flags)
    if(ok)
        set(DH_PPC_CLANG_FOUND "${resolved}")
        set(DH_PPC_CPU_FLAG "${flags}")
        break()
    endif()
endforeach()

set(DH_LD_CANDIDATES "")
if(DH_LD_LLD)
    list(APPEND DH_LD_CANDIDATES "${DH_LD_LLD}")
endif()
list(APPEND DH_LD_CANDIDATES /opt/homebrew/bin/ld.lld /usr/local/bin/ld.lld ld.lld lld)
set(DH_LD_LLD_FOUND "")
foreach(cand IN LISTS DH_LD_CANDIDATES)
    set(resolved "${cand}")
    if(NOT IS_ABSOLUTE "${cand}")
        unset(_found CACHE)
        find_program(_found NAMES "${cand}")
        set(resolved "${_found}")
    endif()
    if(resolved AND EXISTS "${resolved}")
        execute_process(COMMAND "${resolved}" --version RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
        if(rc EQUAL 0)
            set(DH_LD_LLD_FOUND "${resolved}")
            break()
        endif()
    endif()
endforeach()

if(NOT DH_PPC_CLANG_FOUND OR NOT DH_LD_LLD_FOUND)
    if(NOT DH_PPC_CLANG_FOUND)
        message(STATUS "diff_harness: synthetic fixture SKIPPED: no PPC64-capable clang "
                       "(set -DDH_PPC_CLANG=/path/to/clang)")
    else()
        message(STATUS "diff_harness: synthetic fixture SKIPPED: no ld.lld (set -DDH_LD_LLD=/path/to/ld.lld)")
    endif()
    add_test(NAME diff_synth
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/diff_harness/skip.py"
                diff_synth "no PPC64-capable clang / ld.lld")
    set_tests_properties(diff_synth PROPERTIES LABELS "diff;t0" SKIP_RETURN_CODE 77)
    return()
endif()

message(STATUS "diff_harness: synthetic fixture with ${DH_PPC_CLANG_FOUND} ${DH_PPC_CPU_FLAG} + ${DH_LD_LLD_FOUND}")

set(DH_SYNTH_SRC "${CMAKE_CURRENT_SOURCE_DIR}/synth")
set(DH_SYNTH_OUT "${CMAKE_CURRENT_BINARY_DIR}/synth")
set(DH_SYNTH_C "${DH_SYNTH_OUT}/lift/synth_c")

add_custom_command(
    OUTPUT "${DH_SYNTH_OUT}/synth.elf"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DH_SYNTH_OUT}"
    COMMAND "${DH_PPC_CLANG_FOUND}" --target=powerpc64-unknown-linux-gnu ${DH_PPC_CPU_FLAG}
            -ffreestanding -fno-builtin -O2 -c "${DH_SYNTH_SRC}/module.c" -o "${DH_SYNTH_OUT}/module.o"
    COMMAND "${DH_PPC_CLANG_FOUND}" --target=powerpc64-unknown-linux-gnu ${DH_PPC_CPU_FLAG}
            -ffreestanding -fno-builtin -O2 -x c++ -std=c++20 -fno-exceptions -fno-rtti -fno-builtin
            -c "${DH_SYNTH_SRC}/module_cpp.cpp" -o "${DH_SYNTH_OUT}/module_cpp.o"
    COMMAND "${DH_PPC_CLANG_FOUND}" --target=powerpc64-unknown-linux-gnu
            -c "${DH_SYNTH_SRC}/module_asm.S" -o "${DH_SYNTH_OUT}/module_asm.o"
    COMMAND "${DH_LD_LLD_FOUND}" -static -nostdlib -T "${DH_SYNTH_SRC}/synth.ld"
            -o "${DH_SYNTH_OUT}/synth.elf"
            "${DH_SYNTH_OUT}/module.o" "${DH_SYNTH_OUT}/module_cpp.o" "${DH_SYNTH_OUT}/module_asm.o"
    DEPENDS "${DH_SYNTH_SRC}/module.c" "${DH_SYNTH_SRC}/module_cpp.cpp"
            "${DH_SYNTH_SRC}/module_asm.S" "${DH_SYNTH_SRC}/synth.ld"
    VERBATIM)

add_custom_command(
    OUTPUT "${DH_SYNTH_C}/synth.c" "${DH_SYNTH_C}/synth_harness_tables.cpp" "${DH_SYNTH_C}/synth_harness_layout.cpp"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/diff_harness/make_synth.py"
            --elf "${DH_SYNTH_OUT}/synth.elf" --out "${DH_SYNTH_OUT}"
            --lifter "${PROJECT_SOURCE_DIR}/tools/ppu_lifter.py"
            --gen-tables "${PROJECT_SOURCE_DIR}/tools/diff_harness/gen_tables.py"
    DEPENDS "${DH_SYNTH_OUT}/synth.elf"
            "${PROJECT_SOURCE_DIR}/tools/diff_harness/make_synth.py"
            "${PROJECT_SOURCE_DIR}/tools/diff_harness/gen_tables.py"
            "${PROJECT_SOURCE_DIR}/tools/ppu_lifter.py"
            "${PROJECT_SOURCE_DIR}/tools/elf_parser.py"
            "${PROJECT_SOURCE_DIR}/tools/ppu_disasm.py"
    VERBATIM)

set_source_files_properties("${DH_SYNTH_C}/synth.c" PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-w")
set_source_files_properties("${DH_SYNTH_C}/synth_harness_tables.cpp" PROPERTIES COMPILE_OPTIONS "-w")

add_executable(dh_diff_synth
    ${DH_CORE} synth/synth_hle.cpp synth/cases_synth.cpp
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
    "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_hle.cpp"
    "${DH_SYNTH_C}/synth.c" "${DH_SYNTH_C}/synth_harness_tables.cpp" "${DH_SYNTH_C}/synth_harness_layout.cpp")
dh_common(dh_diff_synth)
# This binary only ever runs the checked-in synthetic fixture; its image and
# report live in the repository's build tree, which the firmware runners refuse.
target_compile_definitions(dh_diff_synth PRIVATE PS3RECOMP_DIFF_SYNTHETIC=1)
target_include_directories(dh_diff_synth PRIVATE "${DH_SYNTH_C}")
dh_host_stubs(dh_diff_synth)

add_test(NAME diff_synth
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/diff_harness/test_synth.py"
            "$<TARGET_FILE:dh_diff_synth>" "${DH_SYNTH_OUT}")
set_tests_properties(diff_synth PROPERTIES LABELS "diff;t0" TIMEOUT 240 SKIP_RETURN_CODE 77)
