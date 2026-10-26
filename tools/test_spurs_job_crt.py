#!/usr/bin/env python3
"""The SPURS job loader's JOBCRT gate, Ver14 jm interface, kernel view and bss.

Builds runtime/spu/tests/test_spurs_job_crt_main.c against the real loader
(runtime/spu/spurs_job.c), channel layer, interpreter and lifted-call drain,
the typed configuration and the named-stop runtime, and runs one mode per
process (the stops end the process). The fixture's job images are synthetic:
hand-encoded SPU words over a deterministic junk pattern.

  ver13, ver14   start state, byte-checked: kernel view (0x175, 0x1C8, 0x1CC,
                 0x210), JobContext2 and ctx+0x14, the jm object and the
                 runtime area below it, zeroed bss, buffers and r1 above the
                 bss, and a CRT-like program that reaches main
  dma-fit        an input DMA list that exactly fills sizeInOut runs, with both
                 elements at their 16-byte slots in the ioBuffer
  dma-over, dma-wrap, dma-beyond, dma-zero-ea, dma-size-zero, dma-size-limit,
  dma-size-align, dma-misaligned, dma-desc-range, bin-ea-zero, bin-size-zero
                 spu-job-dma-list stops naming the element (or -1), its EA and
                 size, the LS offset, the cumulative bytes, the capacity and the
                 reason: one byte past sizeInOut; ea+size wrapping 32-bit; an
                 EA past guest memory; a non-zero size from the null page; a
                 zero-size element; a size over 16 KB; a size that is not
                 1/2/4/8 or a 16-byte multiple; an unaligned 8-byte EA; a list
                 that leaves guest memory; eaBinary = 0; sizeBinary = 0
  dma-seam       the local store the loader stages into is observable (the
                 loader is built with SPURS_JOB_LS_TEST): the check behind the
                 "no local-store byte written" assertions
  dma-lsa, dma-lsa-edge, dma-lsa-over
                 1/2/4/8-byte elements with non-zero EAL low nibbles land at
                 the MFC's LSA (low nibble from EAL), byte for byte what the
                 MFC list engine (mfc_run_list) stages; the last short element
                 may end exactly at sizeInOut, one byte less is io-overrun
  dma-stall, dma-stall-zero
                 a stall-and-notify element (bit 31 of the size word), with a
                 transfer and as a zero-size marker: the MFC engine parks the
                 list and raises the notify; a job start has no one to
                 acknowledge it, so the loader stops (spu-job-dma-list,
                 reason stall-notify)
  dma-late-bad   a malformed LAST element after a valid list
  every dma-* stop runs the loader over a shared 0xA5-filled local store and
  asserts that no byte of it was written before the stop
  ver13 (hash)   the Ver13 local store at entry, whole, equals the hash the
                 loader produced before Ver14 existed (GOLDEN_VER13_LS)
  fit            the last region (a cache buffer) ending exactly at the runtime
                 area's base runs, with the bytes in place
  overlap, image-edge, stack-max, cache-over, huge-io
                 spu-job-memory stops naming every size, the region, the end
                 address p and the limit: image + bss one quadword over; image +
                 bss exact (the io buffer above stops); sizeStack = 0xFFFF; a
                 cache buffer one byte over; a 0xFFFFFFFF io size (no wrap)
  swap-*         jm word 1 called with a tag still incomplete returns once the
                 MFC has completed it, with every register but r3 preserved,
                 for an interpreted job and for lifted-style bisl code; with
                 every tag complete it returns after one poll
  word0-*        jm word 0 is a spu-job-jm-entry stop on both paths
  ver12, ver15, garbage, empty
                 spu-job-crt-version stops naming the marker bytes
"""
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests" / "gates"))
import gates_common as g  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

# SHA-256 of the 256 KB local store the loader hands a Ver13 job at entry for
# the fixture's "ver13" image. Produced by the loader as of the commit before
# the Ver14 work (dc177d1); the Ver13 path must not change by a byte.
GOLDEN_VER13_LS = "044a8a8591f84fa1fce1d5c0c89ae921a462c1f45e51893300223827d68ba2df"

RT_LS = 0x3FAC0
IMAGE_SIZE = 0x1000


