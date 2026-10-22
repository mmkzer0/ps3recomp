"""Shared helpers for the startup-gate tests: run a probe, parse its PS3STOP line."""
import os
import subprocess
import sys

STOP_EXIT_CODE = 70


def clean_env(**extra):
    """The ambient environment minus every PS3RECOMP_* variable: those are
    configuration, and a developer's shell must not leak into a test."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("PS3RECOMP_")}
    env.update(extra)
    return env


def run(argv, env=None, timeout=60):
    return subprocess.run(argv, env=clean_env() if env is None else env,
                          capture_output=True, text=True, timeout=timeout)


def _unescape_quoted(s, i):
    """Parse a "..." value starting at s[i] == '"'. Returns (value, next index)."""
    assert s[i] == '"'
    i += 1
    out = []
    simple = {'n': '\n', 'r': '\r', 't': '\t', '"': '"', '\\': '\\'}
    while s[i] != '"':
        if s[i] == '\\':
            c = s[i + 1]
            if c == 'x':
                out.append(chr(int(s[i + 2:i + 4], 16)))
                i += 4
            else:
                out.append(simple[c])
                i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out), i + 1


def parse_stop_line(line):
    """Split a machine line into an ordered dict, per the quoting rule documented
    in include/ps3emu/stop.h. Raises on anything that does not parse."""
    assert line.startswith("PS3STOP "), line
    fields = {}
    s = line.rstrip("\n")
    i = len("PS3STOP ")
    while i < len(s):
        eq = s.index("=", i)
        key = s[i:eq]
        assert key and all(c.islower() or c.isdigit() or c == "_" for c in key), key
        assert key not in fields, f"duplicate key {key}"
        i = eq + 1
        if i < len(s) and s[i] == '"':
            val, i = _unescape_quoted(s, i)
        else:
            j = s.find(" ", i)
            j = len(s) if j < 0 else j
            val, i = s[i:j], j
        fields[key] = val
        if i < len(s):
            assert s[i] == " ", f"expected a space at {i} in {s!r}"
            i += 1
    return fields


def stop_lines(stderr):
    return [l for l in stderr.splitlines() if l.startswith("PS3STOP ")]


def expect_stop(proc, kind):
    """Assert `proc` ended in exactly one stop of `kind`; return its fields."""
    assert proc.returncode == STOP_EXIT_CODE, (
        f"exit {proc.returncode}, wanted {STOP_EXIT_CODE}\nstderr:\n{proc.stderr}")
    lines = stop_lines(proc.stderr)
    assert len(lines) == 1, f"{len(lines)} PS3STOP lines\nstderr:\n{proc.stderr}"
    assert proc.stderr.rstrip("\n").endswith(lines[0]), "PS3STOP is not the last line"
    f = parse_stop_line(lines[0])
    assert f["v"] == "1", f
    assert f["kind"] == kind, f"kind {f['kind']}, wanted {kind}\nstderr:\n{proc.stderr}"
    return f


def main(tests):
    """Run every `test_*` callable; print a line per test; exit non-zero on failure."""
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print(f"PASS {name}")
        except Exception as e:  # noqa: BLE001 - report every failure, then fail
            failed += 1
            print(f"FAIL {name}: {e}", file=sys.stderr)
    return 1 if failed else 0
