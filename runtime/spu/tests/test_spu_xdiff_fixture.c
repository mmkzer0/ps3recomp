/*
 * test_spu_xdiff_fixture.c - the xdiff fixture codec (spu_xdiff_fixture.h,
 * docs/SPU_XDIFF.md section 7)
 *
 * No SPU code runs here. A synthetic fixture is built in memory (a patterned
 * image and data blocks in local store, end-state differences, registers, and
 * a transcript with every effect kind: GET, PUT, GETLLAR/PUTLLC with their
 * statuses, a list command with its staged list, a stripped list resumed by
 * MFC_WrListStallAck, an SDCRZ and a mailbox write), encoded, decoded and
 * compared field for field against what was built. The payload store is
 * checked directly (one copy per distinct payload) and the container is
 * parsed independently of the decoder: magic, sizes, checksum, 8-byte section
 * alignment, zero padding.
 *
 * Then one negative case per rejection of section 7.8, each mutated from a
 * valid encoding with the checksum recomputed unless the checksum is the check
 * under test, so the intended check is the one that answers. The first failure
 * exits nonzero with its message.
 */
#include "../xdiff/spu_xdiff_fixture.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h> /* the test is built on POSIX hosts only (CMakeLists.txt, NOT WIN32) */
#include <unistd.h> /* getpid */

/* Temp files of `path` the writer left in the current directory: `<path>.<pid>.<n>.partial`. */
static int count_partials(const char* path)
{
    const size_t plen = strlen(path);
    int found = 0;
    DIR* d = opendir(".");
    if (!d) return -1;
    for (struct dirent* e; (e = readdir(d)) != NULL;) {
        const size_t len = strlen(e->d_name);
        if (len > plen + sizeof ".partial" && strncmp(e->d_name, path, plen) == 0 && e->d_name[plen] == '.' &&
            strcmp(e->d_name + len - (sizeof ".partial" - 1), ".partial") == 0)
            found++;
    }
    closedir(d);
    return found;
}

/* ---- harness ------------------------------------------------------------ */

#define REQUIRE(cond, ...)                                                                 \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                     \
            printf(__VA_ARGS__);                                                           \
            printf("\n");                                                                  \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

/* spu_context.h's local-store diagnostics: GCC emits their static functions
 * even unused at -O0, so a program that includes the header but links none of
 * the channel runtime needs the two symbols they read. Nothing here runs SPU
 * code, so no watch can be armed: g_spu_ls_watch_n is -1, its unarmed value
 * (spu_channels.c:103), and the slow path is unreachable. It aborts rather
 * than pretend to record. */
int g_spu_ls_watch_n = -1;

void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{
    (void)lsa;
    (void)is_write;
    (void)p;
    (void)pc;
    (void)lr;
    fprintf(stderr, "test_spu_xdiff_fixture: the LS-watch diagnostic was reached; no SPU code "
                    "runs in this program\n");
    abort();
}

/* ---- FNV-1a-64, the algorithm of spu_workload_fingerprint ---------------- */

#define FNV64_BASIS 1469598103934665603ULL
#define FNV64_PRIME 1099511628211ULL

static uint64_t fnv64(uint64_t h, const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= FNV64_PRIME;
    }
    return h;
}

/* ---- bytes -------------------------------------------------------------- */

#define HDR_CHECKSUM 32u /* offsetof(spu_xdiff_file_header, checksum) */

static uint32_t rd32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static uint64_t rd64(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static void wr32(uint8_t* p, uint32_t v)
{
    memcpy(p, &v, sizeof v);
}

static void wr64(uint8_t* p, uint64_t v)
{
    memcpy(p, &v, sizeof v);
}

static void pattern(uint8_t* p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7u + (i >> 5) * 3u);
}

/* The encoder's checksum: FNV-1a-64 over the whole file with the checksum
 * field read as zero. */
static uint64_t checksum_of(const uint8_t* p, size_t n)
{
    uint64_t h = FNV64_BASIS;
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = (i >= HDR_CHECKSUM && i < HDR_CHECKSUM + 8) ? 0 : p[i];
        h ^= b;
        h *= FNV64_PRIME;
    }
    return h;
}

static void fix_checksum(uint8_t* p, size_t n)
{
    const uint64_t sum = checksum_of(p, n);
    memcpy(p + HDR_CHECKSUM, &sum, sizeof sum);
}

static uint8_t* section_entry(uint8_t* p, uint32_t tag)
{
    spu_xdiff_file_header h;
    memcpy(&h, p, sizeof h);
    for (uint32_t i = 0; i < h.section_count; i++) {
        uint8_t* e = p + h.header_bytes + (size_t)i * h.section_bytes;
        if (rd32(e) == tag) return e;
    }
    return NULL;
}

static uint8_t* section_bytes(uint8_t* p, uint32_t tag, uint64_t* bytes)
{
    uint8_t* e = section_entry(p, tag);
    REQUIRE(e != NULL, "the encoded file has no %08x section", (unsigned)tag);
    if (bytes) *bytes = rd64(e + 16);
    return p + rd64(e + 8);
}

/* @LS_0 extent i: the header, then its bytes padded to 8. */
static uint8_t* extent_header(uint8_t* ls0, uint32_t index)
{
    uint64_t at = 0;
    for (uint32_t i = 0; i < index; i++) {
        const uint32_t len = rd32(ls0 + at + 4);
        at += 8 + (((uint64_t)len + 7u) & ~(uint64_t)7u);
    }
    return ls0 + at;
}

/* ---- effect records ----------------------------------------------------- */

static spu_xdiff_effect effect(uint8_t kind, uint8_t channel, uint8_t tag, uint8_t dir,
                               uint32_t pc, uint32_t value, uint32_t status, uint64_t ea,
                               uint32_t lsa, uint32_t size, uint32_t blob)
{
    spu_xdiff_effect e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.channel = channel;
    e.tag = tag;
    e.dir = dir;
    e.pc = pc;
    e.value = value;
    e.status = status;
    e.ea = ea;
    e.lsa = lsa;
    e.size = size;
    e.blob = blob;
    return e;
}

static spu_xdiff_effect wrch(uint32_t pc, uint32_t channel, uint32_t value)
{
    return effect(SPU_FX_WRCH, (uint8_t)channel, 0, 0, pc, value, 0, 0, 0, 0,
                  SPU_XDIFF_NO_BLOB);
}

static spu_xdiff_effect rdch(uint32_t pc, uint32_t channel, uint32_t value)
{
    return effect(SPU_FX_RDCH, (uint8_t)channel, 0, 0, pc, value, 0, 0, 0, 0,
                  SPU_XDIFF_NO_BLOB);
}

static spu_xdiff_effect rchcnt(uint32_t pc, uint32_t channel, uint32_t value)
{
    return effect(SPU_FX_RCHCNT, (uint8_t)channel, 0, 0, pc, value, 0, 0, 0, 0,
                  SPU_XDIFF_NO_BLOB);
}

static spu_xdiff_effect mfc_cmd(uint32_t pc, uint32_t value, uint64_t ea, uint32_t lsa,
                                uint32_t tag, uint32_t size, uint32_t blob)
{
    return effect(SPU_FX_MFC_CMD, 21, (uint8_t)tag, 0, pc, value, 0, ea, lsa, size, blob);
}

static spu_xdiff_effect xfer(uint32_t pc, uint32_t opcode, uint8_t dir, uint64_t ea,
                             uint32_t lsa, uint32_t size, uint32_t tag, uint32_t status,
                             uint32_t blob)
{
    return effect(SPU_FX_XFER, 0, (uint8_t)tag, dir, pc, opcode, status, ea, lsa, size, blob);
}

/* MFC_WrListStallAck with a list parked on its tag: the elements left, at the
 * next element's address (section 6). */
static spu_xdiff_effect stall_ack(uint32_t pc, uint32_t channel, uint32_t tag, uint32_t lsa,
                                  uint32_t size, uint32_t blob)
{
    return effect(SPU_FX_WRCH, (uint8_t)channel, 0, 0, pc, tag, 0, 0, lsa, size, blob);
}

static void add(spu_xdiff_fixture* f, spu_xdiff_effect e)
{
    REQUIRE(spu_xdiff_fixture_add_effect(f, &e) == 0, "add_effect %u", e.kind);
}

static uint32_t add_blob(spu_xdiff_fixture* f, const void* p, uint32_t n)
{
    const uint32_t at = spu_xdiff_fixture_add_blob(f, p, n, 0);
    REQUIRE(at != SPU_XDIFF_NO_BLOB, "add_blob %u bytes", n);
    return at;
}

/* ---- the synthetic fixture ---------------------------------------------- */

#define IMG_LS    0x1000u
#define IMG_BYTES 0x400u

static uint8_t g_get[256];
static uint8_t g_line[128];
static uint8_t g_list[16];
static uint8_t g_elem0[16];
static uint8_t g_elem1[16];

/* The entry state a job loader leaves: spu_context_init, then image_id,
 * job_rt_ls{,_2}, the store, the registers and pc (section 7.4). */
static void set_entry_state(spu_xdiff_fixture* f)
{
    memset(&f->identity, 0, sizeof f->identity);
    memset(&f->unit, 0, sizeof f->unit);
    f->identity.image_fingerprint = fnv64(FNV64_BASIS, f->ls0 + IMG_LS, IMG_BYTES);
    f->identity.authority.tier = SPU_TIER_INTERP;
    f->identity.authority.lifter_abi = 0;
    f->identity.effect_port_abi = SPU_EFFECT_PORT_ABI;
    f->identity.context_census = 0xC0FFEE01u;
    f->identity.host_arch = SPU_XDIFF_HOST_ARM64;
    f->identity.host_isa = SPU_XDIFF_ISA_NEON | SPU_XDIFF_ISA_SCALAR;
    pattern(f->identity.toolkit_commit, sizeof f->identity.toolkit_commit, 0xA5);
    f->identity.toolkit_dirty = 1;
    f->identity.float_rz = 1;

    f->unit.kind = SPU_XDIFF_UNIT_JOB;
    f->unit.profile = SPU_XDIFF_PROFILE_JOBCHAIN;
    f->unit.entry_pc = 0;
    f->unit.return_ls = 0x100;
    f->unit.job_rt_ls = 0x2000;
    f->unit.job_rt_ls2 = 0;
    f->unit.image_ea = 0x00100000;
    f->unit.image_ls = IMG_LS;
    f->unit.image_bytes = IMG_BYTES;
    f->unit.bss_bytes = 0x100;
    f->unit.desc_ls = 0x300;
    f->unit.desc_bytes = 0x100;
    f->unit.io_ls = 0x400;
    f->unit.io_bytes = 0x200;
    f->unit.out_ls = 0x600;
    f->unit.out_bytes = 0x80;
    f->unit.scratch_ls = 0x700;
    f->unit.scratch_bytes = 0x100;
    f->unit.stack_bytes = 0x800;
    f->unit.r1 = 0x1F00;
    f->unit.ctx_ls = 0x200;
    f->unit.origin_queue = 3;
    f->unit.origin_worker = 1;
    f->unit.origin_seq = 42;
    f->unit.origin_descriptor_ea = 0x00200000;

    f->end.end_class = SPU_XDIFF_END_RETURN;
    f->end.stop_code = 0;
    f->end.pc = 0x104;
    f->end.status = 2;
}

static void set_verdict(spu_xdiff_fixture* f)
{
    memset(&f->verdict, 0, sizeof f->verdict);
    f->verdict.verdict = SPU_XDIFF_DIFFER;
    f->verdict.reason = SPU_XDIFF_R_EFFECT_KIND;
    f->verdict.match = SPU_XDIFF_MATCH_PROGRAM;
    f->verdict.effect_index = 3;
    f->verdict.subject_body.tier = SPU_TIER_LIFTED;
    f->verdict.subject_body.lifter_abi = 7;
    f->verdict.subject_body.code_end = 0x1234;
    f->verdict.subject_body.lift_options = 0xDEADBEEFCAFEULL;
    f->verdict.subject = wrch(0x44, 23, 0x3);
    f->verdict.where = 0x8000u + 5u;
    f->verdict.subject_pc = 0x44;
    pattern(f->verdict.want, sizeof f->verdict.want, 0x01);
    pattern(f->verdict.got, sizeof f->verdict.got, 0x81);
    f->verdict.budget = 1000000;
    f->verdict.budget_spent = 4321;
    f->has_verdict = 1;
}

