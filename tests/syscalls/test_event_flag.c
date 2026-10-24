/*
 * Event flag unit tests: sys_event_flag_destroy with/without blocked waiters.
 * Under lv2 (RPCS3 oracle: rpcs3/Emu/Cell/lv2/sys_event_flag.cpp sys_event_flag_destroy),
 * destroying a flag with blocked waiters returns CELL_EBUSY and does not destroy it.
 * The waiter can still be woken by a subsequent set, and once unblocked, destroy returns CELL_OK.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

#include "syscalls/sys_event.h"
#include "ppu_context.h"
#include "ps3emu/error_codes.h"

static uint8_t g_guest[0x10000];
uint8_t* vm_base = g_guest;

int ps3_log_verbose(void) { return 0; }
void ppu_dump_guest_stack(ppu_context* c, const char* t) { (void)c; (void)t; }
int spu_workload_dispatch_async(const uint8_t* e, uint32_t a, uint32_t b) { (void)e; (void)a; (void)b; return 0; }
uint32_t g_spu_pending_evt[3];
int g_spu_pending_evt_valid;
size_t spu_elf_image_size(const uint8_t* elf, size_t max_scan) { (void)elf; (void)max_scan; return 0; }

static int32_t flag_create(uint64_t init_pattern, uint32_t* out_id)
{
    ppu_context ctx = {0};
    ctx.gpr[3] = 0x100; /* id out address */
    ctx.gpr[4] = 0;     /* attr */
    ctx.gpr[5] = init_pattern;
    int64_t rc = sys_event_flag_create(&ctx);
    if (rc == CELL_OK && out_id) {
        uint32_t be;
        memcpy(&be, vm_base + 0x100, 4);
        *out_id = __builtin_bswap32(be);
    }
    return (int32_t)rc;
}

static int32_t flag_destroy(uint32_t flag_id)
{
    ppu_context ctx = {0};
    ctx.gpr[3] = flag_id;
    return (int32_t)sys_event_flag_destroy(&ctx);
}

static int32_t flag_set(uint32_t flag_id, uint64_t bitpat)
{
    ppu_context ctx = {0};
    ctx.gpr[3] = flag_id;
    ctx.gpr[4] = bitpat;
    return (int32_t)sys_event_flag_set(&ctx);
}

static int32_t flag_wait(uint32_t flag_id, uint64_t bitpat, uint32_t mode, uint64_t timeout_us, uint64_t* out_result)
{
    ppu_context ctx = {0};
    ctx.gpr[3] = flag_id;
    ctx.gpr[4] = bitpat;
    ctx.gpr[5] = mode;
    ctx.gpr[6] = 0x200; /* result out address */
    ctx.gpr[7] = timeout_us;
    int64_t rc = sys_event_flag_wait(&ctx);
    if (rc == CELL_OK && out_result) {
        uint64_t be;
        memcpy(&be, vm_base + 0x200, 8);
        *out_result = __builtin_bswap64(be);
    }
    return (int32_t)rc;
}

struct WaiterArg {
    uint32_t flag_id;
    int entered;
    int32_t rc;
    uint64_t result;
};

static void* waiter_thread(void* p)
{
    struct WaiterArg* arg = (struct WaiterArg*)p;
    __atomic_store_n(&arg->entered, 1, __ATOMIC_RELEASE);
    arg->rc = flag_wait(arg->flag_id, 0x1ull, SYS_EVENT_FLAG_WAIT_AND, 0, &arg->result);
    return NULL;
}

static int g_fails = 0;
#define CHECK(cond, msg, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
        g_fails++; \
    } \
} while (0)

int main(void)
{
    /* 1. Destroy without waiters gives OK */
    {
        uint32_t id = 0;
        int32_t rc = flag_create(0, &id);
        CHECK(rc == CELL_OK && id > 0, "flag_create failed: 0x%08X", (unsigned)rc);

        rc = flag_destroy(id);
        CHECK(rc == CELL_OK, "flag_destroy without waiters failed: 0x%08X", (unsigned)rc);

        /* Second destroy on inactive flag gives ESRCH */
        rc = flag_destroy(id);
        CHECK(rc == (int32_t)CELL_ESRCH, "second flag_destroy should be ESRCH: 0x%08X", (unsigned)rc);
    }

    /* 2. Destroy with blocked waiter gives EBUSY, waiter is woken by later set, then destroy gives OK */
    {
        uint32_t id = 0;
        int32_t rc = flag_create(0, &id);
        CHECK(rc == CELL_OK && id > 0, "flag_create failed: 0x%08X", (unsigned)rc);

        struct WaiterArg arg = {0};
        arg.flag_id = id;
        pthread_t th;
        int prc = pthread_create(&th, NULL, waiter_thread, &arg);
        CHECK(prc == 0, "pthread_create failed: %d", prc);

        /* Wait for thread to enter wait */
        while (!__atomic_load_n(&arg.entered, __ATOMIC_ACQUIRE)) {
            usleep(100);
        }
        /* Give waiter thread time to block on the condition variable */
        usleep(20000);

        /* Attempt to destroy while waiter is blocked: must return CELL_EBUSY */
        rc = flag_destroy(id);
        CHECK(rc == (int32_t)CELL_EBUSY, "destroy with waiter returned 0x%08X, expected CELL_EBUSY (0x%08X)",
              (unsigned)rc, (unsigned)CELL_EBUSY);

        /* The waiter is still blocked; set the flag to wake it */
        rc = flag_set(id, 0x1ull);
        CHECK(rc == CELL_OK, "flag_set failed: 0x%08X", (unsigned)rc);

        pthread_join(th, NULL);
        CHECK(arg.rc == CELL_OK, "waiter returned 0x%08X, expected CELL_OK", (unsigned)arg.rc);
        CHECK(arg.result == 0x1ull, "waiter result 0x%llX, expected 0x1", (unsigned long long)arg.result);

        /* Now with no waiters, destroy succeeds */
        rc = flag_destroy(id);
        CHECK(rc == CELL_OK, "destroy after wake failed: 0x%08X", (unsigned)rc);
    }

    if (g_fails != 0) {
        fprintf(stderr, "test_event_flag FAILED (%d failures)\n", g_fails);
        return 1;
    }
    printf("test_event_flag PASSED\n");
    return 0;
}
