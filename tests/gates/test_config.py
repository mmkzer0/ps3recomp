#!/usr/bin/env python3
"""Typed configuration: sources and precedence, the effective-config dump, and
every way a configuration can be wrong ending in a config-invalid stop.

usage: test_config.py <probe_config>
"""
import os
import re
import sys
import tempfile

import gates_common as g

PROBE = None
SHA_A = "a" * 64
SHA_B = "0123456789abcdef" * 4


def probe(*args, env=None, **extra_env):
    e = g.clean_env(**extra_env) if env is None else env
    return g.run([PROBE, *args], env=e)


def toml_str(s):
    """s as a TOML basic string. A Windows path's backslashes are escapes there, so a path is never
    pasted between quotes as it is."""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


class Cfg:
    """A temporary config file, written from lines so line numbers are exact."""

    def __init__(self, *lines):
        self._dir = tempfile.TemporaryDirectory(prefix="ps3cfg-")
        self.path = os.path.join(self._dir.name, "title.toml")
        with open(self.path, "w") as f:
            f.write("\n".join(lines) + "\n")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self._dir.cleanup()


def options(proc):
    """Parse `probe_config get`: {key: (value, source)}."""
    assert proc.returncode == 0, proc.stderr
    out = {}
    for line in proc.stdout.splitlines():
        m = re.match(r"^(\S+) (\S+) (.+)$", line)
        assert m, line
        out[m.group(1)] = (m.group(2), m.group(3))
    return out


def dump_sources(text):
    """{section.key: source} from the `# source:` comments of a dump."""
    out, section = {}, None
    for line in text.splitlines():
        m = re.match(r"^\[(\w+)\]$", line)
        if m:
            section = m.group(1)
            continue
        m = re.match(r"^(\w+) = .*  # source: (.+)$", line)
        if m and section:
            out[f"{section}.{m.group(1)}"] = m.group(2)
    return out


def strip_comments(text):
    return re.sub(r"[ \t]*#.*", "", text)


def config_invalid(proc):
    return g.expect_stop(proc, "config-invalid")


# ---- defaults, file, environment --------------------------------------------

def test_defaults_without_file_or_environment():
    o = options(probe("get"))
    assert o == {
        "ppu.vscr_nj": ("true", "default"),
        "ppu.opd_swap_fixup": ("false", "default"),
        "ppu.null_page": ('"stop"', "default"),
        "ppu.precise_usleep": ("true", "default"),
        "ppu.poll_sleep_coalesce": ("false", "default"),
        "ppu.poll_sleep_spin_us": ("100", "default"),
        "ppu.host_stack_mb": ("256", "default"),      # iOS: 32 (tools/test_host_stack_defaults.py)
        "spu.host_stack_mb": ("16", "default"),
        "spu.dispatch_stack_mb": ("256", "default"),      # iOS: 32 (tools/test_host_stack_defaults.py)
        "spu.dma_repeat_limit": ("256", "default"),
        "spu.dma_repeat_stop_secs": ("0", "default"),
        "spu.job_body": ('"interp"', "default"),
        "spu.xdiff_dir": ('""', "default"),
        "spu.xdiff_capture": ("0", "default"),
        "spu.xdiff": ('"off"', "default"),
        "spu.xdiff_period": ("64", "default"),
        "spu.xdiff_on_divergence": ('"report"', "default"),
        "spu.xdiff_on_latent": ('"report"', "default"),
        "spu.xdiff_budget": ("268435456", "default"),
        "gcm.wait_timeout_ms": ("10000", "default"),
        "gcm.park_resync": ("false", "default"),
        "gcm.park_grace_ms": ("50", "default"),
        "gcm.handler_thread": ("false", "default"),
        "gcm.report_spu_hold_ms": ("0", "default"),
        "eng.depth_clamp": ("true", "default"),
        "eng.depth_bounds": ("true", "default"),
        "rsx.bc_path": ('"auto"', "default"),
        "rsx.bc_gpu_min_texels": ("147456", "default"),
        "vk.draw_batch": ("false", "default"),
        "vk.draw_batch_max": ("512", "default"),
        "vk.draw_batch_min": ("0", "default"),
        "game.free_space_raw": ("false", "default"),
        "trophy.register_wait_cap_vblanks": ("1024", "default"),
        "trophy.pace_ms": ("2000", "default"),
        "adec.silence_fallback": ("false", "default"),
        "video.resolutions": ('""', "default"),
        "video.rsx_capture": ("false", "default"),
        "video.rsx_capture_dir": ('""', "default"),
        "video.rsx_capture_frames": ("0", "default"),
        "pad.record": ('""', "default"),
        "pad.replay": ('""', "default"),
        "pad.replay_repeat": ("1", "default"),
        "title.elf_sha256": ('""', "default"),
        "host.work_interval": ("false", "default"),
        "host.work_interval_us": ("500", "default"),
        "diag.dump_config": ("false", "default"),
        "log.file": ('""', "default"),
        "log.level": ('"info"', "default"),
        "log.mute": ('""', "default"),
        "log.collapse": ('"file"', "default"),
        "log.max_mb": ("256", "default"),
    }, o


def test_valid_file_loads_and_names_its_source():
    with Cfg("schema = 2", "[ppu]", "vscr_nj = false", "[title]", f'elf_sha256 = "{SHA_A}"') as c:
        o = options(probe("--load", c.path, "get"))
        assert o["ppu.vscr_nj"] == ("false", f"file:{c.path}")
        assert o["title.elf_sha256"] == (f'"{SHA_A}"', f"file:{c.path}")
        assert o["diag.dump_config"] == ("false", "default")


def test_dotted_keys_and_other_toml_spellings_are_accepted():
    with Cfg("# a comment", "schema = 2", "ppu.vscr_nj = false", "diag = { dump_config = false }") as c:
        assert options(probe("--load", c.path, "get"))["ppu.vscr_nj"][0] == "false"


def test_environment_beats_file_beats_default():
    with Cfg("schema = 2", "[ppu]", "vscr_nj = false", "[title]", f'elf_sha256 = "{SHA_A}"') as c:
        # the file beats the default; the environment beats the file
        o = options(probe("--load", c.path, "get", PS3RECOMP_PPU_VSCR_NJ="1"))
        assert o["ppu.vscr_nj"] == ("true", "env:PS3RECOMP_PPU_VSCR_NJ")
        assert o["title.elf_sha256"] == (f'"{SHA_A}"', f"file:{c.path}")
        assert o["diag.dump_config"] == ("false", "default")
        o = options(probe("--load", c.path, "get", PS3RECOMP_TITLE_ELF_SHA256=SHA_B.upper()))
        assert o["title.elf_sha256"] == (f'"{SHA_B}"', "env:PS3RECOMP_TITLE_ELF_SHA256"), o  # normalised
        assert o["ppu.vscr_nj"] == ("false", f"file:{c.path}")


def test_environment_alone_sets_options():
    for text, want in (("0", "false"), ("false", "false"), ("1", "true"), ("TRUE", "true")):
        o = options(probe("get", PS3RECOMP_PPU_VSCR_NJ=text))
        assert o["ppu.vscr_nj"] == (want, "env:PS3RECOMP_PPU_VSCR_NJ"), (text, o)


