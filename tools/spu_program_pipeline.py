"""Validated discovery and fresh generation for build_spu_workloads --input.

Uses the existing detector, lifter CLI and registry emitter. Inputs are
embedded SPU ELFs discovered by spu_elf.scan_spu_candidates and, through the
--raw-images manifest, flat local-store job images with no ELF container.
Owns its entire output directory; manifests and logs remain available after
failed generation.
"""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from spu_disasm import CHANNEL_NAMES, SPU_RR, disassemble_spu
from spu_elf import fnv1a64, scan_spu_candidates


def sha256(buf):
    return hashlib.sha256(buf).hexdigest()


def code_ranges(info):
    # The loader applies every PT_LOAD in header order, including zero filling.
    # Lifting a segment in isolation is unsafe if another load can replace its
    # bytes. Treat any overlapping memory layout as unsupported until generation
    # explicitly models the final deployed local store. Adjacent/empty loads
    # remain valid.
    loads = sorted((p['addr'], p['addr'] + p['memsz']) for p in info['loads'] if p['memsz'])
    if any(b > c for (_, b), (c, _) in zip(loads, loads[1:])):
        raise ValueError('overlapping PT_LOAD memory ranges (including BSS) are unsupported')
    executable = info['executable']
    if len(executable) != 1:
        raise ValueError('automated lifting requires exactly one executable PT_LOAD')
    segment = executable[0]
    start, end = segment['addr'], segment['addr'] + segment['filesz']
    sections = [s for s in info['sections'] if s['flags'] & 4 and s['size']]
    ranges = sorted((s['addr'], s['addr'] + s['size']) for s in sections) or [(start, end)]
    if any(a < start or b > end for a, b in ranges):
        raise ValueError('executable sections outside supported text segment')
    if any(b > c for (_, b), (c, _) in zip(ranges, ranges[1:])):
        raise ValueError('overlapping executable sections are unsupported')
    if not any(a <= info['entry'] < b for a, b in ranges):
        raise ValueError('ELF entry is not in an executable file range')
    return ranges


# ---- raw local-store images ------------------------------------------------
#
# A JobQueue2 job image is not an ELF: the PPU stages `sizeBinary << 4` bytes
# at LS 0x7C00 and enters at the base (runtime/spu/spurs_job.c). Its content
# identity is therefore the FNV-1a-64 over the whole staged blob, not over an
# ELF extent, and its function boundaries come from find_spu_functions.py's
# raw mode. The manifest carries what the bytes cannot: the LS base and the
# optional code/data boundary the lifter's --code-end needs.

def _manifest_int(value, field, source):
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            pass
    raise ValueError(f'{source}: {field} is not an integer')


def load_raw_images(manifest_path):
    """Validate a --raw-images manifest and return its entries.

    Every field is checked before anything is lifted: a manifest that names
    the wrong bytes is a configuration error, not partial coverage, so it
    fails the run even with --allow-partial.
    """
    path = Path(manifest_path).resolve()
    try:
        text = path.read_text()
    except OSError as error:
        raise ValueError(f'--raw-images {path}: {error}')
    try:
        entries = json.loads(text)
    except ValueError as error:
        raise ValueError(f'--raw-images {path}: invalid JSON: {error}')
    if not isinstance(entries, list) or not entries:
        raise ValueError('--raw-images requires a non-empty JSON list')
    bad_entry = next((index for index, entry in enumerate(entries)
                      if not isinstance(entry, dict)), None)
    if bad_entry is not None:
        raise ValueError(f'--raw-images {path}[{bad_entry}]: entry is not an object')
    out = []
    for index, entry in enumerate(entries):
        source = f'--raw-images {path}[{index}]'
        blob_name = entry.get('path')
        if not isinstance(blob_name, str) or not blob_name:
            raise ValueError(f'{source}: path must be a non-empty string')
        blob_path = Path(blob_name)
        if not blob_path.is_absolute():
            # Relative paths resolve against the manifest, so the manifest and
            # the images it names move together.
            blob_path = path.parent / blob_path
        digest = entry.get('sha256')
        if not isinstance(digest, str) or not re.fullmatch(r'[0-9A-Fa-f]{64}', digest):
            raise ValueError(f'{source}: sha256 must be 64 hex digits')
        digest = digest.lower()
        if 'ls_base' not in entry:
            raise ValueError(f'{source}: ls_base is required')
        base = _manifest_int(entry['ls_base'], 'ls_base', source)
        if not 0 <= base < 0x40000 or base & 3:
            raise ValueError(f'{source}: ls_base 0x{base:X} is not a 4-byte-aligned '
                             'local-store address')
        try:
            blob = blob_path.read_bytes()
        except OSError as error:
            raise ValueError(f'{source}: {error}')
        if not blob:
            raise ValueError(f'{source}: image is empty')
        actual = sha256(blob)
        if actual != digest:
            raise ValueError(f'{source}: sha256 mismatch for {blob_path}: '
                             f'manifest {digest}, image {actual}')
        end = base + len(blob)
        if end > 0x40000:
            raise ValueError(f'{source}: image ends at LS 0x{end:X}, past the local store')
        code_end = None
        if entry.get('code_end') is not None:
            code_end = _manifest_int(entry['code_end'], 'code_end', source)
            if code_end > end:
                raise ValueError(f'{source}: code_end 0x{code_end:X} is past the image '
                                 f'end 0x{end:X}')
            if code_end <= base or code_end & 3:
                raise ValueError(f'{source}: code_end 0x{code_end:X} must be a '
                                 f'4-byte-aligned address after ls_base 0x{base:X}')
        extra = entry.get('extra_entry_points', [])
        if not isinstance(extra, list):
            extra = None
        if extra is None:
            raise ValueError(f'{source}: extra_entry_points must be a list')
        limit = code_end if code_end is not None else end
        points = sorted({_manifest_int(value, 'extra_entry_points', source) for value in extra})
        for point in points:
            if not (base <= point < limit) or point & 3:
                raise ValueError(f'{source}: extra entry point 0x{point:X} is outside '
                                 'the code range')
        # Both flags change what a port does with the image, so a truthy
        # non-boolean is a manifest mistake, never an implicit true.
        flags = {}
        for field in ('return_entries', 'promoted'):
            value = entry.get(field, False)
            if type(value) is not bool:
                raise ValueError(f'{source}: {field} must be a boolean')
            flags[field] = value
        out.append({'digest': digest, 'blob': blob, 'path': str(blob_path), 'size': len(blob),
                    'ls_base': base, 'code_end': code_end, 'extra_entry_points': points,
                    'return_entries': flags['return_entries'], 'promoted': flags['promoted']})
    return out


# Names mirror runtime/spu/spu_context.h's MFC_*_CMD constants.
MFC_COMMAND_NAMES = {
    0x20: 'PUT', 0x21: 'PUTB', 0x22: 'PUTF', 0x24: 'PUTL', 0x25: 'PUTLB',
    0x26: 'PUTLF', 0x40: 'GET', 0x41: 'GETB', 0x42: 'GETF', 0x44: 'GETL',
    0x45: 'GETLB', 0x46: 'GETLF', 0x89: 'SDCRZ', 0xA0: 'SNDSIG', 0xB0: 'PUTLLUC',
    0xB4: 'PUTLLC', 0xB8: 'PUTQLLUC', 0xC0: 'BARRIER', 0xC8: 'EIEIO',
    0xCC: 'SYNC', 0xD0: 'GETLLAR',
}

_CHANNEL_ACCESS = {'rdch': 'read', 'wrch': 'write', 'rchcnt': 'count'}
_CONTROL_FLOW = {'br', 'bra', 'brsl', 'brasl', 'brz', 'brnz', 'brhz', 'brhnz',
                 'bi', 'bisl', 'biz', 'binz', 'bihz', 'bihnz', 'stop', 'stopd',
                 'iret', 'hbr', 'hbra', 'hbrr'}
# Instructions whose low 7 bits are a source register, not the destination.
# Everything else clears a tracked register conservatively: a false negative
# only leaves a site unresolved, a false positive would claim a command value
# the code may not write.
_NO_RT_WRITE = {'stqd', 'stqa', 'stqr', 'stqx', 'nop', 'lnop', 'sync', 'syncc',
                'dsync', 'wrch', 'mtspr', 'fscrwr'}
