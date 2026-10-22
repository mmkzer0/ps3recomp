#!/usr/bin/env python3
"""Contract tests for tools/check_hygiene.py.

Every case builds a throwaway repository.  The fixtures are synthetic bytes
that carry a PS3 signature and nothing else: the gate is testable without a
single byte of game or firmware material, which is the point.
"""

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().with_name("check_hygiene.py")
FINDING = re.compile(r"^(?P<path>.+?): (?P<rule>[a-z-]+): (?P<detail>.*)$")


def elf_header(machine, data):
    """A 64-byte ELF header: EI_DATA 2 is big-endian, 1 little-endian."""
    header = bytearray(64)
    header[0:4] = b"\x7fELF"
    header[4] = 2  # EI_CLASS: 64-bit
    header[5] = data
    header[6] = 1
    header[18:20] = machine.to_bytes(2, "big" if data == 2 else "little")
    return bytes(header)


PS3_ELF = elf_header(0x15, 2)  # EM_PPC64
PS3_SPU_ELF = elf_header(0x17, 2)  # EM_SPU
BE_PPC32_ELF = elf_header(0x14, 2)
LE_X86_64_ELF = elf_header(0x3E, 1)
LE_PPC64_ELF = elf_header(0x15, 1)

MAGIC_FIXTURES = (
    ("elf-ppc64.bin", PS3_ELF, "ps3-elf"),
    ("elf-spu.bin", PS3_SPU_ELF, "ps3-elf"),
    ("sce.bin", b"SCE\x00" + b"\x00" * 60, "sce-header"),
    ("pkg.bin", b"\x7fPKG" + b"\x00" * 60, "pkg"),
    ("psf.bin", b"\x00PSF" + b"\x00" * 60, "psf/sfo"),
    ("cpk.bin", b"CPK " + b"\x00" * 60, "cri-cpk"),
    ("pup.bin", b"SCEUF\x00\x00\x00" + b"\x00" * 56, "pup"),
    ("npd.bin", b"NPD\x00" + b"\x00" * 60, "npd (edat/sdat)"),
)

NAME_FIXTURES = (
    "game.elf", "Disc.SELF", "module.sprx", "module.prx", "title.pkg",
    "PARAM.SFO", "update.pup", "save.rap", "save.edat", "save.sdat",
    "data.cpk", "trophy.p3t", "keys.keys", "EBOOT.BIN", "PARAM.PFD",
    "act.dat", "idps", "license.rif", "nested/Disc.PrX",
)

CLEAN_FIXTURES = ("README.md", "src/main.c", "src/table.bin", "notes.txt")


class RepoTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="check-hygiene-")
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name) / "repo"
        self.root.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Hygiene Test")
        self.git("config", "user.email", "hygiene@example.invalid")
        self.git("config", "commit.gpgsign", "false")

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.root), *args],
                              check=True, capture_output=True)

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, bytes):
            path.write_bytes(data)
        else:
            path.write_text(data)
        return path

    def symlink(self, target, name):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.symlink(target, path)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"symlinks are unavailable: {exc}")
        return path

    def add(self, *names):
        self.git("add", "--", *names)

    def commit(self, message="fixture"):
        self.git("commit", "-q", "-m", message)

    def run_tool(self, *args, repo=None, cwd=None):
        command = [sys.executable, str(TOOL),
                   "--repo", str(repo or self.root), *args]
        return subprocess.run(command, cwd=str(cwd or self.root),
                              capture_output=True, text=True)

    def findings(self, result):
        findings = []
        for line in result.stdout.splitlines():
            match = FINDING.match(line)
            if match is not None:
                findings.append((match.group("path"), match.group("rule"),
                                 match.group("detail")))
        return findings

    def assert_exit(self, result, expected):
        self.assertEqual(
            result.returncode, expected,
            f"stdout={result.stdout!r} stderr={result.stderr!r}")


