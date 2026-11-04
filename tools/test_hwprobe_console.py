#!/usr/bin/env python3
"""Contract tests for tools/hwprobe/ps3console.py with local fake HTTP server and fake FTP layer.

Verifies:
  1. Strict allow-list enforcement: only /mount.ps3/dev_hdd0/tmp/hwprobe/<name>,
     /mount.ps3/unmount, and /play.ps3 are permitted; /restart.ps3, /reboot.ps3,
     /shutdown.ps3 and path traversals are refused.
  2. Defect 1 fix (RETRY WHILE RUNNING): a probe running longer than --run-window
     is NEVER played twice once file activity (e.g. _runid.txt or result file) is seen.
  3. 8001003A recovery: a silent first play (no file changes within run_window) gets
     exactly one retry.
  4. Defect 2 fix (PARTIAL FETCH): a result file appearing in listing is NOT fetched
     until _last.txt has changed and its first line names that result file.
  5. Multi-run (--runs N): waits until N result files exist AND _last.txt names the last.
  6. Timeout: 3 silent play attempts followed by unmount and TimeoutError.

Determinism: every test injects a FakeClock into run_probe. Time on that clock advances
only when the tool sleeps (poll interval or FTP reconnect backoff), so no test depends on
machine speed. Fake-FTP state changes are triggered by the tool's own actions (the play
request, the Nth directory listing, or a point in fake time reached by the tool's sleeps),
never by a thread sleeping on real time. The only thread is the HTTP server's, and the tool
blocks on each request until the handler (including its callback) has finished.

Run: python3 tools/test_hwprobe_console.py
"""
import ftplib
import http.server
import os
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "hwprobe"))

import ps3console
import ps3ftp

LAST_R001 = b"/dev_hdd0/tmp/hwprobe/test_probe_L1_r001.csv\nabort_reason=0\n"
LAST_R002 = b"/dev_hdd0/tmp/hwprobe/test_probe_L1_r002.csv\nabort_reason=0\n"


class FakeClock:
    """Deterministic monotonic clock; time moves only through sleep().

    Events scheduled with after() fire from inside sleep(), in due-time order, once the
    clock has reached them, so the tool observes them on its next poll.
    """

    def __init__(self):
        self.now = 0.0
        self.sleeps = []
        self._events = []

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        assert seconds > 0, "a zero sleep would spin forever on a fake clock"
        self.sleeps.append(seconds)
        self.now += seconds
        due = sorted((e for e in self._events if e[0] <= self.now), key=lambda e: e[0])
        self._events = [e for e in self._events if e[0] > self.now]
        for _, fn in due:
            fn()

    def after(self, delay, fn):
        self._events.append((self.now + delay, fn))


class FakeFtp:
    """In-memory mock FTP server matching webMAN MOD listing conventions."""

    def __init__(self, files=None):
        self.files = dict(files or {})
        self.stored = {}
        self.deleted = []
        self.connected = False
        self.logged_in = False
        self.pasv = False
        self.drop_next_operation = False
        self.refuse_connect = False
        self.reconnect_count = 0
        self.listing_count = 0
        # Called with the 1-based NLST count before each listing is produced.
        self.on_listing = None
        # Names passed to RETR, in order (includes the _runid/_last polling reads).
        self.retrieved = []

    @property
    def fetched_results(self):
        """RETR'd result files only (the downloads, not the _runid/_last polling reads)."""
        return [n for n in self.retrieved if n.endswith(".csv")]

    def connect(self, host, port=21, timeout=10):
        if self.refuse_connect:
            raise OSError("Connection refused")
        self.connected = True
        self.reconnect_count += 1
        self.logged_in = True

    def set_pasv(self, val):
        self.pasv = val

    def retrlines(self, cmd, cb):
        if self.drop_next_operation:
            self.drop_next_operation = False
            raise EOFError("")
        assert cmd == "NLST " + ps3ftp.REMOTE_DIR
        self.listing_count += 1
        if self.on_listing:
            self.on_listing(self.listing_count)
        for name in list(self.files.keys()):
            cb(ps3ftp.REMOTE_DIR + "/" + name)

    def retrbinary(self, cmd, cb):
        assert cmd.startswith("RETR " + ps3ftp.REMOTE_DIR + "/")
        name = cmd.split("/")[-1]
        if name not in self.files:
            raise ftplib.error_perm(f"550 {name}: not found")
        self.retrieved.append(name)
        cb(self.files[name])

    def storbinary(self, cmd, fh):
        assert cmd.startswith("STOR " + ps3ftp.REMOTE_DIR + "/")
        data = fh.read()
        self.stored[cmd] = data
        name = cmd.split("/")[-1]
        self.files[name] = data

    def size(self, path):
        assert path.startswith(ps3ftp.REMOTE_DIR + "/")
        name = path.split("/")[-1]
        if name in self.files:
            return len(self.files[name])
        raise ftplib.error_perm(f"550 {name}: not found")

    def delete(self, path):
        assert path.startswith(ps3ftp.REMOTE_DIR + "/")
        name = path.split("/")[-1]
        self.deleted.append(path)
        self.files.pop(name, None)

    def mkd(self, path):
        pass

    def quit(self):
        self.connected = False