def test_park_resync_and_grace_come_from_file_and_environment():
    with Cfg("schema = 2", "[gcm]", "park_resync = true", "park_grace_ms = 200") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["gcm.park_resync"] == ("true", f"file:{c.path}"), o
        assert o["gcm.park_grace_ms"] == ("200", f"file:{c.path}"), o
        o = options(probe("--load", c.path, "get", PS3RECOMP_GCM_PARK_RESYNC="0", PS3RECOMP_GCM_PARK_GRACE_MS="75"))
        assert o["gcm.park_resync"] == ("false", "env:PS3RECOMP_GCM_PARK_RESYNC"), o
        assert o["gcm.park_grace_ms"] == ("75", "env:PS3RECOMP_GCM_PARK_GRACE_MS"), o
    o = options(probe("get", PS3RECOMP_GCM_PARK_RESYNC="1"))
    assert o["gcm.park_resync"] == ("true", "env:PS3RECOMP_GCM_PARK_RESYNC"), o
    assert o["gcm.park_grace_ms"] == ("50", "default"), o        # the grace is separate from the mode


def test_park_grace_out_of_range_stops():
    for bad in ("0", "60001"):
        f = config_invalid(probe("get", PS3RECOMP_GCM_PARK_GRACE_MS=bad))
        assert f["key"] == "gcm.park_grace_ms" and "out of range" in f["detail"], (bad, f)


def test_host_stack_sizes_come_from_file_and_environment():
    # Typed keys, no environment knob of their own: SPU_HOST_STACK_BIG is not read any more.
    with Cfg("schema = 2", "[ppu]", "host_stack_mb = 64", "[spu]", "host_stack_mb = 512",
             "dispatch_stack_mb = 128") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["ppu.host_stack_mb"] == ("64", f"file:{c.path}"), o
        assert o["spu.host_stack_mb"] == ("512", f"file:{c.path}"), o
        assert o["spu.dispatch_stack_mb"] == ("128", f"file:{c.path}"), o
        o = options(probe("--load", c.path, "get", PS3RECOMP_PPU_HOST_STACK_MB="2048", PS3RECOMP_SPU_HOST_STACK_MB="1",
                          PS3RECOMP_SPU_DISPATCH_STACK_MB="2048"))
        assert o["ppu.host_stack_mb"] == ("2048", "env:PS3RECOMP_PPU_HOST_STACK_MB"), o
        assert o["spu.host_stack_mb"] == ("1", "env:PS3RECOMP_SPU_HOST_STACK_MB"), o
        assert o["spu.dispatch_stack_mb"] == ("2048", "env:PS3RECOMP_SPU_DISPATCH_STACK_MB"), o
    o = options(probe("get", SPU_HOST_STACK_BIG="1"))
    assert o["spu.host_stack_mb"] == ("16", "default"), o


def test_host_stack_sizes_out_of_range_stop():
    # PPU 1..2048 (the Windows thread API takes the size in 32 bits), SPU 1..1024, dispatch 1..2048.
    for key, env, bads in (("ppu.host_stack_mb", "PS3RECOMP_PPU_HOST_STACK_MB", ("0", "2049", "4294967295")),
                           ("spu.host_stack_mb", "PS3RECOMP_SPU_HOST_STACK_MB", ("0", "1025", "4294967295")),
                           ("spu.dispatch_stack_mb", "PS3RECOMP_SPU_DISPATCH_STACK_MB", ("0", "2049", "4294967295"))):
        for bad in bads:
            f = config_invalid(probe("get", **{env: bad}))
            assert f["key"] == key and "out of range" in f["detail"], (key, bad, f)


def test_ps3recomp_config_names_the_file_and_beats_the_title_path():
    with Cfg("schema = 2", "[ppu]", "vscr_nj = false") as title, \
         Cfg("schema = 2", "[diag]", "dump_config = false") as operator:
        o = options(probe("get", PS3RECOMP_CONFIG=title.path))
        assert o["ppu.vscr_nj"] == ("false", f"file:{title.path}")
        o = options(probe("--load", title.path, "get", PS3RECOMP_CONFIG=operator.path))
        assert o["ppu.vscr_nj"] == ("true", "default")          # the title's file was not read
        assert o["diag.dump_config"] == ("false", f"file:{operator.path}")


def test_legacy_diagnostic_variables_are_not_typos():
    o = options(probe("get", PS3RECOMP_METAL_HEADLESS="1", PS3RECOMP_VK_DUMP="x", PS3RECOMP_DIR="/tmp"))
    assert o["ppu.vscr_nj"] == ("true", "default")


def test_host_display_and_diagnostic_variables_are_not_typos():
    # Names the toolkit reads itself but #147 found absent from the legacy list: the gate stops on
    # any PS3RECOMP_* name it does not know, so PS3RECOMP_FULLSCREEN=1 used to end the run.
    o = options(probe("get",
                      PS3RECOMP_FULLSCREEN="1", PS3RECOMP_METAL_WINDOW="1280x720",
                      PS3RECOMP_WINDOW_STATE="0,0,1280,720,0", PS3RECOMP_HOST_SURFACES="ps3-host",
                      PS3RECOMP_ENG_BORDER="0", PS3RECOMP_ENG_DEPTH_VIA_GUEST="1",
                      PS3RECOMP_ENG_POLY_OFFSET="0", PS3RECOMP_ENG_RECT_CLEAR="0",
                      PS3RECOMP_ENG_USER_CLIP="0", PS3RECOMP_MSL_CACHE="/tmp/msl",
                      PS3RECOMP_MSL_INVARIANT="0", PS3RECOMP_VK_MRT_MASK="0"))
    assert o["ppu.vscr_nj"] == ("true", "default")


# ---- the effective-config dump ----------------------------------------------

