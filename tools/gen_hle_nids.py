#!/usr/bin/env python3
"""
Generate the HLE NID -> handler registration unit from the function definitions
in the HLE library sources.

For every non-static cell*/sys*/sce* function definition it computes the NID
(the firmware-correct algorithm in nid_database.py) and emits
`ps3_hle_register(0x.., "name", &ps3_hle_adapt<&name>)` inside
`ppu_hle_register_all()` (the strong override of the weak default in
ppu_hle.cpp). The adapter (runtime/ppu/ppu_hle_adapt.hpp) derives each
handler's declared C type, so the module headers are #included: a handler the
header does not declare, or declares with a type the adapter cannot map, fails
the build at its name rather than being called through an invented signature.

Pointer-returning handlers have no default mapping, so each one is classified
here, with a comment saying why:

  * GUEST_EA_RETURN      the body returns a guest effective address; the low
                         32 bits are zero-extended into r3.
  * HOST_POINTER_RETURN  the body returns a host address (or an OS handle).
                         There is no correct guest value; the call stops with
                         kind=hle-unsupported-return instead of truncating it.
  * VARIADIC_EXPORTS     the declared type has no static tail. The fixed
                         parameters are marshalled by position and the remaining
                         integer argument registers are forwarded to va_start;
                         see ps3_hle_adapt_variadic in ppu_hle_adapt.hpp for
                         exactly which variadic argument classes that carries.
  * NOT_FIRMWARE_EXPORTS a host-side helper that no guest imports. It is not
                         registered at all (its computed NID would only ever
                         shadow a real import).

Registration is fail-closed: every scanned name is classified with evidence as
a firmware export or a host-side helper (classify_name), and a name that fits
neither stops the generator (exit 2, naming the function) instead of being
registered by guess. The export evidence is the curated NID sources
(tools/nid_database.py's builtin table, tools/nid_from_protos.json) plus the
SDK's export naming shapes; the helper evidence is a naming shape the SDK does
not use for exports, or the explicit list above. docs/HLE_ABI.md's census
section restates the counts.

Usage:
    python gen_hle_nids.py --out gen/ppu_hle_nids.cpp cellGcmSys [cellSysutil ...]
    (lib names resolve to libs/**/<name>.c)
    python gen_hle_nids.py --all --out /tmp/x.cpp
    (--root <dir> scans <dir>/libs instead of this checkout; the generator's
     own test points it at a synthetic tree)
"""
import argparse
import glob
import json
import os
import sys
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nid_database import compute_nid

# Non-static, top-level definitions of exported-looking functions. The NID is
# COMPUTED from the name (the firmware-correct algorithm in nid_database.py),
# not read from a /* NID */ comment -- computation matches the real imports far
# better and needs no manual annotation.
DEF_RE = re.compile(
    r"^(?!static)[A-Za-z_][\w \t\*]*?\b(_?(?:cell|sys|sce|_sys)\w+)\s*\([^;]*?\)\s*\{",
    re.M)

# Modules whose declarations live in another module's header (the header is
# still the module's public surface; only its file name differs).
HEADER_OVERRIDES = {
    # sysPrxForUser.c's user-space PRX entry points, including the
    # _sys_spu_printf_* family, are declared in sysPrxForUser.h.
    "sys_spu_printf": "libs/system/sysPrxForUser.h",
}

