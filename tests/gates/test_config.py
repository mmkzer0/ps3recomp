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
        "spu.dma_repeat_limit": ("256", "default"),
        "spu.dma_repeat_stop_secs": ("0", "default"),
        "gcm.wait_timeout_ms": ("10000", "default"),
        "trophy.register_wait_cap_vblanks": ("1024", "default"),
        "video.resolutions": ('""', "default"),
        "video.rsx_capture": ("false", "default"),
        "video.rsx_capture_dir": ('""', "default"),
        "video.rsx_capture_frames": ("0", "default"),
        "title.elf_sha256": ('""', "default"),
        "diag.dump_config": ("false", "default"),
    }, o


def test_valid_file_loads_and_names_its_source():
    with Cfg("schema = 1", "[ppu]", "vscr_nj = false", "[title]", f'elf_sha256 = "{SHA_A}"') as c:
        o = options(probe("--load", c.path, "get"))
        assert o["ppu.vscr_nj"] == ("false", f"file:{c.path}")
        assert o["title.elf_sha256"] == (f'"{SHA_A}"', f"file:{c.path}")
        assert o["diag.dump_config"] == ("false", "default")


def test_dotted_keys_and_other_toml_spellings_are_accepted():
    with Cfg("# a comment", "schema = 1", "ppu.vscr_nj = false", "diag = { dump_config = false }") as c:
        assert options(probe("--load", c.path, "get"))["ppu.vscr_nj"][0] == "false"


def test_environment_beats_file_beats_default():
    with Cfg("schema = 1", "[ppu]", "vscr_nj = false", "[title]", f'elf_sha256 = "{SHA_A}"') as c:
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


def test_ps3recomp_config_names_the_file_and_beats_the_title_path():
    with Cfg("schema = 1", "[ppu]", "vscr_nj = false") as title, \
         Cfg("schema = 1", "[diag]", "dump_config = false") as operator:
        o = options(probe("get", PS3RECOMP_CONFIG=title.path))
        assert o["ppu.vscr_nj"] == ("false", f"file:{title.path}")
        o = options(probe("--load", title.path, "get", PS3RECOMP_CONFIG=operator.path))
        assert o["ppu.vscr_nj"] == ("true", "default")          # the title's file was not read
        assert o["diag.dump_config"] == ("false", f"file:{operator.path}")


def test_legacy_diagnostic_variables_are_not_typos():
    o = options(probe("get", PS3RECOMP_METAL_HEADLESS="1", PS3RECOMP_VK_DUMP="x", PS3RECOMP_DIR="/tmp"))
    assert o["ppu.vscr_nj"] == ("true", "default")


# ---- the effective-config dump ----------------------------------------------

def test_dump_lists_every_key_value_and_source_as_valid_toml():
    with Cfg("schema = 1", "[ppu]", "vscr_nj = false") as c:
        p = probe("--load", c.path, "dump", PS3RECOMP_DIAG_DUMP_CONFIG="0", PS3RECOMP_TITLE_ELF_SHA256=SHA_A)
        assert p.returncode == 0, p.stderr
        src = dump_sources(p.stdout)
        assert src == {
            "ppu.vscr_nj": f"file:{c.path}",
            "ppu.opd_swap_fixup": "default",
            "ppu.null_page": "default",
            "spu.dma_repeat_limit": "default",
            "spu.dma_repeat_stop_secs": "default",
            "gcm.wait_timeout_ms": "default",
            "trophy.register_wait_cap_vblanks": "default",
            "video.resolutions": "default",
            "video.rsx_capture": "default",
            "video.rsx_capture_dir": "default",
            "video.rsx_capture_frames": "default",
            "title.elf_sha256": "env:PS3RECOMP_TITLE_ELF_SHA256",
            "diag.dump_config": "env:PS3RECOMP_DIAG_DUMP_CONFIG",
        }, src
        try:
            import tomllib
        except ImportError:       # Python < 3.11: the reload below is the validity check
            tomllib = None
        if tomllib:
            doc = tomllib.loads(p.stdout)
            assert doc == {"schema": 1,
                           "ppu": {"vscr_nj": False, "opd_swap_fixup": False, "null_page": "stop"},
                           "spu": {"dma_repeat_limit": 256, "dma_repeat_stop_secs": 0},
                           "gcm": {"wait_timeout_ms": 10000},
                           "trophy": {"register_wait_cap_vblanks": 1024},
                           "video": {"resolutions": "", "rsx_capture": False, "rsx_capture_dir": "",
                                     "rsx_capture_frames": 0},
                           "title": {"elf_sha256": SHA_A}, "diag": {"dump_config": False}}, doc