def test_dump_lists_every_key_value_and_source_as_valid_toml():
    with Cfg("schema = 2", "[ppu]", "vscr_nj = false") as c:
        p = probe("--load", c.path, "dump", PS3RECOMP_DIAG_DUMP_CONFIG="0", PS3RECOMP_TITLE_ELF_SHA256=SHA_A)
        assert p.returncode == 0, p.stderr
        src = dump_sources(p.stdout)
        assert src == {
            "ppu.vscr_nj": f"file:{c.path}",
            "ppu.opd_swap_fixup": "default",
            "ppu.null_page": "default",
            "ppu.precise_usleep": "default",
            "ppu.poll_sleep_coalesce": "default",
            "ppu.poll_sleep_spin_us": "default",
            "ppu.host_stack_mb": "default",
            "spu.host_stack_mb": "default",
            "spu.dispatch_stack_mb": "default",
            "spu.dma_repeat_limit": "default",
            "spu.dma_repeat_stop_secs": "default",
            "spu.job_body": "default",
            "spu.xdiff_dir": "default",
            "spu.xdiff_capture": "default",
            "spu.xdiff": "default",
            "spu.xdiff_period": "default",
            "spu.xdiff_on_divergence": "default",
            "spu.xdiff_on_latent": "default",
            "spu.xdiff_budget": "default",
            "gcm.wait_timeout_ms": "default",
            "gcm.park_resync": "default",
            "gcm.park_grace_ms": "default",
            "gcm.handler_thread": "default",
            "gcm.report_spu_hold_ms": "default",
            "eng.depth_clamp": "default",
            "eng.depth_bounds": "default",
            "rsx.bc_path": "default",
            "rsx.bc_gpu_min_texels": "default",
            "vk.draw_batch": "default",
            "vk.draw_batch_max": "default",
            "vk.draw_batch_min": "default",
            "game.free_space_raw": "default",
            "trophy.register_wait_cap_vblanks": "default",
            "trophy.pace_ms": "default",
            "adec.silence_fallback": "default",
            "video.resolutions": "default",
            "video.rsx_capture": "default",
            "video.rsx_capture_dir": "default",
            "video.rsx_capture_frames": "default",
            "pad.record": "default",
            "pad.replay": "default",
            "pad.replay_repeat": "default",
            "title.elf_sha256": "env:PS3RECOMP_TITLE_ELF_SHA256",
            "host.work_interval": "default",
            "host.work_interval_us": "default",
            "diag.dump_config": "env:PS3RECOMP_DIAG_DUMP_CONFIG",
            "log.file": "default",
            "log.level": "default",
            "log.mute": "default",
            "log.collapse": "default",
            "log.max_mb": "default",
        }, src
        try:
            import tomllib
        except ImportError:       # Python < 3.11: the reload below is the validity check
            tomllib = None
        if tomllib:
            doc = tomllib.loads(p.stdout)
            assert doc == {"schema": 2,
                           "ppu": {"vscr_nj": False, "opd_swap_fixup": False, "null_page": "stop",
                                   "precise_usleep": True, "poll_sleep_coalesce": False,
                                   "poll_sleep_spin_us": 100, "host_stack_mb": 256},
                           "spu": {"host_stack_mb": 16, "dispatch_stack_mb": 256, "dma_repeat_limit": 256,
                                   "dma_repeat_stop_secs": 0,
                                   "job_body": "interp", "xdiff_dir": "",
                                   "xdiff_capture": 0, "xdiff": "off", "xdiff_period": 64,
                                   "xdiff_on_divergence": "report", "xdiff_on_latent": "report",
                                   "xdiff_budget": 268435456},
                           "gcm": {"wait_timeout_ms": 10000, "park_resync": False, "park_grace_ms": 50,
                                   "handler_thread": False, "report_spu_hold_ms": 0},
                           "eng": {"depth_clamp": True, "depth_bounds": True},
                           "rsx": {"bc_path": "auto", "bc_gpu_min_texels": 147456},
                           "vk": {"draw_batch": False, "draw_batch_max": 512, "draw_batch_min": 0},
                           "game": {"free_space_raw": False},
                           "trophy": {"register_wait_cap_vblanks": 1024, "pace_ms": 2000},
                           "adec": {"silence_fallback": False},
                           "video": {"resolutions": "", "rsx_capture": False, "rsx_capture_dir": "",
                                     "rsx_capture_frames": 0},
                           "pad": {"record": "", "replay": "", "replay_repeat": 1},
                           "title": {"elf_sha256": SHA_A},
                           "host": {"work_interval": False, "work_interval_us": 500},
                           "diag": {"dump_config": False},
                           "log": {"file": "", "level": "info", "mute": "", "collapse": "file",
                                   "max_mb": 256}}, doc


def test_dump_reloads_to_the_same_configuration():
    with Cfg("schema = 2", "[ppu]", "vscr_nj = false",
             "[[hle.stub]]", "nid = 0x12345678", "ret = -1", 'reason = "quote \\" and \\\\ and tab \\t"',
             "[[lv2.stub]]", "num = 141", "ret = 0", 'reason = "sleeps are skippable"') as c:
        first = probe("--load", c.path, "dump")
        assert first.returncode == 0, first.stderr
        with Cfg(*first.stdout.splitlines()) as again:
            second = probe("--load", again.path, "dump")
        assert second.returncode == 0, second.stderr
        assert strip_comments(first.stdout) == strip_comments(second.stdout)
        assert "[[hle.stub]]" in first.stdout and "[[lv2.stub]]" in first.stdout


def test_dump_config_option_prints_the_dump_when_loaded():
    p = probe("get", PS3RECOMP_DIAG_DUMP_CONFIG="1")
    assert p.returncode == 0
    assert "schema = 2" in p.stderr and "dump_config = true  # source: env:PS3RECOMP_DIAG_DUMP_CONFIG" in p.stderr


# ---- invalid files -----------------------------------------------------------

def test_typo_key_names_the_key_and_the_line():
    with Cfg("schema = 2", "[ppu]", "# note", "vscr_nk = true") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "ppu.vscr_nk" and f["file"] == c.path and f["line"] == "4", f
        assert "vscr_nj" in f["detail"]                     # the suggestion


def test_unknown_section_and_top_level_key():
    with Cfg("schema = 2", "[ppuu]", "vscr_nj = true") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "ppuu" and f["line"] == "2" and "'ppu'" in f["detail"], f
    with Cfg("schema = 2", "oops = 1") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "oops" and f["line"] == "2", f


def test_wrong_type_stops_naming_key_expected_and_found():
    with Cfg("schema = 2", "[ppu]", 'vscr_nj = "yes"') as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "ppu.vscr_nj" and f["line"] == "3", f
        assert "expected boolean, found string" in f["detail"]
    with Cfg("schema = 2", "[title]", "elf_sha256 = 5") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "title.elf_sha256" and "expected string, found integer" in f["detail"], f
    with Cfg("schema = 2", "ppu = true") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "ppu" and "expected a [ppu] table" in f["detail"], f


def test_out_of_range_and_malformed_values_stop():
    for bad in ("abc", "g" * 64, "a" * 63, "a" * 65, "a" * 200):
        with Cfg("schema = 2", "[title]", f'elf_sha256 = "{bad}"') as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "title.elf_sha256" and f["line"] == "3", (bad, f)


def test_video_resolutions_normalise_and_reject_unknown_ids():
    for text, want in (("2", '"2"'), ("2,1", '"1,2"'), (" 5 , 2,2 ", '"2,5"'), ("13,10,4", '"4,10,13"'),
                       ("1,2,4,5,10,11,12,13", '"1,2,4,5,10,11,12,13"')):
        with Cfg("schema = 2", "[video]", f'resolutions = "{text}"') as c:
            o = options(probe("--load", c.path, "get"))
            assert o["video.resolutions"] == (want, f"file:{c.path}"), (text, o)
    o = options(probe("get", PS3RECOMP_VIDEO_RESOLUTIONS="2"))
    assert o["video.resolutions"] == ('"2"', "env:PS3RECOMP_VIDEO_RESOLUTIONS"), o
    for bad in ("0", "3", "6", "14", "99", "abc", "2,", ",2", "2,,4", "-1", "720", "1 2"):
        with Cfg("schema = 2", "[video]", f'resolutions = "{bad}"') as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "video.resolutions" and f["line"] == "3", (bad, f)
            assert "resolution id" in f["detail"], (bad, f)


