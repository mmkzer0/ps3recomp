#!/usr/bin/env python3
"""Platform defaults of ppu.host_stack_mb, spu.host_stack_mb and spu.dispatch_stack_mb (include/ps3emu/config.h).

The default is a compile-time choice (iOS differs from every other host), so the check is a compile: a
_Static_assert on PS3_CFG_PPU_HOST_STACK_MB_DEFAULT, PS3_CFG_SPU_HOST_STACK_MB_DEFAULT and
PS3_CFG_SPU_DISPATCH_STACK_MB_DEFAULT per target. The host target is always checked, with the compiler the build
uses (CMake passes --cc and --variant: CMAKE_C_COMPILER and CMAKE_C_COMPILER_FRONTEND_VARIANT, so an MSVC build,
cl or clang-cl, gets MSVC syntax-check flags and a GNU-style one gets -fsyntax-only); on macOS the iPhoneOS and
iPhoneSimulator SDKs are checked too, with the Xcode clang. A target whose SDK is missing is skipped with a line
saying so, never silently.

    host (macOS, Linux, Windows):  ppu 256, spu 16, spu dispatch 256
    iOS (device and simulator):    ppu  32, spu  8, spu dispatch  32
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
SRC = '''
#include "ps3emu/config.h"
_Static_assert(PS3_CFG_PPU_HOST_STACK_MB_DEFAULT == %d, "ppu.host_stack_mb default");
_Static_assert(PS3_CFG_SPU_HOST_STACK_MB_DEFAULT == %d, "spu.host_stack_mb default");
_Static_assert(PS3_CFG_SPU_DISPATCH_STACK_MB_DEFAULT == %d, "spu.dispatch_stack_mb default");
int main(void) { return 0; }
'''
HOST = (256, 16, 256)
IOS = (32, 8, 32)


def syntax_check_cmd(cc, variant, src):
    """The command that only parses `src` with the compiler `cc`: cl and clang-cl take /Zs, the rest -fsyntax-only."""
    inc = str(root / 'include')
    if variant == 'MSVC':
        return [*cc, '/nologo', '/Zs', '/std:c11', '/I', inc, str(src)]
    return [*cc, '-std=gnu17', '-fsyntax-only', '-I' + inc, str(src)]


def compile_check(label, want, cc, variant='GNU'):
    with tempfile.TemporaryDirectory(prefix='host-stack-defaults-') as tmp:
        src = Path(tmp) / 'check.c'
        src.write_text(SRC % want)
        p = subprocess.run(syntax_check_cmd(cc, variant, src), check=False, capture_output=True, text=True)
        if p.returncode != 0:
            print(f'FAIL {label}: defaults are not ppu {want[0]}, spu {want[1]}, dispatch {want[2]}\n{p.stdout}{p.stderr}')
            return False
        print(f'ok   {label}: ppu {want[0]} MB, spu {want[1]} MB, spu dispatch {want[2]} MB')
        return True


def sdk_path(sdk):
    p = subprocess.run(['xcrun', '--sdk', sdk, '--show-sdk-path'], check=False, capture_output=True, text=True)
    return p.stdout.strip() if p.returncode == 0 and p.stdout.strip() else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cc', default=os.environ.get('CC', 'cc'), help='the C compiler of the build (CMAKE_C_COMPILER)')
    ap.add_argument('--variant', default='', help='its frontend variant (CMAKE_C_COMPILER_FRONTEND_VARIANT): MSVC or GNU')
    args = ap.parse_args()
    ok = compile_check('host', HOST, [args.cc], args.variant)
    if sys.platform == 'darwin':
        for sdk, flags in (('iphoneos', ['-arch', 'arm64', '-miphoneos-version-min=16.0']),
                           ('iphonesimulator', ['-arch', 'arm64', '-mios-simulator-version-min=16.0'])):
            path = sdk_path(sdk)
            if not path:
                print(f'SKIP {sdk}: SDK not installed')
                continue
            ok &= compile_check(sdk, IOS, ['xcrun', '--sdk', sdk, 'clang', '-isysroot', path, *flags])
    else:
        print('SKIP ios: needs the Apple SDKs')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
