/* Ordered regressions for SPU report-hold state, bitset reads, and taskset registration. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ps3emu/config.h"
#include "ps3emu/stop.h"

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = sizeof(g_guest_mem);

#include "../spu_channels.c"

void spu_taskset_parked_add(uint32_t taskset_ea, uint32_t taskId, uint32_t wait_obj);
void spu_taskset_parked_del(uint32_t taskset_ea, uint32_t taskId);
void spu_taskset_round_drop(uint32_t taskset_ea, uint32_t taskId);
int spu_taskset_round_busy(uint32_t* taskset_ea, uint32_t* taskId);
int spu_taskset_wait_signal(uint32_t taskset_ea, uint32_t taskId);
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);
void spu_taskset_register(uint32_t ea);
void spu_taskset_line_committed(uint32_t line);

#define SIGNALLED 0x40u
#define TASKSET_WAKE 0x00030000u
#define TASKSET_RACE 0x00032000u
#define TASKSET_REG_A 0x00050000u
#define TASKSET_REG_B 0x00052000u
#define WAITER 3u
#define RACE_TASK 7u

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

typedef struct latch {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int set;
    int release;
} latch;

static void latch_init(latch* l)
{
    pthread_mutex_init(&l->mutex, NULL);
    pthread_cond_init(&l->cond, NULL);
    l->set = 0;
    l->release = 0;
}

static void latch_reset(latch* l)
{
    pthread_mutex_lock(&l->mutex);
    l->set = 0;
    l->release = 0;
    pthread_mutex_unlock(&l->mutex);
}

static void latch_set(latch* l)
{
    pthread_mutex_lock(&l->mutex);
    l->set = 1;
    pthread_cond_broadcast(&l->cond);
    pthread_mutex_unlock(&l->mutex);
}

static int latch_wait(latch* l)
{
    pthread_mutex_lock(&l->mutex);
    while (!l->set) pthread_cond_wait(&l->cond, &l->mutex);
    pthread_mutex_unlock(&l->mutex);
    return 1;
}

static void latch_wait_release(latch* l)
{
    pthread_mutex_lock(&l->mutex);
    while (!l->release) pthread_cond_wait(&l->cond, &l->mutex);
    pthread_mutex_unlock(&l->mutex);
}

static void latch_release(latch* l)
{
    pthread_mutex_lock(&l->mutex);
    l->release = 1;
    pthread_cond_broadcast(&l->cond);
    pthread_mutex_unlock(&l->mutex);
}

/* A reusable two-party barrier. The accesses between two waits are unordered. */
typedef struct test_gate {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned count;
    unsigned phase;
} test_gate;

static void gate_init(test_gate* g)
{
    pthread_mutex_init(&g->mutex, NULL);
    pthread_cond_init(&g->cond, NULL);
    g->count = 0;
    g->phase = 0;
}

static void gate_wait(test_gate* g)
{
    pthread_mutex_lock(&g->mutex);
    const unsigned phase = g->phase;
    if (++g->count == 2) {
        g->count = 0;
        g->phase++;
        pthread_cond_broadcast(&g->cond);
    } else {
        while (g->phase == phase) pthread_cond_wait(&g->cond, &g->mutex);
    }
    pthread_mutex_unlock(&g->mutex);
}

static latch s_wait_entered, s_after_clear, s_release_clear;
static test_gate s_registration_gate;
static volatile int s_registration_gate_enabled;
static volatile int s_taskset_seen_a, s_taskset_seen_b;

int ps3_sync_test_hook(const char* point, uint32_t value)
{
    if (strcmp(point, "spu.wait_signal.before_wait") == 0 && value == TASKSET_WAKE)
        latch_set(&s_wait_entered);
    else if (strcmp(point, "spu.wait_signal.after_clear") == 0 && value == TASKSET_WAKE) {
        latch_set(&s_after_clear);
        latch_wait_release(&s_release_clear);
    } else if (strcmp(point, "spu.taskset_register.after_count") == 0 &&
               __atomic_load_n(&s_registration_gate_enabled, __ATOMIC_ACQUIRE)) {
        gate_wait(&s_registration_gate);
    } else if (strcmp(point, "spu.taskset.line_match") == 0) {
        if (value == TASKSET_REG_A)
            __atomic_add_fetch(&s_taskset_seen_a, 1, __ATOMIC_ACQ_REL);
        else if (value == TASKSET_REG_B)
            __atomic_add_fetch(&s_taskset_seen_b, 1, __ATOMIC_ACQ_REL);
    }
    return 0;
}

