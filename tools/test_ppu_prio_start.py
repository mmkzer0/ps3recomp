#!/usr/bin/env python3
"""PS3_PRIO_START through the real sys_ppu_thread.c: run once with the option and once without."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='ppu-prio-start-') as tmp:
    binary=Path(tmp)/'test'
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-pthread','-lm']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections',
        '-I'+str(root/'include'),'-I'+str(root/'runtime'),'-I'+str(root/'runtime/syscalls'),
        str(root/'tests/test_ppu_prio_start.c'),*linker,'-o',str(binary)],check=True)
    for on in (False, True):
        env={k:v for k,v in os.environ.items() if k!='PS3_PRIO_START'}
        if on: env['PS3_PRIO_START']='1'
        # The fixture logs every thread it starts: keep that out of a passing run, but a failing one must say which
        # check failed, so the stderr is captured and shown only then (a CI log with it suppressed names nothing).
        try:
            run=subprocess.run([str(binary)],timeout=60,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,errors='replace')
        except subprocess.TimeoutExpired as e:
            sys.stderr.write(e.stderr.decode(errors='replace') if isinstance(e.stderr,bytes) else (e.stderr or ''))
            raise SystemExit(f"FAIL: PS3_PRIO_START {'on' if on else 'off'}: no result after {e.timeout} s")
        if run.returncode:
            sys.stderr.write(run.stderr)
            raise SystemExit(f"FAIL: PS3_PRIO_START {'on' if on else 'off'}: exit status {run.returncode}")
        sys.stdout.write(run.stdout)
