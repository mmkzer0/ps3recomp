#!/usr/bin/env python3
"""Run a corpus of SPU job fixtures through the offline differential runner.

    tools/spu_xdiff_corpus.py --runner PATH [--tier interp|registered] [--budget N]
                              [--timeout S] [--jobs N] [--json] [--keep-going] CORPUS_DIR

A corpus is a directory of fixtures (docs/SPU_XDIFF.md section 13): jobs recorded by
`spu.xdiff_capture` and replayed by the supervisor (`spu_job_replay`). The driver runs the
supervisor once per fixture and sums its verdicts by image fingerprint and by reason, so
one invocation says which lifted bodies diverge from the interpreter, where, and how much
of the corpus the run could actually judge.

A fixture is a file whose name ends in `.spujob`, or any file whose first four bytes are
the zstd frame magic `28 B5 2F FD`: a corpus is compressed as a whole (section 7.2), and a
compressed fixture is recognised by its bytes, not by its name. A compressed fixture is
decoded into a private temporary directory and the runner is handed that file; the file
and the directory are removed when the fixture is done, also when the run fails. Decoding
uses, in order, the standard library's `compression.zstd` (Python 3.14+) or a `zstd -dc`
child; with neither, the compressed fixtures are driver errors and the plain ones still
run. Fixtures are reported in sorted path order, so two runs without a driver error produce
byte-identical reports whatever `--jobs` is.

Each fixture is run as `RUNNER [--tier T] [--budget N] [--timeout S] --json FIXTURE`. The
supervisor's standard output must hold one JSON object whose schema is
`ps3recomp.spujob-verdict/1` and whose `exit_code` is the status the process exited with;
`verdict`, `reason`, `class` and `fixture.image_fingerprint` are the fields the sums use. A
verdict whose fixture has no readable fingerprint is counted under `<unattributed>`, and a
fingerprint written as a number is shown in the toolkit's 16-hex form. A runner that cannot
be started, crashes, is killed by the driver's deadline, prints nothing or something that
is not one valid object, or reports an `exit_code` the process did not exit with, becomes a
`driver-error` row: counted, named with the tail of its standard error, never dropped. A
`differ` with class `latent` -- the end registers or local store under the JobQueue2
profile (section 8.4) -- is a differ like a `visible` one.

`--timeout S` is forwarded to the supervisor, which reports its own timeout as `void`; the
driver additionally kills a runner still alive 30 seconds after S, so a hung supervisor
cannot hang the corpus. Without `--keep-going` the driver stops handing out fixtures after
the first driver error and counts the rest as not run: a supervisor that cannot produce
one verdict will usually not produce the next either. With it every fixture runs. Under
`--jobs` above 1, the fixtures already running when that first error arrives still finish
and are reported.

Text report:
  corpus <path>: <n> fixtures, <n> images
    equal .., differ .. (visible .., latent ..), void .., malformed .., not-found ..,
    driver errors ..
  per image          the same counts for one image fingerprint, its reasons, its driver
                     errors
  per reason         the reasons over the whole corpus
  first differ per image
                     the first differ of each image in path order, with its fixture path,
                     effect index, reason and where

`--json` prints one object instead (schema `ps3recomp.spujob-corpus/1`):
  schema, tool, version, corpus, config {runner, tier, budget, timeout}
  images    one object per image fingerprint: `fingerprint` (null when the row has no
            readable identity), `fixtures`, `equal`, `differ {visible, latent}`, `void`,
            `malformed`, `not-found`, `driver_errors`, `reasons`, `first_differ`
            {path, effect_index, reason, where} or null
  fixtures  `path` relative to the corpus, `verdict`, `reason`, `class`, `image`; a driver
            error has null reason, class and image, and `error {cause, detail}` instead
  totals    the same count fields over the corpus, `reasons`, `not_run` and `exit`
The driver's own concurrency is not part of the report: it cannot change a verdict.

Exit status: 0 every fixture equal; 4 a differ, visible or latent; 2 a malformed verdict
or a driver error when no differ was seen; 5 a void when neither; 3 no verdict and no
error (an empty corpus, or only not-found); 1 usage. A differ outranks a harness failure
because the divergence is the answer the corpus was recorded for, and the failure is still
named in the report. A missing runner is usage; an empty corpus is not: it reports zero
fixtures and exits 3.
"""
import argparse
import collections
import dataclasses
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading

