/* SPU thread-group syscalls (lv2_register.c): teardown and error returns.
 *
 *   - sys_spu_thread_group_destroy frees each thread's context and local store, so it must not run while a thread is
 *     still executing on its host thread: that is a use-after-free in the thread. A running thread makes it wait a
 *     moment and then answer CELL_EBUSY with nothing freed; once the thread has stopped it succeeds.
 *   - sys_spu_thread_group_start on a group whose thread is still running is CELL_ESTAT.
 *   - A group's unused thread slots are invalid (index MAX_SPU_THREADS), not index 0, which is some other group's thread.
 *   - Error paths return the CELL_E* code. lv2_try_syscall stores the handler's return value in r3, so a handler that
 *     set r3 and then `return -1` handed the guest 0xFFFFFFFFFFFFFFFF.
 *   - sys_spu_thread_get_spu_cfg stores the value through its pointer argument; sys_spu_thread_connect_event on an
 *     unknown thread is ESRCH; sys_spu_image_import answers CELL_EFAULT / CELL_ENOEXEC, not host errnos.
 *
 * clang -std=gnu17 -I include -I runtime -I runtime/syscalls -I runtime/spu -I libs/video
 *   runtime/spu/tests/test_spu_group_lifecycle.c -Wl,-dead_strip -pthread -o /tmp/test_spu_group_lifecycle
 */
#include <stdint.h>
/* Syscall 14's classifier (#341) lives in lv2_lock_line.c, which pulls in the
 * VM map table; nothing here issues syscall 14. */
