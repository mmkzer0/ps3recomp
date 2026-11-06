#!/usr/bin/env bash
# Build (or verify) the PPU cross toolchain and PSL1GHT that hwprobe probes need.
#
#   build-toolchain.sh            build what is missing, skip what is done
#   build-toolchain.sh --check    only verify; exit 0 when the prefix is complete and the pins hold
#   build-toolchain.sh --with-spu add the SPU part (see below); combinable with --check
#   build-toolchain.sh --spu-only like --with-spu but without requiring/building the PPU part
#   build-toolchain.sh --native   run the steps directly on this host instead of in podman. Only for a
#                                 linux host that matches the container (Ubuntu 24.04 with the packages of
#                                 the Containerfile installed); HWPROBE_NATIVE=1 does the same.
#                                 HWPROBE_JOBS=N caps make -j in that mode.
#
# What it builds, inside a native linux/arm64 (or host-arch) podman container so nothing but the
# prefix lands on the host:
#   ps3toolchain  de85d7d5d11dc0a99d823ece3d440eea46c34d0f  steps 001 binutils, 002 gcc + newlib, 004 symlinks
#                 local deviation (patches/ps3toolchain-002-gcc-c-only.patch): gcc is built for C only; the
#                 probes are C and C++ costs about 1 GB and 10 minutes. SPU toolchain, gdb, ps3libraries skipped.
#   PSL1GHT       7e1bd96307eeac8000f18fd75e643d2fa826b5c1   install-ctrl, ppu, tools (no spu, no common)
# Opt-in SPU part (--with-spu), same pins, same idempotency:
#   ps3toolchain  steps 005 binutils 2.22 (target spu), 006 gcc 9.5.0 + newlib 1.20.0 (spu-gcc; the last gcc with
#                 an SPU backend). Local deviations (patches/ps3toolchain-006-gcc-spu.patch): C only, https for the gcc prerequisites (the stock script uses ftp://).
#   PSL1GHT       spu: install-ctrl, then make -C spu and make -C spu install (libsputhread, libspudma,
#                 libspuatomic and the headers; no mars, no samples)
# The default behaviour (no flag) is unchanged: no SPU toolchain is built or required.
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
PATCH_SPU="$HERE/patches/ps3toolchain-006-gcc-spu.patch"
PATCH_SPU_TARGET=scripts/006-gcc-newlib-SPU.sh

TC_SRC="$ROOT/ps3toolchain"
PSL_SRC="$ROOT/PSL1GHT-src"

MODE=build
WITH_SPU=0
WITH_PPU=1
NATIVE="${HWPROBE_NATIVE:-0}"
for arg in "$@"; do
  case "$arg" in
    --check) MODE=check ;;
    --with-spu) WITH_SPU=1 ;;
    --spu-only) WITH_SPU=1; WITH_PPU=0 ;;
    --native) NATIVE=1 ;;
    *) echo "usage: $0 [--check] [--with-spu | --spu-only] [--native]" >&2; exit 2 ;;
  esac
done

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

step005_done() { have spu/bin/spu-ld && have spu/bin/spu-as; }
step006_done() { have spu/bin/spu-gcc && have spu/spu/lib/libc.a; }
psl1ght_spu_done() {
  have spu_rules && have spu/include && have spu/lib/libsputhread.a && have spu/lib/libspudma.a &&
    have spu/lib/libspuatomic.a
}

# patch_state [PATCH] -> applied | pending | conflict (the ps3toolchain tree must exist)
patch_state() {
  local p="${1:-$PATCH}"
  if git -C "$TC_SRC" apply --check --reverse "$p" 2>/dev/null; then echo applied
  elif git -C "$TC_SRC" apply --check "$p" 2>/dev/null; then echo pending
  else echo conflict; fi
}

problems=()
note() { problems+=("$1"); }

