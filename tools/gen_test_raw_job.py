#!/usr/bin/env python3
"""Synthetic raw SPURS job image for the discovery -> registration pipeline.

A JobQueue2 job's image is not an ELF: the PPU copies `sizeBinary << 4` bytes
of guest memory straight to LS 0x7C00 (runtime/spu/spurs_job.c, staging) and
enters at the base. The registration pipeline therefore needs an input form
that names the blob, its LS base and its code/data boundary instead of parsing
an ELF. This builder emits a stand-in with the same *shape* as a jq2 job
header, so the pipeline's raw path is exercised without an SPU toolchain and
without any guest bytes:

  0x00  four `ila` words        (the executed immediate prefix)
  0x10  a branch stub           (jumps over the header data)
  0x30  "JOBCRT Ver14"          (the CRT marker the runtime gates on)
  0x3C  the trailer offset word
  0x40  code: GET, PUT, GETLLAR/PUTLLC, a mailbox write, plus one command
        write whose value is not statically known

Everything is little more than encodings from tools/spu_disasm.py, so the
lifter accepts every word with zero unsupported instructions.

CLI: python3 tools/gen_test_raw_job.py out.bin [--manifest out.json]
"""
import argparse
import hashlib
import json
import struct

LS_BASE = 0x7C00          # runtime/spu/spu_workload.h SPU_JQ2_IMAGE_LS
LS_SIZE = 256 * 1024
CODE_START = 0x40         # end of the synthetic job header
CRT_MARKER = b'JOBCRT Ver14'   # 12 bytes, at header+0x30

IL = 0x081
ILA = 0x021
BR = 0x064
BRNZ = 0x042
BI = 0x1A8
RDCH = 0x00D
WRCH = 0x10D
LQD = 0x034
NOP = 0x201

MFC_LSA, MFC_EAH, MFC_EAL, MFC_Size, MFC_TagID, MFC_Cmd = 16, 17, 18, 19, 20, 21
MFC_RdAtomicStat, SPU_WrOutMbox = 27, 28
MFC_PUT, MFC_GET, MFC_PUTLLC, MFC_GETLLAR = 0x20, 0x40, 0xB4, 0xD0
# Program order: the resolved command writes, then the deliberate unknown.
COMMANDS = (MFC_GET, MFC_PUT, MFC_GETLLAR, MFC_PUTLLC)


def _be32(value):
    return struct.pack('>I', value & 0xFFFFFFFF)


def _ri16(op9, i16, rt=0):
    return ((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F)


def _ri18(op7, i18, rt=0):
    return ((op7 & 0x7F) << 25) | ((i18 & 0x3FFFF) << 7) | (rt & 0x7F)


def _ri10(op8, i10, ra=0, rt=0):
    return ((op8 & 0xFF) << 24) | ((i10 & 0x3FF) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F)


def _rr(op11, rb=0, ra=0, rt=0):
    return ((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F)


def _channel(op11, channel, rt):
    # rdch/wrch/rchcnt carry the channel number in the full 7-bit RA field.
    return ((op11 & 0x7FF) << 21) | ((channel & 0x7F) << 7) | (rt & 0x7F)


def _dma(cmd, lsa, size):
    """Six channel writes that issue one MFC command, command code in $r8."""
    words = [
        _ri16(IL, lsa, 3),
        _ri16(IL, 0, 4),
        _ri16(IL, 0x800, 5),
        _ri16(IL, size, 6),
        _ri16(IL, 0, 7),
        _ri16(IL, cmd, 8),
    ]
    words += [_channel(WRCH, MFC_LSA, 3), _channel(WRCH, MFC_EAH, 4),
              _channel(WRCH, MFC_EAL, 5), _channel(WRCH, MFC_Size, 6),
              _channel(WRCH, MFC_TagID, 7), _channel(WRCH, MFC_Cmd, 8)]
    if cmd in (MFC_GETLLAR, MFC_PUTLLC):
        # The lock-line result is read before the next atomic command.
        words.append(_channel(RDCH, MFC_RdAtomicStat, 9))
    return words


def build(base=LS_BASE, tail_words=0):
    """Return one synthetic raw job image at `base`.

    `tail_words` appends `nop` words after the returning code, reachable only
    by the not-taken-at-entry `brnz $r0` in front of the return. It exists so a
    test can observe the lifter's `--code-end` bound: the tail function runs to
    the end of the blob only when no code end is supplied.
    """
    if base % 4:
        raise ValueError('base must be 4-byte aligned')
    header = bytearray(CODE_START)
    # The four executed immediate words real jq2 images carry at +0x00. Their
    # immediates are deliberately outside this image: the detector seeds every
    # in-code il/ila target as a possible continuation address, and a header
    # word that pointed into the marker data would lift that data as code.
    for offset in range(0, 0x10, 4):
        struct.pack_into('>I', header, offset, _ri18(ILA, 0x3F000 + offset, 2))
    # Branch over the header data to the code at +0x40.
    struct.pack_into('>I', header, 0x10, _ri16(BR, (CODE_START - 0x10) // 4))
    header[0x30:0x30 + len(CRT_MARKER)] = CRT_MARKER
    struct.pack_into('>I', header, 0x3C, CODE_START - 0x10)

    code = []
    for cmd in COMMANDS:
        code += _dma(cmd, 0x100, 128)
    # Mailbox write: a job's normal hand-off to the scheduler.
    code += [_ri16(IL, 0x1234, 10), _channel(WRCH, SPU_WrOutMbox, 10)]
    # A command write whose source is loaded from LS, not set by an immediate:
    # the inventory must report this as unresolved rather than drop it.
    code += [_ri10(LQD, 0, 0, 11), _channel(WRCH, MFC_Cmd, 11)]

    if tail_words:
        brnz_addr = base + CODE_START + 4 * len(code)
        tail_start = brnz_addr + 8          # brnz and the `bi $r0` after it
        code.append(_ri16(BRNZ, (tail_start - brnz_addr) // 4, 0))
    code.append(_rr(BI, 0, 0, 0))           # bi $r0: the SPU ABI return
    if tail_words:
        code.extend([_rr(NOP)] * tail_words)

    image = bytes(header) + b''.join(_be32(word) for word in code)
    if base + len(image) > LS_SIZE:
        raise ValueError('image does not fit the local store')
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', help='path of the raw job image to write')
    parser.add_argument('--base', type=lambda x: int(x, 0), default=LS_BASE)
    parser.add_argument('--tail-words', type=int, default=0,
                        help='append N nop words reachable only via a not-taken branch')
    parser.add_argument('--manifest', help='also write a --raw-images manifest entry')
    args = parser.parse_args()

    image = build(args.base, args.tail_words)
    with open(args.output, 'wb') as out:
        out.write(image)
    if args.manifest:
        entry = {'path': args.output, 'sha256': hashlib.sha256(image).hexdigest(),
                 'ls_base': hex(args.base), 'code_end': hex(args.base + len(image))}
        with open(args.manifest, 'w', encoding='utf-8') as out:
            json.dump([entry], out, indent=2)
            out.write('\n')
    print(f'wrote {args.output}: {len(image)} bytes at LS 0x{args.base:X}, '
          f'sha256 {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
