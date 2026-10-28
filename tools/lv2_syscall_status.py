#!/usr/bin/env python3
"""Audit the toolkit against the stock LV2 4.93 syscall table and generate the
default-answer bitmap for the firmware's own ENOSYS slots.

Input: the FW 4.93 stock lv2_kernel status map (slot -> enosys/handler, from
the official PUP). The map is NOT committed: it holds kernel addresses. Only
the resulting slot status leaves this tool. `--dump-map` optionally adds the
LV2 dump FW 4.81 REBUG/Cobra map as a corroboration column; it never selects a
verdict.

Outputs (relative to the repository root unless overridden):
  docs/LV2-SYSCALL-STATUS.md              one row per slot 0..1023
  runtime/syscalls/lv2_enosys_bitmap.h    1024-bit lookup, generated values
  runtime/syscalls/lv2_enosys_bitmap.c

The other two inputs are read from the repository itself:
  - the toolkit: every `lv2_syscall_register(...)` call under runtime/ and
    libs/ (slot expressions resolved through the `#define SYS_* N` headers),
    plus the inline `case N:` labels of ppu_loader.cpp's syscall switch;
  - RPCS3 at pin d00d80231af3f9b0286f38fd72f7513583f1a9d4,
    rpcs3/Emu/Cell/lv2/lv2.cpp. Its 1024-entry g_ppu_syscall_table is embedded
    below as one character per slot ('b' = BIND_SYSC, 'n' = null_func /
    NULL_FUNC / uns_func) so this tool and its test run offline. uns_func also
    returns CELL_ENOSYS; 'none' would be a slot with no table entry, and the
    pinned table has all 1024.

Verdicts, in order:
  conflict-4.93-vs-rpcs3     stock enosys, RPCS3 binds: keep the stop
  toolkit-handler-on-hw-enosys  stock enosys, toolkit implements: suspicious
  hw-enosys-unregistered     stock enosys, nobody implements: answer ENOSYS
  hw-real-unregistered       stock handler, toolkit does not: still a stop
  ok                         stock handler, toolkit implements

The stock kernel is the primary evidence: 632 real handlers, 392 slots on the
shared ENOSYS stub, which returns CELL_ENOSYS (0x80010003). The 4.81
REBUG/Cobra dump agrees on the other 1018 slots; it has handlers at 6, 7, 8, 9,
10 and 15 (CFW additions such as peek/poke), ENOSYS on stock. RPCS3 binding a
handler where stock says ENOSYS is a 4.93-vs-RPCS3 disagreement and keeps the
stop until evidence says otherwise.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SLOTS = 1024
RPCS3_PIN = "d00d80231af3f9b0286f38fd72f7513583f1a9d4"

# g_ppu_syscall_table at the pin, positionally, one char per slot. 'b' is
# BIND_SYSC(...); 'n' is null_func, NULL_FUNC(...) or uns_func (all of which
# implement no syscall; uns_func returns CELL_ENOSYS). 451 binds, 573 nulls.
RPCS3_TABLE = (
    "nbbbbbnnnnnnbbbnnnbbnnbbnbbbnbbnnnnnnnnnnbnbbbbbbbbbbbnnbbbnbbbb"
    "bbbbbbbbbbbbbnnnnbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    "bbbbbbbbbbbbbbbbbbbbbnbbbbbbbbbbbbnbnbbnnbbbbbbbbbbbbbbnbbbbbnbb"
    "bbbnbbbbnnnnnnnnnnnnnbnnnnnnnnnnnnnnnnbbbbbbbbbbbnnnnnnnnnbbbbbn"
    "nnnnbnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnbbbbbbbbbbbbbbnnnnnn"
    "nnnnbbbbbbbbbnbbbbnbnbbbnbnnbbbbbbnnnnnnnnbnnnnbbbbnbbbbbbbbbnnn"
    "nnbnbnnnbnnnnnbnnnbbnnnnnnbbbnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn"
    "nnbbnnbnnnnnnbnbbbbbnnnnnnnnnnnnbbbbbbbbbbbnnnbbbbbbbnbbnnbnnnbn"
    "bbbnbbbbbbbbbbnnnnbbbbbbbbbbbbbbbbbbbbbbnnnnbnbbnnnnnnnnnnnnnnnn"
    "nnnbnnnnnnnnnnnnnnnnnnnnbbbbbbbbbbbbbbbbbbbbnbbbbbbbnnbbnnnbnnnn"
    "nnnnnnnnnnbbbbbbbbbbnnnnnnbbbbbbbbbbbbnnnnnnnnnnnnnnnnnnnnnbbbbb"
    "bbbbbbbbbbbbbbbbbbbbbbbnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn"
    "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    "bbbbbbbnbbbnnbbbnnnnnnnnnnnnnnbbnbbbbnbbbbbbnnnnnnnnnnnnnnnnnnnn"
    "nnnnnnnnbbnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn"
    "nnnnnnnnnnnnnnnnbbnnnnnnbnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn"
)

# The toolkit scan's roots, relative to ROOT. Test fixtures under them are
# excluded: a fixture that registers a synthetic slot is not toolkit support.
TOOLKIT_SOURCE_DIRS = ("runtime", "libs")
LOADER_SOURCE = "runtime/ppu/ppu_loader.cpp"

VERDICTS = (
    "ok",
    "toolkit-handler-on-hw-enosys",
    "hw-enosys-unregistered",
    "hw-real-unregistered",
    "conflict-4.93-vs-rpcs3",
)

# The stock table's differences from the 4.81 REBUG/Cobra dump are the CFW's
# own additions; name them so the generated document explains the column.
CFW_ONLY = {6, 7, 8, 9, 10, 15}


def parse_stock_map(path: Path) -> set[int]:
    """Slots whose FW 4.93 stock table names a real handler. The map must
    cover all 1024 slots: a missing row would silently classify a slot wrong."""
    status: dict[int, str] = {}
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        m = re.match(r"\|\s*(\d+)\s*\|\s*(handler|enosys)\s*\|", line, re.I)
        if not m:
            continue
        slot, value = int(m.group(1)), m.group(2).lower()
        if not 0 <= slot < SLOTS:
            raise ValueError(f"{path}:{lineno}: slot {slot} outside 0..{SLOTS - 1}")
        if slot in status:
            raise ValueError(f"{path}:{lineno}: duplicate slot {slot}")
        status[slot] = value
    missing = [s for s in range(SLOTS) if s not in status]
    if missing:
        raise ValueError(f"{path}: {len(missing)} stock slots missing "
                         f"(first: {missing[:8]})")
    return {s for s, value in status.items() if value == "handler"}


def parse_dump_map(path: Path) -> set[int]:
    """Slots with a real handler in the 4.81 dump. A slot absent from the
    dump's table points at the shared ENOSYS stub, so absence is 'enosys'."""
    slots: set[int] = set()
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        m = re.match(r"\|\s*(\d+)\s*\|\s*(0x[0-9A-Fa-f]+)\s*\|", line)
        if not m:
            continue
        slot = int(m.group(1))
        if not 0 <= slot < SLOTS:
            raise ValueError(f"{path}:{lineno}: slot {slot} outside 0..{SLOTS - 1}")
        if slot in slots:
            raise ValueError(f"{path}:{lineno}: duplicate slot {slot}")
        slots.add(slot)
    if not slots:
        raise ValueError(f"{path}: no syscall table rows found")
    return slots


