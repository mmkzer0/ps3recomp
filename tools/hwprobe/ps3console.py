#!/usr/bin/env python3
"""webMAN MOD console runner for hwprobe probes.

Automates the execution of homebrew probes on a real PS3 console running webMAN MOD:
  1. Uploads fake-signed SELFs (and optional configs) to /dev_hdd0/tmp/hwprobe/ over FTP.
  2. Mounts the SELF via webMAN HTTP (/mount.ps3/dev_hdd0/tmp/hwprobe/<name>).
  3. Launches the SELF via webMAN HTTP (/play.ps3).
  4. Polls the FTP directory for newly generated result files matching the given prefix.
  5. Implements resilient FTP polling: webMAN drops the FTP control connection shortly
     after /play.ps3 starts a SELF. All poll operations use an auto-reconnecting wrapper
     that handles EOFError, OSError, and FTP errors with backoff until --timeout.
  6. Implements 8001003A retry logic: a play is retried only when a full --run-window
     has passed with NO sign of the probe having started. Signs of start include changes
     to <probe>_runid.txt, <probe>_last.txt, or any result file. Once any file changes,
     the probe is known to be running and /play.ps3 is NEVER retried.
  7. Partial fetch protection: does not fetch results when a result file merely appears
     in the directory. Waits until <probe>_last.txt has changed and its first line names
     the last expected result file (written by hwp_result_note_last after fclose).
  8. For --runs N, waits until N new result files exist AND _last.txt names the last of them.
  9. Safe unmount contract:
     - Unmount is requested ONLY after completion is confirmed and result files are fetched.
     - Never unmounts while a probe may be running (e.g. on timeout/error after activity).
     - On timeout or error after activity, leaves console mounted and reports
       'console left mounted; probe may still be running'.
     - Unmount is called on exit only if no probe activity was ever observed.

Strict HTTP path allow-list enforcement:
  Only these paths may ever be requested over HTTP:
    - /mount.ps3/dev_hdd0/tmp/hwprobe/<name>  (where <name> matches ps3ftp.NAME_RE)
    - /mount.ps3/unmount
    - /play.ps3
  Any other path (including /restart.ps3, /reboot.ps3, /shutdown.ps3, and path traversal)
  is strictly refused.
"""
import argparse
import ftplib
import http.client
import os
import sys
import time
import urllib.error
import urllib.request

try:
    from . import ps3ftp
except ImportError:
    # When run directly as a script
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import ps3ftp


ALLOW_EXACT_PATHS = {
    "/mount.ps3/unmount",
    "/play.ps3",
}
MOUNT_PREFIX = "/mount.ps3/dev_hdd0/tmp/hwprobe/"


def check_http_path(path: str) -> str:
    """Validate that the HTTP path is strictly on the allow-list.

    Refuses any path other than:
      - /mount.ps3/dev_hdd0/tmp/hwprobe/<name> (where <name> matches ps3ftp.NAME_RE)
      - /mount.ps3/unmount
      - /play.ps3

    Refuses reboot/restart/shutdown, path traversal (..), query strings, and fragments.
    """
    if not isinstance(path, str):
        raise TypeError(f"HTTP path must be a string, got {type(path)}")
    if path in ALLOW_EXACT_PATHS:
        return path
    if path.startswith(MOUNT_PREFIX):
        name = path[len(MOUNT_PREFIX):]
        if not name or "/" in name or "\\" in name or "?" in name or "#" in name:
            raise ValueError(f"Refused HTTP path (invalid character or traversal): {path}")
        if name in (".", ".."):
            raise ValueError(f"Refused HTTP path (traversal): {path}")
        if not ps3ftp.NAME_RE.match(name):
            raise ValueError(f"Refused HTTP path (name does not match allowed pattern): {path}")
        return path
    raise ValueError(f"Refused non-allow-listed HTTP path: {path}")


