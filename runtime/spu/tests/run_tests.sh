#!/usr/bin/env bash
# Build and run every integration test in this directory.
#
# Each test is a pair of files:
#   gen_test_<name>.py   - encodes the SPU program and wraps it in an ELF.
#   test_<name>_main.c   - harness with channel-overriding stubs and asserts.
#
# Usage: ./run_tests.sh  [<gcc-path>]
# Honours $GCC env var.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
TOOLS="$REPO/tools"
RUNTIME_SPU="$REPO/runtime/spu"

GCC="${1:-${GCC:-gcc}}"
CXX="${CXX:-c++}"

# spu_dma.h calls the typed configuration (runtime/config/config.cpp) and the
# named-stop runtime (runtime/diag/stop.cpp) directly, so every fixture that
# inlines mfc_submit() needs both: they are strong references, not weak hooks.
# Build them once, as C++, and link them into every fixture.
RT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/spu-tests-runtime.XXXXXX")
trap 'rm -rf "$RT_DIR"' EXIT
"$CXX" -std=c++20 -O2 -I "$REPO/include" -c "$REPO/runtime/config/config.cpp" \
    -o "$RT_DIR/config.o"
"$CXX" -std=c++20 -O2 -I "$REPO/include" -c "$REPO/runtime/diag/stop.cpp" \
    -o "$RT_DIR/stop.o"

pass=0
fail=0
failed_tests=()

for gen in "$HERE"/gen_test_*.py; do
    name=$(basename "$gen" .py)
    name=${name#gen_test_}
    main="$HERE/test_${name}_main.c"
    [ -f "$main" ] || { echo "[skip] $name: no test_${name}_main.c"; continue; }

    elf="$HERE/test_${name}.elf"
    # Use an `out_` prefix for generated dirs so `gen_test_*.py` and
    # `out_*` never collide under a `rm -rf gen_*` glob.
    gen_out="$HERE/out_${name}"
    exe="$HERE/test_${name}.exe"

    rm -rf "$gen_out"
    python "$gen" > /dev/null
    python "$TOOLS/spu_lifter.py" --auto-functions "$elf" \
        --output "$gen_out" > /dev/null

    # Optional per-test extra build args (extra sources / -I dirs). A
    # test_<name>.deps file is sourced with $REPO/$RUNTIME_SPU in scope and may
    # set EXTRA="..." (e.g. to link a libs/ HLE module like cellSync.c).
    EXTRA=""
    [ -f "$HERE/test_${name}.deps" ] && . "$HERE/test_${name}.deps"

    if ( cd "$gen_out" &&
         "$GCC" -std=c11 -O2 -I "$gen_out" -I "$RUNTIME_SPU" \
             -c "$gen_out/spu_recomp.c" "$main" $EXTRA &&
         "$CXX" "$gen_out"/*.o "$RT_DIR/config.o" "$RT_DIR/stop.o" \
             -pthread -lm -o "$exe" ) 2> "$HERE/build_${name}.log"; then
        out=$("$exe" 2>&1) && rc=$? || rc=$?
        if [ "$rc" -eq 0 ]; then
            echo "[PASS] $name -- $out"
            pass=$((pass+1))
        else
            echo "[FAIL] $name (exit $rc)"
            echo "$out" | sed 's/^/   /'
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
