#!/usr/bin/env python3
"""SPU diagnostic state shared by JQ2's worker threads is race-free and keeps
its process-wide semantics where it must.

Two probes, both built from source:

  test_spu_trace_file.c (runtime/spu/spu_channels.c)
    - spu_trace_init(path) on the main thread with SPU_TRACE=1 as the opt-in,
      then two workers trace: both workers' PCs land in the one file and
      nothing goes to stderr. A per-thread destination sends both to stderr.
    - SPU_TRACE_FILE set with no init: eight workers are released together
      and each writes five lines while they race the lazy initialisation. The
      old per-thread open gave each worker its own "w" handle, so one worker's
      flushes truncated the others; the one shared file must hold all 40.
    - SPU_TRACE_MAX=-3 and -1 with the lazy open: a parsed negative budget is
      unbounded, so the run must finish with all 40 lines and no limit marker.
      -3 used to equal the election sentinel and spin every worker forever.

  test_spu_pchist_race.c (runtime/spu/spu_context.h), with
  -fsanitize=thread: spu_pchist_tick's lazy gate runs on every worker's first
  hop, so two workers entering it together must not race (SPU_PCHIST unset).
  If the compiler cannot link a trivial TSan program (Homebrew GCC on macOS),
  that probe prints a SKIP line visibly; every other case still runs.

Fail-before: the trace probe fails on d90409e (empty file after init; the
lazy file loses the first line) and the pchist probe reports a TSan race at
the `en` gate. Pass-after: both pass.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == "nt":
    print("SKIP: fixture currently requires a POSIX C compiler")
    sys.exit(0)

root = Path(__file__).resolve().parents[1]
spu = root / "runtime" / "spu"
cc = os.environ.get("CC", "cc")
base_env = {k: v for k, v in os.environ.items()
            if not k.startswith(("SPU_", "SPURS_", "PS3_", "YDKJ_", "TSAN_"))}
gc_flag = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
cflags = ["-std=gnu17", "-O1", "-g", "-UNDEBUG",
          "-ffunction-sections", "-fdata-sections",
          "-I" + str(root / "include"), "-I" + str(spu),
          "-I" + str(root / "runtime" / "platform")]


def build(out, sources, extra=()):
    subprocess.run([cc, *cflags, *sources, gc_flag, "-pthread", "-lm",
                    *extra, "-o", str(out)], check=True)


def tsan_available(tmp):
    """True when this compiler can compile and link a trivial TSan program."""
    try:
        subprocess.run([cc, "-std=gnu17", "-pthread", "-fsanitize=thread",
                        "-x", "c", "-", "-o", str(tmp / "tsan_probe")],
                       input="int main(void) { return 0; }\n", text=True,
                       check=True, capture_output=True)
        return True
    except subprocess.CalledProcessError:
        return False


def run(binary, argv, env, cwd, timeout=120):
    try:
        return subprocess.run([str(binary), *argv], env=env, cwd=cwd,
                              timeout=timeout, capture_output=True, text=True)
    except subprocess.TimeoutExpired:
        raise AssertionError(f"timed out after {timeout}s: {argv}") from None


def check_trace_init(binary, tmp):
    path = tmp / "init.trace"
    # The env is the opt-in; init only redirects the process-wide destination.
    p = run(binary, ["init", str(path)], dict(base_env, SPU_TRACE="1"), tmp)
    assert p.returncode == 0, (p.returncode, p.stdout, p.stderr[-3000:])
    assert "PASS: trace init" in p.stdout, (p.stdout, p.stderr[-3000:])
    assert p.stderr == "", ("trace leaked to stderr", p.stderr[-3000:])
    lines = path.read_text().splitlines()
    assert sorted(lines) == ["00100", "00200"], lines


def check_trace_lazy(binary, tmp, max_value=None, tag="lazy", timeout=30):
    path = tmp / f"{tag}.trace"
    env = dict(base_env, SPU_TRACE_FILE=str(path))
    if max_value is not None:
        env["SPU_TRACE_MAX"] = str(max_value)
    p = run(binary, ["lazy"], env, tmp, timeout=timeout)
    assert p.returncode == 0, (p.returncode, p.stdout, p.stderr[-3000:])
    assert "PASS: trace lazy" in p.stdout, (p.stdout, p.stderr[-3000:])
    assert p.stderr == "", ("trace leaked to stderr", p.stderr[-3000:])
    text = path.read_text()
    lines = text.splitlines()
    # Every line from the one shared FILE. The old per-worker open gave each
    # thread its own "w" handle, so one worker's flushes truncated the rest.
    want = sorted(f"{0x1000 + w * 0x100 + i:05X}"
                  for w in range(8) for i in range(5))
    assert sorted(lines) == want, lines
    assert len(lines) == 40, lines
    assert "-- SPU_TRACE_MAX reached" not in text, lines[-5:]


def main():
    tsan_ran = False
    with tempfile.TemporaryDirectory(prefix="spu-trace-file-") as tmp:
        tmp = Path(tmp)
        binary = tmp / "test_spu_trace_file"
        build(binary, [spu / "tests" / "test_spu_trace_file.c",
                       spu / "spu_channels.c"])
        for _ in range(3):
            check_trace_init(binary, tmp)
        # Repeated because the lazy-open election is the property under test.
        for _ in range(20):
            check_trace_lazy(binary, tmp)
        # A parsed negative budget normalises to unbounded. -3 used to equal
        # the election sentinel (every worker spins); -1 re-elected per call.
        for max_value in ("-3", "-1"):
            for _ in range(3):
                check_trace_lazy(binary, tmp, max_value=max_value,
                                 tag=f"lazy-max{max_value}", timeout=10)

        tsan_ran = tsan_available(tmp)
        if tsan_ran:
            tsan_bin = tmp / "test_spu_pchist_race"
            build(tsan_bin, [spu / "tests" / "test_spu_pchist_race.c"],
                  extra=("-fsanitize=thread",))
            p = run(tsan_bin, [], base_env, tmp)
            assert p.returncode == 0, (p.returncode, p.stdout, p.stderr[-3000:])
            assert "PASS: pchist tick" in p.stdout, (p.stdout, p.stderr[-3000:])
            assert "ThreadSanitizer" not in p.stderr, p.stderr[-3000:]
        else:
            print(f"SKIP: pchist TSan probe ({cc} has no ThreadSanitizer)")
    if tsan_ran:
        print("PASS: the SPU trace destination is process-wide and the pchist "
              "gate is per-thread")
    else:
        print("PASS: the SPU trace destination is process-wide (pchist TSan "
              "probe skipped)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
