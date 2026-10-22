#!/usr/bin/env python3
"""Refuse game and firmware material in a repository.

The toolkit only stays publishable while it carries no Sony material: the
first ELF, container or key that lands in the index makes the tree private.
This is the mechanical gate for that rule.  It checks four things -- file
names, magic numbers at offset 0, blob size and symlink targets -- because
every finding here has to be explainable to the person about to commit.

    python3 tools/check_hygiene.py --staged         # local pre-commit hook
    python3 tools/check_hygiene.py --tracked        # whole index
    python3 tools/check_hygiene.py --paths dist/    # files, no git needed

`--staged` and `--tracked` read content from the index, never from the
working tree: a clean checkout of a bad commit must still fail, and a dirty
tree must not fail a commit that is clean.

Exit status: 0 clean, 1 findings, 2 usage or environment error.
"""

import argparse
import fnmatch
import json
import os
import posixpath
import re
import subprocess
import sys
from dataclasses import dataclass

DEFAULT_MAX_BYTES = 1024 * 1024
HEAD_BYTES = 64
LINK_TARGET_BYTES = 4096

SYMLINK_MODE = 0o120000
REGULAR_FILE = 0o100000

FORBIDDEN_EXTENSIONS = (
    ".elf",
    ".self",
    ".sprx",
    ".prx",
    ".pkg",
    ".sfo",
    ".rap",
    ".edat",
    ".sdat",
    ".cpk",
    ".pup",
    ".p3t",
    ".keys",
)

# Matching is on the lowercased basename; the display spelling is what the
# finding says, so a report reads the way a person writes these names.
FORBIDDEN_BASENAMES = (
    ("eboot.bin", "EBOOT.BIN"),
    ("param.sfo", "PARAM.SFO"),
    ("param.pfd", "PARAM.PFD"),
    ("act.dat", "act.dat"),
    ("idps", "idps"),
    ("*.rif", "*.rif"),
)

RULE_IDS = ("forbidden-name", "magic", "oversize", "symlink")

# Signatures at offset 0.  `SCE\0` is the SELF/SPRX envelope; the eight-byte
# `SCEUF` form is the firmware update package, and the two do not overlap.
MAGIC_PREFIXES = (
    (b"\x7fPKG", "pkg"),
    (b"\x00PSF", "psf/sfo"),
    (b"CPK ", "cri-cpk"),
    (b"SCEUF\x00\x00\x00", "pup"),
    (b"NPD\x00", "npd (edat/sdat)"),
    (b"SCE\x00", "sce-header"),
)

ELF_MAGIC = b"\x7fELF"
ELF_DATA_OFFSET = 5
ELF_MACHINE_OFFSET = 18
PS3_ELF_MACHINES = (0x15, 0x17)  # EM_PPC64, EM_SPU
BIG_ENDIAN = 2

# A symlink target can be absolute on the host that made it, and a POSIX
# checkout can still carry a Windows-shaped target.
WINDOWS_ABSOLUTE = re.compile(r"^(?:[A-Za-z]:[\\/]|\\\\)")
ALLOW_ENTRY = re.compile(
    r"^(?P<glob>\S+)\s+(?P<rule>\S+)\s+--\s+(?P<reason>\S.*)$")


class CheckError(Exception):
    """A usage or environment problem; the CLI maps it to exit status 2."""


@dataclass(frozen=True)
class Finding:
    path: str
    rule: str
    detail: str


@dataclass(frozen=True)
class AllowEntry:
    pattern: str
    rule: str


@dataclass(frozen=True)
class IndexEntry:
    path: str
    mode: int
    oid: str


def name_findings(path):
    """The forbidden-name rule: a basename or extension nobody may commit."""
    base = posixpath.basename(path).lower()
    for pattern, display in FORBIDDEN_BASENAMES:
        if fnmatch.fnmatchcase(base, pattern):
            return [Finding(path, "forbidden-name", f"forbidden name {display}")]
    for extension in FORBIDDEN_EXTENSIONS:
        if base.endswith(extension):
            return [Finding(path, "forbidden-name",
                            f"forbidden extension {extension}")]
    return []


