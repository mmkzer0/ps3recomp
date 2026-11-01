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

Output is decoded per line with replacement, so undecodable bytes become
U+FFFD in the log and on stderr instead of stopping the reader.  SIGINT and
SIGTERM stop the child's process group, shut down a simulator this run
booted, release the lock and exit 2.

Stopping devicectl does not stop the app: it keeps running on the device.  A
device run that the host ends (timeout, SIGINT/SIGTERM, devicectl gone
without the app's status) therefore finds the app's process by its
executable inside the installed bundle, asks it to end (SIGTERM), waits a
bounded grace, kills it (SIGKILL) and checks the process list again.  The
report says which step ended it, or that the cleanup failed, which fails the
run.
"""

import argparse
import contextlib
import errno
import fcntl
import json
import os
import posixpath
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from urllib.parse import unquote, urlparse

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
CLEANUP_GRACE_S = 10.0
CLEANUP_CALL_S = 20.0
CLEANUP_POLL_S = 1.0
TRACK_POLL_S = 1.0
CALL_POLL_S = 0.1
CALL_SLACK_S = 2.0

LOCK_BUSY_ERRNOS = frozenset((errno.EACCES, errno.EAGAIN))

# A stop line is `PS3STOP v=1 kind=<kind> <k>=<v> ...`; the fields run to the
# end of the line and each value is the token up to the next space.
STOP_RE = re.compile(r"PS3STOP v=1 kind=(\S+)(?: (.*))?$")

# devicectl reports the app's own status on its console.  The trailing period
# is the tool's punctuation, not part of the contract, so it is optional.
DEVICE_EXIT_RE = re.compile(r"The app terminated with the exit code (-?\d+)\b")
DEVICE_SIGNAL_RE = re.compile(r"App terminated due to signal (-?\d+)\b")

# devicectl prints this once the app is running, before it waits on it.
LAUNCHED_RE = re.compile(r"Launched application with \S+ bundle identifier")

EPILOG = """\
The run holds --lock (an exclusive flock, which lockf(1) also takes) across
boot, child and shutdown, and records the load average and its own duration
in the report.

sim needs `-- EXE [ARG...]`; device takes `[-- ARG...]` for the app.

A device run that the host ends also stops the app on the device: SIGTERM,
then SIGKILL, checked against the device's process list; the report's
app_cleanup says which step ended it.

Exit status:
  0  the run passed
  1  it failed: non-zero exit, a signal, a timeout, an unexpected PS3STOP,
     or a stop that was not the expected one
  2  usage or environment error: bad arguments, xcrun missing, an unknown or
     ambiguous simulator name, a busy lock, a failed install, a devicectl
     launch that failed before the app reported a status, an interruption

Examples:
  ios_run.py sim --device 'iPhone 17' -- ./build/host_fiber
  ios_run.py sim --device UDID --timeout 30 --expect-stop fiber-host-context -- ./build/host_fiber
  ios_run.py device --udid UDID --bundle-id com.example.title --json
"""


class UsageError(Exception):
    """The run cannot start or cannot be set up; the CLI maps it to exit 2."""


class Interrupted(BaseException):
    """SIGINT or SIGTERM reached the tool; unwinding runs the cleanup.

    A BaseException, like KeyboardInterrupt, so no broad `except Exception`
    swallows it on the way to the cleanup blocks.
    """

    def __init__(self, signum):
        super().__init__(signum)
        self.signum = signum


class Interrupts:
    """SIGINT/SIGTERM as an exception in the main thread.

    The child runs in its own session, so a signal sent to the tool never
    reaches it; the exception lets run_child stop the child's group before
    run_sim shuts the simulator down and held_lock lets go.  The first signal
    is the only one acted on: later ones are ignored so they cannot cut the
    cleanup short.  Inside deferring() the exception waits for the block's
    end, so a half-finished `simctl shutdown` cannot leave a simulator up.
    """

    SIGNALS = (signal.SIGINT, signal.SIGTERM)

    def __init__(self):
        self.deferred = 0
        self.pending = False
        self.signum = None

    def handle(self, signum, frame):
        for sig in self.SIGNALS:
            signal.signal(sig, signal.SIG_IGN)
        self.signum = signum
        if self.deferred:
            self.pending = True
            return
        raise Interrupted(signum)

    @contextlib.contextmanager
    def installed(self):
        previous = [(sig, signal.signal(sig, self.handle)) for sig in self.SIGNALS]
        try:
            yield
        finally:
            for sig, handler in previous:
                signal.signal(sig, handler)

    @contextlib.contextmanager
    def deferring(self):
        self.deferred += 1
        try:
            yield
        finally:
            self.deferred -= 1
            if not self.deferred and self.pending:
                self.pending = False
                raise Interrupted(self.signum)

    def run_deferred(self, work):
        """Run work so that no interruption can start it late or cut it short.

        A signal that lands before the block is entered is retried, not lost,
        and one that lands inside it waits for the block's end; the
        interruption is raised only after the work is done.
        """
        done = False
        pending = None
        while not done:
            try:
                with self.deferring():
                    work()
                    done = True
            except Interrupted as exc:
                pending = exc
        if pending is not None:
            raise pending


INTERRUPTS = Interrupts()


@dataclass
class Observation:
    """What one child run showed: its status and the stop it printed."""

    exit_code: int | None
    signal: int | None
    timed_out: bool
    ps3stop: dict | None
    reader_errors: list


class Watcher:
    """Per-line observations from the child's merged output.

    The stream is parsed while it is copied, so a stop or a devicectl status
    line is kept even when no log is written.  Only the first stop is kept: a
    run has one outcome, and the first stop is the one that ended it.  The
    devicectl status is the last status line instead: it is devicectl's
    epilogue, printed after the app has ended, so an app that prints the same
    words earlier cannot pose as the status.

    Anything the reader itself fails on is recorded in reader_errors and the
    drain goes on; decide_outcome turns a pass into a fail.
    """

    MAX_READER_ERRORS = 8

    def __init__(self, console_status):
        self.console_status = console_status
        self.ps3stop = None
        self.console = None
        self.launched = threading.Event()
        self.reader_errors = []
        self.closing = False

    def feed(self, line):
        text = line.rstrip("\r\n")
        if self.ps3stop is None:
            self.ps3stop = parse_stop(text)
        if not self.console_status:
            return
        if LAUNCHED_RE.search(text):
            self.launched.set()
        code = matched_int(DEVICE_EXIT_RE, text)
        if code is not None:
            self.console = ("exit", code)
        sig = matched_int(DEVICE_SIGNAL_RE, text)
        if sig is not None:
            self.console = ("signal", sig)

    def note_error(self, where, exc):
        message = "%s: %s: %s" % (where, type(exc).__name__, exc)
        if (message not in self.reader_errors
                and len(self.reader_errors) < self.MAX_READER_ERRORS):
            self.reader_errors.append(message)
            emit_stderr("ios_run.py: warning: reader %s\n" % message)


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


def short_runtime(runtime):
    return runtime.rsplit(".SimRuntime.", 1)[-1]


def resolve_device(xcrun, device):
    """(udid, state) for a simulator UDID or an exact device name.

    A name can exist on several installed runtimes, and the lowest runtime
    id is not the one a program was built for.  Several matches are therefore
    an environment error naming them, unless exactly one is Booted: that one
    is in use already and is taken, with a note.
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
    if len(matches) == 1:
        return matches[0][1], matches[0][2]
    booted = [match for match in matches if match[2] == "Booted"]
    if len(booted) == 1:
        runtime, udid, state = booted[0]
        print("ios_run.py: note: %s matches %d simulators; using the Booted "
              "one, %s on %s" % (device, len(matches), udid,
                                 short_runtime(runtime)), file=sys.stderr)
        return udid, state
    candidates = ", ".join("%s on %s (%s)" % (udid, short_runtime(runtime), state)
                           for runtime, udid, state in matches)
    raise UsageError("device name %s matches %d simulators: %s; pass one UDID"
                     % (device, len(matches), candidates))


def signal_group(proc, sig):
    """Signal the child's whole group so wrapper and payload both stop."""
    if proc.returncode is not None:
        return
    try:
        os.killpg(proc.pid, sig)
    except (ProcessLookupError, PermissionError):
        pass


def stop_group(proc):
    """SIGTERM the child's group, then SIGKILL it after the grace period."""
    signal_group(proc, signal.SIGTERM)
    try:
        proc.wait(timeout=KILL_GRACE_S)
    except subprocess.TimeoutExpired:
        signal_group(proc, signal.SIGKILL)
        proc.wait()


def wait_child(proc, timeout):
    """Wait for the child; on timeout stop its group. -> timed_out."""
    try:
        proc.wait(timeout=timeout)
        return False
    except subprocess.TimeoutExpired:
        pass
    stop_group(proc)
    return True


def emit_stderr(line):
    """Copy one output line to stderr, tolerating a closed stderr."""
    try:
        sys.stderr.write(line)
        sys.stderr.flush()
    except (OSError, ValueError):
        pass


def pump(stream, log, watcher):
    """Copy the merged output to the log and to stderr, parsing each line.

    Bytes are decoded per line with replacement: the guest writes raw bytes
    (Shift-JIS, a cut multibyte string), and a strict decoder would end the
    reader, leave the pipe undrained and stall the child.  A failure in one
    sink is recorded and the others and the drain carry on.
    """
    while True:
        try:
            raw = stream.readline()
        except Exception as exc:
            # run_child closes the pipe itself once the child is gone.
            if not watcher.closing:
                watcher.note_error("read", exc)
            return
        if not raw:
            return
        line = raw.decode("utf-8", errors="replace")
        if log is not None:
            try:
                log.write(line)
                log.flush()
            except Exception as exc:
                watcher.note_error("log", exc)
                log = None
        try:
            watcher.feed(line)
        except Exception as exc:
            watcher.note_error("parse", exc)
        emit_stderr(line)


def describe_status(status):
    """A child's wait status as a sentence part."""
    if status < 0:
        return "was killed by signal %d" % -status
    return "exited %d" % status


def observation(status, timed_out, watcher):
    """The run's status, from the child for sim and its console for device.

    devicectl that fails before the app ran (device locked, app missing, bad
    bundle id) prints no status line and exits non-zero: that is the
    environment, not a failed program, so it is exit 2.
    """
    code = sig = None
    if timed_out:
        pass
    elif watcher.console_status:
        if watcher.console is not None:
            kind, number = watcher.console
            code, sig = (number, None) if kind == "exit" else (None, number)
        elif status != 0:
            raise UsageError("devicectl launch %s before the app reported a "
                             "status" % describe_status(status))
    elif status < 0:
        sig = -status
    else:
        code = status
    return Observation(code, sig, timed_out, watcher.ps3stop,
                       list(watcher.reader_errors))


def run_child(command, env, timeout, log, watcher):
    """Run the child, merging its output, and observe it.

    The child gets its own session, so a timeout can stop the whole process
    group and cannot reach this tool; an interruption of the tool stops the
    group here for the same reason.  The reader drains the pipe; wait()
    therefore never blocks on a full pipe.
    """
    try:
        proc = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                start_new_session=True)
    except OSError as exc:
        raise UsageError("cannot run %s: %s" % (command[0], exc.strerror or exc))
    reader = threading.Thread(target=pump, args=(proc.stdout, log, watcher),
                              daemon=True)
    reader.start()
    try:
        timed_out = wait_child(proc, timeout)
    except BaseException:
        stop_group(proc)
        raise
    finally:
        reader.join(timeout=READER_JOIN_S)
        watcher.closing = True
        try:
            proc.stdout.close()
        except (OSError, ValueError):
            pass
    return observation(proc.returncode, timed_out, watcher)


