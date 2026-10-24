#!/usr/bin/env python3
"""Kernel-produced event delivery: lv2 drops vs a stop for a bad queue id.

usage: run_event_delivery.py <test_event_delivery>
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "gates"))

import gates_common as g

PROBE = None
DROP_LOG = "kernel event dropped"


def probe(*args):
    return g.run([PROBE, *args])


def test_delivery_that_fits_returns():
    p = probe("ok")
    assert p.returncode == 0, p.stderr
    assert p.stdout.strip() == "delivered", p.stdout
    assert DROP_LOG not in p.stderr, p.stderr


def test_full_queue_drops_counts_and_logs_once():
    p = probe("full")
    assert p.returncode == 0, p.stderr
    assert p.stdout.strip() == "drops=2 pending=2", p.stdout
    assert p.stderr.count(DROP_LOG) == 1, p.stderr
    assert "producer=test-event queue=0x1 cause=full" in p.stderr, p.stderr
    assert "kind=" not in p.stderr, p.stderr


def test_destroyed_queue_drops_and_counts():
    p = probe("destroyed")
    assert p.returncode == 0, p.stderr
    assert p.stdout.strip() == "drops=2 pending=0", p.stdout
    assert p.stderr.count(DROP_LOG) == 1, p.stderr
    assert "producer=test-event queue=0x1 cause=destroyed" in p.stderr, p.stderr
    assert "kind=" not in p.stderr, p.stderr


def test_never_created_id_stops():
    f = g.expect_stop(probe("invalid"), "event-delivery-lost")
    assert f["producer"] == "test-event", f
    assert f["queue"] == hex(128), f
    assert f["cause"] == "invalid-id", f
    assert f["source"] == "0x1234", f


def test_out_of_range_id_stops():
    f = g.expect_stop(probe("invalid-range"), "event-delivery-lost")
    assert f["queue"] == hex(129), f
    assert f["cause"] == "invalid-id", f


if __name__ == "__main__":
    PROBE = sys.argv[1]
    sys.exit(g.main([(n, f) for n, f in sorted(globals().items())
                     if n.startswith("test_")]))
