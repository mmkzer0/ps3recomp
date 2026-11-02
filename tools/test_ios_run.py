#!/usr/bin/env python3
"""Contract tests for tools/ios_run.py.

Every case runs the tool against a fake `xcrun`: a shell script that logs
its argv and environment and answers the few simctl and devicectl commands
the tool uses.  No real simulator, device or Xcode is involved, and PATH is
the fake directory alone so the real xcrun is unreachable.

The fake's log is the observable contract: which commands ran, in what
order, with which prefixed environment.
"""

import contextlib
import fcntl
import io
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

TOOL = Path(__file__).resolve().with_name("ios_run.py")

sys.path.insert(0, str(TOOL.parent))
import ios_run  # noqa: E402  (pump and decide_outcome are exercised in-process)

UDID = "AAAA1111-2222-3333-4444-555566667777"
OTHER_UDID = "BBBB1111-2222-3333-4444-555566667777"
RUNTIME = "com.apple.CoreSimulator.SimRuntime.iOS-26-0"
OLD_RUNTIME = "com.apple.CoreSimulator.SimRuntime.iOS-17-5"

CAT = shutil.which("cat") or "/bin/cat"
ENV = shutil.which("env") or "/usr/bin/env"
SLEEP = shutil.which("sleep") or "/bin/sleep"

BUNDLE_ID = "com.example.title"
BUNDLE_DIR = "/private/var/containers/Bundle/Application/1234ABCD/My Title.app/"
APP_PID = 4242


def file_url(path):
    return "file://" + path.replace(" ", "%20")


APP_EXE = file_url(BUNDLE_DIR + "Title")
PLUGIN_EXE = file_url(BUNDLE_DIR + "PlugIns/Share.appex/Share")
OTHER_EXE = file_url("/private/var/containers/Bundle/Application/9999FFFF/Other.app/Other")
LAUNCHD_EXE = "file:///sbin/launchd"
LAUNCHED = ("Launched application with %s bundle identifier.\n"
            "Waiting for the application to terminate...\n" % BUNDLE_ID)


def apps_json():
    return json.dumps({"result": {"apps": [
        {"bundleIdentifier": BUNDLE_ID, "url": file_url(BUNDLE_DIR)}]}})


def procs_json(entries):
    return json.dumps({"result": {"runningProcesses": [
        {"processIdentifier": pid, "executable": exe} for pid, exe in entries]}})


# The fake answers only these commands.  Anything else exits 64, so a test
# that sees a new command fails loudly instead of pretending it worked.
FAKE = """#!/bin/sh
{
  printf 'CALL\\n'
  printf 'arg0=%s\\n' "$0"
  for arg in "$@"; do printf 'arg=%s\\n' "$arg"; done
  printf 'ENV\\n'
  @ENV@
  printf 'END\\n'
} >> "$FAKE_LOG"

case "$1 $2" in
  "simctl list")
    @CAT@ "$FAKE_DEVICES"
    exit 0
    ;;
  "simctl boot" | "simctl bootstatus" | "simctl shutdown")
    exit 0
    ;;
  "simctl spawn")
    [ -n "$FAKE_OUT" ] && @CAT@ "$FAKE_OUT"
    [ -n "$FAKE_ERR" ] && @CAT@ "$FAKE_ERR" >&2
    [ -n "$FAKE_PIDFILE" ] && echo $$ > "$FAKE_PIDFILE"
    [ -n "$FAKE_SLEEP" ] && @SLEEP@ "$FAKE_SLEEP"
    [ -n "$FAKE_SIGNAL" ] && kill -s "$FAKE_SIGNAL" $$
    exit "${FAKE_EXIT:-0}"
    ;;
  "devicectl device")
    out=""
    prev=""
    for arg in "$@"; do
      [ "$prev" = "--json-output" ] && out="$arg"
      prev="$arg"
    done
    case "$3 $4" in
      "install app")
        exit "${FAKE_INSTALL_EXIT:-0}"
        ;;
      "info apps")
        [ -n "$out" ] && @CAT@ "$FAKE_APPS" > "$out"
        exit "${FAKE_APPS_EXIT:-0}"
        ;;
      "info processes")
        src="$FAKE_PROCS"
        [ -e "$FAKE_STATE/dead" ] && src="$FAKE_PROCS_DEAD"
        [ -n "$out" ] && @CAT@ "$src" > "$out"
        exit "${FAKE_PROCS_EXIT:-0}"
        ;;
      "process launch")
        [ -n "$FAKE_OUT" ] && @CAT@ "$FAKE_OUT"
        [ -n "$FAKE_PIDFILE" ] && echo $$ > "$FAKE_PIDFILE"
        [ -n "$FAKE_SLEEP" ] && @SLEEP@ "$FAKE_SLEEP"
        [ -n "$FAKE_SIGNAL" ] && kill -s "$FAKE_SIGNAL" $$
        exit "${FAKE_LAUNCH_EXIT:-0}"
        ;;
      "process terminate")
        case "${FAKE_TERM:-die}" in
          die) : > "$FAKE_STATE/dead" ;;
          fail) exit 1 ;;
          hang) @SLEEP@ 60 ;;
        esac
        exit 0
        ;;
      "process signal")
        case "${FAKE_KILL:-die}" in
          die) : > "$FAKE_STATE/dead" ;;
          fail) exit 1 ;;
          hang) @SLEEP@ 60 ;;
        esac
        exit 0
        ;;
    esac
    ;;
esac
exit 64
"""

