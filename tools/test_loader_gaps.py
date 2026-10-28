#!/usr/bin/env python3
"""Every gap the loader used to paper over is a named stop.

Lifts a synthetic image with the real ppu_lifter.py (one function that is fine,
one that contains an instruction the lifter cannot translate), builds the real
ppu_loader.cpp against that lift, and runs tests/test_loader_gaps.cpp:

  - an lv2 syscall the firmware implements and nobody here does: stop
    unimplemented-syscall, naming the number; allow-listed in [[lv2.stub]] it
    returns exactly the configured value, even when the firmware's slot is the
    ENOSYS stub;
  - a slot the FW 4.93 stock lv2_kernel leaves on the shared ENOSYS stub:
    answered 0x80010003 per call, with r4..r10 and guest memory intact and the
    PS3_SCTRACE note once per slot; a slot RPCS3 binds although stock says
    ENOSYS keeps the stop;
  - lv2 syscall 988 is the table's handler (kind id and flag, firmware contract),
    not an inline TTY write: it reads no guest memory and changes only r3;
  - a call into a function the lift does not contain: unlifted-function, naming
    its address and the caller;
  - an indirect call to no function: unresolved-indirect-call at the first hit;
  - the lifted function that runs up to the unknown instruction: it does not stop
    before it, it stops AT it (unknown-instruction, with the pc and word);
  - the lift report lists that instruction;
  - data the lifted code cannot reach (a literal pool branched over, padding
    after a return) is not lowered: no trap, listed as dead, and the code
    around it still runs;
  - a branch out of the lifted code (here: past the image's end) stops
    (unlifted-function, naming the target) instead of returning;
  - a live in-range branch to an address with no decoded instruction stops
    (unlifted-function, naming the target) and is listed in the report instead
    of falling off the function;
  - a branch below the raw image base defines no function and stops naming
    the target;
  - a lifted function that calls itself through ctr stops at the host
    dispatch-depth cap (dispatch-depth-exceeded, target/depth/limit/caller_lr),
    never with a fake return;
  - an indirect call whose ctr is not a lifted function but whose r2 is stops
    (unresolved-indirect-call) with a hint naming ppu.opd_swap_fixup off, and
    runs the r2 function when ppu.opd_swap_fixup = true;
  - a host-initiated guest callback (ppu_guest_call / ppu_guest_call_ct) with a
    NULL OPD, a 0 code word or an unregistered code stops
    (unresolved-guest-callback, carrying origin/opd/code/toc/descriptor) and
    never prints a return; a registered OPD/code still runs;
  - host-initiated callbacks get guest stack space of their own: concurrent
    threads with and without a guest context, and callbacks that re-enter
    ppu_guest_call_ct, run on distinct, non-overlapping stacks with their
    outer frames, canaries and return values intact; a host thread's stack slot
    is released when it exits; a callback that cannot be given a stack (r1 that
    is no stack address, a VM below the host stack window, a host slot nested
    out of room) stops (unresolved-guest-callback, origin stack).
  - a host thread with no guest context has a guest thread id of its own for its
    callbacks (kept for life, never reused, outside every real thread id), so two
    of them running at once are two owners to an lwmutex: one that holds it blocks
    the other, and the other's trylock is EBUSY and its unlock EPERM;
  - a guest load or store into the null page stops (guest-null-access, with
    access/width/ea/fn/site=guest and the site) at every width, below 0x10000 for loads and
    0x1000 for stores; ppu.null_page = "drop" keeps the legacy log, dropped
    stores and sweep containment. (A host-side handler's access is tools/test_stop_attribution.py.)

A line "RETURNED" from the fixture means the runtime carried on; every case that
must stop treats it as failure.
"""
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests' / 'gates'))
from check_ppu_scaffold import lifter_abi_sources, loader_support_objects, loader_support_sources  # noqa: E402
import gates_common as g  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
# A: li r3,5 ; blr          B: li r3,7 ; .word 0 (no instruction) ; blr
# C: b +0x10 ; .word 0xFFFFFFFF, 0, 0 (a literal pool) ; li r3,3 ; blr ; .word 0 (padding)
# D: li r3,9 ; b 0x20000 (outside the image)
# E: bctr ; .word 0 ; bl 0x10004 (dead after the computed jump: no trap, no func_00010004)
# F: mtctr r4 ; bctrl ; blr -- calls ctr, which the fixture sets to F's own
# address in r4, so it recurses through ps3_indirect_call until the depth cap
# H: li r3,13 ; b 0xFF00 (below the raw image base 0x10000: not a function, stop)
# G: li r3,11 ; b 0x10060 (in range but past the image bytes: a live path with no
#    decoded instruction at the target -- its label must stop, not fall off)
CODE = struct.pack('>IIIII', 0x38600005, 0x4E800020, 0x38600007, 0x00000000, 0x4E800020) + \
    struct.pack('>IIIIIII', 0x48000010, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x38600003, 0x4E800020,
                0x00000000) + \
    struct.pack('>II', 0x38600009, 0x48000000 | (0x20000 - 0x10034)) + \
    struct.pack('>III', 0x4E800420, 0x00000000, 0x48000001 | ((0x10004 - 0x10040) & 0x3FFFFFC)) + \
    struct.pack('>III', 0x7C8903A6, 0x4E800421, 0x4E800020) + \
    struct.pack('>II', 0x3860000D, 0x4BFFFEAC) + \
    struct.pack('>II', 0x3860000B, 0x48000004)


