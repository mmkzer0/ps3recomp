#!/usr/bin/env python3
"""Use the production mapper and exact HLE heap bodies in a synthetic VM."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
def main():
    if os.name == 'nt':
        print('SKIP: standalone mapper/HLE-heap fixture needs a POSIX toolchain')
        return 0
    source = (ROOT / 'libs/system/sysPrxForUser.c').read_text()
    begin = source.index('#define YZ_HEAP_BASE')
    end = source.index('/* The plain libc allocator sysPrxForUser exports', begin)
    body = source[begin:end]
    with tempfile.TemporaryDirectory(prefix='ps3-mmapper-hle-heap-') as tmp:
        temp = Path(tmp)
        (temp / 'heap-under-test.inc').write_text(body)
        executable = temp / 'fixture'
        cc = os.environ.get('CC', 'cc')
        cxx = os.environ.get('CXX', 'c++')
        objs = []
        for i, src in enumerate([ROOT / 'tests/test_mmapper_hle_heap.c',
                                 ROOT / 'runtime/syscalls/sys_memory.c',
                                 ROOT / 'runtime/syscalls/sys_vm.c',
                                 ROOT / 'runtime/diag/ps3_log.c']):
            obj = temp / f'c{i}.o'
            subprocess.run([cc, '-std=gnu17', '-O1', '-UNDEBUG', '-I', str(temp),
                            '-I', str(ROOT / 'include'), '-c', str(src), '-o', str(obj)],
                           check=True, timeout=60)
            objs.append(str(obj))
        # sys_memory.c's migrated diagnostics call ps3_log_*; config.cpp completes it, and the
        # fixture's guest_range_catch.h stands in for stop.cpp.
        for i, src in enumerate([ROOT / 'runtime/config/config.cpp']):
            obj = temp / f'cxx{i}.o'
            subprocess.run([cxx, '-std=c++20', '-O1', '-w', '-I', str(ROOT / 'include'), '-c',
                            str(src), '-o', str(obj)], check=True, timeout=60)
            objs.append(str(obj))
        subprocess.run([cxx, *objs, '-pthread', '-o', str(executable)], check=True, timeout=30)
        subprocess.run([str(executable)], check=True, timeout=15)
    return 0
if __name__ == '__main__':
    raise SystemExit(main())
