"""ps3diff.rsx/1: an RSX capture, its validator and its comparison.

A capture is `capture.ndjson` plus `blobs/sha256/<2 hex>/<64 hex>` beside it
(docs/PS3DIFF.md, "ps3diff.rsx/1"). The envelope rules are event/2's: strict
fields, strictly increasing `seq`, contiguous `ctx_seq`, canonical JSON. This
module adds the event registry, the blob checks and the replay-relevant
invariants (the FIFO walk in executed order, CALL/RET balance, the IO map, the
draw accounting, the three hashes), and compares two captures frame by frame.

Nothing here renders anything; the replay runner (tools/rsx_replay) does that
through the same walker, engine and Metal backend the title uses.
"""

from __future__ import annotations

import hashlib
import os
import re
from pathlib import Path

from .schema import (
    HEX_ADDRESS,
    HEX_SHA256,
    Ps3DiffError,
    RecordProblem,
    canonical_json,
    read_ndjson,
)

SCHEMA_RSX = "ps3diff.rsx/1"

#: Process status of `rsx-compare` when the environments differ: neither pass nor fail.
EXIT_NOT_COMPARABLE = 4

ENVELOPE = frozenset({"schema", "seq", "ctx_seq", "t", "thread", "event", "ctx"})

#: event -> (required payload fields, optional payload fields)
EVENTS: dict[str, tuple[frozenset, frozenset]] = {
    "capture-start": (frozenset({"bootstrap", "producer"}), frozenset()),
    "environment": (frozenset({"machine"}), frozenset()),
    "gcm-init": (frozenset({"cmd_size", "io_size", "io_ea"}), frozenset()),
    "gcm-state": (frozenset({"op"}), frozenset({"ea", "io", "size", "id", "offset", "pitch", "width", "height"})),
    "fifo-method": (frozenset({"get", "put", "call_ret", "subchannel", "type", "count", "method", "bound",
                               "binds", "reads", "writes"}), frozenset()),
    "fifo-branch": (frozenset({"get", "put", "call_ret", "kind", "target", "reads", "writes"}), frozenset()),
    "fifo-word": (frozenset({"get", "put", "call_ret", "reads", "writes"}), frozenset()),
    "fifo-nop": (frozenset({"get", "put", "call_ret", "words", "reads", "writes"}), frozenset()),
    "draw": (frozenset({"outcome", "reason", "fixed_function", "state_hash"}), frozenset()),
    "frame-complete": (frozenset({"frame", "capture_hash", "state_hash", "frame_hash", "readback", "draws",
                                  "unknown_methods", "resyncs", "valid", "invalid"}), frozenset()),
    "capture-end": (frozenset({"frames", "events", "capture_hash"}), frozenset()),
}

#: Events that are inputs: the capture hash covers exactly these lines.
INPUT_EVENTS = frozenset({"capture-start", "gcm-init", "gcm-state", "fifo-method", "fifo-branch", "fifo-word",
                          "fifo-nop"})
FIFO_EVENTS = frozenset({"fifo-method", "fifo-branch", "fifo-word", "fifo-nop"})

GCM_STATE_OPS = {
    "io-map": frozenset({"ea", "io", "size"}),
    "io-unmap": frozenset({"ea", "io", "size"}),
    "display-buffer": frozenset({"id", "offset", "pitch", "width", "height"}),
}

MACHINE_FIELDS = ("gpu", "macos", "metal", "build", "glslang", "spirv_cross", "toolkit_sha", "title_sha")

READ_KINDS = frozenset({"other", "fifo", "label", "vertex", "index", "texture", "program", "dma-source"})
WRITE_KINDS = frozenset({"other", "label", "report", "dma-destination"})
BLOB_REQUIRED = frozenset({"ea", "location", "size", "sha256", "consume_seq"})
BLOB_OPTIONAL = frozenset({"offset", "kind"})

#: rsx_drop_reason_name(), libs/video/rsx_draw_engine.c.
DROP_REASONS = frozenset({
    "not-ready", "no-packets", "fetch-failed", "mixed-sources", "needs-expansion", "unsupported-topology",
    "no-vertices", "topology-rebuild-failed", "no-indices", "no-targets", "pipeline-failed", "debug-skip",
    "texture-feedback", "index-alloc-failed", "constant-alloc-failed",
})

