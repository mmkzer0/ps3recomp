#!/usr/bin/env bash
# Build (or verify) the PPU cross toolchain and PSL1GHT that hwprobe probes need.
#
#   build-toolchain.sh            build what is missing, skip what is done
#   build-toolchain.sh --check    only verify; exit 0 when the prefix is complete and the pins hold
#
# What it builds, inside a native linux/arm64 (or host-arch) podman container so nothing but the
# prefix lands on the host:
#   ps3toolchain  de85d7d5d11dc0a99d823ece3d440eea46c34d0f  steps 001 binutils, 002 gcc + newlib, 004 symlinks
#                 local deviation (patches/ps3toolchain-002-gcc-c-only.patch): gcc is built for C only; the
#                 probes are C and C++ costs about 1 GB and 10 minutes. SPU toolchain, gdb, ps3libraries skipped.
#   PSL1GHT       7e1bd96307eeac8000f18fd75e643d2fa826b5c1   install-ctrl, ppu, tools (no spu, no common)
# The binaries in the prefix are linux binaries: on macOS they run only inside the container (build.sh).
#
# Idempotent: a finished prefix is recognised from its artifacts and left untouched (no podman needed);
# a partial one gets only the missing steps. Sources are never reset or deleted; a source tree on a different
# commit is an error. The one change made to a source tree is applying the checked-in patch.
#
# Operator environment (not runtime behaviour): PS3DEV = install prefix (default
# ~/ps3-dev/ps3dev-toolchain/ps3dev), HWPROBE_TOOLCHAIN_ROOT = directory holding ps3toolchain/ and
# PSL1GHT-src/ (default: the parent of PS3DEV; PS3DEV must live inside it), HWPROBE_IMAGE = image name.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PS3DEV="${PS3DEV:-$HOME/ps3-dev/ps3dev-toolchain/ps3dev}"
ROOT="${HWPROBE_TOOLCHAIN_ROOT:-$(dirname "$PS3DEV")}"
IMG="${HWPROBE_IMAGE:-ps3dev-build}"

PS3TOOLCHAIN_URL=https://github.com/ps3dev/ps3toolchain.git
PS3TOOLCHAIN_PIN=de85d7d5d11dc0a99d823ece3d440eea46c34d0f
PSL1GHT_URL=https://github.com/ps3dev/PSL1GHT.git
PSL1GHT_PIN=7e1bd96307eeac8000f18fd75e643d2fa826b5c1
PATCH="$HERE/patches/ps3toolchain-002-gcc-c-only.patch"
PATCH_TARGET=scripts/002-gcc-newlib-PPU.sh

TC_SRC="$ROOT/ps3toolchain"
PSL_SRC="$ROOT/PSL1GHT-src"

MODE=build
case "${1:-}" in
  "") ;;
  --check) MODE=check ;;
  *) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac

die() { echo "build-toolchain: $*" >&2; exit 1; }
say() { echo "build-toolchain: $*"; }