def http_get(host: str, path: str, port: int = 80, timeout: float = 15.0) -> bytes:
    """Issue a GET request to webMAN on the console after strict allow-list validation."""
    valid_path = check_http_path(path)
    url = f"http://{host}:{port}{valid_path}"
    req = urllib.request.Request(url, headers={"User-Agent": "hwprobe/ps3console"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def mount_self(host: str, self_name: str, port: int = 80, timeout: float = 15.0) -> bytes:
    """Mount a fake-signed SELF via webMAN."""
    ps3ftp.check_name(self_name)
    path = f"{MOUNT_PREFIX}{self_name}"
    return http_get(host, path, port=port, timeout=timeout)


def play(host: str, port: int = 80, timeout: float = 15.0) -> bytes:
    """Trigger /play.ps3 on webMAN."""
    return http_get(host, "/play.ps3", port=port, timeout=timeout)


def unmount(host: str, port: int = 80, timeout: float = 15.0) -> bytes:
    """Unmount currently mapped image via /mount.ps3/unmount."""
    return http_get(host, "/mount.ps3/unmount", port=port, timeout=timeout)


def get_file_content(ftp, name: str):
    """Fetch raw file content from REMOTE_DIR, or None if not found (550)."""
    buf = bytearray()
    try:
        ftp.retrbinary("RETR " + ps3ftp.remote_path(name), buf.extend)
        return bytes(buf)
    except ftplib.error_perm as e:
        if str(e).startswith("550"):
            return None
        raise


def get_file_size(ftp, name: str):
    """Query size of file in REMOTE_DIR, or None if not found (550) or unsupported."""
    try:
        return ftp.size(ps3ftp.remote_path(name))
    except ftplib.error_perm as e:
        if str(e).startswith("550"):
            return None
        raise
    except (AttributeError, *ftplib.all_errors):
        return None


def download_result_file(ftp, rname: str, dest_path: str):
    """Download one result file over FTP to local dest_path."""
    with open(dest_path, "wb") as f:
        ftp.retrbinary("RETR " + ps3ftp.remote_path(rname), f.write)


def is_probe_result(name: str, prefix: str) -> bool:
    """Return True if name is an actual probe result file (excluding _last.txt and _runid.txt)."""
    return (
        ps3ftp.is_result(name, prefix)
        and not name.endswith("_last.txt")
        and not name.endswith("_runid.txt")
    )


class FtpSession:
    """Manages an FTP connection with reconnect-on-failure for poll operations."""

    def __init__(self, connect_fn, deadline: float, backoff: float = 1.0,
                 clock=time.monotonic, sleep=time.sleep):
        self.connect_fn = connect_fn
        self.deadline = deadline
        self.backoff = backoff
        self.clock = clock
        self.sleep = sleep
        self.ftp = None

    def connect(self):
        """Connect to FTP, retrying with backoff until deadline."""
        self.close()
        last_error = None
        while self.clock() < self.deadline:
            try:
                self.ftp = self.connect_fn()
                ps3ftp.ensure_dir(self.ftp)
                return self.ftp
            except (ftplib.Error, OSError, EOFError) as e:
                last_error = e
                self.close()
                self.sleep(self.backoff)
        err_msg = repr(last_error) if last_error else "deadline exceeded"
        raise TimeoutError(f"TIMEOUT: failed to connect to console FTP before deadline ({err_msg})")

    def close(self):
        if self.ftp is not None:
            try:
                self.ftp.quit()
            except (*ftplib.all_errors, OSError) as e:
                sys.stderr.write(f"[console] FTP quit notice: {e!r}\n")
                try:
                    self.ftp.close()
                except (*ftplib.all_errors, OSError) as e2:
                    sys.stderr.write(f"[console] FTP close notice: {e2!r}\n")
            self.ftp = None

    def call(self, fn, *args, **kwargs):
        """Call fn(self.ftp, *args, **kwargs). If an FTP connection error occurs, reconnect and retry."""
        last_error = None
        while self.clock() < self.deadline:
            if self.ftp is None:
                self.connect()
            try:
                return fn(self.ftp, *args, **kwargs)
            except (ftplib.Error, OSError, EOFError) as e:
                # Connection dropped by webMAN or socket closed
                last_error = e
                err_str = repr(e) if repr(e) else type(e).__name__
                print(f"[console] FTP operation notice ({err_str}), reconnecting...")
                self.close()
                self.sleep(self.backoff)
        err_msg = repr(last_error) if last_error else "deadline exceeded"
        raise TimeoutError(f"TIMEOUT: FTP operation failed and could not reconnect before deadline ({err_msg})")


def run_probe(
    self_path: str,
    prefix: str,
    run_window: float,
    probe: str | None = None,
    cfg: tuple | None = None,
    runs: int = 1,
    timeout: float = 120.0,
    out_dir: str = "results",
    host: str | None = None,
    ftp_port: int = 21,
    http_port: int = 80,
    user: str = "anonymous",
    password: str = "anonymous@",
    poll_interval: float = 2.0,
    reconnect_backoff: float = 1.0,
    ftp_client: object | None = None,
    clock=time.monotonic,
    sleep=time.sleep,
) -> list:
    """Execute a probe SELF on the console end-to-end with 8001003A retry logic.

    Args:
      self_path: Path to local fake-signed SELF file.
      prefix: Result file prefix to search for.
      run_window: Maximum time (seconds) one launch may take until its first result.
                  A retry of /play.ps3 is only issued after a full run_window has passed
                  with no change in any probe files.
      probe: Probe name for tracking files (<probe>_runid.txt, <probe>_last.txt).
             Defaults to prefix if not provided (assumption: probe == prefix).
      cfg: Optional (name, path) tuple for probe configuration file.
      runs: Number of expected result files (default 1).
      timeout: Overall execution deadline in seconds (default 120s).
      out_dir: Destination directory for fetched result CSV files.
      host: Console IP or hostname.
      ftp_port: FTP server port (default 21).
      http_port: webMAN HTTP server port (default 80).
      poll_interval: Frequency of FTP polling in seconds.
      reconnect_backoff: Delay in seconds before retrying FTP connection.
      ftp_client: Optional pre-connected/mock FTP client (for testing).
      clock: Monotonic time source in seconds, used for every interval (run window,
             overall timeout, FTP reconnect deadline). Injectable for tests.
      sleep: Sleep function used for poll and FTP reconnect backoff delays.

    Returns:
      List of locally fetched result file paths on success.

    Raises:
      TimeoutError: If execution exceeds timeout or fails after 3 play attempts.
      FileNotFoundError: If self_path or cfg file does not exist locally.
      ValueError: If arguments or HTTP paths fail validation.
    """
    if not host:
        raise ValueError("Console host missing: pass host or set HWPROBE_CONSOLE")
    if not os.path.isfile(self_path):
        raise FileNotFoundError(f"Local SELF file not found: {self_path}")
    if run_window <= 0:
        raise ValueError(f"run_window must be positive, got {run_window}")

    probe_name = probe if probe else prefix
    ps3ftp.check_name(probe_name)

    self_name = os.path.basename(self_path)
    ps3ftp.check_name(self_name)

    if cfg:
        cfg_name, cfg_path = cfg
        if not os.path.isfile(cfg_path):
            raise FileNotFoundError(f"Config file not found: {cfg_path}")
        ps3ftp.check_name(cfg_name + ".cfg")

    total_start = clock()
    deadline = total_start + timeout

    if ftp_client is not None:
        def connect_fn():
            ftp_client.connect(host, ftp_port)
            return ftp_client
    else:
        args_dummy = argparse.Namespace(host=host, port=ftp_port, user=user, password=password)
        def connect_fn():
            return ps3ftp.connect(args_dummy)

    session = FtpSession(connect_fn, deadline=deadline, backoff=reconnect_backoff, clock=clock, sleep=sleep)

    mounted = False
    probe_started = False
    completed = False
    new_results = []
    last_recorded_name = None
    play_attempts = 0

    try:
        session.connect()
        initial_names = set(session.call(ps3ftp.list_names))
        initial_runid = session.call(get_file_content, f"{probe_name}_runid.txt")
        initial_last = session.call(get_file_content, f"{probe_name}_last.txt")
        initial_result_files = {
            f: session.call(get_file_size, f)
            for f in initial_names if is_probe_result(f, prefix)
        }

        # Upload SELF
        uploaded_self = session.call(ps3ftp.upload, self_path, self_name)
        print(f"[console] uploaded {self_path} -> {uploaded_self}")

        # Upload optional config
        if cfg:
            cfg_name, cfg_path = cfg
            uploaded_cfg = session.call(ps3ftp.upload, cfg_path, cfg_name + ".cfg")
            print(f"[console] uploaded {cfg_path} -> {uploaded_cfg}")

        # Mount SELF
        print(f"[console] mounting {self_name} via webMAN HTTP...")
        mount_self(host, self_name, port=http_port)
        mounted = True

        play_attempts = 1
        print("[console] play attempt 1/3 (/play.ps3)...")
        play(host, port=http_port)
        launch_time = clock()

        while clock() - total_start < timeout:
            current_names = set(session.call(ps3ftp.list_names))
            current_runid = session.call(get_file_content, f"{probe_name}_runid.txt")
            current_last = session.call(get_file_content, f"{probe_name}_last.txt")

            # Detect probe activity
            if not probe_started:
                if current_runid != initial_runid or current_last != initial_last:
                    probe_started = True
                else:
                    for f in current_names:
                        if is_probe_result(f, prefix):
                            if f not in initial_result_files:
                                probe_started = True
                                break
                            sz = session.call(get_file_size, f)
                            if sz is not None and sz != initial_result_files.get(f):
                                probe_started = True
                                break
                if probe_started:
                    print("[console] probe activity detected; play will not be retried")

            # Check completion:
            # 1. _last.txt has been rewritten since baseline
            # 2. _last.txt's first line names the last expected result file (written after fclose)
            # 3. at least `runs` new result files are present
            new_results = sorted(
                f for f in current_names
                if f not in initial_result_files and is_probe_result(f, prefix)
            )

            if current_last is not None and current_last != initial_last:
                try:
                    first_line = current_last.decode("utf-8", "replace").splitlines()[0].strip()
                    last_recorded_name = os.path.basename(first_line)
                except (IndexError, UnicodeDecodeError):
                    last_recorded_name = ""

                if (
                    len(new_results) >= runs
                    and last_recorded_name
                    and new_results
                    and last_recorded_name == new_results[-1]
                ):
                    completed = True
                    print(
                        f"[console] probe completed: {len(new_results)} result(s) confirmed, "
                        f"last={last_recorded_name}"
                    )
                    break

            # 8001003A Retry logic:
            if not probe_started and clock() - launch_time >= run_window and play_attempts < 3:
                play_attempts += 1
                print(
                    f"[console] no probe activity after run window ({run_window}s), "
                    f"retrying play ({play_attempts}/3)..."
                )
                play(host, port=http_port)
                launch_time = clock()

            sleep(poll_interval)

        if not completed:
            elapsed = clock() - total_start
            activity_note = "; console left mounted; probe may still be running" if probe_started else ""
            raise TimeoutError(
                f"TIMEOUT: after {play_attempts} play attempts ({elapsed:.1f}s), "
                f"probe '{probe_name}' did not complete (found {len(new_results)}/{runs} results, "
                f"last.txt={last_recorded_name!r}){activity_note}"
            )

        # Download results while still mounted
        os.makedirs(out_dir, exist_ok=True)
        fetched = []
        for rname in new_results:
            dest = os.path.join(out_dir, rname)
            session.call(download_result_file, rname, dest)
            fetched.append(dest)
            print(f"[console] fetched {dest} ({os.path.getsize(dest)} bytes)")

        # UNMOUNT ONLY AFTER COMPLETION CONFIRMED AND RESULTS FETCHED
        print("[console] unmounting image...")
        try:
            unmount(host, port=http_port)
            mounted = False
        except (urllib.error.URLError, OSError, http.client.HTTPException) as e:
            err_str = repr(e) if not str(e) else str(e)
            print(f"[console] unmount notice: {err_str}")
        return fetched

    except Exception as e:
        if probe_started:
            # Leave mounted! Never unmount while probe may run
            err_str = repr(e) if not str(e) else str(e)
            if "console left mounted; probe may still be running" not in err_str:
                err_str += "; console left mounted; probe may still be running"
            raise TimeoutError(err_str) if isinstance(e, TimeoutError) else RuntimeError(err_str) from e
        else:
            # Before play or with no probe activity observed, fine to unmount
            if mounted:
                print("[console] unmounting image (no probe activity observed)...")
                try:
                    unmount(host, port=http_port)
                    mounted = False
                except (urllib.error.URLError, OSError, http.client.HTTPException) as e:
                    sys.stderr.write(f"[console] unmount notice: {e!r}\n")
            raise
    finally:
        session.close()


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--host", default=os.environ.get("HWPROBE_CONSOLE"),
                    help="Console address (default $HWPROBE_CONSOLE)")
    ap.add_argument("--port", type=int, default=21, help="FTP port (default 21)")
    ap.add_argument("--http-port", type=int, default=80, help="webMAN HTTP port (default 80)")
    ap.add_argument("--user", default="anonymous")
    ap.add_argument("--password", default="anonymous@")

    sub = ap.add_subparsers(dest="cmd", required=True)

    # run command
    run_p = sub.add_parser("run", help="Upload, mount, play, poll results, fetch, and unmount")
    run_p.add_argument("self", help="Local fake-signed SELF file to run")
    run_p.add_argument("--prefix", required=True, help="Result filename prefix to wait for")
    run_p.add_argument("--run-window", type=float, required=True,
                       help="Longest time one launch may take until its first result (seconds)")
    run_p.add_argument("--probe", default=None,
                       help="Probe name for _runid.txt and _last.txt (default: same as --prefix)")
    run_p.add_argument("--cfg", nargs=2, metavar=("NAME", "FILE"),
                       help="Optional config name and file to upload (uploaded as NAME.cfg)")
    run_p.add_argument("--runs", type=int, default=1,
                       help="Number of result files expected (default 1)")
    run_p.add_argument("--timeout", type=float, default=120.0,
                       help="Total timeout in seconds (default 120)")
    run_p.add_argument("--out", default="results",
                       help="Local directory to save results to (default results/)")
    run_p.add_argument("--poll-interval", type=float, default=2.0,
                       help="Polling interval in seconds (default 2s)")

    # mount command
    mount_p = sub.add_parser("mount", help="Mount a SELF file via webMAN")
    mount_p.add_argument("name", help="Remote SELF filename in /dev_hdd0/tmp/hwprobe/")

    # play command
    sub.add_parser("play", help="Trigger play.ps3 via webMAN")

    # unmount command
    sub.add_parser("unmount", help="Unmount current image via webMAN")

    a = ap.parse_args(argv)

    if not a.host:
        ap.error("console address missing: --host ADDR or HWPROBE_CONSOLE")

    try:
        if a.cmd == "run":
            cfg_tuple = tuple(a.cfg) if a.cfg else None
            got = run_probe(
                self_path=a.self,
                prefix=a.prefix,
                run_window=a.run_window,
                probe=a.probe,
                cfg=cfg_tuple,
                runs=a.runs,
                timeout=a.timeout,
                out_dir=a.out,
                host=a.host,
                ftp_port=a.port,
                http_port=a.http_port,
                user=a.user,
                password=a.password,
                poll_interval=a.poll_interval,
            )
            for p in got:
                print(f"RESULT: {p}")
            return 0
        elif a.cmd == "mount":
            mount_self(a.host, a.name, port=a.http_port)
            print(f"mounted {a.name}")
            return 0
        elif a.cmd == "play":
            play(a.host, port=a.http_port)
            print("play triggered")
            return 0
        elif a.cmd == "unmount":
            unmount(a.host, port=a.http_port)
            print("unmounted")
            return 0
    except (TimeoutError, ValueError, FileNotFoundError) as e:
        err_str = str(e) if str(e) else repr(e)
        sys.stderr.write(f"ERROR: {err_str}\n")
        return 1
    # ftplib.all_errors is a tuple: unpack it. A nested tuple in an except clause is a
    # TypeError on Python 3.15, which masked the real error and skipped the unmount.
    except (urllib.error.URLError, http.client.HTTPException, OSError, *ftplib.all_errors, RuntimeError) as e:
        err_str = str(e) if str(e) else repr(e)
        sys.stderr.write(f"FATAL: {err_str}\n")
        return 2

    return 0


if __name__ == "__main__":
    sys.exit(main())
