/* The SDK version sys_process_get_sdk_version (lv2 syscall 25) reports, as one function.
 *
 * The syscall handler (lv2_register.c) and the HLE libraries that ask the same question the way
 * the firmware libraries do (cellRescGetBufferSize sizes its fragment buffer by it) must agree,
 * so both call this. The number is the title's: ppu_load_elf sets g_ps3_sdk_version from the
 * ELF's PROC_PARAM segment; PS3_SDK_VERSION, when set, overrides it (a diagnostic knob that
 * predates this file). Kept out of lv2_register.c so a test can link it without the syscall table. */
#include <stdint.h>
#include <stdlib.h>

extern uint32_t g_ps3_sdk_version;

uint32_t ps3_process_sdk_version(void)
{
    const char* override = getenv("PS3_SDK_VERSION");
    return override && *override ? (uint32_t)strtoul(override, NULL, 0) : g_ps3_sdk_version;
}
