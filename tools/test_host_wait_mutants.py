#!/usr/bin/env python3
"""Seeded-bug check for #128: each mutant of the REAL wait code must fail the named fixture.

A mirror of runtime/, include/ and tests/sync_stress is copied to a temp dir, one source line is changed there, the
fixtures are built from the mirror with cc and run. The tree under test is never touched. A mutant that is not
caught, or a baseline that does not pass, fails this script. Fixtures that wait forever on a mutant are cut off by
the run timeout and count as caught.

usage: test_host_wait_mutants.py [--list]
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: POSIX host primitives')
    raise SystemExit(0)

ROOT = Path(__file__).resolve().parents[1]
CC = os.environ.get('CC', 'cc')
CXX = os.environ.get('CXX', 'c++')

# name, file, old text, new text, fixture
MUTANTS = [
    ('REALTIME deadline built in (absolute backend)', 'runtime/platform/host_wait.c',
     "const uint64_t abs_ns = now_ns + rel_ns;",
     ("struct timespec w_; clock_gettime(CLOCK_REALTIME, &w_); "
     "const uint64_t abs_ns = (uint64_t)w_.tv_sec * NS_PER_SEC + (uint64_t)w_.tv_nsec + rel_ns;"),
     'seam_abs'),
    ('full duration reused after a wake', 'runtime/platform/host_wait.c',
     "if (left > PS3_HW_MAX_SLICE_NS) left = PS3_HW_MAX_SLICE_NS;",
     "left = budget->duration_ns; if (left > PS3_HW_MAX_SLICE_NS) left = PS3_HW_MAX_SLICE_NS;",
     'seam_rel'),
    ('guest timeout 0 treated as immediate (queue receive)', 'runtime/syscalls/sys_event.c',
     "ps3_wait_budget_us(timeout_us, timeout_us == 0);\n    while (q->count == 0",
     "ps3_wait_budget_us(timeout_us, 0);\n    while (q->count == 0",
     'consumer_abs'),
    ('nanosecond carry lost (absolute backend)', 'runtime/platform/host_wait.c',
     "abs.tv_sec  = (time_t)(abs_ns / NS_PER_SEC);\n    abs.tv_nsec = (long)(abs_ns % NS_PER_SEC);",
     ("abs.tv_sec  = (time_t)(now_ns / NS_PER_SEC + rel_ns / NS_PER_SEC);\n"
     "    abs.tv_nsec = (long)(now_ns % NS_PER_SEC + rel_ns % NS_PER_SEC);"),
     'seam_abs'),
    ('usec to ns conversion wraps instead of saturating', 'runtime/platform/host_wait.c',
     "usec > UINT64_MAX / 1000u ? UINT64_MAX : usec * 1000u", "usec * 1000u", 'seam_rel'),
    ('condition wait does not re-take the guest mutex', 'runtime/syscalls/sys_cond.c',
     "for (int i = 0; i < depth; ++i) guest_lock(m);\n    __atomic_sub_fetch",
     "__atomic_sub_fetch", 'consumer_rel'),
    ('host error counted as a timeout', 'runtime/platform/host_wait.c',
     "if (rc != ETIMEDOUT && rc != EINTR) return rc;",
     "if (rc != ETIMEDOUT && rc != EINTR) return ETIMEDOUT;", 'seam_rel'),
    ('host error counted as a wake', 'runtime/platform/host_wait.c',
     "if (rc != ETIMEDOUT && rc != EINTR) return rc;",
     "if (rc != ETIMEDOUT && rc != EINTR) return 0;", 'seam_rel'),
    ('semaphore host error mapped to success path (loops on)', 'runtime/syscalls/sys_semaphore.c',
     "if (rc != 0) {                             /* the host refused the wait */",
     "if (0) {                             /* the host refused the wait */", 'consumer_abs'),
    ('timer period budget reset on every wake', 'runtime/syscalls/sys_timer.c',
     "const ps3_wait_budget period = ps3_wait_budget_us(t->period_usec, 0);\n        int rc;\n        do {\n            rc = ps3_cond_wait_monotonic(&t->cv, &t->mtx, &period);",
     "int rc;\n        do {\n            const ps3_wait_budget period = ps3_wait_budget_us(t->period_usec, 0);\n            rc = ps3_cond_wait_monotonic(&t->cv, &t->mtx, &period);",
     'timer_rel'),
    ('Linux condvar left on the default clock (no setclock)', 'runtime/platform/host_wait.c',
     "rc = HW_CONDATTR_SETCLOCK(&attr, CLOCK_MONOTONIC);", "rc = 0;", 'seam_abs'),
]


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, check=False, **kw)


def build_and_run(mirror, which, tmp):
    m = mirror
    rel = '1' if which.endswith('rel') else '0'
    out = Path(tmp) / f'fx_{which}'
    inc = ['-I' + str(m), '-I' + str(m / 'include'), '-I' + str(m / 'runtime'), '-I' + str(m / 'runtime/syscalls'),
           '-I' + str(m / 'runtime/platform'), '-I' + str(m / 'tests')]
    flags = ['-std=gnu17', '-O1', '-w', '-D_GNU_SOURCE', '-ffunction-sections', '-DPS3_HW_RELATIVE=' + rel, '-DHW_FAKE_DEFINES_HOST_CLOCK=1', *inc]
    if which.startswith('timer'):
        srcs = [m / 'tests/sync_stress/test_timer_monotonic.c', m / 'tests/sync_stress/test_shims.c',
                m / 'runtime/platform/tests/host_wait_fake.c', m / 'runtime/platform/win32_compat.c'] + \
               [m / f'runtime/syscalls/{n}.c' for n in ('sys_timer', 'sys_event')]
        cxx = [m / 'runtime/diag/stop.cpp']
    elif which.startswith('seam'):
        srcs = [m / 'runtime/platform/tests/test_host_wait_monotonic.c', m / 'runtime/platform/tests/host_wait_fake.c']
        cxx = []
    else:
        srcs = [m / 'tests/sync_stress/test_timeout_monotonic.c', m / 'tests/sync_stress/test_shims.c',
                m / 'runtime/platform/tests/host_wait_fake.c'] + [m / f'runtime/syscalls/{n}.c' for n in
                ('sys_mutex', 'sys_cond', 'sys_semaphore', 'sys_event')]
        cxx = [m / 'runtime/diag/stop.cpp']
    objs = []
    for i, s in enumerate(srcs):
        o = Path(tmp) / f'{out.name}-{i}.o'
        r = sh([CC, *flags, '-c', str(s), '-o', str(o)])
        if r.returncode:
            return None, 'compile: ' + r.stderr[-300:]
        objs.append(str(o))
    for i, s in enumerate(cxx):
        o = Path(tmp) / f'{out.name}-cxx{i}.o'
        r = sh([CXX, '-std=c++20', '-O1', '-w', '-I' + str(m / 'include'), '-c', str(s), '-o', str(o)])
        if r.returncode:
            return None, 'compile: ' + r.stderr[-300:]
        objs.append(str(o))
    r = sh([CXX, *objs, '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections', '-pthread', '-o', str(out)])
    if r.returncode:
        return None, 'link: ' + r.stderr[-300:]
    try:
        r = sh([str(out)], timeout=30)
    except subprocess.TimeoutExpired:
        return 1, 'timeout (a wait that never ends)'
    return r.returncode, (r.stdout + r.stderr)[-160:].replace('\n', ' ')


def main():
    if '--list' in sys.argv:
        for m in MUTANTS:
            print(f'{m[4]:13} {m[0]}')
        return 0
    tmp = tempfile.mkdtemp(prefix='ps3-mutants-')
    bad = 0
    try:
        base = Path(tmp) / 'tree'
        for d in ('runtime', 'include', 'tests/sync_stress'):
            shutil.copytree(ROOT / d, base / d, ignore=shutil.ignore_patterns('*.o', 'build*', '*.elf', '*.exe'))
        for f in (ROOT / 'tests').glob('*.h'):
            shutil.copy(f, base / 'tests' / f.name)
        for which in ('seam_abs', 'seam_rel', 'consumer_abs', 'consumer_rel', 'timer_rel'):
            rc, msg = build_and_run(base, which, tmp)
            print(f'baseline {which}: {"ok" if rc == 0 else "FAIL " + str(msg)}')
            bad += rc != 0
        for name, rel, old, new, which in MUTANTS:
            mt = Path(tmp) / 'mut'
            if mt.exists():
                shutil.rmtree(mt)
            shutil.copytree(base, mt)
            p = mt / rel
            t = p.read_text()
            if old not in t:
                print(f'MUTANT NOT APPLIED: {name}')
                bad += 1
                continue
            p.write_text(t.replace(old, new, 1))
            rc, msg = build_and_run(mt, which, tmp)
            caught = rc not in (0, None)
            print(f'{"caught " if caught else "MISSED "} [{which}] {name}: {msg}')
            bad += not caught
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
