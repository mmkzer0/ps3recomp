/*
 * cellAdec AC3, driven through its guest API.
 *
 * Built twice by CMakeLists.txt (adec_tests.cmake):
 *   without PS3RECOMP_FFMPEG_RUNTIME   mode "stub"
 *   with it                            modes "stub" (PS3RECOMP_FFMPEG=0) and "decode"
 * Modes:
 *   stub   AC3 AC3_2CH   AC3 5.1 frames give 1536 x 6 float of silence (36864 bytes in the PcmItem size, in
 *                        bsiInfo+0x14 and through cellAdecGetPcm), 2.0 frames 1536 x 2 (12288), codecType 5
 *                        the old 1024 x 2 (8192); AUDONE then PCMOUT for every AU.
 *   decode AC3 REF       every AU of a 5.1 AC3 stream through cellAdecDecodeAu; the PCM handed to the guest
 *                        (big-endian float, interleaved) must equal FFmpeg's own decode REF (f32le) sample
 *                        for sample. Negative controls: two channels swapped and one AU dropped must both
 *                        be caught.
 * The AC3 files are CBR 448 kb/s at 48 kHz, so every frame (= AU) is 1792 bytes (gen_adec_media.cmake).
 */
#include "../cellAdec.c"
#ifdef PS3RECOMP_FFMPEG_RUNTIME
#include "../ffmpeg_runtime.c"
#endif

#include <math.h>
#include <stdlib.h>
#include "../../../tests/guest_range_catch.h"

/* What the runtime would provide (the same stand-ins test_vdec_decode.c uses). */
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base = 0x01000000u;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define MEM_SIZE   (64u << 20)
#define G_TYPE     0x10000u
#define G_CB       0x10010u
#define G_HANDLE   0x10020u
#define G_AUINFO   0x10040u
#define G_ITEMPTR  0x100A0u
#define G_AU       0x00100000u
#define G_OUT      0x00200000u
#define CB_OPD     0x00001000u
#define AC3_FRAME  1792u

static u32 s_msgs[8192];
static int s_nmsg;
static u32 s_handle;
static void fake_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)a2; (void)a4; (void)a5; (void)a6; (void)a7;
    CHECK(opd == CB_OPD && a3 == 0xC0FFEEu && a0 == s_handle);
    /* AUDONE hands the AU back: a title's demuxer may refill it at once. */
    if ((u32)a1 == CELL_ADEC_MSG_TYPE_AUDONE) memset(vm_base + G_AU, 0xA5, 0x10000);
    if (s_nmsg < 8192) s_msgs[s_nmsg++] = (u32)a1;
}

/* The configuration cellAdec.c reads (adec.silence_fallback, off by default: a zeroed ps3_config) and a ps3_stop that
 * records the stop and returns to the test through longjmp, so one process refuses several AUs. A stop outside a
 * STOP_TRY is a failure. */
static ps3_config s_cfg;
const ps3_config* ps3_config_get(void) { return &s_cfg; }

static jmp_buf s_stop_jmp;
static int s_stop_armed, s_stop_hit;
static int s_stop_kind;
static char s_stop_func[48], s_stop_codec[16], s_stop_reason[32];
void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy, const ps3_stop_field* f, size_t n,
              const ps3_stop_site* site)
{
    (void)what; (void)remedy; (void)site;
    if (!s_stop_armed) { fprintf(stderr, "unexpected stop, kind %d\n", (int)kind); abort(); }
    s_stop_kind = (int)kind;
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(f[i].key, "func"))   snprintf(s_stop_func, sizeof s_stop_func, "%s", f[i].value);
        if (!strcmp(f[i].key, "codec"))  snprintf(s_stop_codec, sizeof s_stop_codec, "%s", f[i].value);
        if (!strcmp(f[i].key, "reason")) snprintf(s_stop_reason, sizeof s_stop_reason, "%s", f[i].value);
    }
    s_stop_armed = 0;
    longjmp(s_stop_jmp, 1);
}
#define STOP_TRY(call) do { \
        s_stop_hit = 0; s_stop_armed = 1; \
        if (setjmp(s_stop_jmp) == 0) { (void)(call); s_stop_armed = 0; } else s_stop_hit = 1; \
    } while (0)
