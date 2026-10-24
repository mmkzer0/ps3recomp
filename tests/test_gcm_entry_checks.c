/* The entry checks of cellGcmGetConfiguration, cellGcmAddressToOffset, cellGcmMapMainMemory and cellGcmBindTile, as
 * libgcm_sys makes them (fw/lift/libgcm_sys, disassembly cited at each check in libs/video/cellGcmSys.c). Includes the
 * real implementation and drives its public entry points; the expected codes are the firmware's, not derived from the
 * code under test:
 *   - BindTile: an index of 15 or more is 0x80210003 (INVALID_VALUE), 0x02002AD4.
 *   - MapMainMemory: a misaligned EA or size is 0x802100FF (FAILURE), not INVALID_ALIGNMENT, 0x020048D8; a run that does
 *     not fit is 0x80210001 (NO_IO_PAGE_TABLE), 0x0200499C; nothing is stored on those paths.
 *   - AddressToOffset: an EA with no IO page is 0x802100FF (FAILURE) with *offset untouched, 0x02003FDC -> 0x02003FBC.
 *   - A NULL out-parameter is a store into the null page (guest-null-access), reached only where the firmware stores.
 *   - What the firmware does not test and lv2 or undefined state answers (size 0, a map before Init, the runtime's own
 *     full mapping list) is a named stop, not an invented code. */
#include "../libs/video/cellGcmSys.c"
#include <assert.h>
#include <setjmp.h>

uint8_t* vm_base;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t addr) { (void)addr; abort(); }
void spu_lockline_lock(void) { abort(); }
void spu_lockline_unlock(void) { abort(); }
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);   /* set by cellGcmInit */

/* A stop here is the thing under test: record which one and unwind to the check that armed it. A stop nobody armed
 * for is a bug in the code or the fixture, and ends the run. */
static jmp_buf g_jb;
static int g_armed, g_stopped;
static int g_stop_kind;
static char g_stop_func[64], g_stop_feature[64], g_stop_ea[32];
static void stop_record(int kind, const char* func, const char* feature, const char* ea)
{
    if (!g_armed) { fprintf(stderr, "unexpected stop kind %d in %s\n", kind, func); abort(); }
    g_stopped = 1; g_stop_kind = kind;
    snprintf(g_stop_func, sizeof g_stop_func, "%s", func ? func : "");
    snprintf(g_stop_feature, sizeof g_stop_feature, "%s", feature ? feature : "");
    snprintf(g_stop_ea, sizeof g_stop_ea, "%s", ea ? ea : "");
    longjmp(g_jb, 1);
}
PS3_NORETURN void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy, const ps3_stop_field* f,
                           size_t n, const ps3_stop_site* site)
{
    (void)what; (void)remedy; (void)site;
    const char *fn = "", *ea = "";
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(f[i].key, "fn")) fn = f[i].value;
        if (!strcmp(f[i].key, "ea")) ea = f[i].value;
    }
    stop_record((int)kind, fn, "", ea);
    abort();
}
PS3_NORETURN void ps3_stop_gcm_unmodelled(const char* func, const char* feature, const char* reason)
{
    (void)reason;
    stop_record(PS3_STOP_GCM_UNMODELLED, func, feature, "");
    abort();
}
PS3_NORETURN void ps3_stop_hle_uninitialized(const char* func, const char* module, const char* reason)
{
    (void)reason;
    stop_record(PS3_STOP_HLE_UNINITIALIZED, func, module, "");
    abort();
}

#define MB 0x100000u
#define OUT ((u32*)(uintptr_t)0x100)
#define NOTHING 0xDEADBEEFu

/* Failures are reported and the run goes on (the first 12 are printed; the exit status says so at the end). */
static int g_fails;
#define EXPECT(cond) do { if (!(cond)) { if (g_fails++ < 12) fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define EXPECT_EQ(got, want) do { unsigned long long g_ = (got), w_ = (want); if (g_ != w_) { \
    if (g_fails++ < 12) fprintf(stderr, "FAIL line %d: %s = 0x%llX, want 0x%llX\n", __LINE__, #got, g_, w_); } } while (0)

/* The call must end in the named stop `kind` (and `feature`, when given) and not return. */
#define EXPECT_STOP(call, kind_, feature_) do { \
    g_stopped = 0; g_stop_feature[0] = 0; \
    if (!setjmp(g_jb)) { g_armed = 1; (void)(call); } \
    g_armed = 0; \
    EXPECT(g_stopped); \
    EXPECT_EQ((unsigned long long)g_stop_kind, (unsigned long long)(kind_)); \
    if ((feature_)[0]) EXPECT(!strcmp(g_stop_feature, (feature_))); \
} while (0)

