#!/usr/bin/env bash
# Build test_spu_exhaustive in every backend variant from the one source file.
#   build_exhaustive.sh [OUTDIR]        (default: ./build_exh next to this script)
# Variants (the scalar build is the reference every other build must match bit-exactly):
#   scalar   clang, baseline x86-64, -DSPU_SCALAR_HELPERS -DSPU_SCALAR_INT   (no SIMD fast path anywhere)
#   sse2     clang, baseline x86-64 (SSE2 only: float compares are SSE2, integer ops stay scalar -- SPU_X86 needs SSE4.1)
#   sse41    clang, -march=x86-64-v2   (SPU_X86 integer SIMD without AVX2; scalar shl/rot/rotm/rotma, scalar float)
#   avx2     clang, -march=x86-64-v3   (AVX2 float fast path, estimates/conversions, variable shifts)
#   avx512   clang, -march=x86-64-v4   (same sources, AVX-512 codegen for the loops / intrinsics)
#   neon     aarch64-linux-gnu-gcc, static, run under qemu-aarch64 (tier QEMU)
# Extras for compiler-dependence checks: scalar-gcc, avx2-gcc, avx512-gcc, sse41-gcc.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/build_exh}"; mkdir -p "$OUT"
SRC="$HERE/test_spu_exhaustive.c"; INC="-I$HERE/.. -I$HERE/../../../include"
CF="-std=gnu11 -O2 -Wall -Wno-unused-function -Wno-unused-variable $INC"
build() { local name="$1"; shift; echo "build $name"; "$@" $SRC -lm -o "$OUT/test_spu_exhaustive.$name"; }
build scalar  clang $CF -DSPU_SCALAR_HELPERS -DSPU_SCALAR_INT
build sse2    clang $CF
build sse41   clang $CF -march=x86-64-v2
build avx2    clang $CF -march=x86-64-v3
build avx512  clang $CF -march=x86-64-v4
if command -v aarch64-linux-gnu-gcc >/dev/null; then
  build neon  aarch64-linux-gnu-gcc $CF -static
fi
if [ "${EXTRAS:-1}" = 1 ]; then
  build scalar-gcc gcc $CF -DSPU_SCALAR_HELPERS -DSPU_SCALAR_INT
  build sse41-gcc  gcc $CF -march=x86-64-v2
  build avx2-gcc   gcc $CF -march=x86-64-v3
  build avx512-gcc gcc $CF -march=x86-64-v4
fi
ls -l "$OUT"
