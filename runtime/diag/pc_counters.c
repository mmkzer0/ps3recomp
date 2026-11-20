/*
 * ps3recomp - per-instruction execution counters (see include/ps3emu/pc_counters.h)
 */
#include "ps3emu/pc_counters.h"
#include "ps3emu/host_exit.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t* s_pcs;
static uint64_t* s_slots;
/* Published last, with release: a reader that sees n != 0 sees both pointers. */
static atomic_size_t s_n;
static atomic_flag s_registered = ATOMIC_FLAG_INIT;
static atomic_flag s_dumped = ATOMIC_FLAG_INIT;
static atomic_int s_dump_done;
static char* s_out_path;

static void report_write_failure(const char* path, const char* what, int err)
{
    fprintf(stderr, "[ps3_pc_counters] PS3_PC_COUNTERS_OUT=%s: %s: %s\n", path, what,
            err ? strerror(err) : "write error");
    fflush(stderr);
}

static void write_dump(void)
{
    const size_t n = atomic_load_explicit(&s_n, memory_order_acquire);
    FILE* f = fopen(s_out_path, "w");
    if (!f) {
        report_write_failure(s_out_path, "cannot open for writing", errno);
        return;
    }
    fprintf(f, "# ps3 pc counters N=%zu\n", n);
    for (size_t k = 0; k < n; k++)
        fprintf(f, "0x%08X %llu\n", (unsigned)s_pcs[k],
                (unsigned long long)__atomic_load_n(&s_slots[k], __ATOMIC_RELAXED));
    const int stream_bad = ferror(f);
    const int close_bad = fclose(f);
    if (stream_bad || close_bad)
        report_write_failure(s_out_path, "cannot write", close_bad ? errno : 0);
}

/* The first caller writes, whichever route it came by (dump_now, exit(),
 * quick_exit()); a later one waits for that write to finish and returns, so a
 * process that ends right after a dump_now() on another thread cannot cut the
 * file short. Not async-signal-safe. */
static void dump_once(void)
{
    if (atomic_flag_test_and_set(&s_dumped)) {
        while (!atomic_load_explicit(&s_dump_done, memory_order_acquire)) {}
        return;
    }
    write_dump();
    atomic_store_explicit(&s_dump_done, 1, memory_order_release);
}

static void dump_at_exit(void)
{
    dump_once();
}

void ps3_pc_counters_dump_now(void)
{
    /* No table, or no PS3_PC_COUNTERS_OUT at registration: nothing to write. */
    if (!atomic_load_explicit(&s_n, memory_order_acquire) || !s_out_path) return;
    dump_once();
}

void ps3_pc_counters_register(const uint32_t* pcs, uint64_t* slots, size_t n)
{
    if (!pcs || !slots || n == 0) {
        fprintf(stderr, "[ps3_pc_counters] register: empty or null counter table (n=%zu)\n", n);
        abort();
    }
    if (atomic_flag_test_and_set(&s_registered)) {
        fprintf(stderr, "[ps3_pc_counters] register: a second counter table; "
                        "only one lifted image may carry --pc-counters\n");
        abort();
    }
    s_pcs = pcs;
    s_slots = slots;

    const char* out = getenv("PS3_PC_COUNTERS_OUT");   /* where --pc-counters counts are written */
    if (out && *out) {
        s_out_path = strdup(out);
        if (!s_out_path)
            fprintf(stderr, "[ps3_pc_counters] PS3_PC_COUNTERS_OUT=%s: out of memory; no dump will be written\n", out);
    }
    /* Published last, after the path: dump_now() and the readers key on it. */
    atomic_store_explicit(&s_n, n, memory_order_release);
    if (!s_out_path) return;
    if (atexit(dump_at_exit) != 0 || ps3_host_at_quick_exit(dump_at_exit) != 0)
        fprintf(stderr, "[ps3_pc_counters] PS3_PC_COUNTERS_OUT=%s: could not register the exit "
                        "handler; the dump may not be written\n", out);
}

size_t ps3_pc_counters_count(void)
{
    return atomic_load_explicit(&s_n, memory_order_acquire);
}

uint32_t ps3_pc_counters_pc(size_t k)
{
    return k < ps3_pc_counters_count() ? s_pcs[k] : UINT32_MAX;
}

size_t ps3_pc_counters_read(uint64_t* out, size_t n)
{
    const size_t count = ps3_pc_counters_count();
    const size_t m = n < count ? n : count;
    for (size_t k = 0; k < m; k++)
        out[k] = __atomic_load_n(&s_slots[k], __ATOMIC_RELAXED);
    return count;
}
