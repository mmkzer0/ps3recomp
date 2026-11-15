#!/usr/bin/env python3
"""ps3_log: stderr bytes, per-site limits, rotation, mute and level, collapse, pre-configure
lines, the size cap, concurrent writers, and the stop on a log file that cannot be made.

usage: test_log.py <probe_log> [threads]

Each case is a process of its own (probe_log.c): the log is configured once per process from
the environment. Temp directories are unique per test run, so two ctest processes never share
a file. Nothing sleeps; every ordering is the lock's.
"""
import os
import re
import subprocess
import sys
import tempfile
import time

import gates_common as g

PROBE = None
THREADS = 8
LINES = 1500
LIMIT_ROUNDS = 20000      # probe_log.c

LINE_RE = re.compile(r"^(\d+\.\d{6}) (\S+) ([EWID]) (.*)$")


def probe(scenario, **cfg):
    env = {f"PS3RECOMP_LOG_{k.upper()}": str(v) for k, v in cfg.items()}
    return g.run([PROBE, scenario], env=g.clean_env(**env))


def read_log(path):
    """The file's lines as (seconds, thread, level, message); every line must parse."""
    with open(path, "rb") as f:
        data = f.read().decode("utf-8")
    assert data == "" or data.endswith("\n"), "the file ends inside a line"
    out = []
    for raw in data.split("\n")[:-1]:
        m = LINE_RE.match(raw)
        assert m, f"not a log line: {raw[:120]!r}"
        out.append((float(m.group(1)), m.group(2), m.group(3), m.group(4)))
    return out


def messages(path):
    return [m for _, _, _, m in read_log(path)]


def expected_identity_stderr():
    body = "a" * 3000
    s = "[spu] x 5 y\n[hle] value 0x0000beef\nplain line 1\n[p] partial"
    s += f"[long] {body}\n[e] error level\n[w] warn level\n"
    for n in range(1015, 1031):
        s += f"[b] {'a' * n}\n"
    return s


# ---- 1. stderr is what the fprintf printed -----------------------------------------------------

def test_stderr_is_byte_identical():
    want = expected_identity_stderr()
    old = probe("legacy")
    assert old.returncode == 0 and old.stderr == want, "the fixture's own fprintf lines changed"
    with tempfile.TemporaryDirectory() as d:
        new_plain = probe("migrated")
        new_file = probe("migrated", file=os.path.join(d, "run.log"))
        new_early = probe("migrated-noconfig")
        for name, p in (("no file", new_plain), ("with file", new_file), ("before configure", new_early)):
            assert p.returncode == 0, p.stderr
            assert p.stderr == want, f"stderr differs from the fprintf output ({name})"
        # What goes to the file is the same text behind a prefix, and only the file has the prefix.
        rows = read_log(os.path.join(d, "run.log"))
        msgs = [r[3] for r in rows]
        assert msgs[0] == "[spu] x 5 y" and msgs[1] == "[hle] value 0x0000beef"
        assert msgs[2] == "plain line 1"
        assert msgs[3] == "[p] partial"                      # a partial line still ends its file line
        assert msgs[4] == f"[long] {'a' * 3000}"
        assert [r[2] for r in rows[:7]] == ["I", "I", "I", "I", "I", "E", "W"]
        assert all(r[1] for r in rows)                       # a thread column on every line
        times = [r[0] for r in rows]
        assert times == sorted(times)


# ---- 2. PS3_LOG_N ---------------------------------------------------------------------------------

