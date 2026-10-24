/* Host coroutine boundary for the loader's existing per-thread dispatch state. */
#ifndef PS3_PPU_DISPATCH_STATE_H
#define PS3_PPU_DISPATCH_STATE_H
#include <stdint.h>
#define PS3_PPU_VCALL_SLOTS 128
#ifdef __cplusplus
extern "C" {
#endif
/* All three are NULL only when no PPU dispatcher is linked. With a loader,
 * coroutine owners preserve the counts and exchange the full storage pointer;
 * saving just the stack pointer lets another fiber overwrite live frames. */
int* ps3_ppu_dispatch_depth_slot(void);
int* ps3_ppu_vcall_sp_slot(void);
uint32_t** ps3_ppu_vcall_stack_slot(void);
#ifdef __cplusplus
}
#endif
#endif
