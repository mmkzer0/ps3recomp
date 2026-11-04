#!/usr/bin/env python3
"""Checks that pn.py and probes/ring110/common/pn.h (the code the console runs) produce
identical codes, and that distinct codes are nearly orthogonal (matched-filter usable).
Needs a host `cc` for the first check.

Run:  python3 tools/test_hwprobe_pn.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
RING110 = os.path.join(HERE, "hwprobe", "probes", "ring110")
sys.path.insert(0, RING110)

import pn

IDS = [0, 1, 2, 3, 17, 200, 1000, 1001, 1002, 1200, 4096]


@unittest.skipUnless(shutil.which("cc"), "no host C compiler")
class PnMatchesC(unittest.TestCase):
    def test_same_codes(self):
        src = r'''
#include <stdio.h>
#include "pn.h"
int main(int argc, char **argv) {
    int i; for (i = 1; i < argc; i++) {
        int8_t s[R110_PN_LEN]; int k;
        r110_pn_signs((uint32_t)strtoul(argv[i], 0, 10), s);
        for (k = 0; k < R110_PN_LEN; k++) putchar(s[k] > 0 ? '+' : '-');
        putchar('\n');
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            c = os.path.join(d, "t.c")
            with open(c, "w") as fh:
                fh.write("#include <stdlib.h>\n" + src)
            exe = os.path.join(d, "t")
            subprocess.check_call(["cc", "-std=c99", "-Wall", "-I", os.path.join(RING110, "common"), c, "-o", exe])
            out = subprocess.check_output([exe] + [str(i) for i in IDS]).decode().split()
        self.assertEqual(len(out), len(IDS))
        for cid, line in zip(IDS, out):
            self.assertEqual(line, "".join("+" if v > 0 else "-" for v in pn.pn_signs(cid)), cid)


class PnQuality(unittest.TestCase):
    def test_cross_correlation_is_low(self):
        ref = pn.pn_signs(0)
        worst = 0.0
        for cid in range(1, 400):
            c = pn.pn_signs(cid)
            worst = max(worst, abs(sum(a * b for a, b in zip(ref, c))) / pn.LEN)
        self.assertLess(worst, 0.40)   # random 128-chip codes: sigma = 0.088, max over 400 ~ 0.3

    def test_autocorrelation_sidelobes_are_low(self):
        for cid in (0, 1, 777):
            c = pn.pn_signs(cid)
            for lag in range(1, 64):
                v = sum(c[i] * c[i + lag] for i in range(pn.LEN - lag)) / pn.LEN
                self.assertLess(abs(v), 0.40, (cid, lag))


if __name__ == "__main__":
    unittest.main(verbosity=2)
