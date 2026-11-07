#!/usr/bin/env python3
"""Compare a spuexh hardware result with the toolkit's references and print a per-op verdict table.

    compare_spuexh.py HW.csv --scalar scalar.txt [--interp dryrun.txt] [--rpcs3 precise.txt]
                      [--detail] [--md OUT.md] [--max-diff N]

HW.csv       result file of the probe (probes/spuexh/FORMAT.md): standard hwprobe header, META/FAULT/SKIP/SPEC* rows,
             and hash rows `op<TAB>domain<TAB>block<TAB>hash16` in the line format of the host harness.
--scalar     the host harness' reference (test_spu_exhaustive, scalar build): what the toolkit's helpers compute.
--interp     the same probe run in the toolkit's SPU interpreter (spuexh_host): what the interpreter computes for the
             very SPU program that ran on the console. Optional; when given it separates "the hardware differs from the
             toolkit" from "the host harness feeds the op something an instruction cannot encode".
--rpcs3      any further file in the same hash-line format (for example hashes of RPCS3's precise SPU interpreter).

Only blocks present in both files are compared. Verdict per op (all domains together):
  MATCH      every common block equal to every given reference
  HW!=       the hardware differs from a reference (the reference column says which)
  REF!=      the references disagree with each other where the hardware agrees with one of them
  NOT RUN    the op has no hardware block (skipped, or outside the configuration)
  NO COMMON  hardware blocks exist but no reference block overlaps
A verdict also names the domains whose generator self-check did not validate ("gencheck unvalidated"): the
comparison requires a #gencheck digest for every measured domain on both sides and fails (exit 1) when it is
missing, disagrees, or the evidence sets are disjoint, because op verdicts there are not about the ops.
Exit status: 0 when no op is HW!=, the generator evidence covers every measured domain and no special-A op executed
(the five DP compares must NOT execute: a SPECA_VALID `executed` row is a regression), 1 otherwise,
2 on a usage or format error.
"""
import argparse
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
import hwprobe_format


def load_hashes(path, want_header=False):
    """(dict {(op, dom, blk): hash}, other rows dict). Rows without a tab are header/meta rows of a hwprobe file."""
    h, rows = {}, collections.defaultdict(list)
    header = None
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    if want_header:
        header, _ = hwprobe_format.parse_header(lines)
    for ln in lines:
        if not ln or ln.startswith("#"):
            continue
        if "\t" in ln:
            p = ln.split("\t")
            if len(p) != 4:
                raise ValueError(f"{path}: bad hash row {ln[:60]!r}")
            h[(p[0], p[1], int(p[2]))] = p[3]
        else:
            rows[ln.split(",", 1)[0]].append(ln)
    return h, rows, header


def load_gen(path):
    """{domain: digest} from the '#gencheck<TAB>domain<TAB>digest' lines of a result file or of spuexh_host --gencheck."""
    g = {}
    with open(path, encoding="utf-8") as f:
        for ln in f:
            if ln.startswith("#gencheck\t"):
                p = ln.rstrip("\n").split("\t")
                g[p[1]] = p[2]
    return g