# RRR-format ops write bits 21-27; the low 7 bits are their RC source.
_RRR_DEST = {'selb', 'shufb', 'mpya', 'fnms', 'fma', 'fms'}
# Bi-family opcode values from the decoder table (spu_disasm.py SPU_RR):
# bi, bisl, iret, bisled and biz/binz/bihz/bihnz. The E (0x40000) and D
# (0x80000) bits set/clear int_enable when the branch is taken
# (runtime/spu/spu_interp.c:127-132, mirrored by spu_lifter.py's `_ied`).
# `iret` returns from an interrupt and `bisled` branches only on a pending
# event, so either is an event branch even without its own E/D bit.
_BI_FAMILY = frozenset(SPU_RR[opcode] for opcode in
                       (0x1A8, 0x1A9, 0x1AA, 0x1AB, 0x128, 0x129, 0x12A, 0x12B))
_ALWAYS_EVENT = frozenset({'bisled', 'iret'})
_INTERRUPT_ENABLE = 0x40000
_INTERRUPT_DISABLE = 0x80000
# Link-setting branches write their RT (the ABI's $r0 link register) and call
# another function, whose body may write any caller-visible register: both the
# register write and that clobber follow from the same set.
_LINK_WRITE = frozenset({'brsl', 'brasl', 'bisl', 'bisled'})
# Direct branches whose decoded operands name an absolute target, mirroring
# find_spu_functions._BRANCH_MNEMONICS: the detector seeds functions from
# exactly these numbers, so they are also the complete set of references the
# call-site pass can check. Every other branch computes its target from a
# register and cannot be followed.
_CONDITIONAL_BRANCHES = frozenset({'brz', 'brnz', 'brhz', 'brhnz'})
_UNCONDITIONAL_BRANCHES = frozenset({'br', 'bra'})
_DIRECT_CALLS = frozenset({'brsl', 'brasl'})
_DIRECT_BRANCHES = _CONDITIONAL_BRANCHES | _UNCONDITIONAL_BRANCHES | _DIRECT_CALLS
# Conditional bi-family forms: a register-targeted branch with a fall-through.
_CONDITIONAL_BI = frozenset({'biz', 'binz', 'bihz', 'bihnz'})
# A register-directed dispatch is modelled only when its target set is exact
# and small enough to enumerate.
_DISPATCH_VALUES = 64


def _written_register(ins):
    """The register an instruction writes, or None.

    Same instruction classification as the constant tracker below, so the two
    cannot disagree about what invalidates a tracked value.
    """
    mnemonic = ins.mnemonic
    if mnemonic in _CHANNEL_ACCESS:
        return None if mnemonic == 'wrch' else ins.raw & 0x7F
    if mnemonic in _CONTROL_FLOW:
        return ins.raw & 0x7F if mnemonic in _LINK_WRITE else None
    if mnemonic in _RRR_DEST:
        return (ins.raw >> 21) & 0x7F
    if mnemonic in _NO_RT_WRITE:
        return None
    return ins.raw & 0x7F


def _operand_addresses(ins):
    """Absolute addresses named in a branch's decoded operands.

    Mirrors find_spu_functions.collect_branch_targets, which seeds function
    starts from these same numbers.
    """
    return [int(token, 16) for token in (t.strip() for t in ins.operands.split(','))
            if token.startswith('0x')]


def _immediate_address(insns, target):
    """(site, mnemonic) of the first instruction that forms `target`, else None.

    A function reached through a register that code materialised (`ila`, an
    `il`, or `ilhu`+`iohl`) can be entered without a direct call, so a set of
    call sites would not describe every entry. The scan is linear and never
    resets at a branch: it over-approximates the pairs a real block would
    form, and an over-approximation only keeps a site unresolved.
    """
    high = {}
    for ins in insns:
        mnemonic, register = ins.mnemonic, ins.raw & 0x7F
        if mnemonic == 'ila':
            value = (ins.raw >> 7) & 0x3FFFF
        elif mnemonic == 'il':
            value = _il_value(ins)
        elif mnemonic == 'ilhu':
            high[register] = ((ins.raw >> 7) & 0xFFFF) << 16
            if high[register] == target:
                return ins.addr, mnemonic
            continue
        elif mnemonic == 'iohl':
            low = (ins.raw >> 7) & 0xFFFF
            value = high.pop(register, None)
            if value is None:
                # The high half is not visible here, so this iohl may still
                # have completed the target: treat a matching low half as
                # evidence rather than claim the function is only called.
                if low == target & 0xFFFF:
                    return ins.addr, mnemonic
                continue
            value |= low
        else:
            written = _written_register(ins)
            if written is not None:
                high.pop(written, None)
            continue
        if value == target:
            return ins.addr, mnemonic
        high.pop(register, None)
    return None


def _data_word(blob, target):
    """First aligned 32-bit word in `blob` equal to `target`, else None.

    A function-pointer table holds entry addresses; the bytes cannot say
    whether such a word is data or an instruction operand, so every occurrence
    counts as an indirect reference.
    """
    for offset in range(0, len(blob) - 3, 4):
        if int.from_bytes(blob[offset:offset + 4], 'big') == target:
            return offset
    return None


def _merge_overlapping(ranges):
    """Collapse generated ranges that overlap into their union.

    A merged lift reports the group and its chunks, and a sized ELF symbol can
    span a later function's start; the union is then the one function the site
    belongs to. Analysing the union adds paths (a chunk's exit into the next
    chunk), which can only make the pass more conservative. Adjacent ranges
    stay separate: a chunk's entry is a function boundary, not a fall-through.
    """
    merged = []
    for start, end in sorted(ranges):
        if merged and start < merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def _il_value(ins):
    """`il`'s sign-extended 16-bit immediate as the 32-bit register value."""
    value = (ins.raw >> 7) & 0xFFFF
    return (value - 0x10000 if value & 0x8000 else value) & 0xFFFFFFFF


def _falls_through(ins):
    """Whether execution may continue at the next address (a region edge)."""
    mnemonic = ins.mnemonic
    if mnemonic in _LINK_WRITE or mnemonic in _CONDITIONAL_BRANCHES:
        return True
    if mnemonic in _CONDITIONAL_BI:
        return True
    if mnemonic in _UNCONDITIONAL_BRANCHES or mnemonic in ('bi', 'iret'):
        return False
    if mnemonic in ('stop', 'stopd'):
        from find_spu_functions import is_resuming_stop
        return is_resuming_stop(ins)
    return True


def _bi_register(ins):
    """The register a bi-family branch transfers to, or None.

    The decoded operands carry it in RA for `bi` and for the conditional
    forms; reading RT (zero in many encodings) instead would turn a `bi $r5`
    into a return.
    """
    if ins.mnemonic == 'bi' or ins.mnemonic in _CONDITIONAL_BI:
        return (ins.raw >> 7) & 0x7F
    return None


def _identity_source(ins):
    """Source register of `ori $rD, $rS, 0` / `ai $rD, $rS, 0`, or None.

    Both forms copy the source unchanged, and a helper that forwards an
    argument before issuing a command does exactly this, so the value at the
    site is the caller's argument in the source register.
    """
    if ins.mnemonic not in ('ori', 'ai'):
        return None
    operands = [token.strip() for token in ins.operands.split(',')]
    if len(operands) != 3 or operands[2] not in ('0', '0x0'):
        return None
    return (ins.raw >> 7) & 0x7F


def _pc_getter(ins):
    """Whether a call only loads its link (`brsl $rN, .+4`)."""
    return (ins.mnemonic in _DIRECT_CALLS
            and _operand_addresses(ins) == [ins.addr + 4])


def _value_sum(left, right):
    """Sums of two value sets, or None when either is unknown or too large."""
    if left is None or right is None or len(left) * len(right) > _DISPATCH_VALUES:
        return None
    return frozenset((a + b) & 0xFFFFFFFF for a in left for b in right)


def _masked_values(source, mask):
    """`andi` over a value set: exact over the 32-bit domain when unknown."""
    if source is not None:
        values = frozenset(value & mask for value in source)
        return values if len(values) <= _DISPATCH_VALUES else None
    if (mask & 0xFFFFFFFF).bit_count() > 6:             # 2**bits > the bound
        return None
    # An unknown 32-bit source masked with a constant yields exactly the
    # subsets of the mask's bits, whatever the source was.
    values = frozenset()
    value = mask & 0xFFFFFFFF
    while True:
        values |= frozenset({value})
        if not value:
            return values
        value = (value - 1) & mask


