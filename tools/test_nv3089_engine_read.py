#!/usr/bin/env python3
"""NV3089 source origin, the engine write-back, and the 2D engines through a tile bound over main memory
(libs/video/tests/test_nv3089_engine_read.c, which compiles cellGcmSys.c in)."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the NV3089 fixture uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='nv3089-engine-read-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip', '-pthread'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_nv3089_engine_read.c'),
                    str(root / 'runtime/memory/label_window.c'),
                    str(root / 'libs/video/gcm_default_state.c'), *linker,
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], check=False, timeout=60, capture_output=True, text=True)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-2000:])
        raise SystemExit('test_nv3089_engine_read failed')