def gen_report(hw_gen, ref_gen, ref_name, hw_domains):
    """Generator self-check lines: (lines, unvalidated domain names, failed).

    A hardware domain that was measured but has no #gencheck evidence on either side, a digest disagreement, or
    fully disjoint evidence sets fail the comparison (exit 1): op verdicts there are not about the ops, because the
    SPU code of the generator may be computing different inputs than the host. Returns the domains whose verdicts
    are not generator-validated so the per-op table can label them.
    """
    missing_hw = sorted(hw_domains - set(hw_gen))
    missing_ref = sorted(hw_domains - set(ref_gen))
    common = [d for d in hw_gen if d in ref_gen]
    bad = [d for d in common if hw_gen[d] != ref_gen[d]]
    unvalidated = set(missing_hw) | set(missing_ref) | set(bad)
    if hw_gen and ref_gen and hw_domains and not common:
        return (["**GENERATOR SELF-CHECK DISJOINT: the hardware evidence ({}) and the reference evidence ({}) share no "
                 "domain; nothing is generator-validated.** Reference: {}.".format(
                     ", ".join(sorted(hw_gen)[:8]), ", ".join(sorted(ref_gen)[:8]), ref_name)], unvalidated, True)
    lines = []
    if not hw_gen:
        lines.append("**GENERATOR SELF-CHECK MISSING: the hardware file has no #gencheck rows; op verdicts are not "
                     "generator-validated.**")
    elif missing_hw:
        lines.append("**GENERATOR SELF-CHECK MISSING on measured domain(s) {} (no #gencheck row in the hardware "
                     "file): op verdicts there are not generator-validated.**".format(", ".join(missing_hw)))
    if not ref_gen:
        lines.append("**GENERATOR REFERENCE MISSING: neither --gen nor --interp gave generator evidence; op verdicts "
                     f"are not generator-validated.** Reference: {ref_name}.")
    elif missing_ref:
        lines.append("**GENERATOR REFERENCE MISSING for measured domain(s) {} ({} has no digest for them): op "
                     "verdicts there are not generator-validated.**".format(", ".join(missing_ref), ref_name))
    if bad:
        lines.append(
            f"**GENERATOR SELF-CHECK FAILED on {len(bad)} of {len(common)} domains ({', '.join(sorted(bad))}): the SPU code "
            f"of the generator computes different inputs than the host; op verdicts for these domains are not about the "
            f"ops.** Reference: {ref_name}.")
    extra = sorted(set(ref_gen) - hw_domains - set(hw_gen))
    if extra:
        lines.append(f"Reference-only generator domains (not measured on hardware): {', '.join(extra[:12])}.")
    if not lines:
        lines.append(f"Generator self-check: {len(hw_domains & set(common))} of {len(hw_domains)} measured domains "
                     f"validated against {ref_name}.")
    return lines, unvalidated, bool(lines and any(ln.startswith("**") for ln in lines))


def per_op(hashes):
    d = collections.defaultdict(dict)
    for (op, dom, blk), v in hashes.items():
        d[op][(dom, blk)] = v
    return d


def compare(hw, refs, order, unvalidated=frozenset()):
    """refs: ordered dict name -> hashes. Returns list of row dicts, one per op in `order` + extras.
    Ops measured in a domain without valid generator evidence get their verdict labelled with it."""
    hw_ops = per_op(hw)
    ref_ops = {n: per_op(h) for n, h in refs.items()}
    names = list(order) + sorted(set(hw_ops) - set(order))
    out = []
    for op in names:
        r = {"op": op, "hw": len(hw_ops.get(op, {})), "cmp": {}, "first": {}, "verdict": "", "note": ""}
        if not r["hw"]:
            r["verdict"] = "NOT RUN"
            out.append(r)
            continue
        any_common, bad_refs = False, []
        for n, ro in ref_ops.items():
            mine, theirs = hw_ops[op], ro.get(op, {})
            common = [k for k in mine if k in theirs]
            diff = sorted(k for k in common if mine[k] != theirs[k])
            r["cmp"][n] = (len(common), len(diff))
            if common:
                any_common = True
            if diff:
                bad_refs.append(n)
                r["first"][n] = diff[0]
        if not refs or not any_common:
            r["verdict"] = "NO COMMON"
        elif bad_refs:
            r["verdict"] = "HW!= " + ",".join(bad_refs)
        else:
            r["verdict"] = "MATCH"
        un = {dom for (dom, _blk) in hw_ops.get(op, {}) if dom in unvalidated}
        if un:
            r["verdict"] += " (gencheck unvalidated: " + ",".join(sorted(un)) + ")"
        out.append(r)
    # references that disagree with each other on blocks the hardware agrees with one of
    return out


