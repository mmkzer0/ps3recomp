#!/usr/bin/env python3
"""Raw SPU start through the real spu_raw.c: SPU_RunCntl runs the program on a host thread of its own."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
env = {k: v for k, v in os.environ.items() if not k.startswith('SPU_')}
with tempfile.TemporaryDirectory(prefix='spu-raw-start-') as tmp:
    binary = Path(tmp) / 'test'
    san = ['-fsanitize=thread', '-g'] if os.environ.get('PS3RECOMP_SANITIZE_THREAD') else []
    # The fixture stubs ps3_config_get and ps3_stop itself (it changes spu.host_stack_mb and reads the stop line),
    # so it links the run log but not config.cpp or stop.cpp, which spu_fixture_link.build always adds.
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', *san,
                    '-I'+str(root/'include'), '-I'+str(spu), '-I'+str(root/'runtime/platform'),
                    str(spu/'tests/test_spu_raw_start.c'), str(root/'runtime/diag/ps3_log.c'),
                    '-pthread', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30, cwd=tmp, env=env,
                   stderr=subprocess.DEVNULL if os.environ.get('CI') else None)
