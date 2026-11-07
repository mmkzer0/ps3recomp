#!/usr/bin/env python3
"""Tests for the spuexh probe (tools/hwprobe/probes/spuexh): table generator, comparison tool, and, when the SPU
toolchain is installed, the SPU program in the toolkit's interpreter with injected defects.

Standalone: standard library only. Everything that needs a compiler is skipped, with the reason, when it is missing
(PS3DEV=<prefix with spu/bin/spu-gcc> enables the dry-run tests; `cc` builds the host driver). When the SPU compiler
cannot run on this host (linux toolchain on macOS) the dry-run tests use the ELF that `tools/hwprobe/build.sh spuexh
layer1` left in probes/spuexh/spu/, and the planted-defect test is skipped (it must rebuild the SPU program). No result
file here comes from a console; the synthetic ones are written by this test.

    python3 tools/test_hwprobe_spuexh.py
"""
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)
PROBE = os.path.join(TOOLS, "hwprobe", "probes", "spuexh")
sys.path.insert(0, PROBE)
sys.path.insert(0, os.path.join(TOOLS, "hwprobe"))

import compare_spuexh as cmp
import gen_tables as gt
import hwprobe_format
import lanes_compare
import lanesum_compare

_TMP = None


def setUpModule():
    global _TMP
    _TMP = tempfile.mkdtemp(prefix="hwprobe_spuexh_")


def tearDownModule():
    shutil.rmtree(_TMP, ignore_errors=True)


# ------------------------------------------------------------------------------------------------ toolchain
def _find_spu_gcc():
    ps3dev = os.environ.get("PS3DEV", "")
    for c in (os.path.join(ps3dev, "spu/bin/spu-gcc"), shutil.which("spu-gcc") or ""):
        if c and os.path.exists(c):
            return c, ps3dev
    return None, ps3dev


def _runs_natively(exe):
    """True when `exe` can run on this host (the toolchain is linux/arm64; macOS runs it only in podman)."""
    try:
        p = subprocess.run([exe, "--version"], check=False, capture_output=True, timeout=120)
        return p.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


SPU_GCC, PS3DEV = _find_spu_gcc()
SPU_GCC_NATIVE = bool(SPU_GCC) and _runs_natively(SPU_GCC)
SPU_AS = os.path.join(PS3DEV, "spu/bin/spu-as") if PS3DEV else (shutil.which("spu-as") or "")
SPU_AS_NATIVE = bool(SPU_AS) and os.path.exists(SPU_AS) and _runs_natively(SPU_AS)
BUILT_ELF = os.path.join(PROBE, "spu", "spuexh.elf")      # what build.sh leaves behind (same SPU program, same guard)