static void power_on(void)
{
    memset(&s_config, 0, sizeof(s_config));
    memset(s_io_mappings, 0, sizeof(s_io_mappings)); s_io_mapping_count = 0;
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    s_gcm_initialized = 0;
}
static void init_gcm(void)
{
    power_on();
    EXPECT_EQ((u32)cellGcmInit(0x100000, 8 * MB, 0x40300000), (u32)CELL_OK);
}
static int mappings_in_use(void)
{
    int n = 0;
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) n += s_io_mappings[i].active;
    return n;
}

int main(void)
{
    vm_base = calloc(1, 0x10000); assert(vm_base);
    ppu_hle_inject_base = 0x1000;

    /* --- cellGcmBindTile: 15 and up is INVALID_VALUE (0x02002AD4), 0..14 bind --- */
    init_gcm();
    EXPECT_EQ((u32)cellGcmBindTile(0), (u32)CELL_OK);
    EXPECT_EQ((u32)cellGcmBindTile(14), (u32)CELL_OK);
    EXPECT_EQ((u32)cellGcmBindTile(15), 0x80210003u);
    EXPECT_EQ((u32)cellGcmBindTile(255), 0x80210003u);

    /* --- cellGcmGetConfiguration: stores the six words; a NULL config is a null-page store, not INVALID_VALUE --- */
    init_gcm();
    vm_write32(0x100, NOTHING);
    EXPECT_EQ((u32)cellGcmGetConfiguration((CellGcmConfig*)(uintptr_t)0x100), (u32)CELL_OK);
    EXPECT_EQ(vm_read32(0x100), s_config.localAddress);
    EXPECT_EQ(vm_read32(0x108), s_config.localSize);
    EXPECT_STOP(cellGcmGetConfiguration(NULL), PS3_STOP_GUEST_NULL_ACCESS, "");
    EXPECT(!strcmp(g_stop_func, "cellGcmGetConfiguration"));
    EXPECT(!strcmp(g_stop_ea, "0x00000000"));

    /* --- cellGcmMapMainMemory --- */
    /* a misaligned EA or size is FAILURE (0x020048D8), never INVALID_ALIGNMENT, and nothing is stored or mapped */
    init_gcm();
    const int base_maps = mappings_in_use();
    vm_write32(0x100, NOTHING);
    EXPECT_EQ((u32)cellGcmMapMainMemory(0x40D00000u + 0x1000, MB, OUT), 0x802100FFu);        /* ea */
    EXPECT_EQ((u32)cellGcmMapMainMemory(0x40D00000u, MB + 0x1000, OUT), 0x802100FFu);        /* size */
    EXPECT_EQ((u32)cellGcmMapMainMemory(0x40D00000u + 1, 0x1000, OUT), 0x802100FFu);         /* both */
    EXPECT_EQ(vm_read32(0x100), NOTHING);
    EXPECT_EQ(mappings_in_use(), base_maps);
    /* the alignment tests come before the out-pointer is looked at: a NULL one with them is still FAILURE */
    EXPECT_EQ((u32)cellGcmMapMainMemory(0x40D00001u, MB, NULL), 0x802100FFu);
    EXPECT_EQ((u32)cellGcmMapMainMemory(0x40D00000u, 0x1000, NULL), 0x802100FFu);
    /* size 0 is not tested by the firmware (lv2 answers it): a named stop, nothing mapped */
    EXPECT_STOP(cellGcmMapMainMemory(0x40D00000u, 0, OUT), PS3_STOP_GCM_UNMODELLED, "zero-size");
    EXPECT_EQ(mappings_in_use(), base_maps);
    /* a NULL out-pointer is the store the firmware makes last: a null-page stop, no mapping made */
    EXPECT_STOP(cellGcmMapMainMemory(0x40D00000u, MB, NULL), PS3_STOP_GUEST_NULL_ACCESS, "");
    EXPECT(!strcmp(g_stop_func, "cellGcmMapMainMemory"));
    EXPECT_EQ(mappings_in_use(), base_maps);
    EXPECT_EQ(s_io_address_table[0x40D], 0xFFFFu);
    /* a good map still works after all that */
    EXPECT_EQ((u32)cellGcmMapMainMemory(0x40D00000u, MB, OUT), (u32)CELL_OK);
    EXPECT_EQ(vm_read32(0x100), 0x800000u);
    /* before cellGcmInit the firmware searches IO tables that are zero: a named stop, nothing mapped */
    power_on();
    vm_write32(0x100, NOTHING);
    EXPECT_STOP(cellGcmMapMainMemory(0x40D00000u, MB, OUT), PS3_STOP_HLE_UNINITIALIZED, "cellGcmSys");
    EXPECT_EQ(vm_read32(0x100), NOTHING);
    EXPECT_EQ(mappings_in_use(), 0);
    /* ... but the alignment tests are first, so a misaligned EA before Init is still FAILURE */
    EXPECT_EQ((u32)cellGcmMapMainMemory(0x40D00001u, MB, OUT), 0x802100FFu);
    /* no free run of IO pages is NO_IO_PAGE_TABLE (0x0200499C); the out-parameter stays */
    init_gcm();
    for (u32 n = 0; n < 248; n++) EXPECT_EQ((u32)cellGcmMapMainMemory(0x60000000u + n * MB, MB, OUT), (u32)CELL_OK);
    vm_write32(0x100, NOTHING);
    EXPECT_EQ((u32)cellGcmMapMainMemory(0x70000000u, MB, OUT), 0x80210001u);
    EXPECT_EQ(vm_read32(0x100), NOTHING);
    /* the runtime's own full mapping list is not a firmware condition: a named stop, not a code */
    init_gcm();
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) s_io_mappings[i].active = 1;
    EXPECT_STOP(cellGcmMapMainMemory(0x40D00000u, MB, OUT), PS3_STOP_GCM_UNMODELLED, "io-mapping-slots");

    /* --- cellGcmAddressToOffset --- */
    init_gcm();
    /* an EA with no IO page (page 0 here) is FAILURE, the offset is not written */
    vm_write32(0x100, NOTHING);
    EXPECT_EQ((u32)cellGcmAddressToOffset(0x80000, OUT), 0x802100FFu);
    EXPECT_EQ(vm_read32(0x100), NOTHING);
    EXPECT_EQ((u32)cellGcmAddressToOffset(0, OUT), 0x802100FFu);
    EXPECT_EQ(vm_read32(0x100), NOTHING);
    /* a NULL offset is a store only when there is something to store: with an unconvertible EA the answer is
     * FAILURE, with a mapped one the null-page stop */
    EXPECT_EQ((u32)cellGcmAddressToOffset(0x80000, NULL), 0x802100FFu);
    EXPECT_EQ((u32)cellGcmAddressToOffset(0x40300000u + 0x123, OUT), (u32)CELL_OK);   /* the Init window: io 0x123 */
    EXPECT_EQ(vm_read32(0x100), 0x123u);
    EXPECT_STOP(cellGcmAddressToOffset(0x40300000u + 0x123, NULL), PS3_STOP_GUEST_NULL_ACCESS, "");
    EXPECT(!strcmp(g_stop_func, "cellGcmAddressToOffset"));
    EXPECT_STOP(cellGcmAddressToOffset(s_config.localAddress + 0x40, NULL), PS3_STOP_GUEST_NULL_ACCESS, "");
    /* local memory converts, and writes nothing else */
    vm_write32(0x100, NOTHING);
    EXPECT_EQ((u32)cellGcmAddressToOffset(s_config.localAddress + 0x40, OUT), (u32)CELL_OK);
    EXPECT_EQ(vm_read32(0x100), 0x40u);
    /* the auto-map has no free IO page left: FAILURE and the offset is not written */
    init_gcm();
    for (u32 n = 0; n < 248; n++) EXPECT_EQ((u32)cellGcmMapMainMemory(0x60000000u + n * MB, MB, OUT), (u32)CELL_OK);
    vm_write32(0x100, NOTHING);
    EXPECT_EQ((u32)cellGcmAddressToOffset(0x10000000, OUT), 0x802100FFu);
    EXPECT_EQ(vm_read32(0x100), NOTHING);
    /* ... and before Init nothing is auto-mapped either (FAILURE, untouched) */
    power_on();
    vm_write32(0x100, NOTHING);
    EXPECT_EQ((u32)cellGcmAddressToOffset(0x10000000, OUT), 0x802100FFu);
    EXPECT_EQ(vm_read32(0x100), NOTHING);
    EXPECT_EQ(mappings_in_use(), 0);

    free(vm_base);
    if (g_fails) { fprintf(stderr, "%d check(s) failed\n", g_fails); return 1; }
    puts("PASS: GCM entry checks (BindTile, GetConfiguration, MapMainMemory, AddressToOffset) as libgcm_sys makes them");
    return 0;
}
