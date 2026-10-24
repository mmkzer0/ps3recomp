#!/usr/bin/env python3
"""A guest exit must not tear the host down under the threads still running.

Runs tests/test_quick_exit.c + tests/test_quick_exit_static.cpp: a C++ object
with static storage, a host thread still writing a global, a quick-exit
diagnostic on stdout, then sys_process_exit(7).

What each assertion proves:

  exit status 7           the guest's status arrived through quick_exit.
  diagnostic present      stdout is a pipe (fully buffered), so the line only
                          arrives because the handler flushed its own output;
                          quick_exit flushes nothing.
  destructor absent       exit() would run the static destructor; the guest
                          exit must not.

Before the host exit path used quick_exit(), the same fixture printed the
destructor marker and the diagnostic was lost.
"""
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests" / "gates"))
import gates_common as g  # noqa: E402

DIAGNOSTIC = "[quick-exit] at_quick_exit diagnostic ran"
DESTRUCTOR = "[quick-exit] static destructor ran"


def main(argv):
    if len(argv) != 2:
        print("usage: run_quick_exit.py <fixture>", file=sys.stderr)
        return 2

    env = g.clean_env()
    # Diagnostic detours in sys_process_exit must not hold the fixture open.
    for var in ("YDKJ_NOEXIT", "CELLMARK_HOLD"):
        env.pop(var, None)

    proc = g.run([argv[1]], env=env, timeout=120)

    problems = []
    if proc.returncode != 7:
        problems.append(f"exit status {proc.returncode}, wanted 7 (the guest's status)")
    if DIAGNOSTIC not in proc.stdout:
        problems.append("the at_quick_exit diagnostic never reached the pipe: "
                        "the handler did not flush its own output")
    if DESTRUCTOR in proc.stdout or DESTRUCTOR in proc.stderr:
        problems.append("the C++ static destructor ran: the host exited with exit()")

    if problems:
        for problem in problems:
            print(f"FAIL: {problem}", file=sys.stderr)
        print(f"--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}", file=sys.stderr)
        return 1

    print("PASS: guest exit 7, diagnostic flushed through a pipe, "
          "static destructor never ran")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
