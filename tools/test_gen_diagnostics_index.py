#!/usr/bin/env python3
"""The diagnostics index checker rejects knob drift, not prose differences.

`tools/gen_diagnostics_index.py --check` compares the hand-maintained
docs/DIAGNOSTICS.md against the knobs the sources read. This runs it on copies
of the real document and proves both directions fail: a knob removed from the
document, and a knob the sources do not read.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/gen_diagnostics_index.py'


def check(doc):
    return subprocess.run([sys.executable, str(TOOL), '--check', '--doc', str(doc)],
                          capture_output=True, text=True)


def main():
    text = (ROOT / 'docs' / 'DIAGNOSTICS.md').read_text()
    with tempfile.TemporaryDirectory(prefix='diag-check-') as tmp:
        doc = Path(tmp) / 'DIAGNOSTICS.md'

        doc.write_text(text)
        p = check(doc)
        assert p.returncode == 0, (p.stdout, p.stderr)

        # A knob the sources read, absent from the document, is drift.
        entry = re.compile(r'^- \*\*`([A-Za-z_][A-Za-z_0-9]*)`\*\*[^\n]*\n  `[^\n]*`[^\n]*\n', re.M)
        m = entry.search(text)
        assert m, 'no knob entry found in docs/DIAGNOSTICS.md'
        doc.write_text(text[:m.start()] + text[m.end():])
        p = check(doc)
        assert p.returncode != 0 and m.group(1) in p.stderr and 'missing' in p.stderr, (
            p.stdout, p.stderr)

        # A knob the document lists but no source reads is drift.
        doc.write_text(text.replace('## `A2O`',
                                    '- **`NOT_A_KNOB`** — synthetic\n'
                                    '  `tools/synthetic.c:1`\n\n## `A2O`', 1))
        p = check(doc)
        assert p.returncode != 0 and 'NOT_A_KNOB' in p.stderr and 'no source reads' in p.stderr, (
            p.stdout, p.stderr)

    print('PASS: the diagnostics index check accepts the document and rejects '
          'missing and stale knobs')
    return 0


if __name__ == '__main__':
    sys.exit(main())
