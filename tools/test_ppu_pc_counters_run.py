#!/usr/bin/env python3
"""--pc-counters end to end: lift a raw PPU loop, build it with the runtime's counter module, run it.

The snippet is a loop of r3 trips (r4 counts them):

    0x10000 li r4, 0
    0x10004 mtctr r3
    0x10008 addi r4, r4, 1        <- loop body
    0x1000C bdnz 0x10008          <- loop branch
    0x10010 blr

Counters sit on 0x10000 (before the loop), 0x10008 and 0x1000C (inside it). Checked:

1. the dump written at exit() and at ps3_host_exit() (quick_exit) holds, per slot, `0xPC count` with the
   loop PCs at the summed trip count and the PC before the loop at the number of calls;
2. the read API agrees with the dump, and an out-of-range slot or a short buffer is visible;
3. four threads running the loop at once lose no update (the increments are atomic);
4. a lift without counters links against the same runtime: no slots, no dump, even with the env set;
5. an unwritable PS3_PC_COUNTERS_OUT is reported on stderr with its path;
6. ps3_pc_counters_dump_now() writes the counts at the call (more trips run after it, the exit dump
   after it is a no-op), and does nothing without the env or without counters.

Run: python tools/test_ppu_pc_counters_run.py
"""
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from check_ppu_scaffold import host_exit_object  # noqa: E402

ROOT = TOOLS.parent
CODE = bytes.fromhex(
    "38800000"   # 0x10000 li    r4, 0
    "7c6903a6"   # 0x10004 mtctr r3
    "38840001"   # 0x10008 addi  r4, r4, 1
    "4200fffc"   # 0x1000C bdnz  0x10008
    "4e800020")  # 0x10010 blr
PCS = "# before the loop\n0x10000\n1000C  # the loop branch\n0x00010008\n"


def sh(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300, **kw)
    if r.returncode:
        sys.stderr.write(f"$ {' '.join(map(str, cmd))}\n{r.stdout[-1500:]}{r.stderr[-3000:]}")
        raise SystemExit("FAIL: command failed")
    return r


def lift(tmp, name, flags):
    out = tmp / name
    sh([sys.executable, str(TOOLS / "ppu_lifter.py"), str(tmp / "code.bin"), "--raw", "--base", "0x10000",
        "--functions", str(tmp / "functions.json"), "--output", str(out), *flags])
    return out


def build(tmp, lifted, name):
    cc, cxx = os.environ.get("CC", "cc"), os.environ.get("CXX", "c++")
    pcobj = tmp / "pc_counters.o"
    if not pcobj.exists():
        sh([cc, "-std=gnu17", "-O1", "-I" + str(ROOT / "include"), "-c",
            str(ROOT / "runtime/diag/pc_counters.c"), "-o", str(pcobj)])
    binary = tmp / name
    flags = [] if sys.platform == "darwin" else ["-pthread"]
    sh([cxx, "-std=c++17", "-O1", "-w", *flags, "-I" + str(lifted), "-I" + str(ROOT / "include"),
        "-I" + str(ROOT / "runtime/ppu"), "-I" + str(ROOT / "runtime/memory"),
        "-I" + str(ROOT / "runtime/platform"),
        *sorted(map(str, lifted.glob("ppu_recomp_*.cpp"))),
        str(ROOT / "runtime/ppu/tests/test_pc_counters.cpp"), str(pcobj), host_exit_object(tmp),
        "-o", str(binary)])
    return binary


def run(binary, *args, out=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("PS3_")}
    if out is not None:
        env["PS3_PC_COUNTERS_OUT"] = str(out)
    return subprocess.run([str(binary), *map(str, args)], env=env, capture_output=True, text=True, timeout=60)


def parse_dump(path):
    lines = Path(path).read_text().splitlines()
    m = re.fullmatch(r"# ps3 pc counters N=(\d+)", lines[0])
    assert m, lines
    rows = [re.fullmatch(r"0x([0-9A-F]{8}) (\d+)", ln) for ln in lines[1:]]
    assert all(rows), lines
    assert int(m.group(1)) == len(rows), lines
    return [(int(r.group(1), 16), int(r.group(2))) for r in rows]


def stdout_map(text):
    return dict(ln.split("=", 1) for ln in text.splitlines() if "=" in ln and " " not in ln.split("=", 1)[0])


