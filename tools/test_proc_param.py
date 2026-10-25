#!/usr/bin/env python3
"""ppu_load_elf sets the SDK version sys_process_get_sdk_version reports from PROC_PARAM.

The title's sys_process_param_t (PT 0x60000001: size, magic 0x13BCC5F6, version, sdk_version, ...) is
read by the production loader; an ELF without the segment, or with a malformed one, leaves the value
alone. Manufactured ELFs only: each is loaded by the real ppu_load_elf in its own process, so the
sentinel the fixture plants before loading shows whether the loader touched it.
"""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from check_ppu_scaffold import lifter_abi_sources, loader_support_objects, loader_support_sources, stub_header

ROOT = Path(__file__).resolve().parents[1]
SENTINEL = 0xDEADBEEF
MAGIC = 0x13BCC5F6
PT_LOAD, PT_PROC_PARAM = 1, 0x60000001


def proc_param(sdk, magic=MAGIC, size=0x24):
    return struct.pack('>8I', size, magic, 0x330000, sdk, 0x3E9, 0x100000, 0x100000, 0)[:size if size < 32 else 32] + bytes(max(0, size - 32))


def image(segments):
    """segments: [(p_type, vaddr, data)] -- one PT_LOAD for the code, then whatever else."""
    out = bytearray(0x1000)
    out[:16] = b'\x7fELF\x02\x02\x01' + bytes(9)
    struct.pack_into('>HHIQQQIHHHHHH', out, 16, 2, 21, 1, 0x10000, 64, 0, 0, 64, 56, len(segments), 0, 0, 0)
    for i, (ptype, addr, data) in enumerate(segments):
        struct.pack_into('>IIQQQQQQ', out, 64 + i * 56, ptype, 6, len(out), addr, addr, len(data), len(data), 0x1000)
        out.extend(data)
    return out


def main():
    if os.name == 'nt':
        print('SKIP: standalone loader fixture currently needs POSIX mmap/toolchain')
        return
    code = (PT_LOAD, 0x10000, b'\x60\x00\x00\x00' * 16)
    cases = {
        'odin-like sdk 4.75': ([code, (PT_PROC_PARAM, 0xB0D2E0, proc_param(0x00475001))], 0x00475001),
        'other title sdk 3.30': ([code, (PT_PROC_PARAM, 0xB0D2E0, proc_param(0x00330001))], 0x00330001),
        'sdk 0x280000, the library threshold': ([code, (PT_PROC_PARAM, 0xB0D2E0, proc_param(0x00280000))], 0x00280000),
        'no PROC_PARAM': ([code], SENTINEL),
        'bad magic': ([code, (PT_PROC_PARAM, 0xB0D2E0, proc_param(0x00475001, magic=0x12345678))], SENTINEL),
        'too short': ([code, (PT_PROC_PARAM, 0xB0D2E0, proc_param(0x00475001, size=8))], SENTINEL),
    }
    with tempfile.TemporaryDirectory(prefix='proc-param-') as tmp:
        tmp = Path(tmp)
        (tmp / 'ppu_recomp.h').write_text(stub_header())
        obj = tmp / 'memory.o'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-ffunction-sections', '-fdata-sections',
                        '-c', str(ROOT / 'runtime/syscalls/sys_memory.c'), '-o', str(obj)], check=True, timeout=60)
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
        binary = tmp / 'fixture'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-ffunction-sections', '-fdata-sections',
                        '-I' + str(tmp), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/platform'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'), str(ROOT / 'tests/test_proc_param.cpp'),
                        *loader_support_sources(), *lifter_abi_sources(), *loader_support_objects(tmp), str(obj), *flags,
                        '-o', str(binary)], check=True, timeout=120)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_'))}
        for i, (name, (segments, want)) in enumerate(cases.items()):
            elf = tmp / f'case{i}.elf'
            elf.write_bytes(image(segments))
            sha = hashlib.sha256(elf.read_bytes()).hexdigest()   # not the runtime's own SHA-256
            r = subprocess.run([str(binary), str(elf), sha, f'{want:X}'], env=env, capture_output=True, text=True, timeout=20)
            if r.returncode != 0:
                sys.exit(f'FAIL {name}: {r.stdout}{r.stderr}')
            print(f'PASS {name}: {r.stdout.strip()}')


if __name__ == '__main__':
    main()
