#!/usr/bin/env python3
"""The PUT path's [POISON-DMA] report: region PUTs always, poison-only PUTs opt-in."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import spu_fixture_link  # noqa: E402


@unittest.skipIf(os.name == 'nt', 'needs a lazily committed 1 GiB mapping')
class PoisonScanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get('CC', 'clang'))
        if not cc:
            raise unittest.SkipTest('C compiler required')
        os.environ['CC'] = cc
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.exe = Path(cls.tmp.name) / 'poison-scan'
        # The PUT path raises named stops and reads typed configuration
        # (spu_dma.h), so the fixture links the C++ stop/config runtime.
        spu_fixture_link.build([ROOT / 'runtime/spu/tests/test_spu_poison_scan.c',
                                ROOT / 'runtime/platform/win32_compat.c'], cls.exe,
                               ['-std=gnu17', '-O2', '-I' + str(ROOT / 'include'),
                                '-I' + str(ROOT / 'runtime/spu'),
                                '-I' + str(ROOT / 'runtime/platform')], cls.tmp.name)

    def run_mode(self, mode, watch):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'YDKJ_', 'LBP_'))}
        if watch:
            env['SPU_POISON_DMA_WATCH'] = '1'
        p = subprocess.run([str(self.exe), mode], capture_output=True, text=True, env=env, timeout=30)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('OK: poison PUT report', p.stdout)
        return [l for l in p.stderr.splitlines() if l.startswith('[POISON-DMA]')]

    def test_poison_outside_region_is_opt_in(self):
        self.assertEqual(self.run_mode('outside', False), [])

    def test_poison_outside_region_reported_when_armed(self):
        self.assertEqual(self.run_mode('outside', True), [
            '[POISON-DMA] img7 PUT ea=0x00020000 size=0x40 lsa=0x1000 hitregion=0 haspoison=1'])

    def test_region_reported_with_scan_result(self):
        expected = [
            '[POISON-DMA] img7 PUT ea=0x40003000 size=0x40 lsa=0x1000 hitregion=1 haspoison=0',
            '[POISON-DMA] img7 PUT ea=0x40003100 size=0x40 lsa=0x1000 hitregion=1 haspoison=1']
        self.assertEqual(self.run_mode('region', False), expected)
        self.assertEqual(self.run_mode('region', True), expected)


if __name__ == '__main__':
    unittest.main()
