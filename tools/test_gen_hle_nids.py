#!/usr/bin/env python3
"""The HLE registration table is fail-closed (tools/gen_hle_nids.py).

Runs the real generator over the checkout and checks that a scanned definition
is either a registered firmware export or an excluded host-side helper, never a
NID registered by guess. The helper names an adversarial review found in the
table (`cellfs_translate_path`, `cellGcm_syscall_bringup`, `cellPad_poll`, ...)
must not come back: a title import whose 32-bit NID happened to match one would
dispatch guest arguments into a host helper.

Then it runs the generator over synthetic trees to exercise the decision point:
a helper-shaped name is excluded, an export-shaped one is registered, and a
name that fits neither exits 2 with the name on stderr and no output file.

`python3 tools/test_gen_hle_nids.py --census` (needs clang) prints the signature
census docs/HLE_ABI.md quotes: one clang AST per module source, the definition
of every scanned name, typedefs resolved, classified by the documented tables.
"""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import gen_hle_nids as gen  # noqa: E402

REG_RE = re.compile(r'ps3_hle_register\(0x[0-9A-F]{8}u, "([A-Za-z_][A-Za-z0-9_]*)",')

# The names the review caught in the generated table, plus the helpers that
# were already excluded before it (CamelCase ones the shape rule cannot see).
HOST_HELPERS = set("""
    cellAvconfExt_display_gamma
    cellGame_get_hdd_title_id
    cellGame_get_title
    cellGame_get_title_id
    cellGame_init_from_paramsfo
    cellGame_set_content_path
    cellGame_set_title
    cellGame_set_title_id
    cellGcmOffsetIsDisplay
    cellGcmResolveIO
    cellGcmResolveLocated
    cellGcmResolveOffset
    cellGcm_address_to_offset
    cellGcm_control_guest_addr
    cellGcm_ctx_emit
    cellGcm_ctx_reserve
    cellGcm_display_buffer_count
    cellGcm_emit_flip_command
    cellGcm_fifo_kick_event
    cellGcm_fifo_kick_wait
    cellGcm_fifo_recycle
    cellGcm_flip_request_count
    cellGcm_ref_on_poll
    cellGcm_replay_display_buffers_to_engine
    cellGcm_request_tick
    cellGcm_rsx_process_fifo
    cellGcm_set_capture_sink
    cellGcm_syscall_bringup
    cellGcm_syscall_iomap
    cellGcm_syscall_iounmap
    cellGcm_syscall_set_fifo
    cellGcm_take_flip_pending
    cellGcm_take_flip_pending_synced
    cellGcm_vblank_flip_label
    cellGcm_vblank_signal
    cellKb_keyEvent
    cellMouse_buttonEvent
    cellMouse_moveEvent
    cellMouse_wheelEvent
    cellMsgDialog_tick
    cellPad_poll
    cellResc_convert_register_count
    cellRtcGetTime_t
    cellRtcSetTime_t
    cellSaveData_register_ctx_handlers
    cellSaveData_set_scratch_region
    cellSysutil_pump_seen
    cellSysutil_register_poll_hook
    cellUserInfoSelectUser_ListGet
    cellUserInfoSelectUser_ListSet
    cellVideoOut_refresh_rate
    cellVideoOut_set_resolution
    cellfs_add_path_mapping
    cellfs_set_root_path
    cellfs_translate_path
    sceNpTrophy_register_wait_cap_count
    sys_rsx_init
""".split())


def run(gen_path, out, root=None):
    cmd = [sys.executable, str(gen_path), "--all", "--out", str(out)]
    if root is not None:
        cmd += ["--root", str(root)]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=120)


def registrations(path):
    names = REG_RE.findall(path.read_text())
    assert len(names) == len(set(names)), 'a handler is registered twice'
    return names


# ---- signature census (the numbers docs/HLE_ABI.md quotes) -----------------

_QUALIFIER = re.compile(r"^(?:const|volatile|restrict|_Atomic)\s+")
_INT_TYPES = {
    "void", "_Bool", "bool", "char", "signed char", "unsigned char",
    "short", "short int", "unsigned short", "unsigned short int",
    "int", "unsigned int", "unsigned", "long", "long int", "unsigned long",
    "unsigned long int", "long long", "long long int", "unsigned long long",
    "unsigned long long int", "signed", "__int128", "unsigned __int128",
}
_FP_TYPES = {"float", "double", "long double"}


def _strip_qualifiers(t):
    t = t.strip()
    while True:
        n = _QUALIFIER.sub("", t)
        if n == t:
            return t
        t = n