/* A small valid fixture: the image, one data block, a GET with its payload.
 * The two blocks are far apart, so LS_0 has two extents. */
static void build_minimal(spu_xdiff_fixture* f)
{
    REQUIRE(spu_xdiff_fixture_init(f) == 0, "fixture_init");
    pattern(f->ls0 + IMG_LS, IMG_BYTES, 0x11);
    pattern(f->ls0 + 0x8000, 32, 0x22);
    memcpy(f->ls1, f->ls0, SPU_XDIFF_LS_BYTES);
    f->ls1[0x8000] ^= 0x5A;
    pattern(f->reg0, SPU_XDIFF_REG_BYTES, 0x5A);
    pattern(f->reg1, SPU_XDIFF_REG_BYTES, 0x77);
    set_entry_state(f);

    pattern(g_get, sizeof g_get, 0x55);
    add(f, wrch(0x10, 22, 0x2));                    /* MFC_WrTagMask */
    add(f, mfc_cmd(0x14, 0x40, 0x00A01000ULL, 0x1000, 1, 32, SPU_XDIFF_NO_BLOB));
    add(f, xfer(0x14, 0x40, SPU_XFER_TO_LS, 0x00A01000ULL, 0x1000, 32, 1, 0,
                add_blob(f, g_get, 32)));
    add(f, rdch(0x18, 24, 0x1));                    /* MFC_RdTagStat */
    add(f, wrch(0x1C, 28, 0x1));                    /* the outbound mailbox */
}

/* The full fixture: every effect kind, a list and a stalled list, atomics with
 * their statuses, an SDCRZ, one payload stored twice. */
static void build_full(spu_xdiff_fixture* f, int with_verdict)
{
    REQUIRE(spu_xdiff_fixture_init(f) == 0, "fixture_init");

    pattern(f->ls0 + IMG_LS, IMG_BYTES, 0x11);
    pattern(f->ls0 + 0x8000, 300, 0x22);   /* a long zero run above: LS_0 splits */
    pattern(f->ls0 + 0x9000, 96, 0x44);
    pattern(f->ls0 + 0x10000, 512, 0x66);

    memcpy(f->ls1, f->ls0, SPU_XDIFF_LS_BYTES);
    for (int i = 0; i < 8; i++) f->ls1[0x8000 + 16 + i] ^= 0xFF; /* adjacent changes */
    f->ls1[0x9000 + 3] ^= 0x80;                                  /* isolated change */
    f->ls1[0x10000] ^= 0x01;
    f->ls1[0x10009] ^= 0x02; /* 8 equal bytes between: one extent */
    f->ls1[0x1004A] ^= 0x04; /* 64 equal bytes between: a second extent */

    pattern(f->reg0, SPU_XDIFF_REG_BYTES, 0x5A);
    pattern(f->reg1, SPU_XDIFF_REG_BYTES, 0x77);
    set_entry_state(f);

    pattern(g_get, sizeof g_get, 0x55);
    pattern(g_line, sizeof g_line, 0x99);
    pattern(g_list, sizeof g_list, 0x33);
    pattern(g_elem0, sizeof g_elem0, 0x77);
    pattern(g_elem1, sizeof g_elem1, 0x88);

    const uint32_t line = add_blob(f, g_line, sizeof g_line);
    add(f, wrch(0x10, 22, 0x2)); /* MFC_WrTagMask */
    add(f, mfc_cmd(0x14, 0x40, 0x00A01000ULL, 0x8000, 1, 256, SPU_XDIFF_NO_BLOB));
    add(f, xfer(0x14, 0x40, SPU_XFER_TO_LS, 0x00A01000ULL, 0x8000, 256, 1, 0,
                add_blob(f, g_get, sizeof g_get)));
    add(f, wrch(0x18, 23, 0x2)); /* MFC_WrTagUpdate */
    add(f, rdch(0x1C, 24, 0x1));
    add(f, mfc_cmd(0x20, 0xD0, 0x00A02000ULL, 0x9000, 2, 128, SPU_XDIFF_NO_BLOB));
    add(f, xfer(0x20, 0xD0, SPU_XFER_TO_LS, 0x00A02000ULL, 0x9000, 128, 2, 4, line));
    add(f, rdch(0x24, 27, 0x4));
    /* An atomic keeps bit 2 and ignores MFC_Size: the stale 128 is legal and the
     * record carries no payload (spu_xdiff_cmd_is_list). */
    add(f, mfc_cmd(0x28, 0xB4, 0x00A02000ULL, 0x9000, 2, 128, SPU_XDIFF_NO_BLOB));
    add(f, xfer(0x28, 0xB4, SPU_XFER_FROM_LS, 0x00A02000ULL, 0x9000, 128, 2, 0, line));
    add(f, rdch(0x2C, 27, 0x0));
    add(f, rchcnt(0x30, 25, 0x1)); /* MFC_RdListStallStat */
    /* A list GET: the staged list is the request, at EAL. */
    add(f, mfc_cmd(0x34, 0x44, 0x000000000000C300ULL, 0x8100, 3, 16,
                   add_blob(f, g_list, sizeof g_list)));
    add(f, xfer(0x34, 0x40, SPU_XFER_TO_LS, 0x00A03000ULL, 0x8100, 16, 3, 0,
                add_blob(f, g_elem0, sizeof g_elem0)));
    add(f, xfer(0x34, 0x40, SPU_XFER_TO_LS, 0x00A04000ULL, 0x8110, 16, 3, 0,
                add_blob(f, g_elem1, sizeof g_elem1)));
    /* The stalled list is resumed: the elements still parked on tag 3. */
    add(f, stall_ack(0x38, 26, 3, 0xC310, 8, add_blob(f, g_list + 8, 8)));
    add(f, xfer(0x38, 0x40, SPU_XFER_TO_LS, 0x00A04000ULL, 0x8120, 8, 3, 0,
                add_blob(f, g_list + 8, 8)));
    add(f, xfer(0x3C, 0x89, SPU_XFER_ZERO_EA, 0x00A05000ULL, 0, 64, 4, 0,
                SPU_XDIFF_NO_BLOB));
    add(f, wrch(0x40, 28, 0x1));

    if (with_verdict) set_verdict(f);
}

/* A function unit reuses the state sections and carries only its liveness in FMAS. */
static void build_function_fixture(spu_xdiff_fixture* f)
{
    REQUIRE(spu_xdiff_fixture_init(f) == 0, "function fixture init");
    pattern(f->ls0 + IMG_LS, IMG_BYTES, 0x31);
    pattern(f->ls0 + 0x2000, 32, 0x45);
    memcpy(f->ls1, f->ls0, SPU_XDIFF_LS_BYTES);
    f->ls1[0x2000] ^= 0x80;
    pattern(f->reg0, SPU_XDIFF_REG_BYTES, 0x15);
    pattern(f->reg1, SPU_XDIFF_REG_BYTES, 0x25);
    f->identity.image_fingerprint = fnv64(FNV64_BASIS, f->ls0 + IMG_LS, IMG_BYTES);
    f->identity.authority.tier = (uint8_t)SPU_TIER_INTERP;
    f->identity.effect_port_abi = SPU_EFFECT_PORT_ABI;
    f->identity.context_census = 0xC0FFEE01u;
    f->identity.host_arch = SPU_XDIFF_HOST_ARM64;
    f->identity.host_isa = SPU_XDIFF_ISA_SCALAR;
    f->identity.float_rz = 1;
    f->unit.kind = SPU_XDIFF_UNIT_FUNCTION;
    f->unit.entry_pc = IMG_LS + 0x40u;
    f->unit.return_ls = IMG_LS + 0x44u;
    f->unit.image_ls = IMG_LS;
    f->unit.image_bytes = IMG_BYTES;
    f->unit.r1 = 0x3000u;
    f->function_mask.live_out[0] = (1ULL << 1) | (1ULL << 5);
    f->function_mask.dead_stack_bytes = 16;
    f->end.end_class = SPU_XDIFF_END_RETURN;
}

/* ---- the function unit with CODE ----------------------------------------- */

#define CODE0_LS    (IMG_LS + 0x40u)
#define CODE0_BYTES 0x20u
#define CODE1_LS    (IMG_LS + 0x100u)
#define CODE1_BYTES 0x40u
#define DIRTY_LS    (IMG_LS + 0x200u) /* data the job wrote inside its own image span */

/* The parent job's entry local store: its image span hashes to the fingerprint. */
static uint8_t g_staged[SPU_XDIFF_LS_BYTES];

static void code_ranges(spu_xdiff_code_range r[2])
{
    r[0].ls = CODE0_LS;
    r[0].bytes = CODE0_BYTES;
    r[1].ls = CODE1_LS;
    r[1].bytes = CODE1_BYTES;
}

static uint64_t f_fingerprint_of(const uint8_t* encoded)
{
    return rd64(section_bytes((uint8_t*)encoded, SPU_XDIFF_SEC_IDENTITY, NULL));
}

/* build_function_fixture, entered after the job wrote data inside its image span: the staged store
 * is what was built, the entry store has DIRTY_LS.. overwritten, and CODE names two ranges. */
static void build_code_fixture(spu_xdiff_fixture* f)
{
    spu_xdiff_code_range r[2];
    spu_xdiff_error err;
    build_function_fixture(f);
    memcpy(g_staged, f->ls0, SPU_XDIFF_LS_BYTES);
    pattern(f->ls0 + DIRTY_LS, 0x80, 0x77);
    memcpy(f->ls1, f->ls0, SPU_XDIFF_LS_BYTES);
    f->ls1[0x2000] ^= 0x80;
    code_ranges(r);
    REQUIRE(spu_xdiff_fixture_set_code(f, r, 2, g_staged, &err) == 0, "set_code: %s", err.detail);
}

/* ---- comparisons -------------------------------------------------------- */

static void compare_fixtures(const spu_xdiff_fixture* a, const spu_xdiff_fixture* b)
{
    REQUIRE(memcmp(&a->identity, &b->identity, sizeof a->identity) == 0, "IDEN differs");
    REQUIRE(memcmp(&a->unit, &b->unit, sizeof a->unit) == 0, "UNIT differs");
    REQUIRE(memcmp(a->reg0, b->reg0, SPU_XDIFF_REG_BYTES) == 0, "REG0 differs");
    REQUIRE(memcmp(&a->function_mask, &b->function_mask, sizeof a->function_mask) == 0, "FMAS differs");
    REQUIRE(memcmp(a->reg1, b->reg1, SPU_XDIFF_REG_BYTES) == 0, "REG1 differs");
    REQUIRE(memcmp(&a->end, &b->end, sizeof a->end) == 0, "END_ differs");
    REQUIRE(a->effect_count == b->effect_count, "effect count %u != %u", a->effect_count,
            b->effect_count);
    for (uint32_t i = 0; i < a->effect_count; i++)
        REQUIRE(memcmp(&a->effects[i], &b->effects[i], sizeof a->effects[i]) == 0,
                "EFCT record %u differs", i);
    REQUIRE(a->blob_bytes == b->blob_bytes, "BLOB is %u bytes, not %u", b->blob_bytes,
            a->blob_bytes);
    REQUIRE(a->blob_bytes == 0 || memcmp(a->blob, b->blob, a->blob_bytes) == 0,
            "BLOB bytes differ");
    REQUIRE(memcmp(a->ls0, b->ls0, SPU_XDIFF_LS_BYTES) == 0, "LS_0 decoded differently");
    REQUIRE(memcmp(a->ls1, b->ls1, SPU_XDIFF_LS_BYTES) == 0, "LS_1 decoded differently");
    REQUIRE(a->has_verdict == b->has_verdict, "VRDT presence differs");
    if (a->has_verdict)
        REQUIRE(memcmp(&a->verdict, &b->verdict, sizeof a->verdict) == 0, "VRDT differs");
    REQUIRE(a->code.count == b->code.count &&
                memcmp(&a->code, &b->code, sizeof a->code) == 0, "CODE differs");
    REQUIRE(a->staged_bytes == b->staged_bytes &&
                (a->staged_bytes == 0 || memcmp(a->staged, b->staged, a->staged_bytes) == 0),
            "STAG differs");
}

