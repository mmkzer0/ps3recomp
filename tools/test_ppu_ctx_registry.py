#!/usr/bin/env python3
"""The loader's PPU thread-context registry and the watchdog report that walks it.

The region from the ppu_dump_guest_stack declaration through ppu_report_guest_lrs is cut out of
runtime/ppu/ppu_loader.cpp and compiled into tests/test_ppu_ctx_registry.cpp (the same way
tools/test_sys_gpio.py runs the sys_gpio cases), so the test runs the loader's own code. It checks that the
watchdog report unwinds each registered thread's guest stack and that a context is registered once."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: standalone loader-region fixture requires a POSIX toolchain')
        return 0
    src = (ROOT / 'runtime/ppu/ppu_loader.cpp').read_text()
    start = src.index('extern "C" void ppu_dump_guest_stack(ppu_context*, const char*);')
    fn = src.index('extern "C" void ppu_report_guest_lrs(void)', start)
    end = src.index('\n}\n', fn) + 3
    with tempfile.TemporaryDirectory(prefix='ps3-ctx-registry-') as directory:
        tmp = Path(directory)
        body = tmp / 'registry_body.inc'
        body.write_text(src[start:end])
        includes = ['-I' + str(p) for p in (ROOT / 'include', ROOT / 'runtime/ppu', ROOT / 'runtime/platform')]
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-UNDEBUG', *includes,
                        '-DREGISTRY_BODY="%s"' % body, str(ROOT / 'tests/test_ppu_ctx_registry.cpp'),
                        '-pthread', '-o', str(tmp / 'fixture')], check=True, timeout=120)
        env = {k: v for k, v in os.environ.items()
               if k in ('PATH', 'HOME', 'TMPDIR', 'SYSTEMROOT')}
        subprocess.run([str(tmp / 'fixture')], env=env, check=True, timeout=10)
    print('PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
