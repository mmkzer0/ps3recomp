#!/usr/bin/env python3
"""The SPU_SMC_WATCH gate is race-free when two workers make their first local-store
write together (runtime/spu/spu_context.h, g_spu_smc_watch).

Builds runtime/spu/tests/test_spu_smc_gate.c with -fsanitize=thread and runs it
twice: SPU_SMC_WATCH unset (the gate settles at 0) and set (it settles at 1 and
the probe logs the write). A race is a ThreadSanitizer report, exit status 66.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == "nt":
    print("SKIP: fixture currently requires a POSIX C compiler")
    sys.exit(0)

root = Path(__file__).resolve().parents[1]
spu = root / "runtime" / "spu"
base_env = {k: v for k, v in os.environ.items()
            if not k.startswith(("SPU_", "SPURS_", "YDKJ_", "LBP_", "GH3_", "TSAN_"))}

with tempfile.TemporaryDirectory(prefix="spu-smc-gate-") as tmp:
    binary = Path(tmp) / "test_spu_smc_gate"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-g", "-UNDEBUG",
                    "-fsanitize=thread", "-I" + str(root / "include"), "-I" + str(spu),
                    str(spu / "tests" / "test_spu_smc_gate.c"), "-pthread", "-o", str(binary)],
                   check=True)
    for mode, extra in (("off", {}),
                        ("on", {"SPU_SMC_WATCH": "5", "SPU_SMC_LO": "0x1000", "SPU_SMC_HI": "0x1100"})):
        env = dict(base_env, **extra)
        # several runs: the first writes race in a different order each time
        for _ in range(5):
            p = subprocess.run([str(binary), mode], env=env, cwd=tmp, timeout=60,
                               capture_output=True, text=True)
            assert p.returncode == 0 and f"PASS: smc gate {mode}" in p.stdout, (
                mode, p.returncode, p.stdout, p.stderr[-3000:])
            assert "ThreadSanitizer" not in p.stderr, p.stderr[-3000:]
        if mode == "on":
            assert "WROTE CODE" in p.stderr, p.stderr[-1500:]
print("PASS: the SPU_SMC_WATCH gate under two concurrent first writes, unset and set")