#: The readback formats the contract defines, and their bytes per pixel.
READBACK_BPP = {"rgba8-unorm": 4, "rgba16-float": 8, "rgba32-float": 16}

RING_RET = 0x00020000


def _is_int(v: object) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def _show(v: object) -> str:
    text = repr(v)
    return text if len(text) <= 80 else text[:77] + "..."


def _need(cond: bool, where: str, message: str) -> None:
    if not cond:
        raise RecordProblem(where, message)


def _addr(rec: dict, name: str) -> int:
    v = rec.get(name)
    _need(isinstance(v, str) and HEX_ADDRESS.match(v) is not None, name,
          f"must be 0x followed by 8 lowercase hex digits, found {_show(v)}")
    return int(v, 16)


def _uint(rec: dict, name: str, maximum: int | None = None) -> int:
    v = rec.get(name)
    _need(_is_int(v) and v >= 0 and (maximum is None or v <= maximum), name,
          f"must be an integer >= 0{'' if maximum is None else f' and <= {maximum}'}, found {_show(v)}")
    return v


def _hash_obj(rec: dict, name: str, scope: str | None = None) -> str:
    v = rec.get(name)
    _need(isinstance(v, dict) and set(v) == {"algo", "scope", "value"}, name,
          'must be {"algo", "scope", "value"}')
    _need(v["algo"] == "sha256", f"{name}.algo", "must be 'sha256'")
    _need(isinstance(v["scope"], str) and v["scope"], f"{name}.scope", "must be a non-empty string")
    if scope is not None:
        _need(v["scope"] == scope, f"{name}.scope", f"must be {scope!r}, found {_show(v['scope'])}")
    _need(isinstance(v["value"], str) and HEX_SHA256.match(v["value"]) is not None, f"{name}.value",
          "must be 64 lowercase hex digits")
    return v["value"]


class _Blobs:
    """Content-addressed blob lookup beside a capture, hashing each file once."""

    def __init__(self, root: Path):
        self.root = root
        self._ok: dict[str, int] = {}

    def path(self, sha: str) -> Path:
        return self.root / "blobs" / "sha256" / sha[:2] / sha

    def read(self, sha: str, size: int, where: str) -> bytes:
        p = self.path(sha)
        try:
            data = p.read_bytes()
        except FileNotFoundError:
            raise RecordProblem(where, f"blob {sha} is missing ({p})") from None
        except OSError as exc:
            raise RecordProblem(where, f"blob {sha} cannot be read: {exc.strerror}") from None
        if sha not in self._ok:
            actual = hashlib.sha256(data).hexdigest()
            if actual != sha:
                raise RecordProblem(where, f"blob {sha} is altered: its bytes hash to {actual}")
            self._ok[sha] = len(data)
        if len(data) != size:
            raise RecordProblem(where, f"blob {sha} is {len(data)} bytes, the capture says {size}")
        return data


class RsxCapture:
    """What `validate` learned about a capture."""

    def __init__(self, path: str):
        self.path = path
        self.machine: dict = {}
        self.frames: list[dict] = []        # one dict per frame-complete record, in file order
        self.events = 0
        self.capture_hash = ""


def _blob_ref(ref: object, where: str, kinds: frozenset) -> dict:
    _need(isinstance(ref, dict), where, "must be an object")
    keys = set(ref)
    _need(BLOB_REQUIRED <= keys <= BLOB_REQUIRED | BLOB_OPTIONAL, where,
          f"fields must be {sorted(BLOB_REQUIRED)} plus optional {sorted(BLOB_OPTIONAL)}, found {sorted(keys)}")
    _need(isinstance(ref["ea"], str) and HEX_ADDRESS.match(ref["ea"]) is not None, f"{where}.ea",
          "must be 0x followed by 8 lowercase hex digits")
    _need(ref["location"] in ("local", "main"), f"{where}.location", "must be 'local' or 'main'")
    _need(_is_int(ref["size"]) and ref["size"] >= 1, f"{where}.size", "must be an integer >= 1")
    _need(isinstance(ref["sha256"], str) and HEX_SHA256.match(ref["sha256"]) is not None, f"{where}.sha256",
          "must be 64 lowercase hex digits")
    _need(_is_int(ref["consume_seq"]) and ref["consume_seq"] >= 0, f"{where}.consume_seq",
          "must be an integer >= 0")
    if "offset" in ref:
        _need(isinstance(ref["offset"], str) and HEX_ADDRESS.match(ref["offset"]) is not None, f"{where}.offset",
              "must be 0x followed by 8 lowercase hex digits")
    if "kind" in ref:
        _need(ref["kind"] in kinds, f"{where}.kind", f"must be one of {sorted(kinds)}, found {_show(ref['kind'])}")
    return ref


