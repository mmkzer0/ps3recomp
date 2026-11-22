"""Run the real neutral draw-engine regressions, including immediate vertices."""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    if os.name == "nt":
        print("SKIP: neutral C fixture requires the POSIX toolchain; run on macOS/Linux")
        return
    root = Path(__file__).resolve().parents[1]
    sources = ["tests/test_rsx_draw_engine.c", "rsx_draw_engine.c",
               "rsx_dispatch.c", "rsx_vertex_compact.c", "rsx_texture_layout.c",
               "rsx_vp_decompiler.c", "rsx_fp_decompiler.c"]
    # The second build takes a larger texture cache from the command line, as a
    # title whose frames bind more textures does; the header must let it (the
    # test asserts the size it was told to expect).
    for defines in ([], ["-DRSX_DRAW_ENGINE_TEXTURE_CACHE=2048", "-DRSX_TEST_EXPECT_TEXTURE_CACHE=2048"]):
        with tempfile.TemporaryDirectory(prefix="rsx-immediate-") as directory:
            binary = Path(directory) / "test_rsx_draw_engine"
            # The engine's migrated lines call ps3_log_*; this fixture configures no log (its stub header answers
            # ps3_config_get), so the stop the log refers to exists only to satisfy the link.
            stub = Path(directory) / "log_stub.c"
            stub.write_text('#include <stdlib.h>\n#include "ps3emu/stop.h"\n'
                            'void ps3_stop_set_mirror(ps3_stop_quiesce_fn q, ps3_stop_mirror_fn m) { (void)q; (void)m; }\n'
                            'PS3_NORETURN void ps3_stop(ps3_stop_kind k, const char* w, const char* r, const ps3_stop_field* f,\n'
                            '                           size_t n, const ps3_stop_site* s)\n'
                            '        { (void)k; (void)w; (void)r; (void)f; (void)n; (void)s; _Exit(PS3_STOP_EXIT_CODE); }\n')
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", *defines,
                            "-Iinclude", "-Ilibs/video", "-o", str(binary),
                            *["libs/video/" + s for s in sources],
                            "runtime/diag/ps3_log.c", "runtime/diag/host_exit.c", str(stub)],
                           cwd=root, check=True)
            subprocess.run([str(binary)], cwd=root, check=True)


if __name__ == "__main__":
    main()
