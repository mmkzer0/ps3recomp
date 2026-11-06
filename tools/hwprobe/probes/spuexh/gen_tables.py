#!/usr/bin/env python3
"""Build-time table generator of the spuexh probe.

Reads (all from the repository, nothing is typed in twice):
  runtime/spu/tests/test_spu_exhaustive.c   the op table of the host harness (op name, need mask, domain list)
  runtime/spu/tests/spu_exh_gen.h           the domain table (names, order)
  runtime/spu/spu_interp_tables.inc         the toolkit's decode table (prefix -> op) and operand forms

Writes into the output directory (probes/spuexh/generated/, ignored by git):
  spuexh_ops_gen.h      the op table of the probe: name, domains, instruction word template, immediate field
  spuexh_badprefix_gen.h  every 11-bit prefix the toolkit decodes as SPU_word (the decoder-validity probe)
  spuexh_rawexec_gen.S  the SPU routine that executes one raw word with all 128 registers loaded and dumped

Usage: gen_tables.py <repo-root> <out-dir>      (also importable: load_ops(), encode_info())
Standard library only.
"""
import os
import re
import sys

OPS_C = "runtime/spu/tests/test_spu_exhaustive.c"
GEN_H = "runtime/spu/tests/spu_exh_gen.h"
TAB_INC = "runtime/spu/spu_interp_tables.inc"

SCALES = [0, 1, 2, 126, 127, 128, 154, 155, 156, 157, 173, 255]
CVT = ["cflts", "cfltu", "csflt", "cuflt"]

# host op names that are not the ISA mnemonic (the host helpers use the C-intrinsic style names)
MNEMONIC_ALIAS = {"rothma": "rotmah", "rotmhi": "rothmi"}

# the toolkit decodes dftsv's I7 itself (form NONE); the ISA format is RI7
FORM_OVERRIDE = {"dftsv": "I7U"}

IMM_DOMAINS = ("immb", "immh", "immw", "fsmbi", "dtsv")

# ops whose destination register is also an input (the host's third operand c is the old rt)
RT_INPUT = {"addx", "sfx", "cgx", "bgx", "mpyhha", "mpyhhau", "dfma", "dfms", "dfnms", "dfnma"}   # DP multiply-add: rt = ra*rb (+-) rt


def read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8") as f:
        return f.read()


def load_domains(root):
    s = read(root, GEN_H)
    names = re.findall(r'EXH_D\("(\w+)"', s)
    if not names:
        raise SystemExit("no EXH_D domain rows found in " + GEN_H)
    return names


def _strexpr(expr, macros):
    """Value of a C expression made of string literals and string macros (concatenation)."""
    out = []
    for m in re.finditer(r'"([^"]*)"|([A-Z][A-Z0-9_]*)', expr):
        if m.group(1) is not None:
            out.append(m.group(1))
        else:
            if m.group(2) not in macros:
                raise SystemExit(f"unknown macro {m.group(2)!r} in op table domain expression {expr!r}")
            out.append(macros[m.group(2)])
    return "".join(out)


def load_ops(root):
    """The host harness op list, in table order: [{name, need, doms:[names], heavy, fixed_imm}]."""
    s = read(root, OPS_C)
    macros = dict(re.findall(r'#define\s+([A-Z0-9]+)\s+("[^"\n]*")', s))
    macros = {k: v.strip('"') for k, v in macros.items()}
    body = s[s.index("static const OP g_ops[]"):]
    body = body[:body.index("#define NBASE")]
    ops = []
    for m in re.finditer(r'\bORS?_?\(\s*(\w+)\s*,\s*(\d+)\s*,\s*([^()]*?)\)\s*[,}]|\bO\(\s*(\w+)\s*,\s*(\d+)\s*,\s*([^()]*?)\)\s*[,}]|'
                         r'\{\s*"(\w+)"\s*,\s*\w+\s*,\s*\w+\s*,\s*([^{}]*?)\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*(\d+)\s*\}', body):
        if m.group(1):
            name, need, dexp = m.group(1), int(m.group(2)), m.group(3)
        elif m.group(4):
            name, need, dexp = m.group(4), int(m.group(5)), m.group(6)
        else:
            name, need, dexp = m.group(7), int(m.group(9)), m.group(8)
        ops.append((m.start(), name, need, _strexpr(dexp, macros).split(",")))
    ops.sort()
    out = [{"name": n, "need": nd, "doms": d, "heavy": 0, "fixed_imm": None} for _, n, nd, d in ops]
    # the 48 conversion ops, built at start-up by the host harness (scale is the raw I8 field)
    for o, nm in enumerate(CVT):
        for sc in SCALES:
            heavy = 0 if sc == (173 if o < 2 else 155) else 1
            out.append({"name": f"{nm}@{sc}", "need": 1, "doms": ["w32"], "heavy": heavy, "fixed_imm": sc})
    names = [o["name"] for o in out]
    if len(set(names)) != len(names):
        raise SystemExit("duplicate op names in the host op table")
    return out


