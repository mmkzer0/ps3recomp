#!/usr/bin/env python3
"""SPU lift ABI mismatch, context-layout pin, and cross-data-model pin gates."""
import os
import re
import shutil
import subprocess
import struct
import sys
import tempfile
from pathlib import Path

import gates_common as g

MATCH = None
MISMATCH = None
ROOT = None
EXPECTED_ABI = 1
LP64_LAYOUT_SIGNATURE = 0x8B375EA16245430D
WIN64_LAYOUT_SIGNATURE = 0x918F8F23859E16E5


def runtime_abi():
    text = (Path(ROOT) / "include" / "ps3emu" / "lifter_abi.h").read_text()
    match = re.search(r"^#define SPU_RUNTIME_LIFTER_ABI (\d+)u$", text, re.M)
    assert match, "SPU_RUNTIME_LIFTER_ABI not defined in ps3emu/lifter_abi.h"
    return int(match.group(1))


def header_pins():
    """The two explicit data-model pins; the header selects one by OS/ABI."""
    text = (Path(ROOT) / "include" / "ps3emu" / "lifter_abi.h").read_text()
    pins = {}
    for model, macro in (
            ("lp64", "SPU_RUNTIME_LIFTER_LP64_CONTEXT_LAYOUT_SIGNATURE_PIN"),
            ("win64", "SPU_RUNTIME_LIFTER_WIN64_CONTEXT_LAYOUT_SIGNATURE_PIN")):
        match = re.search(
            rf"^#define {macro} UINT64_C\(0x([0-9A-Fa-f]+)\)$", text, re.M)
        assert match, f"{macro} missing from ps3emu/lifter_abi.h"
        pins[model] = int(match.group(1), 16)
    return pins


def data_model():
    """The pin is per data model, not per OS: LP64 vs Windows LLP64."""
    pointer = struct.calcsize("P")
    long_size = struct.calcsize("l")
    if pointer == 8 and long_size == 8:
        return "lp64"
    if sys.platform == "win32" and pointer == 8 and long_size == 4:
        return "win64"
    raise AssertionError(
        f"no verified SPU context layout pin for pointer={pointer} long={long_size} "
        f"on {sys.platform}")


def test_matching_abi_runs():
    p = g.run([MATCH])
    assert p.returncode == 0, f"exit {p.returncode}\nstderr:\n{p.stderr}"
    assert not g.stop_lines(p.stderr), p.stderr


def test_mismatched_abi_stops_during_registration():
    expected = runtime_abi()
    p = g.run([MISMATCH])
    fields = g.expect_stop(p, "lifter-abi")
    assert fields["processor"] == "spu", fields
    assert fields["expected"] == str(expected), fields
    assert fields["got"] != str(expected), fields


def test_context_layout_is_pinned_to_the_lifter_abi():
    abi = runtime_abi()
    assert abi == EXPECTED_ABI, (
        f"SPU runtime ABI changed from {EXPECTED_ABI} to {abi}; update the ABI and "
        "context-layout pins together")
    model = data_model()
    expected = {"lp64": LP64_LAYOUT_SIGNATURE, "win64": WIN64_LAYOUT_SIGNATURE}[model]
    pins = header_pins()
    assert pins[model] == expected, (
        f"update the {model} SPU layout pin: expected 0x{expected:016X}, "
        f"found 0x{pins[model]:016X}")
    # Compile only: `probe_spu_lifter_layout_pin.c` holds the same comparison as
    # a static assert, so a layout change without an ABI/pin update fails here
    # with the assert's message. Linking the runtime for a printed value made the
    # probe drag in every runtime object (vm_base, g_spu_ls_watch_n, ...), which
    # is not part of the contract being checked and failed GNU ld.
    cc = os.environ.get("CC") or shutil.which("cc")
    assert cc, "no C compiler (CC unset and no cc on PATH)"
    with tempfile.TemporaryDirectory() as tmp:
        obj = Path(tmp) / "probe_spu_lifter_layout_pin.o"
        p = subprocess.run(
            [cc, "-std=gnu17", "-c", "-o", str(obj),
             "-I", str(Path(ROOT) / "include"),
             "-I", str(Path(ROOT) / "runtime" / "spu"),
             str(Path(ROOT) / "tests" / "gates" / "probe_spu_lifter_layout_pin.c")],
            capture_output=True, text=True)
        assert p.returncode == 0, (
            f"the {model} context layout no longer matches its pin; bump "
            f"SPU_RUNTIME_LIFTER_ABI and update the {model} layout pin together:\n"
            f"{p.stdout}{p.stderr}")


def test_windows_x64_pin_matches_cross_compiled_layout():
    # Compile-only: mingw defines _WIN32/_WIN64, so the probe selects the Win64
    # pin and its static assert checks the layout Windows actually builds. Full
    # Windows runtime execution remains the Windows t0 job's coverage.
    cc = shutil.which("x86_64-w64-mingw32-gcc")
    if cc is None:
        print("SKIP test_windows_x64_pin_matches_cross_compiled_layout: "
              "x86_64-w64-mingw32-gcc not on PATH; the Windows t0 job compiles "
              "and runs the same probe natively")
        return
    source = Path(ROOT) / "tests" / "gates" / "probe_spu_lifter_layout_pin.c"
    with tempfile.TemporaryDirectory(prefix="spu-win-pin-") as tmp:
        p = g.run([cc, "-std=gnu17", "-c",
                   "-I", str(Path(ROOT) / "include"),
                   "-I", str(Path(ROOT) / "runtime" / "spu"),
                   str(source), "-o", str(Path(tmp) / "pin.o")])
    assert p.returncode == 0, (
        "the Win64 data model does not match "
        f"SPU_RUNTIME_LIFTER_WIN64_CONTEXT_LAYOUT_SIGNATURE_PIN "
        f"0x{WIN64_LAYOUT_SIGNATURE:016X}:\n{p.stdout}{p.stderr}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("usage: test_spu_lifter_abi.py <match> <mismatch> <source-root>",
              file=sys.stderr)
        sys.exit(2)
    MATCH, MISMATCH, ROOT = sys.argv[1:]
    sys.exit(g.main([(name, fn) for name, fn in sorted(globals().items())
                     if name.startswith("test_")]))