def strip_c_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def parse_defines() -> dict[str, int]:
    defines: dict[str, int] = {}
    for base in (ROOT / "include", ROOT / "runtime", ROOT / "libs"):
        for path in base.rglob("*.h"):
            for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
                m = re.match(r"\s*#\s*define\s+(SYS_[A-Za-z0-9_]+)\s+"
                             r"(0x[0-9A-Fa-f]+|\d+)\b", line)
                if not m:
                    continue
                name, value = m.group(1), int(m.group(2), 0)
                if name in defines and defines[name] != value:
                    raise ValueError(f"{path}: #define {name} disagrees with "
                                     f"{defines[name]} (now {value})")
                defines[name] = value
    return defines


def parse_toolkit(defines: dict[str, int]) -> dict[int, list[tuple[str, str]]]:
    """slot -> [(handler name, "path:line")], in source order."""
    regs: dict[int, list[tuple[str, str]]] = {}
    call = re.compile(r"lv2_syscall_register\s*\(\s*tbl\s*,\s*([^,]+?)\s*,\s*"
                      r"([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*;", re.S)
    for d in TOOLKIT_SOURCE_DIRS:
        for path in sorted((ROOT / d).rglob("*")):
            if path.suffix not in (".c", ".cpp") or "/tests/" in path.as_posix():
                continue
            src = strip_c_comments(path.read_text(encoding="utf-8", errors="ignore"))
            for m in call.finditer(src):
                expr, handler = m.group(1).strip(), m.group(2)
                if expr.startswith("SYS_"):
                    if expr not in defines:
                        raise ValueError(f"{path}: unresolved registration {expr}")
                    slot = defines[expr]
                else:
                    try:
                        slot = int(expr, 0)
                    except ValueError:
                        raise ValueError(f"{path}: registration slot {expr!r} "
                                         "is neither a number nor a resolved SYS_*")
                if not 0 <= slot < SLOTS:
                    raise ValueError(f"{path}: registration slot {slot} outside range")
                rel = path.relative_to(ROOT).as_posix()
                line = src[:m.start()].count("\n") + 1
                regs.setdefault(slot, []).append((handler, f"{rel}:{line}"))
    return regs


