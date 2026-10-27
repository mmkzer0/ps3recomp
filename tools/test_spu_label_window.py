#!/usr/bin/env python3
"""An SPU MFC PUT/PUTL or an atomic line store into a registered guest store
window stops by name.

Builds runtime/spu/tests/test_spu_label_window_main.c against the real DMA
engine (spu_dma.h), the real SPU channel and atomic path (spu_channels.c), the
rest of the SPU runtime, the real window state (runtime/memory/label_window.c),
the typed configuration and the named-stop runtime, then runs one process per
case. A plain PUT/PUTL into the RSX label window is a store the runtime cannot
order safely (it may be the value an ACQUIRE waits for, so waiting for a walker
pass would deadlock the producer), and it ends in

    PS3STOP v=1 kind=spu-label-window func=mfc_do_transfer cmd=0x20
            ea=0x... size=16 tag=7 count=1 pc=0x1234 lr=0 tid=2

An atomic line store (PUTLLC 0xB4, PUTLLUC 0xB0, PUTQLLUC 0xB8) of a 128-byte
line that overlaps the window is the same unorderable store, and ends in the
same kind with func=spu_mfc_atomic, the atomic command, the line base as the EA
(the staged EAL has its low 7 bits cleared), size=128 and the staged MFC tag.

The atomic cases run the command in a forked child over guest memory shared
with the parent; the parent checks that the line did not move and re-exits 70,
so exactly one stop line reaches this driver.

Fail-before: before the change the SPU path had no window check at all, so
`overlap`, `overlap-edge` and `overlap-list` returned 0 with the bytes moved,
and `putllc`, `putlluc` and `putqlluc` returned 0 with the line committed.
`overlap-control` reproduces that state inside the same binary (no window
registered) and must still land; `outside-covered` runs the OUTSIDE case's PUT
with a window that covers it and must stop, so the outside case cannot pass by
never noticing an overlap; `putlluc-outside` is the atomic boundary case (the
line just past the window commits normally).
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

    # The PUT/PUTL check lives in spu_dma.h, the atomic one in spu_channels.c:
    # the fixture links the production runtime in both cases, not a stub.
    runtime = ["spu_channels.c", "spu_drain.c", "spu_lockstep.c", "spu_coherency.c",
               "spu_workload.c", "spurs_policy.c", "spurs_job.c", "spu_tsp_weak.c",
               "spu_vm_pagemap.c", "spurs_policy_blob_weak.c", "spu_interp.c"]

    with tempfile.TemporaryDirectory(prefix="spu-label-window-") as tmp:
        tmp = Path(tmp)
        sources = [spu / "tests/test_spu_label_window_main.c"]
        sources += [spu / name for name in runtime]
        sources += [ROOT / "runtime/memory/reservation.c",
                    ROOT / "runtime/memory/label_window.c",
                    ROOT / "runtime/platform/win32_compat.c"]
        objects = []
        for index, src in enumerate(sources):
            obj = tmp / f"{index}-{src.stem}.o"
            subprocess.run([cc, *cflags, "-c", str(src), "-o", str(obj)], check=True)
            objects.append(str(obj))
        for src, out in ((ROOT / "runtime/config/config.cpp", "config.o"),
                         (ROOT / "runtime/diag/stop.cpp", "stop.o")):
            obj = tmp / out
            subprocess.run([cxx, *cxxflags, "-c", str(src), "-o", str(obj)], check=True)
            objects.append(str(obj))
        binary = tmp / "test_spu_label_window"
        subprocess.run([cxx, *objects, "-pthread", "-lm", "-o", str(binary)], check=True)

        # mode, transfer EA, size, MFC tag, SPU id
        cases = [
            ("overlap",        "0x20020", "16", "7", "2"),
            ("overlap-edge",   "0x20020", "16", "7", "2"),
            ("overlap-list",   "0x20010", "16", "7", "2"),
            ("outside-covered", "0x1FFF0", "16", "7", "2"),
        ]
        for mode, ea, size, tag, spu_id in cases:
            p = g.run([str(binary), mode], env=env, timeout=60)
            f = g.expect_stop(p, "spu-label-window")
            assert f["func"] == "mfc_do_transfer", (mode, f)
            assert f["cmd"] == "0x20", (mode, f)
            assert (f["ea"], f["size"], f["tag"]) == (ea, size, tag), (mode, f)
            assert f["count"] == "1", (mode, f)
            assert f["pc"] == "0x1234" and f["tid"] == spu_id, (mode, f)

        # The atomic line stores: the staged EAL sits at window+0xA0, inside a
        # line whose base (window+0x80) overlaps the window. The line's bytes
        # must be untouched, which the fixture checks in the parent after the
        # child's stop ("UNTOUCHED: <mode>").
        for mode, cmd in (("putllc", "0xB4"), ("putlluc", "0xB0"),
                          ("putqlluc", "0xB8")):
            p = g.run([str(binary), mode], env=env, timeout=60)
            f = g.expect_stop(p, "spu-label-window")
            assert f["func"] == "spu_mfc_atomic", (mode, f)
            assert f["cmd"] == cmd, (mode, f)
            assert (f["ea"], f["size"], f["tag"]) == ("0x20080", "128", "7"), (mode, f)
            assert f["count"] == "1", (mode, f)
            assert f["pc"] == "0x1234" and f["tid"] == "2", (mode, f)
            assert f"UNTOUCHED: {mode}" in p.stdout, (mode, p.stdout, p.stderr)

        for mode in ("overlap-control", "outside", "putlluc-outside"):
            p = g.run([str(binary), mode], env=env, timeout=60)
            assert p.returncode == 0, (mode, p.returncode, p.stdout, p.stderr)
            assert "spu_puts=0" in p.stdout, (mode, p.stdout, p.stderr)
            assert "PASS:" in p.stdout, (mode, p.stdout, p.stderr)
    print("PASS: an SPU PUT/PUTL and the atomic line stores (PUTLLC, PUTLLUC, "
          "PUTQLLUC) into the registered window stop with kind spu-label-window, "
          "the command, EA, size, tag, SPU id and count, leaving the line "
          "unmoved; a PUT and a PUTLLUC outside the window land")
    return 0


if __name__ == "__main__":
    sys.exit(main())
