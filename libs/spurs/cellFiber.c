/* ps3recomp - PPU fibers and UtilWorkerControl.
 *
 * Guest state/queues are authoritative. Host records own only execution state:
 * stacks, contexts, suspended dispatch frames and worker affinity. Lock order:
 * registry, then reservation-model lines; never hold either across a switch or
 * a guest callback. LV2 event waits run outside the registry lock.
 */
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE 1
#endif
#include "cellFiber.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../runtime/ppu/ppu_dispatch_state.h"
#include "../../runtime/syscalls/sys_ppu_thread.h"
#include "../../runtime/syscalls/sys_event.h"
#include "../../runtime/syscalls/sys_timer.h"
#include "../../runtime/memory/vm.h"          /* vm_range_ok: the guest arena rule */
#include "../../runtime/memory/reservation.h"
#include "ps3emu/endian.h"
#include "ps3emu/stop.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <ucontext.h>
#if defined(__APPLE__)
_Static_assert(sizeof(ucontext_t) > 64, "_XOPEN_SOURCE must precede system headers");
#endif
#endif
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
#define FIBER_HOST_STACK (4u * 1024u * 1024u)
#define FIBER_STACK_TOP_PAD 0x100u
#define WORKER_ACTIVE (1ull << 38)
#define WORKER_WAKE (1ull << 40)
#define SCHED_MAGIC 0x44007515864E5F4Cull
#define FIBER_MAGIC 0xF784E782F9EA9359ull

