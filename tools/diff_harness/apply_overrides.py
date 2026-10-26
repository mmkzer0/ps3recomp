#!/usr/bin/env python3
"""Make the lifted functions named by an override-symbol file replaceable by guest address.

    apply_overrides.py --lifted fwx.c --prefix fwx_ --image fwx_image.bin --exports fwx_exports.json \\
                       --symbols fwx_overrides.json --out fwx_ovr.c

The lifted C calls its internal functions by symbol (`fwx_func_<EA>(ctx)`), so the harness cannot
intercept one at run time. This tool rewrites the lifter's own output, once per symbol:

    void fwx_func_<EA>(ppu_context* ctx) {          ->   void fwx_func_<EA>_orig(ppu_context* ctx);
                                                          void fwx_func_<EA>(ppu_context* ctx) {
                                                              if (dh_override_hit(0x<EA>, ctx)) return;
                                                              fwx_func_<EA>_orig(ctx);
                                                          }
                                                          void fwx_func_<EA>_orig(ppu_context* ctx) {

Every caller (direct calls, tail-call trampolines) keeps its text and reaches the wrapper. The
engine's `dh_override_hit` is a no-op unless a case registered an override for that EA, and the
appended `dh_override_wrapped` table tells the engine which EAs have a wrapper, so an override on
an address without one is a harness error rather than a silent no-op. The definition line must
match exactly once: an unlifted address, a lifter that changed its output shape or a symbol on a
tail-entry stub is refused.

The output is derived from firmware: it must not be written inside the repository (the
checked-in synthetic fixture passes --synthetic).
"""
import argparse
import json
import os
import sys

ROOT = os.path.realpath(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dh_symbols  # noqa: E402


def inside_repo(p):
    p = os.path.realpath(p)
    return p == ROOT or p.startswith(ROOT + os.sep)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lifted", required=True)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--exports", required=True, help="<mod>_exports.json (it carries the module base)")
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--synthetic", action="store_true")
    a = ap.parse_args()

    if not a.synthetic:
        for label, p in (("--lifted", a.lifted), ("--image", a.image), ("--exports", a.exports),
                         ("--symbols", a.symbols), ("--out", a.out)):
            if inside_repo(p):
                sys.exit(f"refusing firmware-derived path inside the repository ({label}): {os.path.realpath(p)}")

    image = open(a.image, "rb").read()
    base = int(json.load(open(a.exports))["base"], 16)
    _sha, syms = dh_symbols.load(a.symbols, image, base)
    text = open(a.lifted).read()

    for name, ea, _ev in syms:
        fn = f"{a.prefix}func_{ea:08X}"
        head = f"void {fn}(ppu_context* ctx) {{\n"
        if text.count(head) != 1:
            sys.exit(f"{name}: expected exactly one definition `{head.strip()}` in {a.lifted}, found "
                     f"{text.count(head)}; the address is not a lifted function entry")
        wrapper = (f"void {fn}_orig(ppu_context* ctx);\n"
                   f"void {fn}(ppu_context* ctx) {{\n"
                   f"        if (dh_override_hit(0x{ea:08X}u, ctx)) return;\n"
                   f"        {fn}_orig(ctx);\n"
                   f"}}\n"
                   f"void {fn}_orig(ppu_context* ctx) {{\n")
        text = text.replace(head, wrapper)

    # The lifted file is C++ (extern "C" symbols, thread-local trampoline pointer).
    decl = 'extern "C" int dh_override_hit(uint32_t addr, ppu_context* ctx);\n'
    marker = "\nvoid "
    first = text.index(marker) + 1
    text = text[:first] + decl + text[first:]
    text += ("\nextern \"C\" const uint32_t dh_override_wrapped[] = {" +
             ", ".join(f"0x{ea:08X}u" for _n, ea, _e in syms) + "};\n"
             f"extern \"C\" const uint32_t dh_override_wrapped_count = {len(syms)};\n")

    part = a.out + ".partial"
    with open(part, "w") as f:
        f.write(text)
    os.replace(part, a.out)
    print(f"apply_overrides: {len(syms)} wrapper(s) -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