class DeviceCtlError(Exception):
    """A devicectl query or request did not complete; the message says why."""


def last_line(text):
    """The last non-empty line of text, cut to a reportable length."""
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    return lines[-1][:200] if lines else ""


def ctl_call(command, limit, cancel=None):
    """Run one devicectl call, bounded by limit seconds. -> (status, text)

    The call gets its own session so the kill reaches whatever xcrun started.
    cancel ends it early for a poller that is being stopped.
    """
    try:
        proc = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                start_new_session=True)
    except OSError as exc:
        raise DeviceCtlError("cannot run %s: %s" % (command[0], exc.strerror or exc))
    deadline = time.monotonic() + limit + CALL_SLACK_S
    try:
        while True:
            try:
                out, _ = proc.communicate(timeout=CALL_POLL_S)
                return proc.returncode, out.decode("utf-8", errors="replace")
            except subprocess.TimeoutExpired:
                pass
            if cancel is not None and cancel.is_set():
                raise DeviceCtlError("cancelled")
            if time.monotonic() >= deadline:
                raise DeviceCtlError("timed out after %gs" % limit)
    except BaseException:
        with contextlib.suppress(ProcessLookupError, PermissionError):
            os.killpg(proc.pid, signal.SIGKILL)
        proc.communicate()
        raise


