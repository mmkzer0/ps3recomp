/*
 * ps3recomp - lv2 syscall 988 (0x3DC)
 *
 * Not sys_tty_write. An earlier alias sent 988 to the TTY writer; an SDK
 * CRT issues it from exit() as (r3 = 1, r4 = 0) with r5..r10 left over from
 * the caller, and a TTY writer reading r4 as a buffer faults on the null
 * page. The same SDK's real console write is syscall 403, issued a few
 * instructions after another 988 in one function: two different calls.
 *
 * Contract. Evidence order per AGENTS.md; the firmware tier is an lv2 kernel
 * dump of FW 4.81 REBUG/Cobra (not stock 4.93; the handler carries no Cobra
 * redirection, so stock is assumed identical), handler at lv2 VA
 * 0x8000000000277688 in the syscall array next to 402/403's, where the
 * unimplemented slots point at the shared ENOSYS stub 0x800000000026D2A0.
 * The handler takes no guest pointer and stores nothing to guest memory on
 * any path:
 *   - r3 is a kind id: `cmplwi r,0x10` on the low word, then the bit test
 *     (1 << id) & 0x10116 (`sld`, mask 0x10116 built at ..2776B4/..2776BC,
 *     `and` ..2776C4, `cmpdi` ..2776C8): only 1, 2, 4, 8 and 16 pass; any
 *     other id is CELL_EINVAL.
 *   - r4 is a flag word: `cmpwi r,0` on the low word at ..2776F4; non-zero is
 *     CELL_EINVAL.
 *   - Then it resolves the caller's own process (pid from the thread block);
 *     if that fails the answer is CELL_ESRCH (0x8001000F). The calling guest
 *     thread's process always exists in this runtime, so the branch cannot be
 *     taken here and is not modelled.
 *   - Otherwise CELL_OK, with no capability check (no ROOT/DBG gate).
 * RPCS3 at d00d80231af3f9b0286f38fd72f7513583f1a9d4 (rpcs3/Emu/Cell/lv2/
 * lv2.cpp:961, "sys_dbg_..._ppu_exception_handler") binds the slot to
 * `null_func`: it checks nothing and answers CELL_OK, which agrees with the
 * firmware's success path only. Where they differ (the EINVAL cases) the
 * firmware wins. What the kind ids select is not known, and the success path
 * is not observable from here, so it has no effect.
 */

#include "sys_dbg.h"

#include <stdint.h>

#define SYS_DBG_988_KIND_MAX   0x10u
#define SYS_DBG_988_KIND_MASK  0x10116ull   /* ids 1, 2, 4, 8, 16 */

int64_t sys_dbg_988(ppu_context* ctx)
{
    const uint32_t id   = (uint32_t)ctx->gpr[3];
    const uint32_t flag = (uint32_t)ctx->gpr[4];
    if (id > SYS_DBG_988_KIND_MAX || !((1ull << id) & SYS_DBG_988_KIND_MASK))
        return (int64_t)(int32_t)CELL_EINVAL;
    if (flag != 0)
        return (int64_t)(int32_t)CELL_EINVAL;
    return 0;
}

void sys_dbg_init(lv2_syscall_table* tbl)
{
    lv2_syscall_register(tbl, 988, sys_dbg_988);
}
