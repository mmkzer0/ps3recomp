#!/usr/bin/env python3
"""Synthetic discovery -> generated registration coverage, no guest assets."""
import json
import re
from pathlib import Path
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))


def raw_job_tools():
    """The synthetic raw builder, imported once TOOLS is on sys.path."""
    from gen_test_raw_job import LS_BASE, build
    return LS_BASE, build
INCLUDE = TOOLS.parent / 'include'


def elf(value=7, *, bad_bounds=False, unsupported=False, multi=False, rodata=False):
    # il r3,value; bi r0. Both variants deliberately share entry/LS addresses.
    code = struct.pack('>II', 0x40800003 | (value << 7), 0x35000000)
    if unsupported:
        code = struct.pack('>I', 0x01000000) + code[4:]
    phnum = 2 if multi else 1
    payload = 0x100
    data = bytearray(payload + len(code) + (8 if rodata or multi else 0))
    ident = b'\x7fELF\x01\x02\x01' + b'\x00' * 9
    shoff = len(data) if rodata else 0
    data[:52] = struct.pack('>16sHHIIIIIHHHHHH', ident, 2, 23, 1, 0x100,
                           52, shoff, 0, 52, 32, phnum, 40 if rodata else 0,
                           2 if rodata else 0, 0)
    struct.pack_into('>IIIIIIII', data, 52, 1, payload, 0x100, 0,
                     0x10000 if bad_bounds else len(code) + (8 if rodata else 0),
                     len(code) + (8 if rodata else 0), 5, 16)
    data[payload:payload + len(code)] = code
    if multi:
        struct.pack_into('>IIIIIIII', data, 84, 1, payload + 8, 0x200, 0, 8, 8, 5, 16)
        data[payload + 8:] = code
    if rodata:
        data[payload + 8:] = struct.pack('>II', 0x01000000, 0x01000000)
        data.extend(struct.pack('>IIIIIIIIII', 0, 1, 6, 0x100, payload, 8, 0, 0, 4, 0))
        data.extend(struct.pack('>IIIIIIIIII', 0, 1, 2, 0x108, payload + 8, 8, 0, 0, 4, 0))
    return bytes(data)


