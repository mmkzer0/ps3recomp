"""The override-symbol file of one differential-harness module.

    {"image_sha256": "<hex>",
     "symbols": {"vsh.ring_write": {"addr": "0x02000000", "evidence": "..."}, ...}}

It names internal addresses of a lifted module (the points the harness may replace,
tests/diff_harness/dh_engine.h `override_symbol`). The addresses are firmware-derived: the file
lives next to the lifted module outside the repository, and the committed case files only name
symbols. Every consumer (gen_tables.py: the generated symbol table, apply_overrides.py: the
wrappers) loads it through here, so a file made for one build cannot be used with another and a
mistyped address cannot silently select the wrong function.
"""
import hashlib
import json
import re

NAME = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$")


def load(path, image, base):
    """-> (image_sha256, [(name, addr, evidence)]) sorted by address, or SystemExit."""
    doc = json.load(open(path))
    sha = hashlib.sha256(image).hexdigest()
    if doc.get("image_sha256") != sha:
        raise SystemExit(f"{path}: image_sha256 {doc.get('image_sha256')} is not the image's {sha}; "
                         "the symbols belong to another build")
    out, seen = [], {}
    for name, ent in doc.get("symbols", {}).items():
        if not NAME.match(name):
            raise SystemExit(f"{path}: symbol name {name!r} is not <group>.<name> in lower case")
        addr = int(ent["addr"], 16)
        if addr & 3 or not (base <= addr < base + len(image)):
            raise SystemExit(f"{path}: {name} 0x{addr:08X} is not a word-aligned address inside the image")
        if addr in seen:
            raise SystemExit(f"{path}: {name} and {seen[addr]} name the same address 0x{addr:08X}")
        seen[addr] = name
        if not ent.get("evidence"):
            raise SystemExit(f"{path}: {name} has no evidence; an override point must say why it is that function")
        out.append((name, addr, ent["evidence"]))
    if not out:
        raise SystemExit(f"{path}: no symbols")
    return sha, sorted(out, key=lambda t: t[1])