SCHEMA = "ps3recomp.spujob-corpus/1"
VERDICT_SCHEMA = "ps3recomp.spujob-verdict/1"
TOOL = "spu_xdiff_corpus"
VERSION = 1
FIXTURE_SUFFIX = ".spujob"
# A driver error is not a verdict: section 8.3's reason ids stay verdict-only, so the row
# carries a driver-side cause and the stderr tail instead of a reason.
DRIVER_ERROR = "driver-error"
VERDICTS = ("equal", "differ", "void", "malformed", "not-found")
# Section 8.4: a visible differ changes the observable effects; a latent one is the end
# registers or local store under the JobQueue2 profile. Both count as differ.
CLASSES = ("visible", "latent")
NO_CLASS = "none"

# Section 8.3's codes. The corpus keeps the same values; usage is 1, and a malformed
# verdict would otherwise be indistinguishable from argparse's default exit.
EXIT_USAGE = 1
EXIT_MALFORMED = 2
EXIT_NOT_FOUND = 3
EXIT_DIFFER = 4
EXIT_VOID = 5

ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
ZSTD_COMMAND = "zstd"
# Tried in this order, as tools/ftrace_report.py does. A test may narrow the tuple.
ZSTD_BACKENDS = ("stdlib", "subprocess")
ZSTD_NO_BACKEND = ("a zstd fixture needs a decoder: Python 3.14 or newer (compression.zstd) "
                   f"or the `{ZSTD_COMMAND}` command on PATH; neither is available")

# The supervisor reports its own timeout at S; this is the time it gets to print the
# verdict after the subject's child was killed, before the driver calls the run a crash.
TIMEOUT_GRACE = 30.0
# A supervisor's traceback is the clue to a driver error; its whole log is not. Kept in
# the JSON row and printed under the fixture.
STDERR_TAIL_BYTES = 2000
STDERR_TAIL_LINES = 20


class DriverError(Exception):
    """A fixture that did not yield a trustworthy verdict row."""

    def __init__(self, cause, detail):
        super().__init__(detail)
        self.cause = cause
        self.detail = detail


@dataclasses.dataclass
class Row:
    """One fixture's outcome: a runner verdict, or a driver diagnostic."""

    path: str
    verdict: str
    reason: str | None = None
    kind: str | None = None
    image: str | None = None
    error: dict | None = None
    first: dict | None = None

    def report(self):
        """The row as the JSON report carries it."""
        out = {"path": self.path, "verdict": self.verdict, "reason": self.reason,
               "class": self.kind, "image": self.image}
        if self.error is not None:
            out["error"] = self.error
        return out


@dataclasses.dataclass
class Run:
    """What every fixture invocation needs, and nothing that changes between them."""

    runner: str
    tier: str | None
    budget: int | None
    timeout: float | None
    deadline: float | None
    jobs: int
    keep_going: bool
    tempdir: str | None


class Parser(argparse.ArgumentParser):
    """Usage errors exit 1 (this tool's contract), not argparse's 2, which means malformed."""

    def error(self, message):
        self.print_usage(sys.stderr)
        self.exit(EXIT_USAGE, f"{self.prog}: error: {message}\n")


def positive_int(text):
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r} is not an integer") from None
    if value < 1:
        raise argparse.ArgumentTypeError(f"{text!r} must be at least 1")
    return value


def positive_float(text):
    try:
        value = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r} is not a number") from None
    if not value > 0:
        raise argparse.ArgumentTypeError(f"{text!r} must be greater than 0")
    return value


def zstd_backend():
    """(name, handle) of the first available decoder in ZSTD_BACKENDS, or None.

    "stdlib" carries the compression.zstd module, "subprocess" the path of the zstd command.
    """
    for name in ZSTD_BACKENDS:
        if name == "stdlib":
            try:
                from compression import zstd
            except ImportError:
                continue
            return name, zstd
        if name == "subprocess":
            exe = shutil.which(ZSTD_COMMAND)
            if exe:
                return name, exe
    return None


def tail(text):
    """The last non-empty lines of a diagnostic, at most STDERR_TAIL_BYTES characters."""
    kept = [line.rstrip() for line in text.splitlines() if line.strip()]
    joined = "\n".join(kept[-STDERR_TAIL_LINES:])
    return joined[-STDERR_TAIL_BYTES:]


def stderr_note(raw):
    """`; stderr: ...` for a diagnostic, empty when the runner wrote no standard error."""
    if not raw:
        return ""
    text = raw.decode("utf-8", "replace") if isinstance(raw, bytes) else raw
    kept = tail(text)
    return f"; stderr: {kept}" if kept else ""


