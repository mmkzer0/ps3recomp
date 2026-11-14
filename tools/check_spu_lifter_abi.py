#!/usr/bin/env python3
"""Reject SPU lifts generated before the lifter ABI stamp and check.

A prebuilt lift can be reused instead of regenerated: `build_spu_workloads.py`
keeps an existing `spu_recomp.c`, `--skip-lift` trusts the output directory, and
a title's `SPU_DIR` glob compiles whatever is there. Without this validator such
an image links against the current runtime and its registration never calls
`ps3_spu_lifter_abi_check`, so a layout helper mismatch runs silently.

The validator refuses an image whose generated header has no
`#define SPU_LIFTER_ABI <n>` or whose generated registration does not call
`ps3_spu_lifter_abi_check(SPU_LIFTER_ABI)` before it registers functions. The
numeric comparison stays in the runtime, where a mismatch is the `lifter-abi`
stop; this script only decides whether the build may ship the lift at all.

usage:
    check_spu_lifter_abi.py <images-dir>
    check_spu_lifter_abi.py --image <image-dir> [--image <image-dir> ...]
"""
import argparse
import re
import sys
from pathlib import Path

STAMP = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+SPU_LIFTER_ABI[ \t]+[0-9]+u?[ \t]*$", re.M)
CHECK = re.compile(r"\bps3_spu_lifter_abi_check\s*\(\s*SPU_LIFTER_ABI\s*\)\s*;")
REGISTER = re.compile(r"\bspu_register_function\s*\(")

REMEDY = ("re-lift the image with this toolkit's tools/spu_lifter.py "
          "(or pass --relift); a pre-stamp lift cannot validate against this runtime")


def _without_comments(text):
    """Ignore commented-out stamps/calls: those are not generated code."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def image_problems(image_dir):
    """Return human-readable defects for one generated SPU image directory."""
    image_dir = Path(image_dir)
    header = image_dir / "spu_recomp.h"
    source = image_dir / "spu_recomp.c"
    problems = []

    if not header.is_file():
        problems.append(f"{header}: missing generated header")
    elif not STAMP.search(_without_comments(_read(header))):
        problems.append(
            f"{header}: missing '#define SPU_LIFTER_ABI <n>' (lift predates the ABI gate)")

    if not source.is_file():
        problems.append(f"{source}: missing generated source")
        return problems

    text = _without_comments(_read(source))
    check = CHECK.search(text)
    register = REGISTER.search(text)
    if check is None:
        problems.append(
            f"{source}: generated registration never calls "
            "ps3_spu_lifter_abi_check(SPU_LIFTER_ABI) (lift predates the ABI gate)")
    elif register is None:
        problems.append(
            f"{source}: no spu_register_function call; not a generated registration")
    elif check.start() > register.start():
        problems.append(
            f"{source}: the ABI check comes after the first spu_register_function call")
    return problems


def tree_problems(root):
    """Validate every ``<root>/*/spu_recomp.c`` image; no images is not an error."""
    root = Path(root)
    images = sorted(path.parent for path in root.glob("*/spu_recomp.c"))
    problems = []
    for image in images:
        problems.extend(image_problems(image))
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("images_dir", nargs="?",
                        help="directory containing one subdirectory per lifted image")
    parser.add_argument("--image", action="append", default=[], metavar="DIR",
                        help="one generated image directory (repeatable)")
    args = parser.parse_args(argv)
    if bool(args.images_dir) == bool(args.image):
        parser.error("give exactly one of <images-dir> or --image")

    problems = []
    if args.images_dir:
        problems.extend(tree_problems(args.images_dir))
    for image in args.image:
        problems.extend(image_problems(image))

    if problems:
        print("[spu-abi] stale or unstamped SPU lifted code:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        print(f"[spu-abi] {REMEDY}.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
