#!/usr/bin/env python3
"""ps3diff: convert, validate and compare PS3 execution traces.

The implementation lives in the `ps3diff` package next to this file; this
entry point only keeps the command line stable (`python3 tools/ps3diff.py`).
See docs/PS3DIFF.md for the schema and every subcommand's examples.
"""

import sys

from ps3diff.cli import main

if __name__ == "__main__":
    sys.exit(main())
