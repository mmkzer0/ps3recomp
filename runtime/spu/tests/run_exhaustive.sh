#!/usr/bin/env bash
# Run the exhaustive sweep for the given build variants, then compare each against the scalar reference.
#   run_exhaustive.sh [-b BINDIR] [-o HASHDIR] [-j JOBS] variant[:key=val,...] ...
#   variants: scalar sse2 sse41 avx2 avx512 neon neon+rz scalar-gcc sse41-gcc avx2-gcc avx512-gcc
# Per-variant knobs (key=val after the colon, or env STRIDE_<VARIANT>=...):
#   heavy=S  block stride for the 44 non-nominal-scale cflts/cfltu/csflt/cuflt@N ops (default 8)
#   all=S    block stride for every op (default 1 = every block)
#   shards=N split the op list over N parallel processes (default 1)
#   sdom=A=S/B=S  per-domain block stride (overrides all=)
#   ops=...  op filter (comma list, trailing * = prefix)
# The scalar reference must be run FIRST (or already exist as HASHDIR/scalar.txt). Everything runs under nice -n10.
# Only blocks present in both files are compared, so a strided build is checked on its subset.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/build_exh"; OUT="$HERE/exh_hashes"; JOBS=2
while getopts "b:o:j:" o; do case $o in b) BIN="$OPTARG";; o) OUT="$OPTARG";; j) JOBS="$OPTARG";; esac; done
shift $((OPTIND-1)); mkdir -p "$OUT"
SYSROOT=/usr/aarch64-linux-gnu
declare -a JOBLIST
for spec in "$@"; do
  v="${spec%%:*}"; kv=""; [ "$spec" != "$v" ] && kv="${spec#*:}"
  heavy=8; all=1; shards=1; ops=""; sdom=""; refcheck="--refcheck"
  [ "$v" = scalar ] || [ "$v" = scalar-gcc ] && refcheck=""
  IFS=',' read -ra KV <<< "$kv"
  for p in "${KV[@]}"; do case "$p" in heavy=*) heavy="${p#heavy=}";; all=*) all="${p#all=}";; shards=*) shards="${p#shards=}";; ops=*) ops="${p#ops=}"; ops="${ops//\//,}";; sdom=*) sdom="${p#sdom=}";; esac; done
  exe="$BIN/test_spu_exhaustive.${v%%+*}"; extra=""; pre=""
  case "$v" in neon*) pre="qemu-aarch64 -L $SYSROOT";; esac
  case "$v" in *+rz) extra="--fpcr-rz";; esac
  [ -n "$ops" ] && extra="$extra --ops $ops"
  if [ -n "$sdom" ]; then IFS='/' read -ra SD <<< "$sdom"; for q in "${SD[@]}"; do extra="$extra --stride-dom $q"; done; fi
  for ((k=0;k<shards;k++)); do
    f="$OUT/$v.shard$k.txt"
    JOBLIST+=("nice -n10 $pre $exe --tag $v $extra $refcheck --stride-heavy $heavy --stride-all $all --shard $k/$shards --out $f")
  done
done
printf '%s\n' "${JOBLIST[@]}" | xargs -P "$JOBS" -I{} bash -c '{}'
for spec in "$@"; do
  v="${spec%%:*}"; cat "$OUT/$v".shard*.txt > "$OUT/$v.txt" 2>/dev/null
done
ref="$OUT/scalar.txt"
for spec in "$@"; do
  v="${spec%%:*}"; [ "$v" = scalar ] && continue
  echo "=== $v"; python3 "$HERE/compare_exhaustive.py" "$ref" "$OUT/$v.txt" --bindir "$BIN" --variant "$v" | tee "$OUT/$v.compare.txt"
done
