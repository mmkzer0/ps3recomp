#!/usr/bin/env python3
"""HLE typed-call adapter contract (runtime/ppu/tests/test_hle.cpp, A6).

Builds the real ppu_hle.cpp and libs/misc/cellSheap.c with the fixture and
runs two scenarios:

  * in-process: s32/u32/u64/bool/void/double returns, positional marshalling,
    the 9th/10th arguments from the parameter save area, the guest-EA pointer
    return opt-in, variadic forwarding, and cellSheapAllocate's guest EA;
  * `hostptr`: a handler whose declared return is a host pointer must stop the
    process with kind=hle-unsupported-return (exit 70), never return.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_ppu_scaffold import loader_support_sources  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: POSIX fixture')
        return 0
    with tempfile.TemporaryDirectory(prefix='hle-abi-') as name:
        tmp = Path(name)
        (tmp / 'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
        # No generated table here: the fixture includes only runtime/ppu and
        # include headers, and cellSheap.c's own include path.
        includes = ['-I' + str(ROOT / 'include'),
                    '-I' + str(ROOT / 'runtime/ppu'), '-I' + str(tmp)]
        sheap_o = tmp / 'cellSheap.o'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                        *includes, '-c', str(ROOT / 'libs/misc/cellSheap.c'),
                        '-o', str(sheap_o)], check=True, timeout=120)
        binary = tmp / 'fixture'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-UNDEBUG',
                        '-Wall', '-Wextra', *includes,
                        str(ROOT / 'runtime/ppu/tests/test_hle.cpp'),
                        str(ROOT / 'runtime/ppu/ppu_hle.cpp'),
                        *loader_support_sources(), str(sheap_o),
                        '-pthread', '-o', str(binary)], check=True, timeout=300)

        p = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        assert p.returncode == 0, (p.stdout, p.stderr)
        assert 'Results: OK' in p.stdout, p.stdout
        assert '0xFFFFFFFF8001000F' in p.stdout, p.stdout

        # Host-pointer return: fail closed, not a fabricated truncation.
        p = subprocess.run([str(binary), 'hostptr'], capture_output=True, text=True,
                           timeout=60)
        assert p.returncode == 70, (p.returncode, p.stdout, p.stderr)
        assert 'RETURNED' not in p.stdout, p.stdout
        line = [l for l in p.stderr.splitlines() if l.startswith('PS3STOP v=1')]
        assert len(line) == 1, p.stderr
        line = line[0]
        assert 'kind=hle-unsupported-return' in line, line
        assert 'nid=0x1000000A' in line, line
        assert 'name=mock_host_ptr' in line, line
        assert 'reason="returns a host pointer, not a guest EA"' in line, line
        assert 'lr=0x10100' in line and 'tid=1' in line, line
    print('PASS: typed HLE adapter (returns, positional args, save area, guest EA, variadic, host-ptr stop)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
