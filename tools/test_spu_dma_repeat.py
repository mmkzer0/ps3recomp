#!/usr/bin/env python3
"""The identical-DMA guard: back-off is timing, stopping is an opt-in typed knob.

Builds runtime/spu/tests/test_dma_repeat_main.c against the real DMA engine
(spu_dma.h), the real typed configuration (runtime/config/config.cpp) and the
real named-stop runtime (runtime/diag/stop.cpp), then:

  - default (spu.dma_repeat_stop_secs = 0): 300 identical GETs must neither
    halt the SPU nor stop the process;
  - PS3RECOMP_SPU_DMA_REPEAT_STOP_SECS=1: the same run ends in exit 70 and one
    PS3STOP kind=spu-dma-repeat line carrying image pc ea lsa cmd size count
    secs tid;
  - the same stop, but with config.cpp/stop.cpp reachable only through a
    static archive: the engine's direct calls must pull both members out of
    the archive. With the old weak NULL hooks the fixture resolved them
    locally, the archive stayed unextracted, and the configured stop was
    silently ignored (the run then ends in "no stop", not 70).

Each run is bounded, so a regression to "halt silently" or "never stop" fails
instead of hanging the suite.

Then drives the watchdog's reporting through the synthetic fixture
runtime/spu/tests/test_spu_dma_repeat.c (a test-owned millisecond clock):
SPU_DMA_REPEAT_LOG=<n> must name the repeated transfer, its size included, and
only when it is set.
"""
import os
import re
import shlex
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
                (ROOT / "runtime/memory/label_window.c", cflags, "label_window.o"),
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

        # Partial-runtime regression: config.cpp and stop.cpp live only in a
        # static archive and nothing in the fixture's main names them. The
        # engine's direct ps3_cfg_*/ps3_stop calls must extract both members.
        # A weak NULL hook in the header satisfies them locally instead, the
        # archive stays unextracted, and the configured stop is ignored -- the
        # fixture then hits its 10 s bound and exits 3, not 70.
        archive = tmp / "libps3recomp_runtime.a"
        subprocess.run([os.environ.get("AR", "ar"), "rcs", str(archive),
                        str(tmp / "config.o"), str(tmp / "stop.o"),
                        str(tmp / "label_window.o")], check=True)
        partial = tmp / "test_dma_repeat_archive"
        subprocess.run([cxx, str(tmp / "test.o"), str(tmp / "pagemap.o"),
                        str(tmp / "compat.o"), str(archive),
                        "-pthread", "-lm", "-o", str(partial)], check=True)

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

        p = g.run([str(partial), "stop"],
                  env=dict(env, PS3RECOMP_SPU_DMA_REPEAT_STOP_SECS="1"), timeout=60)
        f = g.expect_stop(p, "spu-dma-repeat")
        assert (f["image"], f["pc"], f["ea"], f["lsa"], f["cmd"], f["size"], f["tid"]) == (
            "7", "0x1234", "0x00000200", "0x00100", "0x40", "16", "1"), f
        assert int(f["count"]) >= 256, f
        assert int(f["secs"]) >= 1, f
    print("PASS: identical DMA transfers back off by default and stop only with "
          "spu.dma_repeat_stop_secs set (direct and static-archive links)")
    return repeat_log_fixture(cc, cxx, cflags, cxxflags, env)


def repeat_log_fixture(cc, cxx, cflags, cxxflags, env):
    """SPU_DMA_REPEAT_LOG: one episode, one line, naming the repeated transfer.

    The fixture includes spu_channels.c, so it links the rest of the SPU runtime
    plus the typed configuration and named-stop runtime the engine calls.
    """
    spu = ROOT / "runtime" / "spu"
    common = ["spu_drain.c", "spu_lockstep.c", "spu_coherency.c", "spu_workload.c",
              "spurs_policy.c", "spurs_job.c", "spu_tsp_weak.c", "spu_vm_pagemap.c",
              "spurs_policy_blob_weak.c", "spu_interp.c"]
    sources = ([spu / "tests/test_spu_dma_repeat.c"] + [spu / n for n in common] +
               [ROOT / "runtime/memory/reservation.c", ROOT / "runtime/memory/label_window.c",
                ROOT / "runtime/platform/win32_compat.c"])
    flags = cflags + shlex.split(os.environ.get("CFLAGS", ""))
    with tempfile.TemporaryDirectory(prefix="spu-dma-repeat-log-") as tmp:
        tmp = Path(tmp)
        objects = []
        for index, src in enumerate(sources):
            obj = tmp / f"{index}-{src.stem}.o"
            subprocess.run([cc, *flags, "-c", str(src), "-o", str(obj)], check=True)
            objects.append(str(obj))
        for src in (ROOT / "runtime/config/config.cpp", ROOT / "runtime/diag/stop.cpp"):
            obj = tmp / (src.stem + ".o")
            subprocess.run([cxx, *cxxflags, "-c", str(src), "-o", str(obj)], check=True)
            objects.append(str(obj))
        binary = tmp / "fixture"
        subprocess.run([cxx, *objects, "-pthread", "-lm", "-o", str(binary)], check=True)

        def run(*args, extra=None):
            p = subprocess.run([str(binary), *args], env={**env, **(extra or {})},
                               cwd=tmp, capture_output=True, text=True, timeout=30)
            assert p.returncode == 0, (p.returncode, p.stdout, p.stderr)
            return p.stderr

        err = run("log")
        assert "[dma-repeat]" not in err, "repeat log printed while unset"

        err = run("log", extra={"SPU_DMA_REPEAT_LOG": "4"})
        lines = [l for l in err.splitlines() if l.startswith("[dma-repeat]")]
        assert len(lines) == 1, err
        m = re.search(r"img=(\d+) pc=0x[0-9A-F]{5} ea=0x([0-9A-F]{8}) "
                      r"lsa=0x([0-9A-F]{5}) size=(\d+) repeats=(\d+) us=\d+", lines[0])
        assert m, lines[0]
        assert m.group(1) == "5" and m.group(2) == "00020000", lines[0]
        assert m.group(3) == "01000" and m.group(4) == "16", lines[0]
        assert m.group(5) == "299", lines[0]
    print("PASS: SPU_DMA_REPEAT_LOG names the repeated transfer and its size")
    return 0


if __name__ == "__main__":
    sys.exit(main())
