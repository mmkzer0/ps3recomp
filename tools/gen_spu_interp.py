#!/usr/bin/env python3
"""Generate the SPU interpreter's decode table (C) from the validated Python
decoder (spu_disasm.py). The interpreter is the correctness floor for the SPU
static recompiler: computed/indirect branches (`bi $reg`) and DMA'd or
self-modified code (LBP overlays, spu_0004's `sync; bi $3` trampoline) can't be
statically lifted, so on a lifted-function-table miss we interpret from live
local store. Emitting the opcode->mnemonic tables from spu_disasm keeps the
interpreter's decode bit-identical to the decoder we already proved 100% vs
spu-lv2-objdump; the execute half (spu_interp.c) calls the SAME spu_<mnemonic>
helpers the lifter emits, so lifted and interpreted code share exact semantics.

    python gen_spu_interp.py > ../runtime/spu/spu_interp_tables.inc
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spu_disasm as D  # RRR_TABLE, RI18_TABLE, RI16_TABLE, RI10_TABLE, SPU_RR, ...

# The RI7 / RI8 / LSX tables live inside spu_decode() as locals; mirror them here
# (kept in sync by this generator; a drift shows immediately in the selftest).
RI7 = {
    0x078: "roti", 0x079: "rotmi", 0x07a: "rotmai", 0x07b: "shli",
    0x07c: "rothi", 0x07d: "rothmi", 0x07e: "rotmahi", 0x07f: "shlhi",
    0x1f4: "cbd", 0x1f5: "chd", 0x1f6: "cwd", 0x1f7: "cdd",
    0x1f8: "rotqbii", 0x1f9: "rotqmbii", 0x1fb: "shlqbii",
    0x1fc: "rotqbyi", 0x1fd: "rotqmbyi", 0x1ff: "shlqbyi",
}
RI7_UNSIGNED = {"cbd", "chd", "cwd", "cdd"}
RI8 = {0x1d8: "cflts", 0x1d9: "cfltu", 0x1da: "csflt", 0x1db: "cuflt"}  # op10
LSX = {0x1c4: "lqx", 0x144: "stqx"}
CHANNEL = {0x00D: "rdch", 0x10D: "wrch", 0x00F: "rchcnt"}  # op11

# These five RR double-precision compare/test mnemonics decode as `.word` for
# the interpreter even though spu_disasm.py names them: a retail CBE SPU does
# not implement them (they are later additions to the SPU ISA [INFERENCE]).
# Executing one raises an SPU invalid-instruction error: the thread group ends
# with join cause 4 after terminate and sys_spu_thread_get_exit_status reads
# 0x8001000F (CELL_ESTAT) on the stopped thread. Hardware, direct: the hwprobe
# `spuexh` special-A probe ran each word alone in its own thread group on a
# CECHC04 with 4.81 REBUG and none executed (issue #124); RPCS3 does not
# implement them either. spu_disasm.py keeps the mnemonics -- naming a word
# (disassembly) is not executing it -- only the interpreter's decode remaps
# them, so a guest that hits one stops exactly where any undecodable word
# stops, at the word's own pc, instead of computing a result the console
# never produced.
DECODE_AS_INVALID = {"dfceq", "dfcmeq", "dfcgt", "dfcmgt", "dftsv"}

# Every mnemonic the decoder can emit -> a stable enum. `.word` is the trap.
all_mn = set()
for t in (D.RRR_TABLE, D.RI18_TABLE, D.RI16_TABLE, D.RI10_TABLE, D.SPU_RR,
          RI7, RI8, LSX, CHANNEL):
    all_mn.update(t.values())
all_mn.add(".word")
all_mn -= DECODE_AS_INVALID
mnems = sorted(all_mn)


def enum_name(m):
    return "SPU_" + ("word" if m == ".word" else m)


# Every opcode field of the SPU ISA (RRR 4 bits, RI18 7, RI16 9, RI10 8, RI8 10,
# RI7/RR/LSX/channel 11) is a prefix of instruction bits 31..21, so the matched
# mnemonic is a pure function of p = insn >> 21 and the whole decode collapses
# into one 2048-entry table of ops. The immediate form is a function of the
# matched mnemonic alone (checked below) and is emitted per op, not per table
# cell: the interpreter dispatches once on the op, and each handler binds only
# the operand its form defines. The position-fixed register fields
# (rt/ra/rb/rc/ch) are plain bit extractions the handlers write themselves.
#
# Prefixes may match more than one table, and the first stage to match wins, so
# this order IS the decoder's precedence and mirrors spu_disasm.spu_decode:
# RI16 before RI10 (an RI10 opcode equal to the top 8 bits of an RI16 opcode
# must not shadow it), channel ops before RI10, RR before RI7, LSX last, else
# `.word`. With the current tables the only prefixes matched twice are
# rdch/rchcnt/wrch (channel and RR) and lqx/stqx (RR and LSX), where both
# stages agree; the order is kept so a table entry added later resolves
# exactly as the old cascade did instead of silently changing which
# instruction executes.
STAGES = (
    ("rrr", 4, D.RRR_TABLE),
    ("ri18", 7, D.RI18_TABLE),
    ("ri16", 9, D.RI16_TABLE),
    ("chan", 11, CHANNEL),
    ("ri10", 8, D.RI10_TABLE),
    ("ri8", 10, RI8),
    ("rr", 11, D.SPU_RR),
    ("ri7", 11, RI7),
    ("lsx", 11, LSX),
)

# How a handler obtains its immediate or branch target from the instruction
# word `insn` (and `pc`, the address of that word). kind "imm" binds
# `const int32_t I`, kind "tgt" binds `const uint32_t TGT`, None binds nothing:
# a handler that names the operand of another form does not compile.
SX16 = "spu_sx(((insn) >> 7) & 0xFFFF, 16)"
FORMS = (
    ("NONE", "no immediate: RRR, RR (except stop), LSX, channel ops, .word",
     None, None),
    ("I18", "ila: full 18-bit immediate",
     "imm", "(int32_t)(((insn) >> 7) & 0x3FFFF)"),
    ("I18LO", "other RI18 (hbra/hbrr): low 16 bits of the 18-bit field",
     "imm", "(int32_t)(((insn) >> 7) & 0xFFFF)"),
    ("LQA", "lqa/stqa: absolute 16-byte aligned LS address",
     "tgt", "((((insn) >> 7) & 0xFFFF) << 2) & 0x3FFF0"),
    ("LQR", "lqr/stqr: pc-relative 16-byte aligned LS address",
     "tgt", f"((uint32_t)({SX16} * 4) + (pc)) & 0x3FFF0"),
    ("BREL", "br/brsl/brz/brnz/brhz/brhnz: pc-relative target",
     "tgt", f"((uint32_t)({SX16} * 4) + (pc)) & 0x3FFFF"),
    ("BABS", "bra/brasl: absolute target",
     "tgt", f"((uint32_t)({SX16} * 4)) & 0x3FFFF"),
    ("I16S", "il: sign-extended 16-bit immediate",
     "imm", SX16),
    ("I16U", "ilh/ilhu/iohl/fsmbi: zero-extended 16-bit immediate",
     "imm", "(int32_t)(((insn) >> 7) & 0xFFFF)"),
    ("I10", "RI10: sign-extended 10-bit immediate",
     "imm", "spu_sx(((insn) >> 14) & 0x3FF, 10)"),
    ("I10Q", "lqd/stqd: sign-extended 10-bit immediate scaled by 16",
     "imm", "(int32_t)((uint32_t)spu_sx(((insn) >> 14) & 0x3FF, 10) << 4)"),
    ("I8", "RI8 float<->int conversions: unsigned 8-bit scale",
     "imm", "(int32_t)(((insn) >> 14) & 0xFF)"),
    ("STOP", "stop: 14-bit signal type",
     "imm", "(int32_t)((insn) & 0x3FFF)"),
    ("I7S", "RI7: sign-extended 7-bit immediate",
     "imm", "spu_sx(((insn) >> 14) & 0x7F, 7)"),
    ("I7U", "cbd/chd/cwd/cdd: unsigned 7-bit immediate",
     "imm", "(int32_t)(((insn) >> 14) & 0x7F)"),
)

RI16_FORMS = {
    "lqa": "LQA", "stqa": "LQA", "lqr": "LQR", "stqr": "LQR",
    "br": "BREL", "brsl": "BREL", "brz": "BREL", "brnz": "BREL",
    "brhz": "BREL", "brhnz": "BREL", "bra": "BABS", "brasl": "BABS",
    "il": "I16S",
}


def form_of(stage, mn):
    if stage == "ri18":
        return "I18" if mn == "ila" else "I18LO"
    if stage == "ri16":
        return RI16_FORMS.get(mn, "I16U")
    if stage == "ri10":
        return "I10Q" if mn in ("lqd", "stqd") else "I10"
    if stage == "ri8":
        return "I8"
    if stage == "rr":
        return "STOP" if mn == "stop" else "NONE"
    if stage == "ri7":
        return "I7U" if mn in RI7_UNSIGNED else "I7S"
    return "NONE"


def decode_prefix(p):
    """(mnemonic, form) for the 11-bit prefix p = insn >> 21."""
    for stage, bits, table in STAGES:
        mn = table.get(p >> (11 - bits))
        if mn is not None:
            if mn in DECODE_AS_INVALID:
                # The retail CBE SPU rejects these (DECODE_AS_INVALID above):
                # same trap as a word no table claims, not an execution.
                return ".word", "NONE"
            return mn, form_of(stage, mn)
    return ".word", "NONE"


def ident(m):
    return "word" if m == ".word" else m


# The table and the per-op forms, both from decode_prefix. An op reached by two
# prefixes must have the same form at both, or the per-op form would not be the
# form the old per-cell column held.
table = []
op_form = {}
for p in range(2048):
    mn, form = decode_prefix(p)
    if op_form.setdefault(mn, form) != form:
        sys.exit(f"{mn}: forms {op_form[mn]} and {form} at prefix {p:03X}")
    table.append(mn)
unreached = [m for m in mnems if m not in op_form]
if unreached:
    sys.exit(f"mnemonics no prefix decodes to: {unreached}")

out = []
w = out.append
w("/* GENERATED by tools/gen_spu_interp.py -- do not edit. */")
w("/* Opcode->mnemonic enum, the insn>>21 decode table and the per-op operand")
w(" * forms for the SPU interpreter, from the validated spu_disasm.py decoder.")
w(" * Regenerate after any decoder table change. */")
w("")
w("typedef enum {")
for m in mnems:
    w(f"    {enum_name(m)},")
w("    SPU_OP_COUNT")
w("} spu_op;")
w("")
w("static const char* const spu_op_name[SPU_OP_COUNT] = {")
for m in mnems:
    w(f'    [{enum_name(m)}] = "{m}",')
w("};")
w("")
w("static inline int32_t spu_sx(uint32_t v, int bits) {")
w("    uint32_t m = 1u << (bits - 1);")
w("    return (int32_t)((v ^ m) - m);")
w("}")
w("")
w("/* Operand forms. SPU_BIND_<form>(insn, pc) declares the operand a handler of")
w(" * that form may read (I or TGT); SPU_SAVE_<form>(d) stores it into a spu_ins. */")
for f, why, kind, expr in FORMS:
    w(f"/* {f}: {why} */")
    if kind == "imm":
        w(f"#define SPU_BIND_{f}(insn, pc) const int32_t I = {expr}; (void)I;")
        w(f"#define SPU_SAVE_{f}(d) (d)->imm = I;")
    elif kind == "tgt":
        w(f"#define SPU_BIND_{f}(insn, pc) const uint32_t TGT = {expr}; (void)TGT;")
        w(f"#define SPU_SAVE_{f}(d) (d)->tgt = TGT;")
    else:
        w(f"#define SPU_BIND_{f}(insn, pc)")
        w(f"#define SPU_SAVE_{f}(d)")
w("")
w("/* The form of each op, as SPU_FORM_<op> for the handlers and as one list for")
w(" * whoever must visit every op. */")
for m in mnems:
    w(f"#define SPU_FORM_{ident(m)} {op_form[m]}")
w("#define SPU_OP_FORMS(X) \\")
for i, m in enumerate(mnems):
    w(f"    X({ident(m)}, {op_form[m]})" + (" \\" if i + 1 < len(mnems) else ""))
w("")
w('_Static_assert(SPU_OP_COUNT <= 256, "spu_op must fit the uint8_t spu_decode_tab");')
w("")
w("/* Indexed by insn >> 21; precedence is documented in tools/gen_spu_interp.py.")
w(" * dfceq, dfcmeq, dfcgt, dfcmgt and dftsv decode as SPU_word here: the retail")
w(" * CBE SPU raises an invalid-instruction error for them (hwprobe spuexh")
w(" * special A, CECHC04 4.81; issue #124), so they take the undecodable-word")
w(" * trap in spu_interp.c instead of executing. */")
w("static const uint8_t spu_decode_tab[2048] = {")
for base in range(0, 2048, 8):
    cells = [enum_name(table[p]) + "," for p in range(base, base + 8)]
    w(f"    /*{base:03X}*/ " + " ".join(cells))
w("};")

sys.stdout.write("\n".join(out) + "\n")
