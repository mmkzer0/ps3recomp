#!/usr/bin/env python3
"""Build and run libs/system/tests/test_sys_spu_printf.c (_sys_spu_printf_* family, F6).

The fixture is linked against the real runtime/ppu/ppu_hle.cpp and a NID
registration unit generated from sys_spu_printf.c by tools/gen_hle_nids.py, so
the attach/detach results are checked as a title sees them: the 64-bit guest r3
after ps3_hle_call (sign-extended s32, not zero-extended).
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_ppu_scaffold import loader_support_sources  # noqa: E402

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: POSIX fixture')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='sys-spu-printf-') as tmp:
    tmp = Path(tmp)
    (tmp / 'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
    subprocess.run([sys.executable, str(root / 'tools/gen_hle_nids.py'), 'sys_spu_printf',
                    '--out', str(tmp / 'nids.cpp')], check=True, timeout=30)
    # ppu_hle.cpp declares vm_write64 with C++ linkage; the C fixture cannot define that symbol.
    (tmp / 'shim.cpp').write_text('#include <stdint.h>\n'
                                  'extern "C" void ps3_test_vm_write64(uint64_t, uint64_t);\n'
                                  'void vm_write64(uint64_t a, uint64_t x) { ps3_test_vm_write64(a, x); }\n')
    includes = ['-I' + str(root / 'include'), '-I' + str(root / 'libs/system'),
                '-I' + str(root / 'runtime/ppu'), '-I' + str(tmp)]
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-Wall', '-Wextra',
                    '-ffunction-sections', '-fdata-sections', '-pthread', *includes, '-c',
                    str(root / 'libs/system/tests/test_sys_spu_printf.c'),
                    '-o', str(tmp / 'test.o')], check=True, timeout=120)
    binary = tmp / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections', '-pthread', *includes,
                    str(tmp / 'test.o'), str(root / 'runtime/ppu/ppu_hle.cpp'),
                    *loader_support_sources(), str(tmp / 'nids.cpp'), str(tmp / 'shim.cpp'),
                    *linker, '-o', str(binary)], check=True, timeout=300)
    subprocess.run([str(binary)], check=True, timeout=60)
