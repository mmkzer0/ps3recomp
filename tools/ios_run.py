#!/usr/bin/env python3
"""Run a test program in the iOS Simulator or on a connected device.

A simulator and a device are shared resources: a run boots a simulator,
occupies the machine and must stop again, so every run holds an exclusive
lock -- the same lock the shell recipe takes -- for its whole duration.

    python3 tools/ios_run.py sim --device 'iPhone 17' -- ./build/host_fiber
    python3 tools/ios_run.py sim --device UDID --expect-stop fiber-host-context \
        -- ./build/host_fiber --trials 2000
    python3 tools/ios_run.py device --udid UDID --bundle-id com.example.title --json

`sim` spawns a program with `xcrun simctl spawn`, whose exit status is the
program's.  `device` launches an installed app with `xcrun devicectl device
process launch --console`; devicectl's own status is not the app's, so the
app's exit code or signal is read from the console text it prints.

A program that fails by design stops by name: it prints a
`PS3STOP v=1 kind=<kind> <k>=<v> ...` line and exits 70.  The first such line
is reported, and `--expect-stop KIND` makes exactly that stop the pass
condition.

`--env K=V` becomes `SIMCTL_CHILD_K=V` (sim) or `DEVICECTL_CHILD_K=V`
(device), the prefixes simctl and devicectl strip before starting the
program.  The value is never printed, only passed on.

Exit status: 0 the run passed, 1 it failed, 2 usage or environment error.
A usage error is a message, never a traceback.
"""

import argparse
import contextlib
import errno
import fcntl
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_USAGE = 2

STOP_EXIT = 70

DEFAULT_LOCK = "/tmp/ps3-metal.lock"
DEFAULT_TIMEOUT = 600.0
DEFAULT_LOCK_WAIT = 600.0
KILL_GRACE_S = 10.0
READER_JOIN_S = 5.0
LOCK_POLL_S = 0.05

LOCK_BUSY_ERRNOS = frozenset((errno.EACCES, errno.EAGAIN))

# A stop line is `PS3STOP v=1 kind=<kind> <k>=<v> ...`; the fields run to the
# end of the line and each value is the token up to the next space.
STOP_RE = re.compile(r"PS3STOP v=1 kind=(\S+)(?: (.*))?$")

# devicectl reports the app's own status on its console.  The trailing period
# is the tool's punctuation, not part of the contract, so it is optional.
DEVICE_EXIT_RE = re.compile(r"The app terminated with the exit code (-?\d+)\b")
DEVICE_SIGNAL_RE = re.compile(r"App terminated due to signal (-?\d+)\b")

EPILOG = """\
The run holds --lock (an exclusive flock, which lockf(1) also takes) across
boot, child and shutdown, and records the load average and its own duration
in the report.

sim needs `-- EXE [ARG...]`; device takes `[-- ARG...]` for the app.

Exit status:
  0  the run passed
  1  it failed: non-zero exit, a signal, a timeout, an unexpected PS3STOP,
     or a stop that was not the expected one
  2  usage or environment error: bad arguments, xcrun missing, an unknown
     simulator, a busy lock, a failed install

Examples:
  ios_run.py sim --device 'iPhone 17' -- ./build/host_fiber
  ios_run.py sim --device UDID --timeout 30 --expect-stop fiber-host-context -- ./build/host_fiber
  ios_run.py device --udid UDID --bundle-id com.example.title --json
"""


class UsageError(Exception):
    """The run cannot start or cannot be set up; the CLI maps it to exit 2."""


@dataclass
class Observation:
    """What one child run showed: its status and the stop it printed."""

    exit_code: int | None
    signal: int | None
    timed_out: bool
    ps3stop: dict | None


