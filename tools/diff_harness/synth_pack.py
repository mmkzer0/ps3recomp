#!/usr/bin/env python3
"""Package a cross-linked synthetic PPU module for the differential harness.

The synthetic modules (tests/diff_harness/synth/syn_c*.c) are built from OUR OWN
source with a stock powerpc64-linux-gnu toolchain and linked flat at a fixed
base below 4 GiB (syn_gcc.ld). This tool turns the linked ELF into exactly what
tools/lift_prx.py produces for a firmware PRX, so the rest of the pipeline
(ppu_lifter.py --raw, gen_tables.py, the harness) cannot tell the difference:

    <name>_image.bin       flat image at --base (PT_LOAD segments, bss zero-filled)
                           followed by the export descriptor table
    <name>_functions.json  function seeds [{start,end}] for ppu_lifter.py --raw
    <name>_exports.json    {module, base, toc, exports:{lib:{nid:opd_ea}}, opd_to_code}
    <name>_imports.json    {lib:{nid:stub_code_ea}}
    <name>_names.json      {nid: symbol name} for reports (gen_tables.py --names)

Why not make a PRX that lift_prx.py accepts: that needs SCE_PPURELA relocation
records, a sys_process_prx_param/module-info block and export/import tables, i.e.
a PRX writer, to test a relocator that is not the subject here. The harness and
the lifter consume the image and the JSON, which this tool writes directly.

What this tool does about the ABI (see docs/DIFF_HARNESS_SYNTHETIC.md): the
toolchain is ELFv1, whose function descriptors are 24 bytes of 64-bit words;
the PS3 PPU ABI calls through 8-byte {u32 code, u32 toc} descriptors. So

  * code addresses come from the ELFv1 descriptors (ld drops the dot symbols),
    or from a dot/plain symbol that sits in an executable section (hand-written
    import stubs);
  * the PS3-style descriptors are written here, after the image, one per
    export, with the module TOC read from the linked descriptors (all must agree);
  * everything is checked to lie below 4 GiB with 32-bit-clean addresses.

NIDs are the toolkit's own algorithm over the symbol name (tools/nid_database.py),
so they cannot collide with real firmware NIDs by construction of the names.
"""
import argparse, json, os, struct, sys

