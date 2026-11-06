#!/usr/bin/env bash
# Host-side step of the spuexh build (run by build.sh before the container, from anywhere):
#   - copies the portable generator header into generated/ (the container sees only tools/hwprobe, not runtime/)
#   - generates the op table, the undecoded-prefix list and the raw-exec routine from the repository's own tables
# Needs python3. Outputs are build products: generated/ is not tracked.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
OUT="$HERE/generated"
mkdir -p "$OUT"
cp -f "$REPO/runtime/spu/tests/spu_exh_gen.h" "$OUT/spu_exh_gen.h"
python3 "$HERE/gen_tables.py" "$REPO" "$OUT"