class _IoMap:
    """The IO offset -> EA page table, mirrored from cellGcmSys.c (populate/clear_offset_table)."""

    def __init__(self):
        self.pages: dict[int, int] = {}

    def map(self, ea: int, io: int, size: int) -> None:
        pages = (size + 0xFFFFF) >> 20
        for i in range(pages):
            if (ea >> 20) + i < 65536 and (io >> 20) + i < 65536:
                self.pages[(io >> 20) + i] = (ea >> 20) + i

    def unmap(self, ea: int, io: int, size: int) -> None:
        for i in range(size >> 20):
            if (ea >> 20) + i < 65536 and (io >> 20) + i < 65536:
                self.pages.pop((io >> 20) + i, None)

    def ea(self, io: int) -> int | None:
        page = self.pages.get(io >> 20)
        return None if page is None else (page << 20) | (io & 0xFFFFF)


def validate_rsx(path: str, entries: list[tuple[int, str, dict]] | None = None) -> RsxCapture:
    """Check a whole capture. Raises Ps3DiffError naming the file, the line and the field."""
    path = str(path)
    if path.endswith(".partial"):
        raise Ps3DiffError(f"{path}: an unfinished capture (the writer renames it when it closes)",
                           "the process that wrote it was killed or stopped before capture-end")
    if entries is None:
        entries = read_ndjson(path)
    if not entries:
        raise Ps3DiffError(f"{path}: empty file: a capture starts with capture-start")
    cap = RsxCapture(path)
    blobs = _Blobs(Path(path).resolve().parent)
    try:
        _walk(path, entries, cap, blobs)
    except _Located as exc:
        raise Ps3DiffError(f"{path}: line {exc.lineno}: {exc.problem.where}: {exc.problem.message}") from None
    return cap


class _Located(Exception):
    def __init__(self, lineno: int, problem: RecordProblem):
        self.lineno = lineno
        self.problem = problem


