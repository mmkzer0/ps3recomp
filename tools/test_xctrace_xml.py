#!/usr/bin/env python3
"""xctrace_xml: refs across rows and tables, sentinels, malformed exports."""
import io
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from xctrace_xml import MalformedExport, iter_rows  # noqa: E402

DOC = b"""<?xml version="1.0"?>
<trace-query-result>
<node><schema name="a"><col><mnemonic>time</mnemonic></col><col><mnemonic>stack</mnemonic></col></schema>
<row><sample-time id="1" fmt="0.1">100</sample-time><backtrace id="2"><frame id="3" name="leaf" addr="0x10"/><frame id="4" name="root" addr="0x20"/></backtrace></row>
<row><sample-time id="5">200</sample-time><backtrace id="6"><frame ref="4"/></backtrace></row>
</node>
<node><schema name="b"><col><mnemonic>time</mnemonic></col><col><mnemonic>value</mnemonic></col></schema>
<row><sample-time ref="1"/><sentinel/></row>
<row><sample-time ref="5"/><backtrace ref="2"/></row>
</node>
</trace-query-result>
"""

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print('FAIL:', what)


def rows(doc, schema=None):
    return list(iter_rows(io.BytesIO(doc), schema))


all_rows = rows(DOC)
check([s for s, _ in all_rows] == ['a', 'a', 'b', 'b'], 'rows keep their table and order')
r0, r1 = all_rows[0][1], all_rows[1][1]
check(r0['time'].int() == 100 and r0['time'].fmt == '0.1', 'value and display string')
check([f.attrib['name'] for f in r0['stack'].findall('frame')] == ['leaf', 'root'], 'frames leaf first')
check(r1['stack'].findall('frame')[0].attrib['name'] == 'root', 'a frame ref resolves inside a new backtrace')
b = rows(DOC, 'b')
check(len(b) == 2, 'schema filter keeps only that table')
check(b[0][1]['time'].int() == 100, 'a ref resolves across tables')
check(b[0][1]['value'].tag == 'sentinel', 'sentinel is an empty value')
check(b[1][1]['value'].find('frame').attrib['name'] == 'leaf', 'a whole interned backtrace resolves by ref')
# A wanted row may refer to an id interned by a row of another table.
check(rows(DOC, 'b')[1][1]['time'].int() == 200, 'ids from skipped tables still intern')

for bad, what in [
    (b'<r><node><schema name="a"><col><mnemonic>t</mnemonic></col></schema>'
     b'<row><x ref="9"/></row></node></r>', 'ref before its id'),
    (b'<r><node><schema name="a"><col><mnemonic>t</mnemonic></col></schema>'
     b'<row><x id="1">1</x><y id="2">2</y></row></node></r>', 'more values than columns'),
    (b'<r><node><row><x id="1">1</x></row></node></r>', 'row before schema'),
]:
    try:
        rows(bad)
        check(False, 'malformed export raised: ' + what)
    except MalformedExport:
        pass

print('xctrace_xml: %s' % ('ok' if not failures else '%d failed' % failures))
sys.exit(1 if failures else 0)