REPORT_KEYS = {
    "target", "device", "bundle_id", "command", "exit_code", "signal",
    "timed_out", "ps3stop", "reader_errors", "expected_stop", "outcome",
    "log", "duration_s", "load_avg", "booted_by_tool", "app_pid",
    "app_cleanup", "app_cleanup_detail",
}

STOP_LINE = ("PS3STOP v=1 kind=fiber-host-context op=create reason=getcontext "
             "error_domain=posix")


def read_calls(path):
    """[{'argv': [...], 'env': {...}}] in call order, from the fake's log."""
    if not Path(path).exists():
        return []
    calls = []
    argv = env = None
    mode = ""
    for line in Path(path).read_text().splitlines():
        if line == "CALL":
            argv, env, mode = [], {}, "argv"
        elif line.startswith("arg0="):
            argv = [line[5:]]
        elif line == "ENV":
            mode = "env"
        elif line == "END":
            calls.append({"argv": argv, "env": env})
            argv, env, mode = None, None, ""
        elif mode == "argv" and line.startswith("arg="):
            argv.append(line[4:])
        elif mode == "env":
            key, sep, value = line.partition("=")
            if sep:
                env[key] = value
    return calls


class IosRunCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="ios-run-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls_path = self.root / "calls.log"
        self.lock = self.root / "run.lock"
        self.devices = self.root / "devices.json"
        self.write_devices([{"name": "iPhone Test", "udid": UDID,
                             "state": "Shutdown"}])
        fake = self.bin / "xcrun"
        fake.write_text(FAKE.replace("@CAT@", CAT).replace("@ENV@", ENV)
                        .replace("@SLEEP@", SLEEP))
        fake.chmod(0o755)
        self.state = self.root / "state"
        self.state.mkdir()
        self.env = {"PATH": str(self.bin),
                    "FAKE_LOG": str(self.calls_path),
                    "FAKE_DEVICES": str(self.devices),
                    "FAKE_STATE": str(self.state),
                    "FAKE_APPS": self.artifact("apps.json", apps_json()),
                    "FAKE_PROCS": str(self.root / "procs.json"),
                    "FAKE_PROCS_DEAD": str(self.root / "procs-dead.json")}
        self.set_procs([(1, LAUNCHD_EXE), (APP_PID, APP_EXE), (4243, PLUGIN_EXE)])

    def set_procs(self, alive, dead=None):
        """The process list while the app runs, and after the fake kills it."""
        if dead is None:
            dead = [entry for entry in alive if entry[0] != APP_PID]
        Path(self.env["FAKE_PROCS"]).write_text(procs_json(alive))
        Path(self.env["FAKE_PROCS_DEAD"]).write_text(procs_json(dead))

    def write_devices(self, devices):
        self.devices.write_text(json.dumps({"devices": {RUNTIME: devices}}))

    def write_runtimes(self, runtimes):
        self.devices.write_text(json.dumps({"devices": runtimes}))

    def artifact(self, name, text):
        path = self.root / name
        path.write_text(text)
        return str(path)

    def run_tool(self, *argv, env=None, timeout=60):
        argv = list(argv)
        if argv and argv[0] in ("sim", "device") and "--lock" not in argv:
            argv[1:1] = ["--lock", str(self.lock)]
        return subprocess.run([sys.executable, str(TOOL), *argv],
                              env={**self.env, **(env or {})},
                              cwd=str(self.root), capture_output=True,
                              text=True, timeout=timeout, check=False)

    def calls(self):
        return read_calls(self.calls_path)

    def commands(self):
        return [call["argv"] for call in self.calls()]

    def child_env_of(self, verb, index):
        """The environment of the index-th call whose argv[1:3] is verb."""
        seen = 0
        for call in self.calls():
            if tuple(call["argv"][1:3]) == verb:
                if seen == index:
                    return call["env"]
                seen += 1
        self.fail("no call %r[%d] in %r" % (verb, index, self.commands()))

    def assertNoTraceback(self, result):
        self.assertNotIn("Traceback", result.stderr, result.stderr)


