#!/usr/bin/env python3
"""The FIFO walker's run route (one step for a packet of 3D methods) with the draw engine on
(libs/video/tests/test_gcm_run_route.c, which compiles cellGcmSys.c in).

Each stream runs twice: as shipped, and with GCM_OBJDBG=1, a per-word diagnostic switch that keeps the
word-by-word route. The two must print the same -- the same words to the engine in the same order, the
same census, the same legacy-state words -- and every [CHECK] the fixture derives from the verdict
contract must hold. A stream with an unknown method must end in the unknown-method stop (exit status 70)
at that very word: the fixture's `expect stop` line names it, the stop line must agree."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM fixture uses POSIX linking')
    raise SystemExit(0)

# stream -> does it end in the unknown-method stop
STREAMS = {'mixed': False, 'unknown-mid': True, 'ni-unknown': True, 'overflow': True}


def run(binary, stream, switch, tmp):
    env = dict(os.environ)
    env.pop('GCM_OBJDBG', None)
    env.pop('RSX_TRACE', None)
    env['TMPDIR'] = str(tmp)            # the stop's FIFO window dump goes here, not into the user's /tmp
    if switch:
        env['GCM_OBJDBG'] = '1'
    r = subprocess.run([str(binary), stream], timeout=60, capture_output=True, text=True, env=env)
    # SET_OBJECT is logged under GCM_OBJDBG only, and the init banner is stderr noise
    out = [l for l in r.stdout.splitlines() if not l.startswith('[GCM-OBJ]')]
    return r.returncode, out, r.stderr


with tempfile.TemporaryDirectory(prefix='gcm-run-route-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_gcm_run_route.c'),
                    str(root / 'runtime/memory/label_window.c'),
                    str(root / 'runtime/diag/host_exit.c'),
                    str(root / 'runtime/diag/ps3_log.c'),
                    str(root / 'tests/ps3_log_mirror_stub.c'),
                    str(root / 'libs/video/gcm_default_state.c'), *linker,
                    '-o', str(binary)], check=True)
    for stream, stops in STREAMS.items():
        results = {}
        for switch in (False, True):
            code, out, err = run(binary, stream, switch, tmp)
            label = f"{stream} ({'word-by-word' if switch else 'run route'})"
            want = 70 if stops else 0
            if code != want:
                print('\n'.join(out[-12:]))
                print(err[-1500:])
                raise SystemExit(f'{label}: exit status {code}, want {want}')
            if any(l.startswith('[CHECK FAIL]') for l in out):
                print('\n'.join(l for l in out if l.startswith('[CHECK')))
                raise SystemExit(f'{label}: a contract check failed')
            if not any(l.startswith('[run_route] PASS') for l in out):
                raise SystemExit(f'{label}: no PASS line')
            if stops:
                expect = next(l for l in out if l.startswith('expect stop '))
                want_pos = dict(re.findall(r'(fifo|word)=(0x[0-9A-F]+)', expect))
                stop = next((l for l in out if 'rsx-unknown-method' in l), None)
                if stop is None:
                    raise SystemExit(f'{label}: the pass did not stop on the unknown method')
                got_pos = dict(re.findall(r'(fifo|word) (0x[0-9A-F]+)', stop))
                if got_pos != want_pos:
                    raise SystemExit(f'{label}: stopped at {got_pos}, the stream says {want_pos}')
            results[switch] = out
        if results[False] != results[True]:
            a, b = results[False], results[True]
            for i, (x, y) in enumerate(zip(a, b)):
                if x != y:
                    raise SystemExit(f'{stream}: run route and word-by-word route differ at line {i}:\n  {x}\n  {y}')
            raise SystemExit(f'{stream}: run route and word-by-word route differ in length ({len(a)} vs {len(b)})')
        print(f'{stream}: ok ({len(results[False])} lines, identical on both routes)')
print('[gcm_run_route] PASS')