def test_limit_lets_exactly_limit_lines_through():
    with tempfile.TemporaryDirectory() as d:
        p = probe("limit", file=os.path.join(d, "run.log"))
        assert p.returncode == 0, p.stderr
        want = "".join(f"[cap] capped line {i}\n" for i in range(3)) + "".join(f"[lazy] lazy {i}\n" for i in range(1, 5))
        assert p.stderr == want, p.stderr
        assert "evaluated=4" in p.stdout, p.stdout     # silenced lines never evaluate their arguments
        msgs = messages(os.path.join(d, "run.log"))
        assert msgs[:7] == want.split("\n")[:7]
        tail = msgs[7:]
        assert len(tail) == 2, tail
        cap = [m for m in tail if m.endswith("[cap]")]
        lazy = [m for m in tail if m.endswith("[lazy]")]
        assert len(cap) == 1 and re.fullmatch(r"\[log\] suppressed 7 lines after the first 3 at \S*probe_log\.c:\d+ \[cap\]", cap[0]), tail
        assert len(lazy) == 1 and re.fullmatch(r"\[log\] suppressed 6 lines after the first 4 at \S*probe_log\.c:\d+ \[lazy\]", lazy[0]), tail
        assert "suppressed" not in p.stderr           # the summary is for the file only


# ---- 3. rotation ----------------------------------------------------------------------------------

def test_rotation_keeps_exactly_one_previous_run():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        with open(path, "w") as f:
            f.write("OLD\n")
        with open(path + ".prev", "w") as f:
            f.write("OLDER\n")
        p = probe("levels", file=path)
        assert p.returncode == 0, p.stderr
        with open(path + ".prev") as f:
            assert f.read() == "OLD\n"                 # the old file replaced the older .prev
        first = messages(path)
        assert first and all(m.startswith("[lv]") for m in first), first   # fresh: nothing of OLD in it
        p = probe("levels", file=path)
        assert p.returncode == 0, p.stderr
        assert messages(path + ".prev") == first       # the previous run survived the next one
        assert messages(path) == first


def test_first_run_makes_no_prev():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        assert probe("levels", file=path).returncode == 0
        assert os.path.exists(path) and not os.path.exists(path + ".prev")


# ---- 4. mute and level ------------------------------------------------------------------------------

def test_mute_hides_stderr_only():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        p = probe("tags", file=path, mute="noisy")
        assert p.returncode == 0, p.stderr
        assert p.stderr == "[quiet] from quiet\n[noisy.sub] from noisy.sub\n", p.stderr   # whole tags, not prefixes
        assert messages(path) == ["[noisy] from noisy", "[quiet] from quiet", "[noisy.sub] from noisy.sub"]
        p = probe("tags", mute="noisy,quiet")
        assert p.stderr == "[noisy.sub] from noisy.sub\n", p.stderr


def test_level_filters_both_sinks():
    with tempfile.TemporaryDirectory() as d:
        for level, shown in (("error", 1), ("warn", 2), ("info", 3), ("debug", 4)):
            path = os.path.join(d, f"{level}.log")
            p = probe("levels", file=path, level=level)
            assert p.returncode == 0, p.stderr
            want = ["[lv] error line", "[lv] warn line", "[lv] info line", "[lv] debug line"][:shown]
            assert p.stderr.split("\n")[:-1] == want, (level, p.stderr)
            assert messages(path) == want, (level, messages(path))
        # The default is info: nothing a migrated site prints today disappears.
        assert probe("levels").stderr.count("\n") == 3


# ---- 5. collapse ------------------------------------------------------------------------------------

COLLAPSED = (["[rep] same", "[log] previous line repeated 4 times", "[rep] other", "[rep] same",
              "[log] previous line repeated 2 times"] + ["[rep] twin"] * 8 +
             ["[rep] same", "[log] previous line repeated 1 times"])
PLAIN = ["[rep] same"] * 5 + ["[rep] other"] + ["[rep] same"] * 3 + ["[rep] twin"] * 8 + ["[rep] same"] * 2


def test_collapse_modes():
    with tempfile.TemporaryDirectory() as d:
        # file (the default): repeats collapse in the file, stderr prints every line.
        path = os.path.join(d, "file.log")
        p = probe("collapse", file=path)
        assert p.stderr.split("\n")[:-1] == PLAIN, p.stderr
        assert messages(path) == COLLAPSED, messages(path)
        # all: stderr collapses too, and says so.
        path = os.path.join(d, "all.log")
        p = probe("collapse", file=path, collapse="all")
        assert p.stderr.split("\n")[:-1] == COLLAPSED, p.stderr
        assert messages(path) == COLLAPSED
        # all with no file still collapses stderr (and the run's last repeat count is flushed at exit).
        p = probe("collapse", collapse="all")
        assert p.stderr.split("\n")[:-1] == COLLAPSED, p.stderr
        # off: nothing collapses.
        path = os.path.join(d, "off.log")
        p = probe("collapse", file=path, collapse="off")
        assert p.stderr.split("\n")[:-1] == PLAIN
        assert messages(path) == PLAIN