# A pointer return is a guest effective address: the body builds it from the
# guest VM (f->ea, an EA constant, a heap base, or the destination EA it was
# handed), so its low 32 bits are the address a guest dereference computes.
GUEST_EA_RETURN = {
    "cellFiberPpuSelf": "returns the fiber's stored guest EA (f->ea)",
    "cellGcmGetControlRegister": "returns GCM_CONTROL_GUEST_ADDR, the guest EA of the control register",
    "cellGcmGetLabelAddress": "returns GCM_LABEL_GUEST_BASE + index*stride",
    "cellGcmGetNotifyDataAddress": "returns gcm_io2ea(0x0F100000) + index*0x40, a mapped guest EA",
    "_sys_strcpy": "returns its dst parameter, a guest EA",
    "_sys_strncat": "returns its dst parameter, a guest EA",
    "_sys_memset": "returns its dst parameter, a guest EA",
    "_sys_memcpy": "returns its dst parameter, a guest EA",
    "_sys_malloc": "returns a yz_heap_alloc guest EA",
    "_sys_memalign": "returns a yz_heap_alloc guest EA",
    "_sys_realloc": "returns a yz_heap_alloc guest EA",
    "_sys_heap_malloc": "forwards sys_heap_malloc, a yz_heap_alloc guest EA",
    "_sys_heap_memalign": "forwards sys_heap_memalign, a yz_heap_alloc guest EA",
    "sys_heap_malloc": "returns a yz_heap_alloc guest EA",
    "sys_heap_memalign": "returns a yz_heap_alloc guest EA",
    "cellSheapAllocate": "returns heap_ea + offset (the guest EA of the block)",
}

# A real firmware export whose implementation returns a HOST address today.
# The guest has no address for that, so the registered adapter stops on the
# call (kind=hle-unsupported-return) instead of fabricating a truncated one.
# Each is a semantic defect in its module, listed in docs/HLE_ABI.md; fixing
# the body to return the guest EA moves it to GUEST_EA_RETURN.
HOST_POINTER_RETURN = {
    "cellGcmGetDisplayBufferByFlipIndex": "returns &s_display_buffers[i], a host array",
    "cellGcmGetDisplayInfo": "returns &s_display_buffers[i], a host array",
    "cellGcmGetReportDataAddress": "returns &s_report_data[i], a host array",
    "cellGcmGetReportDataAddressLocation": "returns &s_report_data[i], a host array",
    "cellGcmGetTileInfo": "returns &s_tiles[i], a host array",
    "cellGcmGetZcullInfo": "returns &s_zcull[i], a host array",
}

# A real firmware export declared variadic. ps3_hle_adapt_variadic makes the
# call through the exact variadic prototype, so the fixed parameters are typed
# and the guest's remaining integer argument registers reach va_start.
VARIADIC_EXPORTS = {
    "_sys_printf": "guest printf: fixed fmt pointer, %d/%s/%llx/%f read via va_list",
    "_sys_sprintf": "guest sprintf: fixed buffer + fmt, same va_list",
    "_sys_snprintf": "guest snprintf: fixed buffer + size + fmt, same va_list",
}

# Host-side helpers the shape rule cannot tell from a firmware export: their
# names are CamelCase after the module prefix, exactly like an SDK export, so
# only a reader can say they are not one. Each entry carries its reason, and an
# entry that is also in the known NID sources is a contradiction the generator
# refuses (classify_name) rather than resolving by guess. They stay ordinary C
# functions for their host callers; registering their computed NID could only
# shadow a real import. Helpers whose names carry a helper shape are caught by
# HELPER_SHAPES and need no entry here.
NOT_FIRMWARE_EXPORTS = {
    # cellGcmSys's host-side EA/state helpers. The other backends declare them
    # with a local extern (rsx_d3d12_backend.c, rsx_draw_engine.c), and the
    # real firmware exports for the same jobs are cellGcmGetOffsetTable /
    # cellGcmAddressToOffset, so a computed NID for these names is not an
    # import any title has.
    "cellGcmOffsetIsDisplay": "host-side display-offset predicate (rsx_d3d12_backend extern)",
    "cellGcmResolveOffset": "host-side offset->EA resolver (rsx_vertex_fetch extern)",
    "cellGcmResolveIO": "host-side IO-table resolver (rsx_draw_engine extern)",
    "cellGcmResolveLocated": "host-side MAIN/LOCAL resolver (cellGcmSys extern)",

    # Sub-operations of a real export (cellUserInfoSelectUser), not exports of
    # their own; the capitalised suffix dodges the helper shape.
    "cellUserInfoSelectUser_ListGet": "host-side half of the select-user list API",
    "cellUserInfoSelectUser_ListSet": "host-side half of the select-user list API",

    # Internal init helper called from lv2_register.c.
    "sys_rsx_init": "host-side init helper, takes a host lv2_syscall_table*",
}