class Watcher:
    """Per-line observations from the child's merged output.

    The stream is parsed while it is copied, so a stop or a devicectl status
    line is kept even when no log is written.  Only the first of each is
    kept: a run has one outcome, and the first stop is the one that ended it.
    """

    def __init__(self, console_status):
        self.console_status = console_status
        self.ps3stop = None
        self.console_exit = None
        self.console_signal = None

    def feed(self, line):
        text = line.rstrip("\r\n")
        if self.ps3stop is None:
            self.ps3stop = parse_stop(text)
        if not self.console_status:
            return
        if self.console_exit is None:
            self.console_exit = matched_int(DEVICE_EXIT_RE, text)
        if self.console_signal is None:
            self.console_signal = matched_int(DEVICE_SIGNAL_RE, text)


def matched_int(pattern, text):
    """The integer in pattern's group 1, or None when it does not match."""
    match = pattern.search(text)
    return int(match.group(1)) if match else None


def parse_stop(line):
    """The reported PS3STOP object for a stop line, or None.

    A value may itself contain '=', so only the first one separates the key
    from the value.
    """
    text = line.rstrip("\r\n")
    match = STOP_RE.match(text.strip())
    if match is None:
        return None
    fields = {}
    for token in (match.group(2) or "").split(" "):
        key, sep, value = token.partition("=")
        if sep and key:
            fields[key] = value
    return {"kind": match.group(1), "fields": fields, "line": text}


def parse_env(pairs):
    """[(K, V)] from --env K=V, refusing a value that is not named."""
    parsed = []
    for item in pairs:
        key, sep, value = item.partition("=")
        if not sep or not key:
            raise UsageError("--env needs K=V")
        parsed.append((key, value))
    return parsed


def child_env(pairs, prefix):
    """The environment for xcrun, with one prefixed variable per --env.

    simctl and devicectl only expose prefixed variables to the program, and
    they strip the prefix, so the program sees the name it was given.
    """
    env = dict(os.environ)
    for key, value in parse_env(pairs):
        env[prefix + key] = value
    return env


def load_average():
    """os.getloadavg(), or zeros on a host without it."""
    try:
        return list(os.getloadavg())
    except (AttributeError, OSError):
        return [0.0, 0.0, 0.0]


def status_text(report):
    """The child's status as a report line uses it."""
    if report["signal"] is not None:
        return "signal %d" % report["signal"]
    if report["exit_code"] is not None:
        return "exit %d" % report["exit_code"]
    return "status unknown"


def xcrun_path():
    """xcrun's absolute path; the Simulator and device tools live behind it."""
    path = shutil.which("xcrun")
    if path is None:
        raise UsageError("xcrun not found; install the Xcode command line tools")
    return path


def capture(command):
    """Run a setup command, capturing its output; OSError becomes exit 2."""
    try:
        return subprocess.run(command, text=True, capture_output=True, check=False)
    except OSError as exc:
        raise UsageError("cannot run %s: %s" % (command[0], exc.strerror or exc))


def require_step(command, what):
    """Run a setup command that must succeed, or stop with exit 2."""
    result = capture(command)
    if result.returncode == 0:
        return
    message = "%s failed with exit %d" % (what, result.returncode)
    detail = (result.stdout + result.stderr).strip().splitlines()
    if detail:
        message += ": " + detail[-1][:200]
    raise UsageError(message)


def open_lock_file(path):
    """The lock file's descriptor, created when missing."""
    try:
        return os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
    except OSError as exc:
        raise UsageError("cannot open lock %s: %s" % (path, exc.strerror))


def try_lock(fd, path):
    """Take the exclusive lock without waiting; False when a run holds it."""
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError as exc:
        if exc.errno not in LOCK_BUSY_ERRNOS:
            raise UsageError("cannot lock %s: %s" % (path, exc.strerror))
        return False


def acquire_lock(path, wait_s):
    """An exclusive flock on path, waiting up to wait_s seconds for it.

    flock and not a POSIX record lock: lockf(1) takes the same lock, so a
    tool run and the `lockf -k` shell recipe exclude each other.  A busy
    lock is an environment error (exit 2), not a failed run.
    """
    fd = open_lock_file(path)
    deadline = time.monotonic() + wait_s
    while not try_lock(fd, path):
        if time.monotonic() >= deadline:
            os.close(fd)
            raise UsageError(
                "lock busy: %s is held by another run (waited %gs)"
                % (path, wait_s))
        time.sleep(LOCK_POLL_S)
    return fd


