#!/usr/bin/env python3
"""gcm.handler_thread: the guest handlers run on an interrupt thread with the configured guest PPU thread host stack."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent))
import runtime_link  # noqa: E402
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-handler-thread-') as tmp:
    binary=Path(tmp)/'test'
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections']
    # Linked as tools/test_gcm_handler_mask.py: cellGcmSys.c's migrated diagnostics call ps3_log_*; the fixture
    # supplies ps3_config_get and the stops (gcm_fixture_stubs.h), so the log links with host_exit.c and the mirror stub.
    runtime_link.build([root/'tests/test_gcm_handler_thread.c',root/'libs/video/gcm_default_state.c'],binary,tmp,
        cflags=['-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections',
                '-I'+str(root/'include'),'-I'+str(root/'libs/video')],
        ldflags=linker,runtime=('log','label_window'))
    subprocess.run([str(binary)],check=True,timeout=10,
        env={k:v for k,v in os.environ.items() if not k.startswith('A2O_')})
