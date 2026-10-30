#!/usr/bin/env python3
"""Contract tests for tools/ios_run.py.

Every case runs the tool against a fake `xcrun`: a shell script that logs
its argv and environment and answers the few simctl and devicectl commands
the tool uses.  No real simulator, device or Xcode is involved, and PATH is
the fake directory alone so the real xcrun is unreachable.

The fake's log is the observable contract: which commands ran, in what
order, with which prefixed environment.
"""

import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

TOOL = Path(__file__).resolve().with_name("ios_run.py")

UDID = "AAAA1111-2222-3333-4444-555566667777"
OTHER_UDID = "BBBB1111-2222-3333-4444-555566667777"
RUNTIME = "com.apple.CoreSimulator.SimRuntime.iOS-26-0"

CAT = shutil.which("cat") or "/bin/cat"
ENV = shutil.which("env") or "/usr/bin/env"
SLEEP = shutil.which("sleep") or "/bin/sleep"

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
    [ -n "$FAKE_SLEEP" ] && @SLEEP@ "$FAKE_SLEEP"
    [ -n "$FAKE_SIGNAL" ] && kill -s "$FAKE_SIGNAL" $$
    exit "${FAKE_EXIT:-0}"
    ;;
  "devicectl device")
    case "$3" in
      install)
        exit "${FAKE_INSTALL_EXIT:-0}"
        ;;
      process)
        [ -n "$FAKE_OUT" ] && @CAT@ "$FAKE_OUT"
        [ -n "$FAKE_SLEEP" ] && @SLEEP@ "$FAKE_SLEEP"
        [ -n "$FAKE_SIGNAL" ] && kill -s "$FAKE_SIGNAL" $$
        exit "${FAKE_LAUNCH_EXIT:-0}"
        ;;
    esac
    ;;
esac
exit 64
"""

REPORT_KEYS = {
    "target", "device", "bundle_id", "command", "exit_code", "signal",
    "timed_out", "ps3stop", "expected_stop", "outcome", "log", "duration_s",
    "load_avg", "booted_by_tool",
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
        self.env = {"PATH": str(self.bin),
                    "FAKE_LOG": str(self.calls_path),
                    "FAKE_DEVICES": str(self.devices)}

    def write_devices(self, devices):
        self.devices.write_text(json.dumps({"devices": {RUNTIME: devices}}))

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
        self.assertEqual(commands[1][1:5],
                         ["devicectl", "device", "process", "launch"])
        self.assertIn("--terminate-existing", commands[1])
        self.assertEqual(commands[1][-2:], ["--console", "com.example.title"])
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
        env = self.child_env_of(("devicectl", "device"), 0)
        self.assertEqual(env["DEVICECTL_CHILD_HOME"], "/tmp/x")
        self.assertNotIn("/tmp/x", result.stdout)
        self.assertNotIn("/tmp/x", result.stderr)

    def test_launch_args_are_passed_through(self):
        out = self.console("The app terminated with the exit code 0.\n")
        result = self.run_tool("device", "--udid", UDID, "--bundle-id",
                               "com.example.title", "--json", "--", "--trials",
                               "5", env={"FAKE_OUT": out})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.commands()[0][-2:], ["--trials", "5"])


if __name__ == "__main__":
    unittest.main()