def zstd_magic(path):
    """Whether the file starts with the zstd frame magic. An unreadable file is not one."""
    try:
        with open(path, "rb") as handle:
            return handle.read(len(ZSTD_MAGIC)) == ZSTD_MAGIC
    except OSError:
        return False


def discover(corpus):
    """(relative path, absolute path, compressed) for every fixture, sorted by path.

    The name decides whether an uncompressed file is a fixture; the magic decides whether
    it is compressed. A corpus is compressed as a whole (section 7.2), so a compressed
    fixture may carry any name and the two rules are independent.
    """
    found = []
    for dirpath, dirnames, filenames in os.walk(corpus):
        dirnames.sort()
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            if not os.path.isfile(path):
                continue
            compressed = zstd_magic(path)
            if name.endswith(FIXTURE_SUFFIX) or compressed:
                relative = os.path.relpath(path, corpus).replace(os.sep, "/")
                found.append((relative, path, compressed))
    found.sort(key=lambda item: item[0])
    return found


def decode_zstd(source, destination):
    """Decode the zstd stream in source into destination; a failure names the cause."""
    backend = zstd_backend()
    if backend is None:
        raise DriverError("no-decoder", ZSTD_NO_BACKEND)
    name, handle = backend
    if name == "stdlib":
        error = handle.ZstdError
        try:
            with open(source, "rb") as raw, handle.ZstdFile(raw) as stream, open(destination, "wb") as out:
                shutil.copyfileobj(stream, out, 1 << 20)
        except (OSError, EOFError, error) as exc:
            raise DriverError("decode", f"truncated or corrupt zstd stream ({exc})") from None
        return
    try:
        with open(destination, "wb") as out:
            proc = subprocess.run([handle, "-dc", "--", source], stdin=subprocess.DEVNULL,
                                  stdout=out, stderr=subprocess.PIPE, check=False)
    except OSError as exc:
        raise DriverError("decode", f"cannot run {handle}: {exc.strerror}") from None
    if proc.returncode != 0:
        why = tail(proc.stderr.decode("utf-8", "replace"))
        raise DriverError("decode", f"truncated or corrupt zstd stream "
                                    f"(zstd exited {proc.returncode}: {why})")


def fingerprint(value):
    """The image fingerprint a verdict names, in the toolkit's 16-hex form, or None.

    A malformed fixture may have no readable identity, and a supervisor may write the u64
    as a JSON number: neither may cost the verdict its row. An absent identity is reported
    under `<unattributed>`, not turned into a driver error.
    """
    if isinstance(value, str) and value:
        return value
    if isinstance(value, int) and not isinstance(value, bool) and 0 <= value < 1 << 64:
        return f"{value:016X}"
    return None


def first_difference(value):
    """The first-difference fields the report shows.

    A differ without a readable `first` record keeps its counts and shows `-` for the
    missing part: one absent detail must not hide a divergence the corpus already found.
    """
    if not isinstance(value, dict):
        return {"effect_index": None, "where": None}
    index = value.get("effect_index")
    if isinstance(index, bool) or not isinstance(index, int):
        index = None
    return {"effect_index": index, "where": value.get("where")}


def parse_verdict(proc, stdout):
    """The verdict row of one runner run, or DriverError saying why the output is not one."""
    stderr = stderr_note(proc.stderr)

    def fail(cause, message):
        raise DriverError(cause, message + stderr)

    if not stdout.strip():
        fail("no-output", f"the runner exited {proc.returncode} with nothing on stdout")
    try:
        doc = json.loads(stdout)
    except ValueError as exc:
        fail("bad-json", f"the runner printed no JSON object ({exc}); "
                         f"stdout starts {stdout.strip()[:120]!r}")
    if not isinstance(doc, dict):
        fail("bad-json", f"stdout is JSON but not an object ({type(doc).__name__})")
    if doc.get("schema") != VERDICT_SCHEMA:
        fail("bad-schema", f"schema {doc.get('schema')!r}, want {VERDICT_SCHEMA!r}")
    verdict = doc.get("verdict")
    if verdict not in VERDICTS:
        fail("bad-schema", f"verdict {verdict!r} is not one of {', '.join(VERDICTS)}")
    reason = doc.get("reason")
    if not isinstance(reason, str) or not reason:
        fail("bad-schema", f"reason {reason!r} is not a non-empty string")
    exit_code = doc.get("exit_code")
    if isinstance(exit_code, bool) or not isinstance(exit_code, int):
        fail("bad-schema", f"exit_code {exit_code!r} is not an integer")
    if exit_code != proc.returncode:
        fail("exit-mismatch", f"the object reports exit_code {exit_code}, "
                              f"the process exited {proc.returncode}")
    fixture = doc.get("fixture")
    identity = fixture.get("image_fingerprint") if isinstance(fixture, dict) else None
    image = fingerprint(identity)
    kind = doc.get("class")
    if verdict == "differ":
        if kind not in CLASSES:
            fail("bad-schema", f"a differ needs class {' or '.join(CLASSES)}, got {kind!r}")
    elif kind is not None and kind != NO_CLASS:
        fail("bad-schema", f"a {verdict} verdict carries class {kind!r}, "
                           f"want {NO_CLASS!r} or none")
    else:
        kind = NO_CLASS
    first = first_difference(doc.get("first")) if verdict == "differ" else None
    return Row(path="", verdict=verdict, reason=reason, kind=kind, image=image, first=first)