# ---- 6. lines before configure ------------------------------------------------------------------------

def raw_lines(path):
    """The file's lines as written, without the newline."""
    with open(path, "rb") as f:
        return f.read().decode("utf-8").split("\n")[:-1]


def loose_messages(path):
    """Messages of the file's lines; a raw line (the mirrored stop banner) stands for itself."""
    out = []
    for r in raw_lines(path):
        m = LINE_RE.match(r)
        out.append(m.group(4) if m else r)
    return out


def test_lines_before_configure_reach_the_file():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        p = probe("pre", file=path)
        assert p.returncode == 0, p.stderr
        want = [f"[early] before configure {i}" for i in range(5)] + ["[late] after configure"]
        assert p.stderr.split("\n")[:-1] == want, p.stderr
        assert messages(path) == want


def test_pre_configure_overflow_is_reported():
    buffer = 64 * 1024
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        p = probe("pre-overflow", file=path)
        assert p.returncode == 0, p.stderr
        assert p.stderr.count("\n") == 2000              # stderr lost nothing
        raw = raw_lines(path)
        notes = [r for r in raw if "[log] " in r and "lines printed before" in r]
        kept = [r for r in raw if "[early] pre " in r]
        assert len(notes) == 1, notes
        dropped = int(re.search(r"\[log\] (\d+) lines printed before", notes[0]).group(1))
        assert len(kept) + dropped == 2000, (len(kept), dropped)
        # The buffer is full to within one line: the oldest lines are kept, as many as fit in 64 KiB.
        used = sum(len(r.encode()) + 1 for r in kept)
        step = len(kept[-1].encode()) + 1                 # every early line is this long
        assert used <= buffer < used + step, (used, step, buffer)
        for i, r in enumerate(kept):
            assert r.endswith(f"[early] pre {i:04d} " + "." * 64), (i, r)


# ---- 7. size cap --------------------------------------------------------------------------------------

def test_size_cap_stops_the_file_not_stderr():
    cap = 1 << 20
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        p = probe("cap", file=path, max_mb=1)
        assert p.returncode == 0, p.stderr
        assert p.stderr.count("\n") == 3000
        raw = raw_lines(path)
        fill = [r for r in raw if "[fill] " in r]
        notes = [r for r in raw if "[log] size cap" in r]
        assert len(notes) == 1 and raw[-1] == notes[0], notes
        used = sum(len(r.encode()) + 1 for r in fill)
        step = len(fill[-1].encode()) + 1                     # every fill line is this long
        # Exactly at the cap: the file holds as many whole lines as fit, and not one more.
        assert used <= cap < used + step, (used, step, cap)
        assert os.path.getsize(path) == used + len(notes[0].encode()) + 1   # and only the report beside them
        n = int(re.search(r"size cap reached, (\d+) further lines not written", notes[0]).group(1))
        assert len(fill) + n == 3000, (len(fill), n)
        assert fill[0].endswith("[fill] 00000 " + "c" * 800)


# ---- 9. a log file that cannot be made ----------------------------------------------------------------

def test_directory_as_log_path_stops():
    with tempfile.TemporaryDirectory() as d:
        p = probe("config-only", file=d)
        f = g.expect_stop(p, "host-log-file")
        assert f["op"] == "open" and f["path_len"] == str(len(d)), f
        if os.name == "posix":
            assert f["errno"] == "21", f                     # EISDIR
        assert d not in p.stderr.split("PS3STOP")[1]        # the path is never printed on the stop line
        assert os.path.isdir(d) and not os.path.exists(d + ".prev")   # refused, not renamed away