verify() {
  problems=()
  [ -f "$PATCH" ] || die "missing $PATCH"
  [ -f "$PATCH_SPU" ] || die "missing $PATCH_SPU"
  local s
  s="$(pin_state "$TC_SRC" "$PS3TOOLCHAIN_PIN")"
  case "$s" in
    ok)
      if [ "$WITH_PPU" = 1 ]; then
        case "$(patch_state)" in
          applied) ;;
          pending) note "ps3toolchain: the C-only patch is not applied" ;;
          *) note "ps3toolchain: $PATCH_TARGET is neither stock nor patched" ;;
        esac
      fi
      if [ "$WITH_SPU" = 1 ]; then
        case "$(patch_state "$PATCH_SPU")" in
          applied) ;;
          pending) note "ps3toolchain: the SPU C-only patch is not applied" ;;
          *) note "ps3toolchain: $PATCH_SPU_TARGET is neither stock nor patched" ;;
        esac
      fi ;;
    absent) say "ps3toolchain sources not present under $TC_SRC (only needed to build)" ;;
    wrong:*) die "$TC_SRC is at ${s#wrong:}, the pin is $PS3TOOLCHAIN_PIN; refusing to build or trust it" ;;
  esac
  s="$(pin_state "$PSL_SRC" "$PSL1GHT_PIN")"
  case "$s" in
    ok|absent) [ "$s" = absent ] && say "PSL1GHT sources not present under $PSL_SRC (only needed to build)" ;;
    wrong:*) die "$PSL_SRC is at ${s#wrong:}, the pin is $PSL1GHT_PIN; refusing to build or trust it" ;;
  esac
  if [ "$WITH_PPU" = 1 ]; then
    step001_done || note "step 001 (binutils) not done"
    step002_done || note "step 002 (gcc + newlib) not done"
    step004_done || note "step 004 (ppu- symlinks) not done"
    psl1ght_done || note "PSL1GHT (install-ctrl, ppu, tools) not complete"
  fi
  if [ "$WITH_SPU" = 1 ]; then
    step005_done || note "step 005 (binutils, spu) not done"
    step006_done || note "step 006 (gcc + newlib, spu) not done"
    psl1ght_spu_done || note "PSL1GHT spu (libsputhread, libspudma, libspuatomic, headers) not complete"
  fi
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
if [ "$NATIVE" != 1 ]; then
  command -v podman >/dev/null || die "podman not found (--native builds directly on a matching linux host)"
  podman info >/dev/null 2>&1 || die "podman cannot be reached (macOS: 'podman machine start')"
fi
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
apply_patch() { # PATCH TARGET
  case "$(patch_state "$1")" in
    applied) ;;
    pending) git -C "$TC_SRC" apply "$1"; say "applied $(basename "$1")" ;;
    *) die "$TC_SRC/$2 does not match the stock file or the patched one" ;;
  esac
}
[ "$WITH_PPU" = 1 ] && apply_patch "$PATCH" "$PATCH_TARGET"
[ "$WITH_SPU" = 1 ] && apply_patch "$PATCH_SPU" "$PATCH_SPU_TARGET"

if [ "$NATIVE" != 1 ]; then
  podman image exists "$IMG" 2>/dev/null || { say "building image $IMG"; podman build -t "$IMG" -f "$HERE/Containerfile" "$HERE"; }
fi

in_container() { # WORKDIR COMMAND
  if [ "$NATIVE" = 1 ]; then
    # ps3toolchain scripts size make -j from `nproc --all`; HWPROBE_JOBS=N overrides that (shared hosts)
    [ -z "${HWPROBE_JOBS:-}" ] || { nproc() { echo "$HWPROBE_JOBS"; }; export -f nproc; }
    ( cd "$1" && PS3DEV="$PS3DEV" PSL1GHT="$PS3DEV" \
      PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
      bash -c "$2" )
    return
  fi
  podman run --rm -v "$ROOT:$ROOT" -e PS3DEV="$PS3DEV" -e PSL1GHT="$PS3DEV" \
    -e PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
    -w "$1" "$IMG" bash -c "$2"
}

if [ "$WITH_PPU" = 1 ]; then
  step001_done || { say "step 001: binutils"; in_container "$TC_SRC" './toolchain.sh 1'; }
  step002_done || { say "step 002: gcc + newlib (C only)"; in_container "$TC_SRC" './toolchain.sh 2'; }
  step004_done || { say "step 004: symlinks"; in_container "$TC_SRC" './toolchain.sh 4'; }
  psl1ght_done || {
    say "PSL1GHT"
    in_container "$PSL_SRC" 'make install-ctrl && make -C ppu && make -C ppu install && make -C tools && make -C tools install'
  }
fi
if [ "$WITH_SPU" = 1 ]; then
  step005_done || { say "step 005: binutils (spu)"; in_container "$TC_SRC" './toolchain.sh 5'; }
  step006_done || { say "step 006: gcc + newlib (spu, C only)"; in_container "$TC_SRC" './toolchain.sh 6'; }
  psl1ght_spu_done || {
    say "PSL1GHT spu"
    in_container "$PSL_SRC" 'make install-ctrl && make -C spu && make -C spu install'
  }
fi

verify
[ "${#problems[@]}" -eq 0 ] || { printf 'build-toolchain: still incomplete: %s\n' "${problems[@]}" >&2; exit 1; }
say "prefix $PS3DEV is complete"
