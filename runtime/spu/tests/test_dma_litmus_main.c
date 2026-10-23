/* Message passing between a PPU thread and an SPU through the real MFC engine
 * (spu_dma.h, mfc_submit): acquire before a GET reads guest memory, release
 * after a PUT wrote it.
 *
 *   test_dma_litmus get <iterations>
 *       The PPU publishes: it stores a 128-byte payload, executes the barrier
 *       the lifter emits for lwsync (a release fence), then stores a sequence
 *       number. The SPU GETs the sequence number, then GETs the payload: every
 *       payload word must belong to a sequence number no older than the one
 *       the SPU saw, or the SPU read data that predates what it was told was
 *       published.
 *   test_dma_litmus put <iterations>
 *       The SPU publishes: it PUTs the payload, then PUTs the sequence number.
 *       The PPU loads the sequence number, executes the barrier the lifter emits
 *       for lwsync (an acquire fence), then loads the payload, with the same
 *       rule.
 *
 * The PPU side is correct by construction (it barriers exactly as lifted code
 * does), so a failure is the SPU side's. The consumer GETs the flag and the
 * payload alternately, which also keeps the identical-transfer back-off (timing
 * only, spu_dma.h) from slowing the poll. tools/test_spu_dma_litmus.py runs both
 * directions, short in the t0 tier and at 10^8 in the stress tier.
 *
 * Word layout: (sequence << 5) | word index. */
#include "spu_dma.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t g_mem[1u << 20];
uint8_t* vm_base = g_mem;

enum { FLAG_GET = 0x10000, DATA_GET = 0x10100, FLAG_PUT = 0x11000, DATA_PUT = 0x11100 };
#define WORDS 32u   /* one 128-byte line */

/* The coherence, diagnostic and clock hooks mfc_submit() references in the
 * full runtime (as in test_dma_repeat_main.c); none is armed here. */
int g_cri_video_dma = 0;
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_coh_notify_write_except(uint32_t ea, const void* self) { (void)ea; (void)self; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockguard_check(spu_context* c, uint32_t ea, const uint8_t* p, uint32_t n, const char* what)
{
    (void)c; (void)ea; (void)p; (void)n; (void)what;
}
void (*g_spu_put_hook)(uint32_t ea, uint32_t size) = NULL;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_taskset_ea;
SPU_THREAD_LOCAL volatile unsigned g_wws_batch_gets;
/* DMA-list stall-and-notify, which a plain GET/PUT never reaches. */
void* volatile g_sn_defer_ctx;
volatile unsigned g_sn_defer;
unsigned spu_sn_defer_ticks(void) { return 0; }
void spu_ch_wake(spu_context* ctx) { (void)ctx; }
void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size)
{
    (void)ctx; (void)ea; (void)ls; (void)size;
}
unsigned* spu_ls_watch_list(int* out_n)
{
    if (out_n) *out_n = 0;
    return NULL;
}
unsigned long long ps3_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

static uint32_t pack(uint32_t seq, uint32_t j) { return (seq << 5) | j; }

static void fail(const char* what, uint32_t seq, uint32_t j, uint32_t w)
{
    fprintf(stderr, "FAIL %s: sequence %u was published but word %u holds sequence %u (0x%08X)\n",
            what, seq, j, w >> 5, w);
    fflush(stderr);
    _exit(1);
}

static void check_payload(const char* what, uint32_t seq, const uint32_t* words /* read in order */)
{
    for (uint32_t j = 0; j < WORDS; j++)
        if ((words[j] & 31u) != j || (words[j] >> 5) < seq) fail(what, seq, j, words[j]);
}

/* ---- direction 1: the PPU publishes, the SPU consumes -------------------- */

static void* ppu_publish(void* arg)
{
    const uint32_t n = (uint32_t)(uintptr_t)arg;
    volatile uint32_t* data = (volatile uint32_t*)(g_mem + DATA_GET);
    volatile uint32_t* flag = (volatile uint32_t*)(g_mem + FLAG_GET);
    for (uint32_t i = 1; i <= n; i++) {
        for (uint32_t j = 0; j < WORDS; j++) data[j] = pack(i, j);
        atomic_thread_fence(memory_order_release);      /* lwsync */
        *flag = i;
    }
    return NULL;
}