def _walk(path: str, entries: list[tuple[int, str, dict]], cap: RsxCapture, blobs: _Blobs) -> None:
    seq_prev = -1
    ctx_prev = -1
    running = hashlib.sha256()
    io = _IoMap()
    gcm_ready = False
    expected_get: int | None = None
    call_slot = 0
    bound = [0] * 8
    last_consume = -1
    last_flip = -1
    frame_draws = {"submitted": 0, "executed": 0, "fixed": 0, "dropped": {}}
    seen_start = seen_env = seen_end = False
    prev_event = None

    for lineno, raw, rec in entries:
        try:
            ev = _envelope(rec, seq_prev, ctx_prev)
            if canonical_json(rec) != raw:
                raise RecordProblem("line", "is not canonical JSON (compact separators, ASCII, the writer's key order)")
            if seen_end:
                raise RecordProblem("event", "nothing may follow capture-end")
            if prev_event is None and ev != "capture-start":
                raise RecordProblem("event", "the first record must be capture-start")
            if ev == "capture-start" and seen_start:
                raise RecordProblem("event", "a second capture-start")
            seq_prev = rec["seq"]
            ctx_prev = rec["ctx_seq"]
            _fields(rec, ev)

            if ev == "capture-start":
                seen_start = True
                b = rec["bootstrap"]
                _need(isinstance(b, dict) and set(b) == {"mode"}, "bootstrap", 'must be {"mode": "clean-reset"}')
                _need(b["mode"] == "clean-reset", "bootstrap.mode",
                      f"must be 'clean-reset' (a capture that starts midstream is not replayable), found {_show(b['mode'])}")
                _need(isinstance(rec["producer"], str) and rec["producer"], "producer", "must be a non-empty string")
            elif ev == "environment":
                _need(not seen_env and prev_event == "capture-start", "event",
                      "environment follows capture-start, once")
                seen_env = True
                m = rec["machine"]
                _need(isinstance(m, dict) and set(m) == set(MACHINE_FIELDS), "machine",
                      f"must have exactly {list(MACHINE_FIELDS)}")
                for k in MACHINE_FIELDS:
                    _need(isinstance(m[k], str) and m[k], f"machine.{k}", "must be a non-empty string")
                cap.machine = dict(m)
            elif ev == "gcm-init":
                cmd = _uint(rec, "cmd_size")
                size = _uint(rec, "io_size")
                ea = _addr(rec, "io_ea")
                _need(cmd > 0xFFFF and cmd % 4 == 0 and cmd + 0x1000 <= size and size % 0x100000 == 0
                      and ea % 0x100000 == 0, "cmd_size",
                      "does not describe a context cellGcmSetupContext accepts")
                io = _IoMap()
                io.map(ea, 0, size)
                gcm_ready = True
                expected_get = None
                call_slot = 0
                bound = [0] * 8
            elif ev == "gcm-state":
                op = rec["op"]
                _need(op in GCM_STATE_OPS, "op", f"must be one of {sorted(GCM_STATE_OPS)}, found {_show(op)}")
                have = {k for k in rec if k not in ENVELOPE and k != "op" and not k.startswith("x-")}
                _need(have == GCM_STATE_OPS[op], "op", f"{op} carries exactly {sorted(GCM_STATE_OPS[op])}")
                if op in ("io-map", "io-unmap"):
                    ea, iov, size = _addr(rec, "ea"), _addr(rec, "io"), _uint(rec, "size")
                    (io.map if op == "io-map" else io.unmap)(ea, iov, size)
                else:
                    _uint(rec, "id", 7)
                    _addr(rec, "offset")
                    _uint(rec, "pitch")
                    _uint(rec, "width")
                    _uint(rec, "height")
            elif ev in FIFO_EVENTS:
                _need(gcm_ready, "event", "a FIFO step before any gcm-init: only a clean reset is a capture")
                last_consume, expected_get, call_slot = _fifo(rec, ev, io, blobs, bound, expected_get,
                                                              call_slot, last_consume)
            elif ev == "draw":
                _need(rec["outcome"] in ("executed", "dropped"), "outcome", "must be 'executed' or 'dropped'")
                _need(isinstance(rec["fixed_function"], bool), "fixed_function", "must be a boolean")
                if rec["outcome"] == "executed":
                    _need(rec["reason"] is None, "reason", "an executed draw has no drop reason")
                    frame_draws["executed"] += 1
                    frame_draws["fixed"] += 1 if rec["fixed_function"] else 0
                else:
                    _need(rec["reason"] in DROP_REASONS, "reason",
                          f"an unnamed drop reason {_show(rec['reason'])}")
                    _need(not rec["fixed_function"], "fixed_function", "only an executed draw ran a pipeline")
                    frame_draws["dropped"][rec["reason"]] = frame_draws["dropped"].get(rec["reason"], 0) + 1
                frame_draws["submitted"] += 1
                _hash_obj(rec, "state_hash", "draw-state/v1")
            elif ev == "frame-complete":
                last_flip = _frame(rec, running.hexdigest(), last_flip, frame_draws, cap)
                frame_draws = {"submitted": 0, "executed": 0, "fixed": 0, "dropped": {}}
            elif ev == "capture-end":
                seen_end = True
                _need(rec["frames"] == len(cap.frames), "frames",
                      f"says {rec['frames']}, the file has {len(cap.frames)} frame-complete records")
                _need(rec["events"] == rec["seq"], "events",
                      f"says {rec['events']} records before it, found {rec['seq']}")
                h = _hash_obj(rec, "capture_hash", "input-events/v1")
                _need(h == running.hexdigest(), "capture_hash",
                      "does not match the SHA-256 of the input events")
                cap.capture_hash = h
                cap.events = rec["events"]
            prev_event = ev
            if ev in INPUT_EVENTS:
                running.update(raw.encode("ascii") + b"\n")
        except RecordProblem as problem:
            raise _Located(lineno, problem) from None
    if not seen_end:
        raise Ps3DiffError(f"{path}: no capture-end: the capture is truncated")
    if not seen_env:
        raise Ps3DiffError(f"{path}: no environment record: a baseline without one cannot be compared")