class Pipeline(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / 'generated'

    def run_pipeline(self, *inputs, partial=False, extra=()):
        command = [sys.executable, str(TOOLS / 'build_spu_workloads.py'),
                   '--lifted', str(self.output), '--out', str(self.output / 'spu_workloads.c')]
        for name, blob in inputs:
            path = self.root / name
            path.write_bytes(blob)
            command += ['--input', str(path)]
        if partial:
            command.append('--allow-partial')
        result = subprocess.run(command + list(extra), capture_output=True, text=True)
        return result

    def manifest(self):
        return json.loads((self.output / 'inventory.json').read_text())

    def run_raw(self, entries, *extra):
        manifest = self.root / 'raw_images.json'
        manifest.write_text(json.dumps(entries))
        command = [sys.executable, str(TOOLS / 'build_spu_workloads.py'),
                   '--raw-images', str(manifest),
                   '--lifted', str(self.output), '--out', str(self.output / 'spu_workloads.c')]
        return subprocess.run(command + list(extra), capture_output=True, text=True, check=False)

    def raw_entry(self, blob, name='raw-job.bin', **overrides):
        ls_base, _ = raw_job_tools()
        path = self.root / name
        path.write_bytes(blob)
        entry = {'path': str(path), 'sha256': hashlib.sha256(blob).hexdigest(),
                 'ls_base': hex(ls_base)}
        entry.update(overrides)
        return entry

    def write_runtime_blocks(self):
        runtime = TOOLS.parent / 'runtime' / 'spu'
        # Compile the runtime's actual registry/fingerprint/ELF-size blocks
        # unchanged, without the unrelated DMA/job/platform dependency graph.
        workload = (runtime / 'spu_workload.c').read_text()
        workload = workload[workload.index('uint64_t spu_workload_fingerprint('):
                            workload.index('/* ---- dispatch')]
        (self.root / 'workload.c').write_text(
            '#include "spu_workload.h"\n#include "spu_xdiff.h"\n#include "ps3emu/lifter_abi.h"\n'
            '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
            'void ps3_spu_lifter_abi_check(uint32_t got) { '
            'if (got != SPU_RUNTIME_LIFTER_ABI) abort(); }\n' + workload)
        channels = (runtime / 'spu_channels.c').read_text()
        channels = channels[channels.index('typedef void (*spu_fn)'):
                            channels.index('/* Which image registered')]
        # SPU_TLS is defined earlier in spu_channels.c, outside the extracted block.
        tls = '#if defined(_MSC_VER)\n#define SPU_TLS __declspec(thread)\n#else\n#define SPU_TLS __thread\n#endif\n'
        (self.root / 'registry.c').write_text('#include "spu_interp.h"\n#include <stdio.h>\n' + tls + channels)
        return runtime

    def test_exact_dedup_and_same_address_isolation(self):
        result = self.run_pipeline(('container.bin', b'prefix' + elf(7) + elf(9) + elf(7)))
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = self.manifest()
        self.assertTrue(report['complete'])
        self.assertEqual(len(report['programs']), 2)
        self.assertEqual(sum(len(p['occurrences']) for p in report['programs']), 3)
        self.assertEqual(len({p['image_id'] for p in report['programs']}), 2)
        for p in report['programs']:
            self.assertGreater(p['image_id'], 22)
            self.assertEqual(p['status'], 'registered')
            self.assertTrue((self.output / p['name'] / 'spu_recomp.c').exists())
        self.assertTrue((self.output / 'sources.cmake').exists())

    def test_bad_bounds_and_unsupported_are_reported_without_registration(self):
        for blob, status in [(elf(bad_bounds=True), 'invalid'), (elf(unsupported=True), 'unsupported'),
                             (elf(multi=True), 'unsupported')]:
            with self.subTest(status=status):
                result = self.run_pipeline(('input.bin', blob))
                self.assertNotEqual(result.returncode, 0)
                report = self.manifest()
                self.assertFalse(report['complete'])
                self.assertEqual(report['programs'][0]['status'], status)
                self.assertFalse((self.output / 'spu_workloads.c').exists())
                self.assertFalse((self.output / 'sources.cmake').exists())

    def test_partial_is_explicit_and_rodata_is_not_lifted(self):
        result = self.run_pipeline(('input.bin', elf(rodata=True) + elf(unsupported=True)), partial=True)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = self.manifest()
        self.assertFalse(report['complete'])
        self.assertEqual(sorted(p['status'] for p in report['programs']), ['registered', 'unsupported'])
        registered = next(p for p in report['programs'] if p['status'] == 'registered')
        source = (self.output / registered['name'] / 'spu_recomp.c').read_text()
        self.assertNotIn('TODO spu', source)

    def test_fresh_rerun_cannot_register_stale_code(self):
        first = self.run_pipeline(('input.bin', elf(7)))
        self.assertEqual(first.returncode, 0, first.stderr)
        old_name = self.manifest()['programs'][0]['name']
        second = self.run_pipeline(('input.bin', elf(unsupported=True)))
        self.assertNotEqual(second.returncode, 0)
        self.assertFalse((self.output / old_name).exists())
        self.assertFalse((self.output / 'spu_workloads.c').exists())

    def test_ambiguous_text_fingerprint_is_rejected(self):
        a = elf(7)
        b = bytearray(a)
        b[15] = 1  # Same loaded code, different exact ELF identity.
        result = self.run_pipeline(('input.bin', a + b))
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(all(p['status'] == 'unsupported' for p in self.manifest()['programs']))
        self.assertFalse((self.output / 'spu_workloads.c').exists())

    def test_strict_mixed_inventory_omits_all_registrations(self):
        result = self.run_pipeline(('input.bin', elf(7) + elf(unsupported=True)))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sorted(p['status'] for p in self.manifest()['programs']), ['generated', 'unsupported'])
        self.assertFalse((self.output / 'spu_workloads.c').exists())

    def test_changed_generation_options_cannot_leave_stale_registry(self):
        first = self.run_pipeline(('input.bin', elf(7)))
        self.assertEqual(first.returncode, 0, first.stderr)
        second = self.run_pipeline(('input.bin', elf(7)), extra=('--extra-funcs', 'unknown=0x100'))
        self.assertNotEqual(second.returncode, 0)
        self.assertFalse((self.output / 'spu_workloads.c').exists())
        self.assertFalse(self.manifest()['complete'])
        self.assertTrue(self.manifest()['errors'])

    def test_extractor_bounds_and_runtime_extent(self):
        sys.path.insert(0, str(TOOLS))
        from extract_spu_images import find_spu_images, parse_spu_elf_size
        original = elf(rodata=True)
        self.assertEqual(parse_spu_elf_size(original, 0), len(original))
        self.assertEqual(list(find_spu_images(b'prefix' + original + original)),
                         [(6, len(original)), (6 + len(original), len(original))])
        # Malformed PH/SH table dimensions and LS bounds must be rejected.
        for offset, fmt, value in [(42, '>H', 1), (28, '>I', len(original)),
                                   (46, '>H', 1), (32, '>I', len(original)),
                                   (60, '>I', 0x40000), (72, '>I', 1)]:
            malformed = bytearray(original)
            struct.pack_into(fmt, malformed, offset, value)
            self.assertIsNone(parse_spu_elf_size(malformed, 0), (offset, value))
        # Non-LOAD content past the code is still in the runtime fingerprint.
        nonload = bytearray(elf(multi=True))
        struct.pack_into('>I', nonload, 84, 4)
        self.assertEqual(parse_spu_elf_size(nonload, 0), len(nonload))

    def test_missing_lifted_entry_is_an_explicit_failure(self):
        blob = bytearray(elf())
        symbol_offset = len(blob)
        blob.extend(struct.pack('>IIIBBH', 0, 0x100, 8, 2, 0, 0))
        section_offset = len(blob)
        blob.extend(struct.pack('>IIIIIIIIII', 0, 1, 6, 0x100, 0x100, 8, 0, 0, 4, 0))
        blob.extend(struct.pack('>IIIIIIIIII', 0, 2, 0, 0, symbol_offset, 16, 0, 0, 4, 16))
        struct.pack_into('>I', blob, 24, 0x104)
        struct.pack_into('>I', blob, 32, section_offset)
        struct.pack_into('>H', blob, 46, 40)
        struct.pack_into('>H', blob, 48, 2)
        result = self.run_pipeline(('input.bin', blob))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('ELF entry', self.manifest()['programs'][0]['reason'])
        self.assertFalse((self.output / 'spu_workloads.c').exists())

    def test_load_overwrite_and_bss_overlap_are_rejected(self):
        cc = shutil.which('clang') or shutil.which('cc')
        for bss in (False, True):
            with self.subTest(bss=bss):
                blob = bytearray(elf(7, multi=True))
                # The later non-executable load writes onto the first load's
                # code. In BSS mode its zero-filled tail reaches the same code.
                struct.pack_into('>I', blob, 84 + 24, 4)
                if bss:
                    struct.pack_into('>I', blob, 84 + 8, 0xf0)
                    struct.pack_into('>I', blob, 84 + 16, 0)
                    struct.pack_into('>I', blob, 84 + 20, 24)
                else:
                    struct.pack_into('>I', blob, 84 + 8, 0x100)
                    struct.pack_into('>I', blob, 0x108, 0x40800003 | (9 << 7))
                result = self.run_pipeline(('input.bin', blob))
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                program = self.manifest()['programs'][0]
                self.assertEqual(program['status'], 'unsupported')
                self.assertIn('overlapping PT_LOAD', program['reason'])
                self.assertFalse((self.output / 'spu_workloads.c').exists())
                if not cc:
                    if __import__('os').environ.get('CI'):
                        self.fail('C compiler required for runtime overwrite control in CI')
                    continue
                # Negative control: the compatibility CLI still demonstrates
                # why accepting this layout is unsafe. Compile its generated
                # entry against the actual runtime loader and observe different
                # loaded instructions versus the instructions baked into C.
                images = self.root / 'legacy_images'
                images.mkdir(exist_ok=True)
                (images / 'sample.elf').write_bytes(blob)
                output = self.root / 'legacy_lift'
                subprocess.run([sys.executable, str(TOOLS / 'build_spu_workloads.py'),
                                '--images', str(images), '--lifted', str(output),
                                '--out', str(output / 'registry.c'), '--relift'],
                               check=True, capture_output=True, text=True)
                runtime = self.write_runtime_blocks()
                byte_list = ','.join(str(x) for x in blob)
                loaded = 0 if bss else 9
                control = (
                    '#include "spu_workload.h"\n#include <assert.h>\n'
                    'extern void spu_register_all(void);\n'
                    'SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);\n'
                    'void spu_indirect_branch(spu_context* ctx) { (void)ctx; assert(0); }\n'
                    'unsigned char image[] = {' + byte_list + '};\n'
                    'int main(void) { unsigned char ls[SPU_LS_SIZE] = {0}; uint32_t entry;\n'
                    'assert(spu_elf_load_to_ls(image, sizeof image, ls, &entry));\n'
                    'uint32_t word = ((uint32_t)ls[0x100]<<24) | ((uint32_t)ls[0x101]<<16) | '
                    '((uint32_t)ls[0x102]<<8) | ls[0x103];\n'
                    'assert(((word>>7)&65535) == ' + str(loaded) + ');\n'
                    'assert(word != 0x40800383u);\n'
                    'spu_register_all(); spu_lifted_entry_fn f = spu_workload_find('
                    'spu_workload_fingerprint(image, spu_elf_image_size(image, sizeof image))); assert(f);\n'
                    'spu_context ctx = {0}; ctx.ls = ls; f(&ctx);\n'
                    'assert(ctx.gpr[3]._u32[0] == 7); return 0; }\n')
                (self.root / 'overlap_control.c').write_text(control)
                binary = self.root / ('overlap_control.exe' if sys.platform == 'win32' else 'overlap_control')
                compiled = subprocess.run([cc, '-std=c11', '-I' + str(runtime), '-I' + str(INCLUDE),
                                str(self.root / 'overlap_control.c'), str(self.root / 'workload.c'),
                                str(self.root / 'registry.c'), str(output / 'registry.c'),
                                str(output / 'sample' / 'spu_recomp.c'), '-o', str(binary)],
                               capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stderr + compiled.stdout)
                subprocess.run([str(binary)], check=True)

    def test_compiled_runtime_registration_and_cmake(self):
        cc = shutil.which('clang') or shutil.which('cc')
        cmake = shutil.which('cmake')
        if not cc or not cmake:
            if __import__('os').environ.get('CI'):
                self.fail('C compiler and CMake required in CI')
            self.skipTest('C compiler or CMake unavailable')
        note = bytearray(elf(13, multi=True))
        struct.pack_into('>I', note, 84, 4)
        result = self.run_pipeline(('input.bin', elf(7) + elf(9) + elf(11, rodata=True) + note))
        self.assertEqual(result.returncode, 0, result.stderr)
        programs = self.manifest()['programs']
        runtime = self.write_runtime_blocks()
        body = []
        for i, program in enumerate(programs):
            blob = (self.output / program['name'] / 'image.elf').read_bytes()
            immediate = (struct.unpack_from('>I', blob, 0x100)[0] >> 7) & 0xffff
            body += [f'unsigned char image{i}[] = {{' + ','.join(str(x) for x in blob) + '};',
                     f'assert(spu_elf_image_size(image{i}, sizeof image{i}) == sizeof image{i});',
                     f'uint64_t fp{i} = spu_workload_fingerprint(image{i}, sizeof image{i});',
                     f'assert(fp{i} == 0x{program["fingerprint"]}ULL);',
                     f'int id{i} = 0; spu_lifted_entry_fn f{i} = spu_workload_find_img(fp{i}, &id{i});',
                     f'assert(f{i} && id{i} == {program["image_id"]});',
                     f'assert(spu_lookup(0x100, id{i}) == f{i});',
                     # Each context contains a 256 KB local store. Keep the
                     # four fixtures off Windows' default 1 MB main stack.
                     f'static spu_context c{i}; f{i}(&c{i});',
                     f'assert(c{i}.gpr[3]._u32[0] == {immediate});']
        body += ['assert(f0 != f1 && id0 != id1);', 'assert(spu_workload_count() == 8);']
        (self.root / 'proof.c').write_text(
            '#include "spu_workload.h"\n#include <assert.h>\n'
            'extern void spu_register_all(void);\n'
            'extern spu_lifted_entry_fn spu_lookup(uint32_t, int);\n'
            'SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);\n'
            'void spu_indirect_branch(spu_context* ctx) { (void)ctx; assert(0); }\n'
            'int main(void) { spu_register_all();\n' + '\n'.join(body) + '\nreturn 0; }\n')
        # Minimal consumer uses the same CMake generation helper as the template.
        cmake_source = 'cmake_minimum_required(VERSION 3.20)\nproject(proof C)\n'
        cmake_source += f'set(PS3RECOMP_DIR "{TOOLS.parent.as_posix()}")\n'
        cmake_source += 'include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")\n'
        cmake_source += 'ps3recomp_generate_spu("${CMAKE_CURRENT_BINARY_DIR}/spu" "${CMAKE_CURRENT_SOURCE_DIR}/input.bin")\n'
        cmake_source += 'add_executable(proof proof.c workload.c registry.c ${SPU_GENERATED_SOURCES})\n'
        cmake_source += f'target_include_directories(proof PRIVATE "{runtime.as_posix()}" "{INCLUDE.as_posix()}")\n'
        (self.root / 'CMakeLists.txt').write_text(cmake_source)
        build = self.root / 'build'
        # Visual Studio generators select their own toolset instead of this
        # compiler, and put the executable in a configuration subdirectory.
        # Ninja honors the selected Clang and the same executable path as POSIX.
        generator = ['-G', 'Ninja'] if sys.platform == 'win32' else []
        configured = subprocess.run([cmake, '-S', str(self.root), '-B', str(build), *generator,
                                     '-DCMAKE_C_COMPILER=' + cc,
                                     '-DPython3_EXECUTABLE=' + sys.executable], capture_output=True, text=True)
        self.assertEqual(configured.returncode, 0, configured.stderr + configured.stdout)
        built = subprocess.run([cmake, '--build', str(build)], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr + built.stdout)
        subprocess.run([str(build / ('proof.exe' if sys.platform == 'win32' else 'proof'))], check=True)
        # A changed input must reconfigure and fail coverage instead of linking
        # the previously successful registration on an incremental build.
        (self.root / 'input.bin').write_bytes(elf(unsupported=True))
        rebuild = subprocess.run([cmake, '--build', str(build)], capture_output=True, text=True)
        self.assertNotEqual(rebuild.returncode, 0)
        self.assertIn('SPU generation incomplete', rebuild.stdout + rebuild.stderr)
        self.assertFalse((build / 'spu' / 'spu_workloads.c').exists())

    def digests(self, *blobs):
        import hashlib
        return [hashlib.sha256(b).hexdigest() for b in blobs]

    def test_handled_programs_are_covered_without_a_second_registration(self):
        a, b = elf(7), elf(9)
        da, db = self.digests(a, b)
        handled = self.root / 'handled.json'
        handled.write_text(json.dumps({da: 'registered by the title build as image 102'}))
        result = self.run_pipeline(('input.bin', a + b), extra=['--handled', str(handled)])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = self.manifest()
        self.assertTrue(report['complete'])
        by = {p['sha256']: p for p in report['programs']}
        self.assertEqual(by[da]['status'], 'handled')
        self.assertEqual(by[da]['reason'], 'registered by the title build as image 102')
        self.assertFalse((self.output / by[da]['name']).exists())
        self.assertEqual(by[db]['status'], 'registered')
        registry = (self.output / 'spu_workloads.c').read_text()
        self.assertNotIn(by[da]['fingerprint'], registry)
        self.assertIn(by[db]['fingerprint'], registry)

    def test_pinned_image_ids_are_used_and_required(self):
        a, b = elf(7), elf(9)
        da, db = self.digests(a, b)
        ids = self.root / 'ids.json'
        ids.write_text(json.dumps({da: 120, db: 121}))
        result = self.run_pipeline(('input.bin', a + b), extra=['--image-ids', str(ids)])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = self.manifest()
        self.assertEqual({p['sha256']: p['image_id'] for p in report['programs']}, {da: 120, db: 121})
        self.assertIn('spu_begin_image(120)', (self.output / 'spu_workloads.c').read_text())
        # A discovered program without a pinned id fails coverage instead of
        # taking a content-derived id.
        ids.write_text(json.dumps({da: 120}))
        result = self.run_pipeline(('input.bin', a + b), extra=['--image-ids', str(ids)])
        self.assertNotEqual(result.returncode, 0)
        report = self.manifest()
        self.assertFalse(report['complete'])
        self.assertEqual({p['sha256']: p['status'] for p in report['programs']}[db], 'unsupported')

    def test_stale_handled_or_pinned_entries_fail(self):
        stale = self.root / 'stale.json'
        stale.write_text(json.dumps({'0' * 64: 'gone'}))
        result = self.run_pipeline(('input.bin', elf(7)), extra=['--handled', str(stale)])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.manifest()['complete'])
        self.assertTrue(any('not discovered' in e for e in self.manifest()['errors']))

    def test_synthetic_raw_job_registers_and_reports_its_effects(self):
        RAW_BASE, build_raw_job = raw_job_tools()
        sys.path.insert(0, str(TOOLS))
        from spu_elf import fnv1a64
        blob = build_raw_job()
        result = self.run_raw([self.raw_entry(blob)])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = self.manifest()
        self.assertTrue(report['complete'])
        self.assertEqual(len(report['programs']), 1)
        program = report['programs'][0]
        self.assertEqual(program['status'], 'registered')
        self.assertEqual(program['image_kind'], 'raw')
        self.assertEqual(program['entry'], RAW_BASE)
        self.assertEqual(program['ls_base'], RAW_BASE)
        self.assertIsNone(program['code_end'])
        self.assertEqual(program['occurrences'][0]['kind'], 'raw')
        # The registration fingerprint is the runtime's hash over the whole
        # staged blob, not an ELF extent or the executable text slice.
        self.assertEqual(program['fingerprint'], f'{fnv1a64(blob):016X}')
        self.assertNotIn('text_fingerprint', program)

        effects = program['effects']
        channels = {entry['channel']: (entry['read'], entry['write'], entry['count'])
                    for entry in effects['channels']}
        self.assertEqual(channels, {16: (0, 4, 0), 17: (0, 4, 0), 18: (0, 4, 0),
                                    19: (0, 4, 0), 20: (0, 4, 0), 21: (0, 5, 0),
                                    27: (2, 0, 0), 28: (0, 1, 0)})
        self.assertEqual({entry['command']: entry['count'] for entry in effects['mfc_commands']},
                         {0x20: 1, 0x40: 1, 0xB4: 1, 0xD0: 1})
        self.assertEqual({entry['command']: entry['name'] for entry in effects['mfc_commands']},
                         {0x20: 'PUT', 0x40: 'GET', 0xB4: 'PUTLLC', 0xD0: 'GETLLAR'})
        # Same-block resolution keeps its original record shape: no provenance key.
        self.assertTrue(all('via' not in entry for entry in effects['mfc_commands']))
        self.assertTrue(all('callers' not in entry for entry in effects['mfc_commands']))
        # One `wrch MFC_Cmd` takes its command from LS: remembered as
        # unresolved and still counted as a channel-21 write.
        self.assertEqual(len(effects['unresolved']), 1)
        unresolved = effects['unresolved'][0]
        self.assertEqual((unresolved['channel'], unresolved['access'], unresolved['register']),
                         (21, 'write', '$r11'))
        # The record says which pass refused it and why; `reason` stays for
        # compatibility.
        self.assertEqual(unresolved['reason'], 'command value is not a same-block immediate')
        self.assertRegex(unresolved['function'],
                         r'^definition at 0x[0-9A-F]+ \(lqd\) is not a constant$')
        self.assertEqual(unresolved['call_sites'], 'not reached')
        sites = {entry['channel']: entry['sites'] for entry in effects['channels']}
        self.assertIn(unresolved['site'], sites[21]['write'])

        cc = shutil.which('clang') or shutil.which('cc')
        if not cc:
            if __import__('os').environ.get('CI'):
                self.fail('C compiler required for the raw registration harness in CI')
            self.skipTest('C compiler unavailable')
        runtime = self.write_runtime_blocks()
        lifted = self.output / program['name']
        body = [
            'unsigned char image[] = {' + ','.join(str(x) for x in blob) + '};',
            'int main(void) {',
            '    uint64_t fp = spu_workload_fingerprint(image, sizeof image);',
            f'    assert(fp == 0x{program["fingerprint"]}ULL);',
            '    spu_register_all();',
            '    assert(spu_workload_count() == 1);',
            # The registration carries the inventory: this image has one unresolved MFC site and no
            # event branch; an unregistered fingerprint is unknown, not clean. The lift record
            # names its options even when it passes none (the empty string's FNV-1a-64 is nonzero).
            '    spu_workload_inventory inv = {0};',
            '    assert(spu_workload_find_inventory(fp, &inv));',
            f'    assert(inv.flags == SPU_WORKLOAD_INV_UNRESOLVED_MFC && inv.code_end == 0 && inv.lift_options == 0x{program["lift_options"]}ULL);',
            '    assert(!spu_workload_find_inventory(fp ^ 1u, &inv));',
            '    int id = 0; spu_lifted_entry_fn f = spu_workload_find_img(fp, &id);',
            f'    assert(f && id == {program["image_id"]});',
            f'    assert(spu_lookup(0x{RAW_BASE:X}, id) == f);',
            '    static spu_context ctx;',
            '    ctx.ls = ctx.ls_store;',
            '    f(&ctx);',
            # A cross-function branch is a trampoline transfer in the port's
            # execution model; drain it. `spu_indirect_branch` here is the
            # top-level `bi $r0` return (host_depth 0), not a dispatch.
            '    while (g_spu_trampoline_fn && g_spu_trampoline_fn != spu_indirect_branch) {',
            '        spu_lifted_entry_fn next = g_spu_trampoline_fn;',
            '        g_spu_trampoline_fn = 0;',
            '        next(&ctx);',
            '    }',
            '    assert(s_writes[16] == 4 && s_writes[17] == 4 && s_writes[18] == 4);',
            '    assert(s_writes[19] == 4 && s_writes[20] == 4 && s_writes[21] == 5);',
            '    assert(s_writes[27] == 2 && s_writes[28] == 1);',
            '    assert(s_ncommands == 5);',
            '    assert(s_commands[0] == 0x40u && s_commands[1] == 0x20u);',
            '    assert(s_commands[2] == 0xD0u && s_commands[3] == 0xB4u);',
            '    assert(s_commands[4] == 0u);   /* lqd from the zeroed local store */',
            '    return 0;',
            '}',
        ]
        (self.root / 'proof.c').write_text(
            '#include "spu_workload.h"\n#include <assert.h>\n#include <string.h>\n'
            'extern void spu_register_all(void);\n'
            'extern spu_lifted_entry_fn spu_lookup(uint32_t, int);\n'
            'SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);\n'
            'int g_spu_direct = 0;\n'
            'uint8_t g_spu_stack_reset_hint[1024];\n'
            'int g_spu_ls_watch_n = 0;\n'
            'void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p,\n'
            '                       uint32_t pc, uint32_t lr) {\n'
            '    (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr; assert(0);\n'
            '}\n'
            'static unsigned s_writes[64];\n'
            'static uint32_t s_commands[8];\n'
            'static unsigned s_ncommands;\n'
            'void spu_indirect_branch(spu_context* ctx) { (void)ctx; assert(0); }\n'
            'void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {\n'
            '    (void)ctx; assert(channel < 64); s_writes[channel]++;\n'
            '    if (channel == 21) { assert(s_ncommands < 8);\n'
            '        s_commands[s_ncommands++] = value._u32[0]; }\n'
            '}\n'
            'u128 spu_rdch(spu_context* ctx, uint32_t channel) {\n'
            '    (void)ctx; assert(channel < 64); s_writes[channel]++;\n'
            '    u128 value; memset(&value, 0, sizeof value); return value;\n'
            '}\n'
            'uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel) {\n'
            '    (void)ctx; (void)channel; return 0;\n'
            '}\n'
            + '\n'.join(body) + '\n')
        binary = self.root / ('raw_proof.exe' if sys.platform == 'win32' else 'raw_proof')
        compiled = subprocess.run([cc, '-std=c11', '-I' + str(runtime), '-I' + str(INCLUDE),
                                   str(self.root / 'proof.c'), str(self.root / 'workload.c'),
                                   str(self.root / 'registry.c'),
                                   str(self.output / 'spu_workloads.c'),
                                   str(lifted / 'spu_recomp.c'), '-o', str(binary)],
                                  capture_output=True, text=True, check=False)
        self.assertEqual(compiled.returncode, 0, compiled.stderr + compiled.stdout)
        subprocess.run([str(binary)], check=True)

    def raw_event_branches(self, blob, name):
        result = self.run_raw([self.raw_entry(blob, name=name)])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        program = self.manifest()['programs'][0]
        self.assertEqual(program['status'], 'registered')
        return program['effects']['event_branches']

    def test_raw_job_event_branches_are_reported(self):
        from gen_test_raw_job import BI, BISLED, INTERRUPT_DISABLE, IRET
        RAW_BASE, build_raw_job = raw_job_tools()
        # variant: encoded word, decoded mnemonic, E bit, D bit.
        cases = [('bisled', (BISLED << 21) | (2 << 7), 'bisled', False, False),
                 ('disable', (BI << 21) | INTERRUPT_DISABLE, 'bi', False, True),
                 ('iret', IRET << 21, 'iret', False, False)]
        for variant, word, mnemonic, enable, disable in cases:
            with self.subTest(variant=variant):
                blob = build_raw_job(variant=variant)
                encoded = struct.pack('>I', word)
                # The word identifies its own site: the inventory must report
                # that LS address, not the start of the function or blob.
                self.assertEqual(blob.count(encoded), 1)
                site = RAW_BASE + blob.index(encoded)
                self.assertEqual(site % 4, 0)
                self.assertEqual(self.raw_event_branches(blob, f'raw-{variant}.bin'),
                                 [{'site': site, 'mnemonic': mnemonic,
                                   'enable': enable, 'disable': disable}])

    def test_raw_job_without_event_branches_lists_none(self):
        _, build_raw_job = raw_job_tools()
        self.assertEqual(self.raw_event_branches(build_raw_job(), 'raw-default.bin'), [])

    def test_raw_image_sha256_mismatch_is_named(self):
        _, build_raw_job = raw_job_tools()
        blob = build_raw_job()
        result = self.run_raw([self.raw_entry(blob, sha256='0' * 64)])
        self.assertNotEqual(result.returncode, 0)
        report = self.manifest()
        self.assertFalse(report['complete'])
        self.assertTrue(any('sha256 mismatch' in error for error in report['errors']),
                        report['errors'])
        self.assertFalse((self.output / 'spu_workloads.c').exists())
        self.assertIn('sha256 mismatch', result.stderr)

    def test_raw_image_code_end_past_blob_is_named(self):
        RAW_BASE, build_raw_job = raw_job_tools()
        blob = build_raw_job()
        result = self.run_raw([self.raw_entry(blob,
                                              code_end=hex(RAW_BASE + len(blob) + 16))])
        self.assertNotEqual(result.returncode, 0)
        report = self.manifest()
        self.assertFalse(report['complete'])
        self.assertTrue(any('code_end' in error and 'past the image end' in error
                            for error in report['errors']), report['errors'])
        self.assertFalse((self.output / 'spu_workloads.c').exists())

    def test_raw_code_end_reaches_the_lifter(self):
        RAW_BASE, build_raw_job = raw_job_tools()
        blob = build_raw_job(tail_words=4)
        tail = RAW_BASE + len(blob) - 16
        code_end = tail + 8
        result = self.run_raw([self.raw_entry(blob, code_end=hex(code_end),
                                              extra_entry_points=[hex(tail + 4)])])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        program = self.manifest()['programs'][0]
        self.assertEqual(program['code_end'], code_end)
        self.assertEqual(program['extra_entry_points'], [tail + 4])
        functions = {start: end for start, end in program['diagnostics']['functions']}
        self.assertEqual(functions[tail], tail + 4)
        self.assertEqual(functions[tail + 4], code_end)
        self.assertIn('code-end', (self.output / program['name'] / 'lift.log').read_text())
        # Without the boundary the same tail function runs to the blob end.
        result = self.run_raw([self.raw_entry(blob)])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        program = self.manifest()['programs'][0]
        functions = {start: end for start, end in program['diagnostics']['functions']}
        self.assertEqual(functions[tail], RAW_BASE + len(blob))

    def pic_lift(self, **overrides):
        """Function starts and record of the position-independent synthetic image."""
        _, build_raw_job = raw_job_tools()
        blob = build_raw_job(variant='pic-jump-table')
        result = self.run_raw([self.raw_entry(blob, name='raw-pic.bin', **overrides)])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        program = self.manifest()['programs'][0]
        self.assertEqual(program['status'], 'registered', program.get('reason'))
        return {start for start, _ in program['diagnostics']['functions']}, program

    def test_raw_link_base_lifts_the_pic_table_cases_and_pointer_target(self):
        from gen_test_raw_job import pic_layout
        layout = pic_layout()
        wanted = set(layout['cases']) | {layout['pointer']}
        # Every immediate in the image is image-relative: read as an LS address
        # none of them names code, so the case bodies and the function the
        # pointer reaches are not function starts.
        plain, program = self.pic_lift()
        self.assertEqual(wanted & plain, set())
        self.assertIsNone(program['link_base'])
        self.assertEqual(program['lift_options_string'], '')
        linked, program = self.pic_lift(link_base='0x0')
        self.assertEqual(wanted - linked, set())
        self.assertLessEqual(plain, linked)
        self.assertEqual(program['link_base'], 0)
        self.assertEqual(program['lift_options_string'], 'link_base=0x0')
        # The lift covers them: each one is a generated function.
        source = (self.output / program['name'] / 'spu_recomp.c').read_text()
        for address in sorted(wanted):
            self.assertIn(f"{program['name']}_spu_func_{address:08X}(", source)

    def test_raw_link_base_equal_to_ls_base_changes_no_reading(self):
        RAW_BASE, _ = raw_job_tools()
        plain, _ = self.pic_lift()
        same, program = self.pic_lift(link_base=hex(RAW_BASE))
        self.assertEqual(same, plain)
        # The body is the same but the manifest said something different, and
        # the options say so: they are a statement about the lift, not a diff.
        self.assertEqual(program['lift_options_string'], f'link_base=0x{RAW_BASE:x}')

    def test_raw_link_base_is_validated(self):
        RAW_BASE, build_raw_job = raw_job_tools()
        blob = build_raw_job()
        cases = (('not-a-number', 'link_base is not an integer'),
                 (True, 'link_base is not an integer'),
                 (1.5, 'link_base is not an integer'),
                 (hex(RAW_BASE - 8), 'must be a 16-byte-aligned'),
                 (-16, 'must be a 16-byte-aligned'),
                 (hex(RAW_BASE + 16), 'is after ls_base'))
        for value, message in cases:
            with self.subTest(link_base=value):
                result = self.run_raw([self.raw_entry(blob, name='raw-link.bin', link_base=value)])
                self.assertNotEqual(result.returncode, 0)
                report = self.manifest()
                self.assertFalse(report['complete'])
                self.assertTrue(any('link_base' in error and message in error
                                    for error in report['errors']), report['errors'])
        # null is "no link base", like code_end; a valid value is recorded.
        for value, want in ((None, None), (0, 0), (hex(RAW_BASE), RAW_BASE)):
            with self.subTest(link_base=value):
                result = self.run_raw([self.raw_entry(blob, name='raw-link.bin', link_base=value)])
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                self.assertEqual(self.manifest()['programs'][0]['link_base'], want)

    def test_raw_link_base_must_agree_between_listings_of_the_same_bytes(self):
        _, build_raw_job = raw_job_tools()
        blob = build_raw_job()
        result = self.run_raw([self.raw_entry(blob, name='raw-a.bin'),
                               self.raw_entry(blob, name='raw-b.bin', link_base='0x0')])
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(any('link_base' in error for error in self.manifest()['errors']))

    def test_lift_options_string_names_link_base_only_when_present(self):
        from spu_program_pipeline import lift_options_digest, lift_options_string
        self.assertEqual(lift_options_string(None, [], False, link_base=0), 'link_base=0x0')
        self.assertEqual(lift_options_string(0x16168, [0xB00], True, True, link_base=0x7C00),
                         'code_end=0x16168;link_base=0x7c00;extra=0xb00;merge_chunks=1;return_entries=1')
        # Absent: today's string, so every existing image keeps its digest.
        self.assertEqual(lift_options_string(0x16168, [0xB00], True, True),
                         'code_end=0x16168;extra=0xb00;merge_chunks=1;return_entries=1')
        self.assertNotEqual(lift_options_digest(lift_options_string(None, [], False, link_base=0)),
                            lift_options_digest(lift_options_string(None, [], False)))

    def test_raw_link_base_refuses_call_sites_for_an_image_relative_address_taken_entry(self):
        _, build_raw_job = raw_job_tools()
        blob = build_raw_job(variant='calls-pic-materialised')
        # `ila $r5, <helper - base>` is no LS address, so by itself it takes
        # nothing's address: the callers still prove the helper's commands.
        result = self.run_raw([self.raw_entry(blob, name='raw-pic-calls.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        effects = self.manifest()['programs'][0]['effects']
        self.assertEqual([(entry['command'], entry['via']) for entry in effects['mfc_commands']],
                         [(0x20, 'call-sites'), (0x40, 'call-sites')])
        # With the link base it is the helper's address: code adds the load
        # delta and branches, so the helper has an entry no call site shows.
        result = self.run_raw([self.raw_entry(blob, name='raw-pic-calls.bin', link_base='0x0')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        effects = self.manifest()['programs'][0]['effects']
        self.assertEqual(effects['mfc_commands'], [])
        [unresolved] = [entry for entry in effects['unresolved'] if entry['register'] == '$r8']
        self.assertEqual(unresolved['function'], 'live-in')
        self.assertRegex(unresolved['call_sites'],
                         r'^entry 0x[0-9A-F]+ is materialised by ila at 0x[0-9A-F]+$')

    def test_link_base_never_adds_a_resolution(self):
        # The resolver's soundness rule is that no entry has an unseen way in.
        # A link base only adds readings of an immediate or word, and every use
        # of them refuses or cuts a run, so over the same function list the
        # sites left unresolved with it include every site left unresolved
        # without it, whatever the delta. Checked over every synthetic shape,
        # two deltas, and both a single range and the detector's chunks (the
        # chunks are what make a helper an entry the call-site pass looks at).
        from find_spu_functions import detect_functions
        from gen_test_raw_job import CALL_VARIANTS, CODE_START, FLOW_VARIANTS
        from spu_program_pipeline import effects_inventory
        base, build_raw_job = raw_job_tools()
        stricter = 0
        for variant in ('default', *CALL_VARIANTS, *FLOW_VARIANTS, 'pic-jump-table'):
            blob = build_raw_job(variant=variant)
            chunks, _ = detect_functions(blob, verbose=False, raw=True, base_override=base)
            for functions in ([(base + CODE_START, base + len(blob))], chunks):
                plain = effects_inventory(blob, base, functions, entry=base + CODE_START)
                plain_open = {entry['site'] for entry in plain['unresolved']}
                for link_base in (0, base - 16):
                    linked = effects_inventory(blob, base, functions, entry=base + CODE_START,
                                               link_base=link_base)
                    linked_open = {entry['site'] for entry in linked['unresolved']}
                    self.assertLessEqual(plain_open, linked_open, (variant, link_base))
                    stricter += linked_open > plain_open
        self.assertGreater(stricter, 0)

    def flow_effects(self, variant):
        """The inventory of one synthetic function range covering the whole code.

        The detector splits a raw image at every branch target, so a diamond or
        a loop is several chunks when the pipeline lifts it end to end. The
        function pass is defined on the ranges the lifter generated, so this
        feeds it the one range a merged lift would have produced.
        """
        from gen_test_raw_job import CODE_START
        from spu_program_pipeline import effects_inventory
        base, build = raw_job_tools()
        blob = build(variant=variant)
        return effects_inventory(blob, base, [(base + CODE_START, base + len(blob))],
                                 entry=base + CODE_START)

    def test_raw_helper_command_resolves_through_direct_call_sites(self):
        RAW_BASE, build_raw_job = raw_job_tools()
        from gen_test_raw_job import call_helper
        from spu_disasm import disassemble_spu
        blob = build_raw_job(variant='calls')
        helper = call_helper(variant='calls')
        result = self.run_raw([self.raw_entry(blob, name='raw-calls.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        program = self.manifest()['programs'][0]
        self.assertEqual(program['status'], 'registered')
        effects = program['effects']
        # One helper site, proven twice: GET from one caller and PUT from the
        # other. Each command carries the call site that proves it.
        self.assertEqual(len({entry['sites'][0] for entry in effects['mfc_commands']}), 1)
        self.assertEqual([entry['command'] for entry in effects['mfc_commands']], [0x20, 0x40])
        instructions = {ins.addr: ins for ins in disassemble_spu(blob, RAW_BASE)}
        for value, name in ((0x20, 'PUT'), (0x40, 'GET')):
            [entry] = [entry for entry in effects['mfc_commands'] if entry['command'] == value]
            self.assertEqual((entry['name'], entry['count'], entry['via']),
                             (name, 1, 'call-sites'))
            [caller] = entry['callers']
            self.assertEqual(instructions[caller].mnemonic, 'brsl')
            self.assertEqual(int(instructions[caller].operands.split(',')[1], 16), helper)
            # The proof is the immediate in the caller's own straight-line run.
            immediate = instructions[caller - 4]
            self.assertEqual(immediate.mnemonic, 'il')
            self.assertEqual((immediate.raw & 0x7F, (immediate.raw >> 7) & 0xFFFF),
                             (8, value))
        # The local-store command write in main is still reported, unchanged.
        self.assertEqual([entry['register'] for entry in effects['unresolved']], ['$r11'])

    def test_raw_helper_command_from_a_computed_argument_stays_unresolved(self):
        _, build_raw_job = raw_job_tools()
        blob = build_raw_job(variant='calls-computed')
        result = self.run_raw([self.raw_entry(blob, name='raw-calls-computed.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        effects = self.manifest()['programs'][0]['effects']
        # One caller loads the argument, so no direct call site proves the
        # command: the site stays visible as unresolved, named by register.
        self.assertEqual(effects['mfc_commands'], [])
        self.assertEqual(sorted(entry['register'] for entry in effects['unresolved']),
                         ['$r11', '$r8'])
        [unresolved] = [entry for entry in effects['unresolved'] if entry['register'] == '$r8']
        self.assertEqual(unresolved['function'], 'live-in')
        self.assertRegex(unresolved['call_sites'],
                         r'^entry 0x[0-9A-F]+ caller 0x[0-9A-F]+ passes a computed value$')

    def test_raw_helper_with_an_indirect_entry_stays_unresolved(self):
        RAW_BASE, build_raw_job = raw_job_tools()
        from gen_test_raw_job import call_helper
        helper = call_helper(variant='calls')
        data_word = build_raw_job(variant='calls-data-word')
        cases = {
            # The helper's address as an ila immediate in main.
            'immediate': (build_raw_job(variant='calls-materialised'), {},
                          r'^entry 0x[0-9A-F]+ is materialised by ila at 0x[0-9A-F]+$'),
            # The appended word is the only evidence here, so the code ends
            # before it and the lifter never reads it as an instruction.
            'data word': (data_word, {'code_end': hex(RAW_BASE + len(data_word) - 4)},
                          r'^entry 0x[0-9A-F]+ appears as a data word at 0x[0-9A-F]+$'),
            # An external entry could be reached with any argument.
            'extra entry point': (build_raw_job(variant='calls'),
                                  {'extra_entry_points': [hex(helper)]},
                                  r'^entry 0x[0-9A-F]+ is an extra entry point$'),
        }
        for label, (blob, overrides, refusal) in cases.items():
            with self.subTest(entry=label):
                result = self.run_raw([self.raw_entry(blob, name='raw-indirect.bin',
                                                      **overrides)])
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                effects = self.manifest()['programs'][0]['effects']
                self.assertEqual(effects['mfc_commands'], [])
                self.assertEqual(sorted(entry['register'] for entry in effects['unresolved']),
                                 ['$r11', '$r8'])
                [unresolved] = [entry for entry in effects['unresolved']
                                if entry['register'] != '$r11']
                self.assertEqual(unresolved['function'], 'live-in')
                self.assertRegex(unresolved['call_sites'], refusal)

    def test_raw_helper_that_writes_the_command_register_stays_unresolved(self):
        _, build_raw_job = raw_job_tools()
        blob = build_raw_job(variant='calls-rewrite')
        result = self.run_raw([self.raw_entry(blob, name='raw-calls-rewrite.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        effects = self.manifest()['programs'][0]['effects']
        # The helper redefines the register before the site, so the immediate
        # the callers pass is not what the site issues.
        self.assertEqual(effects['mfc_commands'], [])
        self.assertEqual(sorted(entry['register'] for entry in effects['unresolved']),
                         ['$r11', '$r8'])

    def test_function_pass_resolves_an_immediate_dominating_the_site(self):
        from gen_test_raw_job import CODE_START
        RAW_BASE, build_raw_job = raw_job_tools()
        blob = build_raw_job(variant='flow-dominates')
        result = self.run_raw([self.raw_entry(blob, name='raw-flow.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        program = self.manifest()['programs'][0]
        self.assertEqual(program['status'], 'registered')
        effects = program['effects']
        # The conditional branch ends the block-local run, but the function's
        # own flow proves the site sees only the one immediate.
        self.assertEqual(effects['unresolved'], [])
        [entry] = effects['mfc_commands']
        self.assertEqual((entry['command'], entry['name'], entry['via']), (0x40, 'GET', 'function'))
        self.assertNotIn('callers', entry)
        # The site is the `wrch MFC_Cmd` after the branch, in the chunk that
        # also holds the immediate.
        self.assertEqual(entry['sites'], [RAW_BASE + CODE_START + 8])

    def test_function_pass_resolves_a_preheader_immediate_in_a_loop(self):
        effects = self.flow_effects('flow-loop')
        # The loop head is a branch target and the back edge re-enters it, so
        # the block-local run cannot carry the value; the preheader's immediate
        # still dominates every entry to the site.
        self.assertEqual(effects['unresolved'], [])
        [entry] = effects['mfc_commands']
        self.assertEqual((entry['command'], entry['name'], entry['via']),
                         (0x40, 'GET', 'function'))

    def test_merged_lift_resolves_across_chunks(self):
        _, build_raw_job = raw_job_tools()
        blob = build_raw_job(variant='flow-loop')
        result = self.run_raw([self.raw_entry(blob, name='raw-merged.bin')], '--merge-chunks')
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        program = self.manifest()['programs'][0]
        # A merged lift reports the group and its chunks, so ranges overlap;
        # their union is the one function the site belongs to.
        [entry] = program['effects']['mfc_commands']
        self.assertEqual((entry['command'], entry['name'], entry['via']),
                         (0x40, 'GET', 'function'))
        self.assertEqual(program['effects']['unresolved'], [])
        self.assertEqual(program['lift_options_string'], 'merge_chunks=1')

    def test_raw_forwarded_argument_resolves_with_its_caller_register(self):
        RAW_BASE, build_raw_job = raw_job_tools()
        from gen_test_raw_job import call_helper
        from spu_disasm import disassemble_spu
        blob = build_raw_job(variant='calls-forward')
        helper = call_helper(variant='calls-forward')
        result = self.run_raw([self.raw_entry(blob, name='raw-forward.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        program = self.manifest()['programs'][0]
        self.assertEqual(program['status'], 'registered')
        effects = program['effects']
        # The helper copies $r7 into $r14 before the site; the proof must
        # follow the copy to the register the callers actually set.
        self.assertEqual([entry['command'] for entry in effects['mfc_commands']], [0x20, 0x40])
        instructions = {ins.addr: ins for ins in disassemble_spu(blob, RAW_BASE)}
        for entry in effects['mfc_commands']:
            self.assertEqual((entry['via'], entry['count']), ('call-sites', 1))
            [caller] = entry['callers']
            self.assertEqual(int(instructions[caller].operands.split(',')[1], 16), helper)
        self.assertEqual([entry['register'] for entry in effects['unresolved']], ['$r11'])

    def test_raw_forwarded_argument_from_a_computed_caller_stays_unresolved(self):
        _, build_raw_job = raw_job_tools()
        blob = build_raw_job(variant='calls-forward-computed')
        result = self.run_raw([self.raw_entry(blob, name='raw-forward-computed.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        effects = self.manifest()['programs'][0]['effects']
        self.assertEqual(effects['mfc_commands'], [])
        [unresolved] = [entry for entry in effects['unresolved'] if entry['register'] == '$r14']
        # The copy keeps the provenance: the refusal names the register the
        # caller was supposed to set and the caller that did not set it.
        self.assertEqual(unresolved['function'], 'live-in via $r7')
        self.assertRegex(unresolved['call_sites'],
                         r'^entry 0x[0-9A-F]+ caller 0x[0-9A-F]+ passes a computed value$')

    def test_region_pass_resolves_across_branch_connected_chunks(self):
        from gen_test_raw_job import CODE_START
        RAW_BASE, build_raw_job = raw_job_tools()
        blob = build_raw_job(variant='flow-loop')
        result = self.run_raw([self.raw_entry(blob, name='raw-region.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        program = self.manifest()['programs'][0]
        # The detector lifts the loop head as its own function, so the
        # immediate and the site are in different chunks; the branch-connected
        # region is what proves the value reaches the site.
        functions = {start: end for start, end in program['diagnostics']['functions']}
        self.assertEqual(functions.get(RAW_BASE + CODE_START), RAW_BASE + CODE_START + 4)
        self.assertIn(RAW_BASE + CODE_START + 4, functions)
        self.assertEqual(program['effects']['unresolved'], [])
        [entry] = program['effects']['mfc_commands']
        self.assertEqual((entry['command'], entry['via']), (0x40, 'function'))
        self.assertEqual(entry['sites'], [RAW_BASE + CODE_START + 4])

    def test_raw_return_entries_adds_call_return_sites(self):
        from gen_test_raw_job import CODE_START
        RAW_BASE, build_raw_job = raw_job_tools()
        blob = build_raw_job(variant='calls')
        returns = [RAW_BASE + CODE_START + 16, RAW_BASE + CODE_START + 24]

        def lift(**overrides):
            result = self.run_raw([self.raw_entry(blob, name='raw-return-entries.bin',
                                                  **overrides)])
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            return self.manifest()['programs'][0]

        plain = lift()
        self.assertTrue(all(start not in [s for s, _e in plain['diagnostics']['functions']]
                            for start in returns))
        lifted = lift(return_entries=True)
        starts = {start for start, _end in lifted['diagnostics']['functions']}
        # The lifter splits the bounds at both call return addresses, so the
        # resumed sites are registered entries of their own.
        self.assertTrue(set(returns) <= starts)
        self.assertEqual(lifted['lift_options_string'], 'return_entries=1')
        self.assertNotEqual(lifted['lift_options'], plain['lift_options'])

    def test_raw_promoted_is_copied_to_the_record(self):
        _, build_raw_job = raw_job_tools()
        blob = build_raw_job()
        for promoted, want in ((False, False), (True, True)):
            with self.subTest(promoted=promoted):
                result = self.run_raw([self.raw_entry(blob, name='raw-promoted.bin',
                                                      promoted=promoted)])
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                program = self.manifest()['programs'][0]
                self.assertEqual(program['promoted'], want)
                # The flag does not change the lift, so the options stay empty.
                self.assertEqual(program['lift_options_string'], '')
        result = self.run_raw([self.raw_entry(blob, name='raw-promoted.bin',
                                             return_entries=False)])
        self.assertEqual(self.manifest()['programs'][0]['return_entries'], False)





    def test_raw_manifest_flags_must_be_booleans(self):
        _, build_raw_job = raw_job_tools()
        blob = build_raw_job()
        for field in ('return_entries', 'promoted'):
            for value in (1, 0, 'true', None):
                with self.subTest(field=field, value=value):
                    result = self.run_raw([self.raw_entry(blob, name='raw-flag.bin',
                                                          **{field: value})])
                    self.assertNotEqual(result.returncode, 0)
                    report = self.manifest()
                    self.assertFalse(report['complete'])
                    self.assertTrue(any(field in error and 'must be a boolean' in error
                                        for error in report['errors']), report['errors'])

    def test_raw_link_base_vetoes_relative_epilogue_immediates(self):
        from gen_test_raw_job import (
            AI,
            BI,
            BR,
            IL,
            LQD,
            LS_BASE,
            NOP,
            STQD,
            _ri10,
            _ri16,
            _rr,
        )
        for link_base in (0, 0x1000):
            with self.subTest(link_base=link_base):
                words = [_rr(NOP)] * (0x78 // 4)
                words[0] = _ri16(BR, 0x40 // 4)
                words[0x40 // 4:0x40 // 4 + 3] = [
                    _ri10(STQD, 1, 1, 0), _ri10(AI, -32, 1, 1),
                    _ri16(BR, (0x60 - 0x48) // 4),
                ]
                words[0x60 // 4:0x60 // 4 + 3] = [
                    _ri10(LQD, 3, 1, 0), _ri10(AI, 32, 1, 1), _rr(BI, ra=0),
                ]
                words[0x70 // 4:0x70 // 4 + 2] = [
                    _ri16(IL, link_base + 0x60, 3), _rr(BI, ra=0),
                ]
                blob = b''.join(struct.pack('>I', word) for word in words)
                site = LS_BASE + 0x68
                # With no linked-base metadata the immediate cannot name the
                # epilogue. The positive control proves direct inheritance.
                result = self.run_raw([self.raw_entry(blob, name='split-frame.bin')])
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                program = self.manifest()['programs'][0]
                returns = {item['site']: item for item in program['diagnostics']['r0_returns']}
                self.assertTrue(returns[site]['inherited'])

                result = self.run_raw([self.raw_entry(
                    blob, name='split-frame.bin', link_base=hex(link_base))])
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                program = self.manifest()['programs'][0]
                dispatched = {item['site'] for item in program['diagnostics']['r0_dispatches']}
                self.assertIn(site, dispatched)
                source = (self.output / program['name'] / 'spu_recomp.c').read_text()
                function = source.split(
                    f'spu_func_{LS_BASE + 0x60:08X}(spu_context* ctx) {{', 1)[1].split('}', 1)[0]
                self.assertIn('ctx->pc = ctx->gpr[0]._u32[0];', function)

    def test_raw_indirect_dispatch_names_the_assumption(self):
        _, build_raw_job = raw_job_tools()
        from gen_test_raw_job import dispatch_entry
        cases = (('calls', {}, 'spu-abi-link-slot'),
                 ('calls-dispatch', {'extra_entry_points': [hex(dispatch_entry())]},
                  ['no-unmodelled-indirect-entry', 'spu-abi-link-slot']))
        for variant, overrides, assumes in cases:
            with self.subTest(variant=variant):
                result = self.run_raw([self.raw_entry(build_raw_job(variant=variant),
                                                      name='raw-assumes.bin', **overrides)])
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                effects = self.manifest()['programs'][0]['effects']
                self.assertTrue(effects['mfc_commands'])
                for entry in effects['mfc_commands']:
                    self.assertEqual(entry.get('assumes'), assumes)

    def test_unresolved_entry_names_an_opaque_successor(self):
        effects = self.flow_effects('flow-dispatch')
        # A register-targeted transfer on a reachable path has unknown
        # successors, so the region proves nothing and says so.
        self.assertEqual(effects['mfc_commands'], [])
        [unresolved] = effects['unresolved']
        self.assertRegex(unresolved['function'],
                         r'^opaque successor at 0x[0-9A-F]+: bi \$r5$')
        self.assertEqual(unresolved['call_sites'], 'not reached')

    def test_masked_pc_relative_dispatch_resolves(self):
        effects = self.flow_effects('flow-dispatch')
        self.assertEqual(effects['mfc_commands'], [])   # unknown successors
        effects = self.flow_effects('flow-dispatch-pc')
        # The PC-getter link plus the masked offset set give the exact targets,
        # so the dispatch is a known edge and the site sees one constant.
        self.assertEqual(effects['unresolved'], [])
        [entry] = effects['mfc_commands']
        self.assertEqual((entry['command'], entry['via']), (0x40, 'function'))
        self.assertEqual(entry['dispatch'], 'masked-pc-relative')
        self.assertNotIn('assumes', entry)

    def test_masked_pc_relative_dispatch_end_to_end(self):
        from gen_test_raw_job import CODE_START
        RAW_BASE, build_raw_job = raw_job_tools()
        blob = build_raw_job(variant='flow-dispatch-pc')
        result = self.run_raw([self.raw_entry(blob, name='raw-masked.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        program = self.manifest()['programs'][0]
        self.assertEqual(program['status'], 'registered')
        self.assertEqual(program['effects']['unresolved'], [])
        [entry] = program['effects']['mfc_commands']
        self.assertEqual((entry['command'], entry['via'], entry['dispatch']),
                         (0x40, 'function', 'masked-pc-relative'))
        self.assertEqual(entry['sites'], [RAW_BASE + CODE_START + 0x4C])
        # The only transfer is the modelled one, so no assumption is needed.
        self.assertNotIn('assumes', entry)

    def test_masked_pc_relative_dispatch_stays_opaque_when_not_exact(self):
        cases = {
            'register mask': 'flow-dispatch-and',
            'set over the bound': 'flow-dispatch-wide',
            'target outside the region': 'flow-dispatch-outside',
        }
        for label, variant in cases.items():
            with self.subTest(case=label):
                effects = self.flow_effects(variant)
                self.assertEqual(effects['mfc_commands'], [])
                [unresolved] = effects['unresolved']
                self.assertRegex(unresolved['function'],
                                 r'^opaque successor at 0x[0-9A-F]+: bi \$r7$')
                self.assertEqual(unresolved['call_sites'], 'not reached')

    def test_function_pass_terminates_on_a_live_back_edge(self):
        effects = self.flow_effects('flow-loop-live')
        # The back edge re-enters the head with the same definition as the
        # preheader; the state has to converge on one atom instead of growing
        # a new one on every iteration.
        self.assertEqual(effects['unresolved'], [])
        [entry] = effects['mfc_commands']
        self.assertEqual((entry['command'], entry['name'], entry['via']),
                         (0x40, 'GET', 'function'))

    def test_call_site_pass_refuses_a_helper_with_a_tail_branch_entry(self):
        _, build_raw_job = raw_job_tools()
        # The helper is called with 0x20 and tail-branched to with 0x40, so no
        # single caller value describes the site. Under a merged lift the same
        # helper is a chunk start inside one range.
        for extra in ((), ('--merge-chunks',)):
            with self.subTest(merged=bool(extra)):
                result = self.run_raw([self.raw_entry(
                    build_raw_job(variant='flow-tail-call'), name='raw-tail.bin')], *extra)
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                effects = self.manifest()['programs'][0]['effects']
                self.assertEqual(effects['mfc_commands'], [])
                [unresolved] = [entry for entry in effects['unresolved']
                                if entry['register'] == '$r8']
                self.assertEqual(unresolved['function'], 'live-in')
                self.assertRegex(unresolved['call_sites'],
                                 r'^entry 0x[0-9A-F]+ direct branch at 0x[0-9A-F]+ '
                                 r'\(br\) targets the entry$')

    def test_call_site_pass_ignores_the_link_register_as_an_argument(self):
        _, build_raw_job = raw_job_tools()
        # `brsl $r8, helper` writes the return address into $r8, so the
        # caller's own $r8 immediate is not the argument the helper reads.
        result = self.run_raw([self.raw_entry(build_raw_job(variant='flow-link-arg'),
                                              name='raw-link.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        effects = self.manifest()['programs'][0]['effects']
        self.assertEqual(effects['mfc_commands'], [])
        [unresolved] = effects['unresolved']
        self.assertEqual(unresolved['function'], 'live-in')
        self.assertRegex(unresolved['call_sites'],
                         r'^entry 0x[0-9A-F]+ caller 0x[0-9A-F]+ passes a computed value$')

    def test_block_local_pass_does_not_cross_an_extra_entry_chunk_start(self):
        RAW_BASE, build_raw_job = raw_job_tools()
        from spu_disasm import disassemble_spu
        blob = build_raw_job(variant='flow-extra-entry')
        [site] = [ins.addr for ins in disassemble_spu(blob, RAW_BASE)
                  if ins.mnemonic == 'wrch' and (ins.raw >> 7) & 0x7F == 21]
        # Code can enter at the site's chunk start without the immediate
        # before it running, so the block-local run must not carry it across.
        result = self.run_raw([self.raw_entry(blob, name='raw-extra-entry.bin',
                                              extra_entry_points=[hex(site)])])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        effects = self.manifest()['programs'][0]['effects']
        self.assertEqual(effects['mfc_commands'], [])
        [unresolved] = effects['unresolved']
        self.assertEqual(unresolved['register'], '$r8')
        self.assertEqual(unresolved['function'], 'live-in')
        self.assertRegex(unresolved['call_sites'],
                         r'^entry 0x[0-9A-F]+ is an extra entry point$')

    def test_ila_seeded_chunk_start_is_not_resolved_block_locally(self):
        _, build_raw_job = raw_job_tools()
        # The `ila` target is a chunk start, and the dispatch reaches it with
        # PUT while the fall-through block defines GET: neither the block-local
        # run nor the function pass may pick one.
        result = self.run_raw([self.raw_entry(build_raw_job(variant='flow-ila-landing'),
                                              name='raw-ila-landing.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        effects = self.manifest()['programs'][0]['effects']
        self.assertEqual(effects['mfc_commands'], [])
        [unresolved] = effects['unresolved']
        self.assertRegex(unresolved['function'],
                         r'^two values: 0x(20|40) at 0x[0-9A-F]+, 0x(20|40) at 0x[0-9A-F]+$')

    def test_mid_block_landing_is_never_a_clean_block_local_result(self):
        _, build_raw_job = raw_job_tools()
        # A computed `biz` can land on the `wrch` itself, past the GET immediate
        # of its block, with PUT in the register; the target is no chunk start,
        # so the site may stay block-local GET only under the named assumption.
        result = self.run_raw([self.raw_entry(build_raw_job(variant='flow-mid-landing'),
                                              name='raw-mid-landing.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        effects = self.manifest()['programs'][0]['effects']
        for entry in effects['mfc_commands']:
            self.assertEqual(entry.get('assumes'), 'no-unmodelled-indirect-entry', entry)

    def test_block_local_results_carry_the_entry_assumption(self):
        _, build_raw_job = raw_job_tools()
        from gen_test_raw_job import default_extra_entry
        # A register-targeted transfer past the return can enter a chunk start
        # mid-block; every block-local command then rests on the assumption.
        for variant in ('default-dispatch', 'flow-r0-identity'):
            with self.subTest(variant=variant):
                result = self.run_raw([self.raw_entry(
                    build_raw_job(variant=variant), name='raw-default-dispatch.bin',
                    extra_entry_points=[hex(default_extra_entry())])])
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                effects = self.manifest()['programs'][0]['effects']
                self.assertEqual([entry['command'] for entry in effects['mfc_commands']],
                                 [0x20, 0x40, 0xB4, 0xD0])
                for entry in effects['mfc_commands']:
                    self.assertNotIn('via', entry)
                    self.assertEqual(entry['assumes'], 'no-unmodelled-indirect-entry')

    def test_function_pass_rejects_two_constants_on_two_paths(self):
        effects = self.flow_effects('flow-split')
        # Reporting either constant would claim a command set the site does
        # not have; the unresolved record names both and their definitions.
        self.assertEqual(effects['mfc_commands'], [])
        [unresolved] = effects['unresolved']
        self.assertRegex(unresolved['function'],
                         r'^two values: 0x(20|40) at 0x[0-9A-F]+, 0x(20|40) at 0x[0-9A-F]+$')
        self.assertEqual(unresolved['call_sites'], 'not reached')

    def test_function_pass_rejects_a_definition_on_only_one_path(self):
        effects = self.flow_effects('flow-one-path')
        # The other path leaves the register live-in, and this function is the
        # image entry, so no call site can supply that value either.
        self.assertEqual(effects['mfc_commands'], [])
        self.assertEqual([entry['register'] for entry in effects['unresolved']], ['$r8'])

    def test_function_pass_rejects_a_definition_that_does_not_dominate(self):
        effects = self.flow_effects('flow-sibling')
        # The immediate is in a branch that never reaches the site.
        self.assertEqual(effects['mfc_commands'], [])
        self.assertEqual([entry['register'] for entry in effects['unresolved']], ['$r8'])

    def test_lift_options_identify_the_generated_body(self):
        from spu_program_pipeline import lift_options_digest, lift_options_string
        # Canonical form: fixed order, lowercase hex, sorted entry points, an
        # absent option omitted.
        self.assertEqual(lift_options_string(0x16168, [0xB00, 0xA2C], True),
                         'code_end=0x16168;extra=0xa2c,0xb00;merge_chunks=1')
        self.assertEqual(lift_options_string(None, [], False), '')
        self.assertEqual(lift_options_string(0x100, [], False), 'code_end=0x100')
        self.assertEqual(lift_options_string(None, [0x7C64], False), 'extra=0x7c64')
        self.assertEqual(lift_options_string(None, [], True, True),
                         'merge_chunks=1;return_entries=1')
        # The empty string still hashes to a value, so a lift that passed no
        # option never reads as "unknown lift".
        self.assertNotEqual(lift_options_digest(''), '0' * 16)
        self.assertEqual(lift_options_digest('extra=0x7c64'),
                         lift_options_digest(lift_options_string(None, [0x7C64], False)))

        RAW_BASE, build_raw_job = raw_job_tools()
        from gen_test_raw_job import CODE_START
        entry = RAW_BASE + CODE_START

        def lift(variant='default', **overrides):
            image = build_raw_job(variant=variant)
            result = self.run_raw([self.raw_entry(image, name='raw-lift.bin', **overrides)])
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            return self.manifest()['programs'][0]

        plain = lift()
        self.assertEqual(plain['lift_options_string'], '')
        self.assertEqual(plain['lift_options'], lift_options_digest(''))
        # The value names the options, not the bytes: another image lifted with
        # the same options agrees.
        self.assertEqual(lift('bisled')['lift_options'], plain['lift_options'])
        # One extra entry point changes the value and the string says which.
        extra = lift(extra_entry_points=[hex(entry)])
        self.assertEqual(extra['lift_options_string'], f'extra=0x{entry:x}')
        self.assertNotEqual(extra['lift_options'], plain['lift_options'])
        self.assertEqual(self.registered_inventory()[0][3], int(extra['lift_options'], 16))
        # A code end and the return-entry option are recorded the same way.
        code_end = RAW_BASE + len(build_raw_job())
        bounded = lift(code_end=hex(code_end))
        self.assertEqual(bounded['lift_options_string'], f'code_end=0x{code_end:x}')
        resumed = lift(return_entries=True)
        self.assertEqual(resumed['lift_options_string'], 'return_entries=1')
        self.assertNotEqual(resumed['lift_options'], plain['lift_options'])

    def test_raw_return_entries_reach_the_lifter(self):
        _, build_raw_job = raw_job_tools()
        blob = build_raw_job()
        result = self.run_raw([self.raw_entry(blob, name='raw-return.bin', return_entries=True)])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        program = self.manifest()['programs'][0]
        self.assertTrue(program['return_entries'])
        self.assertEqual(program['lift_options_string'], 'return_entries=1')
        # The flag reached the lifter: it reports the return entries it split.
        self.assertIn('return entries', (self.output / program['name'] / 'lift.log').read_text())
        result = self.run_raw([self.raw_entry(blob, name='raw-return.bin')])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        plain = self.manifest()['programs'][0]
        self.assertFalse(plain['return_entries'])
        self.assertEqual(plain['lift_options_string'], '')
        self.assertNotEqual(plain['lift_options'], program['lift_options'])

    def registered_inventory(self):
        """(fingerprint, flags names, code_end) of every spu_workload_set_inventory call in the
        generated registration."""
        calls = re.findall(r'spu_workload_set_inventory\(0x([0-9A-F]{16})ULL, ([^,]+), 0x([0-9A-F]+)u, '
                           r'0x([0-9A-F]{16})ULL\);', (self.output / 'spu_workloads.c').read_text())
        return [(fp, flags.strip(), int(code_end, 16), int(options, 16)) for fp, flags, code_end, options in calls]

    def test_raw_registration_carries_the_inventory_flags(self):
        _, build_raw_job = raw_job_tools()
        for variant in ('default', 'bisled'):
            with self.subTest(variant=variant):
                blob = build_raw_job(variant=variant)
                result = self.run_raw([self.raw_entry(blob, name=f'raw-{variant}.bin')])
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                program = self.manifest()['programs'][0]
                effects = program['effects']
                # The synthetic job has one `wrch MFC_Cmd` whose word comes from local store.
                self.assertEqual(len(effects['unresolved']), 1)
                want = ['SPU_WORKLOAD_INV_UNRESOLVED_MFC']
                if effects['event_branches']:
                    want.insert(0, 'SPU_WORKLOAD_INV_EVENT_BRANCHES')
                self.assertEqual(bool(effects['event_branches']), variant == 'bisled')
                self.assertEqual(self.registered_inventory(),
                                 [(program['fingerprint'], ' | '.join(want), 0,
                                   int(program['lift_options'], 16))])

    def test_raw_registration_carries_code_end(self):
        RAW_BASE, build_raw_job = raw_job_tools()
        blob = build_raw_job(tail_words=4)
        code_end = RAW_BASE + len(blob) - 8
        result = self.run_raw([self.raw_entry(blob, code_end=hex(code_end))])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        [(_, _, registered_end, _)] = self.registered_inventory()
        self.assertEqual(registered_end, code_end)

    def test_elf_registration_has_no_inventory(self):
        # An ELF image gets no effects inventory from the pipeline: it must stay unknown to the
        # runtime, never be registered as clean.
        result = self.run_pipeline(('input.bin', elf(7)))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('spu_workload_register_img', (self.output / 'spu_workloads.c').read_text())
        self.assertNotIn('spu_workload_set_inventory', (self.output / 'spu_workloads.c').read_text())

    def test_inventory_without_event_branch_list_is_no_inventory(self):
        # An inventory from before event-branch detection says nothing about event branches.
        from build_spu_workloads import registration_inventory
        self.assertIsNone(registration_inventory({'effects': {'unresolved': []}}))
        self.assertIsNone(registration_inventory({}))
        self.assertIsNone(registration_inventory(None))
        self.assertEqual(registration_inventory({'effects': {'event_branches': [], 'unresolved': []},
                                                 'code_end': None}), (0, 0, 0))

    def legacy_images(self):
        images = self.root / 'images'
        images.mkdir(exist_ok=True)
        (images / 'sample.elf').write_bytes(elf())
        return images

    def legacy_run(self, images, output, *extra):
        command = [sys.executable, str(TOOLS / 'build_spu_workloads.py'),
                   '--images', str(images), '--lifted', str(output),
                   '--out', str(output / 'registry.c')] + list(extra)
        return subprocess.run(command, capture_output=True, text=True)

    def test_reuse_and_skip_lift_reject_pre_stamp_lift(self):
        # A pre-stamp lift is what a port has before it re-runs the current
        # lifter. Both reuse paths must fail before a registry exists.
        images = self.legacy_images()
        output = self.root / 'lifted'
        first = self.legacy_run(images, output)
        self.assertEqual(first.returncode, 0, first.stderr + first.stdout)
        header = output / 'sample' / 'spu_recomp.h'
        source = output / 'sample' / 'spu_recomp.c'
        header_text = header.read_text()
        source_text = source.read_text()
        self.assertIn('#define SPU_LIFTER_ABI 2u', header_text)
        self.assertIn('ps3_spu_lifter_abi_check(SPU_LIFTER_ABI);', source_text)
        header.write_text(header_text.replace('#define SPU_LIFTER_ABI 2u\n', ''))
        source.write_text(source_text.replace(
            '    ps3_spu_lifter_abi_check(SPU_LIFTER_ABI);\n', ''))
        for extra in ((), ('--skip-lift',)):
            with self.subTest(extra=extra):
                (output / 'registry.c').unlink(missing_ok=True)
                result = self.legacy_run(images, output, *extra)
                output_text = result.stderr + result.stdout
                self.assertNotEqual(result.returncode, 0, output_text)
                self.assertIn('SPU_LIFTER_ABI', output_text)
                self.assertIn('re-lift', output_text)
                self.assertFalse((output / 'registry.c').exists())

    def test_reuse_of_stamped_lift_still_succeeds(self):
        images = self.legacy_images()
        output = self.root / 'lifted'
        first = self.legacy_run(images, output)
        self.assertEqual(first.returncode, 0, first.stderr + first.stdout)
        second = self.legacy_run(images, output)  # no --relift: reuse
        self.assertEqual(second.returncode, 0, second.stderr + second.stdout)
        self.assertTrue((output / 'registry.c').exists())

    def test_custom_lifter_without_stamp_fails(self):
        images = self.legacy_images()
        output = self.root / 'custom_lift'
        fake = self.root / 'fake_lifter.py'
        fake.write_text(
            "import pathlib, sys\n"
            "out = pathlib.Path(sys.argv[sys.argv.index('-o') + 1])\n"
            "out.mkdir(parents=True, exist_ok=True)\n"
            "(out / 'spu_recomp.h').write_text('#pragma once\\n"
            "#include \"spu_context.h\"\\n"
            "void sample_spu_recomp_register(void);\\n')\n"
            "(out / 'spu_recomp.c').write_text('#include \"spu_recomp.h\"\\n"
            "void sample_spu_recomp_register(void) {}\\n')\n")
        result = self.legacy_run(images, output, '--lifter', str(fake))
        output_text = result.stderr + result.stdout
        self.assertNotEqual(result.returncode, 0, output_text)
        self.assertIn('ps3_spu_lifter_abi_check', output_text)
        self.assertIn('re-lift', output_text)
        self.assertFalse((output / 'registry.c').exists())

    def test_prebuilt_spu_dir_helper_rejects_pre_stamp_lift(self):
        # The template's SPU_DIR branch calls this helper; configure must fail
        # before a pre-stamp image reaches the target.
        cmake = shutil.which('cmake')
        if not cmake:
            if __import__('os').environ.get('CI'):
                self.fail('CMake required in CI')
            self.skipTest('CMake unavailable')
        project = self.root / 'spu_prebuilt'
        image = project / 'spu' / 'sample'
        image.mkdir(parents=True)
        (project / 'main.c').write_text('int main(void) { return 0; }\n')

        def write_lift(stamped):
            header = '#pragma once\n#include "spu_context.h"\n'
            source = '#include "spu_recomp.h"\n'
            if stamped:
                header += ('#define SPU_LIFTER_ABI 1u\n'
                           'void ps3_spu_lifter_abi_check(uint32_t got);\n')
                source += ('void sample_spu_recomp_register(void) { '
                           'ps3_spu_lifter_abi_check(SPU_LIFTER_ABI); '
                           'spu_register_function(0, 0); }\n')
            else:
                header += 'void sample_spu_recomp_register(void);\n'
                source += ('void sample_spu_recomp_register(void) { '
                           'spu_register_function(0, 0); }\n')
            (image / 'spu_recomp.h').write_text(header)
            (image / 'spu_recomp.c').write_text(source)

        (project / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.20)\n'
            'project(spu_prebuilt C)\n'
            f'set(PS3RECOMP_DIR "{TOOLS.parent.as_posix()}")\n'
            'include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")\n'
            'ps3recomp_spu_prebuilt_sources(SPU_PREBUILT_SOURCES SPU_IMAGE_COUNT '
            '"${CMAKE_CURRENT_SOURCE_DIR}/spu")\n'
            'add_executable(proof main.c ${SPU_PREBUILT_SOURCES})\n')

        write_lift(False)
        configured = subprocess.run(
            [cmake, '-S', str(project), '-B', str(self.root / 'build_stale'),
             '-DPython3_EXECUTABLE=' + sys.executable],
            capture_output=True, text=True)
        output_text = configured.stdout + configured.stderr
        self.assertNotEqual(configured.returncode, 0, output_text)
        self.assertIn('SPU_LIFTER_ABI', output_text)
        self.assertIn('Re-lift', output_text)

        write_lift(True)
        configured = subprocess.run(
            [cmake, '-S', str(project), '-B', str(self.root / 'build_stamped'),
             '-DPython3_EXECUTABLE=' + sys.executable],
            capture_output=True, text=True)
        self.assertEqual(configured.returncode, 0,
                         configured.stdout + configured.stderr)

    def test_legacy_images_mode_still_builds(self):
        images = self.root / 'images'
        images.mkdir()
        (images / 'sample.elf').write_bytes(elf())
        result = subprocess.run([sys.executable, str(TOOLS / 'build_spu_workloads.py'),
                                 '--images', str(images), '--lifted', str(self.output),
                                 '--out', str(self.output / 'registry.c')], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn('sample_spu_func_00000100', (self.output / 'registry.c').read_text())


if __name__ == '__main__':
    unittest.main()
