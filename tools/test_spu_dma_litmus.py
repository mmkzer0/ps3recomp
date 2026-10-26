#!/usr/bin/env python3
"""SPU DMA ordering: acquire before a GET reads guest memory, release after a PUT wrote it.

Builds runtime/spu/tests/test_dma_litmus_main.c against the real DMA engine
(spu_dma.h) and runs message passing in both directions between a PPU thread,
which barriers as the lifter's lwsync does, and an SPU issuing GET/PUT through
mfc_submit():

  get: the PPU stores a payload, executes an release fence, stores a sequence
       number; the SPU GETs the sequence number and then the payload and must
       never find the payload older than the number;
  put: the SPU PUTs a payload and then a sequence number; the PPU loads the
       number, executes an acquire fence and loads the payload, with the same rule.

    test_spu_dma_litmus.py [iterations]      (default 2,000,000 per direction;
                                              the stress tier passes 100,000,000)
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == "nt":
        print("SKIP: fixture currently requires the POSIX compiler/linker")
        return 0
    iterations = sys.argv[1] if len(sys.argv) > 1 else "2000000"
    spu = ROOT / "runtime" / "spu"
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    cflags = ["-std=gnu17", "-O2", "-UNDEBUG", "-pthread",
              "-I" + str(ROOT / "include"), "-I" + str(spu),
              "-I" + str(ROOT / "runtime/platform")]
    cxxflags = ["-std=c++20", "-O1", "-UNDEBUG", "-I" + str(ROOT / "include")]
    # The ambient environment must not arm a diagnostic knob the test did not choose.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("SPU_", "PS3_", "PS3RECOMP_", "YDKJ_", "SPURS_", "LBP_"))}

    with tempfile.TemporaryDirectory(prefix="spu-dma-litmus-") as tmp:
        tmp = Path(tmp)
        objects = []
        for src, out in ((spu / "tests/test_dma_litmus_main.c", "test.o"),
                         (spu / "spu_vm_pagemap.c", "pagemap.o"),
                         (ROOT / "runtime/memory/label_window.c", "label_window.o"),
                         (ROOT / "runtime/platform/win32_compat.c", "compat.o")):
            obj = tmp / out
            subprocess.run([cc, *cflags, "-c", str(src), "-o", str(obj)], check=True)
            objects.append(str(obj))
        # spu_dma.h calls the typed configuration and the named-stop runtime directly.
        for src, out in ((ROOT / "runtime/config/config.cpp", "config.o"),
                         (ROOT / "runtime/diag/stop.cpp", "stop.o")):
            obj = tmp / out
            subprocess.run([cxx, *cxxflags, "-c", str(src), "-o", str(obj)], check=True)
            objects.append(str(obj))
        binary = tmp / "test_dma_litmus"
        subprocess.run([cxx, *objects, "-pthread", "-lm", "-o", str(binary)], check=True)

        for direction in ("get", "put"):
            p = subprocess.run([str(binary), direction, iterations], env=env, cwd=tmp,
                               capture_output=True, text=True, timeout=3000)
            assert p.returncode == 0 and "LITMUS" in p.stdout, (direction, p.returncode, p.stdout, p.stderr)
            print(p.stdout.strip())
    print("PASS: SPU DMA GET acquires and PUT releases, %s publications in each direction" % iterations)
    return 0


if __name__ == "__main__":
    sys.exit(main())