def main():
    if os.name == "nt":
        print("SKIP: fixture uses pthreads")
        return
    with tempfile.TemporaryDirectory(prefix="pc-counters-") as t:
        tmp = Path(t)
        (tmp / "code.bin").write_bytes(CODE)
        (tmp / "functions.json").write_text(json.dumps([{"start": "0x10000", "end": "0x10014"}]))
        (tmp / "pcs.txt").write_text(PCS)
        counted = build(tmp, lift(tmp, "lift_counted", ["--pc-counters", str(tmp / "pcs.txt")]), "counted")
        plain = build(tmp, lift(tmp, "lift_plain", []), "plain")

        # 1. exit() and quick_exit() both dump; trips 7 + 3 = 10 loop executions over 2 calls.
        for how in ("exit", "quick"):
            dump = tmp / f"dump_{how}.txt"
            r = run(counted, how, 1, 7, 3, out=dump)
            assert r.returncode == 0, (how, r.stdout, r.stderr)
            assert parse_dump(dump) == [(0x10000, 2), (0x10008, 10), (0x1000C, 10)], (how, dump.read_text())
            assert r.stderr == "", r.stderr
            print(f"[PASS] the {how} path writes `0xPC count` per slot: loop PCs 10, the PC before the loop 2")

        # 2. the read API.
        r = run(counted, "exit", 1, 7, 3)
        got = stdout_map(r.stdout)
        assert r.returncode == 0 and got["slots"] == "3" and got["r4"] == "3", r.stdout
        assert "slot 0 pc=0x00010000 count=2" in r.stdout and "slot 1 pc=0x00010008 count=10" in r.stdout \
            and "slot 2 pc=0x0001000C count=10" in r.stdout, r.stdout
        assert got["read_total"] == "3" and got["read_short_total"] == "3", r.stdout
        assert got["pc_oob"] == "0xFFFFFFFF", r.stdout
        print("[PASS] count/pc/read report the slots; an out-of-range slot reads 0xFFFFFFFF, a short buffer reports the size")

        # 3. atomic under contention: 4 threads x (1000 + 500) trips.
        dump = tmp / "dump_threads.txt"
        r = run(counted, "quick", 4, 1000, 500, out=dump)
        assert r.returncode == 0, r.stderr
        assert parse_dump(dump) == [(0x10000, 8), (0x10008, 6000), (0x1000C, 6000)], dump.read_text()
        print("[PASS] four threads lose no update (8 calls, 6000 loop executions)")

        # 4. no counters in the lift: empty table, nothing written.
        dump = tmp / "dump_plain.txt"
        r = run(plain, "exit", 1, 5, out=dump)
        got = stdout_map(r.stdout)
        assert r.returncode == 0 and got["slots"] == "0" and got["read_total"] == "0" \
            and got["pc_oob"] == "0xFFFFFFFF", (r.stdout, r.stderr)
        assert not dump.exists(), "a lift without counters wrote a dump"
        print("[PASS] a lift without counters links, reports 0 slots and writes no dump")

        # 5. an unwritable dump path is named on stderr.
        bad = tmp / "no_such_dir" / "dump.txt"
        for how in ("exit", "quick"):
            r = run(counted, how, 1, 2, out=bad)
            assert f"PS3_PC_COUNTERS_OUT={bad}" in r.stderr and "cannot open for writing" in r.stderr, \
                (how, r.stderr)
        print("[PASS] an unwritable PS3_PC_COUNTERS_OUT is reported on stderr with its path")

        # 6. dump_now: the file holds the counts at the first call; the exit dump after it is a no-op,
        # and so is a second dump_now. The process keeps counting (the printed counts are doubled).
        for how in ("dumpnow-exit", "dumpnow-quick"):
            dump = tmp / f"dump_{how}.txt"
            r = run(counted, how, 1, 7, 3, out=dump)
            assert r.returncode == 0 and r.stderr == "", (how, r.stdout, r.stderr)
            assert parse_dump(dump) == [(0x10000, 2), (0x10008, 10), (0x1000C, 10)], (how, dump.read_text())
            assert "slot 0 pc=0x00010000 count=4" in r.stdout and "slot 1 pc=0x00010008 count=20" in r.stdout, \
                r.stdout
            print(f"[PASS] dump_now ({how}) writes the counts at the call; the exit dump after it changes nothing")
        r = run(counted, "dumpnow-exit", 1, 7, 3)
        assert r.returncode == 0 and r.stderr == "", (r.stdout, r.stderr)
        r = run(plain, "dumpnow-quick", 1, 5, out=tmp / "dump_plain_now.txt")
        assert r.returncode == 0 and r.stderr == "" and not (tmp / "dump_plain_now.txt").exists(), \
            (r.stdout, r.stderr)
        r = run(counted, "dumpnow-exit", 1, 2, out=bad)
        assert r.stderr.count(f"PS3_PC_COUNTERS_OUT={bad}") == 1, r.stderr
        print("[PASS] dump_now does nothing without the env or without counters, and reports an unwritable path once")
    print("PASS: --pc-counters counts executions per PC and dumps them on both exit routes")


if __name__ == "__main__":
    main()