def test_missing_directory_stops_on_open():
    with tempfile.TemporaryDirectory() as d:
        f = g.expect_stop(probe("config-only", file=os.path.join(d, "no", "such", "run.log")), "host-log-file")
        assert f["op"] == "open", f


def test_unmovable_old_log_stops_on_rename():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        with open(path, "w") as fh:
            fh.write("keep me\n")
        os.mkdir(path + ".prev")                              # a directory cannot be replaced by a file
        f = g.expect_stop(probe("config-only", file=path), "host-log-file")
        assert f["op"] == "rename", f
        with open(path) as fh:
            assert fh.read() == "keep me\n"                   # the old log is untouched


def test_path_that_does_not_fit_stops_with_path_limit():
    f = g.expect_stop(probe("config-only", file="a" * 1020), "host-path-limit")
    assert f["func"] == "ps3_log_configure" and int(f["need"]) > int(f["limit"]) - 5, f


# ---- the stop banner reaches the file -------------------------------------------------------------------

def test_stop_banner_is_mirrored_after_the_summary():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        p = probe("stop", file=path)
        g.expect_stop(p, "config-invalid")
        with open(path, "rb") as fh:
            text = fh.read().decode()
        assert "[before] a line before the stop" in text
        i_sum = text.index("[log] suppressed 3 lines after the first 2")
        i_rep = text.index("[log] previous line repeated 2 times")
        i_ban = text.index("PS3RECOMP STOP: config-invalid")
        assert i_rep < i_sum < i_ban, (i_rep, i_sum, i_ban)   # pending repeats, summary, then the banner
        assert text.rstrip("\n").endswith(g.stop_lines(p.stderr)[0])   # the machine line is the file's last


# ---- a guest exit ends in quick_exit -----------------------------------------------------------------------

def test_guest_exit_still_writes_the_summary():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        p = probe("quick-exit", file=path)
        assert p.returncode == 0, p.stderr
        msgs = messages(path)
        assert msgs[:3] == ["[capped] capped 0", "[capped] capped 1", "[rep] same"], msgs
        assert "[log] previous line repeated 2 times" in msgs, msgs
        assert any(m.startswith("[log] suppressed 3 lines after the first 2 ") for m in msgs), msgs


# ---- a run killed before ppu_run starts ps3_stdio's flusher ----------------------------------------------------