def test_null_page_is_stop_or_drop_from_file_and_environment():
    with Cfg("schema = 2", "[ppu]", "# a title that reads page 0 on purpose", 'null_page = "drop"') as c:
        o = options(probe("--load", c.path, "get"))
        assert o["ppu.null_page"] == ('"drop"', f"file:{c.path}"), o
        o = options(probe("--load", c.path, "get", PS3RECOMP_PPU_NULL_PAGE="stop"))
        assert o["ppu.null_page"] == ('"stop"', "env:PS3RECOMP_PPU_NULL_PAGE"), o
    o = options(probe("get", PS3RECOMP_PPU_NULL_PAGE="drop"))
    assert o["ppu.null_page"] == ('"drop"', "env:PS3RECOMP_PPU_NULL_PAGE"), o
    for bad in ("land", "Stop", "DROP", "", "true"):
        with Cfg("schema = 2", "[ppu]", f'null_page = "{bad}"') as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "ppu.null_page" and f["line"] == "3", (bad, f)
            assert '"stop" or "drop"' in f["detail"], (bad, f)
        f = config_invalid(probe("get", PS3RECOMP_PPU_NULL_PAGE=bad))
        assert f["env"] == "PS3RECOMP_PPU_NULL_PAGE" and f["key"] == "ppu.null_page", (bad, f)
    with Cfg("schema = 2", "[ppu]", "null_page = true") as c:      # a bool is the wrong type
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "ppu.null_page" and f["line"] == "3", f


def test_job_body_is_interp_or_lifted_from_file_and_environment():
    with Cfg("schema = 2", "[spu]", "# run the registered lifted body of an image", 'job_body = "lifted"') as c:
        o = options(probe("--load", c.path, "get"))
        assert o["spu.job_body"] == ('"lifted"', f"file:{c.path}"), o
        o = options(probe("--load", c.path, "get", PS3RECOMP_SPU_JOB_BODY="interp"))
        assert o["spu.job_body"] == ('"interp"', "env:PS3RECOMP_SPU_JOB_BODY"), o
    o = options(probe("get", PS3RECOMP_SPU_JOB_BODY="lifted"))
    assert o["spu.job_body"] == ('"lifted"', "env:PS3RECOMP_SPU_JOB_BODY"), o
    for bad in ("lift", "Lifted", "INTERP", "", "true"):
        with Cfg("schema = 2", "[spu]", f'job_body = "{bad}"') as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "spu.job_body" and f["line"] == "3", (bad, f)
            assert '"interp" or "lifted"' in f["detail"], (bad, f)
        f = config_invalid(probe("get", PS3RECOMP_SPU_JOB_BODY=bad))
        assert f["env"] == "PS3RECOMP_SPU_JOB_BODY" and f["key"] == "spu.job_body", (bad, f)
    with Cfg("schema = 2", "[spu]", "job_body = true") as c:      # a bool is the wrong type
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "spu.job_body" and f["line"] == "3", f


def test_rsx_bc_path_is_one_of_four_from_file_and_environment():
    # The Metal backend's BC1/2/3 emulation: auto (native where the device samples BC) | cpu | gpu | tiered.
    for good in ("auto", "cpu", "gpu", "tiered"):
        with Cfg("schema = 2", "[rsx]", f'bc_path = "{good}"') as c:
            o = options(probe("--load", c.path, "get"))
            assert o["rsx.bc_path"] == (f'"{good}"', f"file:{c.path}"), (good, o)
        o = options(probe("get", PS3RECOMP_RSX_BC_PATH=good))
        assert o["rsx.bc_path"] == (f'"{good}"', "env:PS3RECOMP_RSX_BC_PATH"), (good, o)
    with Cfg("schema = 2", "[rsx]", 'bc_path = "gpu"') as c:     # the environment beats the file
        o = options(probe("--load", c.path, "get", PS3RECOMP_RSX_BC_PATH="cpu"))
        assert o["rsx.bc_path"] == ('"cpu"', "env:PS3RECOMP_RSX_BC_PATH"), o
    for bad in ("native", "GPU", "Tiered", "", "bc", "true"):
        with Cfg("schema = 2", "[rsx]", f'bc_path = "{bad}"') as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "rsx.bc_path" and f["line"] == "3", (bad, f)
            assert '"auto", "cpu", "gpu" or "tiered"' in f["detail"], (bad, f)
        f = config_invalid(probe("get", PS3RECOMP_RSX_BC_PATH=bad))
        assert f["env"] == "PS3RECOMP_RSX_BC_PATH" and f["key"] == "rsx.bc_path", (bad, f)
    with Cfg("schema = 2", "[rsx]", "bc_path = true") as c:      # a bool is the wrong type
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "rsx.bc_path" and f["line"] == "3", f
    f = config_invalid(probe("get", PS3RECOMP_RSX_BC_PATHH="gpu"))   # a near miss of the key is a typo, not a no-op
    assert "PS3RECOMP_RSX_BC_PATH" in f["detail"], f


def test_rsx_bc_gpu_min_texels_is_a_positive_u32_from_file_and_environment():
    with Cfg("schema = 2", "[rsx]", "bc_gpu_min_texels = 65536") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["rsx.bc_gpu_min_texels"] == ("65536", f"file:{c.path}"), o
    for text, want in (("1", "1"), ("0x4000", "16384"), ("4294967295", "4294967295")):
        o = options(probe("get", PS3RECOMP_RSX_BC_GPU_MIN_TEXELS=text))
        assert o["rsx.bc_gpu_min_texels"] == (want, "env:PS3RECOMP_RSX_BC_GPU_MIN_TEXELS"), (text, o)
    for bad in ("0", "-1", "4294967296", "", "64k", " 7"):      # below 1, negative, above 32 bits, not a number
        f = config_invalid(probe("get", PS3RECOMP_RSX_BC_GPU_MIN_TEXELS=bad))
        assert f["env"] == "PS3RECOMP_RSX_BC_GPU_MIN_TEXELS" and f["key"] == "rsx.bc_gpu_min_texels", (bad, f)
    for bad in ("0", "-5", "4294967296"):
        with Cfg("schema = 2", "[rsx]", f"bc_gpu_min_texels = {bad}") as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "rsx.bc_gpu_min_texels" and f["line"] == "3", (bad, f)
    with Cfg("schema = 2", "[rsx]", 'bc_gpu_min_texels = "16384"') as c:     # a string is the wrong type
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "rsx.bc_gpu_min_texels" and f["line"] == "3", f


