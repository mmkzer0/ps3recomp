#!/usr/bin/env python3
"""Contract test for PPU lifter code window bounds without --code-end.

Finding (P2):
`tools/ppu_lifter.py` previously made `--code-end` optional, and without it
derived `code_lo` from the lowest SHF_EXECINSTR section but allowed `code_hi`
to fall back to `max(e for _, e in func_bounds)`. When an executable PT_LOAD
segment also contained non-executable data (such as a trailing .rodata section),
bogus function entries or branch targets in data were kept inside the computed
window, promoting data to functions (func_<rodata_addr>) and emitting calls or
trampolines to them rather than stopping at `ppu_unlifted_stub`.

Why it fails on HEAD:
1. `args.code_end` defaults to None.
2. The function-bounds filter at `func_bounds = [(s, min(e, args.code_end)) ...]`
   only guarded against `args.code_end is not None`. Without `--code-end`, any
   function declared in trailing data remained in `func_bounds`.
3. `lifter.code_hi` fell back to `max(e for _, e in func_bounds)`, extending the
   code window all the way through trailing data.
4. `self._outside_code(target)` returned False for targets in .rodata, so branches
   targeting .rodata emitted direct calls/trampolines to `func_<rodata_addr>`.
5. `ppu_lift_report.json` included the bogus .rodata function.

The fix derives `code_hi` from the highest SHF_EXECINSTR section (falling back to
the executable PT_LOAD end for section-stripped ELFs), matching `ppu_loader.py --code-end`
and `cmake/PS3RecompPPU.cmake`.
"""

import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
LIFTER = HERE / "ppu_lifter.py"


def build_synthetic_elf(vaddr=0x10000, stripped=False, exec_segment=True, exec_section=True) -> bytes:
    """Build a minimal big-endian PPC64 ELF with one PT_LOAD segment.

    The segment contains:
      - .text (SHF_ALLOC | SHF_EXECINSTR = 0x6) at [0x10000, 0x10020)
        - func_00010000 at [0x10000, 0x10010):
            0x10000: bl  0x10020  (call targeting .rodata)
            0x10004: blr
            0x10008: nop
            0x1000C: nop
        - func_00010010 at [0x10010, 0x10020):
            0x10010: b   0x10020  (jump targeting .rodata)
            0x10014: blr
            0x10018: nop
            0x1001C: nop
      - .rodata (SHF_ALLOC = 0x2, non-executable) at [0x10020, 0x10030)
        Data words that would decode as instructions if wrongly lifted as code:
            0x10020: 0x38600001 (li r3, 1)
            0x10024: 0x4E800020 (blr)
            0x10028: 0x00000000
            0x1002C: 0x00000000
    """
    f1 = struct.pack(">IIII", 0x48000021, 0x4E800020, 0x60000000, 0x60000000)
    f2 = struct.pack(">IIII", 0x48000010, 0x4E800020, 0x60000000, 0x60000000)
    text_data = f1 + f2
    rodata_data = struct.pack(">IIII", 0x38600001, 0x4E800020, 0x00000000, 0x00000000)
    seg_data = text_data + rodata_data

    shstr = b"\x00.text\x00.rodata\x00.shstrtab\x00"

    def sh_name(s):
        return shstr.index(b"\x00" + s + b"\x00") + 1

    off_ph = 64
    off_data = off_ph + 56
    off_shstr = off_data + len(seg_data)
    off_sh = off_shstr + len(shstr)

    ehdr = bytearray(64)
    ehdr[0:4] = b"\x7fELF"
    ehdr[4] = 2  # 64-bit
    ehdr[5] = 2  # big-endian
    ehdr[6] = 1
    struct.pack_into(">H", ehdr, 16, 2)       # ET_EXEC
    struct.pack_into(">H", ehdr, 18, 21)      # PPC64
    struct.pack_into(">I", ehdr, 20, 1)
    struct.pack_into(">Q", ehdr, 24, vaddr)   # e_entry
    struct.pack_into(">Q", ehdr, 32, off_ph)  # e_phoff
    if not stripped:
        struct.pack_into(">Q", ehdr, 40, off_sh)  # e_shoff
        struct.pack_into(">H", ehdr, 58, 64)      # e_shentsize
        struct.pack_into(">H", ehdr, 60, 4)       # e_shnum
        struct.pack_into(">H", ehdr, 62, 3)       # e_shstrndx
    else:
        struct.pack_into(">Q", ehdr, 40, 0)
        struct.pack_into(">H", ehdr, 58, 0)
        struct.pack_into(">H", ehdr, 60, 0)
        struct.pack_into(">H", ehdr, 62, 0)

    struct.pack_into(">H", ehdr, 52, 64)      # e_ehsize
    struct.pack_into(">H", ehdr, 54, 56)      # e_phentsize
    struct.pack_into(">H", ehdr, 56, 1)       # e_phnum (1 PT_LOAD)

    phdr_flags = (1 if exec_segment else 0) | 4  # PF_X | PF_R if exec_segment, else PF_R
    phdr = struct.pack(">IIQQQQQQ",
                       1,                      # p_type: PT_LOAD
                       phdr_flags,             # p_flags
                       off_data,               # p_offset
                       vaddr,                  # p_vaddr
                       vaddr,                  # p_paddr
                       len(seg_data),          # p_filesz
                       len(seg_data),          # p_memsz
                       0x10000)                # p_align

    if stripped:
        return bytes(ehdr) + phdr + seg_data

    def shdr(name, typ, flags, addr, off, size):
        return struct.pack(">IIQQQQIIQQ", name, typ, flags, addr, off, size, 0, 0, 4, 0)

    shtab = b"".join([
        shdr(0, 0, 0, 0, 0, 0),
        shdr(sh_name(b".text"), 1, 6 if exec_section else 2, vaddr, off_data, len(text_data)),
        shdr(sh_name(b".rodata"), 1, 2, vaddr + len(text_data), off_data + len(text_data), len(rodata_data)),
        shdr(sh_name(b".shstrtab"), 3, 0, 0, off_shstr, len(shstr)),
    ])
    return bytes(ehdr) + phdr + seg_data + shstr + shtab