class FakeHttpServer:
    """Threaded local HTTP server on 127.0.0.1 with an ephemeral port."""

    def __init__(self, on_play_callback=None, on_get_callback=None):
        self.requested_paths = []
        self.play_count = 0
        self.on_play_callback = on_play_callback
        self.on_get_callback = on_get_callback

        server_instance = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                server_instance.requested_paths.append(self.path)
                if server_instance.on_get_callback:
                    server_instance.on_get_callback(self.path)
                if self.path == "/play.ps3":
                    server_instance.play_count += 1
                    if server_instance.on_play_callback:
                        server_instance.on_play_callback(server_instance.play_count)
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(b"OK")

            def log_message(self, format, *args):
                pass  # suppress HTTP logging during tests

        self.server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(
            target=self.server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True
        )
        self.thread.start()

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2.0)


class TestHttpAllowList(unittest.TestCase):
    def test_allowed_paths_pass(self):
        self.assertEqual(
            ps3console.check_http_path("/mount.ps3/dev_hdd0/tmp/hwprobe/probe.fake.self"),
            "/mount.ps3/dev_hdd0/tmp/hwprobe/probe.fake.self",
        )
        self.assertEqual(
            ps3console.check_http_path("/mount.ps3/dev_hdd0/tmp/hwprobe/lv2conf.fake.self"),
            "/mount.ps3/dev_hdd0/tmp/hwprobe/lv2conf.fake.self",
        )
        self.assertEqual(ps3console.check_http_path("/mount.ps3/unmount"), "/mount.ps3/unmount")
        self.assertEqual(ps3console.check_http_path("/play.ps3"), "/play.ps3")

    def test_forbidden_paths_are_refused(self):
        forbidden = [
            "/restart.ps3",
            "/reboot.ps3",
            "/shutdown.ps3",
            "/mount.ps3/dev_hdd0/tmp/hwprobe/../dev_flash",
            "/mount.ps3/dev_hdd0/tmp/hwprobe/..",
            "/mount.ps3/dev_hdd0/tmp/hwprobe/subdir/file.self",
            "/mount.ps3/dev_hdd0/tmp/hwprobe/file.self?cmd=reboot",
            "/mount.ps3/dev_hdd0/tmp/hwprobe/file.self#fragment",
            "/mount.ps3/dev_hdd0/tmp/hwprobe/",
            "/play.ps3?extra=1",
            "/setup.ps3",
            "/cpursx.ps3",
            "",
            "not_a_path",
        ]
        for path in forbidden:
            with self.assertRaises(ValueError, msg=f"Should have refused: {path}"):
                ps3console.check_http_path(path)