def test_xdiff_enumerations_and_bounds_from_file_and_environment():
    # spu.xdiff and the two actions are enumerations (a validator each), the period and the budget are u32 >= 1.
    with Cfg("schema = 2", "[spu]", 'xdiff = "sample"', "xdiff_period = 8", 'xdiff_on_divergence = "stop"',
             'xdiff_on_latent = "stop"', "xdiff_budget = 1000") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["spu.xdiff"] == ('"sample"', f"file:{c.path}"), o
        assert o["spu.xdiff_period"] == ("8", f"file:{c.path}"), o
        assert o["spu.xdiff_on_divergence"] == ('"stop"', f"file:{c.path}"), o
        assert o["spu.xdiff_on_latent"] == ('"stop"', f"file:{c.path}"), o
        assert o["spu.xdiff_budget"] == ("1000", f"file:{c.path}"), o
        o = options(probe("--load", c.path, "get", PS3RECOMP_SPU_XDIFF="all"))
        assert o["spu.xdiff"] == ('"all"', "env:PS3RECOMP_SPU_XDIFF"), o
    o = options(probe("get", PS3RECOMP_SPU_XDIFF_ON_DIVERGENCE="stop", PS3RECOMP_SPU_XDIFF_PERIOD="1"))
    assert o["spu.xdiff_on_divergence"] == ('"stop"', "env:PS3RECOMP_SPU_XDIFF_ON_DIVERGENCE"), o
    assert o["spu.xdiff_period"] == ("1", "env:PS3RECOMP_SPU_XDIFF_PERIOD"), o
    for key, env, detail, values in (
            ("xdiff", "PS3RECOMP_SPU_XDIFF", '"off", "sample" or "all"', ("on", "Sample", "ALL", "", "true")),
            ("xdiff_on_divergence", "PS3RECOMP_SPU_XDIFF_ON_DIVERGENCE", '"report" or "stop"', ("halt", "Stop", "")),
            ("xdiff_on_latent", "PS3RECOMP_SPU_XDIFF_ON_LATENT", '"report" or "stop"', ("ignore", "REPORT", ""))):
        for bad in values:
            with Cfg("schema = 2", "[spu]", f'{key} = "{bad}"') as c:
                f = config_invalid(probe("--load", c.path, "get"))
                assert f["key"] == f"spu.{key}" and f["line"] == "3", (key, bad, f)
                assert detail in f["detail"], (key, bad, f)
            f = config_invalid(probe("get", **{env: bad}))
            assert f["env"] == env and f["key"] == f"spu.{key}", (key, bad, f)
        with Cfg("schema = 2", "[spu]", f"{key} = true") as c:      # a bool is the wrong type
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == f"spu.{key}" and f["line"] == "3", (key, f)
    for key in ("xdiff_period", "xdiff_budget"):
        for bad in ("0", "4294967296", "-1"):
            with Cfg("schema = 2", "[spu]", f"{key} = {bad}") as c:
                f = config_invalid(probe("--load", c.path, "get"))
                assert f["key"] == f"spu.{key}" and f["line"] == "3", (key, bad, f)
                assert "out of range" in f["detail"], (key, bad, f)


def test_pad_trace_paths_are_checked_at_load():
    # A mistyped path must end the run at startup, not minutes into it; and the recorder must never
    # overwrite a recording.
    with tempfile.TemporaryDirectory(prefix="ps3pad-") as d:
        fresh = os.path.join(d, "fresh.trace")
        existing = os.path.join(d, "existing.trace")
        with open(existing, "w") as f:
            f.write("keep\n")
        with Cfg("schema = 2", "[pad]", f"record = {toml_str(fresh)}", f"replay = {toml_str(existing)}") as c:
            o = options(probe("--load", c.path, "get"))
            assert o["pad.record"] == (f'"{fresh}"', f"file:{c.path}"), o
            assert o["pad.replay"] == (f'"{existing}"', f"file:{c.path}"), o
        assert not os.path.exists(fresh), "validating a record path must not create it"
        bad = [("record", existing, "already exists"),
               ("record", os.path.join(d, "no", "dir", "x.trace"), "does not exist"),
               ("replay", os.path.join(d, "missing.trace"), "not a regular file"),
               ("replay", d, "not a regular file")]
        if os.name == "posix" and os.geteuid() != 0:
            ro = os.path.join(d, "ro")
            os.mkdir(ro)
            os.chmod(ro, 0o555)
            secret = os.path.join(d, "secret.trace")
            with open(secret, "w") as f:
                f.write("x\n")
            os.chmod(secret, 0)
            bad += [("record", os.path.join(ro, "x.trace"), "not writable"),
                    ("replay", secret, "cannot be read")]
        try:
            for key, path, needle in bad:
                with Cfg("schema = 2", "[pad]", f"{key} = {toml_str(path)}") as c:
                    f = config_invalid(probe("--load", c.path, "get"))
                    assert f["key"] == f"pad.{key}" and f["line"] == "3", (key, path, f)
                    assert needle in f["detail"], (key, path, f)
            f = config_invalid(probe("get", PS3RECOMP_PAD_RECORD=existing))
            assert f["env"] == "PS3RECOMP_PAD_RECORD" and f["key"] == "pad.record", f
        finally:
            for p in (os.path.join(d, "ro"), os.path.join(d, "secret.trace")):
                if os.path.exists(p):
                    os.chmod(p, 0o755)


def test_pad_replay_repeat_is_bounded_and_comes_from_file_and_environment():
    # k = 2 lets a recording made at one guest frame per 60 Hz tick feed a build that runs two guest
    # frames per tick; bounding it keeps a typo from becoming an unbounded frame multiplier.
    with Cfg("schema = 2", "[pad]", "replay_repeat = 2") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["pad.replay_repeat"] == ("2", f"file:{c.path}"), o
    o = options(probe("get", PS3RECOMP_PAD_REPLAY_REPEAT="8"))
    assert o["pad.replay_repeat"] == ("8", "env:PS3RECOMP_PAD_REPLAY_REPEAT"), o
    assert options(probe("get"))["pad.replay_repeat"] == ("1", "default")
    for bad in ("0", "9", "4294967295"):
        f = config_invalid(probe("get", PS3RECOMP_PAD_REPLAY_REPEAT=bad))
        assert f["env"] == "PS3RECOMP_PAD_REPLAY_REPEAT" and f["key"] == "pad.replay_repeat", (bad, f)
        assert "out of range" in f["detail"], (bad, f)
    for bad in ("0", "9"):
        with Cfg("schema = 2", "[pad]", f"replay_repeat = {bad}") as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "pad.replay_repeat" and f["line"] == "3", (bad, f)
            assert "out of range" in f["detail"], (bad, f)


def test_schema_mismatch_missing_and_mistyped_stop():
    for lines, line, needle in ((["schema = 1"], "1", "unsupported schema 1"),
                                (["schema = 3"], "1", "unsupported schema 3"),
                                (["schema = 0"], "1", "unsupported schema 0"),
                                (['schema = "2"'], "1", "expected integer"),
                                (["# no schema", "[ppu]", "vscr_nj = true"], None, "missing")):
        with Cfg(*lines) as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "schema" and needle in f["detail"], (lines, f)
            assert f.get("line") == line, (lines, f)


def test_schema_is_checked_before_anything_else_in_the_file():
    with Cfg("schema = 1", "[spu]", "mfc_lookup_cache = false") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "schema" and f.get("line") == "1", f
        assert f["detail"] == "unsupported schema 1; this runtime reads schema 2", f

def test_toml_syntax_error_names_the_line():
    with Cfg("schema = 2", "[ppu", "vscr_nj = true") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["file"] == c.path and f["line"] == "2" and "TOML" in f["detail"], f


def test_unreadable_files_stop():
    f = config_invalid(probe("--load", "/nonexistent/ps3.toml", "get"))
    assert f["file"] == "/nonexistent/ps3.toml", f
    f = config_invalid(probe("get", PS3RECOMP_CONFIG="/nonexistent/other.toml"))
    assert f["file"] == "/nonexistent/other.toml", f
    f = config_invalid(probe("get", PS3RECOMP_CONFIG=""))
    assert f["env"] == "PS3RECOMP_CONFIG", f


# ---- invalid environment -----------------------------------------------------