class HygieneRules(RepoTestCase):
    def test_clean_repository_passes(self):
        for name in CLEAN_FIXTURES:
            self.write(name, b"no binary material here\n")
        self.add(".")
        self.commit()
        for mode in ("--tracked", "--staged"):
            result = self.run_tool(mode)
            self.assert_exit(result, 0)
            self.assertIn("0 finding(s)", result.stdout)

    def test_forbidden_names_fire_in_staged_and_tracked(self):
        for name in NAME_FIXTURES + CLEAN_FIXTURES:
            self.write(name, b"plain text\n")
        self.add(".")
        for mode in ("--tracked", "--staged"):
            result = self.run_tool(mode)
            self.assert_exit(result, 1)
            flagged = {path for path, rule, _ in self.findings(result)
                       if rule == "forbidden-name"}
            self.assertEqual(flagged, set(NAME_FIXTURES))

    def test_magic_signatures_fire_in_staged_and_tracked(self):
        for name, data, _label in MAGIC_FIXTURES:
            self.write(name, data)
        self.add(".")
        expected = {name: label for name, _data, label in MAGIC_FIXTURES}
        for mode in ("--tracked", "--staged"):
            result = self.run_tool(mode)
            self.assert_exit(result, 1)
            got = {path: detail for path, rule, detail in self.findings(result)
                   if rule == "magic"}
            self.assertEqual(got, expected)

    def test_non_ps3_elfs_are_not_flagged(self):
        self.write("host.bin", LE_X86_64_ELF)
        self.write("ppc32.bin", BE_PPC32_ELF)
        self.write("little-ppc64.bin", LE_PPC64_ELF)
        self.add(".")
        for mode in ("--tracked", "--staged"):
            result = self.run_tool(mode)
            self.assert_exit(result, 0)

    def test_oversize_boundary(self):
        self.write("exact.bin", b"\x00" * 32)
        self.write("over.bin", b"\x00" * 33)
        self.add(".")
        for mode in ("--tracked", "--staged"):
            result = self.run_tool(mode, "--max-bytes", "32")
            self.assert_exit(result, 1)
            oversize = [path for path, rule, _ in self.findings(result)
                        if rule == "oversize"]
            self.assertEqual(oversize, ["over.bin"])
            self.assertIn("33 bytes exceeds 32", result.stdout)

    def test_symlinks_fire_in_staged_and_tracked(self):
        self.write("inside.txt", b"ok\n")
        self.symlink("inside.txt", "link-inside")
        self.symlink("../inside.txt", "sub/up")
        self.symlink("../outside.txt", "link-outside")
        self.symlink("../../../outside", "sub/deep/escape")
        self.symlink("/etc/passwd", "link-absolute")
        self.add(".")
        for mode in ("--tracked", "--staged"):
            result = self.run_tool(mode)
            self.assert_exit(result, 1)
            links = {path: detail for path, rule, detail in self.findings(result)
                     if rule == "symlink"}
            self.assertNotIn("link-inside", links)
            self.assertNotIn("sub/up", links)
            self.assertIn("escapes the repository", links["link-outside"])
            self.assertIn("escapes the repository", links["sub/deep/escape"])
            self.assertEqual(links["link-absolute"],
                             "absolute target /etc/passwd")


class HygieneIndexContent(RepoTestCase):
    def test_staged_reads_the_index_not_the_working_tree(self):
        self.write("payload.bin", PS3_ELF)
        self.add("payload.bin")
        self.write("payload.bin", b"clean\n")

        result = self.run_tool("--staged")
        self.assert_exit(result, 1)
        self.assertIn("ps3-elf", result.stdout)

        result = self.run_tool("--tracked")
        self.assert_exit(result, 1)

        # The reverse: clean staged bytes behind a dirty working file.
        self.add("payload.bin")
        self.write("payload.bin", PS3_ELF)
        result = self.run_tool("--staged")
        self.assert_exit(result, 0)


class HygienePaths(RepoTestCase):
    def test_paths_mode_does_not_need_git(self):
        plain = Path(self._tmp.name) / "plain"
        (plain / "assets").mkdir(parents=True)
        (plain / "assets" / "game.pkg").write_bytes(b"text\n")
        (plain / "ok.txt").write_text("ok\n")
        result = self.run_tool("--paths", "assets", repo=plain, cwd=plain)
        self.assert_exit(result, 1)
        self.assertEqual(
            [(path, rule) for path, rule, _ in self.findings(result)],
            [("assets/game.pkg", "forbidden-name")])

    def test_paths_mode_checks_real_symlinks(self):
        self.write("tree/ok.txt", b"ok\n")
        self.write("outside.txt", b"x\n")
        self.symlink("ok.txt", "tree/inside")
        self.symlink("../../outside.txt", "tree/escape")
        result = self.run_tool("--paths", "tree")
        self.assert_exit(result, 1)
        links = {path: detail for path, rule, detail in self.findings(result)
                 if rule == "symlink"}
        self.assertNotIn("tree/inside", links)
        self.assertIn("tree/escape", links)

    def test_missing_path_exits_2(self):
        result = self.run_tool("--paths", "does-not-exist")
        self.assert_exit(result, 2)
        self.assertIn("no such path", result.stderr)


