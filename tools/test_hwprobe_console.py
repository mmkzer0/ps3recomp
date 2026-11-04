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

Run: python3 tools/test_hwprobe_console.py
"""
import ftplib
import http.server
import os
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "hwprobe"))

import ps3console
import ps3ftp


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
        for name in list(self.files.keys()):
            cb(ps3ftp.REMOTE_DIR + "/" + name)
    def retrbinary(self, cmd, cb):
        assert cmd.startswith("RETR " + ps3ftp.REMOTE_DIR + "/")
        name = cmd.split("/")[-1]
        if name not in self.files:
            raise ftplib.error_perm(f"550 {name}: not found")
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
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
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

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_probe_running_longer_than_window_never_played_twice(self):
        """Defect 1: A probe that takes longer than --run-window must NOT get a second play.

        Simulates a probe where activity (_runid.txt) appears during run_window, but the
        execution continues beyond run_window before writing the result and _last.txt.
        Verify play is called exactly ONCE.
        """
        fake_ftp = FakeFtp()
        run_window = 0.06

        def on_play(count):
            def simulate_probe_progress():
                # Step 1: probe starts, updates _runid.txt quickly (e.g. 20ms)
                time.sleep(0.02)
                fake_ftp.files["test_probe_runid.txt"] = b"1\n"
                # Step 2: probe runs past the run_window (e.g. total 120ms > 60ms)
                time.sleep(0.10)
                fake_ftp.files["test_probe_L1_r001.csv"] = b"HWPROBE,1\nHDR,probe,test\n"
                fake_ftp.files["test_probe_last.txt"] = (
                    b"/dev_hdd0/tmp/hwprobe/test_probe_L1_r001.csv\nabort_reason=0\n"
                )

            t = threading.Thread(target=simulate_probe_progress, daemon=True)
            t.start()

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            got = ps3console.run_probe(
                self_path=self.self_path,
                prefix="test_probe",
                run_window=run_window,
                runs=1,
                timeout=5.0,
                out_dir=self.out_dir,
                host="127.0.0.1",
                http_port=http_server.port,
                poll_interval=0.01,
                ftp_client=fake_ftp,
            )
            self.assertEqual(len(got), 1)
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

    def test_silent_first_play_gets_exactly_one_retry(self):
        """8001003A recovery: If no file changes occur within run_window, retries play once."""
        fake_ftp = FakeFtp()
        run_window = 0.05

        def on_play(count):
            if count == 2:
                # Result appears only on second play attempt
                fake_ftp.files["test_probe_runid.txt"] = b"1\n"
                fake_ftp.files["test_probe_L1_r001.csv"] = b"HWPROBE,1\nHDR,probe,test\n"
                fake_ftp.files["test_probe_last.txt"] = (
                    b"/dev_hdd0/tmp/hwprobe/test_probe_L1_r001.csv\nabort_reason=0\n"
                )

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            got = ps3console.run_probe(
                self_path=self.self_path,
                prefix="test_probe",
                run_window=run_window,
                runs=1,
                timeout=5.0,
                out_dir=self.out_dir,
                host="127.0.0.1",
                http_port=http_server.port,
                poll_interval=0.01,
                ftp_client=fake_ftp,
            )
            self.assertEqual(len(got), 1)
            self.assertEqual(http_server.play_count, 2)

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
        fake_ftp = FakeFtp()

        def on_play(count):
            def simulate_staged_output():
                # Step 1: result file appears in listing (e.g. opened for write)
                fake_ftp.files["test_probe_runid.txt"] = b"1\n"
                fake_ftp.files["test_probe_L1_r001.csv"] = b"HWPROBE,1\n"
                time.sleep(0.08)

                # Step 2: _last.txt is finally written after fclose
                fake_ftp.files["test_probe_last.txt"] = (
                    b"/dev_hdd0/tmp/hwprobe/test_probe_L1_r001.csv\nabort_reason=0\n"
                )

            t = threading.Thread(target=simulate_staged_output, daemon=True)
            t.start()

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            got = ps3console.run_probe(
                self_path=self.self_path,
                prefix="test_probe",
                run_window=0.2,
                runs=1,
                timeout=5.0,
                out_dir=self.out_dir,
                host="127.0.0.1",
                http_port=http_server.port,
                poll_interval=0.01,
                ftp_client=fake_ftp,
            )
            self.assertEqual(len(got), 1)
            # Verify that when fetched, _last.txt had been properly written
            self.assertIn("test_probe_last.txt", fake_ftp.files)
        finally:
            http_server.stop()

    def test_n_runs_waits_for_all_and_last_names_last(self):
        """Multi-run (--runs N): waits until N results exist AND _last.txt names the last."""
        fake_ftp = FakeFtp()

        def on_play(count):
            def simulate_two_runs():
                time.sleep(0.02)
                # Run 1 produces file 1
                fake_ftp.files["test_probe_runid.txt"] = b"1\n"
                fake_ftp.files["test_probe_L1_r001.csv"] = b"run1\n"
                fake_ftp.files["test_probe_last.txt"] = (
                    b"/dev_hdd0/tmp/hwprobe/test_probe_L1_r001.csv\nabort_reason=0\n"
                )
                time.sleep(0.06)
                # Run 2 produces file 2
                fake_ftp.files["test_probe_runid.txt"] = b"2\n"
                fake_ftp.files["test_probe_L1_r002.csv"] = b"run2\n"
                fake_ftp.files["test_probe_last.txt"] = (
                    b"/dev_hdd0/tmp/hwprobe/test_probe_L1_r002.csv\nabort_reason=0\n"
                )

            t = threading.Thread(target=simulate_two_runs, daemon=True)
            t.start()

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            got = ps3console.run_probe(
                self_path=self.self_path,
                prefix="test_probe",
                run_window=0.2,
                runs=2,
                timeout=5.0,
                out_dir=self.out_dir,
                host="127.0.0.1",
                http_port=http_server.port,
                poll_interval=0.01,
                ftp_client=fake_ftp,
            )
            self.assertEqual(len(got), 2)
            self.assertTrue(got[0].endswith("test_probe_L1_r001.csv"))
            self.assertTrue(got[1].endswith("test_probe_L1_r002.csv"))
        finally:
            http_server.stop()

    def test_timeout_after_3_silent_plays(self):
        """All 3 plays silent -> exactly 3 plays, unmount called, TimeoutError raised."""
        fake_ftp = FakeFtp()
        http_server = FakeHttpServer(on_play_callback=None)
        try:
            with self.assertRaises(TimeoutError) as ctx:
                ps3console.run_probe(
                    self_path=self.self_path,
                    prefix="test_probe",
                    run_window=0.03,
                    runs=1,
                    timeout=5.0,
                    out_dir=self.out_dir,
                    host="127.0.0.1",
                    http_port=http_server.port,
                    poll_interval=0.01,
                    ftp_client=fake_ftp,
                )
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
        fake_ftp = FakeFtp()

        def on_play(count):
            # webMAN drops connection after play
            fake_ftp.drop_next_operation = True
            # Probe completes after the drop
            fake_ftp.files["test_probe_runid.txt"] = b"1\n"
            fake_ftp.files["test_probe_L1_r001.csv"] = b"HWPROBE,1\nHDR,probe,test\n"
            fake_ftp.files["test_probe_last.txt"] = (
                b"/dev_hdd0/tmp/hwprobe/test_probe_L1_r001.csv\nabort_reason=0\n"
            )

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            got = ps3console.run_probe(
                self_path=self.self_path,
                prefix="test_probe",
                run_window=0.2,
                runs=1,
                timeout=5.0,
                out_dir=self.out_dir,
                host="127.0.0.1",
                http_port=http_server.port,
                poll_interval=0.01,
                ftp_client=fake_ftp,
            )
            self.assertEqual(len(got), 1)
            self.assertGreater(fake_ftp.reconnect_count, 1)
        finally:
            http_server.stop()

    def test_unmount_not_requested_before_fetch(self):
        """Real-console issue (2): unmount must ONLY happen after result files are fetched."""
        fake_ftp = FakeFtp()
        expected_dest = os.path.join(self.out_dir, "test_probe_L1_r001.csv")
        file_present_at_unmount = None

        def on_play(count):
            fake_ftp.files["test_probe_runid.txt"] = b"1\n"
            fake_ftp.files["test_probe_L1_r001.csv"] = b"HWPROBE,1\nHDR,probe,test\n"
            fake_ftp.files["test_probe_last.txt"] = (
                b"/dev_hdd0/tmp/hwprobe/test_probe_L1_r001.csv\nabort_reason=0\n"
            )

        def on_get(path):
            nonlocal file_present_at_unmount
            if path == "/mount.ps3/unmount":
                file_present_at_unmount = os.path.isfile(expected_dest)

        http_server = FakeHttpServer(on_play_callback=on_play, on_get_callback=on_get)
        try:
            got = ps3console.run_probe(
                self_path=self.self_path,
                prefix="test_probe",
                run_window=0.1,
                runs=1,
                timeout=5.0,
                out_dir=self.out_dir,
                host="127.0.0.1",
                http_port=http_server.port,
                poll_interval=0.01,
                reconnect_backoff=0.01,
                ftp_client=fake_ftp,
            )
            self.assertEqual(len(got), 1)
            # File MUST have been present in out_dir at the exact moment unmount was requested!
            self.assertIsNotNone(file_present_at_unmount)
            self.assertTrue(file_present_at_unmount)
        finally:
            http_server.stop()

    def test_unmount_not_requested_on_timeout_after_activity(self):
        """Probe activity seen -> on timeout, leave mounted, do not send unmount/exit."""
        fake_ftp = FakeFtp()

        def on_play(count):
            # Probe starts: _runid.txt is updated
            fake_ftp.files["test_probe_runid.txt"] = b"1\n"
            # But probe stalls and never finishes before timeout

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            with self.assertRaises(TimeoutError) as ctx:
                ps3console.run_probe(
                    self_path=self.self_path,
                    prefix="test_probe",
                    run_window=0.05,
                    runs=1,
                    timeout=0.08,
                    out_dir=self.out_dir,
                    host="127.0.0.1",
                    http_port=http_server.port,
                    poll_interval=0.01,
                    reconnect_backoff=0.01,
                    ftp_client=fake_ftp,
                )
            self.assertIn("console left mounted; probe may still be running", str(ctx.exception))
            # Critical assertion: /mount.ps3/unmount was NEVER requested!
            self.assertNotIn("/mount.ps3/unmount", http_server.requested_paths)
        finally:
            http_server.stop()

    def test_refused_reconnect_until_timeout_gives_named_error(self):
        """Refused FTP reconnect until timeout gives a clear, named TimeoutError."""
        fake_ftp = FakeFtp()

        def on_play(count):
            fake_ftp.drop_next_operation = True
            fake_ftp.refuse_connect = True

        http_server = FakeHttpServer(on_play_callback=on_play)
        try:
            with self.assertRaises(TimeoutError) as ctx:
                ps3console.run_probe(
                    self_path=self.self_path,
                    prefix="test_probe",
                    run_window=0.05,
                    runs=1,
                    timeout=0.08,
                    out_dir=self.out_dir,
                    host="127.0.0.1",
                    http_port=http_server.port,
                    poll_interval=0.01,
                    reconnect_backoff=0.01,
                    ftp_client=fake_ftp,
                )
            err = str(ctx.exception)
            self.assertIn("TIMEOUT", err)
            self.assertTrue("failed to connect to console FTP" in err or "Connection refused" in err)
        finally:
            http_server.stop()


if __name__ == "__main__":
    unittest.main(verbosity=2)