def test_log_survives_a_kill_between_configure_and_ppu_run():
    # Hosts load the config (which opens the file) long before ppu_run() starts ps3_stdio. The log's own
    # flusher must already be running: the file is fully buffered, so without it the line is lost when the
    # process is killed. The flusher's period is 50 ms (LOG_FLUSH_MS); the test does not assert that in wall
    # time, because a loaded host can delay any thread by far more, which would make it flaky. It asserts
    # the property instead: with the process alive, idle, and nothing else that could flush (no exit, no
    # stdio), the line reaches the file, and survives SIGKILL. The wait is a poll with a generous deadline,
    # not an ordering by time.
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        proc = subprocess.Popen([PROBE, "kill-wait"], env=g.clean_env(PS3RECOMP_LOG_FILE=path),
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            assert proc.stdout.readline().strip() == b"ready"
            deadline = time.monotonic() + 15
            seen = False
            while time.monotonic() < deadline and not seen:
                if os.path.exists(path):
                    with open(path, "rb") as f:
                        seen = b"line before ppu_run" in f.read()
                if not seen:
                    time.sleep(0.005)
        finally:
            proc.kill()
            proc.wait()
            for s in (proc.stdin, proc.stdout, proc.stderr):
                s.close()
        assert seen, "the line was still in the process's buffer after 15 s"
        assert messages(path) == ["[startup] line before ppu_run"]


# ---- config -----------------------------------------------------------------------------------------------

def test_log_options_are_validated():
    for key, value in (("level", "loud"), ("collapse", "some"), ("mute", "a b"), ("mute", "a,,b"),
                       ("max_mb", "0"), ("max_mb", "65536")):
        p = probe("config-only", **{key: value})
        g.expect_stop(p, "config-invalid")
        assert f"PS3RECOMP_LOG_{key.upper()}" in p.stderr, (key, value, p.stderr)
    p = probe("config-only", mute="a.b-c_D,e", level="debug", collapse="off", max_mb=65535)
    assert p.returncode == 0, p.stderr


# ---- 8. concurrent writers --------------------------------------------------------------------------------

PAD = ("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
       "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")


def expected_thread_line(t, i):
    body = "z" * 1600 if i % 7 == 0 else PAD[:(i * 13) % (len(PAD))]
    return f"[thr] t{t} n{i:05d} {body}|"


def check_stream(lines, what):
    per = {t: [] for t in range(THREADS)}
    capped = 0
    for line in lines:
        if line.startswith("[thr] "):
            m = re.match(r"\[thr\] t(\d+) n(\d{5}) ", line)
            assert m, f"{what}: a line was cut or mixed: {line[:100]!r}"
            t, i = int(m.group(1)), int(m.group(2))
            assert line == expected_thread_line(t, i), f"{what}: thread {t} line {i} is not intact: {line[:100]!r}"
            per[t].append(i)
        elif line.startswith("[capped] "):
            assert re.fullmatch(r"\[capped\] capped t\d+ n\d{5}", line), f"{what}: {line!r}"
            capped += 1
        else:
            assert line.startswith("[log] "), f"{what}: a stray line {line[:100]!r}"
    for t in range(THREADS):
        assert per[t] == list(range(LINES)), f"{what}: thread {t} lost or reordered lines"
    assert capped == 100, f"{what}: {capped} lines through a limit of 100"


def test_threads_never_interleave_and_counts_are_exact():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        p = probe("threads", file=path, collapse="off")
        assert p.returncode == 0, p.stderr
        err = p.stderr.split("\n")
        assert err[-1] == ""
        err = err[:-1]
        check_stream(err, "stderr")
        file_msgs = messages(path)
        check_stream(file_msgs, "file")
        # One lock around both sinks: the two see the same lines in the same order.
        assert [m for m in file_msgs if not m.startswith("[log]")] == err
        summary = [m for m in file_msgs if m.startswith("[log] suppressed")]
        assert len(summary) == 1 and f"suppressed {THREADS * LINES - 100} lines after the first 100 " in summary[0], summary


def test_threads_limit_is_exact_when_they_reach_it_together():
    # Eight threads start on a barrier and hammer one capped site with nothing between the calls. The slot is
    # taken before the arguments are evaluated, so exactly `limit` calls evaluate them (the argument counts),
    # print, and the rest are counted. A limit read in one step and taken in another lets several through.
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        total = THREADS * LIMIT_ROUNDS
        for _ in range(3):
            p = probe("threads-limit", file=path)
            assert p.returncode == 0, p.stderr
            assert "evaluated=100" in p.stdout, p.stdout
            assert p.stderr.count("[hot] hot t") == 100, p.stderr.count("[hot] hot t")
            msgs = messages(path)
            assert len([m for m in msgs if m.startswith("[hot]")]) == 100
            summary = [m for m in msgs if m.startswith("[log] suppressed")]
            assert len(summary) == 1 and f"suppressed {total - 100} lines after the first 100 " in summary[0], summary


def inflight(scenario):
    """A worker blocked inside a logging operation when the process ends (probe_log.c: the releaser frees it
    only once the closer has shut the gate). Returns (process, file messages)."""
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        # `probe` lines come from the releaser while it waits; muted on stderr, collapsed in the file.
        p = probe(scenario, file=path, mute="probe")
        return p, loose_messages(path)


def check_inflight(msgs):
    assert "[inflight] worker line 42" in msgs, "the operation in flight at the end never reached the file"
    worker = msgs.index("[inflight] worker line 42")
    summary = next(i for i, m in enumerate(msgs) if m.startswith("[log] suppressed 3 lines after the first 2 "))
    assert msgs.index("[before] a line before the exit") < worker < summary, msgs
    return summary


def test_threads_inflight_line_is_written_before_a_guest_exit_ends_the_process():
    p, msgs = inflight("inflight-exit")
    assert p.returncode == 0, (p.returncode, p.stderr)
    check_inflight(msgs)


def test_threads_inflight_line_is_written_before_the_stop_banner():
    p, msgs = inflight("inflight-stop")
    g.expect_stop(p, "config-invalid")
    summary = check_inflight(msgs)
    assert any("PS3RECOMP STOP: config-invalid" in m for m in msgs[summary:]), msgs   # worker, summary, banner


def test_threads_late_operation_is_dropped_after_the_exit_summary():
    # The worker is released only by the last quick-exit handler, after the log's own: long after the 250 ms
    # drain gave up on it. Its line must reach neither stderr nor the file, and the summary stays the last
    # thing the file says.
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        p = probe("late-exit", file=path)
        assert p.returncode == 0, (p.returncode, p.stderr)
        assert "worker line" not in p.stderr, p.stderr
        msgs = messages(path)
        assert not any("worker line" in m for m in msgs), msgs
        assert msgs[-1].startswith("[log] suppressed 3 lines after the first 2 "), msgs


def test_threads_late_operation_is_dropped_after_the_stop_banner():
    # Released after the banner was written and mirrored: nothing may follow the PS3STOP line on stderr
    # (expect_stop checks it is the last line) or the banner in the file.
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "run.log")
        p = probe("late-stop", file=path)
        g.expect_stop(p, "config-invalid")
        assert "worker line" not in p.stderr, p.stderr
        msgs = loose_messages(path)
        assert not any("worker line" in m for m in msgs), msgs
        assert msgs[-1].startswith("PS3STOP v=1 kind=config-invalid"), msgs[-3:]


