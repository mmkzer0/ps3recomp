#!/usr/bin/env python3
"""A SPURS task or job the HLE cannot dispatch is a named stop or the firmware's error, never CELL_OK (#20, #40).

Builds tests/test_spurs_dispatch_miss.c (the real libs/spurs/cellSpurs.c over the real runtime units) and runs
one mode per process:

  task-ok, task-null-elf, task-unaligned-elf
        a parseable task is dispatched; a null ELF is CELL_SPURS_TASK_ERROR_NULL_POINTER and one that is not
        16-byte aligned CELL_SPURS_TASK_ERROR_ALIGN (libsre 0x02012520), with no task id taken
  task-bad-elf, task-bad-machine, task-bad-entry, task-vaddr-low, task-attr-bad-elf
        the PPU-side task-image check (libsre 0x02011ACC, and the same check in
        _cellSpursTaskAttributeInitialize 0x02010D3C) returns 0x80410907 (NOEXEC) for a bad magic,
        e_machine != 23, e_entry <= 0x2FFF or a PT_LOAD vaddr <= 0x2FFF -- a return value, never a stop
  task-ctx-align, task-ctx-size, task-ls-pattern, task-ls-pattern-word0, task-no-slot
        the creator's other PPU checks: the context alignment by SDK version (0x80410910), a context below
        0x400 bytes or an lsPattern the context save area cannot hold (0x80410902), and no free task id
        (0x80410901)
  task-segment
        an image the PPU accepts but the taskset policy module cannot load (a PT_LOAD outside the task area)
        ends in spurs-task-halt (RPCS3 spursTasksetLoadElf fails, then spursHalt): the taskset, task id, ELF
        EA and reason, and the calling guest thread as the site
  task-unmapped-elf
        an ELF EA whose header is not inside guest memory is hle-guest-range, not a host fault
  task-no-lifted, job-no-lifted
        a loadable task or job binary the real registry has no lifted program for ends in spurs-dispatch-miss,
        with the fingerprint of the image
  job-ok
        a job whose binary is inside guest memory and dispatches goes on
  job-no-ea, job-no-size, job-out-of-range
        the job-chain walker stops with spu-job-dma-list (eaBinary / sizeBinary / binary-range) where it used
        to log "implausible binary -- skipped" and carry on
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "gates"))
import gates_common as g  # noqa: E402


def fnv1a64(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def spu_elf(vaddr):
    """The 100-byte ELF make_elf builds in the fixture."""
    b = bytearray(100)
    b[0:4] = b"\x7fELF"; b[4] = 1; b[5] = 2; b[6] = 1
    def be16(o, v): b[o:o + 2] = v.to_bytes(2, "big")
    def be32(o, v): b[o:o + 4] = v.to_bytes(4, "big")
    be16(0x10, 2); be16(0x12, 23); be32(0x14, 1); be32(0x18, vaddr); be32(0x1C, 52)
    be16(0x28, 52); be16(0x2A, 32); be16(0x2C, 1)
    be32(52, 1); be32(56, 84); be32(60, vaddr); be32(64, vaddr)
    be32(68, 16); be32(72, 16); be32(76, 5); be32(80, 16); b[84:100] = b"\x5a" * 16
    return bytes(b)


def build(tmp):
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    units = ["spu_channels", "spu_drain", "spu_lockstep", "spu_coherency", "spu_workload",
             "spurs_policy", "spurs_job", "spu_tsp_weak", "spu_vm_pagemap",
             "spurs_policy_blob_weak", "spu_interp"]
    cflags = ["-std=gnu17", "-O1", "-UNDEBUG", "-ffunction-sections", "-fdata-sections",
              "-I" + str(ROOT / "include"), "-I" + str(ROOT / "runtime/spu"),
              "-I" + str(ROOT / "runtime/platform")]
    cxxflags = ["-std=c++20", "-O1", "-UNDEBUG", "-ffunction-sections", "-fdata-sections",
                "-I" + str(ROOT / "include")]
    c_sources = [ROOT / "tests/test_spurs_dispatch_miss.c",
                 *[ROOT / "runtime/spu" / f"{u}.c" for u in units],
                 ROOT / "libs/spurs/spurs_taskset.c",
                 ROOT / "runtime/memory/reservation.c", ROOT / "runtime/platform/win32_compat.c"]
    objects = []
    for index, src in enumerate(c_sources):
        obj = tmp / f"{index}.o"
        subprocess.run([cc, *cflags, "-c", str(src), "-o", str(obj)], check=True)
        objects.append(str(obj))
    for src in (ROOT / "runtime/config/config.cpp", ROOT / "runtime/diag/stop.cpp"):
        obj = tmp / (src.stem + ".o")
        subprocess.run([cxx, *cxxflags, "-c", str(src), "-o", str(obj)], check=True)
        objects.append(str(obj))
    binary = tmp / "test_spurs_dispatch_miss"
    link = ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]
    subprocess.run([cxx, *objects, *link, "-pthread", "-lm", "-o", str(binary)], check=True)
    return str(binary)


def main():
    if os.name == "nt":
        print("SKIP: POSIX fixture")
        return 0
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("SPU_", "SPURS_", "PS3_", "PS3RECOMP_", "YDKJ_"))}
    with tempfile.TemporaryDirectory(prefix="spurs-dispatch-miss-") as tmp:
        binary = build(Path(tmp))

        def run(mode):
            return g.run([binary, mode], env=env, timeout=60)

        def passes(mode):
            p = run(mode)
            assert p.returncode == 0 and f"PASS: {mode}" in p.stdout, (mode, p.returncode, p.stdout, p.stderr[-3000:])

        for mode in ("task-ok", "task-null-elf", "task-unaligned-elf", "job-ok",
                     "task-ok-phdr-paddr", "task-ok-vaddr-wrap", "task-ok-empty-elf",
                     "task-bad-elf", "task-bad-machine", "task-bad-entry", "task-vaddr-low",
                     "task-ctx-align", "task-ctx-size", "task-ls-pattern",
                     "task-ls-pattern-word0", "task-no-slot", "task-attr-bad-elf"):
            passes(mode)

        # spursTasksetLoadElf alignment (RPCS3 :1760 masks three flag bits, :1999 rejects the rest):
        # the restart scan keeps bit 3, so the loader sees a 16-byte-misaligned address and halts.
        f = g.expect_stop(run("task-restart-bit3"), "spurs-task-halt")
        assert f["api"] == "taskset-restart" and f["taskset"] == "0x00004000" and f["task"] == "0" \
            and f["elf"] == "0x00010008" and f["reason"] == "unaligned-elf", f

        f = g.expect_stop(run("task-segment"), "spurs-task-halt")
        assert f["api"] == "cellSpursCreateTask" and f["taskset"] == "0x00004000" and f["task"] == "0" \
            and f["elf"] == "0x00010000" and f["reason"] == "segment-outside-task-area", f
        assert f["pc"] == "0x123450" and f["lr"] == "0x123450" and f["tid"] == "7", f
        f = g.expect_stop(run("task-unmapped-elf"), "hle-guest-range")
        assert f["func"] == "cellSpursCreateTask" and f["arg"] == "elf" and f["ea"] == "0x3FFF0" \
            and f["size"] == "0x34" and f["limit"] == "0x40000", f

        f = g.expect_stop(run("task-no-lifted"), "spurs-dispatch-miss")
        assert f["api"] == "cellSpursCreateTask" and f["taskset"] == "0x00004000" and f["task"] == "0" \
            and f["image"] == "0x00010000" and f["size"] == "0x64" and "job" not in f \
            and f["fingerprint"] == f"0x{fnv1a64(spu_elf(0x3000)):016X}" \
            and f["pc"] == "0x123450" and f["tid"] == "7", f
        f = g.expect_stop(run("job-no-lifted"), "spurs-dispatch-miss")
        assert f["api"] == "jobchain" and f["job"] == "0x00003000" and f["image"] == "0x00030000" \
            and f["size"] == "0x100" and "task" not in f and "taskset" not in f \
            and f["fingerprint"] == f"0x{fnv1a64(bytes([0x11]) * 0x100):016X}", f
        for mode, reason, ea, size in (("job-no-ea", "eaBinary", "0x0", "0x100"),
                                       ("job-no-size", "sizeBinary", "0x30000", "0x0"),
                                       ("job-out-of-range", "binary-range", "0x3FF80", "0x100")):
            f = g.expect_stop(run(mode), "spu-job-dma-list")
            assert f["reason"] == reason and f["job"] == "0x00003000" and f["image"] == "-1" \
                and f["index"] == "-1" and f["ea"] == ea and f["size"] == size, (mode, f)
    print("PASS: a task or job that cannot be dispatched stops by name")
    return 0


if __name__ == "__main__":
    sys.exit(main())
