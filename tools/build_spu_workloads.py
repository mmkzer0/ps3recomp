#!/usr/bin/env python3
"""build_spu_workloads.py -- lift a title's SPU images and emit a workload registry.

General, title-agnostic replacement for gen_spu_workloads.py (which hard-coded
flOw's paths/glob/symbol names). For a directory of SPU ELFs it will:

  1. Lift each image with spu_lifter.py under a per-image C *symbol prefix*
     (derived from the image filename) so that dozens of images -- each of which
     defines spu_func_* / spu_recomp_register / a static function table -- link
     together without symbol collisions.
  2. Rewrite the two depth-sensitive relative includes the lifter emits
     ("../../runtime/spu/spu_context.h", ".../spu_helpers.h") to bare includes,
     so the lifted tree can live anywhere and is resolved via a build -I path
     (add <repo>/runtime/spu to the target's include dirs).
  3. Emit a single registration C file that registers every image with the
     runtime workload registry by FNV-1a-64 content fingerprint, under a distinct
     image id, and (optionally) auto-registers via a constructor.

cellSpurs AddWorkload / CreateTask / RunJobChain call spu_workload_dispatch(),
which fingerprints the guest image the game hands to SPURS, finds the lifted
entry registered here, loads it into a fresh local store and runs it. The
fingerprint computed here is byte-identical to runtime/spu/spu_workload.c's
spu_workload_fingerprint() over spu_elf_image_size() bytes, so registrations and
the images the title passes to cellSpurs line up.

A JobQueue2 job image has no ELF container: the PPU stages `sizeBinary << 4`
bytes of guest memory at LS 0x7C00. Those blobs are supplied through
--raw-images, a JSON list of {path, sha256, ls_base, code_end?,
extra_entry_points?}. Their registration fingerprint is the FNV-1a-64 over the
whole staged blob (exactly what the runtime hashes), and the inventory reports
the channels and MFC commands the lifted code contains.

The pipeline's effects inventory of a raw image reaches the runtime through the registration: a
`spu_workload_set_inventory` call carrying whether the image has event branches or unresolved
`wrch MFC_Cmd` sites, its lift's code_end and (when the pipeline recorded one) its lift options
hash. An image the pipeline gave no inventory (an ELF image, an inventory from before event-branch
detection) gets no call and stays unknown to the runtime, which does not record or shadow it.

Registration says an image's lifted body exists, which is enough to capture the image and to replay it
offline (`--tier registered`). Promotion says a corpus proved it: a raw image whose pipeline program
record has `promoted: true` gets a `spu_workload_promote_img` call right after its registration, and
only a promoted image's body is run by a JobQueue2 worker under `spu.job_body=lifted`
(docs/SPU_XDIFF.md section 12.4). An image without the key stays registered and runs interpreted.

Example:
  python tools/build_spu_workloads.py \
      --images <port>/spu/images --lifted <port>/spu \
      --out <port>/spu/spu_workloads.c \
      --register-fn port_spu_register_all --constructor --title <port>

  python tools/build_spu_workloads.py \
      --raw-images <port>/spu/raw_images.json --lifted <port>/spu/raw \
      --out <port>/spu/raw/spu_workloads.c \
      --register-fn port_spu_register_all --title <port>

With --budget-twin every image also gets a budgeted twin (docs/SPU_XDIFF.md
section 10): <lifted>/<image>/spu_recomp_budget.c compiles that image's
spu_recomp.c a second time under -DSPU_LIFT_BUDGET, and the registry registers
the twin beside its production entry (spu_workload_register_twin) under an
image id above all the others. The twin doubles the lifted code a title
compiles and the entries of the function registry it fills, so it is opt-in;
<lifted>/budget_sources.cmake lists the twin sources for CMake.
"""
import argparse, glob, os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_spu_lifter_abi import image_problems
from spu_lifter import TWIN_SUFFIX

# ---- SPU ELF sizing + fingerprint (must match runtime/spu/spu_workload.c) ----

def be16(b, o): return (b[o] << 8) | b[o + 1]
def be32(b, o): return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]

