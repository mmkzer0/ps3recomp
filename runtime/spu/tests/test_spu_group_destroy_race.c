/* sys_spu_thread_group_destroy against a host thread that is finishing (lv2_register.c, #131).
 *
 * `spu_thread_t.running` is what lets group_destroy free a thread's context and local store: the PPU side frees once it
 * reads 0. It was a plain int read through a volatile cast and cleared before the thread's last access to the slot, so
 * the free could race that last access (and the slot, reused by the next spu_alloc_thread's memset, race the thread's
 * own unlock of finish_event). It is an acquire/release atomic now and is cleared last, by spu_thread_finish.
 *
 * Ordered with the hook the sync primitives use (PS3_SYNC_TEST_HOOKS, ps3_sync_test_hook), never a sleep: the
 * hook "spu-thread-finish" runs on the host thread at its tail, after the wake-up of group_join and just before the
 * release store of `running`. The test parks the thread there, in both thread procs (a PPU-fallback thread and a thread
 * running its lifted image):
 *   - group_destroy while the thread is parked is CELL_EBUSY with the thread, its context and its local store
 *     untouched (a `running` cleared earlier than the hook, or not consulted, frees under a live thread);
 *   - the thread's last touch of the local store (written inside the hook) is followed by the PPU's free once the
 *     thread is released: the release store / acquire load pair is what orders them, so under ThreadSanitizer a
 *     relaxed or volatile `running` is a reported race (ctest --preset tsan -R spu_group_destroy_race);
 *   - after the release the same destroy succeeds and frees everything.
 *
 * clang -std=gnu17 -DPS3_SYNC_TEST_HOOKS -I include -I runtime -I runtime/syscalls -I runtime/spu -I libs/video
 *   runtime/spu/tests/test_spu_group_destroy_race.c -Wl,-dead_strip -pthread -o /tmp/test_spu_group_destroy_race
 */
#include <stdint.h>
int64_t lv2_spu_lock_line_classify(uint32_t a, uint64_t f) { (void)a; (void)f; return 0; }
#include "../../syscalls/lv2_register.c"
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "../../../tests/guest_range_catch.h"

uint8_t* vm_base;
uint32_t ppu_vm_size;
void SetLastError(DWORD e) { (void)e; }
/* Link stubs for what lv2_register.c references and the rest of the runtime supplies; none is reached below, except
 * spu_lifted_thread_run, which a lifted thread's proc calls and which here runs nothing and reports no group exit. */
uint32_t g_spu_interp_last_pc;
uint64_t g_spu_interp_steps;
void (*g_spu_out_mbox_hook)(uint32_t, uint32_t, int, uint32_t);
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void ppu_guard_page(uint32_t ea) { (void)ea; }
uint32_t ps3_spu_image_source_ea(uint32_t img_ea) { (void)img_ea; return 0; }
void spu_coh_unregister(spu_context* c) { (void)c; }
int spu_elf_load_to_ls(const uint8_t* image, size_t n, uint8_t* ls, uint32_t* entry) { (void)image; (void)n; (void)ls; (void)entry; return 0; }
uint32_t spu_interp_run(spu_context* c, uint32_t lsa) { (void)c; (void)lsa; return 0; }
int spu_lifted_thread_available(uint32_t entry) { (void)entry; return 0; }
void spu_lifted_thread_run(struct spu_context* c, spu_lifted_thread_result* out) { (void)c; memset(out, 0, sizeof *out); }
void spu_lifted_thread_setup(struct spu_context* c, const spu_lifted_thread_desc* d) { (void)c; (void)d; }
spu_ppu_fallback_fn spu_lookup_ppu_fallback(uint32_t entry, void** user) { (void)entry; (void)user; return 0; }
void spu_mfc_release(spu_context* c) { (void)c; }
void spu_raw_note_image(uint32_t src_ea, uint32_t entry) { (void)src_ea; (void)entry; }
int32_t spu_registry_fallback(uint32_t a, uint32_t b, uint32_t c, void* d) { (void)a; (void)b; (void)c; (void)d; return 0; }
spu_lifted_entry_fn spu_workload_find_img(uint64_t fp, int* id) { (void)fp; (void)id; return 0; }
uint64_t spu_workload_fingerprint(const void* d, size_t n) { (void)d; (void)n; return 0; }
void sys_event_queue_push_from_kernel(uint32_t q, uint64_t s, uint64_t d1, uint64_t d2, uint64_t d3, const char* who)
{ (void)q; (void)s; (void)d1; (void)d2; (void)d3; (void)who; }
uint32_t spurs_port_queue(uint32_t port) { (void)port; return 0; }
int64_t sys_event_flag_set(ppu_context* c) { (void)c; return 0; }
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail) { (void)image; (void)max_avail; return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* A one-shot gate: open() releases a waiter; wait_ms() reports whether it opened in time. The bound only stops a run
 * where the hook never fires (the code under test lacks it) from hanging; a healthy run never waits on the clock. */
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; int open; } gate_t;
#define GATE_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 }
static void gate_open(gate_t* g)
{
    pthread_mutex_lock(&g->mu); g->open = 1; pthread_cond_broadcast(&g->cv); pthread_mutex_unlock(&g->mu);
}
static int gate_wait_ms(gate_t* g, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int ok = 1;
    pthread_mutex_lock(&g->mu);
    while (!g->open && ok) ok = pthread_cond_timedwait(&g->cv, &g->mu, &ts) == 0;
    ok = g->open;
    pthread_mutex_unlock(&g->mu);
    return ok;
}