class SimTest(IosRunCase):
    def test_pass_reports_zero(self):
        out = self.artifact("out.txt", "hello\n")
        result = self.run_tool("sim", "--device", UDID, "--json", "--",
                               "prog", "--flag", env={"FAKE_OUT": out})
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["outcome"], "pass")
        self.assertEqual(report["exit_code"], 0)
        self.assertIsNone(report["signal"])
        self.assertIs(report["timed_out"], False)
        self.assertIsNone(report["ps3stop"])
        self.assertEqual(report["command"][0], str(self.bin / "xcrun"))
        self.assertEqual(report["command"][1:4], ["simctl", "spawn", UDID])
        self.assertEqual(report["command"][4:], ["prog", "--flag"])
        self.assertIn("hello", result.stderr)
        self.assertEqual(result.stdout.count("\n"), 1)

    def test_nonzero_exit_fails(self):
        result = self.run_tool("sim", "--device", UDID, "--json", "--", "prog",
                               env={"FAKE_EXIT": "3"})
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["exit_code"], 3)
        self.assertIsNone(report["signal"])
        self.assertEqual(report["outcome"], "fail")

    def test_signal_fails(self):
        result = self.run_tool("sim", "--device", UDID, "--json", "--", "prog",
                               env={"FAKE_SIGNAL": "KILL"})
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["signal"], 9)
        self.assertIsNone(report["exit_code"])
        self.assertEqual(report["outcome"], "fail")

    def test_timeout_kills_the_process_group(self):
        result = self.run_tool("sim", "--device", UDID, "--json", "--timeout",
                               "1", "--", "prog", env={"FAKE_SLEEP": "30"})
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertIs(report["timed_out"], True)
        self.assertEqual(report["outcome"], "fail")
        self.assertLess(report["duration_s"], 30.0)

    def test_stop_without_expectation_fails(self):
        out = self.artifact("out.txt", "before\n" + STOP_LINE + "\nafter\n")
        result = self.run_tool("sim", "--device", UDID, "--json", "--", "prog",
                               env={"FAKE_OUT": out, "FAKE_EXIT": "70"})
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["exit_code"], 70)
        self.assertEqual(report["ps3stop"]["kind"], "fiber-host-context")
        self.assertEqual(report["ps3stop"]["fields"],
                         {"op": "create", "reason": "getcontext",
                          "error_domain": "posix"})
        self.assertEqual(report["ps3stop"]["line"], STOP_LINE)
        self.assertEqual(report["outcome"], "fail")

    def test_expected_stop_passes(self):
        out = self.artifact("out.txt", STOP_LINE + "\n")
        result = self.run_tool("sim", "--device", UDID, "--json",
                               "--expect-stop", "fiber-host-context", "--",
                               "prog", env={"FAKE_OUT": out, "FAKE_EXIT": "70"})
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["outcome"], "pass")
        self.assertEqual(report["expected_stop"], "fiber-host-context")
        self.assertEqual(report["ps3stop"]["kind"], "fiber-host-context")

    def test_only_the_first_stop_counts(self):
        out = self.artifact("out.txt",
                            STOP_LINE + "\nPS3STOP v=1 kind=other x=1\n")
        result = self.run_tool("sim", "--device", UDID, "--json", "--",
                               "prog", env={"FAKE_OUT": out, "FAKE_EXIT": "70"})
        report = json.loads(result.stdout)
        self.assertEqual(report["ps3stop"]["kind"], "fiber-host-context")

    def test_wrong_stop_kind_fails(self):
        out = self.artifact("out.txt", STOP_LINE + "\n")
        result = self.run_tool("sim", "--device", UDID, "--json",
                               "--expect-stop", "something-else", "--", "prog",
                               env={"FAKE_OUT": out, "FAKE_EXIT": "70"})
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(json.loads(result.stdout)["outcome"], "fail")

    def test_expected_stop_needs_exit_70(self):
        out = self.artifact("out.txt", STOP_LINE + "\n")
        result = self.run_tool("sim", "--device", UDID, "--json",
                               "--expect-stop", "fiber-host-context", "--",
                               "prog", env={"FAKE_OUT": out, "FAKE_EXIT": "0"})
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(json.loads(result.stdout)["outcome"], "fail")

    def test_boots_and_shuts_down(self):
        result = self.run_tool("sim", "--device", UDID, "--json", "--", "prog")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([cmd[1:3] for cmd in self.commands()],
                         [["simctl", "list"], ["simctl", "boot"],
                          ["simctl", "bootstatus"], ["simctl", "spawn"],
                          ["simctl", "shutdown"]])
        self.assertIs(json.loads(result.stdout)["booted_by_tool"], True)

    def test_keep_booted_skips_shutdown(self):
        result = self.run_tool("sim", "--device", UDID, "--json",
                               "--keep-booted", "--", "prog")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(["simctl", "shutdown"],
                         [cmd[1:3] for cmd in self.commands()])
        self.assertIs(json.loads(result.stdout)["booted_by_tool"], True)

    def test_booted_simulator_is_left_alone(self):
        self.write_devices([{"name": "iPhone Test", "udid": UDID,
                             "state": "Booted"}])
        result = self.run_tool("sim", "--device", UDID, "--json", "--", "prog")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([cmd[1:3] for cmd in self.commands()],
                         [["simctl", "list"], ["simctl", "spawn"]])
        self.assertIs(json.loads(result.stdout)["booted_by_tool"], False)

    def test_device_name_resolves_to_udid(self):
        result = self.run_tool("sim", "--device", "iPhone Test", "--json", "--",
                               "prog")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["device"], UDID)

    def test_env_uses_simctl_child_prefix(self):
        result = self.run_tool("sim", "--device", UDID, "--json", "--env",
                               "FOO=bar", "--", "prog")
        self.assertEqual(result.returncode, 0, result.stderr)
        env = self.child_env_of(("simctl", "spawn"), 0)
        self.assertEqual(env["SIMCTL_CHILD_FOO"], "bar")
        self.assertNotIn("FOO", env)
        self.assertNotIn("bar", result.stdout)
        self.assertNotIn("bar", result.stderr)

    def test_log_tees_the_output(self):
        out = self.artifact("out.txt", "hello\n")
        log = self.root / "run.log"
        result = self.run_tool("sim", "--device", UDID, "--json", "--log",
                               str(log), "--", "prog", env={"FAKE_OUT": out})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("hello", result.stderr)
        self.assertIn("hello", log.read_text())
        self.assertEqual(json.loads(result.stdout)["log"],
                         os.path.abspath(str(log)))

    def test_merged_streams_keep_order(self):
        stdout = self.artifact("out.txt", "one\n")
        stderr = self.artifact("err.txt", "two\n")
        log = self.root / "run.log"
        result = self.run_tool("sim", "--device", UDID, "--log", str(log),
                               "--", "prog",
                               env={"FAKE_OUT": stdout, "FAKE_ERR": stderr})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(log.read_text(), "one\ntwo\n")

    def test_unknown_device_exits_2(self):
        result = self.run_tool("sim", "--device", "No Such Phone", "--json",
                               "--", "prog")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("device not found", result.stderr)
        self.assertNoTraceback(result)
        self.assertEqual(json.loads(result.stdout)["outcome"], "error")
        self.assertEqual([cmd[1:3] for cmd in self.commands()],
                         [["simctl", "list"]])

    def test_name_on_several_runtimes_needs_a_udid(self):
        self.write_runtimes({
            RUNTIME: [{"name": "iPhone Test", "udid": UDID, "state": "Shutdown"}],
            OLD_RUNTIME: [{"name": "iPhone Test", "udid": OTHER_UDID,
                           "state": "Shutdown"}]})
        result = self.run_tool("sim", "--device", "iPhone Test", "--json", "--",
                               "prog")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertNoTraceback(result)
        self.assertIn(UDID, result.stderr)
        self.assertIn(OTHER_UDID, result.stderr)
        self.assertIn("iOS-26-0", result.stderr)
        self.assertIn("iOS-17-5", result.stderr)
        self.assertIn("pass one UDID", result.stderr)
        self.assertEqual([cmd[1:3] for cmd in self.commands()],
                         [["simctl", "list"]])

    def test_udid_picks_one_of_several_runtimes(self):
        self.write_runtimes({
            RUNTIME: [{"name": "iPhone Test", "udid": UDID, "state": "Shutdown"}],
            OLD_RUNTIME: [{"name": "iPhone Test", "udid": OTHER_UDID,
                           "state": "Shutdown"}]})
        result = self.run_tool("sim", "--device", OTHER_UDID, "--json", "--",
                               "prog")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["device"], OTHER_UDID)

    def test_name_on_several_runtimes_uses_the_one_booted(self):
        self.write_runtimes({
            OLD_RUNTIME: [{"name": "iPhone Test", "udid": OTHER_UDID,
                           "state": "Shutdown"}],
            RUNTIME: [{"name": "iPhone Test", "udid": UDID, "state": "Booted"}]})
        result = self.run_tool("sim", "--device", "iPhone Test", "--json", "--",
                               "prog")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["device"], UDID)
        self.assertIs(report["booted_by_tool"], False)
        self.assertIn("using the Booted one", result.stderr)
        self.assertIn(UDID, result.stderr)
        self.assertEqual([cmd[1:3] for cmd in self.commands()],
                         [["simctl", "list"], ["simctl", "spawn"]])

    def test_name_on_several_booted_runtimes_needs_a_udid(self):
        self.write_runtimes({
            RUNTIME: [{"name": "iPhone Test", "udid": UDID, "state": "Booted"}],
            OLD_RUNTIME: [{"name": "iPhone Test", "udid": OTHER_UDID,
                           "state": "Booted"}]})
        result = self.run_tool("sim", "--device", "iPhone Test", "--json", "--",
                               "prog")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("pass one UDID", result.stderr)

    def test_undecodable_output_does_not_stall_the_child(self):
        # More than a pipe buffer follows the bad bytes: a reader that died on
        # them would leave the child blocked until the timeout.
        noise = (b"\x83\x41\xff\xfe not utf-8\n" + b"x" * 200 + b"\n") * 600
        path = self.root / "binary.out"
        path.write_bytes(noise + STOP_LINE.encode() + b"\n")
        self.assertGreater(path.stat().st_size, 100 * 1024)
        result = self.run_tool("sim", "--device", UDID, "--json", "--timeout",
                               "30", "--expect-stop", "fiber-host-context",
                               "--", "prog",
                               env={"FAKE_OUT": str(path), "FAKE_EXIT": "70"})
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertIs(report["timed_out"], False)
        self.assertEqual(report["ps3stop"]["kind"], "fiber-host-context")
        self.assertEqual(report["reader_errors"], [])
        self.assertIn("\ufffd", result.stderr)

    @unittest.skipUnless(os.path.exists("/dev/full"), "needs /dev/full")
    def test_log_write_failure_is_reported(self):
        out = self.artifact("out.txt", "hello\n")
        result = self.run_tool("sim", "--device", UDID, "--json", "--log",
                               "/dev/full", "--", "prog", env={"FAKE_OUT": out})
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["outcome"], "fail")
        self.assertTrue(report["reader_errors"][0].startswith("log: OSError"))

    def interrupted_run(self, signum):
        """Interrupt a tool whose child sleeps; (result, child pid)."""
        pidfile = self.root / "child.pid"
        pidfile.unlink(missing_ok=True)
        proc = subprocess.Popen(
            [sys.executable, str(TOOL), "sim", "--lock", str(self.lock),
             "--device", UDID, "--json", "--timeout", "120", "--", "prog"],
            env={**self.env, "FAKE_SLEEP": "120", "FAKE_PIDFILE": str(pidfile)},
            cwd=str(self.root), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True)
        self.addCleanup(proc.kill)
        deadline = time.monotonic() + 30
        while not (pidfile.exists() and pidfile.read_text().endswith("\n")):
            self.assertLess(time.monotonic(), deadline, "child never started")
            self.assertIsNone(proc.poll(), "tool exited before the child ran")
            time.sleep(0.02)
        child = int(pidfile.read_text())
        self.addCleanup(self.kill_group, child)
        proc.send_signal(signum)
        out, err = proc.communicate(timeout=60)
        return subprocess.CompletedProcess(proc.args, proc.returncode, out, err), child

    @staticmethod
    def kill_group(pgid):
        with contextlib.suppress(ProcessLookupError, PermissionError):
            os.killpg(pgid, signal.SIGKILL)

    def group_is_gone(self, pgid):
        for _ in range(250):
            try:
                os.killpg(pgid, 0)
            except ProcessLookupError:
                return True
            time.sleep(0.02)
        return False

    def test_interrupt_stops_the_child_and_cleans_up(self):
        for name in ("SIGINT", "SIGTERM"):
            with self.subTest(signal=name):
                self.calls_path.unlink(missing_ok=True)
                result, child = self.interrupted_run(getattr(signal, name))
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNoTraceback(result)
                self.assertIn("interrupted by " + name, result.stderr)
                report = json.loads(result.stdout)
                self.assertEqual(report["outcome"], "error")
                self.assertTrue(self.group_is_gone(child),
                                "the child's process group outlived the tool")
                # The tool booted the simulator, so it shuts it down again.
                self.assertEqual(self.commands()[-1][1:3], ["simctl", "shutdown"])
                fd = os.open(self.lock, os.O_RDWR)
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                finally:
                    os.close(fd)

    @unittest.skipUnless(sys.platform == "darwin",
                         "lockf(1) holding a lock is a macOS test")
    def test_lock_busy_exits_2(self):
        holder = subprocess.Popen(["lockf", "-k", str(self.lock), "sleep", "5"])
        self.addCleanup(holder.wait, 10)
        self.addCleanup(holder.terminate)
        fd = os.open(self.lock, os.O_RDWR | os.O_CREAT, 0o666)
        try:
            for _ in range(500):
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    fcntl.flock(fd, fcntl.LOCK_UN)
                except OSError:
                    break
                time.sleep(0.01)
            else:
                self.skipTest("lockf did not take the lock")
        finally:
            os.close(fd)
        result = self.run_tool("sim", "--device", UDID, "--lock", str(self.lock),
                               "--lock-wait", "0", "--json", "--", "prog")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("lock busy", result.stderr)
        self.assertNoTraceback(result)

    def test_json_report_shape(self):
        result = self.run_tool("sim", "--device", UDID, "--json", "--", "prog")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(set(report), REPORT_KEYS)
        self.assertEqual(report["target"], "sim")
        self.assertIsInstance(report["device"], str)
        self.assertIsNone(report["bundle_id"])
        self.assertIsInstance(report["command"], list)
        self.assertIsInstance(report["exit_code"], int)
        self.assertIsNone(report["signal"])
        self.assertIs(report["timed_out"], False)
        self.assertIsNone(report["ps3stop"])
        self.assertIsNone(report["expected_stop"])
        self.assertEqual(report["outcome"], "pass")
        self.assertIsNone(report["log"])
        self.assertIsInstance(report["duration_s"], float)
        self.assertEqual(len(report["load_avg"]), 3)
        for value in report["load_avg"]:
            self.assertIsInstance(value, float)
        self.assertIs(report["booted_by_tool"], True)
        self.assertIsNone(report["app_pid"])
        self.assertIsNone(report["app_cleanup"])
        self.assertIsNone(report["app_cleanup_detail"])

    def test_summary_is_one_line(self):
        result = self.run_tool("sim", "--device", UDID, "--", "prog")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")
        lines = [line for line in result.stderr.splitlines() if line]
        self.assertEqual(len(lines), 1, result.stderr)
        self.assertIn("outcome=pass", lines[0])