def _envelope(rec: dict, seq_prev: int, ctx_prev: int) -> str:
    _need(rec.get("schema") == SCHEMA_RSX, "schema",
          f"must be {SCHEMA_RSX!r}, found {_show(rec.get('schema'))}")
    ev = rec.get("event")
    _need(isinstance(ev, str) and ev in EVENTS, "event", f"unknown event {_show(ev)}")
    _need(_is_int(rec.get("seq")) and rec["seq"] > seq_prev, "seq", f"must be an integer > {seq_prev}")
    _need(_is_int(rec.get("ctx_seq")) and rec["ctx_seq"] == ctx_prev + 1, "ctx_seq",
          f"must be {ctx_prev + 1} (0, 1, 2, ... in file order)")
    t = rec.get("t")
    _need(isinstance(t, (int, float)) and not isinstance(t, bool) and t >= 0, "t", "must be a number >= 0")
    _need(rec.get("thread") == "rsx", "thread", "must be 'rsx'")
    _need(rec.get("ctx") == {"kind": "ppu", "id": "rsx"}, "ctx", 'must be {"kind": "ppu", "id": "rsx"}')
    return ev


def _fields(rec: dict, ev: str) -> None:
    required, optional = EVENTS[ev]
    own = {k for k in rec if k not in ENVELOPE and not k.startswith("x-")}
    missing = required - own
    _need(not missing, sorted(missing)[0] if missing else "", f"{ev} is missing a required field")
    extra = own - required - optional
    _need(not extra, sorted(extra)[0] if extra else "", f"{ev} has an unknown field (extensions start with 'x-')")


