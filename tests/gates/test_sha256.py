#!/usr/bin/env python3
"""SHA-256: the FIPS 180-4 / NIST example vectors, then every length around the
block and padding boundaries against hashlib.

usage: test_sha256.py <probe_sha256>
"""
import hashlib
import sys

import gates_common as g

PROBE = None


def sha(*args):
    p = g.run([PROBE, *args])
    assert p.returncode == 0, p.stderr
    return p.stdout.strip()


def pattern(n):
    return bytes((i * 31 + 7) & 0xFF for i in range(n))


# NIST CSRC "SHA-256 example values" (FIPS 180-4 appendix B and the long-message example).
NIST = [
    ("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
    ("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
    ("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
    ("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
     "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"),
]


def test_nist_short_vectors():
    for text, want in NIST:
        assert sha("str", text) == want, text


def test_nist_million_a():
    assert sha("repeat", "0x61", "1000000") == \
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"


def test_every_length_around_the_padding_boundaries():
    # 55/56 and 63/64 are where the length field no longer fits and a second
    # block appears; check them and everything near them, plus a few blocks.
    for n in list(range(0, 200)) + [255, 256, 1000, 4096, 65537]:
        assert sha("pattern", str(n)) == hashlib.sha256(pattern(n)).hexdigest(), n


def test_chunked_updates_equal_one_shot():
    for n in (1, 63, 64, 65, 127, 128, 129, 1000):
        want = hashlib.sha256(pattern(n)).hexdigest()
        for chunk in (1, 3, 7, 63, 64, 65, 1000):
            assert sha("pattern", str(n), str(chunk)) == want, (n, chunk)


if __name__ == "__main__":
    PROBE = sys.argv[1]
    sys.exit(g.main([(n, f) for n, f in sorted(globals().items()) if n.startswith("test_")]))