def img_size(b):
    """On-disk extent spanning the section-header table and all PT_LOAD /
    non-NOBITS section content -- matches spu_elf_image_size()."""
    e_phoff = be32(b, 0x1C); e_shoff = be32(b, 0x20)
    e_phentsize = be16(b, 0x2A); e_phnum = be16(b, 0x2C)
    e_shentsize = be16(b, 0x2E); e_shnum = be16(b, 0x30)
    end = e_shoff + e_shnum * e_shentsize
    for k in range(e_phnum):
        po = e_phoff + k * e_phentsize
        if po + 0x14 > len(b): break
        end = max(end, be32(b, po + 0x04) + be32(b, po + 0x10))
    for k in range(e_shnum):
        so = e_shoff + k * e_shentsize
        if so + 0x18 > len(b): break
        if be32(b, so + 0x04) != 8:  # not SHT_NOBITS
            end = max(end, be32(b, so + 0x10) + be32(b, so + 0x14))
    return min(end, len(b))

def fnv1a64(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

def text_fp(b):
    """Fingerprint of the executable PT_LOAD's file bytes -- what lands in local
    store. A raw SPU whose program the PPU streams in by proxy DMA never hands
    the runtime the ELF, only these bytes (runtime/spu/spu_raw.c)."""
    phoff, phes, phn = be32(b, 0x1C), be16(b, 0x2A), be16(b, 0x2C)
    for i in range(phn):
        o = phoff + i * phes
        if be32(b, o) == 1 and be32(b, o + 0x18) & 1:
            off, fsz = be32(b, o + 4), be32(b, o + 0x10)
            return fnv1a64(b[off:off + fsz])
    return None


def is_spu_elf(b):
    return (len(b) >= 0x34 and b[0:4] == b"\x7fELF"
            and b[4] == 1 and b[5] == 2  # ELFCLASS32, ELFDATA2MSB
            and be16(b, 0x12) == 0x17)   # EM_SPU

def sanitize(name):
    s = re.sub(r"[^0-9A-Za-z_]", "_", name)
    if not s or s[0].isdigit():
        s = "img_" + s
    return s

# ---- include rewrite: make the lifted tree location-independent --------------

_INC_RE = re.compile(r'#include\s+"(?:\.\./)+runtime/spu/([A-Za-z0-9_]+\.h)"')

def debase_includes(path):
    try:
        txt = open(path, encoding="utf-8", errors="ignore").read()
    except OSError:
        return
    new = _INC_RE.sub(r'#include "\1"', txt)
    if new != txt:
        open(path, "w", encoding="utf-8", newline="\n").write(new)

# ---- main --------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    inputs = ap.add_mutually_exclusive_group()
    inputs.add_argument("--input", action="append", help="Discover every embedded SPU ELF in supplied files (repeatable)")
    inputs.add_argument("--images", help="dir containing SPU ELFs (*.elf)")
    ap.add_argument("--raw-images", metavar="JSON",
                    help="JSON list of raw local-store job images (a JobQueue2 job blob is "
                         "not an ELF); usable alone or with --input. Each entry names path, "
                         "sha256, ls_base (0x7C00 for JobQueue2 jobs), optional code_end "
                         "(passed to the lifter's --code-end) and optional "
                         "extra_entry_points. A blob whose sha256 does not match the "
                         "manifest fails the run.")
    ap.add_argument("--lifted", required=True, help="output dir for per-image lifted C (one subdir per image)")
    ap.add_argument("--out", required=True, help="output registration C file")
    ap.add_argument("--register-fn", default="spu_register_all",
                    help="name of the emitted registration function")
    ap.add_argument("--title", default="spu", help="label used in comments")
    ap.add_argument("--lifter", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "spu_lifter.py"))
    ap.add_argument("--budget-twin", action="store_true",
                    help="also emit each image's budgeted twin (spu_recomp_budget.c) and register it "
                         "beside the production entry; doubles the compiled lifted code")
    ap.add_argument("--extra-funcs", action="append", default=[],
                    metavar="IMAGE=ADDR[,ADDR...]",
                    help="Extra function entry addresses for one image, passed "
                         "through to spu_lifter --extra-funcs. Repeatable. An SPU "
                         "image entered by an INTERRUPT VECTOR needs this: the "
                         "handler is not reachable from the entry point by any "
                         "branch, so --auto-functions cannot see it, and dispatch "
                         "to it lands on address 0 at runtime. "
                         "e.g. --extra-funcs pm_wwsjob=0xA2C")
    ap.add_argument("--constructor", action="store_true",
                    help="also emit an __attribute__((constructor)) that calls the register fn at startup")
    ap.add_argument("--relift", action="store_true", help="re-lift even if a prior lift exists")
    ap.add_argument("--skip-lift", action="store_true",
                    help="do not run the lifter; assume --lifted already populated (still fixes includes + emits registry)")
    ap.add_argument("--allow-partial", action="store_true",
                    help="With --input, emit supported registrations despite incomplete coverage")
    ap.add_argument("--merge-chunks", action="store_true",
                    help="Pass --merge-chunks to spu_lifter.py: each run of chunks from one "
                         "call target to the next becomes one C function, so branches "
                         "inside it are gotos instead of trampoline round trips")
    ap.add_argument("--handled", metavar="JSON",
                    help="With --input: {sha256: reason} for discovered programs another path "
                         "already provides (an existing registration, an interpreter route). They "
                         "are not lifted here, count as covered, and the reason is recorded")
    ap.add_argument("--image-ids", metavar="JSON",
                    help="With --input: {sha256: image_id} pinning every registered program's "
                         "image id. A registered program missing from the map is an error, so "
                         "ids are reviewed and stable instead of content-derived")
    args = ap.parse_args()
    if args.raw_images and args.images:
        ap.error("--raw-images is a --input pipeline option; it cannot be combined with --images")
    if not args.input and not args.raw_images and not args.images:
        ap.error("one of --input, --raw-images or --images is required")
    # The pipeline calls the writer with its own narrow arguments, so the option
    # reaches it through the writer, not through them.
    def writer(wargs, imgs, text_fps, image_ids=None):
        write_registry(wargs, imgs, text_fps, image_ids, budget_twin=args.budget_twin)
    if args.input or args.raw_images:
        from spu_program_pipeline import build_programs
        sys.exit(build_programs(args, writer))

    elfs = sorted(glob.glob(os.path.join(args.images, "*.elf")))
    if not elfs:
        sys.exit(f"[build_spu_workloads] no *.elf in {args.images}")

    extra_funcs = {}
    for spec in args.extra_funcs:
        if "=" not in spec:
            sys.exit(f"[build_spu_workloads] --extra-funcs wants IMAGE=ADDR, got {spec!r}")
        k, v = spec.split("=", 1)
        extra_funcs[k.strip()] = v.strip()

    imgs = []  # (img, prefix, fingerprint, e_entry)
    text_fps = {}  # img -> executable-segment fingerprint
    for e in elfs:
        b = open(e, "rb").read()
        if not is_spu_elf(b):
            print(f"[build_spu_workloads] skip non-SPU ELF: {e}")
            continue
        img = os.path.splitext(os.path.basename(e))[0]
        prefix = sanitize(img) + "_"
        outdir = os.path.join(args.lifted, img)
        srcc = os.path.join(outdir, "spu_recomp.c")

        if not args.skip_lift and (args.relift or not os.path.exists(srcc)):
            os.makedirs(outdir, exist_ok=True)
            cmd = [sys.executable, args.lifter, e, "--auto-functions", e,
                   "--symbol-prefix", prefix, "-o", outdir]
            if img in extra_funcs:
                cmd += ["--extra-funcs", extra_funcs[img]]
            if args.merge_chunks:
                cmd += ["--merge-chunks"]
                print(f"[build_spu_workloads] {img}: extra entries {extra_funcs[img]}")
            print(f"[build_spu_workloads] LIFT {img}  (prefix {prefix})")
            if subprocess.run(cmd).returncode != 0:
                sys.exit(f"[build_spu_workloads] lift FAILED for {img}")

        # Make the lifted headers/sources location-independent.
        debase_includes(os.path.join(outdir, "spu_recomp.h"))
        debase_includes(srcc)

        sz = img_size(b)
        e_entry = be32(b, 0x18)
        imgs.append((img, prefix, fnv1a64(b[:sz]), e_entry))
        text_fps[img] = text_fp(b)

    if not imgs:
        sys.exit("[build_spu_workloads] no SPU images lifted")

    # Verify each registered entry symbol actually exists in the lifted header.
    for img, prefix, fp, ent in imgs:
        hdr = os.path.join(args.lifted, img, "spu_recomp.h")
        sym = f"{prefix}spu_func_{ent:08X}"
        if os.path.exists(hdr) and f"{sym}(" not in open(hdr, encoding="utf-8", errors="ignore").read():
            print(f"[build_spu_workloads] WARNING: entry symbol {sym} not found for {img} "
                  f"(e_entry=0x{ent:X}); registration may not link")

    try:
        write_registry(args, imgs, text_fps, budget_twin=args.budget_twin)
    except ValueError as error:
        sys.exit(f"[build_spu_workloads] {error}")