/* The container as section 7.1 states it, parsed without the decoder. */
static void check_container_v(const uint8_t* p, size_t n, uint32_t version)
{
    spu_xdiff_file_header h;
    memcpy(&h, p, sizeof h);
    REQUIRE(memcmp(h.magic, "SPUJOB\r\n", 8) == 0, "magic");
    REQUIRE(h.version == version, "version %u, want %u", (unsigned)h.version, (unsigned)version);
    REQUIRE(h.header_bytes == sizeof h, "header_bytes %u", (unsigned)h.header_bytes);
    REQUIRE(h.section_bytes == sizeof(spu_xdiff_section), "section_bytes %u",
            (unsigned)h.section_bytes);
    REQUIRE(h.file_bytes == (uint64_t)n, "file_bytes %llu, %zu written",
            (unsigned long long)h.file_bytes, n);
    REQUIRE(h.checksum == checksum_of(p, n), "checksum");
    REQUIRE(h.section_count >= 9 && h.section_count <= SPU_XDIFF_MAX_SECTIONS,
            "section_count %u", (unsigned)h.section_count);

    const uint64_t table_end = h.header_bytes + (uint64_t)h.section_count * h.section_bytes;

    /* Sections at 8-byte boundaries, none overlapping, padding zero between
     * consecutive ones (the table itself is covered by the checksum). */
    spu_xdiff_section s[SPU_XDIFF_MAX_SECTIONS];
    for (uint32_t i = 0; i < h.section_count; i++)
        memcpy(&s[i], p + h.header_bytes + (size_t)i * h.section_bytes, sizeof s[i]);
    for (uint32_t i = 0; i < h.section_count; i++) {
        REQUIRE(s[i].offset % 8 == 0, "section %u is not 8-byte aligned", (unsigned)i);
        REQUIRE(s[i].offset >= table_end, "section %u starts inside the table", (unsigned)i);
        REQUIRE(s[i].offset + s[i].bytes <= n, "section %u runs past the file", (unsigned)i);
        for (uint32_t j = i + 1; j < h.section_count; j++) {
            const int disjoint = s[i].bytes == 0 || s[j].bytes == 0 ||
                                 s[i].offset + s[i].bytes <= s[j].offset ||
                                 s[j].offset + s[j].bytes <= s[i].offset;
            REQUIRE(disjoint, "sections %u and %u overlap", (unsigned)i, (unsigned)j);
        }
    }
    uint64_t at = table_end;
    for (uint32_t i = 0; i < h.section_count; i++) {
        uint32_t best = i;
        for (uint32_t j = i + 1; j < h.section_count; j++)
            if (s[j].offset < s[best].offset) best = j;
        const spu_xdiff_section t = s[best];
        s[best] = s[i];
        s[i] = t;
        for (uint64_t k = at; k < s[i].offset; k++)
            REQUIRE(p[k] == 0, "padding at 0x%llx is not zero", (unsigned long long)k);
        if (s[i].bytes) at = s[i].offset + s[i].bytes;
    }
}

static void check_container(const uint8_t* p, size_t n)
{
    check_container_v(p, n, SPU_XDIFF_VERSION_V2);
}

/* ---- positive tests ----------------------------------------------------- */

static void test_fingerprint_known_answers(void)
{
    static const uint8_t bytes16[16] = { 0, 1, 2,  3,  4,  5,  6,  7,
                                         8, 9, 10, 11, 12, 13, 14, 15 };
    REQUIRE(fnv64(FNV64_BASIS, "", 0) == 0x14650FB0739D0383ULL, "FNV-1a-64 of nothing");
    REQUIRE(fnv64(FNV64_BASIS, bytes16, sizeof bytes16) == 0x088B84CE5271B343ULL,
            "FNV-1a-64 of 0..15 is 0x%016llx",
            (unsigned long long)fnv64(FNV64_BASIS, bytes16, sizeof bytes16));
    printf("ok FNV-1a-64 known answers\n");
}

static void roundtrip(int with_verdict)
{
    spu_xdiff_fixture f, g;
    spu_xdiff_error err;
    uint8_t* bytes = NULL;
    size_t n = 0;

    build_full(&f, with_verdict);
    REQUIRE(spu_xdiff_fixture_encode(&f, &bytes, &n, &err) == 0, "encode: %s", err.detail);
    REQUIRE(bytes != NULL && n > 0, "encode produced no bytes");
    check_container(bytes, n);

    REQUIRE(spu_xdiff_fixture_decode(bytes, n, &g, &err) == 0, "decode: %s", err.detail);
    compare_fixtures(&f, &g);
    REQUIRE(g.has_verdict == with_verdict, "VRDT presence %d", g.has_verdict);
    REQUIRE(g.blob_index == NULL, "a decoded fixture carries no payload table");

    if (with_verdict) {
        /* The transcript of build_full: 19 records, six distinct payloads. */
        REQUIRE(g.effect_count == 19, "record count %u", g.effect_count);
        REQUIRE(g.blob_bytes == 440, "the payload store is %u bytes", g.blob_bytes);
        REQUIRE(g.effects[6].blob == g.effects[9].blob,
                "the atomic line is stored once (0x%x != 0x%x)", g.effects[6].blob,
                g.effects[9].blob);
        REQUIRE(g.effects[15].blob == g.effects[16].blob,
                "the parked list's tail is stored once (0x%x != 0x%x)", g.effects[15].blob,
                g.effects[16].blob);
        REQUIRE(g.effects[8].kind == SPU_FX_MFC_CMD && g.effects[8].value == 0xB4 &&
                    g.effects[8].blob == SPU_XDIFF_NO_BLOB && g.effects[8].size == 128,
                "the PUTLLC request with a stale MFC_Size survives");
    }
    /* The same bytes encode the same way twice. */
    {
        uint8_t* again = NULL;
        size_t again_n = 0;
        REQUIRE(spu_xdiff_fixture_encode(&f, &again, &again_n, &err) == 0, "encode twice: %s",
                err.detail);
        REQUIRE(again_n == n && memcmp(again, bytes, n) == 0, "the encoding is not stable");
        free(again);
    }
    free(bytes);
    spu_xdiff_fixture_free(&f);
    spu_xdiff_fixture_free(&g);
    printf("ok roundtrip (%s)\n", with_verdict ? "with VRDT" : "no VRDT");
}

static void function_roundtrip(void)
{
    spu_xdiff_fixture f, g;
    spu_xdiff_error err;
    uint8_t* bytes = NULL;
    size_t n = 0;
    uint64_t mask_bytes = 0;
    build_function_fixture(&f);
    REQUIRE(spu_xdiff_fixture_encode(&f, &bytes, &n, &err) == 0, "function encode: %s", err.detail);
    check_container(bytes, n);
    (void)section_bytes(bytes, SPU_XDIFF_SEC_FMASK, &mask_bytes);
    REQUIRE(mask_bytes == sizeof(spu_xdiff_function_mask), "FMAS size %llu",
            (unsigned long long)mask_bytes);
    REQUIRE(spu_xdiff_fixture_decode(bytes, n, &g, &err) == 0, "function decode: %s", err.detail);
    compare_fixtures(&f, &g);
    free(bytes);
    spu_xdiff_fixture_free(&f);
    spu_xdiff_fixture_free(&g);
    printf("ok function unit roundtrip\n");
}

/* An empty transcript and an empty payload store are legal (section 7.2). */
static void test_empty_transcript(void)
{
    spu_xdiff_fixture f, g;
    spu_xdiff_error err;
    uint8_t* bytes = NULL;
    size_t n = 0;
    build_minimal(&f);
    /* EFCT and BLOB may be empty (section 7.2); the arrays keep their
     * allocations, only the counts go. */
    f.effect_count = 0;
    f.blob_bytes = 0;
    REQUIRE(spu_xdiff_fixture_encode(&f, &bytes, &n, &err) == 0, "encode empty: %s",
            err.detail);
    REQUIRE(spu_xdiff_fixture_decode(bytes, n, &g, &err) == 0, "decode empty: %s", err.detail);
    REQUIRE(g.effect_count == 0 && g.blob_bytes == 0, "empty sections stay empty");
    compare_fixtures(&f, &g);
    free(bytes);
    spu_xdiff_fixture_free(&f);
    spu_xdiff_fixture_free(&g);
    printf("ok empty transcript and payload store\n");
}

static void test_payload_store(void)
{
    spu_xdiff_fixture f;
    uint8_t a[64], b[64];
    REQUIRE(spu_xdiff_fixture_init(&f) == 0, "fixture_init");
    pattern(a, sizeof a, 1);
    memcpy(b, a, sizeof b);
    b[10] ^= 1;

    const uint32_t a1 = spu_xdiff_fixture_add_blob(&f, a, sizeof a, 0);
    const uint32_t a2 = spu_xdiff_fixture_add_blob(&f, a, sizeof a, 0);
    const uint32_t b1 = spu_xdiff_fixture_add_blob(&f, b, sizeof b, 0);
    REQUIRE(a1 == a2, "one payload stored twice has one offset (0x%x != 0x%x)", a1, a2);
    REQUIRE(a1 != b1, "same length, other bytes: its own copy");
    REQUIRE(f.blob_bytes == 2 * sizeof a, "two copies, not three (%u bytes)", f.blob_bytes);
    REQUIRE(spu_xdiff_fixture_blob_failed(&f) == 0, "no failure");

    REQUIRE(spu_xdiff_fixture_add_blob(&f, a, 0, 0) == SPU_XDIFF_NO_BLOB, "a zero payload");
    REQUIRE(spu_xdiff_fixture_blob_failed(&f) == 0, "a zero payload is not a failure");

    {
        uint8_t c[64];
        pattern(c, sizeof c, 0x77);
        REQUIRE(spu_xdiff_fixture_add_blob(&f, c, sizeof c, 64) == SPU_XDIFF_NO_BLOB,
                "a payload past max_blob_bytes");
        REQUIRE(spu_xdiff_fixture_blob_failed(&f) == 1, "a bound hit is a failure");
        REQUIRE(f.blob_bytes == 2 * sizeof a, "a refused payload stores nothing");
    }

    REQUIRE(spu_xdiff_fixture_add_blob(&f, a, sizeof a, 128) == a1,
            "an already stored payload is answered before the bound");

    /* many payloads, so the table grows and the offsets stay right */
    for (uint32_t i = 0; i < 500; i++) {
        uint8_t p[8];
        memset(p, 0, sizeof p);
        wr32(p, i); /* distinct even where a repeating pattern would collide */
        pattern(p + 4, 4, 0x300u + i);
        const uint32_t at = spu_xdiff_fixture_add_blob(&f, p, sizeof p, 0);
        REQUIRE(at != SPU_XDIFF_NO_BLOB, "payload %u", i);
        REQUIRE(at == f.blob_bytes - sizeof p, "payload %u lands at the top", i);
        REQUIRE(spu_xdiff_fixture_add_blob(&f, p, sizeof p, 0) == at, "payload %u dedup", i);
    }
    REQUIRE(spu_xdiff_fixture_blob_failed(&f) == 0, "no failure after growth");
    spu_xdiff_fixture_free(&f);
    printf("ok payload store dedup, bounds and growth\n");
}

