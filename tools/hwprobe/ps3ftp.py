#!/usr/bin/env python3
"""FTP helper for hwprobe probes (webMAN / multiMAN FTP server on the console, standard library only).

Everything happens inside /dev_hdd0/tmp/hwprobe on the console: remote names are checked and
cannot name another directory.

  ps3ftp.py --host ADDR ls                         list the directory
  ps3ftp.py --host ADDR put FILE [REMOTE_NAME]     upload (a SELF to launch, a config, ...)
  ps3ftp.py --host ADDR put-cfg PROBE FILE         upload FILE as PROBE.cfg (the probe reads it at start)
  ps3ftp.py --host ADDR rm-cfg PROBE               delete PROBE.cfg (back to the probe's defaults)
  ps3ftp.py --host ADDR get [DEST] [--prefix P]    download the result files (default DEST ./results)
  ps3ftp.py --host ADDR rm-results [--prefix P]    delete the result files on the console

ADDR is the console's address (or set HWPROBE_CONSOLE). Result files are every file that is not a SELF,
a config, a package or a run-id counter; --prefix restricts them to names starting with P (a probe name).
Options: --port N (default 21), --user/--password (webMAN accepts anything; default anonymous).
"""
import argparse
import ftplib
import os
import re
import sys

REMOTE_DIR = "/dev_hdd0/tmp/hwprobe"
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$")
NOT_RESULTS = (".self", ".cfg", ".pkg", ".elf")


def check_name(name):
    """A single file name for REMOTE_DIR; anything that could leave the directory is refused."""
    if not NAME_RE.match(name) or ".." in name:
        raise ValueError(f"refusing remote name {name!r} (plain file names only: letters, digits, . _ -)")
    return name


def remote_path(name):
    return f"{REMOTE_DIR}/{check_name(name)}"


def is_result(name, prefix=""):
    return (name.startswith(prefix) and not name.endswith(NOT_RESULTS) and not name.endswith("_runid.txt")
            and bool(NAME_RE.match(name)))


def list_names(ftp):
    out = []
    try:
        ftp.retrlines("NLST " + REMOTE_DIR, out.append)
    except ftplib.error_perm:
        return []  # directory does not exist yet: nothing there
    return sorted(n for n in (x.split("/")[-1] for x in out if x.strip()) if n not in (".", ".."))


def ensure_dir(ftp):
    try:
        ftp.mkd(REMOTE_DIR)
    except ftplib.error_perm:
        pass  # exists


def upload(ftp, local, remote_name):
    ensure_dir(ftp)
    with open(local, "rb") as fh:
        ftp.storbinary("STOR " + remote_path(remote_name), fh)
    return remote_path(remote_name)


def fetch_results(ftp, dest, prefix=""):
    os.makedirs(dest, exist_ok=True)
    got = []
    for n in list_names(ftp):
        if not is_result(n, prefix):
            continue
        path = os.path.join(dest, n)
        with open(path, "wb") as fh:
            ftp.retrbinary("RETR " + remote_path(n), fh.write)
        got.append(path)
    return got


def delete_results(ftp, prefix=""):
    gone = []
    for n in list_names(ftp):
        if is_result(n, prefix):
            ftp.delete(remote_path(n))
            gone.append(n)
    return gone


def connect(a):
    ftp = ftplib.FTP()
    ftp.connect(a.host, a.port, timeout=15)
    ftp.login(a.user, a.password)
    ftp.set_pasv(True)
    return ftp


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("HWPROBE_CONSOLE"))
    ap.add_argument("--port", type=int, default=21)
    ap.add_argument("--user", default="anonymous")
    ap.add_argument("--password", default="anonymous@")
    ap.add_argument("--prefix", default="")
    ap.add_argument("cmd", choices=["ls", "put", "put-cfg", "rm-cfg", "get", "rm-results"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args(argv)
    if not a.host:
        ap.error("console address missing: --host ADDR or HWPROBE_CONSOLE")
    try:
        if a.cmd == "put" and not 1 <= len(a.args) <= 2:
            ap.error("put FILE [REMOTE_NAME]")
        if a.cmd == "put-cfg" and len(a.args) != 2:
            ap.error("put-cfg PROBE FILE")
        if a.cmd == "rm-cfg" and len(a.args) != 1:
            ap.error("rm-cfg PROBE")
        if a.cmd == "put":
            check_name(a.args[1] if len(a.args) == 2 else os.path.basename(a.args[0]))
        if a.cmd in ("put-cfg", "rm-cfg"):
            check_name(a.args[0] + ".cfg")
    except ValueError as e:
        ap.error(str(e))
    ftp = connect(a)
    try:
        if a.cmd == "ls":
            for n in list_names(ftp):
                print(n)
        elif a.cmd == "put":
            name = a.args[1] if len(a.args) == 2 else os.path.basename(a.args[0])
            print(f"uploaded {a.args[0]} as {upload(ftp, a.args[0], name)}")
        elif a.cmd == "put-cfg":
            print("uploaded {} as {}".format(a.args[1], upload(ftp, a.args[1], a.args[0] + ".cfg")))
        elif a.cmd == "rm-cfg":
            ftp.delete(remote_path(a.args[0] + ".cfg"))
            print("deleted {}".format(remote_path(a.args[0] + ".cfg")))
        elif a.cmd == "get":
            got = fetch_results(ftp, a.args[0] if a.args else "results", a.prefix)
            for p in got:
                print(f"{p}  {os.path.getsize(p)} bytes")
            if not got:
                print(f"no result files in {REMOTE_DIR}")
                return 1
        elif a.cmd == "rm-results":
            for n in delete_results(ftp, a.prefix):
                print(f"deleted {n}")
    finally:
        try:
            ftp.quit()
        except (*ftplib.all_errors, OSError):
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