def parse_loader_cases() -> set[int]:
    """The inline `case N:` labels of ppu_loader.cpp's `switch (num)` block."""
    path = ROOT / LOADER_SOURCE
    if not path.exists():
        return set()
    src = strip_c_comments(path.read_text(encoding="utf-8", errors="ignore"))
    at = src.find("switch (num)")
    if at < 0:
        raise ValueError(f"{path}: no `switch (num)` found")
    start = src.find("{", at)
    depth = 0
    end = -1
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end < 0:
        raise ValueError(f"{path}: unterminated `switch (num)`")
    return {int(n, 0) for n in
            re.findall(r"\bcase\s+(0x[0-9A-Fa-f]+|\d+)\s*:", src[start:end])}


def classify(hw: set[int], regs: dict[int, list[tuple[str, str]]],
             loader: set[int], rpcs3: str) -> list[str]:
    out = []
    for slot in range(SLOTS):
        has_hw = slot in hw
        has_tk = slot in regs or slot in loader
        if not has_hw and rpcs3[slot] == "b":
            out.append("conflict-4.93-vs-rpcs3")
        elif not has_hw and has_tk:
            out.append("toolkit-handler-on-hw-enosys")
        elif not has_hw:
            out.append("hw-enosys-unregistered")
        elif not has_tk:
            out.append("hw-real-unregistered")
        else:
            out.append("ok")
    return out


def toolkit_kind(slot: int, regs: dict[int, list[tuple[str, str]]],
                 loader: set[int]) -> str:
    if slot in loader:
        return "loader-case"
    if slot in regs:
        return "handler"
    return "none"


