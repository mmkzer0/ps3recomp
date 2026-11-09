#!/usr/bin/env python3
"""PS3_FTRACE=<start>:<seconds>:<path>.zst writes a zstd stream through a `zstd` child.

Runs tests/test_ftrace_zst.c once per scenario (the recorder reads its environment once and dumps at
exit) and checks what the process leaves behind, and decodes the output with tools/ftrace_report.py:

  a .zst recording decodes to the expected spans; a stale <path>.partial is replaced, none is left;
  quick_exit and a bulk recording (more than a pipe and the stdio buffer hold) end the same way;
  a text path is untouched by all of this and needs no zstd;
  zstd not on PATH: recording is off at init, with a named message, and no file is created;
  a zstd child that fails (exit status, killed, dead before the write ends) is a named [ftrace] error,
    no file at <path>, no <path>.partial, and the process still exits normally (no SIGPIPE death, and
    SIGPIPE is back to what the process had);
  an output path that cannot be created is named, and nothing is left;
  the child sees no descriptor beyond 0, 1 and 2 (where the platform can close them).

Usage: run_ftrace_zst.py <test_ftrace_zst>. Scenarios that need the real zstd command print SKIP
when it is not on PATH.
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import ftrace_report as fr

ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


def fake_zstd(directory, body):
    """A `zstd` stand-in: a shell script that behaves as `body` says."""
    d = Path(directory)
    d.mkdir(exist_ok=True)
    p = d / "zstd"
    p.write_text("#!/bin/sh\n" + body + "\n")
    p.chmod(p.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return str(d) + ":/usr/bin:/bin"


def run(exe, mode, out, path_env, cap=None):
    env = {"PS3_FTRACE": f"0:100000:{out}"}
    if cap:
        env["PS3_FTRACE_CAP"] = str(cap)
    if path_env is not None:
        env["PATH"] = path_env
    q = subprocess.run([exe, mode], env=env, capture_output=True, text=True, timeout=120, check=False)
    return q.returncode, q.stdout, q.stderr


def leftovers(out):
    return [p for p in (out, out + ".partial") if os.path.exists(p)]


def expect_spans(path, extra=0):
    header, names, spans, _ = fr.load(str(path))
    got = [(s[0], s[1], s[2], s[3], round(s[4], 6), round(s[5], 6)) for s in spans[:5]]
    want = [(9, 1, 0, 0, 0.0, 0.0), (15, 1, 0x1573DC3F, 0x1000, 5.0, 9.0), (9, 1, 0, 0, 20.0, 20.0),
            (9, 1, 0, 0, 40.0, 40.0), (9, 1, 0, 0, 60.0, 60.0)]
    return (header.startswith("# ps3_ftrace ") and f"spans={5 + extra} " in header + " " and names == {1: "main"}
            and got == want and len(spans) == 5 + extra), (header, names, got, len(spans))


def main(argv):
    if len(argv) != 2:
        print("usage: run_ftrace_zst.py <test_ftrace_zst>", file=sys.stderr)
        return 2
    exe = argv[1]
    real_zstd = shutil.which("zstd")
    real_path = str(Path(real_zstd).parent) if real_zstd else None

    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        empty = t / "empty-bin"
        empty.mkdir()

        # A text path is what it always was, and needs no zstd on PATH.
        out = str(t / "plain.txt")
        rc, so, err = run(exe, "record", out, str(empty))
        ok, info = expect_spans(out) if os.path.exists(out) else (False, "no file")
        check(rc == 0 and ok and not os.path.exists(out + ".partial"), f"text path: records as text ({rc} {err!r} {info})")
        check(Path(out).read_bytes()[:4] != ZSTD_MAGIC, "text path: the file is text, not zstd")

        # zstd not reachable: named at init, recording off, nothing created.
        for label, penv in (("an empty PATH dir", str(empty)), ("PATH unset", None)):
            out = str(t / "missing.zst")
            rc, so, err = run(exe, "record", out, penv)
            check(rc == 3, f"no zstd ({label}): recording is off, exit {rc}: {err!r}")
            check("[ftrace] .zst output needs the zstd command on PATH" in err and "recording is off" in err,
                  f"no zstd ({label}): the init error is named ({err!r})")
            check("[ftrace] recording " not in err and not leftovers(out), f"no zstd ({label}): no recording, no file")

        # A child that fails: named status, no file at the path, no .partial, the process lives.
        failing = (
            ("exits 3 after reading everything", "cat >/dev/null\nexit 3", "many", "zstd exited with status 3"),
            ("exits 4 without reading", "exit 4", "record", "zstd exited with status 4"),
            ("closes its input and exits 4 (a write meets a dead pipe)", "exec 0<&-\nexit 4", "many",
             "zstd exited with status 4"),
            ("is killed", "kill -9 $$", "record", "zstd was killed by signal 9"),
        )
        for label, body, mode, want in failing:
            out = str(t / "failing.zst")
            Path(out + ".partial").write_text("stale")
            rc, so, err = run(exe, mode, out, fake_zstd(t / "fake-bin", body), cap=400000)
            check(rc == 0, f"child {label}: the process exits normally, not by SIGPIPE or the SIGPIPE check ({rc}: {err[-200:]!r})")
            check(want in err and f"{out} not written" in err, f"child {label}: the status is named ({err[-300:]!r})")
            check("wrote " not in err and not leftovers(out), f"child {label}: no file at the path, no .partial")

        # An output path that cannot be created: named, nothing left.
        out = str(t / "no-such-dir" / "out.zst")
        rc, so, err = run(exe, "record", out, fake_zstd(t / "fake-bin", "cat"))
        check(rc == 0 and f"[ftrace] cannot write {out}.partial" in err and "wrote " not in err
              and not leftovers(out), f"unwritable output: named, nothing left ({rc}: {err!r})")

        # The child sees only 0, 1 and 2. A descriptor the process left inheritable would otherwise be there.
        rc, closes, err = run(exe, "closes-fds", str(t / "x.zst"), None)
        if closes.strip() == "1":
            out = str(t / "leak.zst")
            body = 'for i in 3 4 5 6 7 8 9; do if [ -e /dev/fd/$i ]; then echo "fd $i leaked" >&2; exit 9; fi; done\ncat'
            rc, so, err = run(exe, "leak", out, fake_zstd(t / "fake-bin", body))
            leaked_fd = int(so.split()[0]) if so.split() else -1
            check(3 <= leaked_fd <= 9, f"fd check: the test's own descriptor is in the probed range ({so!r})")
            check(rc == 0 and "wrote 5 spans" in err and "leaked" not in err and os.path.exists(out),
                  f"fd check: the child had no descriptor beyond 0-2 ({rc}: {err!r})")
        else:
            print("SKIP: fd check: this platform's spawn cannot close the other descriptors")

        if real_zstd is None:
            print("SKIP: .zst recording: no zstd command on PATH")
        else:
            for mode, extra, cap in (("record", 0, None), ("quick", 0, None), ("many", 300000, 400000)):
                out = str(t / f"{mode}.zst")
                Path(out + ".partial").write_text("stale partial from an earlier run")
                rc, so, err = run(exe, mode, out, real_path, cap=cap)
                check(rc == 0, f".zst {mode}: exit {rc}: {err[-300:]!r}")
                check(f"wrote {5 + extra} spans (0 dropped) to {out}" in err, f".zst {mode}: the dump says so ({err[-200:]!r})")
                check(os.path.exists(out) and not os.path.exists(out + ".partial"),
                      f".zst {mode}: the file is there and the .partial is gone")
                if os.path.exists(out):
                    check(Path(out).read_bytes()[:4] == ZSTD_MAGIC, f".zst {mode}: a zstd frame")
                    ok, info = expect_spans(out, extra)
                    check(ok, f".zst {mode}: ftrace_report reads the expected spans ({info})")

    print("ftrace_zst: %s" % ("ok" if not failures else f"{failures} failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
