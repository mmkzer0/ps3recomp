#!/usr/bin/env python3
"""A raw-SPU image registered with its parameter area masked out is found whatever that area holds."""
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
with tempfile.TemporaryDirectory(prefix='spu-workload-masked-') as tmp:
    binary = Path(tmp) / 'test'
    # spu_workload.c raises named stops (spurs-task-halt, spurs-dispatch-miss),
    # so the fixture links the named-stop and typed-configuration runtime.
    spu_fixture_link.build([root / 'runtime/spu/tests/test_spu_workload_masked.c'], binary,
                           ['-std=gnu17', '-O1', '-UNDEBUG',
                            '-I' + str(root / 'include'), '-I' + str(root / 'runtime'),
                            '-I' + str(root / 'runtime/spu')], tmp, runtime_c=False)
    subprocess.run([str(binary)], check=True, timeout=30, cwd=tmp)
