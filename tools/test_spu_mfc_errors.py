#!/usr/bin/env python3
"""The MFC rejects malformed transfers; each error class is a named stop.

Builds runtime/spu/tests/test_mfc_errors_main.c against the real DMA engine
(spu_dma.h), the real typed configuration (runtime/config/config.cpp) and the
real named-stop runtime (runtime/diag/stop.cpp), then runs one process per
case. Every case that used to return -1 -- a value the MFC channel write
discarded -- must now end in exit 70 and one

    PS3STOP v=1 kind=spu-mfc-error cmd=... lsa=... ea=... size=... tag=...
            reason=... pc=... tid=...

line naming the staged command, the SPU's pc and its id.

Two states are checked as no-ops, because they are not errors: a zero-byte
transfer (RPCS3's process_mfc_cmd executes a command only when size != 0) and
an MFC_WrListStallAck for a tag with no parked list (RPCS3 acts only when the
tag's ch_stall_mask bit is set).
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests" / "gates"))
import gates_common as g  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == "nt":
        print("SKIP: fixture currently requires the POSIX compiler/linker")
        return 0
    spu = ROOT / "runtime" / "spu"
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    cflags = ["-std=gnu17", "-O1", "-UNDEBUG",
              "-I" + str(ROOT / "include"), "-I" + str(spu),
              "-I" + str(ROOT / "runtime/platform")]
    cxxflags = ["-std=c++20", "-O1", "-UNDEBUG", "-I" + str(ROOT / "include")]
    # The ambient environment must not arm a knob the test did not choose.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("SPU_", "PS3_", "PS3RECOMP_", "YDKJ_", "SPURS_"))}

    with tempfile.TemporaryDirectory(prefix="spu-mfc-errors-") as tmp:
        tmp = Path(tmp)
        objects = []
        for src, flags, out in (
                (spu / "tests/test_mfc_errors_main.c", cflags, "fixture.o"),
                (spu / "spu_vm_pagemap.c", cflags, "pagemap.o"),
                (ROOT / "runtime/platform/win32_compat.c", cflags, "compat.o")):
            obj = tmp / out
            subprocess.run([cc, *flags, "-c", str(src), "-o", str(obj)], check=True)
            objects.append(str(obj))
        for src, out in ((ROOT / "runtime/config/config.cpp", "config.o"),
                         (ROOT / "runtime/diag/stop.cpp", "stop.o")):
            obj = tmp / out
            subprocess.run([cxx, *cxxflags, "-c", str(src), "-o", str(obj)], check=True)
            objects.append(str(obj))
        binary = tmp / "test_mfc_errors"
        subprocess.run([cxx, *objects, "-pthread", "-lm", "-o", str(binary)], check=True)

        for case in ("zero", "ack-unparked"):
            p = g.run([str(binary), case], env=env, timeout=60)
            assert p.returncode == 0, (case, p.returncode, p.stdout, p.stderr)
            assert "PASS:" in p.stdout, (case, p.stdout, p.stderr)

        # case, reason, cmd, lsa, ea, size. All run with tag 7, pc 0x1234 and
        # SPU id 2 (begin() in the fixture).
        cases = [
            ("size",         "size",          "0x40", "0x01000", "0x2000",     "32768"),
            ("alignment",    "alignment",     "0x40", "0x01000", "0x2000",     "24"),
            ("alignment-addr", "alignment",   "0x40", "0x01000", "0x2001",     "8"),
            ("ls-overrun",   "ls-overrun",    "0x40", "0x3FFF8", "0x2008",     "16"),
            ("ea-range",     "ea-uncommitted", "0x40", "0x01008", "0xFFFFFFF8", "16"),
            ("null-ea",      "null-ea",       "0x20", "0x01000", "0x100",      "16"),
            ("list-submit",  "size",          "0x40", "0x01000", "0x2000",     "32752"),
            ("list-resume",  "size",          "0x40", "0x01000", "0x2010",     "32752"),
        ]
        for case, reason, cmd, lsa, ea, size in cases:
            p = g.run([str(binary), case], env=env, timeout=60)
            f = g.expect_stop(p, "spu-mfc-error")
            assert f["reason"] == reason, (case, f)
            assert (f["cmd"], f["lsa"], f["ea"], f["size"]) == (cmd, lsa, ea, size), (case, f)
            assert f["tag"] == "7", (case, f)
            assert f["pc"] == "0x1234", (case, f)
            assert f["tid"] == "2", (case, f)
    print("PASS: every malformed MFC transfer class stops with kind "
          "spu-mfc-error, and zero-size/unparked-ack remain no-ops")
    return 0


if __name__ == "__main__":
    sys.exit(main())
