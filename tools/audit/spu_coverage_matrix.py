#!/usr/bin/env python3
"""SPU op coverage matrix. Static analysis of the tree (enum, interpreter switch, spu_helpers.h backend
branches, test sources) joined with a hand-audited class table (oracles, domains).
usage: spu_coverage_matrix.py <repo> > coverage.csv"""
import csv
import glob
import os
import re
import sys

repo = sys.argv[1]
def rd(p):
    with open(f"{repo}/{p}", errors="ignore") as fh:
        return fh.read()
H = rd("runtime/spu/spu_helpers.h"); HL = H.split("\n")
I = rd("runtime/spu/spu_interp.c"); T = rd("runtime/spu/spu_interp_tables.inc")
ops = [m for m in re.findall(r"^\s*SPU_(\w+),", T.split("} spu_op;")[0], re.MULTILINE) if m != "OP_COUNT"]

# ---- interpreter case -> helper
sw = I[I.index("switch (spu_decode_tab[insn >> 21])"):I.index("    default:\n        fprintf")]
case_help = {}
# one `OP(name, body)` entry per op; the body runs to the next entry (bodies may span lines)
for m in re.finditer(r"^\s*OP\((\w+),(.*?)(?=^\s*OP\(|\Z)", sw, re.DOTALL | re.MULTILINE):
    hs = sorted({h for h in re.findall(r"\b(spu_\w+)\s*\(", m.group(2))
                    if not re.match(r"spu_(ls_|trace|link$|pref_u32|splat|zero|rdch|wrch|rchcnt)", h)})
    case_help[m.group(1)] = hs

# ---- helper definitions -> backend tags (a one-line `return spu_X_ref(...)` alias is NOT a distinct path)
stack = []; defs = {}
for n, l in enumerate(HL, 1):
    m = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", l)
    if m:
        k, r = m.group(1), m.group(2).strip()
        if k in ("if", "ifdef", "ifndef"): stack.append([(k, r)])
        elif k in ("elif", "else"): stack[-1].append((k, r))
        else: stack.pop()
        continue
    m = (re.match(r"(?:static\s+)?(?:inline\s+)?(?:SPU__\w+\s+)?(?:u128|uint\d+_t|int\d+_t|int|void|double|float)\s+(spu_\w+)\s*\(", l)
         or re.match(r"SPU__XI\s+\S+\s+(spu_\w+)\s*\(", l) or re.match(r"SPU__XE_INLINE\s+\S+\s+(spu_\w+)\s*\(", l)
         or re.match(r"static\s+inline\s+\S+\s+(spu_\w+)\s*\(", l))
    mac = re.match(r"SPU__SSE_CMP\((spu_\w+),", l) or re.match(r"SPU__SF_V[23]\((spu_\w+_ref),", l)
    if mac: m = mac
    if not m or (l.rstrip().endswith(";") and not mac): continue
    alias = bool(re.search(r"\{\s*return\s+spu_\w+_ref\(.*\}\s*$", l))
    tags = set()
    for lvl in stack:
        k, r = lvl[-1]
        if k == "else": tags.add("else")
        elif k in ("if", "elif"):
            pos = re.sub(r"!\s*defined\s*\(\s*\w+\s*\)", "", r)
            if "__ARM_NEON" in pos: tags.add("NEON")
            if re.search(r"\bSPU_X86\b|__SSE4_1__", pos): tags.add("SSE4.1")
            if "__AVX2__" in pos: tags.add("AVX2")
            if re.search(r"__SSE2__|_M_X64", pos): tags.add("SSE2")
    defs.setdefault(m.group(1), []).append(set() if alias else (tags or {"plain"}))
POS = {"spu_cbd": "spu_cbd_pos", "spu_chd": "spu_chd_pos", "spu_cdd": "spu_cdd_pos", "spu_cbx": "spu_cbd_pos",
       "spu_chx": "spu_chd_pos", "spu_cwx": "spu_cwd_pos", "spu_cdx": "spu_cdd_pos"}   # thin wrappers over the *_pos generators
def backends(h):
    h = POS.get(h, h)
    t = defs.get(h)
    if t is None: return set()
    s = {"scalar"}
    for x in t:
        s |= x - {"else", "plain"}
    return s
has_ref = lambda h: f"{h}_ref" in defs or re.search(rf"\b{h}_ref\b", H) is not None

# ---- tests
def _read_abs(f):
    with open(f, errors="ignore") as fh:
        return fh.read()
tests = {os.path.basename(f): _read_abs(f) for f in
         sorted(glob.glob(f"{repo}/runtime/spu/tests/test_spu_*.c") + [f"{repo}/runtime/spu/tests/spu_interp_diff_test.c"])}