def test_threads_flusher_cannot_flush_after_the_final_flush():
    # The flusher is parked at a tick when the process ends. The last handler (after the log's) releases it
    # and waits for it to leave. Without the stop signal it would flush the file at that point.
    for scenario in ("flusher-exit", "flusher-quick"):
        with tempfile.TemporaryDirectory() as d:
            p = probe(scenario, file=os.path.join(d, "run.log"))
            assert p.returncode == 0, (scenario, p.returncode, p.stderr)
            assert "flush_after_final=0 exit_seen=1" in p.stdout, (scenario, p.stdout)


def test_threads_a_stalled_log_lock_does_not_hang_the_ending():
    # The flusher holds the log lock forever (a stalled disk). Exit and stop must still end, within the
    # bounded waits (drain, flusher, lock: 250 ms each), skip the file's final work and say so once on stderr.
    for scenario in ("stall-exit", "stall-stop"):
        with tempfile.TemporaryDirectory() as d:
            started = time.monotonic()
            p = probe(scenario, file=os.path.join(d, "run.log"))
            elapsed = time.monotonic() - started
            if scenario == "stall-stop":
                g.expect_stop(p, "config-invalid")
            else:
                assert p.returncode == 0, (p.returncode, p.stderr)
            assert elapsed < 10, (scenario, elapsed)
            assert p.stderr.count("the log lock was still held at the shutdown deadline") == 1, p.stderr


def test_level_expression_is_evaluated_once():
    for level, shown in (("info", 3), ("warn", 0), ("debug", 6)):
        p = probe("level-once", level=level)
        assert p.returncode == 0, p.stderr
        assert "evals=6" in p.stdout, (level, p.stdout)       # once per macro use, whatever the filter says
        assert p.stderr.count("[lvl] ") == shown, (level, p.stderr)


def main():
    global PROBE
    PROBE = sys.argv[1]
    want_threads = len(sys.argv) > 2 and sys.argv[2] == "threads"
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith("test_")]
    tests = [(n, f) for n, f in tests if n.startswith("test_threads_") == want_threads]
    return g.main(tests)


if __name__ == "__main__":
    sys.exit(main())
