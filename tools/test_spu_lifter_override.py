#!/usr/bin/env python3
"""Native SPU overrides substitute references but retain their lifted bodies."""

import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
LIFTER = TOOLS / "spu_lifter.py"


def ri16(op9, immediate, rt):
    return (op9 << 23) | ((immediate & 0xFFFF) << 7) | rt
def ri10(op8, immediate, ra, rt):
    return (op8 << 24) | ((immediate & 0x3FF) << 14) | (ra << 7) | rt




def rr(op11, rb=0, ra=0, rt=0):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt


def brsl(pc, target, rt=0):
    return ri16(0x066, (target - pc) // 4, rt)


def image_with_refs():
    words = [0] * (0x58 // 4)
    words[0] = brsl(0x00, 0x40)
    words[1] = rr(0x1A8, ra=0)
    words[4] = ri16(0x042, (0x40 - 0x10) // 4, 3)  # brnz $r3, F
    words[5] = rr(0x1A8, ra=0)
    words[8] = ri16(0x064, (0x40 - 0x20) // 4, 0)  # tail branch to F
    words[16] = rr(0x0C0, 4, 3, 5)
    words[17] = rr(0x1A8, ra=0)
    words[20] = rr(0x0C0, 4, 3, 5)
    words[21] = rr(0x1A8, ra=0)
    return b"".join(struct.pack(">I", word) for word in words)


def run_lifter(image, bounds, overrides, out, extra_args=()):
    out.mkdir(parents=True, exist_ok=True)
    image_path = out.parent / "input.bin"
    functions_path = out.parent / "functions.json"
    image_path.write_bytes(image)
    functions_path.write_text(json.dumps([{"start": a, "end": b} for a, b in bounds]))
    command = [sys.executable, str(LIFTER), str(image_path), "--base", "0",
               "--functions", str(functions_path), "--symbol-prefix", "xfn_",
               "--report", str(out / "lift.json"), "--output", str(out)]
    command += list(extra_args)
    for spec in overrides:
        command += ["--override", spec]
    return subprocess.run(command, capture_output=True, text=True, check=False)


BISL_DRIVER = r"""
#include "spu_helpers.h"
#include "spu_ls_watch_fixture.h"
#include <assert.h>
#include <stdlib.h>
static unsigned calls, drains, restores;
static uint32_t expected_target;
static spu_context context;
void spu_indirect_branch(spu_context* ctx) {
    assert(ctx->pc == expected_target);
    assert(ctx->host_depth == 3);
    assert(ctx->gpr[%(link)d]._u32[0] == %(return_pc)d);
    calls++;
    ctx->image_id = 9;
    ctx->gpr[4] = spu_il(0x5A); /* Code at the dispatched target executed. */
    if (%(delta)d) ctx->reloc_delta = 0x600;
}
void spu_drain_call(spu_context* ctx, uint32_t return_pc) {
    assert(return_pc == %(return_pc)d);
    assert(ctx->host_depth == 3);
    drains++;
    ctx->pc = return_pc;
}
void spu_img_restore(spu_context* ctx, int32_t saved) {
    assert(saved == 7 && ctx->host_depth == 2);
    restores++;
    ctx->image_id = saved;
}
static void lifted(spu_context* ctx) {
    %(statement)s
    ctx->gpr[3] = spu_il(7);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    uint32_t target = (uint32_t)strtoul(argv[1], NULL, 0);
    expected_target = target & 0x3FFFCu;
    context.gpr[9]._u32[0] = target;
    context.image_id = 7;
    context.host_depth = 2;
    context.reloc_delta = %(delta)d;
    context.pc = %(start_pc)d;
    context.int_enable = !%(enabled)d;
    lifted(&context);
    unsigned expected_calls = !%(fallthrough)d || expected_target != %(return_pc)d;
    assert(calls == expected_calls && drains == expected_calls && restores == expected_calls);
    assert(context.gpr[%(link)d]._u32[0] == %(return_pc)d);
    for (int lane = 1; lane < 4; lane++) assert(context.gpr[%(link)d]._u32[lane] == 0);
    assert(context.host_depth == 2 && context.image_id == 7);
    assert(context.reloc_delta == %(delta)d && context.int_enable == %(enabled)d);
    assert(context.pc == %(return_pc)d);
    assert(context.gpr[3]._u32[0] == 7);
    assert(context.gpr[4]._u32[0] == (expected_calls ? 0x5A : 0));
    return 0;
}
"""


class NativeOverrides(unittest.TestCase):
    def test_every_reference_uses_override_and_reference_body_remains(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = root / "lift"
            result = run_lifter(image_with_refs(),
                                [(0, 8), (0x10, 0x18), (0x20, 0x24),
                                 (0x40, 0x48), (0x50, 0x58)],
                                ["0x40=native_f", "0x50=native_g"], out)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            header = (out / "spu_recomp.h").read_text()
            source = (out / "spu_recomp.c").read_text()
            self.assertIn("void native_f(spu_context* ctx);", header)
            self.assertIn("void native_g(spu_context* ctx);", header)
            self.assertIn("void xfn_spu_func_00000040(spu_context* ctx)", source)
            self.assertNotIn("xfn_spu_func_00000040(ctx);", source)
            self.assertIn("ctx->host_depth++; native_f(ctx);", source)
            self.assertGreaterEqual(source.count("SPU_DIRECT(ctx, 0x40, native_f)"), 2)
            self.assertIn("{ 0x00000040u, native_f, xfn_spu_func_00000040, 1,", source)
            self.assertIn("{ 0x00000050u, native_g, xfn_spu_func_00000050, 1,", source)
            self.assertIn("spu_register_function_with_reference", source)

    def test_nonleaf_call_is_refused_by_address_and_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            words = [0] * (0x68 // 4)
            words[0x40 // 4] = brsl(0x40, 0x60)
            words[0x44 // 4] = rr(0x1A8, ra=0)
            words[0x60 // 4] = rr(0x1A8, ra=0)
            image = b"".join(struct.pack(">I", word) for word in words)
            result = run_lifter(image, [(0x40, 0x48), (0x60, 0x64)],
                                ["0x40=native_f"], root / "lift")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("--override 0x40 refuses non-leaf function", result.stderr)
            self.assertRegex(result.stderr, r"call out via [a-z]+ at 0x40")

    def test_arithmetic_r0_target_is_dispatched_and_refused_for_override(self):
        words = [0] * (0x48 // 4)
        words[0x40 // 4] = rr(0x0C0, rb=4, ra=3, rt=0)  # a $r0,$r3,$r4
        words[0x44 // 4] = rr(0x1A8, ra=0)              # bi $r0
        image = b"".join(struct.pack(">I", word) for word in words)
        bounds = [(0x40, 0x48)]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = run_lifter(image, bounds, [], root / "reference")
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            source = (root / "reference" / "spu_recomp.c").read_text()
            self.assertIn("ctx->pc = ctx->gpr[0]._u32[0];", source)
            self.assertNotIn("SPU_RET(ctx)", source)

            result = run_lifter(image, bounds, ["0x40=native_f"], root / "override")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unproven $r0 write by a at 0x40", result.stderr)
    def test_stack_link_restore_requires_matching_abi_slot(self):
        invalid = (
            ("other-base", [
                ri10(0x34, 0, 3, 0),  # lqd $r0,0($r3)
                rr(0x1A8, ra=0),
            ]),
            ("mismatched-offset", [
                ri10(0x24, 1, 1, 0),  # stqd $r0,16($r1)
                ri10(0x1C, -32, 1, 1),
                ri10(0x1C, 32, 1, 1),
                ri10(0x34, 0, 1, 0),  # lqd $r0,0($r1)
                rr(0x1A8, ra=0),
            ]),
            ("save-not-dominating", [
                ri16(0x040, 3, 3),  # brz $r3, 0x4C
                ri10(0x24, 1, 1, 0),  # stqd $r0,16($r1)
                ri16(0x064, 2, 0),  # br 0x50
                ri16(0x064, 1, 0),  # br 0x50
                ri10(0x34, 1, 1, 0),  # lqd $r0,16($r1)
                rr(0x1A8, ra=0),
            ]),
            ("save-after-r0-write", [
                rr(0x0C0, rb=4, ra=3, rt=0),  # a $r0,$r3,$r4
                ri10(0x24, 1, 1, 0),  # stqd $r0,16($r1)
                ri10(0x34, 1, 1, 0),  # lqd $r0,16($r1)
                rr(0x1A8, ra=0),
            ]),
        )
        for name, body in invalid:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                words = [0] * (0x40 // 4) + body
                image = b"".join(struct.pack(">I", word) for word in words)
                bounds = [(0x40, 0x40 + len(body) * 4)]
                root = Path(tmp)
                result = run_lifter(image, bounds, [], root / "reference")
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                source = (root / "reference" / "spu_recomp.c").read_text()
                self.assertIn("ctx->pc = ctx->gpr[0]._u32[0];", source)
                self.assertNotIn("SPU_RET(ctx)", source)

                result = run_lifter(image, bounds, ["0x40=native_f"], root / "override")
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(result.stderr,
                                 r"unproven \$r0 write by lqd at 0x[0-9A-F]+")

    def test_pc_getter_preserves_the_frame_across_a_split(self):
        words = [rr(0x201)] * (0x58 // 4)
        words[0x40 // 4:] = [
            ri10(0x24, 1, 1, 0), ri10(0x1C, -32, 1, 1),
            brsl(0x48, 0x4C, 126),
            ri10(0x34, 3, 1, 0), ri10(0x1C, 32, 1, 1), rr(0x1A8, ra=0),
        ]
        image = b"".join(struct.pack(">I", word) for word in words)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "reference"
            result = run_lifter(image, [(0x40, 0x4C), (0x4C, 0x58)], [], output,
                                ["--return-entries"])
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            report = json.loads((output / "lift.json").read_text())
            [entry] = report["r0_returns"]
            self.assertEqual(entry["site"], 0x54)
            self.assertEqual(entry["assumes"], "spu-abi-link-slot")
            self.assertEqual(report["call_targets"], [])

    def test_indirect_pc_getter_keeps_a_guarded_call_fallback(self):
        words = [rr(0x201)] * (0x50 // 4)
        words[0x40 // 4:] = [
            ri16(0x081, 0x48, 9), rr(0x1A9, ra=9, rt=126),
            ri16(0x081, 0, 3), 0x1234,
        ]
        image = b"".join(struct.pack(">I", word) for word in words)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "reference"
            result = run_lifter(image, [(0x40, 0x50)], [], output, ["--return-entries"])
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            source = (output / "spu_recomp.c").read_text()
            self.assertIn("if (_tg != _ret)", source)
            self.assertIn("ctx->gpr[126] = spu_link(_ret)", source)
            self.assertIn("const uint32_t _ret = 0x48", source)
            report = json.loads((output / "lift.json").read_text())
            self.assertEqual(report["functions"], [[0x40, 0x50]])

    @unittest.skipUnless(shutil.which("cc"), "no C compiler")
    def test_bisl_guard_executes_both_paths_and_preserves_link_aliasing(self):
        from spu_disasm import spu_decode
        from spu_lifter import SPULifter

        root = TOOLS.parent
        with tempfile.TemporaryDirectory() as tmp:
            for pc, fallthrough in ((0x40, True), (0x40, False), (0x3FFFC, False)):
                for link in (9, 126):
                    for delta in ((0, 0x200) if fallthrough else (0,)):
                        for flag, enabled in ((0x40000, 1), (0x80000, 0)):
                            with self.subTest(pc=pc, fallthrough=fallthrough, link=link,
                                              delta=delta, enabled=enabled):
                                lifter = SPULifter()
                                lifter.reloc = bool(delta)
                                instructions = [
                                    spu_decode(rr(0x1A9, ra=9, rt=link) | flag, pc)]
                                if fallthrough:
                                    instructions.append(spu_decode(rr(0x201), pc + 4))
                                statement = lifter.lift_function(
                                    instructions, pc, pc + len(instructions) * 4).body_lines[0]
                                return_pc = ((pc + 4) & 0x3FFFC) + delta
                                source = Path(tmp) / "guard.c"
                                source.write_text(BISL_DRIVER % {
                                    "statement": statement, "link": link,
                                    "return_pc": return_pc, "delta": delta,
                                    "start_pc": pc + delta, "fallthrough": int(fallthrough),
                                    "enabled": enabled,
                                })
                                binary = source.with_suffix("")
                                compiled = subprocess.run(
                                    ["cc", "-std=gnu17", "-O1",
                                     "-I", str(root / "include"),
                                     "-I", str(root / "runtime/spu"),
                                     "-I", str(root / "tests"),
                                     str(source), "-lm", "-o", str(binary)],
                                    capture_output=True, text=True, check=False)
                                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                                for target in (return_pc, return_pc | 3, return_pc | 0x40000,
                                               return_pc + 0x40):
                                    executed = subprocess.run(
                                        [str(binary), hex(target)], capture_output=True,
                                        text=True, check=False)
                                    self.assertEqual(executed.returncode, 0, executed.stderr)

    def test_last_word_pc_getter_transfers_instead_of_falling_through(self):
        from spu_disasm import spu_decode
        from spu_lifter import SPULifter
        from spu_program_pipeline import _pc_getter

        instruction = spu_decode(brsl(0x3FFFC, 0, 126), 0x3FFFC)
        self.assertFalse(_pc_getter(instruction))
        self.assertFalse(_pc_getter(spu_decode(rr(0x1A9, ra=10, rt=9), 0x3FFFC), {0}))
        lifter = SPULifter()
        function = lifter.lift_function([instruction], 0x3FFFC, 0x40000)
        source = "\n".join(function.body_lines)
        self.assertIn("ctx->host_depth++", source)
        self.assertIn("spu_func_00000000(ctx)", source)
        self.assertIn("spu_link(0x0)", source)

    def test_call_link_register_controls_saved_r0_admission(self):
        for kind in ("brsl", "bisl"):
            for link in (0, 4):
                with self.subTest(kind=kind, link=link), tempfile.TemporaryDirectory() as tmp:
                    words = [rr(0x201)] * (0x94 // 4)
                    if kind == "brsl":
                        words[0] = brsl(0, 0x80, link)
                        words[1] = 0x1234
                        caller_end = 8
                    else:
                        words[0] = ri16(0x081, 0x80, 9)
                        words[1] = rr(0x1A9, ra=9, rt=link)
                        words[2] = 0x1234
                        caller_end = 12
                    words[0x80 // 4:] = [
                        ri10(0x24, 1, 1, 0), ri10(0x1C, -32, 1, 1),
                        ri10(0x34, 3, 1, 0), ri10(0x1C, 32, 1, 1),
                        rr(0x1A8, ra=0),
                    ]
                    image = b"".join(struct.pack(">I", word) for word in words)
                    bounds = [(0, caller_end), (0x80, 0x94)]
                    output = Path(tmp) / "reference"
                    result = run_lifter(image, bounds, [], output)
                    self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                    report = json.loads((output / "lift.json").read_text())
                    self.assertEqual(bool(report["r0_returns"]), link == 0)
                    self.assertEqual(0x90 in {item["site"] for item in report["r0_dispatches"]},
                                     link != 0)
                    result = run_lifter(image, bounds, ["0x80=native_f"],
                                        Path(tmp) / "override")
                    self.assertEqual(result.returncode == 0, link == 0, result.stderr)
                    if link != 0:
                        self.assertIn("unproven $r0 write", result.stderr)

    def test_mixed_call_links_do_not_supply_a_universal_r0_link(self):
        words = [rr(0x201)] * (0x94 // 4)
        words[0:2] = [brsl(0, 0x80, 0), 0x1234]
        words[0x40 // 4:0x48 // 4] = [brsl(0x40, 0x80, 4), 0x1234]
        words[0x80 // 4:] = [
            ri10(0x24, 1, 1, 0), ri10(0x1C, -32, 1, 1),
            ri10(0x34, 3, 1, 0), ri10(0x1C, 32, 1, 1), rr(0x1A8, ra=0),
        ]
        image = b"".join(struct.pack(">I", word) for word in words)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "reference"
            result = run_lifter(image, [(0, 8), (0x40, 0x48), (0x80, 0x94)], [], output)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            report = json.loads((output / "lift.json").read_text())
            self.assertEqual(report["r0_returns"], [])
            self.assertIn(0x90, {item["site"] for item in report["r0_dispatches"]})

    def test_non_r0_link_return_classification_is_unchanged(self):
        words = [rr(0x201)] * (0x84 // 4)
        words[0:2] = [brsl(0, 0x80, 4), 0x1234]
        words[0x80 // 4] = rr(0x1A8, ra=4)
        image = b"".join(struct.pack(">I", word) for word in words)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "reference"
            result = run_lifter(image, [(0, 8), (0x80, 0x84)], [], output)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("SPU_RET_REG(ctx, 4);", (output / "spu_recomp.c").read_text())

    def test_spilled_stack_pointer_store_keeps_visible_private_slot_assumption(self):
        body = [
            ri10(0x24, 1, 1, 0), ri10(0x1C, -32, 1, 1),
            ri10(0x1C, 48, 1, 5),  # form a pointer to the active link slot
            ri16(0x041, 0x300 // 4, 5),  # stqa $r5, scratch
            ri16(0x061, 0x300 // 4, 6),  # lqa $r6, scratch
            ri16(0x081, 0, 4),
            ri16(0x081, 0xA70, 7),
            rr(0x144, rb=4, ra=6, rt=7),  # stqx through the reloaded pointer
            ri10(0x34, 3, 1, 0), ri10(0x1C, 32, 1, 1), rr(0x1A8, ra=0),
        ]
        words = [rr(0x201)] * (0x40 // 4) + body
        image = b"".join(struct.pack(">I", word) for word in words)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "reference"
            result = run_lifter(image, [(0x40, 0x40 + len(body) * 4)], [], output)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            [entry] = json.loads((output / "lift.json").read_text())["r0_returns"]
            self.assertIn("spu-abi-link-slot-private", entry["assumes"])
            result = run_lifter(image, [(0x40, 0x40 + len(body) * 4)],
                                ["0x40=native_f"], Path(tmp) / "override")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("spu-abi-link-slot-private", result.stderr)

    def test_override_refuses_every_extra_return_assumption(self):
        for assumption in ("spu-abi-link-slot", "spu-abi-link-slot-private",
                           "spu-abi-callee-saved", "no-unmodelled-indirect-entry"):
            with self.subTest(assumption=assumption), tempfile.TemporaryDirectory() as tmp:
                words = [rr(0x201)] * (0xC4 // 4)
                caller = [
                    ri10(0x24, 1, 1, 0), ri10(0x1C, -32, 1, 1),
                    brsl(0x48, 0xA0) if assumption == "spu-abi-callee-saved"
                    else ri16(0x064, (0x80 - 0x48) // 4, 0),
                ]
                words[0x40 // 4:0x4C // 4] = caller
                if assumption == "spu-abi-callee-saved":
                    words[0x4C // 4] = ri16(0x064, (0x80 - 0x4C) // 4, 0)
                epilogue = [ri10(0x34, 3, 1, 0), ri10(0x1C, 32, 1, 1),
                            rr(0x1A8, ra=0)]
                if assumption == "spu-abi-link-slot-private":
                    epilogue = [
                        ri10(0x34, 0, 3, 5), rr(0x144, rb=4, ra=5, rt=3),
                    ] + epilogue
                end = 0x80 + len(epilogue) * 4
                words[0x80 // 4:end // 4] = epilogue
                words[0xA0 // 4] = rr(0x1A8, ra=0)
                words[0xC0 // 4] = rr(0x1A8, ra=9 if assumption ==
                                         "no-unmodelled-indirect-entry" else 0)
                bounds = [(0x40, 0x4C), (0x80, end), (0xA0, 0xA4), (0xC0, 0xC4)]
                if assumption == "spu-abi-callee-saved":
                    bounds.insert(1, (0x4C, 0x50))
                image = b"".join(struct.pack(">I", word) for word in words)
                output = Path(tmp) / "reference"
                result = run_lifter(image, bounds, [], output)
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                report = json.loads((output / "lift.json").read_text())
                [entry] = [item for item in report["r0_returns"] if item["site"] == end - 4]
                tags = entry["assumes"]
                tags = [tags] if isinstance(tags, str) else tags
                self.assertIn(assumption, tags)
                result = run_lifter(image, bounds, ["0x80=native_f"],
                                    Path(tmp) / "override")
                if assumption == "spu-abi-link-slot":
                    self.assertEqual(result.returncode, 0, result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("return proof requires", result.stderr)
                    self.assertIn(assumption, result.stderr)

    def test_opaque_stores_distinguish_stack_and_nonstack_bases(self):
        for case in ("stack", "loaded-pointer", "copied-stack", "offset-stack", "sum-stack",
                     "subtract-stack", "stack-index"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                setup = {
                    "stack": rr(0x201),
                    "loaded-pointer": ri10(0x34, 0, 3, 5),
                    "copied-stack": ri10(0x04, 0, 1, 5),
                    "offset-stack": ri10(0x1C, 16, 1, 5),
                    "sum-stack": rr(0x0C0, rb=3, ra=1, rt=5),
                    "subtract-stack": rr(0x040, rb=1, ra=3, rt=5),
                    "stack-index": ri10(0x04, 0, 1, 4),
                }[case]
                base = 1 if case == "stack" else 5
                body = [
                    ri10(0x24, 1, 1, 0), ri10(0x1C, -32, 1, 1), setup,
                    rr(0x144, rb=4, ra=base, rt=3),  # stqx through an unknown index
                    ri10(0x34, 3, 1, 0), ri10(0x1C, 32, 1, 1), rr(0x1A8, ra=0),
                ]
                words = [rr(0x201)] * (0x40 // 4) + body
                image = b"".join(struct.pack(">I", word) for word in words)
                output = Path(tmp) / "lift"
                result = run_lifter(image, [(0x40, 0x40 + len(body) * 4)], [], output)
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                report = json.loads((output / "lift.json").read_text())
                self.assertEqual(bool(report["r0_returns"]), case == "loaded-pointer")
                if case == "loaded-pointer":
                    self.assertEqual(report["r0_returns"][0]["assumes"],
                                     ["spu-abi-link-slot", "spu-abi-link-slot-private"])
                else:
                    self.assertTrue(report["r0_dispatches"])

    def test_private_slot_assumption_follows_a_reloaded_link_to_another_slot(self):
        body = [
            ri10(0x24, 1, 1, 0), ri10(0x34, 0, 3, 5),
            rr(0x144, rb=4, ra=5, rt=3),
            ri10(0x34, 1, 1, 0), ri10(0x24, 2, 1, 0),
            ri10(0x34, 2, 1, 0), rr(0x1A8, ra=0),
        ]
        words = [rr(0x201)] * (0x40 // 4) + body
        image = b"".join(struct.pack(">I", word) for word in words)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "lift"
            result = run_lifter(image, [(0x40, 0x40 + len(body) * 4)], [], output)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            report = json.loads((output / "lift.json").read_text())
            [entry] = report["r0_returns"]
            self.assertEqual(entry["assumes"],
                             ["spu-abi-link-slot", "spu-abi-link-slot-private"])

    def test_loop_join_preserves_saved_slot_after_r0_clobber(self):
        for saved_on_both_paths in (True, False):
            with self.subTest(saved_on_both_paths=saved_on_both_paths):
                words = [rr(0x201)] * (0xA4 // 4)
                words[0x40 // 4:0x60 // 4] = [
                    ri16(0x040, (0x4C - 0x40) // 4, 3),
                    ri10(0x24, 1, 1, 0),
                    ri16(0x064, (0x50 - 0x48) // 4, 0),
                    ri10(0x24, 1, 1, 0) if saved_on_both_paths else rr(0x201),
                    ri10(0x1C, -32, 1, 1),
                    ri16(0x040, (0x70 - 0x54) // 4, 3),
                    brsl(0x58, 0xA0),
                    ri16(0x064, (0x54 - 0x5C) // 4, 0),
                ]
                words[0x70 // 4:0x7C // 4] = [
                    ri10(0x34, 3, 1, 0), ri10(0x1C, 32, 1, 1), rr(0x1A8, ra=0),
                ]
                words[0xA0 // 4] = rr(0x1A8, ra=0)
                image = b"".join(struct.pack(">I", word) for word in words)
                with tempfile.TemporaryDirectory() as tmp:
                    output = Path(tmp) / "lift"
                    result = run_lifter(
                        image, [(0x40, 0x54), (0x54, 0x60), (0x70, 0x7C),
                                (0xA0, 0xA4)], [], output)
                    self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                    report = json.loads((output / "lift.json").read_text())
                    returns = {item["site"] for item in report["r0_returns"]}
                    self.assertEqual(0x78 in returns, saved_on_both_paths)
                    dispatches = {item["site"] for item in report["r0_dispatches"]}
                    self.assertEqual(0x78 in dispatches, not saved_on_both_paths)

    def test_return_entry_inherits_only_its_known_call_frame(self):
        for case in ("same-frame", "different-frame", "extra-entry"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                words = [rr(0x201)] * (0xA4 // 4)
                words[0x40 // 4:0x5C // 4] = [
                    ri10(0x24, 1, 1, 0), ri10(0x1C, -32, 1, 1),
                    ri16(0x040, (0x70 - 0x48) // 4, 3),
                    ri10(0x1C, -16, 1, 1) if case == "different-frame" else rr(0x201),
                    brsl(0x50, 0xA0), rr(0x201),
                    ri16(0x064, (0x70 - 0x58) // 4, 0),
                ]
                words[0x70 // 4:0x7C // 4] = [
                    ri10(0x34, 3, 1, 0), ri10(0x1C, 32, 1, 1), rr(0x1A8, ra=0),
                ]
                words[0xA0 // 4] = rr(0x1A8, ra=0)
                image = b"".join(struct.pack(">I", word) for word in words)
                output = Path(tmp) / "lift"
                extra = ["--extra-funcs", "0x54"] if case == "extra-entry" else []
                result = run_lifter(
                    image, [(0x40, 0x54), (0x54, 0x5C), (0x70, 0x7C), (0xA0, 0xA4)],
                    [], output, extra)
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                report = json.loads((output / "lift.json").read_text())
                returns = {item["site"]: item for item in report["r0_returns"]}
                self.assertEqual(0x78 in returns, case == "same-frame")
                if case == "same-frame":
                    self.assertEqual(returns[0x78]["assumes"],
                                     ["spu-abi-callee-saved", "spu-abi-link-slot"])

    def test_split_epilogue_inherits_a_fallthrough_frame(self):
        words = [rr(0x201)] * (0x54 // 4)
        words[0x40 // 4:] = [
            ri10(0x24, 1, 1, 0), ri10(0x1C, -32, 1, 1),
            ri10(0x34, 3, 1, 0), ri10(0x1C, 32, 1, 1), rr(0x1A8, ra=0),
        ]
        image = b"".join(struct.pack(">I", word) for word in words)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "lift"
            result = run_lifter(image, [(0x40, 0x48), (0x48, 0x54)], [], output)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            report = json.loads((output / "lift.json").read_text())
            [entry] = report["r0_returns"]
            self.assertEqual(entry["site"], 0x50)
            self.assertTrue(entry["inherited"])
            self.assertEqual(entry["assumes"], "spu-abi-link-slot")

    def test_shared_epilogue_inherits_only_unambiguous_direct_frames(self):
        for case in ("same-frame", "different-frame", "call-target", "immediate",
                     "data-word", "unresolved-transfer"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                words = [rr(0x201)] * (0xD0 // 4)
                # Two independently callable procedures share one epilogue.
                for start, frame in ((0x40, 32),
                                     (0x60, 64 if case == "different-frame" else 32)):
                    words[start // 4:start // 4 + 3] = [
                        ri10(0x24, 1, 1, 0),  # save entry link at old SP+16
                        ri10(0x1C, -frame, 1, 1),
                        ri16(0x064, (0x80 - start - 8) // 4, 0),
                    ]
                words[0x80 // 4:0x80 // 4 + 4] = [
                    ri10(0x34, 3, 1, 0),  # new SP+48 = old SP+16
                    ri10(0x1C, 32, 1, 1),
                    ri10(0x34, 0, 1, 80),  # unrelated callee-saved register
                    rr(0x1A8, ra=0),
                ]
                words[0xC0 // 4] = (
                    brsl(0xC0, 0x80) if case == "call-target" else
                    ri16(0x081, 0x80, 3) if case == "immediate" else rr(0x201))
                words[0xC4 // 4] = rr(0x1A8, ra=0)
                if case == "data-word":
                    words[0xCC // 4] = 0x80
                if case == "unresolved-transfer":
                    words[0xC0 // 4] = rr(0x1A8, ra=9)
                image = b"".join(struct.pack(">I", word) for word in words)
                bounds = [(0x40, 0x4C), (0x60, 0x6C), (0x80, 0x90),
                          (0xC0, 0xC8)]
                output = Path(tmp) / "lift"
                result = run_lifter(image, bounds, [], output)
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                source = (output / "spu_recomp.c").read_text()
                epilogue = source.split(
                    "void xfn_spu_func_00000080(spu_context* ctx) {", 1)[1].split("}", 1)[0]
                if case in ("same-frame", "unresolved-transfer"):
                    self.assertIn("SPU_RET(ctx)", epilogue)
                    self.assertNotIn("ctx->pc = ctx->gpr[0]._u32[0];", epilogue)
                    report = json.loads((output / "lift.json").read_text())
                    [entry] = [item for item in report["r0_returns"] if item["site"] == 0x8C]
                    self.assertTrue(entry["inherited"])
                    self.assertEqual(
                        entry["assumes"],
                        ["no-unmodelled-indirect-entry", "spu-abi-link-slot"]
                        if case == "unresolved-transfer" else "spu-abi-link-slot")
                else:
                    self.assertIn("ctx->pc = ctx->gpr[0]._u32[0];", epilogue)
                    self.assertNotIn("SPU_RET(ctx)", epilogue)

    def test_balanced_stack_link_restore_is_a_return(self):
        body = [
            ri10(0x24, 1, 1, 0),  # stqd $r0,16($r1)
            ri10(0x1C, -32, 1, 1),
            ri10(0x1C, 32, 1, 1),
            ri10(0x34, 1, 1, 0),  # lqd $r0,16($r1)
            rr(0x1A8, ra=0),
        ]
        words = [0] * (0x40 // 4) + body
        image = b"".join(struct.pack(">I", word) for word in words)
        bounds = [(0x40, 0x40 + len(body) * 4)]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = run_lifter(image, bounds, [], root / "reference")
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            source = (root / "reference" / "spu_recomp.c").read_text()
            self.assertIn("SPU_RET(ctx)", source)
            self.assertNotIn("ctx->pc = ctx->gpr[0]._u32[0];", source)

            result = run_lifter(image, bounds, ["0x40=native_f"], root / "override")
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

    def test_stack_slot_uses_r1_delta_at_the_load(self):
        body = [
            ri10(0x24, 1, 1, 0),  # stqd $r0,16($r1)
            ri10(0x1C, -32, 1, 1),
            ri10(0x34, 3, 1, 0),  # lqd $r0,48($r1), same slot after -32
            ri10(0x1C, 32, 1, 1),
            rr(0x1A8, ra=0),
        ]
        words = [0] * (0x40 // 4) + body
        image = b"".join(struct.pack(">I", word) for word in words)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "lift"
            result = run_lifter(image, [(0x40, 0x40 + len(body) * 4)], [], output)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            source = (output / "spu_recomp.c").read_text()
            self.assertIn("SPU_RET(ctx)", source)
            self.assertNotIn("ctx->pc = ctx->gpr[0]._u32[0];", source)

    def test_ori_from_other_register_is_not_a_link_preserving_move(self):
        body = [ri10(0x04, 0, 3, 0), rr(0x1A8, ra=0)]
        words = [0] * (0x40 // 4) + body
        image = b"".join(struct.pack(">I", word) for word in words)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = run_lifter(image, [(0x40, 0x48)], [], root / "reference")
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            source = (root / "reference" / "spu_recomp.c").read_text()
            self.assertIn("ctx->pc = ctx->gpr[0]._u32[0];", source)
            self.assertNotIn("SPU_RET(ctx)", source)
            result = run_lifter(image, [(0x40, 0x48)], ["0x40=native_f"],
                                root / "override")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unproven $r0 write by ori at 0x40", result.stderr)


    def test_channel_instruction_is_refused_by_address_and_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            words = [0] * 0x12
            words[0x40 // 4] = (0x00D << 21) | (7 << 7) | 3  # rdch $r3, channel 7
            words[0x44 // 4] = rr(0x1A8, ra=0)
            image = b"".join(struct.pack(">I", word) for word in words)
            result = run_lifter(image, [(0x40, 0x48)], ["0x40=native_f"], root / "lift")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("--override 0x40 refuses non-leaf function", result.stderr)
            self.assertIn("channel instruction rdch", result.stderr)
    def test_lqx_is_never_an_abi_link_restore(self):
        body = [
            ri10(0x24, 1, 1, 0),  # stqd $r0,16($r1)
            rr(0x1C4, rb=3, ra=2, rt=0),  # lqx $r0,$r2,$r3
            rr(0x1A8, ra=0),
        ]
        words = [0] * (0x40 // 4) + body
        image = b"".join(struct.pack(">I", word) for word in words)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = run_lifter(image, [(0x40, 0x4C)], [], root / "reference")
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            source = (root / "reference" / "spu_recomp.c").read_text()
            self.assertIn("ctx->pc = ctx->gpr[0]._u32[0];", source)
            self.assertNotIn("SPU_RET(ctx)", source)



if __name__ == "__main__":
    unittest.main()