int64_t lv2_spu_lock_line_classify(uint32_t a, uint64_t f) { (void)a; (void)f; return 0; }
#include "../../syscalls/lv2_register.c"
#include <assert.h>
#include <stdio.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
#ifndef _WIN32
void SetLastError(DWORD e) { (void)e; }
#endif
#ifndef _WIN32
/* Link stubs for what lv2_register.c references and the rest of the runtime supplies; nothing below is reached by
 * the checks here (Windows gets the same from test_spu_group_lifecycle_win_stubs.c). */
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
void spu_lifted_thread_run(struct spu_context* c, spu_lifted_thread_result* out) { (void)c; (void)out; }
void spu_lifted_thread_setup(struct spu_context* c, const spu_lifted_thread_desc* d) { (void)c; (void)d; }
spu_ppu_fallback_fn spu_lookup_ppu_fallback(uint32_t entry, void** user) { (void)entry; (void)user; return 0; }
void spu_mfc_release(spu_context* c) { (void)c; }
void spu_raw_note_image(uint32_t src_ea, uint32_t entry) { (void)src_ea; (void)entry; }
int32_t spu_registry_fallback(uint32_t a, uint32_t b, uint32_t c, void* d) { (void)a; (void)b; (void)c; (void)d; return 0; }
spu_lifted_entry_fn spu_workload_find_img(uint64_t fp, int* id) { (void)fp; (void)id; return 0; }
uint64_t spu_workload_fingerprint(const void* d, size_t n) { (void)d; (void)n; return 0; }
void sys_event_queue_push_from_kernel(uint32_t q, uint64_t s, uint64_t d1, uint64_t d2, uint64_t d3, const char* who)
{ (void)q; (void)s; (void)d1; (void)d2; (void)d3; (void)who; }
#endif
uint32_t spurs_port_queue(uint32_t port) { (void)port; return 0; }
int64_t sys_event_flag_set(ppu_context* c) { (void)c; return 0; }
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail) { (void)image; (void)max_avail; return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint32_t rd32(uint32_t a) { const uint8_t* p = vm_base + a; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Run a handler as lv2_try_syscall does: r3 receives the return value. */
static uint32_t sc(int64_t (*h)(ppu_context*), uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d;
    ctx.gpr[3] = (uint64_t)h(&ctx);
    return (uint32_t)ctx.gpr[3];
}

/* A group with one thread that owns a lifted context and a local store. */
static spu_thread_t* make_thread(spu_group_t** gout)
{
    spu_group_t* g = spu_alloc_group(); assert(g);
    spu_thread_t* t = spu_alloc_thread(); assert(t);
    t->group_id = g->id;
    g->num_threads = 1;
    g->thread_indices[0] = (uint32_t)(t - s_spu_threads);
    t->sctx = (struct spu_context*)calloc(1, sizeof(spu_context)); assert(t->sctx);
    t->local_store = (uint8_t*)calloc(1, 64); assert(t->local_store);
    if (gout) *gout = g;
    return t;
}

int main(void)
{
    vm_base = calloc(1, 1u << 20); assert(vm_base);
    ppu_vm_size = 1u << 20;

    /* Unused thread slots of a fresh group are invalid, not index 0. */
    {
        spu_group_t* g = spu_alloc_group(); assert(g);
        int ok = 1;
        for (int k = 0; k < 8; k++) ok &= g->thread_indices[k] == MAX_SPU_THREADS;
        CHECK(ok, "a fresh group's thread slots are all invalid");
        g->in_use = 0;
    }

    /* Error returns are the codes, not -1. */
    CHECK(sc(sys_spu_thread_group_start_handler, 0xDEAD, 0, 0, 0) == (uint32_t)CELL_ESRCH,
          "group_start of an unknown group is ESRCH");
    CHECK(sc(sys_spu_thread_get_exit_status_handler, 0xDEAD, 0x100, 0, 0) == (uint32_t)CELL_ESRCH,
          "get_exit_status of an unknown thread is ESRCH");
    CHECK(sc(sys_spu_thread_connect_event_handler, 0xDEAD, 1, 1, 0) == (uint32_t)CELL_ESRCH,
          "connect_event on an unknown thread is ESRCH");
    CHECK(sc(sys_spu_thread_write_ls_handler, 0xDEAD, 0, 0, 4) == (uint32_t)CELL_ESRCH,
          "write_ls on an unknown thread is ESRCH");
    CHECK(sc(sys_spu_thread_get_spu_cfg_handler, 0xDEAD, 0x100, 0, 0) == (uint32_t)CELL_ESRCH,
          "get_spu_cfg on an unknown thread is ESRCH");
    CHECK(sc(sys_spu_image_import_handler, 0x2000, 0, 0, 0) == (uint32_t)CELL_EFAULT,
          "image_import with a null source is EFAULT");
    memset(vm_base + 0x3000, 0, 16);
    CHECK(sc(sys_spu_image_import_handler, 0x2000, 0x3000, 0, 0) == (uint32_t)CELL_ENOEXEC,
          "image_import of something that is not an ELF is ENOEXEC");

    /* A thread that is still running. */
    spu_group_t* g = NULL;
    spu_thread_t* t = make_thread(&g);
    t->running = 1;
    CHECK(sc(sys_spu_thread_get_exit_status_handler, t->tid, 0x100, 0, 0) == (uint32_t)CELL_ESTAT,
          "get_exit_status of a running thread is ESTAT (got 0x%08X)",
          sc(sys_spu_thread_get_exit_status_handler, t->tid, 0x100, 0, 0));
    CHECK(sc(sys_spu_thread_group_start_handler, g->id, 0, 0, 0) == (uint32_t)CELL_ESTAT,
          "group_start with a running thread is ESTAT");

    /* get_spu_cfg stores through the pointer. */
    t->spu_cfg = 3;
    memset(vm_base + 0x400, 0xEE, 16);
    CHECK(sc(sys_spu_thread_get_spu_cfg_handler, t->tid, 0x400, 0, 0) == 0, "get_spu_cfg succeeds");
    CHECK(rd32(0x400) == 0 && rd32(0x404) == 3, "get_spu_cfg stores 3 through the pointer (0x%08X 0x%08X)",
          rd32(0x400), rd32(0x404));
    CHECK(vm_base[0x408] == 0xEE, "and nothing past the u64");

    /* group_destroy with the thread running: refused, nothing freed. */
    uint8_t* ls = t->local_store; struct spu_context* sctx = t->sctx;
    CHECK(sc(sys_spu_thread_group_destroy_handler, g->id, 0, 0, 0) == (uint32_t)CELL_EBUSY,
          "group_destroy with a running thread is EBUSY");
    CHECK(t->in_use && g->in_use && t->local_store == ls && t->sctx == sctx,
          "and the thread, its context and its local store are untouched");

    /* The thread stops: the same destroy now goes through. */
    t->running = 0;
    CHECK(sc(sys_spu_thread_group_destroy_handler, g->id, 0, 0, 0) == 0, "group_destroy of a stopped group succeeds");
    CHECK(!t->in_use && !g->in_use && !t->local_store && !t->sctx, "and releases the thread and the group");

    printf("[spu_group_lifecycle] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
