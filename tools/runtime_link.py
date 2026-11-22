"""The runtime sources a host-built test fixture links, in one place.

Almost every runtime file reaches the same few services: the run log (ps3_log.c, which runs
host_exit.c's quick-exit hook), the typed configuration (config.cpp) and the named stops (stop.cpp). A
fixture that compiles one runtime file therefore needs them too, and the list used to be repeated, and
drifted, in every tools/test_*.py. A driver names the groups it needs and lets build() do the rest:

    import runtime_link
    runtime_link.build([ROOT / 'tests/test_x.c', ROOT / 'runtime/syscalls/sys_x.c'], exe, tmp,
                       cflags=['-std=gnu17', '-O1', '-UNDEBUG', '-I' + str(ROOT / 'include')],
                       runtime=('diag', 'platform'))

Groups (RUNTIME):
    diag          ps3_log.c, host_exit.c, config.cpp, stop.cpp: the log, its exit hook, the typed
                  configuration and the stop runtime. The default; most fixtures want exactly this.
    log           ps3_log.c, host_exit.c and tests/ps3_log_mirror_stub.c, for a fixture that supplies the
                  stops and the configuration itself (the single-TU GCM and engine fixtures, through
                  libs/video/tests/gcm_fixture_stubs.h): the stub answers ps3_stop_set_mirror and
                  weakly ps3_config_get and ps3_stop. Do not combine with 'diag'.
    config        config/config.cpp: the real typed configuration, for a 'log' fixture that reads options
                  but defines its own stops (guest_range_catch.h's ps3_stop_guest_range, say).
    platform      win32_compat.c, host_clock.c, host_wait.c: the host clock, the monotonic wait helpers
                  (ps3_cond_init_monotonic, host_monotonic_ns) and the Win32 shims on POSIX.
    label_window  memory/label_window.c: the device-model window the SPU DMA and the RSX walker consult.
    reservation   memory/reservation.c: the lwarx/stwcx. reservation model (resv_*).

A fixture that defines one of these services itself (its own ps3_stop, say) must not ask for 'diag': the
symbols would be defined twice. It asks for 'log' instead (the run log with the fixture's own stops).

The runtime files are built with the flags below, not the caller's, so a fixture's -std or -O does not
change them; sanitizer flags (-fsanitize=..., -g) are carried over so an instrumented fixture is
instrumented throughout. Under clang-cl/cl (the MSVC driver) the caller's flags are used for everything:
the CRT (/MT or /MD) has to match across objects.
"""
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

RUNTIME = {
    'diag': ['runtime/diag/ps3_log.c', 'runtime/diag/host_exit.c',
             'runtime/config/config.cpp', 'runtime/diag/stop.cpp'],
    'log': ['runtime/diag/ps3_log.c', 'runtime/diag/host_exit.c', 'tests/ps3_log_mirror_stub.c'],
    'platform': ['runtime/platform/win32_compat.c', 'runtime/platform/host_clock.c',
                 'runtime/platform/host_wait.c'],
    'label_window': ['runtime/memory/label_window.c'],
    'reservation': ['runtime/memory/reservation.c'],
    'config': ['runtime/config/config.cpp'],
}

_CXX_SUFFIXES = ('.cpp', '.cc', '.cxx')
_RUNTIME_INCLUDES = [str(ROOT / 'include'), str(ROOT / 'runtime/platform'), str(ROOT / 'runtime/spu')]
_RUNTIME_CFLAGS = ['-std=gnu17', '-O1', '-UNDEBUG', '-D_GNU_SOURCE'] + ['-I' + d for d in _RUNTIME_INCLUDES]
_RUNTIME_CXXFLAGS = ['-std=c++20', '-O1', '-UNDEBUG', '-D_GNU_SOURCE'] + ['-I' + d for d in _RUNTIME_INCLUDES]


def sources(runtime=('diag',)):
    """The runtime source files of the named groups, each once, in group order."""
    unknown = [g for g in runtime if g not in RUNTIME]
    if unknown:
        raise ValueError('unknown runtime group: ' + ', '.join(unknown))
    out = []
    for group in runtime:
        for rel in RUNTIME[group]:
            path = ROOT / rel
            if path not in out:
                out.append(path)
    return out


def is_msvc(compiler):
    return Path(compiler).stem.lower() in ('clang-cl', 'cl')


def _is_cxx(path):
    return Path(path).suffix.lower() in _CXX_SUFFIXES


def build(fixture, out, tmp, cflags=(), cxxflags=None, ldflags=(), runtime=('diag',), timeout=None,
          capture_output=False, cc=None, cxx=None):
    """Compile `fixture` (C and C++ files of the test itself) and the runtime groups, link into `out`.

    C files go through `cc` (default $CC) with `cflags`, C++ files through `cxx` (default $CXX) with
    `cxxflags` (default: cflags minus any -std=, plus -std=c++20); the link is `cxx` with the runtime's
    C++ files in. Under clang-cl both use the same driver. A cross build sets both CC and CXX (or both
    arguments): the runtime's C files are compiled with `cc`. Raises CalledProcessError on any failing
    step, like subprocess.run(check=True) (with capture_output the compiler's output is on the
    exception); returns the link step's CompletedProcess."""
    tmp = Path(tmp)
    capture = {'capture_output': True, 'text': True} if capture_output else {}
    cc = cc or os.environ.get('CC', 'clang-cl' if os.name == 'nt' else 'cc')
    cxx = cxx or os.environ.get('CXX', 'clang-cl' if os.name == 'nt' else 'c++')
    msvc = is_msvc(cxx)
    cflags = list(cflags)
    if cxxflags is None:
        cxxflags = [f for f in cflags if not f.startswith('-std=')] + ['-std=c++20']
    cxxflags = list(cxxflags)
    sanitize = [f for f in cflags if f.startswith('-fsanitize') or f == '-g']
    objects = []
    stem = Path(out).name
    jobs = [(Path(s), False) for s in fixture] + [(s, True) for s in sources(runtime)]
    for index, (src, is_runtime) in enumerate(jobs):
        cxx_file = _is_cxx(src)
        if msvc:
            obj = tmp / f'{stem}-{index}-{src.stem}.obj'
            command = [cxx if cxx_file else cc, *(cxxflags if cxx_file else cflags),
                       '/nologo', '/c', ('/Tp' if cxx_file else '/Tc') + str(src), '/Fo' + str(obj)]
        else:
            obj = tmp / f'{stem}-{index}-{src.stem}.o'
            if is_runtime:
                flags = (_RUNTIME_CXXFLAGS if cxx_file else _RUNTIME_CFLAGS) + sanitize
            else:
                flags = cxxflags if cxx_file else cflags
            command = [cxx if cxx_file else cc, *flags, '-c', str(src), '-o', str(obj)]
        subprocess.run(command, check=True, timeout=timeout, **capture)
        objects.append(str(obj))
    if msvc:
        link = [cxx, *objects, '/nologo', '/Fe' + str(out), '/link', *ldflags]
    else:
        link = [cxx, *objects, *sanitize, *ldflags, '-pthread', '-lm', '-o', str(out)]
    return subprocess.run(link, check=True, timeout=timeout, **capture)
