#!/usr/bin/env python3
"""Tests for tools/xctrace_profile.py on synthetic `xctrace export` time-profile XML.

The XML is built here the way xctrace writes it: the first occurrence of a value
carries id="N", later occurrences (possibly in later rows) are <tag ref="N"/>.
"""

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from xml.sax.saxutils import escape, quoteattr

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xctrace_profile as xp  # noqa: E402

TOOL = os.path.join(HERE, "xctrace_profile.py")
S = 1_000_000_000
BASE = 5 * S            # absolute time of the first sample; reports are relative to it

COLS = ("time", "thread", "process", "core", "thread-state", "weight", "stack")
SYSLIB = "/usr/lib/system/libsystem_pthread.dylib"
APP = "/build/app"


class Exporter:
    """Writes rows with xctrace-style interning."""

    def __init__(self):
        self.next_id = 1
        self.seen = {}
        self.rows = []

    def _emit(self, key, tag, attrs, inner=lambda: ""):
        if key in self.seen:
            return '<%s ref="%d"/>' % (tag, self.seen[key])
        i = self.next_id
        self.next_id += 1
        self.seen[key] = i
        a = "".join(" %s=%s" % (k, quoteattr(v)) for k, v in attrs)
        return '<%s id="%d"%s>%s</%s>' % (tag, i, a, inner(), tag)

    def frame(self, name, binary=APP, src=None, inlined=False):
        """src = (path, line) or None. A frame repeated with the same fields becomes a ref."""
        key = ("frame", name, binary, src, inlined)

        def inner():
            out = self._emit(("bin", binary), "binary", [("name", os.path.basename(binary)), ("path", binary)])
            if src:
                out += '<source line="%d">%s</source>' % (src[1], self._emit(("path", src[0]), "path", [], lambda: escape(src[0])))
            return out
        attrs = ([("inlined", "true")] if inlined else []) + [("name", name), ("addr", "0x1000")]
        return self._emit(key, "frame", attrs, inner)

    def sample(self, t, tid, core, frames, weight=1_000_000):
        """frames: leaf-first list of Exporter.frame() strings (or (name, kwargs) pairs)."""
        fr = "".join(self.frame(f) if isinstance(f, str) else self.frame(f[0], **f[1]) for f in frames)
        thread = self._emit(("thread", tid), "thread", [("fmt", "app (0x%x)" % tid)],
                            lambda: self._emit(("tid", tid), "tid", [("fmt", "0x%x" % tid)], lambda: str(tid)))
        core_fmt = "CPU %d (%s Core)" % (0 if core == "P" else 9, core)
        row = ("<row>%s%s%s%s%s%s%s</row>" % (
            '<sample-time fmt="x">%d</sample-time>' % t,
            thread,
            self._emit(("proc",), "process", [("fmt", "app (1)")]),
            self._emit(("core", core), "core", [("fmt", core_fmt)], lambda: "0"),
            self._emit(("state",), "thread-state", [("fmt", "Running")], lambda: "Running"),
            self._emit(("w", weight), "weight", [("fmt", "1.00 ms")], lambda: str(weight)),
            "<tagged-backtrace>%s</tagged-backtrace>" % fr))
        self.rows.append(row)

    def xml(self):
        cols = "".join("<col><mnemonic>%s</mnemonic><name>%s</name></col>" % (c, c) for c in COLS)
        return ('<?xml version="1.0"?>\n<trace-query-result>\n<node xpath="//table[1]">'
                '<schema name="time-profile">%s</schema>\n%s\n</node>\n</trace-query-result>\n'
                % (cols, "\n".join(self.rows)))


def spu(ex, t, core="P", busy=True):
    if busy:
        ex.sample(t, 100, core, [("pthread_rwlock_unlock", dict(binary=SYSLIB)),
                                 ("_pthread_rwlock_unlock_slow", dict(binary=SYSLIB)),
                                 ("mfc_for", dict(src=("/rt/spu_channels.c", 10))),
                                 ("spu_step", dict(src=("/rt/spu_interp.c", 237))),
                                 "run_job", "worker_loop"])
    else:
        ex.sample(t, 100, core, ["__psynch_cvwait", "wait<lambda>", "worker_loop"])


