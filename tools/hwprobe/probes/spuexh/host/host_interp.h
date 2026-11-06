#ifndef HOST_INTERP_H
#define HOST_INTERP_H
#include <stddef.h>
#include <stdint.h>
typedef struct { int exited, timed_out; uint64_t steps; char desc[160]; } hi_res_t;
/* Runs n copies of the SPU ELF (thread i gets arg1[i] in $3 and the supervisor's SPU index idx[i]); poll() is called
 * every 20 ms and a nonzero return aborts the threads; limit_ms bounds the call (0 = none). guest is the byte array
 * "main memory" (an effective address is an offset into it). inject_spu/inject_n: kill that SPU at its n-th DMA put;
 * inject_group: the kill ends the whole group (every other thread stops mid-work, the console's group-exception model).
 * hang_exit_spu >= 0: that SPU publishes its final status and then never leaves the thread (a thread lv2 keeps running);
 * only the supervisor's abort releases it, and res[].exited stays 0. */
int hi_run(int n, const int *idx, const uint64_t *arg1, uint8_t *guest, size_t guest_size, const uint8_t *elf, size_t elf_len,
           int inject_spu, int inject_n, int inject_group, int hang_exit_spu, int (*poll)(void *), void *pu, int limit_ms,
           hi_res_t *res);
#endif
