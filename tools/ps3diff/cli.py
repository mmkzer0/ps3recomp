"""Command line: convert, validate and diff, with examples in every --help."""

from __future__ import annotations

import argparse
import json
import sys

from .convert import convert_file
from .diff import diff_files, report_text
from .rsx import SCHEMA_RSX, compare_files, report_text as rsx_report_text, validate_rsx
from .schema import (
    Ps3DiffError,
    context_keys,
    read_ndjson,
    validate_entries,
)

_EXAMPLES_CONVERT = """\
examples:
  ps3diff convert --to 2 trace.ndjson -o trace.v2.ndjson
  ps3diff convert --to 1 trace.v2.ndjson -o trace.v1.ndjson
  ps3diff convert --to 2 trace.ndjson          # stdout
"""
_EXAMPLES_VALIDATE = """\
examples:
  ps3diff validate trace.v2.ndjson
  ps3diff validate trace.ndjson                # event/1 is checked too
  ps3diff validate captures/h1/capture.ndjson  # ps3diff.rsx/1, blobs checked
"""
_EXAMPLES_DIFF = """\
examples:
  ps3diff diff ref.ndjson new.ndjson
  ps3diff diff ref.v2.ndjson new.v2.ndjson --json
  ps3diff diff ref.v2.ndjson new.v2.ndjson --mask odin.mask.json
  ps3diff diff ref.v2.ndjson new.v2.ndjson --ctx-map threads.json
"""
_EXAMPLES_RSX_COMPARE = """\
examples:
  ps3diff rsx-compare capture.ndjson replay/capture.ndjson
  ps3diff rsx-compare capture.ndjson replay/capture.ndjson --flip 99 --json

exit status: 0 equal, 1 a frame differs or is invalid, 2 data or schema error,
3 nothing was compared (an error), 4 the environment records differ (not comparable).
"""


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3diff",
        description="Convert, validate and compare ps3diff NDJSON traces.",
        epilog="The schema is ps3diff.event/2, the successor of "
               "rpcs3-gdb-trace.event/1. See docs/PS3DIFF.md.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True,
                                metavar="{convert,validate,diff,rsx-compare}")

    convert = sub.add_parser(
        "convert", help="convert between event/1 and event/2",
        description="Convert a trace losslessly. A v1 -> v2 -> v1 round trip "
                    "reproduces every canonical input line byte for byte; a "
                    "v2 record without a v1 meaning refuses --to 1.",
        epilog=_EXAMPLES_CONVERT, formatter_class=argparse.RawDescriptionHelpFormatter)
    convert.add_argument("--to", type=int, choices=(1, 2), required=True,
                         help="target schema version")
    convert.add_argument("input", metavar="IN.ndjson",
                         help="input trace ('-' for stdin)")
    convert.add_argument("-o", "--output", default="-", metavar="OUT.ndjson",
                         help="output trace, written atomically (default: stdout)")
    convert.set_defaults(func=_cmd_convert)

    validate = sub.add_parser(
        "validate", help="schema-check a trace",
        description="Check the record schema, per-context ctx_seq contiguity, "
                    "strictly increasing seq, and that every call_seq names an "
                    "earlier record of the right kind in the same context.",
        epilog=_EXAMPLES_VALIDATE, formatter_class=argparse.RawDescriptionHelpFormatter)
    validate.add_argument("file", metavar="FILE", help="trace to check")
    validate.set_defaults(func=_cmd_validate)

    diff = sub.add_parser(
        "diff", help="report the first divergence per context",
        description="Align records per context by ctx_seq and report the first "
                    "divergence of each context and the globally first one. "
                    "Timing, sequence identity and address fields that move "
                    "between runs are not compared.",
        epilog=_EXAMPLES_DIFF, formatter_class=argparse.RawDescriptionHelpFormatter)
    diff.add_argument("a", metavar="A", help="reference trace")
    diff.add_argument("b", metavar="B", help="candidate trace")
    diff.add_argument("--mask", metavar="MASK.json",
                      help="fields to ignore or address-normalise")
    diff.add_argument("--ctx-map", metavar="MAP.json",
                      help="map A context keys to B context keys")
    diff.add_argument("--json", action="store_true",
                      help="print the report as JSON")
    diff.set_defaults(func=_cmd_diff)

    rsx = sub.add_parser(
        "rsx-compare", help="compare two ps3diff.rsx/1 captures frame by frame",
        description="Validate both captures, then compare the frames they share by flip_id: identity, "
                    "capture hash (inputs), state hash, readback descriptor and frame hash (pixels), "
                    "in that order. Differing environment records are 'not comparable', never a pass "
                    "or a failure; zero compared frames is an error.",
        epilog=_EXAMPLES_RSX_COMPARE, formatter_class=argparse.RawDescriptionHelpFormatter)
    rsx.add_argument("a", metavar="A", help="reference capture (capture.ndjson)")
    rsx.add_argument("b", metavar="B", help="candidate capture (capture.ndjson)")
    rsx.add_argument("--flip", type=int, metavar="N", help="compare only the frame with this flip_id")
    rsx.add_argument("--json", action="store_true", help="print the report as JSON")
    rsx.set_defaults(func=_cmd_rsx_compare)
    return parser


def _cmd_convert(args: argparse.Namespace) -> int:
    convert_file(args.input, args.output, args.to)
    return 0


def _cmd_validate(args: argparse.Namespace) -> int:
    entries = read_ndjson(args.file)
    if entries and entries[0][2].get("schema") == SCHEMA_RSX:
        cap = validate_rsx(args.file, entries)
        print(f"{args.file}: valid: {len(entries)} records, {len(cap.frames)} frames, "
              f"{sum(1 for f in cap.frames if f['valid'])} valid ({SCHEMA_RSX})")
        return 0
    schema = validate_entries(args.file, entries)
    contexts = context_keys(schema, [rec for _, _, rec in entries])
    state = schema if schema is not None else "empty"
    print(f"{args.file}: valid: {len(entries)} records, "
          f"{len(contexts)} contexts ({state})")
    return 0


def _cmd_rsx_compare(args: argparse.Namespace) -> int:
    report, status = compare_files(args.a, args.b, args.flip)
    if args.json:
        print(json.dumps(report, indent=1, ensure_ascii=False))
    else:
        print(rsx_report_text(report))
    if status == 3:
        print("ps3diff: error: " + report["error"], file=sys.stderr)
    return status


def _cmd_diff(args: argparse.Namespace) -> int:
    report, status = diff_files(args.a, args.b, args.mask, args.ctx_map)
    if args.json:
        print(json.dumps(report, indent=1, ensure_ascii=False))
    else:
        print(report_text(report))
    if status == 3:
        print("ps3diff: error: zero records compared; a comparison that proves "
              "nothing is not a pass", file=sys.stderr)
    return status


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        return args.func(args)
    except Ps3DiffError as err:
        print(f"ps3diff: error: {err.message}", file=sys.stderr)
        if err.hint:
            print(f"  hint: {err.hint}", file=sys.stderr)
        return err.exit_code
    except BrokenPipeError:
        return 2
