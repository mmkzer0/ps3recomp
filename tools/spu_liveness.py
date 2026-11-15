#!/usr/bin/env python3
"""Register and stack liveness of an SPU image, function by function.

This is what a native override (tier T2, docs/SPU_XDIFF.md section 8.2) may
rely on: which registers a function's callers can still read after it returns,
and how much of the stack below its entry stack pointer is dead once it has.
Nothing here is assumed from the SPU ABI; a register is dead only when the
analysis proves that no caller reads it before writing it, and every doubt
makes it live. The offline runner's poison run (`spu_job_replay --poison`)
checks the claims against a recorded job.

Model
-----
* A function is a bound of the lifter's function list (tools/find_spu_functions.py
  `detect_functions`, split at extra entry points and capped at --code-end exactly
  as tools/spu_lifter.py does), or the list passed with --functions.
* Liveness is a backward dataflow over every instruction of every bound, with
  exact register defs and uses per instruction (spu_decode's mnemonics; an
  instruction this tool does not know uses every register and defines none).
* A call (`brsl`/`brasl`) to a function entry E is live-in(E) minus the link
  register, plus live-in(next instruction): a callee is never assumed to write
  anything, only the link is. A call to anything else, or an indirect call
  (`bisl`), uses every register. A branch into another bound is an edge into
  that bound, and that bound's live-out then contains this one's (the return of
  a tail call goes to this function's callers).
* live_out(F) is the union, over every call site of F, of live-in at the
  instruction after the call, plus live_out of every function that branches
  into F. $r1 is always live-out (the stack pointer is the caller's).
* A function has `unknown_callers` when anything but a direct call or branch
  can reach it: a given entry point, no direct caller at all, its address
  materialised as a constant (`ila`, `il`, `ilhu`+`iohl`) or stored as an
  aligned word anywhere in the image, a call landing inside it, or the return
  continuation of a call. Its live_out is then every register.
* Terminating `stop` (any code outside find_spu_functions.LV2_RESUMING_STOPS)
  ends the program: nothing is live after it. A resuming stop uses every
  register, as does any branch whose target the tool cannot see (`bi $rN`,
  a computed `bi $r0`, `iret`, `bisled`, a branch out of the image).
* dead_stack: the frame [entry_sp - n, entry_sp), when a forward analysis of the
  stack pointer proves that the function lowers $r1 only with `ai $r1,$r1,imm`,
  that it never reaches an indirect branch, that every path leaves it with $r1
  back at its entry value, and n is the deepest the pointer goes. Calls are
  assumed to return with $r1 unchanged; the poison run checks that at every
  return it poisons. No frame: null.

Limits: a function entered through arithmetic on a table base (offset tables,
halfword tables, `ila` plus `ai`) is not seen as address-taken; name such
entries with --entry. Registers and stack bytes of a terminating `stop` are
not part of any claim.

Output: JSON `ps3recomp.spu-liveness/1`, one object per function (docs/SPU_LIFTER.md "Liveness").

Usage:
    spu_liveness.py IMAGE.bin --base 0x7C00 [--code-end N] [--entry N]... [--functions FILE] [-o live.json]
    spu_liveness.py --elf IMAGE.elf [-o live.json]
"""
import argparse
import bisect
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from find_spu_functions import (
    LV2_RESUMING_STOPS,
    detect_functions,
    parse_elf,
    text_image,
)
from spu_disasm import disassemble_spu
from spu_elf import fnv1a64
from spu_lifter import compute_bi_r0_jumps

SCHEMA = "ps3recomp.spu-liveness/1"
LS_MASK = 0x3FFFF
ALL = (1 << 128) - 1
SP = 1

# instruction kinds (control flow shape)
PLAIN, JUMP, COND, CALL, ICALL, RET, CRET, IJUMP, CIJUMP, TERM, STOPALL = range(11)
_FALLS = (PLAIN, COND, CALL, ICALL, CRET, CIJUMP, STOPALL)