def _shifted_values(source, mnemonic, amount):
    """Apply a constant shift to every value of a set (`shli`/`roti`/`rotmi`)."""
    if source is None:
        return None
    if mnemonic == 'shli':
        shift = amount & 0x3F
        values = {0 if shift > 31 else (value << shift) & 0xFFFFFFFF for value in source}
    elif mnemonic == 'roti':
        shift = amount & 0x1F
        values = {((value << shift) | (value >> (32 - shift))) & 0xFFFFFFFF
                  if shift else value for value in source}
    else:
        shift = (0 - amount) & 0x3F
        values = {0 if shift > 31 else value >> shift for value in source}
    return frozenset(values) if len(values) <= _DISPATCH_VALUES else None


def _dispatch_targets(code, index, cuts, contains):
    """Exact targets of the register-directed branch at `code[index]`, or None.

    The branch register is evaluated over the straight-line run ending at the
    branch, which may start at a `brsl $rN, .+4` PC-getter (the link is a
    constant and nothing is called). The run stops at `cuts`: chunk starts
    that code outside the straight line can enter, and every target of a
    branch other than a PC-getter. A cut is image-wide, because a target
    inside one range can be reached from another range or from code outside
    the lifted ranges. Every value set is exact over the 32-bit domain: an
    immediate, the image of `andi` with a constant mask through constant
    shifts, and `ai`/`a` sums of those. Anything else makes the value
    unknown; an empty set, one larger than `_DISPATCH_VALUES`, an unaligned
    target, or a target outside the region keeps the branch opaque.
    """
    register = _bi_register(code[index])
    if register is None:
        return None
    start = index
    while start > 0:
        previous = code[start - 1]
        if previous.addr + 4 != code[start].addr:
            break
        if code[start].addr in cuts:
            break
        if previous.mnemonic in _CONTROL_FLOW and not _pc_getter(previous):
            break
        start -= 1
    values = {}
    for ins in code[start:index]:
        if ins.mnemonic in _LINK_WRITE:
            if _pc_getter(ins):
                values[ins.raw & 0x7F] = frozenset({ins.addr + 4})
            else:
                values.clear()                          # a real call clobbers
            continue
        destination = _written_register(ins)
        if destination is None:
            continue
        mnemonic = ins.mnemonic
        source = _identity_source(ins)
        if source is not None:
            values[destination] = values.get(source)
        elif mnemonic == 'il':
            values[destination] = frozenset({_il_value(ins)})
        elif mnemonic == 'ila':
            values[destination] = frozenset({(ins.raw >> 7) & 0x3FFFF})
        elif mnemonic == 'andi':
            mask = (ins.raw >> 14) & 0x3FF
            mask = (mask - 0x400 if mask & 0x200 else mask) & 0xFFFFFFFF
            values[destination] = _masked_values(values.get((ins.raw >> 7) & 0x7F), mask)
        elif mnemonic in ('shli', 'roti', 'rotmi'):
            amount = (ins.raw >> 14) & 0x7F             # RI7: signed 7-bit
            amount = amount - 0x80 if amount & 0x40 else amount
            values[destination] = _shifted_values(values.get((ins.raw >> 7) & 0x7F),
                                                  mnemonic, amount)
        elif mnemonic == 'ai':
            amount = (ins.raw >> 14) & 0x3FF
            amount = (amount - 0x400 if amount & 0x200 else amount) & 0xFFFFFFFF
            values[destination] = _value_sum(values.get((ins.raw >> 7) & 0x7F),
                                             frozenset({amount}))
        elif mnemonic == 'a':
            values[destination] = _value_sum(values.get((ins.raw >> 7) & 0x7F),
                                             values.get((ins.raw >> 14) & 0x7F))
        else:
            values[destination] = None
    targets = values.get(register)
    if not targets or len(targets) > _DISPATCH_VALUES:
        return None
    if any(target & 3 or not contains(target) for target in targets):
        return None
    return frozenset(targets)


def _indirect_dispatch(insns, modelled=(), computed_jumps=()):
    """Whether the image contains an unmodelled register-targeted transfer.

    Such a transfer can enter a generated function at its start, which is an
    entry the call-site pass cannot see; every resolution then rests on the
    named assumption that it does not. Returns are not transfers in this
    sense: the lifter emits them as host returns. `computed_jumps` is the
    lifter's own computed set, so a branch this pass does not model but the
    generated code dispatches still counts as a transfer.
    """
    for ins in insns:
        if ins.addr in modelled:
            continue
        if ins.mnemonic in ('bisl', 'bisled', 'iret'):
            return True
        target = _bi_register(ins)
        if target is not None and (target != 0 or ins.addr in computed_jumps):
            return True
    return False


def _flow_regions(insns, ranges):
    """Group lifted ranges into the branch-connected regions they form.

    A raw lift generates one chunk per branch target, so one procedure is many
    ranges. Two ranges are the same region when a direct branch in one targets
    the other's start, or when the first ends on an instruction that can fall
    through into the next range. Unrelated neighbours stay separate.
    """
    by_addr = {ins.addr: ins for ins in insns}
    starts = {start: index for index, (start, _end) in enumerate(ranges)}
    parent = list(range(len(ranges)))

    def find(index):
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    def union(left, right):
        left, right = find(left), find(right)
        if left != right:
            parent[right] = left

    for index, (start, end) in enumerate(ranges):
        code = [by_addr[address] for address in range(start, end, 4) if address in by_addr]
        if code and end in starts and _falls_through(code[-1]):
            union(index, starts[end])
        for ins in code:
            # A call is not an edge: the callee is another region, and the
            # analysis treats it as a clobber. A direct branch or a tail
            # transfer does continue in the same register flow.
            if ins.mnemonic in _DIRECT_BRANCHES and ins.mnemonic not in _DIRECT_CALLS:
                for target in _operand_addresses(ins):
                    if target in starts:
                        union(index, starts[target])
    groups = {}
    for index in range(len(ranges)):
        groups.setdefault(find(index), []).append(ranges[index])
    return list(groups.values())