def _fifo(rec: dict, ev: str, io: _IoMap, blobs: _Blobs, bound: list[int], expected_get: int | None,
          call_slot: int, last_consume: int) -> tuple[int, int, int]:
    get = _addr(rec, "get")
    _addr(rec, "put")
    call_ret = _addr(rec, "call_ret")
    _need(io.ea(get) is not None, "get", f"IO offset 0x{get:08x} has no mapping")
    _need(expected_get is None or get == expected_get, "get",
          f"0x{get:08x} is not where the previous step left the walker (0x{expected_get:08x}): "
          "commands were lost, so the executed order is not the captured one" if expected_get is not None else "")
    _need(call_ret == call_slot, "call_ret",
          f"is 0x{call_ret:08x} but the CALL/RET state before this step is 0x{call_slot:08x}")

    reads = rec["reads"]
    writes = rec["writes"]
    _need(isinstance(reads, list) and reads, "reads", "must be a non-empty list of blobs")
    _need(isinstance(writes, list), "writes", "must be a list of blobs")
    seqs = []
    fifo_bytes = []
    for i, ref in enumerate(reads):
        where = f"reads[{i}]"
        _blob_ref(ref, where, READ_KINDS)
        data = blobs.read(ref["sha256"], ref["size"], where)
        seqs.append(ref["consume_seq"])
        if ref.get("kind") == "fifo":
            _need("offset" in ref, f"{where}.offset", "a FIFO read names its IO offset")
            off = int(ref["offset"], 16)
            _need(ref["location"] == "main" and io.ea(off) == int(ref["ea"], 16), f"{where}.ea",
                  "is not the EA the IO map gives for the read's offset")
            fifo_bytes.append((off, data))
        else:
            _need(ref["size"] <= 256 << 20, f"{where}.size", "is not a plausible span")
    for i, ref in enumerate(writes):
        where = f"writes[{i}]"
        _blob_ref(ref, where, WRITE_KINDS)
        blobs.read(ref["sha256"], ref["size"], where)
        seqs.append(ref["consume_seq"])
    _need(len(set(seqs)) == len(seqs), "reads", "consume_seq repeats inside one event")
    _need(not seqs or min(seqs) > last_consume, "reads",
          f"consume_seq {min(seqs) if seqs else 0} does not follow the previous event's {last_consume}")
    _need(all(a < b for a, b in zip([s for s in seqs[:len(reads)]], [s for s in seqs[1:len(reads)]])), "reads",
          "consume_seq is not increasing")
    last = max(seqs) if seqs else last_consume

    fifo_bytes.sort()
    stream = b""
    nxt = get
    for off, data in fifo_bytes:
        _need(off == nxt, "reads", f"FIFO bytes are not contiguous from get (gap at 0x{nxt:08x})")
        stream += data
        nxt = off + len(data)
    _need(fifo_bytes and fifo_bytes[0][0] == get, "reads", "no FIFO read starts at get")
    _need(len(stream) % 4 == 0, "reads", "FIFO reads are not whole words")
    words = [int.from_bytes(stream[i:i + 4], "big") for i in range(0, len(stream), 4)]

    if ev == "fifo-method":
        w = words[0]
        _need(len(words) >= 1 and w >> 29 in (0, 2), "reads", "the first FIFO word is not a method header")
        count = (w >> 18) & 0x7FF
        sub = (w >> 13) & 7
        method = w & 0x1FFC
        _need(_uint(rec, "subchannel", 7) == sub, "subchannel", "disagrees with the header word")
        _need(rec["type"] == (0 if w >> 29 == 0 else 2), "type", "disagrees with the header word")
        _need(_uint(rec, "count") == count, "count", f"is {rec['count']}, the header says {count}")
        _need(rec["method"] == f"0x{method:04x}", "method", "disagrees with the header word")
        _need(len(words) == 1 + count, "reads", f"the method packet is {len(words)} words, the header says {1 + count}")
        b = rec["bound"]
        _need(b is None or (isinstance(b, str) and HEX_ADDRESS.match(b)), "bound", "must be null or 0x%08x")
        _need((0 if b is None else int(b, 16)) == bound[sub], "bound",
              f"is {b}, the binding the capture itself recorded for subchannel {sub} is 0x{bound[sub]:08x}")
        want = []
        for i in range(count):
            m = method + 4 * i if w >> 29 == 0 else method
            if m == 0:
                want.append({"subchannel": sub, "handle": f"0x{words[1 + i]:08x}"})
                bound[sub] = words[1 + i]
        _need(rec["binds"] == want, "binds", "does not match the SET_OBJECT words of the packet")
        expected_get = get + 4 + 4 * count
    elif ev == "fifo-nop":
        n = _uint(rec, "words")
        _need(n >= 1 and len(words) == n and not any(words), "words", "a nop run is that many zero words")
        expected_get = get + 4 * n
    elif ev == "fifo-word":
        _need(len(words) == 1, "reads", "a FIFO word is one word")
        w = words[0]
        flip = (w & 0xFFFF0000) == 0xFEAD0000
        _need(flip or not (w >> 29 in (0, 1, 2) or (w & 3) == 2 or w == RING_RET), "reads",
              "the word decodes as a method or a branch, which has its own event")
        expected_get = get + 4
    else:
        _need(len(words) == 1, "reads", "a branch is one word")
        w = words[0]
        kind = rec["kind"]
        _need(kind in ("jump", "call", "ret"), "kind", "must be jump, call or ret")
        tgt = _addr(rec, "target")
        if kind == "jump":
            _need(w >> 29 == 1 and tgt == (w & 0x1FFFFFFC), "target", "does not match the JUMP word")
            _need(io.ea(tgt) is not None, "target", f"0x{tgt:08x} has no mapping")
            expected_get = tgt
        elif kind == "call":
            _need(w >> 29 != 1 and (w & 3) == 2 and tgt == (w & 0x1FFFFFFC), "target", "does not match the CALL word")
            _need(io.ea(tgt) is not None, "target", f"0x{tgt:08x} has no mapping")
            _need(call_slot == 0, "kind", "CALL while a CALL is outstanding: the RSX has one return slot")
            call_slot = get + 4
            expected_get = tgt
        else:
            _need(w == RING_RET, "target", "does not match the RET word")
            _need(call_slot != 0, "kind", "RET with no CALL outstanding")
            _need(tgt == call_slot, "target", "is not the CALL's return address")
            expected_get = call_slot
            call_slot = 0
    return last, expected_get, call_slot


