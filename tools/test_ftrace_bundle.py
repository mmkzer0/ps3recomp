#!/usr/bin/env python3
"""ftrace_bundle: create from text and zstd runs, verify against tampering, prunable, failure atomicity, null-not-0 statistics, reports-only, backends, CLI contract."""
import contextlib
import hashlib
import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE / "ftrace_bundle.py"
REPORT_TOOL = HERE / "ftrace_report.py"
sys.path.insert(0, str(HERE))
import ftrace_bundle as fb
import ftrace_report as fr

FLIP_REQ, AUDIO_MIX, AUDIO_UNDERRUN = 9, 18, 20

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


def span(kind, who, a, b, t0_ms, t1_ms):
    return f"{kind} {who} {a:x} {b:x} {t0_ms * 1000:.0f} {t1_ms * 1000:.0f}"


def trace(flip_ms, audio=False, dropped=0):
    lines = [f"# ps3_ftrace start_us=0 end_us=200000 spans=0 dropped={dropped}", "T 1 main"]
    lines += [span(FLIP_REQ, 1, 0, 0, t, t) for t in flip_ms]
    if audio:
        lines += [span(AUDIO_MIX, 0, 1, 480, t, t + 0.1) for t in (0, 5, 10, 15, 20)]
        lines += [span(AUDIO_UNDERRUN, 0, 100, 3, 12, 12)]
    return ("\n".join(lines) + "\n").encode()


def raw_zstd(data, block=1 << 16):
    """A valid zstd frame of raw blocks, so a zstd source needs no compressor to be made."""
    out = bytearray(b"\x28\xb5\x2f\xfd\x00\x58")
    chunks = [data[i:i + block] for i in range(0, len(data), block)] or [b""]
    for n, c in enumerate(chunks):
        out += ((len(c) << 3) | (1 if n == len(chunks) - 1 else 0)).to_bytes(3, "little") + c
    return bytes(out)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def backend_available(name):
    saved = fr.ZSTD_BACKENDS
    fr.ZSTD_BACKENDS = (name,)
    try:
        return fr.zstd_backend() is not None
    finally:
        fr.ZSTD_BACKENDS = saved


HAVE_STDLIB = backend_available("stdlib")
HAVE_COMMAND = backend_available("subprocess")
CAN_COMPRESS = HAVE_STDLIB or HAVE_COMMAND


def tool(*args, cwd=None):
    q = subprocess.run([sys.executable, str(TOOL), *map(str, args)], capture_output=True, text=True, cwd=cwd,
                       timeout=300, check=False)
    return q.returncode, q.stdout, q.stderr


def tool_bytes(*args, cwd=None):
    q = subprocess.run([sys.executable, str(TOOL), *map(str, args)], capture_output=True, cwd=cwd,
                       timeout=300, check=False)
    return q.returncode, q.stdout, q.stderr.decode()


def inproc(backends, *args):
    saved = fr.ZSTD_BACKENDS
    fr.ZSTD_BACKENDS = backends
    out, err = io.StringIO(), io.StringIO()
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                code = fb.main([str(a) for a in args])
            except SystemExit as e:
                code = e.code
    finally:
        fr.ZSTD_BACKENDS = saved
    return code, out.getvalue(), err.getvalue()


def tree(path):
    p = Path(path)
    return sorted(str(x.relative_to(p)) for x in p.rglob("*")) if p.is_dir() else None


def mutate(bundle, rel, fn):
    p = Path(bundle) / rel
    p.write_bytes(fn(p.read_bytes()))


# Three runs: a text file, a zstd run directory, a text run directory. Only the zstd one has audio.
TEXT_A = trace([0, 20, 40, 80])
TEXT_B = trace([0, 10, 20, 30, 40, 50], audio=True)
TEXT_C = trace([0, 30, 60, 90, 120], dropped=5)


def make_runs(tmp):
    t = Path(tmp)
    (t / "runB").mkdir()
    (t / "runC").mkdir()
    (t / "a.txt").write_bytes(TEXT_A)
    (t / "runB" / "ftrace.txt.zst").write_bytes(raw_zstd(TEXT_B, block=200))
    (t / "runC" / "ftrace.txt").write_bytes(TEXT_C)
    return t / "a.txt", t / "runB", t / "runC"


