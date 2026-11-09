#!/usr/bin/env python3
"""Bundle PS3_FTRACE traces of several runs, report on them together, and say which loose traces may go.

  ftrace_bundle.py create OUT RUN... [--reports-only]
  ftrace_bundle.py verify OUT
  ftrace_bundle.py prunable OUT [-0] [--allow-reports-only]

RUN is a trace file (text or zstd, told apart by content) or a run directory holding exactly one of
ftrace.txt and ftrace.txt.zst. The tool reads traces through tools/ftrace_report.py, so a zstd trace
needs Python 3.14+ (compression.zstd) or the `zstd` command on PATH; so does storing a text trace.

Layout of OUT (a directory, built as OUT.partial and renamed, so a failure leaves neither):
  manifest.json            schema ps3recomp.ftrace-bundle/1: per run its name, source path, the source file's
                           size and sha256, the sha256 of the decoded text, the parsed header fields and
                           the stored paths; the creation time (UTC); the toolkit git sha; a size and sha256
                           for every other file below
  traces/<name>.ftrace.zst a byte copy of a zstd source, or a compressed copy of a text source, checked by
                           decoding it back to the same sha256 (absent with --reports-only)
  runs/<name>/report.json  exactly what `ftrace_report.py --json` prints for that trace
  report.json, REPORT.md   the headline metrics per run and their n/mean/min/max across runs

Headline metrics (keys of the ftrace_report.py JSON; a metric a run does not have is null, shown as n/a,
and is left out of the statistics, never counted as 0):
  frames.count, frames.median_ms, frames.min_ms, frames.max_ms   frame time between flip requests (the
                           report carries median/min/max, no percentiles of frame time)
  flip_requests, dropped   flip requests in the window; spans the recorder dropped (header)
  drawables.per_flip, drawables.mean_wait_ms                     nextDrawable calls per flip, mean wait
  label_wait.ms_per_flip, .p99_ms, .max_ms                        guest label-store wait
  audio.underruns.count, audio.underruns.rate (per s), audio.stale.count (blocks), audio.stale.runs,
  audio.stale.longest_run_ms, audio.blocks_per_s, audio.mix_interval_ms.p99
  jq2.q<N>.sync_latency_ms.p50|p95|p99                          per SPURS job queue, from the credit to
                           the blocked thread running again (queues are numbered in creation order; a run
                           without that queue has null)
The report has no flips-per-second key, so there is none here: frames.median_ms is its inverse.

`prunable` prints the source files of the runs the bundle covers whose sha256 still equals the manifest's,
but only if the bundle verifies and holds the stored traces. For a run given as a directory that is the
trace file inside it. This tool never deletes anything: you do, from that list.

Exit status: 0 done (verify: every hash matches); 1 verify or prunable found the bundle damaged;
2 usage error or bad input (an unreadable, truncated or malformed trace, no zstd decoder, OUT exists, a
name collision, a directory with both or neither trace file).
Standard output carries data only (create: the bundle path; prunable: the paths); progress and errors go to
standard error.
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ftrace_report as fr

SCHEMA = "ps3recomp.ftrace-bundle/1"
REPORT_SCHEMA = "ps3recomp.ftrace-bundle-report/1"
ZSTD_LEVEL = 3
CHUNK = 1 << 20
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")
HASH_RE = re.compile(r"^[0-9a-f]{64}$")
DIR_TRACES = ("ftrace.txt", "ftrace.txt.zst")
NO_COMPRESSOR = ("cannot store a text trace: compressing needs Python 3.14 or newer (compression.zstd) or the "
                 f"`{fr.ZSTD_COMMAND}` command on PATH, and neither is available; use --reports-only to skip "
                 "the stored traces")


class Failure(Exception):
    """Exit 2: usage or bad input."""


def say(msg):
    print(f"ftrace_bundle: {msg}", file=sys.stderr)


def sha256_file(path):
    h, size = hashlib.sha256(), 0
    with open(path, "rb") as f:
        while chunk := f.read(CHUNK):
            h.update(chunk)
            size += len(chunk)
    return size, h.hexdigest()


def decoded_sha256(path):
    """sha256 of the trace's bytes as ftrace_report reads them (a zstd file is decoded first)."""
    h = hashlib.sha256()
    with fr.open_trace_bytes(path) as f:
        while chunk := f.read(CHUNK):
            h.update(chunk)
    return h.hexdigest()