DOMAIN = {   # test -> (short name, what it compares, domain, arch gating)
 "test_spu_helpers.c": ("helpers", "ISA vectors; fcmp family vs _ref", "directed + all edge-value pairs + 1M random (fcmp only)", "all"),
 "test_spu_int_neon.c": ("int_neon", "helper vs _ref", "sampled: 200k..800k random x ops, small shift counts, equal operands", "NEON only (SKIP, exit 77, elsewhere)"),
 "test_spu_int_x86.c": ("int_x86", "helper vs _ref", "sampled: 2M iterations, edge words + random", "SPU_X86 only (SSE4.1/AVX2)"),
 "test_spu_float_neon.c": ("float_neon", "fa/fs/fm/fma/fms/fnms vs _ref", "sampled: 4M + boundary 2M + zero-lane 2M", "NEON or AVX2"),
 "test_spu_float_exact.c": ("float_exact", "fa/fs/fm/fma/fms/fnms (+dp) vs exact _ref", "structured: all exponent pairs/triples x edge fractions + 256M random lanes; FPCR RZ pass on arm64", "all"),
 "test_spu_float_est.c": ("float_est", "frest/frsqest/fi/cflts/cfltu/csflt/cuflt vs _ref", "sampled: 2M x4 lanes + every imm x 2000", "AVX2 only"),
 "test_spu_numeric_semantics.c": ("numeric", "directed spot checks", "17 vectors", "all"),
 "test_spu_shufb.c": ("shufb", "spu_shufb and _ref vs an ISA selector model", "all 256 selectors x 16 positions + 200k random", "all"),
 "spu_interp_diff_test.c": ("rpcs3_diff", "interpreter(+lifter) vs RPCS3", "all imm-field values + 81 reg-alias patterns + ~2.7k random/special operand trials per op", "all (x86 oracle)"),
}
def mentions(h):
    h = POS.get(h, h)
    base = h[4:]; out = []
    for fn, src in tests.items():
        if fn == "spu_interp_diff_test.c": continue
        if re.search(rf"\b{h}(_ref)?\b", src) or re.search(rf"\b(?:C[123I]|T[123I]|CHECK[12I])\(\s*{base}\b", src) or re.search(rf"\bSPU__SSE_CMP\(\s*{h}\b", src):
            out.append(DOMAIN[fn][0] if fn in DOMAIN else fn[9:-2])
    return out

