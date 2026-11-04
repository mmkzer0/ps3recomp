#!/usr/bin/env python3
"""Contract tests for tools/hwprobe/ps3ftp.py with a fake FTP server object: every remote path stays
inside /dev_hdd0/tmp/hwprobe, and the fetch/delete selectors treat SELFs, configs and run-id
counters as non-results.

Run:  python3 tools/test_hwprobe_ps3ftp.py
"""
import ftplib
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "hwprobe"))

import ps3ftp


class FakeFtp:
    def __init__(self, files=None, dir_exists=True):
        self.files = dict(files or {})
        self.dir_exists = dir_exists
        self.deleted = []
        self.stored = {}
        self.made = []

    def retrlines(self, cmd, cb):
        if not self.dir_exists:
            raise ftplib.error_perm("550 no such directory")
        assert cmd == "NLST " + ps3ftp.REMOTE_DIR
        for n in self.files:
            cb(ps3ftp.REMOTE_DIR + "/" + n)  # webMAN lists full paths

    def retrbinary(self, cmd, cb):
        assert cmd.startswith("RETR " + ps3ftp.REMOTE_DIR + "/")
        cb(self.files[cmd.split("/")[-1]])

    def storbinary(self, cmd, fh):
        assert cmd.startswith("STOR " + ps3ftp.REMOTE_DIR + "/")
        self.stored[cmd] = fh.read()

    def delete(self, path):
        assert path.startswith(ps3ftp.REMOTE_DIR + "/")
        self.deleted.append(path)

    def mkd(self, path):
        self.made.append(path)
        raise ftplib.error_perm("550 exists")


CONSOLE = {"ring110_L1_r001_n8c2.csv": b"a", "ring110_last.txt": b"b", "ring110_runid.txt": b"1",
           "ring110.cfg": b"x", "ring110_l1.fake.self": b"s", "other_L1_r001.csv": b"c"}


class RemoteNames(unittest.TestCase):
    def test_names_that_could_leave_the_directory_are_refused(self):
        for bad in ("../x", "a/b", "/dev_flash/x", "..", "", ".hidden", "a..b", "x y", "x;y"):
            with self.assertRaises(ValueError, msg=bad):
                ps3ftp.remote_path(bad)

    def test_plain_names_map_into_the_directory(self):
        self.assertEqual(ps3ftp.remote_path("ring110.cfg"), "/dev_hdd0/tmp/hwprobe/ring110.cfg")


class ResultSelection(unittest.TestCase):
    def test_fetch_takes_results_only_and_honours_the_prefix(self):
        with tempfile.TemporaryDirectory() as d:
            got = ps3ftp.fetch_results(FakeFtp(CONSOLE), d, "ring110")
            self.assertEqual(sorted(os.path.basename(p) for p in got),
                             ["ring110_L1_r001_n8c2.csv", "ring110_last.txt"])
            got = ps3ftp.fetch_results(FakeFtp(CONSOLE), d)
            self.assertEqual(len(got), 3)  # the other probe's csv comes along without a prefix

    def test_delete_never_touches_selfs_configs_or_run_counters(self):
        ftp = FakeFtp(CONSOLE)
        gone = ps3ftp.delete_results(ftp, "ring110")
        self.assertEqual(sorted(gone), ["ring110_L1_r001_n8c2.csv", "ring110_last.txt"])
        for n in ("ring110.cfg", "ring110_l1.fake.self", "ring110_runid.txt"):
            self.assertNotIn(ps3ftp.REMOTE_DIR + "/" + n, ftp.deleted)

    def test_missing_remote_directory_means_no_results(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(ps3ftp.fetch_results(FakeFtp(dir_exists=False), d), [])


class Upload(unittest.TestCase):
    def test_upload_creates_the_directory_and_stays_inside(self):
        ftp = FakeFtp()
        with tempfile.NamedTemporaryFile(delete=False) as fh:
            fh.write(b"cfg")
        try:
            ps3ftp.upload(ftp, fh.name, "ring110.cfg")
        finally:
            os.unlink(fh.name)
        self.assertEqual(ftp.made, [ps3ftp.REMOTE_DIR])
        self.assertEqual(ftp.stored, {"STOR /dev_hdd0/tmp/hwprobe/ring110.cfg": b"cfg"})

    def test_cli_refuses_a_bad_remote_name_before_connecting(self):
        with self.assertRaises(SystemExit):
            ps3ftp.main(["--host", "192.0.2.1", "put", "x.self", "../evil"])

    def test_cli_needs_a_host(self):
        old = os.environ.pop("HWPROBE_CONSOLE", None)
        try:
            with self.assertRaises(SystemExit):
                ps3ftp.main(["ls"])
        finally:
            if old is not None:
                os.environ["HWPROBE_CONSOLE"] = old


if __name__ == "__main__":
    unittest.main(verbosity=2)