@contextlib.contextmanager
def held_lock(path, wait_s):
    """Hold the lock for boot, child and shutdown, then release it."""
    fd = acquire_lock(path, wait_s)
    try:
        yield
    finally:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


def resolve_device(xcrun, device):
    """(udid, state) for a simulator UDID or an exact device name.

    Names repeat across installed runtimes, so matches are ordered by
    (runtime, udid) and the first one wins.
    """
    result = capture([xcrun, "simctl", "list", "devices", "available", "--json"])
    if result.returncode != 0:
        raise UsageError("simctl list devices failed with exit %d"
                         % result.returncode)
    try:
        devices = json.loads(result.stdout)["devices"]
    except (ValueError, KeyError, TypeError):
        raise UsageError("simctl list devices: unexpected JSON on stdout")
    matches = []
    for runtime, entries in devices.items():
        for entry in entries or []:
            if device in (str(entry.get("udid", "")), entry.get("name")):
                matches.append((runtime, str(entry.get("udid", "")),
                                str(entry.get("state", ""))))
    if not matches:
        raise UsageError("device not found: %s" % device)
    matches.sort()
    return matches[0][1], matches[0][2]


def signal_group(proc, sig):
    """Signal the child's whole group so wrapper and payload both stop."""
    if proc.returncode is not None:
        return
    try:
        os.killpg(proc.pid, sig)
    except (ProcessLookupError, PermissionError):
        pass


def wait_child(proc, timeout):
    """Wait for the child; on timeout stop its group. -> timed_out."""
    try:
        proc.wait(timeout=timeout)
        return False
    except subprocess.TimeoutExpired:
        pass
    signal_group(proc, signal.SIGTERM)
    try:
        proc.wait(timeout=KILL_GRACE_S)
    except subprocess.TimeoutExpired:
        signal_group(proc, signal.SIGKILL)
        proc.wait()
    return True


def emit_stderr(line):
    """Copy one output line to stderr, tolerating a closed stderr."""
    try:
        sys.stderr.write(line)
        sys.stderr.flush()
    except (OSError, ValueError):
        pass


def pump(stream, log, watcher):
    """Copy the merged output to the log and to stderr, parsing each line."""
    try:
        for line in stream:
            if log is not None:
                log.write(line)
                log.flush()
            watcher.feed(line)
            emit_stderr(line)
    except (OSError, ValueError):
        pass


def observation(status, timed_out, watcher):
    """The run's status, from the child for sim and its console for device."""
    if timed_out:
        return Observation(None, None, True, watcher.ps3stop)
    if watcher.console_status:
        return Observation(watcher.console_exit, watcher.console_signal, False,
                           watcher.ps3stop)
    if status < 0:
        return Observation(None, -status, False, watcher.ps3stop)
    return Observation(status, None, False, watcher.ps3stop)


def run_child(command, env, timeout, log, watcher):
    """Run the child, merging its output, and observe it.

    The child gets its own session, so a timeout can stop the whole process
    group and cannot reach this tool.  The reader drains the pipe; wait()
    therefore never blocks on a full pipe.
    """
    try:
        proc = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, bufsize=1, start_new_session=True)
    except OSError as exc:
        raise UsageError("cannot run %s: %s" % (command[0], exc.strerror or exc))
    reader = threading.Thread(target=pump, args=(proc.stdout, log, watcher),
                              daemon=True)
    reader.start()
    timed_out = wait_child(proc, timeout)
    reader.join(timeout=READER_JOIN_S)
    try:
        proc.stdout.close()
    except (OSError, ValueError):
        pass
    return observation(proc.returncode, timed_out, watcher)


def apply_observation(report, observed):
    report["exit_code"] = observed.exit_code
    report["signal"] = observed.signal
    report["timed_out"] = observed.timed_out
    report["ps3stop"] = observed.ps3stop


