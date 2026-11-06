/* host_interp.c - the toolkit's SPU interpreter driven as "SPU threads" for the spuexh dry run.
 * Separate translation unit: the interpreter source defines single-letter macros that clash with the probe's headers. */
#define _GNU_SOURCE
#include "../../../../../runtime/spu/spu_interp.c"

#include <pthread.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
#include "host_interp.h"

int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context *c, uint32_t a) { (void)c; (void)a; return 0; }
void spu_spurs_taskset_syscall(spu_context *c) { (void)c; }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t *p, uint32_t pc, uint32_t lr) { (void)l; (void)w; (void)p; (void)pc; (void)lr; }

static uint8_t *g_guest; static size_t g_guest_size;
#define GUEST_BASE 0x10000u
static int g_inject_spu = -1, g_inject_n, g_inject_done;   /* the injected fault fires once per process */
static int g_inject_group;                                 /* 1: the group ends with the injected fault (console model) */
static int g_hang_exit_spu = -1, g_hang_exit_armed;        /* that SPU never leaves the thread after its final status */
static volatile int g_group_kill_spu = -1;                 /* set while a group-wide injected fault unwinds */

/* ---- one interpreted SPU thread */
typedef struct {
    spu_context *ctx;
    jmp_buf jb;
    int idx, abort_flag;
    int inject_after;                 /* >0: die at the N-th DMA put */
    int puts;
    uint32_t outmbox, have_outmbox;
    uint32_t dec_val;
    int dma_violation; char viol[96];
    uint64_t steps;
    char desc[160];
    int exited;
    int done;                         /* thread_main has returned (release); the poller joins then (acquire) */
    pthread_t th;
    uint32_t entry;
} hthread_t;
static hthread_t g_threads[6];
static int g_nthreads;
static SPU_THREAD_LOCAL hthread_t *t_cur;

static void dma_fail(hthread_t *t, const char *why) {
    snprintf(t->viol, sizeof t->viol, "%s", why);
    t->dma_violation = 1;
    longjmp(t->jb, 1);
}

/* the channel operations the probe uses; anything else is ignored (loud for the ones that would change results) */
u128 spu_rdch(spu_context *c, uint32_t ch) {
    hthread_t *t = t_cur; u128 r; memset(&r, 0, sizeof r);
    if (t->abort_flag) longjmp(t->jb, 2);
    switch (ch) {
    case MFC_RdTagStat: r._u32[0] = c->mfc_tag_mask; break;           /* every transfer is complete */
    case SPU_RdDec: t->dec_val -= 997; r._u32[0] = t->dec_val; break;
    default: break;
    }
    return r;
}
uint32_t spu_rchcnt(spu_context *c, uint32_t ch) { (void)c; return ch == MFC_Cmd ? 16u : 1u; }
void spu_wrch(spu_context *c, uint32_t ch, u128 v) {
    hthread_t *t = t_cur; const uint32_t x = v._u32[0];
    if (t->abort_flag) longjmp(t->jb, 2);
    switch (ch) {
    case MFC_LSA: c->mfc_lsa = x; break;
    case MFC_EAH: c->mfc_eah = x; break;
    case MFC_EAL: c->mfc_eal = x; break;
    case MFC_Size: c->mfc_size = x; break;
    case MFC_TagID: c->mfc_tag = x & 31u; break;
    case MFC_WrTagMask: c->mfc_tag_mask = x; break;
    case MFC_WrTagUpdate: break;
    case SPU_WrDec: t->dec_val = x; break;
    case SPU_WrOutMbox: t->outmbox = x; t->have_outmbox = 1; break;
    case MFC_Cmd: {
        const uint32_t cmd = x & 0xFFFFu;
        const uint64_t ea = ((uint64_t)c->mfc_eah << 32) | c->mfc_eal;
        const uint32_t lsa = c->mfc_lsa, n = c->mfc_size;
        const int put = cmd == MFC_PUT_CMD || cmd == MFC_PUTF_CMD, get = cmd == MFC_GET_CMD;
        if (!put && !get) { char b[64]; snprintf(b, sizeof b, "unsupported MFC command 0x%x", cmd); dma_fail(t, b); }
        if ((ea & 15u) || (lsa & 15u) || (n & 15u) || !n || n > 16384u) { char b[96]; snprintf(b, sizeof b, "bad DMA ea=%llx lsa=%x size=%x", (unsigned long long)ea, lsa, n); dma_fail(t, b); }
        if (ea < GUEST_BASE || ea + n > g_guest_size || lsa + n > SPU_LS_SIZE) dma_fail(t, "DMA outside memory");
        if (put) {
            if (t->idx == g_inject_spu && g_inject_n > 0 && !g_inject_done && ++t->puts == g_inject_n) {
                g_inject_done = 1;
                snprintf(t->viol, sizeof t->viol, "injected fault at put %d", t->puts);
                t->dma_violation = 1;
                if (g_inject_group) {
                    /* the console model: a group exception ends every thread of the group mid-work */
                    g_group_kill_spu = t->idx;
                    for (hthread_t *o = g_threads; o < g_threads + g_nthreads; o++)
                        if (o != t) o->abort_flag = 1;
                }
                longjmp(t->jb, 1);
            }
            memcpy(g_guest + ea, c->ls + lsa, n);
        } else memcpy(c->ls + lsa, g_guest + ea, n);
        break; }
    default: break;
    }
}