def magic_label(head):
    """The PS3 signature at offset 0, or None.

    Little-endian ELFs and host ELFs are ordinary build products, not game
    material, so the ELF test needs both EI_DATA == 2 and a PS3 machine.
    """
    if head.startswith(ELF_MAGIC):
        if len(head) < ELF_MACHINE_OFFSET + 2:
            return None
        if head[ELF_DATA_OFFSET] != BIG_ENDIAN:
            return None
        machine = int.from_bytes(
            head[ELF_MACHINE_OFFSET:ELF_MACHINE_OFFSET + 2], "big")
        return "ps3-elf" if machine in PS3_ELF_MACHINES else None
    for prefix, label in MAGIC_PREFIXES:
        if head.startswith(prefix):
            return label
    return None


def content_findings(path, size, head, max_bytes):
    findings = []
    if size > max_bytes:
        findings.append(
            Finding(path, "oversize", f"{size} bytes exceeds {max_bytes}"))
    label = magic_label(head)
    if label is not None:
        findings.append(Finding(path, "magic", label))
    return findings


def symlink_detail(path, target):
    """Why a symlink leaves the repository, or None when it stays inside.

    The target is resolved against the directory holding the link, the way
    the kernel would.  A target that normalises to `..` or starts with
    `../` climbs out of the root; whether it exists does not matter.
    """
    if not target:
        return None
    if posixpath.isabs(target) or WINDOWS_ABSOLUTE.match(target):
        return f"absolute target {target}"
    resolved = posixpath.normpath(
        posixpath.join(posixpath.dirname(path), target))
    if resolved == posixpath.pardir or resolved.startswith(
            posixpath.pardir + "/"):
        return f"target {target} escapes the repository"
    return None


def _read_exact(stream, count):
    chunks = []
    while count:
        chunk = stream.read(count)
        if not chunk:
            raise CheckError("git cat-file ended early")
        chunks.append(chunk)
        count -= len(chunk)
    return b"".join(chunks)


def _discard(stream, count):
    while count:
        chunk = stream.read(min(count, 1 << 16))
        if not chunk:
            raise CheckError("git cat-file ended early")
        count -= len(chunk)


