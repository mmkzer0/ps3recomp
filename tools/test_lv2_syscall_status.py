#!/usr/bin/env python3
"""The LV2 status generator against synthetic maps, not the real firmware.

The FW 4.93 stock map is the required input; an optional 4.81 dump map adds a
corroboration column only. The toolkit and RPCS3 classifications come from the
repository and the embedded pin. The synthetic tables pick the interesting
slots out of the real data:

  - 1     stock handler, toolkit handler     -> ok
  - 2     stock handler, toolkit none        -> hw-real-unregistered (stop)
  - 337   stock handler, loader case         -> ok
  - 988   stock handler, toolkit handler     -> ok
  - 0     stock ENOSYS                       -> hw-enosys-unregistered (bitmap)
  - 6     stock ENOSYS, dump handler only    -> bitmap, corroboration differs
  - 15    stock ENOSYS                       -> bitmap
  - 16    stock ENOSYS                       -> bitmap
  - 42    stock ENOSYS, toolkit registers sys_ppu_thread_exit -> suspicious
  - 60    stock ENOSYS, RPCS3 binds sys_trace_create -> conflict (stop kept)

It then compiles the generated bitmap and checks the same slots from C.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/lv2_syscall_status.py"

STOCK_HANDLERS = {1, 2, 337, 988}
DUMP_HANDLERS = {1, 6, 337, 988}          # 6 only in the dump; 2 only on stock


def stock_map(handlers: set[int]) -> str:
    lines = ["# synthetic FW 4.93 stock syscall status", "",
             "| slot | 4.93 stock |", "|---|---|"]
    for s in range(1024):
        lines.append(f"| {s} | {'handler' if s in handlers else 'enosys'} |")
    return "\n".join(lines) + "\n"


def dump_map(handlers: set[int]) -> str:
    lines = ["# synthetic LV2 dump FW 4.81 REBUG/Cobra syscall map", "",
             "| slot | handler EA |", "|---:|---|"]
    for s in sorted(handlers):
        lines.append(f"| {s} | 0x{0x1000 + 4 * s:X} |")
    return "\n".join(lines) + "\n"


def doc_row(doc: str, slot: int) -> list[str]:
    """The 6-column row from the main table (the suspicious list has 3)."""
    table = doc.split("## Table", 1)[1]
    prefix = f"| {slot} | "
    for line in table.splitlines():
        if line.startswith(prefix):
            return [c.strip() for c in line.strip().strip("|").split("|")]
    raise AssertionError(f"no table row for slot {slot}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lv2-status-") as tmp:
        tmp = Path(tmp)
        m = tmp / "stock.md"
        d = tmp / "dump.md"
        m.write_text(stock_map(STOCK_HANDLERS))
        d.write_text(dump_map(DUMP_HANDLERS))
        doc = tmp / "status.md"
        src = tmp / "lv2_enosys_bitmap.c"
        hdr = tmp / "lv2_enosys_bitmap.h"
        subprocess.run([sys.executable, str(TOOL), str(m), "--dump-map", str(d),
                        "--doc", str(doc), "--c", str(src), "--h", str(hdr)],
                       check=True, capture_output=True)
        text = doc.read_text()

        # The header makes the stock map primary and states the corroboration.
        assert "FW 4.93 stock lv2_kernel (PUP)" in text, text[:1200]
        assert "LV2 dump FW 4.81 REBUG/Cobra" in text, text[:1200]
        assert "handler only in the dump: 6" in text, text[:1600]
        assert "handler only on stock: 2" in text, text[:1600]
        assert "| **total** | **1024** |" in text, text[:2000]
        # No map address reaches the document, from either map.
        for s in DUMP_HANDLERS:
            ea = f"0x{0x1000 + 4 * s:04X}"
            assert ea not in text, f"dump EA {ea} leaked into the document"

        assert doc_row(text, 1) == ["1", "handler", "handler", "bind",
                                    "handler", "ok"]
        assert doc_row(text, 2) == ["2", "handler", "enosys", "bind",
                                    "none", "hw-real-unregistered"]
        assert doc_row(text, 6) == ["6", "enosys", "handler", "null",
                                    "none", "hw-enosys-unregistered"]
        assert doc_row(text, 15) == ["15", "enosys", "enosys", "null",
                                     "none", "hw-enosys-unregistered"]
        assert doc_row(text, 337) == ["337", "handler", "handler", "bind",
                                      "loader-case", "ok"]
        assert doc_row(text, 988) == ["988", "handler", "handler", "null",
                                      "handler", "ok"]
        assert doc_row(text, 0) == ["0", "enosys", "enosys", "null", "none",
                                    "hw-enosys-unregistered"]
        assert doc_row(text, 42) == ["42", "enosys", "enosys", "null", "handler",
                                     "toolkit-handler-on-hw-enosys"]
        assert doc_row(text, 60) == ["60", "enosys", "enosys", "bind", "none",
                                     "conflict-4.93-vs-rpcs3"]
        # The suspicious slot is named with its registration site.
        assert "sys_ppu_thread_exit" in text
        assert "runtime/syscalls/sys_ppu_thread.c:" in text

        # The generated C answers from the same set: stock ENOSYS slots
        # (including the CFW-only 6 and 15), but not the suspicious 42, the
        # conflict 60, or an implemented slot; out-of-range is 0.
        prog = tmp / "check.c"
        prog.write_text(
            '#include <stdio.h>\n'
            '#include "lv2_enosys_bitmap.h"\n'
            'int main(void) {\n'
            '    static const unsigned num[] = {0, 6, 15, 16, 1023, 42, 60, 1, 2, 337, 988, 1024};\n'
            '    static const int want[] = {1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};\n'
            '    for (unsigned i = 0; i < sizeof num / sizeof num[0]; i++)\n'
            '        if (lv2_enosys_default_answer(num[i]) != want[i]) {\n'
            '            printf("slot %u -> %d\\n", num[i],\n'
            '                   lv2_enosys_default_answer(num[i]));\n'
            '            return 1;\n'
            '        }\n'
            '    puts("ok");\n'
            '    return 0;\n'
            '}\n')
        exe = tmp / "check"
        cc = os.environ.get("CC", "cc")
        subprocess.run([cc, "-std=gnu17", "-I", str(tmp), str(prog), str(src),
                        "-o", str(exe)], check=True)
        p = subprocess.run([str(exe)], capture_output=True, text=True)
        assert p.returncode == 0 and p.stdout.strip() == "ok", (p.stdout, p.stderr)

        generated = src.read_text()
        assert "Generated by tools/lv2_syscall_status.py" in generated
        assert "FW 4.93 stock lv2_kernel" in generated
        assert "_Static_assert" in generated
        print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
