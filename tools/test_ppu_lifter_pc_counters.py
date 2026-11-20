#!/usr/bin/env python3
"""--pc-counters: listed instructions carry a counter, nothing else changes, bad lists stop the lift.

1. Emission: a relaxed atomic add on slot k (k-th listed PC, ascending, however the file orders them)
   starts exactly the listed instructions -- after a branch label, so a jump to the PC counts -- and the
   PC table and the registration are emitted next to the function table. Serial and parallel lifts agree.
2. Nothing else: removing the counter statements and the two counter blocks gives the output of the same
   lift without the flag, and without the flag no counter symbol appears at all.
3. Rejections, each exiting non-zero and naming the file, the line and the PC, with no output written:
   malformed line, duplicate PC, misaligned PC, PC above 32 bits, empty list, missing file, a PC no lifted
   function covers, a PC in code the lift proved unreachable, and --symbol-prefix.

Run: python tools/test_ppu_lifter_pc_counters.py
"""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

LIFTER = Path(__file__).with_name("ppu_lifter.py")
STMT = "__atomic_fetch_add(&ppu_pc_counter_slots[{}], 1, __ATOMIC_RELAXED);"
COUNT_RE = re.compile(r"^\s*__atomic_fetch_add\(&ppu_pc_counter_slots\[\d+\], 1, __ATOMIC_RELAXED\);$")

LOOP = bytes.fromhex(
    "38800000"   # 0x10000 li    r4, 0
    "7c6903a6"   # 0x10004 mtctr r3
    "38840001"   # 0x10008 addi  r4, r4, 1      (bdnz target)
    "4200fffc"   # 0x1000C bdnz  0x10008
    "4e800020")  # 0x10010 blr
LOOP_FUNCS = [{"start": "0x10000", "end": "0x10014"}]
# A literal word after the blr: no path from the entry reaches it.
DEAD = bytes.fromhex("38600001" "4e800020" "12345678")
DEAD_FUNCS = [{"start": "0x10000", "end": "0x1000C"}]


def lift(root, code, funcs, pcs, jobs="1", flag=True, extra=(), tag="o"):
    (root / "code.bin").write_bytes(code)
    (root / "functions.json").write_text(json.dumps(funcs))
    cmd = [sys.executable, str(LIFTER), str(root / "code.bin"), "--raw", "--base", "0x10000",
           "--functions", str(root / "functions.json"), "--jobs", jobs,
           "--output", str(root / f"out_{tag}")]
    if flag:
        if pcs is not None:
            (root / "pcs.txt").write_text(pcs)
        cmd += ["--pc-counters", str(root / "pcs.txt")]
    r = subprocess.run(cmd + list(extra), capture_output=True, text=True, timeout=120)
    out = root / f"out_{tag}"
    text = "".join(p.read_text() for p in sorted(out.glob("ppu_recomp_*.cpp"))) if r.returncode == 0 else ""
    return r, text, out


def strip_counters(text):
    """The flagged output with the counter statements and the two counter blocks taken out."""
    lines = [ln for ln in text.split("\n") if not COUNT_RE.match(ln)]
    for marker in ("/* --pc-counters: slot k", "/* --pc-counters table"):
        while True:
            i = next((n for n, ln in enumerate(lines) if ln.startswith(marker)), None)
            if i is None:
                break
            j = next(n for n in range(i, len(lines)) if lines[n] == "")
            del lines[i:j + 1]
    return "\n".join(lines)