static void test_clear(void)
{
    spu_xdiff_fixture f, g;
    spu_xdiff_error err;
    uint8_t* bytes = NULL;
    size_t n = 0;
    build_full(&f, 1);

    uint8_t* const ls0 = f.ls0;
    uint8_t* const ls1 = f.ls1;
    spu_xdiff_effect* const effects = f.effects;
    uint8_t* const blob = f.blob;
    const uint32_t effects_cap = f.effect_cap;
    const uint32_t blob_cap = f.blob_cap;

    spu_xdiff_fixture_clear(&f);
    REQUIRE(f.ls0 == ls0 && f.ls1 == ls1, "the local stores are kept");
    REQUIRE(f.effects == effects && f.blob == blob, "the record and payload arrays are kept");
    REQUIRE(f.effect_cap == effects_cap && f.blob_cap == blob_cap, "the capacities are kept");
    REQUIRE(f.effect_count == 0 && f.blob_bytes == 0, "the records are gone");
    REQUIRE(f.has_verdict == 0, "the verdict is gone");
    REQUIRE(f.blob_index == NULL, "the payload table is gone");
    for (size_t i = 0; i < SPU_XDIFF_LS_BYTES; i++) {
        REQUIRE(f.ls0[i] == 0, "LS_0 at 0x%zx is not zero", i);
        REQUIRE(f.ls1[i] == 0, "LS_1 at 0x%zx is not zero", i);
    }
    {
        spu_xdiff_identity zero_identity;
        spu_xdiff_unit zero_unit;
        spu_xdiff_end zero_end;
        memset(&zero_identity, 0, sizeof zero_identity);
        memset(&zero_unit, 0, sizeof zero_unit);
        memset(&zero_end, 0, sizeof zero_end);
        REQUIRE(memcmp(&f.identity, &zero_identity, sizeof f.identity) == 0, "IDEN is not zero");
        REQUIRE(memcmp(&f.unit, &zero_unit, sizeof f.unit) == 0, "UNIT is not zero");
        REQUIRE(memcmp(&f.end, &zero_end, sizeof f.end) == 0, "END_ is not zero");
    }

    /* Refill the same allocation with another, smaller job. */
    pattern(f.ls0 + 0x2000, 64, 0x13);
    set_entry_state(&f);
    f.identity.image_fingerprint = fnv64(FNV64_BASIS, f.ls0 + IMG_LS, IMG_BYTES);
    memcpy(f.ls1, f.ls0, SPU_XDIFF_LS_BYTES);
    add(&f, wrch(0x50, 28, 0x7));
    REQUIRE(spu_xdiff_fixture_encode(&f, &bytes, &n, &err) == 0, "re-encode: %s", err.detail);
    REQUIRE(spu_xdiff_fixture_decode(bytes, n, &g, &err) == 0, "re-decode: %s", err.detail);
    compare_fixtures(&f, &g);
    free(bytes);
    spu_xdiff_fixture_free(&f);
    spu_xdiff_fixture_free(&g);
    printf("ok clear keeps the allocations and reuse round-trips\n");
}

static void test_name(void)
{
    char buf[64];
    const int n = spu_xdiff_fixture_name(buf, sizeof buf, 0x0123456789ABCDEFULL, 3, 42);
    REQUIRE(strcmp(buf, "0123456789abcdef-q3-s42.spujob") == 0, "name is \"%s\"", buf);
    REQUIRE(n == (int)strlen(buf), "length is %d, the name is %zu bytes", n, strlen(buf));

    char small[8];
    const int m = spu_xdiff_fixture_name(small, sizeof small, 0x0123456789ABCDEFULL, 3, 42);
    REQUIRE(m == n, "truncation reports the full length (%d != %d)", m, n);
    REQUIRE(strlen(small) == sizeof small - 1, "truncated name is %zu bytes", strlen(small));
    printf("ok file name \"%s\"\n", buf);
}

static void test_files(void)
{
    /* One name per process: ctest runs this binary as spu_xdiff_fixture and spu_xdiff_fixture_sweep, in
     * parallel, in one directory; a shared name let one run remove the other's file. */
    char path_buf[64];
    snprintf(path_buf, sizeof path_buf, "test_spu_xdiff_fixture.%ld.spujob", (long)getpid());
    const char* path = path_buf;
    spu_xdiff_fixture f, g;
    spu_xdiff_error err;

    build_full(&f, 1);
    remove(path);
    REQUIRE(spu_xdiff_fixture_write(&f, path, &err) == 0, "write: %s", err.detail);
    REQUIRE(count_partials(path) == 0, "%d temp file(s) of %s left behind", count_partials(path), path);
    REQUIRE(spu_xdiff_fixture_read(path, &g, &err) == 0, "read: %s", err.detail);
    compare_fixtures(&f, &g);
    spu_xdiff_fixture_free(&g);

    /* Writing over a fixture that is already there replaces it. */
    REQUIRE(spu_xdiff_fixture_write(&f, path, &err) == 0, "rewrite: %s", err.detail);
    REQUIRE(spu_xdiff_fixture_read(path, &g, &err) == 0, "re-read: %s", err.detail);
    compare_fixtures(&f, &g);
    spu_xdiff_fixture_free(&g);

    REQUIRE(spu_xdiff_fixture_read("test_spu_xdiff_fixture.absent.spujob", &g, &err) == -1,
            "a missing file reads as a fixture");
    REQUIRE(err.reason == SPU_XDIFF_R_NO_FIXTURE, "missing file is %s",
            spu_xdiff_reason_id(err.reason));

    /* A file that is not a fixture is malformed, not absent. */
    {
        FILE* fp = fopen(path, "wb");
        REQUIRE(fp != NULL, "cannot write a junk file");
        fputs("not a fixture", fp);
        fclose(fp);
        REQUIRE(spu_xdiff_fixture_read(path, &g, &err) == -1, "junk reads as a fixture");
        REQUIRE(err.reason == SPU_XDIFF_R_FIXTURE_FORMAT, "junk is %s: %s",
                spu_xdiff_reason_id(err.reason), err.detail);
    }
    remove(path);
    spu_xdiff_fixture_free(&f);
    printf("ok write/read through a per-process temp file\n");
}

/* The encoder refuses what the decoder refuses: it never writes a file its own
 * reader would answer without. */
static void test_encoder_refuses(void)
{
    spu_xdiff_fixture f;
    spu_xdiff_error err;
    uint8_t* bytes = NULL;
    size_t n = 0;

    build_minimal(&f);
    f.effects[0].kind = 9;
    REQUIRE(spu_xdiff_fixture_encode(&f, &bytes, &n, &err) == -1, "the encoder wrote a bad record");
    REQUIRE(bytes == NULL && n == 0, "no bytes on failure");
    REQUIRE(err.reason == SPU_XDIFF_R_CAPTURE_LIMIT, "encode answers %s",
            spu_xdiff_reason_id(err.reason));
    REQUIRE(strstr(err.detail, "EFCT") != NULL, "detail \"%s\" names EFCT", err.detail);

    f.effects[0].kind = SPU_FX_WRCH;
    f.identity.image_fingerprint ^= 1;
    REQUIRE(spu_xdiff_fixture_encode(&f, &bytes, &n, &err) == -1,
            "the encoder wrote a stale fingerprint");
    REQUIRE(err.reason == SPU_XDIFF_R_CAPTURE_LIMIT, "encode answers %s",
            spu_xdiff_reason_id(err.reason));
    REQUIRE(strstr(err.detail, "IDEN") != NULL, "detail \"%s\" names IDEN", err.detail);

    f.identity.image_fingerprint ^= 1;
    REQUIRE(spu_xdiff_fixture_encode(&f, &bytes, &n, &err) == 0, "the restored fixture: %s",
            err.detail);
    free(bytes);
    spu_xdiff_fixture_free(&f);
    printf("ok the encoder refuses a record and an identity it would not read\n");
}

/* ---- negative cases (section 7.8) --------------------------------------- */

static void expect_reject(const char* label, uint8_t* p, size_t n, uint32_t reason,
                          const char* where)
{
    spu_xdiff_fixture f;
    spu_xdiff_error err;
    memset(&err, 0, sizeof err);
    memset(&f, 0, sizeof f);
    const int rc = spu_xdiff_fixture_decode(p, n, &f, &err);
    REQUIRE(rc == -1, "%s: the decoder accepted it", label);
    REQUIRE(err.reason == reason, "%s: reason is %s (%u), want %s (%u): %s", label,
            spu_xdiff_reason_id(err.reason), (unsigned)err.reason,
            spu_xdiff_reason_id(reason), (unsigned)reason, err.detail);
    REQUIRE(where == NULL || strstr(err.detail, where) != NULL,
            "%s: detail \"%s\" does not name %s", label, err.detail, where);
    printf("ok reject %-36s %-12s %s\n", label, spu_xdiff_reason_id(err.reason), err.detail);
    free(p);
}

static uint8_t* encode_of(spu_xdiff_fixture* f, size_t* n)
{
    spu_xdiff_error err;
    uint8_t* bytes = NULL;
    REQUIRE(spu_xdiff_fixture_encode(f, &bytes, n, &err) == 0, "encode: %s", err.detail);
    return bytes;
}

static uint8_t* encode_minimal(size_t* n)
{
    spu_xdiff_fixture f;
    build_minimal(&f);
    uint8_t* p = encode_of(&f, n);
    spu_xdiff_fixture_free(&f);
    return p;
}

static uint8_t* encode_full(size_t* n)
{
    spu_xdiff_fixture f;
    build_full(&f, 1);
    uint8_t* p = encode_of(&f, n);
    spu_xdiff_fixture_free(&f);
    return p;
}

static uint8_t* encode_code_fixture(size_t* n)
{
    spu_xdiff_fixture f;
    build_code_fixture(&f);
    uint8_t* p = encode_of(&f, n);
    spu_xdiff_fixture_free(&f);
    return p;
}

static void test_v1_job_compatibility(void)
{
    spu_xdiff_fixture original, decoded;
    spu_xdiff_error err;
    size_t bytes_count = 0;
    build_minimal(&original);
    uint8_t* bytes = encode_of(&original, &bytes_count);
    wr32(bytes + offsetof(spu_xdiff_file_header, version), SPU_XDIFF_VERSION_V1);
    fix_checksum(bytes, bytes_count);
    memset(&decoded, 0, sizeof decoded);
    REQUIRE(spu_xdiff_fixture_decode(bytes, bytes_count, &decoded, &err) == 0,
            "version-1 job fixture did not decode: %s", err.detail);
    REQUIRE(decoded.unit.kind == SPU_XDIFF_UNIT_JOB, "version-1 fixture kind changed");
    compare_fixtures(&original, &decoded);
    free(bytes);
    spu_xdiff_fixture_free(&original);
    spu_xdiff_fixture_free(&decoded);
}

static void test_v1_reader_rejects_v2_function_fixture(void)
{
    spu_xdiff_fixture function_fixture, decoded;
    spu_xdiff_error err;
    size_t bytes_count = 0;
    build_function_fixture(&function_fixture);
    uint8_t* bytes = encode_of(&function_fixture, &bytes_count);
    memset(&decoded, 0, sizeof decoded);
    REQUIRE(spu_xdiff_fixture_decode_max_version(bytes, bytes_count, SPU_XDIFF_VERSION_V1,
                                                 &decoded, &err) == -1,
            "version-1 reader accepted a version-2 function fixture");
    REQUIRE(err.reason == SPU_XDIFF_R_UNSUPPORTED_VERSION &&
                strstr(err.detail, "version 2") != NULL,
            "old reader did not name the unsupported version: %s", err.detail);
    free(bytes);
    spu_xdiff_fixture_free(&function_fixture);
    spu_xdiff_fixture_free(&decoded);
}

