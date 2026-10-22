#!/usr/bin/env python3
"""The executable identity gate, end to end through the boot smoke title.

The smoke title is linked against a lift whose ppu_lift_image_sha256() was
generated from smoke.elf (tools/make_smoke_elf.py, as ppu_lifter.py does for a
real image) and boots it through the production ppu_load_elf. Here that title is
given smoke.elf itself, which must boot, and files that differ from it, which must
not: a stop of kind identity-mismatch naming both hashes, before a byte is read
as an ELF. The expected hashes are computed here with hashlib, not by the runtime.

usage: test_identity.py <ps3recomp_boot_smoke> <smoke.elf>
"""
import hashlib
import os
import sys
import tempfile

import gates_common as g

BOOT = None
GOOD_PATH = None
GOOD = None
GOOD_SHA = None


def sha(data):
    return hashlib.sha256(data).hexdigest()


def boot(path, **env):
    # The Metal backend needs its headless mode where there is no display.
    return g.run([BOOT, path], env=g.clean_env(PS3RECOMP_METAL_HEADLESS="1", **env), timeout=120)


def booted(proc):
    assert proc.returncode == 0, f"exit {proc.returncode}\n{proc.stderr[-2000:]}"
    assert "[smoke] PASS" in proc.stdout + proc.stderr


class Variant:
    def __init__(self, data):
        self._dir = tempfile.TemporaryDirectory(prefix="ps3id-")
        self.path = os.path.join(self._dir.name, "variant.elf")
        self.data = data
        with open(self.path, "wb") as f:
            f.write(data)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self._dir.cleanup()


def mismatch(proc, variant, expected=None, expected_from="lift"):
    f = g.expect_stop(proc, "identity-mismatch")
    assert f["path"] == variant.path, f
    assert f["expected"] == (expected or GOOD_SHA), f
    assert f["actual"] == sha(variant.data), f
    assert f["size"] == str(len(variant.data)), f
    assert f["expected_from"] == expected_from, f
    assert f["expected"] != f["actual"], f
    return f


def flipped(offset):
    data = bytearray(GOOD)
    data[offset] ^= 0x01
    return bytes(data)


def test_the_title_boots_from_its_own_image():
    booted(boot(GOOD_PATH))


def test_one_byte_different_inside_a_loaded_segment_is_refused():
    # In the data segment's initialised bytes: the file loads fine as an ELF.
    with Variant(flipped(len(GOOD) - 1)) as v:
        mismatch(boot(v.path), v)


def test_one_byte_the_loader_never_reads_is_refused_too():
    # e_flags and padding inside the text segment: no segment the loader maps
    # would change, so only a hash of the whole file can tell.
    zero = next(i for i in range(0x1010, 0x2000) if GOOD[i] == 0)
    for offset in (48, zero):
        with Variant(flipped(offset)) as v:
            mismatch(boot(v.path), v)


def test_a_longer_or_shorter_file_is_refused():
    for data in (GOOD + b"\x00", GOOD[:-1]):
        with Variant(data) as v:
            mismatch(boot(v.path), v)


def test_a_file_that_is_not_an_elf_is_an_identity_mismatch_not_a_load_error():
    for data in (b"not an elf at all", b""):
        with Variant(data) as v:
            mismatch(boot(v.path), v)


def test_the_banner_names_both_hashes_and_the_remedy():
    with Variant(flipped(48)) as v:
        p = boot(v.path)
        g.expect_stop(p, "identity-mismatch")
        assert GOOD_SHA in p.stderr and sha(v.data) in p.stderr
        assert "re-lift" in p.stderr


def test_a_matching_pin_is_accepted_in_either_case():
    booted(boot(GOOD_PATH, PS3RECOMP_TITLE_ELF_SHA256=GOOD_SHA))
    booted(boot(GOOD_PATH, PS3RECOMP_TITLE_ELF_SHA256=GOOD_SHA.upper()))


def test_a_pin_that_names_another_image_refuses_even_the_lifted_one():
    other = "0123456789abcdef" * 4
    p = boot(GOOD_PATH, PS3RECOMP_TITLE_ELF_SHA256=other)
    f = g.expect_stop(p, "identity-mismatch")
    assert f["expected"] == other and f["actual"] == GOOD_SHA, f
    assert f["expected_from"] == "config", f
    assert f["pin_source"] == "env:PS3RECOMP_TITLE_ELF_SHA256", f


def test_the_pin_can_come_from_the_config_file():
    other = "fedcba9876543210" * 4
    with tempfile.TemporaryDirectory(prefix="ps3id-cfg-") as d:
        cfg = os.path.join(d, "title.toml")
        with open(cfg, "w") as fh:
            fh.write(f'schema = 1\n[title]\nelf_sha256 = "{other}"\n')
        f = g.expect_stop(boot(GOOD_PATH, PS3RECOMP_CONFIG=cfg), "identity-mismatch")
        assert f["expected_from"] == "config" and f["pin_source"] == f"file:{cfg}", f
        with open(cfg, "w") as fh:
            fh.write(f'schema = 1\n[title]\nelf_sha256 = "{GOOD_SHA}"\n')
        booted(boot(GOOD_PATH, PS3RECOMP_CONFIG=cfg))


def test_a_pin_only_narrows_it_cannot_vouch_for_a_different_image():
    # Pinning the hash of the modified file does not make it loadable: the lift's
    # own hash is checked first and the pin is checked in addition.
    with Variant(flipped(48)) as v:
        mismatch(boot(v.path, PS3RECOMP_TITLE_ELF_SHA256=sha(v.data)), v)


def test_a_malformed_pin_is_a_config_error_not_an_identity_one():
    f = g.expect_stop(boot(GOOD_PATH, PS3RECOMP_TITLE_ELF_SHA256="not-a-hash"), "config-invalid")
    assert f["key"] == "title.elf_sha256", f


def test_a_config_typo_stops_the_title_before_it_loads_anything():
    f = g.expect_stop(boot(GOOD_PATH, PS3RECOMP_FOO="1"), "config-invalid")
    assert f["env"] == "PS3RECOMP_FOO", f


if __name__ == "__main__":
    BOOT, GOOD_PATH = sys.argv[1], sys.argv[2]
    with open(GOOD_PATH, "rb") as fh:
        GOOD = fh.read()
    GOOD_SHA = sha(GOOD)
    sys.exit(g.main([(n, f) for n, f in sorted(globals().items()) if n.startswith("test_")]))
