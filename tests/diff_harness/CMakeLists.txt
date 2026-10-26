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
endif()

if(DH_PPC_CLANG_FOUND AND DH_LD_LLD_FOUND)
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
endif()

# ---------------------------------------------------------------------------
# Cross-compiled synthetic variants (synlib): a second, independent module
# ---------------------------------------------------------------------------
# tests/diff_harness/synth/syn_*.{c,cpp,S} implement one small library
# (syn_spec.h) four ways -- C, C++, hand-written assembly and a deliberately
# buggy C -- plus ref_native.c as the HLE side. tools/diff_harness/synth_pack.py
# packages each linked powerpc64-linux-gnu ELF as a PRX-shaped image + JSON and
# lift_module.py lifts it with the toolkit's own ppu_lifter.py, so the fixture
# also exercises the lifter's ABI and table handling (see DIFF_HARNESS.md).
#
# The GCC route is optional: without the cross toolchain these variants
# register a visible SKIP. Set PS3RECOMP_DIFF_SYNTHETIC_REQUIRED=ON in CI,
# where the toolchain is installed, so a silently vanished suite is an error.
option(PS3RECOMP_DIFF_SYNTHETIC_REQUIRED
       "Fail configuration when the PowerPC64 cross toolchain is missing" OFF)
set(PS3RECOMP_SYN_PREFIX "powerpc64-linux-gnu" CACHE STRING
    "Triplet prefix of the PowerPC64 big-endian cross toolchain")

find_program(DH_SYN_GCC ${PS3RECOMP_SYN_PREFIX}-gcc)
find_program(DH_SYN_GXX ${PS3RECOMP_SYN_PREFIX}-g++)
find_program(DH_SYN_LD  ${PS3RECOMP_SYN_PREFIX}-ld)
if(NOT DH_SYN_GCC OR NOT DH_SYN_GXX OR NOT DH_SYN_LD)
    set(DH_SYN_WHY "needs ${PS3RECOMP_SYN_PREFIX}-gcc/-g++/-ld (apt: gcc-powerpc64-linux-gnu g++-powerpc64-linux-gnu binutils-powerpc64-linux-gnu)")
    if(PS3RECOMP_DIFF_SYNTHETIC_REQUIRED)
        message(FATAL_ERROR "diff_harness synthetic variants: ${DH_SYN_WHY}")
    endif()
    message(STATUS "diff_harness: cross-compiled synthetic variants SKIPPED: ${DH_SYN_WHY}")
    add_test(NAME diff_synth_synlib
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/diff_harness/skip.py"
                synlib "${DH_SYN_WHY}")
    set_tests_properties(diff_synth_synlib PROPERTIES LABELS "diff;t0" SKIP_RETURN_CODE 77)
    return()
endif()

# -mcpu=cell is the PS3's PPU; newer GCCs dropped it, the generic core is the fallback.
set(DH_SYN_CPU "-mcpu=powerpc64")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/syn-probe")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/syn-probe/probe.c" "unsigned probe(unsigned a) { return a + 1u; }\n")
execute_process(
    COMMAND "${DH_SYN_GCC}" -m64 -mcpu=cell -c "${CMAKE_CURRENT_BINARY_DIR}/syn-probe/probe.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/syn-probe/cell.o"
    RESULT_VARIABLE DH_SYN_CELL_RC OUTPUT_QUIET ERROR_QUIET)
if(DH_SYN_CELL_RC EQUAL 0)
    set(DH_SYN_CPU "-mcpu=cell")
endif()

execute_process(COMMAND "${DH_SYN_LD}" --help OUTPUT_VARIABLE DH_SYN_LD_HELP ERROR_QUIET)
string(FIND "${DH_SYN_LD_HELP}" "--no-warn-rwx-segments" DH_SYN_HAS_RWX)
set(DH_SYN_LD_OPTS -z noexecstack --no-toc-optimize)
if(NOT DH_SYN_HAS_RWX EQUAL -1)
    list(APPEND DH_SYN_LD_OPTS --no-warn-rwx-segments)
endif()

# The PS3's terms: 64-bit registers, 32-bit effective addresses.
#   -mcmodel=small     TOC entries loaded with `ld rX,disp(r2)`, the form the
#                      lifter's jump-table discovery reads, kept by the linker
#                      with --no-toc-optimize
#   -ffreestanding     no libc, no memset/memcpy conjured from loops, no unwind tables
set(DH_SYN_BASE 0x02000000)
set(DH_SYN_ARCH -m64 ${DH_SYN_CPU} -mcmodel=small -mno-altivec -mno-vsx)
set(DH_SYN_CFLAGS ${DH_SYN_ARCH} -O2 -ffreestanding -fno-builtin -fno-pic -fno-pie
    -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector
    -fno-tree-loop-distribute-patterns -ffp-contract=off -Wall -Wextra
    -I "${CMAKE_CURRENT_SOURCE_DIR}/synth")
set(DH_SYN_CXXFLAGS ${DH_SYN_CFLAGS} -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics)