def is_zstd(path):
    with open(path, "rb") as f:
        return f.read(len(fr.ZSTD_MAGIC)) == fr.ZSTD_MAGIC


def write_file(path, data):
    """Write bytes or str to a new file and sync it. Returns (size, sha256)."""
    raw = data.encode("utf-8") if isinstance(data, str) else data
    with open(path, "xb") as f:
        f.write(raw)
        f.flush()
        os.fsync(f.fileno())
    return len(raw), hashlib.sha256(raw).hexdigest()


def parse_header(header):
    """The key=value fields of the trace's `# ps3_ftrace ...` line (numbers where they parse), or {}."""
    fields = {}
    for tok in header.lstrip("# ").split()[1:]:
        key, eq, val = tok.partition("=")
        if not eq:
            continue
        for conv in (int, float):
            try:
                fields[key] = conv(val)
                break
            except ValueError:
                continue
        else:
            fields[key] = val
    return fields


# --- runs ------------------------------------------------------------------------------------------

def locate(run):
    """(trace file, run name) of a RUN argument."""
    if os.path.isdir(run):
        found = [n for n in DIR_TRACES if os.path.isfile(os.path.join(run, n))]
        if len(found) != 1:
            raise Failure(f"{run}: a run directory must hold exactly one of {' / '.join(DIR_TRACES)}, "
                          f"found {' and '.join(found) if found else 'neither'}")
        return os.path.join(run, found[0]), os.path.basename(os.path.abspath(run))
    if not os.path.isfile(run):
        raise Failure(f"{run}: no such trace file or run directory")
    name = os.path.basename(run)
    name = name.removesuffix(".zst").removesuffix(".txt")
    if name == "ftrace":    # a trace picked out of its run directory: the directory names the run
        name = os.path.basename(os.path.dirname(os.path.abspath(run)))
    return run, name


def check_names(runs):
    seen = {}
    for given, _, name in runs:
        src = given
        if not NAME_RE.match(name):
            raise Failure(f"{src}: run name {name!r} must match {NAME_RE.pattern} (it names files in the bundle)")
        # Compared case-insensitively: two names that differ in case are one directory on macOS and Windows.
        if name.lower() in seen:
            raise Failure(f"{src}: run name {name!r} is already taken by {seen[name.lower()]}; "
                          "rename one run (a directory or file name decides it)")
        seen[name.lower()] = src


def toolkit_sha():
    here = Path(__file__).resolve().parent
    try:
        q = subprocess.run(["git", "-C", str(here), "rev-parse", "HEAD"], capture_output=True, text=True,
                           timeout=10, check=False)
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    sha = q.stdout.strip()
    return sha if q.returncode == 0 and re.fullmatch(r"[0-9a-f]{40,64}", sha) else "unknown"


def compress_copy(src, dst):
    """Write src (a text trace) to dst as a zstd file through whichever backend is available."""
    backend = fr.zstd_backend()
    if backend is None:
        raise Failure(NO_COMPRESSOR)
    name, handle = backend
    with open(src, "rb") as fin, open(dst, "xb") as fout:
        if name == "stdlib":
            with handle.ZstdFile(fout, "wb", level=ZSTD_LEVEL) as z:
                shutil.copyfileobj(fin, z, CHUNK)
        else:
            q = subprocess.run([handle, "-q", f"-{ZSTD_LEVEL}", "-T0", "-c"], stdin=fin, stdout=fout,
                               stderr=subprocess.PIPE, check=False)
            if q.returncode != 0:
                why = q.stderr.decode("utf-8", "replace").strip()[:300]
                raise Failure(f"{src}: zstd exited {q.returncode} while compressing: {why}")
        fout.flush()
        os.fsync(fout.fileno())


def copy_file(src, dst):
    with open(src, "rb") as fin, open(dst, "xb") as fout:
        shutil.copyfileobj(fin, fout, CHUNK)
        fout.flush()
        os.fsync(fout.fileno())


# --- headline metrics --------------------------------------------------------------------------------

def dig(report, path):
    cur = report
    for part in path.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


def _blocks_per_s(report):
    # 0.0 is what the report says when there are no mix spans; with no mix interval it is "no data".
    return dig(report, "audio.blocks_per_s") if dig(report, "audio.mix_interval_ms.p50") is not None else None