class _FlowRegion:
    """Blocks and register values of one branch-connected group of ranges.

    Calls are the only barrier: the callee is another lifted region, so every
    register it may write is unknown at the return. The successor model is the
    lifter's own: a bi-family branch to $r0 is the ABI return unless the
    generated code emits an indirect dispatch for that address
    (compute_bi_r0_jumps), branch hints are no-ops, and a non-resuming stop
    ends the path. An indirect dispatch has unknown successors, so a region
    that reaches one proves nothing.

    Reaching values are atoms: a constant with its definition site, the entry
    value of a register (with the register it came from and the entry block it
    entered at, so a copy keeps its provenance), or a non-constant definition
    with its site. Equal facts are the same atom, so the join is a set union
    and the analysis terminates.

    The caller supplies the image-wide facts: `chunk_starts` (so a merged
    chunk cannot hide a landing site), `external` (the chunk starts code
    outside the region can reach), `cuts` (the addresses a dispatch run must
    not look past) and `computed_jumps` (the lifter's own dispatch set).
    """

    def __init__(self, insns, ranges, chunk_starts=(), external=(), cuts=(),
                 computed_jumps=()):
        by_addr = {ins.addr: ins for ins in insns}
        self.code = [by_addr[address] for start, end in ranges
                     for address in range(start, end, 4) if address in by_addr]
        position = {ins.addr: index for index, ins in enumerate(self.code)}
        # The lifter's own computed-jump set, so a branch it dispatches but
        # this pass cannot evaluate is still a transfer with unknown targets.
        self.computed_jump = set(computed_jumps)
        # A dispatch whose register this pass can evaluate exactly becomes a
        # set of known successors; every other one stays opaque.
        self.dispatch_targets = {}
        cut_addresses = set(cuts)
        for index, ins in enumerate(self.code):
            register = _bi_register(ins)
            if register is None or (register == 0 and ins.addr not in self.computed_jump):
                continue
            targets = _dispatch_targets(self.code, index, cut_addresses,
                                        position.__contains__)
            if targets is not None:
                self.dispatch_targets[ins.addr] = targets
        self.modelled = set(self.dispatch_targets)
        leaders = {0}
        for index, ins in enumerate(self.code):
            if index + 1 < len(self.code) and ins.mnemonic in _CONTROL_FLOW:
                leaders.add(index + 1)
            if ins.mnemonic in _DIRECT_BRANCHES:
                leaders.update(position[target] for target in _operand_addresses(ins)
                               if target in position)
        for targets in self.dispatch_targets.values():
            leaders.update(position[target] for target in targets)
        # A chunk start inside the region is a possible landing site: with
        # merged chunks the next chunk starts mid-block, so the dispatch and
        # caller-site passes must not see it as one straight line.
        leaders.update(position[address] for address in chunk_starts
                       if address in position)
        ordered = sorted(leaders)
        self.blocks = [self.code[first:last]
                       for first, last in zip(ordered, ordered[1:] + [len(self.code)])]
        self.block_of = {ins.addr: index for index, block in enumerate(self.blocks)
                         for ins in block}
        self.successors = [self._successors(index) for index in range(len(self.blocks))]
        # An entry is a chunk start code outside the region may enter without
        # the region's own dataflow: a call target, a branch target from
        # another region, the image entry, an extra entry point, a return
        # entry, or an address materialised as an immediate or stored as a
        # data word. A block with predecessors is an entry too; its live atoms
        # join the incoming states. Every other no-predecessor block is code
        # only an unmodelled transfer can reach, which the `assumes` note
        # covers, not an entry to invent values for.
        self.external = set(external)
        self.entries = [index for index in range(len(self.blocks))
                        if self.blocks[index][0].addr in self.external]
        self.opaque = self._first_opaque()
        self.values = {}

    def _successors(self, index):
        last = self.blocks[index][-1]
        mnemonic = last.mnemonic
        fall = self.block_of.get(last.addr + 4)
        target = None
        if mnemonic in _DIRECT_BRANCHES:
            target = next((self.block_of[address] for address in _operand_addresses(last)
                           if address in self.block_of), None)
        if mnemonic in _LINK_WRITE:                     # the callee returns here
            return (fall,) if fall is not None else ()
        if mnemonic in _CONDITIONAL_BRANCHES:
            return tuple(edge for edge in (fall, target) if edge is not None)
        if mnemonic in _UNCONDITIONAL_BRANCHES:         # outside the region: leaves
            return (target,) if target is not None else ()
        if mnemonic in ('hbr', 'hbra', 'hbrr'):         # hints are no-ops
            return (fall,) if fall is not None else ()
        if mnemonic == 'bi' or mnemonic in _CONDITIONAL_BI:
            if last.addr in self.dispatch_targets:
                edges = {self.block_of[address]
                         for address in self.dispatch_targets[last.addr]}
                if mnemonic != 'bi' and fall is not None:
                    edges.add(fall)
                return tuple(sorted(edges))
            if _bi_register(last) != 0 or last.addr in self.computed_jump:
                return None                             # an indirect dispatch
            return (fall,) if mnemonic != 'bi' and fall is not None else ()
        if mnemonic in ('stop', 'stopd'):
            from find_spu_functions import is_resuming_stop
            return (fall,) if is_resuming_stop(last) and fall is not None else ()
        if mnemonic == 'iret':
            return None                                 # through SRR0
        return (fall,) if fall is not None else ()

    def _first_opaque(self):
        """Lowest opaque instruction reachable from an entry, or None."""
        opaque = None
        reached = set(self.entries)
        work = list(self.entries)
        while work:
            index = work.pop()
            edges = self.successors[index]
            if edges is None:
                last = self.blocks[index][-1]
                if opaque is None or last.addr < opaque.addr:
                    opaque = last
                continue
            for edge in edges:
                if edge not in reached:
                    reached.add(edge)
                    work.append(edge)
        return opaque

    def tracked(self, register):
        """Registers whose values flow into `register` through copies."""
        tracked = {register}
        changed = True
        while changed:
            changed = False
            for ins in self.code:
                if _written_register(ins) in tracked:
                    source = _identity_source(ins)
                    if source is not None and source not in tracked:
                        tracked.add(source)
                        changed = True
        return tracked

    def atoms_at(self, register, site):
        """Atoms of `register` just before `site`; None when unreachable."""
        state = self._states(register)
        block = self.block_of.get(site)
        if block is None or block not in state:
            return None
        before = next(index for index, ins in enumerate(self.blocks[block])
                      if ins.addr == site)
        return self._transfer(block, state[block], before).get(register, frozenset())

    def _states(self, register):
        if register in self.values:
            return self.values[register]
        tracked = self.tracked(register)
        state = {}
        for entry in self.entries:
            state[entry] = {name: frozenset({('live', name, self.blocks[entry][0].addr)})
                            for name in tracked}
        work = list(self.entries)
        while work:
            index = work.pop()
            edges = self.successors[index]
            if edges is None:
                continue
            out = self._transfer(index, state[index])
            for edge in edges:
                current = state.get(edge)
                if current is None:
                    state[edge] = dict(out)
                    work.append(edge)
                    continue
                # Progress is a state that grows. A state can also acquire
                # the same number of different atoms as the incoming one
                # (live-at-a-call-target versus a constant, say); that is not
                # growth, and treating it as growth would never terminate.
                if not all(out[name] <= current[name] for name in tracked):
                    current.update({name: current[name] | out[name]
                                    for name in tracked})
                    work.append(edge)
        self.values[register] = state
        return state

    def _transfer(self, index, state, upto=None):
        state = dict(state)
        for ins in self.blocks[index][:upto]:
            if ins.mnemonic in _LINK_WRITE:
                if _pc_getter(ins):
                    # `brsl $rN, .+4` only loads the link: the generated code
                    # calls nothing, so no other register changes.
                    if (ins.raw & 0x7F) in state:
                        state[ins.raw & 0x7F] = frozenset(
                            {('const', ins.addr + 4, ins.addr)})
                    continue
                # The callee writes what it likes before returning.
                for name in state:
                    state[name] = frozenset({('unknown', ins.addr, 'call')})
                continue
            destination = _written_register(ins)
            if destination is None or destination not in state:
                continue
            source = _identity_source(ins)
            if source is not None:
                state[destination] = state[source]
                continue
            mnemonic = ins.mnemonic
            if mnemonic == 'il':
                state[destination] = frozenset({('const', _il_value(ins), ins.addr)})
            elif mnemonic == 'ila':
                state[destination] = frozenset({('const', (ins.raw >> 7) & 0x3FFFF, ins.addr)})
            else:
                # The block-local pass already evaluates ilhu+iohl pairs; a
                # lone half here is a definition whose value is not claimed.
                state[destination] = frozenset({('unknown', ins.addr, mnemonic)})
        return state


def _entry_values(blob, insns, address, register, entry, extra_entry_points, call_sites):
    """Constants every caller passes in `register` at an entry, or a refusal.

    A region entry is observed from outside: every reference must be a direct
    `brsl`/`brasl` inside the lifted ranges, and the entry must not also be
    reachable indirectly (a materialised immediate, a pointer-table word, the
    image entry or an extra entry point). A caller that computes the argument,
    or a call from outside the lifted ranges, proves nothing.
    """
    if address == entry:
        return 'is the image entry'
    if address in extra_entry_points:
        return 'is an extra entry point'
    word = _data_word(blob, address)
    if word is not None:
        return f'appears as a data word at 0x{word:X}'
    materialised = _immediate_address(insns, address)
    if materialised is not None:
        return f'is materialised by {materialised[1]} at 0x{materialised[0]:X}'
    callers = []
    for ins in insns:
        if ins.mnemonic not in _DIRECT_BRANCHES or address not in _operand_addresses(ins):
            continue
        if ins.mnemonic in _DIRECT_CALLS:
            callers.append(ins.addr)
        else:
            return f'direct branch at 0x{ins.addr:X} ({ins.mnemonic}) targets the entry'
    if not callers:
        return 'has no direct brsl/brasl caller'
    values = {}
    for caller in callers:
        consts = call_sites.get(caller)
        if consts is None:
            return f'caller 0x{caller:X} is outside the lifted ranges'
        value = consts.get(register)
        if value is None:
            return f'caller 0x{caller:X} passes a computed value'
        values.setdefault(value, []).append(caller)
    return {value: sorted(sites) for value, sites in values.items()}