enum { F_READY, F_RUNNING, F_WAIT_SIGNAL, F_WAIT_JOIN, F_DONE };
typedef struct Sched Sched;
typedef struct Fiber Fiber;
typedef struct Runner {
#ifdef _WIN32
    void* back;
#else
    ucontext_t back;
#endif
} Runner;
struct Fiber {
    Fiber* next;
    Sched* sched;
    Runner* runner;
    u32 ea, opd, stack_ea, stack_size;
    u64 arg;
    int state, pinned, executing;
    u32 join_ea;
    s32 status;
    int dispatch_depth, vcall_sp;
    u32 vcall[PS3_PPU_VCALL_SLOTS];
    ppu_context* active_ctx;
#ifdef _WIN32
    void* host_fiber;
    DWORD owner;
#else
    ucontext_t uc;
    void* host_stack;
    pthread_t owner;
#endif
    ppu_context ctx;
};
struct Sched {
    Sched* next;
    u32 ea;
    unsigned running;
    int utility;
};
static Sched* s_scheds;
static Fiber* s_fibers;
static _Atomic u32 s_tls_offset = UINT32_MAX;
static PPU_THREAD_LOCAL Fiber* t_cur;
static void (*s_worker_exit_hook)(void*, u32);
static void* s_worker_exit_user;
#ifdef _WIN32
static SRWLOCK s_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_cv = CONDITION_VARIABLE_INIT;
#define LOCK() AcquireSRWLockExclusive(&s_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#define WAIT() SleepConditionVariableSRW(&s_cv, &s_lock, INFINITE, 0)
#define WAKE() WakeAllConditionVariable(&s_cv)
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv = PTHREAD_COND_INITIALIZER;
#define LOCK() pthread_mutex_lock(&s_lock)
#define UNLOCK() pthread_mutex_unlock(&s_lock)
#define WAIT() pthread_cond_wait(&s_cv, &s_lock)
#define WAKE() pthread_cond_broadcast(&s_cv)
#endif
static PS3_NORETURN void unsupported(const char* api, u32 ea, const char* field, const char* reason)
{
    char address[16]; snprintf(address, sizeof address, "0x%X", ea);
    const ps3_stop_field f[] = {{"api", api}, {"ea", address}, {"field", field}, {"reason", reason}};
    ps3_stop(PS3_STOP_FIBER_UNSUPPORTED, "unsupported fiber execution or attribute state",
             "Implement the pinned firmware path; do not discard a suspended context or callback.", f, 4, NULL);
}
static void range(const char* api, const char* arg, u32 ea, u64 size)
{
    /* The arena rule lives in vm.h: a zero ppu_vm_size means the whole 32-bit
     * space, so it is not a zero-length limit, and readiness is not
     * "vm_base != NULL" (an identity mapping has vm_base == 0). */
    if (!vm_range_ok(ea, size))
        ps3_stop_guest_range(api, arg, ea, size, vm_guest_limit());
}
static u32 ld32(u32 ea)
{
    return ps3_bswap32(atomic_load_explicit((_Atomic u32*)(vm_base + ea), memory_order_acquire));
}
static u64 ld64(u32 ea)
{
    return ps3_bswap64(atomic_load_explicit((_Atomic u64*)(vm_base + ea), memory_order_acquire));
}
typedef struct Word { u32 ea; u64 clear, set; int wide; } Word;
static int word_rmw(u8* line, void* user)
{
    const Word* w = user;
    if (w->wide) {
        _Atomic u64* p = (_Atomic u64*)(line + (w->ea & 127));
        const u64 v = ps3_bswap64(atomic_load_explicit(p, memory_order_relaxed));
        atomic_store_explicit(p, ps3_bswap64((v & ~w->clear) | w->set), memory_order_release);
    } else {
        _Atomic u32* p = (_Atomic u32*)(line + (w->ea & 127));
        const u32 v = ps3_bswap32(atomic_load_explicit(p, memory_order_relaxed));
        atomic_store_explicit(p, ps3_bswap32((v & ~(u32)w->clear) | (u32)w->set), memory_order_release);
    }
    return 1;
}
static void bits32(u32 ea, u32 clear, u32 set) { Word w = {ea, clear, set, 0}; resv_host_rmw(ea, word_rmw, &w); }
static void st32(u32 ea, u32 v) { bits32(ea, UINT32_MAX, v); }
static void st64(u32 ea, u64 v) { Word w = {ea, UINT64_MAX, v, 1}; resv_host_rmw(ea, word_rmw, &w); }
static void st8(u32 ea, u8 v)
{
    const unsigned shift = 24 - 8 * (ea & 3);
    bits32(ea & ~3u, 0xFFu << shift, (u32)v << shift);
}
typedef struct WorkerWord { u32 ea; u64 clear, set, delta, old; int idle; } WorkerWord;
static int worker_rmw(u8* line, void* user)
{
    WorkerWord* w = user;
    _Atomic u64* p = (_Atomic u64*)(line + (w->ea & 127));
    w->old = ps3_bswap64(atomic_load_explicit(p, memory_order_relaxed));
    const u64 set = w->idle && (w->old & WORKER_WAKE) ? 0 : w->set;
    atomic_store_explicit(p, ps3_bswap64(((w->old & ~w->clear) | set) + w->delta), memory_order_release);
    return 1;
}
static u64 worker_update(u32 ea, u64 clear, u64 set, u64 delta, int idle)
{
    WorkerWord word = {ea + 0x200, clear, set, delta, 0, idle};
    resv_host_rmw(ea + 0x200, worker_rmw, &word);
    return word.old;
}
static u32 tls_slot(void)
{
    const u32 offset = atomic_load_explicit(&s_tls_offset, memory_order_acquire);
    if (offset == UINT32_MAX || !g_active_ctx) return 0;
    const u32 ea = (u32)(g_active_ctx->gpr[13] + offset + 0x20u);
    if (ea) range("cellFiberPpuSelf", "tls", ea, 4);
    return ea;
}
static Sched* find_sched(u32 ea)
{
    for (Sched* s = s_scheds; s; s = s->next) if (s->ea == ea) return s;
    return NULL;
}
static Fiber* find_fiber(u32 ea)
{
    for (Fiber* f = s_fibers; f; f = f->next) if (f->ea == ea) return f;
    return NULL;
}
static int all_done(const Sched* s)
{
    for (Fiber* f = s_fibers; f; f = f->next) if (f->sched == s && f->state != F_DONE) return 0;
    return 1;
}
static int on_owner(const Fiber* f)
{
    if (!f->pinned) return 1;
#ifdef _WIN32
    return f->owner == GetCurrentThreadId();
#else
    return pthread_equal(f->owner, pthread_self());
#endif
}
static void enqueue(Fiber* f)
{
    const u32 priority = (ld32(f->ea + 0xC) >> 24);
    if (priority > 3) unsupported("SendSignal", f->ea, "priority", "fiber-state");
    const u32 head = f->sched->ea + 0x80 + 4 * priority, tail = head + 0x10;
    st32(f->ea + 4, 0);
    const u32 previous = ld32(tail);
    if (previous) { range("SendSignal", "tail", previous, CELL_FIBER_PPU_SIZE); st32(previous + 4, f->ea); }
    else st32(head, f->ea);
    st32(tail, f->ea);
    st8(f->sched->ea + 0x48 + priority, 0xFF);
}
static Fiber* take_runnable(Sched* s)
{
    for (unsigned p = 0; p < 4; ++p) {
        const u32 head = s->ea + 0x80 + 4 * p;
        u32 link = head, ea = ld32(link);
        while (ea) {
            Fiber* f = find_fiber(ea);
            if (!f || f->sched != s || !(ld32(ea) & 1)) unsupported("RunFibers", s->ea, "runnable", "queue-state");
            const u32 next = ld32(ea + 4);
            if (!f->executing && on_owner(f)) {
                st32(link, next);
                if (!next) st32(head + 0x10, link == head ? 0 : link - 4);
                if (!ld32(head)) st8(s->ea + 0x48 + p, 0);
                st32(ea + 4, 0);
                bits32(ea, 1, 2);
                return f;
            }
            link = ea + 4; ea = next;
        }
    }
    return NULL;
}
static void lifetime_add(Fiber* f)
{
    const u32 sentinel = f->sched->ea + 0xE8, node = f->ea + 0x320;
    const u32 previous = ld32(sentinel + 4);
    st32(node, sentinel); st32(node + 4, previous); st32(previous, node); st32(sentinel + 4, node);
}
static void lifetime_remove(Fiber* f)
{
    const u32 node = f->ea + 0x320, next = ld32(node), previous = ld32(node + 4);
    if (!next || !previous) unsupported("Exit", f->ea, "lifetime", "queue-state");
    range("Exit", "next", next, 8); range("Exit", "previous", previous, 8);
    st32(next + 4, previous); st32(previous, next); st32(node, 0); st32(node + 4, 0);
}
static void fiber_body(Fiber* f);
#ifdef _WIN32
static VOID CALLBACK fiber_entry(void* p) { fiber_body(p); }
static int host_create(Fiber* f) { f->host_fiber = CreateFiberEx(0, FIBER_HOST_STACK, 0, fiber_entry, f); return f->host_fiber != NULL; }
static void host_destroy(Fiber* f) { if (f->host_fiber) DeleteFiber(f->host_fiber); f->host_fiber = NULL; }
static void host_resume(Runner* r, Fiber* f)
{
    if (!IsThreadAFiber()) ConvertThreadToFiber(NULL);
    r->back = GetCurrentFiber(); SwitchToFiber(f->host_fiber);
}
static void host_yield(Fiber* f) { SwitchToFiber(f->runner->back); }
#else
static void fiber_entry(unsigned hi, unsigned lo) { fiber_body((Fiber*)(((uintptr_t)hi << 32) | lo)); }
static int host_create(Fiber* f)
{
    f->host_stack = malloc(FIBER_HOST_STACK);
    if (!f->host_stack) return 0;
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = f->host_stack; f->uc.uc_stack.ss_size = FIBER_HOST_STACK; f->uc.uc_link = NULL;
    const uintptr_t p = (uintptr_t)f;
    makecontext(&f->uc, (void (*)(void))fiber_entry, 2, (unsigned)(p >> 32), (unsigned)p);
    return 1;
}
static void host_destroy(Fiber* f) { free(f->host_stack); f->host_stack = NULL; }
static void host_resume(Runner* r, Fiber* f) { swapcontext(&r->back, &f->uc); }
static void host_yield(Fiber* f) { swapcontext(&f->uc, &f->runner->back); }
#endif
static void wake_control_locked(u32 ea);
static void finish_locked(Fiber* f, s32 status)
{
    f->status = status; f->state = F_DONE;
    st32(f->ea, 0x40); lifetime_remove(f);
    for (Fiber* g = s_fibers; g; g = g->next) {
        if (g->state == F_WAIT_JOIN && g->join_ea == f->ea) {
            g->state = F_READY; bits32(g->ea, 0x20, 1); enqueue(g);
            if (g->sched->utility) wake_control_locked(g->sched->ea);
        }
    }
}
static PS3_NORETURN void leave(Fiber* f)
{
#ifdef _WIN32
    SwitchToFiber(f->runner->back);
#else
    setcontext(&f->runner->back);
#endif
    abort();
}
static void fiber_body(Fiber* f)
{
    f->ctx.gpr[1] = (f->stack_ea + f->stack_size - FIBER_STACK_TOP_PAD) & ~0xFull;
    f->ctx.gpr[3] = f->arg; f->ctx.cia = f->opd;
    if (!g_ppu_thread_entry_trampoline) unsupported("CreateFiber", f->ea, "entry", "missing-trampoline");
    g_ppu_thread_entry_trampoline(&f->ctx);
    LOCK(); finish_locked(f, (s32)f->ctx.gpr[3]); UNLOCK(); WAKE(); leave(f);
}
static void resume(Runner* r, Fiber* f)
{
    ppu_context* saved_active = g_active_ctx;
    if (!f->pinned) {
#ifdef _WIN32
        f->owner = GetCurrentThreadId();
#else
        f->owner = pthread_self();
#endif
        f->pinned = 1;
        if (saved_active) { f->ctx.gpr[13] = saved_active->gpr[13]; f->ctx.thread_id = saved_active->thread_id; }
    }
    const u32 slot = tls_slot(), saved_self = slot ? ld32(slot) : 0;
    int* depth = ps3_ppu_dispatch_depth_slot();
    int* sp = ps3_ppu_vcall_sp_slot();
    u32** storage = ps3_ppu_vcall_stack_slot();
    const int old_depth = depth ? *depth : 0, old_sp = sp ? *sp : 0;
    u32* old_storage = storage ? *storage : NULL;
    if (depth) *depth = f->dispatch_depth;
    if (sp) *sp = f->vcall_sp;
    if (storage) *storage = f->vcall;
    if (slot) st32(slot, f->ea);
    f->runner = r; t_cur = f; g_active_ctx = f->active_ctx;
    host_resume(r, f);
    f->active_ctx = g_active_ctx;
    if (depth) { f->dispatch_depth = *depth; *depth = old_depth; }
    if (sp) { f->vcall_sp = *sp; *sp = old_sp; }
    if (storage) *storage = old_storage;
    g_active_ctx = saved_active; t_cur = NULL;
    if (slot) st32(slot, saved_self);
}