ROOT = os.path.realpath(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from nid_database import compute_nid  # noqa: E402

EM_PPC64 = 0x15
SHF_EXECINSTR = 4
STT_FUNC = 2
PT_LOAD = 1


class Elf:
    def __init__(self, path):
        d = self.d = open(path, "rb").read()
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 2:
            sys.exit("%s: not an ELF64 big-endian file" % path)
        (e_type, e_machine, _v, self.entry, e_phoff, e_shoff, _fl, _eh, e_phentsize, e_phnum,
         e_shentsize, e_shnum, e_shstrndx) = struct.unpack(">HHIQQQIHHHHHH", d[16:64])
        if e_machine != EM_PPC64:
            sys.exit("%s: not a PPC64 object (e_machine 0x%X)" % (path, e_machine))
        if e_type != 2:
            sys.exit("%s: not a linked executable (ET_EXEC expected, got %d)" % (path, e_type))
        self.loads = []
        for i in range(e_phnum):
            p_type, _fl, p_off, p_vaddr, _pa, p_filesz, p_memsz, _al = struct.unpack(
                ">IIQQQQQQ", d[e_phoff + i * e_phentsize: e_phoff + i * e_phentsize + 56])
            if p_type == PT_LOAD:
                self.loads.append((p_off, p_vaddr, p_filesz, p_memsz))
        self.sections = []
        for i in range(e_shnum):
            o = e_shoff + i * e_shentsize
            n, t, fl, addr, off, size, link, info, al, es = struct.unpack(">IIQQQQIIQQ", d[o:o + 64])
            self.sections.append(dict(name_off=n, type=t, flags=fl, addr=addr, off=off, size=size, link=link, entsize=es))
        shstr = self.sections[e_shstrndx]
        for s in self.sections:
            s["name"] = self.cstr(shstr["off"] + s["name_off"])
        self.symbols = []
        for s in self.sections:
            if s["type"] == 2:      # SHT_SYMTAB
                strtab = self.sections[s["link"]]
                for k in range(s["size"] // 24):
                    n, info, other, shndx, value, size = struct.unpack(">IBBHQQ", d[s["off"] + k * 24: s["off"] + k * 24 + 24])
                    self.symbols.append(dict(name=self.cstr(strtab["off"] + n), type=info & 15, bind=info >> 4,
                                             shndx=shndx, value=value, size=size))

    def cstr(self, off):
        return self.d[off:self.d.index(b"\0", off)].decode("ascii", "replace")

    def sec_of(self, ea):
        for s in self.sections:
            if s["flags"] & 2 and s["addr"] <= ea < s["addr"] + s["size"]:
                return s
        return None

    def read(self, ea, n):
        for off, vaddr, filesz, memsz in self.loads:
            if vaddr <= ea and ea + n <= vaddr + memsz:
                if ea + n > vaddr + filesz:
                    return bytes(n)          # bss
                return self.d[off + ea - vaddr: off + ea - vaddr + n]
        sys.exit("address 0x%X is not inside a PT_LOAD segment" % ea)


def nid_header(manifest, path):
    lines = ["/* GENERATED by tools/diff_harness/synth_pack.py --emit-nid-header. Do not edit. */",
             "#ifndef SYN_NIDS_H", "#define SYN_NIDS_H"]
    for kind in ("exports", "imports"):
        for n in manifest[kind]:
            lines.append("#define SYN_NID_%s 0x%08Xu" % (n, compute_nid(n)))
    lines += ["#endif", ""]
    open(path, "w").write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", required=True, help="JSON: {library, exports:[names], imports:[names]}")
    ap.add_argument("--emit-nid-header", help="write the NID #defines and stop (no ELF needed)")
    ap.add_argument("--elf")
    ap.add_argument("--name", help="module name, e.g. libsyn_c (output file stem)")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0x02000000)
    ap.add_argument("--out", help="output directory")
    a = ap.parse_args()
    manifest = json.load(open(a.manifest))
    if a.emit_nid_header:
        nid_header(manifest, a.emit_nid_header)
        return 0
    if not (a.elf and a.name and a.out):
        ap.error("--elf, --name and --out are required")

    elf = Elf(a.elf)
    base = a.base
    top = max(v + m for _, v, _, m in elf.loads)
    low = min(v for _, v, _, _ in elf.loads)
    if low != base:
        sys.exit("lowest PT_LOAD is at 0x%X, expected the base 0x%X (check SYN_BASE)" % (low, base))
    if top >= 1 << 32:
        sys.exit("image reaches 0x%X: not below 4 GiB" % top)
    img = bytearray(top - base)
    for off, vaddr, filesz, memsz in elf.loads:
        img[vaddr - base: vaddr - base + filesz] = elf.d[off: off + filesz]

    # --- code addresses ------------------------------------------------------
    code_of = {}      # name -> code ea
    size_of = {}      # code ea -> size
    sizeless = set()  # code ea of FUNC symbols without a size (linker-made helpers such as _restgpr0_30)
    toc_vals = set()
    for s in elf.symbols:
        if s["type"] != STT_FUNC or s["shndx"] in (0, 0xFFF1):
            continue
        sec = elf.sections[s["shndx"]]
        name = s["name"]
        if sec["flags"] & SHF_EXECINSTR:                  # dot symbol / hand-written stub
            ea = s["value"]
            code_of.setdefault(name.lstrip("."), ea)
            if not s["size"]:
                sizeless.add(ea)
        elif sec["name"] == ".opd":                       # ELFv1 descriptor: {code, toc, env}
            ea, toc = struct.unpack(">QQ", elf.read(s["value"], 16))
            if ea == 0:
                continue
            code_of.setdefault(name.lstrip("."), ea)
            toc_vals.add(toc)
        else:
            continue
        if s["size"]:
            size_of[ea] = max(size_of.get(ea, 0), s["size"])
    if len(toc_vals) != 1:
        sys.exit("the exported functions use %d different TOC values (%s); a PS3 module has one"
                 % (len(toc_vals), ", ".join(hex(t) for t in sorted(toc_vals))))
    toc = toc_vals.pop()
    if not (base <= toc < 1 << 32):
        sys.exit("TOC 0x%X is not a 32-bit address inside the image range" % toc)

    lib = manifest["library"]
    exports, opd_to_code = {}, {}
    opd_off = (len(img) + 15) & ~15
    img.extend(bytes(opd_off - len(img)))
    for name in manifest["exports"]:
        if name not in code_of:
            sys.exit("export %s: no function symbol in %s" % (name, a.elf))
        code = code_of[name]
        if code & 3 or code >= 1 << 32:
            sys.exit("export %s: code address 0x%X is not a word-aligned 32-bit address" % (name, code))
        opd = base + len(img)
        img.extend(struct.pack(">II", code, toc))        # the PS3 descriptor: {u32 code, u32 toc}
        exports["0x%08X" % compute_nid(name)] = "0x%08X" % opd
        opd_to_code["0x%08X" % opd] = "0x%08X" % code
    imports = {}
    for name in manifest["imports"]:
        if name not in code_of:
            sys.exit("import %s: no stub symbol (.%s) in %s" % (name, name, a.elf))
        imports["0x%08X" % compute_nid(name)] = "0x%08X" % code_of[name]

    # --- function seeds: every sized function in an executable section -------
    # (a size-less function symbol, e.g. the out-of-line register save/restore helpers ld adds
    # for -Os code, runs to the next function symbol or the end of its section)
    seeds = []
    starts = sorted(set(size_of) | sizeless)
    for ea in starts:
        sec = elf.sec_of(ea)
        if not (sec and sec["flags"] & SHF_EXECINSTR and ea & 3 == 0):
            continue
        if size_of.get(ea):
            end = ea + size_of[ea]
        else:
            later = [x for x in starts if x > ea]
            end = min(later[0] if later else sec["addr"] + sec["size"], sec["addr"] + sec["size"])
        seeds.append({"start": "0x%08X" % ea, "end": "0x%08X" % end})
    os.makedirs(a.out, exist_ok=True)
    p = lambda suffix: os.path.join(a.out, a.name + suffix)
    open(p("_image.bin"), "wb").write(bytes(img))
    json.dump(seeds, open(p("_functions.json"), "w"), indent=1)
    json.dump({"module": a.name, "base": "0x%08X" % base, "toc": "0x%08X" % toc,
               "exports": {lib: exports}, "opd_to_code": opd_to_code}, open(p("_exports.json"), "w"), indent=1)
    json.dump({lib: imports}, open(p("_imports.json"), "w"), indent=1)
    json.dump({"0x%08X" % compute_nid(n): n for n in manifest["exports"] + manifest["imports"]},
              open(p("_names.json"), "w"), indent=1)
    print("synth_pack: %s: image %d bytes @0x%08X, TOC 0x%08X, %d exports, %d imports, %d function seeds"
          % (a.name, len(img), base, toc, len(exports), len(imports), len(seeds)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