static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

static volatile int s_wait_done;
static void* wait_signal_main(void* unused)
{
    (void)unused;
    spu_taskset_parked_add(TASKSET_WAKE, WAITER, 0);
    spu_taskset_wait_signal(TASKSET_WAKE, WAITER);
    /* The real channel caller removes this entry after WAIT_SIGNAL returns. */
    spu_taskset_parked_del(TASKSET_WAKE, WAITER);
    spu_taskset_round_drop(TASKSET_WAKE, WAITER);
    __atomic_store_n(&s_wait_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_woken_task_stays_busy_until_handoff(void)
{
    latch_reset(&s_wait_entered);
    latch_reset(&s_after_clear);
    latch_reset(&s_release_clear);
    __atomic_store_n(&s_wait_done, 0, __ATOMIC_RELEASE);
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL, wait_signal_main, NULL) == 0);
    CHECK(latch_wait(&s_wait_entered));
    CHECK(spu_taskset_round_busy(NULL, NULL) == 0);

    spu_taskset_signal_task(TASKSET_WAKE, WAITER);
    CHECK(latch_wait(&s_after_clear));
    CHECK(spu_taskset_round_busy(NULL, NULL) == 1);

    latch_release(&s_release_clear);
    CHECK(pthread_join(waiter, NULL) == 0);
    CHECK(__atomic_load_n(&s_wait_done, __ATOMIC_ACQUIRE));
    CHECK(spu_taskset_round_busy(NULL, NULL) == 0);
}

static void wr_be32(uint32_t ea, uint32_t value)
{
    uint8_t* p = g_guest_mem + ea;
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

typedef struct race_args {
    test_gate* gate;
    int signal_bit;
} race_args;

static void* round_busy_reader(void* p)
{
    race_args* a = (race_args*)p;
    gate_wait(a->gate);
    (void)spu_taskset_round_busy(NULL, NULL);
    gate_wait(a->gate);
    return NULL;
}

static void* round_state_writer(void* p)
{
    race_args* a = (race_args*)p;
    gate_wait(a->gate);
    if (a->signal_bit)
        spu_taskset_signal_task(TASKSET_RACE, RACE_TASK);
    else
        spu_taskset_parked_del(TASKSET_RACE, RACE_TASK);
    gate_wait(a->gate);
    return NULL;
}

static void test_report_state_reads_are_synchronized(void)
{
    const uint32_t bit_word = TASKSET_RACE + SIGNALLED + (RACE_TASK / 32u) * 4u;
    memset(g_guest_mem + TASKSET_RACE, 0, 0x100);
    spu_taskset_parked_add(TASKSET_RACE, RACE_TASK, 0);

    test_gate gate;
    gate_init(&gate);
    race_args args = { &gate, 0 };
    pthread_t reader, writer;
    CHECK(pthread_create(&reader, NULL, round_busy_reader, &args) == 0);
    CHECK(pthread_create(&writer, NULL, round_state_writer, &args) == 0);
    CHECK(pthread_join(reader, NULL) == 0);
    CHECK(pthread_join(writer, NULL) == 0);
    spu_taskset_round_drop(TASKSET_RACE, RACE_TASK);

    memset(g_guest_mem + TASKSET_RACE, 0, 0x100);
    wr_be32(bit_word, 0);
    spu_taskset_parked_add(TASKSET_RACE, RACE_TASK, 0);
    args.signal_bit = 1;
    CHECK(pthread_create(&reader, NULL, round_busy_reader, &args) == 0);
    CHECK(pthread_create(&writer, NULL, round_state_writer, &args) == 0);
    CHECK(pthread_join(reader, NULL) == 0);
    CHECK(pthread_join(writer, NULL) == 0);
    spu_taskset_parked_del(TASKSET_RACE, RACE_TASK);
    spu_taskset_round_drop(TASKSET_RACE, RACE_TASK);
    memset(g_guest_mem + TASKSET_RACE, 0, 0x100);
}

static void* register_taskset(void* p)
{
    spu_taskset_register((uint32_t)(uintptr_t)p);
    return NULL;
}

static void test_concurrent_taskset_registration(void)
{
    memset(g_guest_mem + TASKSET_REG_A, 0, 0x100);
    memset(g_guest_mem + TASKSET_REG_B, 0, 0x100);
    __atomic_store_n(&s_taskset_seen_a, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_taskset_seen_b, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_registration_gate_enabled, 1, __ATOMIC_RELEASE);
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, register_taskset, (void*)(uintptr_t)TASKSET_REG_A) == 0);
    CHECK(pthread_create(&b, NULL, register_taskset, (void*)(uintptr_t)TASKSET_REG_B) == 0);
    CHECK(pthread_join(a, NULL) == 0);
    CHECK(pthread_join(b, NULL) == 0);
    __atomic_store_n(&s_registration_gate_enabled, 0, __ATOMIC_RELEASE);

    spu_taskset_line_committed(TASKSET_REG_A);
    spu_taskset_line_committed(TASKSET_REG_B);
    CHECK(__atomic_load_n(&s_taskset_seen_a, __ATOMIC_ACQUIRE) == 1);
    CHECK(__atomic_load_n(&s_taskset_seen_b, __ATOMIC_ACQUIRE) == 1);
}