case "$PS3DEV/" in "$ROOT"/*) ;; *) die "PS3DEV ($PS3DEV) must be inside the toolchain root ($ROOT)" ;; esac

# ---- state probes (host side only, nothing here needs podman) ---------------------------------

have() { [ -e "$PS3DEV/$1" ]; }

step001_done() { have ppu/bin/powerpc64-ps3-elf-ld && have ppu/bin/powerpc64-ps3-elf-as; }
step002_done() { have ppu/bin/powerpc64-ps3-elf-gcc && have ppu/powerpc64-ps3-elf/lib/libc.a; }
step004_done() { have ppu/bin/ppu-gcc && have ppu/ppu; }
psl1ght_done() {
  have ppu_rules && have base_rules && have ppu/include/ppu-types.h &&
    have ppu/lib/liblv2.a && have ppu/lib/librsx.a && have ppu/lib/libgcm_sys.a && have ppu/lib/libaudio.a &&
    have ppu/lib/libsysutil.a && have ppu/lib/libio.a &&
    have bin/fself && have bin/make_self && have bin/sprxlinker && have bin/sfo && have bin/pkg
}

# pin_state DIR PIN -> ok | absent | wrong:<head>
pin_state() {
  if [ ! -d "$1/.git" ]; then echo absent; return; fi
  local head
  head="$(git -C "$1" rev-parse HEAD)"
  if [ "$head" = "$2" ]; then echo ok; else echo "wrong:$head"; fi
}

# patch_state -> applied | pending | conflict (the ps3toolchain tree must exist)
patch_state() {
  if git -C "$TC_SRC" apply --check --reverse "$PATCH" 2>/dev/null; then echo applied
  elif git -C "$TC_SRC" apply --check "$PATCH" 2>/dev/null; then echo pending
  else echo conflict; fi
}

problems=()
note() { problems+=("$1"); }

verify() {
  problems=()
  [ -f "$PATCH" ] || die "missing $PATCH"
  local s
  s="$(pin_state "$TC_SRC" "$PS3TOOLCHAIN_PIN")"
  case "$s" in
    ok)
      case "$(patch_state)" in
        applied) ;;
        pending) note "ps3toolchain: the C-only patch is not applied" ;;
        *) note "ps3toolchain: $PATCH_TARGET is neither stock nor patched" ;;
      esac ;;
    absent) say "ps3toolchain sources not present under $TC_SRC (only needed to build)" ;;
    wrong:*) die "$TC_SRC is at ${s#wrong:}, the pin is $PS3TOOLCHAIN_PIN; refusing to build or trust it" ;;
  esac
  s="$(pin_state "$PSL_SRC" "$PSL1GHT_PIN")"
  case "$s" in
    ok|absent) [ "$s" = absent ] && say "PSL1GHT sources not present under $PSL_SRC (only needed to build)" ;;
    wrong:*) die "$PSL_SRC is at ${s#wrong:}, the pin is $PSL1GHT_PIN; refusing to build or trust it" ;;
  esac
  step001_done || note "step 001 (binutils) not done"
  step002_done || note "step 002 (gcc + newlib) not done"
  step004_done || note "step 004 (ppu- symlinks) not done"
  psl1ght_done || note "PSL1GHT (install-ctrl, ppu, tools) not complete"
}

verify
if [ "${#problems[@]}" -eq 0 ]; then
  say "prefix $PS3DEV is complete, pins hold: nothing to do"
  exit 0
fi
if [ "$MODE" = check ]; then
  printf 'build-toolchain: incomplete: %s\n' "${problems[@]}" >&2
  exit 1
fi
printf 'build-toolchain: to do: %s\n' "${problems[@]}"

# ---- build ------------------------------------------------------------------------------------

command -v git >/dev/null || die "git not found"
command -v podman >/dev/null || die "podman not found"
podman info >/dev/null 2>&1 || die "podman cannot be reached (macOS: 'podman machine start')"
mkdir -p "$PS3DEV"

fetch() { # DIR URL PIN
  if [ ! -d "$1/.git" ]; then
    git clone "$2" "$1"
    git -C "$1" checkout --detach "$3"
  fi
  [ "$(git -C "$1" rev-parse HEAD)" = "$3" ] || die "$1 is not at $3"
}
fetch "$TC_SRC" "$PS3TOOLCHAIN_URL" "$PS3TOOLCHAIN_PIN"
fetch "$PSL_SRC" "$PSL1GHT_URL" "$PSL1GHT_PIN"
case "$(patch_state)" in
  applied) ;;
  pending) git -C "$TC_SRC" apply "$PATCH"; say "applied $(basename "$PATCH")" ;;
  *) die "$TC_SRC/$PATCH_TARGET does not match the stock file or the patched one" ;;
esac

podman image exists "$IMG" 2>/dev/null || { say "building image $IMG"; podman build -t "$IMG" -f "$HERE/Containerfile" "$HERE"; }

in_container() { # WORKDIR COMMAND
  podman run --rm -v "$ROOT:$ROOT" -e PS3DEV="$PS3DEV" -e PSL1GHT="$PS3DEV" \
    -e PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
    -w "$1" "$IMG" bash -c "$2"
}

step001_done || { say "step 001: binutils"; in_container "$TC_SRC" './toolchain.sh 1'; }
step002_done || { say "step 002: gcc + newlib (C only)"; in_container "$TC_SRC" './toolchain.sh 2'; }
step004_done || { say "step 004: symlinks"; in_container "$TC_SRC" './toolchain.sh 4'; }
psl1ght_done || {
  say "PSL1GHT"
  in_container "$PSL_SRC" 'make install-ctrl && make -C ppu && make -C ppu install && make -C tools && make -C tools install'
}

verify
[ "${#problems[@]}" -eq 0 ] || { printf 'build-toolchain: still incomplete: %s\n' "${problems[@]}" >&2; exit 1; }
say "prefix $PS3DEV is complete"
