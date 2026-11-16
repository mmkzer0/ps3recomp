#!/usr/bin/env bash
# SPU operation benchmark: operations per second for every SPU operation,
# lifted and interpreted, built against the real SPU runtime.
#
#   tests/bench/spu/run_spu_bench.sh [--no-barriers] [harness args...]
#     harness args: --quick  --only fa,fm  --no-interp  --csv FILE  --repeats N
#
# Generated files go to $SPU_BENCH_OUT (default: tests/bench/spu/out).
# The lifted kernels are compiled with the flags the game compiles lifted SPU
# code with (-O2 -fwrapv).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="${SPU_BENCH_OUT:-$HERE/out}"
CC="${CC:-clang}"
CXX="${CXX:-c++}"
PYTHON="${PYTHON:-python3}"
BARRIERS=1
if [ "${1:-}" = "--no-barriers" ]; then BARRIERS=0; shift; fi

mkdir -p "$OUT"
"$PYTHON" "$HERE/gen_spu_bench.py" --out "$OUT"
"$PYTHON" "$REPO/tools/spu_lifter.py" --auto-functions "$OUT/bench.elf" --output "$OUT/lift" > /dev/null
if [ $BARRIERS = 1 ]; then
    "$PYTHON" "$HERE/apply_barriers.py" "$OUT/lift/spu_recomp.c" "$OUT/bench_cases.barriers"
else
    echo "barriers: none (the compiler may fold or hoist repeated ops)"
fi

S="$REPO/runtime/spu"
FLAGS=(-std=gnu17 -O2 -fwrapv -w -I "$S" -I "$REPO/include" -I "$REPO/runtime/platform" -I "$OUT")
"$CC" "${FLAGS[@]}" -include "$HERE/bench_barrier.h" -c "$OUT/lift/spu_recomp.c" -o "$OUT/lifted.o"
# spu_dma.h calls ps3_cfg_* and ps3_stop directly (strong references), so the
# benchmark must link the typed configuration and the named-stop runtime.
C_SOURCES=("$HERE/spu_bench_main.c" "$S/spu_channels.c" "$S/spu_drain.c" "$S/spu_lockstep.c"
    "$S/spu_coherency.c" "$REPO/runtime/memory/reservation.c" "$REPO/runtime/memory/label_window.c"
    "$S/spu_workload.c" "$S/spurs_policy.c" "$S/spurs_job.c"
    "$S/spu_tsp_weak.c" "$S/spu_vm_pagemap.c" "$S/spurs_policy_blob_weak.c"
    "$S/spu_interp.c" "$REPO/runtime/platform/win32_compat.c"
    "$REPO/runtime/platform/host_clock.c" "$REPO/runtime/platform/host_wait.c"
    "$REPO/runtime/diag/lifter_abi.c" "$REPO/runtime/diag/ps3_log.c" "$REPO/runtime/diag/host_exit.c")
for src in "${C_SOURCES[@]}"; do
    "$CC" "${FLAGS[@]}" -c "$src" -o "$OUT/$(basename "${src%.c}").o"
done
"$CXX" -std=c++20 -O2 -I "$REPO/include" -c "$REPO/runtime/config/config.cpp" -o "$OUT/config.o"
"$CXX" -std=c++20 -O2 -I "$REPO/include" -c "$REPO/runtime/diag/stop.cpp" -o "$OUT/stop.o"
OBJECTS=("$OUT/lifted.o" "$OUT/config.o" "$OUT/stop.o")
for src in "${C_SOURCES[@]}"; do OBJECTS+=("$OUT/$(basename "${src%.c}").o"); done
"$CXX" -o "$OUT/spu_bench" "${OBJECTS[@]}" -lpthread -lm
"$OUT/spu_bench" "$@"