# ---- class table: op -> (class, rpcs3 oracle, min exhaustive domain)
RPCS3_PRECISE = {"fcgt", "fa", "fs", "fm", "fcmgt", "dfa", "dfs", "dfm", "dfma", "dfms", "dfnms", "dfnma", "fscrrd", "fesd", "frds", "fscrwr", "fceq", "fcmeq", "cflts", "cfltu", "csflt", "cuflt", "fnms", "fma", "fms"}
HAND_ONLY = {"dfceq", "dfcgt", "dfcmeq", "dfcmgt", "dftsv"}
NO_ORACLE = {"fi"}
def cls(op):
    g = lambda *s: op in s
    if g("word"): return ("pseudo", "-", "-")
    if g("and","or","xor","nand","nor","andc","orc","eqv","selb"): return ("logic", "2^3 bit truth table (bit-sliced) / 2^96 selb", "bitwise: 8 (a,b[,c]) bit combinations prove all 128 bits")
    if g("andi","ori","xori","andbi","orbi","xorbi","andhi","orhi","xorhi","iohl","ilh","il","ila","ilhu","fsmbi"): return ("imm-logic/load", "imm field", "imm 2^10/2^16/2^18 x bit-sliced data")
    if g("a","sf","addx","sfx","cg","cgx","bg","bgx","ai","sfi"): return ("int32 add/sub/carry", "2^64 per lane (2^96 with carry-in)", "bit-serial carry chain: 2^2 x 32 positions proves a/sf; carry ops: boundary classes + 2^32 one-operand sweep")
    if g("ah","sfh","ahi","sfhi"): return ("int16 lane", "2^32 per lane", "all 2^32 halfword pairs (8 lanes/call: 2^29 calls)")
    if g("ceq","cgt","clgt","ceqi","cgti","clgti"): return ("cmp32", "2^64 per lane", "equal/ordering classes + 2^32 sweep vs sign-flipped neighbours; imm forms 2^10 x 2^32")
    if g("ceqh","cgth","clgth","ceqhi","cgthi","clgthi"): return ("cmp16", "2^32 per lane", "all 2^32 halfword pairs; imm forms 2^16 x 2^10")
    if g("ceqb","cgtb","clgtb","ceqbi","cgtbi","clgtbi","absdb","avgb"): return ("byte lane", "2^16 per lane", "all 65536 byte pairs (imm forms: 2^8 x 2^10 imm)")
    if g("shl","rot","rotm","rotma"): return ("shift32 variable", "count 2^6 (low 6 bits of rb) x data 2^32", "all 64 counts x 2^32 data words; plus count-high-bit masking: 2^32 count words x 8 data classes")
    if g("shli","roti","rotmi","rotmai"): return ("shift32 imm", "i7 2^7 x data 2^32", "all 128 i7 x 2^32 data (2^39) or data class sweep")
    if g("shlh","roth","rothm","rotmah"): return ("shift16 variable", "count 2^5/2^6 x data 2^16", "all counts x all 2^16 data halfwords (and all 2^16 count halfwords for masking)")
    if g("shlhi","rothi","rothmi","rotmahi"): return ("shift16 imm", "i7 x data 2^16", "all 128 i7 x 2^16 data")
    if g("shlqby","rotqby","rotqmby","shlqbyi","rotqbyi","rotqmbyi","shlqbybi","rotqbybi","rotqmbybi"): return ("quad byte shift/rot", "count 2^5..2^8 (data-independent byte permutation)", "all count values x identity-byte pattern (16B distinct): 2^8 calls")
    if g("shlqbi","rotqbi","rotqmbi","shlqbii","rotqbii","rotqmbii"): return ("quad bit shift/rot", "count 2^3 (+byte part ignored)", "all 8 counts x 128-bit patterns (single-bit sweep proves linear shift)")
    if g("shufb"): return ("shuffle", "control byte 2^8 per lane", "256 control byte values x 16 lanes with identity data (256 calls) + 0xC0/0xE0/0x80 special classes")
    if g("fsm","fsmb","fsmh","gb","gbh","gbb"): return ("mask/gather", "2^4 / 2^8 / 2^16 relevant bits", "all 2^4 (fsm,gb) / 2^8 (fsmh,gbh) / 2^16 (fsmb,gbb) relevant-bit patterns")
    if g("cntb"): return ("byte popcount", "2^8 per lane", "all 256 byte values")
    if g("clz"): return ("clz32", "2^32 per lane", "all 2^32 words (4 lanes/call: 2^30 calls)")
    if g("xsbh"): return ("sign-extend", "2^8 per lane", "all 256 low-byte values")
    if g("xshw"): return ("sign-extend", "2^16 per lane", "all 65536 low-halfword values")
    if g("xswd"): return ("sign-extend", "2^32 per lane", "all 2^32 low-word values")
    if g("orx"): return ("reduce", "bitwise", "bit-sliced: 2^4 word-OR patterns")
    if g("mpy","mpyu","mpyh","mpyhh","mpyhhu","mpys","mpyi","mpyui"): return ("mul16x16", "2^32 per lane (imm forms 2^16 x 2^10)", "all 2^32 (x,y) halfword pairs placed in the relevant halves")
    if g("mpya","mpyhha","mpyhhau"): return ("mul16x16+acc", "2^32 x 2^32 accumulator", "2^32 pairs with accumulator classes (add is linear): 0, -1, random")
    if g("sumb"): return ("byte sum", "2^64 per word pair", "per-byte linear: bit-serial check or 8 byte-position one-hot sweeps x 2^8 x 2^8")
    if g("cbd","chd","cwd","cdd"): return ("gen-control imm", "(ra.w0 + i7) & 0xF: 2^4 positions", "all 16 positions (x 2^7 i7)")
    if g("cbx","chx","cwx","cdx"): return ("gen-control reg", "2^4 positions", "all 16 positions (ra.w0+rb.w0 low nibble)")
    if g("fa","fs","fm"): return ("float32 arith", "2^64", "exponent-pair (2x256x256 x signs) x fraction-edge classes: structured, not exhaustive; unary slices (x op const) exhaustive 2^32")
    if g("fma","fms","fnms"): return ("float32 fused", "2^96", "all exponent triples 2^24 x fraction-edge classes (done in float_exact pass 2)")
    if g("fceq","fcgt","fcmeq","fcmgt"): return ("float32 cmp", "2^64", "compare depends on (sign, mag, zero-class): 2^32 vs fixed probe words + structured pairs")
    if g("frest","frsqest"): return ("float32 estimate", "2^32 per lane", "ALL 2^32 words (4 lanes/call: 2^30 calls, seconds with AVX2)")
    if g("fi"): return ("float32 interpolate", "ra low 19 bits x rb 32 bits = 2^51", "rb exp irrelevant beyond 0/underflow: (base13,step10) 2^23 x y 2^19 = 2^42 sampled by y; exhaustive over (base,step) with y in {0,1,2^k,all-ones,random}")
    if g("cflts","cfltu","csflt","cuflt"): return ("int<->float", "2^32 data x i8 (128 defined scales)", "all 2^32 words x each of 128 defined scales = 2^39 lane-ops (~10-20 min AVX2); invalid scales > 127 are ISA-undefined")
    if g("fesd","frds"): return ("float<->double", "2^32 per lane", "fesd: all 2^32 floats; frds: 2^64 doubles -> exponent/rounding classes + 2^32 sweep of the high word")
    if op.startswith("df"): return ("double", "2^64..2^192", "class-structured (exp, frac edges, specials); dftsv: 2^64 x 2^7 mask -> classify per 7 flags exhaustive over 11 class representatives")
    if g("fscrrd","fscrwr","mfspr","mtspr"): return ("fpscr/spr", "-", "constant behaviour (reads zero)")
    if re.match(r"(lq|stq)", op): return ("ls access", "addr 2^18 x data", "all 16B-aligned addrs/wrap + masking low 4 bits: boundary classes")
    if re.match(r"(br|bi)", op) or op in ("iret",): return ("branch", "cond x target 2^16", "cond {0,nonzero,0x10000} x target classes (+E/D bits for bi*)")
    if re.match(r"h(eq|gt|lgt)", op) or g("stop","stopd"): return ("halt/stop", "cond classes", "cond true/false x imm 2^14")
    if g("rdch","wrch","rchcnt"): return ("channel", "ch 2^7", "all 128 channel numbers (stubbed in unit tests)")
    if g("nop","lnop","sync","dsync","hbr","hbra","hbrr"): return ("nop/hint", "-", "n/a")
    return ("other", "?", "?")