def file_path(value):
    """The normalised absolute path of a devicectl file URL or plain path."""
    path = unquote(urlparse(value).path) if value.startswith("file:") else value
    if not path.startswith("/"):
        raise ValueError("not an absolute path: %s" % value)
    return posixpath.normpath(path)


def bundle_processes(listing, bundle):
    """{pid: path} of the listed processes whose executable is the bundle's own.

    Only a file directly inside the .app directory counts: an app extension
    runs from a subdirectory and is not the app.
    """
    return {pid: path for pid, path in listing.items()
            if path is not None and posixpath.dirname(path) + "/" == bundle}


class AppControl:
    """The devicectl calls that find and stop one app on one device."""

    def __init__(self, xcrun, udid, bundle_id, limit):
        self.xcrun = xcrun
        self.udid = udid
        self.bundle_id = bundle_id
        self.limit = limit

    def run(self, words, cancel=None, json_path=None):
        command = [self.xcrun, "devicectl", "device"] + words + [
            "--device", self.udid, "--timeout", "%g" % self.limit]
        if json_path is not None:
            command += ["--json-output", json_path]
        what = " ".join(words[:2])
        try:
            status, text = ctl_call(command, self.limit, cancel)
        except DeviceCtlError as exc:
            raise DeviceCtlError("%s: %s" % (what, exc))
        if status != 0:
            detail = last_line(text)
            raise DeviceCtlError("%s exited %d%s"
                                 % (what, status, ": " + detail if detail else ""))

    def query(self, words, cancel=None):
        """The "result" object of a devicectl JSON answer."""
        with tempfile.TemporaryDirectory(prefix="ios-run-") as tmp:
            path = os.path.join(tmp, "result.json")
            self.run(words, cancel, path)
            try:
                with open(path, encoding="utf-8") as handle:
                    return json.load(handle)["result"]
            except (OSError, ValueError, KeyError, TypeError) as exc:
                raise DeviceCtlError("%s: no usable JSON result (%s: %s)"
                                     % (" ".join(words[:2]), type(exc).__name__, exc))

    def bundle_dir(self):
        """The installed bundle's directory, with a trailing slash."""
        result = self.query(["info", "apps", "--bundle-id", self.bundle_id])
        try:
            for app in result["apps"]:
                if app["bundleIdentifier"] == self.bundle_id:
                    return file_path(app["url"]) + "/"
        except (KeyError, TypeError, AttributeError, ValueError) as exc:
            raise DeviceCtlError("info apps: unexpected result (%s: %s)"
                                 % (type(exc).__name__, exc))
        raise DeviceCtlError("%s is not in the device's app list" % self.bundle_id)

    def listing(self, cancel=None):
        """{pid: executable path, or None when not given} of every process."""
        result = self.query(["info", "processes"], cancel)
        try:
            entries = result["runningProcesses"]
            processes = {}
            for entry in entries:
                pid = entry["processIdentifier"]
                exe = entry.get("executable")
                if not isinstance(pid, int) or isinstance(pid, bool):
                    raise TypeError("processIdentifier %r" % (pid,))
                processes[pid] = file_path(exe) if isinstance(exe, str) else None
            return processes
        except (KeyError, TypeError, AttributeError, ValueError) as exc:
            raise DeviceCtlError("info processes: unexpected result (%s: %s)"
                                 % (type(exc).__name__, exc))

    def gone(self, pid, path, grace):
        """(True, None) once pid is no longer the app's process, else (False, why).

        A pid listed with another executable was reused after the app ended.
        An unreadable listing never counts as gone: the end was not seen.
        """
        deadline = time.monotonic() + grace
        poll = min(CLEANUP_POLL_S, grace / 4)
        while True:
            why = None
            try:
                running = self.listing().get(pid) == path
            except DeviceCtlError as exc:
                running, why = True, "cannot verify: %s" % exc
            if not running:
                return True, None
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False, why
            time.sleep(min(poll, remaining))

    def stop_app(self, bundle, tracked, grace):
        """(pid, result, detail) of ending the app's process.

        result is exited (already gone), sigterm or sigkill (the step after
        which the pid left the list), unmatched (no process of the bundle is
        listed, nothing was signalled) or failed.  tracked is the (pid, path)
        recorded at launch, or None.  Only a pid whose executable is the
        bundle's is ever signalled.
        """
        try:
            listing = self.listing()
        except DeviceCtlError as exc:
            return (tracked[0] if tracked else None, "failed",
                    "cannot list the device's processes: %s" % exc)
        live = bundle_processes(listing, bundle)
        if tracked is not None:
            pid, path = tracked
            if listing.get(pid) != path:
                others = sorted(set(live) - {pid})
                if others:
                    return pid, "failed", (
                        "pid %d of the launched app is gone but pid %d of the "
                        "same bundle is listed; not touched" % (pid, others[0]))
                reused = listing.get(pid)
                return pid, "exited", (
                    "pid %d now runs %s; not touched" % (pid, reused)
                    if reused is not None else None)
        elif len(live) == 1:
            pid, path = next(iter(live.items()))
        elif live:
            return None, "failed", (
                "several processes of the bundle are listed (pids %s); none was "
                "signalled" % ", ".join(str(p) for p in sorted(live)))
        else:
            return None, "unmatched", (
                "no process runs from %s; nothing was signalled" % bundle)
        notes = []
        steps = (("sigterm", ["process", "terminate", "--pid", str(pid)]),
                 ("sigkill", ["process", "signal", "--signal", "SIGKILL",
                              "--pid", str(pid)]))
        for name, words in steps:
            try:
                self.run(words)
            except DeviceCtlError as exc:
                notes.append("%s request: %s" % (name, exc))
            ended, why = self.gone(pid, path, grace)
            if ended:
                return pid, name, "; ".join(notes) or None
            if why:
                notes.append("after %s %s" % (name, why))
        notes.insert(0, "pid %d is still listed after SIGKILL" % pid)
        return pid, "failed", "; ".join(notes)