# ---- register defs and uses by mnemonic ---------------------------------------------------
def _names(text):
    return frozenset(text.split())


# RT written, RA and RB read.
_RT_RA_RB = _names("""
    sf or bg sfh nor absdb a and cg ah nand avgb rot rotm rotma shl roth rothm rotmah shlh
    rotqbybi rotqmbybi shlqbybi rotqbi rotqmbi shlqbi rotqby rotqmby shlqby cbx chx cwx cdx
    xor eqv andc orc cgt cgth cgtb sumb clgt clgth clgtb ceq ceqh ceqb mpy mpyh mpyhh mpys
    mpyu mpyhhu fcgt dfcgt fa fs fm fcmgt dfcmgt dfa dfs dfm fceq dfceq fcmeq dfcmeq fi lqx
""")
# RT written, RA read (RB, where the format has one, holds an immediate).
_RT_RA = _names("""
    lqd ai sfi ahi sfhi andi ori orhi orbi andhi andbi xori xorhi xorbi ceqi ceqhi ceqbi
    cgti cgthi cgtbi clgti clgthi clgtbi mpyi mpyui cflts cfltu csflt cuflt roti rotmi rotmai
    shli rothi rothmi rotmahi shlhi cbd chd cwd cdd rotqbii rotqmbii shlqbii rotqbyi rotqmbyi
    shlqbyi gb gbh gbb fsm fsmh fsmb clz xswd xshw cntb xsbh orx fesd frds dftsv frest frsqest
""")
# RT written, nothing read.
_RT_ONLY = _names("il ilh ilhu ila fsmbi rdch rchcnt fscrrd mfspr lqa lqr")
# RT read and written, RA and RB read (an accumulator).
_RT_ACC = _names("addx sfx cgx bgx mpyhha mpyhhau dfma dfms dfnms dfnma")
_RRR = _names("selb shufb mpya fnms fma fms")   # RT4 (bits 21-27) written; RA, RB, RC read
# Nothing written; the listed fields are read.
_USE_RT = _names("stqa stqr wrch mtspr")
_USE_RT_RA = _names("stqd")
_USE_RT_RA_RB = _names("stqx stopd")
_USE_RA_RB = _names("hgt hlgt heq")
_USE_RA = _names("hgti hlgti heqi hbr")
_USE_RT_RA_ONLY = _names("fscrwr")   # RT may be written by the ISA: read, never killed
_NO_REGS = _names("nop lnop sync syncc dsync hbra hbrr")
_BRANCHES = _names("br bra brsl brasl brz brnz brhz brhnz bi bisl biz binz bihz bihnz bisled iret stop")

KNOWN = (_RT_RA_RB | _RT_RA | _RT_ONLY | _RT_ACC | _RRR | _USE_RT | _USE_RT_RA | _USE_RT_RA_RB
         | _USE_RA_RB | _USE_RA | _USE_RT_RA_ONLY | _NO_REGS | _BRANCHES | {"iohl"})


