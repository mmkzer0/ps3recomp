#!/usr/bin/env python3
"""Turn one synthetic PPC ELF into the metadata, flat image and lifted C the
call-level differential harness consumes.

    make_synth.py --elf synth.elf --out BUILD/synth \\
                  --lifter tools/ppu_lifter.py --gen-tables tools/diff_harness/gen_tables.py

Only synthetic input belongs here: the ELF is built from tests/diff_harness/synth
by a PPC-capable clang, and nothing in this path is firmware-derived. The output
layout mirrors the firmware modules so the harness sees the same shapes:

    <out>/lift/synth/synth_{image.bin,exports.json,imports.json,functions.json}
    <out>/lift/synth_c/synth.{c,h}, synth_harness_{tables,layout}.cpp

Exports are the `.opd` symbols named synth_nid_<8 hex digits> (ELFv1 keeps a
function descriptor in .opd); imports are the `.text` symbols named
synth_import_<8 hex digits> whose body is the SCE stub shape."""
import argparse
import json
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.realpath(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from elf_parser import ELFFile, PT_LOAD, SHT_SYMTAB  # noqa: E402

SYMBOL = struct.Struct(">IBBHQQ")     # Elf64_Sym
NID_SYM = re.compile(r"^synth_nid_([0-9A-Fa-f]{8})$")
IMP_SYM = re.compile(r"^synth_import_([0-9A-Fa-f]{8})$")


def be32(buf, off):
    return struct.unpack(">I", buf[off:off + 4])[0]


def decode_slot(image, base, stub_ea):
    """The SCE import-stub shape: li r12,0 ; oris r12,r12,hi ; lwz r12,lo(r12)."""
    o = stub_ea - base
    if o < 0 or o + 12 > len(image):
        return 0
    i0, i1, i2 = be32(image, o), be32(image, o + 4), be32(image, o + 8)
    if i0 != 0x39800000 or (i1 >> 16) != 0x658C or (i2 >> 16) != 0x818C:
        return 0
    hi, lo = i1 & 0xFFFF, i2 & 0xFFFF
    if lo & 0x8000:
        lo -= 0x10000
    return ((hi << 16) + lo) & 0xFFFFFFFF


def read_symbols(elf):
    """[(name, section_name, value, type)] from the ELF symtab."""
    for i, sh in enumerate(elf.section_headers):
        if sh.sh_type != SHT_SYMTAB:
            continue
        symtab = elf.get_section_data(i)
        strtab = elf.get_section_data(sh.sh_link)
        out = []
        for off in range(0, len(symtab) - SYMBOL.size + 1, SYMBOL.size):
            st_name, st_info, _st_other, st_shndx, st_value, _st_size = SYMBOL.unpack_from(symtab, off)
            end = strtab.find(b"\0", st_name)
            name = strtab[st_name:end].decode("latin1") if end >= 0 else ""
            sec = elf.section_headers[st_shndx].name_str if st_shndx < len(elf.section_headers) else ""
            out.append((name, sec, st_value, st_info & 0xF))
        return out
    raise SystemExit("make_synth: no SHT_SYMTAB; link without --strip")


def section(elf, name):
    for i, sh in enumerate(elf.section_headers):
        if sh.name_str == name:
            return i, sh
    raise SystemExit(f"make_synth: missing section {name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--out", required=True, help="build directory for metadata and lifted C")
    ap.add_argument("--lifter", default=os.path.join(ROOT, "tools", "ppu_lifter.py"))
    ap.add_argument("--gen-tables", default=os.path.join(ROOT, "tools", "diff_harness", "gen_tables.py"))
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x02000000)
    ap.add_argument("--name", default="synth")
    ap.add_argument("--prefix", default="synth_")
    a = ap.parse_args()

    elf = ELFFile(a.elf)
    elf.load()
    if not elf.elf_header.is64 or not elf.elf_header.big_endian or elf.elf_header.e_machine != 21:
        raise SystemExit("make_synth: input must be ELF64 big-endian PowerPC64")

    text_i, text_sh = section(elf, ".text")
    opd_i, opd_sh = section(elf, ".opd")
    opd_data = elf.get_section_data(opd_i)
    syms = read_symbols(elf)

    meta_dir = os.path.join(a.out, "lift", a.name)
    lifted_dir = os.path.join(a.out, "lift", a.name + "_c")
    os.makedirs(meta_dir, exist_ok=True)
    os.makedirs(lifted_dir, exist_ok=True)

    # ---- flat image from the PT_LOAD segments -----------------------------
    loads = [ph for ph in elf.program_headers if ph.p_type == PT_LOAD and ph.p_filesz > 0]
    if not loads:
        raise SystemExit("make_synth: no loadable segments")
    lo = min(ph.p_vaddr for ph in elf.program_headers if ph.p_type == PT_LOAD)
    hi = max(ph.p_vaddr + ph.p_memsz for ph in elf.program_headers if ph.p_type == PT_LOAD)
    if lo != a.base:
        raise SystemExit(f"make_synth: image base 0x{lo:X} does not match --base 0x{a.base:X}")
    image = bytearray(hi - lo)
    for i, ph in enumerate(elf.program_headers):
        if ph.p_type != PT_LOAD or ph.p_filesz == 0:
            continue
        data = elf.get_segment_data(i)
        image[ph.p_vaddr - lo: ph.p_vaddr - lo + len(data)] = data
    for s in elf.section_headers:
        if s.sh_type == 8 and s.sh_size + s.sh_addr > hi:   # SHT_NOBITS
            raise SystemExit("make_synth: .bss outside the loadable image")
    # ---- exports: .opd descriptors named synth_nid_<nid> -------------------
    exports, code_seeds, toc_votes, opd_entries = {}, [], {}, []
    for name, sec, value, _typ in syms:
        m = NID_SYM.match(name)
        if not m or sec != ".opd":
            continue
        off = value - opd_sh.sh_addr
        if off < 0 or off + 16 > len(opd_data):
            raise SystemExit(f"make_synth: {name} outside .opd")
        code, toc = struct.unpack_from(">QQ", opd_data, off)
        if not (text_sh.sh_addr <= code < text_sh.sh_addr + text_sh.sh_size):
            raise SystemExit(f"make_synth: export {name} code 0x{code:X} outside .text")
        nid = int(m.group(1), 16)
        exports[nid] = value
        code_seeds.append(code)
        opd_entries.append((value, code, toc))
        toc_votes[toc] = toc_votes.get(toc, 0) + 1
    if not exports:
        raise SystemExit("make_synth: no synth_nid_<nid> symbols in .opd")
    toc = int(max(toc_votes.items(), key=lambda kv: kv[1])[0])
    for name, _sec, value, _typ in syms:
        if name == ".TOC." and value:
            toc = value
    # base of the TOC is what a raw lift expects; use the descriptor value as-is

    # The firmware's relocated image stores every OPD as two big-endian 32-bit
    # words (the PRX relocation types are ADDR32), and the harness's loader reads
    # it that way. An ELFv1 link emits 64-bit pointers, so normalize the flat
    # image to the convention the loader consumes. The ELF itself is unchanged
    # for the lifter.
    for opd, code, opd_toc in opd_entries:
        struct.pack_into(">II", image, opd - a.base, code & 0xFFFFFFFF, opd_toc & 0xFFFFFFFF)
    image_path = os.path.join(meta_dir, a.name + "_image.bin")
    open(image_path, "wb").write(image)

    # ---- imports: .text stubs named synth_import_<nid> ---------------------
    imports, slot_syms = {}, {}
    for name, sec, value, _typ in syms:
        m = IMP_SYM.match(name)
        if not m or sec != ".text":
            continue
        nid = int(m.group(1), 16)
        slot = decode_slot(image, a.base, value)
        if not slot:
            raise SystemExit(f"make_synth: {name} is not an SCE-shaped import stub")
        imports[nid] = value
        slot_syms[nid] = slot
    for name, sec, value, _typ in syms:
        m = re.match(r"^synth_import_slot_([0-9A-Fa-f]{8})$", name)
        if m and sec == ".synth_import_slots":
            nid = int(m.group(1), 16)
            if nid in slot_syms and slot_syms[nid] != value:
                raise SystemExit(f"make_synth: stub slot 0x{slot_syms[nid]:X} != symbol 0x{value:X}")

    # ---- function seeds ----------------------------------------------------
    text_end = text_sh.sh_addr + text_sh.sh_size
    for name, sec, value, typ in syms:
        if sec == ".text" and name.startswith("synth_") and typ in (0, 2):
            code_seeds.append(value)
    seeds = sorted(set(code_seeds))
    seeds = [s for s in seeds if text_sh.sh_addr <= s < text_end]
    funcs = []
    for i, s in enumerate(seeds):
        e = seeds[i + 1] if i + 1 < len(seeds) else text_end
        funcs.append({"start": f"0x{s:08X}", "end": f"0x{e:08X}"})

    # ---- metadata ----------------------------------------------------------
    json.dump({"module": a.name, "base": f"0x{a.base:08X}", "toc": f"0x{toc:08X}",
               "exports": {"synth": {f"{nid:08X}": f"0x{opd:08X}" for nid, opd in sorted(exports.items())}},
               "opd_to_code": {f"0x{opd:08X}": f"0x{code:08X}" for opd, code in zip(exports.values(), code_seeds)}},
              open(os.path.join(meta_dir, a.name + "_exports.json"), "w"), indent=1)
    json.dump({"synth": {f"{nid:08X}": f"0x{stub:08X}" for nid, stub in sorted(imports.items())}},
              open(os.path.join(meta_dir, a.name + "_imports.json"), "w"), indent=1)
    json.dump(funcs, open(os.path.join(meta_dir, a.name + "_functions.json"), "w"), indent=1)

    # ---- lift with the toolkit's own lifter --------------------------------
    cmd = [sys.executable, "-I", a.lifter, image_path, "--raw", "--base", f"0x{a.base:08X}",
           "--toc", f"0x{toc:08X}", "--functions", os.path.join(meta_dir, a.name + "_functions.json"),
           "--symbol-prefix", a.prefix, "--header-name", "synth.h", "--source-name", "synth.c",
           "--single-file", "--output", lifted_dir]
    print("+ " + " ".join(cmd))
    if subprocess.run(cmd).returncode:
        return 2

    # The synthetic output may live inside the repository build tree: it is not
    # firmware-derived, so gen_tables gets its explicit synthetic switch.
    cmd = [sys.executable, a.gen_tables, "--synthetic", "--name", a.name, "--prefix", a.prefix,
           "--header", "synth.h", "--exports", os.path.join(meta_dir, a.name + "_exports.json"),
           "--imports", os.path.join(meta_dir, a.name + "_imports.json"), "--image", image_path,
           "--out", lifted_dir]
    print("+ " + " ".join(cmd))
    if subprocess.run(cmd).returncode:
        return 2

    print(f"make_synth: {a.name}: {len(exports)} exports, {len(imports)} imports, "
          f"{len(funcs)} function seeds, image {len(image)} bytes, TOC 0x{toc:08X} -> {meta_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