static void test_rejects(void)
{
    /* A zstd frame is refused by name, before anything is read as a header. */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        p[0] = 0x28;
        p[1] = 0xB5;
        p[2] = 0x2F;
        p[3] = 0xFD;
        expect_reject("zstd frame", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "zstd");
    }
    /* header fields */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        p[0] ^= 0xFF;
        fix_checksum(p, n);
        expect_reject("magic", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "header");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        wr32(p + offsetof(spu_xdiff_file_header, version), SPU_XDIFF_VERSION + 1u);
        fix_checksum(p, n);
        expect_reject("unsupported version", p, n, SPU_XDIFF_R_UNSUPPORTED_VERSION, "version");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        wr32(p + 12, 32);
        fix_checksum(p, n);
        expect_reject("header_bytes", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "header");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        wr32(p + 20, 16);
        fix_checksum(p, n);
        expect_reject("section_bytes", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "header");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        wr32(p + 16, SPU_XDIFF_MAX_SECTIONS + 1);
        fix_checksum(p, n);
        expect_reject("section_count over the bound", p, n, SPU_XDIFF_R_FIXTURE_FORMAT,
                      "header");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        wr64(p + 24, (uint64_t)n + 8);
        fix_checksum(p, n);
        expect_reject("file_bytes not the file's size", p, n, SPU_XDIFF_R_FIXTURE_FORMAT,
                      "header");
    }
    /* truncated: the same bytes, one short */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        expect_reject("truncated", p, n - 1, SPU_XDIFF_R_FIXTURE_FORMAT, "header");
    }
    /* a header byte only the checksum reads, and the checksum field itself */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        p[HDR_CHECKSUM + 5] ^= 0x01;
        expect_reject("checksum field byte", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "checksum");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint64_t iden_bytes = 0;
        uint8_t* iden = section_bytes(p, SPU_XDIFF_SEC_IDENTITY, &iden_bytes);
        REQUIRE(iden_bytes == 80, "IDEN is %llu bytes", (unsigned long long)iden_bytes);
        iden[20] ^= 0x01; /* IDEN's content is not checked field by field */
        expect_reject("body byte", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "checksum");
    }
    /* section spans */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* e = section_entry(p, SPU_XDIFF_SEC_LS0);
        REQUIRE(e != NULL, "LS_0 entry");
        wr64(e + 8, (uint64_t)n - 8); /* inside the file, but its bytes run out */
        fix_checksum(p, n);
        expect_reject("section outside the file", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "LS_0");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* a = section_entry(p, SPU_XDIFF_SEC_LS0);
        uint8_t* b = section_entry(p, SPU_XDIFF_SEC_LS1);
        REQUIRE(a != NULL && b != NULL, "LS_0 and LS_1 entries");
        wr64(b + 8, rd64(a + 8));
        fix_checksum(p, n);
        expect_reject("overlapping sections", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "LS_1");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* e = section_entry(p, SPU_XDIFF_SEC_BLOB);
        REQUIRE(e != NULL, "BLOB entry");
        wr64(e + 8, rd64(e + 8) + 4);
        fix_checksum(p, n);
        expect_reject("misaligned section", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "BLOB");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* e = section_entry(p, SPU_XDIFF_SEC_END);
        REQUIRE(e != NULL, "END_ entry");
        wr64(e + 16, 8); /* END_ is the last of the nine required sections */
        fix_checksum(p, n);
        expect_reject("wrong section size", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "END_");
    }
    /* UNIT names the entry state a replay is built from: a recording that
     * failed before its loader filled it must be refused, not replayed. */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* unit = section_bytes(p, SPU_XDIFF_SEC_UNIT, NULL);
        wr32(unit + 0, 0); /* kind */
        fix_checksum(p, n);
        expect_reject("UNIT kind 0", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "UNIT");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* unit = section_bytes(p, SPU_XDIFF_SEC_UNIT, NULL);
        wr32(unit + 4, 3); /* profile */
        fix_checksum(p, n);
        expect_reject("UNIT profile outside 1..2", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "UNIT");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* unit = section_bytes(p, SPU_XDIFF_SEC_UNIT, NULL);
        wr32(unit + 8, 2); /* entry_pc, not a word boundary */
        fix_checksum(p, n);
        expect_reject("UNIT entry_pc misaligned", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "UNIT");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* unit = section_bytes(p, SPU_XDIFF_SEC_UNIT, NULL);
        wr32(unit + 8, SPU_XDIFF_LS_BYTES); /* entry_pc past the store */
        fix_checksum(p, n);
        expect_reject("UNIT entry_pc outside LS", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "UNIT");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* unit = section_bytes(p, SPU_XDIFF_SEC_UNIT, NULL);
        wr32(unit + 32, 0); /* image_bytes: the identity would cover nothing */
        fix_checksum(p, n);
        expect_reject("UNIT image_bytes 0", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "UNIT");
    }
    /* END_'s class: an unclassified end is not comparable (section 7.6) */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* end = section_bytes(p, SPU_XDIFF_SEC_END, NULL);
        wr32(end + 0, 0); /* end_class */
        fix_checksum(p, n);
        expect_reject("END_ end_class 0", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "END_");
    }
    /* only MFC_WrListStallAck carries a remaining list (section 6) */
    {
        size_t n;
        uint8_t* p = encode_full(&n);
        uint8_t* efct = section_bytes(p, SPU_XDIFF_SEC_EFFECTS, NULL);
        REQUIRE(rd32(efct + 15 * 40 + 8) == 3 && efct[15 * 40] == SPU_FX_WRCH,
                "record 15 is the stall-ack WRCH");
        efct[15 * 40 + 1] = 22; /* channel WrTagMask, still with lsa/size/blob */
        fix_checksum(p, n);
        expect_reject("WRCH payload on another channel", p, n, SPU_XDIFF_R_FIXTURE_FORMAT,
                      "EFCT");
    }
    /* a transfer follows the write that caused it, not a read */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* efct = section_bytes(p, SPU_XDIFF_SEC_EFFECTS, NULL);
        memmove(efct + 40, efct + 3 * 40, 40); /* record 1 becomes an RDCH */
        fix_checksum(p, n);
        expect_reject("XFER after a channel read", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "EFCT");
    }
    /* the padding outside every section is zero (7.1) */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* q = (uint8_t*)calloc(1, n + 8);
        REQUIRE(q != NULL, "calloc");
        memcpy(q, p, n);
        wr64(q + 24, (uint64_t)n + 8); /* the file is one 8-byte pad longer */
        q[n] = 0x7F;
        fix_checksum(q, n + 8);
        free(p);
        expect_reject("padding is not zero", q, n + 8, SPU_XDIFF_R_FIXTURE_FORMAT, "padding");
    }
    /* a gap between two sections must be zero too: insert 8 bytes after LS_0
     * and move every later section with them, so only the padding rule breaks */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* ls0 = section_entry(p, SPU_XDIFF_SEC_LS0);
        REQUIRE(ls0 != NULL, "LS_0 entry");
        const uint64_t cut = rd64(ls0 + 8) + rd64(ls0 + 16);
        REQUIRE(cut + 8 <= n, "the gap fits in the file");
        uint8_t* q = (uint8_t*)calloc(1, n + 8);
        REQUIRE(q != NULL, "calloc");
        memcpy(q, p, (size_t)cut);
        memset(q + cut, 0xA5, 8);
        memcpy(q + cut + 8, p + cut, n - (size_t)cut);
        spu_xdiff_file_header h;
        memcpy(&h, q, sizeof h);
        for (uint32_t i = 0; i < h.section_count; i++) {
            uint8_t* e = q + h.header_bytes + (size_t)i * h.section_bytes;
            if (rd64(e + 8) >= cut) wr64(e + 8, rd64(e + 8) + 8);
        }
        wr64(q + 24, (uint64_t)n + 8);
        fix_checksum(q, n + 8);
        free(p);
        expect_reject("gap between sections not zero", q, n + 8, SPU_XDIFF_R_FIXTURE_FORMAT,
                      "padding before EFCT");
    }
    /* a required section missing, and a duplicate that hides one */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        spu_xdiff_file_header h;
        memcpy(&h, p, sizeof h);
        const uint8_t* e = p + h.header_bytes + (size_t)(h.section_count - 1) * h.section_bytes;
        REQUIRE(rd32(e) == SPU_XDIFF_SEC_END, "END_ is the last table entry");
        wr32(p + 16, h.section_count - 1);
        fix_checksum(p, n);
        expect_reject("required section missing", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "END_");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* e = section_entry(p, SPU_XDIFF_SEC_END);
        REQUIRE(e != NULL, "END_ entry");
        wr32(e, SPU_XDIFF_SEC_REG1);
        fix_checksum(p, n);
        expect_reject("required section duplicated", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "REG1");
    }
    /* an unknown tag: a reader answers fixture-format, naming the tag */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* e = section_entry(p, SPU_XDIFF_SEC_BLOB);
        REQUIRE(e != NULL, "BLOB entry");
        wr32(e, SPU_XDIFF_FOURCC('Z', 'Z', 'Z', 'Z'));
        fix_checksum(p, n);
        expect_reject("unknown section tag", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "ZZZZ");
    }
    /* effect records */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* efct = section_bytes(p, SPU_XDIFF_SEC_EFFECTS, NULL);
        efct[0] = 9; /* record 0, kind */
        fix_checksum(p, n);
        expect_reject("unknown effect kind", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "EFCT");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* efct = section_bytes(p, SPU_XDIFF_SEC_EFFECTS, NULL);
        efct[36] = 1; /* record 0, reserved */
        fix_checksum(p, n);
        expect_reject("nonzero unused field", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "EFCT");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* efct = section_bytes(p, SPU_XDIFF_SEC_EFFECTS, NULL);
        wr32(efct + 2 * 40 + 32, 24); /* record 2: its payload at 24..56 of a 32-byte BLOB */
        fix_checksum(p, n);
        expect_reject("blob + size outside BLOB", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "EFCT");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* efct = section_bytes(p, SPU_XDIFF_SEC_EFFECTS, NULL);
        wr32(efct + 2 * 40 + 24, SPU_XDIFF_LS_BYTES - 8); /* record 2: lsa + 32 */
        fix_checksum(p, n);
        expect_reject("lsa + size outside LS", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "EFCT");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* efct = section_bytes(p, SPU_XDIFF_SEC_EFFECTS, NULL);
        memmove(efct, efct + 2 * 40, 40); /* record 0 becomes the first XFER */
        fix_checksum(p, n);
        expect_reject("XFER with no command before it", p, n, SPU_XDIFF_R_FIXTURE_FORMAT,
                      "EFCT");
    }
    /* local-store extents */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* ls0 = section_bytes(p, SPU_XDIFF_SEC_LS0, NULL);
        uint8_t* second = extent_header(ls0, 1);
        wr32(second, 0x800); /* below the first extent's lsa */
        fix_checksum(p, n);
        expect_reject("unsorted extents", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "LS_0");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* ls0 = section_bytes(p, SPU_XDIFF_SEC_LS0, NULL);
        uint8_t* second = extent_header(ls0, 1);
        wr32(second, rd32(ls0) + 0x100); /* inside the first extent */
        fix_checksum(p, n);
        expect_reject("overlapping extents", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "LS_0");
    }
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint8_t* ls0 = section_bytes(p, SPU_XDIFF_SEC_LS0, NULL);
        uint8_t* second = extent_header(ls0, 1);
        wr32(second, SPU_XDIFF_LS_BYTES - 8); /* 32 bytes from there */
        fix_checksum(p, n);
        expect_reject("extent outside local store", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "LS_0");
    }
    /* the identity: a well-formed file whose image bytes do not hash to IDEN */
    {
        size_t n;
        uint8_t* p = encode_minimal(&n);
        uint64_t ls0_bytes = 0;
        uint8_t* ls0 = section_bytes(p, SPU_XDIFF_SEC_LS0, &ls0_bytes);
        REQUIRE(rd32(ls0) == IMG_LS, "the first extent starts at the image");
        ls0[8 + 5] ^= 0x01;
        fix_checksum(p, n);
        expect_reject("image fingerprint mismatch", p, n, SPU_XDIFF_R_FIXTURE_IDENTITY, "IDEN");
    }
    /* VRDT is bounds-checked only, but its size is still its type's */
    {
        size_t n;
        uint8_t* p = encode_full(&n);
        uint8_t* e = section_entry(p, SPU_XDIFF_SEC_VERDICT);
        REQUIRE(e != NULL, "VRDT entry");
        wr64(e + 16, 140);
        fix_checksum(p, n);
        expect_reject("wrong VRDT size", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "VRDT");
    }
}

/* A count bound must answer without the file holding the records it counts:
 * the declared size of a section is checked before its span. */
