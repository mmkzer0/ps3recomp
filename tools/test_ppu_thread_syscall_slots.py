#!/usr/bin/env python3
"""lv2 syscall slot 41 is _sys_ppu_thread_exit and 42 is the stock ENOSYS slot, through the real sys_ppu_thread.c and its table."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parent))
import runtime_link  # noqa: E402
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='ppu-thread-syscall-slots-') as tmp:
    binary=Path(tmp)/'test'
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections']
    # The fixture models ppu_loader.cpp's default syscall path with the generated FW 4.93 bitmap; it stubs the
    # stop and the configuration itself, so the run log links with the mirror stub, not with stop.cpp.
    runtime_link.build([root/'tests/test_ppu_thread_syscall_slots.c',
                        root/'runtime/syscalls/lv2_enosys_bitmap.c'],binary,tmp,
        cflags=['-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections',
                '-I'+str(root/'include'),'-I'+str(root/'runtime'),'-I'+str(root/'runtime/syscalls')],
        ldflags=linker,runtime=('log',))
    subprocess.run([str(binary)],check=True,timeout=60,stderr=subprocess.DEVNULL if os.environ.get('CI') else None)