class TestConsoleRunner(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.self_path = os.path.join(self.temp_dir.name, "test_probe.fake.self")
        with open(self.self_path, "wb") as f:
            f.write(b"FAKE_SELF_BYTES")
        self.cfg_path = os.path.join(self.temp_dir.name, "test_probe.cfg")
        with open(self.cfg_path, "wb") as f:
            f.write(b"runs=1\n")
        self.out_dir = os.path.join(self.temp_dir.name, "results")
        self.clock = FakeClock()
        self.fake_ftp = FakeFtp()

    def tearDown(self):
        self.temp_dir.cleanup()

    def run_probe(self, http_server, **overrides):
        """Run the tool against the fakes with the fake clock; times are in fake seconds."""
        kwargs = {
            "self_path": self.self_path,
            "prefix": "test_probe",
            "run_window": 60.0,
            "runs": 1,
            "timeout": 500.0,
            "out_dir": self.out_dir,
            "host": "127.0.0.1",
            "http_port": http_server.port,
            "poll_interval": 10.0,
            "reconnect_backoff": 1.0,
            "ftp_client": self.fake_ftp,
            "clock": self.clock,
            "sleep": self.clock.sleep,
        }
        kwargs.update(overrides)
        return ps3console.run_probe(**kwargs)

    def write_run1(self):
        self.fake_ftp.files["test_probe_runid.txt"] = b"1\n"
        self.fake_ftp.files["test_probe_L1_r001.csv"] = b"HWPROBE,1\nHDR,probe,test\n"
        self.fake_ftp.files["test_probe_last.txt"] = LAST_R001

    def test_probe_running_longer_than_window_never_played_twice(self):
        """Defect 1: A probe that takes longer than --run-window must NOT get a second play.

        Fake-time timeline after play: activity (_runid.txt) at +20s, result and
        _last.txt at +120s, run_window 60s, poll every 10s. Polls at +60s..+110s see
        activity but no completion; play must still be pressed exactly ONCE.
        """
        fake_ftp = self.fake_ftp

        def write_runid():
            fake_ftp.files["test_probe_runid.txt"] = b"1\n"

        def write_result():
            fake_ftp.files["test_probe_L1_r001.csv"] = b"HWPROBE,1\nHDR,probe,test\n"
            fake_ftp.files["test_probe_last.txt"] = LAST_R001

        def on_play(count):
            self.clock.after(20.0, write_runid)
            self.clock.after(120.0, write_result)

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            got = self.run_probe(http_server, run_window=60.0)
            self.assertEqual(len(got), 1)
            # The run really outlasted the window, so the guard was exercised.
            self.assertGreater(self.clock.now, 60.0)
            # Critical assertion: play was called only once even though run took > run_window!
            self.assertEqual(http_server.play_count, 1)

            expected_paths = [
                "/mount.ps3/dev_hdd0/tmp/hwprobe/test_probe.fake.self",
                "/play.ps3",
                "/mount.ps3/unmount",
            ]
            self.assertEqual(http_server.requested_paths, expected_paths)
        finally:
            http_server.stop()

    def test_activity_first_seen_after_window_via_reconnect_backoff_not_replayed(self):
        """A slow FTP reconnect may delay the first sight of activity past run_window.

        The play drops the FTP connection; the reconnect backoff (70s of fake time) sleeps
        through the whole 60s window while _runid.txt appears at +20s. The first poll that
        sees activity happens after the window had elapsed; play must still be pressed once.
        """
        fake_ftp = self.fake_ftp

        def on_play(count):
            fake_ftp.drop_next_operation = True
            self.clock.after(20.0, lambda: fake_ftp.files.__setitem__("test_probe_runid.txt", b"1\n"))
            self.clock.after(200.0, self.write_run1)

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            got = self.run_probe(http_server, run_window=60.0, reconnect_backoff=70.0)
            self.assertEqual(len(got), 1)
            self.assertGreater(fake_ftp.reconnect_count, 1)
            self.assertIn(70.0, self.clock.sleeps)
            self.assertEqual(http_server.play_count, 1)
        finally:
            http_server.stop()

    def test_silent_first_play_gets_exactly_one_retry(self):
        """8001003A recovery: If no file changes occur within run_window, retries play once."""

        def on_play(count):
            if count == 2:
                # Result appears only on second play attempt
                self.write_run1()

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            got = self.run_probe(http_server, run_window=50.0)
            self.assertEqual(len(got), 1)
            self.assertEqual(http_server.play_count, 2)
            # Retry only after a full run window of silence, never earlier.
            self.assertGreaterEqual(self.clock.now, 50.0)

            expected_paths = [
                "/mount.ps3/dev_hdd0/tmp/hwprobe/test_probe.fake.self",
                "/play.ps3",
                "/play.ps3",
                "/mount.ps3/unmount",
            ]
            self.assertEqual(http_server.requested_paths, expected_paths)
        finally:
            http_server.stop()

    def test_result_listed_before_last_txt_not_fetched_until_updated(self):
        """Defect 2: Result file listed before _last.txt is updated must NOT trigger fetch."""
        fake_ftp = self.fake_ftp
        order = []
        listings_with_result_but_no_last = []

        def on_play(count):
            # Step 1: result file appears in listing (e.g. opened for write)
            fake_ftp.files["test_probe_runid.txt"] = b"1\n"
            fake_ftp.files["test_probe_L1_r001.csv"] = b"HWPROBE,1\n"

        def on_listing(n):
            # n counts every NLST; #1 is the initial baseline listing, so #2 and #3 are the
            # first two polls after play. _last.txt lands only at poll #4 (the third poll).
            if n == 4:
                order.append("write_last")
                fake_ftp.files["test_probe_last.txt"] = LAST_R001
            if "test_probe_L1_r001.csv" in fake_ftp.files and "test_probe_last.txt" not in fake_ftp.files:
                listings_with_result_but_no_last.append(n)

        fake_ftp.on_listing = on_listing

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            got = self.run_probe(http_server, run_window=200.0)
            self.assertEqual(len(got), 1)
            # The tool saw the result listed with no _last.txt twice and did not fetch.
            self.assertEqual(listings_with_result_but_no_last, [2, 3])
            self.assertEqual(order, ["write_last"])
            self.assertEqual(fake_ftp.fetched_results, ["test_probe_L1_r001.csv"])
            self.assertEqual(fake_ftp.listing_count, 4)
            self.assertIn("test_probe_last.txt", fake_ftp.files)
        finally:
            http_server.stop()

    def test_n_runs_waits_for_all_and_last_names_last(self):
        """Multi-run (--runs N): waits until N results exist AND _last.txt names the last."""
        fake_ftp = self.fake_ftp
        run2_written = False

        def write_run2():
            nonlocal run2_written
            run2_written = True
            fake_ftp.files["test_probe_runid.txt"] = b"2\n"
            fake_ftp.files["test_probe_L1_r002.csv"] = b"run2\n"
            fake_ftp.files["test_probe_last.txt"] = LAST_R002

        def on_play(count):
            # Run 1 produces file 1 right after play
            fake_ftp.files["test_probe_runid.txt"] = b"1\n"
            fake_ftp.files["test_probe_L1_r001.csv"] = b"run1\n"
            fake_ftp.files["test_probe_last.txt"] = LAST_R001

        def on_listing(n):
            # Run 2 is produced only after the tool has polled run 1's complete state twice.
            if n == 4:
                write_run2()

        fake_ftp.on_listing = on_listing

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            got = self.run_probe(http_server, runs=2, run_window=200.0)
            self.assertEqual(len(got), 2)
            self.assertTrue(got[0].endswith("test_probe_L1_r001.csv"))
            self.assertTrue(got[1].endswith("test_probe_L1_r002.csv"))
            # The tool did not stop at run 1 even though _last.txt was already updated.
            self.assertTrue(run2_written)
            self.assertEqual(fake_ftp.fetched_results, ["test_probe_L1_r001.csv", "test_probe_L1_r002.csv"])
        finally:
            http_server.stop()

    def test_timeout_after_3_silent_plays(self):
        """All 3 plays silent -> exactly 3 plays, unmount called, TimeoutError raised."""
        http_server = FakeHttpServer(on_play_callback=None)
        try:
            with self.assertRaises(TimeoutError) as ctx:
                self.run_probe(http_server, run_window=30.0, timeout=200.0)
            self.assertIn("TIMEOUT", str(ctx.exception))
            self.assertIn("3 play attempts", str(ctx.exception))

            expected_paths = [
                "/mount.ps3/dev_hdd0/tmp/hwprobe/test_probe.fake.self",
                "/play.ps3",
                "/play.ps3",
                "/play.ps3",
                "/mount.ps3/unmount",
            ]
            self.assertEqual(http_server.requested_paths, expected_paths)
            self.assertEqual(http_server.play_count, 3)
        finally:
            http_server.stop()

    def test_ftp_connection_drop_after_play_reconnects_and_completes(self):
        """Real-console issue (1): webMAN drops FTP control connection 4-6s after /play.ps3.

        A dropped connection (EOFError) during the poll loop must trigger a reconnect
        and allow the run to complete without unmounting or crashing with FATAL:.
        """
        fake_ftp = self.fake_ftp

        def on_play(count):
            # webMAN drops connection after play
            fake_ftp.drop_next_operation = True
            # Probe completes after the drop
            self.write_run1()

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            got = self.run_probe(http_server, run_window=200.0, reconnect_backoff=1.0)
            self.assertEqual(len(got), 1)
            self.assertGreater(fake_ftp.reconnect_count, 1)
            # The reconnect backoff went through the injected sleep.
            self.assertIn(1.0, self.clock.sleeps)
        finally:
            http_server.stop()

    def test_unmount_not_requested_before_fetch(self):
        """Real-console issue (2): unmount must ONLY happen after result files are fetched."""
        expected_dest = os.path.join(self.out_dir, "test_probe_L1_r001.csv")
        file_present_at_unmount = None

        def on_play(count):
            self.write_run1()

        def on_get(path):
            nonlocal file_present_at_unmount
            if path == "/mount.ps3/unmount":
                file_present_at_unmount = os.path.isfile(expected_dest)

        http_server = FakeHttpServer(on_play_callback=on_play, on_get_callback=on_get)
        try:
            got = self.run_probe(http_server, run_window=100.0)
            self.assertEqual(len(got), 1)
            # File MUST have been present in out_dir at the exact moment unmount was requested!
            self.assertIsNotNone(file_present_at_unmount)
            self.assertTrue(file_present_at_unmount)
        finally:
            http_server.stop()

    def test_unmount_not_requested_on_timeout_after_activity(self):
        """Probe activity seen -> on timeout, leave mounted, do not send unmount/exit."""
        fake_ftp = self.fake_ftp

        def on_play(count):
            # Probe starts: _runid.txt is updated
            fake_ftp.files["test_probe_runid.txt"] = b"1\n"
            # But probe stalls and never finishes before timeout

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            with self.assertRaises(TimeoutError) as ctx:
                self.run_probe(http_server, run_window=50.0, timeout=80.0)
            self.assertIn("console left mounted; probe may still be running", str(ctx.exception))
            # Critical assertion: /mount.ps3/unmount was NEVER requested!
            self.assertNotIn("/mount.ps3/unmount", http_server.requested_paths)
            self.assertEqual(http_server.play_count, 1)
        finally:
            http_server.stop()

    def test_refused_reconnect_until_timeout_gives_named_error(self):
        """Refused FTP reconnect until timeout gives a clear, named TimeoutError."""
        fake_ftp = self.fake_ftp

        def on_play(count):
            fake_ftp.drop_next_operation = True
            fake_ftp.refuse_connect = True

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            with self.assertRaises(TimeoutError) as ctx:
                self.run_probe(http_server, run_window=50.0, timeout=80.0, reconnect_backoff=10.0)
            err = str(ctx.exception)
            self.assertIn("TIMEOUT", err)
            self.assertTrue("failed to connect to console FTP" in err or "Connection refused" in err)
            # Backoff sleeps used the fake clock and stopped at the deadline.
            self.assertGreaterEqual(self.clock.now, 80.0)
        finally:
            http_server.stop()


if __name__ == "__main__":
    unittest.main(verbosity=2)