def run_runner(run, path):
    """Run the supervisor once and return its verdict row; raise DriverError otherwise."""
    command = [run.runner]
    if run.tier is not None:
        command += ["--tier", run.tier]
    if run.budget is not None:
        command += ["--budget", str(run.budget)]
    if run.timeout is not None:
        command += ["--timeout", f"{run.timeout:g}"]
    command += ["--json", path]
    try:
        proc = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                              timeout=run.deadline, check=False)
    except subprocess.TimeoutExpired as exc:
        note = stderr_note(exc.stderr)
        raise DriverError("timeout", f"the runner was still alive {run.deadline:g} s "
                                     f"after the start{note}") from None
    except OSError as exc:
        raise DriverError("spawn", f"cannot run {run.runner}: {exc.strerror}") from None
    if proc.returncode < 0:
        raise DriverError("signal", f"the runner was killed by signal {-proc.returncode}"
                                    f"{stderr_note(proc.stderr)}")
    return parse_verdict(proc, proc.stdout.decode("utf-8", "replace"))


def run_fixture(run, relative, path, compressed):
    """One fixture: decode it when it is compressed, run the supervisor, clean up."""
    target = path
    temporary = None
    try:
        if compressed:
            if run.tempdir is None:
                raise DriverError("decode", "no temporary directory for the decoded fixture")
            stem = os.path.splitext(os.path.basename(relative))[0] or "fixture"
            # mkstemp makes the decoded file 0600; the directory is mkdtemp's 0700.
            handle, temporary = tempfile.mkstemp(prefix=stem + "-", suffix=FIXTURE_SUFFIX,
                                                 dir=run.tempdir)
            os.close(handle)
            decode_zstd(path, temporary)
            target = temporary
        row = run_runner(run, target)
        row.path = relative
        return row
    finally:
        if temporary is not None:
            try:
                os.unlink(temporary)
            except OSError:
                pass


def evaluate(run, candidates):
    """(rows sorted by path, number of fixtures not run) for a corpus."""
    rows = []
    pending = collections.deque(candidates)
    lock = threading.Lock()
    stop = threading.Event()

    def worker():
        while True:
            with lock:
                if stop.is_set() or not pending:
                    return
                item = pending.popleft()
            try:
                row = run_fixture(run, *item)
            except DriverError as exc:
                row = Row(path=item[0], verdict=DRIVER_ERROR,
                          error={"cause": exc.cause, "detail": exc.detail})
            except Exception as exc:  # a driver bug is a row too, never a lost fixture  # noqa: BLE001
                row = Row(path=item[0], verdict=DRIVER_ERROR,
                          error={"cause": "driver", "detail": f"{type(exc).__name__}: {exc}"})
            with lock:
                rows.append(row)
                if row.verdict == DRIVER_ERROR and not run.keep_going:
                    stop.set()

    threads = [threading.Thread(target=worker, name=f"fixture-{index}", daemon=True)
               for index in range(min(run.jobs, len(candidates)))]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    rows.sort(key=lambda row: row.path)
    return rows, len(pending)


def empty_counts():
    return {"fixtures": 0, "equal": 0, "differ": {"visible": 0, "latent": 0},
            "void": 0, "malformed": 0, "not-found": 0, "driver_errors": 0, "reasons": {}}


