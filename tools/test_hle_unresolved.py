#!/usr/bin/env python3
"""An import nothing implements stops with its NID; only an allow-list entry lets it return.

Builds the real ppu_hle.cpp with tests/test_hle_unresolved.cpp. A line "RETURNED"
from an unlisted NID means the bridge answered for an import it does not have
(it used to return CELL_OK), and fails the test.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests' / 'gates'))
from check_ppu_scaffold import loader_support_sources  # noqa: E402
import gates_common as g  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: POSIX fixture')
        return 0
    with tempfile.TemporaryDirectory(prefix='hle-unresolved-') as tmp:
        tmp = Path(tmp)
        (tmp / 'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
        binary = tmp / 'fixture'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-w', '-I' + str(ROOT / 'include'),
                        '-I' + str(ROOT / 'runtime/ppu'), '-I' + str(tmp),
                        str(ROOT / 'tests/test_hle_unresolved.cpp'), str(ROOT / 'runtime/ppu/ppu_hle.cpp'),
                        *loader_support_sources(), '-pthread', '-o', str(binary)], check=True, timeout=300)

        def run(nid, cfg=None):
            env = g.clean_env(**({'PS3RECOMP_CONFIG': str(cfg)} if cfg else {}))
            return g.run([str(binary), nid], env=env)

        p = run('0xDEADBEEF')
        assert 'RETURNED' not in p.stdout, 'the bridge answered an import it does not have'
        f = g.expect_stop(p, 'unresolved-nid')
        assert f['nid'] == '0xDEADBEEF' and f['caller_lr'] == '0x00010100' and f['tid'] == '1', f

        cfg = tmp / 'title.toml'
        cfg.write_text('schema = 1\n[[hle.stub]]\nnid = 0xDEADBEEF\nret = 0x8002cb07\n'
                       'reason = "synthetic test gap"\n[[hle.stub]]\nnid = 0x1234\nret = 0\nreason = "zero is a choice too"\n')
        p = run('0xDEADBEEF', cfg)
        assert p.returncode == 0 and 'RETURNED r3=0xFFFFFFFF8002CB07' in p.stdout, (p.stdout, p.stderr)
        assert 'ALLOW-LISTED unresolved import 0xDEADBEEF' in p.stderr and 'synthetic test gap' in p.stderr
        assert (str(cfg) + ':2') in p.stderr                         # the config line
        p = run('0x1234', cfg)
        assert p.returncode == 0 and 'RETURNED r3=0x0' in p.stdout, p.stdout
        g.expect_stop(run('0xDEADBEE0', cfg), 'unresolved-nid')      # only the listed NID
    print('PASS: unresolved NIDs stop; allow-listed ones return exactly their configured value')
    return 0


if __name__ == '__main__':
    sys.exit(main())