class BlobReader:
    """One `git cat-file --batch` process, bounded prefixes out.

    The batch header carries the blob size, so a large blob costs the first
    HEAD_BYTES in memory and a streamed skip for the rest.  The index is the
    source of truth: these are the bytes a commit would contain.
    """

    def __init__(self, root):
        try:
            self._process = subprocess.Popen(
                ["git", "-C", root, "cat-file", "--batch"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE)
        except OSError as exc:
            raise CheckError(f"cannot start git cat-file: {exc}")

    def prefix(self, oid, limit):
        stdin = self._process.stdin
        stdout = self._process.stdout
        if stdin is None or stdout is None:
            raise CheckError("git cat-file is not connected")
        try:
            stdin.write(oid.encode("ascii") + b"\n")
            stdin.flush()
            header = stdout.readline()
        except OSError as exc:
            raise CheckError(f"git cat-file failed: {exc}")
        fields = header.split()
        if len(fields) != 3 or fields[1] != b"blob":
            detail = header.decode("utf-8", "replace").strip() or "no response"
            raise CheckError(f"cannot read index blob {oid}: {detail}")
        try:
            size = int(fields[2])
        except ValueError:
            raise CheckError(f"cannot read index blob {oid}: bad size")
        head = _read_exact(stdout, min(size, limit))
        _discard(stdout, size - len(head))
        stdout.read(1)  # the record's trailing newline
        return size, head

    def close(self):
        process = self._process
        if process.stdin is not None:
            process.stdin.close()
        stderr = b""
        if process.stderr is not None:
            stderr = process.stderr.read()
        status = process.wait()
        if status != 0:
            detail = stderr.decode("utf-8", "replace").strip()
            raise CheckError(f"git cat-file failed: {detail or status}")


def git_stdout(root, *args):
    try:
        result = subprocess.run(["git", "-C", root, *args],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
    except OSError as exc:
        raise CheckError(f"cannot run git: {exc}")
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise CheckError(
            f"git {' '.join(args)} failed: {detail or result.returncode}")
    return result.stdout


def git_toplevel(root):
    top = os.fsdecode(
        git_stdout(root, "rev-parse", "--show-toplevel").strip())
    if not top:
        raise CheckError(f"not a git repository: {root}")
    return top


def split_z(data):
    return [part for part in data.split(b"\0") if part]


def index_entries(root, staged):
    """Index entries to check, with their modes and blob ids."""
    entries = []
    for record in split_z(git_stdout(root, "ls-files", "-s", "-z")):
        meta, _tab, path = record.partition(b"\t")
        fields = meta.split()
        if len(fields) != 3 or not path:
            raise CheckError(f"unexpected git ls-files record: {record!r}")
        entries.append(IndexEntry(
            os.fsdecode(path), int(fields[0], 8), fields[1].decode("ascii")))
    if staged:
        changed = {
            os.fsdecode(path) for path in split_z(
                git_stdout(root, "diff", "--cached", "--name-only",
                           "--diff-filter=ACMR", "-z"))
        }
        entries = [entry for entry in entries if entry.path in changed]
    entries.sort(key=lambda entry: (entry.path, entry.oid))
    return entries


def entry_findings(reader, entry, max_bytes):
    findings = name_findings(entry.path)
    if entry.oid.strip("0") == "":
        # Intent-to-add: the index has a path but no blob yet.
        return findings
    if entry.mode == SYMLINK_MODE:
        size, data = reader.prefix(entry.oid, LINK_TARGET_BYTES)
        if size > LINK_TARGET_BYTES:
            findings.append(
                Finding(entry.path, "symlink",
                        f"target longer than {LINK_TARGET_BYTES} bytes"))
        else:
            detail = symlink_detail(entry.path, os.fsdecode(data))
            if detail is not None:
                findings.append(Finding(entry.path, "symlink", detail))
    elif (entry.mode & 0o170000) == REGULAR_FILE:
        size, head = reader.prefix(entry.oid, HEAD_BYTES)
        findings.extend(content_findings(entry.path, size, head, max_bytes))
    # Submodule entries (0o160000) have no blob in this repository.
    return findings


def git_findings(root, staged, max_bytes):
    entries = index_entries(root, staged)
    findings = []
    reader = BlobReader(root)
    try:
        for entry in entries:
            findings.extend(entry_findings(reader, entry, max_bytes))
    finally:
        reader.close()
    return len({entry.path for entry in entries}), findings


def _walk(absolute):
    """Yield (absolute path, kind) for one filesystem path, recursing dirs."""
    if os.path.islink(absolute):
        # A link is checked as a link and never followed.
        yield absolute, "link"
        return
    if os.path.isdir(absolute):
        try:
            with os.scandir(absolute) as iterator:
                children = sorted(iterator, key=lambda child: child.name)
        except OSError as exc:
            raise CheckError(
                f"cannot read directory {absolute}: {exc.strerror or exc}")
        for child in children:
            # VCS internals are not repository content, and walking them
            # would flag packed blobs and the sample hooks.
            if child.name == ".git" and not child.is_symlink() \
                    and child.is_dir(follow_symlinks=False):
                continue
            yield from _walk(child.path)
        return
    if os.path.isfile(absolute):
        yield absolute, "file"


def walk_paths(paths):
    for raw in paths:
        absolute = os.path.abspath(raw)
        # Resolve the directory prefix, never the entry: the entry itself may
        # be a symlink whose target is exactly what the symlink rule checks.
        # Without this, an absolute argument spelled through a symlinked
        # directory would not compare against the canonical repository root.
        parent = os.path.realpath(os.path.dirname(absolute))
        absolute = os.path.join(parent, os.path.basename(absolute))
        if not os.path.lexists(absolute):
            raise CheckError(f"no such path: {raw}")
        yield from _walk(absolute)


def repo_path(root, absolute):
    """The repository-relative POSIX path, or the absolute one if outside."""
    relative = os.path.relpath(absolute, root)
    if relative == os.pardir or relative.startswith(os.pardir + os.sep):
        return absolute.replace(os.sep, "/")
    return relative.replace(os.sep, "/")


def path_findings(root, paths, max_bytes):
    seen = set()
    findings = []
    for absolute, kind in walk_paths(paths):
        path = repo_path(root, absolute)
        if path in seen:
            continue
        seen.add(path)
        findings.extend(name_findings(path))
        if kind == "link":
            try:
                target = os.readlink(absolute)
            except OSError as exc:
                raise CheckError(
                    f"cannot read symlink {absolute}: {exc.strerror or exc}")
            detail = symlink_detail(path, target)
            if detail is not None:
                findings.append(Finding(path, "symlink", detail))
            continue
        try:
            size = os.path.getsize(absolute)
            with open(absolute, "rb") as handle:
                head = handle.read(HEAD_BYTES)
        except OSError as exc:
            raise CheckError(f"cannot read {absolute}: {exc.strerror or exc}")
        findings.extend(content_findings(path, size, head, max_bytes))
    return len(seen), findings


def parse_allow(path):
    try:
        with open(path, encoding="utf-8", errors="surrogateescape") as handle:
            lines = handle.read().splitlines()
    except OSError as exc:
        raise CheckError(f"cannot read allow file {path}: {exc.strerror or exc}")
    entries = []
    for number, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = ALLOW_ENTRY.match(line)
        if match is None:
            raise CheckError(
                f"{path}:{number}: malformed allow entry, expected "
                f"'<glob> <rule-id> -- <reason>': {line}")
        rule = match.group("rule")
        if rule not in RULE_IDS:
            raise CheckError(f"{path}:{number}: unknown rule id '{rule}'")
        entries.append(AllowEntry(match.group("glob"), rule))
    return entries


def load_allow(root, explicit):
    if explicit:
        if not os.path.isfile(explicit):
            raise CheckError(f"cannot read allow file {explicit}")
        return parse_allow(explicit)
    default = os.path.join(root, "tools", "hygiene-allow.txt")
    if os.path.isfile(default):
        return parse_allow(default)
    return []


def apply_allow(findings, allow):
    kept = []
    suppressed = 0
    for finding in findings:
        if any(entry.rule == finding.rule
               and fnmatch.fnmatchcase(finding.path, entry.pattern)
               for entry in allow):
            suppressed += 1
        else:
            kept.append(finding)
    return kept, suppressed


def byte_count(text):
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not an integer: {text}")
    if value < 0:
        raise argparse.ArgumentTypeError(f"must be non-negative: {text}")
    return value


def run(args):
    # Canonicalise the root: a directory reached through a symlink (macOS
    # /var -> /private/var) must still compare equal to the paths found by
    # walking it, or every file looks like it lives outside the repository.
    base = os.path.realpath(os.path.abspath(args.repo or os.getcwd()))
    if not os.path.isdir(base):
        raise CheckError(f"--repo is not a directory: {base}")
    if args.paths is None:
        # git reports paths relative to the worktree root, so check there.
        root = git_toplevel(base)
    else:
        root = base
    allow = load_allow(root, args.allow)
    if args.paths is None:
        checked, findings = git_findings(root, args.staged, args.max_bytes)
    else:
        checked, findings = path_findings(root, args.paths, args.max_bytes)
    findings = sorted(set(findings), key=lambda item: (
        item.path, item.rule, item.detail))
    kept, suppressed = apply_allow(findings, allow)
    if args.json:
        json.dump([{"path": item.path, "rule": item.rule,
                    "detail": item.detail} for item in kept],
                  sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        for item in kept:
            print(f"{item.path}: {item.rule}: {item.detail}")
        print(f"checked {checked} path(s): {len(kept)} finding(s), "
              f"{suppressed} allowed")
    return 1 if kept else 0


def main(argv=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(
        description="Refuse game and firmware material in a repository.")
    parser.add_argument(
        "--repo", metavar="DIR",
        help="repository root (default: the current directory)")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--staged", action="store_true",
        help="entries added, copied, modified or renamed in the index")
    mode.add_argument(
        "--tracked", action="store_true", help="every entry in the index")
    mode.add_argument(
        "--paths", nargs="+", metavar="P",
        help="filesystem files or directories; no git needed")
    parser.add_argument(
        "--allow", metavar="FILE",
        help="allow list (default: <repo>/tools/hygiene-allow.txt if present)")
    parser.add_argument(
        "--max-bytes", type=byte_count, default=DEFAULT_MAX_BYTES, metavar="N",
        help=f"largest allowed blob (default: {DEFAULT_MAX_BYTES})")
    parser.add_argument(
        "--json", action="store_true", help="print a JSON array of findings")
    args = parser.parse_args(argv)
    try:
        return run(args)
    except CheckError as exc:
        print(f"check_hygiene: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