def test_unknown_ps3recomp_variable_stops():
    f = config_invalid(probe("get", PS3RECOMP_FOO="1"))
    assert f["env"] == "PS3RECOMP_FOO" and "not a ps3recomp option" in f["detail"], f


def test_near_miss_variable_suggests_the_real_one():
    f = config_invalid(probe("get", PS3RECOMP_PPU_VSCR_NK="1"))
    assert f["env"] == "PS3RECOMP_PPU_VSCR_NK" and "PS3RECOMP_PPU_VSCR_NJ" in f["detail"], f


def test_bad_environment_value_stops():
    for bad in ("", "yes", "2", "on"):
        f = config_invalid(probe("get", PS3RECOMP_PPU_VSCR_NJ=bad))
        assert f["env"] == "PS3RECOMP_PPU_VSCR_NJ" and f["key"] == "ppu.vscr_nj", (bad, f)
    f = config_invalid(probe("get", PS3RECOMP_TITLE_ELF_SHA256="beef"))
    assert f["key"] == "title.elf_sha256", f


def test_first_bad_variable_reported_is_deterministic():
    f = config_invalid(probe("get", PS3RECOMP_ZZZ="1", PS3RECOMP_AAA="1"))
    assert f["env"] == "PS3RECOMP_AAA", f


# ---- u32 options -------------------------------------------------------------

def test_u32_options_accept_in_range_file_and_environment():
    with Cfg("schema = 2", "[spu]", "dma_repeat_limit = 1000", "dma_repeat_stop_secs = 86400") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["spu.dma_repeat_limit"] == ("1000", f"file:{c.path}"), o
        assert o["spu.dma_repeat_stop_secs"] == ("86400", f"file:{c.path}"), o
    for text, want in (("300", "300"), ("0x100", "256"), ("0X10", "16")):
        o = options(probe("get", PS3RECOMP_SPU_DMA_REPEAT_LIMIT=text))
        assert o["spu.dma_repeat_limit"] == (want, "env:PS3RECOMP_SPU_DMA_REPEAT_LIMIT"), (text, o)
    o = options(probe("get", PS3RECOMP_SPU_DMA_REPEAT_STOP_SECS="0"))
    assert o["spu.dma_repeat_stop_secs"] == ("0", "env:PS3RECOMP_SPU_DMA_REPEAT_STOP_SECS"), o




def test_adec_silence_fallback_comes_from_file_and_environment():
    # Off by default (#130): the silent block is an allow-list a title's config asks for.
    with Cfg("schema = 2", "[adec]", "silence_fallback = true") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["adec.silence_fallback"] == ("true", f"file:{c.path}"), o
        o = options(probe("--load", c.path, "get", PS3RECOMP_ADEC_SILENCE_FALLBACK="0"))
        assert o["adec.silence_fallback"] == ("false", "env:PS3RECOMP_ADEC_SILENCE_FALLBACK"), o
    o = options(probe("get", PS3RECOMP_ADEC_SILENCE_FALLBACK="1"))
    assert o["adec.silence_fallback"] == ("true", "env:PS3RECOMP_ADEC_SILENCE_FALLBACK"), o
    f = config_invalid(probe("get", PS3RECOMP_ADEC_SILENCE_FALLBACK="yes"))
    assert f["key"] == "adec.silence_fallback", f


def test_vk_draw_batch_comes_from_file_and_environment():
    # Off by default until the pixel diff of #129 is clean; the registry's names are PS3RECOMP_VK_*
    # (VK_DRAW_BATCH=1 was the unregistered switch this replaces).
    with Cfg("schema = 2", "[vk]", "draw_batch = true", "draw_batch_max = 64") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["vk.draw_batch"] == ("true", f"file:{c.path}"), o
        assert o["vk.draw_batch_max"] == ("64", f"file:{c.path}"), o
        o = options(probe("--load", c.path, "get", PS3RECOMP_VK_DRAW_BATCH="0", PS3RECOMP_VK_DRAW_BATCH_MAX="1024"))
        assert o["vk.draw_batch"] == ("false", "env:PS3RECOMP_VK_DRAW_BATCH"), o
        assert o["vk.draw_batch_max"] == ("1024", "env:PS3RECOMP_VK_DRAW_BATCH_MAX"), o
    o = options(probe("get", PS3RECOMP_VK_DRAW_BATCH="1"))
    assert o["vk.draw_batch"] == ("true", "env:PS3RECOMP_VK_DRAW_BATCH"), o
    assert o["vk.draw_batch_max"] == ("512", "default"), o
    o = options(probe("get", PS3RECOMP_VK_DRAW_BATCH_MAX="0x20"))
    assert o["vk.draw_batch_max"] == ("32", "env:PS3RECOMP_VK_DRAW_BATCH_MAX"), o
    f = config_invalid(probe("get", PS3RECOMP_VK_DRAW_BATCH="yes"))
    assert f["env"] == "PS3RECOMP_VK_DRAW_BATCH" and f["key"] == "vk.draw_batch", f
    o = options(probe("get", PS3RECOMP_VK_DRAW_BATCH_MIN="0x10"))
    assert o["vk.draw_batch_min"] == ("16", "env:PS3RECOMP_VK_DRAW_BATCH_MIN"), o
    with Cfg("schema = 2", "[vk]", "draw_batch_min = 32") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["vk.draw_batch_min"] == ("32", f"file:{c.path}"), o


def test_vk_draw_batch_min_is_zero_or_within_the_pool():
    # 0 is the fixed cap alone; the backend lowers a minimum above the maximum, so only the pool bounds it here.
    for good in ("0", "1", "1024"):
        o = options(probe("get", PS3RECOMP_VK_DRAW_BATCH_MIN=good))
        assert o["vk.draw_batch_min"] == (good, "env:PS3RECOMP_VK_DRAW_BATCH_MIN"), (good, o)
    for bad in ("1025", "4294967295", "-1"):
        f = config_invalid(probe("get", PS3RECOMP_VK_DRAW_BATCH_MIN=bad))
        assert f["env"] == "PS3RECOMP_VK_DRAW_BATCH_MIN" and f["key"] == "vk.draw_batch_min", (bad, f)


def test_vk_draw_batch_max_is_bounded_by_the_descriptor_pool():
    # 1024 is the backend's descriptor-set count; VK_DRAW_BATCH_MAX used to cap a larger value silently.
    for bad in ("0", "1025", "4294967295"):
        f = config_invalid(probe("get", PS3RECOMP_VK_DRAW_BATCH_MAX=bad))
        assert f["env"] == "PS3RECOMP_VK_DRAW_BATCH_MAX" and f["key"] == "vk.draw_batch_max", (bad, f)
        assert "out of range" in f["detail"], (bad, f)
    for bad in ("0", "1025"):
        with Cfg("schema = 2", "[vk]", f"draw_batch_max = {bad}") as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "vk.draw_batch_max" and f["line"] == "3", (bad, f)
            assert "out of range" in f["detail"], (bad, f)


def test_u32_out_of_range_file_values_stop():
    for key, bad in (("dma_repeat_limit", "0"), ("dma_repeat_limit", "4294967296"),
                     ("dma_repeat_limit", "-1"), ("dma_repeat_stop_secs", "86401"),
                     ("dma_repeat_stop_secs", "-1")):
        with Cfg("schema = 2", "[spu]", f"{key} = {bad}") as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == f"spu.{key}" and f["line"] == "3", (key, bad, f)
            assert "out of range" in f["detail"], (key, bad, f)