def report_of(path):
    q = subprocess.run([sys.executable, str(REPORT_TOOL), str(path), "--json"], capture_output=True, text=True,
                       timeout=120, check=False)
    return q.stdout


# --- the contract of the headline metrics, without any file ---------------------------------------------
rep = {"frames": {"count": 3, "median_ms": 20.0, "min_ms": 10.0, "max_ms": 40.0}, "flip_requests": 4,
       "dropped": None, "audio": None, "jq2": None, "drawables": {"per_flip": None, "mean_wait_ms": 0.0}}
v = fb.headline_values(rep)
check(v["frames.median_ms"] == 20.0 and v["dropped"] is None and v["audio.underruns.count"] is None,
      "a metric the report lacks or holds as null is None, not 0")
check(v["drawables.mean_wait_ms"] == 0.0 and v["drawables.per_flip"] is None, "a real 0.0 stays 0.0")
check(fb.number(True) is None and fb.number(float("nan")) is None and fb.number("3") is None and fb.number(2) == 2,
      "only finite numbers reach the statistics")
audio_no_mix = {"audio": {"blocks_per_s": 0.0, "mix_interval_ms": {"p50": None, "p99": None}}}
check(fb.headline_values(audio_no_mix)["audio.blocks_per_s"] is None,
      "audio blocks/s of 0.0 with no mix spans is no data, not a rate of 0")
two = {"q": 1, "sync_latency_ms": {"p50": 1.5, "p95": 2.5, "p99": None}}
vq = fb.headline_values({"jq2": {"queues": [two]}})
check(vq["jq2.q1.sync_latency_ms.p50"] == 1.5 and vq["jq2.q1.sync_latency_ms.p99"] is None,
      "jq2 sync-wait percentiles are read per queue")
metrics, values, stats = fb.metric_table({"x": {"frames.count": 4}, "y": {"frames.count": None}, "z": {}})
check(stats["frames.count"] == {"n": 1, "mean": 4.0, "min": 4, "max": 4} and values["z"]["frames.count"] is None,
      "statistics count only the runs that have the metric")
check(stats["audio.stale.count"] == {"n": 0, "mean": None, "min": None, "max": None},
      "a metric no run has: n 0 and null statistics")
check(fb.parse_header("# ps3_ftrace start_us=0.000 end_us=2e5 spans=7 dropped=0 note=x") ==
      {"start_us": 0.0, "end_us": 200000.0, "spans": 7, "dropped": 0, "note": "x"}, "header fields parse as numbers")

# --- CLI contract --------------------------------------------------------------------------------------
rc, out, err = tool("--help")
check(rc == 0 and "examples:" in out and "create" in out and "verify" in out and "prunable" in out, "--help: examples and subcommands")
for sub in ("create", "verify", "prunable"):
    rc, out, err = tool(sub, "--help")
    check(rc == 0 and "example" in out, f"{sub} --help exits 0 with an example")
rc, out, err = tool()
check(rc == 2 and out == "", "no subcommand: exit 2")
rc, out, err = tool("create", "only-out")
check(rc == 2 and out == "", "create without a RUN: usage error, exit 2")
rc, out, err = tool("frobnicate")
check(rc == 2, "an unknown subcommand: exit 2")

