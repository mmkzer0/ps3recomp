#!/usr/bin/env python3
"""SPU_SCALAR_HELPERS / SPU_SCALAR_INT builds contain no SIMD permute for spu_shufb.

The scalar switches exist so a test run compares the vector helpers with something that is not
the vector helpers. spu_shufb's pshufb / NEON tbl branch used to ignore them, so the "scalar"
shufb run compared the SIMD path with itself and no host-independent test could tell.

The contract is about emitted code, so this compiles runtime/spu/tests/test_spu_shufb.c to
assembly and looks for the permute instruction:
  - with the scalar switch: none;
  - without it: at least one, otherwise the instruction pattern or the build is blind and an
    empty result above would prove nothing.
The compiler's own vectorizers are off for every build: they turn the scalar byte loop's
fixed byte reorder (SPU_W) into a pshufb of their own on Win64, where a u128 argument arrives
in memory. That is the scalar algorithm compiled, not the SIMD helper the switch removes.
On macOS both architectures are checked from either host (clang -arch); elsewhere the host's.
The compiler is CC (CTest passes CMake's), spelled for its driver (tools/host_cc.py).
Run:  python3 tools/test_spu_scalar_no_simd.py
"""
import os
import platform
import re
import sys
import tempfile
from pathlib import Path

from host_cc import c_compiler

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "runtime/spu/tests/test_spu_shufb.c"
# pshufb/vpshufb (x86 byte permute), tbl/tbx (NEON table lookup).
X86 = ("x86_64", ["-msse4.1"], re.compile(r"^\s*(?:v?pshufb)\b", re.MULTILINE))
ARM = ("arm64", [], re.compile(r"^\s*tb[lx]\b", re.MULTILINE))
NO_AUTOVEC = ["-fno-tree-vectorize", "-fno-tree-slp-vectorize"]   # gcc and clang both


def targets():
    """(name, flags, permute pattern, is_host). The host's architecture must build; on macOS the other
    one is checked as well when the compiler can target it (clang -arch), and skipped visibly if not."""
    m = platform.machine().lower()
    host_x86 = m in ("x86_64", "amd64")
    host_arm = m in ("aarch64", "arm64")
    if sys.platform == "darwin":
        return [(X86[0], ["-arch", "x86_64"] + X86[1], X86[2], host_x86),
                (ARM[0], ["-arch", "arm64"] + ARM[1], ARM[2], host_arm)]
    if host_x86:
        return [(X86[0], X86[1], X86[2], True)]
    if host_arm:
        return [(ARM[0], ARM[1], ARM[2], True)]
    return []


def asm(cc, flags, defines, tmp):
    out = Path(tmp) / "out.s"
    novec = ["/clang:" + f for f in NO_AUTOVEC] if cc.msvc else NO_AUTOVEC
    r = cc.run(cc.asm(SRC, out, std="gnu17", includes=[ROOT / "include", ROOT / "runtime/spu"],
                      defines=defines, flags=["-O2", *novec, *flags]))
    # a failing build is reported by the caller
    return out.read_text(encoding="utf-8", errors="replace") if r.returncode == 0 else None, r.stderr + r.stdout


def main():
    cc = c_compiler()
    if cc is None or cc.kind == "cl":
        why = "no C compiler" if cc is None else "plain cl cannot target SSE4.1 by flag"
        if os.environ.get("CI"):
            sys.exit(f"FAILED: {why}")
        print(f"skip test_spu_scalar_no_simd: {why}")
        return
    failed = False
    checked = 0
    with tempfile.TemporaryDirectory(prefix="spu-scalar-nosimd-") as tmp:
        for name, flags, pat, is_host in targets():
            control, err = asm(cc, flags, [], tmp)
            if control is None:
                if is_host:
                    print(f"FAILED {name}: the vector build does not compile\n{err[-1500:]}")
                    failed = True
                else:
                    print(f"skip {name}: this compiler cannot target it")
                continue
            if not pat.search(control):
                print(f"FAILED {name}: the vector build has no permute instruction; the check is blind")
                failed = True
                continue
            for define in ("SPU_SCALAR_HELPERS", "SPU_SCALAR_INT"):
                text, err = asm(cc, flags, [define], tmp)
                if text is None:
                    print(f"FAILED {name} -D{define}: build failed\n{err[-1500:]}")
                    failed = True
                    continue
                hit = pat.search(text)
                if hit:
                    print(f"FAILED {name} -D{define}: emitted '{hit.group(0).strip()}'")
                    failed = True
                else:
                    print(f"ok {name} -D{define}: no SIMD permute")
            checked += 1
    if failed:
        sys.exit(1)
    if not checked:
        print("skip test_spu_scalar_no_simd: no target the compiler can build")


if __name__ == "__main__":
    main()