def render_doc(hw: set[int], dump: set[int] | None,
               regs: dict[int, list[tuple[str, str]]],
               loader: set[int], verdicts: list[str]) -> str:
    counts = {v: verdicts.count(v) for v in VERDICTS}
    lines = [
        "# LV2 syscall status, slots 0..1023",
        "",
        "Generated by `tools/lv2_syscall_status.py` from the **FW 4.93 stock "
        "lv2_kernel (PUP)** syscall status map. The map is not committed: it "
        "holds kernel addresses. Do not edit this file by hand; rerun the "
        "generator after adding a handler or a registration.",
        "",
        "The stock kernel is the primary hardware evidence. The **LV2 dump "
        "FW 4.81 REBUG/Cobra** table is corroboration only: it agrees on the "
        "other 1018 slots, and its 632 real handlers have the same code "
        "addresses. It has handlers at slots 6, 7, 8, 9, 10 and 15 (CFW "
        "additions such as peek/poke); on stock those slots are ENOSYS. The "
        "`dump 4.81` column shows the corroboration when the generator runs "
        "with `--dump-map`, and `-` otherwise; it never selects a verdict.",
        "",
        "## Columns",
        "",
        "- `hw 4.93` — `handler` when the stock map names a real handler for "
        "the slot; `enosys` when it points at the shared ENOSYS stub, which "
        "returns `0x80010003` (`CELL_ENOSYS`).",
        "- `dump 4.81` — the same question for the LV2 dump FW 4.81 "
        "REBUG/Cobra table, corroboration only (see above).",
        "- `rpcs3` — `bind` = `BIND_SYSC(...)` in `rpcs3/Emu/Cell/lv2/lv2.cpp` "
        f"at pin `{RPCS3_PIN}`; `null` = `null_func`, `NULL_FUNC(...)` or "
        "`uns_func` (the last also returns `CELL_ENOSYS`); `none` = no table "
        "entry (the pinned table has all 1024).",
        "- `toolkit` — `loader-case` when `ppu_loader.cpp`'s syscall `switch` "
        "answers the slot inline (it runs before the table); `handler` when "
        "only an `lv2_syscall_register(...)` call under `runtime/` or `libs/` "
        "implements it; `none` = neither.",
        "",
        "## Verdicts",
        "",
        "| verdict | meaning |",
        "|---|---|",
        "| `ok` | the stock firmware has a handler and the toolkit implements "
        "the slot. |",
        "| `toolkit-handler-on-hw-enosys` | the stock firmware's slot is the "
        "ENOSYS stub, yet the toolkit implements it — the same shape as the "
        "old 988 alias. The handler is not changed here; it needs evidence. |",
        "| `hw-enosys-unregistered` | stock ENOSYS, nobody implements it, "
        "RPCS3 binds none. The default syscall path answers `CELL_ENOSYS` per "
        "call from the generated `runtime/syscalls/lv2_enosys_bitmap.c`. |",
        "| `hw-real-unregistered` | the stock firmware has a handler and the "
        "toolkit does not. Still stops with `unimplemented-syscall`. |",
        "| `conflict-4.93-vs-rpcs3` | stock ENOSYS but RPCS3 binds a real "
        "handler. The stop stays: the firmware wins until evidence says "
        "otherwise. |",
        "",
        "## Counts",
        "",
        f"The stock map lists {len(hw)} slots with a real handler; "
        f"{SLOTS - len(hw)} point at the shared ENOSYS stub.",
        "",
        "| verdict | slots |",
        "|---|---:|",
    ]
    lines += [f"| `{v}` | {counts[v]} |" for v in VERDICTS]
    lines += [f"| **total** | **{SLOTS}** |", ""]

    if dump is not None:
        only_dump = sorted(dump - hw)
        only_stock = sorted(hw - dump)
        if not only_dump and not only_stock:
            text = "The 4.81 dump agrees with stock on every slot."
        else:
            parts = []
            if only_dump:
                parts.append("handler only in the dump: "
                             + ", ".join(str(s) for s in only_dump))
            if only_stock:
                parts.append("handler only on stock: "
                             + ", ".join(str(s) for s in only_stock))
            text = "The 4.81 dump differs from stock. " + "; ".join(parts) + "."
            if set(only_dump) == CFW_ONLY and not only_stock:
                text += (" Those six are CFW additions such as peek/poke; stock "
                         "leaves them on the ENOSYS stub.")
        lines += ["## 4.81 dump corroboration", "", text, ""]

    suspicious = [s for s in range(SLOTS)
                  if verdicts[s] == "toolkit-handler-on-hw-enosys"]
    lines += ["## `toolkit-handler-on-hw-enosys`", ""]
    if suspicious:
        lines += ["| slot | handler | location |", "|---:|---|---|"]
        for s in suspicious:
            for handler, loc in regs.get(s, []):
                lines.append(f"| {s} | `{handler}` | `{loc}` |")
            if s in loader:
                lines.append(f"| {s} | inline syscall `case` | `{LOADER_SOURCE}` |")
    else:
        lines.append("None.")
    lines.append("")

    conflicts = [s for s in range(SLOTS)
                 if verdicts[s] == "conflict-4.93-vs-rpcs3"]
    lines += ["## `conflict-4.93-vs-rpcs3`", ""]
    lines.append(", ".join(f"`{s}`" for s in conflicts) if conflicts else "None.")
    lines.append("")

    lines += [
        "## Table",
        "",
        "| slot | hw 4.93 | dump 4.81 | rpcs3 | toolkit | verdict |",
        "|---:|---|---|---|---|---|",
    ]
    for s in range(SLOTS):
        dump_cell = "-" if dump is None else ("handler" if s in dump else "enosys")
        lines.append(f"| {s} | {'handler' if s in hw else 'enosys'} | "
                     f"{dump_cell} | "
                     f"{'bind' if RPCS3_TABLE[s] == 'b' else 'null'} | "
                     f"{toolkit_kind(s, regs, loader)} | {verdicts[s]} |")
    lines.append("")
    return "\n".join(lines)