def _materialised_addresses(insns):
    """Candidate addresses code forms as immediates, mapped to their first site.

    The same linear over-approximation as `_immediate_address`: an `ilhu`'s
    high half, a matching lone `iohl` low half and the completed pair are all
    candidates, so this can only add references, never hide one.
    """
    found = {}
    high = {}

    def note(value, ins):
        found.setdefault(value & 0xFFFFFFFF, (ins.addr, ins.mnemonic))

    for ins in insns:
        mnemonic, register = ins.mnemonic, ins.raw & 0x7F
        if mnemonic == 'ila':
            note((ins.raw >> 7) & 0x3FFFF, ins)
        elif mnemonic == 'il':
            note(_il_value(ins), ins)
        elif mnemonic == 'ilhu':
            high[register] = ((ins.raw >> 7) & 0xFFFF) << 16
            note(high[register], ins)
        elif mnemonic == 'iohl':
            low = (ins.raw >> 7) & 0xFFFF
            note(low, ins)
            value = high.pop(register, None)
            if value is not None:
                note(value | low, ins)
        else:
            written = _written_register(ins)
            if written is not None:
                high.pop(written, None)
    return found


def _resolve_sites(blob, insns, functions, regions, call_sites, entry,
                   extra_entry_points, unresolved, registers, computed_jumps):
    """Resolve `wrch MFC_Cmd` sites the block-local pass left unresolved.

    The site belongs to a branch-connected region (one procedure, however many
    chunks it was lifted as). Its register's reaching values decide:

    Function pass -- every path arrives with one constant, including through
    `ori $rD, $rS, 0` / `ai $rD, $rS, 0` copies of the region's own values.
    Two different constants, a non-constant definition, or an indirect
    dispatch with unknown successors keep the site unresolved.

    Call-site pass -- a value still live-in is the register the region was
    entered with (a caller's argument forwarded by a copy keeps that
    provenance). Every entry block such a path came through must be provable
    by the image's own callers: their block-local immediate, one level only,
    with no indirect or branch entry. The proven set must be non-empty and
    must agree with any constant the region's own paths produce.

    "Entry" is image-wide, not a property of one lifted range. A chunk start
    is an entry when it is the image entry, an extra entry point, a call
    target, a direct-branch target from another region, a return entry, or an
    address materialised as an immediate or stored as a data word. A block
    that still has a predecessor is an entry too: a call can land on it. The
    straight-line dispatch run likewise stops at an image-wide cut: a target
    of a branch other than a PC-getter, a chunk start no reference names, or
    an extra entry point. A PC-getter's target is exempt from both, because
    its continuation is the run's own straight line.

    Each refusal is recorded in the entry as data: `function` names the
    region-pass verdict, `call_sites` the entry proof's refusal (or
    "not reached" when the region pass already refused). Returns the resolved
    entries, the remaining refusals and the dispatch addresses modelled along
    the way.
    """
    entries = []
    remaining = []
    modelled = set()
    chunk_starts = {int(start) for start, _end in functions}
    extra = {int(point) for point in extra_entry_points}
    references = {}
    incoming = {}
    call_targets = set()
    return_entries = set()
    for ins in insns:
        if ins.mnemonic in _DIRECT_CALLS and not _pc_getter(ins):
            # A PC-getter reads the next address, not a callee: its
            # continuation is straight-line, so it is not an entry.
            targets = _operand_addresses(ins)
            call_targets.update(targets)
            return_entries.update(ins.addr + 4 for _target in targets)
            for target in targets:
                references.setdefault(target, []).append(ins)
        elif ins.mnemonic in _DIRECT_BRANCHES:
            for target in _operand_addresses(ins):
                references.setdefault(target, []).append(ins)
    cuts = {target for target, refs in references.items()
            if any(not _pc_getter(ref) for ref in refs)}
    materialised = _materialised_addresses(insns)
    data_words = {int.from_bytes(blob[offset:offset + 4], 'big')
                  for offset in range(0, len(blob) - 3, 4)}
    # A chunk start no branch names can still be entered by the loader, the
    # lifter's seed list or a computed transfer, so a run must not cross it. A
    # chunk start only a PC-getter names is the dispatch idiom's own
    # continuation and stays inside the run, unless code can also enter it
    # through a materialised address, a data word or an extra entry point.
    for start in chunk_starts:
        named = references.get(start)
        if start in extra or named is None or (
                all(_pc_getter(ref) for ref in named)
                and (start in materialised or start in data_words)):
            cuts.add(start)
    spans = [(start, end, number) for number, region in enumerate(regions)
             for start, end in region]
    for target, refs in references.items():
        for ref in refs:
            if _pc_getter(ref):
                continue
            source = next((number for start, end, number in spans
                           if start <= ref.addr < end), None)
            if source is not None:
                incoming.setdefault(target, set()).add(source)
    external_cache = {}

    def external_entries(number):
        """Chunk starts in this region that code outside it can enter."""
        if number in external_cache:
            return external_cache[number]
        region = regions[number]
        found = set()
        for address in chunk_starts:
            if not any(start <= address < end for start, end in region):
                continue
            if (address == entry or address in extra
                    or address in call_targets or address in return_entries
                    or any(source != number for source in incoming.get(address, ()))
                    or address in materialised or address in data_words):
                found.add(address)
        external_cache[number] = found
        return found

    flows, proofs = {}, {}
    for item in unresolved:
        site, register = item['site'], registers[item['site']]
        item['function'], item['call_sites'] = '', 'not reached'
        index = next((number for number, region in enumerate(regions)
                      if any(start <= site < end for start, end in region)), None)
        if index is None:
            item['function'] = 'no lifted range contains the site'
            remaining.append(item)
            continue
        if index not in flows:
            flows[index] = _FlowRegion(insns, regions[index], chunk_starts,
                                       external_entries(index), cuts,
                                       computed_jumps)
        flow = flows[index]
        modelled |= flow.modelled
        dispatch = 'masked-pc-relative' if flow.modelled else None
        if flow.opaque is not None:
            item['function'] = (f'opaque successor at 0x{flow.opaque.addr:X}: '
                                f'{flow.opaque.mnemonic} {flow.opaque.operands}'.strip())
            remaining.append(item)
            continue
        atoms = flow.atoms_at(register, site)
        if atoms is None:
            item['function'] = 'no path from a region entry reaches the site'
            remaining.append(item)
            continue
        constants = sorted({atom[1] for atom in atoms if atom[0] == 'const'})
        unknowns = sorted((atom for atom in atoms if atom[0] == 'unknown'),
                          key=lambda atom: (atom[1], atom[2]))
        live = sorted((atom for atom in atoms if atom[0] == 'live'),
                      key=lambda atom: (atom[2], atom[1]))
        if unknowns:
            address, mnemonic = unknowns[0][1], unknowns[0][2]
            if mnemonic == 'call':
                item['function'] = f'call at 0x{address:X} clobbers {item["register"]}'
            else:
                item['function'] = (f'definition at 0x{address:X} ({mnemonic}) is '
                                    'not a constant')
            remaining.append(item)
            continue
        if len(constants) > 1:
            first_site = {}
            for atom in atoms:
                if atom[0] == 'const':
                    first_site.setdefault(atom[1], atom[2])
            item['function'] = 'two values: ' + ', '.join(
                f'0x{value:02X} at 0x{first_site[value]:X}' for value in constants)
            remaining.append(item)
            continue
        if not live:
            if not constants:
                item['function'] = 'no definition reaches the site'
                remaining.append(item)
                continue
            entry = {'command': constants[0], 'name': MFC_COMMAND_NAMES.get(constants[0]),
                     'count': 1, 'sites': [site], 'via': 'function'}
            if dispatch is not None:
                entry['dispatch'] = dispatch
            entries.append(entry)
            continue
        origins = {atom[1] for atom in live}
        item['function'] = 'live-in' if origins == {register} else (
            'live-in via ' + ', '.join(f'$r{origin}' for origin in sorted(origins)))
        expected = constants[0] if constants else None
        proven, refusal = {}, None
        for atom in live:
            origin, address = atom[1], atom[2]
            key = (address, origin)
            if key not in proofs:
                proofs[key] = _entry_values(blob, insns, address, origin, entry,
                                            extra, call_sites)
            result = proofs[key]
            if isinstance(result, str):
                refusal = f'entry 0x{address:X} {result}'
                break
            for value, callers in result.items():
                proven.setdefault(value, set()).update(callers)
        if refusal is not None:
            item['call_sites'] = refusal
            remaining.append(item)
            continue
        if not proven:
            item['call_sites'] = 'no call site proves the entry value'
            remaining.append(item)
            continue
        if expected is not None and set(proven) != {expected}:
            item['call_sites'] = (f'callers pass '
                                  f'{", ".join(f"0x{value:02X}" for value in sorted(proven))} '
                                  f'but the region defines 0x{expected:02X}')
            remaining.append(item)
            continue
        for value, callers in sorted(proven.items()):
            entry = {'command': value, 'name': MFC_COMMAND_NAMES.get(value),
                     'count': 1, 'sites': [site], 'via': 'call-sites',
                     'callers': sorted(callers)}
            if dispatch is not None:
                entry['dispatch'] = dispatch
            entries.append(entry)
    return entries, remaining, modelled


