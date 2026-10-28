#!/usr/bin/env python3
"""A guest-null-access stop names the site that made the access.

Builds the real ppu_loader.cpp and ppu_hle.cpp against a lift of one trivial function and runs
tests/test_stop_attribution.cpp:

  - a host-side handler that loads or stores at guest address 0 (an lv2 syscall, an HLE import from the
    context table, one from the generic table) is `site=host-handler`: the stop names `in_syscall=<num>` or
    `in_hle=<name>/<nid>`, carries the guest call site as pc and lr, and has no `fn`. Before, `fn` was the
    nearest lifted function below the host return address: unrelated code (#83);
  - the same on a thread with no active guest context;
  - a guest callback that a handler runs (ppu_guest_call, a context of its own) is the guest's own access:
    `site=guest` with its `fn`, and no handler named, even though the handler is still on the stack;
  - a guest function faulting after a handler has returned is `site=guest`: the handler is not left behind.
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import json

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests' / 'gates'))
from check_ppu_scaffold import lifter_abi_sources, loader_support_objects, loader_support_sources  # noqa: E402
import gates_common as g  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CODE = struct.pack('>II', 0x38600005, 0x4E800020)   # li r3,5 ; blr
CALL_SITE = '0x10100'                               # the lr the fixture's context carries
GUEST_FN = '0x00030060'                             # the fixture's registered guest function


def main():
    if os.name == 'nt':
        print('SKIP: POSIX fixture')
        return 0
    with tempfile.TemporaryDirectory(prefix='stop-attribution-') as tmp:
        tmp = Path(tmp)
        (tmp / 'code.bin').write_bytes(CODE)
        (tmp / 'fn.json').write_text(json.dumps([{'start': '0x10000', 'end': '0x10008'}]))
        lift = tmp / 'lift'
        subprocess.run([sys.executable, str(ROOT / 'tools/ppu_lifter.py'), str(tmp / 'code.bin'), '--raw',
                        '--base', '0x10000', '--functions', str(tmp / 'fn.json'), '--jobs', '1',
                        '--code-end', '0x10008', '--single-file', '--source-name', 'ppu_recomp.cpp',
                        '--output', str(lift)], check=True, capture_output=True)
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
        binary = tmp / 'attribution'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-w', '-ffunction-sections',
                        '-fdata-sections', '-I' + str(lift), '-I' + str(ROOT / 'include'),
                        '-I' + str(ROOT / 'runtime/ppu'), '-I' + str(ROOT / 'runtime/platform'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'), str(ROOT / 'runtime/ppu/ppu_hle.cpp'),
                        str(ROOT / 'tests/test_stop_attribution.cpp'), str(ROOT / 'runtime/syscalls/sys_lwsync.cpp'),
                        str(lift / 'ppu_recomp.cpp'), *loader_support_sources(), *lifter_abi_sources(),
                        *loader_support_objects(tmp), *flags, '-o', str(binary)],
                       check=True, timeout=300)

        def stop(*args):
            p = g.run([str(binary), *args], env=g.clean_env())
            assert 'RETURNED' not in p.stdout, (args, p.stdout, p.stderr)
            return g.expect_stop(p, 'guest-null-access'), p

        def host(f, p, args):
            """What every host-handler stop shares."""
            assert f['site'] == 'host-handler', (args, f)
            assert 'fn' not in f, (args, f)          # no function resolved from a host return address
            assert f['pc'] == CALL_SITE and f['lr'] == CALL_SITE and f['tid'] == '1', (args, f)
            assert f['ea'] == '0x00000000', (args, f)

        def guest(f, args):
            assert f['site'] == 'guest' and f['fn'] == GUEST_FN, (args, f)
            assert 'in_syscall' not in f and 'in_hle' not in f, (args, f)

        # a host syscall handler's load and store
        for num, access, width in (('1000', 'read', '4'), ('1001', 'write', '4')):
            for mode in ('syscall', 'syscall-noctx'):
                f, p = stop(mode, num)
                host(f, p, (mode, num))
                assert (f['access'], f['width'], f['in_syscall']) == (access, width, num), f
                assert 'in_hle' not in f, f
                assert f['syscall'] == num, f       # the frontier agrees

        # an HLE import handler, from the context table and the generic table
        for nid, name in (('0x5A000001', 'fixtureCtxLoad'), ('0x5A000002', 'fixtureTableLoad')):
            f, p = stop('hle', nid)
            host(f, p, ('hle', nid))
            assert f['in_hle'] == '%s/0x%s' % (name, nid[2:]), f
            assert f['access'] == 'read' and 'in_syscall' not in f, f

        # a guest callback run by a handler is the guest's own access, not the handler's
        for args in (('syscall', '1002'), ('hle', '0x5A000003'), ('hle', '0x5A000004')):
            f, p = stop(*args)
            guest(f, args)
            assert f['access'] == 'read' and f['width'] == '4' and f['ea'] == '0x00000000', f

        # a guest function on the thread's own context
        f, p = stop('guest')
        guest(f, 'guest')
        assert f['lr'] == CALL_SITE, f

        # once a handler returns, it is not left behind as the site of a later guest fault
        for args in (('after-syscall', '1003'), ('after-hle', '0x5A000005')):
            f, p = stop(*args)
            guest(f, args)
    print('PASS: a host-side handler\'s null-page access names the handler and its guest call site; '
          'a guest callback or a later guest access keeps its function')
    return 0


if __name__ == '__main__':
    sys.exit(main())