static gate_t g_at_tail = GATE_INIT, g_release = GATE_INIT;
static uint32_t g_hook_tid;
static spu_thread_t* g_hook_slot;

/* The host thread's tail (spu_thread_finish): its last touch of the memory group_destroy frees, then park. */
int ps3_sync_test_hook(const char* point, uint32_t id)
{
    if (strcmp(point, "spu-thread-finish") != 0 || id != g_hook_tid) return 0;
    gate_open(&g_at_tail);
    gate_wait_ms(&g_release, 20000);
    /* The thread's last use of its own slot, after the PPU side opened the release gate: nothing but `running` can
     * order it before the PPU's destroy, which writes these fields (the gate carries release -> thread, never
     * thread -> release). ThreadSanitizer sees a free only as a mark, not as a write that races an earlier access, so
     * the race is made visible on the slot's fields, which destroy stores to. */
    uint8_t* ls = g_hook_slot->local_store;
    ls[0] = 0x5A;
    return 0;
}

static int32_t quick_handler(uint32_t tid, uint32_t args_ea, uint32_t args_size, void* user)
{
    (void)tid; (void)args_ea; (void)args_size; (void)user;
    return 7;
}

static uint32_t sc(int64_t (*h)(ppu_context*), uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d;
    ctx.gpr[3] = (uint64_t)h(&ctx);
    return (uint32_t)ctx.gpr[3];
}

static void reset_gate(gate_t* g) { pthread_mutex_lock(&g->mu); g->open = 0; pthread_mutex_unlock(&g->mu); }

/* One host thread through the proc `lifted` selects: parked at its tail, destroyed while parked, released, destroyed. */
static void run_case(int lifted)
{
    const char* name = lifted ? "lifted thread" : "fallback thread";
    spu_group_t* g = spu_alloc_group(); assert(g);
    spu_thread_t* t = spu_alloc_thread(); assert(t);
    t->group_id = g->id;
    g->num_threads = 1;
    g->state = SPU_GROUP_STATE_RUNNING;
    g->thread_indices[0] = (uint32_t)(t - s_spu_threads);
    t->sctx = (struct spu_context*)calloc(1, sizeof(spu_context)); assert(t->sctx);
    t->local_store = (uint8_t*)calloc(1, 64); assert(t->local_store);
    t->fb_handler = quick_handler;
    g_hook_tid = t->tid;
    g_hook_slot = t;
    reset_gate(&g_at_tail);
    reset_gate(&g_release);

    uint8_t* ls = t->local_store; struct spu_context* sctx = t->sctx;
    pthread_mutex_init(&t->finish_event.mu, NULL);
    pthread_cond_init(&t->finish_event.cv, NULL);
    t->finish_event.done = 0;
    spu_thread_set_running(t, 1);
    assert(pthread_create(&t->host_thread, NULL, lifted ? spu_exec_thread_proc : spu_fallback_thread_proc, t) == 0);
    const pthread_t host = t->host_thread;

    CHECK(gate_wait_ms(&g_at_tail, 20000), "%s: never reached the finish hook (spu_thread_finish)", name);
    if (!fails) {
        /* The thread has done everything but publish `running`: still running, so destroy refuses and frees nothing. */
        CHECK(spu_thread_running(t), "%s: running is still set at the tail", name);
        CHECK(sc(sys_spu_thread_group_destroy_handler, g->id, 0, 0, 0) == (uint32_t)CELL_EBUSY,
              "%s: group_destroy of a thread that has not published `running` = 0 is EBUSY", name);
        CHECK(t->in_use && g->in_use && t->local_store == ls && t->sctx == sctx,
              "%s: and the thread, its context and its local store are untouched", name);
    }
    gate_open(&g_release);
    /* No join first: a join would order the thread's last write before the free by itself. The destroy sees the thread
     * through `running` alone, which is what is under test. */
    CHECK(sc(sys_spu_thread_group_destroy_handler, g->id, 0, 0, 0) == 0, "%s: group_destroy after the thread ended succeeds", name);
    CHECK(!t->in_use && !g->in_use && !t->local_store && !t->sctx, "%s: and releases the thread and the group", name);
    pthread_join(host, NULL);
    CHECK(!spu_thread_running(t) && t->exit_status == (lifted ? 0 : 7), "%s: the thread ended, exit status %d", name, t->exit_status);
    pthread_mutex_destroy(&t->finish_event.mu);
    pthread_cond_destroy(&t->finish_event.cv);
}

int main(void)
{
    vm_base = calloc(1, 1u << 20); assert(vm_base);
    ppu_vm_size = 1u << 20;
    run_case(0);
    run_case(1);
    printf("[spu_group_destroy_race] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
