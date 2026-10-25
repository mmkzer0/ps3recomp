#!/usr/bin/env python3
"""Exercise production taskset requeue and ELF parsing with synthetic tagged ELFs.

A task the policy module cannot load (not an ELF; a null address under the tag bits) is the named stop
spurs-task-halt, one mode per process; every loadable one is dispatched with its tag bits preserved."""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C compiler')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tests' / 'gates'))
import gates_common as g  # noqa: E402
source = (root / 'runtime/spu/spu_workload.c').read_text()

def function(name):
    match = re.search(r'^.*\b' + re.escape(name) + r'\([^;\n]*\)\s*\{', source, re.M)
    if not match:
        raise AssertionError('Missing production function ' + name)
    brace = source.index('{', match.start())
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

functions = '\n'.join(function(name) for name in
                      ['spu_workload_fingerprint', 'rd_be16', 'rd_be32', 'spu_elf_image_size',
                       'spu_task_elf_halt_reason', 'ts_running', 'ts_lock', 'ts_unlock',
                       'spu_taskset_task_exited'])
tables = source[source.index('#define TS_TRACK '):
                source.index('static uint8_t* ts_running(')]
fixture = (root / 'runtime/spu/tests/test_spu_taskset_elf_flags.c').read_text()
fixture = fixture.replace('/* PRODUCTION_FUNCTIONS */', tables + functions)
env = {k: v for k, v in os.environ.items()
       if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-taskset-elf-flags-') as tmp:
    tmp = Path(tmp)
    c = tmp / 'test.c'
    binary = tmp / 'test'
    c.write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-I' + str(root / 'runtime'),
                    '-Wall', '-Wextra', '-I' + str(root / 'include'), '-c', str(c), '-o', str(tmp / 'test.o')], check=True)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-I' + str(root / 'include'),
                    '-c', str(root / 'runtime/diag/stop.cpp'), '-o', str(tmp / 'stop.o')], check=True)
    subprocess.run([os.environ.get('CXX', 'c++'), str(tmp / 'test.o'), str(tmp / 'stop.o'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5, env=env, cwd=tmp)
    for mode, reason, elf in (('bad-elf', 'not-an-spu-elf', '0x00030000'), ('null-elf', 'null-elf', '0x00000000'),
                              ('segment', 'segment-outside-task-area', '0x00030000'),
                              ('unmapped', 'elf-outside-guest-memory', '0x003FFFF0')):
        f = g.expect_stop(g.run([str(binary), mode], env=env, timeout=5), 'spurs-task-halt')
        assert f['api'] == 'taskset-restart' and f['taskset'] == '0x00010000' and f['task'] == '0' \
            and f['elf'] == elf and f['reason'] == reason and 'pc' not in f, (mode, f)
    f = g.expect_stop(g.run([str(binary), 'no-lifted'], env=env, timeout=5), 'spurs-dispatch-miss')
    assert f['api'] == 'taskset-restart' and f['taskset'] == '0x00010000' and f['task'] == '0' \
        and f['image'] == '0x00030000' and f['size'] == '0x64' and 'job' not in f, f
    print('PASS: an unloadable task is the named stop spurs-task-halt')
