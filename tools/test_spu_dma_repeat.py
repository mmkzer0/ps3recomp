#!/usr/bin/env python3
"""The identical-DMA guard: back-off is timing, stopping is an opt-in typed knob.

Builds runtime/spu/tests/test_dma_repeat_main.c against the real DMA engine
(spu_dma.h), the real typed configuration (runtime/config/config.cpp) and the
real named-stop runtime (runtime/diag/stop.cpp), then:

  - default (spu.dma_repeat_stop_secs = 0): 300 identical GETs must neither
    halt the SPU nor stop the process;
  - PS3RECOMP_SPU_DMA_REPEAT_STOP_SECS=1: the same run ends in exit 70 and one
    PS3STOP kind=spu-dma-repeat line carrying image pc ea lsa cmd size count
    secs tid.

Each run is bounded, so a regression to "halt silently" or "never stop" fails
instead of hanging the suite.
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

    with tempfile.TemporaryDirectory(prefix="spu-dma-repeat-") as tmp:
        tmp = Path(tmp)
        objects = []
        for src, flags, out in (
                (spu / "tests/test_dma_repeat_main.c", cflags, "test.o"),
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
        binary = tmp / "test_dma_repeat"
        subprocess.run([cxx, *objects, "-pthread", "-lm", "-o", str(binary)], check=True)

        p = g.run([str(binary)], env=env, timeout=60)
        assert p.returncode == 0, (p.returncode, p.stdout, p.stderr)
        assert "PASS:" in p.stdout, p.stdout

        p = g.run([str(binary), "stop"],
                  env=dict(env, PS3RECOMP_SPU_DMA_REPEAT_STOP_SECS="1"), timeout=60)
        f = g.expect_stop(p, "spu-dma-repeat")
        assert (f["image"], f["pc"], f["ea"], f["lsa"], f["cmd"], f["size"], f["tid"]) == (
            "7", "0x1234", "0x00000200", "0x00100", "0x40", "16", "1"), f
        assert int(f["count"]) >= 256, f
        assert int(f["secs"]) >= 1, f
    print("PASS: identical DMA transfers back off by default and stop only with "
          "spu.dma_repeat_stop_secs set")
    return 0


if __name__ == "__main__":
    sys.exit(main())