class UsageTest(IosRunCase):
    def test_help_exits_zero(self):
        result = self.run_tool("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("sim", result.stdout)
        self.assertIn("device", result.stdout)

    def test_subcommand_help_exits_zero(self):
        for name in ("sim", "device"):
            result = self.run_tool(name, "--help")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(name, result.stdout)

    def test_sim_without_arguments_is_a_usage_error(self):
        result = self.run_tool("sim")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertNoTraceback(result)

    def test_sim_without_program_is_a_usage_error(self):
        result = self.run_tool("sim", "--device", UDID)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertNoTraceback(result)
        self.assertEqual(self.commands(), [])

    def test_device_without_bundle_id_is_a_usage_error(self):
        result = self.run_tool("device", "--udid", UDID)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertNoTraceback(result)

    def test_bad_env_is_a_usage_error(self):
        result = self.run_tool("sim", "--device", UDID, "--env", "FOO", "--",
                               "prog")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("--env needs K=V", result.stderr)
        self.assertNoTraceback(result)
        self.assertEqual(self.commands(), [])


class DeviceTest(IosRunCase):
    def console(self, text):
        return self.artifact("console.txt", text)

    def launch(self, *extra, console, env=None):
        return self.run_tool("device", "--udid", UDID, "--bundle-id",
                             "com.example.title", "--json", *extra,
                             env={**(env or {}), "FAKE_OUT": self.console(console)})

    def launch_call(self, env=False):
        """The launch call's argv (or its environment), None without one."""
        for call in self.calls():
            if call["argv"][1:5] == ["devicectl", "device", "process", "launch"]:
                return call["env"] if env else call["argv"]
        return None

    def test_exit_code_line(self):
        result = self.launch(console="The app terminated with the exit code 7.\n")
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["exit_code"], 7)
        self.assertIsNone(report["signal"])
        self.assertEqual(report["outcome"], "fail")

    def test_signal_line_without_period(self):
        result = self.launch(console="App terminated due to signal 11\n")
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["signal"], 11)
        self.assertIsNone(report["exit_code"])
        self.assertEqual(report["outcome"], "fail")

    def test_exit_code_zero_passes(self):
        result = self.launch(console="The app terminated with the exit code 0.\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["outcome"], "pass")

    def test_missing_status_line_fails(self):
        result = self.launch(console="hello\n")
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertIsNone(report["exit_code"])
        self.assertEqual(report["outcome"], "fail")

    def test_expected_stop_on_device(self):
        result = self.launch("--expect-stop", "fiber-host-context",
                             console=STOP_LINE + "\n"
                             "The app terminated with the exit code 70.\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["ps3stop"]["kind"], "fiber-host-context")
        self.assertEqual(report["exit_code"], 70)

    def test_install_then_launch(self):
        app = self.artifact("Title.app", "")
        out = self.console("The app terminated with the exit code 0.\n")
        result = self.run_tool("device", "--udid", UDID, "--bundle-id",
                               "com.example.title", "--app", app, "--json",
                               env={"FAKE_OUT": out})
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = self.commands()
        self.assertEqual(commands[0][1:5], ["devicectl", "device", "install", "app"])
        self.assertIn("--device", commands[0])
        self.assertIn(app, commands[0])
        self.assertEqual(commands[1][1:5], ["devicectl", "device", "info", "apps"])
        launch = self.launch_call()
        self.assertIn("--terminate-existing", launch)
        self.assertEqual(launch[-2:], ["--console", "com.example.title"])
        report = json.loads(result.stdout)
        self.assertEqual(report["bundle_id"], "com.example.title")
        self.assertEqual(report["device"], UDID)

    def test_install_failure_exits_2(self):
        app = self.artifact("Title.app", "")
        result = self.run_tool("device", "--udid", UDID, "--bundle-id",
                               "com.example.title", "--app", app, "--json",
                               env={"FAKE_INSTALL_EXIT": "1"})
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(json.loads(result.stdout)["outcome"], "error")
        self.assertEqual(len(self.commands()), 1)

    def test_env_uses_devicectl_child_prefix(self):
        result = self.launch("--env", "HOME=/tmp/x",
                             console="The app terminated with the exit code 0.\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        env = self.launch_call(env=True)
        self.assertEqual(env["DEVICECTL_CHILD_HOME"], "/tmp/x")
        self.assertNotIn("/tmp/x", result.stdout)
        self.assertNotIn("/tmp/x", result.stderr)

    def test_launch_args_are_passed_through(self):
        out = self.console("The app terminated with the exit code 0.\n")
        result = self.run_tool("device", "--udid", UDID, "--bundle-id",
                               "com.example.title", "--json", "--", "--trials",
                               "5", env={"FAKE_OUT": out})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.launch_call()[-2:], ["--trials", "5"])

    def test_status_printed_by_the_app_is_not_devicectls(self):
        # The app's words come first; devicectl's epilogue is the last line.
        result = self.launch(console="The app terminated with the exit code 0.\n"
                             "App terminated due to signal 11.\n")
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["signal"], 11)
        self.assertIsNone(report["exit_code"])

    def test_signal_text_before_a_real_exit_code(self):
        result = self.launch(console="App terminated due to signal 11.\n"
                             "The app terminated with the exit code 0.\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["exit_code"], 0)
        self.assertIsNone(report["signal"])

    def test_launch_failure_without_status_exits_2(self):
        result = self.launch(console="ERROR: The device is locked.\n",
                             env={"FAKE_LAUNCH_EXIT": "1"})
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertNoTraceback(result)
        self.assertIn("devicectl launch exited 1", result.stderr)
        self.assertEqual(json.loads(result.stdout)["outcome"], "error")

    def test_status_line_wins_over_devicectl_exit(self):
        result = self.launch(console="The app terminated with the exit code 7.\n",
                             env={"FAKE_LAUNCH_EXIT": "1"})
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["exit_code"], 7)
        self.assertEqual(report["outcome"], "fail")

    # Host-ended runs: the app outlives devicectl, so the tool stops it itself.

    def stop_calls(self):
        """[(verb, signal, pid)] of every terminate and signal request."""
        calls = []
        for argv in self.commands():
            if argv[1:4] == ["devicectl", "device", "process"] and argv[4] in (
                    "terminate", "signal"):
                sig = argv[argv.index("--signal") + 1] if "--signal" in argv else None
                calls.append((argv[4], sig, int(argv[argv.index("--pid") + 1])))
        return calls

    def hung_run(self, *extra, env=None):
        """A launch that never ends on its own; the tool's timeout ends it."""
        return self.launch("--timeout", "2", "--cleanup-grace", "1",
                           "--cleanup-call-timeout", "3", *extra,
                           console=LAUNCHED,
                           env={"FAKE_SLEEP": "30", **(env or {})})

    def test_app_that_exits_by_itself_is_left_alone(self):
        result = self.launch(console=LAUNCHED
                             + "The app terminated with the exit code 0.\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["outcome"], "pass")
        self.assertIsNone(report["app_cleanup"])
        self.assertEqual(self.stop_calls(), [])

    def test_timeout_ends_the_app_with_sigterm(self):
        result = self.hung_run()
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertIs(report["timed_out"], True)
        self.assertEqual(report["outcome"], "fail")
        self.assertEqual(report["app_cleanup"], "sigterm")
        self.assertEqual(report["app_pid"], APP_PID)
        self.assertIsNone(report["app_cleanup_detail"])
        self.assertEqual(self.stop_calls(), [("terminate", None, APP_PID)])
        terminate = [argv for argv in self.commands() if argv[4:5] == ["terminate"]][0]
        self.assertEqual(terminate[terminate.index("--device") + 1], UDID)

    def test_ignored_sigterm_is_followed_by_sigkill(self):
        result = self.hung_run(env={"FAKE_TERM": "ignore"})
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertIs(report["timed_out"], True)
        self.assertEqual(report["outcome"], "fail")
        self.assertEqual(report["app_cleanup"], "sigkill")
        self.assertEqual(self.stop_calls(),
                         [("terminate", None, APP_PID),
                          ("signal", "SIGKILL", APP_PID)])

    def test_a_kill_that_does_not_end_the_app_fails_the_run(self):
        for how in ("fail", "ignore"):
            with self.subTest(kill=how):
                self.calls_path.unlink(missing_ok=True)
                (self.state / "dead").unlink(missing_ok=True)
                env = {"FAKE_TERM": "ignore", "FAKE_KILL": how}
                result = self.hung_run(env=env)
                self.assertEqual(result.returncode, 1, result.stderr)
                report = json.loads(result.stdout)
                self.assertEqual(report["outcome"], "fail")
                self.assertEqual(report["app_cleanup"], "failed")
                self.assertIn("still listed after SIGKILL",
                              report["app_cleanup_detail"])
                self.assertEqual(report["app_pid"], APP_PID)
                self.assertEqual(len(self.stop_calls()), 2)

    def test_the_reason_names_a_failed_cleanup(self):
        result = self.run_tool(
            "device", "--udid", UDID, "--bundle-id", BUNDLE_ID, "--timeout", "2",
            "--cleanup-grace", "1",
            env={"FAKE_SLEEP": "30", "FAKE_TERM": "ignore", "FAKE_KILL": "ignore",
                 "FAKE_OUT": self.console(LAUNCHED)})
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("outcome=fail", result.stderr)
        self.assertIn("timed out", result.stderr)
        self.assertIn("app cleanup failed", result.stderr)
        self.assertIn("pid %d is still listed after SIGKILL" % APP_PID, result.stderr)

    def test_a_listing_without_the_app_signals_nothing(self):
        cases = {
            "another bundle": [(1, LAUNCHD_EXE), (APP_PID, OTHER_EXE)],
            "an extension only": [(1, LAUNCHD_EXE), (4243, PLUGIN_EXE)],
            "no executable": [(1, LAUNCHD_EXE)],
        }
        for name, listing in cases.items():
            with self.subTest(listing=name):
                self.calls_path.unlink(missing_ok=True)
                self.set_procs(listing, listing)
                result = self.hung_run()
                self.assertEqual(result.returncode, 1, result.stderr)
                report = json.loads(result.stdout)
                self.assertIs(report["timed_out"], True)
                self.assertEqual(report["outcome"], "fail")
                self.assertEqual(report["app_cleanup"], "unmatched")
                self.assertIsNone(report["app_pid"])
                self.assertIn("nothing was signalled", report["app_cleanup_detail"])
                self.assertEqual(self.stop_calls(), [])

    def test_a_reused_pid_is_not_signalled(self):
        # The tracker recorded the app's pid; by cleanup that pid runs
        # another executable, so it is not the app and must not be touched.
        pidfile = self.root / "child.pid"
        proc = subprocess.Popen(
            [sys.executable, str(TOOL), "device", "--lock", str(self.lock),
             "--udid", UDID, "--bundle-id", BUNDLE_ID, "--json", "--timeout", "6",
             "--cleanup-grace", "1"],
            env={**self.env, "FAKE_SLEEP": "30", "FAKE_OUT": self.console(LAUNCHED),
                 "FAKE_PIDFILE": str(pidfile)},
            cwd=str(self.root), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True)
        self.addCleanup(proc.kill)
        deadline = time.monotonic() + 30
        while not any(argv[3:5] == ["info", "processes"] for argv in self.commands()):
            self.assertLess(time.monotonic(), deadline, "tracker never listed")
            time.sleep(0.05)
        time.sleep(0.5)
        self.set_procs([(1, LAUNCHD_EXE), (APP_PID, OTHER_EXE)],
                       [(1, LAUNCHD_EXE), (APP_PID, OTHER_EXE)])
        out, err = proc.communicate(timeout=60)
        self.assertEqual(proc.returncode, 1, err)
        report = json.loads(out)
        self.assertEqual(report["app_cleanup"], "exited")
        self.assertEqual(report["app_pid"], APP_PID)
        self.assertIn("not touched", report["app_cleanup_detail"])
        self.assertEqual(self.stop_calls(), [])

    def test_a_devicectl_call_that_hangs_is_bounded(self):
        result = self.launch("--timeout", "2", "--cleanup-grace", "1",
                             "--cleanup-call-timeout", "1", console=LAUNCHED,
                             env={"FAKE_SLEEP": "30", "FAKE_TERM": "hang"})
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["app_cleanup"], "sigkill")
        self.assertIn("timed out after 1s", report["app_cleanup_detail"])
        self.assertEqual([call[0] for call in self.stop_calls()],
                         ["terminate", "signal"])

    def test_a_hung_kill_fails_the_run(self):
        result = self.launch("--timeout", "2", "--cleanup-grace", "1",
                             "--cleanup-call-timeout", "1", console=LAUNCHED,
                             env={"FAKE_SLEEP": "30", "FAKE_TERM": "hang",
                                  "FAKE_KILL": "hang"})
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["app_cleanup"], "failed")
        self.assertIn("timed out after 1s", report["app_cleanup_detail"])

    def test_an_unreadable_listing_fails_the_run(self):
        result = self.hung_run(env={"FAKE_PROCS_EXIT": "1"})
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["app_cleanup"], "failed")
        self.assertIn("cannot list", report["app_cleanup_detail"])
        self.assertEqual(self.stop_calls(), [])

    def test_an_unresolvable_bundle_stops_before_the_launch(self):
        result = self.launch(console=LAUNCHED, env={"FAKE_APPS_EXIT": "1"})
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("cannot resolve the installed bundle", result.stderr)
        self.assertIsNone(self.launch_call())

    def test_interrupt_stops_the_app(self):
        pidfile = self.root / "child.pid"
        proc = subprocess.Popen(
            [sys.executable, str(TOOL), "device", "--lock", str(self.lock),
             "--udid", UDID, "--bundle-id", BUNDLE_ID, "--json", "--timeout", "120",
             "--cleanup-grace", "1"],
            env={**self.env, "FAKE_SLEEP": "120", "FAKE_OUT": self.console(LAUNCHED),
                 "FAKE_PIDFILE": str(pidfile)},
            cwd=str(self.root), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True)
        self.addCleanup(proc.kill)
        deadline = time.monotonic() + 30
        while not (pidfile.exists() and pidfile.read_text().endswith("\n")):
            self.assertLess(time.monotonic(), deadline, "launch never started")
            self.assertIsNone(proc.poll(), "tool exited before the launch")
            time.sleep(0.02)
        self.addCleanup(SimTest.kill_group, int(pidfile.read_text()))
        proc.send_signal(signal.SIGINT)
        out, err = proc.communicate(timeout=60)
        self.assertEqual(proc.returncode, 2, err)
        self.assertIn("interrupted by SIGINT", err)
        self.assertIn("the app ended after SIGTERM", err)
        report = json.loads(out)
        self.assertEqual(report["outcome"], "error")
        self.assertEqual(report["app_cleanup"], "sigterm")
        self.assertEqual(self.stop_calls(), [("terminate", None, APP_PID)])