class AppTracker:
    """Records the app's pid once devicectl reports the launch.

    The console attach blocks until the app ends, so the lookup runs on a
    thread of its own.  A failed lookup is not an error here: the cleanup
    asks again and reports its own failure.
    """

    def __init__(self, control, bundle, watcher):
        self.control = control
        self.bundle = bundle
        self.watcher = watcher
        self.cancel = threading.Event()
        self.lock = threading.Lock()
        self.found = None
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()

    def run(self):
        while not self.cancel.is_set():
            if self.watcher.launched.wait(CALL_POLL_S):
                try:
                    live = bundle_processes(self.control.listing(self.cancel),
                                            self.bundle)
                except DeviceCtlError:
                    live = {}
                if len(live) == 1:
                    with self.lock:
                        self.found = next(iter(live.items()))
                    return
                self.cancel.wait(TRACK_POLL_S)

    def stop(self):
        """End the lookup, and return the recorded (pid, path) or None."""
        self.cancel.set()
        self.thread.join(timeout=READER_JOIN_S)
        with self.lock:
            return self.found


def apply_observation(report, observed):
    report["exit_code"] = observed.exit_code
    report["signal"] = observed.signal
    report["timed_out"] = observed.timed_out
    report["ps3stop"] = observed.ps3stop
    report["reader_errors"] = observed.reader_errors


