/*
 * ps3recomp - per-instruction execution counters (tools/ppu_lifter.py --pc-counters)
 *
 * A lift made with --pc-counters FILE starts each listed guest instruction with
 * a relaxed atomic add on its slot of a u64 table the generated source defines
 * (ppu_pc_counter_slots[], PCs in ppu_pc_counter_pcs[], slot k <-> k-th listed
 * PC in ascending order) and registers that table here before main(). This
 * module only reads it. A lift without counters never registers, so every
 * read below sees an empty table and nothing links against missing symbols.
 *
 * Diagnostic only. With PS3_PC_COUNTERS_OUT=<path> in the environment when the
 * table registers, the counts are written to <path> when the process ends --
 * through exit() or through ps3_host_exit() (quick_exit) alike:
 *
 *     # ps3 pc counters N=<n>
 *     0x<PC, 8 hex digits> <decimal count>        (one line per slot, slot order)
 *
 * A file that cannot be written is reported on stderr with its path. Counts
 * read while guest threads still run are each a consistent u64, not a snapshot
 * of all slots at one instant. A process that ends through _exit() or a signal
 * (a headless run stopped by SIGTERM/SIGKILL) writes nothing at exit: call
 * ps3_pc_counters_dump_now() where the measurement window ends instead.
 */

#ifndef PS3EMU_PC_COUNTERS_H
#define PS3EMU_PC_COUNTERS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the generated source (a constructor in the table chunk) with its
 * counter table. A second table, or an empty/null one, aborts: one image
 * carries counters. */
void ps3_pc_counters_register(const uint32_t* pcs, uint64_t* slots, size_t n);

/* Number of slots; 0 when the lift has no counters. */
size_t ps3_pc_counters_count(void);

/* Guest PC of slot k. Listed PCs are 4-byte aligned, so UINT32_MAX (what an
 * out-of-range k returns) never names one. */
uint32_t ps3_pc_counters_pc(size_t k);

/* Copies min(n, count) slot values to out (each a relaxed atomic load) and
 * returns the slot count, so a result above n means out was too short. */
size_t ps3_pc_counters_read(uint64_t* out, size_t n);

/* Writes the PS3_PC_COUNTERS_OUT dump now, in the format above. It shares the
 * exit dump's once-flag: the first dump from any route wins (a later
 * dump_now() or the exit handler waits for it, then returns), so the file
 * holds the counts at that first call. Does nothing when no table is
 * registered or PS3_PC_COUNTERS_OUT was unset at registration. Not
 * async-signal-safe: call it from a thread, not a signal handler. */
void ps3_pc_counters_dump_now(void);

/* Writes the counts now to `path`, outside the exit dump's once-flag: any number of snapshots can be taken while
 * the process runs. Does nothing when no table is registered. Same format and caveats as the dump above. */
void ps3_pc_counters_snapshot(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_PC_COUNTERS_H */
