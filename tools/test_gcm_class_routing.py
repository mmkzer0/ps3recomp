#!/usr/bin/env python3
"""A method is interpreted in the class bound on its subchannel, with the real FIFO walker and the real
register-file draw engine (libs/video/tests/test_gcm_class_routing.c, which compiles cellGcmSys.c and
rsx_draw_engine.c in): a 2D class never reaches the NV4097 register file, a rebind inside one stream
is followed, an unbound slot drops only the methods that need an object, and a register only the engine
reads is classified."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM class routing fixture uses POSIX linking')
    raise SystemExit(0)
video = root / 'libs/video'
with tempfile.TemporaryDirectory(prefix='gcm-class-routing-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(video),
                    str(video / 'tests/test_gcm_class_routing.c'),
                    str(video / 'rsx_dispatch.c'),
                    str(video / 'rsx_vertex_compact.c'),
                    str(video / 'rsx_texture_layout.c'),
                    str(video / 'rsx_vp_decompiler.c'),
                    str(video / 'rsx_fp_decompiler.c'),
                    str(root / 'runtime/memory/label_window.c'),
                    str(video / 'gcm_default_state.c'), *linker,
                    '-o', str(binary)], check=True)
    # Engine on (every walker path with the per-word mirrors), then the engine off with no diagnostic: the
    # 3D-run fast path is eligible.
    for args in ([], ['fast']):
        result = subprocess.run([str(binary), *args], timeout=60, capture_output=True, text=True)
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2000:])
            raise SystemExit('test_gcm_class_routing failed')