# ------------------------------------------------------------------------------------------------ generator
class TestGenTables(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.domains, cls.rows, cls.tab = gt.build_op_rows(REPO)
        cls.by = {r["name"]: r for r in cls.rows}

    def test_ops_come_from_the_host_harness(self):
        names = [r["name"] for r in self.rows]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(len(names), 193)                       # 145 table rows + 48 conversion ops
        for n in ("absdb", "frest", "frsqest", "fi", "fma", "dfa", "shufb", "cflts@173", "cuflt@255"):
            self.assertIn(n, names)
        # the five DP compares are decoded as invalid since #124: no op, no domain (special probe A has fixed words)
        for n in ("dfceq", "dfcmeq", "dfcgt", "dfcmgt", "dftsv"):
            self.assertNotIn(n, names)
        self.assertEqual(len(self.domains), 36)
        self.assertNotIn("dtsv", self.domains)
        self.assertEqual(self.domains[-1], "dfrds")             # dtsv was the last domain: no other index moved

    def test_known_words(self):
        # words assembled by spu-as 2.22 (verify_encodings.py checks all 193 against the assembler when it is installed)
        self.assertEqual(self.by["a"]["word"], 0x18010183)             # a $3,$3,$4
        self.assertEqual(self.by["ai"]["word"], 0x1C000183)            # ai $3,$3,0
        self.assertEqual(self.by["fma"]["word"], 0xE0610185)           # fma $3,$3,$4,$5
        self.assertEqual(self.by["selb"]["word"], 0x80610185)
        self.assertEqual(self.by["frest"]["word"], 0x37000183)         # frest $3,$3 (unary: rb = 0)
        self.assertEqual(self.by["cflts@173"]["word"], 0x76000183)
        self.assertEqual(self.by["addx"]["rt"], 5)                     # rt is an input of addx: result read from $5

    def test_immediate_fields(self):
        self.assertEqual((self.by["ai"]["imm_shift"], self.by["ai"]["imm_bits"]), (14, 10))
        self.assertEqual((self.by["shli"]["imm_shift"], self.by["shli"]["imm_bits"]), (14, 7))
        self.assertEqual((self.by["il"]["imm_shift"], self.by["il"]["imm_bits"]), (7, 16))
        self.assertEqual((self.by["ila"]["imm_shift"], self.by["ila"]["imm_bits"]), (7, 18))
        self.assertEqual((self.by["cflts@0"]["imm_shift"], self.by["cflts@0"]["imm_bits"]), (14, 8))
        self.assertEqual((self.by["shlqbii"]["imm_shift"], self.by["shlqbii"]["imm_bits"]), (14, 7))
        self.assertEqual(self.by["cflts@155"]["fixed_imm"], 155)
        self.assertEqual(self.by["cflts@173"]["heavy"], 0)
        self.assertEqual(self.by["cflts@0"]["heavy"], 1)
        self.assertEqual(self.by["csflt@155"]["heavy"], 0)

    def test_every_op_is_in_a_known_domain(self):
        for r in self.rows:
            self.assertTrue(r["dom_idx"], r["name"])
            for d in r["doms"]:
                self.assertIn(d, self.domains)

    def test_bad_prefix_list_is_the_undecoded_set(self):
        bad = [i for i, t in enumerate(self.tab) if t == "word"]
        self.assertEqual(len(bad), 789)                                 # the 784 of before #124 and the five DP prefixes
        known = {"stop", "ori", "a", "fma", "frest", "cflts"}
        self.assertFalse(known & {self.tab[i] for i in bad})
        # dfceq 0x3c3, dfcmeq 0x3cb, dfcgt 0x2c3, dfcmgt 0x2cb, dftsv 0x3bf are undecoded, so probe B runs them too
        self.assertEqual({0x3C3, 0x3CB, 0x2C3, 0x2CB, 0x3BF} - set(bad), set())
        out = os.path.join(_TMP, "gen")
        os.makedirs(out)
        gt.emit_badprefix(self.tab, os.path.join(out, "b.h"))
        with open(os.path.join(out, "b.h")) as f:
            text = f.read()
        self.assertIn("SPUEXH_NBAD 789", text)
        self.assertIn("0x004", text)                                    # first undecoded prefix (stop/lnop/sync/dsync precede it)
        self.assertIn("0x2c3", text)

    def test_generator_rejects_an_op_the_decode_table_does_not_have(self):
        # the DP compares are not in the decode table since #124: asking for one must make the generator stop, not
        # silently drop the op; the same for a table that lost a real op
        for name in ("dfceq", "dftsv"):
            with self.assertRaises(SystemExit):
                gt.encode_info(name, self.tab, {})
        bad = ["word" if t == "fceq" else t for t in self.tab]
        with self.assertRaises(SystemExit):
            gt.encode_info("fceq", bad, {})

    @unittest.skipUnless(SPU_AS_NATIVE, "needs a natively runnable spu-as (the toolchain is linux/arm64; macOS runs it only in podman)")
    def test_encodings_against_the_gnu_assembler(self):
        import verify_encodings
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = verify_encodings.main(["verify", REPO])
        self.assertEqual(rc, 0, out.getvalue())


# ------------------------------------------------------------------------------------------------ comparison
HDR = ["HWPROBE,1", "HDR,probe,spuexh", "HDR,probe_version,1", "HDR,git_describe,t", "HDR,source_sha256,abc", "HDR,fw_version,4.81",
       "HDR,fw_type,CEX", "HDR,console_model,CECHC04", "HDR,console_info_source,operator", "HDR,config,t", "HDR,tbfreq,79800000"]
_n = 0


def write(lines):
    global _n
    _n += 1
    p = os.path.join(_TMP, f"f{_n}.txt")
    with open(p, "w") as f:
        f.write("\n".join(lines) + "\n")
    return p


def hrow(op, dom, blk, h):
    return f"{op}\t{dom}\t{blk}\t{h:016x}"


def gencheck_rows(domains):
    """Generator self-check rows with a deterministic per-domain digest (the coverage gate needs them on both sides)."""
    return ["#gencheck\t{}\t{:016x}".format(d, int.from_bytes(d.encode(), "big") % (1 << 64)) for d in domains]


BASE = [("a", "wrand", 0, 0x11), ("a", "wrand", 1, 0x12), ("frest", "w32", 0, 0x21), ("frest", "w32", 1, 0x22), ("fi", "fi", 0, 0x31)]
BASE_DOMS = sorted({r[1] for r in BASE})


def ref_file(rows=BASE, gencheck=True):
    lines = [hrow(*r) for r in rows]
    return write(lines + (gencheck_rows(sorted({r[1] for r in rows})) if gencheck else []))


def hw_file(rows=BASE, extra=(), gencheck=True):
    lines = HDR + ["META,backend,ppu"] + [hrow(*r) for r in rows] + list(extra)
    return write(lines + (gencheck_rows(sorted({r[1] for r in rows})) if gencheck else []))


class TestCompare(unittest.TestCase):
    def run_cmp(self, hw, **refs):
        argv = [hw]
        if "gen" not in refs and "interp" not in refs:      # default generator evidence for both sides
            with open(hw, encoding="utf-8") as f:
                doms = sorted({ln.split("\t")[1] for ln in f if "\t" in ln and not ln.startswith("#")})
            refs["gen"] = write(gencheck_rows(doms))
        for k, v in refs.items():
            argv += ["--" + k, v]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = cmp.main(argv)
        return rc, out.getvalue(), err.getvalue()

    def test_identical_is_a_match(self):
        rc, out, _ = self.run_cmp(hw_file(), scalar=ref_file())
        self.assertEqual(rc, 0)
        self.assertIn("| a | 2 | 2/0 | MATCH |", out)
        self.assertIn("3 ops MATCH, 0 HW!=", out)

    def test_injected_hash_defect_is_found_and_localised(self):
        bad = [r if r[:3] != ("frest", "w32", 1) else r[:3] + (0x99,) for r in BASE]
        rc, out, _ = self.run_cmp(hw_file(bad), scalar=ref_file())
        self.assertEqual(rc, 1)
        self.assertIn("| frest | 2 | 2/1 | HW!= scalar |", out)
        self.assertIn("first differing block w32/1", out)
        self.assertIn("| a | 2 | 2/0 | MATCH |", out)

    def test_hardware_agrees_with_the_interpreter_not_the_host_harness(self):
        scalar = [r if r[:3] != ("a", "wrand", 1) else r[:3] + (0x77,) for r in BASE]       # host harness differs
        rc, out, _ = self.run_cmp(hw_file(), scalar=ref_file(scalar), interp=ref_file())
        self.assertEqual(rc, 1)
        self.assertIn("| a | 2 | 2/1 | 2/0 | HW!= scalar |", out)
        self.assertIn("References scalar vs interp disagree on 1 ops", out)

    def test_hardware_differing_from_both_references(self):
        bad = [r if r[:3] != ("fi", "fi", 0) else r[:3] + (0xDEAD,) for r in BASE]
        rc, out, _ = self.run_cmp(hw_file(bad), scalar=ref_file(), interp=ref_file())
        self.assertEqual(rc, 1)
        self.assertIn("| fi | 1 | 1/1 | 1/1 | HW!= scalar,interp |", out)

    def test_op_without_hardware_blocks_is_not_run(self):
        rc, out, _ = self.run_cmp(hw_file([r for r in BASE if r[0] != "fi"]), scalar=ref_file())
        self.assertEqual(rc, 0)
        self.assertIn("| fi | 0 | - | NOT RUN |", out)

    def test_no_overlap_is_no_common(self):
        other = [("a", "wrand", 40, 1)]
        rc, out, _ = self.run_cmp(hw_file(BASE), scalar=ref_file(other))
        self.assertEqual(rc, 0)
        self.assertIn("NO COMMON", out)

    def test_header_is_required_and_version_checked(self):
        rc, _, err = self.run_cmp(ref_file(), scalar=ref_file())          # a bare hash file has no HWPROBE header
        self.assertEqual(rc, 2)
        self.assertIn("not a hwprobe result file", err)
        p = write(["HWPROBE,99"] + HDR[1:] + [hrow(*BASE[0])])
        rc, _, err = self.run_cmp(p, scalar=ref_file())
        self.assertEqual(rc, 2)
        self.assertIn("unsupported hwprobe format version 99", err)
        p = write([HDR[0]] + [h for h in HDR[1:] if "tbfreq" not in h] + [hrow(*BASE[0])])
        rc, _, err = self.run_cmp(p, scalar=ref_file())
        self.assertEqual(rc, 2)
        self.assertIn("lacks required keys", err)

    def test_malformed_hash_row_is_an_error(self):
        p = write(HDR + ["a\twrand\t0"])
        rc, _, err = self.run_cmp(p, scalar=ref_file())
        self.assertEqual(rc, 2)
        self.assertIn("bad hash row", err)

    def test_fault_and_skip_rows_are_reported(self):
        p = hw_file(extra=["FAULT,spu2,w32,17,job_5,state_1,join 0x0 cause=4 status=0x7e57 FAULT event[src=1 d1=4]",
                           "SKIP,dfceq,rejected_by_special_a"])
        rc, out, _ = self.run_cmp(p, scalar=ref_file())
        self.assertEqual(rc, 0)
        self.assertIn("fault: `FAULT,spu2,w32,17", out)
        self.assertIn("skipped: `SKIP,dfceq,rejected_by_special_a`", out)

    def test_generator_self_check_is_compared(self):
        hw = hw_file()                                                  # measures wrand, w32, fi with #gencheck rows
        same = write(gencheck_rows(BASE_DOMS))                          # digests identical on both sides
        rc, out, _ = self.run_cmp(hw, scalar=ref_file(), gen=same)
        self.assertEqual(rc, 0)
        self.assertIn("Generator self-check: 3 of 3 measured domains validated against the native host.", out)
        digest = gencheck_rows(["wrand"])[0].split("\t")[2]
        diff = write([ln.replace(digest, "00000000000000cc") for ln in gencheck_rows(BASE_DOMS)])
        rc, out, _ = self.run_cmp(hw, scalar=ref_file(), gen=diff)
        self.assertEqual(rc, 1)
        self.assertIn("GENERATOR SELF-CHECK FAILED on 1 of 3 domains (wrand)", out)
        self.assertIn("(gencheck unvalidated: wrand)", out)

    def test_measured_domain_without_gencheck_evidence_fails(self):
        # the hardware file has evidence for wrand only, but measured wrand, w32 and fi
        hw = hw_file(extra=gencheck_rows(["wrand"]), gencheck=False)
        gen = write(gencheck_rows(BASE_DOMS))
        rc, out, _ = self.run_cmp(hw, scalar=ref_file(), gen=gen)
        self.assertEqual(rc, 1)
        self.assertIn("GENERATOR SELF-CHECK MISSING on measured domain(s) fi, w32", out)

    def test_reference_missing_gencheck_evidence_fails(self):
        hw = hw_file()                                                  # has evidence, measures wrand/w32/fi
        gen = write(["#gencheck\twrand\t" + gencheck_rows(["wrand"])[0].split("\t")[2]])   # reference covers only wrand
        rc, out, _ = self.run_cmp(hw, scalar=ref_file(), gen=gen)
        self.assertEqual(rc, 1)
        self.assertIn("GENERATOR REFERENCE MISSING for measured domain(s) fi, w32", out)

    def test_disjoint_gencheck_sets_fail(self):
        # neither side has evidence for a measured domain, and the two evidence sets share no domain at all
        hw = hw_file(extra=gencheck_rows(["byte2"]), gencheck=False)
        gen = write(gencheck_rows(["qcnt"]))
        rc, out, _ = self.run_cmp(hw, scalar=ref_file(), gen=gen)
        self.assertEqual(rc, 1)
        self.assertIn("GENERATOR SELF-CHECK DISJOINT", out)

    def test_retry_fatal_and_timing_rows_are_reported(self):
        p = hw_file(extra=["RETRY,spu1,job_2,ok",
                           "FATAL,launch,terminate 0x80010009 (injected)",
                           "TIMING_OVERFLOW,w32,7",
                           "FAULT,spu0,w32,0,job_1,state_1,join 0x0 cause=4 status=0x7e57 FAULT event"])
        rc, out, _ = self.run_cmp(p, scalar=ref_file())
        self.assertEqual(rc, 0)
        self.assertIn("retry (collateral worker of a dead group re-run alone, completed): `RETRY,spu1,job_2,ok`", out)
        self.assertIn("FATAL (the probe stopped): `FATAL,launch,terminate 0x80010009", out)
        self.assertIn("timing overflow (job outlasted one decrementer period, ~53.8 s): `TIMING_OVERFLOW,w32,7`", out)
        self.assertIn("fault: `FAULT,spu0,w32,0", out)

    def test_compare_exhaustive_reads_the_hash_rows(self):
        # A hash row is four tab-separated fields; the runtime tool's reader takes those and skips blank/'#' rows,
        # so the other rows of a result file (header, META, FAULT) are dropped before it sees the file (RUN.md).
        sys.path.insert(0, os.path.join(REPO, "runtime", "spu", "tests"))
        import compare_exhaustive as ce
        with open(hw_file(extra=["FAULT,spu0,x,1,job_1,state_1,t", "META,hashes,5"])) as f:
            rows = [ln.rstrip("\n") for ln in f if "\t" in ln]
        h, _cov = ce.load(write(rows))
        self.assertEqual(len(h), 5)
        self.assertEqual(h[("fi", "fi", 0)], f"{0x31:016x}")

    SPECA_FAULT_TEXT = "join 0x00000000 cause=4 status=0x7e57 exit=0xffffffff FAULT terminate-join event[src=ffffffff53505503 d1=403010000030100]"

    def test_special_a_not_executed_is_the_expected_answer(self):
        # the shape of the hardware's r002 result: all five ended as an invalid instruction
        words = {"dfceq": "78610183", "dfcmeq": "79610183", "dfcgt": "58610183", "dfcmgt": "59610183", "dftsv": "77e00183"}
        rows = [f"SPECA_VALID,{op},NOT_executed,word_{w},started_1,{self.SPECA_FAULT_TEXT}" for op, w in words.items()]
        rc, out, _ = self.run_cmp(hw_file(extra=rows), scalar=ref_file())
        self.assertEqual(rc, 0)
        self.assertIn("(expected: NOT executed)", out)
        self.assertIn("| dfceq | NO: join 0x00000000 cause=4", out)
        self.assertIn("| dftsv | NO: join 0x00000000 cause=4", out)
        self.assertNotIn("REGRESSION", out)
        self.assertEqual(cmp.special_a_regressions(cmp.load_hashes(write(HDR + rows))[1]), [])

    def test_special_a_not_run_is_not_a_regression(self):
        rc, out, _ = self.run_cmp(hw_file(), scalar=ref_file())
        self.assertEqual(rc, 0)
        self.assertIn("| dfceq | not run | 0 | |", out)
        self.assertIn("| dftsv | not run | 0 | |", out)

    def test_special_a_executed_on_hardware_is_a_regression(self):
        zero = "0" * 32
        a = "800a56da8a8d5917" + "0" * 16
        b = "8008000000000000" + "0" * 16
        rows = ["SPECA_VALID,dfceq,executed,word_78610183,started_1,exit status 0x0",
                "SPECA,dfceq,78610183,0,{},{},{}".format(a, b, "ffffffffffffffff" + "0" * 16),
                f"SPECA_VALID,dfcgt,NOT_executed,word_58610183,started_1,{self.SPECA_FAULT_TEXT}"]
        hw = write(HDR + rows)
        ref_rows = cmp.load_hashes(write([f"SPECA,dfceq,78610183,0,{a},{b},{zero}"]))[1]
        text = "\n".join(cmp.special_a_report(cmp.load_hashes(hw)[1], ref_rows))
        self.assertIn("| dfceq | YES (REGRESSION) | 1 | 1 |", text)
        self.assertIn("| dfcgt | NO: join 0x00000000", text)
        self.assertIn("| dfcmeq | not run | 0 | |", text)
        self.assertIn("**REGRESSION: the hardware executed dfceq, which the toolkit decodes as an invalid instruction", text)
        self.assertEqual(cmp.special_a_regressions(cmp.load_hashes(hw)[1]), ["dfceq"])
        rc, out, _ = self.run_cmp(hw_file(extra=rows), scalar=ref_file())      # everything else clean: the regression alone fails the run
        self.assertEqual(rc, 1)
        self.assertIn("REGRESSION", out)
        rows = [r.replace("dfceq", "dftsv") for r in rows[:1]]
        self.assertEqual(cmp.special_a_regressions(cmp.load_hashes(write(HDR + rows))[1]), ["dftsv"])

    def test_special_b_report_counts_outcomes(self):
        rows = ["SPECB,0,004,zero,00800000,no_return,0,,0,00000000,join 0x0 cause=4 FAULT",
                "SPECB,0,004,rand,0093ba28,no_return,0,,0,00000000,join 0x0 cause=4 FAULT",
                "SPECB,1,3c3,zero,78600000,executed,1,3:00000000000000000000000000000000,0,00000000,join 0x0",
                "SPECB,1,3c3,rand,7a6bdef0,executed,0,,0,00000000,join 0x0"]
        text = "\n".join(cmp.special_b_report(cmp.load_hashes(write(HDR + rows))[1]))
        self.assertIn("| executed | 2 |", text)
        self.assertIn("| no_return | 2 |", text)
        self.assertIn("valid there although the toolkit has no decode): 3c3", text)
        self.assertIn("resume a lost run with special_b_from=2", text)

    def test_special_b_try_rows_pre_log_and_name_a_lost_run(self):
        # the pre-logged TRY row names the word a lost run was attempting; the last prefix lacks an outcome row
        rows = ["SPECB_TRY,0,004,zero,00800000", "SPECB,0,004,zero,00800000,no_return,0,,0,00000000,t",
                "SPECB_TRY,0,004,rand,0093ba28", "SPECB,0,004,rand,0093ba28,no_return,0,,0,00000000,t",
                "SPECB_TRY,1,3c3,zero,78600000"]
        text = "\n".join(cmp.special_b_report(cmp.load_hashes(write(HDR + rows))[1]))
        self.assertIn("3 words were started (SPECB_TRY rows are flushed before each launch); 2 have an outcome row",
                      text)
        self.assertIn("1 were started but never got one.", text)
        self.assertIn("Last word started: index 1 (prefix 3c3, pattern zero, word 78600000)", text)
        self.assertIn("resume a lost run with special_b_from=2", text)


# ------------------------------------------------------------------------------------------------ lanes
_SCALAR_BIN = []


def scalar_bin():
    """The host harness' scalar build (test_spu_exhaustive, the reference build of run_exhaustive.sh), built once with cc."""
    if not _SCALAR_BIN:
        out = os.path.join(_TMP, "test_spu_exhaustive.scalar")
        subprocess.run(["cc", "-std=gnu11", "-O2", "-w", "-I", os.path.join(REPO, "runtime/spu"), "-I", os.path.join(REPO, "include"),
                        "-DSPU_SCALAR_HELPERS", "-DSPU_SCALAR_INT", os.path.join(REPO, "runtime/spu/tests/test_spu_exhaustive.c"),
                        "-lm", "-o", out], check=True, capture_output=True)
        _SCALAR_BIN.append(out)
    return _SCALAR_BIN[0]


def run_lanes_compare(path, *extra):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = lanes_compare.main([path, "--scalar-bin", scalar_bin()] + list(extra))
    return rc, out.getvalue() + err.getvalue()


def synth_lanes(op, dom, blk, n, imm_bits, edit=None, start=0, header=7):
    """A lanes result file whose hardware lanes are the harness' own model (so everything is equal), written from `--dump`.
    edit(rows) may change the rows (a list of [call, a, b, c, imm, out]) before they are written. header=6 writes the LANES
    row of the first lanes mode (without the start call)."""
    p = subprocess.run([scalar_bin(), "--dump", op, dom, str(blk), "--dump-start", str(start), "--dump-n", str(n)], check=True,
                       capture_output=True, text=True)
    rows = []
    for line in p.stdout.splitlines():
        f = line.split()
        imm = int(f[4]) & ((1 << imm_bits) - 1) if imm_bits else None
        rows.append([int(f[0]), f[1], f[2], f[3], imm, f[5]])
    if edit:
        edit(rows)
    lines = HDR + [f"LANES,{op},{dom},{blk},{n},{imm_bits}" + (f",{start}" if header == 7 else "")]
    for call, a, b, c, imm, out in rows:
        lines.append(f"LANE,{op},{dom},{blk},{call},{a},{b},{c},{'-' if imm is None else imm},{out}")
    lines.append(f"LANES_DONE,{op},{dom},{blk},{len(rows)}")
    return write(lines)


def run_lanesum_compare(path, *extra):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = lanesum_compare.main([path, "--scalar-bin", scalar_bin()] + list(extra))
    return rc, out.getvalue() + err.getvalue()


def synth_lanesum(op, dom, blk, chunk, edit=None):
    """A lanesum result file whose hardware hashes are the harness' own chunk hashes. edit(rows) may change the rows
    (a list of [start, count, hash]) before they are written."""
    p = subprocess.run([scalar_bin(), "--dump", op, dom, str(blk), "--dump-hash", str(chunk)], check=True, capture_output=True, text=True)
    rows = [[int(f[0]), int(f[1]), int(f[2], 16)] for f in (ln.split() for ln in p.stdout.splitlines())]
    n = len(rows)
    if edit:
        edit(rows)
    lines = HDR + [f"LANESUM_BEGIN,{op},{dom},{blk},{chunk},{n}"]
    lines += [f"LANESUM,{op},{dom},{blk},{s},{c},{h:016x}" for s, c, h in rows]
    lines.append(f"LANESUM_DONE,{op},{dom},{blk},{len(rows)}")
    return write(lines)


@unittest.skipUnless(shutil.which("cc"), "needs cc (it builds the host harness' scalar reference)")
class TestLanesCompare(unittest.TestCase):
    def test_lanes_equal_to_the_model_pass(self):
        rc, text = run_lanes_compare(synth_lanes("cflts@2", "w32", 0, 16, 8))
        self.assertEqual(rc, 0, text)
        self.assertIn("lanes 64", text)
        self.assertIn("equal 64    differ 0", text)
        self.assertIn("generator check OK", text)

    def test_a_differing_result_is_data_not_an_error(self):

        def edit(rows):
            rows[3][5] = rows[3][5][:8] + "deadbeef" + rows[3][5][16:]       # word 1 of call 3

        rc, text = run_lanes_compare(synth_lanes("cflts@2", "w32", 0, 16, 8, edit), "--only-diff")
        self.assertEqual(rc, 0, text)
        self.assertIn("63 equal, 1 differ", text)
        self.assertIn("call 3 lane 1:", text)
        self.assertIn("hw=deadbeef", text)
        diff_lines = [ln for ln in text.splitlines() if ln.endswith(" !=")]
        self.assertEqual(len(diff_lines), 1)
        self.assertFalse([ln for ln in text.splitlines() if ln.endswith(" ==")])          # --only-diff
        rc, text = run_lanes_compare(synth_lanes("cflts@2", "w32", 0, 16, 8, edit))
        self.assertTrue([ln for ln in text.splitlines() if ln.endswith(" ==")])

    def test_the_differing_lane_is_classified_by_its_input_word(self):
        sc = subprocess.run([scalar_bin(), "--dump", "cflts@2", "w32", "0", "--dump-n", "16"], check=True, capture_output=True, text=True)
        a = sc.stdout.splitlines()[5].split()[1]
        inw = int(a[16:24], 16)                                              # word 2 of call 5
        sign, ec, fc = lanes_compare.classify(inw)

        def edit(rows):
            rows[5][5] = rows[5][5][:16] + "deadbeef" + rows[5][5][24:]

        rc, text = run_lanes_compare(synth_lanes("cflts@2", "w32", 0, 16, 8, edit))
        self.assertEqual(rc, 0, text)
        self.assertIn(f"{sign} {ec} frac={fc}: 1 / ", text)
        self.assertIn(f"call 5 lane 2: in={inw:08x}", text)

    def test_fesd_lanes_are_doublewords_fed_by_the_even_words(self):

        def edit(rows):
            w = rows[2][5]
            rows[2][5] = w[:31] + ("1" if w[31] == "0" else "0")          # the last word: the second doubleword

        rc, text = run_lanes_compare(synth_lanes("fesd", "w32e", 0, 8, 0, edit), "--only-diff")
        self.assertEqual(rc, 0, text)
        self.assertIn("16 lanes: 15 equal, 1 differ", text)                  # 2 doubleword lanes per call
        self.assertIn("call 2 lane 1:", text)

    def test_a_wrong_call_index_or_operand_or_immediate_fails_the_generator_check(self):

        def shifted(rows):
            for r in rows:
                r[0] += 1

        def operand(rows):
            rows[4][1] = "0" * 32

        def imm(rows):
            rows[1][4] = 3

        for name, edit in (("call", shifted), ("operand", operand), ("imm", imm)):
            rc, text = run_lanes_compare(synth_lanes("cflts@2", "w32", 0, 16, 8, edit))
            self.assertEqual(rc, 1, (name, text))
            self.assertIn("GENERATOR MISMATCH", text)
            self.assertIn("generator check FAILED", text)
        rc, text = run_lanes_compare(synth_lanes("cflts@2", "w32", 0, 16, 8))
        self.assertEqual(rc, 0, text)

    def test_a_signed_immediate_is_compared_as_the_patched_field(self):
        f = lanes_compare._imm_equal
        self.assertTrue(f(1023, -1, 10))                                      # ai: -1 in a 10-bit field
        self.assertTrue(f(0, -1024, 10))
        self.assertTrue(f(173, 173, 8))
        self.assertFalse(f(172, 173, 8))
        self.assertFalse(f(1022, -1, 10))
        self.assertTrue(f(None, 5, 0))                                        # no field, nothing patched

    def test_classes_of_a_single(self):
        c = lanes_compare.classify
        self.assertEqual(c(0x00000000), ("+", "zero", "0"))
        self.assertEqual(c(0x80000001), ("-", "denormal", "other"))
        self.assertEqual(c(0x3F800000), ("+", "2^0", "0"))
        self.assertEqual(c(0xC2FFFFFF), ("-", "2^6", "all-ones"))
        self.assertEqual(c(0x7FFFFFFF), ("+", "nan", "all-ones"))
        self.assertEqual(c(0xFF800000), ("-", "inf", "0"))

    def test_markdown_report_and_errors(self):
        md = os.path.join(_TMP, "lanes.md")
        rc, _ = run_lanes_compare(synth_lanes("cflts@2", "w32", 0, 4, 8), "--md", md)
        self.assertEqual(rc, 0)
        with open(md, encoding="utf-8") as f:
            text = f.read()
        self.assertIn("| cflts@2 | w32 | 0 | 4 | 16 | 16 | 0 | OK |", text)
        rc, text = run_lanes_compare(write(HDR + ["META,backend,ppu"]))
        self.assertEqual(rc, 2)
        self.assertIn("no LANES/LANE rows", text)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = lanes_compare.main([synth_lanes("cflts@2", "w32", 0, 4, 8), "--scalar-bin", os.path.join(_TMP, "no_such_binary")])
        self.assertEqual(rc, 2)
        self.assertIn("cannot run", err.getvalue())

    def test_a_range_that_starts_inside_the_block(self):
        rc, text = run_lanes_compare(synth_lanes("cflts@45", "w32", 0, 16, 8, start=1000))
        self.assertEqual(rc, 0, text)
        self.assertIn("equal 64    differ 0", text)
        rc, text = run_lanes_compare(synth_lanes("cflts@2", "w32", 0, 16, 8, header=6))        # a file of the first lanes mode
        self.assertEqual(rc, 0, text)

        def shifted(rows):
            for r in rows:
                r[0] += 1

        rc, text = run_lanes_compare(synth_lanes("cflts@45", "w32", 0, 16, 8, shifted, start=1000))
        self.assertEqual(rc, 1, text)
        self.assertIn("GENERATOR MISMATCH", text)

        # the entry asked for calls from 1000, the SPU dumped the first ones (it ignored the start): input errors, not equal lanes
        with open(synth_lanes("cflts@45", "w32", 0, 16, 8), encoding="utf-8") as f:
            lines = f.read().splitlines()
        lines[lines.index("LANES,cflts@45,w32,0,16,8,0")] = "LANES,cflts@45,w32,0,16,8,1000"
        rc, text = run_lanes_compare(write(lines))
        self.assertEqual(rc, 1, text)
        self.assertIn("the entry asked for calls from 1000 of the block, the dump starts at call 0", text)

    def test_the_harness_dumps_any_scale_from_a_start_call_and_chunk_hashes(self):
        def dump(*argv):
            return subprocess.run([scalar_bin(), "--dump", *argv], check=False, capture_output=True, text=True)

        self.assertEqual(dump("cflts@45", "w32", "0", "--dump-n", "1").returncode, 0)
        for bad in ("cflts@256", "cflts@", "cflts@-1", "cflts@4x", "fesd@3", "dfma@3", "cflts"):
            p = dump(bad, "w32", "0", "--dump-n", "1")
            self.assertEqual(p.returncode, 2, bad)
        self.assertEqual(dump("cflts@173", "w32", "0", "--dump-start", "1048576").returncode, 2)      # beyond the block
        # a table scale and the same scale spelled differently take the same path through the harness
        self.assertEqual(dump("cflts@173", "w32", "0", "--dump-n", "64").stdout, dump("cflts@0173", "w32", "0", "--dump-n", "64").stdout)
        # a range from a start call is the same lines as the same calls of a dump from the start
        full = dump("cflts@45", "w32", "0", "--dump-n", "16").stdout.splitlines()
        part = dump("cflts@45", "w32", "0", "--dump-start", "5", "--dump-n", "4").stdout.splitlines()
        self.assertEqual(part, full[5:9])
        self.assertEqual(part[0].split()[0], "5")
        # chunk hashes: start counted from the block start, the last chunk is short, one chunk of the whole block is the sweep's hash
        ch = dump("cflts@173", "w32", "0", "--dump-start", "10", "--dump-n", "10", "--dump-hash", "4").stdout.splitlines()
        self.assertEqual([c.split()[:2] for c in ch], [["10", "4"], ["14", "4"], ["18", "2"]])
        sweep = subprocess.run([scalar_bin(), "--ops", "cflts@173", "--doms", "w32", "--stride-all", "100000"], check=True,
                               capture_output=True, text=True).stdout
        block = [ln.split("\t")[3] for ln in sweep.splitlines() if ln.startswith("cflts@173\tw32\t0\t")]
        whole = dump("cflts@173", "w32", "0", "--dump-hash", str(1 << 20)).stdout.split()
        self.assertEqual((whole[0], whole[1], [whole[2]]), ("0", str(1 << 20), block))


@unittest.skipUnless(shutil.which("cc"), "needs cc (it builds the host harness' scalar reference)")
class TestLanesumCompare(unittest.TestCase):
    def test_equal_chunks_pass(self):
        rc, text = run_lanesum_compare(synth_lanesum("cflts@173", "w32", 0, 262144))
        self.assertEqual(rc, 0, text)
        self.assertIn("chunks 4     equal 4", text)
        self.assertIn("4 chunks: 4 equal, 0 differ, 0 missing", text)

    def test_a_differing_chunk_is_listed_with_its_calls_and_lanes_lines(self):
        def edit(rows):
            rows[2][2] ^= 1

        rc, text = run_lanesum_compare(synth_lanesum("cflts@173", "w32", 0, 262144, edit))
        self.assertEqual(rc, 0, text)                                           # differences are data
        self.assertIn("4 chunks: 3 equal, 1 differ, 0 missing", text)
        self.assertIn("524288..786431", text)
        self.assertIn("lanes=cflts@173:w32:0:1024:524288", text)
        self.assertIn("lanes=cflts@173:w32:0:1024:525312", text)
        self.assertEqual(lanesum_compare.suggested_lanes(lanesum_compare.Group("fesd", "w32e", 0), 4096, 2500),
                         ["lanes=fesd:w32e:0:1024:4096", "lanes=fesd:w32e:0:1024:5120", "lanes=fesd:w32e:0:452:6144"])

    def test_a_missing_chunk_and_a_wrong_layout(self):
        def drop(rows):
            del rows[1]

        rc, text = run_lanesum_compare(synth_lanesum("cflts@173", "w32", 0, 262144, drop))
        self.assertEqual(rc, 0, text)
        self.assertIn("4 chunks: 3 equal, 0 differ, 1 missing", text)
        self.assertIn("262144..524287", text)

        def shifted(rows):
            rows[1][0] += 1

        rc, text = run_lanesum_compare(synth_lanesum("cflts@173", "w32", 0, 262144, shifted))
        self.assertEqual(rc, 1, text)
        self.assertIn("LAYOUT MISMATCH", text)

    def test_errors_and_markdown(self):
        md = os.path.join(_TMP, "lanesum.md")
        rc, _ = run_lanesum_compare(synth_lanesum("cflts@173", "w32", 0, 524288), "--md", md)
        self.assertEqual(rc, 0)
        with open(md, encoding="utf-8") as f:
            self.assertIn("| cflts@173 | w32 | 0 | 524288 | 2 | 2 | 0 | 0 | OK |", f.read())
        rc, text = run_lanesum_compare(write(HDR + ["META,backend,ppu"]))
        self.assertEqual(rc, 2)
        self.assertIn("no LANESUM rows", text)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = lanesum_compare.main([synth_lanesum("cflts@173", "w32", 0, 524288), "--scalar-bin", os.path.join(_TMP, "no_such_binary")])
        self.assertEqual(rc, 2)
        self.assertIn("cannot run", err.getvalue())


# ------------------------------------------------------------------------------------------------ dry run
def build_dry_run(workdir, mutant=False):
    """Builds the SPU ELF (make, native toolchain only) and the host driver; returns (elf, host_binary).

    When the SPU compiler cannot run on this host (linux toolchain on macOS), the ELF that
    `tools/hwprobe/build.sh spuexh layer1` produced is used instead; build it first in that case.
    """
    env = dict(os.environ, PS3DEV=PS3DEV, PSL1GHT=PS3DEV)
    subprocess.run([os.path.join(PROBE, "prebuild.sh")], check=True, capture_output=True, env=env)
    spu_src = os.path.join(PROBE, "spu")
    if mutant:                                    # a private copy of the probe tree with a planted adapter defect
        root = os.path.join(workdir, "pm")
        for sub in ("common", "generated", "spu"):
            shutil.copytree(os.path.join(PROBE, sub), os.path.join(root, sub), ignore=shutil.ignore_patterns("build", "*.elf", "*.map"))
        src = os.path.join(root, "spu", "source", "spu_main.c")
        with open(src) as f:
            text = f.read()
        with open(src, "w") as f:
            f.write("#define SPUEXH_MUTANT 1\n" + text)
        spu_src = os.path.join(root, "spu")
    if mutant or SPU_GCC_NATIVE:
        subprocess.run(["make", "-C", spu_src], check=True, capture_output=True, env=env)
        elf = os.path.join(spu_src, "spuexh.elf")
    else:
        if not os.path.exists(BUILT_ELF):
            raise unittest.SkipTest(f"no native SPU compiler and no prebuilt {BUILT_ELF}; "
                                    "run tools/hwprobe/build.sh spuexh layer1 first")
        elf = BUILT_ELF
    host = os.path.join(workdir, "spuexh_host_mut" if mutant else "spuexh_host")
    inc = ["-I", os.path.join(REPO, "runtime/spu"), "-I", os.path.join(REPO, "include"), "-I", os.path.join(REPO, "runtime/platform"),
           "-I", os.path.join(PROBE, "common"), "-I", os.path.join(PROBE, "generated")]
    objs = []
    for src, extra in (("host/host_interp.c", ["-w", "-pthread"] + inc), ("host/spuexh_host.c", ["-pthread"] + inc),
                       ("common/spuexh_core.c", inc)):
        o = os.path.join(workdir, os.path.basename(src) + ".o" + ("m" if mutant else ""))
        subprocess.run(["cc", "-std=gnu11", "-O1", "-Wno-unused-function", "-Wno-unused-variable", "-c", os.path.join(PROBE, src), "-o", o] + extra,
                       check=True, capture_output=True)
        objs.append(o)
    subprocess.run(["cc"] + objs + ["-pthread", "-lm", "-o", host], check=True, capture_output=True)
    return elf, host


def run_host(host, elf, sets, out, extra=()):
    argv = [host, "--elf", elf, "--preset", "smoke", "--set", "special_a=0", "--set", "special_b=0"]
    for s in sets:
        argv += ["--set", s]
    argv += ["--out", out] + list(extra)
    p = subprocess.run(argv, check=False, capture_output=True, text=True, timeout=600)
    with open(out) as f:
        return p, f.read().splitlines()


# Hashes of the host harness (dl/w2 scalar.txt, `test_spu_exhaustive` scalar build) for block 0 of the cheap domains.
HOST_REF = {
    ("absdb", "byte2", 0): "4bff3a566a5a2a83",
    ("avgb", "byte2", 0): "b5f4266cec6d6b2b",
}


@unittest.skipUnless(SPU_GCC and shutil.which("cc"), "needs the SPU toolchain (PS3DEV with spu/bin/spu-gcc) and cc")
class TestDryRun(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = os.path.join(_TMP, "dry")
        os.makedirs(cls.work)
        cls.elf, cls.host = build_dry_run(cls.work)

    def hashes(self, lines):
        return {tuple(l.split("\t")[:3]): l.split("\t")[3] for l in lines if "\t" in l and not l.startswith("#")}

    def test_hashes_equal_the_host_harness(self):
        p, lines = run_host(self.host, self.elf, ["doms=byte2", "ops=absdb,avgb", "spus=1"], os.path.join(_TMP, "d1.txt"))
        self.assertEqual(p.returncode, 0, p.stderr)
        h = self.hashes(lines)
        for (op, dom, blk), want in HOST_REF.items():
            self.assertEqual(h[(op, dom, str(blk))], want, op)
        self.assertIn("META,completed,1", lines)
        self.assertIn("META,faults,0", lines)

    def test_generator_self_check_equals_the_native_digests(self):
        _, lines = run_host(self.host, self.elf, ["doms=byte2", "ops=absdb", "spus=1"], os.path.join(_TMP, "d2g.txt"))
        sim = {l.split("\t")[1]: l.split("\t")[2] for l in lines if l.startswith("#gencheck")}
        nat = subprocess.run([self.host, "--gencheck"], capture_output=True, text=True, check=True).stdout.splitlines()
        nat = {l.split("\t")[1]: l.split("\t")[2] for l in nat}
        self.assertEqual(len(sim), 36)
        self.assertEqual(sim, nat)

    def test_result_file_has_the_standard_header(self):
        _, lines = run_host(self.host, self.elf, ["doms=byte2", "ops=absdb", "spus=1"], os.path.join(_TMP, "d2.txt"))
        hdr, _ = hwprobe_format.parse_header(lines)
        self.assertEqual(hdr["probe"], "spuexh")
        self.assertEqual(hdr["fw_type"], "host-interp")

    def test_thread_count_does_not_change_the_hashes(self):
        sets = ["doms=byte2,bits16,fsmbi", "ops=absdb,avgb,gb,gbh,fsmbi"]
        _, a = run_host(self.host, self.elf, sets + ["spus=1"], os.path.join(_TMP, "d3a.txt"))
        _, b = run_host(self.host, self.elf, sets + ["spus=3"], os.path.join(_TMP, "d3b.txt"))
        self.assertEqual(self.hashes(a), self.hashes(b))
        self.assertGreaterEqual(len(self.hashes(a)), 5)

    def test_an_injected_fault_is_retried_alone_and_costs_nothing(self):
        sets = ["doms=byte2,bits16,fsmbi", "ops=absdb,gb,fsmbi", "spus=1"]
        _, clean = run_host(self.host, self.elf, sets, os.path.join(_TMP, "d4a.txt"))
        # puts of SPU 0: 1 initial status, 2-3 generator self-check (entries, status), 4 job 1 started, 5 job 1 entries,
        # 6 job 1 status: dying at the 6th leaves an unfinished worker, which the supervisor re-runs alone (RETRY)
        p, hurt = run_host(self.host, self.elf, sets, os.path.join(_TMP, "d4b.txt"), ["--inject", "0:6"])
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual([l for l in hurt if l.startswith("FAULT,")], [], hurt)
        retries = [l for l in hurt if l.startswith("RETRY,")]
        self.assertEqual(retries, ["RETRY,spu0,job_1,ok"], hurt)
        self.assertIn("META,completed,1", hurt)
        self.assertIn("META,faults,0", hurt)
        self.assertEqual(self.hashes(clean), self.hashes(hurt))    # the interrupted job's hashes are kept

    def test_a_group_ending_fault_does_not_blame_healthy_workers(self):
        # a group-wide kill (the console's exception model) ends every worker mid-run; before the collateral
        # handling each unfinished worker was declared FAULT and its running job was lost
        sets = ["doms=byte2,immb,fsmbi,bits16,half2,wcnt,qcnt,qcnta,h16ll", "spus=2"]
        _, clean = run_host(self.host, self.elf, sets, os.path.join(_TMP, "d7a.txt"))
        p, hurt = run_host(self.host, self.elf, sets, os.path.join(_TMP, "d7b.txt"), ["--inject", "group:0:2"])
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual([l for l in hurt if l.startswith("FAULT,")], [], hurt)
        retries = [l.split(",")[1] for l in hurt if l.startswith("RETRY,")]
        self.assertEqual(sorted(retries), ["spu0", "spu1"], hurt)  # every interrupted worker completed alone
        self.assertIn("META,completed,1", hurt)
        self.assertIn("META,faults,0", hurt)
        self.assertEqual(self.hashes(clean), self.hashes(hurt))
        # an exception event is not taken as evidence of group termination: the launch had to terminate before joining
        self.assertIn("META,terminate_join,launch1", hurt)
        self.assertIn("META,exit_status_pending,0x8001ffff", hurt)

    def test_a_thread_that_never_exits_after_its_final_status_goes_through_terminate(self):
        # the SPU publishes its final status but never leaves the thread: the final DMA status is not a join permit
        sets = ["doms=byte2", "ops=absdb", "spus=1", "launch_timeout_ms=300"]
        p, lines = run_host(self.host, self.elf, sets, os.path.join(_TMP, "d10.txt"), ["--inject-hang-exit", "0"])
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual([l for l in lines if l.startswith("FATAL,")], [], lines)
        self.assertIn("META,terminate_join,launch1", lines)
        self.assertIn("META,exit_status_pending,0x8001ffff", lines)
        self.assertIn("META,completed,1", lines)
        self.assertEqual(self.hashes(lines), {("absdb", "byte2", "0"): HOST_REF[("absdb", "byte2", 0)]})

    def test_group_fault_with_a_failing_terminate_is_fatal_without_join(self):
        sets = ["doms=byte2,bits16,fsmbi", "ops=absdb,gb,fsmbi", "spus=2"]
        p, lines = run_host(self.host, self.elf, sets, os.path.join(_TMP, "d11.txt"),
                            ["--inject", "group:0:2", "--inject-fail", "terminate"])
        self.assertEqual(p.returncode, 1, p.stderr)
        fatals = [l for l in lines if l.startswith("FATAL,")]
        self.assertEqual(len(fatals), 1, lines)
        self.assertIn("no join after a failed terminate", fatals[0])
        self.assertEqual([l for l in lines if l.startswith("RETRY,")], [], lines)   # no join, no retry: the probe stopped
        self.assertIn("META,sweep_error,fatal_launch", lines)

    def test_injected_connect_failure_stops_the_probe(self):
        sets = ["doms=byte2,bits16,fsmbi", "ops=absdb,gb,fsmbi", "spus=1"]
        p, lines = run_host(self.host, self.elf, sets, os.path.join(_TMP, "d8.txt"), ["--inject-fail", "connect"])
        self.assertEqual(p.returncode, 1, p.stderr)
        fatals = [l for l in lines if l.startswith("FATAL,")]
        self.assertEqual(len(fatals), 1, lines)
        self.assertIn("injected connect failure", fatals[0])
        self.assertIn("META,sweep_error,fatal_launch", lines)
        self.assertEqual(self.hashes(lines), {})                   # nothing ran
        self.assertIn("META,completed,0", lines)

    def test_timeout_with_a_failing_terminate_stops_the_probe(self):
        sets = ["doms=byte2,bits16,fsmbi", "ops=absdb,gb,fsmbi", "spus=2", "launch_timeout_ms=1"]
        p, lines = run_host(self.host, self.elf, sets, os.path.join(_TMP, "d9.txt"), ["--inject-fail", "terminate"])
        self.assertEqual(p.returncode, 1, p.stderr)
        fatals = [l for l in lines if l.startswith("FATAL,")]
        self.assertEqual(len(fatals), 1, lines)
        self.assertIn("no join after a failed terminate", fatals[0])
        self.assertIn("META,sweep_error,fatal_launch", lines)
        self.assertIn("META,completed,0", lines)

    def test_special_probes_report_what_the_interpreter_does(self):
        argv = [self.host, "--elf", self.elf, "--preset", "smoke", "--no-sweep", "--special-a", "--special-b", "--set", "special_b_max=6",
                "--out", os.path.join(_TMP, "d6.txt")]
        p = subprocess.run(argv, check=False, capture_output=True, text=True, timeout=300)
        self.assertEqual(p.returncode, 0, p.stderr)
        with open(os.path.join(_TMP, "d6.txt")) as f:
            lines = f.read().splitlines()
        # the retail SPU executes none of the five DP compares (hardware, issue #124) and neither does the interpreter: the five
        # words are probed at their fixed encodings (verify_encodings.py checks them against spu-as), none executes, no case row follows
        speca = [l.split(",") for l in lines if l.startswith("SPECA_VALID")]
        self.assertEqual([(r[1], r[2], r[3]) for r in speca],
                         [("dfceq", "NOT_executed", "word_78610183"), ("dfcmeq", "NOT_executed", "word_79610183"),
                          ("dfcgt", "NOT_executed", "word_58610183"), ("dfcmgt", "NOT_executed", "word_59610183"),
                          ("dftsv", "NOT_executed", "word_77e00183")])
        for r in speca:
            self.assertIn("invalid instruction", ",".join(r[5:]))
        self.assertEqual([l for l in lines if l.startswith(("SPECA,", "SPECA_FAULT"))], [])
        b = [l.split(",") for l in lines if l.startswith("SPECB,")]
        self.assertEqual(len(b), 12)                                # 6 prefixes x 2 patterns
        self.assertTrue(all(r[5] == "no_return" for r in b))        # the interpreter has no decode for them
        self.assertIn("invalid instruction", b[0][-1])
        tr = [l.split(",") for l in lines if l.startswith("SPECB_TRY,")]
        self.assertEqual(len(tr), 12)                               # every word is pre-logged before its launch

    def test_special_a_ops_can_be_narrowed(self):
        out = os.path.join(_TMP, "d6a.txt")
        p = subprocess.run([self.host, "--elf", self.elf, "--preset", "smoke", "--no-sweep", "--special-a", "--set", "a_ops=dftsv,dfcgt",
                            "--out", out], check=False, capture_output=True, text=True, timeout=300)
        self.assertEqual(p.returncode, 0, p.stderr)
        with open(out) as f:
            valid = [l.split(",")[1] for l in f.read().splitlines() if l.startswith("SPECA_VALID")]
        self.assertEqual(valid, ["dfcgt", "dftsv"])                 # table order

    def test_dp_compares_cannot_be_selected_in_the_sweep(self):
        # the five ops are not in the op table since #124: ops=dfceq leaves no op, so the driver stops instead of running nothing
        p = subprocess.run([self.host, "--preset", "smoke", "--set", "ops=dfceq,dftsv", "--plan"], check=False, capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 2)
        self.assertIn("leave no op", p.stderr)
        p = subprocess.run([self.host, "--preset", "smoke", "--set", "doms=dtsv", "--plan"], check=False, capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 2)
        self.assertIn("matches no domain", p.stderr)

    def lanes_run(self, sets, tag, extra=()):
        out = os.path.join(_TMP, tag + ".csv")
        p, lines = run_host(self.host, self.elf, ["sweep=0", "spus=1"] + sets, out, extra)
        return out, p, lines

    def test_lanes_of_the_interpreter_equal_the_model(self):
        # the plumbing proof covers normal lanes; #126 now stops cflts/cfltu at I8 <= 14 before modelled results exist.
        sets = ["lanes=cflts@2:w32:0:100", "lanes=ai:immw:0:200", "lanes=ila:immw:0:100", "lanes=fma:fpool3:0:70",
                "lanes=a:wrand:0:130", "lanes=fesd:w32e:0:64"]
        out, p, lines = self.lanes_run(sets, "l1")
        self.assertEqual(p.returncode, 0, p.stderr)
        hdr = [i for i, l in enumerate(lines) if l.startswith("LANES,")]
        self.assertEqual(len(hdr), 6)
        self.assertIn("LANES,cflts@2,w32,0,100,8,0", lines)
        self.assertIn("LANES,fesd,w32e,0,64,0,0", lines)
        self.assertIn("LANES_DONE,ai,immw,0,200", lines)
        done = [l for l in lines if l.startswith("LANES_DONE,")]
        faults = [l for l in lines if l.startswith("LANES_FAULT,")]
        self.assertEqual(len(done), 5)                            # #126 stops only the cflts@2 entry in this config.
        self.assertEqual(len(faults), 1)
        self.assertTrue(faults[0].startswith("LANES_FAULT,cflts@2,w32,0,0,"), faults)
        self.assertEqual(sum(l.startswith("LANE,") for l in lines), 564)  # Its 100 calls stop before a 64-row chunk commits.
        self.assertIn("META,lanes,6", lines)
        rc, text = run_lanes_compare(out)
        self.assertEqual(rc, 0, text)
        for g in lanes_compare.parse_result(out):
            r = lanes_compare.compare_group(scalar_bin(), g)
            self.assertEqual(r.n_input_errors, 0, g.key)         # the immediates, signed ones included, are the generator's
            if g.status and g.status.startswith("fault:"):
                self.assertEqual((g.key[0], g.rows), ("cflts@2", []))
                continue
            self.assertGreater(len(r.lanes), 0)
            if g.op not in ("ai", "ila"):
                # (a harness older than 742e288d does not cut call immediates to the field the instruction has: its model
                # of the immediate-form ops differs from the instruction, which is why these two only check the inputs)
                self.assertEqual(r.differ(), 0, g.key)

    def test_lanes_conv_cfg_in_the_interpreter(self):
        out, p, lines = self.lanes_run([], "l2", ["--cfg", os.path.join(PROBE, "cfg", "lanes_conv.cfg")])
        self.assertEqual(p.returncode, 0, p.stderr)
        done = [l for l in lines if l.startswith("LANES_DONE,")]
        faults = [l for l in lines if l.startswith("LANES_FAULT,")]
        self.assertEqual(len(done), 3)                            # #126 stops the six cflts/cfltu I8=0..2 entries.
        self.assertEqual(len(faults), 6)
        self.assertEqual({l.split(",")[1] for l in faults},
                         {"cflts@0", "cflts@1", "cflts@2", "cfltu@0", "cfltu@1", "cfltu@2"})
        self.assertTrue(all(l.split(",")[4] == "0" for l in faults))
        self.assertEqual(sum(l.startswith("LANE,") for l in lines), 384)  # Only the two I8=173 controls and fesd publish rows.
        self.assertFalse([k for k in self.hashes(lines)])                       # sweep=0: no hash rows
        rc, text = run_lanes_compare(out)
        self.assertEqual(rc, 0, text)
        self.assertEqual(text.count("equal 256   differ 0"), 2)
        self.assertIn("equal 512   differ 0", text)

    def test_a_lanes_entry_that_faults_costs_that_entry_only(self):
        # puts of the SPU: 1 initial status, 2 "in progress" status, 3 first record chunk: dying there loses entry 1
        sets = ["lanes=cflts@173:w32:0:64", "lanes=cfltu@173:w32:0:64"]
        out, p, lines = self.lanes_run(sets, "l3", ["--inject", "0:3"])
        self.assertEqual(p.returncode, 0, p.stderr)
        faults = [l for l in lines if l.startswith("LANES_FAULT,")]
        self.assertEqual(len(faults), 1, lines)
        self.assertTrue(faults[0].startswith("LANES_FAULT,cflts@173,w32,0,0,"), faults)
        self.assertIn("LANES_DONE,cfltu@173,w32,0,64", lines)                  # the next entry ran
        self.assertEqual(sum(l.startswith("LANE,cfltu@173") for l in lines), 64)
        rc, text = run_lanes_compare(out)
        self.assertEqual(rc, 0, text)
        self.assertIn("LANES_FAULT", text)

    def test_a_lanes_entry_for_a_dp_compare_fails_at_start_up(self):
        # dfceq is not in the op table since #124: the entry names an unknown op and the host driver stops (rc 2) before any launch
        p = subprocess.run([self.host, "--elf", self.elf, "--preset", "smoke", "--set", "sweep=0", "--set", "lanes=dfceq:dpool2:0:10",
                            "--out", os.path.join(_TMP, "l4.csv")], check=False, capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 2)
        self.assertIn("lanes: unknown op 'dfceq'", p.stderr)

    def test_lanes_from_a_start_call_and_at_any_conversion_scale(self):
        sets = ["lanes=cflts@45:w32:0:100:1000", "lanes=cfltu@200:w32:0:64:500000", "lanes=csflt@7:w32:0:64", "lanes=cuflt@255:w32:0:64",
                "lanes=cflts@173:w32:0:1024:1047552"]                    # the last entry ends exactly at the end of the block
        out, p, lines = self.lanes_run(sets, "l5")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("LANES,cflts@45,w32,0,100,8,1000", lines)
        self.assertIn("LANES,cuflt@255,w32,0,64,8,0", lines)
        self.assertEqual([l for l in lines if l.startswith("LANES_FAULT")], [])
        calls = [int(l.split(",")[4]) for l in lines if l.startswith("LANE,cflts@45,")]
        self.assertEqual(calls, list(range(1000, 1100)))
        calls = [int(l.split(",")[4]) for l in lines if l.startswith("LANE,cflts@173,")]
        self.assertEqual((calls[0], calls[-1], len(calls)), (1047552, 1048575, 1024))
        for g in lanes_compare.parse_result(out):
            r = lanes_compare.compare_group(scalar_bin(), g)
            self.assertEqual((r.n_input_errors, r.differ()), (0, 0), g.key)    # the interpreter is the model at every scale
            self.assertGreater(len(r.lanes), 0)
        rc, text = run_lanes_compare(out)
        self.assertEqual(rc, 0, text)

    def test_lanesum_chunk_accounting_and_the_model(self):
        sets = ["lanesum=cflts@45:w32:0:300000", "lanesum=cflts@173:w32:0:262144"]
        out, p, lines = self.lanes_run(sets, "s1")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("LANESUM_BEGIN,cflts@45,w32,0,300000,4", lines)
        self.assertIn("LANESUM_DONE,cflts@45,w32,0,4", lines)
        self.assertIn("META,lanesums,2", lines)
        rows = [l.split(",") for l in lines if l.startswith("LANESUM,cflts@45,")]
        self.assertEqual([int(r[4]) for r in rows], [0, 300000, 600000, 900000])
        self.assertEqual([int(r[5]) for r in rows], [300000, 300000, 300000, 148576])    # the last chunk is short
        self.assertEqual(sum(int(r[5]) for r in rows), 1 << 20)
        self.assertEqual(self.hashes(lines), {})                                # no sweep rows
        # one chunk of the block is the sweep's block hash itself: lanesum at chunk = block size is the sweep's hash
        _out1, p, lines1 = self.lanes_run(["lanesum=cflts@173:w32:0:1048576"], "s2")
        one = [l.split(",") for l in lines1 if l.startswith("LANESUM,")]
        self.assertEqual(len(one), 1)
        out2 = os.path.join(_TMP, "s2sweep.csv")
        _, sw = run_host(self.host, self.elf, ["doms=w32", "ops=cflts@173", "spus=1", "bmax=1"], out2)
        self.assertEqual(one[0][6], self.hashes(sw)[("cflts@173", "w32", "0")])
        rc, text = run_lanesum_compare(out)
        self.assertEqual(rc, 0, text)
        self.assertIn("equal 4 ", text)

    def test_a_lanesum_entry_that_faults_costs_that_entry_only(self):
        # puts of the SPU: 1 initial status, 2 "in progress" status, 3 the hashes: dying there loses entry 1
        sets = ["lanesum=cflts@173:w32:0:524288", "lanesum=cfltu@173:w32:0:524288"]
        out, p, lines = self.lanes_run(sets, "s3", ["--inject", "0:3"])
        self.assertEqual(p.returncode, 0, p.stderr)
        faults = [l for l in lines if l.startswith("LANESUM_FAULT,")]
        self.assertEqual(len(faults), 1, lines)
        self.assertTrue(faults[0].startswith("LANESUM_FAULT,cflts@173,w32,0,0,"), faults)
        self.assertIn("LANESUM_DONE,cfltu@173,w32,0,2", lines)
        rc, text = run_lanesum_compare(out)
        self.assertEqual(rc, 0, text)
        self.assertIn("NOTE: LANESUM_FAULT", text)
        self.assertIn("2 missing", text)

    def test_the_conversion_and_fesd_cfgs_in_the_interpreter(self):
        out, p, lines = self.lanes_run([], "c2", ["--cfg", os.path.join(PROBE, "cfg", "lanes_conv2.cfg")])
        self.assertEqual(p.returncode, 0, p.stderr)
        done = [l for l in lines if l.startswith("LANES_DONE,")]
        faults = [l for l in lines if l.startswith("LANES_FAULT,")]
        self.assertEqual(len(done), 13)                           # #126 stops cflts@3, cflts@10 and cfltu@3.
        self.assertEqual(len(faults), 3)
        self.assertEqual({l.split(",")[1] for l in faults}, {"cflts@3", "cflts@10", "cfltu@3"})
        self.assertTrue(all(l.split(",")[4] == "0" for l in faults))
        self.assertEqual(sum(l.startswith("LANE,") for l in lines), 13 * 256)  # Each stopped entry has no committed chunk.
        rc, text = run_lanes_compare(out)
        self.assertEqual(rc, 0, text)
        self.assertEqual(text.count("equal 1024  differ 0"), 13)  # The remaining I8>=15 entries complete normally.
        out, p, lines = self.lanes_run([], "c3", ["--cfg", os.path.join(PROBE, "cfg", "lanesum_fesd.cfg")])
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(sum(l.startswith("LANESUM,") for l in lines), 256)
        rc, text = run_lanesum_compare(out)
        self.assertEqual(rc, 0, text)
        self.assertIn("256 chunks: 256 equal, 0 differ, 0 missing", text)


@unittest.skipUnless(shutil.which("cc"), "needs cc")
class TestHostWithoutConsole(unittest.TestCase):
    """The shared core and the host driver without the SPU toolchain: config parsing and the write-containment check."""

    @classmethod
    def setUpClass(cls):
        cls.work = os.path.join(_TMP, "hostonly")
        os.makedirs(cls.work)
        subprocess.run([os.path.join(PROBE, "prebuild.sh")], check=True, capture_output=True)
        inc = ["-I", os.path.join(REPO, "runtime/spu"), "-I", os.path.join(REPO, "include"),
               "-I", os.path.join(REPO, "runtime/platform"), "-I", os.path.join(PROBE, "common"),
               "-I", os.path.join(PROBE, "generated")]
        objs = []
        for src, extra in (("host/host_interp.c", ["-w", "-pthread"] + inc), ("host/spuexh_host.c", ["-pthread"] + inc),
                           ("common/spuexh_core.c", inc)):
            o = os.path.join(cls.work, os.path.basename(src) + ".o")
            subprocess.run(["cc", "-std=gnu11", "-O1", "-Wno-unused-function", "-Wno-unused-variable", "-c",
                            os.path.join(PROBE, src), "-o", o] + extra, check=True, capture_output=True)
            objs.append(o)
        cls.host = os.path.join(cls.work, "spuexh_host")
        subprocess.run(["cc"] + objs + ["-pthread", "-lm", "-o", cls.host], check=True, capture_output=True)
        # the outdir validator alone, exercised with the same core source the console build compiles
        probe_src = os.path.join(cls.work, "outdir_probe.c")
        with open(probe_src, "w") as f:
            f.write(OUTDIR_PROBE_SRC)
        cls.outdir_probe = os.path.join(cls.work, "outdir_probe")
        subprocess.run(["cc", "-std=gnu11", "-O1", "-Wno-unused-function", "-o", cls.outdir_probe, probe_src,
                        os.path.join(PROBE, "common/spuexh_core.c")] + inc, check=True, capture_output=True)

    def test_outdir_must_stay_inside_the_allowed_directory(self):
        p = subprocess.run([self.outdir_probe], check=False, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout)
        self.assertEqual(p.stdout, "")

    def test_non_positive_launch_limits_are_rejected(self):
        for key in ("case_timeout_ms", "b_timeout_ms", "launch_timeout_ms"):
            p = subprocess.run([self.host, "--set", f"{key}=0"], check=False, capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 2, p.stdout)
            self.assertIn(f"{key} must be > 0", p.stderr)
        p = subprocess.run([self.host, "--set", "special_b_patterns=bogus"], check=False, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 2)
        self.assertIn("special_b_patterns must be zero or both", p.stderr)

    def test_an_unknown_key_stops_the_driver_before_any_launch(self):
        # a misspelt key (here the safety keys special_b, launch_timeout_ms, outdir) must not fall back to a default
        out = os.path.join(_TMP, "unk.csv")
        for key in ("special_bb", "special_B", "launch_timeout", "outdirr", "preset_", ""):
            p = subprocess.run([self.host, "--elf", "/nonexistent.elf", "--out", out, "--set", f"{key}=1"], check=False,
                               capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 2, (key, p.stdout, p.stderr))
            self.assertIn(f"unknown key '{key}'", p.stderr)         # the config is checked before the ELF is even opened
            self.assertFalse(os.path.exists(out), key)              # nothing was started, no result file
        # the same in a cfg file, which also rejects a line without '=' instead of skipping it
        for text, want in (("preset=smoke\nspecial_bb=1\n", "unknown key 'special_bb'"), ("preset=smoke\nspecial_b\n", "line without '='"),
                           ("preset=smoke\nspecial_b=ture\n", "special_b must be 0 or 1, got 'ture'"),
                           ("preset=smoke\n sweep=1\n", "unknown key ' sweep'")):
            cfg = os.path.join(_TMP, "bad.cfg")
            with open(cfg, "w") as f:
                f.write(text)
            p = subprocess.run([self.host, "--elf", "/nonexistent.elf", "--out", out, "--cfg", cfg], check=False,
                               capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 2, (text, p.stderr))
            self.assertIn(want, p.stderr)
            self.assertFalse(os.path.exists(out), text)

    def test_the_console_keys_are_accepted_and_every_shipped_cfg_loads(self):
        for key in ("pad", "hold_s", "screen", "tag", "outdir", "fw_version", "fw_type", "console_model", "progress_file"):
            p = subprocess.run([self.host, "--plan", "--set", f"{key}=0"], check=False, capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 0, (key, p.stderr))
        cfgs = sorted(n for n in os.listdir(os.path.join(PROBE, "cfg")) if n.endswith(".cfg"))
        self.assertGreaterEqual(len(cfgs), 11)
        for name in cfgs:
            p = subprocess.run([self.host, "--cfg", os.path.join(PROBE, "cfg", name), "--plan"], check=False, capture_output=True,
                               text=True, timeout=60)
            self.assertEqual(p.returncode, 0, (name, p.stderr))

    def test_special_b_staging_keys_default_to_every_word_both_patterns(self):
        p = subprocess.run([self.host, "--plan"], check=False, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)                 # parses with the defaults alone
        self.assertIn("plan preset=smoke", p.stdout)

    def test_lanes_keys_are_validated_at_start_up(self):
        bad = {
            "lanes=nosuchop:w32:0": "unknown op",
            "lanes=cflts@0:nosuchdom:0": "unknown domain",
            "lanes=cflts@0:wrand:0": "is not run in domain",          # cflts@0 is a w32 op
            "lanes=cflts@0:w32": "expected OP:DOMAIN:BLOCK",
            "lanes=cflts@0:w32:0:64:1:1": "expected OP:DOMAIN:BLOCK",
            "lanes=cflts@0:w32:x": "is not a block number",
            "lanes=cflts@0:w32:-1": "is not a block number",
            "lanes=cflts@0:w32:99999999": "is not a block number",
            "lanes=cflts@0:w32:0:0": "count",
            "lanes=cflts@0:w32:0:1025": "count",
            "lanes=cflts@0:w32:0:12abc": "count",
        }
        for val, msg in bad.items():
            p = subprocess.run([self.host, "--set", val], check=False, capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 2, (val, p.stdout, p.stderr))
            self.assertIn(msg, p.stderr, val)
        argv = [self.host] + [a for i in range(17) for a in ("--set", f"lanes=a:wrand:0:{i + 1}")]
        p = subprocess.run(argv, check=False, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 2)
        self.assertIn("at most 16 lanes entries", p.stderr)
        argv = [self.host, "--plan"] + [a for i in range(16) for a in ("--set", f"lanes=cflts@173:w32:0:{i + 1}")]
        self.assertEqual(subprocess.run(argv, check=False, capture_output=True, text=True, timeout=60).returncode, 0)   # 16 are fine

    def test_lanes_conv_cfg_parses(self):
        p = subprocess.run([self.host, "--cfg", os.path.join(PROBE, "cfg", "lanes_conv.cfg"), "--plan"], check=False,
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_lanes_start_scale_and_lanesum_keys_are_validated(self):
        bad = {
            "lanes=cflts@256:w32:0": "unknown op",                  # the I8 field is 8 bits wide
            "lanes=cflts@:w32:0": "unknown op",
            "lanes=cflts@-1:w32:0": "unknown op",
            "lanes=cflts@1x:w32:0": "unknown op",
            "lanes=dfma@3:dpool3:0": "unknown op",                  # only the conversions take a scale
            "lanes=cflts@45:wrand:0": "is not run in domain",
            "lanes=cflts@45:w32:0:64:1048576": "start",              # the block has 2^20 calls
            "lanes=cflts@45:w32:0:64:x": "start",
            "lanes=cflts@45:w32:0:64:-1": "start",
            "lanes=cflts@45:w32:0:1024:1047553": "exceeds",
            "lanes=cflts@45:w32:0:64:0:0": "expected OP:DOMAIN:BLOCK",
            "lanesum=cflts@45:w32:0": "expected OP:DOMAIN:BLOCK:CHUNK",
            "lanesum=cflts@45:w32:0:0": "chunk",
            "lanesum=cflts@45:w32:0:99999999": "chunk",
            "lanesum=cflts@45:w32:0:255": "at most 4096",               # 4113 chunks
            "lanesum=cflts@256:w32:0:4096": "unknown op",
            "lanesum=cflts@45:wrand:0:4096": "is not run in domain",
            "lanesum=cflts@45:w32:99999999:4096": "is not a block number",
        }
        for val, msg in bad.items():
            p = subprocess.run([self.host, "--set", val], check=False, capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 2, (val, p.stdout, p.stderr))
            self.assertIn(msg, p.stderr, val)
        argv = [self.host] + [a for i in range(17) for a in ("--set", f"lanesum=cflts@45:w32:0:{4096 + i}")]
        p = subprocess.run(argv, check=False, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 2)
        self.assertIn("at most 16 lanesum entries", p.stderr)
        for val in ("lanes=cflts@255:w32:0:1:0", "lanes=cflts@0:w32:0:1024:1047552", "lanes=csflt@7:w32:0:64:1000", "lanes=cflts@173:w32:0",
                    "lanesum=cflts@45:w32:0:256", "lanesum=cuflt@200:w32:0:1048576", "lanesum=fesd:w32e:0:4096"):
            p = subprocess.run([self.host, "--plan", "--set", val], check=False, capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 0, (val, p.stderr))

    def test_new_cfgs_parse(self):
        for name in ("lanes_conv2.cfg", "lanesum_fesd.cfg"):
            p = subprocess.run([self.host, "--cfg", os.path.join(PROBE, "cfg", name), "--plan"], check=False,
                               capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 0, (name, p.stderr))


# The outdir tests of tools/hwprobe/probes/spuexh/common/spuexh_core.c: spuexh_outdir_ok() is the console probe's
# write-containment gate (results may only land below /dev_hdd0/tmp/hwprobe, with no ".." component).
OUTDIR_PROBE_SRC = r'''
#include <stdio.h>
#include "spuexh_core.h"
double spuexh_now(void) { return 0.0; }
int main(void) {
    static const char *const good[] = { "/dev_hdd0/tmp/hwprobe", "/dev_hdd0/tmp/hwprobe/sub",
                                        "/dev_hdd0/tmp/hwprobe/a/b", 0 };
    static const char *const bad[] = { "", "/dev_usb000", "/dev_usb000/x", "/dev_hdd0/tmp/hwprobeX",
                                       "/dev_hdd0/tmp/hwprobe/../evil", "/dev_hdd0/tmp/hwprobe/a/../../x",
                                       "/tmp/hwprobe", "/dev_hdd0", 0 };
    int fail = 0;
    for (int i = 0; good[i]; i++)
        if (spuexh_outdir_ok(good[i]) != 1) { printf("rejected allowed outdir %s\n", good[i]); fail++; }
    for (int i = 0; bad[i]; i++)
        if (spuexh_outdir_ok(bad[i]) != 0) { printf("accepted forbidden outdir %s\n", bad[i]); fail++; }
    return fail;
}
'''


@unittest.skipUnless(SPU_GCC_NATIVE and shutil.which("cc"), "needs a natively runnable SPU toolchain (it rebuilds the SPU program) and cc")
class TestPlantedAdapterDefect(unittest.TestCase):
    def test_a_wrong_instruction_word_is_caught_against_the_host_harness(self):
        work = os.path.join(_TMP, "mut")
        os.makedirs(work)
        elf, host = build_dry_run(work, mutant=True)
        _, lines = run_host(host, elf, ["doms=byte2", "ops=absdb,avgb", "spus=1"], os.path.join(_TMP, "m1.txt"))
        h = {tuple(l.split("\t")[:3]): l.split("\t")[3] for l in lines if "\t" in l and not l.startswith("#")}
        self.assertNotEqual(h[("absdb", "byte2", "0")], HOST_REF[("absdb", "byte2", 0)])      # planted: absdb executes avgb's word
        self.assertEqual(h[("avgb", "byte2", "0")], HOST_REF[("avgb", "byte2", 0)])
        ref = write([hrow("absdb", "byte2", 0, int(HOST_REF[("absdb", "byte2", 0)], 16)), hrow("avgb", "byte2", 0, int(HOST_REF[("avgb", "byte2", 0)], 16))])
        gen = write([ln for ln in lines if ln.startswith("#gencheck\t")])   # same generator evidence on both sides
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = cmp.main([os.path.join(_TMP, "m1.txt"), "--scalar", ref, "--gen", gen])
        self.assertEqual(rc, 1)
        self.assertIn("| absdb | 1 | 1/1 | HW!= scalar |", out.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
