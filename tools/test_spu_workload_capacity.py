#!/usr/bin/env python3
"""The default SPU workload registry holds 4096 images: all of them are found again, the next is refused."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spu_fixture_link  # noqa: E402

if os.name == 'nt':
    print('SKIP: fixture requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='spu-workload-capacity-') as tmp:
    binary = Path(tmp) / 'test'
    # Linked as tools/test_spu_workload_masked.py: spu_workload.c raises named stops and logs.
    spu_fixture_link.build([root / 'runtime/spu/tests/test_spu_workload_capacity.c',
                            root / 'runtime/diag/ps3_log.c', root / 'runtime/diag/host_exit.c',
                            root / 'runtime/platform/win32_compat.c',
                            root / 'runtime/platform/host_clock.c',
                            root / 'runtime/platform/host_wait.c'], binary,
                           ['-std=gnu17', '-O1', '-UNDEBUG',
                            '-I' + str(root / 'include'), '-I' + str(root / 'runtime'),
                            '-I' + str(root / 'runtime/spu'), '-I' + str(root / 'runtime/platform')], tmp,
                           runtime_c=False)
    p = subprocess.run([str(binary)], check=True, timeout=30, cwd=tmp, capture_output=True, text=True)
    assert 'test_spu_workload_capacity: ok' in p.stdout, p.stdout + p.stderr
