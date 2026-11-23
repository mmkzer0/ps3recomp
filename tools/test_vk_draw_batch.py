#!/usr/bin/env python3
"""The Vulkan backend's typed switches and its presented-frame dump, through the real backend.

usage: test_vk_draw_batch.py <ps3recomp_host built with PS3RECOMP_RSX_VULKAN=ON>

The host's `--draw` scene presents two frames on the register-file draw engine. Each case runs it in a process
of its own (the backend reads the registry once) and looks only at what the backend says and writes:

  vk.draw_batch, vk.draw_batch_max   `[RSX vulkan] draw batching: on (up to N ...)` appears exactly when the
                                     registry's PS3RECOMP_VK_DRAW_BATCH is on, carries
                                     PS3RECOMP_VK_DRAW_BATCH_MAX, and the old unregistered VK_DRAW_BATCH is
                                     not read; a maximum above the descriptor pool is the config-invalid stop;
  PS3RECOMP_VK_DUMP_LIST / _RANGE    the files are `<base>_s<present_seq>_f<flip>.ppm` for exactly the
                                     presents named, a list beats a range, a malformed or over-long value is
                                     the config-invalid stop before any frame is written, and a dump with
                                     neither keeps writing one file;
  batching on against off            the presented frames are byte-identical (what the dump is for);
  batches in flight                  with a submission cap of 1 or 2 operations every batch is queued without
                                     render-to-texture, feedback, depth-as-texture,
                                     MRT, cube, texture and 2D/cube dummy-fallback
                                     scenes, and many full-frame draws over
                                     several frames, present the frames batching
                                     off does: a present, a one-shot or a slot's
                                     reuse never reads or overwrites what an
                                     earlier, unwaited batch has still to write.
  vk.draw_batch_min                  the adaptive kick says so in its line, is lowered to the maximum when above
                                     it, is silent at its default 0 (the fixed cap), and with a minimum of 1 or
                                     16 and a small maximum the same scenes, and 200 scissored draws, present
                                     the frames batching off does: a batch kicked on an idle GPU, with others
                                     still in flight, is ordered as a full one is.
  validation layer                   where VK_LAYER_KHRONOS_validation can be loaded (VK_LAYER_PATH, the SDK or
                                     vulkan-validationlayers), the scenes under synchronization validation at
                                     several submission sizes report no validation or hazard error: every
                                     barrier the batch no longer records per draw was not needed; a skip says
                                     when the layer is missing, and VK_DRAW_BATCH_REQUIRE_VALIDATION=1 makes
                                     it a failure (not PS3RECOMP_*: that prefix is reserved for configuration,
                                     and the other tests in the same ctest run would stop on it).
  vk.coalesce_passes                 off by default, said when on, ignored without batching, a malformed value is
                                     the config-invalid stop; 100 draws into one framebuffer take a few passes,
                                     a clear between draws of one framebuffer is recorded inside the pass (the
                                     host scene `--draws=N:S:W:C`), and every scene above, at submission sizes of
                                     1, 2, 64 and 512, presents the frames batching off does, and reports no
                                     synchronization error where the validation layer is installed.

Every ordinary scene must exit successfully before its logs or pixels count as evidence; deliberately invalid
configuration cases assert their nonzero exit as part of the stop contract. Exit 77: no usable Vulkan device or
the draw engine path is not built (needs glslang), nothing to run."""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

FAILED = 0


def check(cond, what):
    global FAILED
    print(("[PASS] " if cond else "[FAIL] ") + what)
    if not cond:
        FAILED += 1


