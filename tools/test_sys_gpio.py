#!/usr/bin/env python3
"""sys_gpio_set (630) / sys_gpio_get (631): the answers of the loader's real syscall cases.

The two case bodies are cut out of runtime/ppu/ppu_loader.cpp and compiled into tests/test_sys_gpio.cpp
against a small guest memory (the same way tools/test_loader_memory_domain.py runs cases 330/337), so the
test runs what lv2_syscall runs. It checks each answer RPCS3 gives (unknown device id CELL_ESRCH, get
writes 0, set on the LED CELL_OK, on the DIP switch CELL_EINVAL, no state) and that a value pointer whose
8-byte span leaves guest memory ends in the hle-guest-range stop."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: standalone loader-case fixture requires a POSIX toolchain')
        return 0
    src = (ROOT / 'runtime/ppu/ppu_loader.cpp').read_text()
    start = src.index('    case 630: {')
    end = src.index('    default: {', start)
    with tempfile.TemporaryDirectory(prefix='ps3-sys-gpio-') as directory:
        tmp = Path(directory)
        body = tmp / 'gpio_body.inc'
        body.write_text(src[start:end])
        includes = ['-I' + str(p) for p in (ROOT / 'include', ROOT / 'runtime/ppu', ROOT / 'tests')]
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-UNDEBUG', *includes,
                        '-DGPIO_BODY="%s"' % body, str(ROOT / 'tests/test_sys_gpio.cpp'),
                        '-o', str(tmp / 'fixture')], check=True, timeout=120)
        env = {k: v for k, v in os.environ.items()
               if k in ('PATH', 'HOME', 'TMPDIR', 'SYSTEMROOT')}
        subprocess.run([str(tmp / 'fixture')], env=env, check=True, timeout=10)
    print('PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