TWIN_WRAPPER = """\
/* {name} -- GENERATED by tools/build_spu_workloads.py --budget-twin.
 * The budgeted twin of spu_recomp.c (docs/SPU_XDIFF.md section 10): the same
 * generated source compiled a second time with SPU_LIFT_BUDGET, which counts
 * every transfer that can close a loop and renames the symbols (the header
 * says how). The production compilation of spu_recomp.c is untouched. */
#define SPU_LIFT_BUDGET 1
#include "spu_recomp.c"
"""


def write_twin_sources(lift_root, imgs):
    """Per image, the twin's translation unit; and the CMake list of them."""
    sources = []
    problems = []
    for img, _prefix, _fp, _ent in imgs:
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", img):
            raise ValueError(f"--budget-twin: image name {img!r} is not usable in a CMake source list")
        header = os.path.join(lift_root, img, "spu_recomp.h")
        with open(header, encoding="utf-8", errors="replace") as f:
            if "#ifdef SPU_LIFT_BUDGET" not in f.read():
                problems.append(f"{header}: no budgeted-twin rename block (lift predates --budget-twin)")
    if problems:
        raise ValueError(
            "SPU lift cannot be compiled as a budgeted twin; re-lift the affected images with "
            "this toolkit's tools/spu_lifter.py (or pass --relift):\n  " + "\n  ".join(problems))
    for img, _prefix, _fp, _ent in imgs:
        name = "spu_recomp_budget.c"
        with open(os.path.join(lift_root, img, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(TWIN_WRAPPER.format(name=name))
        sources.append(f'  "${{CMAKE_CURRENT_LIST_DIR}}/{img}/{name}"\n')
    with open(os.path.join(lift_root, "budget_sources.cmake"), "w", encoding="utf-8", newline="\n") as f:
        f.write("set(SPU_BUDGET_TWIN_SOURCES\n" + "".join(sources) + ")\n")


# Bits of spu_workload_inventory.flags (runtime/spu/spu_workload.h), with the names the registry uses.
INVENTORY_FLAGS = ((1, "SPU_WORKLOAD_INV_EVENT_BRANCHES"), (2, "SPU_WORKLOAD_INV_UNRESOLVED_MFC"))

def registration_inventory(program):
    """(flags, code_end, lift_options) for the compact inventory of a registration, from the
    pipeline's record of the image (the entry of inventory.json); None when there is none.

    A record is an inventory only when it carries the effects object with both lists the runtime
    acts on: an absent `event_branches` (an inventory from before commit 3b) does not mean no event
    branches, so it is no inventory. None is not "clean": the runtime refuses an image it has no
    inventory for, which is the safe side of the same doubt."""
    effects = program.get("effects") if program else None
    if not isinstance(effects, dict):
        return None
    event_branches, unresolved = effects.get("event_branches"), effects.get("unresolved")
    if not isinstance(event_branches, list) or not isinstance(unresolved, list):
        return None
    flags = (1 if event_branches else 0) | (2 if unresolved else 0)
    code_end = program.get("code_end") or 0
    lift_options = program.get("lift_options")
    return flags, code_end, int(lift_options, 16) if lift_options else 0


def program_promoted(program):
    """True only for a program record whose `promoted` is the JSON true. Any other value (absent, a
    string, 1) is not a promotion: a promoted image is run by spu.job_body=lifted in place of the
    interpreter, so the safe reading of a doubtful record is the interpreter."""
    return bool(program) and program.get("promoted") is True


def write_registry(args, imgs, text_fps, image_ids=None, budget_twin=False):
    programs = getattr(args, "programs", None) or {}
    # Every path here may reuse code instead of lifting it: an existing
    # spu_recomp.c, --skip-lift, a caller-supplied --lifter, or the pipeline's
    # staged output. Validate before a registry exists, so a pre-stamp image
    # cannot link into a registration that never checks the ABI. The pipeline
    # passes a narrow args object without `lifted`; its registry lives in the
    # staged lift root, beside the image directories.
    lift_root = getattr(args, "lifted", None) or os.path.dirname(os.path.abspath(args.out))
    problems = []
    for img, _prefix, _fp, _ent in imgs:
        problems.extend(image_problems(os.path.join(lift_root, img)))
    if problems:
        raise ValueError(
            "SPU lift ABI validation failed; re-lift the affected images with "
            "this toolkit's tools/spu_lifter.py (or pass --relift):\n  "
            + "\n  ".join(problems))
    ids = {img: (image_ids[img] if image_ids else index) for index, (img, _p, _f, _e) in enumerate(imgs, 1)}
    # The twin runs under an image id of its own, so that its indirect branches
    # resolve to its functions and never leave it for the production body;
    # ids above every production id cannot collide with a pinned one.
    twin_ids = {img: max(ids.values()) + n for n, (img, _p, _f, _e) in enumerate(imgs, 1)} if budget_twin else {}
    if budget_twin:
        write_twin_sources(lift_root, imgs)
    L = []
    L.append(f"/* {os.path.basename(args.out)} -- GENERATED by tools/build_spu_workloads.py.")
    L.append(f" * Registers the {len(imgs)} lifted SPU images of \"{args.title}\" with the workload")
    L.append(" * dispatch registry (by FNV-1a-64 image fingerprint) and populates each image's")
    L.append(" * indirect-branch table under a distinct image id. cellSpurs AddWorkload/")
    L.append(" * CreateTask/RunJobChain call spu_workload_dispatch(), which fingerprints the")
    L.append(" * guest image, finds the lifted entry here, and runs it on a fresh local store.")
    L.append(" * Build note: add <repo>/runtime/spu to this target's include dirs (the lifted")
    L.append(' * sources use bare #include "spu_context.h" etc.). */')
    L.append('#include "spu_workload.h"')
    L.append("")
    L.append("extern void spu_begin_image(int image_id);")
    L.append("")
    L.append("/* per-image extern decls (prefixed entry fn + recomp_register) */")
    for img, prefix, fp, ent in imgs:
        L.append(f"extern void {prefix}spu_func_{ent:08X}(spu_context*);")
        L.append(f"extern void {prefix}spu_recomp_register(void);")
        if budget_twin:
            L.append(f"extern void {prefix}spu_func_{ent:08X}{TWIN_SUFFIX}(spu_context*);")
            L.append(f"extern void {prefix}spu_recomp_register{TWIN_SUFFIX}(void);")
    L.append("")
    L.append(f"void {args.register_fn}(void)")
    L.append("{")
    for index, (img, prefix, fp, ent) in enumerate(imgs, 1):
        i = ids[img]
        L.append(f"    spu_begin_image({i}); {prefix}spu_recomp_register();")
        L.append(f"    spu_workload_register_img(0x{fp:016X}ULL, {prefix}spu_func_{ent:08X}, {i}, \"{img}\");")
        if text_fps.get(img):  # same entry, keyed by the text segment (raw SPU proxy-DMA load)
            L.append(f"    spu_workload_register_img(0x{text_fps[img]:016X}ULL, {prefix}spu_func_{ent:08X}, {i}, \"{img}.text\");")
        inventory = registration_inventory(programs.get(img))
        if inventory:  # no record: the image is unknown to the runtime, which refuses to record or shadow it
            flags, code_end, lift_options = inventory
            names = [name for bit, name in INVENTORY_FLAGS if flags & bit]
            L.append(f"    spu_workload_set_inventory(0x{fp:016X}ULL, {' | '.join(names) or '0u'}, "
                     f"0x{code_end:X}u, 0x{lift_options:016X}ULL);")
        if program_promoted(programs.get(img)):  # the corpus proved this image: spu.job_body=lifted may run it
            L.append(f"    spu_workload_promote_img(0x{fp:016X}ULL);")
        if budget_twin:
            t = twin_ids[img]
            L.append(f"    spu_begin_image({t}); {prefix}spu_recomp_register{TWIN_SUFFIX}();")
            L.append(f"    spu_workload_register_twin(0x{fp:016X}ULL, {prefix}spu_func_{ent:08X}{TWIN_SUFFIX}, {t});")
            if text_fps.get(img):
                L.append(f"    spu_workload_register_twin(0x{text_fps[img]:016X}ULL, {prefix}spu_func_{ent:08X}{TWIN_SUFFIX}, {t});")
    L.append("    spu_begin_image(0);")
    L.append("}")
    if args.constructor:
        L.append("")
        L.append("/* Auto-register at startup (before the game adds any SPURS workload/task). */")
        L.append(f"__attribute__((constructor)) static void {args.register_fn}_ctor(void)")
        L.append("{")
        L.append(f"    {args.register_fn}();")
        L.append("}")
    L.append("")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, "w", encoding="utf-8", newline="\n").write("\n".join(L))
    print(f"[build_spu_workloads] wrote {args.out} with {len(imgs)} images "
          f"(register fn: {args.register_fn}{', +constructor' if args.constructor else ''})")

if __name__ == "__main__":
    main()
