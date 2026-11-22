#!/usr/bin/env python3
"""Synthetic PPU cellFsUnlink host deletion test; no game files required."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runtime_link  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    compiler = shutil.which(os.environ.get('CXX', 'c++')) or shutil.which('clang++')
    if not compiler:
        print('No C++ compiler found')
        return 1 if os.environ.get('CI') else 0
    msvc = Path(compiler).stem.lower() in ('clang-cl', 'cl')
    with tempfile.TemporaryDirectory(prefix='ppu-fs-unlink-') as temp:
        work = Path(temp)
        (work / 'ppu_recomp.h').write_text('#include "runtime/ppu/ppu_context.h"\n')
        includes = [work, ROOT, ROOT / 'include', ROOT / 'runtime/ppu']
        binary = work / ('fixture.exe' if os.name == 'nt' else 'fixture')
        fixture = ROOT / 'runtime/ppu/tests/test_fs_unlink.cpp'
        # host_path maps /dev_flash through the shared C helper.
        dev_flash = ROOT / 'runtime/platform/ps3_dev_flash.c'
        # ppu_fs.cpp logs, reads the typed configuration and raises named stops (ps3_stop_guest_range).
        if msvc:
            cflags = ['/nologo', '/std:c17', '/O2', '/Gy', '/Gw', '/MT', '/W0',
                      '/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_DEPRECATE']
            cflags += ['/I' + str(path) for path in includes]
            # config.cpp needs C++20, so the fixture is built as C++20 here as well: one CRT, one set of flags.
            cxxflags = [*cflags[:1], '/std:c++20', '/EHsc', *cflags[2:]]
            ldflags = ['/OPT:REF']
        else:
            cflags = ['-O2', '-w'] + ['-I' + str(path) for path in includes]
            cxxflags = [*cflags, '-std=c++17']
            ldflags = []
        try:
            # The fixture defines ps3_stop_guest_range itself: the log and the configuration, not stop.cpp.
            runtime_link.build([fixture, dev_flash], binary, work, cflags=cflags, cxxflags=cxxflags,
                               ldflags=ldflags, cxx=compiler, timeout=180, capture_output=True,
                               runtime=('log', 'config'))
        except subprocess.CalledProcessError as e:
            print(((e.stdout or '') + (e.stderr or ''))[-5000:])
            return e.returncode
        result = subprocess.run([str(binary), str(work / 'private')], timeout=10)
        if result.returncode:
            print('FAIL: filesystem regression exit', result.returncode)
        return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