def oracle(op, hs):
    if op == "word": return "-"
    o = []
    if op in NO_ORACLE: o.append("RPCS3: NONE (UNK, 0 trials)")
    elif op in HAND_ONLY: o.append("hand ref_exec (RPCS3 lacks)")
    elif op in RPCS3_PRECISE: o.append("RPCS3 precise")
    else: o.append("RPCS3 fast")
    if any(has_ref(h) for h in hs): o.append("helper _ref")
    if op in ("fa","fs","fm","fma","fms","fnms","cflts","cfltu","csflt","cuflt","fceq","fcgt","fcmeq","fcmgt"): o.append("py exact referee (ISA rational)")
    if op in ("frest","frsqest"): o.append("(RPCS3 LUT == _ref LUT: same table, not independent)")
    if op in ("dfa","dfs","dfm","dfma","dfms","dfnma","dfnms"): o.append("float_exact dp pass (host IEEE)")
    return " + ".join(o)

w = csv.writer(sys.stdout)
w.writerow(["op", "class", "helpers", "distinct_backend_paths", "x86_default_build_path", "avx512_path", "oracles",
            "unit_tests_mentioning_helper", "diff_test_trials_domain", "full_operand_domain", "smallest_exhaustive_domain", "parity_test_for_fast_path"])
for op in ops:
    hs = case_help.get(op, [])
    c, dom, ex = cls(op)
    bk = set()
    for h in hs: bk |= backends(h)
    nonscalar = sorted(bk - {"scalar"}, key=lambda x: ["SSE2","SSE4.1","AVX2","NEON"].index(x))
    # which x86 backend a plain x86_64 (no -msse4.1) build gets
    x86def = "-"
    if hs:
        x86def = "SSE2" if "SSE2" in bk else "scalar"
    ms = sorted({m for h in hs for m in mentions(h)})
    # parity-test status for each non-scalar backend
    st = []
    for b in nonscalar:
        gate = {"NEON": {"int_neon", "float_neon", "float_est", "float_exact", "helpers", "shufb"},
                "SSE4.1": {"int_x86", "shufb", "helpers"}, "AVX2": {"int_x86", "float_neon", "float_est", "float_exact", "helpers"},
                "SSE2": {"helpers"}}[b]
        if b == "NEON": ok = any(m in gate for m in ms)
        else: ok = any(m in gate for m in ms)
        st.append(f"{b}:{'yes' if ok else 'NO'}")
    diff = "none" if op in NO_ORACLE else ("lifter+interp" if op != "word" else "-")
    w.writerow([op, c, ";".join(hs) or "(inline in spu_interp.c)", "/".join(["scalar"] + nonscalar) if hs else "-", x86def, "none (no AVX-512 intrinsics anywhere)" if hs else "-",
                oracle(op, hs), ";".join(ms) or "-", "0 trials" if op in NO_ORACLE else ("~2.7k sampled; imm fields exhaustive" if op != "word" else "-"),
                dom, ex, ";".join(st) or "n/a"])
