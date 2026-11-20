// Fixture for tools/test_ppu_pc_counters_run.py: runs the lifted snippet
//     r4 = 0; ctr = r3; do { r4 += 1; } while (--ctr);
// (a loop of r3 trips), prints what the counter API reports, then leaves the
// process the way argv[1] says.
//
//   test_pc_counters <exit|quick|dumpnow-exit|dumpnow-quick> <threads> <trips>...
//
// Every thread calls the function once per <trips> value, so a counter that
// loses an update under contention shows up as a short count. The dumpnow-
// modes call ps3_pc_counters_dump_now() after that first round, then run the
// whole round again (the printed counts are the doubled ones) and leave
// through exit() or ps3_host_exit(): the dump file must hold the first round.
#include "ppu_recomp.h"
#include "ps3emu/host_exit.h"
#include "ps3emu/pc_counters.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_ntrips;
static uint64_t g_trips[16];

static void* run(void* out_r4)
{
    static thread_local ppu_context ctx;
    for (int i = 0; i < g_ntrips; i++) {
        memset(&ctx, 0, sizeof ctx);
        ctx.gpr[3] = g_trips[i];
        func_00010000(&ctx);
    }
    *static_cast<uint64_t*>(out_r4) = ctx.gpr[4];
    return nullptr;
}

static uint64_t run_round(int threads)
{
    pthread_t t[64];
    uint64_t r4[64] = {};
    for (int i = 0; i < threads; i++) pthread_create(&t[i], nullptr, run, &r4[i]);
    for (int i = 0; i < threads; i++) pthread_join(t[i], nullptr);
    return r4[0];
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "";
    const bool dump_now = !strncmp(mode, "dumpnow-", 8);
    const char* leave = dump_now ? mode + 8 : mode;
    if (argc < 4 || (strcmp(leave, "exit") && strcmp(leave, "quick")) || argc - 3 > 16) {
        fprintf(stderr, "usage: %s <exit|quick|dumpnow-exit|dumpnow-quick> <threads> <trips>...\n", argv[0]);
        return 2;
    }
    const int threads = atoi(argv[2]);
    g_ntrips = argc - 3;
    for (int i = 0; i < g_ntrips; i++) g_trips[i] = strtoull(argv[3 + i], nullptr, 0);
    if (threads < 1 || threads > 64) return 2;

    uint64_t r4 = run_round(threads);
    if (dump_now) {
        ps3_pc_counters_dump_now();
        ps3_pc_counters_dump_now();   // a second call is a no-op: the first dump wins
        r4 = run_round(threads);
    }

    printf("r4=%llu\n", (unsigned long long)r4);
    const size_t n = ps3_pc_counters_count();
    printf("slots=%zu\n", n);
    uint64_t counts[64] = {};
    const size_t total = ps3_pc_counters_read(counts, n < 64 ? n : 64);
    for (size_t k = 0; k < n && k < 64; k++)
        printf("slot %zu pc=0x%08X count=%llu\n", k, (unsigned)ps3_pc_counters_pc(k),
               (unsigned long long)counts[k]);
    uint64_t one[1] = {0};
    printf("read_total=%zu\n", total);
    printf("read_short_total=%zu\n", ps3_pc_counters_read(one, 1));
    printf("pc_oob=0x%08X\n", (unsigned)ps3_pc_counters_pc(n));
    fflush(stdout);

    if (!strcmp(leave, "quick")) ps3_host_exit(0);
    return 0;
}