def settle_app(report, control, bundle, tracker, watcher, observed, interrupted,
               grace):
    """Stop the app when the host ended the run without the app's own status."""
    tracked = tracker.stop()
    report["app_pid"] = tracked[0] if tracked else None
    timed_out = observed is not None and observed.timed_out
    if watcher.console is not None and not timed_out:
        return
    if not (timed_out or interrupted or watcher.launched.is_set()):
        return
    pid, result, detail = control.stop_app(bundle, tracked, grace)
    if pid is not None:
        report["app_pid"] = pid
    report["app_cleanup"] = result
    report["app_cleanup_detail"] = detail


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
    report["booted_by_tool"] = booted_here
    command = [xcrun, "simctl", "spawn", udid] + args.child
    report["command"] = command
    # The boot is inside the try: an interruption during `simctl boot` can
    # leave a simulator booting, and only the finally block shuts it down.
    try:
        if booted_here:
            require_step([xcrun, "simctl", "boot", udid], "simctl boot")
            require_step([xcrun, "simctl", "bootstatus", udid, "-b"],
                         "simctl bootstatus")
        watcher = Watcher(console_status=False)
        apply_observation(report, run_child(
            command, child_env(args.env, "SIMCTL_CHILD_"), args.timeout,
            log, watcher))
    finally:
        if booted_here and not args.keep_booted:
            with INTERRUPTS.deferring():
                shutdown(xcrun, udid)


