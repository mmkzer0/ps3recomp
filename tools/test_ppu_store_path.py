#!/usr/bin/env python3
"""ppu_loader.cpp's guest store family does the same on its short path and on its full one.

Builds the real runtime/ppu/ppu_loader.cpp with runtime/ppu/tests/test_ppu_store_path.cpp. While no write
watch is armed (ppu_store_diag, runtime/ppu/ppu_store_diag.h) vm_write8/16/32/64 skip the write watches; the
fixture checks that what a store does is the same either way (bytes, reservation lock and notify, raw SPU
register effect, bounds, the store window's hook), and the probes below check that every watch that can arm
itself still sees a store: PPU_WWATCH and PPU_WVAL read from the environment on the first store, and the
sync-line probe (g_barrier_sync_watch) written at run time. A store into the null page still stops by name.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from check_ppu_scaffold import lifter_abi_sources, loader_support_objects, loader_support_sources, stub_header  # noqa: E402

ROOT = TOOLS.parent


def run(binary, mode, extra_env=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_'))}
    env.update(extra_env or {})
    return subprocess.run([str(binary), mode], env=env, capture_output=True, text=True, timeout=60)


def main():
    if os.name == 'nt':
        print('SKIP: fixture uses POSIX mmap')
        return
    with tempfile.TemporaryDirectory(prefix='ppu-store-path-') as tmp:
        tmp = Path(tmp)
        (tmp / 'ppu_recomp.h').write_text(stub_header())
        binary = tmp / 'test'
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
        san = ['-fsanitize=thread', '-g'] if os.environ.get('PS3RECOMP_SANITIZE_THREAD') else []
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-w', *san,
                        '-ffunction-sections', '-fdata-sections',
                        '-I' + str(tmp), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/platform'),
                        '-I' + str(ROOT / 'runtime/memory'), '-I' + str(ROOT / 'runtime/spu'),
                        '-I' + str(ROOT / 'runtime/ppu'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'),
                        str(ROOT / 'runtime/ppu/tests/test_ppu_store_path.cpp'),
                        *loader_support_sources(), *lifter_abi_sources(), *loader_support_objects(tmp),
                        *flags, '-o', str(binary)], check=True)

        r = run(binary, 'all')
        print(r.stdout, end='')
        if r.returncode:
            sys.stderr.write(r.stderr[-2000:])
            raise SystemExit('FAIL: the guest store path')

        def probe(mode, env, needle, present, why):
            r = run(binary, mode, env)
            seen = needle in r.stderr
            if r.returncode or seen != present:
                sys.stderr.write(r.stderr[-1500:])
                raise SystemExit(f'FAIL: {why} (exit {r.returncode}, {needle!r} {"missing" if present else "present"})')
            print(f'[PASS] {why}')

        probe('quiet', {}, '[ww]', False, 'no watch armed: a store prints no [ww]')
        probe('quiet', {}, '[wv]', False, 'no watch armed: a store prints no [wv]')
        probe('quiet', {}, '[sync-write]', False, 'no watch armed: a store prints no [sync-write]')
        probe('ww', {'PPU_WWATCH': '20010'}, '[ww] 0x00020010 <- 0x55 (w4)', True,
              'PPU_WWATCH set: the first store is reported')
        probe('wval', {'PPU_WVAL': '0x1234'}, '[wv] 0x00020300 <- 0x1234 (w4)', True,
              'PPU_WVAL set: the first store of that value is reported')
        probe('sync', {}, '[sync-write] +0x40 <- 0x1 (w4)', True,
              'the sync-line probe armed at run time sees a store in its line')
        r = run(binary, 'null')
        if r.returncode == 0 or 'guest-null-access' not in r.stderr + r.stdout:
            sys.stderr.write((r.stdout + r.stderr)[-1500:])
            raise SystemExit(f'FAIL: a store into the null page did not stop by name (exit {r.returncode})')
        print('[PASS] a store into the null page stops with guest-null-access')
        r = run(binary, 'sweep', {'PS3RECOMP_PPU_NULL_PAGE': 'drop'})
        if r.returncode or 'sweep: 00 00' not in r.stdout or '[null-write]' not in r.stderr:
            sys.stderr.write((r.stdout + r.stderr)[-1500:])
            raise SystemExit(f'FAIL: ppu.null_page=drop did not drop the null-page store and its sweep (exit {r.returncode})')
        print('[PASS] ppu.null_page=drop drops a null-page store and the sweep that follows it out of the page')
        print('PASS: the guest store path: the short path and the full one agree; every armed diagnostic still sees a store')


if __name__ == '__main__':
    main()