def shutdown(xcrun, udid):
    """Shut down a simulator this run booted; a failure is only a warning."""
    result = capture([xcrun, "simctl", "shutdown", udid])
    if result.returncode != 0:
        print("ios_run.py: warning: simctl shutdown %s failed with exit %d"
              % (udid, result.returncode), file=sys.stderr)


def run_sim(args, report, xcrun, log):
    udid, state = resolve_device(xcrun, args.device)
    report["device"] = udid
    booted_here = state == "Shutdown"
    if booted_here:
        require_step([xcrun, "simctl", "boot", udid], "simctl boot")
    report["booted_by_tool"] = booted_here
    command = [xcrun, "simctl", "spawn", udid] + args.child
    report["command"] = command
    try:
        if booted_here:
            require_step([xcrun, "simctl", "bootstatus", udid, "-b"],
                         "simctl bootstatus")
        watcher = Watcher(console_status=False)
        apply_observation(report, run_child(
            command, child_env(args.env, "SIMCTL_CHILD_"), args.timeout,
            log, watcher))
    finally:
        if booted_here and not args.keep_booted:
            shutdown(xcrun, udid)


def run_device(args, report, xcrun, log):
    report["device"] = args.udid
    report["bundle_id"] = args.bundle_id
    devicectl = [xcrun, "devicectl", "device"]
    if args.app:
        require_step(devicectl + ["install", "app", "--device", args.udid,
                                  args.app], "devicectl install")
    command = devicectl + ["process", "launch", "--device", args.udid,
                           "--terminate-existing", "--console",
                           args.bundle_id] + args.child
    report["command"] = command
    watcher = Watcher(console_status=True)
    apply_observation(report, run_child(
        command, child_env(args.env, "DEVICECTL_CHILD_"), args.timeout,
        log, watcher))


def decide_stop(report):
    """(--expect-stop KIND): pass only for that stop, exiting 70."""
    stop = report["ps3stop"]
    want = report["expected_stop"]
    if stop is None:
        return "fail", "expected PS3STOP %s, saw none" % want
    if stop["kind"] != want:
        return "fail", "expected PS3STOP %s, saw %s" % (want, stop["kind"])
    if report["exit_code"] != STOP_EXIT or report["signal"] is not None:
        return "fail", "expected exit %d, got %s" % (STOP_EXIT, status_text(report))
    return "pass", "ok"


def decide_outcome(report):
    """(outcome, reason) for a run that reached the child."""
    if report["timed_out"]:
        return "fail", "timed out"
    if report["expected_stop"] is not None:
        return decide_stop(report)
    if report["ps3stop"] is not None:
        return "fail", "unexpected PS3STOP %s" % report["ps3stop"]["kind"]
    if report["exit_code"] == 0 and report["signal"] is None:
        return "pass", "ok"
    return "fail", status_text(report)


def new_report(args):
    """The report skeleton, filled in by the run and by decide_outcome."""
    return {
        "target": args.target,
        "device": args.device if args.target == "sim" else args.udid,
        "bundle_id": getattr(args, "bundle_id", None),
        "command": [],
        "exit_code": None,
        "signal": None,
        "timed_out": False,
        "ps3stop": None,
        "expected_stop": args.expect_stop,
        "outcome": "error",
        "log": os.path.abspath(args.log) if args.log else None,
        "duration_s": 0.0,
        "load_avg": load_average(),
        "booted_by_tool": False,
    }


def summary_line(report, reason):
    """The one human line for a run, without --json."""
    stop = report["ps3stop"]["kind"] if report["ps3stop"] is not None else "none"
    return ("ios_run.py: %s %s outcome=%s (%s): %s, timed_out=%s, ps3stop=%s, %.2fs"
            % (report["target"], report["device"], report["outcome"], reason,
               status_text(report), str(report["timed_out"]).lower(), stop,
               report["duration_s"]))


