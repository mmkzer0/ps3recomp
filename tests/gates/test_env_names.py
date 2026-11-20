#!/usr/bin/env python3
"""Every PS3RECOMP_* name the runtime or a library reads is one the environment
gate knows: a typed option's derived name (PS3RECOMP_<SECTION>_<KEY>),
PS3RECOMP_CONFIG, or a PS3_CFG_LEGACY_ENV entry. The gate stops with
config-invalid on any other PS3RECOMP_* variable, so a literal that is in
neither list ends a run before the title starts (toolkit #147).

The scan is over string literals, not getenv() call arguments: a name behind a
macro is still read (rsx_host_surface.h's RSX_HOST_SURFACE_ENV was the one #147
missed), and so is a name passed to a helper that calls getenv.

usage: test_env_names.py <source root>
"""
import re
import sys
from pathlib import Path

import gates_common as g

ROOT = None

# config.cpp builds each typed option's environment name the same way: the section and key as
# written here, upper-cased under the PS3RECOMP_ prefix.
TYPED = re.compile(r'^PS3_CFG_(?:BOOL|U32|STR)\(\s*([a-z0-9_]+)\s*,\s*([a-z0-9_]+)\s*,', re.M)
LEGACY = re.compile(r'^PS3_CFG_LEGACY_ENV\("([^"]+)"\)', re.M)
LITERAL = re.compile(r'"(PS3RECOMP_[A-Z0-9_]+)"')
SOURCE_SUFFIXES = {".c", ".h", ".m", ".mm", ".cc", ".cpp", ".hpp"}


def names_the_gate_knows():
    defs = (ROOT / "include/ps3emu/config_options.def").read_text(encoding="utf-8")
    typed = {"PS3RECOMP_%s_%s" % (section.upper(), key.upper())
             for section, key in TYPED.findall(defs)}
    legacy = set(LEGACY.findall(defs))
    assert typed and legacy, "config_options.def parsed as empty; the file moved?"
    # The gate reads the config file's path from PS3RECOMP_CONFIG itself and skips it by name
    # (config.cpp): it is not an option and must never be a typo.
    return typed | legacy | {"PS3RECOMP_CONFIG"}


def literals():
    """Every (name, path, line) for a "PS3RECOMP_*" literal in libs/ and runtime/.

    Test fixtures are skipped: they are probes, not code a title links, and they set knobs with
    synthetic names on purpose.
    """
    for base in ("libs", "runtime"):
        for path in sorted((ROOT / base).rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            rel = path.relative_to(ROOT)
            if "tests" in rel.parts:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for number, line in enumerate(text.splitlines(), 1):
                for match in LITERAL.finditer(line):
                    yield match.group(1), rel, number


def test_every_ps3recomp_literal_is_typed_or_legacy():
    known = names_the_gate_knows()
    found = list(literals())
    assert found, f"no PS3RECOMP_* literals under {ROOT / 'libs'} and {ROOT / 'runtime'}"
    unknown = [(name, path, line) for name, path, line in found if name not in known]
    assert not unknown, (
        "PS3RECOMP_* literal read by the runtime or a library but in neither the typed keys nor "
        "PS3_CFG_LEGACY_ENV; setting it would end a run as config-invalid. Add it to the legacy "
        "list in include/ps3emu/config_options.def (docs/UPSTREAM-DIVERGENCES.md CFG-7), or make "
        "it a typed option:\n" + "\n".join(f"  {name} ({path}:{line})"
                                            for name, path, line in unknown))


def main(argv):
    global ROOT
    assert len(argv) == 2, "usage: test_env_names.py <source root>"
    ROOT = Path(argv[1]).resolve()
    return g.main([(n, f) for n, f in sorted(globals().items()) if n.startswith("test_")])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