def bitmap_words(slots: set[int]) -> list[int]:
    words = [0] * (SLOTS // 64)
    for s in slots:
        words[s >> 6] |= 1 << (s & 63)
    return words


def render_header() -> str:
    return """\
/* ps3recomp - default answers for the firmware's own LV2 ENOSYS slots
 *
 * Generated by tools/lv2_syscall_status.py from the FW 4.93 stock
 * lv2_kernel (PUP) status map. Do not edit by hand; rerun the generator.
 */
#ifndef LV2_ENOSYS_BITMAP_H
#define LV2_ENOSYS_BITMAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the FW 4.93 stock lv2_kernel leaves slot `num` on the shared ENOSYS
 * stub, the toolkit implements no handler for it, and RPCS3 at pin
 * d00d80231af3f9b0286f38fd72f7513583f1a9d4 binds none either. The kernel's
 * stub returns CELL_ENOSYS (0x80010003), so the default syscall path answers
 * the same per call instead of stopping. */
int lv2_enosys_default_answer(uint32_t num);

#ifdef __cplusplus
}
#endif

#endif /* LV2_ENOSYS_BITMAP_H */
"""


def render_source(slots: set[int]) -> str:
    words = bitmap_words(slots)
    body = ",\n".join("    0x%016Xull" % w for w in words)
    return f"""\
/* ps3recomp - default answers for the firmware's own LV2 ENOSYS slots
 *
 * Generated by tools/lv2_syscall_status.py. Do not edit by hand.
 * Tier: FW 4.93 stock lv2_kernel (from the official PUP), primary hardware
 * source. The LV2 dump FW 4.81 REBUG/Cobra table is corroboration only; it
 * does not select any bit here.
 * RPCS3 at pin {RPCS3_PIN} is consulted only to exclude
 * slots it binds; those are a 4.93-vs-RPCS3 disagreement and keep their stop.
 *
 * The kernel's shared ENOSYS stub returns CELL_ENOSYS (0x80010003). Answering
 * the same per call is reproducing that stub, not a compatibility choice for
 * one title. {len(slots)} slots are set. See docs/LV2-SYSCALL-STATUS.md.
 */
#include "lv2_enosys_bitmap.h"

static const uint64_t k_lv2_enosys[{SLOTS // 64}] = {{
{body}
}};

_Static_assert(sizeof k_lv2_enosys / sizeof k_lv2_enosys[0] == {SLOTS // 64},
               "LV2 ENOSYS bitmap must cover 1024 slots");

int lv2_enosys_default_answer(uint32_t num)
{{
    if (num >= {SLOTS}u)
        return 0;
    return (int)((k_lv2_enosys[num >> 6] >> (num & 63)) & 1u);
}}
"""


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("map", type=Path,
                    help="FW 4.93 stock lv2_kernel status map "
                         "(Markdown: | slot | enosys/handler |)")
    ap.add_argument("--dump-map", type=Path,
                    help="optional LV2 dump FW 4.81 REBUG/Cobra map, "
                         "corroboration column only")
    ap.add_argument("--doc", type=Path,
                    default=ROOT / "docs/LV2-SYSCALL-STATUS.md")
    ap.add_argument("--c", type=Path,
                    default=ROOT / "runtime/syscalls/lv2_enosys_bitmap.c")
    ap.add_argument("--h", type=Path,
                    default=ROOT / "runtime/syscalls/lv2_enosys_bitmap.h")
    args = ap.parse_args(argv)

    if len(RPCS3_TABLE) != SLOTS or set(RPCS3_TABLE) - {"b", "n"}:
        raise ValueError(f"embedded RPCS3 table is malformed: "
                         f"{len(RPCS3_TABLE)} entries")
    hw = parse_stock_map(args.map)
    dump = parse_dump_map(args.dump_map) if args.dump_map else None
    defs = parse_defines()
    regs = parse_toolkit(defs)
    loader = parse_loader_cases()
    verdicts = classify(hw, regs, loader, RPCS3_TABLE)
    enosys_slots = {s for s in range(SLOTS)
                    if verdicts[s] == "hw-enosys-unregistered"}

    args.doc.parent.mkdir(parents=True, exist_ok=True)
    args.c.parent.mkdir(parents=True, exist_ok=True)
    args.h.parent.mkdir(parents=True, exist_ok=True)
    args.doc.write_text(render_doc(hw, dump, regs, loader, verdicts), encoding="utf-8")
    args.h.write_text(render_header(), encoding="utf-8")
    args.c.write_text(render_source(enosys_slots), encoding="utf-8")

    counts = {v: verdicts.count(v) for v in VERDICTS}
    print(f"map {args.map}: {len(hw)} handlers, {SLOTS - len(hw)} enosys")
    if dump is not None:
        print(f"dump {args.dump_map}: {len(dump)} handlers, "
              f"differs at {sorted(dump ^ hw)}")
    print("verdicts: " + ", ".join(f"{v}={counts[v]}" for v in VERDICTS))
    print(f"ENOSYS default bitmap: {len(enosys_slots)} slots")
    print(f"wrote {args.doc}")
    print(f"wrote {args.h}")
    print(f"wrote {args.c}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