def _split_params(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def _classify_type(t, typedefs, records, enums):
    t = _strip_qualifiers(t)
    if t in ("", "void"):
        return "void"
    if t == "...":
        return "variadic"
    if t.endswith("&"):
        return "reference"
    if "[" in t:
        return "array"
    if t in typedefs:
        return _classify_type(typedefs[t], typedefs, records, enums)
    if "(*" in t or "(^" in t:
        return "function-pointer"
    if t.endswith("*"):
        return "pointer"
    if t.startswith(("struct ", "union ")) or t in records:
        return "record"
    if t.startswith("enum ") or t in enums:
        return "enum"
    if t in _FP_TYPES:
        return "float"
    if t in _INT_TYPES:
        return "integral"
    return "unknown:" + t


def _walk_ast(node, wanted, out):
    kind = node.get("kind")
    if kind == "TypedefDecl":
        out["typedefs"][node["name"]] = node.get("type", {}).get("qualType", "")
    elif kind == "EnumDecl" and node.get("name"):
        out["enums"].add(node["name"])
    elif kind == "RecordDecl" and node.get("name"):
        out["records"].add(node["name"])
    elif kind == "FunctionDecl" and node.get("name") in wanted:
        # The definition is the truth (a header may disagree with the body).
        if any(c.get("kind") == "CompoundStmt" for c in node.get("inner", [])):
            out["defs"][node["name"]] = node.get("type", {}).get("qualType", "")
    for child in node.get("inner", ()):
        _walk_ast(child, wanted, out)


def census():
    """Print the signature census docs/HLE_ABI.md quotes (requires clang)."""
    libs = gen.discover_all(ROOT)
    wanted = set()
    for lib in libs:
        for _nid, name in gen.scan(gen.module_source(lib, ROOT)):
            wanted.add(name)
    info = {"typedefs": {}, "enums": set(), "records": set(), "defs": {}}
    for lib in libs:
        src = gen.module_source(lib, ROOT)
        p = subprocess.run(
            ["clang", "-std=c11", "-fsyntax-only", "-w",
             "-I" + str(ROOT), "-I" + str(ROOT / "include"),
             "-I" + str(ROOT / "runtime"), "-I" + str(ROOT / "runtime/ppu"),
             "-I" + str(ROOT / "libs"), "-Xclang", "-ast-dump=json", src],
            capture_output=True, text=True, timeout=120)
        try:
            ast = json.loads(p.stdout)
        except ValueError:
            print(f'cannot get a clang AST for {src}: {p.stderr.strip()[:300]}',
                  file=sys.stderr)
            return 1
        _walk_ast(ast, wanted, info)

    known = gen.known_export_names()
    registered = {n for n in wanted if gen.classify_name(n, known) == "export"}
    excluded = wanted - registered
    ret_counts, parm_counts = {}, {}
    fp_handlers, over8, pointer_names = set(), [], []
    for name in sorted(wanted):
        qt = info["defs"].get(name)
        if qt is None:
            continue
        # qualType is "<return>(<params>)"; match the final ')' to its '('.
        depth, open_i = 0, -1
        for i in range(len(qt) - 1, -1, -1):
            if qt[i] == ")":
                depth += 1
            elif qt[i] == "(":
                depth -= 1
                if depth == 0:
                    open_i = i
                    break
        assert open_i >= 0 and qt.endswith(")"), (name, qt)
        ret = _classify_type(qt[:open_i], info["typedefs"], info["records"],
                             info["enums"])
        ret_counts[ret] = ret_counts.get(ret, 0) + 1
        if ret == "pointer":
            pointer_names.append(name)
        params = [p for p in _split_params(qt[open_i + 1:-1]) if p != "void"]
        if len(params) > 8:
            over8.append((name, len(params)))
        nfp = 0
        for pt in params:
            pc = _classify_type(pt, info["typedefs"], info["records"],
                                info["enums"])
            parm_counts[pc] = parm_counts.get(pc, 0) + 1
            if pc == "float":
                nfp += 1
        if nfp:
            fp_handlers.add(name)

    missing = sorted(wanted - set(info["defs"]))
    preg = sorted(n for n in pointer_names if n in registered)
    pexc = sorted(n for n in pointer_names if n in excluded)
    assert set(preg) == set(gen.GUEST_EA_RETURN) | set(gen.HOST_POINTER_RETURN), \
        (set(preg) ^ (set(gen.GUEST_EA_RETURN) | set(gen.HOST_POINTER_RETURN)))
    print(f'modules scanned: {len(libs)}')
    print(f'scanned definition names: {len(wanted)}')
    print(f'definitions with a clang AST: {len(info["defs"])} ({len(missing)} missing)')
    known_n = sum(1 for n in registered if n in known)
    print(f'registered: {len(registered)} ({known_n} in the known NID sources, '
          f'{len(registered) - known_n} by SDK export shape), '
          f'excluded host-side helpers: {len(excluded)}')
    print('returns: ' + ', '.join(f'{k} {ret_counts[k]}' for k in
                                  ('integral', 'void', 'float', 'pointer')))
    print(f'pointer returns: {len(pointer_names)} total = {len(preg)} registered '
          f'({len(gen.GUEST_EA_RETURN)} guest EA + {len(gen.HOST_POINTER_RETURN)} '
          f'host-pointer stop) + {len(pexc)} excluded ({", ".join(pexc)})')
    print('parameters: ' + ', '.join(
        f'{k} {parm_counts.get(k, 0)}' for k in
        ('pointer', 'integral', 'float', 'function-pointer', 'variadic',
         'record', 'reference', 'array', 'enum')))
    print(f'float parameters: {parm_counts.get("float", 0)} in {len(fp_handlers)} handlers')
    print('more than 8 parameters: ' + ', '.join(f'{n}({c})' for n, c in over8))
    return 0 if not missing else 1


def main():
    if sys.argv[1:] == ['--census']:
        return census()
    if sys.argv[1:]:
        print('usage: test_gen_hle_nids.py [--census]', file=sys.stderr)
        return 2
    # ---- the checkout: what the generator actually ships -------------------
    with tempfile.TemporaryDirectory(prefix='gen-hle-') as tmp:
        out = Path(tmp) / 'nids.cpp'
        p = run(TOOLS / 'gen_hle_nids.py', out)
        assert p.returncode == 0, (p.stdout, p.stderr)
        names = registrations(out)
        registered = set(names)

        leaked = sorted(HOST_HELPERS & registered)
        assert not leaked, ('host-side helpers registered as firmware exports: '
                            + ', '.join(leaked))

        # One decision point for every scanned name, with evidence either way.
        known = gen.known_export_names()
        scanned, excluded = set(), set()
        for lib in gen.discover_all(ROOT):
            for _nid, name in gen.scan(gen.module_source(lib, ROOT)):
                scanned.add(name)
        for name in sorted(scanned):
            kind = gen.classify_name(name, known)
            assert kind in ('export', 'helper'), (name, kind)
            if kind == 'helper':
                excluded.add(name)
                assert name not in registered, f'{name}: classified helper but registered'
            else:
                assert name in registered, f'{name}: classified export but not registered'
        assert excluded == HOST_HELPERS, (sorted(excluded - HOST_HELPERS),
                                          sorted(HOST_HELPERS - excluded))

        # The summary line is the doc's counting basis: it must be exact.
        m = re.search(r'scanned (\d+) definition name\(s\), excluded (\d+) '
                      r'host-side helper\(s\)', p.stdout)
        assert m, p.stdout
        assert int(m.group(1)) == len(scanned) and int(m.group(2)) == len(excluded), p.stdout
        assert f'{len(names)} NID handlers' in p.stdout, p.stdout

    # ---- classify_name: every class, and the two refusals ------------------
    assert gen.classify_name('cellGcmInit', {'cellGcmInit'}) == 'export'
    assert gen.classify_name('cellGcmInit', set()) == 'export'        # SDK shape
    assert gen.classify_name('sys_heap_malloc', set()) == 'export'    # sys_ family
    assert gen.classify_name('_sys_malloc', set()) == 'export'        # _sys_ family
    assert gen.classify_name('cellWidget_frobnicate', set()) == 'helper'
    assert gen.classify_name('cellWidget_syscall_x', set()) == 'helper'
    assert gen.classify_name('sys_rsx_init', set()) == 'helper'       # explicit list

    def refuses(name, known=()):
        try:
            gen.classify_name(name, set(known))
        except gen.UnclassifiedName as e:
            assert name in str(e), e
            return
        raise AssertionError(f'{name}: classified instead of refusing')

    for name in ('cellwidget', 'scemodule', 'cellfrob_'):
        refuses(name)
    # A helper shape that is also a known export must narrow the rule, not drop it.
    refuses('cellWidget_frobnicate', {'cellWidget_frobnicate'})

    # ---- synthetic tree: process-level fail-closed behaviour ---------------
    with tempfile.TemporaryDirectory(prefix='gen-hle-tree-') as tmp:
        tree = Path(tmp)
        (tree / 'libs' / 'misc').mkdir(parents=True)
        src = tree / 'libs' / 'misc' / 'cellFake.c'
        src.write_text('int cellFakeDoThing(void) { return 0; }\n'
                       'void cellFake_do_thing(void) {}\n')
        out = tree / 'out' / 'nids.cpp'
        p = run(TOOLS / 'gen_hle_nids.py', out, root=tree)
        assert p.returncode == 0, (p.stdout, p.stderr)
        assert registrations(out) == ['cellFakeDoThing'], registrations(out)
        assert 'cellFake_do_thing' in p.stderr, p.stderr

        # A name with no evidence is a named exit, not a registration.
        src.write_text('int cellfrob(void) { return 0; }\n')
        bad = tree / 'out' / 'bad.cpp'
        p = run(TOOLS / 'gen_hle_nids.py', bad, root=tree)
        assert p.returncode == 2, (p.returncode, p.stdout, p.stderr)
        assert 'cellfrob' in p.stderr, p.stderr
        assert not bad.exists(), 'the generator wrote a table containing an unclassified name'

    print('PASS: every scanned definition is a classified export or an excluded '
          'helper; unclassified names stop the generator')
    return 0


if __name__ == '__main__':
    sys.exit(main())