static void test_synthesized_header_bounds(void)
{
    enum { N = 9, HDR = 40, SEC = 24 };
    static const uint32_t tags[N] = {
        SPU_XDIFF_SEC_IDENTITY, SPU_XDIFF_SEC_UNIT, SPU_XDIFF_SEC_REG0, SPU_XDIFF_SEC_LS0,
        SPU_XDIFF_SEC_EFFECTS,  SPU_XDIFF_SEC_BLOB, SPU_XDIFF_SEC_REG1, SPU_XDIFF_SEC_LS1,
        SPU_XDIFF_SEC_END,
    };
    const uint64_t sizes[N] = {
        sizeof(spu_xdiff_identity), sizeof(spu_xdiff_unit), SPU_XDIFF_REG_BYTES,
        0, (uint64_t)(SPU_XDIFF_MAX_EFFECTS + 1u) * sizeof(spu_xdiff_effect), 0,
        SPU_XDIFF_REG_BYTES, 0, sizeof(spu_xdiff_end),
    };
    const size_t n = HDR + N * SEC;

    for (int blob = 0; blob < 2; blob++) {
        uint8_t* p = (uint8_t*)calloc(1, n);
        REQUIRE(p != NULL, "calloc");
        memcpy(p, "SPUJOB\r\n", 8);
        wr32(p + 8, SPU_XDIFF_VERSION);
        wr32(p + 12, HDR);
        wr32(p + 16, N);
        wr32(p + 20, SEC);
        wr64(p + 24, n);
        for (int i = 0; i < N; i++) {
            uint64_t size = sizes[i];
            if (blob && tags[i] == SPU_XDIFF_SEC_BLOB) size = (uint64_t)SPU_XDIFF_MAX_BLOB_BYTES + 1u;
            if (blob && tags[i] == SPU_XDIFF_SEC_EFFECTS) size = 0;
            uint8_t* e = p + HDR + (size_t)i * SEC;
            wr32(e, tags[i]);
            wr32(e + 4, 0);
            wr64(e + 8, n);
            wr64(e + 16, size);
        }
        fix_checksum(p, n);
        if (blob)
            expect_reject("blob over the bound", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "BLOB");
        else
            expect_reject("effect count over the bound", p, n, SPU_XDIFF_R_FIXTURE_FORMAT,
                          "EFCT");
    }
    printf("ok synthesized header bounds\n");
}

/* Every prefix of a valid fixture is refused as a format error: a short file
 * is a truncated one (7.1), and the reader must never walk a section whose
 * bytes are not there. Run under ASan, this is the bounds-check sweep. */
static void test_truncations(void)
{
    size_t n = 0;
    uint8_t* p = encode_full(&n);
    for (size_t k = 0; k < n; k++) {
        spu_xdiff_fixture f;
        spu_xdiff_error err;
        memset(&err, 0, sizeof err);
        memset(&f, 0, sizeof f);
        if (spu_xdiff_fixture_decode(p, k, &f, &err) != -1) {
            spu_xdiff_fixture_free(&f);
            REQUIRE(0, "a %zu-byte prefix of a %zu-byte fixture decoded", k, n);
        }
        REQUIRE(err.reason == SPU_XDIFF_R_FIXTURE_FORMAT,
                "a %zu-byte prefix answered %s: %s", k, spu_xdiff_reason_id(err.reason),
                err.detail);
        REQUIRE(err.detail[0] != 0, "a %zu-byte prefix has no detail", k);
    }
    free(p);
    printf("ok %zu truncations refused as fixture-format\n", n);
}

static void mutation_result(const uint8_t* bytes, size_t size, int index, size_t at,
                            uint32_t* accepted, uint32_t* refused)
{
    spu_xdiff_fixture f;
    spu_xdiff_error err;
    memset(&err, 0, sizeof err);
    memset(&f, 0, sizeof f);
    if (spu_xdiff_fixture_decode(bytes, size, &f, &err) == 0) {
        uint8_t* again = NULL;
        size_t again_n = 0;
        REQUIRE(spu_xdiff_fixture_encode(&f, &again, &again_n, &err) == 0,
                "mutation %d at 0x%zx decoded but does not encode: %s", index, at, err.detail);
        REQUIRE(again != NULL && again_n != 0, "mutation %d re-encoded to nothing", index);
        free(again);
        spu_xdiff_fixture_free(&f);
        (*accepted)++;
    } else {
        REQUIRE(err.reason == SPU_XDIFF_R_FIXTURE_FORMAT ||
                    err.reason == SPU_XDIFF_R_FIXTURE_IDENTITY ||
                    err.reason == SPU_XDIFF_R_UNSUPPORTED_VERSION,
                "mutation %d at 0x%zx answered %s: %s", index, at,
                spu_xdiff_reason_id(err.reason), err.detail);
        REQUIRE(err.detail[0] != 0, "mutation %d at 0x%zx has no detail", index, at);
        (*refused)++;
    }
}

static void function_mask_mutations(void)
{
    spu_xdiff_fixture f;
    spu_xdiff_error err;
    uint8_t* base = NULL;
    size_t size = 0;
    uint64_t mask_bytes = 0;
    uint32_t accepted = 0, refused = 0;
    build_function_fixture(&f);
    REQUIRE(spu_xdiff_fixture_encode(&f, &base, &size, &err) == 0,
            "function mutation base encode: %s", err.detail);
    uint8_t* mutation = (uint8_t*)malloc(size);
    REQUIRE(mutation != NULL, "function mask mutation buffer");
    uint8_t* mask = section_bytes(base, SPU_XDIFF_SEC_FMASK, &mask_bytes);
    const size_t offset = (size_t)(mask - base);
    for (uint32_t i = 0; i < mask_bytes; i++) {
        memcpy(mutation, base, size);
        mutation[offset + i] ^= (uint8_t)(1u << (i & 7u));
        fix_checksum(mutation, size);
        mutation_result(mutation, size, (int)i, offset + i, &accepted, &refused);
    }
    free(mutation);
    free(base);
    spu_xdiff_fixture_free(&f);
    printf("ok %llu FMAS byte mutations: %u decoded, %u refused\n",
           (unsigned long long)mask_bytes, accepted, refused);
}

/* The fixed random sequence alternates job and function units; FMAS bytes are
 * also exhaustively mutated above so the small section is always exercised. */
static void test_mutation_sweep(int mutations)
{
    size_t job_bytes = 0, function_bytes = 0;
    uint8_t* job_base = encode_full(&job_bytes);
    spu_xdiff_fixture function_fixture;
    spu_xdiff_error err;
    uint8_t* function_base = NULL;
    build_function_fixture(&function_fixture);
    REQUIRE(spu_xdiff_fixture_encode(&function_fixture, &function_base, &function_bytes, &err) == 0,
            "function mutation encode: %s", err.detail);
    spu_xdiff_fixture_free(&function_fixture);
    size_t code_bytes = 0;
    uint8_t* code_base = encode_code_fixture(&code_bytes);
    size_t largest = job_bytes > function_bytes ? job_bytes : function_bytes;
    if (code_bytes > largest) largest = code_bytes;
    uint8_t* const mutation = (uint8_t*)malloc(largest);
    REQUIRE(mutation != NULL, "mutation buffer");
    uint32_t seed = 0x784F2D31u; /* fixed: the sweep is the same on every host */
    uint32_t accepted = 0, refused = 0;
    function_mask_mutations();

    for (int i = 0; i < mutations; i++) {
        const int use_function = i & 1;
        const int use_code = use_function && (i & 2);
        const uint8_t* base = use_code ? code_base : use_function ? function_base : job_base;
        const size_t size = use_code ? code_bytes : use_function ? function_bytes : job_bytes;
        memcpy(mutation, base, size);
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        const size_t at = (size_t)(seed % size);
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        mutation[at] ^= (uint8_t)(1u + (seed >> 24)); /* never a no-op mutation */
        fix_checksum(mutation, size);
        mutation_result(mutation, size, i, at, &accepted, &refused);
    }
    free(mutation);
    free(function_base);
    free(code_base);
    free(job_base);
    printf("ok %d mutations: %u decoded and re-encoded, %u refused\n", mutations, accepted, refused);
}

/* ---- CODE and STAG (docs/SPU_XDIFF.md 4 and 7.4) ------------------------- */

typedef struct extra_section {
    uint32_t       tag;
    const uint8_t* data;
    uint64_t       bytes;
} extra_section;

/* Rebuilds an encoded file with sections dropped and added, under `version`, with a valid
 * container around them: the decoder's section rules are then what refuses it. */
static uint8_t* retable(const uint8_t* p, size_t n, uint32_t version, uint32_t drop_a,
                        uint32_t drop_b, const extra_section* extra, uint32_t extra_count,
                        size_t* out_n)
{
    spu_xdiff_file_header h;
    extra_section list[SPU_XDIFF_MAX_SECTIONS];
    uint32_t count = 0;
    memcpy(&h, p, sizeof h);
    for (uint32_t i = 0; i < h.section_count; i++) {
        const uint8_t* e = p + h.header_bytes + (size_t)i * h.section_bytes;
        const uint32_t tag = rd32(e);
        if (tag == drop_a || tag == drop_b) continue;
        list[count].tag = tag;
        list[count].data = p + rd64(e + 8);
        list[count].bytes = rd64(e + 16);
        count++;
    }
    for (uint32_t i = 0; i < extra_count; i++) list[count++] = extra[i];
    REQUIRE(count <= SPU_XDIFF_MAX_SECTIONS, "retable: too many sections");
    uint64_t cursor = (sizeof(spu_xdiff_file_header) + (uint64_t)count * sizeof(spu_xdiff_section) + 7u) & ~7ull;
    uint64_t at[SPU_XDIFF_MAX_SECTIONS];
    for (uint32_t i = 0; i < count; i++) {
        at[i] = cursor;
        cursor = (cursor + list[i].bytes + 7u) & ~7ull;
    }
    uint8_t* q = (uint8_t*)calloc(1, (size_t)cursor);
    REQUIRE(q != NULL, "retable: calloc");
    memcpy(q, p, sizeof h);
    wr32(q + offsetof(spu_xdiff_file_header, version), version);
    wr32(q + offsetof(spu_xdiff_file_header, section_count), count);
    wr64(q + offsetof(spu_xdiff_file_header, file_bytes), cursor);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* e = q + sizeof(spu_xdiff_file_header) + (size_t)i * sizeof(spu_xdiff_section);
        wr32(e, list[i].tag);
        wr64(e + 8, at[i]);
        wr64(e + 16, list[i].bytes);
        memcpy(q + at[i], list[i].data, (size_t)list[i].bytes);
    }
    fix_checksum(q, (size_t)cursor);
    (void)n;
    *out_n = (size_t)cursor;
    return q;
}

/* The byte of LS_0 that holds LS address `lsa` in an encoded file. */
static uint8_t* ls0_byte(uint8_t* p, uint32_t lsa)
{
    uint64_t bytes = 0, at = 0;
    uint8_t* s = section_bytes(p, SPU_XDIFF_SEC_LS0, &bytes);
    while (at < bytes) {
        const uint32_t start = rd32(s + at), len = rd32(s + at + 4);
        if (lsa >= start && lsa - start < len) return s + at + 8 + (lsa - start);
        at += 8 + (((uint64_t)len + 7u) & ~(uint64_t)7u);
    }
    REQUIRE(0, "LS_0 holds no byte at 0x%x", (unsigned)lsa);
    return NULL;
}

