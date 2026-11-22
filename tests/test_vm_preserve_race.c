/* A preserved read against a write-fault release that starts between the
 * reader's wait for a non-BUSY page and its state load (SYNC-9 in
 * docs/UPSTREAM-DIVERGENCES.md). Ordered with the PS3_SYNC_TEST_HOOKS points
 * in vm_watch.c, never a sleep: after its wait the reader lets a writer fault
 * on the page, and the writer's release holds BUSY after its claim until the
 * reader has loaded the state. The reader so sees BUSY with WATCHED clear and
 * no copy published yet; the read must still return the sealed bytes, from
 * the copy. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../runtime/memory/vm_watch.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;

#define REGION (UINT64_C(64) << 20) /* guest [0, 64 MB) */
#define BASE UINT32_C(0x100000)
#define SEALED 0x11
#define WRITTEN 0x77

static uint32_t s_raced;   /* host page index of the raced page */
static int s_armed;        /* hooks act only once the session is sealed */
static int s_go, s_claimed, s_loaded;

static void wait_flag(const int* f)
{
    while (!__atomic_load_n(f, __ATOMIC_ACQUIRE)) sched_yield();
}
static void set_flag(int* f) { __atomic_store_n(f, 1, __ATOMIC_RELEASE); }

/* Runs in the writer's fault handler for the release point: atomics only. */
int ps3_sync_test_hook(const char* point, uint32_t value)
{
    if (value != s_raced || !__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE)) return 0;
    if (strcmp(point, "vm.preserve_read.after_wait") == 0) {
        /* The first pass only: the page is WATCHED and not BUSY here. */
        if (!__atomic_load_n(&s_go, __ATOMIC_ACQUIRE)) {
            set_flag(&s_go);
            wait_flag(&s_claimed);
        }
    } else if (strcmp(point, "vm.preserve_read.after_load") == 0) {
        set_flag(&s_loaded);
    } else if (strcmp(point, "vm.release.after_claim") == 0) {
        set_flag(&s_claimed);
        wait_flag(&s_loaded);
    }
    return 0;
}

static void* writer(void* arg)
{
    wait_flag(&s_go);
    *(volatile uint8_t*)arg = WRITTEN; /* faults: the page is watched */
    return NULL;
}

int main(void)
{
    const uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    vm_base = (uint8_t*)mmap(NULL, (size_t)REGION, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(vm_base != MAP_FAILED);
    ppu_vm_size = (uint32_t)REGION;
    setenv("PS3RECOMP_VM_WRITE_WATCH", "1", 1);
    assert(vm_watch_enable());

    memset(vm_base + BASE, SEALED, (size_t)page);
    const int s = vm_preserve_begin();
    assert(s);
    assert(vm_watch_preserve(BASE, (uint32_t)page));
    assert(vm_preserve_seal(s));
    s_raced = (uint32_t)(BASE / page);
    set_flag(&s_armed);

    pthread_t t;
    assert(pthread_create(&t, NULL, writer, vm_base + BASE) == 0);
    uint8_t* buf = (uint8_t*)malloc((size_t)page);
    assert(buf);
    uint32_t pages = 0;
    const int r = vm_preserve_read(s, BASE, buf, (uint32_t)page, &pages);
    pthread_join(t, NULL);
    assert(__atomic_load_n(&s_loaded, __ATOMIC_ACQUIRE)); /* the race was staged */
    if (r != 0 || pages != 1) {
        fprintf(stderr, "FAIL: read %d, %u preserved page(s): the release in progress read as finished\n", r,
                pages);
        return 1;
    }
    for (uint64_t i = 0; i < page; ++i) assert(buf[i] == SEALED);
    assert(vm_base[BASE] == WRITTEN);
    vm_preserve_end(s);
    free(buf);
    printf("vm_preserve_race: ok\n");
    return 0;
}
