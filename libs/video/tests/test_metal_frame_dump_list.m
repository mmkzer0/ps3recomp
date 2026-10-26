/* PS3RECOMP_METAL_FRAME_DUMP_LIST: at most 32 present numbers, and a longer list is a named stop that
 * says how many it was, not a silent cut to the first 32 (M3 gap 3a).
 *
 * Drives the backend's own list parser (fdump_parse) with the environment set the way a user sets it. No GPU
 * is touched. usage: test_metal_frame_dump_list <count>
 *   32 or fewer   prints "kept N" and exits 0 when the parser kept exactly N entries
 *   more          the parser must stop (config-invalid); the exit status is the stop's, 70
 */
#include "rsx_metal_backend.m"
#include <stdlib.h>

u8* vm_base;
u32 ppu_vm_size;

int main(int argc, char** argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s <count>\n", argv[0]); return 2; }
    const unsigned count = (unsigned)strtoul(argv[1], NULL, 10);
    char list[512];
    size_t at = 0;
    for (unsigned i = 0; i < count; i++)
        at += (size_t)snprintf(list + at, sizeof list - at, "%s%u", i ? "," : "", 100u + i);
    /* The backend finds ps3_stop by name; dead-stripping must not remove what only that lookup reaches. The
     * variable itself is volatile (not the pointee), or an optimised build drops the store and the symbol. */
    static const void* volatile keep_stop;
    keep_stop = (const void*)ps3_stop;
    setenv("PS3RECOMP_METAL_FRAME_DUMP_LIST", list, 1);
    fdump_parse();
    printf("kept %u of %u, first %u, last %u\n", s_fdump.n_list, count, s_fdump.list[0],
           s_fdump.list[s_fdump.n_list ? s_fdump.n_list - 1 : 0]);
    return s_fdump.n_list == count ? 0 : 1;
}
