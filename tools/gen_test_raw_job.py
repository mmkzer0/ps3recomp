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
BRZ = 0x040
BRSL = 0x066
BRNZ = 0x042
BI = 0x1A8
BISL = 0x1A9
IRET = 0x1AA
BISLED = 0x1AB
BIZ = 0x128
RDCH = 0x00D
WRCH = 0x10D
LQD = 0x034
STQD = 0x024
ORI = 0x004
ANDI = 0x014
AI = 0x01C
ROTMI = 0x079
SHLI = 0x07B
AND = 0x0C1
A = 0x0C0
LQX = 0x1C4
ROTQBY = 0x1DC
NOP = 0x201

# E/D interrupt bits of the bi family (runtime/spu/spu_interp.c:127-132):
# E sets int_enable and D clears it when the branch is taken.
INTERRUPT_ENABLE = 0x40000
INTERRUPT_DISABLE = 0x80000
# `default` is the byte-identical job image; the others add one event branch
# for the static effect inventory tests. The `calls*` variants issue their MFC
# commands from a helper that takes the command word in $r8, so the inventory
# has to prove the command through the image's own call sites. The `flow*`
# variants put one function's command immediate and its `wrch` in different
# blocks, for the function-wide reaching-value pass. The `pic-*` variants and
# `calls-pic-materialised` are position-independent: their immediates are
# image-relative (the image is linked at 0 and staged at `base`), see
# `pic_layout`.
VARIANTS = ('default', 'bisled', 'disable', 'iret',
            'calls', 'calls-computed', 'calls-materialised', 'calls-rewrite',
            'calls-data-word', 'calls-forward', 'calls-forward-computed',
            'calls-dispatch', 'calls-pic-materialised',
            'flow-dominates', 'flow-loop', 'flow-split', 'flow-one-path', 'flow-sibling',
            'flow-dispatch', 'flow-dispatch-pc', 'flow-dispatch-and',
            'flow-dispatch-wide', 'flow-dispatch-outside',
            'flow-loop-live', 'flow-tail-call', 'flow-link-arg',
            'flow-extra-entry', 'flow-ila-landing', 'flow-mid-landing',
            'default-dispatch', 'flow-r0-identity', 'pic-jump-table')
CALL_VARIANTS = ('calls', 'calls-computed', 'calls-materialised', 'calls-rewrite',
                 'calls-data-word', 'calls-forward', 'calls-forward-computed',
                 'calls-dispatch', 'calls-pic-materialised')
FORWARD_VARIANTS = ('calls-forward', 'calls-forward-computed')
FLOW_VARIANTS = ('flow-dominates', 'flow-loop', 'flow-split', 'flow-one-path',
                 'flow-sibling', 'flow-dispatch', 'flow-dispatch-pc',
                 'flow-dispatch-and', 'flow-dispatch-wide', 'flow-dispatch-outside',
                 'flow-loop-live', 'flow-tail-call', 'flow-link-arg',
                 'flow-extra-entry', 'flow-ila-landing', 'flow-mid-landing')

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