class HygieneAllow(RepoTestCase):
    def test_allow_glob_suppresses_only_its_rule(self):
        pkg = b"\x7fPKG" + b"\x00" * 16
        self.write("game.pkg", pkg)
        self.write("assets/audio.pkg", pkg)
        self.add(".")

        result = self.run_tool("--tracked")
        self.assert_exit(result, 1)
        self.assertEqual(len(self.findings(result)), 4)  # name + magic, twice

        # The default allow file lives at <repo>/tools/hygiene-allow.txt.
        self.write("tools/hygiene-allow.txt",
                   "# fixture\n"
                   "*.pkg forbidden-name -- synthetic packaging test\n")
        result = self.run_tool("--tracked")
        self.assert_exit(result, 1)
        self.assertEqual(
            {(path, rule) for path, rule, _ in self.findings(result)},
            {("game.pkg", "magic"), ("assets/audio.pkg", "magic")})
        self.assertIn("2 allowed", result.stdout)

        self.write("tools/hygiene-allow.txt",
                   "*.pkg forbidden-name -- synthetic packaging test\n"
                   "game.pkg magic -- synthetic root container\n"
                   "assets/audio.pkg magic -- synthetic nested container\n")
        result = self.run_tool("--tracked")
        self.assert_exit(result, 0)

    def test_explicit_allow_replaces_the_default(self):
        self.write("game.pkg", b"text\n")
        self.add(".")
        self.write("tools/hygiene-allow.txt",
                   "*.pkg forbidden-name -- default allow file\n")
        self.assert_exit(self.run_tool("--tracked"), 0)

        self.write("alt.txt", "*.bin magic -- unrelated rule\n")
        result = self.run_tool("--tracked", "--allow", "alt.txt")
        self.assert_exit(result, 1)

    def test_allow_entry_without_reason_exits_2(self):
        for content in ("game.pkg forbidden-name\n",
                        "game.pkg forbidden-name --   \n"):
            self.write("allow.txt", "# header\n" + content)
            result = self.run_tool("--tracked", "--allow", "allow.txt")
            self.assert_exit(result, 2)
            self.assertIn("allow.txt:2", result.stderr)

    def test_allow_entry_with_unknown_rule_exits_2(self):
        self.write("allow.txt", "\n# header\ngame.pkg not-a-rule -- reason\n")
        result = self.run_tool("--tracked", "--allow", "allow.txt")
        self.assert_exit(result, 2)
        self.assertIn("allow.txt:3", result.stderr)
        self.assertIn("unknown rule", result.stderr)

    def test_unreadable_allow_file_exits_2(self):
        result = self.run_tool("--tracked", "--allow", "missing-allow.txt")
        self.assert_exit(result, 2)
        self.assertIn("missing-allow.txt", result.stderr)


class HygieneOutput(RepoTestCase):
    def test_json_output_is_an_array_of_findings(self):
        result = self.run_tool("--tracked", "--json")
        self.assert_exit(result, 0)
        self.assertEqual(json.loads(result.stdout), [])

        self.write("game.pkg", b"text\n")
        self.add("game.pkg")
        result = self.run_tool("--tracked", "--json")
        self.assert_exit(result, 1)
        data = json.loads(result.stdout)
        self.assertEqual(data, [{"path": "game.pkg", "rule": "forbidden-name",
                                 "detail": "forbidden extension .pkg"}])

    def test_tracked_outside_a_repository_exits_2(self):
        plain = Path(self._tmp.name) / "plain"
        plain.mkdir()
        result = self.run_tool("--tracked", repo=plain, cwd=plain)
        self.assert_exit(result, 2)
        self.assertIn("not a git repository", result.stderr)

    def test_mode_is_required(self):
        result = subprocess.run(
            [sys.executable, str(TOOL), "--repo", str(self.root)],
            cwd=str(self.root), capture_output=True, text=True)
        self.assert_exit(result, 2)


if __name__ == "__main__":
    unittest.main()
