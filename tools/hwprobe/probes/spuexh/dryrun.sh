#!/usr/bin/env bash
# Dry run of the spuexh probe in the toolkit's SPU interpreter (no console), compared with the host harness' hashes.
#
#   dryrun.sh <preset|cfg-file> <out-dir> [--set key=val]...
#
# Builds the SPU ELF (spu-gcc from $PS3DEV) and the host driver, runs the configuration one domain per process (so
# several cores are used and the interpreter's instruction count is known per domain), merges the result files into
# <out-dir>/dryrun.txt and, if $SCALAR points at the host harness' scalar.txt, compares. Environment:
#   PS3DEV   toolchain prefix with spu/bin/spu-gcc          JOBS  parallel processes (default 3)
#   SCALAR   host harness reference (scalar.txt)            CC    host C compiler (default cc)
# Every process runs under nice -n 10. The merged file is a result file in the format of FORMAT.md.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
CFG="${1:?usage: dryrun.sh <preset|cfg-file> <out-dir> [--set key=val]...}"
OUT="${2:?usage: dryrun.sh <preset|cfg-file> <out-dir>}"
shift 2
: "${PS3DEV:?set PS3DEV to the toolchain prefix (build-toolchain.sh --with-spu)}"
CC="${CC:-cc}"; JOBS="${JOBS:-3}"
mkdir -p "$OUT/dom"
"$HERE/prebuild.sh" >/dev/null
( cd "$HERE/spu" && PSL1GHT="$PS3DEV" PS3DEV="$PS3DEV" make >/dev/null )
ELF="$HERE/spu/spuexh.elf"
INC=(-I "$REPO/runtime/spu" -I "$REPO/include" -I "$REPO/runtime/platform" -I "$HERE/common" -I "$HERE/generated")
HOST="$OUT/spuexh_host"
$CC -std=gnu11 -O2 -w -pthread "${INC[@]}" -c "$HERE/host/host_interp.c" -o "$OUT/host_interp.o"
$CC -std=gnu11 -O2 -w -pthread "${INC[@]}" -c "$HERE/host/spuexh_host.c" -o "$OUT/spuexh_host.o"
$CC -std=gnu11 -O2 -w "${INC[@]}" -c "$HERE/common/spuexh_core.c" -o "$OUT/core.o"
$CC "$OUT"/host_interp.o "$OUT"/spuexh_host.o "$OUT"/core.o -pthread -lm -o "$HOST"

if [ -f "$CFG" ]; then SEL=(--cfg "$CFG"); else SEL=(--preset "$CFG"); fi
DOMS="$("$HOST" "${SEL[@]}" "$@" --plan | awk '/^dom /{print $2}')"
echo "dry run: $(echo $DOMS | wc -w) domains, $JOBS processes"
# the DP compares have no special probe in the per-domain runs: force them (the special probes are run once, below)
printf '%s\n' $DOMS | xargs -P "$JOBS" -I{} nice -n 10 "$HOST" --elf "$ELF" "${SEL[@]}" "$@" \
    --set doms={} --set special_a=0 --set special_b=0 --set force_dpcmp=1 --set spus=1 --out "$OUT/dom/{}.txt"
python3 "$HERE/merge_dryrun.py" "$OUT/dom" "$OUT/dryrun.txt"
if [ -n "${SCALAR:-}" ]; then
  python3 "$HERE/compare_spuexh.py" "$OUT/dryrun.txt" --scalar "$SCALAR" --md "$OUT/dryrun_vs_scalar.md" | sed -n '1,/^\*\*/p' | tail -4 || true
fi
