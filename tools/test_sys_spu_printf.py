#!/usr/bin/env python3
"""Build and run libs/system/tests/test_sys_spu_printf.c (_sys_spu_printf_* family, F6)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: POSIX fixture')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='sys-spu-printf-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-Wall', '-Wextra',
                    '-ffunction-sections', '-fdata-sections', '-pthread',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/system'),
                    str(root / 'libs/system/tests/test_sys_spu_printf.c'),
                    *linker, '-o', str(binary)], check=True, timeout=120)
    subprocess.run([str(binary)], check=True, timeout=60)