with tempfile.TemporaryDirectory() as tmp:
    a, rb, rc_dir = make_runs(tmp)
    t = Path(tmp)
    out_dir = t / "bundle"

    if not CAN_COMPRESS:
        print("SKIP: stored traces: neither compression.zstd nor the zstd command is available")
    else:
        # --- create from a text file, a zstd run directory and a text run directory --------------------
        rc, so, err = tool("create", out_dir, a, rb, rc_dir)
        check(rc == 0, f"create: exit {rc}: {err}")
        check(so == str(out_dir) + "\n", f"create: stdout is the bundle path only ({so!r})")
        check("runB" in err, "create: progress goes to stderr")
        check(out_dir.is_dir() and not Path(str(out_dir) + ".partial").exists(), "create: OUT there, OUT.partial gone")
        man = json.loads((out_dir / "manifest.json").read_text())
        check(man["schema"] == "ps3recomp.ftrace-bundle/1" and man["reports_only"] is False, "manifest schema")
        check(re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", man["created_utc"]) is not None, "creation time is UTC ISO")
        check(re.fullmatch(r"[0-9a-f]{40,64}|unknown", man["toolkit_git_sha"]) is not None, "toolkit sha or 'unknown'")
        names = [r["name"] for r in man["runs"]]
        check(names == ["a", "runB", "runC"], f"run names from the file and directory names: {names}")
        sources = {"a": (a, TEXT_A), "runB": (rb / "ftrace.txt.zst", TEXT_B), "runC": (rc_dir / "ftrace.txt", TEXT_C)}
        for r in man["runs"]:
            src, text = sources[r["name"]]
            raw = src.read_bytes()
            check(r["size"] == len(raw) and r["sha256"] == sha(raw), f"{r['name']}: source size and sha256")
            check(r["decoded_sha256"] == sha(text), f"{r['name']}: sha256 of the decoded text")
            check(r["source_file"] == str(src) and r["source"] in (str(a), str(rb), str(rc_dir)),
                  f"{r['name']}: source as given and the file")
            check(r["header"] == {"start_us": 0, "end_us": 200000, "spans": 0, "dropped": 5 if r["name"] == "runC" else 0},
                  f"{r['name']}: parsed header {r['header']}")
            stored = out_dir / r["stored"]
            check(r["stored"] == f"traces/{r['name']}.ftrace.zst" and stored.is_file(), f"{r['name']}: stored trace")
            with fr.open_trace_bytes(str(stored)) as f:
                check(f.read() == text, f"{r['name']}: the stored trace decodes to the source text")
            check(man["files"][r["stored"]]["sha256"] == sha(stored.read_bytes()), f"{r['name']}: stored hash in the manifest")
            check((out_dir / r["report"]).read_text() == report_of(src),
                  f"{r['name']}: runs/<name>/report.json is exactly `ftrace_report.py --json`")
        check((out_dir / "traces/runB.ftrace.zst").read_bytes() == (rb / "ftrace.txt.zst").read_bytes(),
              "a zstd source is stored as a byte copy")

        # The joint report: null where a run lacks the metric, statistics over the others only.
        agg = json.loads((out_dir / "report.json").read_text())
        check(agg["runs"] == names and agg["values"]["a"]["frames.median_ms"] == 20.0
              and agg["values"]["runB"]["frames.median_ms"] == 10.0 and agg["values"]["runC"]["frames.median_ms"] == 30.0,
              "report.json: frame time median per run")
        s = agg["stats"]["frames.median_ms"]
        check(s["n"] == 3 and abs(s["mean"] - 20.0) < 1e-9 and s["min"] == 10.0 and s["max"] == 30.0, f"frame time statistics {s}")
        check(agg["values"]["a"]["audio.underruns.count"] is None and agg["values"]["runC"]["audio.underruns.count"] is None
              and agg["values"]["runB"]["audio.underruns.count"] == 1, "audio underruns: null for runs without audio")
        s = agg["stats"]["audio.underruns.count"]
        check(s == {"n": 1, "mean": 1.0, "min": 1, "max": 1}, f"the two runs without audio are not counted as 0: {s}")
        check(agg["stats"]["dropped"]["n"] == 3 and agg["stats"]["dropped"]["max"] == 5, "dropped spans from the header")
        md = (out_dir / "REPORT.md").read_text()
        check("| frame time, median (`frames.median_ms`) | ms | 20.000 | 10.000 | 30.000 | 3 | 20.000 | 10.000 | 30.000 |" in md
              and "n/a" in md, "REPORT.md: a row per metric, n/a for a missing value")

        rc, so, err = tool("verify", out_dir)
        check(rc == 0 and so == "", f"verify: a fresh bundle is ok, nothing on stdout ({rc}: {err})")

        # --- verify catches every kind of damage ---------------------------------------------------------
        damage = (
            ("a tampered per-run report", "runs/a/report.json", lambda b: b.replace(b"20", b"21", 1)),
            ("a tampered stored trace", "traces/runB.ftrace.zst", lambda b: b + b"x"),
            ("a tampered stored text-source copy", "traces/a.ftrace.zst", lambda b: b[:-1] + bytes([b[-1] ^ 1])),
            ("a tampered joint report", "report.json", lambda b: b.replace(b"20", b"22", 1)),
            ("a tampered REPORT.md", "REPORT.md", lambda b: b + b"\n"),
        )
        for label, rel, fn in damage:
            copy = t / "damaged"
            shutil.copytree(out_dir, copy)
            mutate(copy, rel, fn)
            rc, so, err = tool("verify", copy)
            check(rc == 1 and rel in err and so == "", f"verify: {label} is exit 1 naming {rel} ({rc}: {err})")
            rc, so, err = tool("prunable", copy, "-0")
            check(rc == 1 and so == "", f"prunable: {label}: a bundle that does not verify lists nothing")
            shutil.rmtree(copy)
        copy = t / "damaged"
        shutil.copytree(out_dir, copy)
        (copy / "traces/runC.ftrace.zst").unlink()
        rc, so, err = tool("verify", copy)
        check(rc == 1 and "traces/runC.ftrace.zst: missing" in err, "verify: a missing stored file is exit 1")
        shutil.rmtree(copy)
        shutil.copytree(out_dir, copy)
        man2 = json.loads((copy / "manifest.json").read_text())
        man2["schema"] = "ps3recomp.ftrace-bundle/2"
        (copy / "manifest.json").write_text(json.dumps(man2))
        rc, so, err = tool("verify", copy)
        check(rc == 1 and "schema" in err, "verify: another schema is exit 1")
        shutil.rmtree(copy)
        shutil.copytree(out_dir, copy)
        man2 = json.loads((copy / "manifest.json").read_text())
        man2["files"]["../outside"] = {"size": 0, "sha256": "0" * 64}
        (copy / "manifest.json").write_text(json.dumps(man2))
        rc, so, err = tool("verify", copy)
        check(rc == 1 and "not a path inside the bundle" in err, "verify: a manifest entry outside the bundle is refused")
        shutil.rmtree(copy)
        rc, so, err = tool("verify", t / "nothing-here")
        check(rc == 2, "verify: not a directory is bad input, exit 2")
        (t / "empty-dir").mkdir()
        rc, so, err = tool("verify", t / "empty-dir")
        check(rc == 1 and "manifest.json" in err, "verify: a directory without a manifest does not verify")

        # --- prunable ------------------------------------------------------------------------------------
        sources_listed = [str(a), str(rb / "ftrace.txt.zst"), str(rc_dir / "ftrace.txt")]
        rc, so, err = tool_bytes("prunable", out_dir, "-0")
        check(rc == 0 and so == b"\0".join(p.encode() for p in sources_listed) + b"\0", f"prunable -0: NUL-terminated sources ({so!r})")
        rc, so, err = tool("prunable", out_dir)
        check(rc == 0 and so.split("\n") == sources_listed + [""], "prunable: one source per line, a directory run names its trace file")
        before = tree(t)
        (rc_dir / "ftrace.txt").write_bytes(TEXT_C + b"# edited after bundling\n")     # modified source
        moved = t / "moved.txt"
        a.rename(moved)                                                               # missing source
        rc, so, err = tool_bytes("prunable", out_dir, "-0")
        check(rc == 0 and so == str(rb / "ftrace.txt.zst").encode() + b"\0", f"prunable: only the untouched source is listed ({so!r})")
        check("runC" in err and "no longer matches" in err and "a: " in err and "missing" in err,
              f"prunable: it says why the others are kept ({err!r})")
        check(tree(t) == sorted(set(before) - {"a.txt"} | {"moved.txt"}) and (rb / "ftrace.txt.zst").is_file(),
              "prunable deletes nothing")
        moved.rename(a)
        (rc_dir / "ftrace.txt").write_bytes(TEXT_C)
        rc, so, err = tool("prunable", out_dir, "-0", cwd=t / "runB")
        check(rc == 0 and so.count("\0") == 3, "prunable: sources are found from any directory (absolute in the manifest)")

        # A source inside the bundle is never listed.
        inner = t / "inner"
        rc, so, err = tool("create", "--quiet", inner, out_dir / "traces" / "runB.ftrace.zst")
        check(rc == 0, f"create from a trace inside another bundle: {err}")
        rc, so, err = tool("prunable", inner)
        check(rc == 0 and so == str(out_dir / "traces" / "runB.ftrace.zst") + "\n", "prunable lists a source outside its own bundle")

        # --- create never overwrites ---------------------------------------------------------------------
        (out_dir / "sentinel").write_text("keep")
        snap = tree(out_dir)
        rc, so, err = tool("create", out_dir, a)
        check(rc == 2 and "exists" in err and so == "" and tree(out_dir) == snap
              and (out_dir / "sentinel").read_text() == "keep", "OUT exists: exit 2, untouched")
        (out_dir / "sentinel").unlink()
        filepath = t / "afile"
        filepath.write_text("x")
        rc, so, err = tool("create", filepath, a)
        check(rc == 2 and filepath.read_text() == "x", "OUT is a file: exit 2, untouched")
        stale = Path(str(t / "stale") + ".partial")
        stale.mkdir()
        rc, so, err = tool("create", t / "stale", a)
        check(rc == 2 and stale.is_dir() and not (t / "stale").exists() and tree(stale) == [], "a stale OUT.partial is refused, not deleted")
        rc, so, err = tool("create", t / "no-parent" / "b", a)
        check(rc == 2 and "parent directory" in err, "OUT in a missing directory: exit 2")

        # --- the same inputs again: same reports ---------------------------------------------------------
        again = t / "again"
        rc, so, err = tool("create", "--quiet", again, a, rb, rc_dir)
        man3 = json.loads((again / "manifest.json").read_text())
        check(rc == 0 and all(man3["files"][k] == man["files"][k] for k in man["files"] if k.startswith(("runs/", "traces/"))),
              "creating again from the same inputs gives the same reports and stored traces")

        # --- a relative RUN: the manifest keeps it as given and the absolute file ------------------------
        rel = t / "rel.bundle"
        rc, so, err = tool("create", "--quiet", rel, "runB", cwd=t)
        m = json.loads((rel / "manifest.json").read_text())["runs"][0]
        check(rc == 0 and m["source"] == "runB" and Path(m["source_file"]).is_absolute()
              and Path(m["source_file"]).samefile(rb / "ftrace.txt.zst"), "relative RUN: source as given, file absolute")

    # --- atomicity: a bad trace anywhere leaves neither OUT nor OUT.partial -------------------------------
    bad_runs = {
        "a truncated zstd trace": raw_zstd(TEXT_A, block=100)[:-30],
        "fewer than 3 flip requests": trace([0, 20]),
        "a malformed span line": TEXT_A + b"9 1 zz 0 1 2\n",
    }
    for label, blob in bad_runs.items():
        bad = t / "bad.txt"
        bad.write_bytes(blob)
        target = t / "never"
        rc, so, err = tool("create", "--reports-only", target, a, bad, rc_dir)
        check(rc == 2 and so == "" and err.startswith("ftrace_bundle:") and "Traceback" not in err,
              f"{label}: exit 2 with a message ({rc}: {err[:120]!r})")
        check(not target.exists() and not Path(str(target) + ".partial").exists(), f"{label}: neither OUT nor OUT.partial remains")
    rc, so, err = tool("create", "--reports-only", t / "never", a, t / "missing.txt")
    check(rc == 2 and not (t / "never").exists(), "a RUN that does not exist: exit 2")

    # --- names ---------------------------------------------------------------------------------------------
    (t / "d1").mkdir()
    (t / "d2").mkdir()
    (t / "d1" / "x").mkdir()
    (t / "d2" / "x").mkdir()
    (t / "d1" / "x" / "ftrace.txt").write_bytes(TEXT_A)
    (t / "d2" / "x" / "ftrace.txt").write_bytes(TEXT_B)
    rc, so, err = tool("create", "--reports-only", t / "collide", t / "d1" / "x", t / "d2" / "x")
    check(rc == 2 and "already taken" in err and not (t / "collide").exists()
          and not Path(str(t / "collide") + ".partial").exists(), f"a name collision: exit 2, nothing left ({err!r})")
    (t / "d3" / "X").mkdir(parents=True)
    (t / "d3" / "X" / "ftrace.txt").write_bytes(TEXT_B)
    rc, so, err = tool("create", "--reports-only", t / "collide", t / "d1" / "x", t / "d3" / "X")
    check(rc == 2 and "already taken" in err, "names that differ only in case collide")
    (t / "weird name").mkdir()
    (t / "weird name" / "ftrace.txt").write_bytes(TEXT_A)
    rc, so, err = tool("create", "--reports-only", t / "weird", t / "weird name")
    check(rc == 2 and "must match" in err and not (t / "weird").exists(), "a name that is not safe as a path: exit 2")

    # --- run directories ---------------------------------------------------------------------------------
    (t / "both").mkdir()
    (t / "both" / "ftrace.txt").write_bytes(TEXT_A)
    (t / "both" / "ftrace.txt.zst").write_bytes(raw_zstd(TEXT_A))
    (t / "neither").mkdir()
    for label, d in (("both traces", "both"), ("no trace", "neither")):
        rc, so, err = tool("create", "--reports-only", t / "dirout", t / d)
        check(rc == 2 and "exactly one of" in err and not (t / "dirout").exists(), f"a run directory with {label}: exit 2")

    # --- reports-only --------------------------------------------------------------------------------------
    ro = t / "ro"
    rc, so, err = tool("create", "--quiet", "--reports-only", ro, a, rb, rc_dir)
    check(rc == 0 and not (ro / "traces").exists(), f"--reports-only: no traces directory ({err})")
    mro = json.loads((ro / "manifest.json").read_text())
    check(mro["reports_only"] is True and all(r["stored"] is None for r in mro["runs"])
          and not any(k.startswith("traces/") for k in mro["files"]), "--reports-only: the manifest says so")
    check(json.loads((ro / "report.json").read_text())["stats"]["frames.median_ms"]["n"] == 3, "--reports-only: still a joint report")
    rc, so, err = tool("verify", ro)
    check(rc == 0, f"--reports-only: verifies ({err})")
    rc, so, err = tool("prunable", ro, "-0")
    check(rc == 0 and so == "" and "reports only" in err, "--reports-only: prunable prints nothing")
    rc, so, err = tool_bytes("prunable", ro, "-0", "--allow-reports-only")
    check(rc == 0 and so.count(b"\0") == 3, "--reports-only with --allow-reports-only: the sources are listed")
    # A reports-only bundle needs no compressor, even for a text source.
    code, so, err = inproc((), "create", "--quiet", "--reports-only", t / "ro-nobackend", a)
    check(code == 0, f"--reports-only with no zstd backend works for a text source ({err})")

    # --- backends -------------------------------------------------------------------------------------------
    code, so, err = inproc((), "create", "--quiet", t / "nb1", a)
    check(code == 2 and "compression.zstd" in err and "`zstd` command" in err and "--reports-only" in err
          and not (t / "nb1").exists() and not Path(str(t / "nb1") + ".partial").exists(),
          f"no backend and a text source to store: exit 2 naming both options, nothing left ({err!r})")
    code, so, err = inproc((), "create", "--quiet", t / "nb2", rb)
    check(code == 2 and "compression.zstd" in err and "`zstd` command" in err and not (t / "nb2").exists()
          and not Path(str(t / "nb2") + ".partial").exists(), "no backend and a zstd source: exit 2 naming both options")
    for name, available, why in (("stdlib", HAVE_STDLIB, "Python < 3.14 has no compression.zstd"),
                                 ("subprocess", HAVE_COMMAND, "no zstd command on PATH")):
        if not available:
            print(f"SKIP: bundle zstd {name} backend: {why}")
            continue
        target = t / f"be-{name}"
        code, so, err = inproc((name,), "create", "--quiet", target, a, rb)
        check(code == 0, f"backend {name}: create ({err})")
        if code == 0:
            for n, text in (("a", TEXT_A), ("runB", TEXT_B)):
                with fr.open_trace_bytes(str(target / "traces" / f"{n}.ftrace.zst")) as f:
                    check(f.read() == text, f"backend {name}: {n} stored and decodes to the source")
            code, so, err = inproc((name,), "verify", target)
            check(code == 0, f"backend {name}: verify ({err})")

print("ftrace_bundle: %s" % ("ok" if not failures else f"{failures} failed"))
sys.exit(1 if failures else 0)