def ref_vs_ref(refs):
    """Pairwise block disagreements between references: {(a, b): {op: (common, diff)}}."""
    names = list(refs)
    res = {}
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            ops = collections.defaultdict(lambda: [0, 0])
            for k, v in refs[a].items():
                w = refs[b].get(k)
                if w is None:
                    continue
                ops[k[0]][0] += 1
                if v != w:
                    ops[k[0]][1] += 1
            res[(a, b)] = {o: tuple(c) for o, c in ops.items()}
    return res


def op_order_from(path_ref):
    seen = []
    for (op, _, _) in path_ref:
        if op not in seen:
            seen.append(op)
    return seen


# ---------------------------------------------------------------- special probes
# The five words of special probe A (common/spuexh_core.c). A retail CBE SPU raises an invalid-instruction error for every
# one of them (hardware, CECHC04 4.81, r002 specialA; issue #124), so the expected answer is NOT_executed.
SPECA_OPS = ("dfceq", "dfcmeq", "dfcgt", "dfcmgt", "dftsv")


def parse_speca(rows):
    d = {}
    for ln in rows.get("SPECA", []):
        p = ln.split(",")
        d[(p[1], p[2], int(p[3]))] = (p[4], p[5], p[6])
    valid = {}
    for ln in rows.get("SPECA_VALID", []):
        p = ln.split(",")
        valid[p[1]] = (p[2], ",".join(p[5:]))        # SPECA_VALID,op,state,word_W,started_K,text
    faults = [ln for ln in rows.get("SPECA_FAULT", [])]
    return d, valid, faults


def special_a_regressions(hw_rows):
    """The special-A ops the hardware executed: each one contradicts the toolkit's decode (invalid instruction)."""
    _d, valid, _f = parse_speca(hw_rows)
    return [op for op in SPECA_OPS if valid.get(op, ("",))[0] == "executed"]


def special_a_report(hw_rows, ref_rows, name_ref="interp"):
    """Lines describing probe A: the expected answer is that no op executes; an `executed` row is a regression."""
    hw, valid, faults = parse_speca(hw_rows)
    ref = parse_speca(ref_rows)[0] if ref_rows is not None else None
    out = ["#### Special probe A: DP compare / test-special-value as raw words (expected: NOT executed)", "",
           f"| op | hardware executed it | cases | differ from {name_ref} |", "|---|---|---|---|"]
    for op in SPECA_OPS:
        v = valid.get(op)
        if v is None:
            out.append(f"| {op} | not run | 0 | |")
            continue
        cases = [k for k in hw if k[0] == op]
        diffs = [k for k in cases if ref is not None and k in ref and ref[k][2] != hw[k][2]]
        out.append(f"| {op} | {'YES (REGRESSION)' if v[0] == 'executed' else 'NO: ' + v[1][:50]} | {len(cases)} | "
                   f"{len(diffs) if ref is not None else 'n/a'} |")
    regs = special_a_regressions(hw_rows)
    if regs:
        out += ["", (f"**REGRESSION: the hardware executed {', '.join(regs)}, which the toolkit decodes as an invalid "
                     "instruction (issue #124). The results are in the SPECA rows of the file.**")]
    for ln in faults[:10]:
        out.append("")
        out.append(f"fault row: `{ln[:200]}`")
    return out