def load_decode(root):
    """(enum names list index->name, decode table 2048 of names, forms dict mnemonic->FORM)."""
    s = read(root, TAB_INC)
    m = re.search(r'typedef enum \{(.*?)\} spu_op;', s, re.DOTALL)
    enum = re.findall(r'SPU_(\w+)', m.group(1))
    m = re.search(r'spu_decode_tab\[2048\]\s*=\s*\{(.*?)\};', s, re.DOTALL)
    tab = re.findall(r'SPU_(\w+)', m.group(1))
    if len(tab) != 2048:
        raise SystemExit(f"decode table has {len(tab)} entries, expected 2048")
    forms = dict(re.findall(r'#define SPU_FORM_(\w+) (\w+)', s))
    return enum, tab, forms


# field layouts: (prefix count, form) -> kind
def encode_info(name, tab, forms):
    """Instruction word template for mnemonic `name`: dict(kind, base, imm_shift, imm_bits)."""
    pre = [i for i, t in enumerate(tab) if t == name]
    if not pre:
        raise SystemExit(f"op {name!r} is not in the toolkit decode table")
    n = len(pre)
    if pre != list(range(pre[0], pre[0] + n)) or pre[0] % n:
        raise SystemExit(f"op {name!r}: prefixes {pre} are not one aligned opcode block")
    form = forms.get(name, "NONE")
    if name in FORM_OVERRIDE:
        form = FORM_OVERRIDE[name]
    base = pre[0] << 21
    if n == 128:
        return {"kind": "RRR", "base": base, "imm_shift": 0, "imm_bits": 0}
    if n == 1:
        if form in ("I7S", "I7U"):
            return {"kind": "RI7", "base": base, "imm_shift": 14, "imm_bits": 7}
        if form == "NONE":
            return {"kind": "RR", "base": base, "imm_shift": 0, "imm_bits": 0}
    if n == 2 and form == "I8":
        return {"kind": "RI8", "base": base, "imm_shift": 14, "imm_bits": 8}
    if n == 4 and form in ("I16S", "I16U"):
        return {"kind": "RI16", "base": base, "imm_shift": 7, "imm_bits": 16}
    if n == 8 and form == "I10":
        return {"kind": "RI10", "base": base, "imm_shift": 14, "imm_bits": 10}
    if n == 16 and form == "I18":
        return {"kind": "RI18", "base": base, "imm_shift": 7, "imm_bits": 18}
    raise SystemExit(f"op {name!r}: unsupported shape ({n} prefixes, form {form})")


def reg_fields(info, need, name, has_a=True):
    """(rt, ra, rb, rc) register numbers of the stub instruction. a/b/c are passed in $3/$4/$5, result read from rt."""
    kind = info["kind"]
    rt = 5 if name in RT_INPUT else 3
    if kind == "RRR":
        return rt, 3, 4, 5
    if kind == "RR":
        return rt, 3, 4 if (need & 2) else 0, 0
    if kind in ("RI7", "RI8", "RI10"):
        return 3, 3, 0, 0
    if kind == "RI16":
        return 3, 0, 0, 0       # il, ila-like loaders and fsmbi have no ra; iohl reads rt = $3 = a
    if kind == "RI18":
        return 3, 0, 0, 0
    raise SystemExit("bad kind")


def word_template(info, rt, ra, rb, rc):
    k = info["kind"]
    b = info["base"]
    if k == "RRR":
        return b | (rt << 21) | (rb << 14) | (ra << 7) | rc
    if k == "RR":
        return b | (rb << 14) | (ra << 7) | rt
    if k in ("RI7", "RI8", "RI10"):
        return b | (ra << 7) | rt
    if k in ("RI16", "RI18"):
        return b | rt
    raise SystemExit("bad kind")


def build_op_rows(root):
    domains = load_domains(root)
    ops = load_ops(root)
    _enum, tab, forms = load_decode(root)
    rows = []
    for o in ops:
        mn = o["name"].split("@")[0]
        mn = MNEMONIC_ALIAS.get(mn, mn)
        info = encode_info(mn, tab, forms)
        rt, ra, rb, rc = reg_fields(info, o["need"], mn)
        w = word_template(info, rt, ra, rb, rc)
        for d in o["doms"]:
            if d not in domains:
                raise SystemExit("op {}: unknown domain {!r}".format(o["name"], d))
        imm_dom = any(d in IMM_DOMAINS for d in o["doms"])
        if imm_dom != (info["imm_bits"] > 0 and o["fixed_imm"] is None):
            raise SystemExit("op {}: immediate domain/instruction shape mismatch (doms {}, kind {})".format(o["name"], o["doms"], info["kind"]))
        rows.append(dict(o, mnemonic=mn, kind=info["kind"], word=w, imm_shift=info["imm_shift"],
                         imm_bits=info["imm_bits"], rt=rt, dom_idx=[domains.index(d) for d in o["doms"]]))
    return domains, rows, tab


