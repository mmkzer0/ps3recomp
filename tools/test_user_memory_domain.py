#!/usr/bin/env python3
"""Exercise the real LV2 user and mapper allocators with a manufactured VM."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
def main():
 if os.name=='nt':
  print('SKIP: standalone allocation-domain regression needs a POSIX toolchain');return 0
 with tempfile.TemporaryDirectory(prefix='ps3-memory-domain-') as tmp:
  tmp=Path(tmp); exe=tmp/'fixture'
  cc=os.environ.get('CC','cc'); cxx=os.environ.get('CXX','c++')
  objs=[]
  for i,src in enumerate([root/'tests/test_user_memory_domain.c',root/'runtime/syscalls/sys_memory.c',
                          root/'runtime/diag/ps3_log.c',root/'runtime/diag/host_exit.c']):
   obj=tmp/f'c{i}.o'; objs.append(str(obj))
   subprocess.run([cc,'-std=gnu17','-O1','-UNDEBUG','-I'+str(root/'include'),'-c',str(src),'-o',str(obj)],check=True,timeout=30)
  # sys_memory.c's migrated diagnostics call ps3_log_*; config.cpp completes it, and the
  # fixture's guest_range_catch.h stands in for stop.cpp.
  for i,src in enumerate([root/'runtime/config/config.cpp']):
   obj=tmp/f'cxx{i}.o'; objs.append(str(obj))
   subprocess.run([cxx,'-std=c++20','-O1','-w','-I'+str(root/'include'),'-c',str(src),'-o',str(obj)],check=True,timeout=60)
  subprocess.run([cxx,*objs,'-pthread','-o',str(exe)],check=True,timeout=30)
  subprocess.run([str(exe)],check=True,timeout=15)
 return 0
if __name__=='__main__':raise SystemExit(main())