/* Runtime functions outside the path under test. */
static ps3_config s_test_config;
const ps3_config* ps3_config_get(void) { return &s_test_config; }

uint64_t ppu_store_window;
int ps3_label_window_overlaps(uint64_t ea, uint32_t size) { (void)ea; (void)size; return 0; }
unsigned long long ps3_label_window_note_spu_put(void) { return 0; }

PS3_NORETURN void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy,
                           const ps3_stop_field* fields, size_t nfields,
                           const ps3_stop_site* site)
{
    (void)kind; (void)remedy; (void)fields; (void)nfields; (void)site;
    fprintf(stderr, "unexpected stop: %s\n", what);
    abort();
}
PS3_NORETURN void ps3_stop_guest_range(const char* func, const char* arg,
                                       uint64_t ea, uint64_t size, uint64_t limit)
{
    (void)func; (void)arg; (void)ea; (void)size; (void)limit;
    abort();
}
PS3_NORETURN void ps3_stop_spu_label_window(const char* func, uint32_t cmd, uint64_t ea,
                                            uint32_t size, uint32_t tag, uint64_t count,
                                            const ps3_stop_site* site)
{
    (void)func; (void)cmd; (void)ea; (void)size; (void)tag; (void)count; (void)site;
    abort();
}
PS3_NORETURN void ps3_stop_spurs_task_halt(const char* api, uint32_t taskset, uint32_t task,
                                           uint32_t elf, const char* reason,
                                           const ps3_stop_site* site)
{
    (void)api; (void)taskset; (void)task; (void)elf; (void)reason; (void)site;
    abort();
}
PS3_NORETURN void ps3_stop_spurs_dispatch_miss(const char* api, uint32_t owner, int32_t task,
                                               uint32_t image, uint32_t size, uint64_t fingerprint,
                                               const ps3_stop_site* site)
{
    (void)api; (void)owner; (void)task; (void)image; (void)size; (void)fingerprint; (void)site;
    abort();
}

/* Runtime functions outside the path under test. */
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void) { return now_ms(); }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) { }
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t taskset, uint32_t task, uint32_t a, uint32_t b)
{ (void)ls; (void)taskset; (void)task; (void)a; (void)b; return 0; }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

int main(void)
{
    latch_init(&s_wait_entered);
    latch_init(&s_after_clear);
    latch_init(&s_release_clear);
    gate_init(&s_registration_gate);

    test_woken_task_stays_busy_until_handoff();
    test_report_state_reads_are_synchronized();
    test_concurrent_taskset_registration();

    printf("SPU report-hold concurrency: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