def main():
    if os.name == 'nt':
        print('SKIP: POSIX fixture')
        return 0
    with tempfile.TemporaryDirectory(prefix='ppu-gaps-') as tmp:
        tmp = Path(tmp)
        (tmp / 'code.bin').write_bytes(CODE)
        (tmp / 'fn.json').write_text(json.dumps([{'start': '0x10000', 'end': '0x10008'},
                                                 {'start': '0x10008', 'end': '0x10014'},
                                                 {'start': '0x10014', 'end': '0x10030'},
                                                 {'start': '0x10030', 'end': '0x10038'},
                                                 {'start': '0x10038', 'end': '0x10044'},
                                                 {'start': '0x10048', 'end': '0x10050'},
                                                 {'start': '0x10050', 'end': '0x10058'},
                                                 {'start': '0x10058', 'end': '0x10064'}]))
        lift = tmp / 'lift'
        subprocess.run([sys.executable, str(ROOT / 'tools/ppu_lifter.py'), str(tmp / 'code.bin'), '--raw',
                        '--base', '0x10000', '--functions', str(tmp / 'fn.json'), '--jobs', '1',
                        '--code-end', '0x10064',   # the declared end of the last function, past the image bytes
                        '--single-file', '--source-name', 'ppu_recomp.cpp', '--output', str(lift)],
                       check=True, capture_output=True)
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
        binary = tmp / 'gaps'
        # The handler under test is C11 (atomic_flag): an object, like the loader's other C dependencies.
        sys_dbg_obj = tmp / 'sys_dbg.o'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-c', '-I' + str(ROOT / 'include'),
                        str(ROOT / 'runtime/syscalls/sys_dbg.c'), '-o', str(sys_dbg_obj)], check=True)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-w', '-ffunction-sections',
                        '-fdata-sections', '-I' + str(lift), '-I' + str(ROOT / 'include'),
                        '-I' + str(ROOT / 'runtime/ppu'), '-I' + str(ROOT / 'runtime/platform'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'), str(ROOT / 'tests/test_loader_gaps.cpp'),
                        str(ROOT / 'runtime/syscalls/sys_lwsync.cpp'), str(sys_dbg_obj),
                        str(lift / 'ppu_recomp.cpp'), *loader_support_sources(), *lifter_abi_sources(), *loader_support_objects(tmp),
                        *flags, '-o', str(binary)],
                       check=True, timeout=300)

        def run(*args, cfg=None):
            env = g.clean_env(**({'PS3RECOMP_CONFIG': str(cfg)} if cfg else {}))
            return g.run([str(binary), *args], env=env)

        # the lift report records the instruction with its pc, word and text
        report = json.loads((lift / 'ppu_lift_report.json').read_text())
        assert report['summary']['trapped_instructions'] == 1, report
        fn = report['functions'][0]
        assert fn['name'] == 'func_00010008' and fn['traps'][0][:2] == ['0x0001000C', '0x00000000'], fn
        assert 'word' in fn['traps'][0][2], fn
        # the pool and the padding in C are dead in the lifted code: listed, not trapped
        fc = report['functions'][1]
        assert fc['name'] == 'func_00010014' and fc['traps'] == [], fc
        assert fc['dead'] == [['0x00010018', '0x00010020'], ['0x0001002C', '0x0001002C']], fc
        assert fn['dead'] == [['0x00010010', '0x00010010']], fn   # the blr after the trap
        fe = report['functions'][2]
        assert fe['name'] == 'func_00010038' and fe['traps'] == [], fe
        assert fe['dead'] == [['0x0001003C', '0x00010040']], fe
        # 8 declared functions, plus func_00010044 (E's tail entry): 9 total;
        # no function is seeded by DEAD code alone
        assert report['summary']['functions'] == 9, report
        # a live branch to an in-range address with no decoded instruction stops
        # at the target instead of falling off the function; the report lists it
        fg = report['functions'][3]
        assert fg['name'] == 'func_00010058' and fg['traps'] == [] and fg['dead'] == [], fg
        assert fg['undecoded_targets'] == ['0x00010060'], fg
        assert report['summary']['dead_instructions'] == 7, report
        assert report['summary']['undecoded_targets'] == 1, report
        assert report['summary']['functions_with_undecoded_targets'] == 1, report
        # a branch below the raw image base never becomes a function
        src = (lift / 'ppu_recomp.cpp').read_text()
        assert 'func_0000FF00' not in src, 'below-base target became a function'
        assert 'func_00010060' not in src, 'undecoded target became a function'
        # the parallel lift (the default: --jobs = CPU count) reports the same gaps
        subprocess.run([sys.executable, str(ROOT / 'tools/ppu_lifter.py'), str(tmp / 'code.bin'), '--raw',
                        '--base', '0x10000', '--functions', str(tmp / 'fn.json'), '--jobs', '2',
                        '--code-end', '0x10064',
                        '--single-file', '--source-name', 'ppu_recomp.cpp', '--output', str(tmp / 'lift2')],
                       check=True, capture_output=True)
        report2 = json.loads((tmp / 'lift2' / 'ppu_lift_report.json').read_text())
        assert report2 == report, (report, report2)

        # A conditional branch that can fall off the function must reach the
        # continuation before a tail stop label: only `goto loc_X` may land on
        # the stop. li r3,21 ; beq 0x0C (0x0C is in [0,0x10) but past the bytes).
        (tmp / 'tail.bin').write_bytes(bytes.fromhex('3860001541820008'))
        (tmp / 'tail.json').write_text(json.dumps([{'start': '0x0', 'end': '0x10'}]))
        subprocess.run([sys.executable, str(ROOT / 'tools/ppu_lifter.py'), str(tmp / 'tail.bin'), '--raw',
                        '--base', '0', '--functions', str(tmp / 'tail.json'), '--jobs', '1',
                        '--code-end', '0x10',
                        '--single-file', '--source-name', 'ppu_recomp.cpp',
                        '--output', str(tmp / 'tail')],
                       check=True, capture_output=True)
        tail_src = (tmp / 'tail' / 'ppu_recomp.cpp').read_text()
        tail_report = json.loads((tmp / 'tail' / 'ppu_lift_report.json').read_text())
        assert tail_report['functions'][0]['undecoded_targets'] == ['0x0000000C'], tail_report
        assert (tail_src.index('missing continuation 0x00000010')
                < tail_src.index('loc_0000000C: ppu_unlifted_stub')), tail_src

        # a function that is fine runs; the one with the unknown instruction does not stop before it
        p = run('call', '0x10000')
        assert p.returncode == 0 and 'RETURNED r3=0x5' in p.stdout, (p.stdout, p.stderr)
        p = run('call', '0x10014')
        assert p.returncode == 0 and 'RETURNED r3=0x3' in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(run('call', '0x10008'), 'unknown-instruction')
        assert (f['pc'], f['word']) == ('0x1000C', '0x00000000') and f['tid'] == '1', f
        # a branch out of the lifted code stops naming the target, it does not return
        p = run('call', '0x10030')
        assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(p, 'unlifted-function')
        assert f['target'] == '0x00020000', f
        # a live goto to an undecoded in-range address stops naming it
        p = run('call', '0x10058')
        assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(p, 'unlifted-function')
        assert f['target'] == '0x00010060', f
        # a branch below the raw image base stops naming the target
        p = run('call', '0x10050')
        assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(p, 'unlifted-function')
        assert f['target'] == '0x0000FF00', f

        # A slot the firmware implements and the toolkit does not is still a
        # named stop. Slot 2 has a handler in the FW 4.93 stock lv2_kernel
        # table (docs/LV2-SYSCALL-STATUS.md).
        p = run('syscall', '2')
        assert 'RETURNED' not in p.stdout, 'the runtime answered an unimplemented syscall'
        f = g.expect_stop(p, 'unimplemented-syscall')
        assert f['num'] == '2' and f['caller_lr'] == '0x00010100', f
        # A slot stock 4.93 leaves on the kernel's shared ENOSYS stub is
        # answered 0x80010003 per call, as the stub does; r4..r10 and guest
        # memory stay. Slot 6 is one of the CFW-only handlers (present in the
        # 4.81 REBUG/Cobra dump, ENOSYS on stock), so this pins the stock map
        # as primary.
        p = run('scenosys', '6')
        assert p.returncode == 0, (p.stdout, p.stderr)
        assert 'SCENOSYS regs_intact=1 memory_intact=1 answers_ok=1' in p.stdout, (p.stdout, p.stderr)
        assert 'RETURNED r3=0xFFFFFFFF80010003' in p.stdout, (p.stdout, p.stderr)
        assert 'lv2 ENOSYS slot' not in p.stderr, p.stderr
        # The PS3_SCTRACE diagnostic says so once per slot, not once per call.
        p = g.run([str(binary), 'scenosys', '6', '2'], env=g.clean_env(PS3_SCTRACE='1'))
        assert p.returncode == 0, (p.stdout, p.stderr)
        assert p.stderr.count('lv2 ENOSYS slot 6 answered 0x80010003') == 1, p.stderr
        # RPCS3 binds slot 60 although the stock table says ENOSYS: the
        # 4.93-vs-RPCS3 conflict keeps the stop.
        g.expect_stop(run('syscall', '60'), 'unimplemented-syscall')
        # The allow-list wins over the firmware ENOSYS answer.
        cfg = tmp / 'title.toml'
        cfg.write_text('schema = 1\n[[lv2.stub]]\nnum = 16\nret = 0x12345678\nreason = "synthetic test gap"\n')
        p = run('syscall', '16', cfg=cfg)
        assert p.returncode == 0 and 'RETURNED r3=0x12345678' in p.stdout, (p.stdout, p.stderr)
        assert 'ALLOW-LISTED unimplemented syscall 16' in p.stderr and 'synthetic test gap' in p.stderr
        g.expect_stop(run('syscall', '2', cfg=cfg), 'unimplemented-syscall')   # only the listed number

        # Syscall 988 is not sys_tty_write: with r4 = 0 and r5 pointing into live memory it
        # reads no buffer. Contract from the lv2 handler (sys_dbg.c): kind ids 1, 2, 4, 8 and
        # 16 with a zero flag answer CELL_OK, anything else CELL_EINVAL, judged on the low
        # words; no register but r3 and no guest byte changes, nothing is printed.
        EINVAL = 'RETURNED r3=0xFFFFFFFF80010002'
        for r3, r4, want in [('1', '0', 'RETURNED r3=0x0'), ('4', '0', 'RETURNED r3=0x0'),
                             ('2', '0', 'RETURNED r3=0x0'), ('8', '0', 'RETURNED r3=0x0'),
                             ('16', '0', 'RETURNED r3=0x0'),
                             ('0x100000001', '0', 'RETURNED r3=0x0'),       # id is the low word
                             ('1', '0x100000000', 'RETURNED r3=0x0'),       # so is the flag
                             ('0', '0', EINVAL), ('3', '0', EINVAL), ('12', '0', EINVAL),
                             ('17', '0', EINVAL), ('0xFFFFFFFF', '0', EINVAL),
                             ('1', '1', EINVAL), ('4', '0x80000000', EINVAL)]:
            p = run('sc988', r3, r4)
            where = (r3, r4, p.returncode, p.stdout, p.stderr)
            assert p.returncode == 0 and 'PS3STOP' not in p.stdout + p.stderr, where
            assert p.stdout.splitlines() == ['SC988 regs_intact=1 memory_intact=1', want], where
            assert p.stderr == '', where

        # call into a function this lift does not contain
        p = run('unlifted', '0x12340')
        assert 'RETURNED' not in p.stdout
        f = g.expect_stop(p, 'unlifted-function')
        assert f['target'] == '0x00012340' and f['pc'] == '0x12340' and f['caller_lr'] == '0x00010100', f

        # indirect call to nothing: stops at the first hit
        p = run('indirect', '0x00020000')
        assert 'RETURNED' not in p.stdout
        f = g.expect_stop(p, 'unresolved-indirect-call')
        assert f['target'] == '0x00020000' and 'descriptors' in f and 'target_words' in f, f
        f = g.expect_stop(run('indirect', '0x39800000'), 'unresolved-indirect-call')
        assert 'li r12,0' in f['hint'], f
        f = g.expect_stop(run('indirect', '0x20000000'), 'unresolved-indirect-call')
        assert 'data/heap' in f['hint'], f
        # a NULL function pointer is not a "return to the caller": it stops too
        p = run('indirect', '0x0')
        assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(p, 'unresolved-indirect-call')
        assert f['target'] == '0x00000000' and 'NULL function pointer' in f['hint'], f

        # a lifted function that calls ITSELF through ctr recurses on the host
        # stack until the dispatch-depth cap: a named stop, never a fake return
        p = run('depth', '0x10048')
        assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(p, 'dispatch-depth-exceeded')
        assert (f['target'], f['pc'], f['limit'], f['tid']) == (
            '0x00010048', '0x10048', '4000', '1'), f
        assert int(f['depth']) > 4000, f

        # ctr is not a lifted function but r2 is: off by default, so the
        # unresolved stop says the OPD may be swapped and names the knob
        p = run('opdswap', '0x00020000', '0x10000')
        assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(p, 'unresolved-indirect-call')
        assert f['target'] == '0x00020000', f
        assert 'r2 holds a lifted function' in f['hint'] and 'ppu.opd_swap_fixup' in f['hint'], f
        # ... and with the knob on, the r2 function runs (r3=5) instead of stopping
        opd_cfg = tmp / 'opd.toml'
        opd_cfg.write_text('schema = 1\n[ppu]\nopd_swap_fixup = true\n')
        p = run('opdswap', '0x00020000', '0x10000', cfg=opd_cfg)
        assert p.returncode == 0 and 'RETURNED r3=0x5' in p.stdout, (p.stdout, p.stderr)

        # A host-initiated guest callback that never ran must not look like one
        # that returned 0: NULL OPD, 0 code and unregistered code all stop.
        p = run('guestcall', '0x0')
        assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(p, 'unresolved-guest-callback')
        assert (f['origin'], f['opd'], f['code'], f['pc']) == (
            'opd', '0x00000000', '0x00000000', '0x0'), f
        assert f['descriptor'] == 'none' and 'OPD 0' in f['hint'], f

        p = run('guestcallopd', '0x00020000', '0', '0x10000')
        assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(p, 'unresolved-guest-callback')
        assert (f['origin'], f['opd'], f['code'], f['toc']) == (
            'opd', '0x00020000', '0x00000000', '0x00010000'), f
        assert f['descriptor'].startswith('00000000 00010000'), f
        assert 'code word is 0' in f['hint'], f

        p = run('guestcallopd', '0x00020000', '0x12340', '0x10000')
        assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(p, 'unresolved-guest-callback')
        assert f['code'] == '0x00012340' and f['descriptor'].startswith('00012340'), f
        assert 'no lifted function starts at' in f['hint'], f

        for code, toc in (('0x0', '0x00010000'), ('0x12340', '0x0')):
            p = run('guestcallct', code, toc)
            assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
            f = g.expect_stop(p, 'unresolved-guest-callback')
            assert (f['origin'], f['opd'], f['code'], f['toc']) == (
                'code', '0x00000000', "0x%08X" % int(code, 16), "0x%08X" % int(toc, 16)), f

        # a registered OPD/code still runs and returns its r3
        p = run('guestcallopd', '0x00020000', '0x10000', '0x0')
        assert p.returncode == 0 and 'RETURNED r3=0x5' in p.stdout, (p.stdout, p.stderr)
        p = run('guestcallct', '0x10000')
        assert p.returncode == 0 and 'RETURNED r3=0x5' in p.stdout, (p.stdout, p.stderr)

        # Every active host-initiated callback has stack space no other frame uses
        p = run('cbstack', '0')
        assert p.returncode == 0 and 'CBSTACK OK' in p.stdout, (p.stdout, p.stderr)
        # Two host threads with no guest context: distinct, stable, never-reused thread ids
        p = run('hostid', '0')
        assert p.returncode == 0 and 'HOSTID OK' in p.stdout, (p.stdout, p.stderr)
        p = run('cbexhaust', '0')
        assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(p, 'unresolved-guest-callback')
        assert f['origin'] == 'stack' and 'nest too deep' in f['hint'], f
        p = run('cbbadr1', '0')
        assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(p, 'unresolved-guest-callback')
        assert f['origin'] == 'stack' and f['sp'] == '0x00000000' and f['tid'] == '7', f
        assert 'not a guest stack address' in f['hint'], f
        p = run('cbsmallvm', '0')
        assert 'RETURNED' not in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(p, 'unresolved-guest-callback')
        assert f['origin'] == 'stack' and f['vm_size'] == '0x20000000', f
        assert 'ends below the host callback stack window' in f['hint'], f
        # a host-slot callback whose r1 is at / below its slot's lowest address must
        # not let a nested callback run on whatever memory lies there
        for below in ('0', '0x1000', '0x100000'):
            p = run('cbleave', below)
            assert 'RETURNED' not in p.stdout and 'NESTED CALLBACK RAN' not in p.stdout, (below, p.stdout, p.stderr)
            f = g.expect_stop(p, 'unresolved-guest-callback')
            assert f['origin'] == 'stack' and 'left its host callback stack slot' in f['hint'], (below, f)

        # A guest load or store into the null page is a named stop by default. The
        # accesses are the ones the Odin canary made (NULL+0x0..0x38 and
        # NULL+0x5B0..0x5DC), at every width, through the same inline load path
        # the lifted code uses: below 0x10000 it falls back to the checked accessor.
        hexfn = re.compile(r'^0x[0-9A-F]{8}$')
        for access, ea, width in (('read', '0x0', '4'), ('read', '0x38', '8'), ('read', '0x5B0', '2'),
                                  ('read', '0x5DC', '1'), ('read', '0x2000', '4'), ('read', '0xFFFC', '4'),
                                  ('write', '0x0', '4'), ('write', '0x5B0', '8'), ('write', '0x5DC', '2'),
                                  ('write', '0xFFF', '1')):
            p = run('null' + access, ea, width)
            assert 'RETURNED' not in p.stdout, (access, ea, width, p.stdout, p.stderr)
            f = g.expect_stop(p, 'guest-null-access')
            assert (f['access'], f['width'], f['ea'], f['tid']) == (
                access, width, '0x%08X' % int(ea, 16), '1'), (access, ea, width, f)
            assert hexfn.match(f['fn']) and int(f['pc'], 16) == int(f['fn'], 16) and f['lr'] == '0x10100', f
            assert f['site'] == 'guest' and 'in_syscall' not in f and 'in_hle' not in f, f
            assert '[null-read]' not in p.stderr and '[null-write]' not in p.stderr, p.stderr
        # the first store of a sweep stops; nothing is dropped and carried on from
        f = g.expect_stop(run('nullsweep', '0'), 'guest-null-access')
        assert (f['access'], f['ea']) == ('write', '0x00000010'), f
        # the bounds are the ones the checks always had: loads below 0x10000, stores below 0x1000
        p = run('nullread', '0x10000', '4')
        assert p.returncode == 0 and 'RETURNED r3=0xABABABAB' in p.stdout, (p.stdout, p.stderr)
        for ea in ('0x1000', '0x2000'):
            p = run('nullwrite', ea, '4')
            assert p.returncode == 0 and 'RETURNED r3=0x5A' in p.stdout, (ea, p.stdout, p.stderr)

        # ppu.null_page = "drop": the legacy handling for a title that needs it. The
        # file carries the reason, as docs/CONFIG.md requires.
        drop_cfg = tmp / 'drop.toml'
        drop_cfg.write_text('schema = 1\n[ppu]\n# synthetic test: stands in for a title that dereferences\n'
                            '# page 0 on purpose\nnull_page = "drop"\n')
        p = run('nullread', '0x38', '4', cfg=drop_cfg)
        assert p.returncode == 0 and 'RETURNED r3=0xABABABAB' in p.stdout, (p.stdout, p.stderr)
        assert '[null-read] guest read32 from 0x00000038' in p.stderr, p.stderr
        p = run('nullread', '0x5B0', '8', cfg=drop_cfg)
        assert p.returncode == 0 and 'RETURNED r3=0xABABABABABABABAB' in p.stdout, (p.stdout, p.stderr)
        for width in ('1', '2', '4', '8'):
            p = run('nullwrite', '0x5B0', width, cfg=drop_cfg)
            assert p.returncode == 0 and 'RETURNED r3=0xAB' in p.stdout, (width, p.stdout, p.stderr)   # dropped
            assert '[null-write] guest wrote%d' % (int(width) * 8) in p.stderr and 'DROPPED' in p.stderr, p.stderr
        # the sweep out of the null page stays dropped, and its end lets the next store land
        p = run('nullsweep', '0', cfg=drop_cfg)
        assert p.returncode == 0 and 'SWEEP 10=AB 1010=AB 3000=5A' in p.stdout, (p.stdout, p.stderr)
        assert '[null-write] sweep contained' in p.stderr, p.stderr
        # the environment sets it too, and anything but stop/drop is a config stop
        p = g.run([str(binary), 'nullread', '0x38', '4'], env=g.clean_env(PS3RECOMP_PPU_NULL_PAGE='drop'))
        assert p.returncode == 0 and 'RETURNED r3=0xABABABAB' in p.stdout, (p.stdout, p.stderr)
        bad_cfg = tmp / 'bad-null-page.toml'
        bad_cfg.write_text('schema = 1\n[ppu]\nnull_page = "land"\n')
        f = g.expect_stop(run('nullread', '0x38', '4', cfg=bad_cfg), 'config-invalid')
        assert f['key'] == 'ppu.null_page' and '"stop" or "drop"' in f['detail'], f
    print('PASS: unimplemented syscall, unlifted call, unresolved indirect call, dispatch depth, '
          'OPD swap, unresolved guest callback, unknown instruction and null-page access all stop '
          '(ppu.null_page=drop keeps the legacy handling); host-initiated '
          'callbacks get distinct, non-overlapping guest stacks and their own thread ids')
    return 0


if __name__ == '__main__':
    sys.exit(main())