def emit_ops(domains, rows, out):
    L = ["/* GENERATED by gen_tables.py -- do not edit. The op table of the spuexh probe. */", "",
         f"#define SPUEXH_NDOMS_GEN {len(domains)}", f"#define SPUEXH_NOPS {len(rows)}", ""]
    L.append(f"static const char *const spuexh_dom_names[{len(domains)}] = {{")
    L += [f'    "{d}",' for d in domains]
    L += ["};", ""]
    L.append("/* word: instruction template with the registers filled in (a=$3 b=$4 c=$5, result in rt); the immediate field")
    L.append("   (imm_bits wide at imm_shift) is OR-ed in at run time from the call's imm (fixed_imm >= 0: the op's own). */")
    L.append("static const spuexh_op_t spuexh_ops[SPUEXH_NOPS] = {")
    for o in rows:
        doms = list(o["dom_idx"]) + [255] * (5 - len(o["dom_idx"]))
        assert len(o["dom_idx"]) <= 5
        L.append('    {{ "{}", 0x{:08x}, {}, {}, {}, {}, {}, {}, {{{}}} }},'.format(
            o["name"], o["word"], o["imm_shift"], o["imm_bits"], o["need"], o["heavy"],
            -1 if o["fixed_imm"] is None else o["fixed_imm"], o["rt"], ",".join(str(d) for d in doms)))
    L += ["};", ""]
    with open(out, "w") as f:
        f.write("\n".join(L))


def emit_badprefix(tab, out):
    bad = [i for i, t in enumerate(tab) if t == "word"]
    L = ["/* GENERATED by gen_tables.py -- do not edit. 11-bit prefixes (insn >> 21) the toolkit's decode table maps to SPU_word. */",
         f"#define SPUEXH_NBAD {len(bad)}", "static const unsigned short spuexh_bad_prefix[SPUEXH_NBAD] = {"]
    for k in range(0, len(bad), 12):
        L.append("    " + ", ".join(f"0x{b:03x}" for b in bad[k:k + 12]) + ",")
    L += ["};", ""]
    with open(out, "w") as f:
        f.write("\n".join(L))
    return bad


# Layout of the raw-exec scratch (absolute LS addresses are linker symbols, see rawexec .S below).
def emit_rawexec(out):
    L = []
    a = L.append
    a("/* GENERATED by gen_tables.py -- do not edit.")
    a(" *")
    a(" * void spuexh_rawexec(void)   (no arguments; everything is in the fixed buffers below)")
    a(" *   1. saves $0..$127 to spuexh_save")
    a(" *   2. loads $0..$127 from spuexh_init")
    a(" *   3. falls into the word in spuexh_slot (patched by the caller, followed by sync)")
    a(" *   4. dumps $0..$127 to spuexh_dump with absolute stqa (no register is needed to address them)")
    a(" *   5. branches (absolute) to the restore code, which reloads $0..$127 from spuexh_save and returns")
    a(" * No instruction after step 3 depends on any register value, so a word that clobbers any register, including")
    a(" * $0 and $1, cannot lose the way back. A word that branches, stops or faults never reaches step 4: the")
    a(" * caller detects that from the progress flag it wrote beforehand.")
    a(" */")
    a("\t.section .text")
    a("\t.align 4")
    a("\t.global spuexh_rawexec")
    a("\t.type spuexh_rawexec, @function")
    a("spuexh_rawexec:")
    for r in range(128):
        a(f"\tstqa ${r}, spuexh_save+{r * 16}")
    for r in range(128):
        a(f"\tlqa ${r}, spuexh_init+{r * 16}")
    a("\tsync")
    a("\tbra spuexh_slot")
    a("\t.size spuexh_rawexec, .-spuexh_rawexec")
    a("")
    a("\t.section .data")
    a("\t.align 4")
    a("\t.global spuexh_slot")
    a("spuexh_slot:")
    a("\t.long 0x40200000")     # nop placeholder: ori $0,$0,0? replaced by the caller
    for r in range(128):
        a(f"\tstqa ${r}, spuexh_dump+{r * 16}")
    a("\tbra spuexh_restore")
    a("\t.align 4")
    a("spuexh_restore:")
    for r in range(128):
        a(f"\tlqa ${r}, spuexh_save+{r * 16}")
    a("\tbi $0")
    a("")
    a("\t.section .bss")
    a("\t.align 4")
    for nm in ("spuexh_save", "spuexh_init", "spuexh_dump"):
        a(f"\t.global {nm}")
        a(f"{nm}:")
        a("\t.space 2048")
    a("")
    with open(out, "w") as f:
        f.write("\n".join(L))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, outd = sys.argv[1], sys.argv[2]
    os.makedirs(outd, exist_ok=True)
    domains, rows, tab = build_op_rows(root)
    emit_ops(domains, rows, os.path.join(outd, "spuexh_ops_gen.h"))
    bad = emit_badprefix(tab, os.path.join(outd, "spuexh_badprefix_gen.h"))
    emit_rawexec(os.path.join(outd, "spuexh_rawexec_gen.S"))
    print(f"gen_tables: {len(rows)} ops, {len(domains)} domains, {len(bad)} undecoded prefixes")


if __name__ == "__main__":
    main()
