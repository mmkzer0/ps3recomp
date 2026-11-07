/*
 * The lv2 event timer's period wait (sys_timer.c timer_thread_proc) on one monotonic budget per period.
 *
 * The real timer thread runs against host_wait_fake.c (host_wait.c with a scripted clock and native waits). The
 * script makes it see a FALSE wake 100 us into a 1000 us period, then the timeout. What this pins:
 *  - the false wake neither delivers an event early nor restarts the period: the next wait asks for the 900 us
 *    that are left (a timer that reset its budget on a wake asks for 1000 and the fake reports it);
 *  - the event is delivered once, after the period expired, and the next period is a fresh budget;
 *  - a stop request ends a sleeping timer thread without delivering anything.
 * A hook runs inside the thread's native wait with the timer's lock released; the last one parks the thread until
 * the main thread has asked it to stop, which is what orders the two threads (no sleeps).
 */
#include "runtime/platform/win32_compat.h"
#include "runtime/syscalls/sys_event.h"
#include "runtime/syscalls/sys_timer.h"
#include "runtime/platform/tests/host_wait_fake.h"
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail) { (void)image; (void)max_avail; return 0; }
int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size, uint32_t args_ea)
{ (void)image; (void)image_size; (void)args_ea; return 0; }
void ppu_dump_guest_stack(ppu_context* ctx, const char* tag) { (void)ctx; (void)tag; }
uint32_t yz_thread_current_id(void) { return 0; }

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

#define US 1000ull
#define SEC 1000000000ull
#define BASE (1000ull * SEC)

typedef int64_t (*sc_fn)(ppu_context*);
static int32_t call(sc_fn f, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = 1;
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d; ctx.gpr[7] = e;
    return (int32_t)f(&ctx);
}
static uint32_t be_read(unsigned off)
{
    const uint8_t* p = vm_base + off;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static sys_event_queue_info* g_q;
static sys_timer_info* g_t;
static volatile int g_count_in_false_wake = -1, g_count_before_expiry = -1, g_parked;

static void hook_false_wake(void* a) { (void)a; g_count_in_false_wake = g_q->count; }
static void hook_expiry(void* a) { (void)a; g_count_before_expiry = g_q->count; }
static void hook_park_until_stop(void* a)
{
    (void)a;
    __atomic_store_n(&g_parked, 1, __ATOMIC_RELEASE);
    /* The fake runs the hook where the host wait would sleep, with the timer mutex released. The stop request is
     * guarded by that mutex, so read it under the lock like the worker does. */
    for (;;) {
        pthread_mutex_lock(&g_t->mtx);
        const int stop = g_t->stop_flag;
        pthread_mutex_unlock(&g_t->mtx);
        if (stop) break;
        sched_yield();
    }
}

static void push(int kind, int rc, uint64_t advance, uint64_t want_rel, hw_fake_hook hook)
{
    hw_fake_step s;
    memset(&s, 0, sizeof s);
    s.kind = kind; s.rc = rc; s.advance_ns = advance; s.want_rel_ns = want_rel; s.hook = hook;
    hw_fake_push(&s);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 65536);
    CHECK(call(sys_event_queue_create, 0x200, 0, 0x1234, 8, 0) == CELL_OK);
    const uint32_t q = be_read(0x200);
    g_q = &g_sys_event_queues[q - 1];
    CHECK(call(sys_timer_create, 0x210, 0, 0, 0, 0) == CELL_OK);
    const uint32_t tid = be_read(0x210);
    g_t = &g_sys_timers[tid - 1];
    CHECK(call(sys_timer_connect_event_queue, tid, q, 0x44, 1, 2) == CELL_OK);

    hw_fake_reset(BASE, 1700000000ull * SEC);
    push(HW_FAKE_TIMED, 0, 100 * US, 1000 * US, hook_false_wake);          /* a wake that is not the stop request */
    push(HW_FAKE_TIMED, ETIMEDOUT, 900 * US, 900 * US, hook_expiry);       /* the same period, 900 us left */
    push(HW_FAKE_TIMED, 0, 0, 1000 * US, hook_park_until_stop);            /* the next period starts fresh */
    CHECK(call(sys_timer_start, tid, 0, 1000, 0, 0) == CELL_OK);

    while (!__atomic_load_n(&g_parked, __ATOMIC_ACQUIRE)) sched_yield();
    CHECK(g_count_in_false_wake == 0);       /* the false wake delivered nothing */
    CHECK(g_count_before_expiry == 0);       /* nor did the period before it ran out */
    CHECK(g_q->count == 1);                  /* the expiry delivered exactly one event, before the next period began */
    CHECK(hw_fake_now() - BASE == 1000 * US);

    CHECK(call(sys_timer_stop, tid, 0, 0, 0, 0) == CELL_OK);   /* wakes and ends the parked thread, no event */
    CHECK(g_q->count == 1);
    CHECK(hw_fake_failures() == 0 && hw_fake_steps_left() == 0);
    CHECK(call(sys_timer_destroy, tid, 0, 0, 0, 0) == CELL_OK);

    printf("event timer period wait on the monotonic budget, %s backend: %s\n",
           hw_fake_backend_is_relative() ? "relative" : "absolute", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
