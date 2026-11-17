#!/usr/bin/env python3
"""Exercise cellGcmMapMainMemory's IO allocation (first fit, no map before Init) through the real cellGcmSys.c."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-io-alloc-') as tmp:
    binary=Path(tmp)/'test'
    # cellGcmSys.c's migrated lines call ps3_log_*; this fixture supplies the stops and no config, so
    # the log links with host_exit.c and the mirror stub (its configure path is unreached).
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-pthread','-lm']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections','-I'+str(root/'include'),
        str(root/'tests/test_gcm_io_alloc.c'),str(root/'runtime/diag/ps3_log.c'),str(root/'runtime/diag/host_exit.c'),str(root/'tests/ps3_log_mirror_stub.c'),*linker,'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10,
        env={k:v for k,v in os.environ.items() if not k.startswith('A2O_')})