def count_row(counts, row):
    counts["fixtures"] += 1
    if row.verdict == DRIVER_ERROR:
        counts["driver_errors"] += 1
        return
    if row.verdict == "differ":
        # There is no separate differ total: the class split is the count, and the total
        # is their sum (a latent differ still counts as a differ).
        counts["differ"][row.kind] += 1
    else:
        counts[row.verdict] += 1
    reasons = counts["reasons"]
    reasons[row.reason] = reasons.get(row.reason, 0) + 1


def ordered_counts(counts):
    """The counts with a fixed key order, for byte-identical reports."""
    return {"fixtures": counts["fixtures"], "equal": counts["equal"],
            "differ": {"visible": counts["differ"]["visible"],
                       "latent": counts["differ"]["latent"]},
            "void": counts["void"], "malformed": counts["malformed"],
            "not-found": counts["not-found"], "driver_errors": counts["driver_errors"],
            "reasons": {name: counts["reasons"][name] for name in sorted(counts["reasons"])}}


def exit_status(totals):
    """Section 8.3's codes with the corpus precedence: 4 over 2 over 5 over 3 over 0.

    A differ is the answer the corpus was recorded for, so it outranks a harness failure;
    a malformed verdict or a driver error says there is a defect to fix, which outranks a
    void's undecided job; a void outranks not-found, because it at least ran.
    """
    if totals["differ"]["visible"] or totals["differ"]["latent"]:
        return EXIT_DIFFER
    if totals["malformed"] or totals["driver_errors"]:
        return EXIT_MALFORMED
    if totals["void"]:
        return EXIT_VOID
    if totals["not-found"] or totals["fixtures"] == 0:
        return EXIT_NOT_FOUND
    return 0


def summarize(run, rows, not_run, corpus):
    """The `ps3recomp.spujob-corpus/1` object for the rows of one run."""
    totals = empty_counts()
    per_image = {}
    firsts = {}
    for row in rows:
        count_row(totals, row)
        # A driver error is not attributed to an image: the object it printed was not
        # trusted. A verdict whose fixture has no readable identity lands here too, so a
        # malformed fixture is still counted instead of becoming a second driver error.
        image = None if row.verdict == DRIVER_ERROR else row.image
        count_row(per_image.setdefault(image, empty_counts()), row)
        if row.verdict == "differ" and image not in firsts:
            firsts[image] = {"path": row.path,
                             "effect_index": row.first["effect_index"],
                             "reason": row.reason,
                             "where": row.first["where"]}
    totals_doc = ordered_counts(totals)
    totals_doc["fixtures"] = totals["fixtures"] + not_run
    totals_doc["not_run"] = not_run
    totals_doc["exit"] = exit_status(totals_doc)
    images = []
    for image in sorted(per_image, key=lambda name: (name is None, name or "")):
        entry = {"fingerprint": image}
        entry.update(ordered_counts(per_image[image]))
        entry["first_differ"] = firsts.get(image)
        images.append(entry)
    return {"schema": SCHEMA, "tool": TOOL, "version": VERSION,
            "corpus": corpus,
            "config": {"runner": run.runner, "tier": run.tier, "budget": run.budget,
                       "timeout": run.timeout},
            "images": images,
            "fixtures": [row.report() for row in rows],
            "totals": totals_doc}


def counts_text(counts):
    differ = counts["differ"]["visible"] + counts["differ"]["latent"]
    return (f"fixtures {counts['fixtures']}  equal {counts['equal']}  "
            f"differ {differ} (visible {counts['differ']['visible']}, "
            f"latent {counts['differ']['latent']})  void {counts['void']}  "
            f"malformed {counts['malformed']}  not-found {counts['not-found']}  "
            f"driver errors {counts['driver_errors']}")


def reasons_text(reasons):
    ordered = sorted(reasons.items(), key=lambda item: (-item[1], item[0]))
    return ", ".join(f"{name} {count}" for name, count in ordered)


def shown(value):
    """A JSON value as one text-report field; `-` for a detail the runner did not give."""
    if value is None:
        return "-"
    if isinstance(value, str):
        return value
    if isinstance(value, bool) or not isinstance(value, int):
        return json.dumps(value, sort_keys=True, separators=(",", ":"))
    return str(value)