class ReaderTest(unittest.TestCase):
    def pump(self, stream, log=None):
        watcher = ios_run.Watcher(console_status=False)
        with contextlib.redirect_stderr(io.StringIO()):
            ios_run.pump(stream, log, watcher)
        return watcher

    def test_a_failing_log_does_not_stop_the_drain(self):
        class FullLog:
            def write(self, text):
                raise OSError(28, "No space left on device")

            def flush(self):
                pass

        stream = io.BytesIO(b"\xff\n" + STOP_LINE.encode() + b"\nmore\n")
        watcher = self.pump(stream, FullLog())
        self.assertEqual(watcher.ps3stop["kind"], "fiber-host-context")
        self.assertEqual(len(watcher.reader_errors), 1)
        self.assertTrue(watcher.reader_errors[0].startswith("log: OSError"))
        self.assertEqual(stream.read(), b"")

    def test_a_failing_read_is_recorded(self):
        class Broken:
            def __init__(self):
                self.lines = [STOP_LINE.encode() + b"\n"]

            def readline(self):
                if self.lines:
                    return self.lines.pop()
                raise OSError(5, "Input/output error")

        watcher = self.pump(Broken())
        self.assertEqual(watcher.ps3stop["kind"], "fiber-host-context")
        self.assertTrue(watcher.reader_errors[0].startswith("read: OSError"))

    def test_an_unread_output_never_passes(self):
        report = {"timed_out": False, "expected_stop": None, "ps3stop": None,
                  "exit_code": 0, "signal": None, "reader_errors": ["log: x"],
                  "app_cleanup": None, "app_pid": None,
                  "app_cleanup_detail": None}
        self.assertEqual(ios_run.decide_outcome(report)[0], "fail")
        report["reader_errors"] = []
        self.assertEqual(ios_run.decide_outcome(report)[0], "pass")


if __name__ == "__main__":
    unittest.main()