/* ---- ELF loader (32-bit big-endian SPU executable) */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint8_t *g_elf; static size_t g_elf_len;
static int load_elf(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END); g_elf_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    g_elf = malloc(g_elf_len); if (fread(g_elf, 1, g_elf_len, f) != g_elf_len) { fclose(f); return -1; }
    fclose(f);
    if (g_elf_len < 52 || memcmp(g_elf, "\177ELF", 4) || g_elf[4] != 1 || g_elf[5] != 2) { fprintf(stderr, "%s: not a 32-bit big-endian ELF\n", path); return -1; }
    return 0;
}
static int elf_to_ls(spu_context *c, uint32_t *entry) {
    memset(c->ls, 0, SPU_LS_SIZE);
    const uint32_t phoff = be32(g_elf + 28); const unsigned phnum = be16(g_elf + 44), phes = be16(g_elf + 42);
    *entry = be32(g_elf + 24);
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = g_elf + phoff + i * phes;
        if (be32(ph) != 1) continue;                        /* PT_LOAD */
        const uint32_t off = be32(ph + 4), va = be32(ph + 8), fs = be32(ph + 16), ms = be32(ph + 20);
        if (va + ms > SPU_LS_SIZE || off + fs > g_elf_len) return -1;
        memcpy(c->ls + va, g_elf + off, fs);
    }
    return 0;
}

static spu_context *new_ctx(int idx) {
    spu_context *c = aligned_alloc(64, (sizeof(spu_context) + 63) & ~(size_t)63);
    memset(c, 0, sizeof *c);
    spu_context_init(c, (uint32_t)idx);
    c->image_id = -1;                 /* pure interpretation: the interpreter's fast loop (no lifted-function lookup per step) */
    return c;
}

static void *thread_main(void *arg) {
    hthread_t *t = (hthread_t *)arg;
    t_cur = t;
    uint32_t stop = 0;
    const int jr = setjmp(t->jb);
    if (jr == 0) {
        stop = spu_interp_run(t->ctx, t->entry);
        t->steps = g_spu_interp_steps;
        if (t->idx == g_hang_exit_spu && g_hang_exit_armed) {
            /* the program published its final status but never exits: the model of a thread lv2 keeps running */
            g_hang_exit_armed = 0;
            snprintf(t->desc, sizeof t->desc, "hung after final status (injected)");
            while (!t->abort_flag) usleep(2000);   /* only the supervisor's end (abort_flag) releases it */
        } else if (t->ctx->status == SPU_STATUS_STOPPED_BY_STOP && t->ctx->stop_code == 0x102) {
            t->exited = 1; snprintf(t->desc, sizeof t->desc, "exit status 0x%x", t->outmbox);
        } else if (t->ctx->status == SPU_STATUS_STOPPED_BY_STOP) {
            snprintf(t->desc, sizeof t->desc, "stop 0x%x", t->ctx->stop_code);
        } else {
            const uint32_t insn = be32(t->ctx->ls + (t->ctx->pc & 0x3FFFC));
            if (spu_decode_tab[insn >> 21] == SPU_word) snprintf(t->desc, sizeof t->desc, "invalid instruction %08x at 0x%x (prefix %03x undecoded)", insn, t->ctx->pc, insn >> 21);
            else snprintf(t->desc, sizeof t->desc, "halt status %u at 0x%x", t->ctx->status, t->ctx->pc);
        }
        (void)stop;
    } else if (jr == 1) snprintf(t->desc, sizeof t->desc, "%s", t->viol);
    else if (g_group_kill_spu >= 0) snprintf(t->desc, sizeof t->desc, "group ended by fault of spu%d", g_group_kill_spu);
    else snprintf(t->desc, sizeof t->desc, "aborted by the supervisor");
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    return NULL;
}


int hi_run(int n, const int *idx, const uint64_t *arg1, uint8_t *guest, size_t guest_size, const uint8_t *elf, size_t elf_len,
           int inject_spu, int inject_n, int inject_group, int hang_exit_spu, int (*poll)(void *), void *pu, int limit_ms, hi_res_t *res) {
    g_guest = guest; g_guest_size = guest_size; g_inject_spu = inject_spu; g_inject_n = inject_n; g_inject_group = inject_group;
    g_hang_exit_spu = hang_exit_spu; g_hang_exit_armed = hang_exit_spu >= 0;
    g_group_kill_spu = -1;
    g_elf = (uint8_t *)elf; g_elf_len = elf_len;
    hthread_t *th = g_threads;
    g_nthreads = n;
    for (int i = 0; i < n; i++) {
        hthread_t *t = &th[i];
        memset(t, 0, sizeof *t);
        t->ctx = new_ctx(i); t->idx = idx[i];
        if (elf_to_ls(t->ctx, &t->entry) < 0) return -1;
        t->ctx->gpr[3]._u32[0] = (uint32_t)(arg1[i] >> 32); t->ctx->gpr[3]._u32[1] = (uint32_t)arg1[i];
        t->dec_val = 0xFFFFFFFFu;
    }
    for (int i = 0; i < n; i++) pthread_create(&th[i].th, NULL, thread_main, &th[i]);
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    int joined[6] = {0}, nj = 0;
    while (nj < n) {
        usleep(20000);
        for (int i = 0; i < n; i++)
            if (!joined[i] && __atomic_load_n(&th[i].done, __ATOMIC_ACQUIRE) && pthread_join(th[i].th, NULL) == 0) { joined[i] = 1; nj++; }
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        const double el = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if ((poll && poll(pu)) || (limit_ms > 0 && el > limit_ms)) for (int i = 0; i < n; i++) th[i].abort_flag = 1;
    }
    for (int i = 0; i < n; i++) {
        snprintf(res[i].desc, sizeof res[i].desc, "%s", th[i].desc);
        res[i].exited = th[i].exited; res[i].timed_out = th[i].abort_flag; res[i].steps = th[i].steps;
        free(th[i].ctx);
    }
    return 0;
}