def report_text(doc, rows, not_run):
    totals = doc["totals"]
    images = [entry for entry in doc["images"] if entry["fingerprint"] is not None]
    fixtures_word = "fixture" if totals["fixtures"] == 1 else "fixtures"
    images_word = "image" if len(images) == 1 else "images"
    header = (f"corpus {doc['corpus']}: {totals['fixtures']} {fixtures_word}, "
              f"{len(images)} {images_word}")
    out = [header, "  " + counts_text(totals)]
    if not_run:
        out.append(f"  not run {not_run}: stopped after the first driver error "
                   f"(--keep-going runs every fixture)")
    if not totals["fixtures"]:
        out.append("  no fixture found: a fixture is a *.spujob file or a file that starts "
                   "with the zstd frame magic 28 B5 2F FD")
    out += ["", "per image"]
    if not doc["images"]:
        out.append("  (none)")
    for entry in doc["images"]:
        out.append(f"  {entry['fingerprint'] or '<unattributed>'}  " + counts_text(entry))
        if entry["reasons"]:
            out.append("    reasons " + reasons_text(entry["reasons"]))
        if entry["fingerprint"] is not None:
            continue
        for row in rows:
            if row.verdict != DRIVER_ERROR:
                continue
            lines = row.error["detail"].splitlines() or [""]
            out.append(f"    driver error {row.path}: {row.error['cause']}: {lines[0]}")
            out.extend("      " + line for line in lines[1:])
    out += ["", "per reason"]
    out.append("  " + reasons_text(totals["reasons"]) if totals["reasons"] else "  (none)")
    out += ["", "first differ per image"]
    differs = [(entry["fingerprint"], entry["first_differ"]) for entry in doc["images"]
               if entry["first_differ"]]
    if not differs:
        out.append("  (none)")
    for fingerprint, first in differs:
        out.append(f"  {fingerprint or '<unattributed>'}  {first['path']}  "
                   f"effect {shown(first['effect_index'])}  {first['reason']}  "
                   f"at {shown(first['where'])}")
    return "\n".join(out)


def parse_args(argv):
    parser = Parser(description=__doc__,
                    formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("corpus", metavar="CORPUS_DIR",
                        help="directory searched recursively for fixtures")
    parser.add_argument("--runner", required=True, metavar="PATH",
                        help="the supervisor run once per fixture (spu_job_replay)")
    parser.add_argument("--tier", choices=("interp", "registered"),
                        help="subject tier, forwarded to the runner (its own default when absent)")
    parser.add_argument("--budget", type=positive_int, metavar="N",
                        help="instructions or backward transfers per run, forwarded to the runner")
    parser.add_argument("--timeout", type=positive_float, metavar="S",
                        help="seconds per run, forwarded to the runner; the driver kills a "
                             f"runner still alive {TIMEOUT_GRACE:g} s later")
    parser.add_argument("--jobs", type=positive_int, default=1, metavar="N",
                        help="fixtures run in parallel (default 1)")
    parser.add_argument("--json", action="store_true",
                        help=f"print one {SCHEMA} object instead of the text report")
    parser.add_argument("--keep-going", action="store_true",
                        help="run every fixture after a driver error instead of stopping")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    found = shutil.which(args.runner)
    if found is None:
        print(f"{TOOL}: --runner {args.runner}: no such executable file", file=sys.stderr)
        return EXIT_USAGE
    runner = os.path.abspath(found)
    corpus = os.path.abspath(args.corpus)
    if not os.path.isdir(corpus):
        print(f"{TOOL}: corpus {args.corpus}: no such directory", file=sys.stderr)
        return EXIT_USAGE
    candidates = discover(corpus)
    tempdir = None
    try:
        if any(compressed for _, _, compressed in candidates):
            # One mkdtemp (0700) for every decoded fixture; each mkstemp is 0600.
            tempdir = tempfile.mkdtemp(prefix="spu_xdiff_corpus.")
        run = Run(runner=runner, tier=args.tier, budget=args.budget, timeout=args.timeout,
                  deadline=args.timeout + TIMEOUT_GRACE if args.timeout is not None else None,
                  jobs=args.jobs, keep_going=args.keep_going, tempdir=tempdir)
        rows, not_run = evaluate(run, candidates)
    finally:
        if tempdir is not None:
            shutil.rmtree(tempdir, ignore_errors=True)
    doc = summarize(run, rows, not_run, corpus)
    if args.json:
        json.dump(doc, sys.stdout, indent=1)
        sys.stdout.write("\n")
    else:
        sys.stdout.write(report_text(doc, rows, not_run) + "\n")
    return doc["totals"]["exit"]


if __name__ == "__main__":
    sys.exit(main())
