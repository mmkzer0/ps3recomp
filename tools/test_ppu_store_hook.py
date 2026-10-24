#!/usr/bin/env python3
"""ppu_loader.cpp's vm_write32 runs the registered store hook before a guest store into its window lands.

Builds the real runtime/ppu/ppu_loader.cpp with runtime/ppu/tests/test_ppu_store_hook.cpp: a 32-bit store
inside the window calls the hook with the address and value while memory still holds the old value; stores
outside it, narrower or wider stores, an empty window and a removed hook call nothing.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_ppu_scaffold import loader_support_objects, loader_support_sources, stub_header  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: fixture uses POSIX linking')
        return
    with tempfile.TemporaryDirectory(prefix='ppu-store-hook-') as tmp:
        tmp = Path(tmp)
        (tmp / 'ppu_recomp.h').write_text(stub_header())
        binary = tmp / 'test'
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-w',
                        '-ffunction-sections', '-fdata-sections',
                        '-I' + str(tmp), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/platform'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'),
                        str(ROOT / 'runtime/ppu/tests/test_ppu_store_hook.cpp'),
                        *loader_support_sources(), *loader_support_objects(tmp),
                        *flags, '-o', str(binary)], check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_'))}
        r = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=60)
        print(r.stdout, end='')
        if r.returncode:
            sys.stderr.write(r.stderr[-2000:])
            raise SystemExit('FAIL: the guest store hook')
        print('PASS: the guest store hook runs before a 32-bit store into its window')


if __name__ == '__main__':
    main()