def run_device(args, report, xcrun, log):
    report["device"] = args.udid
    report["bundle_id"] = args.bundle_id
    devicectl = [xcrun, "devicectl", "device"]
    if args.app:
        require_step(devicectl + ["install", "app", "--device", args.udid,
                                  args.app], "devicectl install")
    # Resolved before the launch: an app that cannot be found again cannot be
    # stopped again, and such a run must not start.
    control = AppControl(xcrun, args.udid, args.bundle_id,
                         args.cleanup_call_timeout)
    try:
        bundle = control.bundle_dir()
    except DeviceCtlError as exc:
        raise UsageError("cannot resolve the installed bundle of %s: %s"
                         % (args.bundle_id, exc))
    command = devicectl + ["process", "launch", "--device", args.udid,
                           "--terminate-existing", "--console",
                           args.bundle_id] + args.child
    report["command"] = command
    watcher = Watcher(console_status=True)
    tracker = AppTracker(control, bundle, watcher)
    observed = None
    interrupted = False
    try:
        tracker.start()
        observed = run_child(
            command, child_env(args.env, "DEVICECTL_CHILD_"), args.timeout,
            log, watcher)
    except Interrupted:
        interrupted = True
        raise
    finally:
        INTERRUPTS.run_deferred(lambda: settle_app(
            report, control, bundle, tracker, watcher, observed, interrupted,
            args.cleanup_grace))
    apply_observation(report, observed)


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


def cleanup_note(report):
    """The app cleanup as a clause of a reason, or "" when there was none."""
    result = report["app_cleanup"]
    if result is None:
        return ""
    text = {"exited": "the app had already exited",
            "sigterm": "the app ended after SIGTERM",
            "sigkill": "the app ended only after SIGKILL",
            "unmatched": "no app process was found",
            "failed": "app cleanup failed"}[result]
    if report["app_pid"] is not None:
        text += " (pid %d)" % report["app_pid"]
    if report["app_cleanup_detail"]:
        text += ": " + report["app_cleanup_detail"]
    return text


def with_cleanup(report, reason):
    """reason, followed by what became of the app when the run had to stop it."""
    note = cleanup_note(report)
    return "%s; %s" % (reason, note) if note else reason


def decide_outcome(report):
    """(outcome, reason) for a run that reached the child.

    A run whose output could not be read completely never passes: the stop or
    status it would be judged on may be the part that was lost.  A cleanup
    that did not end the app fails the run whatever the app's status was.
    """
    outcome, reason = decide_observed(report)
    if outcome == "pass" and report["reader_errors"]:
        outcome, reason = "fail", "output not fully read: " + report["reader_errors"][0]
    reason = with_cleanup(report, reason)
    if report["app_cleanup"] == "failed":
        outcome = "fail"
    return outcome, reason


def decide_observed(report):
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
        "reader_errors": [],
        "expected_stop": args.expect_stop,
        "outcome": "error",
        "log": os.path.abspath(args.log) if args.log else None,
        "duration_s": 0.0,
        "load_avg": load_average(),
        "booted_by_tool": False,
        "app_pid": None,
        "app_cleanup": None,
        "app_cleanup_detail": None,
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
    device.add_argument("--cleanup-grace", type=float, default=CLEANUP_GRACE_S,
                        metavar="S",
                        help="after a timeout or interruption, wait S seconds"
                             " for the app to leave the process list after"
                             " SIGTERM, then after SIGKILL (default %(default)s)")
    device.add_argument("--cleanup-call-timeout", type=float,
                        default=CLEANUP_CALL_S, metavar="S",
                        help="bound each devicectl query or request about the"
                             " app at S seconds (default %(default)s)")
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
        with INTERRUPTS.installed():
            if args.target == "sim" and not args.child:
                raise UsageError("sim: missing program after '--'")
            parse_env(args.env)
            if args.target == "device" and (args.cleanup_grace < 0
                                            or args.cleanup_call_timeout <= 0):
                raise UsageError("--cleanup-grace needs S >= 0 and "
                                 "--cleanup-call-timeout needs S > 0")
            log = open_log(args.log)
            run_target(args, report, log)
            report["outcome"], reason = decide_outcome(report)
    except UsageError as exc:
        report["outcome"], reason = "error", with_cleanup(report, str(exc))
    except Interrupted as exc:
        report["outcome"] = "error"
        reason = with_cleanup(
            report, "interrupted by %s" % signal.Signals(exc.signum).name)
    finally:
        if log is not None:
            log.close()
    report["duration_s"] = round(time.monotonic() - started, 3)
    emit_report(report, reason, args.json)
    return {"pass": EXIT_OK, "fail": EXIT_FAIL}.get(report["outcome"], EXIT_USAGE)


if __name__ == "__main__":
    sys.exit(main())
