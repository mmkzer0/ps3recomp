#!/usr/bin/env python3
"""Independent check of the instruction words the probe executes: assemble every op of the generated table
with the real SPU assembler (spu-as, GNU binutils) and compare the word with gen_tables.py's template.

    verify_encodings.py <repo-root> [--spu-as PATH]

The toolkit's decode table is where gen_tables.py takes the opcodes from; this check does not share it, so a
wrong opcode there cannot pass unnoticed. Immediates are checked at 0 and at one non-zero value (the scale of the
conversion ops is checked against the documented field = base - scale mapping). Exit status 1 on any difference.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_tables as g

# assembler operand -> field mapping differs from the raw field for these (documented ISA behaviour)
NEG_IMM = set()   # spu-as 2.22 takes the raw field for the rotate-and-mask immediates too (checked)
SCALE_BASE = {"cflts": 173, "cfltu": 173, "csflt": 155, "cuflt": 155}        # field = base - scale
ADDR_FORM = {"cbd", "chd", "cwd", "cdd"}                                       # syntax rt,imm(ra)


def asm_line(row, imm):
    m, k = row["mnemonic"], row["kind"]
    if k == "RRR":
        return f"{m} ${row['rt']},$3,$4,$5"
    if k == "RR":
        return f"{m} ${row['rt']},$3,$4" if row["need"] & 2 else f"{m} ${row['rt']},$3"
    if k in ("RI7", "RI10") and m in ADDR_FORM:
        return f"{m} $3,{imm}($3)"
    if k in ("RI7", "RI10", "RI8"):
        return f"{m} $3,$3,{imm}"
    if k in ("RI16", "RI18"):
        return f"{m} $3,{imm}"
    raise SystemExit("kind " + k)


def assemble(spu_as, lines):
    d = tempfile.mkdtemp(prefix="spuexh_as_")
    try:
        src = os.path.join(d, "t.s")
        with open(src, "w") as f:
            f.write("\n".join(lines) + "\n")
        obj = os.path.join(d, "t.o")
        subprocess.run([spu_as, src, "-o", obj], check=True, capture_output=True, text=True)
        objdump = os.path.join(os.path.dirname(spu_as), "spu-objdump")
        out = subprocess.run([objdump, "-d", obj], check=True, capture_output=True, text=True).stdout
        words = []
        for ln in out.splitlines():
            mm = re.match(r"\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} ){4})", ln)
            if mm:
                words.append(int(mm.group(1).replace(" ", ""), 16))
        return words
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main(argv):
    root = argv[1]
    spu_as = shutil.which("spu-as") or os.path.join(os.environ.get("PS3DEV", ""), "spu/bin/spu-as")
    if "--spu-as" in argv:
        spu_as = argv[argv.index("--spu-as") + 1]
    if not os.path.exists(spu_as):
        print("verify_encodings: no spu-as (set PS3DEV or --spu-as)")
        return 2
    _domains, rows, _tab = g.build_op_rows(root)
    bad = 0
    for row in rows:
        m, k = row["mnemonic"], row["kind"]
        samples = [0] if row["imm_bits"] == 0 else [0, 5]
        if m in SCALE_BASE:
            samples = [0, 5]
        lines = [asm_line(row, s) for s in samples]
        words = assemble(spu_as, lines)
        for s, w in zip(samples, words):
            if m in SCALE_BASE:
                want_field = SCALE_BASE[m] - s
                want = (row["word"] & ~(0xFF << 14)) | (want_field << 14)
            elif row["imm_bits"]:
                field = ((-s) if m in NEG_IMM else s) & ((1 << row["imm_bits"]) - 1)
                want = row["word"] | (field << row["imm_shift"])
            else:
                want = row["word"]
            if w != want:
                bad += 1
                print(f"MISMATCH {row['name']:<12} {k:<4} imm={s} asm {w:08x}  template {want:08x}   ({asm_line(row, s)})")
    print(f"verify_encodings: {len(rows)} ops checked against {os.path.basename(spu_as)}, {bad} mismatches")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