# (key, label, unit, dotted path into the report JSON or a function of the report)
HEADLINE = (
    ("frames.count", "frames", "", "frames.count"),
    ("frames.median_ms", "frame time, median", "ms", "frames.median_ms"),
    ("frames.min_ms", "frame time, min", "ms", "frames.min_ms"),
    ("frames.max_ms", "frame time, max", "ms", "frames.max_ms"),
    ("flip_requests", "flip requests", "", "flip_requests"),
    ("dropped", "dropped spans", "", "dropped"),
    ("drawables.per_flip", "drawable waits per flip", "", "drawables.per_flip"),
    ("drawables.mean_wait_ms", "drawable wait, mean", "ms", "drawables.mean_wait_ms"),
    ("label_wait.ms_per_flip", "label-store wait per flip", "ms", "label_wait.ms_per_flip"),
    ("label_wait.p99_ms", "label-store wait, p99", "ms", "label_wait.p99_ms"),
    ("label_wait.max_ms", "label-store wait, max", "ms", "label_wait.max_ms"),
    ("audio.underruns.count", "audio underruns", "", "audio.underruns.count"),
    ("audio.underruns.rate", "audio underruns per s", "1/s", "audio.underruns.rate"),
    ("audio.stale.count", "audio stale blocks", "", "audio.stale.count"),
    ("audio.stale.runs", "audio stale runs", "", "audio.stale.runs"),
    ("audio.stale.longest_run_ms", "audio stale, longest run", "ms", "audio.stale.longest_run_ms"),
    ("audio.blocks_per_s", "audio blocks mixed per s", "1/s", _blocks_per_s),
    ("audio.mix_interval_ms.p99", "audio mix interval, p99", "ms", "audio.mix_interval_ms.p99"),
)
JQ2_PERCENTILES = ("p50", "p95", "p99")


def number(v):
    """v if it is a finite number (not a bool), else None: what the statistics may use."""
    if isinstance(v, bool) or not isinstance(v, (int, float)) or (isinstance(v, float) and not math.isfinite(v)):
        return None
    return v


def headline_values(report):
    """{metric key: number or None} for one run's report; jq2 keys exist for the queues the run has."""
    out = {key: number(getter(report) if callable(getter) else dig(report, getter))
           for key, _, _, getter in HEADLINE}
    jq2 = report.get("jq2") or {}
    for q in jq2.get("queues") or []:
        for p in JQ2_PERCENTILES:
            out[f"jq2.q{q['q']}.sync_latency_ms.{p}"] = number(dig(q, f"sync_latency_ms.{p}"))
    return out


def metric_table(per_run):
    """(metric descriptors, {run: {key: value or None}}, {key: stats}) over all runs, in a stable order."""
    meta = {key: (label, unit) for key, label, unit, _ in HEADLINE}
    keys = [key for key, *_ in HEADLINE]
    jq2 = set()
    for values in per_run.values():
        jq2.update(k for k in values if k.startswith("jq2."))
    for key in sorted(jq2, key=lambda k: (int(k.split(".")[1][1:]), JQ2_PERCENTILES.index(k.rsplit(".", 1)[1]))):
        keys.append(key)
        q = key.split(".")[1]
        meta[key] = (f"jq2 {q} sync latency, {key.rsplit('.', 1)[1]}", "ms")
    values = {run: {k: v.get(k) for k in keys} for run, v in per_run.items()}
    stats = {}
    for k in keys:
        xs = [values[r][k] for r in values if values[r][k] is not None]
        stats[k] = {"n": len(xs), "mean": sum(xs) / len(xs) if xs else None,
                    "min": min(xs) if xs else None, "max": max(xs) if xs else None}
    return [{"key": k, "label": meta[k][0], "unit": meta[k][1]} for k in keys], values, stats


def fmt(v):
    if v is None:
        return "n/a"
    return str(v) if isinstance(v, int) else f"{v:.3f}"


