#!/usr/bin/env python3
"""The Metal backend's record cap (ENG_MAX_RECORDS): a full record array is submitted mid-frame, with the
bound constants staged again in the next arena, instead of dropping the rest of the frame. Built twice: with
the default cap, and with a larger one defined at build time as a title whose frames record more does (the
fixture asserts the cap it was told to expect)."""
import pathlib
import platform
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
if platform.system() != "Darwin":
    print("SKIP: Metal requires macOS")
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix="metal-record-cap-") as tmp:
    # rsx_metal_backend.m's migrated lines call ps3_log_*; this fixture configures no log, so the
    # configuration, the stop hook and the mirror the log refers to exist only to satisfy the link.
    (pathlib.Path(tmp) / 'log_stub.c').write_text(
        '#include <stdlib.h>\n#include "ps3emu/config.h"\n#include "ps3emu/stop.h"\n'
        'const ps3_config* ps3_config_get(void) { static ps3_config c; return &c; }\n'
        'void ps3_stop_set_mirror(ps3_stop_quiesce_fn q, ps3_stop_mirror_fn m) { (void)q; (void)m; }\n'
        'PS3_NORETURN void ps3_stop(ps3_stop_kind k, const char* w, const char* r, const ps3_stop_field* f,\n'
        '                           size_t n, const ps3_stop_site* s)\n'
        '        { (void)k; (void)w; (void)r; (void)f; (void)n; (void)s; _Exit(PS3_STOP_EXIT_CODE); }\n')
    for name, defines in (("8k", []),
                          ("16k", ["-DENG_MAX_RECORDS=16384", "-DRSX_TEST_EXPECT_MAX_RECORDS=16384"])):
        binary = pathlib.Path(tmp) / ("test_" + name)
        subprocess.run(["clang", "-fobjc-arc", *defines, "-Iinclude", "-Ilibs/video",
                        "libs/video/tests/test_metal_record_cap.m",
                        "libs/video/rsx_texture_layout.c", "runtime/diag/ps3_log.c",
                        "runtime/diag/host_exit.c", str(pathlib.Path(tmp) / "log_stub.c"),
                        "-Wl,-dead_strip",
                        "-framework", "Metal", "-framework", "Cocoa",
                        "-framework", "QuartzCore", "-o", str(binary)],
                       cwd=ROOT, check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=120)