def _frame(rec: dict, running_hash: str, last_flip: int, draws: dict, cap: RsxCapture) -> int:
    f = rec["frame"]
    _need(isinstance(f, dict) and set(f) == {"flip_id", "guest_frame", "display_buffer", "present_seq"}, "frame",
          "must be {flip_id, guest_frame, display_buffer, present_seq}")
    _need(_is_int(f["flip_id"]) and f["flip_id"] > last_flip, "frame.flip_id",
          f"must be an integer > {last_flip}: a flip id appears once, in order")
    _need(f["guest_frame"] is None or (_is_int(f["guest_frame"]) and f["guest_frame"] >= 0), "frame.guest_frame",
          "must be an integer >= 0, or null when the head had no queued flip")
    _need(_is_int(f["display_buffer"]) and 0 <= f["display_buffer"] <= 7, "frame.display_buffer", "must be 0..7")
    _need(_is_int(f["present_seq"]) and f["present_seq"] >= 0, "frame.present_seq", "must be an integer >= 0")
    ch = _hash_obj(rec, "capture_hash", "input-events/v1")
    _need(ch == running_hash, "capture_hash", "does not match the SHA-256 of the input events before this frame")
    sh = _hash_obj(rec, "state_hash", "frame-state/v1")

    rb = rec["readback"]
    fh = rec["frame_hash"]
    if rb is None:
        _need(fh is None, "frame_hash", "a frame with no readback has no frame hash")
        fhv = None
    else:
        _need(isinstance(rb, dict) and set(rb) == {"format", "channel_order", "row_pitch", "alpha", "colorspace",
                                                   "clamp", "width", "height"}, "readback",
              "must be {format, channel_order, row_pitch, alpha, colorspace, clamp, width, height}")
        _need(rb["format"] in READBACK_BPP, "readback.format",
              f"{_show(rb['format'])} is not a readback format the contract defines")
        _need(rb["channel_order"] == "rgba", "readback.channel_order", "must be 'rgba'")
        _need(rb["alpha"] == "kept", "readback.alpha", "must be 'kept'")
        _need(rb["colorspace"] in ("linear", "srgb"), "readback.colorspace", "must be 'linear' or 'srgb'")
        _need(rb["clamp"] == "none", "readback.clamp", "must be 'none'")
        _need(_is_int(rb["width"]) and rb["width"] > 0 and _is_int(rb["height"]) and rb["height"] > 0,
              "readback.width", "must be positive integers")
        _need(rb["row_pitch"] == rb["width"] * READBACK_BPP[rb["format"]], "readback.row_pitch",
              "is not width * bytes per pixel")
        fhv = _hash_obj(rec, "frame_hash", "raw-readback/v1")

    d = rec["draws"]
    _need(isinstance(d, dict) and set(d) == {"submitted", "executed", "dropped", "fixed_function"}, "draws",
          "must be {submitted, executed, dropped, fixed_function}")
    _need(isinstance(d["dropped"], dict) and all(k in DROP_REASONS and _is_int(v) and v > 0
                                                 for k, v in d["dropped"].items()), "draws.dropped",
          "maps a drop reason name to a positive count")
    total = sum(d["dropped"].values())
    _need(_is_int(d["submitted"]) and _is_int(d["executed"]) and d["submitted"] == d["executed"] + total,
          "draws", f"submitted {d['submitted']} != executed {d['executed']} + dropped {total}")
    _need(_is_int(d["fixed_function"]) and 0 <= d["fixed_function"] <= d["executed"], "draws.fixed_function",
          "counts executed draws")
    _need(d["submitted"] == draws["submitted"] and d["executed"] == draws["executed"] and
          d["dropped"] == draws["dropped"] and d["fixed_function"] == draws["fixed"], "draws",
          "do not match the draw events since the previous frame")
    for k in ("unknown_methods", "resyncs"):
        _need(_is_int(rec[k]) and rec[k] >= 0, k, "must be an integer >= 0")
    # A method no object class handles is input the replay cannot account for, so the frame is not a
    # baseline for anything: an error here, in rsx-compare (which validates both sides first) and in a
    # replay, never merely an `invalid` frame that a comparison could skip past. The capture still records
    # the count and the reason (M3-SCOPE 3f); this is the reader's refusal.
    _need(rec["unknown_methods"] == 0, "unknown_methods",
          f"the frame consumed {rec['unknown_methods']} method(s) that no object class handles: not a baseline "
          "until each is classified as handled, or allow-listed as inert with evidence")
    _need(isinstance(rec["valid"], bool), "valid", "must be a boolean")
    inv = rec["invalid"]
    _need(isinstance(inv, list) and inv == sorted(set(inv)) and all(isinstance(x, str) and x for x in inv), "invalid",
          "must be a sorted list of distinct reason names")
    _need(rec["valid"] == (not inv), "valid", "is true exactly when `invalid` is empty")
    must = set()
    if total:
        must.add("dropped-draws")
    if d["fixed_function"]:
        must.add("fixed-function")
    if rec["resyncs"]:
        must.add("fifo-resync")
    if f["guest_frame"] is None:
        must.add("no-queued-flip")
    if rb is None:
        _need(bool({"readback-unavailable", "readback-incomplete"} & set(inv)), "invalid",
              "a frame with no readback names why")
    _need(must <= set(inv), "invalid", f"lacks {sorted(must - set(inv))}, which the counts above require")
    cap.frames.append({"flip_id": f["flip_id"], "guest_frame": f["guest_frame"],
                       "display_buffer": f["display_buffer"], "present_seq": f["present_seq"],
                       "capture_hash": ch, "state_hash": sh, "frame_hash": fhv,
                       "readback": rb, "valid": rec["valid"], "invalid": list(inv)})
    return f["flip_id"]