def test_u32_wrong_type_file_values_stop():
    for bad, found in (('"256"', "string"), ("2.5", "float"), ("true", "boolean")):
        with Cfg("schema = 2", "[spu]", f"dma_repeat_limit = {bad}") as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "spu.dma_repeat_limit" and f["line"] == "3", (bad, f)
            assert f"expected integer, found {found}" in f["detail"], (bad, f)


def test_u32_bad_environment_values_stop():
    for bad in ("", "-1", "1.5", "abc", "0x", "4294967296"):
        f = config_invalid(probe("get", PS3RECOMP_SPU_DMA_REPEAT_LIMIT=bad))
        assert f["env"] == "PS3RECOMP_SPU_DMA_REPEAT_LIMIT", (bad, f)
        assert f["key"] == "spu.dma_repeat_limit", (bad, f)


def test_u32_value_and_source_appear_in_the_dump():
    p = probe("dump", PS3RECOMP_SPU_DMA_REPEAT_LIMIT="0x200")
    assert p.returncode == 0, p.stderr
    assert ("dma_repeat_limit = 512  # source: env:PS3RECOMP_SPU_DMA_REPEAT_LIMIT"
            in p.stdout), p.stdout
    with Cfg("schema = 2", "[spu]", "dma_repeat_stop_secs = 30") as c:
        p = probe("--load", c.path, "dump")
        assert p.returncode == 0, p.stderr
        assert f"dma_repeat_stop_secs = 30  # source: file:{c.path}" in p.stdout, p.stdout


# ---- structured sections: [[hle.stub]] and [[lv2.stub]] ---------------------

STUBS = ["schema = 2",
         "[[hle.stub]]", "nid = 0xDEADBEEF", "ret = 0x8002cb07", 'reason = "title tolerates the error"',
         "[[hle.stub]]", "nid = 16", "ret = -1", 'reason = "second"',
         "[[lv2.stub]]", "num = 1023", "ret = 0", 'reason = "never queried"']


def test_stubs_parse_and_are_found_by_key():
    with Cfg(*STUBS) as c:
        p = probe("--load", c.path, "hle", "0xDEADBEEF")
        assert p.stdout.strip() == "nid=0xDEADBEEF ret=0x8002CB07 line=2 reason=title tolerates the error", p.stdout
        p = probe("--load", c.path, "hle", "16")
        assert p.stdout.strip() == "nid=0x00000010 ret=0xFFFFFFFF line=6 reason=second", p.stdout   # -1 -> two's complement
        assert probe("--load", c.path, "hle", "17").stdout.strip() == "none"
        assert probe("--load", c.path, "lv2", "1023").stdout.strip() == "num=1023 ret=0x00000000 line=10 reason=never queried"
        assert probe("--load", c.path, "lv2", "1").stdout.strip() == "none"


def test_no_stubs_means_none_found():
    assert probe("hle", "1").stdout.strip() == "none"
    assert probe("lv2", "1").stdout.strip() == "none"


def stub_case(lines, key, line, needle):
    with Cfg(*lines) as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == key and f["file"] == c.path, (lines, f)
        assert needle in f["detail"], (lines, f)
        if line is not None:
            assert f["line"] == line, (lines, f)


def test_stub_missing_a_field_stops_naming_the_entry_and_field():
    stub_case(["schema = 2", "[[hle.stub]]", "nid = 1", "ret = 0"], "hle.stub[0].reason", "2", "missing required field")
    stub_case(["schema = 2", "[[hle.stub]]", "ret = 0", 'reason = "r"'], "hle.stub[0].nid", "2", "missing required field")
    stub_case(["schema = 2", "[[hle.stub]]", "nid = 1", 'reason = "r"'], "hle.stub[0].ret", "2", "missing required field")
    stub_case(["schema = 2", "[[lv2.stub]]", "num = 1", "ret = 0"], "lv2.stub[0].reason", "2", "missing required field")
    stub_case(["schema = 2", "[[lv2.stub]]", "ret = 0", 'reason = "r"'], "lv2.stub[0].num", "2", "missing required field")
    # the second entry is the broken one
    stub_case(STUBS[:5] + ["[[hle.stub]]", "nid = 2", "ret = 0"], "hle.stub[1].reason", "6", "missing required field")


def test_stub_reason_must_say_something():
    stub_case(["schema = 2", "[[hle.stub]]", "nid = 1", "ret = 0", 'reason = ""'], "hle.stub[0].reason", "5", "blank")
    stub_case(["schema = 2", "[[lv2.stub]]", "num = 1", "ret = 0", 'reason = "  \\t"'], "lv2.stub[0].reason", "5", "blank")
    stub_case(["schema = 2", "[[hle.stub]]", "nid = 1", "ret = 0", "reason = 5"], "hle.stub[0].reason", "5", "expected string")


def test_stub_numbers_are_range_and_type_checked():
    base = ["schema = 2", "[[hle.stub]]", 'reason = "r"']
    stub_case(base + ["nid = 0x100000000", "ret = 0"], "hle.stub[0].nid", "4", "out of range")
    stub_case(base + ["nid = -1", "ret = 0"], "hle.stub[0].nid", "4", "out of range")
    stub_case(base + ["nid = 1", "ret = 0x100000000"], "hle.stub[0].ret", "5", "out of range")
    stub_case(base + ["nid = 1", "ret = -2147483649"], "hle.stub[0].ret", "5", "out of range")
    stub_case(base + ['nid = "1"', "ret = 0"], "hle.stub[0].nid", "4", "expected integer, found string")
    stub_case(base + ["nid = 1", "ret = 1.5"], "hle.stub[0].ret", "5", "expected integer, found float")
    lv2 = ["schema = 2", "[[lv2.stub]]", 'reason = "r"']
    stub_case(lv2 + ["num = 1024", "ret = 0"], "lv2.stub[0].num", "4", "out of range")
    stub_case(lv2 + ["num = -1", "ret = 0"], "lv2.stub[0].num", "4", "out of range")


def test_stub_duplicates_and_unknown_fields_stop():
    stub_case(STUBS[:5] + ["[[hle.stub]]", "nid = 0xDEADBEEF", "ret = 0", 'reason = "again"'],
              "hle.stub[1].nid", "6", "duplicate")
    stub_case(["schema = 2", "[[hle.stub]]", "nid = 1", "ret = 0", 'reason = "r"', 'why = "typo"'],
              "hle.stub[0].why", "6", "unknown field")
    stub_case(["schema = 2", "[[hle.stub]]", "num = 1", "nid = 1", "ret = 0", 'reason = "r"'],
              "hle.stub[0].num", "3", "unknown field")     # lv2's key in hle
    stub_case(["schema = 2", "[[hle.stubs]]", "nid = 1"], "hle.stubs", "2", "unknown key")


def test_stub_section_must_be_arrays_of_tables():
    stub_case(["schema = 2", "[hle.stub]", "nid = 1", "ret = 0", 'reason = "r"'], "hle.stub", "2", "array of tables")
    stub_case(["schema = 2", "hle = 3"], "hle", "2", "expected a table")