def test_dump_reloads_to_the_same_configuration():
    with Cfg("schema = 1", "[ppu]", "vscr_nj = false",
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
    assert "schema = 1" in p.stderr and "dump_config = true  # source: env:PS3RECOMP_DIAG_DUMP_CONFIG" in p.stderr


# ---- invalid files -----------------------------------------------------------

def test_typo_key_names_the_key_and_the_line():
    with Cfg("schema = 1", "[ppu]", "# note", "vscr_nk = true") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "ppu.vscr_nk" and f["file"] == c.path and f["line"] == "4", f
        assert "vscr_nj" in f["detail"]                     # the suggestion


def test_unknown_section_and_top_level_key():
    with Cfg("schema = 1", "[ppuu]", "vscr_nj = true") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "ppuu" and f["line"] == "2" and "'ppu'" in f["detail"], f
    with Cfg("schema = 1", "oops = 1") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "oops" and f["line"] == "2", f


def test_wrong_type_stops_naming_key_expected_and_found():
    with Cfg("schema = 1", "[ppu]", 'vscr_nj = "yes"') as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "ppu.vscr_nj" and f["line"] == "3", f
        assert "expected boolean, found string" in f["detail"]
    with Cfg("schema = 1", "[title]", "elf_sha256 = 5") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "title.elf_sha256" and "expected string, found integer" in f["detail"], f
    with Cfg("schema = 1", "ppu = true") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "ppu" and "expected a [ppu] table" in f["detail"], f


def test_out_of_range_and_malformed_values_stop():
    for bad in ("abc", "g" * 64, "a" * 63, "a" * 65, "a" * 200):
        with Cfg("schema = 1", "[title]", f'elf_sha256 = "{bad}"') as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "title.elf_sha256" and f["line"] == "3", (bad, f)


def test_video_resolutions_normalise_and_reject_unknown_ids():
    for text, want in (("2", '"2"'), ("2,1", '"1,2"'), (" 5 , 2,2 ", '"2,5"'), ("13,10,4", '"4,10,13"'),
                       ("1,2,4,5,10,11,12,13", '"1,2,4,5,10,11,12,13"')):
        with Cfg("schema = 1", "[video]", f'resolutions = "{text}"') as c:
            o = options(probe("--load", c.path, "get"))
            assert o["video.resolutions"] == (want, f"file:{c.path}"), (text, o)
    o = options(probe("get", PS3RECOMP_VIDEO_RESOLUTIONS="2"))
    assert o["video.resolutions"] == ('"2"', "env:PS3RECOMP_VIDEO_RESOLUTIONS"), o
    for bad in ("0", "3", "6", "14", "99", "abc", "2,", ",2", "2,,4", "-1", "720", "1 2"):
        with Cfg("schema = 1", "[video]", f'resolutions = "{bad}"') as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "video.resolutions" and f["line"] == "3", (bad, f)
            assert "resolution id" in f["detail"], (bad, f)


def test_null_page_is_stop_or_drop_from_file_and_environment():
    with Cfg("schema = 1", "[ppu]", "# a title that reads page 0 on purpose", 'null_page = "drop"') as c:
        o = options(probe("--load", c.path, "get"))
        assert o["ppu.null_page"] == ('"drop"', f"file:{c.path}"), o
        o = options(probe("--load", c.path, "get", PS3RECOMP_PPU_NULL_PAGE="stop"))
        assert o["ppu.null_page"] == ('"stop"', "env:PS3RECOMP_PPU_NULL_PAGE"), o
    o = options(probe("get", PS3RECOMP_PPU_NULL_PAGE="drop"))
    assert o["ppu.null_page"] == ('"drop"', "env:PS3RECOMP_PPU_NULL_PAGE"), o
    for bad in ("land", "Stop", "DROP", "", "true"):
        with Cfg("schema = 1", "[ppu]", f'null_page = "{bad}"') as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "ppu.null_page" and f["line"] == "3", (bad, f)
            assert '"stop" or "drop"' in f["detail"], (bad, f)
        f = config_invalid(probe("get", PS3RECOMP_PPU_NULL_PAGE=bad))
        assert f["env"] == "PS3RECOMP_PPU_NULL_PAGE" and f["key"] == "ppu.null_page", (bad, f)
    with Cfg("schema = 1", "[ppu]", "null_page = true") as c:      # a bool is the wrong type
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "ppu.null_page" and f["line"] == "3", f


def test_schema_mismatch_missing_and_mistyped_stop():
    for lines, line, needle in ((["schema = 2"], "1", "unsupported schema 2"),
                                (["schema = 0"], "1", "unsupported schema 0"),
                                (['schema = "1"'], "1", "expected integer"),
                                (["# no schema", "[ppu]", "vscr_nj = true"], None, "missing")):
        with Cfg(*lines) as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == "schema" and needle in f["detail"], (lines, f)
            assert f.get("line") == line, (lines, f)


def test_schema_is_checked_before_anything_else_in_the_file():
    with Cfg("[ppu]", "future_option = 1", "schema = 2") as c:
        f = config_invalid(probe("--load", c.path, "get"))
        assert f["key"] == "schema", f


def test_toml_syntax_error_names_the_line():
    with Cfg("schema = 1", "[ppu", "vscr_nj = true") as c:
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
    with Cfg("schema = 1", "[spu]", "dma_repeat_limit = 1000", "dma_repeat_stop_secs = 86400") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["spu.dma_repeat_limit"] == ("1000", f"file:{c.path}"), o
        assert o["spu.dma_repeat_stop_secs"] == ("86400", f"file:{c.path}"), o
    for text, want in (("300", "300"), ("0x100", "256"), ("0X10", "16")):
        o = options(probe("get", PS3RECOMP_SPU_DMA_REPEAT_LIMIT=text))
        assert o["spu.dma_repeat_limit"] == (want, "env:PS3RECOMP_SPU_DMA_REPEAT_LIMIT"), (text, o)
    o = options(probe("get", PS3RECOMP_SPU_DMA_REPEAT_STOP_SECS="0"))
    assert o["spu.dma_repeat_stop_secs"] == ("0", "env:PS3RECOMP_SPU_DMA_REPEAT_STOP_SECS"), o


def test_u32_out_of_range_file_values_stop():
    for key, bad in (("dma_repeat_limit", "0"), ("dma_repeat_limit", "4294967296"),
                     ("dma_repeat_limit", "-1"), ("dma_repeat_stop_secs", "86401"),
                     ("dma_repeat_stop_secs", "-1")):
        with Cfg("schema = 1", "[spu]", f"{key} = {bad}") as c:
            f = config_invalid(probe("--load", c.path, "get"))
            assert f["key"] == f"spu.{key}" and f["line"] == "3", (key, bad, f)
            assert "out of range" in f["detail"], (key, bad, f)


def test_u32_wrong_type_file_values_stop():
    for bad, found in (('"256"', "string"), ("2.5", "float"), ("true", "boolean")):
        with Cfg("schema = 1", "[spu]", f"dma_repeat_limit = {bad}") as c:
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
    with Cfg("schema = 1", "[spu]", "dma_repeat_stop_secs = 30") as c:
        p = probe("--load", c.path, "dump")
        assert p.returncode == 0, p.stderr
        assert f"dma_repeat_stop_secs = 30  # source: file:{c.path}" in p.stdout, p.stdout


# ---- structured sections: [[hle.stub]] and [[lv2.stub]] ---------------------

STUBS = ["schema = 1",
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
    stub_case(["schema = 1", "[[hle.stub]]", "nid = 1", "ret = 0"], "hle.stub[0].reason", "2", "missing required field")
    stub_case(["schema = 1", "[[hle.stub]]", "ret = 0", 'reason = "r"'], "hle.stub[0].nid", "2", "missing required field")
    stub_case(["schema = 1", "[[hle.stub]]", "nid = 1", 'reason = "r"'], "hle.stub[0].ret", "2", "missing required field")
    stub_case(["schema = 1", "[[lv2.stub]]", "num = 1", "ret = 0"], "lv2.stub[0].reason", "2", "missing required field")
    stub_case(["schema = 1", "[[lv2.stub]]", "ret = 0", 'reason = "r"'], "lv2.stub[0].num", "2", "missing required field")
    # the second entry is the broken one
    stub_case(STUBS[:5] + ["[[hle.stub]]", "nid = 2", "ret = 0"], "hle.stub[1].reason", "6", "missing required field")


def test_stub_reason_must_say_something():
    stub_case(["schema = 1", "[[hle.stub]]", "nid = 1", "ret = 0", 'reason = ""'], "hle.stub[0].reason", "5", "blank")
    stub_case(["schema = 1", "[[lv2.stub]]", "num = 1", "ret = 0", 'reason = "  \\t"'], "lv2.stub[0].reason", "5", "blank")
    stub_case(["schema = 1", "[[hle.stub]]", "nid = 1", "ret = 0", "reason = 5"], "hle.stub[0].reason", "5", "expected string")


def test_stub_numbers_are_range_and_type_checked():
    base = ["schema = 1", "[[hle.stub]]", 'reason = "r"']
    stub_case(base + ["nid = 0x100000000", "ret = 0"], "hle.stub[0].nid", "4", "out of range")
    stub_case(base + ["nid = -1", "ret = 0"], "hle.stub[0].nid", "4", "out of range")
    stub_case(base + ["nid = 1", "ret = 0x100000000"], "hle.stub[0].ret", "5", "out of range")
    stub_case(base + ["nid = 1", "ret = -2147483649"], "hle.stub[0].ret", "5", "out of range")
    stub_case(base + ['nid = "1"', "ret = 0"], "hle.stub[0].nid", "4", "expected integer, found string")
    stub_case(base + ["nid = 1", "ret = 1.5"], "hle.stub[0].ret", "5", "expected integer, found float")
    lv2 = ["schema = 1", "[[lv2.stub]]", 'reason = "r"']
    stub_case(lv2 + ["num = 1024", "ret = 0"], "lv2.stub[0].num", "4", "out of range")
    stub_case(lv2 + ["num = -1", "ret = 0"], "lv2.stub[0].num", "4", "out of range")


def test_stub_duplicates_and_unknown_fields_stop():
    stub_case(STUBS[:5] + ["[[hle.stub]]", "nid = 0xDEADBEEF", "ret = 0", 'reason = "again"'],
              "hle.stub[1].nid", "6", "duplicate")
    stub_case(["schema = 1", "[[hle.stub]]", "nid = 1", "ret = 0", 'reason = "r"', 'why = "typo"'],
              "hle.stub[0].why", "6", "unknown field")
    stub_case(["schema = 1", "[[hle.stub]]", "num = 1", "nid = 1", "ret = 0", 'reason = "r"'],
              "hle.stub[0].num", "3", "unknown field")     # lv2's key in hle
    stub_case(["schema = 1", "[[hle.stubs]]", "nid = 1"], "hle.stubs", "2", "unknown key")


def test_stub_section_must_be_arrays_of_tables():
    stub_case(["schema = 1", "[hle.stub]", "nid = 1", "ret = 0", 'reason = "r"'], "hle.stub", "2", "array of tables")
    stub_case(["schema = 1", "hle = 3"], "hle", "2", "expected a table")


# ---- lifetime ----------------------------------------------------------------

def test_loading_after_use_is_a_stop_not_a_reload():
    f = config_invalid(probe("load-again"))
    assert "after the configuration was already" in f["detail"], f
    with Cfg("schema = 1") as c:
        f = config_invalid(probe("--load", c.path, "load-again"))
        assert "after the configuration was already" in f["detail"], f


def test_concurrent_first_readers_share_one_configuration():
    for _ in range(20):
        p = probe("race")
        assert p.returncode == 0 and p.stdout.strip() == "distinct=1", p.stdout + p.stderr


# ---- ppu.opd_swap_fixup ----------------------------------------------------------

def test_opd_swap_fixup_defaults_off_and_is_read_from_file_and_environment():
    with Cfg("schema = 1", "[ppu]", "opd_swap_fixup = true") as c:
        o = options(probe("--load", c.path, "get"))
        assert o["ppu.opd_swap_fixup"] == ("true", f"file:{c.path}"), o
    o = options(probe("get", PS3RECOMP_PPU_OPD_SWAP_FIXUP="1"))
    assert o["ppu.opd_swap_fixup"] == ("true", "env:PS3RECOMP_PPU_OPD_SWAP_FIXUP"), o


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
    with Cfg("schema = 1", "[ppu]", "vscr_nj = false") as c:
        assert vscr("--load", c.path)["init"] == "0x00000000"
        assert vscr("--load", c.path, PS3RECOMP_PPU_VSCR_NJ="true")["init"] == "0x00010000"   # env beats file
    with Cfg("schema = 1", "[ppu]", "vscr_nj = true") as c:
        assert vscr("--load", c.path)["init"] == "0x00010000"
        assert vscr("--load", c.path, PS3RECOMP_PPU_VSCR_NJ="0")["init"] == "0x00000000"


def test_vscr_nj_value_and_source_appear_in_the_dump():
    for env, value, source in ((None, "true", "default"),
                               ("0", "false", "env:PS3RECOMP_PPU_VSCR_NJ")):
        p = probe("dump", **({"PS3RECOMP_PPU_VSCR_NJ": env} if env else {}))
        assert p.returncode == 0, p.stderr
        assert f"vscr_nj = {value}  # source: {source}" in p.stdout, p.stdout


if __name__ == "__main__":
    PROBE = sys.argv[1]
    sys.exit(g.main([(n, f) for n, f in sorted(globals().items()) if n.startswith("test_")]))