# --------------------------------------------------------------------------- compare


def compare_files(a_path: str, b_path: str, flip: int | None = None) -> tuple[dict, int]:
    """Compare two captures frame by frame (matched by flip_id).

    Returns (report, exit status): 0 identical with at least one frame compared, 1 a frame differs or is
    invalid, 3 no frame was compared (an error: a comparison that proves nothing is never a pass), and
    EXIT_NOT_COMPARABLE when the environment records differ (neither a pass nor a failure).
    """
    a = validate_rsx(a_path)
    b = validate_rsx(b_path)
    report: dict = {"schema": "ps3diff.rsx.report/1", "a": str(a_path), "b": str(b_path), "status": "",
                    "compared": 0, "frames": [], "first": None}
    diff_env = {k: [a.machine.get(k), b.machine.get(k)] for k in MACHINE_FIELDS if a.machine.get(k) != b.machine.get(k)}
    if diff_env:
        report["status"] = "not-comparable"
        report["environment"] = diff_env
        return report, EXIT_NOT_COMPARABLE
    by_b = {f["flip_id"]: f for f in b.frames}
    for fa in a.frames:
        if flip is not None and fa["flip_id"] != flip:
            continue
        fb = by_b.get(fa["flip_id"])
        if fb is None:
            report["frames"].append({"flip_id": fa["flip_id"], "result": "missing-in-b"})
            _first(report, fa["flip_id"], "frame", "present", "absent")
            continue
        report["compared"] += 1
        row = {"flip_id": fa["flip_id"], "result": "equal"}
        # Identity first, then the hashes in the order a divergence is looked for: the inputs, the
        # register state, then the pixels.
        checks = [("guest_frame", fa["guest_frame"], fb["guest_frame"]),
                  ("display_buffer", fa["display_buffer"], fb["display_buffer"]),
                  ("capture_hash", fa["capture_hash"], fb["capture_hash"]),
                  ("state_hash", fa["state_hash"], fb["state_hash"]),
                  ("readback", fa["readback"], fb["readback"]),
                  ("frame_hash", fa["frame_hash"], fb["frame_hash"])]
        for name, va, vb in checks:
            if va != vb:
                row["result"] = "differs"
                row["field"] = name
                row["a"], row["b"] = va, vb
                _first(report, fa["flip_id"], name, va, vb)
                break
        for side, fr in (("a", fa), ("b", fb)):
            if not fr["valid"]:
                if row["result"] == "equal":
                    row["result"] = "invalid"
                row["invalid_" + side] = fr["invalid"]
                _first(report, fa["flip_id"], "invalid-" + side, fr["invalid"], None)
        report["frames"].append(row)
    if flip is not None and not any(f["flip_id"] == flip for f in a.frames):
        report["status"] = "error"
        report["error"] = f"flip {flip} is not a frame of {a_path}"
        return report, 3
    if report["compared"] == 0:
        report["status"] = "error"
        report["error"] = "zero frames were compared: a comparison that proves nothing is never a pass"
        return report, 3
    bad = report["first"] is not None
    report["status"] = "differ" if bad else "equal"
    return report, 1 if bad else 0


def _first(report: dict, flip_id: int, field: str, a, b) -> None:
    if report["first"] is None:
        report["first"] = {"flip_id": flip_id, "field": field, "a": a, "b": b}


def report_text(report: dict) -> str:
    lines = [f"rsx-compare {report['a']} vs {report['b']}: {report['status']}"]
    if report["status"] == "not-comparable":
        lines.append("  the environment records differ, so byte equality proves nothing either way:")
        for k, (x, y) in sorted(report["environment"].items()):
            lines.append(f"    {k}: {x!r} vs {y!r}")
    elif report["status"] == "error":
        lines.append(f"  error: {report['error']}")
    else:
        lines.append(f"  frames compared: {report['compared']}")
        if report["first"]:
            f = report["first"]
            lines.append(f"  first divergence: flip {f['flip_id']}: {f['field']}: {f['a']!r} vs {f['b']!r}")
    return "\n".join(lines)
