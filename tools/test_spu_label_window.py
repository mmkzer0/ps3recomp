#!/usr/bin/env python3
"""An SPU MFC PUT/PUTL into a registered guest store window stops by name.

Builds runtime/spu/tests/test_spu_label_window_main.c against the real DMA
engine (spu_dma.h), the real window state (runtime/memory/label_window.c), the
typed configuration and the named-stop runtime, then runs one process per
case. A PUT into the RSX label window is a store the runtime cannot order
safely (it may be the value an ACQUIRE waits for, so waiting for a walker pass
would deadlock the producer), and it now ends in

    PS3STOP v=1 kind=spu-label-window func=mfc_do_transfer cmd=0x20
            ea=0x... size=16 tag=7 count=1 pc=0x1234 lr=0 tid=2

with the transfer's EA and size, the SPU's PC and id, and the MFC tag.

Fail-before: before the change the SPU path had no window check at all, so
`overlap`, `overlap-edge` and `overlap-list` returned 0 with the bytes moved.
`overlap-control` reproduces that state inside the same binary (no window
registered) and must still land; `outside-covered` runs the OUTSIDE case's PUT
with a window that covers it and must stop, so the outside case cannot pass by
never noticing an overlap.
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

    with tempfile.TemporaryDirectory(prefix="spu-label-window-") as tmp:
        tmp = Path(tmp)
        objects = []
        for src, out in ((spu / "tests/test_spu_label_window_main.c", "fixture.o"),
                         (spu / "spu_vm_pagemap.c", "pagemap.o"),
                         (ROOT / "runtime/memory/label_window.c", "label_window.o"),
                         (ROOT / "runtime/platform/win32_compat.c", "compat.o")):
            obj = tmp / out
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

        for mode, want in (("overlap-control", "0x20020"), ("outside", "0x20000")):
            p = g.run([str(binary), mode], env=env, timeout=60)
            assert p.returncode == 0, (mode, p.returncode, p.stdout, p.stderr)
            assert "spu_puts=0" in p.stdout, (mode, p.stdout, p.stderr)
            assert "PASS:" in p.stdout, (mode, p.stdout, p.stderr)
    print("PASS: an SPU PUT/PUTL into the registered window stops with kind "
          "spu-label-window and the transfer's EA, size, tag, SPU id and count; "
          "a PUT outside the window lands")
    return 0


if __name__ == "__main__":
    sys.exit(main())
