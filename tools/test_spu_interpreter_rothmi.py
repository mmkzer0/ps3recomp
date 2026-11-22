#!/usr/bin/env python3
"""Run the real interpreter's halfword logical shift-immediate vector semantics and existing self-test."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runtime_link  # noqa: E402

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C compiler')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
env = {k: v for k, v in os.environ.items()
       if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-rothmi-') as tmp:
    for name in ['test_spu_interpreter_rothmi', 'spu_interp_selftest']:
        binary = Path(tmp) / name
        # spu_interp.c logs through ps3_log, which needs the typed configuration and the stop runtime.
        runtime_link.build([spu/'tests'/f'{name}.c', spu/'spu_interp.c'], binary, tmp,
                           cflags=['-std=gnu17', '-O1', '-UNDEBUG', '-I'+str(root/'include'), '-I'+str(spu)])
        subprocess.run([str(binary)], check=True, timeout=5, env=env, cwd=tmp)
