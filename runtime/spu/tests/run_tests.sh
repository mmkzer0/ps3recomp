#!/usr/bin/env bash
# Build and run every integration test in this directory.
#
# Each test is a pair of files:
#   gen_test_<name>.py   - encodes the SPU program and wraps it in an ELF.
#   test_<name>_main.c   - harness with channel-overriding stubs and asserts.
#
# The harnesses provide their own channel and dispatch entry points, so the
# runtime is compiled once with those definitions renamed out of the way
# (RT_RENAMES) and the resulting objects are linked into every fixture. The
# fixtures also place scratch data below EA 0x10000, which the runtime's
# null-page PUT guard rejects, so the runs set SPU_DMA_LAX=1; their transfers
# are otherwise well formed.
#
# Usage: ./run_tests.sh [<C compiler>]
# Honours $CC / $GCC, $CXX and $PYTHON.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
TOOLS="$REPO/tools"
RUNTIME_SPU="$REPO/runtime/spu"

GCC="${1:-${GCC:-${CC:-clang}}}"
CXX="${CXX:-c++}"
PYTHON="${PYTHON:-python3}"

command -v "$PYTHON" >/dev/null || {
    echo "error: $PYTHON not found (set PYTHON=...)" >&2
    exit 2
}

# Build the runtime once. spu_channels.c supplies the real channel engine, whose
# entry points the fixtures replace, so its copies are renamed; spu_workload.c
# defines g_cri_video_dma, which the fixtures define too (spu_dma.h's inline
# writes it in every TU), so the runtime copy is renamed as well.
RT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/spu-tests-runtime.XXXXXX")
cleanup()
{
    if command -v trash >/dev/null 2>&1; then
        trash "$RT_DIR"
    elif command -v trash-put >/dev/null 2>&1; then
        trash-put "$RT_DIR"
    elif command -v gio >/dev/null 2>&1; then
        gio trash "$RT_DIR"
    else
        printf '[spu-tests] temporary runtime objects retained at %s (no trash utility)\n' "$RT_DIR" >&2
    fi
}
trap cleanup EXIT
RTCFLAGS=(-std=gnu17 -O2 -w -pthread -I "$RUNTIME_SPU" -I "$REPO/include"
          -I "$REPO/runtime/platform")
RT_RENAMES=(-Dspu_rdch=rt_spu_rdch -Dspu_rchcnt=rt_spu_rchcnt
            -Dspu_wrch=rt_spu_wrch -Dspu_halt=rt_spu_halt -Dspu_stop=rt_spu_stop
            -Dspu_register_function=rt_spu_register_function
            -Dspu_indirect_branch=rt_spu_indirect_branch)
for src in spu_channels.c spu_drain.c spu_lockstep.c spu_coherency.c spu_interp.c \
           spu_workload.c spurs_policy.c spurs_job.c spu_tsp_weak.c spu_vm_pagemap.c \
           spurs_policy_blob_weak.c; do
    extra=()
    if [ "$src" = spu_channels.c ]; then extra=("${RT_RENAMES[@]}"); fi
    if [ "$src" = spu_workload.c ]; then extra=(-Dg_cri_video_dma=rt_g_cri_video_dma); fi
    "$GCC" "${RTCFLAGS[@]}" "${extra[@]}" -c "$RUNTIME_SPU/$src" -o "$RT_DIR/${src%.c}.o"
done
"$GCC" "${RTCFLAGS[@]}" -c "$REPO/runtime/memory/reservation.c" -o "$RT_DIR/reservation.o"
"$GCC" "${RTCFLAGS[@]}" -c "$REPO/runtime/memory/label_window.c" -o "$RT_DIR/label_window.o"
"$GCC" "${RTCFLAGS[@]}" -c "$REPO/runtime/platform/win32_compat.c" -o "$RT_DIR/win32_compat.o"
"$GCC" "${RTCFLAGS[@]}" -c "$REPO/runtime/platform/host_clock.c" -o "$RT_DIR/host_clock.o"
"$GCC" "${RTCFLAGS[@]}" -c "$REPO/runtime/platform/host_wait.c" -o "$RT_DIR/host_wait.o"
"$GCC" "${RTCFLAGS[@]}" -c "$HERE/test_runtime_stubs.c" -o "$RT_DIR/test_runtime_stubs.o"
"$CXX" -std=c++20 -O2 -I "$REPO/include" -c "$REPO/runtime/config/config.cpp" -o "$RT_DIR/config.o"
"$CXX" -std=c++20 -O2 -I "$REPO/include" -c "$REPO/runtime/diag/stop.cpp" -o "$RT_DIR/stop.o"
"$GCC" "${RTCFLAGS[@]}" -c "$REPO/runtime/diag/lifter_abi.c" -o "$RT_DIR/lifter_abi.o"

pass=0
fail=0
failed_tests=()

for gen in "$HERE"/gen_test_*.py; do
    name=$(basename "$gen" .py)
    name=${name#gen_test_}
    main="$HERE/test_${name}_main.c"
    [ -f "$main" ] || { echo "[skip] $name: no test_${name}_main.c"; continue; }

    elf="$HERE/test_${name}.elf"
    # Generated C stays checked in at out_<name>; compiler objects and binaries
    # live under RT_DIR so re-running the producer never removes user files.
    gen_out="$HERE/out_${name}"
    obj_dir="$RT_DIR/$name"
    exe="$RT_DIR/test_${name}.exe"
    "$PYTHON" "$gen" > /dev/null
    "$PYTHON" "$TOOLS/spu_lifter.py" --auto-functions "$elf" \
        --output "$gen_out" > /dev/null

    # Optional per-test extra build args (extra sources / -I dirs). A
    # test_<name>.deps file is sourced with $REPO/$RUNTIME_SPU in scope and may
    # set EXTRA="..." (e.g. to link a libs/ HLE module like cellSync.c).
    EXTRA=""
    [ -f "$HERE/test_${name}.deps" ] && . "$HERE/test_${name}.deps"
    mkdir "$obj_dir"
    if ( cd "$obj_dir" &&
         "$GCC" -std=gnu17 -O2 -w -I "$gen_out" -I "$RUNTIME_SPU" \
             -I "$REPO/include" -I "$REPO/runtime/platform" -I "$REPO/libs/sync" \
             -c "$gen_out/spu_recomp.c" "$main" $EXTRA &&
         "$CXX" ./*.o "$RT_DIR"/*.o \
             -pthread -lm -o "$exe" ) 2> "$HERE/build_${name}.log"; then
        if out=$(SPU_DMA_LAX=1 "$exe" 2>&1); then rc=0; else rc=$?; fi
        if [ "$rc" -eq 0 ]; then
            echo "[PASS] $name -- $out"
            pass=$((pass+1))
        else
            echo "[FAIL] $name (exit $rc)"
            while IFS= read -r line; do
                printf '   %s\n' "$line"
            done <<< "$out"
            failed_tests+=("$name")
            fail=$((fail+1))
        fi
    else
        echo "[FAIL] $name (build error -- see build_${name}.log)"
        failed_tests+=("$name")
        fail=$((fail+1))
    fi
done

echo
echo "==========================================="
echo "Results: $pass passed, $fail failed"
[ $fail -eq 0 ] || { echo "Failed: ${failed_tests[*]}"; exit 1; }