def test_lifter_code_window_filters_rodata():
    """Without --code-end, the lifter must derive code_hi from SHF_EXECINSTR and
    must not promote data in the executable segment past .text to functions."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        elf_path = root / "synthetic.elf"
        elf_path.write_bytes(build_synthetic_elf(vaddr=0x10000))

        funcs_path = root / "functions.json"
        funcs = [
            {"start": "0x10000", "end": "0x10010"},  # function 1 in .text (live bl into .rodata)
            {"start": "0x10010", "end": "0x10020"},  # function 2 in .text (live b into .rodata)
            {"start": "0x10020", "end": "0x10030"},  # bogus entry in .rodata
        ]
        funcs_path.write_text(json.dumps(funcs))

        out_no_ce = root / "out_no_code_end"
        out_ce = root / "out_code_end"

        # 1. Run lifter WITHOUT --code-end
        subprocess.run([
            sys.executable, str(LIFTER),
            str(elf_path),
            "--functions", str(funcs_path),
            "--output", str(out_no_ce),
            "--single-file",
            "--jobs", "1"
        ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        # 2. Assertions on output without --code-end:
        c_src = (out_no_ce / "ppu_recomp.c").read_text()
        hdr_src = (out_no_ce / "ppu_recomp.h").read_text()
        report = json.loads((out_no_ce / "ppu_lift_report.json").read_text())

        # No func_<rodata addr> is defined or declared
        assert "func_00010020" not in c_src, "Bogus rodata function defined in ppu_recomp.c"
        assert "func_00010020" not in hdr_src, "Bogus rodata function declared in ppu_recomp.h"

        # The live branch lowers to ppu_unlifted_stub(0x<target>ULL, ctx)
        assert "ctx->lr = 0x00010004; ppu_unlifted_stub(0x00010020ULL, ctx);" in c_src, (
            "bl targeting .rodata must lower to ppu_unlifted_stub"
        )
        assert "{ ppu_unlifted_stub(0x00010020ULL, ctx); return; }" in c_src, (
            "b targeting .rodata must lower to ppu_unlifted_stub"
        )

        # The report has no function starting in .rodata
        rep_starts = [int(fn["start"], 16) for fn in report["functions"]]
        assert not any(s >= 0x10020 for s in rep_starts), (
            f"Report contains function starting in .rodata: {report['functions']}"
        )
        assert [fn["name"] for fn in report["functions"]] == ["func_00010000", "func_00010010"]

        # 3. Run WITH --code-end 0x10020 and assert identical output
        subprocess.run([
            sys.executable, str(LIFTER),
            str(elf_path),
            "--functions", str(funcs_path),
            "--output", str(out_ce),
            "--single-file",
            "--jobs", "1",
            "--code-end", "0x10020"
        ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        c_ce = (out_ce / "ppu_recomp.c").read_text()
        hdr_ce = (out_ce / "ppu_recomp.h").read_text()
        rep_ce = (out_ce / "ppu_lift_report.json").read_text()

        assert c_src == c_ce, "C output without --code-end differs from output with --code-end"
        assert hdr_src == hdr_ce, "Header output without --code-end differs from output with --code-end"
        assert (out_no_ce / "ppu_lift_report.json").read_text() == rep_ce, (
            "Report output without --code-end differs from output with --code-end"
        )


def test_lifter_code_window_section_stripped_fallback():
    """Section-stripped ELFs fall back to the executable PT_LOAD end."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        elf_path = root / "stripped.elf"
        elf_path.write_bytes(build_synthetic_elf(vaddr=0x10000, stripped=True))

        funcs_path = root / "functions.json"
        funcs = [
            {"start": "0x10000", "end": "0x10010"},
            {"start": "0x10030", "end": "0x10040"},  # past the entire PT_LOAD (ends at 0x10030)
        ]
        funcs_path.write_text(json.dumps(funcs))

        out = root / "out_stripped"
        subprocess.run([
            sys.executable, str(LIFTER),
            str(elf_path),
            "--functions", str(funcs_path),
            "--output", str(out),
            "--single-file",
            "--jobs", "1"
        ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        report = json.loads((out / "ppu_lift_report.json").read_text())
        rep_starts = [int(fn["start"], 16) for fn in report["functions"]]
        assert not any(s >= 0x10030 for s in rep_starts), (
            f"Stripped ELF report contains function starting past PT_LOAD: {report['functions']}"
        )


def test_lifter_code_window_no_executable_sections_or_segments_fails():
    """An ELF with no SHF_EXECINSTR section and no executable PT_LOAD segment
    must fail closed and exit non-zero with an actionable error message."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        elf_path = root / "no_exec.elf"
        elf_path.write_bytes(build_synthetic_elf(vaddr=0x10000, exec_segment=False, exec_section=False))

        funcs_path = root / "functions.json"
        funcs = [{"start": "0x10000", "end": "0x10010"}]
        funcs_path.write_text(json.dumps(funcs))

        out = root / "out_no_exec"
        res = subprocess.run([
            sys.executable, str(LIFTER),
            str(elf_path),
            "--functions", str(funcs_path),
            "--output", str(out),
            "--single-file",
            "--jobs", "1"
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        assert res.returncode != 0, f"Expected non-zero exit, got {res.returncode}"
        assert f"ppu_lifter: {elf_path} has no executable section or segment; pass --code-end" in res.stderr, (
            f"Expected actionable error message in stderr, got: {res.stderr!r}"
        )


if __name__ == "__main__":
    failed = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            try:
                fn()
                print("PASS", name)
            except Exception as e:
                failed += 1
                print("FAIL", name, e, file=sys.stderr)
    sys.exit(1 if failed else 0)
