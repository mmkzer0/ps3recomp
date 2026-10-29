#!/usr/bin/env python3
"""Streaming reader for `xcrun xctrace export` table XML.

  xcrun xctrace export --input run.trace --toc                       # list tables
  xcrun xctrace export --input run.trace \
      --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' \
      --output tp.xml

The export interns values: the first occurrence of a value carries id="N" and
every later occurrence is an empty element with ref="N", possibly in a later
row. A value can also be the empty <sentinel/>. Rows are positional: child i
of a <row> belongs to column i of the <schema> that opened the enclosing
<node>. This module resolves refs and columns and yields one dict per row, so
callers never see ids, refs or positions.

Memory holds one resolved Node per interned id (not the XML tree): rows are
cleared as they are consumed. A missing ref is a malformed export and raises.
"""
import xml.etree.ElementTree as ET

__all__ = ['Node', 'MalformedExport', 'iter_rows']


class MalformedExport(ValueError):
    pass


class Node:
    """One resolved value: tag, attributes, text and resolved children."""
    __slots__ = ('tag', 'attrib', 'text', 'children')

    def __init__(self, tag, attrib, text, children):
        self.tag = tag
        self.attrib = attrib
        self.text = text
        self.children = children

    @property
    def fmt(self):
        """Instruments' display string (for example "Running", "1.00 ms")."""
        return self.attrib.get('fmt', self.text)

    def int(self):
        """The raw integer value (sample times and weights are in ns)."""
        if self.text is None:
            raise MalformedExport('<%s> has no value' % self.tag)
        return int(self.text)

    def find(self, tag):
        for c in self.children:
            if c.tag == tag:
                return c
        return None

    def findall(self, tag):
        return [c for c in self.children if c.tag == tag]

    def __repr__(self):
        return 'Node(%s, %r)' % (self.tag, self.fmt)


SENTINEL = Node('sentinel', {}, None, ())


def _resolve(el, interned):
    ref = el.get('ref')
    if ref is not None:
        try:
            return interned[ref]
        except KeyError:
            raise MalformedExport('ref=%s before its id' % ref) from None
    if el.tag == 'sentinel':
        return SENTINEL
    children = tuple(_resolve(c, interned) for c in el)
    attrib = {k: v for k, v in el.attrib.items() if k != 'id'}
    text = el.text.strip() if el.text and el.text.strip() else None
    node = Node(el.tag, attrib, text, children)
    nid = el.get('id')
    if nid is not None:
        interned[nid] = node
    return node


def iter_rows(source, schema=None):
    """Yield (schema_name, {column mnemonic: Node}) for every row.

    source is a path or a binary file object. schema, if given, skips rows of
    other tables (an export of several tables holds one <node> per table).
    A path is opened here and closed when the generator finishes or is closed.
    """
    if isinstance(source, (str, bytes)) or hasattr(source, '__fspath__'):
        with open(source, 'rb') as f:
            yield from _iter_rows(f, schema)
    else:
        yield from _iter_rows(source, schema)


def _iter_rows(source, schema):
    interned = {}
    columns = None
    name = None
    depth = 0
    for event, el in ET.iterparse(source, events=('start', 'end')):
        if event == 'start':
            depth += 1
            continue
        depth -= 1
        if el.tag == 'schema':
            name = el.get('name')
            columns = [c.findtext('mnemonic') for c in el.findall('col')]
            if None in columns:
                raise MalformedExport('schema %s has a column without a mnemonic' % name)
        elif el.tag == 'row':
            if columns is None:
                raise MalformedExport('<row> before any <schema>')
            if schema is None or name == schema:
                # Resolve even rows of another table: they may intern ids that
                # a wanted row refers to.
                values = [_resolve(c, interned) for c in el]
                if len(values) != len(columns):
                    raise MalformedExport('row has %d values for %d columns of %s'
                                          % (len(values), len(columns), name))
                yield name, dict(zip(columns, values))
            else:
                for c in el:
                    _resolve(c, interned)
            el.clear()
        elif el.tag == 'node' and depth == 1:
            el.clear()