def special_b_report(hw_rows):
    """SPECB_TRY,index,prefix,zero|rand,word and SPECB,index,prefix,zero|rand,word,outcome,nchanged,regs,arena,arena_addr,text"""
    rows = [ln.split(",") for ln in hw_rows.get("SPECB", [])]
    tries = [ln.split(",") for ln in hw_rows.get("SPECB_TRY", [])]
    if not rows and not tries:
        return ["#### Special probe B: not run"]
    out = [f"#### Special probe B: decoder validity of the {len({r[2] for r in rows})} prefixes the toolkit decodes as SPU_word",
           ""]
    if tries:
        lost = len(tries) - len(rows)
        out.append(f"{len(tries)} words were started (SPECB_TRY rows are flushed before each launch); "
                   f"{len(rows)} have an outcome row" + (f"; {lost} were started but never got one." if lost else "."))
        out.append("")
    if rows:
        cnt = collections.Counter(r[5] for r in rows)
        out += ["| outcome on hardware | words |", "|---|---|"]
        for k, v in sorted(cnt.items()):
            out.append(f"| {k} | {v} |")
    executed = sorted({r[2] for r in rows if r[5] == "executed"})
    invalid = sorted({r[2] for r in rows if r[5] != "executed"})
    executed_txt = ", ".join(executed[:80]) + (" ..." if len(executed) > 80 else "") if executed else "none"
    out += ["", f"Prefixes that executed on hardware (valid there although the toolkit has no decode): {executed_txt}",
            "", f"Prefixes that did not complete (invalid instruction, hang or fault): {len(invalid)}"]
    last = tries[-1] if tries else rows[-1]
    out += ["", (f"Last word started: index {last[1]} (prefix {last[2]}, pattern {last[3]}, word {last[4]}); "
                 f"resume at special_b_from={int(last[1])} (inclusive), using prefix {last[2]} as the identity")]
    descs = collections.Counter(",".join(r[10:])[:90] for r in rows if r[5] != "executed")
    out += ["", "How the non-completing words ended (first 8 distinct):", ""]
    for k, v in descs.most_common(8):
        out.append(f"- {v} x `{k}`")
    eff = [r for r in rows if r[5] == "executed" and (r[6] != "0" or r[8] != "0")]
    if eff:
        out += ["", "Executed words with a register or memory effect (first 12):", "",
                "| prefix | pattern | word | changed | regs | arena |", "|---|---|---|---|---|---|"]
        for r in eff[:12]:
            out.append(f"| {r[2]} | {r[3]} | {r[4]} | {r[6]} | {r[7][:60]} | {r[8]} |")
    return out


