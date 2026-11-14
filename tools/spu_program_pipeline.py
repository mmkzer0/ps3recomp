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

from spu_disasm import CHANNEL_NAMES, disassemble_spu
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
        out.append({'digest': digest, 'blob': blob, 'path': str(blob_path), 'size': len(blob),
                    'ls_base': base, 'code_end': code_end, 'extra_entry_points': points})
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


def effects_inventory(image, base, functions):
    """Channels and MFC commands reachable from the lifted function ranges.

    `functions` are the lifter's generated [start, end) LS ranges, so the
    inventory describes the code that was actually lifted, not the data tail.
    A `wrch MFC_Cmd` counts as resolved only when its source register holds an
    immediate from the same straight-line run; everything else is reported in
    `unresolved` with its site, never dropped.
    """
    from find_spu_functions import collect_branch_targets
    insns = disassemble_spu(image, base)
    targets = collect_branch_targets(insns)
    ranges = sorted((int(start), int(end)) for start, end in functions)
    channels = {}
    commands = {}
    unresolved = []
    index = 0
    consts = {}
    high = {}

    for ins in insns:
        while index < len(ranges) and ins.addr >= ranges[index][1]:
            index += 1
        if index >= len(ranges) or ins.addr < ranges[index][0]:
            continue
        if ins.addr in targets:
            consts.clear()
            high.clear()
        mnemonic = ins.mnemonic
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
                else:
                    commands.setdefault(value, []).append(ins.addr)
            if mnemonic in ('rdch', 'rchcnt'):
                consts.pop(register, None)
                high.pop(register, None)
            continue
        if mnemonic in _CONTROL_FLOW:
            consts.clear()
            high.clear()
            continue
        if mnemonic == 'il':
            register = ins.raw & 0x7F
            value = (ins.raw >> 7) & 0xFFFF
            if value & 0x8000:
                value -= 0x10000
            consts[register] = value & 0xFFFFFFFF
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

    return {
        'channels': [{'channel': channel, 'name': CHANNEL_NAMES.get(channel, f'ch{channel}'),
                      'read': len(bucket['read']), 'write': len(bucket['write']),
                      'count': len(bucket['count']), 'sites': bucket}
                     for channel, bucket in sorted(channels.items())],
        'mfc_commands': [{'command': value, 'name': MFC_COMMAND_NAMES.get(value),
                          'count': len(sites), 'sites': sites}
                         for value, sites in sorted(commands.items())],
        'unresolved': sorted(unresolved, key=lambda entry: entry['site'])}


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
                    if (previous['ls_base'], previous['code_end']) != (spec['ls_base'], spec['code_end']):
                        raise ValueError('raw image ' + spec['path'] + ': the same bytes are listed '
                                         'with a different ls_base/code_end')
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
                 'extra_entry_points': spec['extra_entry_points']}
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
                (outdir / 'code.bin').write_bytes(blob[offset:offset + size])
                (outdir / 'functions.json').write_text(json.dumps([dict(start=s, end=e) for s, e in bounds]))
                command = [sys.executable, args.lifter, str(outdir / 'code.bin'), '--base', str(base),
                           '--functions', str(outdir / 'functions.json'), '--symbol-prefix', prefix,
                           '--report', str(outdir / 'lift.json'), '--output', str(outdir)]
                if entries:
                    command += ['--extra-funcs', ','.join(hex(a) for a in entries)]
                if raw and code_end is not None:
                    command += ['--code-end', str(code_end)]
                if getattr(args, 'merge_chunks', False):
                    command += ['--merge-chunks']
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
                                                           diagnostics['functions'])
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
            registry_args = type('RegistryArgs', (), dict(out=str(stage / registry.name), title='discovered SPU programs',
                                                        register_fn=args.register_fn, constructor=args.constructor))()
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