def body(text):
    m = re.search(r"^void func_00010000\(ppu_context\* ctx\) \{\n(.*?)^\}", text, re.S | re.M)
    assert m, text
    return [ln.strip() for ln in m.group(1).split("\n") if ln.strip()]


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        # 1. Emission: listed out of order, with a prefix-less hex form and a comment.
        pcs = "# the blr\n0x10010\n\n00010000  # before the loop\n0x00010008\n"
        r, text, out = lift(root, LOOP, LOOP_FUNCS, pcs)
        assert r.returncode == 0, r.stderr
        b = body(text)
        counts = [ln for ln in b if COUNT_RE.match(ln)]
        assert counts == [STMT.format(0), STMT.format(1), STMT.format(2)], b
        i0, i1, i2 = (b.index(STMT.format(k)) for k in range(3))
        assert b[i0 - 1] == "PPU_MM_SNAP" and "(0)" in b[i0 + 1], b           # before `li r4,0`
        assert b[i1 - 1] == "loc_00010008:" and "gpr[4] + " in b[i1 + 1], b   # after the label, before addi
        assert b[i2 + 1] == "return;", b                                      # before the blr
        assert len(b) == len(body(lift(root, LOOP, LOOP_FUNCS, None, flag=False, tag="plain")[1])) + 3, b
        assert "extern uint64_t ppu_pc_counter_slots[3];" in text, text
        assert "extern const uint32_t ppu_pc_counter_pcs[3];" in text, text
        assert re.search(r"uint64_t ppu_pc_counter_slots\[3\];\nconst uint32_t ppu_pc_counter_pcs\[3\] = \{\n"
                         r"    0x00010000u, 0x00010008u, 0x00010010u,\n\};", text), text
        assert "ps3_pc_counters_register(ppu_pc_counter_pcs, ppu_pc_counter_slots, 3);" in text, text
        r2, text2, _ = lift(root, LOOP, LOOP_FUNCS, pcs, jobs="2", tag="par")
        assert r2.returncode == 0 and text2 == text, r2.stderr
        print("[PASS] counters start exactly the listed instructions, after their label, in sorted slot order")

        # 2. Nothing else changes.
        r, plain, _ = lift(root, LOOP, LOOP_FUNCS, None, flag=False, tag="plain")
        assert r.returncode == 0 and "ppu_pc_counter" not in plain and "ps3_pc_counters" not in plain, r.stderr
        assert strip_counters(text) == plain, "stripping the counters did not give the unflagged output"
        print("[PASS] without the flag no counter symbol exists; stripping the counters restores the unflagged output")

        # 3. Rejections.
        def reject(code, funcs, pcs, needles, extra=()):
            r, _, out = lift(root, code, funcs, pcs, extra=extra, tag="bad")
            assert r.returncode != 0, f"accepted {pcs!r}"
            for n in needles:
                assert n in r.stderr, (n, r.stderr[-600:])
            assert not out.exists(), f"output written for {pcs!r}"

        reject(LOOP, LOOP_FUNCS, "0x10000\nbanana\n", ["pcs.txt:2:", "banana"])
        reject(LOOP, LOOP_FUNCS, "0x10000\n# c\n10000\n", ["pcs.txt:3:", "duplicate PC 0x00010000",
                                                           "first listed at line 1"])
        reject(LOOP, LOOP_FUNCS, "0x10000\n0x10002\n", ["pcs.txt:2:", "0x00010002", "not 4-byte aligned"])
        reject(LOOP, LOOP_FUNCS, "0x100000000\n", ["pcs.txt:1:", "malformed"])
        reject(LOOP, LOOP_FUNCS, "0x10000 0x10004\n", ["pcs.txt:1:", "malformed"])
        reject(LOOP, LOOP_FUNCS, "# nothing\n\n", ["no PCs listed"])
        reject(LOOP, LOOP_FUNCS, "0x10000\n0x20000\n", ["pcs.txt:2:", "PC 0x00020000",
                                                        "outside every lifted function"])
        reject(DEAD, DEAD_FUNCS, "0x10000\n0x10008\n", ["pcs.txt:2:", "PC 0x00010008",
                                                        "unreachable from the entry of func_00010000"])
        reject(LOOP, LOOP_FUNCS, "0x10000\n", ["unprefixed"], extra=["--symbol-prefix", "lib_"])
        (root / "pcs.txt").unlink()
        r, _, out = lift(root, LOOP, LOOP_FUNCS, None, tag="bad")
        assert r.returncode != 0 and "pcs.txt" in r.stderr and not out.exists(), r.stderr[-400:]
        print("[PASS] malformed, duplicate, misaligned, wide, empty, missing, uncovered, unreachable and "
              "prefixed lists stop the lift naming file, line and PC")
    print("PASS: --pc-counters emits per-instruction counters and nothing else, and fails closed")


if __name__ == "__main__":
    main()
