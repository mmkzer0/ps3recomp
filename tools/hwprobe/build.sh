#!/usr/bin/env bash
# Build one hwprobe layer to a fake-signed SELF inside the build container.
#
#   tools/hwprobe/build.sh <probe> <layer>        e.g.  build.sh ring110 layer1
#
# <probe>/<layer> is the directory tools/hwprobe/probes/<probe>/<layer>/ (its Makefile sets
# TARGET). Outputs go to tools/hwprobe/out/ (gitignored): <TARGET>.fake.self (fself, what to
# copy to the console) and <TARGET>.self (make_self). The SHA-256 of each is printed and
# written to out/SHA256SUMS.
#
# The result files of the probe carry the git describe and a source hash computed here
# (HWP_GIT_DESCRIBE, HWP_SOURCE_SHA256), so a result can be tied back to the sources.
#
# Needs: podman (machine running on macOS), the image and prefix from
# toolchain/build-toolchain.sh. Operator environment: PS3DEV (prefix, default as in
# build-toolchain.sh), HWPROBE_IMAGE (default ps3dev-build).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PS3DEV="${PS3DEV:-$HOME/ps3-dev/ps3dev-toolchain/ps3dev}"
IMG="${HWPROBE_IMAGE:-ps3dev-build}"

die() { echo "build.sh: $*" >&2; exit 1; }

[ $# -eq 2 ] || die "usage: $0 <probe> <layer>   (see probes/*/)"
PROBE="$1"
LAYER="$2"
case "$PROBE$LAYER" in *[!A-Za-z0-9_.-]*) die "probe and layer must be plain directory names" ;; esac
SRC="$HERE/probes/$PROBE/$LAYER"
[ -f "$SRC/Makefile" ] || die "no $SRC/Makefile"
[ -x "$PS3DEV/bin/fself" ] || die "toolchain prefix $PS3DEV is not built; run toolchain/build-toolchain.sh"
command -v podman >/dev/null || die "podman not found"
podman image exists "$IMG" 2>/dev/null || die "podman image '$IMG' is missing or podman cannot be reached (macOS: 'podman machine start'); toolchain/build-toolchain.sh builds the image"

sha256() {
  if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

GIT_DESCRIBE="$(git -C "$HERE" describe --always --dirty --abbrev=12 2>/dev/null || true)"
GIT_DESCRIBE="$(printf '%s' "${GIT_DESCRIBE:-unknown}" | tr -c 'A-Za-z0-9._-' '_')"
# Hash of every file that goes into the binary: the shared library, the build rules and the probe
# (sorted, so the order of the file system does not matter; outputs and results are not part of it).
SRC_SHA="$(cd "$HERE" && LC_ALL=C find common mk "probes/$PROBE" -type f \
  \( -name '*.c' -o -name '*.h' -o -name '*.mk' -o -name Makefile \) -not -path '*/build/*' \
  | LC_ALL=C sort | while read -r f; do sha256 "$f"; done | sha256 | cut -d' ' -f1)"

TARGET="$(grep -E '^TARGET[[:space:]]*:?=' "$SRC/Makefile" | head -1 | cut -d= -f2 | tr -d ' \t')"
[ -n "$TARGET" ] || die "no TARGET in $SRC/Makefile"

in_container() {
  podman run --rm -v "$HERE:$HERE" -v "$PS3DEV:$PS3DEV:ro" -w "$SRC" \
    -e PS3DEV="$PS3DEV" -e PSL1GHT="$PS3DEV" \
    -e PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
    "$IMG" "$@"
}

echo "probe $PROBE $LAYER  target $TARGET  describe $GIT_DESCRIBE  source sha256 $SRC_SHA"
in_container make clean >/dev/null
in_container make HWP_GIT_DESCRIBE="$GIT_DESCRIBE" HWP_SOURCE_SHA256="$SRC_SHA"

OUT="$HERE/out"
mkdir -p "$OUT"
for ext in fake.self self; do
  [ -f "$SRC/$TARGET.$ext" ] || die "build did not produce $SRC/$TARGET.$ext"
  cp -f "$SRC/$TARGET.$ext" "$OUT/$TARGET.$ext"
done
(cd "$OUT" && sha256 *.self > SHA256SUMS)
for ext in fake.self self; do
  printf '%s  %s bytes  ' "$OUT/$TARGET.$ext" "$(wc -c < "$OUT/$TARGET.$ext" | tr -d ' ')"
  sha256 "$OUT/$TARGET.$ext" | cut -d' ' -f1
done
