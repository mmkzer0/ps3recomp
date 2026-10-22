#!/usr/bin/env python3
"""Normalise and check a git commit message (commit-msg hook).

    check_commit_msg.py [--fix] MESSAGE_FILE

Normalisation (written back with --fix, otherwise only checked against):
  - CRLF line endings become LF; trailing whitespace is removed from every
    line; leading blank lines and runs of blank lines collapse;
  - git's comment lines (the commented status block) are dropped, and so is
    everything below a verbose diff's scissors line;
  - exactly one blank line separates the subject from the body, and the file
    ends with one newline.

Checks (exit 1 with every problem named):
  - the subject is `area: what changes`, no longer than 72 characters, with
    no trailing period; `area` is the module, file stem or tool the change
    is about, as in this repository's history (`cellGcmSys: ...`,
    `spu_dma: ...`, `ci: ...`);
  - body lines are at most 72 characters, except lines that are a single
    unbreakable token such as a URL.

Merge, revert, fixup! and squash! messages that git generates are only
normalised.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SUBJECT_MAX = 72
BODY_MAX = 72
SCISSORS = "# ------------------------ >8 ------------------------"
# `area: summary`. The area may name several modules ("HLE_TOP / SPU_STATS")
# but holds no colon, and the summary starts right after one space.
SUBJECT = re.compile(r"^(?P<area>[^\s:][^:]*?): (?P<summary>\S.*)$")
GENERATED = re.compile(r"^(Merge |Revert \"|fixup! |squash! |amend! )")


def normalise(text: str) -> str:
    lines = []
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if line == SCISSORS:
            break
        if line.startswith("#"):
            continue
        lines.append(line.rstrip())

    collapsed: list[str] = []
    for line in lines:
        if line == "" and (not collapsed or collapsed[-1] == ""):
            continue
        collapsed.append(line)
    while collapsed and collapsed[-1] == "":
        collapsed.pop()
    if not collapsed:
        return ""

    subject, body = collapsed[0], collapsed[1:]
    if body and body[0] != "":
        body.insert(0, "")
    return "\n".join([subject, *body]) + "\n"


def problems(message: str) -> list[str]:
    if not message.strip():
        return ["the message is empty"]
    lines = message.rstrip("\n").split("\n")
    subject = lines[0]
    if GENERATED.match(subject):
        return []

    found = []
    if not SUBJECT.match(subject):
        found.append(f"subject is not `area: what changes`: {subject!r}")
    if len(subject) > SUBJECT_MAX:
        found.append(f"subject is {len(subject)} characters (max {SUBJECT_MAX})")
    if subject.endswith("."):
        found.append("subject ends with a period")
    for number, line in enumerate(lines[2:], start=3):
        if len(line) > BODY_MAX and " " in line.strip():
            found.append(f"line {number} is {len(line)} characters (max {BODY_MAX})")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("message_file", type=Path)
    parser.add_argument("--fix", action="store_true",
                        help="write the normalised message back before checking")
    args = parser.parse_args(argv)

    try:
        original = args.message_file.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        print(f"check_commit_msg: cannot read {args.message_file}: {error}", file=sys.stderr)
        return 2

    message = normalise(original)
    if args.fix and message != original:
        args.message_file.write_text(message, encoding="utf-8")

    found = problems(message)
    for problem in found:
        print(f"commit message: {problem}", file=sys.stderr)
    if found:
        print(f"commit message: kept in {args.message_file}; fix it and commit again "
              f"(git commit -e -F {args.message_file})", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