def report_markdown(manifest, metrics, values, stats):
    names = [r["name"] for r in manifest["runs"]]
    created = f"Created {manifest['created_utc']} from {len(names)} run(s); toolkit {manifest['toolkit_git_sha']}. "
    stored = "no (--reports-only)" if manifest["reports_only"] else "yes"
    lines = ["# ftrace bundle", "",
             created + f"Stored traces: {stored}.", "",
             "n/a means the run's report has no such metric; it is left out of n, mean, min and max.", "",
             "| metric | unit | " + " | ".join(names) + " | n | mean | min | max |",
             "|---|---|" + "---|" * len(names) + "---|---|---|---|"]
    for m in metrics:
        s = stats[m["key"]]
        cells = [fmt(values[n][m["key"]]) for n in names]
        lines.append(f"| {m['label']} (`{m['key']}`) | {m['unit']} | " + " | ".join(cells)
                     + f" | {s['n']} | {fmt(s['mean'])} | {fmt(s['min'])} | {fmt(s['max'])} |")
    lines += ["", "## Runs", ""]
    for r in manifest["runs"]:
        lines.append(f"- `{r['name']}`: {r['source']} ({r['size']} bytes, sha256 `{r['sha256'][:16]}...`)")
    return "\n".join(lines) + "\n"


# --- create ------------------------------------------------------------------------------------------

def add_run(root, given, src, name, reports_only, log):
    """Process one run into the bundle under root. Returns (manifest entry, files entries, report)."""
    log(f"{name}: {src}")
    size, digest = sha256_file(src)
    zst = is_zstd(src)
    decoded = decoded_sha256(src)
    header, names, spans, images = fr.load(src)
    report = fr.build_report(header, names, spans, images)
    del spans
    files = {}
    rdir = root / "runs" / name
    rdir.mkdir(parents=True)
    rel_report = f"runs/{name}/report.json"
    files[rel_report] = dict(zip(("size", "sha256"), write_file(root / rel_report, fr.report_json(report))))
    stored = None
    if not reports_only:
        stored = f"traces/{name}.ftrace.zst"
        (root / "traces").mkdir(exist_ok=True)
        dst = root / stored
        if zst:
            copy_file(src, dst)
        else:
            compress_copy(src, dst)
        back = decoded_sha256(dst)
        if back != decoded:
            raise Failure(f"{name}: the stored copy decodes to sha256 {back}, the source to {decoded}")
        s_size, s_sha = sha256_file(dst)
        files[stored] = {"size": s_size, "sha256": s_sha}
    entry = {"name": name, "source": given, "source_file": os.path.abspath(src), "size": size, "sha256": digest,
             "decoded_sha256": decoded, "header": parse_header(header), "stored": stored, "report": rel_report}
    return entry, files, report