def build(tmp):
    spu = ROOT / "runtime" / "spu"
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    cflags = ["-std=gnu17", "-O1", "-UNDEBUG", "-DSPURS_JOB_LS_TEST",
              "-I" + str(ROOT / "include"), "-I" + str(spu),
              "-I" + str(ROOT / "runtime/platform")]
    cxxflags = ["-std=c++20", "-O1", "-UNDEBUG", "-I" + str(ROOT / "include")]
    sources = ["spu_channels.c", "spu_interp.c", "spu_coherency.c", "spu_lockstep.c",
               "spu_drain.c", "spu_vm_pagemap.c", "spurs_job.c"]
    c_sources = [spu / "tests/test_spurs_job_crt_main.c", *[spu / s for s in sources],
                 ROOT / "runtime/memory/reservation.c",
                 ROOT / "runtime/memory/label_window.c",
                 ROOT / "runtime/platform/win32_compat.c"]
    objects = []
    for index, src in enumerate(c_sources):
        obj = tmp / f"{index}.o"
        subprocess.run([cc, *cflags, "-c", str(src), "-o", str(obj)], check=True)
        objects.append(str(obj))
    for src in (ROOT / "runtime/config/config.cpp", ROOT / "runtime/diag/stop.cpp"):
        obj = tmp / (src.stem + ".o")
        subprocess.run([cxx, *cxxflags, "-c", str(src), "-o", str(obj)], check=True)
        objects.append(str(obj))
    binary = tmp / "test_spurs_job_crt"
    subprocess.run([cxx, *objects, "-pthread", "-lm", "-o", str(binary)], check=True)
    return binary


def hexs(n):
    return f"0x{n:X}"