def run(host, workdir, *scene, expected_rc=0, **env):
    """One host scene run; returns (exit code, combined output)."""
    e = {k: v for k, v in os.environ.items()
         if not k.startswith("PS3RECOMP_") and k not in ("VK_DRAW_BATCH", "VK_DRAW_BATCH_MAX")}
    e["PS3RECOMP_VK_GUEST_PROGRAMS"] = "1"
    e.update(env)
    args = [host, *(scene or ("--draw",))]
    p = subprocess.run(args, check=False, cwd=workdir, env=e, capture_output=True, text=True, timeout=120)
    out = p.stdout + p.stderr
    if expected_rc is not None and p.returncode != expected_rc:
        check(False, f"{' '.join(args[1:])}: expected exit {expected_rc}, got {p.returncode}: {out[-500:]}")
    if expected_rc == 0:
        # The host compares the presented centre only when the readback returned a non-zero pixel; a run
        # without its verdict line exited 0 without checking anything.
        check("[host] presented pixel:" in out, f"{' '.join(args[1:])}: the host checked the presented pixel")
    return p.returncode, out


def dumps(directory):
    return sorted(p.name for p in Path(directory).iterdir() if p.name.startswith("f"))


def main():
    if len(sys.argv) != 2:
        # dev-ci's tool loop runs every tools/test_*.py bare; this one needs the host binary that
        # ctest (vk_draw_batch) passes, so a bare run is a visible skip, not a failure of the loop.
        print("SKIP: tools/test_vk_draw_batch.py needs a ps3recomp_host built with "
              "PS3RECOMP_RSX_VULKAN=ON (ctest vk_draw_batch passes it)")
        return 0
    host = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="vk-draw-batch-") as tmp:
        made = []

        def fresh(name):
            d = Path(tmp) / f"{len(made)}-{name}"
            made.append(d)
            d.mkdir()
            return d

        # Is there anything to test? A host with no Vulkan device, or built without the translator, never
        # reaches the engine path; that is a skip, not a pass.
        rc, out = run(host, fresh("probe"), expected_rc=None)
        if "[host] backend: Vulkan" not in out or "register-file draw engine path" not in out:
            print("SKIP: no Vulkan device or no draw engine path here:\n" + out[-600:])
            return 77

        rc, out = run(host, fresh("off"))
        check("draw batching" not in out, "batching is off by default")

        rc, out = run(host, fresh("on"), PS3RECOMP_VK_DRAW_BATCH="1")
        check("draw batching: on (up to 512 draws per submission)" in out, "PS3RECOMP_VK_DRAW_BATCH=1: on, 512 by default")

        rc, out = run(host, fresh("max"), PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_DRAW_BATCH_MAX="7")
        check("draw batching: on (up to 7 draws per submission)" in out, "PS3RECOMP_VK_DRAW_BATCH_MAX=7 is the cap")

        rc, out = run(host, fresh("edge"), PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_DRAW_BATCH_MAX="1024")
        check("draw batching: on (up to 1024 draws per submission)" in out, "1024, the descriptor pool, is accepted")

        rc, out = run(host, fresh("zero"), PS3RECOMP_VK_DRAW_BATCH="0", PS3RECOMP_VK_DRAW_BATCH_MAX="7")
        check("draw batching" not in out, "PS3RECOMP_VK_DRAW_BATCH=0 is off whatever the maximum")

        rc, out = run(host, fresh("old"), VK_DRAW_BATCH="1", VK_DRAW_BATCH_MAX="7")
        check("draw batching" not in out, "the old unregistered VK_DRAW_BATCH is not read")

        rc, out = run(host, fresh("big"), expected_rc=None,
                      PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_DRAW_BATCH_MAX="1025")
        check(rc != 0 and "kind=config-invalid" in out and "key=vk.draw_batch_max" in out and "draw batching" not in out,
              "a maximum above the descriptor pool is a config-invalid stop, not a silent cap")

        # ---- the dump --------------------------------------------------------------------------------
        d = fresh("range")
        rc, out = run(host, d, PS3RECOMP_VK_DUMP=str(d / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9")
        check(dumps(d) == ["f_s00000_fnone.ppm", "f_s00001_fnone.ppm", "f_s00002_fnone.ppm"],
              f"RANGE 0-9 writes the scene's three presents (the host's default --frames=3): {dumps(d)}")

        d = fresh("range1")
        rc, out = run(host, d, PS3RECOMP_VK_DUMP=str(d / "f"), PS3RECOMP_VK_DUMP_RANGE="1:1")
        check(dumps(d) == ["f_s00001_fnone.ppm"], f"RANGE 1:1 writes that present only: {dumps(d)}")

        d = fresh("list")
        rc, out = run(host, d, PS3RECOMP_VK_DUMP=str(d / "f.ppm"), PS3RECOMP_VK_DUMP_LIST="0",
                      PS3RECOMP_VK_DUMP_RANGE="1-9")
        check(dumps(d) == ["f_s00000_fnone.ppm"], f"LIST 0 beats RANGE 1-9, and a .ppm on the base is replaced: {dumps(d)}")

        d = fresh("none")
        rc, out = run(host, d, PS3RECOMP_VK_DUMP=str(d / "f"), PS3RECOMP_VK_DUMP_LIST="5,6")
        check(dumps(d) == [], f"presents the list does not name write nothing: {dumps(d)}")

        d = fresh("legacy")
        rc, out = run(host, d, PS3RECOMP_VK_DUMP=str(d / "f"))
        check(dumps(d) == ["f"], f"a dump with neither writes the one file, overwritten: {dumps(d)}")
        head = (d / "f").read_bytes()[:16]
        check(head.startswith(b"P6\n1280 720\n255\n"), f"it is a 1280x720 binary PPM: {head!r}")

        for env, value, key in (("PS3RECOMP_VK_DUMP_LIST", "120,,240", "PS3RECOMP_VK_DUMP_LIST"),
                                ("PS3RECOMP_VK_DUMP_LIST", "abc", "PS3RECOMP_VK_DUMP_LIST"),
                                ("PS3RECOMP_VK_DUMP_LIST", ",".join(str(i) for i in range(33)), "PS3RECOMP_VK_DUMP_LIST"),
                                ("PS3RECOMP_VK_DUMP_RANGE", "9-1", "PS3RECOMP_VK_DUMP_RANGE"),
                                ("PS3RECOMP_VK_DUMP_RANGE", "5", "PS3RECOMP_VK_DUMP_RANGE")):
            d = fresh("bad")
            rc, out = run(host, d, expected_rc=None, PS3RECOMP_VK_DUMP=str(d / "f"), **{env: value})
            check(rc != 0 and "kind=config-invalid" in out and f"env={key}" in out and dumps(d) == [],
                  f"{env}={value[:24]!r} is a config-invalid stop naming it, and nothing is written (rc {rc})")
        d = fresh("long")
        rc, out = run(host, d, expected_rc=None, PS3RECOMP_VK_DUMP=str(d / "f"),
                      PS3RECOMP_VK_DUMP_LIST=",".join(str(i) for i in range(33)))
        check(rc != 0 and "count=33" in out and "max=32" in out,
              "an over-long list stops with its count and the limit")

        d = fresh("selectoff")
        rc, out = run(host, d, PS3RECOMP_VK_DUMP_LIST="abc")
        check("kind=config-invalid" not in out, "LIST without a dump base is not looked at")

        # ---- batching on against off: the same frames ------------------------------------------------
        a, b = fresh("frames-off"), fresh("frames-on")
        run(host, a, PS3RECOMP_VK_DUMP=str(a / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9")
        run(host, b, PS3RECOMP_VK_DUMP=str(b / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9", PS3RECOMP_VK_DRAW_BATCH="1")
        check(dumps(a) == dumps(b) and len(dumps(a)) == 3, f"the same presents were written: {dumps(a)} / {dumps(b)}")
        for name in dumps(a):
            same = (a / name).read_bytes() == (b / name).read_bytes()
            check(same and (a / name).stat().st_size > 1000, f"{name}: batching on is byte-identical to off")

        # ---- batches in flight: the same frames with every operation its own, unwaited submission ----------
        rc, out = run(host, fresh("slots"), PS3RECOMP_VK_DRAW_BATCH="1")
        check("submissions in flight" in out, "batching says how many submissions it keeps in flight")
        scenes = (("--rtt",), ("--feedback",), ("--depthtex",), ("--mrt",), ("--cube",), ("--tex",),
                  ("--unbound",), ("--cube-unbound",), ("--draws=24", "--frames=4"))
        for scene in scenes:
            name = " ".join(scene)
            off = fresh("flight-off")
            run(host, off, *scene, PS3RECOMP_VK_DUMP=str(off / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9")
            check(len(dumps(off)) >= 2, f"{name}: batching off presents frames to compare: {dumps(off)}")
            for cap in ("1", "2"):
                on = fresh("flight-on")
                rc, out = run(host, on, *scene, PS3RECOMP_VK_DUMP=str(on / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9",
                              PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_DRAW_BATCH_MAX=cap)
                same = dumps(on) == dumps(off) and all(
                    (on / n).read_bytes() == (off / n).read_bytes() for n in dumps(off))
                check(same and "VkResult" not in out,
                      f"{name}, {cap} operation(s) per submission: frames are byte-identical to batching off")

        # ---- gpu timing: a diagnostic that changes no pixel, and says what it did --------------------------
        scene = ("--draws=24", "--frames=4")
        quiet = fresh("timing-off")
        rc, out = run(host, quiet, *scene, PS3RECOMP_VK_DUMP=str(quiet / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9",
                      PS3RECOMP_VK_DRAW_BATCH="1")
        check("gpu timing" not in out, "gpu timing is silent unless asked for")
        timed = fresh("timing-on")
        rc, out = run(host, timed, *scene, PS3RECOMP_VK_DUMP=str(timed / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9",
                      PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_GPU_TIMING="2")
        if "gpu timing: on" in out:
            check("gpu timing (window)" in out and "gpu timing (total):" in out and "busy" in out,
                  "PS3RECOMP_VK_GPU_TIMING=2: a window line every 2 presents and a total at shutdown")
            check("pairs were not available" not in out, "every timestamp pair was available when read")
        else:
            check("gpu timing: not available" in out, "a queue without timestamps says so and measures nothing")
        same = dumps(timed) == dumps(quiet) and len(dumps(quiet)) >= 2 and all(
            (timed / n).read_bytes() == (quiet / n).read_bytes() for n in dumps(quiet))
        check(same and "VkResult" not in out, "gpu timing on: frames are byte-identical to gpu timing off")
        rc, out = run(host, fresh("timing-bad"), PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_GPU_TIMING="often")
        check("stays off" in out and "gpu timing: on" not in out, "a malformed PS3RECOMP_VK_GPU_TIMING keeps it off, said")

        # ---- vk.draw_batch_min: the adaptive kick -------------------------------------------------------
        rc, out = run(host, fresh("min-off"), PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_DRAW_BATCH_MIN="0")
        check("whenever the GPU is idle" not in out, "PS3RECOMP_VK_DRAW_BATCH_MIN=0 is the fixed cap: no adaptive line")
        rc, out = run(host, fresh("min-on"), PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_DRAW_BATCH_MIN="16")
        check("from 16 draws a batch is also submitted whenever the GPU is idle" in out,
              "PS3RECOMP_VK_DRAW_BATCH_MIN=16: said in the batching lines")
        rc, out = run(host, fresh("min-hi"), PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_DRAW_BATCH_MAX="4",
                      PS3RECOMP_VK_DRAW_BATCH_MIN="9")
        check("vk.draw_batch_min 9 is above vk.draw_batch_max 4, using 4" in out and "from 4 draws" in out,
              "a minimum above the maximum is lowered to it, said")
        rc, out = run(host, fresh("min-big"), expected_rc=None,
                      PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_DRAW_BATCH_MIN="1025")
        check(rc != 0 and "kind=config-invalid" in out and "key=vk.draw_batch_min" in out,
              "a minimum above the descriptor pool is a config-invalid stop")
        rc, out = run(host, fresh("min-kick"), "--draws=200:16", "--frames=4", PS3RECOMP_VK_DRAW_BATCH="1",
                      PS3RECOMP_VK_DRAW_BATCH_MIN="16")
        m = re.search(r"batch kicks: (\d+) on a full batch, (\d+) on an idle GPU", out)
        check(m is not None and int(m.group(2)) > 0,
              "min 16 over 200 draws a frame: the GPU is idle when a frame starts, so a batch is kicked on it "
              f"({m.group(0) if m else 'no kick line'})")
        for scene in scenes + (("--draws=200:16", "--frames=4"),):
            name = " ".join(scene)
            off = fresh("adaptive-off")
            run(host, off, *scene, PS3RECOMP_VK_DUMP=str(off / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9")
            for lo, hi in (("1", "512"), ("16", "64")):
                on = fresh("adaptive-on")
                rc, out = run(host, on, *scene, PS3RECOMP_VK_DUMP=str(on / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9",
                              PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_DRAW_BATCH_MIN=lo, PS3RECOMP_VK_DRAW_BATCH_MAX=hi)
                same = dumps(on) == dumps(off) and len(dumps(off)) >= 2 and all(
                    (on / n).read_bytes() == (off / n).read_bytes() for n in dumps(off))
                check(same and "VkResult" not in out,
                      f"{name}, kick from {lo} on an idle GPU up to {hi}: frames are byte-identical to batching off")

        # ---- the Khronos validation layer with synchronization validation, where it is installed ------------
        # The ordering inside a batch (one barrier per hazard, not one per draw) is only provable there: the
        # frames cannot tell a missing barrier from a lucky schedule on a software rasteriser.
        sync = dict(VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation", VK_VALIDATION_VALIDATE_SYNC="1")
        rc, out = run(host, fresh("layer-probe"), PS3RECOMP_VK_DRAW_BATCH="1", VK_LOADER_DEBUG="layer", **sync)
        if 'Insert instance layer "VK_LAYER_KHRONOS_validation"' not in out:
            msg = ("the Khronos validation layer is not installed (VK_LAYER_PATH, the SDK or "
                   "vulkan-validationlayers): the batch's barriers are not checked here")
            print("[SKIP] " + msg)
            if os.environ.get("VK_DRAW_BATCH_REQUIRE_VALIDATION") == "1":
                check(False, msg)
        else:
            for scene in scenes + (("--draws=200:16", "--frames=4"), ("--draws=40:0:0:5", "--frames=3")):
                name = " ".join(scene)
                for lo, hi, coal in (("0", "1", "0"), ("0", "2", "0"), ("16", "64", "0"), ("16", "512", "0"),
                                     ("0", "1", "1"), ("0", "2", "1"), ("16", "64", "1"), ("16", "512", "1")):
                    rc, out = run(host, fresh("layer"), *scene, PS3RECOMP_VK_DRAW_BATCH="1",
                                  PS3RECOMP_VK_DRAW_BATCH_MIN=lo, PS3RECOMP_VK_DRAW_BATCH_MAX=hi,
                                  PS3RECOMP_VK_COALESCE_PASSES=coal, **sync)
                    bad = [ln for ln in out.splitlines() if "Validation Error" in ln or "SYNC-HAZARD" in ln]
                    check(not bad, f"{name}, kick from {lo} up to {hi}, coalescing {'on' if coal == '1' else 'off'}: "
                                   "no validation or synchronization error" + (f": {bad[0][:200]}" if bad else ""))

        # ---- vk.coalesce_passes: a render pass kept open across draws into one framebuffer ---------------------
        rc, out = run(host, fresh("coal-default"), PS3RECOMP_VK_DRAW_BATCH="1")
        check("render pass stays open" not in out and "render passes:" not in out, "pass coalescing is off by default")
        rc, out = run(host, fresh("coal-on"), PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_COALESCE_PASSES="1")
        check("a render pass stays open across draws" in out, "PS3RECOMP_VK_COALESCE_PASSES=1 with batching: said")
        rc, out = run(host, fresh("coal-nobatch"), PS3RECOMP_VK_COALESCE_PASSES="1")
        check("render pass stays open" not in out and "render passes:" not in out,
              "without vk.draw_batch it is ignored")
        rc, out = run(host, fresh("coal-bad"), expected_rc=None,
                      PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_COALESCE_PASSES="maybe")
        check(rc != 0 and "kind=config-invalid" in out and "env=PS3RECOMP_VK_COALESCE_PASSES" in out,
              "a malformed PS3RECOMP_VK_COALESCE_PASSES is a config-invalid stop")
        rc, out = run(host, fresh("coal-count"), "--draws=100:0", "--frames=4", PS3RECOMP_VK_DRAW_BATCH="1",
                      PS3RECOMP_VK_DRAW_BATCH_MIN="0", PS3RECOMP_VK_COALESCE_PASSES="1")
        m = re.search(r"render passes: (\d+) batched draws in (\d+) passes, (\d+) clears recorded inside a pass", out)
        check(m is not None and int(m.group(1)) >= 300 and int(m.group(2)) * 10 <= int(m.group(1)),
              "100 draws into one framebuffer are a few passes, not one each "
              f"({m.group(0) if m else 'no pass line'})")
        rc, out = run(host, fresh("coal-clears"), "--draws=40:0:0:5", "--frames=3", PS3RECOMP_VK_DRAW_BATCH="1",
                      PS3RECOMP_VK_DRAW_BATCH_MIN="0", PS3RECOMP_VK_COALESCE_PASSES="1")
        m = re.search(r"render passes: (\d+) batched draws in (\d+) passes, (\d+) clears recorded inside a pass", out)
        check(m is not None and int(m.group(3)) > 0,
              f"a clear between draws of one framebuffer is recorded inside the pass ({m.group(0) if m else 'no pass line'})")
        for scene in scenes + (("--draws=200:16", "--frames=4"), ("--draws=40:0:0:5", "--frames=3"),
                               ("--draws=60:0:0:3", "--frames=3")):
            name = " ".join(scene)
            off = fresh("coal-off")
            run(host, off, *scene, PS3RECOMP_VK_DUMP=str(off / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9")
            check(len(dumps(off)) >= 2, f"{name}: batching off presents frames to compare: {dumps(off)}")
            for lo, hi in (("0", "1"), ("0", "2"), ("0", "512"), ("16", "64")):
                on = fresh("coal-on")
                rc, out = run(host, on, *scene, PS3RECOMP_VK_DUMP=str(on / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9",
                              PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_DRAW_BATCH_MIN=lo,
                              PS3RECOMP_VK_DRAW_BATCH_MAX=hi, PS3RECOMP_VK_COALESCE_PASSES="1")
                same = dumps(on) == dumps(off) and len(dumps(off)) >= 2 and all(
                    (on / n).read_bytes() == (off / n).read_bytes() for n in dumps(off))
                check(same and "VkResult" not in out,
                      f"{name}, coalescing on, kick from {lo} up to {hi}: frames are byte-identical to batching off")

        # ---- the bench's per-draw CPU cost (--draws=N:S:W) hands each draw to the walker at once and changes no pixel
        # of the steady frames (the first present of this host's frame loop depends on where the walker first meets
        # the ring, with or without a cost, so the last present is the one compared)
        for work in ("0", "20"):
            d = fresh("work")
            rc, out = run(host, d, "--draws=100:16:" + work, "--frames=4", PS3RECOMP_VK_DUMP=str(d / "f"),
                          PS3RECOMP_VK_DUMP_RANGE="0-9", PS3RECOMP_VK_DRAW_BATCH="1")
            ref = fresh("work-ref")
            run(host, ref, "--draws=100:16", "--frames=4", PS3RECOMP_VK_DUMP=str(ref / "f"),
                PS3RECOMP_VK_DUMP_RANGE="0-9", PS3RECOMP_VK_DRAW_BATCH="1")
            same = len(dumps(d)) >= 3 and len(dumps(ref)) >= 3 and \
                (d / dumps(d)[-1]).read_bytes() == (ref / dumps(ref)[-1]).read_bytes()
            check(same, f"--draws=100:16:{work}: the last frame is the one without a per-draw CPU cost")
    print(f"failed {FAILED}" if FAILED else "all checks passed")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