def create(args):
    out = os.path.abspath(args.out)   # also strips a trailing slash, which would put OUT.partial inside OUT
    partial = out + ".partial"
    if os.path.lexists(out):
        raise Failure(f"{out} exists; refusing to touch it")
    if os.path.lexists(partial):
        raise Failure(f"{partial} exists, left by an earlier failed run; remove it first")
    if not os.path.isdir(os.path.dirname(out)):
        raise Failure(f"{out}: the parent directory does not exist")
    runs = []   # (as given, trace file, name)
    for given in args.runs:
        src, name = locate(given)
        runs.append((given, src, name))
    check_names(runs)
    log = (lambda m: None) if args.quiet else say
    root = Path(partial)
    root.mkdir()
    try:
        entries, files, reports = [], {}, {}
        for given, src, name in runs:
            entry, f, report = add_run(root, given, src, name, args.reports_only, log)
            entries.append(entry)
            files.update(f)
            reports[name] = headline_values(report)
        metrics, values, stats = metric_table(reports)
        manifest = {"schema": SCHEMA,
                    "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    "toolkit_git_sha": toolkit_sha(), "reports_only": args.reports_only, "runs": entries}
        agg = {"schema": REPORT_SCHEMA, "runs": [e["name"] for e in entries], "metrics": metrics,
               "values": values, "stats": stats}
        files["report.json"] = dict(zip(("size", "sha256"), write_file(root / "report.json",
                                                                          json.dumps(agg, indent=1) + "\n")))
        files["REPORT.md"] = dict(zip(("size", "sha256"), write_file(
            root / "REPORT.md", report_markdown(manifest, metrics, values, stats))))
        manifest["files"] = dict(sorted(files.items()))
        write_file(root / "manifest.json", json.dumps(manifest, indent=1) + "\n")
        os.rename(partial, out)
    except BaseException:
        # Only what this run made: OUT never existed, so a failure leaves neither OUT nor OUT.partial.
        shutil.rmtree(partial, ignore_errors=True)
        raise
    log(f"wrote {out}")
    print(out)
    return 0


# --- verify and prunable ------------------------------------------------------------------------------

def safe_rel(rel):
    p = PurePosixPath(rel)
    return (isinstance(rel, str) and rel and not p.is_absolute() and "\\" not in rel and ".." not in p.parts
            and str(p) == rel and "." not in p.parts)


def verify_bundle(out):
    """(problems, manifest or None). No problems means the bundle is what its manifest says."""
    root = Path(out)
    try:
        manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return [f"manifest.json: {e.strerror if isinstance(e, OSError) else e}"], None
    if not isinstance(manifest, dict) or manifest.get("schema") != SCHEMA:
        got = manifest.get("schema") if isinstance(manifest, dict) else type(manifest).__name__
        return [f"manifest.json: schema is {got!r}, want {SCHEMA!r}"], None
    problems = []
    files, runs = manifest.get("files"), manifest.get("runs")
    if not isinstance(files, dict) or not isinstance(runs, list) or not runs:
        return ["manifest.json: missing files table or runs"], None
    real_root = os.path.realpath(root)
    for rel, meta in files.items():
        if not safe_rel(rel):
            problems.append(f"{rel!r}: not a path inside the bundle")
            continue
        path = root / rel
        if os.path.commonpath([real_root, os.path.realpath(path)]) != real_root:
            problems.append(f"{rel}: leaves the bundle through a link")
            continue
        if not isinstance(meta, dict) or not isinstance(meta.get("size"), int) or not HASH_RE.match(str(meta.get("sha256"))):
            problems.append(f"{rel}: malformed manifest entry")
            continue
        if not path.is_file():
            problems.append(f"{rel}: missing")
            continue
        size, digest = sha256_file(path)
        if size != meta["size"] or digest != meta["sha256"]:
            problems.append(f"{rel}: changed (size {size}, sha256 {digest[:16]}...; manifest has "
                            f"{meta['size']}, {meta['sha256'][:16]}...)")
    for need in ("report.json", "REPORT.md"):
        if need not in files:
            problems.append(f"{need}: not in the manifest")
    seen = set()
    for r in runs:
        if not isinstance(r, dict) or not isinstance(r.get("name"), str) or not NAME_RE.match(r["name"]):
            problems.append(f"run entry without a valid name: {r!r:.80}")
            continue
        if r["name"].lower() in seen:
            problems.append(f"{r['name']}: duplicate run name")
        seen.add(r["name"].lower())
        if r.get("report") not in files:
            problems.append(f"{r['name']}: its report is not in the manifest")
        if not manifest.get("reports_only") and r.get("stored") not in files:
            problems.append(f"{r['name']}: its stored trace is not in the manifest")
        if not isinstance(r.get("source_file"), str) or not HASH_RE.match(str(r.get("sha256"))) \
                or not isinstance(r.get("size"), int):
            problems.append(f"{r['name']}: source file, size or sha256 missing")
    return problems, manifest


def cmd_verify(args):
    if not os.path.isdir(args.out):
        raise Failure(f"{args.out}: not a bundle directory")
    problems, manifest = verify_bundle(args.out)
    for p in problems:
        say(f"{args.out}: {p}")
    if problems:
        return 1
    known = set(manifest["files"]) | {"manifest.json"}
    for dirpath, _, names in os.walk(args.out):
        for n in names:
            rel = os.path.relpath(os.path.join(dirpath, n), args.out).replace(os.sep, "/")
            if rel not in known:
                say(f"{args.out}: warning: {rel} is not in the manifest")
    if not args.quiet:
        say(f"{args.out}: ok, {len(manifest['runs'])} run(s), {len(manifest['files'])} file(s)")
    return 0


def unsafe_to_prune(run, real_root):
    """Why the run's source file must be kept, or None when it is the file the manifest describes."""
    path = run["source_file"]
    try:
        if os.path.commonpath([real_root, os.path.realpath(path)]) == real_root:
            return "it is inside the bundle"
        if not os.path.isfile(path):
            return "it is missing"
        if sha256_file(path) != (run["size"], run["sha256"]):
            return "it no longer matches the manifest"
    except OSError as e:
        return f"it cannot be read ({e.strerror})"
    return None


def cmd_prunable(args):
    if not os.path.isdir(args.out):
        raise Failure(f"{args.out}: not a bundle directory")
    problems, manifest = verify_bundle(args.out)
    if problems:
        for p in problems:
            say(f"{args.out}: {p}")
        say("the bundle does not verify; nothing is safe to delete")
        return 1
    if manifest.get("reports_only") and not args.allow_reports_only:
        say(f"{args.out} holds reports only, not the traces: nothing is listed "
            "(--allow-reports-only if the reports are all you want to keep)")
        return 0
    real_root = os.path.realpath(args.out)
    sep = b"\0" if args.null else b"\n"
    printed = set()
    for r in manifest["runs"]:
        path = r["source_file"]
        why = unsafe_to_prune(r, real_root)
        if why is None and not args.null and "\n" in path:
            why = "the path contains a newline (use -0)"
        if why:
            say(f"{r['name']}: {path} not listed: {why}")
        elif path not in printed:
            printed.add(path)
            sys.stdout.buffer.write(os.fsencode(path) + sep)
    sys.stdout.buffer.flush()
    return 0


EPILOG = """\
examples:
  ftrace_bundle.py create /tmp/before-after.bundle /tmp/run-a /tmp/run-b/ftrace.txt.zst
  ftrace_bundle.py create --reports-only /tmp/reports.bundle run1.txt run2.txt
  ftrace_bundle.py verify /tmp/before-after.bundle
  ftrace_bundle.py prunable /tmp/before-after.bundle -0 | xargs -0 trash     # you delete, the tool never does

exit status: 0 done; 1 the bundle is damaged (verify, prunable); 2 usage error or bad input.
standard output carries data only; progress and errors go to standard error.
create never overwrites: OUT must not exist, and a failure leaves neither OUT nor OUT.partial.
bundle layout and the headline metrics: the top of this file and docs/TOOLS.md.
"""


def build_parser():
    ap = argparse.ArgumentParser(
        prog="ftrace_bundle.py", description="Bundle PS3_FTRACE traces of several runs, report on them together "
        "and list the loose traces that are safe to delete.", epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True, metavar="{create,verify,prunable}")
    c = sub.add_parser("create", help="build a bundle from runs",
                       description="Bundle RUN... into OUT: traces, per-run reports, a joint report and a manifest.",
                       epilog="example: ftrace_bundle.py create /tmp/b.bundle /tmp/run-a /tmp/run-b",
                       formatter_class=argparse.RawDescriptionHelpFormatter)
    c.add_argument("out", metavar="OUT", help="bundle directory to create; must not exist")
    c.add_argument("runs", metavar="RUN", nargs="+",
                   help="a trace file (text or zstd) or a directory holding exactly one of ftrace.txt, ftrace.txt.zst")
    c.add_argument("--reports-only", action="store_true", help="do not store the traces, only the reports")
    c.add_argument("--quiet", action="store_true", help="no progress on standard error")
    v = sub.add_parser("verify", help="check the schema and every stored file's hash",
                       description="Check OUT's manifest schema and the size and sha256 of every file it lists.",
                       epilog="example: ftrace_bundle.py verify /tmp/b.bundle",
                       formatter_class=argparse.RawDescriptionHelpFormatter)
    v.add_argument("out", metavar="OUT", help="a bundle directory")
    v.add_argument("--quiet", action="store_true", help="no success line on standard error")
    p = sub.add_parser("prunable", help="print the source traces that are safe to delete",
                       description="Print the source files OUT covers whose sha256 still equals the manifest's, "
                       "if OUT verifies and holds the stored traces. Deletes nothing.",
                       epilog="example: ftrace_bundle.py prunable /tmp/b.bundle -0 | xargs -0 trash",
                       formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("out", metavar="OUT", help="a bundle directory")
    p.add_argument("-0", dest="null", action="store_true", help="end each path with NUL, for xargs -0")
    p.add_argument("--allow-reports-only", action="store_true",
                   help="also list the sources of a --reports-only bundle (its traces are not stored)")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if args.cmd == "create":
            return create(args)
        if args.cmd == "verify":
            return cmd_verify(args)
        return cmd_prunable(args)
    except (Failure, fr.BadInput) as e:
        say(str(e))
        return 2
    except OSError as e:
        say(f"{e.filename or ''}: {e.strerror}".lstrip(": "))
        return 2
    except KeyboardInterrupt:
        say("interrupted")
        return 130


if __name__ == "__main__":
    sys.exit(main())