set(DH_SYNLIB_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/synth/syn_module.json")
set(DH_SYNLIB_OUT "${CMAKE_CURRENT_BINARY_DIR}/synlib")
file(MAKE_DIRECTORY "${DH_SYNLIB_OUT}")

add_custom_command(OUTPUT "${DH_SYNLIB_OUT}/syn_nids.h"
    COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/diff_harness/synth_pack.py"
            --manifest "${DH_SYNLIB_MANIFEST}" --emit-nid-header "${DH_SYNLIB_OUT}/syn_nids.h"
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/diff_harness/synth_pack.py" "${DH_SYNLIB_MANIFEST}"
    VERBATIM)

add_custom_command(OUTPUT "${DH_SYNLIB_OUT}/syn_stubs.o"
    COMMAND "${DH_SYN_GCC}" ${DH_SYN_ARCH} -I "${CMAKE_CURRENT_SOURCE_DIR}/synth"
            -c "${CMAKE_CURRENT_SOURCE_DIR}/synth/syn_stubs.S" -o "${DH_SYNLIB_OUT}/syn_stubs.o"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/synth/syn_stubs.S" "${CMAKE_CURRENT_SOURCE_DIR}/synth/syn_spec.h"
    VERBATIM)

# Engine, runtime stand-ins, the reference and the HLE core, shared by the variants.
add_library(dh_synlib_common OBJECT
    ${DH_CORE}
    "${CMAKE_CURRENT_SOURCE_DIR}/synth/ref_native.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/synth/syn_ref_register.cpp"
    "${DH_SYNLIB_OUT}/syn_nids.h"
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
    "${PROJECT_SOURCE_DIR}/runtime/ppu/ppu_hle.cpp")
target_include_directories(dh_synlib_common PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/synth" "${DH_SYNLIB_OUT}"
    "${PROJECT_SOURCE_DIR}/runtime" "${PROJECT_SOURCE_DIR}/runtime/ppu"
    "${PROJECT_SOURCE_DIR}/libs" "${PROJECT_SOURCE_DIR}/libs/spurs"
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/tests/spurs_jq2")
target_compile_definitions(dh_synlib_common PRIVATE
    PS3RECOMP_TOOLKIT_SHA="${DH_TOOLKIT_SHA}" PS3RECOMP_REPO_ROOT="${PROJECT_SOURCE_DIR}"
    PS3RECOMP_DIFF_SYNTHETIC=1)
dh_host_stubs(dh_synlib_common)

# dh_syn_add_variant(<tag> <source> <c|cxx|asm> <expect-diverge> [CFLAGS ...] [KEEP_TOC_OPT] [JT_GAP])
#   CFLAGS        extra compiler flags after the defaults (a later -O wins)
#   KEEP_TOC_OPT  let the linker rewrite TOC loads into addis/addi (its default)
#   JT_GAP        code generation puts the switch dispatch in a shape the lifter
#                 does not recognise yet (issue #45); the cases that reach it
#                 are declared lifted-blocked rather than silently passing
function(dh_syn_add_variant TAG SRC KIND EXPECT_DIVERGE)
    cmake_parse_arguments(V "KEEP_TOC_OPT;JT_GAP" "" "CFLAGS" ${ARGN})
    set(ld_opts ${DH_SYN_LD_OPTS})
    if(V_KEEP_TOC_OPT)
        list(REMOVE_ITEM ld_opts --no-toc-optimize)
    endif()
    set(D "${DH_SYNLIB_OUT}/${TAG}")
    set(NAME libsyn_${TAG})
    set(PREFIX syn${TAG}_)
    set(SRC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/synth/${SRC}")
    file(MAKE_DIRECTORY "${D}")

    if(KIND STREQUAL "cxx")
        set(compile "${DH_SYN_GXX}" ${DH_SYN_CXXFLAGS} ${V_CFLAGS} -c "${SRC_PATH}" -o "${D}/syn.o")
    elseif(KIND STREQUAL "asm")
        set(compile "${DH_SYN_GCC}" ${DH_SYN_ARCH} -Wa,-mregnames
                    -I "${CMAKE_CURRENT_SOURCE_DIR}/synth" -c "${SRC_PATH}" -o "${D}/syn.o")
    else()
        set(compile "${DH_SYN_GCC}" ${DH_SYN_CFLAGS} ${V_CFLAGS} -c "${SRC_PATH}" -o "${D}/syn.o")
    endif()
    add_custom_command(OUTPUT "${D}/syn.elf"
        COMMAND ${compile}
        COMMAND "${DH_SYN_LD}" -T "${CMAKE_CURRENT_SOURCE_DIR}/synth/syn_gcc.ld"
                --defsym SYN_BASE=${DH_SYN_BASE} ${ld_opts}
                -o "${D}/syn.elf" "${D}/syn.o" "${DH_SYNLIB_OUT}/syn_stubs.o"
        DEPENDS "${SRC_PATH}" "${DH_SYNLIB_OUT}/syn_stubs.o"
                "${CMAKE_CURRENT_SOURCE_DIR}/synth/syn_spec.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/synth/syn_gcc.ld" "${CMAKE_CURRENT_SOURCE_DIR}/synth/syn_c.c"
        VERBATIM)

    add_custom_command(OUTPUT "${D}/${NAME}_image.bin" "${D}/${NAME}_exports.json"
                              "${D}/${NAME}_imports.json" "${D}/${NAME}_functions.json"
                              "${D}/${NAME}_names.json"
        COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/diff_harness/synth_pack.py"
                --manifest "${DH_SYNLIB_MANIFEST}" --elf "${D}/syn.elf" --name ${NAME}
                --base ${DH_SYN_BASE} --out "${D}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tools/diff_harness/synth_pack.py" "${DH_SYNLIB_MANIFEST}"
                "${D}/syn.elf"
        VERBATIM)

    add_custom_command(OUTPUT "${D}/lifted.c" "${D}/lifted.h"
        COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/diff_harness/lift_module.py"
                --meta "${D}" --name ${NAME} --prefix ${PREFIX} --stem lifted --out "${D}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tools/diff_harness/lift_module.py"
                "${PROJECT_SOURCE_DIR}/tools/ppu_lifter.py"
                "${D}/${NAME}_image.bin" "${D}/${NAME}_exports.json" "${D}/${NAME}_functions.json"
        BYPRODUCTS "${D}/ppu_lift_report.json"
        VERBATIM)

    add_custom_command(OUTPUT "${D}/${PREFIX}harness_tables.cpp" "${D}/${PREFIX}harness_layout.cpp"
        COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/diff_harness/gen_tables.py"
                --synthetic --name ${NAME} --prefix ${PREFIX} --header lifted.h
                --names "${D}/${NAME}_names.json" --exports "${D}/${NAME}_exports.json"
                --imports "${D}/${NAME}_imports.json" --image "${D}/${NAME}_image.bin" --out "${D}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tools/diff_harness/gen_tables.py" "${D}/${NAME}_names.json"
                "${D}/${NAME}_exports.json" "${D}/${NAME}_imports.json" "${D}/${NAME}_image.bin"
        VERBATIM)

    # The lifted C uses C++ constructs (extern "C", thread-local g_trampoline_fn).
    set_source_files_properties("${D}/lifted.c" PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-w")
    set_source_files_properties("${D}/${PREFIX}harness_tables.cpp" PROPERTIES COMPILE_OPTIONS "-w")
    add_executable(dh_syn_${TAG} $<TARGET_OBJECTS:dh_synlib_common>
        "${CMAKE_CURRENT_SOURCE_DIR}/synth/syn_cases.cpp"
        "${D}/lifted.c" "${D}/${PREFIX}harness_tables.cpp" "${D}/${PREFIX}harness_layout.cpp")
    dh_common(dh_syn_${TAG})
    target_include_directories(dh_syn_${TAG} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/synth" "${DH_SYNLIB_OUT}" "${D}")
    target_compile_definitions(dh_syn_${TAG} PRIVATE PS3RECOMP_DIFF_SYNTHETIC=1)
    add_dependencies(dh_syn_${TAG} dh_synlib_common)
    if(EXPECT_DIVERGE GREATER 0)
        target_compile_definitions(dh_syn_${TAG} PRIVATE SYN_VARIANT_BUG=1)
    endif()
    if(V_JT_GAP)
        target_compile_definitions(dh_syn_${TAG} PRIVATE SYN_VARIANT_JT_GAP=1)
    endif()
    # min-cases is the table's exact size, repeated here on purpose: editing the
    # table alone cannot make the bug variant's count check vacuous.
    add_test(NAME diff_synth_${TAG}
        COMMAND $<TARGET_FILE:dh_syn_${TAG}> --min-cases 138 --expect-diverge ${EXPECT_DIVERGE}
                --out "${D}/results_${TAG}.json")
    set_tests_properties(diff_synth_${TAG} PROPERTIES LABELS "diff;t0" TIMEOUT 180)
endfunction()

dh_syn_add_variant(c      syn_c.c     c   0)
dh_syn_add_variant(cpp    syn_cpp.cpp cxx 0)
dh_syn_add_variant(asm    syn_asm.S   asm 0)
# The six cases marked `bug` in syn_cases.cpp (init06, init12, cmd60, cmd63,
# seq02, seq03); the number is repeated here so the test cannot go vacuous.
dh_syn_add_variant(cbug   syn_c_bug.c c   6)
# More code generation for the lifter to chew on (same source, same table):
#   -Os  out-of-line register save/restore helpers made by the linker
#   -O3  unrolled loops
dh_syn_add_variant(c_Os   syn_c.c     c   0 CFLAGS -Os)
dh_syn_add_variant(c_O3   syn_c.c     c   0 CFLAGS -O3 -funroll-loops)
# Known lifter gaps (issue #45), declared rather than hidden: GCC's two other
# ways to dispatch a switch.
#   -O0                table base + index added, then lwz rX,0(rY) (no lwzx)
#   -mcmodel=medium    table base made with addis/addi from r2 (Ubuntu default)
dh_syn_add_variant(c_O0     syn_c.c   c   0 CFLAGS -O0 JT_GAP)
dh_syn_add_variant(c_medium syn_c.c   c   0 CFLAGS -mcmodel=medium KEEP_TOC_OPT JT_GAP)