def open_log(path):
    """The --log handle, validated before anything is booted."""
    if not path:
        return None
    try:
        return open(path, "w", encoding="utf-8")
    except OSError as exc:
        raise UsageError("cannot write log %s: %s" % (path, exc.strerror))


def add_run_options(parser):
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        metavar="S",
                        help="kill the child's process group after S seconds"
                             " (default %(default)s)")
    parser.add_argument("--lock", default=DEFAULT_LOCK, metavar="PATH",
                        help="lock held for the whole run (default %(default)s)")
    parser.add_argument("--lock-wait", type=float, default=DEFAULT_LOCK_WAIT,
                        metavar="S",
                        help="wait at most S seconds for the lock"
                             " (default %(default)s)")
    parser.add_argument("--log", metavar="FILE",
                        help="also write the child's output to FILE")
    parser.add_argument("--env", action="append", default=[], metavar="K=V",
                        help="pass K to the child as SIMCTL_CHILD_K or"
                             " DEVICECTL_CHILD_K; the value is never printed")
    parser.add_argument("--expect-stop", metavar="KIND",
                        help="pass only if PS3STOP KIND appeared and the exit"
                             " status is 70")
    parser.add_argument("--json", action="store_true",
                        help="write one JSON report object to stdout")


def build_parser():
    parser = argparse.ArgumentParser(
        prog="ios_run.py",
        description="Run a test program in the iOS Simulator or on a device.",
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="target", metavar="{sim,device}",
                                required=True)

    sim = sub.add_parser("sim", help="run a program inside a simulator",
                         description="Spawn a program in a simulator.")
    sim.add_argument("--device", required=True, metavar="UDID|NAME",
                     help="simulator to use, by UDID or exact name")
    sim.add_argument("--keep-booted", action="store_true",
                     help="leave a simulator the tool booted running")
    add_run_options(sim)

    device = sub.add_parser("device", help="launch an installed app on a device",
                            description="Launch an installed app on a device.")
    device.add_argument("--udid", required=True, metavar="UDID",
                        help="connected device")
    device.add_argument("--bundle-id", required=True, metavar="ID",
                        help="app bundle identifier")
    device.add_argument("--app", metavar="PATH",
                        help="install this .app before launching")
    add_run_options(device)
    return parser


def split_argv(argv):
    """(options, child argv) split at the first --.

    The child's own flags must never be read as the tool's, so everything
    after -- is left untouched, including -h and --json.
    """
    if "--" not in argv:
        return argv, None
    index = argv.index("--")
    return argv[:index], argv[index + 1:]


def run_target(args, report, log):
    """Run the selected target under the lock and fill the report."""
    xcrun = xcrun_path()
    with held_lock(args.lock, args.lock_wait):
        if args.target == "sim":
            run_sim(args, report, xcrun, log)
        else:
            run_device(args, report, xcrun, log)


def emit_report(report, reason, as_json):
    """The report on stdout with --json, else one summary line on stderr."""
    if not as_json:
        print(summary_line(report, reason), file=sys.stderr)
        return
    sys.stdout.write(json.dumps(report, sort_keys=True) + "\n")
    if report["outcome"] == "error":
        print("ios_run.py: error: " + reason, file=sys.stderr)


def main(argv=None):
    options, child = split_argv(list(sys.argv[1:] if argv is None else argv))
    args = build_parser().parse_args(options)
    args.child = child or []
    started = time.monotonic()
    report = new_report(args)
    log = None
    reason = ""
    try:
        if args.target == "sim" and not args.child:
            raise UsageError("sim: missing program after '--'")
        parse_env(args.env)
        log = open_log(args.log)
        run_target(args, report, log)
        report["outcome"], reason = decide_outcome(report)
    except UsageError as exc:
        report["outcome"], reason = "error", str(exc)
    finally:
        if log is not None:
            log.close()
    report["duration_s"] = round(time.monotonic() - started, 3)
    emit_report(report, reason, args.json)
    return {"pass": EXIT_OK, "fail": EXIT_FAIL}.get(report["outcome"], EXIT_USAGE)


if __name__ == "__main__":
    sys.exit(main())
