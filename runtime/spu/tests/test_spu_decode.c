/* The interpreter's decode contract: op and every field spu_decode1 hands back
 * for instructions at the opcode-width and immediate-form boundaries, and the
 * operand each handler of the run loop binds for its own op's form. Encodings
 * are built here from the SPU ISA field layouts (IBM SPU ISA v1.2, ch. 2), not
 * from the generated table, so a table whose precedence or immediate forms
 * drift fails against independent expectations. spu_decode1 and spu_step are
 * static, hence the include of the interpreter itself. */
#include "../spu_interp.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Decode-only test: nothing here executes, so the external hooks must never run. */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c; (void)a; abort(); }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}

/* ISA layouts, bit 0 = LSB. */
static uint32_t rrr(uint32_t op4, uint32_t rt, uint32_t rb, uint32_t ra, uint32_t rc) {
    return (op4 << 28) | (rt << 21) | (rb << 14) | (ra << 7) | rc;
}
static uint32_t ri18(uint32_t op7, uint32_t i18, uint32_t rt) {
    return (op7 << 25) | ((i18 & 0x3FFFF) << 7) | rt;
}
static uint32_t ri16(uint32_t op9, uint32_t i16, uint32_t rt) {
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt;
}
static uint32_t ri10(uint32_t op8, uint32_t i10, uint32_t ra, uint32_t rt) {
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt;
}
static uint32_t ri8(uint32_t op10, uint32_t i8, uint32_t ra, uint32_t rt) {
    return (op10 << 22) | ((i8 & 0xFF) << 14) | (ra << 7) | rt;
}
/* RI7 and RR share the 11-bit opcode; RR's rb sits where RI7's i7 does. */
static uint32_t ri7(uint32_t op11, uint32_t i7, uint32_t ra, uint32_t rt) {
    return (op11 << 21) | ((i7 & 0x7F) << 14) | (ra << 7) | rt;
}

static int failures;
static spu_ins dec(uint32_t insn, uint32_t pc) {
    spu_ins d;
    memset(&d, 0xFF, sizeof d); /* decode must overwrite every field, not leak the caller's */
    spu_decode1(insn, pc, &d);
    return d;
}
#define CHECK(cond, what, insn) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s insn=%08X: %s\n", what, (unsigned)(insn), #cond); ++failures; } } while (0)

static void expect_imm(const char* what, uint32_t insn, spu_op op, int32_t imm) {
    spu_ins d = dec(insn, 0x100);
    CHECK(d.op == op, what, insn);
    CHECK(d.imm == imm, what, insn);
    CHECK(d.tgt == 0, what, insn);
}
static void expect_tgt(const char* what, uint32_t insn, uint32_t pc, spu_op op, uint32_t tgt) {
    spu_ins d = dec(insn, pc);
    CHECK(d.op == op, what, insn);
    CHECK(d.tgt == tgt, what, insn);
    CHECK(d.imm == 0, what, insn);
}

/* ---- the handlers bind the same operands ----
 * spu_decode1 above is the operand binding as data. The run loop never calls
 * it: each handler binds the operand of its own op's form inline. So one
 * instruction of every immediate form is also executed here (spu_step runs the
 * same handlers as the run loop) and its effect compared with what the ISA
 * defines, worked out below without the interpreter's helpers. The immediates
 * are chosen so that a handler reading another form's field gets a different
 * answer: bits above the low 7 set where the form is 10 bits wide, a value
 * above 127 where the field is unsigned 8 bits, a negative immediate where the
 * field is signed. */