def main():
    if os.name == "nt":
        print("SKIP: fixture currently requires the POSIX compiler/linker")
        return 0
    # The ambient environment must not arm a knob the test did not choose.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("SPU_", "PS3_", "PS3RECOMP_", "YDKJ_", "SPURS_"))}

    with tempfile.TemporaryDirectory(prefix="spurs-job-crt-") as tmp:
        tmp = Path(tmp)
        binary = str(build(tmp))

        def run(*args):
            return g.run([binary, *args], env=env, timeout=120)

        def passes(mode, *args):
            p = run(mode, *args)
            assert p.returncode == 0 and f"PASS: {mode}" in p.stdout, (
                mode, p.returncode, p.stdout, p.stderr[-3000:])
            assert "FAIL" not in p.stdout, p.stdout

        dump = tmp / "ver13.ls"
        passes("ver13", str(dump))
        digest = hashlib.sha256(dump.read_bytes()).hexdigest()
        assert digest == GOLDEN_VER13_LS, (
            f"the Ver13 local store at entry changed: sha256 {digest}")
        for mode in ("ver14", "fit", "dma-fit", "dma-seam", "dma-lsa", "dma-lsa-edge",
                     "swap-interp", "swap-lifted", "swap-idle-interp", "swap-idle-lifted"):
            passes(mode)

        # The input DMA list: a violation is a named stop before any byte is
        # staged. Fields: the element, its EA and size, where it would land,
        # the bytes through it, the declared capacity and the reason.
        def dma_stop(mode, index, ea, size, ls, cumulative, capacity, reason,
                     job="0x00010000"):
            p = run(mode)
            f = g.expect_stop(p, "spu-job-dma-list")
            assert "FAIL" not in p.stdout, (mode, p.stdout)
            assert f"LS-UNTOUCHED: {mode}" in p.stdout, (mode, p.stdout)
            want = {"job": job, "image": "7", "index": str(index),
                    "ea": hexs(ea), "size": hexs(size), "ls": f"0x{ls:05X}",
                    "cumulative": hexs(cumulative), "capacity": hexs(capacity),
                    "reason": reason}
            assert {k: f.get(k) for k in want} == want, (mode, f, want)

        # one byte past sizeInOut (0x80): the second element starts at 0x1080
        dma_stop("dma-over", 1, 0x30080, 1, 0x1080, 0x81, 0x80, "io-overrun")
        # ea + size wraps 32-bit to 0; only the 64-bit bound sees it
        dma_stop("dma-wrap", 0, 0xFFFFFFF0, 0x20, 0x1000, 0x20, 0x80, "ea-range")
        # past the fixture's 1 MB guest VM
        dma_stop("dma-beyond", 0, 0x200000, 0x10, 0x1000, 0x10, 0x80, "ea-range")
        # the reserved null page
        dma_stop("dma-zero-ea", 0, 0, 0x10, 0x1000, 0x10, 0x80, "ea-null")
        # a zero-size element is not a transfer to skip
        dma_stop("dma-size-zero", 0, 0x30000, 0, 0x1000, 0, 0x80, "size-zero")
        # one byte past the MFC's 16 KB limit
        dma_stop("dma-size-limit", 0, 0x30000, 0x4001, 0x1000, 0x4001, 0x80,
                 "size-limit")
        # 24 bytes is neither 1/2/4/8 nor a 16-byte multiple
        dma_stop("dma-size-align", 0, 0x30000, 0x18, 0x1000, 0x18, 0x80,
                 "size-align")
        # an 8-byte transfer needs 8-byte alignment (it would sit at nibble 4)
        dma_stop("dma-misaligned", 0, 0x30004, 8, 0x1004, 0xC, 0x80, "ea-align")
        # the 8-byte element at EAL nibble 8 takes ioBuffer+0x38..0x40: one
        # byte past a 0x3F buffer, though its quadword base is well inside
        dma_stop("dma-lsa-over", 3, 0x30038, 8, 0x1038, 0x40, 0x3F, "io-overrun")
        # stall-and-notify, with a transfer (size 0x10) and as a zero-size
        # marker; the size is the low 15 bits of the word
        dma_stop("dma-stall", 1, 0x30010, 0x10, 0x1010, 0x20, 0x80, "stall-notify")
        dma_stop("dma-stall-zero", 1, 0x30010, 0, 0x1010, 0x10, 0x80, "stall-notify")
        # a bad last element, 0x18 after 0x80 + 0x10 bytes: nothing written
        dma_stop("dma-late-bad", 2, 0x30090, 0x18, 0x1090, 0xA8, 0x100, "size-align")
        # the list itself leaves guest memory before its last element
        dma_stop("dma-desc-range", -1, 0xFFF30, 0x100, 0, 0, 0x80,
                 "descriptor-range", job="0x000FFF00")
        # P3: no binary named by the descriptor
        dma_stop("bin-ea-zero", -1, 0, 0x1000, 0, 0, 0x80, "eaBinary")
        dma_stop("bin-size-zero", -1, 0x20000, 0, 0, 0, 0x80, "sizeBinary")
        # a binary or a cache entry outside the (bounded) guest VM, which is
        # followed by an inaccessible page: a stop, not a fault past the VM
        dma_stop("bin-range", -1, 0xFFFF0, 0x1000, 0, 0, 0x80, "binary-range")
        dma_stop("cache-range", 0, 0x100000, 1, 0, 1, 0x80, "cache-range")
        # the chain walker's descriptor span (256 bytes) leaves the VM
        dma_stop("desc-range", -1, 0xFFF80, 0x100, 0, 0, 0, "descriptor-range",
                 job="0x000FFF80")

        # jm word 0: the trap, on both paths
        for mode in ("word0-interp", "word0-lifted"):
            f = g.expect_stop(run(mode), "spu-job-jm-entry")
            assert (f["job"], f["image"], f["entry"], int(f["pc"], 16)) == (
                "0x00010000", "7", "0x3FAC0", RT_LS), f
            assert int(f["lr"], 16) > 0, f
        assert int(g.expect_stop(run("word0-lifted"), "spu-job-jm-entry")["lr"], 16) == 0x2F0

        # the version gate
        for mode, marker, raw in (("ver12", "JOBCRT Ver12", b"JOBCRT Ver12"),
                                  ("ver15", "JOBCRT Ver15", b"JOBCRT Ver15"),
                                  ("garbage", "??JOBCRT ver", b"\x01\x02JOBCRT ver"),
                                  ("empty", "?" * 12, bytes(12))):
            f = g.expect_stop(run(mode), "spu-job-crt-version")
            assert (f["job"], f["image"], f["marker"], f["bytes"]) == (
                "0x00010000", "7", marker, raw.hex().upper()), f

        # Every stop names the descriptor's sizes (bytes), the region that
        # crossed, the end address p it reached and the limit. Defaults of the
        # fixture's descriptor: io 0x80, out 0x100, scratch 0x200, stack 0x400.
        def memory_stop(mode, region, p, **sizes):
            f = g.expect_stop(run(mode), "spu-job-memory")
            want = {"job": "0x00010000", "image": "7", "binary": hexs(IMAGE_SIZE),
                    "bss": "0x0", "io": "0x80", "out": "0x100", "scratch": "0x200",
                    "stack": "0x400", "cache": "0x0", "limit": hexs(RT_LS),
                    "region": region, "p": hexs(p)}
            want.update(sizes)
            want.setdefault("memory", hexs(IMAGE_SIZE + int(want["bss"], 16)))
            assert {k: f.get(k) for k in want} == want, (mode, f, want)

        # image + bss one quadword past the runtime area
        bss = ((RT_LS - IMAGE_SIZE) // 16 + 1) * 16
        memory_stop("overlap", "image", IMAGE_SIZE + bss, bss=hexs(bss))
        # image + bss ending exactly at the runtime area is not the image's
        # fault: the io buffer above (at the next 1 KB) stops
        edge = RT_LS - IMAGE_SIZE
        memory_stop("image-edge", "io", 0x3FC00 + 0x80, bss=hexs(edge))
        # sizeStack = 0xFFFF: r1 would wrap LS addressing onto the context
        memory_stop("stack-max", "stack", 0x1800 + 0x100400, stack="0xFFFF0")
        # the cache buffer one byte past the runtime area (0x3F800 + 0x2C1)
        memory_stop("cache-over", "cache", RT_LS + 1, bss=hexs(0x3E800 - IMAGE_SIZE),
                    cache="0x2C1")
        # a 32-bit size must not wrap: ALIGN1024(0xFFFFFFFF) used to be 0
        memory_stop("huge-io", "io", IMAGE_SIZE + 0xFFFFFFFF, io="0xFFFFFFFF")
    print("PASS: the JOBCRT gate, the Ver14 jm interface, kernel view, bss and input DMA list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