# ---------------------------------------------------------------- main
def render(hw_hashes, hw_rows, hdr, refs, rows_by_ref, detail=False, max_diff=8, gen_lines=(), unvalidated=frozenset()):
    order = op_order_from(refs[next(iter(refs))]) if refs else []
    res = compare(hw_hashes, refs, order, unvalidated)
    names = list(refs)
    L = ["# spuexh hardware verdicts", ""]
    if hdr:
        L.append("Console `{}`, firmware `{} {}`, build `{}`, source `{}`, config `{}`.".format(
            hdr.get("console_model"), hdr.get("fw_version"), hdr.get("fw_type"), hdr.get("git_describe"),
            hdr.get("source_sha256", "")[:12], hdr.get("config")))
        L.append("")
    nblk = len(hw_hashes)
    nops = len({k[0] for k in hw_hashes})
    L.append(f"Hardware blocks: {nblk}; ops with blocks: {nops}.")
    L += list(gen_lines)
    for ln in hw_rows.get("FAULT", [])[:20]:
        L.append(f"- fault: `{ln[:160]}`")
    for ln in hw_rows.get("RETRY", [])[:8]:
        L.append(f"- retry (collateral worker of a dead group re-run alone, completed): `{ln[:120]}`")
    for ln in hw_rows.get("TIMING_OVERFLOW", [])[:8]:
        L.append(f"- timing overflow (job outlasted one decrementer period, ~53.8 s): `{ln[:80]}`")
    for ln in hw_rows.get("FATAL", []):
        L.append(f"- **FATAL (the probe stopped): `{ln[:160]}`**")
    for ln in hw_rows.get("SKIP", [])[:20]:        # not written any more; result files from before #124 carry them
        L.append(f"- skipped: `{ln[:120]}`")
    L += ["", "| op | hw blocks | " + " | ".join(f"vs {n} (cmp/diff)" for n in names) + " | verdict |",
          "|---|---|" + "---|" * len(names) + "---|"]
    for r in res:
        cells = []
        for n in names:
            c = r["cmp"].get(n)
            cells.append(f"{c[0]}/{c[1]}" if c else "-")
        L.append(f"| {r['op']} | {r['hw']} | {' | '.join(cells)} | {r['verdict']} |")
    bad = [r for r in res if r["verdict"].startswith("HW!=")]
    L += ["", "**{} ops MATCH, {} HW!=, {} NOT RUN, {} NO COMMON**".format(
        sum(r["verdict"].startswith("MATCH") for r in res), len(bad), sum(r["verdict"] == "NOT RUN" for r in res),
        sum(r["verdict"] == "NO COMMON" for r in res))]
    rr = ref_vs_ref(refs) if len(refs) > 1 else {}
    for (a, b), ops in rr.items():
        d = {o: c for o, c in ops.items() if c[1]}
        L += ["", "References {} vs {} disagree on {} ops (blocks): {}".format(
            a, b, len(d), ", ".join(f"{o} {c[1]}/{c[0]}" for o, c in sorted(d.items())[:20]) or "none")]
    if detail or bad:
        L += ["", "## First differing blocks", ""]
        for r in bad[:max_diff * 4]:
            for n, k in r["first"].items():
                L.append("- `{}` vs {}: first differing block {}/{} (hw {}, {} {})".format(
                    r["op"], n, k[0], k[1], hw_hashes[(r["op"],) + k], n, refs[n][(r["op"],) + k]))
    L += [""]
    a_rows = rows_by_ref.get("interp") or rows_by_ref.get(names[0] if names else "")
    L += special_a_report(hw_rows, a_rows, "interp" if "interp" in rows_by_ref else "reference")
    L += [""] + special_b_report(hw_rows)
    return "\n".join(L) + "\n", res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hw")
    ap.add_argument("--scalar")
    ap.add_argument("--interp")
    ap.add_argument("--rpcs3")
    ap.add_argument("--gen", help="output of `spuexh_host --gencheck` (the generator digests computed natively); default: the --interp file's")
    ap.add_argument("--detail", action="store_true")
    ap.add_argument("--md")
    ap.add_argument("--max-diff", type=int, default=8)
    a = ap.parse_args(argv)
    try:
        hw, hw_rows, hdr = load_hashes(a.hw, want_header=True)
        refs, rows_by_ref = collections.OrderedDict(), {}
        for name, path in (("scalar", a.scalar), ("interp", a.interp), ("rpcs3", a.rpcs3)):
            if path:
                h, rows, _ = load_hashes(path)
                refs[name] = h
                rows_by_ref[name] = rows
    except (OSError, ValueError, hwprobe_format.FormatError) as e:
        print(f"compare_spuexh: {e}", file=sys.stderr)
        return 2
    if not refs:
        print("compare_spuexh: give at least one of --scalar/--interp/--rpcs3", file=sys.stderr)
        return 2
    hw_domains = {dom for (_op, dom, _blk) in hw}
    gen_lines, unvalidated, gen_failed = gen_report(
        load_gen(a.hw), load_gen(a.gen) if a.gen else (load_gen(a.interp) if a.interp else {}),
        "the native host" if a.gen else "the interpreter run", hw_domains)
    text, res = render(hw, hw_rows, hdr, refs, rows_by_ref, a.detail, a.max_diff, gen_lines, unvalidated)
    if a.md:
        with open(a.md, "w", encoding="utf-8") as f:
            f.write(text)
    sys.stdout.write(text)
    return 1 if any(r["verdict"].startswith("HW!=") for r in res) or gen_failed or special_a_regressions(hw_rows) else 0


if __name__ == "__main__":
    sys.exit(main())