static void test_code_roundtrip(void)
{
    spu_xdiff_fixture f, g;
    spu_xdiff_error err;
    uint8_t* bytes = NULL;
    size_t n = 0;
    uint64_t code_bytes = 0, stag_bytes = 0;

    build_code_fixture(&f);
    /* The job's own writes inside its image span: the whole-span hash no longer names the image. */
    REQUIRE(fnv64(FNV64_BASIS, f.ls0 + IMG_LS, IMG_BYTES) != f.identity.image_fingerprint,
            "the entry store was not dirtied");
    REQUIRE(f.code.count == 2 && f.staged != NULL && f.staged_bytes == IMG_BYTES,
            "set_code left count %u, staged %u bytes", (unsigned)f.code.count, (unsigned)f.staged_bytes);
    bytes = encode_of(&f, &n);
    check_container_v(bytes, n, SPU_XDIFF_VERSION);
    (void)section_bytes(bytes, SPU_XDIFF_SEC_CODE, &code_bytes);
    (void)section_bytes(bytes, SPU_XDIFF_SEC_STAGED, &stag_bytes);
    REQUIRE(code_bytes == sizeof(spu_xdiff_code) && stag_bytes == IMG_BYTES,
            "CODE is %llu bytes, STAG %llu", (unsigned long long)code_bytes,
            (unsigned long long)stag_bytes);

    REQUIRE(spu_xdiff_fixture_decode(bytes, n, &g, &err) == 0, "v3 function decode: %s", err.detail);
    compare_fixtures(&f, &g);
    REQUIRE(g.code.count == 2 && g.staged != NULL && g.staged_bytes == IMG_BYTES, "decoded CODE/STAG");
    REQUIRE(memcmp(g.staged, g_staged + IMG_LS, IMG_BYTES) == 0, "STAG bytes differ from the staged image");
    REQUIRE(g.ls0[DIRTY_LS] != g.staged[DIRTY_LS - IMG_LS],
            "LS_0 outside the CODE ranges was expected to differ from the staged image");
    {
        uint8_t* again = NULL;
        size_t again_n = 0;
        REQUIRE(spu_xdiff_fixture_encode(&g, &again, &again_n, &err) == 0, "v3 re-encode: %s", err.detail);
        REQUIRE(again_n == n && memcmp(again, bytes, n) == 0, "the v3 encoding is not stable");
        free(again);
    }
    /* A version-2 reader refuses it by version, before it interprets a section. */
    {
        spu_xdiff_fixture old;
        REQUIRE(spu_xdiff_fixture_decode_max_version(bytes, n, SPU_XDIFF_VERSION_V2, &old, &err) == -1,
                "a version-2 reader accepted a version-3 fixture");
        REQUIRE(err.reason == SPU_XDIFF_R_UNSUPPORTED_VERSION && strstr(err.detail, "version 3") != NULL,
                "version-2 reader answered %s: %s", spu_xdiff_reason_id(err.reason), err.detail);
    }
    /* The staged store and the file are independent copies. */
    spu_xdiff_fixture_clear(&f);
    REQUIRE(f.staged == NULL && f.code.count == 0, "clear kept CODE/STAG");
    free(bytes);
    spu_xdiff_fixture_free(&f);
    spu_xdiff_fixture_free(&g);
    printf("ok v3 function unit with CODE roundtrip\n");
}

static void test_code_modified_refused(void)
{
    spu_xdiff_fixture f;
    spu_xdiff_error err;
    spu_xdiff_code_range r[2];
    char want[32];

    REQUIRE(SPU_XDIFF_R_UNIT_CODE_MODIFIED == 39 &&
                strcmp(spu_xdiff_reason_id(SPU_XDIFF_R_UNIT_CODE_MODIFIED), "unit-code-modified") == 0 &&
                spu_xdiff_reason_verdict(SPU_XDIFF_R_UNIT_CODE_MODIFIED) == (uint32_t)SPU_XDIFF_VOID,
            "the reason's stable id, slug or verdict changed");
    code_ranges(r);
    build_function_fixture(&f);
    memcpy(g_staged, f.ls0, SPU_XDIFF_LS_BYTES);

    /* The first and the last byte of a range, then both ranges at once: the lowest address is named. */
    static const uint32_t flips[][2] = {
        { CODE0_LS, 0 }, { CODE0_LS + CODE0_BYTES - 1u, 0 }, { CODE1_LS, 0 },
        { CODE1_LS + CODE1_BYTES - 1u, 0 }, { CODE1_LS + 8u, CODE0_LS + 4u },
    };
    for (size_t i = 0; i < sizeof flips / sizeof flips[0]; i++) {
        const uint32_t first = flips[i][1] ? flips[i][1] : flips[i][0];
        f.ls0[flips[i][0]] ^= 0x01;
        if (flips[i][1]) f.ls0[flips[i][1]] ^= 0x10;
        memset(&err, 0, sizeof err);
        REQUIRE(spu_xdiff_fixture_set_code(&f, r, 2, g_staged, &err) == -1, "flip %zu was accepted", i);
        REQUIRE(err.reason == SPU_XDIFF_R_UNIT_CODE_MODIFIED, "flip %zu answered %s: %s", i,
                spu_xdiff_reason_id(err.reason), err.detail);
        snprintf(want, sizeof want, "LS 0x%x ", (unsigned)first);
        REQUIRE(strstr(err.detail, want) != NULL, "flip %zu: detail \"%s\" does not name 0x%x", i,
                err.detail, (unsigned)first);
        REQUIRE(f.code.count == 0 && f.staged == NULL, "a refused set_code changed the fixture");
        f.ls0[flips[i][0]] ^= 0x01;
        if (flips[i][1]) f.ls0[flips[i][1]] ^= 0x10;
    }
    /* A difference outside every range is the job's data, not its code. */
    f.ls0[DIRTY_LS] ^= 0xFF;
    REQUIRE(spu_xdiff_fixture_set_code(&f, r, 2, g_staged, &err) == 0, "data outside the ranges: %s",
            err.detail);

    /* The encoder holds the same line after the fact: the entry store drifting from STAG inside a
     * range is refused, and a job unit never carries CODE. */
    {
        uint8_t* bytes = NULL;
        size_t n = 0;
        f.ls0[CODE1_LS + 4u] ^= 0x01;
        REQUIRE(spu_xdiff_fixture_encode(&f, &bytes, &n, &err) == -1, "encode wrote a drifted fixture");
        REQUIRE(err.reason == SPU_XDIFF_R_CAPTURE_LIMIT, "drifted encode answered %s",
                spu_xdiff_reason_id(err.reason));
        f.ls0[CODE1_LS + 4u] ^= 0x01;
        f.unit.kind = SPU_XDIFF_UNIT_JOB;
        f.unit.profile = SPU_XDIFF_PROFILE_JOBCHAIN;
        REQUIRE(spu_xdiff_fixture_encode(&f, &bytes, &n, &err) == -1, "encode wrote CODE on a job unit");
        REQUIRE(err.reason == SPU_XDIFF_R_CAPTURE_LIMIT, "job-with-CODE encode answered %s",
                spu_xdiff_reason_id(err.reason));
    }
    spu_xdiff_fixture_free(&f);
    printf("ok set_code refuses modified code at the first differing byte\n");
}

static void test_set_code_rejects_ranges(void)
{
    typedef struct { const char* name; uint32_t count; spu_xdiff_code_range r[17]; } rcase;
    rcase cases[] = {
        { "no ranges", 0, { { CODE0_LS, 4 } } },
        { "empty range", 1, { { CODE0_LS, 0 } } },
        { "unaligned start", 1, { { CODE0_LS + 2u, 8 } } },
        { "unaligned length", 1, { { CODE0_LS, 6 } } },
        { "unsorted", 2, { { CODE1_LS, CODE1_BYTES }, { CODE0_LS, CODE0_BYTES } } },
        { "overlap", 2, { { CODE0_LS, 0x40 }, { CODE0_LS + 0x20u, 0x20 } } },
        { "duplicate", 2, { { CODE0_LS, 8 }, { CODE0_LS, 8 } } },
        { "before the image", 1, { { IMG_LS - 4u, 8 } } },
        { "past the image", 1, { { IMG_LS + IMG_BYTES - 4u, 8 } } },
        { "address wrap", 1, { { 0xFFFFFFFCu, 8 } } },
        { "too many ranges", 17, { { IMG_LS, 4 } } },
    };
    for (uint32_t i = 0; i < 17; i++) { /* ascending and valid, so only the count is wrong */
        cases[10].r[i].ls = IMG_LS + 8u * i;
        cases[10].r[i].bytes = 4;
    }
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        spu_xdiff_fixture f;
        spu_xdiff_error err;
        build_function_fixture(&f);
        memcpy(g_staged, f.ls0, SPU_XDIFF_LS_BYTES);
        memset(&err, 0, sizeof err);
        REQUIRE(spu_xdiff_fixture_set_code(&f, cases[i].r, cases[i].count, g_staged, &err) == -1,
                "%s: set_code accepted it", cases[i].name);
        REQUIRE(err.reason == SPU_XDIFF_R_FIXTURE_FORMAT, "%s: answered %s: %s", cases[i].name,
                spu_xdiff_reason_id(err.reason), err.detail);
        REQUIRE(strstr(err.detail, "CODE") != NULL, "%s: detail \"%s\" does not name CODE",
                cases[i].name, err.detail);
        REQUIRE(f.code.count == 0 && f.staged == NULL, "%s: the fixture changed", cases[i].name);
        spu_xdiff_fixture_free(&f);
    }
    /* The last word of the image and the full maximum count are fine. */
    {
        spu_xdiff_fixture f;
        spu_xdiff_error err;
        spu_xdiff_code_range last = { IMG_LS + IMG_BYTES - 4u, 4 };
        build_function_fixture(&f);
        memcpy(g_staged, f.ls0, SPU_XDIFF_LS_BYTES);
        REQUIRE(spu_xdiff_fixture_set_code(&f, &last, 1, g_staged, &err) == 0, "last word: %s", err.detail);
        REQUIRE(spu_xdiff_fixture_set_code(&f, cases[10].r, SPU_XDIFF_CODE_RANGES_MAX, g_staged, &err) == 0,
                "maximum ranges: %s", err.detail);
        REQUIRE(f.code.count == SPU_XDIFF_CODE_RANGES_MAX, "a second set_code did not replace the first");
        spu_xdiff_fixture_free(&f);
    }
    /* A job unit has no CODE. */
    {
        spu_xdiff_fixture f;
        spu_xdiff_error err;
        spu_xdiff_code_range r[2];
        code_ranges(r);
        build_minimal(&f);
        REQUIRE(spu_xdiff_fixture_set_code(&f, r, 2, f.ls0, &err) == -1 &&
                    err.reason == SPU_XDIFF_R_FIXTURE_FORMAT,
                "set_code on a job unit: %s", err.detail);
        spu_xdiff_fixture_free(&f);
    }
    printf("ok set_code refuses %zu malformed range lists as fixture-format\n",
           sizeof cases / sizeof cases[0]);
}

/* A v3 file `p` with the CODE field at byte `field` of the record set to `value`. */
static uint8_t* with_code_u32(const uint8_t* base, size_t n, size_t field, uint32_t value)
{
    uint8_t* p = (uint8_t*)malloc(n);
    REQUIRE(p != NULL, "malloc");
    memcpy(p, base, n);
    wr32(section_bytes(p, SPU_XDIFF_SEC_CODE, NULL) + field, value);
    fix_checksum(p, n);
    return p;
}

#define CODE_RANGE(i) (16u + 8u * (i)) /* offset of range i in the CODE record */