def _sext(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def _bit(reg):
    return 1 << reg


def describe(ins, computed_jumps):
    """(kind, uses, defs, target) of one decoded instruction; uses and defs are 128-bit masks.

    `target` is the static branch or call target (an LS address) or None.
    """
    m, raw, addr = ins.mnemonic, ins.raw, ins.addr
    rt, ra, rb, rc = raw & 0x7F, (raw >> 7) & 0x7F, (raw >> 14) & 0x7F, (raw >> 21) & 0x7F
    i16 = _sext((raw >> 7) & 0xFFFF, 16)
    if m in _RRR:
        return PLAIN, _bit(ra) | _bit(rb) | _bit(rt), _bit(rc), None
    if m in _RT_RA_RB:
        return PLAIN, _bit(ra) | _bit(rb), _bit(rt), None
    if m in _RT_RA:
        return PLAIN, _bit(ra), _bit(rt), None
    if m in _RT_ONLY:
        return PLAIN, 0, _bit(rt), None
    if m in _RT_ACC:
        return PLAIN, _bit(rt) | _bit(ra) | _bit(rb), _bit(rt), None
    if m == "iohl":
        return PLAIN, _bit(rt), _bit(rt), None
    if m in _USE_RT:
        return PLAIN, _bit(rt), 0, None
    if m in _USE_RT_RA:
        return PLAIN, _bit(rt) | _bit(ra), 0, None
    if m in _USE_RT_RA_RB:
        return (TERM if m == "stopd" else PLAIN), _bit(rt) | _bit(ra) | _bit(rb), 0, None
    if m in _USE_RA_RB:
        return PLAIN, _bit(ra) | _bit(rb), 0, None
    if m in _USE_RA:
        return PLAIN, _bit(ra), 0, None
    if m in _USE_RT_RA_ONLY:
        return PLAIN, _bit(rt) | _bit(ra), 0, None
    if m in _NO_REGS:
        return PLAIN, 0, 0, None
    if m == "br":
        return JUMP, 0, 0, (addr + 4 * i16) & LS_MASK
    if m == "bra":
        return JUMP, 0, 0, (4 * i16) & LS_MASK
    if m == "brsl":
        return CALL, 0, _bit(rt), (addr + 4 * i16) & LS_MASK
    if m == "brasl":
        return CALL, 0, _bit(rt), (4 * i16) & LS_MASK
    if m in ("brz", "brnz", "brhz", "brhnz"):
        return COND, _bit(rt), 0, (addr + 4 * i16) & LS_MASK
    if m == "bi":
        if ra == 0 and addr not in computed_jumps:
            return RET, _bit(0), 0, None
        return IJUMP, _bit(ra), 0, None
    if m in ("biz", "binz", "bihz", "bihnz"):
        if ra == 0 and addr not in computed_jumps:
            return CRET, _bit(rt) | _bit(0), 0, None
        return CIJUMP, _bit(rt) | _bit(ra), 0, None
    if m == "bisl":
        return ICALL, _bit(ra), _bit(rt), None
    if m == "bisled":
        return CIJUMP, _bit(ra), 0, None
    if m == "iret":
        return IJUMP, 0, 0, None
    if m == "stop":
        return (STOPALL if (raw & 0x3FFF) in LV2_RESUMING_STOPS else TERM), 0, 0, None
    # An instruction this tool does not know (or data decoded as code): everything is read, nothing is killed.
    return STOPALL, ALL, 0, None


# ---- function bounds -----------------------------------------------------------------------

def function_bounds(image, base, code_end=0, entries=(), functions=None):
    """The lifter's bounds for a raw image: detect, split at `entries`, cap at `code_end`."""
    if functions is None:
        bounds, _ = detect_functions(image, base_override=base, verbose=False, raw=True)
    else:
        bounds = [(int(s), int(e)) for s, e in functions]
    bset = sorted(set(bounds))
    limit = base + len(image)
    for a in entries:   # tools/spu_lifter.py --extra-funcs: split the bound that holds it
        newb, split = [], False
        for s, e in bset:
            if s < a < e:
                newb += [(s, a), (a, e)]
                split = True
            else:
                newb.append((s, e))
        if not split and not any(s == a for s, _ in bset):
            newb.append((a, min([s for s, _ in bset if s > a] + [limit])))
        bset = sorted(set(newb))
    by_start = {}
    for s, e in bset:
        if s not in by_start or e < by_start[s]:
            by_start[s] = e
    bset = sorted(by_start.items())
    if code_end:
        bset = [(s, min(e, code_end)) for s, e in bset if s < code_end]
    prev_end = None
    for s, e in bset:
        if s % 4 or e % 4 or e <= s or s < base or e > limit:
            raise ValueError(f"function bound [0x{s:X}, 0x{e:X}) is not a word range inside the image")
        if prev_end is not None and s < prev_end:
            raise ValueError(f"function bounds overlap at 0x{s:X}")
        prev_end = e
    return bset


# ---- analysis ------------------------------------------------------------------------------

class _Fn:
    __slots__ = ("addrs", "end", "entry", "index")

    def __init__(self, entry, end, index):
        self.entry, self.end, self.index = entry, end, index
        self.addrs = list(range(entry, end, 4))


def _address_taken(image, entry_set, info, by_addr, fns):
    """Entries whose address appears as an aligned word or as a constant a function materialises."""
    taken = set()
    for off in range(0, len(image) - 3, 4):
        value = int.from_bytes(image[off:off + 4], "big")
        if value in entry_set:
            taken.add(value)
    for fn in fns:
        high = {}   # register -> (value of an ilhu, instructions it stays pairable for)
        for a in fn.addrs:
            ins = by_addr[a]
            m, raw = ins.mnemonic, ins.raw
            rt, imm = raw & 0x7F, (raw >> 7) & 0xFFFF
            defs = info[a][2]
            if m == "ila":
                taken.add((raw >> 7) & 0x3FFFF)
            elif m == "il":
                taken.add(imm)
                taken.add(_sext(imm, 16) & 0xFFFFFFFF)
            elif m == "ilhu":
                taken.add(imm << 16)
            elif m == "iohl" and rt in high:
                taken.add(high[rt][0] | imm)
            high = {r: (v, n - 1) for r, (v, n) in high.items() if n > 1 and not (defs >> r) & 1}
            if m == "ilhu":
                high[rt] = (imm << 16, 8)
    return taken & entry_set


def analyze(image, base, code_end=0, entries=(), functions=None):
    """The liveness document for one raw image; see the module docstring."""
    entries = sorted(set(entries))
    code = image[:len(image) & ~3]   # a trailing partial word is not an instruction
    bounds = function_bounds(code, base, code_end, entries, functions)
    all_insns = disassemble_spu(code, base)
    by_addr = {i.addr: i for i in all_insns}
    jumps = compute_bi_r0_jumps(all_insns, bounds)
    fns = [_Fn(s, e, i) for i, (s, e) in enumerate(bounds)]
    starts = [f.entry for f in fns]
    entry_index = {f.entry: f.index for f in fns}

    def fn_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return fns[i] if i >= 0 and addr < fns[i].end else None

    info = {}
    for fn in fns:
        for a in fn.addrs:
            info[a] = describe(by_addr[a], jumps)

    callers = {f.entry: [] for f in fns}
    tail_callers = {f.entry: [] for f in fns}
    cross = set()                 # (source function, destination function): the destination's live-out contains the source's
    call_returns = []             # (callee function, address after the call)
    reasons = {f.entry: set() for f in fns}
    ext_targets = {f.entry: set() for f in fns}   # addresses other than the entry that code outside the bound reaches
    has_indirect = {f.entry: False for f in fns}
    exits = {f.entry: [] for f in fns}
    tail_targets = {f.entry: set() for f in fns}

    def edge(src, a, target, tail_ok):
        dst = fn_of(target) if target in info else None
        if dst is None or dst is src:
            return
        cross.add((src.index, dst.index))
        if target == dst.entry:
            if tail_ok:
                tail_callers[dst.entry].append(a)
                tail_targets[src.entry].add(dst.entry)
            else:
                reasons[dst.entry].add("return-continuation")
        else:
            ext_targets[dst.entry].add(target)

    for fn in fns:
        for a in fn.addrs:
            kind, _uses, _defs, target = info[a]
            if kind in (IJUMP, CIJUMP):
                has_indirect[fn.entry] = True
            if kind in (RET, CRET):
                exits[fn.entry].append(a)
            if kind == CALL:
                if target in entry_index:
                    callers[target].append(a)
                    call_returns.append((entry_index[target], a + 4))
                else:
                    held = fn_of(target) if target in info else None
                    if held is not None:
                        reasons[held.entry].add("mid-entry-call")
                        ext_targets[held.entry].add(target)
            elif kind in (JUMP, COND):
                edge(fn, a, target, True)
            if kind in _FALLS:
                edge(fn, a, a + 4, kind not in (CALL, ICALL))

    entry_set = set(starts)
    taken = _address_taken(code, entry_set, info, by_addr, fns)
    for fn in fns:
        r = reasons[fn.entry]
        if fn.entry in entries:
            r.add("entry-point")
        if fn.entry in taken:
            r.add("address-taken")
        if not callers[fn.entry] and not tail_callers[fn.entry]:
            r.add("no-caller")

    out = {}
    for fn in fns:
        out[fn.index] = ALL if reasons[fn.entry] else 1 << SP
    live = dict.fromkeys(info, 0)

    def li(addr):
        return live.get(addr, ALL)

    def transfer(a, fn):
        kind, uses, defs, target = info[a]
        if kind == PLAIN:
            return uses | (li(a + 4) & ~defs)
        if kind == JUMP:
            return li(target)
        if kind == COND:
            return uses | li(target) | li(a + 4)
        if kind == CALL:
            callee = (li(target) & ~defs) if target in entry_index else ALL
            return uses | callee | li(a + 4)
        if kind == RET:
            return uses | out[fn.index]
        if kind == CRET:
            return uses | out[fn.index] | li(a + 4)
        if kind == TERM:
            return uses
        return ALL   # ICALL, IJUMP, CIJUMP, STOPALL: an unseen callee or target, or a service that reads registers

    changed = True
    while changed:
        changed = False
        for fn in fns:
            for a in reversed(fn.addrs):
                new = live[a] | transfer(a, fn)
                if new != live[a]:
                    live[a] = new
                    changed = True
        for callee, after in call_returns:
            new = out[callee] | li(after)
            if new != out[callee]:
                out[callee] = new
                changed = True
        for src, dst in cross:
            new = out[dst] | out[src]
            if new != out[dst]:
                out[dst] = new
                changed = True

    functions_doc = []
    for fn in fns:
        live_out = out[fn.index] | (1 << SP)
        dead = [r for r in range(128) if r != SP and not (live_out >> r) & 1]
        frame = None
        if not has_indirect[fn.entry] and not ext_targets[fn.entry] and "return-continuation" not in reasons[fn.entry]:
            frame = _frame_bytes(fn, info, by_addr, fn_of)
        functions_doc.append({
            "entry": f"0x{fn.entry:05x}",
            "end": f"0x{fn.end:05x}",
            "exits": [f"0x{a:05x}" for a in exits[fn.entry]],
            "tail_calls": [f"0x{t:05x}" for t in sorted(tail_targets[fn.entry])],
            "live_out": [r for r in range(128) if (live_out >> r) & 1],
            "dead_regs": dead,
            "dead_stack": {"below_sp": frame} if frame else None,
            "callers": [f"0x{a:05x}" for a in sorted(callers[fn.entry])],
            "tail_callers": [f"0x{a:05x}" for a in sorted(tail_callers[fn.entry])],
            "unknown_callers": bool(reasons[fn.entry]),
            "unknown_reasons": sorted(reasons[fn.entry]),
        })
    return {
        "schema": SCHEMA,
        "tool": "spu_liveness",
        "version": 1,
        "image": {
            "sha256": hashlib.sha256(image).hexdigest(),
            "fingerprint": f"0x{fnv1a64(image):016x}",
            "ls_base": f"0x{base:05x}",
            "bytes": len(image),
            "code_end": f"0x{code_end:05x}" if code_end else None,
        },
        "functions": functions_doc,
    }


def _frame_bytes(fn, info, by_addr, fn_of):
    """Bytes of the frame below the entry stack pointer, or None when it cannot be proven."""
    state = {fn.entry: 0}
    work = [fn.entry]
    lowest = 0
    while work:
        a = work.pop()
        d = state[a]
        kind, _uses, defs, target = info[a]
        nd = d
        if defs & (1 << SP):
            ins = by_addr[a]
            if ins.mnemonic == "ai" and (ins.raw & 0x7F) == SP and ((ins.raw >> 7) & 0x7F) == SP:
                nd = d + _sext((ins.raw >> 14) & 0x3FF, 10)
            else:
                return None
        if nd > 0:
            return None
        lowest = min(lowest, nd)
        if kind in (RET, CRET) and nd != 0:
            return None
        succ = []
        if kind in (JUMP, COND):
            succ.append(target)
        if kind in _FALLS:
            succ.append(a + 4)
        for s in succ:
            if s not in info or fn_of(s) is not fn:
                if nd != 0:      # leaves the function (a tail call, a fall-through into the next bound, or the unknown)
                    return None
                continue
            if s in state:
                if state[s] != nd:
                    return None
            else:
                state[s] = nd
                work.append(s)
    return -lowest or None


def summary(doc):
    """One-paragraph human summary of a document."""
    fns = doc["functions"]
    unknown = sum(1 for f in fns if f["unknown_callers"])
    known = [f for f in fns if not f["unknown_callers"]]
    mean_all = sum(len(f["dead_regs"]) for f in fns) / len(fns) if fns else 0.0
    mean_known = sum(len(f["dead_regs"]) for f in known) / len(known) if known else 0.0
    frames = sum(1 for f in fns if f["dead_stack"])
    return (f"functions {len(fns)}  unknown callers {unknown}  with a dead frame {frames}  "
            f"mean dead registers {mean_all:.1f} (known callers only: {mean_known:.1f})")


def _int(text):
    return int(text, 0)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="raw local-store image, or an SPU ELF with --elf")
    ap.add_argument("--elf", action="store_true",
                    help="IMAGE is an SPU ELF: its executable segments, the lifter's --auto-functions bounds, "
                         "and the ELF entry as an entry point")
    ap.add_argument("--base", type=_int, default=0, help="LS address of the first byte (default 0; not with --elf)")
    ap.add_argument("--code-end", type=_int, default=0, help="where executable code ends (the lifter's --code-end)")
    ap.add_argument("--entry", type=_int, action="append", default=[],
                    help="an extra entry point; its callers are unknown (repeatable)")
    ap.add_argument("--functions", metavar="FILE",
                    help="the lifter's function list ([{start, end}]) instead of detecting one")
    ap.add_argument("--output", "-o", metavar="FILE", help="write the JSON here instead of standard output")
    ap.add_argument("--summary", action="store_true", help="print a one-line summary to standard error")
    args = ap.parse_args(argv)
    try:
        with open(args.image, "rb") as f:
            image = f.read()
    except OSError as exc:
        print(f"spu_liveness: {exc}", file=sys.stderr)
        return 1
    functions = None
    entries = list(args.entry)
    base = args.base
    if args.elf:
        if args.functions or args.base:
            ap.error("--elf takes its bounds and base from the ELF")
        try:
            elf = parse_elf(image)
            functions, _ = detect_functions(image, verbose=False)
            image, base = text_image(image, elf["phs"])
        except (ValueError, SystemExit, struct.error) as exc:
            print(f"spu_liveness: {args.image}: not an SPU ELF with code ({exc})", file=sys.stderr)
            return 1
        entries.append(elf["entry"])
    elif args.functions:
        with open(args.functions, encoding="utf-8") as f:
            functions = [(int(str(e["start"]), 0), int(str(e["end"]), 0)) for e in json.load(f)]
    try:
        doc = analyze(image, base, args.code_end, entries, functions)
    except ValueError as exc:
        print(f"spu_liveness: {exc}", file=sys.stderr)
        return 1
    text = json.dumps(doc, indent=1)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    else:
        print(text)
    if args.summary:
        print(summary(doc), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
