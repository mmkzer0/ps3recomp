#!/usr/bin/env bash
# Build+run the standalone SPU helper unit tests for one compiler-flag variant.
#   audit_variants.sh <name> <CC> [cflags...]      (set RUNNER="qemu-aarch64 -L /usr/aarch64-linux-gnu" for cross)
# Output: one line per test: <variant> <test> BUILD_FAIL|PASS|SKIP|FAIL rc=<n> <last summary line>
# SKIP = exit 77, the code a test returns when its comparison is compiled out for this arch/flag set
# (CTest's SKIP_RETURN_CODE); it is not a pass.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$HERE/../../.." && pwd)"
NAME="$1"; CC="$2"; shift 2
OUT="${AUDIT_OUT:-/tmp/audit_out}/$NAME"; mkdir -p "$OUT"
TESTS="${TESTS:-test_spu_helpers test_spu_shufb test_spu_numeric_semantics test_spu_decode test_spu_float_neon test_spu_int_neon test_spu_int_x86 test_spu_float_exact test_spu_float_est}"
for t in $TESTS; do
  if ! nice -n 10 $CC -std=gnu17 -O2 -UNDEBUG -w -I"$REPO/include" -I"$REPO/runtime/spu" -I"$REPO/runtime" -I"$REPO/runtime/spu/tests" -I"$REPO" "$@" \
        "$HERE/$t.c" -o "$OUT/$t" -lm >"$OUT/$t.build.log" 2>&1; then
    echo "$NAME $t BUILD_FAIL $(grep -m1 -E 'error' "$OUT/$t.build.log" | cut -c1-110)"; continue
  fi
  ${RUNNER:-} "$OUT/$t" >"$OUT/$t.log" 2>&1; rc=$?
  case $rc in 0) st=PASS;; 77) st=SKIP;; *) st=FAIL;; esac
  echo "$NAME $t $st rc=$rc | $(grep -v '^\s*$' "$OUT/$t.log" | tail -1 | cut -c1-120)"
done