static void test_code_decoder_rejects(void)
{
    size_t n = 0;
    uint8_t* base = encode_code_fixture(&n);
    uint8_t* p;

    /* malformed CODE: fixture-format */
    p = with_code_u32(base, n, 0, 0);
    for (uint32_t i = 0; i < 2; i++) { /* no ranges at all, so only the count is wrong */
        wr32(section_bytes(p, SPU_XDIFF_SEC_CODE, NULL) + CODE_RANGE(i), 0);
        wr32(section_bytes(p, SPU_XDIFF_SEC_CODE, NULL) + CODE_RANGE(i) + 4, 0);
    }
    fix_checksum(p, n);
    expect_reject("CODE count 0", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");
    expect_reject("CODE count over the maximum", with_code_u32(base, n, 0, SPU_XDIFF_CODE_RANGES_MAX + 1u),
                  n, SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");
    expect_reject("CODE ranges overlap", with_code_u32(base, n, CODE_RANGE(1), CODE0_LS + 4u), n,
                  SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");
    expect_reject("CODE range unaligned", with_code_u32(base, n, CODE_RANGE(0), CODE0_LS + 2u), n,
                  SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");
    expect_reject("CODE range before the image", with_code_u32(base, n, CODE_RANGE(0), IMG_LS - 4u), n,
                  SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");
    expect_reject("CODE range past the image",
                  with_code_u32(base, n, CODE_RANGE(1), IMG_LS + IMG_BYTES - 0x20u), n,
                  SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");
    expect_reject("CODE range empty", with_code_u32(base, n, CODE_RANGE(1) + 4, 0), n,
                  SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");
    expect_reject("CODE reserved", with_code_u32(base, n, 4, 1), n, SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");
    expect_reject("CODE reserved2 (the dropped fingerprint)", with_code_u32(base, n, 8, 1), n,
                  SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");
    expect_reject("CODE unused range", with_code_u32(base, n, CODE_RANGE(5), 4), n,
                  SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");

    /* STAG of another length than the image */
    p = (uint8_t*)malloc(n);
    REQUIRE(p != NULL, "malloc");
    memcpy(p, base, n);
    wr64(section_entry(p, SPU_XDIFF_SEC_STAGED) + 16, IMG_BYTES - 8u);
    fix_checksum(p, n);
    expect_reject("STAG shorter than the image", p, n, SPU_XDIFF_R_FIXTURE_FORMAT, "STAG");

    /* the sections' presence against the version and the unit kind */
    {
        size_t m = 0;
        size_t fn = 0;
        spu_xdiff_fixture fx;
        extra_section both[2];
        uint8_t zeros[IMG_BYTES];
        uint8_t code_record[sizeof(spu_xdiff_code)];
        uint64_t cb = 0;
        memcpy(code_record, section_bytes(base, SPU_XDIFF_SEC_CODE, &cb), sizeof code_record);
        memset(zeros, 0, sizeof zeros);
        both[0].tag = SPU_XDIFF_SEC_CODE;
        both[0].data = code_record;
        both[0].bytes = sizeof code_record;
        both[1].tag = SPU_XDIFF_SEC_STAGED;
        both[1].data = zeros;
        both[1].bytes = sizeof zeros;

        expect_reject("CODE without STAG",
                      retable(base, n, SPU_XDIFF_VERSION, SPU_XDIFF_SEC_STAGED, 0, NULL, 0, &m), m,
                      SPU_XDIFF_R_FIXTURE_FORMAT, "STAG");
        expect_reject("STAG without CODE",
                      retable(base, n, SPU_XDIFF_VERSION, SPU_XDIFF_SEC_CODE, 0, NULL, 0, &m), m,
                      SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");

        build_function_fixture(&fx);
        uint8_t* v2fn = encode_of(&fx, &fn);
        expect_reject("v3 function unit without CODE",
                      retable(v2fn, fn, SPU_XDIFF_VERSION, 0, 0, NULL, 0, &m), m,
                      SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");
        expect_reject("v2 function unit with CODE",
                      retable(v2fn, fn, SPU_XDIFF_VERSION_V2, 0, 0, both, 2, &m), m,
                      SPU_XDIFF_R_UNSUPPORTED_VERSION, "CODE");
        free(v2fn);
        spu_xdiff_fixture_free(&fx);

        build_minimal(&fx);
        uint8_t* job = encode_of(&fx, &fn);
        expect_reject("CODE on a job unit", retable(job, fn, SPU_XDIFF_VERSION, 0, 0, both, 2, &m), m,
                      SPU_XDIFF_R_FIXTURE_FORMAT, "CODE");
        expect_reject("STAG on a job unit", retable(job, fn, SPU_XDIFF_VERSION, 0, 0, both + 1, 1, &m), m,
                      SPU_XDIFF_R_FIXTURE_FORMAT, "STAG");
        /* a v3 job fixture without CODE is a job fixture: it keeps the whole-span check */
        {
            spu_xdiff_fixture g;
            spu_xdiff_error err;
            uint8_t* v3job = retable(job, fn, SPU_XDIFF_VERSION, 0, 0, NULL, 0, &m);
            REQUIRE(spu_xdiff_fixture_decode(v3job, m, &g, &err) == 0, "v3 job without CODE: %s", err.detail);
            spu_xdiff_fixture_free(&g);
            free(v3job);
        }
        free(job);
        spu_xdiff_fixture_free(&fx);
    }

    /* identity: well-formed, but it does not name this image or these bytes */
    {
        uint64_t sb = 0;
        p = (uint8_t*)malloc(n);
        REQUIRE(p != NULL, "malloc");
        memcpy(p, base, n);
        section_bytes(p, SPU_XDIFF_SEC_STAGED, &sb)[DIRTY_LS - IMG_LS] ^= 0x40;
        fix_checksum(p, n);
        expect_reject("STAG does not hash to the fingerprint", p, n, SPU_XDIFF_R_FIXTURE_IDENTITY, "STAG");

        p = (uint8_t*)malloc(n);
        REQUIRE(p != NULL, "malloc");
        memcpy(p, base, n);
        wr64(section_bytes(p, SPU_XDIFF_SEC_IDENTITY, NULL), f_fingerprint_of(base) ^ 1u);
        fix_checksum(p, n);
        expect_reject("fingerprint of another image", p, n, SPU_XDIFF_R_FIXTURE_IDENTITY, "STAG");

        p = (uint8_t*)malloc(n);
        REQUIRE(p != NULL, "malloc");
        memcpy(p, base, n);
        *ls0_byte(p, CODE0_LS + 4u) ^= 0x08;
        fix_checksum(p, n);
        expect_reject("LS_0 differs from STAG in a range", p, n, SPU_XDIFF_R_FIXTURE_IDENTITY, "0x1044");

        /* and outside the ranges it is allowed to differ: this is the v3 point */
        p = (uint8_t*)malloc(n);
        REQUIRE(p != NULL, "malloc");
        memcpy(p, base, n);
        *ls0_byte(p, DIRTY_LS + 4u) ^= 0x08;
        fix_checksum(p, n);
        {
            spu_xdiff_fixture g;
            spu_xdiff_error err;
            REQUIRE(spu_xdiff_fixture_decode(p, n, &g, &err) == 0,
                    "data outside the CODE ranges must not matter: %s", err.detail);
            spu_xdiff_fixture_free(&g);
            free(p);
        }
    }
    free(base);
    printf("ok v3 decoder refusals\n");
}

/* Version-2 fixtures (everything without CODE) are written exactly as before: the version is 2, no
 * CODE or STAG exists, and decode then encode is the identity on the bytes. The checksums pin the
 * bytes of the three synthetic fixtures as the version-2 writer produced them before version 3. */
static void test_v2_bytes_unchanged(void)
{
    spu_xdiff_fixture f[3], g;
    spu_xdiff_error err;
    /* build_minimal, build_full(1) and build_function_fixture as the writer before version 3 encoded them */
    static const uint64_t golden[3] = { 0xE1672A53078250A8ULL, 0xCB7A94A3BE51DB18ULL, 0x0B51BDB57067AE83ULL };
    build_minimal(&f[0]);
    build_full(&f[1], 1);
    build_function_fixture(&f[2]);
    for (int i = 0; i < 3; i++) {
        size_t n = 0;
        uint8_t* bytes = encode_of(&f[i], &n);
        check_container_v(bytes, n, SPU_XDIFF_VERSION_V2);
        REQUIRE(section_entry(bytes, SPU_XDIFF_SEC_CODE) == NULL &&
                    section_entry(bytes, SPU_XDIFF_SEC_STAGED) == NULL,
                "fixture %d: a fixture without CODE grew a CODE or STAG section", i);
        REQUIRE(rd64(bytes + HDR_CHECKSUM) == golden[i],
                "fixture %d: checksum 0x%016llx, version 2 wrote 0x%016llx", i,
                (unsigned long long)rd64(bytes + HDR_CHECKSUM), (unsigned long long)golden[i]);
        REQUIRE(spu_xdiff_fixture_decode(bytes, n, &g, &err) == 0, "fixture %d decode: %s", i, err.detail);
        REQUIRE(g.code.count == 0 && g.staged == NULL, "fixture %d decoded with CODE", i);
        uint8_t* again = NULL;
        size_t again_n = 0;
        REQUIRE(spu_xdiff_fixture_encode(&g, &again, &again_n, &err) == 0, "fixture %d re-encode: %s", i,
                err.detail);
        REQUIRE(again_n == n && memcmp(again, bytes, n) == 0, "fixture %d: decode then encode changed bytes", i);
        free(again);
        free(bytes);
        spu_xdiff_fixture_free(&g);
        spu_xdiff_fixture_free(&f[i]);
    }
    printf("ok version-2 fixtures keep their bytes\n");
}

/* spu_xdiff_fixture_check_identity is the decoder's rule for a fixture held in memory. */
static void test_check_identity_in_memory(void)
{
    spu_xdiff_fixture f;
    spu_xdiff_error err;

    build_code_fixture(&f);
    REQUIRE(spu_xdiff_fixture_check_identity(&f, &err) == 0, "a captured CODE fixture: %s", err.detail);
    f.ls0[CODE1_LS + 8u] ^= 1;
    REQUIRE(spu_xdiff_fixture_check_identity(&f, &err) == -1 && err.reason == SPU_XDIFF_R_FIXTURE_IDENTITY &&
                strstr(err.detail, "0x1108") != NULL,
            "drifted code: %s %s", spu_xdiff_reason_id(err.reason), err.detail);
    f.ls0[CODE1_LS + 8u] ^= 1;
    f.staged[0] ^= 1;
    REQUIRE(spu_xdiff_fixture_check_identity(&f, &err) == -1 && err.reason == SPU_XDIFF_R_FIXTURE_IDENTITY,
            "STAG that does not hash: %s", spu_xdiff_reason_id(err.reason));
    f.staged[0] ^= 1;
    f.code.range[1].ls = f.code.range[0].ls;
    REQUIRE(spu_xdiff_fixture_check_identity(&f, &err) == -1 && err.reason == SPU_XDIFF_R_FIXTURE_FORMAT,
            "overlapping ranges: %s", spu_xdiff_reason_id(err.reason));
    spu_xdiff_fixture_free(&f);

    /* Without CODE the whole span is hashed, and a stray STAG is malformed. */
    build_function_fixture(&f);
    REQUIRE(spu_xdiff_fixture_check_identity(&f, &err) == 0, "whole-span fixture: %s", err.detail);
    f.ls0[IMG_LS + 0x200u] ^= 1;
    REQUIRE(spu_xdiff_fixture_check_identity(&f, &err) == -1 && err.reason == SPU_XDIFF_R_FIXTURE_IDENTITY,
            "whole-span drift: %s", spu_xdiff_reason_id(err.reason));
    f.ls0[IMG_LS + 0x200u] ^= 1;
    f.staged = (uint8_t*)calloc(1, IMG_BYTES);
    f.staged_bytes = IMG_BYTES;
    REQUIRE(spu_xdiff_fixture_check_identity(&f, &err) == -1 && err.reason == SPU_XDIFF_R_FIXTURE_FORMAT,
            "STAG without CODE: %s", spu_xdiff_reason_id(err.reason));
    spu_xdiff_fixture_free(&f);
    printf("ok in-memory identity check\n");
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char** argv)
{
    test_fingerprint_known_answers();
    roundtrip(0);
    roundtrip(1);
    function_roundtrip();
    test_v1_job_compatibility();
    test_v1_reader_rejects_v2_function_fixture();
    test_empty_transcript();
    test_payload_store();
    test_clear();
    test_name();
    test_files();
    test_encoder_refuses();
    test_rejects();
    test_synthesized_header_bounds();
    test_truncations();
    test_code_roundtrip();
    test_code_modified_refused();
    test_set_code_rejects_ranges();
    test_code_decoder_rejects();
    test_v2_bytes_unchanged();
    test_check_identity_in_memory();
    test_mutation_sweep(argc > 1 ? atoi(argv[1]) : 1000);
    printf("PASS test_spu_xdiff_fixture\n");
    return 0;
}