static spu_context g_spu;   /* one SPU per direction; the directions run one after the other */

static void dma(mfc_engine* mfc, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size)
{
    g_spu.mfc_lsa = lsa;
    g_spu.mfc_eah = 0;
    g_spu.mfc_eal = ea;
    g_spu.mfc_size = size;
    g_spu.mfc_tag = 1;
    if (mfc_submit(mfc, &g_spu, cmd) != 0) {
        fprintf(stderr, "FAIL: mfc_submit(0x%X, ea=0x%X, size=%u) refused the transfer\n", cmd, ea, size);
        _exit(1);
    }
}

static void run_get(uint32_t n)
{
    mfc_engine mfc;
    mfc_engine_init(&mfc);
    pthread_t ppu;
    pthread_create(&ppu, NULL, ppu_publish, (void*)(uintptr_t)n);
    uint32_t seen = 0, polls = 0;
    while (seen < n) {
        dma(&mfc, MFC_GET_CMD, 0x0, FLAG_GET, 16);
        uint32_t seq;
        memcpy(&seq, g_spu.ls, 4);
        dma(&mfc, MFC_GET_CMD, 0x100, DATA_GET, 128);
        polls++;
        if (!seq) continue;
        uint32_t words[WORDS];
        memcpy(words, g_spu.ls + 0x100, sizeof words);
        check_payload("GET", seq, words);
        seen = seq;
    }
    pthread_join(ppu, NULL);
    printf("LITMUS dma-get OK: %u publications, %u polls; the SPU never read a payload older than its flag\n", n, polls);
}

/* ---- direction 2: the SPU publishes, the PPU consumes -------------------- */

static void* ppu_consume(void* arg)
{
    const uint32_t n = (uint32_t)(uintptr_t)arg;
    volatile uint32_t* data = (volatile uint32_t*)(g_mem + DATA_PUT);
    volatile uint32_t* flag = (volatile uint32_t*)(g_mem + FLAG_PUT);
    uint32_t seen = 0;
    while (seen < n) {
        const uint32_t seq = *flag;
        atomic_thread_fence(memory_order_acquire);      /* lwsync */
        if (!seq) continue;
        uint32_t words[WORDS];
        for (uint32_t j = 0; j < WORDS; j++) words[j] = data[j];
        check_payload("PUT", seq, words);
        seen = seq;
    }
    return NULL;
}

static void run_put(uint32_t n)
{
    mfc_engine mfc;
    mfc_engine_init(&mfc);
    pthread_t ppu;
    pthread_create(&ppu, NULL, ppu_consume, (void*)(uintptr_t)n);
    for (uint32_t i = 1; i <= n; i++) {
        uint32_t words[WORDS];
        for (uint32_t j = 0; j < WORDS; j++) words[j] = pack(i, j);
        memcpy(g_spu.ls + 0x100, words, sizeof words);
        dma(&mfc, MFC_PUT_CMD, 0x100, DATA_PUT, 128);
        memcpy(g_spu.ls, &i, 4);
        dma(&mfc, MFC_PUT_CMD, 0x0, FLAG_PUT, 16);
    }
    pthread_join(ppu, NULL);
    printf("LITMUS dma-put OK: %u publications; the PPU never read a payload older than the SPU's flag\n", n);
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: test_dma_litmus get|put <iterations>\n");
        return 2;
    }
    const uint64_t n = strtoull(argv[2], NULL, 0);
    if (n == 0 || n >= (1u << 27)) {   /* the sequence number shares a word with the index */
        fprintf(stderr, "iterations must be in 1 .. 2^27\n");
        return 2;
    }
    spu_context_init(&g_spu, 1);
    g_spu.image_id = -1;
    memset(g_mem, 0, sizeof g_mem);
    if (!strcmp(argv[1], "get")) run_get((uint32_t)n);
    else if (!strcmp(argv[1], "put")) run_put((uint32_t)n);
    else return 2;
    return 0;
}