/* The call ended in adec-no-decoder from cellAdecDecodeAu, for this codec type and reason. */
static int stop_is(u32 codec, const char* reason)
{
    char c[16];
    snprintf(c, sizeof c, "%u", codec);
    return s_stop_hit && s_stop_kind == (int)PS3_STOP_ADEC_NO_DECODER && !strcmp(s_stop_func, "cellAdecDecodeAu") &&
           !strcmp(s_stop_codec, c) && !strcmp(s_stop_reason, reason);
}
ps3_guest_caller_fn g_ps3_guest_caller = fake_guest_call;

static u8* read_file(const char* path, size_t* len)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* p = (u8*)malloc((size_t)n + 1);
    CHECK(p && fread(p, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *len = (size_t)n;
    return p;
}

static void open_adec(u32 codec)
{
    vm_write32(G_TYPE, codec);
    vm_write32(G_CB, CB_OPD);
    vm_write32(G_CB + 4, 0xC0FFEEu);
    CHECK(cellAdecOpen((const CellAdecType*)(uintptr_t)G_TYPE, NULL,
                       (const CellAdecCb*)(uintptr_t)G_CB, (CellAdecHandle*)(uintptr_t)G_HANDLE) == CELL_OK);
    s_handle = vm_read32(G_HANDLE);
    CHECK(cellAdecStartSeq(s_handle, NULL) == CELL_OK);
}

/* One AU through the guest API: returns the PCM size the item announced and leaves the PCM at G_OUT. */
static u32 decode_one(const u8* au, u32 size)
{
    memcpy(vm_base + G_AU, au, size);
    vm_write32(G_AUINFO + 0, G_AU);
    vm_write32(G_AUINFO + 4, size);
    int before = s_nmsg;
    CHECK(cellAdecDecodeAu(s_handle, (const CellAdecAuInfo*)(uintptr_t)G_AUINFO) == CELL_OK);
    CHECK(s_nmsg == before + 2 && s_msgs[before] == CELL_ADEC_MSG_TYPE_AUDONE &&
          s_msgs[before + 1] == CELL_ADEC_MSG_TYPE_PCMOUT);
    CHECK(cellAdecGetPcmItem(s_handle, (const CellAdecPcmItem**)(uintptr_t)G_ITEMPTR) == CELL_OK);
    u32 item = vm_read32(G_ITEMPTR);
    u32 bytes = vm_read32(item + PCMITEM_SIZE);
    u32 bsi = vm_read32(item + PCMITEM_BSI_INFO);
    CHECK(bsi && vm_read32(bsi + 0x14) == bytes);
    CHECK(vm_read32(item + PCMITEM_AU_START) == G_AU && vm_read32(item + PCMITEM_AU_SIZE) == size);
    memset(vm_base + G_OUT, 0x5A, bytes + 64);
    CHECK(cellAdecGetPcm(s_handle, (void*)(uintptr_t)G_OUT) == CELL_OK);
    CHECK(vm_base[G_OUT + bytes] == 0x5A);           /* exactly `bytes` written */
    return bytes;
}

static int all_zero(u32 ea, u32 n)
{
    for (u32 i = 0; i < n; i++) if (vm_base[ea + i]) return 0;
    return 1;
}

static int mode_stub(int argc, char** argv)
{
    CHECK(argc >= 4);
    s_cfg.adec_silence_fallback = true;   /* this mode is about the silent block the allow-list keeps */
    size_t n6, n2;
    u8* a6 = read_file(argv[2], &n6);
    u8* a2 = read_file(argv[3], &n2);
    CHECK(n6 >= AC3_FRAME && n2 >= AC3_FRAME && a6[0] == 0x0B && a6[1] == 0x77);
    CHECK(adec_ac3_channels(a6, AC3_FRAME) == 6 && adec_ac3_channels(a2, AC3_FRAME) == 2);
    open_adec(CELL_ADEC_TYPE_AC3);
    CHECK(s_adec[s_handle].ff == NULL);
    for (int i = 0; i < 3; i++) {
        u32 b = decode_one(a6 + (size_t)i * AC3_FRAME, AC3_FRAME);
        CHECK(b == 1536u * 6u * 4u && all_zero(G_OUT, b));
    }
    u32 b = decode_one(a2, AC3_FRAME);
    CHECK(b == 1536u * 2u * 4u && all_zero(G_OUT, b));
    CHECK(cellAdecClose(s_handle) == CELL_OK);
    open_adec(5);
    b = decode_one(a6, AC3_FRAME);
    CHECK(b == 1024u * 2u * 4u && all_zero(G_OUT, b));
    CHECK(cellAdecClose(s_handle) == CELL_OK);
    printf("OK stub: AC3 5.1 36864, 2.0 12288, codecType 5 8192 bytes of silence per AU\n");
    return 0;
}

#ifdef PS3RECOMP_FFMPEG_RUNTIME
/* Decode the whole stream; compare against REF. swap: exchange channels 0 and 1 of the guest PCM; drop: skip AU 3. */
static int run_decode(const u8* ac3, size_t n, const float* ref, size_t nref, int swap, int drop)
{
    open_adec(CELL_ADEC_TYPE_AC3);
    CHECK(s_adec[s_handle].ff != NULL);
    size_t at = 0;
    int bad = 0;
    for (size_t off = 0, k = 0; off + AC3_FRAME <= n; off += AC3_FRAME, k++) {
        if (drop && k == 3) continue;
        u32 b = decode_one(ac3 + off, AC3_FRAME);
        if (b != 1536u * 6u * 4u) { printf("AU %zu: %u bytes\n", k, b); bad = 1; break; }
        for (u32 i = 0; i < b / 4u; i++) {
            u32 idx = i;
            if (swap && (i % 6u) < 2u) idx = i ^ 1u;
            u32 w = vm_read32(G_OUT + idx * 4u);
            float v;
            memcpy(&v, &w, 4);
            if (at + i >= nref || v != ref[at + i]) {
                if (!bad) printf("sample %zu (AU %zu): %g vs %g\n", at + i, k, v, at + i < nref ? ref[at + i] : -1.f);
                bad = 1;
                break;
            }
        }
        if (bad) break;
        at += b / 4u;
    }
    if (!bad && at != nref) { printf("%zu samples, reference has %zu\n", at, nref); bad = 1; }
    CHECK(cellAdecClose(s_handle) == CELL_OK);
    return bad;
}

static int mode_decode(int argc, char** argv)
{
    CHECK(argc >= 4);
    if (!ps3_ffmpeg_get()) { printf("SKIP: libavcodec not loadable\n"); return 77; }
    size_t n, nr;
    u8* ac3 = read_file(argv[2], &n);
    u8* refb = read_file(argv[3], &nr);
    CHECK(n % AC3_FRAME == 0 && n / AC3_FRAME >= 8 && nr % 4 == 0);
    const float* ref = (const float*)refb;
    CHECK(run_decode(ac3, n, ref, nr / 4, 0, 0) == 0);
    /* Nonzero audio came through, not silence. */
    double e = 0;
    for (size_t i = 0; i < nr / 4; i++) e += fabs(ref[i]);
    CHECK(e > 1.0);
    printf("OK decode: %zu AUs, %zu samples equal FFmpeg's own decode\n", n / AC3_FRAME, nr / 4);
    CHECK(run_decode(ac3, n, ref, nr / 4, 1, 0) != 0);
    CHECK(run_decode(ac3, n, ref, nr / 4, 0, 1) != 0);
    printf("OK negative controls: swapped channels and a dropped AU are caught\n");
    return 0;
}
#endif

/* range: the guest spans cellAdecDecodeAu reads (the AU info block, the AC3 bytes at startAddr) are bounded by
 * guest memory; a span that leaves it is the named stop hle-guest-range before AUDONE, before the copy. No
 * media file is needed: the bytes are not decoded. */
static int mode_range(void)
{
    open_adec(CELL_ADEC_TYPE_AC3);
    s_cfg.adec_silence_fallback = true;   /* the exact-fit AU below is not decoded; its silence is the allow-list's */
    const int before = s_nmsg;

    GR_TRY(cellAdecDecodeAu(s_handle, (const CellAdecAuInfo*)(uintptr_t)(MEM_SIZE - 8)));
    CHECK(gr_named("cellAdecDecodeAu", "auInfo"));
    GR_TRY(cellAdecDecodeAu(s_handle, (const CellAdecAuInfo*)(uintptr_t)0xFFFFFFF0u));
    CHECK(gr_named("cellAdecDecodeAu", "auInfo"));

    vm_write32(G_AUINFO + 0, MEM_SIZE - 16);          /* startAddr: the AU runs 48 bytes past the arena */
    vm_write32(G_AUINFO + 4, 64);
    GR_TRY(cellAdecDecodeAu(s_handle, (const CellAdecAuInfo*)(uintptr_t)G_AUINFO));
    CHECK(gr_named("cellAdecDecodeAu", "au"));
    vm_write32(G_AUINFO + 0, 0xFFFFFF00u);            /* far outside, a size that fits u32 only by wrapping */
    vm_write32(G_AUINFO + 4, 0x400);
    GR_TRY(cellAdecDecodeAu(s_handle, (const CellAdecAuInfo*)(uintptr_t)G_AUINFO));
    CHECK(gr_named("cellAdecDecodeAu", "au"));
    CHECK(s_nmsg == before);                          /* no AUDONE, no PCMOUT for a refused AU */

    /* A span that fits exactly is accepted: AUDONE, then PCMOUT. */
    memset(vm_base + MEM_SIZE - 64, 0, 64);
    vm_write32(G_AUINFO + 0, MEM_SIZE - 64);
    vm_write32(G_AUINFO + 4, 64);
    CHECK(cellAdecDecodeAu(s_handle, (const CellAdecAuInfo*)(uintptr_t)G_AUINFO) == CELL_OK);
    CHECK(s_nmsg == before + 2);
    printf("PASS cellAdecDecodeAu bounds its guest spans\n");
    return 0;
}

/* stop: no decoder is open (every codec but AC3 here; AC3 when FFmpeg is absent or off), adec.silence_fallback off:
 * cellAdecDecodeAu is the stop adec-no-decoder, reason no-decoder, before AUDONE and before anything is written for
 * the guest: no callback, no PCM item. The same AU with the allow-list on is the silence the old code answered. */
static void submit(u32 size)
{
    memset(vm_base + G_AU, 0, size);
    vm_write32(G_AUINFO + 0, G_AU);
    vm_write32(G_AUINFO + 4, size);
}

static int mode_stop(void)
{
    s_cfg.adec_silence_fallback = false;
    const CellAdecAuInfo* au = (const CellAdecAuInfo*)(uintptr_t)G_AUINFO;
    const CellAdecPcmItem** pi = (const CellAdecPcmItem**)(uintptr_t)G_ITEMPTR;
    for (u32 codec = 0; codec < 2; codec++) {
        const u32 type = codec ? CELL_ADEC_TYPE_AC3 : 5u;
        open_adec(type);
        CHECK(s_adec[s_handle].ff == NULL);
        submit(64);
        const int before = s_nmsg;
        STOP_TRY(cellAdecDecodeAu(s_handle, au));
        CHECK(stop_is(type, "no-decoder"));
        CHECK(s_nmsg == before);                                   /* neither AUDONE nor PCMOUT */
        CHECK(cellAdecGetPcmItem(s_handle, pi) == (s32)CELL_ADEC_ERROR_EMPTY);   /* no PCM item was produced */
        CHECK(s_adec[s_handle].auCount == 0);
        /* The allow-list is what turns the stop back into the silent block, AUDONE then PCMOUT. */
        s_cfg.adec_silence_fallback = true;
        STOP_TRY(cellAdecDecodeAu(s_handle, au));
        CHECK(!s_stop_hit && s_nmsg == before + 2 && s_adec[s_handle].auCount == 1);
        CHECK(cellAdecGetPcmItem(s_handle, pi) == CELL_OK);
        s_cfg.adec_silence_fallback = false;
        CHECK(cellAdecClose(s_handle) == CELL_OK);
    }
    printf("OK stop: no decoder is adec-no-decoder (no-decoder) before AUDONE; adec.silence_fallback allows the silence\n");
    return 0;
}

/* stop-decoder: a decoder is open and the AU yields no PCM (reason not-decoded). An AU the decoder is never given (no
 * bytes) stops before AUDONE; one it is given and fails on (garbage) stops after AUDONE, before PCMOUT and before the
 * PCM item is written. Without FFmpeg a stand-in pointer marks the decoder open and the stub decode returns nothing. */
static int mode_stop_decoder(void)
{
    s_cfg.adec_silence_fallback = false;
    const CellAdecAuInfo* au = (const CellAdecAuInfo*)(uintptr_t)G_AUINFO;
    const CellAdecPcmItem** pi = (const CellAdecPcmItem**)(uintptr_t)G_ITEMPTR;
#ifdef PS3RECOMP_FFMPEG_RUNTIME
    if (!ps3_ffmpeg_get()) { printf("SKIP: libavcodec not loadable\n"); return 77; }
#endif
    open_adec(CELL_ADEC_TYPE_AC3);
#ifndef PS3RECOMP_FFMPEG_RUNTIME
    s_adec[s_handle].ff = &s_cfg;
#endif
    CHECK(s_adec[s_handle].ff != NULL);
    int before = s_nmsg;
    submit(0);                                                     /* nothing to hand the decoder */
    STOP_TRY(cellAdecDecodeAu(s_handle, au));
    CHECK(stop_is(CELL_ADEC_TYPE_AC3, "not-decoded") && s_nmsg == before);
    submit(64);                                                    /* handed over, nothing comes out */
    STOP_TRY(cellAdecDecodeAu(s_handle, au));
    CHECK(stop_is(CELL_ADEC_TYPE_AC3, "not-decoded"));
    CHECK(s_nmsg == before + 1 && s_msgs[before] == CELL_ADEC_MSG_TYPE_AUDONE);   /* AUDONE only, no PCMOUT */
    CHECK(cellAdecGetPcmItem(s_handle, pi) == (s32)CELL_ADEC_ERROR_EMPTY);
    s_cfg.adec_silence_fallback = true;                            /* allow-listed: the silent frame, as before */
    before = s_nmsg;
    STOP_TRY(cellAdecDecodeAu(s_handle, au));
    CHECK(!s_stop_hit && s_nmsg == before + 2 && cellAdecGetPcmItem(s_handle, pi) == CELL_OK);
#ifndef PS3RECOMP_FFMPEG_RUNTIME
    s_adec[s_handle].ff = NULL;
#endif
    CHECK(cellAdecClose(s_handle) == CELL_OK);
    printf("OK stop-decoder: an AU with no PCM is adec-no-decoder (not-decoded); the allow-list restores the silence\n");
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = (uint8_t*)calloc(1, MEM_SIZE);
    CHECK(vm_base);
    ppu_vm_size = MEM_SIZE;
    const char* mode = argc > 1 ? argv[1] : "stub";
    if (strcmp(mode, "stop") == 0) return mode_stop();
    if (strcmp(mode, "stop-decoder") == 0) return mode_stop_decoder();
    if (strcmp(mode, "stub") == 0) return mode_stub(argc, argv);
    if (strcmp(mode, "range") == 0) return mode_range();
#ifdef PS3RECOMP_FFMPEG_RUNTIME
    if (strcmp(mode, "decode") == 0) return mode_decode(argc, argv);
#endif
    printf("unknown mode %s\n", mode);
    return 2;
}