u64 _cellFiberPpuInitialize(u64 anchor)
{
    /* libfiber 02003D18..3D28: the SDK C declaration is unknown. This
     * describes the instruction-level result, NOT a success code: subf leaves
     * the full wrapped difference in r3 and stw stores only its low word. */
    if (!g_active_ctx) unsupported(__func__, (u32)anchor, "r13", "missing-context");
    const u64 offset = anchor - g_active_ctx->gpr[13];
    atomic_store_explicit(&s_tls_offset, (u32)offset, memory_order_release);
    return offset;
}
static s32 attribute(u32 ea, u32 sdk, int worker, int fiber)
{
    if (!ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;
    if (ea & 7) return (s32)CELL_FIBER_ERROR_ALIGN;
    range("AttributeInitialize", "attribute", ea, worker ? 0x180 : 0x100);
    memset(vm_base + ea, 0, worker ? 0x180 : 0x100);
    vm_write64(ea, fiber ? FIBER_MAGIC : SCHED_MAGIC); vm_write32(ea + 8, sdk);
    if (fiber) memcpy(vm_base + ea + 0x10, "*NO NAME*", sizeof "*NO NAME*");
    if (worker) vm_write32(ea + 0x108, sdk);
    return CELL_OK;
}
s32 _cellFiberPpuSchedulerAttributeInitialize(CellFiberPpuSchedulerAttribute* a, u32 sdk) { return attribute((u32)(uintptr_t)a, sdk, 0, 0); }
s32 _cellFiberPpuAttributeInitialize(CellFiberPpuAttribute* a, u32 sdk) { return attribute((u32)(uintptr_t)a, sdk, 0, 1); }
s32 _cellFiberPpuUtilWorkerControlAttributeInitialize(u32 a, u32 sdk) { return attribute(a, sdk, 1, 0); }
static s32 check_attribute(u32 ea, int fiber, int worker)
{
    if (!ea) return CELL_OK;
    if (ea & 7) return (s32)CELL_FIBER_ERROR_ALIGN;
    range("Initialize", "attribute", ea, worker ? 0x180 : 0x100);
    if (ld64(ea) != (fiber ? FIBER_MAGIC : SCHED_MAGIC)) return (s32)CELL_FIBER_ERROR_INVAL;
    return CELL_OK;
}
static void check_extensions(u32 ea, int fiber, int worker)
{
    if (!ea) return;
    if (!fiber) {
        /* libfiber FW 4.93 InitializeScheduler 0x02001CA4..0x02002000 reads only
         * +0x10 (autoCheckFlags 0x02001E10), +0x11 (debuggerSupport 0x02001E28),
         * and +0x14 (interval 0x02001E04). Unread bytes in +0x0C..+0xFF and
         * +0x100..+0x17F are ignored by firmware.
         * Periodic stack-canary checking (dispatch loop 0x02002C58, check 0x0200150C)
         * is not modelled; fail closed if autoCheckFlags is non-zero to avoid hiding
         * stack overflow. */
        if (vm_base[ea + 0x10]) unsupported("Initialize", ea, "+0x10", "auto-check");
        return;
    }
    for (u32 i = 0xC; i < (worker ? 0x180u : 0x100u); ++i) {
        if (fiber && i >= 0x10 && i < 0x30) continue;
        if (worker && i >= 0x108 && i < 0x10C) continue;
        if (vm_base[ea + i]) {
            char field[16]; snprintf(field, sizeof field, "+0x%X", i);
            unsupported("Initialize", ea, field, "attribute-field");
        }
    }
}
static s32 scheduler_init(u32 ea, u32 attr, int utility)
{
    s32 rc = check_attribute(attr, 0, utility);
    if (rc) return rc;
    if (!tls_slot()) return (s32)CELL_FIBER_ERROR_NOSYSINIT;
    check_extensions(attr, 0, utility);
    if (find_sched(ea)) return (s32)CELL_FIBER_ERROR_STAT;
    Sched* s = calloc(1, sizeof *s);
    if (!s) return (s32)CELL_FIBER_ERROR_NOMEM;
    s->ea = ea; s->utility = utility; s->next = s_scheds; s_scheds = s;
    memset(vm_base + ea, 0, 0x200);
    vm_write8(ea + 0x41, 1);
    vm_write32(ea + 0xE0, ea + 0xE0); vm_write32(ea + 0xE4, ea + 0xE0);
    vm_write32(ea + 0xE8, ea + 0xE8); vm_write32(ea + 0xEC, ea + 0xE8);
    if (attr) {
        /* libfiber FW 4.93 InitializeScheduler 0x02001CA4..0x02002000 reads ONLY:
         * - +0x10 autoCheckFlags (0x02001E10), stored at scheduler+0x148 (0x02001E20);
         * - +0x11 debuggerSupport (0x02001E28), registers scheduler in internal debugger
         *   table and stores slot address at scheduler+0x168 (0x02005AD8, 0x02005BB8).
         *   Deviation: libfiber debugger table does not exist in this HLE, so
         *   scheduler+0x168 remains 0 and fibers are not registered (debugger-only,
         *   not visible to guest scheduling).
         * - +0x14 autoCheckFlagsIntervalUsec (0x02001E04), converted to timebase ticks
         *   and stored as BE64 at scheduler+0x138, with scheduler+0x140 zeroed
         *   (0x02001E38, 0x02001E44).
         * Unread bytes in +0x0C..+0xFF and +0x100..+0x17F are ignored as firmware does. */
        const u32 usec = ld32(attr + 0x14);
        const u64 ticks = (u64)usec * PS3_TIMEBASE_FREQ / 1000000ULL;
        vm_write64(ea + 0x138, ticks);
        vm_write64(ea + 0x140, 0);
        vm_write8(ea + 0x148, vm_base[attr + 0x10]);
    }
    return CELL_OK;
}
s32 cellFiberPpuInitializeScheduler(CellFiberPpuScheduler* scheduler, const CellFiberPpuSchedulerAttribute* attr)
{
    const u32 ea = (u32)(uintptr_t)scheduler;
    if (!ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;
    if (ea & 127) return (s32)CELL_FIBER_ERROR_ALIGN;
    range(__func__, "scheduler", ea, 0x200);
    LOCK(); const s32 rc = scheduler_init(ea, (u32)(uintptr_t)attr, 0); UNLOCK(); return rc;
}
static void scheduler_free(Sched* s)
{
    for (Fiber** p = &s_fibers; *p;) {
        Fiber* f = *p;
        if (f->sched == s) {
            *p = f->next; host_destroy(f); free(f);
        } else p = &f->next;
    }
    for (Sched** p = &s_scheds; *p; p = &(*p)->next) if (*p == s) { *p = s->next; break; }
    st8(s->ea + 0x41, 0); free(s);
}
s32 cellFiberPpuFinalizeScheduler(CellFiberPpuScheduler* scheduler)
{
    const u32 ea = (u32)(uintptr_t)scheduler;
    if (!ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;
    LOCK(); Sched* s = find_sched(ea);
    s32 rc = CELL_OK;
    if (!s) rc = (s32)CELL_FIBER_ERROR_INVAL;
    else if (s->running) rc = (s32)CELL_FIBER_ERROR_STAT;
    else if (s->utility) rc = (s32)CELL_FIBER_ERROR_PERM;
    /* 02002864..286C / 02002A5C..2A74: no stack can be discarded while
     * its lifetime node remains linked, even if it has never run. */
    else if (!all_done(s)) rc = (s32)CELL_FIBER_ERROR_BUSY;
    else scheduler_free(s);
    UNLOCK(); WAKE(); return rc;
}
static s32 create_fiber(u32 scheduler, u32 fiber, u32 entry, u64 arg, u32 priority, u32 stack, u32 size, u32 attr, int utility)
{
    if (utility && (attr & 7)) return (s32)CELL_FIBER_ERROR_ALIGN;
    if (!scheduler || !fiber || !entry || !stack) return (s32)CELL_FIBER_ERROR_NULL_POINTER;
    if ((scheduler & 127) || (fiber & 127) || (stack & 15) || (size & 15)) return (s32)CELL_FIBER_ERROR_ALIGN;
    if (priority > 3) return (s32)CELL_FIBER_ERROR_INVAL;
    if (attr && (attr & 7)) return (s32)CELL_FIBER_ERROR_ALIGN;
    s32 rc = check_attribute(attr, 1, 0);
    if (rc) return rc;
    if (!tls_slot()) return (s32)CELL_FIBER_ERROR_NOSYSINIT;
    check_extensions(attr, 1, 0);
    range("CreateFiber", "fiber", fiber, CELL_FIBER_PPU_SIZE);
    range("CreateFiber", "entry", entry, 8); range("CreateFiber", "stack", stack, size);
    LOCK(); Sched* s = find_sched(scheduler);
    if (!s) { UNLOCK(); return (s32)CELL_FIBER_ERROR_INVAL; }
    if (utility != s->utility) { UNLOCK(); return (s32)CELL_FIBER_ERROR_STAT; }
    if (find_fiber(fiber)) { UNLOCK(); return (s32)CELL_FIBER_ERROR_STAT; }
    Fiber* f = calloc(1, sizeof *f);
    if (!f) { UNLOCK(); return (s32)CELL_FIBER_ERROR_NOMEM; }
    f->ea = fiber; f->opd = entry; f->arg = arg; f->stack_ea = stack; f->stack_size = size;
    f->state = F_READY; f->sched = s; f->ctx.vscr = ps3_ppu_initial_vscr();
    f->ctx.gpr[13] = g_active_ctx->gpr[13];
    f->ctx.thread_id = g_active_ctx->thread_id;
    f->active_ctx = &f->ctx;
    if (!host_create(f)) { free(f); UNLOCK(); return (s32)CELL_FIBER_ERROR_NOMEM; }
    f->next = s_fibers; s_fibers = f;
    memset(vm_base + fiber, 0, CELL_FIBER_PPU_SIZE);
    vm_write32(fiber + 8, scheduler); vm_write8(fiber + 0xC, priority);
    vm_write32(fiber + 0x300, entry); vm_write64(fiber + 0x308, arg); vm_write32(fiber + 0x328, fiber);
    if (utility && (!attr || ld32(attr + 8) > 0x26FFFF)) { vm_write32(fiber + 0x10, 1); vm_write64(fiber + 0x18, scheduler); }
    st32(fiber, 1); lifetime_add(f); enqueue(f);
    /* A worker already asleep in RunFibers must see a fiber created later. The
     * firmware's creation path is not decoded far enough to name how it wakes
     * them, so this is the same notification Wakeup gives (an inference). */
    if (utility) wake_control_locked(scheduler);
    UNLOCK(); WAKE(); return CELL_OK;
}
s32 cellFiberPpuCreateFiber(CellFiberPpuScheduler* s, CellFiberPpu* f, u32 entry, u64 arg, u32 priority, void* stack, u32 size, const CellFiberPpuAttribute* attr)
{
    return create_fiber((u32)(uintptr_t)s, (u32)(uintptr_t)f, entry, arg, priority, (u32)(uintptr_t)stack, size, (u32)(uintptr_t)attr, 0);
}
s32 cellFiberPpuUtilWorkerControlCreateFiber(u32 s, u32 f, u32 entry, u64 arg, u32 priority, u32 stack, u32 size, u32 attr)
{
    return create_fiber(s, f, entry, arg, priority, stack, size, attr, 1);
}
s32 cellFiberPpuRunFibers(CellFiberPpuScheduler* scheduler)
{
    const u32 ea = (u32)(uintptr_t)scheduler;
    if (!ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;
    if (ea & 127) return (s32)CELL_FIBER_ERROR_ALIGN;
    if (!tls_slot()) return (s32)CELL_FIBER_ERROR_NOSYSINIT;
    range(__func__, "scheduler", ea, 0x200);
    LOCK(); Sched* s = find_sched(ea);
    if (!s) { UNLOCK(); return (s32)CELL_FIBER_ERROR_INVAL; }
    if (s->running || t_cur || s->utility) { UNLOCK(); return (s32)CELL_FIBER_ERROR_STAT; }
    ++s->running; st8(ea + 0x40, 1); Runner runner;
    for (;;) {
        Fiber* f = take_runnable(s);
        if (!f) { if (all_done(s)) break; WAIT(); continue; }
        f->state = F_RUNNING; f->executing = 1;
        UNLOCK(); resume(&runner, f); LOCK(); f->executing = 0;
        if (f->state == F_DONE) host_destroy(f);
    }
    --s->running; st8(ea + 0x40, 0); UNLOCK(); return CELL_OK;
}
typedef struct SignalOp { u32 ea; int wait, transition, invalid; } SignalOp;
static int signal_rmw(u8* line, void* user)
{
    SignalOp* op = user;
    _Atomic u32* p = (_Atomic u32*)(line + (op->ea & 127));
    u32 state = ps3_bswap32(atomic_load_explicit(p, memory_order_relaxed));
    if (!state || (state & 0x40)) { op->invalid = 1; return 0; }
    if (op->wait) {
        if (state & 0x10) state &= ~0x10u;
        else { state = (state & ~2u) | 8; op->transition = 1; }
    } else {
        if (state & 8) { state = (state & ~8u) | 1; op->transition = 1; }
        else state |= 0x10;
    }
    atomic_store_explicit(p, ps3_bswap32(state), memory_order_release);
    return 1;
}
s32 cellFiberPpuWaitSignal(void)
{
    if (!tls_slot()) return (s32)CELL_FIBER_ERROR_NOSYSINIT;
    Fiber* f = t_cur;
    if (!f) return (s32)CELL_FIBER_ERROR_PERM;
    LOCK();
    SignalOp op = {f->ea, 1, 0, 0};
    resv_host_rmw(f->ea, signal_rmw, &op);
    if (op.invalid) { UNLOCK(); return (s32)CELL_FIBER_ERROR_STAT; }
    if (op.transition) f->state = F_WAIT_SIGNAL;
    UNLOCK();
    if (op.transition) host_yield(f);
    return CELL_OK;
}
static ps3_fiber_notify_status signal_locked(u32 ea)
{
    Fiber* f = find_fiber(ea);
    if (!f) return PS3_FIBER_NOTIFY_STATE;
    SignalOp op = {ea, 0, 0, 0};
    resv_host_rmw(ea, signal_rmw, &op);
    if (op.invalid) return PS3_FIBER_NOTIFY_STATE;
    if (op.transition) { f->state = F_READY; enqueue(f); }
    return PS3_FIBER_NOTIFY_OK;
}
static s32 public_signal(u32 ea, u32 out)
{
    if (!ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;
    if (ea & 127) return (s32)CELL_FIBER_ERROR_ALIGN;
    range("SendSignal", "fiber", ea, CELL_FIBER_PPU_SIZE);
    LOCK(); const ps3_fiber_notify_status status = signal_locked(ea);
    u32 owner = 0, count = 0;
    if (status == PS3_FIBER_NOTIFY_OK) {
        const u32 scheduler = ld32(ea + 8);
        range("SendSignal", "scheduler", scheduler, 0x200);
        if (ld32(ea + 0x10) == 1) owner = (u32)ld64(ea + 0x18);
        else count = ld32(scheduler + 0x40) >> 24;
    }
    UNLOCK();
    if (status != PS3_FIBER_NOTIFY_OK) return (s32)CELL_FIBER_ERROR_STAT;
    WAKE();
    if (owner) return cellFiberPpuUtilWorkerControlWakeup(owner);
    if (out) { range("SendSignal", "output", out, 4); vm_write32(out, count); }
    return CELL_OK;
}
ps3_fiber_notify_status ps3_fiber_notify(u32 ea)
{
    if (!ea) return PS3_FIBER_NOTIFY_NULL;
    if (ea & 127) return PS3_FIBER_NOTIFY_ALIGN;
    const s32 rc = public_signal(ea, 0);
    return rc == CELL_OK ? PS3_FIBER_NOTIFY_OK : PS3_FIBER_NOTIFY_STATE;
}
s32 cellFiberPpuSendSignal(CellFiberPpu* f, u32* out) { return public_signal((u32)(uintptr_t)f, (u32)(uintptr_t)out); }
s32 cellFiberPpuUtilWorkerControlSendSignal(u32 f, u32 out) { return public_signal(f, out); }
s32 cellFiberPpuYield(void)
{
    Fiber* f = t_cur; if (!f) return (s32)CELL_FIBER_ERROR_PERM;
    LOCK(); f->state = F_READY; bits32(f->ea, 2, 1); enqueue(f); UNLOCK(); host_yield(f); return CELL_OK;
}
s32 cellFiberPpuExit(s32 status)
{
    Fiber* f = t_cur; if (!f) return (s32)CELL_FIBER_ERROR_PERM;
    LOCK(); finish_locked(f, status); UNLOCK(); WAKE(); leave(f);
}
CellFiberPpu* cellFiberPpuSelf(void)
{
    const u32 slot = tls_slot(); return (CellFiberPpu*)(uintptr_t)(slot ? ld32(slot) : 0);
}
s32 cellFiberPpuJoinFiber(CellFiberPpu* fiber, s32* status)
{
    const u32 ea = (u32)(uintptr_t)fiber;
    if (!ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;
    Fiber* self = t_cur;
    LOCK(); Fiber* f = find_fiber(ea);
    if (!f || f == self) { UNLOCK(); return (s32)CELL_FIBER_ERROR_INVAL; }
    if (self && f->state != F_DONE) {
        self->state = F_WAIT_JOIN; self->join_ea = ea; bits32(self->ea, 2, 0x20);
        UNLOCK(); host_yield(self); LOCK(); self->join_ea = 0;
    }
    while ((f = find_fiber(ea)) && f->state != F_DONE) WAIT();
    s32 rc = CELL_OK;
    if (!f) rc = (s32)CELL_FIBER_ERROR_INVAL;
    else if (status) { range(__func__, "status", (u32)(uintptr_t)status, 4); vm_write32((u32)(uintptr_t)status, (u32)f->status); }
    UNLOCK(); return rc;
}

static s32 flag_call(u32 op, u32 id, u64 bits)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = id; ctx.gpr[4] = bits;
    if (op == 82) return (s32)sys_event_flag_create(&ctx);
    if (op == 83) return (s32)sys_event_flag_destroy(&ctx);
    if (op == 87) return (s32)sys_event_flag_set(&ctx);
    ctx.gpr[5] = SYS_EVENT_FLAG_WAIT_AND | SYS_EVENT_FLAG_WAIT_CLEAR;
    return (s32)sys_event_flag_wait(&ctx);
}
static void flag_notify(u32 ea, u32 sleepers)
{
    if (!sleepers) return;
    const s32 rc = flag_call(87, ld32(ea + 0x208), sleepers);
    if (rc && (u32)rc != 0x8001000A) unsupported("Wakeup", ea, "event-flag", "notification-failure");
}
static void wake_control_locked(u32 ea)
{
    const u64 old = worker_update(ea, 0xFFFFFFFFull, WORKER_WAKE, 0, 0);
    flag_notify(ea, (u32)old);
}
static s32 control_check(u32 ea)
{
    if (!ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;
    if (ea & 127) return (s32)CELL_FIBER_ERROR_ALIGN;
    range("UtilWorkerControl", "control", ea, 0x300); return CELL_OK;
}
s32 cellFiberPpuUtilWorkerControlInitializeWithAttribute(u32 ea, u32 attr)
{
    s32 rc = control_check(ea); if (rc) return rc;
    LOCK();
    if (find_sched(ea)) unsupported(__func__, ea, "control", "reinitialize-live");
    memset(vm_base + ea, 0, 0x300);
    rc = flag_call(82, ea + 0x208, 0);
    if (!rc) rc = scheduler_init(ea, attr, 1);
    if (rc) { if (ld32(ea + 0x208)) flag_call(83, ld32(ea + 0x208), 0); UNLOCK(); return rc; }
    vm_write32(ea + 0x28C, attr ? ld32(attr + 0x108) : 0);
    st64(ea + 0x200, WORKER_ACTIVE);
    UNLOCK(); return CELL_OK;
}
s32 cellFiberPpuUtilWorkerControlWakeup(u32 ea)
{
    s32 rc = control_check(ea); if (rc) return rc;
    LOCK(); Sched* s = find_sched(ea);
    if (!s || !s->utility) { UNLOCK(); return (s32)CELL_FIBER_ERROR_STAT; }
    const u64 old = ld64(ea + 0x200);
    /* 02005368 permits the old-SDK inactive wake path through 0x2FFFFF. */
    if (!(old & WORKER_ACTIVE) && ld32(ea + 0x28C) > 0x2FFFFF) { UNLOCK(); return (s32)CELL_FIBER_ERROR_STAT; }
    wake_control_locked(ea); UNLOCK(); return CELL_OK;
}
void ps3_fiber_set_worker_exit_hook(void (*hook)(void*, u32), void* user)
{
    LOCK(); s_worker_exit_hook = hook; s_worker_exit_user = user; UNLOCK();
}
s32 cellFiberPpuUtilWorkerControlRunFibers(u32 ea)
{
    s32 rc = control_check(ea); if (rc) return rc;
    if (t_cur) return (s32)CELL_FIBER_ERROR_PERM;
    LOCK(); Sched* s = find_sched(ea);
    if (!s || !s->utility || !(ld64(ea + 0x200) & WORKER_ACTIVE)) { UNLOCK(); return (s32)CELL_FIBER_ERROR_STAT; }
    if (!tls_slot()) { UNLOCK(); return (s32)CELL_FIBER_ERROR_NOSYSINIT; }
    u64 state = ld64(ea + 0x200);
    if ((state >> 56) > 32) { UNLOCK(); return (s32)CELL_FIBER_ERROR_AGAIN; }
    const u32 used = ld32(ea + 0x288);
    worker_update(ea, 0, 0, 1ull << 56, 0);
    if (used == UINT32_MAX) unsupported(__func__, ea, "worker-slot", "worker-capacity");
    const unsigned slot = (unsigned)__builtin_clz(~used);
    const u32 bit = 1u << slot;
    st32(ea + 0x288, used | (0x80000000u >> slot)); ++s->running;
    Runner runner;
    for (;;) {
        Fiber* f = take_runnable(s);
        if (f) {
            worker_update(ea, (u64)bit | WORKER_WAKE, 0, 1ull << 48, 0);
            st8(ea + 0x40, (u8)(ld64(ea + 0x200) >> 48));
            f->state = F_RUNNING; f->executing = 1;
            UNLOCK(); resume(&runner, f); LOCK(); f->executing = 0;
            worker_update(ea, 0, 0, -(1ull << 48), 0);
            st8(ea + 0x40, (u8)(ld64(ea + 0x200) >> 48));
            if (f->state == F_DONE) host_destroy(f);
            continue;
        }
        state = ld64(ea + 0x200);
        if (!(state & WORKER_ACTIVE)) break;
        if (worker_update(ea, WORKER_WAKE, bit, 0, 1) & WORKER_WAKE) continue;
        const u32 flag = ld32(ea + 0x208);
        UNLOCK(); rc = flag_call(85, flag, bit); LOCK();
        if (rc) unsupported(__func__, ea, "event-flag", "wait-failure");
    }
    void (*hook)(void*, u32) = s_worker_exit_hook; void* user = s_worker_exit_user;
    UNLOCK(); if (hook) hook(user, ea); LOCK();
    worker_update(ea, (u64)bit, 0, -(1ull << 56), 0);
    bits32(ea + 0x288, 0x80000000u >> slot, 0); --s->running;
    if (!s->running) st8(ea + 0x40, 0);
    UNLOCK(); return CELL_OK;
}
s32 cellFiberPpuUtilWorkerControlShutdown(u32 ea)
{
    s32 rc = control_check(ea); if (rc) return rc;
    LOCK(); Sched* s = find_sched(ea);
    const u64 state = ld64(ea + 0x200);
    if (!s || !s->utility || !(state & WORKER_ACTIVE)) { UNLOCK(); return (s32)CELL_FIBER_ERROR_STAT; }
    /* 02001768: the lifetime-list sentinel, not runnable/worker counts. */
    if (ld32(ea + 0xE8) != ea + 0xE8) { UNLOCK(); return (s32)CELL_FIBER_ERROR_BUSY; }
    const u64 old = worker_update(ea, WORKER_ACTIVE | 0xFFFFFFFFull, WORKER_WAKE, 0, 0);
    flag_notify(ea, (u32)old); UNLOCK(); return CELL_OK;
}
s32 cellFiberPpuUtilWorkerControlFinalize(u32 ea)
{
    s32 rc = control_check(ea); if (rc) return rc;
    LOCK(); Sched* s = find_sched(ea); const u64 state = ld64(ea + 0x200);
    if (!s || !s->utility || (state & WORKER_ACTIVE)) { UNLOCK(); return (s32)CELL_FIBER_ERROR_STAT; }
    if (state >> 56) { UNLOCK(); return (s32)CELL_FIBER_ERROR_BUSY; }
    if (s->running || !all_done(s)) unsupported(__func__, ea, "lifetime", "suspended-context");
    const u32 flag = ld32(ea + 0x208);
    scheduler_free(s);
    rc = flag_call(83, flag, 0);
    if (rc) unsupported(__func__, ea, "event-flag", "destroy-failure");
    UNLOCK(); return CELL_OK;
}
