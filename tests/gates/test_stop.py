#!/usr/bin/env python3
"""Named stop: exit code, banner, the machine line, concurrency.

usage: test_stop.py <probe_stop>
"""
import re
import sys

import gates_common as g

PROBE = None

KINDS = ["identity-mismatch", "config-invalid", "unresolved-nid",
         "unimplemented-syscall", "unlifted-function",
         "unresolved-indirect-call", "unknown-instruction",
         "dispatch-depth-exceeded", "spu-dma-repeat",
         "unresolved-guest-callback",
         "hle-unsupported-return", "spu-job-crt-version", "spu-job-memory",
         "spu-job-jm-entry", "spu-job-dma-list", "jq2-pool-release",
         "jq2-job-end", "jq2-ring-entry", "jq2-wake", "jq2-state", "jq2-jm-call",
         "hle-guest-range", "hle-uninitialized",
         "sync2-semaphore-unsupported", "fiber-unsupported", "gcm-unmodelled",
         "gcm-wait-timeout"]


def probe(*args):
    return g.run([PROBE, *args])


def test_kind_ids_are_stable():
    # The ids are an interface: a rename is a breaking change for every tool that
    # counts or allow-lists by kind, so the list is pinned here on purpose.
    p = probe("kinds")
    assert p.returncode == 0 and p.stdout.split() == KINDS, p.stdout


def test_every_kind_stops_with_its_id():
    for i, kind in enumerate(KINDS):
        f = g.expect_stop(probe("kind", str(i)), kind)
        assert "pc" not in f and "tid" not in f, f


def test_out_of_range_kind_still_stops():
    g.expect_stop(probe("kind", "99"), "invalid-kind")


def test_plain_stop_banner_and_line():
    p = probe("plain")
    f = g.expect_stop(p, "identity-mismatch")
    assert list(f) == ["v", "kind", "path", "expected", "actual"], list(f)
    assert f["path"] == "/tmp/x.elf" and f["expected"] == "aa" and f["actual"] == "bb"
    assert "PS3RECOMP STOP: identity-mismatch" in p.stderr
    assert "re-lift from this file" in p.stderr          # the remedy is in the banner
    assert "status 70" in p.stderr
    assert p.stdout == ""


def test_site_and_frontier_reach_both_outputs():
    p = probe("site")
    f = g.expect_stop(p, "unresolved-nid")
    assert (f["nid"], f["name"]) == ("0x12345678", "cellFooBar")
    assert (f["pc"], f["lr"], f["tid"]) == ("0x123456", "0x123450", "7")
    assert f["last_hle_nid"] == "0xDEADBEEF" and f["last_hle"] == "cellProbeLastCall"
    assert f["syscall"] == "141"
    assert "pc=0x123456 lr=0x123450 tid=7" in p.stderr
    assert "last HLE import 0xDEADBEEF (cellProbeLastCall)" in p.stderr
    assert "inside lv2 syscall 141" in p.stderr


def test_frontier_is_per_thread():
    f = g.expect_stop(probe("other-thread"), "unknown-instruction")
    assert "syscall" not in f and f["tid"] == "9"


def test_values_are_escaped_and_keys_sanitised():
    f = g.expect_stop(probe("escape"), "config-invalid")
    assert f["mixed_key"] == "has space"
    assert f["f_kind"] == "collides"            # a reserved key cannot be forged
    assert f["quote"] == 'a"b\\c'
    assert f["multi"] == "line1\nline2"
    assert f["empty"] == ""
    assert f["ctl"] == "tab\there\x01"


def test_missing_what_and_remedy_are_loud_not_silent():
    p = probe("bare")
    g.expect_stop(p, "unlifted-function")
    assert "bug in the caller" in p.stderr


def test_nested_stop_exits_at_once():
    p = probe("nested")
    assert p.returncode == g.STOP_EXIT_CODE, p.stderr
    assert "nested=1" in p.stderr


def test_buffered_stdout_is_flushed_before_the_banner():
    p = probe("buffered-stdout")
    g.expect_stop(p, "unimplemented-syscall")
    assert "log line before the stop" in p.stdout


def test_concurrent_stops_print_one_coherent_banner():
    for _ in range(25):
        p = probe("concurrent")
        f = g.expect_stop(p, "unresolved-nid")
        assert p.stderr.count("PS3RECOMP STOP:") == 1, p.stderr
        assert p.stderr.count("concurrent stop") == 1, p.stderr
        # The banner rows and the machine line describe the same winner.
        rows = re.findall(r"^  worker\s+(\d+)$", p.stderr, re.M)
        assert rows == [f["worker"]], (rows, f)
        assert f"tid={f['tid']}" in p.stderr
        assert f["tid"] == f["worker"] and f["pc"] == hex(int(f["worker"])).upper().replace("0X", "0x"), f


if __name__ == "__main__":
    PROBE = sys.argv[1]
    sys.exit(g.main([(n, f) for n, f in sorted(globals().items()) if n.startswith("test_")]))