def guest(ex, t, leaf):
    """PPU guest thread 300: func_00000020 calls func_00000010's helper chain."""
    ex.sample(t, 300, "P", leaf + [("func_00000020(ppu_context*)", {}), ("func_00000010(ppu_context*)", {}),
                                   "ppu_run"])


def synthetic():
    ex = Exporter()
    # t (s from first sample): 0.000 spu busy P, 0.001 rsx P, 1.000 spu busy E, 1.500 guest
    spu(ex, BASE + 0, "P")
    ex.sample(BASE + 1_000_000, 200, "P", [("memmove", dict(binary="/usr/lib/libsystem_platform.dylib")),
                                           "cellGcm_rsx_process_fifo"])
    guest(ex, BASE + 2_000_000, [("barrier_watch_hit", dict(src=("/rt/barrier.c", 5))),
                                  ("barrier_watch_hit", dict(src=("/rt/barrier.c", 5), inlined=True))])
    ex.sample(BASE + 3_000_000, 400, "E", ["nanosleep", "usleep", "audio_mix_thread_func"])
    ex.sample(BASE + 4_000_000, 500, "P", ["__CFRunLoopRun", "run_windowed", "main"])
    spu(ex, BASE + 1 * S, "E")                       # exactly on a window boundary
    spu(ex, BASE + 1 * S + 2_000_000, "P", busy=False)
    guest(ex, BASE + 1 * S + 3_000_000, ["vm_write32"])
    guest(ex, BASE + 2 * S, [("barrier_watch_hit", dict(src=("/rt/barrier.c", 5)))])   # on the next boundary
    ex.sample(BASE + 2 * S + 500_000_000, 400, "P", ["nanosleep", "usleep", "audio_mix_thread_func"])
    spu(ex, BASE + 4 * S, "P")
    return ex.xml()


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def write(self, text, name="tp.xml"):
        p = os.path.join(self.dir.name, name)
        with open(p, "w") as f:
            f.write(text)
        return p

    def run_cli(self, path, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = xp.main([path, *args])
        return code, out.getvalue(), err.getvalue()

    def doc(self, path, *args):
        code, out, err = self.run_cli(path, "--json", *args)
        self.assertEqual((code, err), (0, ""))
        return json.loads(out)


class Aggregation(Base):
    def setUp(self):
        super().setUp()
        self.path = self.write(synthetic())

    def test_roles_and_cores(self):
        d = self.doc(self.path, "--roles")
        roles = {r["role"]: r for r in d["roles"]["roles"]}
        self.assertEqual(list(roles), ["spu-worker-busy", "spu-worker-idle", "rsx", "ppu-guest", "audio-mix",
                                       "main/ui"])      # fixed order, "other" absent
        self.assertEqual((roles["spu-worker-busy"]["samples"], roles["spu-worker-busy"]["p_core_samples"],
                          roles["spu-worker-busy"]["e_core_samples"]), (3, 2, 1))
        self.assertEqual(roles["spu-worker-idle"]["samples"], 1)
        self.assertEqual(roles["ppu-guest"]["samples"], 3)
        self.assertEqual((roles["audio-mix"]["p_core_samples"], roles["audio-mix"]["e_core_samples"]), (1, 1))
        self.assertEqual(roles["audio-mix"]["e_core_pct"], 50.0)
        threads = {t["tid"]: t for t in d["roles"]["threads"]}
        self.assertEqual(threads["0x64"]["role"], "spu-worker-busy")
        self.assertEqual(threads["0x64"]["top_self"], "pthread_rwlock_unlock")

    def test_summary_counts_every_row(self):
        d = self.doc(self.path)
        self.assertEqual(d["summary"]["samples_in_window"], 11)
        self.assertEqual(d["summary"]["cpu_ms_selected"], 11.0)
        self.assertEqual(d["summary"]["window_s"], [0.0, 4.0])

    def test_hotspots_self_inclusive_line(self):
        by_self = {r["name"]: r["samples"] for r in self.doc(self.path, "--by", "self")["hotspots"]["rows"]}
        self.assertEqual(by_self["pthread_rwlock_unlock"], 3)
        self.assertEqual(by_self["barrier_watch_hit"], 2)       # the inlined duplicate is one leaf frame
        incl = {r["name"]: r["samples"] for r in self.doc(self.path, "--by", "inclusive")["hotspots"]["rows"]}
        # barrier_watch_hit appears twice on one stack (real + inlined frame): counted once there.
        self.assertEqual(incl["barrier_watch_hit"], 2)
        self.assertEqual(incl["worker_loop"], 4)
        self.assertEqual(incl["func_00000010(ppu_context*)"], 3)
        line = {r["site"]: r["samples"] for r in self.doc(self.path, "--by", "line")["hotspots"]["rows"]
                if "site" in r}
        self.assertEqual(line["/rt/barrier.c:5"], 2)
        self.assertEqual(line["pthread_rwlock_unlock [libsystem_pthread.dylib] (no source)"], 3)

    def test_ref_across_rows_resolves_to_same_frame(self):
        # spu() is emitted 4 times: later rows only carry ref= for thread, core and every frame.
        xml = synthetic()
        self.assertIn("ref=", xml)
        d = self.doc(self.path, "--thread", "100", "--by", "self")
        self.assertEqual(d["hotspots"]["rows"][0], {"name": "pthread_rwlock_unlock",
                                                    "binary": "libsystem_pthread.dylib", "samples": 3, "pct": 75.0})

    def test_thread_filter_accepts_hex_and_decimal(self):
        a = self.doc(self.path, "--thread", "0x64,0xc8")
        b = self.doc(self.path, "--thread", "100,200")
        self.assertEqual(a["summary"]["samples_selected"], 5)
        self.assertEqual(a, b)

    def test_role_filter(self):
        self.assertEqual(self.doc(self.path, "--role", "spu-worker")["summary"]["samples_selected"], 4)
        self.assertEqual(self.doc(self.path, "--role", "rsx,audio-mix")["summary"]["samples_selected"], 3)

    def test_guest_aggregation(self):
        g = self.doc(self.path, "--guest")["guest"]
        self.assertEqual(g["guest_samples"], 3)
        incl = {r["guest"]: r for r in g["inclusive"]}
        self.assertEqual(incl["0x00000010"]["inclusive"], 3)
        self.assertEqual(incl["0x00000020"]["self"], 3)         # innermost guest frame of all three
        self.assertEqual(incl["0x00000020"]["own"], 0)          # never the leaf frame
        self.assertEqual(incl["0x00000010"]["self"], 0)
        leaf = {r["name"]: r for r in g["host_leaf"]}
        self.assertEqual(leaf["barrier_watch_hit"]["samples"], 2)
        self.assertEqual(leaf["barrier_watch_hit"]["guest_callers"], [{"guest": "0x00000020", "samples": 2}])
        self.assertEqual(leaf["vm_write32"]["samples"], 1)

    def test_callers_skip_frames_of_the_leaf_system_library(self):
        c = self.doc(self.path, "--callers", "pthread_rwlock", "--caller-depth", "2")["callers"]
        self.assertEqual(c["matched_samples"], 3)
        row = c["rows"][0]
        self.assertEqual(row["leaf"], "pthread_rwlock_unlock")
        self.assertEqual(row["callers"], [{"name": "mfc_for", "site": "/rt/spu_channels.c:10"},
                                          {"name": "spu_step", "site": "/rt/spu_interp.c:237"}])

    def test_timeline_buckets(self):
        t = self.doc(self.path, "--timeline", "1")["timeline"]
        self.assertEqual([b["t_s"] for b in t["buckets"]], [0.0, 1.0, 2.0, 3.0, 4.0])
        self.assertEqual([b["total"] for b in t["buckets"]], [5, 3, 2, 0, 1])
        self.assertEqual(t["buckets"][0]["samples"]["rsx"], 1)
        self.assertEqual(t["buckets"][4]["samples"]["spu-worker-busy"], 1)
        stats = {s["role"]: s for s in t["stats"]}
        self.assertEqual(stats["rsx"]["zero_buckets"], 4)
        self.assertEqual(stats["spu-worker-busy"]["max"], 1)

    def test_gaps_report_other_roles_inside_the_gap(self):
        g = self.doc(self.path, "--thread", "400", "--gaps", "100")["gaps"]
        self.assertEqual(g["selection_samples"], 2)
        self.assertEqual(g["gaps"], 1)
        self.assertEqual(g["longest"][0]["gap_ms"], 2497.0)
        self.assertEqual(g["longest"][0]["samples_by_role"], {"spu-worker-busy": 1, "spu-worker-idle": 1,
                                                              "ppu-guest": 2, "main/ui": 1})
        self.assertEqual(g["one_second_buckets_without_sample"], 3)    # 1.0-2.0, 3.0-4.0 and 4.0-5.0 s have none
        self.assertEqual(g["threads"][0]["gaps"], 1)

    def test_runs_split_where_samples_are_further_apart_than_the_threshold(self):
        r = self.doc(self.path, "--runs", "2.5")["runs"]
        roles = {x["role"]: x for x in r["roles"]}
        busy = roles["spu-worker-busy"]      # runs: 0.000 (1 ms), 1.000-1.002 (3 ms), 4.000 (1 ms)
        self.assertEqual((busy["runs"], busy["total_ms"], busy["median_ms"], busy["max_ms"]), (3, 5.0, 1.0, 3.0))
        self.assertEqual(busy["runs_per_s"], round(3 / 4.000000001, 3))
        self.assertEqual((roles["ppu-guest"]["runs"], roles["ppu-guest"]["max_ms"]), (3, 1.0))
        self.assertEqual(r["longest"][0], {"tid": "0x64", "role": "spu-worker-busy", "from_s": 1.0, "length_ms": 3.0})
        # a threshold below the 2 ms spacing splits the 3 ms run; its second half is the idle sample
        r = self.doc(self.path, "--runs", "1.5")["runs"]
        by_role = {x["role"]: x["runs"] for x in r["roles"]}
        self.assertEqual((by_role["spu-worker-busy"], by_role["spu-worker-idle"]), (3, 1))

    def test_text_report_is_deterministic(self):
        a = self.run_cli(self.path, "--roles", "--timeline", "1", "--guest", "--gaps", "100")
        b = self.run_cli(self.path, "--roles", "--timeline", "1", "--guest", "--gaps", "100")
        self.assertEqual(a[0], 0)
        self.assertEqual(a, b)
        self.assertIn("== roles", a[1])
        self.assertIn("0x00000020", a[1])


class Window(Base):
    def setUp(self):
        super().setUp()
        self.path = self.write(synthetic())

    def sel(self, *args):
        return self.doc(self.path, *args)["summary"]["samples_selected"]

    def test_from_is_inclusive_and_to_is_exclusive(self):
        self.assertEqual(self.sel("--from", "1", "--to", "2"), 3)       # 1.000, 1.002, 1.003; not 2.000
        self.assertEqual(self.sel("--to", "1"), 5)                      # not the sample at exactly 1.000
        self.assertEqual(self.sel("--from", "2"), 3)                    # 2.000, 2.500, 4.000
        self.assertEqual(self.sel("--from", "4"), 1)
        self.assertEqual(self.sel("--from", "4.0001"), 0)

    def test_times_are_relative_to_the_first_sample(self):
        d = self.doc(self.path, "--from", "1", "--to", "2", "--timeline", "0.5")["timeline"]
        self.assertEqual([b["t_s"] for b in d["buckets"]], [1.0, 1.5])
        self.assertEqual([b["total"] for b in d["buckets"]], [3, 0])

    def test_window_does_not_change_other_filters(self):
        d = self.doc(self.path, "--from", "1", "--role", "spu-worker")["summary"]
        self.assertEqual(d["samples_in_window"], 6)
        self.assertEqual(d["samples_selected"], 3)

    def test_empty_window_is_not_an_error(self):
        code, out, err = self.run_cli(self.path, "--from", "4.5", "--to", "9", "--roles", "--timeline", "1", "--guest")
        self.assertEqual((code, err), (0, ""))
        self.assertIn("0 selected", out)

    def test_inverted_window_is_a_usage_error(self):
        code, out, err = self.run_cli(self.path, "--from", "3", "--to", "2")
        self.assertEqual(code, 2)
        self.assertIn("window is empty", err)


class LongRuns(Base):
    def test_run_length_counters(self):
        ex = Exporter()
        for i in range(6):                       # a 6 ms run, then a 1 ms run 100 ms later
            spu(ex, BASE + i * 1_000_000)
        spu(ex, BASE + 100_000_000)
        for i in range(12):                      # and a 12 ms run on another thread
            ex.sample(BASE + 500_000_000 + i * 1_000_000, 200, "P", ["memmove", "cellGcm_rsx_process_fifo"])
        r = {x["role"]: x for x in self.doc(self.write(ex.xml()), "--runs", "2.5")["runs"]["roles"]}
        busy = r["spu-worker-busy"]
        self.assertEqual((busy["runs"], busy["max_ms"], busy["ge_5ms"], busy["ge_10ms"], busy["ge_20ms"]), (2, 6.0, 1, 0, 0))
        self.assertEqual((r["rsx"]["runs"], r["rsx"]["max_ms"], r["rsx"]["ge_5ms"], r["rsx"]["ge_10ms"],
                          r["rsx"]["ge_20ms"]), (1, 12.0, 1, 1, 0))


class BadInput(Base):
    def assert_exit2(self, argv, needle=""):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = xp.main(argv)
        self.assertEqual(code, 2, err.getvalue())
        self.assertEqual(out.getvalue(), "")
        self.assertNotIn("Traceback", err.getvalue())
        self.assertIn(needle, err.getvalue())

    def test_malformed_xml(self):
        p = self.write(synthetic()[:-120])
        self.assert_exit2([p], "not well-formed")

    def test_garbage_file(self):
        self.assert_exit2([self.write("this is not xml")], "not well-formed")

    def test_missing_file(self):
        self.assert_exit2([os.path.join(self.dir.name, "nope.xml")], "cannot read")

    def test_no_time_profile_rows(self):
        self.assert_exit2([self.write('<?xml version="1.0"?><trace-query-result/>')], "no time-profile rows")

    def test_ref_before_id(self):
        xml = synthetic().replace('<core id="', '<core ref="99" x="', 1)
        self.assert_exit2([self.write(xml)], "ref=99")

    def test_other_table_is_not_time_profile(self):
        xml = synthetic().replace('name="time-profile"', 'name="counters"')
        self.assert_exit2([self.write(xml)], "no time-profile rows")

    def test_row_with_the_wrong_columns(self):
        xml = synthetic().replace("<mnemonic>stack</mnemonic>", "<mnemonic>other</mnemonic>")
        self.assert_exit2([self.write(xml)], "no stack column")

    def test_bad_option_values(self):
        p = self.write(synthetic())
        self.assert_exit2([p, "--thread", "zz"], "--thread")
        self.assert_exit2([p, "--role", "gpu"], "--role")
        self.assert_exit2([p, "--timeline", "0"], "--timeline")
        self.assert_exit2([p, "--gaps", "-1"], "--gaps")
        self.assert_exit2([p, "--caller-depth", "0", "--callers", "x"], "--caller-depth")
        self.assert_exit2([p, "--from", "-1"], "--from")
        self.assert_exit2([p, "--by", "bogus"])
        self.assert_exit2([p, "--top", "0"])
        self.assert_exit2([])


class Cli(Base):
    def test_help_documents_every_option(self):
        r = subprocess.run([sys.executable, TOOL, "--help"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0)
        for opt in ("--from", "--to", "--thread", "--role", "--top", "--by", "--roles", "--timeline", "--guest",
                    "--gaps", "--runs", "--callers", "--caller-depth", "--json"):
            self.assertIn(opt, r.stdout)
        for role in xp.ROLES:
            self.assertIn(role, r.stdout)
        self.assertIn("func_XXXXXXXX", r.stdout)

    def test_subprocess_exit_codes(self):
        p = self.write(synthetic())
        ok = subprocess.run([sys.executable, TOOL, p, "--roles"], capture_output=True, text=True)
        self.assertEqual((ok.returncode, ok.stderr), (0, ""))
        bad = subprocess.run([sys.executable, TOOL, self.write("<a>", "bad.xml")], capture_output=True, text=True)
        self.assertEqual(bad.returncode, 2)
        self.assertNotIn("Traceback", bad.stderr)

    def test_json_schema_and_sections(self):
        p = self.write(synthetic())
        d = self.doc(p, "--roles", "--timeline", "2", "--guest", "--gaps", "10", "--callers", "mfc", "--runs", "2.5")
        self.assertEqual(d["schema"], xp.SCHEMA)
        for key in ("summary", "roles", "timeline", "guest", "gaps", "callers", "runs"):
            self.assertIn(key, d)
        self.assertNotIn("hotspots", d)      # a section flag replaces the default table
        self.assertIn("hotspots", self.doc(p, "--roles", "--by", "line"))


if __name__ == "__main__":
    unittest.main()