# ---- lifetime ----------------------------------------------------------------

def test_loading_after_use_is_a_stop_not_a_reload():
    f = config_invalid(probe("load-again"))
    assert "after the configuration was already" in f["detail"], f
    with Cfg("schema = 2") as c:
        f = config_invalid(probe("--load", c.path, "load-again"))
        assert "after the configuration was already" in f["detail"], f


def test_concurrent_first_readers_share_one_configuration():
    for _ in range(20):
        p = probe("race")
        assert p.returncode == 0 and p.stdout.strip() == "distinct=1", p.stdout + p.stderr


# ---- ppu.opd_swap_fixup ----------------------------------------------------------

def test_opd_swap_fixup_defaults_off_and_is_read_from_file_and_environment():
    with Cfg("schema = 2", "[ppu]", "opd_swap_fixup = true") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["ppu.opd_swap_fixup"] == ("true", f"file:{c.path}"), o
    o = options(probe("get", PS3RECOMP_PPU_OPD_SWAP_FIXUP="1"))
    assert o["ppu.opd_swap_fixup"] == ("true", "env:PS3RECOMP_PPU_OPD_SWAP_FIXUP"), o


# ---- ppu.precise_usleep ----------------------------------------------------------

def test_precise_usleep_defaults_on_and_is_read_from_file_and_environment():
    # On by default: the reference nanosleep path is what a file or the environment asks for.
    with Cfg("schema = 2", "[ppu]", "precise_usleep = false") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["ppu.precise_usleep"] == ("false", f"file:{c.path}"), o
        o = options(probe("--load", c.path, "get", PS3RECOMP_PPU_PRECISE_USLEEP="1"))
        assert o["ppu.precise_usleep"] == ("true", "env:PS3RECOMP_PPU_PRECISE_USLEEP"), o
    o = options(probe("get", PS3RECOMP_PPU_PRECISE_USLEEP="0"))
    assert o["ppu.precise_usleep"] == ("false", "env:PS3RECOMP_PPU_PRECISE_USLEEP"), o
    f = config_invalid(probe("get", PS3RECOMP_PPU_PRECISE_USLEEP="yes"))
    assert f["key"] == "ppu.precise_usleep", f


# ---- ppu.poll_sleep_coalesce / ppu.poll_sleep_spin_us ------------------------------

def test_poll_sleep_options_default_off_and_are_range_checked():
    # Off by default: a site a title registers is ignored until the file or the environment asks for it.
    with Cfg("schema = 2", "[ppu]", "poll_sleep_coalesce = true", "poll_sleep_spin_us = 0") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["ppu.poll_sleep_coalesce"] == ("true", f"file:{c.path}"), o
        assert o["ppu.poll_sleep_spin_us"] == ("0", f"file:{c.path}"), o
    o = options(probe("get", PS3RECOMP_PPU_POLL_SLEEP_COALESCE="1", PS3RECOMP_PPU_POLL_SLEEP_SPIN_US="2000"))
    assert o["ppu.poll_sleep_coalesce"] == ("true", "env:PS3RECOMP_PPU_POLL_SLEEP_COALESCE"), o
    assert o["ppu.poll_sleep_spin_us"] == ("2000", "env:PS3RECOMP_PPU_POLL_SLEEP_SPIN_US"), o
    f = config_invalid(probe("get", PS3RECOMP_PPU_POLL_SLEEP_SPIN_US="2001"))
    assert f["key"] == "ppu.poll_sleep_spin_us", f


# ---- host.work_interval / host.work_interval_us ---------------------------------------

def test_work_interval_options_default_off_and_deadline_is_range_checked():
    o = options(probe("get", PS3RECOMP_HOST_WORK_INTERVAL="1", PS3RECOMP_HOST_WORK_INTERVAL_US="16000"))
    assert o["host.work_interval"] == ("true", "env:PS3RECOMP_HOST_WORK_INTERVAL"), o
    assert o["host.work_interval_us"] == ("16000", "env:PS3RECOMP_HOST_WORK_INTERVAL_US"), o
    o = options(probe("get", PS3RECOMP_HOST_WORK_INTERVAL_US="50"))
    assert o["host.work_interval_us"] == ("50", "env:PS3RECOMP_HOST_WORK_INTERVAL_US"), o
    for bad in ("49", "16001", "0"):
        f = config_invalid(probe("get", PS3RECOMP_HOST_WORK_INTERVAL_US=bad))
        assert f["key"] == "host.work_interval_us" and "out of range" in f["detail"], (bad, f)


# ---- ppu.vscr_nj ---------------------------------------------------------------

def vscr(*args, **env):
    p = probe(*args, "vscr", **env)
    assert p.returncode == 0, p.stderr
    return dict(kv.split("=") for kv in p.stdout.split())


def test_new_context_starts_with_nj_set_by_default():
    v = vscr()
    assert v == {"init": "0x00010000", "clear": "0x00000000", "policy": "0x00010000"}, v


def test_new_context_vscr_follows_the_option_whichever_source_sets_it():
    assert vscr(PS3RECOMP_PPU_VSCR_NJ="0")["init"] == "0x00000000"
    assert vscr(PS3RECOMP_PPU_VSCR_NJ="1")["init"] == "0x00010000"
    with Cfg("schema = 2", "[ppu]", "vscr_nj = false") as c:
        assert vscr("--load", c.path)["init"] == "0x00000000"
        assert vscr("--load", c.path, PS3RECOMP_PPU_VSCR_NJ="true")["init"] == "0x00010000"   # env beats file
    with Cfg("schema = 2", "[ppu]", "vscr_nj = true") as c:
        assert vscr("--load", c.path)["init"] == "0x00010000"
        assert vscr("--load", c.path, PS3RECOMP_PPU_VSCR_NJ="0")["init"] == "0x00000000"


def test_vscr_nj_value_and_source_appear_in_the_dump():
    for env, value, source in ((None, "true", "default"),
                               ("0", "false", "env:PS3RECOMP_PPU_VSCR_NJ")):
        p = probe("dump", **({"PS3RECOMP_PPU_VSCR_NJ": env} if env else {}))
        assert p.returncode == 0, p.stderr
        assert f"vscr_nj = {value}  # source: {source}" in p.stdout, p.stdout


def test_log_file_is_configured_by_a_config_only_consumer():
    # probe_config only reads the configuration: it names no logger symbol. Loading the config must still
    # open log.file, so an unusable path stops it (host-log-file) instead of being ignored, and a usable
    # one is created. A weak no-op in config.cpp used to satisfy the call and leave this silently unconfigured.
    with tempfile.TemporaryDirectory(prefix="ps3cfg-log-") as d:
        f = g.expect_stop(probe("get", PS3RECOMP_LOG_FILE=os.path.join(d, "no", "such", "run.log")),
                          "host-log-file")
        assert f["op"] == "open", f
        path = os.path.join(d, "run.log")
        p = probe("get", PS3RECOMP_LOG_FILE=path)
        assert p.returncode == 0, p.stderr
        assert os.path.isfile(path)


if __name__ == "__main__":
    PROBE = sys.argv[1]
    sys.exit(g.main([(n, f) for n, f in sorted(globals().items()) if n.startswith("test_")]))
