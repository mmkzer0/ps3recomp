#!/usr/bin/env python3
"""Emit a tiny two-call leaf job and its liveness plan for the T2 pilot."""

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
import spu_liveness

BASE = 0x7C00
MAIN = BASE + 0x40
FUNCTION = BASE + 0x70


def ri16(op9, immediate, rt):
    return (op9 << 23) | ((immediate & 0xFFFF) << 7) | rt


def ri10(op8, immediate, ra, rt):
    return (op8 << 24) | ((immediate & 0x3FF) << 14) | (ra << 7) | rt


def rr(op11, rb=0, ra=0, rt=0):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt


def brsl(pc, target, rt=0):
    return ri16(0x066, (target - pc) // 4, rt)


def image_words():
    words = [0] * (0x88 // 4)
    main = [
        ri16(0x081, 7, 3),             # il $r3, 7
        ri16(0x081, 4, 4),             # il $r4, 4
        brsl(MAIN + 8, FUNCTION),
        brsl(MAIN + 12, FUNCTION),
        ri10(0x24, 0, 1, 5),           # stqd $r5, 0($r1)
        0,                             # stop 0
    ]
    function = [
        ri10(0x1C, -16, 1, 1),         # ai $r1, $r1, -16
        ri10(0x24, 0, 1, 7),           # save dead $r7 in the dead frame
        rr(0x0C0, 4, 3, 5),            # a $r5, $r3, $r4
        ri10(0x34, 0, 1, 7),           # restore $r7
        ri10(0x1C, 16, 1, 1),          # ai $r1, $r1, 16
        rr(0x1A8, ra=0),               # bi $r0
    ]
    main_offset = (MAIN - BASE) // 4
    function_offset = (FUNCTION - BASE) // 4
    words[main_offset:main_offset + len(main)] = main
    words[function_offset:function_offset + len(function)] = function
    return words


def tail_image(patch_member=False):
    words = [0] * (0x100 // 4)
    main = [
        (0x21 << 25) | ((BASE + 0xE0) << 7) | 2,
        ri10(0x34, 0, 2, 4),  # load a quadword of data
        (0x21 << 25) | ((FUNCTION if patch_member else BASE + 0xF0) << 7) | 2,
        ri10(0x24, 0, 2, 4),  # patch an unreachable member byte, or ordinary image data
        (0x21 << 25) | ((MAIN + 28) << 7),
        ri16(0x081, 3, 3),
        ri16(0x064, (FUNCTION - (MAIN + 24)) // 4, 0),
        (0x21 << 25) | ((MAIN + 40) << 7),
        ri16(0x081, 2, 3),
        ri16(0x064, (FUNCTION - (MAIN + 36)) // 4, 0),
        0,
    ]
    member = [
        ri10(0x1C, -1, 3, 3),
        ri16(0x042, -1, 3),
        rr(0x1A8, ra=0),
        rr(0x201),  # unreachable nop, still in the replaced member range
    ]
    words[0x40 // 4:0x40 // 4 + len(main)] = main
    words[0x70 // 4:0x80 // 4] = member
    words[0xE0 // 4:0xF0 // 4] = [*member[:-1], member[-1] ^ (1 if patch_member else 0)]
    return b"".join(word.to_bytes(4, "big") for word in words)


def main():
    out = Path(sys.argv[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    words = image_words()
    image = b"".join(word.to_bytes(4, "big") for word in words)
    image_path = out / "fn_override_job.bin"
    image_path.write_bytes(image)
    bounds = [
        {"start": MAIN, "end": FUNCTION},
        {"start": FUNCTION, "end": BASE + len(image)},
    ]
    functions_path = out / "functions.json"
    functions_path.write_text(json.dumps(bounds) + "\n", encoding="utf-8")
    plan = spu_liveness.analyze(image, BASE, BASE + len(image), (),
                                [(item["start"], item["end"]) for item in bounds])
    plan_path = out / "liveness.json"
    plan_path.write_text(json.dumps(plan, indent=1) + "\n", encoding="utf-8")
    for name, patch in (("tail_unit", False), ("tail_code_unit", True)):
        image = tail_image(patch)
        (out / f"{name}.bin").write_bytes(image)
        tail_bounds = [{"start": MAIN, "end": FUNCTION}, {"start": FUNCTION, "end": BASE + 0x80}]
        (out / f"{name}_functions.json").write_text(json.dumps(tail_bounds) + "\n", encoding="utf-8")
        plan = spu_liveness.analyze(
            image, BASE, functions=[(MAIN, FUNCTION), (FUNCTION, BASE + 0x80)],
            capture_units=(FUNCTION,))
        (out / f"{name}.json").write_text(json.dumps(plan, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {image_path}, {functions_path}, {plan_path}")


if __name__ == "__main__":
    main()
