#!/usr/bin/env python3
"""Build and run libs/system/tests/test_sys_crashdump.c (sys_crashdump).

Checks:
- sys_crash_dump_set_user_log_area (NID 0xB20F87B3)
- sys_crash_dump_get_user_log_area (NID 0x52696620)
dispatched via ps3_hle_call as a guest would see them.
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
with tempfile.TemporaryDirectory(prefix='sys-crashdump-') as tmp:
    tmp = Path(tmp)
    (tmp / 'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
    subprocess.run([sys.executable, str(root / 'tools/gen_hle_nids.py'), 'sys_crashdump',
                    '--out', str(tmp / 'nids.cpp')], check=True, timeout=30)
    (tmp / 'shim.cpp').write_text(
        '#include <cstdint>\n'
        '#include <cstring>\n'
        'extern "C" uint8_t* vm_base;\n'
        'extern "C" uint32_t vm_read32(uint64_t a) {\n'
        '    uint32_t v = 0;\n'
        '    std::memcpy(&v, vm_base + (uint32_t)a, 4);\n'
        '    return __builtin_bswap32(v);\n'
        '}\n'
        'extern "C" void vm_write32(uint64_t a, uint32_t v) {\n'
        '    v = __builtin_bswap32(v);\n'
        '    std::memcpy(vm_base + (uint32_t)a, &v, 4);\n'
        '}\n'
        'extern "C" uint64_t vm_read64(uint64_t a) {\n'
        '    uint64_t v = 0;\n'
        '    std::memcpy(&v, vm_base + (uint32_t)a, 8);\n'
        '    return __builtin_bswap64(v);\n'
        '}\n'
        'void vm_write64(uint64_t a, uint64_t v) {\n'
        '    v = __builtin_bswap64(v);\n'
        '    std::memcpy(vm_base + (uint32_t)a, &v, 8);\n'
        '}\n'
    )
    includes = ['-I' + str(root), '-I' + str(root / 'include'), '-I' + str(root / 'libs/system'),
                '-I' + str(root / 'runtime/ppu'), '-I' + str(tmp)]
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-Wall', '-Wextra',
                    '-ffunction-sections', '-fdata-sections', '-pthread', *includes, '-c',
                    str(root / 'libs/system/tests/test_sys_crashdump.c'),
                    '-o', str(tmp / 'test.o')], check=True, timeout=120)
    binary = tmp / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections', '-pthread', *includes,
                    str(tmp / 'test.o'), str(root / 'runtime/ppu/ppu_hle.cpp'),
                    *loader_support_sources(), str(tmp / 'nids.cpp'), str(tmp / 'shim.cpp'),
                    *linker, '-o', str(binary)], check=True, timeout=300)
    subprocess.run([str(binary)], check=True, timeout=60)
