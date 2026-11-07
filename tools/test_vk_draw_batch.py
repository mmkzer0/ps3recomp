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
  batching on against off            the presented frames are byte-identical (what the dump is for).

The scene's own verdict (its exit code) is not looked at: it checks the draw path's pixels, not these.
Exit 77: no usable Vulkan device or the draw engine path is not built (needs glslang), nothing to run."""
import os
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


def run(host, workdir, **env):
    """One `--draw` scene run; returns (exit code, combined output)."""
    e = {k: v for k, v in os.environ.items()
         if not k.startswith("PS3RECOMP_") and k not in ("VK_DRAW_BATCH", "VK_DRAW_BATCH_MAX")}
    e["PS3RECOMP_VK_GUEST_PROGRAMS"] = "1"
    e.update(env)
    p = subprocess.run([host, "--draw"], check=False, cwd=workdir, env=e, capture_output=True, text=True,
                       timeout=120)
    return p.returncode, p.stdout + p.stderr


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
        rc, out = run(host, fresh("probe"))
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

        rc, out = run(host, fresh("big"), PS3RECOMP_VK_DRAW_BATCH="1", PS3RECOMP_VK_DRAW_BATCH_MAX="1025")
        check(rc != 0 and "kind=config-invalid" in out and "key=vk.draw_batch_max" in out and "draw batching" not in out,
              "a maximum above the descriptor pool is a config-invalid stop, not a silent cap")

        # ---- the dump --------------------------------------------------------------------------------
        d = fresh("range")
        rc, out = run(host, d, PS3RECOMP_VK_DUMP=str(d / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9")
        check(dumps(d) == ["f_s00000_fnone.ppm", "f_s00001_fnone.ppm"], f"RANGE 0-9 writes the scene's two presents: {dumps(d)}")

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
            rc, out = run(host, d, PS3RECOMP_VK_DUMP=str(d / "f"), **{env: value})
            check(rc != 0 and "kind=config-invalid" in out and f"env={key}" in out and dumps(d) == [],
                  f"{env}={value[:24]!r} is a config-invalid stop naming it, and nothing is written (rc {rc})")
        d = fresh("long")
        rc, out = run(host, d, PS3RECOMP_VK_DUMP=str(d / "f"),
                      PS3RECOMP_VK_DUMP_LIST=",".join(str(i) for i in range(33)))
        check("count=33" in out and "max=32" in out, "an over-long list names its count and the limit")

        d = fresh("selectoff")
        rc, out = run(host, d, PS3RECOMP_VK_DUMP_LIST="abc")
        check("kind=config-invalid" not in out, "LIST without a dump base is not looked at")

        # ---- batching on against off: the same frames ------------------------------------------------
        a, b = fresh("frames-off"), fresh("frames-on")
        run(host, a, PS3RECOMP_VK_DUMP=str(a / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9")
        run(host, b, PS3RECOMP_VK_DUMP=str(b / "f"), PS3RECOMP_VK_DUMP_RANGE="0-9", PS3RECOMP_VK_DRAW_BATCH="1")
        check(dumps(a) == dumps(b) and len(dumps(a)) == 2, f"the same presents were written: {dumps(a)} / {dumps(b)}")
        for name in dumps(a):
            same = (a / name).read_bytes() == (b / name).read_bytes()
            check(same and (a / name).stat().st_size > 1000, f"{name}: batching on is byte-identical to off")
    print(f"failed {FAILED}" if FAILED else "all checks passed")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
