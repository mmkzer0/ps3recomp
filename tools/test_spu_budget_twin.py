#!/usr/bin/env python3
"""build_spu_workloads.py --budget-twin (docs/SPU_XDIFF.md section 10, row 9).

The budgeted twin of an image is its generated source compiled a second time
under -DSPU_LIFT_BUDGET. This test drives the registry writer on a synthetic
image and holds it to what the runtime and the build need:

  - without the option the registry, the lift directory and the objects are the
    ones a title always had: no twin file, no twin registration;
  - with it, `spu_recomp_budget.c` and `budget_sources.cmake` exist, the registry
    registers each fingerprint's twin (and its text alias) under an image id above
    every production id, and the twin names are the lifter's (`TWIN_SUFFIX`);
  - compiled, the twin object defines only suffixed symbols and the production
    object has no trace of the twin (no suffixed symbol, no budget unwind);
  - a lift made before the rename block existed is refused, not linked.

Usage:  python3 tools/test_spu_budget_twin.py
"""
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
SPU = ROOT / 'runtime' / 'spu'
sys.path.insert(0, str(TOOLS))

from host_cc import c_compiler  # noqa: E402
from spu_lifter import TWIN_SUFFIX  # noqa: E402

BUILDER = TOOLS / 'build_spu_workloads.py'
GENERATOR = SPU / 'tests' / 'gen_xdiff_budget_job.py'
ENTRY = 'xb_spu_func_00000000'


def run(*args, check=True):
    result = subprocess.run([sys.executable, *map(str, args)], capture_output=True, text=True)
    if check and result.returncode:
        raise AssertionError(f'{args}: exit {result.returncode}\n{result.stdout}{result.stderr}')
    return result


class BudgetTwinRegistry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='budget-twin-')
        cls.root = Path(cls.tmp.name)
        images = cls.root / 'images'
        images.mkdir()
        gen = cls.root / 'gen'
        run(GENERATOR, gen)
        (images / 'xb.elf').write_bytes((gen / 'xdiff_budget_job.elf').read_bytes())
        cls.images = images

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build(self, name, *extra, check=True):
        out = self.root / name
        result = run(BUILDER, '--images', self.images, '--lifted', out / 'lifted', '--out', out / 'spu_workloads.c',
                     *extra, check=check)
        return out, result

    def test_without_the_option_nothing_of_the_twin_exists(self):
        out, _ = self.build('plain')
        registry = (out / 'spu_workloads.c').read_text()
        self.assertNotIn('register_twin', registry)
        self.assertNotIn(TWIN_SUFFIX, registry)
        self.assertFalse((out / 'lifted' / 'xb' / 'spu_recomp_budget.c').exists())
        self.assertFalse((out / 'lifted' / 'budget_sources.cmake').exists())

    def test_the_twin_is_registered_beside_the_body(self):
        out, _ = self.build('twin', '--budget-twin')
        registry = (out / 'spu_workloads.c').read_text()
        wrapper = (out / 'lifted' / 'xb' / 'spu_recomp_budget.c').read_text()
        self.assertIn('#define SPU_LIFT_BUDGET 1\n#include "spu_recomp.c"', wrapper)
        cmake = (out / 'lifted' / 'budget_sources.cmake').read_text()
        self.assertIn('${CMAKE_CURRENT_LIST_DIR}/xb/spu_recomp_budget.c', cmake)
        self.assertIn(f'extern void {ENTRY}{TWIN_SUFFIX}(spu_context*);', registry)
        self.assertIn(f'extern void xb_spu_recomp_register{TWIN_SUFFIX}(void);', registry)
        body = re.findall(r'spu_workload_register_img\((0x[0-9A-F]+)ULL, ' + ENTRY + r', (\d+),', registry)
        twin = re.findall(r'spu_workload_register_twin\((0x[0-9A-F]+)ULL, ' + ENTRY + TWIN_SUFFIX + r', (\d+)\)', registry)
        self.assertTrue(body and twin, registry)
        # every fingerprint the body is registered under has its twin, under one id above every body id
        self.assertEqual({fp for fp, _ in body}, {fp for fp, _ in twin})
        self.assertEqual(len({i for _, i in twin}), 1)
        self.assertGreater(int(twin[0][1]), max(int(i) for _, i in body))
        # the twin's indirect-branch table is filled under its own id, before the twin is registered
        self.assertRegex(registry, r'spu_begin_image\(%s\); xb_spu_recomp_register%s\(\);' % (twin[0][1], TWIN_SUFFIX))

    def test_object_of_the_body_has_no_trace_of_the_twin(self):
        cc = c_compiler()
        if not cc or cc.msvc or not shutil.which('nm'):
            self.skipTest('needs a GNU-style C compiler and nm to read the objects')
        out, _ = self.build('objects', '--budget-twin')
        lift = out / 'lifted' / 'xb'
        includes = [lift, SPU, ROOT / 'include', ROOT / 'runtime' / 'platform']
        body, twin = out / 'body.o', out / 'twin.o'
        for src, obj in ((lift / 'spu_recomp.c', body), (lift / 'spu_recomp_budget.c', twin)):
            built = cc.run(cc.obj(src, obj, std='gnu17', includes=includes, flags=['-O1', '-w']))
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        nm_body = subprocess.run(['nm', str(body)], capture_output=True, text=True, check=True).stdout
        nm_twin = subprocess.run(['nm', str(twin)], capture_output=True, text=True, check=True).stdout
        self.assertNotIn('budget', nm_body)
        self.assertRegex(nm_body, r'\bT _?' + ENTRY + r'\n')
        self.assertRegex(nm_twin, r'\bT _?' + ENTRY + TWIN_SUFFIX + r'\n')
        self.assertNotRegex(nm_twin, r'\bT _?' + ENTRY + r'\n')
        self.assertRegex(nm_twin, r'\bU _?spu_lift_budget_exhausted\n')

    def test_a_lift_without_the_rename_block_is_refused(self):
        out, _ = self.build('old', '--budget-twin')
        header = out / 'lifted' / 'xb' / 'spu_recomp.h'
        text = header.read_text()
        header.write_text(re.sub(r'#ifdef SPU_LIFT_BUDGET.*?#endif\n', '', text, flags=re.S))
        result = run(BUILDER, '--images', self.images, '--lifted', out / 'lifted', '--out', out / 'again.c',
                     '--skip-lift', '--budget-twin', check=False)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('rename block', result.stdout + result.stderr)
        self.assertFalse((out / 'again.c').exists())


if __name__ == '__main__':
    unittest.main()
