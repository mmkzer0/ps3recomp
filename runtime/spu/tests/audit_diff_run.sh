#!/usr/bin/env bash
# Run the already-built differential-test binaries (build_diff_test.sh, or DIFF_OUT=...) through the float referee.
#   audit_diff_run.sh <build_diff dir> [scale] [seed]
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$1"; shift
rc=0
for mode in interp lifted; do
  bin="$OUT/spu_interp_diff_test"; flag=""; [ $mode = lifted ] && { bin="$OUT/spu_lifted_diff_test"; flag=--lifted; }
  SPU_DIFF_SHOW=1000000 nice -n 10 "$bin" $flag "$@" > "$OUT/run_$mode.log" 2>&1; echo "[$mode] binary rc=$?"
  grep -v "MISMATCH\|operands:" "$OUT/run_$mode.log" | tail -4
  python3 "$HERE/spu_float_referee.py" "$OUT/run_$mode.log" > "$OUT/referee_$mode.log" 2>&1; r=$?; tail -3 "$OUT/referee_$mode.log"
  echo "[$mode] referee rc=$r"; [ $r = 0 ] || rc=1
done
echo "audit (hand ref vs RPCS3):"; nice -n 10 "$OUT/spu_interp_diff_test" --audit 2>&1 | tail -8
[ $rc = 0 ] && echo "SPU differential test: PASS" || echo "SPU differential test: FAIL"
exit $rc
