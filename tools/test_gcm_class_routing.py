#!/usr/bin/env python3
"""A method is interpreted in the class bound on its subchannel, with the real FIFO walker and the real
register-file draw engine (libs/video/tests/test_gcm_class_routing.c, which compiles cellGcmSys.c and
rsx_draw_engine.c in): a 2D class never reaches the NV4097 register file, a rebind inside one stream
is followed, an unbound slot drops only the methods that need an object, and a register only the engine
reads is classified."""
import os
from pathlib import Path
import re
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
                    str(video / 'resc_convert.c'),
                    str(video / 'rsx_vertex_compact.c'),
                    str(video / 'rsx_texture_layout.c'),
                    str(video / 'rsx_vp_decompiler.c'),
                    str(video / 'rsx_fp_decompiler.c'),
                    str(root / 'runtime/memory/label_window.c'),
                    str(video / 'gcm_default_state.c'), *linker,
                    '-o', str(binary)], check=True)
    # The methods the legacy state machine takes, from the real rsx_commands.c (the same probe that bounds
    # gcm_legacy_method_has_effect, tools/test_rsx_legacy_effect_methods.py).
    probe = Path(tmp) / 'legacy_probe'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I' + str(root / 'include'), '-I' + str(video),
                    str(video / 'tests/test_rsx_legacy_effect_methods.c'),
                    str(video / 'rsx_commands.c'), *([] if sys.platform == 'darwin' else ['-lm']),
                    '-o', str(probe)], check=True)
    listing = subprocess.run([str(probe), '--handled'], timeout=60, capture_output=True, text=True)
    handled = Path(tmp) / 'legacy_handled.txt'
    handled.write_text(''.join(l + '\n' for l in listing.stdout.splitlines() if re.fullmatch(r'0x[0-9A-F]{4}', l)))
    # Engine on (every walker path with the per-word mirrors, the default-state and RESC streams), the
    # engine off with no diagnostic (the 3D-run fast path is eligible), and the legacy-handled methods'
    # verdicts.
    for args in ([], ['fast'], ['legacy-handled', str(handled)]):
        result = subprocess.run([str(binary), *args], timeout=60, capture_output=True, text=True)
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2000:])
            raise SystemExit('test_gcm_class_routing failed')
