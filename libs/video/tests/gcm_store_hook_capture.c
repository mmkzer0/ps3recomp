/* The loader's side of ppu_set_store_hook for test_gcm_label_store.c: remembers what the runtime registered.
 * A strong definition here overrides the default setter in cellGcmSys.c, which registers nothing. */
#include <stdint.h>

void (*g_hook)(uint32_t ea, uint32_t value);
uint32_t g_hook_lo, g_hook_len;

void ppu_set_store_hook(uint32_t lo, uint32_t len, void (*fn)(uint32_t ea, uint32_t value))
{
    g_hook_lo = lo;
    g_hook_len = len;
    g_hook = fn;
}
