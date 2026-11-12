#!/usr/bin/env python3
"""The host C/C++ compiler a test shells out to, spelled for its driver.

CTest hands a test the compiler CMake configured (CC, CXX); a script run by hand
falls back to the usual names. GNU drivers (cc, gcc, clang) take -std/-c/-S/-o.
clang-cl takes the MSVC spelling, and a GNU flag it does not know is only a
warning there: `-S` built an executable and `-std=gnu17` was dropped, and the
test went on to check the wrong file. So the spelling follows the driver, not
the platform (a GNU clang on Windows is spelled as GNU).

    from host_cc import c_compiler
    cc = c_compiler()                       # None: no compiler found
    r = cc.run(cc.exe([src], out, std="gnu17", includes=[inc]))

-I and -D are spelled the same by both. Plain cl is recognised (kind "cl") so a
caller can refuse it; the toolkit itself builds with clang-cl.
"""
import os
import shutil
import subprocess
from pathlib import Path

import msvc_env

# What an executable built by exe() is called: exe() appends nothing.
EXE = ".exe" if os.name == "nt" else ""


class Compiler:
    def __init__(self, argv, env):
        self.argv = argv
        self.env = env
        stem = Path(argv[0]).stem.lower()
        self.kind = stem if stem in ("clang-cl", "cl") else "gnu"
        self.msvc = self.kind != "gnu"

    def _base(self, std, includes, defines, flags):
        if self.msvc:
            a = ["/nologo"]
            if std:
                a.append("/std:" + std.replace("gnu", "c"))   # c11, c17, c++17
        else:
            a = ["-std=" + std] if std else []
        a += ["-I" + str(i) for i in includes]
        a += ["-D" + d for d in defines]
        return self.argv + a + list(flags)

    def obj(self, src, out, std=None, includes=(), defines=(), flags=()):
        """Compile one source to an object file."""
        o = ["/c", str(src), "/Fo" + str(out)] if self.msvc else ["-c", str(src), "-o", str(out)]
        return self._base(std, includes, defines, flags) + o

    def asm(self, src, out, std=None, includes=(), defines=(), flags=()):
        """Compile one source to assembly text in `out`."""
        if self.msvc:
            o = ["/c", str(src), "/FA", "/Fa" + str(out), "/Fo" + str(out) + ".obj"]
        else:
            o = ["-S", str(src), "-o", str(out)]
        return self._base(std, includes, defines, flags) + o

    def exe(self, inputs, out, std=None, includes=(), defines=(), flags=()):
        """Compile and/or link `inputs` (sources or objects) into the executable `out`."""
        o = [str(i) for i in inputs] + (["/Fe" + str(out)] if self.msvc else ["-o", str(out)])
        return self._base(std, includes, defines, flags) + o

    def run(self, cmd, **kw):
        """Run a command built above with the compiler's environment; output captured as text.
        A linker's diagnostics can arrive on stdout (link.exe), so read both."""
        return subprocess.run(cmd, env=self.env, capture_output=True, text=True, **kw)


def _environ():
    # A developer prompt or CI has already set the MSVC environment; vcvars costs a second per call.
    if os.name != "nt" or os.environ.get("INCLUDE"):
        return dict(os.environ)
    return msvc_env.environ()


def _find(var, msvc_names, gnu_names):
    env = _environ()
    path = env.get("PATH")
    want = os.environ.get(var)
    if want:
        # CMake's compiler path can contain spaces ("C:/Program Files/..."); split only a
        # value that is not itself an executable ("clang -m64").
        argv = [want] if shutil.which(want, path=path) else want.split()
        cands = [argv]
    else:
        cands = [[n] for n in (msvc_names if os.name == "nt" else ()) + gnu_names]
    for argv in cands:
        exe = shutil.which(argv[0], path=path)
        if exe:
            return Compiler([exe] + argv[1:], env)
    return None


def c_compiler():
    return _find("CC", ("clang-cl",), ("cc", "clang", "gcc"))


def cxx_compiler():
    return _find("CXX", ("clang-cl",), ("c++", "clang++", "g++"))