def _ri7(op11, i7, ra=0, rt=0):
    return ((op11 & 0x7FF) << 21) | ((i7 & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F)


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


def _dma_helper():
    """A DMA helper: command word in $r8, a fixed transfer, six channel writes.

    It never writes $r8, so its command register still holds the caller's
    argument at the `wrch MFC_Cmd` site -- the shape the interprocedural
    pass is allowed to resolve.
    """
    words = [_ri16(IL, 0, 3), _ri16(IL, 0, 4), _ri16(IL, 0x800, 5),
             _ri16(IL, 128, 6), _ri16(IL, 0, 7)]
    words += [_channel(WRCH, MFC_LSA, 3), _channel(WRCH, MFC_EAH, 4),
              _channel(WRCH, MFC_EAL, 5), _channel(WRCH, MFC_Size, 6),
              _channel(WRCH, MFC_TagID, 7), _channel(WRCH, MFC_Cmd, 8)]
    return words


def _call_body(base, variant):
    """(main words, helper words, helper LS address) of a `calls*` variant.

    One shape decides the helper's address, so the two call displacements and
    the one instruction that names the helper are encoded after the layout is
    known. The mailbox write and the local-store command write stay in main:
    they are what the default image already proves, and neither may move.
    """
    second_arg = (_ri10(LQD, 0, 3, 8) if variant == 'calls-computed'
                  else _ri16(IL, MFC_PUT, 8))
    main = [_ri10(AI, -1, 1, 1), _ri10(STQD, 0, 1, 0),
            _ri16(IL, MFC_GET, 8), 0, second_arg, 0]
    if variant in ('calls-materialised', 'calls-pic-materialised'):
        main.append(0)                                  # ila $r5, <helper>
    main += [_ri16(IL, 0x1234, 10), _channel(WRCH, SPU_WrOutMbox, 10)]
    main += [_ri10(LQD, 0, 0, 11), _channel(WRCH, MFC_Cmd, 11)]
    main += [_ri10(LQD, 0, 1, 0), _ri10(AI, 1, 1, 1), _rr(BI, 0, 0, 0)]
    helper = _dma_helper()
    if variant == 'calls-rewrite':
        helper.insert(0, _ri10(LQD, 0, 3, 8))           # the helper redefines $r8
    helper.append(_rr(BI, 0, 0, 0))
    address = base + CODE_START + 4 * len(main)
    code_start = base + CODE_START
    main[3] = _ri16(BRSL, (address - (code_start + 12)) // 4, 0)
    main[5] = _ri16(BRSL, (address - (code_start + 20)) // 4, 0)
    if variant == 'calls-materialised':
        main[6] = _ri18(ILA, address, 5)
    elif variant == 'calls-pic-materialised':
        # Linked at 0: the immediate is the helper's offset in the image, and
        # the code adds the load delta (`a $r5,$r5,$r126`) before it branches.
        main[6] = _ri18(ILA, address - base, 5)
    return main, helper, address


def _forward_body(base, variant):
    """(main words, helper words, helper LS address) of a forwarding variant.

    The caller passes the command in $r7 and the helper copies it to $r14 with
    `ori $r14, $r7, 0` before the channel writes, so the inventory has to keep
    the copy's provenance to name the register the caller set. The computed
    variant loads the second argument instead of materialising it.
    """
    second_arg = (_ri10(LQD, 0, 3, 7) if variant == 'calls-forward-computed'
                  else _ri16(IL, MFC_PUT, 7))
    main = [_ri10(AI, -1, 1, 1), _ri10(STQD, 0, 1, 0),
            _ri16(IL, MFC_GET, 7), 0, second_arg, 0]
    main += [_ri16(IL, 0x1234, 10), _channel(WRCH, SPU_WrOutMbox, 10)]
    main += [_ri10(LQD, 0, 0, 11), _channel(WRCH, MFC_Cmd, 11)]
    main += [_ri10(LQD, 0, 1, 0), _ri10(AI, 1, 1, 1), _rr(BI, 0, 0, 0)]
    helper = [_ri10(ORI, 0, 7, 14)]                     # ori $r14, $r7, 0
    helper += [_ri16(IL, 0, 3), _ri16(IL, 0, 4), _ri16(IL, 0x800, 5),
               _ri16(IL, 128, 6), _ri16(IL, 0, 10)]
    helper += [_channel(WRCH, MFC_LSA, 3), _channel(WRCH, MFC_EAH, 4),
               _channel(WRCH, MFC_EAL, 5), _channel(WRCH, MFC_Size, 6),
               _channel(WRCH, MFC_TagID, 10), _channel(WRCH, MFC_Cmd, 14)]
    helper.append(_rr(BI, 0, 0, 0))
    address = base + CODE_START + 4 * len(main)
    code_start = base + CODE_START
    main[3] = _ri16(BRSL, (address - (code_start + 12)) // 4, 0)
    main[5] = _ri16(BRSL, (address - (code_start + 20)) // 4, 0)
    return main, helper, address


def call_helper(base=LS_BASE, variant='calls'):
    """LS address of the helper the `calls*` variants call."""
    if variant not in CALL_VARIANTS:
        raise ValueError(f'{variant!r} is not a calls variant')
    if variant in FORWARD_VARIANTS:
        return _forward_body(base, variant)[2]
    return _call_body(base, variant)[2]


def dispatch_entry(base=LS_BASE):
    """LS address of the trailing `bi $r5` a `calls-dispatch` image appends.

    It is one word past every generated function, so a caller lists it as an
    extra entry point to lift it; it exists so the inventory sees an image with
    a register-targeted transfer next to sites it resolves.
    """
    return base + len(build(variant='calls'))


def default_extra_entry(base=LS_BASE):
    """LS address of the words a default-image dispatch variant appends.

    Both `default-dispatch` and `flow-r0-identity` put them past the default
    code's return, so a caller lists this address as an extra entry point.
    """
    return base + len(build(variant='default'))


def pic_layout(base=LS_BASE):
    """Addresses inside the `pic-jump-table` image, as LS addresses.

    `table` is the jump table (its words are TABLE-relative), `cases` the four
    case bodies it names, `pointer` the function whose image-relative address
    an `ila` materialises, and `delta` the load delta the code computes
    (`base`: the image is linked at 0).
    """
    return _pic_body(base)[1]


def _pic_body(base):
    """(words, layout) of the `pic-jump-table` variant.

    The shape a position-independent job compiles to (docs/SPU_LIFTER.md, raw
    manifest `link_base`):

        brsl $r126, .+4 ; ai $r126, $r126, -0x44   <- the load delta
        ila  $r20, <fn - base> ; a $r20,$r20,$r126 ; bisl $r0, $r20
        ila  $r14, <table - base> ; a $r10,$r14,$r126
        shli ; lqx ; rotqby ; a $r2,$r3,$r10 ; bi $r2
        table: case_k - table   (table-relative words)

    Neither the table nor the pointer target is named by an absolute address
    anywhere, so a reader that takes every immediate as an LS address sees
    neither the four case bodies nor the function.
    """
    cases = 4
    code_start = base + CODE_START
    head = 12                                           # words before the table
    table = code_start + 4 * head
    first_case = table + 4 * cases
    case_addrs = [first_case + 8 * k for k in range(cases)]
    pointer = first_case + 8 * cases
    words = [
        _ri16(BRSL, 1, 126),                            # PC-getter
        _ri10(AI, -(CODE_START + 4), 126, 126),         # delta = base
        _ri18(ILA, pointer - base, 20),
        _rr(A, 126, 20, 20),
        _rr(BISL, 0, 20, 0),
        _ri18(ILA, table - base, 14),
        _rr(A, 126, 14, 10),
        _ri7(SHLI, 2, 15, 13),
        _rr(LQX, 10, 13, 11),
        _rr(ROTQBY, 13, 11, 3),
        _rr(A, 10, 3, 2),
        _rr(BI, 0, 2, 0),
    ]
    assert len(words) == head
    words += [(address - table) & 0xFFFFFFFF for address in case_addrs]
    for k in range(cases):
        words += [_ri16(IL, k, 3), _rr(BI, 0, 0, 0)]
    words += [_ri16(IL, 99, 3), _rr(BI, 0, 0, 0)]
    return words, {'table': table, 'cases': case_addrs, 'pointer': pointer, 'delta': base}


def _flow_body(variant, base):
    """Words of a `flow*` variant: one function with internal control flow.

    `flow-dominates` puts the command immediate in a block that dominates the
    site (the branch target is outside the function, so the pipeline lifts the
    function as one chunk and the pass has to reach across blocks);
    `flow-split` defines two different immediates on two paths into the site;
    `flow-one-path` defines it on one path only; `flow-sibling` defines it in
    a branch that never reaches the site. `flow-loop-live` keeps the definition
    on every loop path, so the state at the head must converge instead of
    growing a second atom each iteration; `flow-tail-call` enters a helper both
    by a direct call and by a tail branch; `flow-link-arg` calls through the
    register the helper reads, so the caller's block never set it;
    `flow-extra-entry` enters the site's chunk at a manifest extra entry point;
    `flow-ila-landing` seeds the site's chunk through an `ila` target and
    reaches it with a different command; `flow-mid-landing` reaches the site
    through a computed conditional `biz` whose target (an `ila` of the body's
    start plus an `ai` offset) is no chunk start, so only the dispatch rule can
    see that PUT arrives there. Labels are patched once the layout is known.
    """
    words, labels = [], {}

    def emit(*items):
        words.extend(items)

    def label(name):
        labels[name] = len(words)

    def branch(opcode, name, rt=0):
        words.append((opcode, name, rt))

    if variant == 'flow-dominates':
        emit(_ri16(IL, MFC_GET, 8))
        branch(BRZ, 'elsewhere')
        emit(_channel(WRCH, MFC_Cmd, 8))
        emit(_rr(BI, 0, 0, 0))
        label('elsewhere')
        emit(_rr(BI, 0, 0, 0))
    elif variant == 'flow-loop':
        # The preheader defines the register; the loop head (the site's block)
        # is a branch target, so the block-local run cannot carry the value
        # across, while the preheader dominates every re-entry.
        emit(_ri16(IL, MFC_GET, 8))
        label('loop')
        emit(_channel(WRCH, MFC_Cmd, 8))
        branch(BRZ, 'exit')
        branch(BR, 'loop')
        label('exit')
        emit(_rr(BI, 0, 0, 0))
    elif variant == 'flow-split':
        branch(BRZ, 'left')
        emit(_ri16(IL, MFC_GET, 8))
        branch(BR, 'join')
        label('left')
        emit(_ri16(IL, MFC_PUT, 8))
        label('join')
        emit(_channel(WRCH, MFC_Cmd, 8), _rr(BI, 0, 0, 0))
    elif variant == 'flow-one-path':
        branch(BRZ, 'skip')
        emit(_ri16(IL, MFC_GET, 8))
        label('skip')
        emit(_channel(WRCH, MFC_Cmd, 8), _rr(BI, 0, 0, 0))
    elif variant == 'flow-sibling':
        branch(BRZ, 'side')
        emit(_channel(WRCH, MFC_Cmd, 8), _rr(BI, 0, 0, 0))
        label('side')
        emit(_ri16(IL, MFC_GET, 8), _rr(BI, 0, 0, 0))
    elif variant == 'flow-dispatch':
        # A register-targeted transfer on a reachable path: its successors are
        # unknown, so the region proves nothing.
        branch(BRZ, 'body')
        emit(_rr(BI, ra=5))
        label('body')
        emit(_channel(WRCH, MFC_Cmd, 8), _rr(BI, 0, 0, 0))
    elif variant.startswith('flow-dispatch-'):
        # The masked PC-relative dispatch: `brsl $r7, .+4` gives a constant
        # link, `andi` supplies a small exact offset set, and `bi $r7` jumps
        # into the run after the branch.
        if variant == 'flow-dispatch-outside':
            emit(_channel(WRCH, MFC_Cmd, 8))            # site, live-in
        else:
            emit(_ri16(IL, MFC_GET, 8))                 # the command value
        brsl = len(words)
        emit(0)                                         # brsl $r7, .+4
        if variant == 'flow-dispatch-and':
            emit(_rr(AND, rb=5, ra=6, rt=6))            # a register mask
        else:
            emit(_ri10(ANDI, 127 if variant == 'flow-dispatch-wide' else 112, 6, 6))
        emit(_ri7(ROTMI, -2, 6, 4))                     # rotmi $r4, $r6, -2
        emit(_ri10(AI, 36, 7, 7))                       # ai $r7, $r7, 36
        emit(_rr(A, rb=4, ra=7, rt=7))                  # a $r7, $r7, $r4
        emit(_rr(BI, ra=7))                             # bi $r7
        words[brsl] = _ri16(BRSL, 1, 7)                 # link = pc+4 into $r7
        if variant != 'flow-dispatch-outside':
            first = brsl + 10                           # link + 36 = pc+40
            while len(words) < first + 8:               # padding and the targets
                words.append(_rr(NOP))
            words.append(_channel(WRCH, MFC_Cmd, 8))    # reached from every target
            words.append(_rr(BI, 0, 0, 0))
    elif variant == 'flow-loop-live':
        # The preheader dominates the head and the back edge defines the same
        # constant, so every iteration reaches the site with one value.
        emit(_ri16(IL, MFC_GET, 8))
        branch(BR, 'head')
        label('head')
        emit(_channel(WRCH, MFC_Cmd, 8), _ri16(IL, MFC_GET, 8),
             _channel(RDCH, 29, 4))
        branch(BRNZ, 'head', 4)
        emit(_rr(BI, 0, 0, 0))
    elif variant == 'flow-tail-call':
        # One helper, entered by a direct call with 0x20 and by a tail branch
        # with 0x40: no single caller value proves the site.
        emit(_ri16(IL, MFC_PUT, 8))
        branch(BRSL, 'helper')
        branch(BRSL, 'other')
        emit(_rr(BI, 0, 0, 0))
        label('other')
        emit(_ri16(IL, MFC_GET, 8), (BR, 'helper', 0))
        label('helper')
        emit(_channel(WRCH, MFC_Cmd, 8), _rr(BI, 0, 0, 0))
    elif variant == 'flow-link-arg':
        # `brsl $r8, helper` leaves the return address in the register the
        # helper reads: the call site never set $r8 to a command word.
        emit(_ri16(IL, MFC_GET, 8), (BRSL, 'helper', 8))
        emit(_rr(BI, 0, 0, 0))
        label('helper')
        emit(_channel(WRCH, MFC_Cmd, 8), _rr(BI, ra=8))
    elif variant == 'flow-extra-entry':
        # The site's chunk start is a manifest extra entry point: code can
        # enter there without the preceding immediate running.
        emit(_ri16(IL, MFC_GET, 8))
        label('site')
        emit(_channel(WRCH, MFC_Cmd, 8), _rr(BI, 0, 0, 0))
    elif variant == 'flow-ila-landing':
        # `ila` seeds the site's chunk start as an entry; the seeded path
        # arrives with PUT, the fall-through path with GET.
        emit(_channel(RDCH, 29, 4), (BRNZ, 'dispatch', 4),
             _ri16(IL, MFC_GET, 8))
        label('site')
        emit(_channel(WRCH, MFC_Cmd, 8), _rr(BI, 0, 0, 0))
        label('dispatch')
        emit(_ri16(IL, MFC_PUT, 8), (ILA, 'site', 5), _rr(BI, ra=5))
    elif variant == 'flow-mid-landing':
        # `biz $r4,$r5` jumps over the GET straight to the wrch when $r4 is 0,
        # so the site issues PUT on that path; nothing names the site's address.
        label('start')
        emit(_ri16(IL, MFC_PUT, 8), (ILA, 'start', 5), ('AI_TO', 'site', 'start'),
             _channel(RDCH, 29, 4), _rr(BIZ, 0, 5, 4), _ri16(IL, MFC_GET, 8))
        label('site')
        emit(_channel(WRCH, MFC_Cmd, 8), _rr(BI, 0, 0, 0))
    else:
        raise ValueError(f'{variant!r} is not a flow variant')
    for index, word in enumerate(words):
        if isinstance(word, tuple):
            opcode, name, rt = word
            if opcode == ILA:
                # An absolute target, unlike the PC-relative branch forms.
                words[index] = _ri18(ILA, base + CODE_START + 4 * labels[name], rt)
            elif opcode == 'AI_TO':
                # $r5 += the byte distance from `rt`'s label to `name`'s.
                words[index] = _ri10(AI, 4 * (labels[name] - labels[rt]), 5, 5)
            else:
                words[index] = _ri16(opcode, labels[name] - index, rt)
    return words


def build(base=LS_BASE, tail_words=0, variant='default'):
    """Return one synthetic raw job image at `base`.

    `tail_words` appends `nop` words after the returning code, reachable only
    by the not-taken-at-entry `brnz $r0` in front of the return. It exists so a
    test can observe the lifter's `--code-end` bound: the tail function runs to
    the end of the blob only when no code end is supplied.

    `variant` picks the final control transfer, for the event-branch inventory:
    `bisled` inserts a `bisled $r0, $r2` before the return, `disable` sets the
    D bit on the `bi $r0` return (`bid`), and `iret` replaces the return with
    `iret`. The default image is unchanged. The `calls*` variants instead issue
    both transferred commands from one helper called with the command word in
    $r8 (see `_call_body`); `calls-data-word` additionally appends the helper's
    address as a data word past the code, for a caller that supplies it only
    the `code_end` that ends the code before it. `default-dispatch` and
    `flow-r0-identity` append their transfer past the default code's return
    (see `default_extra_entry`).
    """
    if base % 4:
        raise ValueError('base must be 4-byte aligned')
    if variant not in VARIANTS:
        raise ValueError(f'unknown variant {variant!r}; expected one of {VARIANTS}')
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

    if variant in CALL_VARIANTS:
        if tail_words:
            raise ValueError('tail_words applies to the default image only')
        if variant in FORWARD_VARIANTS:
            main, helper, address = _forward_body(base, variant)
        else:
            main, helper, address = _call_body(base, variant)
        image = bytes(header) + b''.join(_be32(word) for word in main + helper)
        if variant == 'calls-data-word':
            # Aligned and past every generated function, so only the blob-wide
            # data scan sees it.
            image += _be32(address)
        if variant == 'calls-dispatch':
            # `bi $r5`: a register-targeted transfer, reachable only when the
            # image lists its address as an extra entry point.
            image += _be32(_rr(BI, ra=5))
        if base + len(image) > LS_SIZE:
            raise ValueError('image does not fit the local store')
        return image

    if variant == 'pic-jump-table':
        if tail_words:
            raise ValueError('tail_words applies to the default image only')
        image = bytes(header) + b''.join(_be32(word) for word in _pic_body(base)[0])
        if base + len(image) > LS_SIZE:
            raise ValueError('image does not fit the local store')
        return image

    if variant in FLOW_VARIANTS:
        if tail_words:
            raise ValueError('tail_words applies to the default image only')
        image = bytes(header) + b''.join(
            _be32(word) for word in _flow_body(variant, base))
        if base + len(image) > LS_SIZE:
            raise ValueError('image does not fit the local store')
        return image

    code = []
    for cmd in COMMANDS:
        code += _dma(cmd, 0x100, 128)
    # Mailbox write: a job's normal hand-off to the scheduler.
    code += [_ri16(IL, 0x1234, 10), _channel(WRCH, SPU_WrOutMbox, 10)]
    # A command write whose source is loaded from LS, not set by an immediate:
    # the inventory must report this as unresolved rather than drop it.
    code += [_ri10(LQD, 0, 0, 11), _channel(WRCH, MFC_Cmd, 11)]

    if variant == 'bisled':
        # Link in $r0, target $r2: taken only while an event is pending.
        code.append(_rr(BISLED, ra=2))
    if tail_words:
        brnz_addr = base + CODE_START + 4 * len(code)
        tail_start = brnz_addr + 8          # brnz and the transfer after it
        code.append(_ri16(BRNZ, (tail_start - brnz_addr) // 4, 0))
    if variant == 'disable':
        code.append(_rr(BI, 0, 0, 0) | INTERRUPT_DISABLE)   # bid $r0
    elif variant == 'iret':
        code.append(_rr(IRET))              # interrupt return through SRR0
    else:
        code.append(_rr(BI, 0, 0, 0))       # bi $r0: the SPU ABI return
    if tail_words:
        code.extend([_rr(NOP)] * tail_words)
    if variant == 'default-dispatch':
        # `bi $r5`: a register-targeted transfer the block-local pass cannot
        # see as an entry; reachable only through a manifest extra entry.
        code.append(_rr(BI, ra=5))
    elif variant == 'flow-r0-identity':
        # The interrupt-window idiom's shape: `bi $r0` whose nearest write is
        # an identity move over an `ila` is a computed dispatch, not a return.
        # Its target is outside the lifted ranges, so it stays unmodelled.
        code += [_ri18(ILA, 0x3F000, 0), _ri10(AI, 0, 0, 0), _rr(BI, ra=0)]

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
    parser.add_argument('--variant', choices=VARIANTS, default='default',
                        help='event-branch word to add or substitute at the end, or a '
                             'calls*/flow* code shape for the resolution passes')
    parser.add_argument('--manifest', help='also write a --raw-images manifest entry')
    args = parser.parse_args()

    image = build(args.base, args.tail_words, args.variant)
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