# The naming shapes the SDK does not use for an export, tested BEFORE the
# export evidence. `cell*` + `_` + a lowercase word is the host code's
# snake_case (cellfs_translate_path, cellGcm_syscall_bringup, cellPad_poll);
# `_syscall_` marks a guest-syscall bridge helper. sys_* and _sys_* are
# deliberately absent: sys_net_bnet_accept, sys_heap_malloc and _sys_malloc are
# real exports. A hit that is also a known export is a contradiction that stops
# the generator -- narrow the rule, never drop a real export.
HELPER_SHAPES = (
    r"^_?cell[A-Za-z0-9]*_[a-z]",
    r"_syscall_",
)

# The SDK's export naming shapes. A scanned name absent from the known NID
# sources must match one of these to be registered.
EXPORT_FAMILIES = (
    r"^_?cell[A-Z0-9]",    # cellGcmInit, _cellSpursSendSignal (underscore alias)
    r"^_?sce[A-Z0-9]",     # sceNpScoreCreateRequest
    r"^_?sys_[a-z0-9_]",   # sys_net_bnet_accept, sys_heap_malloc, _sys_malloc
)


class UnclassifiedName(Exception):
    """A scanned definition that is neither a proven export nor a helper."""


def known_export_names():
    """Every name the known firmware-export sources contain.

    tools/nid_database.py's curated _BUILTIN_FUNCTIONS plus
    tools/nid_from_protos.json (names recovered from debug-build symbol tables,
    each validated against a NID those builds actually import by
    tools/nid_harvest.py). Deliberately not NIDDatabase.load_builtins(): that
    also ingests every name this repo defines (load_implemented), which would
    call every host helper "known".
    """
    from nid_database import _BUILTIN_FUNCTIONS

    names = {name for _module, name in _BUILTIN_FUNCTIONS}
    proto = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "nid_from_protos.json")
    try:
        with open(proto, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        raise SystemExit(f"error: cannot read the harvested NID table {proto}: {e}\n"
                         "  the export/helper classification needs it; restore it "
                         "rather than registering unclassified names")
    for module in data.values():
        names.update(module)
    return names


def classify_name(name, known_exports):
    """Classify one scanned definition as "export" or "helper".

    Evidence in order: the explicit host-helper list; the host-helper naming
    shapes; the known NID sources; the SDK export shapes. Anything else raises
    UnclassifiedName, and main() refuses to write the table: the alternative is
    to register a NID whose export status nobody established.
    """
    if name in NOT_FIRMWARE_EXPORTS:
        if name in known_exports:
            raise UnclassifiedName(
                f"{name}: listed in NOT_FIRMWARE_EXPORTS as a host helper but present "
                f"in the known firmware-export sources; one of the two is wrong")
        return "helper"
    for pattern in HELPER_SHAPES:
        if re.search(pattern, name):
            if name in known_exports:
                raise UnclassifiedName(
                    f"{name}: matches host-helper shape {pattern!r} but is present in "
                    f"the known firmware-export sources; narrow HELPER_SHAPES instead "
                    f"of dropping a real export")
            return "helper"
    if name in known_exports:
        return "export"
    for pattern in EXPORT_FAMILIES:
        if re.match(pattern, name):
            return "export"
    raise UnclassifiedName(
        f"{name}: not in the known firmware-export sources, matches neither an SDK "
        f"export shape ({' | '.join(EXPORT_FAMILIES)}) nor a host-helper shape "
        f"({' | '.join(HELPER_SHAPES)})")


def scan(c_path):
    txt = open(c_path, encoding="latin1").read()
    out = []
    for m in DEF_RE.finditer(txt):
        name = m.group(1)
        out.append((compute_nid(name), name))
    return out


def discover_all(root=ROOT):
    """Every PRX-style HLE module compiled into the runtime lib.

    The registration table must cover EVERY library the runtime implements:
    an unregistered import stops as unresolved, and a hand-passed module list
    is the single point where a whole library gets dropped by accident --
    exactly what happened when gen/ppu_hle_nids.cpp was regenerated with only
    cellFs/cellGcmSys/cellSysmodule and LBP then called cellSysutilCheckCallback
    unregistered, faked it, and diverged at boot. So `--all` is the canonical
    invocation; discover the same dirs CMakeLists.txt globs into
    ps3recomp_runtime, and take the cell*/sce*/sys* file names."""
    dirs = ["system", "filesystem", "input", "audio", "video", "network",
            "spurs", "sync", "codec", "font", "misc", "hardware"]
    names = set()
    for d in dirs:
        for p in glob.glob(os.path.join(root, "libs", d, "*.c")):
            n = os.path.splitext(os.path.basename(p))[0]
            if re.match(r"^(cell|sce|sys)", n) and not re.match(r"^(test_|validate_)", n):
                names.add(n)
    return sorted(names)


def module_source(lib, root=ROOT):
    cs = glob.glob(os.path.join(root, "libs", "**", lib + ".c"), recursive=True)
    return cs[0] if cs else None


def module_header(lib, c_path, root=ROOT):
    """Repository-relative header that declares the module's handlers."""
    rel = os.path.relpath(c_path, root)
    if lib in HEADER_OVERRIDES:
        return HEADER_OVERRIDES[lib]
    sibling = os.path.splitext(rel)[0] + ".h"
    if os.path.exists(os.path.join(root, sibling)):
        return sibling
    return None


def registration(name):
    """The adapter instantiation for one handler (see the lists above)."""
    if name in GUEST_EA_RETURN:
        return f"&ps3_hle_adapt_guest_ea<&{name}>"
    if name in HOST_POINTER_RETURN:
        return f"&ps3_hle_adapt_host_ptr_stop<&{name}>"
    if name in VARIADIC_EXPORTS:
        return f"&ps3_hle_adapt_variadic<&{name}>"
    return f"&ps3_hle_adapt<&{name}>"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("libs", nargs="*",
                    help="Module (file) names to register; omit with --all.")
    ap.add_argument("--all", action="store_true",
                    help="Register EVERY PRX module compiled into the runtime "
                         "(the canonical invocation -- can't silently drop a "
                         "library the way a hand-typed list can).")
    ap.add_argument("--root", default=ROOT,
                    help="Tree whose libs/ directory is scanned (default: this "
                         "checkout; the generator's test uses a synthetic tree).")
    ap.add_argument("--out", default="ppu_hle_nids.cpp")
    args = ap.parse_args()

    if args.all:
        args.libs = discover_all(args.root) + list(args.libs)
    if not args.libs:
        ap.error("no modules given; pass module names or --all")

    known = known_export_names()
    regs, seen, nlibs, missing = [], set(), 0, []
    headers, excluded, unclassified, scanned = set(), [], [], set()
    for lib in args.libs:
        c_path = module_source(lib, args.root)
        if not c_path:
            missing.append(lib)
            continue
        nlibs += 1
        header = module_header(lib, c_path, args.root)
        if header:
            headers.add(header)
        else:
            # The generated unit must see the declared types. A module with no
            # header (or a header that does not declare its handlers) is a
            # build failure at the handler's name; say it before the compiler.
            print(f"warning: no header found for {os.path.relpath(c_path, args.root)}; "
                  f"the generated table cannot compile until one declares its "
                  f"handlers", file=sys.stderr)
        for nid, name in scan(c_path):
            scanned.add(name)
            try:
                kind = classify_name(name, known)
            except UnclassifiedName as e:
                unclassified.append(str(e))
                continue
            if kind == "helper":
                excluded.append(name)
                continue
            if nid in seen:
                continue
            seen.add(nid)
            regs.append((nid, name))

    # A name that resolves to no .c file used to print a warning and carry on.
    # That is the worst possible outcome: every NID in that module stays
    # unregistered, so the dispatcher stops the first import with
    # unresolved-nid, and the run is much shorter than it looks. It is also
    # silent -- the warning scrolls past in the build noise. The trap is that
    # these are PRX names, while the files are named after the LIBRARY:
    # `sys_io` is cellMouse.c + cellKb.c, `sys_fs` is cellFs.c, and so on. LBP
    # shipped six such names (sys_io, sys_fs, sys_net, sceNp2,
    # cellSysutilAvconfExt, cellDiscGame) and silently faked all of them --
    # cellMouseInit/cellMouseGetInfo/cellKbInit were fully implemented and never
    # registered. Fail loudly instead, and suggest the file that probably meant.
    if missing:
        have = sorted(os.path.splitext(os.path.basename(p))[0]
                      for p in glob.glob(os.path.join(args.root, "libs", "**", "*.c"),
                                         recursive=True))
        print("error: these module names match no libs/**/<name>.c, so every NID "
              "they export would go unregistered:", file=sys.stderr)
        for lib in missing:
            near = [h for h in have if lib.lower() in h.lower()
                    or h.lower() in lib.lower()]
            hint = f"  did you mean: {', '.join(near)}" if near else \
                   "  no similar file -- implement it or drop it from the list"
            print(f"  {lib}.c NOT FOUND\n{hint}", file=sys.stderr)
        sys.exit(1)

    # Fail-closed: a name whose export status nobody established must not be
    # registered by guess. Registering it would give its computed NID to a
    # title import by accident (the exact bug this classification exists to
    # prevent), so say which names and write nothing.
    if unclassified:
        print("error: cannot classify these scanned definitions as firmware "
              "exports or host-side helpers; refusing to register them:",
              file=sys.stderr)
        for msg in sorted(set(unclassified)):
            print(f"  {msg}", file=sys.stderr)
        print("  Name it like an SDK export (CamelCase after the module), or add "
              "it to NOT_FIRMWARE_EXPORTS with a reason.", file=sys.stderr)
        sys.exit(2)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("/* Auto-generated by gen_hle_nids.py -- do not edit. */\n")
        # The adapter derives every handler's declared C type, so the headers
        # that declare them are included by repository-relative path (the unit
        # is compiled with the toolkit root on the include path). Referencing a
        # handler the header does not declare is a build error at its name.
        f.write('#include "ppu_hle_adapt.hpp"\n')
        for h in sorted(headers):
            f.write(f'#include "{h}"\n')
        if any(name == 'cellSaveDataListAutoLoad' for _, name in regs):
            f.write('struct ppu_context;\n')
            f.write('extern "C" void ps3_savedata_list_auto_load(ppu_context*);\n')
            f.write('extern "C" void ps3_hle_register_ctx(unsigned, const char*, void (*)(ppu_context*));\n')
        f.write('extern "C" void ppu_hle_register_all(void) {\n')
        for nid, name in regs:
            f.write(f'    ps3_hle_register(0x{nid:08X}u, "{name}", {registration(name)});\n')
        if any(name == 'cellSaveDataListAutoLoad' for _, name in regs):
            f.write('    ps3_hle_register_ctx(0x21425307u, "cellSaveDataListAutoLoad", ps3_savedata_list_auto_load);\n')
        f.write("}\n")
    print(f"wrote {args.out}: {len(regs)} NID handlers from {nlibs} module(s); "
          f"scanned {len(scanned)} definition name(s), excluded "
          f"{len(set(excluded))} host-side helper(s)")
    if excluded:
        print("not registered (host-side helper, not a firmware export): "
              + ", ".join(sorted(set(excluded))), file=sys.stderr)


if __name__ == "__main__":
    main()
