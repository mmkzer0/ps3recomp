#!/usr/bin/env python3
"""Run real SPU channel event producers against concurrent reservation writes."""
import os, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import spu_fixture_link
r=Path(__file__).resolve().parents[1]
s=r/'runtime/spu'
with tempfile.TemporaryDirectory(prefix='spu-event-producers-') as tmp:
 b=Path(tmp)/'events'
 units=['spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
 # The default build takes the lock-line lock for every GETLLAR; the optional
 # lock-free GETLLAR (SPU_COH_FAST_GETLLAR=1) is built and run as well.
 for fast in (0,1):
  b=Path(tmp)/('events-fast' if fast else 'events')
  spu_fixture_link.build([s/'tests/test_spu_lockline_peer.c',*[s/(u+'.c') for u in units],r/'runtime/platform/win32_compat.c'],b,['-std=gnu17','-O1','-UNDEBUG','-DSPU_COH_FAST_GETLLAR=%d'%fast,'-I'+str(r/'include'),'-I'+str(s),'-I'+str(r/'runtime/platform')],tmp)
  subprocess.run([str(b)],check=True,timeout=30)