def effects_inventory(image, base, functions, entry=None, extra_entry_points=()):
    """Channels, MFC commands and event branches in the lifted ranges.

    `functions` are the lifter's generated [start, end) LS ranges, so the
    inventory describes the code that was actually lifted, not the data tail;
    overlapping ranges (a merged lift's group and its chunks) are analysed as
    their union, and ranges a direct branch or fall-through connects are
    analysed as one region (one procedure is many chunks).
    A `wrch MFC_Cmd` counts as resolved when its source register holds an
    immediate from the same straight-line run, when the region's control flow
    proves one constant at the site (`via: function`), or when the region's
    entry is provable from the image's callers (`via: call-sites`). The last
    two passes need the image `entry` and its `extra_entry_points`, and record
    their verdict or refusal in `function` / `call_sites` when they cannot
    resolve the site. Everything else is reported in `unresolved` with its
    site, never dropped. `event_branches` lists every bi-family word that
    transfers control on an SPU event or interrupt, so an image that cannot be
    shadowed faithfully is visible before it is run.
    """
    from find_spu_functions import collect_branch_targets
    insns = disassemble_spu(image, base)
    targets = collect_branch_targets(insns)
    ranges = _merge_overlapping((int(start), int(end)) for start, end in functions)
    call_sites = {}
    channels = {}
    commands = {}
    events = []
    unresolved = []
    registers = {}
    index = 0
    consts = {}
    high = {}
    chunk_starts = {int(start) for start, _end in functions} | set(extra_entry_points)
    block_start = None
    block_of_site = {}

    for ins in insns:
        while index < len(ranges) and ins.addr >= ranges[index][1]:
            index += 1
        if index >= len(ranges) or ins.addr < ranges[index][0]:
            continue
        if ins.addr in targets or ins.addr in chunk_starts:
            # A branch target ends a straight-line block, and so does a chunk
            # start: the lifter and the loader's entry list can enter there
            # without the instructions before it running, so constants from
            # an earlier block must not flow across.
            consts.clear()
            high.clear()
            block_start = ins.addr
        elif block_start is None:
            block_start = ins.addr
        mnemonic = ins.mnemonic
        if mnemonic in _DIRECT_CALLS:
            # The caller's block-local constants at the call: the arguments
            # the interprocedural pass reads, taken before the call clears
            # them like any other control transfer. `brsl $rD, target` puts
            # the return address in $rD, so it never describes this call's
            # argument.
            call_sites[ins.addr] = dict(consts)
            call_sites[ins.addr].pop(ins.raw & 0x7F, None)
        if mnemonic in _BI_FAMILY and (mnemonic in _ALWAYS_EVENT or
                                       ins.raw & (_INTERRUPT_ENABLE | _INTERRUPT_DISABLE)):
            events.append({'site': ins.addr, 'mnemonic': mnemonic,
                           'enable': bool(ins.raw & _INTERRUPT_ENABLE),
                           'disable': bool(ins.raw & _INTERRUPT_DISABLE)})
        if mnemonic in _CHANNEL_ACCESS:
            channel = (ins.raw >> 7) & 0x7F
            bucket = channels.setdefault(channel, {'read': [], 'write': [], 'count': []})
            bucket[_CHANNEL_ACCESS[mnemonic]].append(ins.addr)
            register = ins.raw & 0x7F
            if mnemonic == 'wrch' and channel == 21:
                value = consts.get(register)
                if value is None:
                    unresolved.append({'channel': 21, 'access': 'write', 'site': ins.addr,
                                       'register': f'$r{register}',
                                       'reason': 'command value is not a same-block immediate'})
                    registers[ins.addr] = register
                else:
                    commands.setdefault(value, []).append(ins.addr)
                    block_of_site[ins.addr] = block_start
            if mnemonic in ('rdch', 'rchcnt'):
                consts.pop(register, None)
                high.pop(register, None)
            continue
        if mnemonic in _CONTROL_FLOW:
            consts.clear()
            high.clear()
            block_start = None
            continue
        if mnemonic == 'il':
            register = ins.raw & 0x7F
            consts[register] = _il_value(ins)
            high.pop(register, None)
            continue
        if mnemonic == 'ilhu':
            register = ins.raw & 0x7F
            high[register] = ((ins.raw >> 7) & 0xFFFF) << 16
            consts.pop(register, None)
            continue
        if mnemonic == 'iohl':
            register = ins.raw & 0x7F
            if register in high:
                consts[register] = high.pop(register) | ((ins.raw >> 7) & 0xFFFF)
            else:
                consts.pop(register, None)
            continue
        if mnemonic in _RRR_DEST:
            register = (ins.raw >> 21) & 0x7F
        elif mnemonic in _NO_RT_WRITE:
            continue
        else:
            register = ins.raw & 0x7F
        consts.pop(register, None)
        high.pop(register, None)

    # The lifter's own computed-jump set for the whole image: a branch it
    # dispatches but this pass cannot evaluate has unknown successors even
    # when a linear scan would read the register as an ABI return.
    from spu_lifter import compute_bi_r0_jumps
    computed_jumps = compute_bi_r0_jumps(insns, [list(span) for span in functions])
    modelled = set()
    if entry is not None and unresolved:
        resolved, unresolved, modelled = _resolve_sites(
            image, insns, functions, _flow_regions(insns, ranges), call_sites,
            entry, extra_entry_points, unresolved, registers, computed_jumps)
    else:
        resolved = []
        for item in unresolved:
            item['function'] = 'not analysed: no image entry'
            item['call_sites'] = 'not reached'
    block_entries = [{'command': value, 'name': MFC_COMMAND_NAMES.get(value),
                      'count': len(sites), 'sites': sites}
                     for value, sites in sorted(commands.items())]
    if _indirect_dispatch(insns, modelled, computed_jumps):
        # A register-targeted transfer elsewhere in the image can enter a
        # generated function at its start without appearing in call_sites, and
        # can land mid-block, past the immediate a block-local result rests on
        # (a computed `biz` to the `wrch` itself); every resolution therefore
        # rests on the named assumption.
        for item in resolved + block_entries:
            item['assumes'] = 'no-unmodelled-indirect-entry'
    mfc_commands = block_entries + resolved
    # Same-block entries keep today's shape (no `via`); resolved entries follow
    # them for the same command so every evidence path stays visible.
    mfc_commands.sort(key=lambda entry: (entry['command'], entry.get('via') or ''))
    return {
        'channels': [{'channel': channel, 'name': CHANNEL_NAMES.get(channel, f'ch{channel}'),
                      'read': len(bucket['read']), 'write': len(bucket['write']),
                      'count': len(bucket['count']), 'sites': bucket}
                     for channel, bucket in sorted(channels.items())],
        'mfc_commands': mfc_commands,
        'event_branches': sorted(events, key=lambda entry: entry['site']),
        'unresolved': sorted(unresolved, key=lambda entry: entry['site'])}


def lift_options_string(code_end, extra_entry_points, merge_chunks, return_entries=False):
    """The lifter options that change one image's generated code, canonically.

    Fixed order, lowercase hex, sorted extra entry points, an absent option
    omitted: two lifts of the same bytes with the same knobs hash equal, and
    any knob that changes the generated code changes the digest. `--base`,
    `--functions`, `--symbol-prefix`, `--report` and `--output` are not lift
    options in this sense -- for one image they follow from its own bytes (the
    manifest or the ELF, the detector, the fingerprint) or only name paths.
    """
    parts = []
    if code_end is not None:
        parts.append(f'code_end=0x{code_end:x}')
    if extra_entry_points:
        parts.append('extra=' + ','.join(f'0x{address:x}' for address in sorted(extra_entry_points)))
    if merge_chunks:
        parts.append('merge_chunks=1')
    if return_entries:
        parts.append('return_entries=1')
    return ';'.join(parts)


def lift_options_digest(text):
    """`lift_options` for a registration: FNV-1a-64 of the canonical string.

    16 hex digits, the runtime's width. A lift that passes none of the options
    hashes the empty string, which is still nonzero, so the field never reads
    as "unknown lift".
    """
    return f'{fnv1a64(text.encode("ascii")):016X}'


