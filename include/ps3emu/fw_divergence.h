/*
 * ps3recomp - count the branches where an HLE handler knowingly differs from the firmware
 */
#ifndef PS3RECOMP_FW_DIVERGENCE_H
#define PS3RECOMP_FW_DIVERGENCE_H

/* A firmware comparison can show that an HLE branch returns something the firmware would not, without
 * showing whether any title ever reaches that branch. Before such a branch is changed, a run must show
 * whether the title hits it:
 *
 *     if (find_fiber(fiber)) { PS3_FW_DIVERGENCE("F21"); return STAT; }
 *
 * Each site counts its hits and logs the 1st, 2nd, 4th, 8th, ... hit, so a hot path cannot flood the log,
 * and a log cut short by SIGKILL still shows any hit:
 *
 *     [fw-divergence] id=F21 hits=4 at=cellFiber.c:471
 *
 * The id names the row of the firmware comparison that describes the divergence. Counting never changes
 * behaviour. The counter is process-wide by contract (one per site, shared by every guest thread), so it
 * is atomic; a relaxed increment suffices because only the count is observed. */

#if defined(__FILE_NAME__)
#  define PS3_FW_DIVERGENCE_FILE __FILE_NAME__
#else
#  define PS3_FW_DIVERGENCE_FILE __FILE__
#endif

#ifdef __cplusplus
#include <atomic>
#include <cstdio>
typedef std::atomic<unsigned long long> ps3_fw_divergence_counter;
static inline unsigned long long ps3_fw_divergence_bump(ps3_fw_divergence_counter* c)
{
    return c->fetch_add(1, std::memory_order_relaxed) + 1;
}
#else
#include <stdatomic.h>
#include <stdio.h>
typedef _Atomic unsigned long long ps3_fw_divergence_counter;
static inline unsigned long long ps3_fw_divergence_bump(ps3_fw_divergence_counter* c)
{
    return atomic_fetch_add_explicit(c, 1, memory_order_relaxed) + 1;
}
#endif

static inline void ps3_fw_divergence_hit(ps3_fw_divergence_counter* c, const char* id, const char* file, int line)
{
    const unsigned long long n = ps3_fw_divergence_bump(c);
    if ((n & (n - 1)) == 0)
        fprintf(stderr, "[fw-divergence] id=%s hits=%llu at=%s:%d\n", id, n, file, line);
}

#define PS3_FW_DIVERGENCE(id) do { \
        static ps3_fw_divergence_counter ps3_fw_divergence_site_; \
        ps3_fw_divergence_hit(&ps3_fw_divergence_site_, (id), PS3_FW_DIVERGENCE_FILE, __LINE__); \
    } while (0)

#endif /* PS3RECOMP_FW_DIVERGENCE_H */
