#!/usr/bin/env python3
"""sys_memory containers take their memory out of the default pool (the real sys_memory.c, a manufactured VM)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parent))
import runtime_link  # noqa: E402
root=Path(__file__).resolve().parents[1]
def main():
 if os.name=='nt':
  print('SKIP: standalone memory-container regression needs a POSIX toolchain');return 0
 with tempfile.TemporaryDirectory(prefix='ps3-memory-container-') as tmp:
  exe=Path(tmp)/'fixture'
  # sys_memory.c logs, reads the typed configuration and ends refused calls in named stops; the fixture catches
  # ps3_stop_guest_range itself (guest_range_catch.h), so the runtime adds the log and the configuration only.
  runtime_link.build([root/'tests/test_sys_memory_container.c',root/'runtime/syscalls/sys_memory.c'],exe,tmp,
                     cflags=['-std=gnu17','-O1','-UNDEBUG','-I'+str(root/'include')],timeout=120,
                     runtime=('log','config'))
  subprocess.run([str(exe)],check=True,timeout=15)
 return 0
if __name__=='__main__':raise SystemExit(main())
