#!/usr/bin/env python3
"""Print a visible ctest SKIP line and exit 77.

    skip.py <module> <reason>

Used when a firmware module is configured but its lifted metadata is missing:
the suite must say so, never pass silently."""
import sys


def main(argv):
    if len(argv) != 3:
        print("usage: skip.py <module> <reason>", file=sys.stderr)
        return 2
    print(f"SKIP {argv[1]}: {argv[2]}")
    return 77


if __name__ == "__main__":
    sys.exit(main(sys.argv))