def publish(stage, destination):
    """Replace owned output as a unit, never reuse a previous generated lift."""
    backup = destination.with_name(destination.name + '.previous')
    if backup.exists():
        raise ValueError('previous interrupted publication exists: ' + str(backup))
    if destination.exists():
        destination.rename(backup)
    try:
        stage.rename(destination)
    except BaseException:
        if backup.exists():
            backup.rename(destination)
        raise
    if backup.exists():
        shutil.rmtree(backup)


def build_programs(args, registry_writer):
    destination = Path(args.lifted).absolute()
    registry = Path(args.out).absolute()
    if destination.is_symlink():
        raise SystemExit('--input output directory must not be a symlink')
    if registry.parent != destination:
        raise SystemExit('--input --out must be directly inside the owned --lifted directory')
    if destination.exists() and any(destination.iterdir()):
        try:
            previous = json.loads((destination / 'inventory.json').read_text())
            owned = previous.get('generator') == 'build_spu_workloads' and previous.get('schema_version') == 1
        except (OSError, ValueError):
            owned = False
        if not owned:
            raise SystemExit('--input requires an empty or previously generated output directory')
    inputs = sorted({str(Path(p).resolve()) for p in (args.input or [])})
    if any(Path(p).is_relative_to(destination.resolve()) for p in inputs):
        raise SystemExit('input files must be outside the owned output directory')
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=destination.name + '.stage-', dir=destination.parent))
    report = dict(schema_version=1, generator='build_spu_workloads', complete=False,
                  inputs=[], programs=[], errors=[], provenance={})
    try:
        if args.skip_lift:
            raise ValueError('--input always generates fresh code; --skip-lift is incompatible')
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', args.register_fn):
            raise ValueError('--register-fn must be a C identifier')
        # Hash every tool contributing to generation; fresh execution means changed
        # source/options/input can never validate an old registration implicitly.
        tool_names = ['spu_program_pipeline.py', 'build_spu_workloads.py', 'spu_elf.py',
                      'find_spu_functions.py', 'spu_disasm.py']
        tool_paths = [Path(__file__).with_name(n) for n in tool_names] + [Path(args.lifter).resolve()]
        handled = json.loads(Path(args.handled).read_text()) if getattr(args, 'handled', None) else {}
        pinned_ids = json.loads(Path(args.image_ids).read_text()) if getattr(args, 'image_ids', None) else None
        if not all(isinstance(v, str) and v for v in handled.values()):
            raise ValueError('--handled maps sha256 to a non-empty reason')
        if pinned_ids is not None:
            values = list(pinned_ids.values())
            if not all(type(v) is int and 0 < v < 0x80000000 for v in values) or len(set(values)) != len(values):
                raise ValueError('--image-ids maps sha256 to distinct positive image ids')
        report['provenance'] = dict(tools={str(p): sha256(p.read_bytes()) for p in tool_paths},
                                    python=sys.version, options=dict(register_fn=args.register_fn, constructor=args.constructor,
                                                 extra_funcs=args.extra_funcs, allow_partial=args.allow_partial,
                                                 handled=handled, image_ids=pinned_ids))
        raw_specs = {}
        if getattr(args, 'raw_images', None):
            manifest = Path(args.raw_images).resolve()
            report['provenance']['raw_manifest'] = {'path': str(manifest),
                                                    'sha256': sha256(manifest.read_bytes())}
            if manifest.is_relative_to(destination.resolve()):
                raise ValueError('--raw-images manifest must be outside the owned output directory')
            for spec in load_raw_images(manifest):
                if Path(spec['path']).is_relative_to(destination.resolve()):
                    raise ValueError('raw image blobs must be outside the owned output directory')
                previous = raw_specs.get(spec['digest'])
                if previous is not None:
                    if ((previous['ls_base'], previous['code_end'], previous['return_entries'],
                         previous['promoted'])
                            != (spec['ls_base'], spec['code_end'], spec['return_entries'],
                                spec['promoted'])):
                        raise ValueError('raw image ' + spec['path'] + ': the same bytes are listed '
                                         'with a different ls_base/code_end/return_entries/promoted')
                    previous['extra_entry_points'] = sorted(
                        set(previous['extra_entry_points']) | set(spec['extra_entry_points']))
                    previous['paths'].append(spec['path'])
                    continue
                spec['paths'] = [spec['path']]
                raw_specs[spec['digest']] = spec

        unique = {}
        blobs = {}
        layouts = {}
        for path in inputs:
            try:
                buf = Path(path).read_bytes()
            except OSError as error:
                report['errors'].append(str(error))
                continue
            report['inputs'].append(dict(path=path, sha256=sha256(buf), size=len(buf)))
            for offset, info, error in scan_spu_candidates(buf):
                occurrence = dict(path=path, offset=offset)
                if error:
                    report['programs'].append(dict(status='invalid', reason=error, occurrences=[occurrence]))
                    continue
                blob = buf[offset:offset + info['size']]
                digest = sha256(blob)
                if digest in unique:
                    if blobs[digest] != blob:
                        raise ValueError('SHA256 collision between distinct images')
                    unique[digest]['occurrences'].append(occurrence)
                    continue
                name = 'spu_' + digest
                image_id = 0x10000000 | int(digest[:7], 16)  # positive signed-int range, away from legacy IDs
                if pinned_ids is not None and digest in pinned_ids:
                    image_id = pinned_ids[digest]
                p = dict(name=name, sha256=digest, size=len(blob), entry=info['entry'], image_id=image_id,
                         fingerprint=f'{fnv1a64(blob):016X}', status='discovered', occurrences=[occurrence])
                unique[digest], blobs[digest], layouts[digest] = p, blob, info
                report['programs'].append(p)
        for digest, spec in sorted(raw_specs.items()):
            if digest in unique:
                raise ValueError('raw image ' + spec['path'] + ': the same bytes are also '
                                 'discovered as an embedded ELF; list the blob only once')
            occurrences = [{'path': path, 'offset': 0, 'kind': 'raw', 'ls_base': spec['ls_base']}
                           for path in spec['paths']]
            for occurrence in occurrences:
                report['inputs'].append({'path': occurrence['path'], 'sha256': digest,
                                         'size': spec['size'], 'kind': 'raw',
                                         'ls_base': spec['ls_base']})
            name = 'spu_' + digest
            image_id = 0x10000000 | int(digest[:7], 16)  # positive signed-int range, away from legacy IDs
            if pinned_ids is not None and digest in pinned_ids:
                image_id = pinned_ids[digest]
            p = {'name': name, 'sha256': digest, 'size': spec['size'], 'entry': spec['ls_base'],
                 'image_id': image_id, 'fingerprint': f'{fnv1a64(spec["blob"]):016X}',
                 'status': 'discovered', 'occurrences': occurrences, 'image_kind': 'raw',
                 'ls_base': spec['ls_base'], 'code_end': spec['code_end'],
                 'extra_entry_points': spec['extra_entry_points'],
                 'return_entries': spec['return_entries'], 'promoted': spec['promoted']}
            unique[digest], blobs[digest] = p, spec['blob']
            report['programs'].append(p)

        # Runtime keys cannot distinguish collisions, including identical text
        # with differing data or one image's text matching another's full ELF.
        owners = {}
        ids = {}
        for digest, program in sorted(unique.items()):
            info = layouts.get(digest)
            if info is not None and len(info['executable']) == 1:
                x = info['executable'][0]
                program['text_fingerprint'] = f"{fnv1a64(blobs[digest][x['off']:x['off'] + x['filesz']]):016X}"
            keys = {program['fingerprint'], program.get('text_fingerprint')} - {None}
            for key in keys:
                owners.setdefault(key, []).append(program)
            ids.setdefault(program['image_id'], []).append(program)
        for group in list(owners.values()) + list(ids.values()):
            if len(group) > 1:
                for program in group:
                    program.update(status='unsupported', reason='ambiguous runtime fingerprint or image-id collision')
        for digest, program in unique.items():
            if digest in handled and program['status'] == 'discovered':
                program.update(status='handled', reason=handled[digest])
                program.pop('image_id', None)
            elif pinned_ids is not None and program['status'] == 'discovered' and digest not in pinned_ids:
                program.update(status='unsupported', reason='no pinned image id (--image-ids)')
        stale = (set(handled) | set(pinned_ids or {})) - set(unique)
        if stale:
            report['errors'].append('--handled/--image-ids name programs that were not discovered: ' +
                                    ', '.join(sorted(stale)))
        extra = {}
        for spec in args.extra_funcs:
            name, sep, value = spec.partition('=')
            if not sep or name not in {p['name'] for p in unique.values()}:
                raise ValueError('--extra-funcs requires discovered content name=ADDR[,ADDR...]')
            extra[name] = [int(s.strip(), 0) for s in value.split(',')]
        imgs, text_fps, image_ids = [], {}, {}
        from find_spu_functions import detect_functions
        from build_spu_workloads import debase_includes
        for digest, program in sorted(unique.items()):
            if program['status'] in ('unsupported', 'handled'):
                continue
            name, prefix = program['name'], program['name'] + '_'
            outdir = stage / name
            outdir.mkdir()
            blob = blobs[digest]
            raw = program.get('image_kind') == 'raw'
            if raw:
                (outdir / 'image.bin').write_bytes(blob)
            else:
                info = layouts[digest]
                (outdir / 'image.elf').write_bytes(blob)
            try:
                if raw:
                    ls_base, code_end = program['ls_base'], program['code_end']
                    ranges = [(ls_base, ls_base + len(blob))]
                    functions, (offset, base, size) = detect_functions(
                        blob, verbose=False, raw=True, base_override=ls_base)
                else:
                    ranges = code_ranges(info)
                    functions, (offset, base, size) = detect_functions(blob, verbose=False)
                bounds = sorted({(max(s, a), min(e, b)) for s, e in functions for a, b in ranges
                                 if max(s, a) < min(e, b)})
                entries = sorted(set(extra.get(name, [])) | set(program.get('extra_entry_points', [])))
                for address in entries:
                    if raw:
                        limit = code_end if code_end is not None else ls_base + len(blob)
                        if not (ls_base <= address < limit and address % 4 == 0):
                            raise ValueError('extra function address is outside the raw code range')
                    elif not any(a <= address < b and address % 4 == 0 for a, b in ranges):
                        raise ValueError('extra function address is outside executable ranges')
                program['executable_ranges'] = ranges
                # The lift's options are recorded before it runs: they are what
                # the command below passes, and they identify the generated
                # body even when generation fails.
                merge_chunks = bool(getattr(args, 'merge_chunks', False))
                return_entries = bool(program.get('return_entries', False))
                program['lift_options_string'] = lift_options_string(
                    code_end if raw else None, entries, merge_chunks, return_entries)
                program['lift_options'] = lift_options_digest(program['lift_options_string'])
                (outdir / 'code.bin').write_bytes(blob[offset:offset + size])
                (outdir / 'functions.json').write_text(json.dumps([dict(start=s, end=e) for s, e in bounds]))
                command = [sys.executable, args.lifter, str(outdir / 'code.bin'), '--base', str(base),
                           '--functions', str(outdir / 'functions.json'), '--symbol-prefix', prefix,
                           '--report', str(outdir / 'lift.json'), '--output', str(outdir)]
                if entries:
                    command += ['--extra-funcs', ','.join(hex(a) for a in entries)]
                if raw and code_end is not None:
                    command += ['--code-end', str(code_end)]
                if merge_chunks:
                    command += ['--merge-chunks']
                if return_entries:
                    command += ['--return-entries']
                result = subprocess.run(command, capture_output=True, text=True)
                (outdir / 'lift.log').write_text(result.stdout + result.stderr)
                if result.returncode:
                    raise ValueError('lifter failed; see ' + name + '/lift.log')
                diagnostics = json.loads((outdir / 'lift.json').read_text())
                program['diagnostics'] = diagnostics
                if diagnostics['unsupported']:
                    raise ValueError('unsupported instructions emitted by lifter')
                starts = {s for s, _ in diagnostics['functions']}
                if program['entry'] not in starts:
                    raise ValueError('lifter did not generate the raw image entry' if raw
                                     else 'lifter did not generate the ELF entry')
                if any(not any(a <= s < e <= b for a, b in ranges) for s, e in diagnostics['functions']):
                    raise ValueError('generated function extends beyond executable ranges')
                if (set(diagnostics['call_targets']) | set(diagnostics['branch_targets'])) - starts:
                    raise ValueError('direct branch/call targets lack generated function entries')
                for filename in ['spu_recomp.c', 'spu_recomp.h']:
                    if not (outdir / filename).is_file():
                        raise ValueError('lifter output missing ' + filename)
                    debase_includes(str(outdir / filename))
                expected = f"{prefix}spu_func_{program['entry']:08X}("
                if '/* TODO spu:' in (outdir / 'spu_recomp.c').read_text():
                    raise ValueError('unresolved instruction or branch emitted by lifter')
                if expected not in (outdir / 'spu_recomp.h').read_text():
                    raise ValueError('generated header is missing entry symbol')
                if raw:
                    # The inventory describes the functions the lifter generated,
                    # so it is computed only after every lift gate has passed.
                    program['effects'] = effects_inventory(blob, program['ls_base'],
                                                           diagnostics['functions'],
                                                           entry=program['entry'],
                                                           extra_entry_points=entries)
            except (ValueError, OSError, KeyError, SystemExit) as error:
                program.update(status='unsupported', reason=str(error))
                continue
            program['status'] = 'registered'
            imgs.append((name, prefix, int(program['fingerprint'], 16), program['entry']))
            image_ids[name] = program['image_id']
            if program.get('text_fingerprint'):
                text_fps[name] = int(program['text_fingerprint'], 16)
        # Default runtime capacity is 256 registrations, including text aliases
        # (a raw image registers one fingerprint and has no ELF text alias).
        if sum(len({fp} | ({text_fps[name]} if name in text_fps else set()))
               for name, _, fp, _ in imgs) > 256:
            report['errors'].append('generated registrations exceed default runtime capacity (256)')
        if sum(len(p.get('diagnostics', {}).get('functions', [])) for p in report['programs']
               if p['status'] == 'registered') > 262144:
            report['errors'].append('generated functions exceed default runtime registry capacity (262144)')
        report['complete'] = not report['errors'] and all(p['status'] in ('registered', 'handled')
                                                          for p in report['programs'])
        usable = not report['errors'] and (report['complete'] or args.allow_partial)
        if usable:
            # The writer carries each registered image's effects inventory into the registration
            # (build_spu_workloads.registration_inventory); inventory.json gets the same records below.
            registry_args = type('RegistryArgs', (), dict(out=str(stage / registry.name), title='discovered SPU programs',
                                                        register_fn=args.register_fn, constructor=args.constructor,
                                                        programs={p['name']: p for p in report['programs']
                                                                  if p.get('status') == 'registered'}))()
            registry_writer(registry_args, imgs, text_fps, image_ids)
            sources = [destination / name / 'spu_recomp.c' for name, _, _, _ in imgs] + [registry]
            # Bracket arguments preserve spaces and avoid expansion of $ in paths.
            cmake = 'set(SPU_GENERATED_SOURCES\n' + ''.join(f'  [==[{p.as_posix()}]==]\n' for p in sources) + ')\n'
            (stage / 'sources.cmake').write_text(cmake)
        if not usable:
            for program in report['programs']:
                if program['status'] == 'registered':
                    program.update(status='generated', reason='registration omitted because coverage is incomplete')
        for program in report['programs']:
            if 'name' in program:
                outdir = stage / program['name']
                if outdir.exists():
                    program['outputs'] = {str(p.relative_to(stage)): sha256(p.read_bytes())
                                          for p in sorted(outdir.iterdir()) if p.is_file()}
        report['outputs'] = {filename: sha256((stage / filename).read_bytes())
                             for filename in [registry.name, 'sources.cmake'] if (stage / filename).is_file()}
        (stage / 'inventory.json').write_text(json.dumps(report, indent=2) + '\n')
        publish(stage, destination)
        print(f"[build_spu_workloads] {len(imgs) if usable else 0}/{len(report['programs'])} candidates registered; "
              f"complete={report['complete']}; report={destination / 'inventory.json'}")
        return 0 if usable else 1
    except (ValueError, OSError) as error:
        report['complete'] = False
        report['errors'].append(str(error))
        for filename in [registry.name, 'sources.cmake']:
            (stage / filename).unlink(missing_ok=True)
        (stage / 'inventory.json').write_text(json.dumps(report, indent=2) + '\n')
        publish(stage, destination)
        print('[build_spu_workloads] generation failed: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if stage.exists():
            shutil.rmtree(stage)
