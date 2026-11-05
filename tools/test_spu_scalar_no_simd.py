#!/usr/bin/env python3
"""SPU_SCALAR_HELPERS / SPU_SCALAR_INT builds contain no SIMD permute for spu_shufb.

The scalar switches exist so a test run compares the vector helpers with something that is not
the vector helpers. spu_shufb's pshufb / NEON tbl branch used to ignore them, so the "scalar"
shufb run compared the SIMD path with itself and no host-independent test could tell.

The contract is about emitted code, so this compiles runtime/spu/tests/test_spu_shufb.c to
assembly (-S) and looks for the permute instruction:
  - with the scalar switch: none;
  - without it: at least one, otherwise the instruction pattern or the build is blind and an
    empty result above would prove nothing.
On macOS both architectures are checked from either host (clang -arch); elsewhere the host's.
Run:  python3 tools/test_spu_scalar_no_simd.py
"""
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "runtime/spu/tests/test_spu_shufb.c"
# pshufb/vpshufb (x86 byte permute), tbl/tbx (NEON table lookup).
X86 = ("x86_64", ["-msse4.1"], re.compile(r"^\s*(?:v?pshufb)\b", re.MULTILINE))
ARM = ("arm64", [], re.compile(r"^\s*tb[lx]\b", re.MULTILINE))


def compiler():
    for cand in (os.environ.get("CC"), "clang", "gcc", "cc"):
        if cand and shutil.which(cand.split()[0]):
            return cand.split()
    return None


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
    cmd = cc + ["-std=gnu17", "-O2", "-S", "-I", str(ROOT / "include"), "-I", str(ROOT / "runtime/spu")] \
        + flags + defines + [str(SRC), "-o", str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True, check=False)   # a failing build is reported by the caller
    return out.read_text() if r.returncode == 0 else None, r.stderr


def main():
    cc = compiler()
    if cc is None:
        if os.environ.get("CI"):
            sys.exit("FAILED: no C compiler")
        print("skip test_spu_scalar_no_simd: no C compiler")
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
                text, err = asm(cc, flags, ["-D" + define], tmp)
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