static spu_context g_ctx;
static void be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t rd32(uint32_t at) { const uint8_t* p = g_ctx.ls + at; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void fresh(uint32_t pc) {
    memset(&g_ctx, 0, sizeof g_ctx);
    spu_context_init(&g_ctx, 0);
    g_ctx.pc = pc;
}
static int run1(uint32_t insn) {
    be32(g_ctx.ls + (g_ctx.pc & SPU_LS_MASK), insn);
    return spu_step(&g_ctx);
}
static void setw(int r, uint32_t v) { g_ctx.gpr[r] = spu_splat_u32(v); }
static uint32_t W(int r, int lane) { return g_ctx.gpr[r]._u32[lane]; }
static void all_lanes(const char* what, int r, uint32_t want, uint32_t insn) {
    for (int l = 0; l < 4; l++) CHECK(W(r, l) == want, what, insn);
}
/* 16 bytes 00 01 .. 0F at `at` (a quadword of recognisable words). */
static void pattern(uint32_t at) { for (int i = 0; i < 16; i++) g_ctx.ls[at + i] = (uint8_t)i; }
static void expect_pattern(const char* what, int r, uint32_t insn) {
    CHECK(W(r, 0) == 0x00010203u && W(r, 1) == 0x04050607u && W(r, 2) == 0x08090A0Bu &&
          W(r, 3) == 0x0C0D0E0Fu, what, insn);
}

static void test_handler_operands(void) {
    uint32_t i;

    /* RI16 signed (il) and unsigned (ilh, ilhu, iohl, fsmbi) immediates. */
    fresh(0x100); i = ri16(0x81, 0x8001, 5);
    CHECK(!run1(i), "il runs", i); all_lanes("il sign-extends", 5, 0xFFFF8001u, i);
    fresh(0x100); i = ri16(0x83, 0x8001, 5);
    run1(i); all_lanes("ilh", 5, 0x80018001u, i);
    fresh(0x100); i = ri16(0x82, 0x8001, 5);
    run1(i); all_lanes("ilhu", 5, 0x80010000u, i);
    fresh(0x100); setw(5, 0x12340000u); i = ri16(0xC1, 0x8001, 5);
    run1(i); all_lanes("iohl zero-extends", 5, 0x12348001u, i);
    fresh(0x100); i = ri16(0x65, 0xA000, 5);   /* bits 15..12 = 1010 select bytes 0 and 2 */
    run1(i); CHECK(W(5, 0) == 0xFF00FF00u && W(5, 1) == 0 && W(5, 2) == 0 && W(5, 3) == 0, "fsmbi", i);

    /* RI18: ila takes the whole 18-bit field. */
    fresh(0x100); i = ri18(0x21, 0x2ABCD, 5);
    run1(i); all_lanes("ila", 5, 0x2ABCDu, i);

    /* RI10 signed: the immediate's bits above the low 7 matter. */
    fresh(0x100); setw(4, 100); i = ri10(0x1C, (uint32_t)-300, 4, 5);
    run1(i); all_lanes("ai -300", 5, (uint32_t)(100 - 300), i);
    fresh(0x100); setw(4, 1); i = ri10(0x04, 0x200, 4, 5);   /* ori -512 */
    run1(i); all_lanes("ori sign-extends", 5, 1u | 0xFFFFFE00u, i);
    fresh(0x100); setw(4, 0xFFFFFFFFu); i = ri10(0x7C, 0x3FF, 4, 5);   /* ceqi -1 */
    run1(i); all_lanes("ceqi -1", 5, 0xFFFFFFFFu, i);

    /* RI10 scaled by 16: lqd/stqd. */
    fresh(0x100); setw(4, 0x1000); pattern(0x1000 - 32); i = ri10(0x34, (uint32_t)-2, 4, 5);
    run1(i); expect_pattern("lqd -2", 5, i);
    fresh(0x100); setw(4, 0x1000); i = ri10(0x34, 0x155, 4, 5);   /* +341 * 16 = 0x1550 */
    pattern(0x1000 + 0x1550); run1(i); expect_pattern("lqd +0x155", 5, i);
    fresh(0x100); setw(4, 0x1000); g_ctx.gpr[5]._u32[0] = 0x00010203u; g_ctx.gpr[5]._u32[1] = 0x04050607u;
    g_ctx.gpr[5]._u32[2] = 0x08090A0Bu; g_ctx.gpr[5]._u32[3] = 0x0C0D0E0Fu;
    i = ri10(0x24, 3, 4, 5);
    run1(i); CHECK(rd32(0x1030) == 0x00010203u && rd32(0x103C) == 0x0C0D0E0Fu, "stqd +3", i);

    /* RI16 local-store addresses: absolute (lqa/stqa) and pc-relative (lqr/stqr). */
    fresh(0x100); pattern((0x1234u << 2) & 0x3FFF0u); i = ri16(0x61, 0x1234, 5);
    run1(i); expect_pattern("lqa", 5, i);
    fresh(0x300); pattern(0x2F0); i = ri16(0x67, (uint32_t)-2, 5);   /* (0x300 - 8) & ~0xF */
    run1(i); expect_pattern("lqr -2", 5, i);
    fresh(0x4); pattern(0x3FFE0); i = ri16(0x67, (uint32_t)-8, 5);   /* wraps below 0 */
    run1(i); expect_pattern("lqr wrap", 5, i);
    fresh(0x100); setw(5, 0xA5A5A5A5u); i = ri16(0x41, 0x1234, 5);
    run1(i); CHECK(rd32(0x48D0) == 0xA5A5A5A5u && rd32(0x48DC) == 0xA5A5A5A5u, "stqa", i);
    fresh(0x300); setw(5, 0x5A5A5A5Au); i = ri16(0x47, (uint32_t)-2, 5);
    run1(i); CHECK(rd32(0x2F0) == 0x5A5A5A5Au && rd32(0x2FC) == 0x5A5A5A5Au, "stqr -2", i);
    fresh(0x100); setw(4, 0x1000); setw(3, 0x20); pattern(0x1020); i = ri7(0x1C4, 3, 4, 5);   /* lqx $5, $4, $3 */
    run1(i); expect_pattern("lqx", 5, i);

    /* Branches: pc-relative (wrapping at 256 KiB), absolute, and link. */
    fresh(0x100); i = ri16(0x64, 3, 0);
    run1(i); CHECK(g_ctx.pc == 0x10C, "br +3", i);
    fresh(0x100); i = ri16(0x64, (uint32_t)-4, 0);
    run1(i); CHECK(g_ctx.pc == 0xF0, "br -4", i);
    fresh(0x4); i = ri16(0x64, (uint32_t)-2, 0);
    run1(i); CHECK(g_ctx.pc == 0x3FFFC, "br wraps", i);
    fresh(0x100); i = ri16(0x66, 0x40, 7);   /* brsl $7 */
    run1(i); CHECK(g_ctx.pc == 0x200 && W(7, 0) == 0x104, "brsl", i);
    fresh(0x100); i = ri16(0x60, 0x1234, 0);
    run1(i); CHECK(g_ctx.pc == 0x48D0, "bra", i);
    fresh(0x100); i = ri16(0x60, (uint32_t)-2, 0);
    run1(i); CHECK(g_ctx.pc == 0x3FFF8, "bra -2", i);
    fresh(0x100); i = ri16(0x62, 0x1234, 7);   /* brasl $7 */
    run1(i); CHECK(g_ctx.pc == 0x48D0 && W(7, 0) == 0x104, "brasl", i);
    fresh(0x100); setw(6, 1); i = ri16(0x42, 5, 6);   /* brnz taken */
    run1(i); CHECK(g_ctx.pc == 0x114, "brnz taken", i);
    fresh(0x100); setw(6, 0); i = ri16(0x42, 5, 6);
    run1(i); CHECK(g_ctx.pc == 0x104, "brnz not taken", i);
    fresh(0x100); setw(6, 0); i = ri16(0x40, 5, 6);   /* brz taken */
    run1(i); CHECK(g_ctx.pc == 0x114, "brz taken", i);

    /* RI8: unsigned scale, above 127. 3 / 2^(155-154) = 1.5; 3.0 * 2^(173-173) = 3. */
    fresh(0x100); setw(4, 3); i = ri8(0x1DA, 154, 4, 5);
    run1(i); all_lanes("csflt 154", 5, 0x3FC00000u, i);
    fresh(0x100); setw(4, 0x40400000u); i = ri8(0x1D8, 173, 4, 5);
    run1(i); all_lanes("cflts 173", 5, 3, i);

    /* RI7: the field sits where an RR's rb does. cbd inserts 0x03 at byte (ra + i7) & 15. */
    fresh(0x100); setw(4, 0x100); i = ri7(0x1F4, 0x7B, 4, 5);
    run1(i); CHECK(W(5, 0) == 0x10111213u && W(5, 1) == 0x14151617u && W(5, 2) == 0x18191A03u &&
                   W(5, 3) == 0x1C1D1E1Fu, "cbd", i);
    fresh(0x100); setw(4, 0x80000001u); i = ri7(0x078, 0x23, 4, 5);   /* roti by 35 & 31 = 3 */
    run1(i); all_lanes("roti", 5, 0x0Cu, i);

    /* stop: the 14-bit code, and stopd, which has none. */
    fresh(0x100); i = 0x2ABC;
    CHECK(run1(i) == 1 && g_ctx.stop_code == 0x2ABC && g_ctx.pc == 0x104 &&
          g_ctx.status == SPU_STATUS_STOPPED_BY_STOP, "stop", i);
    fresh(0x100); i = 0x140u << 21;
    CHECK(run1(i) == 1 && g_ctx.stop_code == 0x3FFF && g_ctx.pc == 0x104, "stopd", i);

    /* A halt with an immediate: the immediate is signed 10 bits, and the halt
     * leaves pc at the instruction. */
    fresh(0x100); setw(4, 0xFFFFFFFFu); i = ri10(0x7F, 0x3FF, 4, 0);   /* heqi $4, -1 */
    CHECK(run1(i) == 1 && g_ctx.stop_code == 0x1000 && g_ctx.pc == 0x100 &&
          g_ctx.status == SPU_STATUS_STOPPED_BY_HALT, "heqi -1", i);

    /* RRR: the destination is the high field; the third source is the low one. */
    fresh(0x100); setw(6, 0xFF00FF00u); setw(7, 0x0F0F0F0Fu); setw(8, 0xFFFF0000u);
    i = rrr(0x8, 9, 6, 7, 8);   /* selb: dest field 9, rb=6, ra=7, third source 8 */
    run1(i); all_lanes("selb", 9, (0xFF00FF00u & 0xFFFF0000u) | (0x0F0F0F0Fu & ~0xFFFF0000u), i);
}

int main(void) {
    /* RRR: shufb rt=5 rb=6 ra=7 rc=8. The destination is the high field, which
     * the interpreter calls rc; the low field is the third source (rt). */
    {
        uint32_t i = rrr(0xB, 5, 6, 7, 8);
        spu_ins d = dec(i, 0);
        CHECK(d.op == SPU_shufb, "shufb", i);
        CHECK(d.rc == 5 && d.rb == 6 && d.ra == 7 && d.rt == 8, "shufb fields", i);
        CHECK(d.imm == 0 && d.tgt == 0, "shufb no imm", i);
    }

    /* RI18 */
    expect_imm("ila 18 bits", ri18(0x21, 0x3FFFF, 3), SPU_ila, 0x3FFFF);
    expect_imm("ila high bit", ri18(0x21, 0x20000, 3), SPU_ila, 0x20000);
    {
        spu_ins d = dec(ri18(0x21, 0x2ABCD, 9), 0);
        CHECK(d.rt == 9, "ila rt", 0);
    }

    /* Hints (hbra/hbrr) are the other RI18 ops: only the low 16 bits of the
     * 18-bit field are the immediate; the top two belong to the split ROH field. */
    expect_imm("hbra low 16", ri18(0x08, 0x3FFFF, 3), SPU_hbra, 0xFFFF);
    expect_imm("hbrr low 16", ri18(0x09, 0x2ABCD, 3), SPU_hbrr, 0xABCD);

    /* RI16 immediates: il sign-extends; ilh/ilhu/iohl/fsmbi zero-extend. */
    expect_imm("il negative", ri16(0x081, 0x8000, 4), SPU_il, -32768);
    expect_imm("il -1", ri16(0x081, 0xFFFF, 4), SPU_il, -1);
    expect_imm("il positive", ri16(0x081, 0x7FFF, 4), SPU_il, 0x7FFF);
    expect_imm("ilhu", ri16(0x082, 0xFFFF, 4), SPU_ilhu, 0xFFFF);
    expect_imm("ilh", ri16(0x083, 0x8001, 4), SPU_ilh, 0x8001);
    expect_imm("iohl unsigned", ri16(0x0C1, 0x8000, 4), SPU_iohl, 0x8000);
    expect_imm("fsmbi", ri16(0x065, 0xFFFF, 4), SPU_fsmbi, 0xFFFF);

    /* RI16 targets: pc-relative forms add pc, absolute ones do not; the
     * quadword forms mask to 16-byte alignment, branches to word alignment. */
    expect_tgt("lqr back", ri16(0x067, 0xFFFF, 4), 0x104, SPU_lqr, 0x100);
    expect_tgt("stqr back", ri16(0x047, 0xFFFF, 4), 0x104, SPU_stqr, 0x100);
    expect_tgt("lqr wraps past 256K", ri16(0x067, 8, 4), 0x3FFF0, SPU_lqr, 0x10);
    expect_tgt("lqr wraps below 0", ri16(0x067, 0xFFF0, 4), 0x20, SPU_lqr, 0x3FFE0);
    expect_tgt("br wraps past 256K", ri16(0x064, 1, 0), 0x3FFFC, SPU_br, 0);
    expect_tgt("br wraps below 0", ri16(0x064, 0xFFFF, 0), 0, SPU_br, 0x3FFFC);
    expect_tgt("br forward", ri16(0x064, 0x10, 0), 0x200, SPU_br, 0x240);
    expect_tgt("brsl", ri16(0x066, 4, 0), 0x400, SPU_brsl, 0x410);
    expect_tgt("brz", ri16(0x040, 2, 3), 0x800, SPU_brz, 0x808);
    expect_tgt("brnz", ri16(0x042, 0xFFFE, 3), 0x800, SPU_brnz, 0x7F8);
    expect_tgt("brhz", ri16(0x044, 1, 3), 0x800, SPU_brhz, 0x804);
    expect_tgt("brhnz", ri16(0x046, 1, 3), 0x800, SPU_brhnz, 0x804);
    expect_tgt("bra ignores pc", ri16(0x060, 0x4000, 0), 0x3FFFC, SPU_bra, 0x10000);
    expect_tgt("bra negative", ri16(0x060, 0xFFFF, 0), 0x100, SPU_bra, 0x3FFFC);
    expect_tgt("brasl", ri16(0x062, 3, 0), 0x100, SPU_brasl, 0xC);
    expect_tgt("lqa", ri16(0x061, 0xFFFF, 4), 0x100, SPU_lqa, 0x3FFF0);
    expect_tgt("stqa", ri16(0x041, 0x0123, 4), 0x100, SPU_stqa, 0x0480);

    /* RI10: lqd/stqd scale the displacement by 16; the rest do not. */
    expect_imm("lqd +3", ri10(0x34, 3, 1, 2), SPU_lqd, 48);
    expect_imm("lqd -2", ri10(0x34, 0x3FE, 1, 2), SPU_lqd, -32);
    expect_imm("stqd -512", ri10(0x24, 0x200, 1, 2), SPU_stqd, -8192);
    expect_imm("stqd +511", ri10(0x24, 0x1FF, 1, 2), SPU_stqd, 8176);
    expect_imm("ai -1", ri10(0x1C, 0x3FF, 1, 2), SPU_ai, -1);
    expect_imm("ori +511", ri10(0x04, 0x1FF, 1, 2), SPU_ori, 511);
    {
        uint32_t i = ri10(0x1C, 0x155, 17, 90);
        spu_ins d = dec(i, 0);
        CHECK(d.ra == 17 && d.rt == 90 && d.ch == 17, "ai fields", i);
    }

    /* Channel ops are 11-bit opcodes; wrch (0x10D) must not be taken for the
     * shift-immediate family whose 11-bit opcode (shli 0x07B) differs. */
    {
        uint32_t i = ri7(0x00D, 0, 5, 6); /* rdch $6, $ch5 */
        spu_ins d = dec(i, 0);
        CHECK(d.op == SPU_rdch && d.ch == 5 && d.rt == 6 && d.imm == 0, "rdch", i);
        i = ri7(0x10D, 0, 7, 8);
        d = dec(i, 0);
        CHECK(d.op == SPU_wrch && d.ch == 7 && d.rt == 8 && d.imm == 0, "wrch", i);
        i = ri7(0x00F, 0, 9, 10);
        d = dec(i, 0);
        CHECK(d.op == SPU_rchcnt && d.ch == 9 && d.rt == 10 && d.imm == 0, "rchcnt", i);
        /* The whole 7-bit channel field reaches spu_rdch/spu_wrch; SPU channels go up to 127. */
        i = ri7(0x00D, 0, 0x7F, 1);
        d = dec(i, 0);
        CHECK(d.op == SPU_rdch && d.ch == 0x7F && d.rt == 1, "rdch channel 127", i);
        expect_imm("shli is not wrch", ri7(0x07B, 3, 5, 6), SPU_shli, 3);
    }

    /* STOP: 14-bit signal type, and no other RR form carries an immediate. */
    expect_imm("stop 14 bits", 0x00003FFFu, SPU_stop, 0x3FFF);
    expect_imm("stop 0x2A5", 0x000002A5u, SPU_stop, 0x2A5);
    expect_imm("stop ignores bit 14", 0x00004001u, SPU_stop, 1);
    expect_imm("a has no imm", ri7(0x0C0, 0x7F, 1, 2), SPU_a, 0);

    /* RI7: shifts/rotates sign-extend, the generate-controls zero-extend. */
    expect_imm("shli -64", ri7(0x07B, 0x40, 1, 2), SPU_shli, -64);
    expect_imm("roti 63", ri7(0x078, 0x3F, 1, 2), SPU_roti, 63);
    expect_imm("rotqbyi -1", ri7(0x1FC, 0x7F, 1, 2), SPU_rotqbyi, -1);
    expect_imm("cwd 64 unsigned", ri7(0x1F6, 0x40, 1, 2), SPU_cwd, 64);
    expect_imm("cbd 127 unsigned", ri7(0x1F4, 0x7F, 1, 2), SPU_cbd, 127);

    /* RI8: unsigned 8-bit scale, for all four conversions. */
    expect_imm("cflts 255", ri8(0x1D8, 0xFF, 1, 2), SPU_cflts, 255);
    expect_imm("cfltu 255", ri8(0x1D9, 0xFF, 1, 2), SPU_cfltu, 255);
    expect_imm("csflt 0x81", ri8(0x1DA, 0x81, 1, 2), SPU_csflt, 0x81);
    expect_imm("cuflt 0x80", ri8(0x1DB, 0x80, 1, 2), SPU_cuflt, 0x80);

    /* RR/LSX register forms. */
    {
        uint32_t i = ri7(0x1C4, 11, 12, 13); /* lqx $13, $12, $11 */
        spu_ins d = dec(i, 0);
        CHECK(d.op == SPU_lqx && d.rb == 11 && d.ra == 12 && d.rt == 13 && d.imm == 0 && d.tgt == 0,
              "lqx", i);
        i = ri7(0x144, 11, 12, 13);
        d = dec(i, 0);
        CHECK(d.op == SPU_stqx && d.rb == 11 && d.ra == 12 && d.rt == 13, "stqx", i);
    }

    /* A word no table claims traps as `.word`, with no immediate. */
    {
        uint32_t i = 0x00800000u; /* 11-bit opcode 0x004 */
        spu_ins d = dec(i, 0x40);
        CHECK(d.op == SPU_word && d.imm == 0 && d.tgt == 0, "invalid word", i);
    }

    /* dfceq, dfcmeq, dfcgt, dfcmgt and dftsv decode as the same trap on this
     * target: a retail CBE SPU raises an invalid-instruction error for each
     * (hwprobe spuexh special A, CECHC04 4.81; issue #124), so the generator
     * maps their RR prefixes to SPU_word even though spu_disasm.py still names
     * them. Every register field at its extreme proves the decode does not
     * look at one. The neighbouring DP op keeps its decode: dfa is 0x2CC. */
    {
        static const uint32_t op11[5] = { 0x3C3, 0x3CB, 0x2C3, 0x2CB, 0x3BF };
        for (unsigned k = 0; k < sizeof op11 / sizeof op11[0]; k++) {
            uint32_t i = (op11[k] << 21) | (0x7Fu << 14) | (0x7Fu << 7) | 0x7Fu;
            spu_ins d = dec(i, 0x40);
            CHECK(d.op == SPU_word && d.imm == 0 && d.tgt == 0, "CBE-invalid DP op", i);
        }
        {
            uint32_t i = ri7(0x2CC, 0x7F, 0x7F, 0x7F);   /* dfa (rb field = 0x7F) */
            spu_ins d = dec(i, 0);
            CHECK(d.op == SPU_dfa, "dfa still decodes", i);
        }
    }

    test_handler_operands();

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("spu_decode1: all contract checks passed; handler operand checks passed\n");
    return 0;
}
