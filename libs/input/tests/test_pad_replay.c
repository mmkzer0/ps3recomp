/* cellPad with pad.record / pad.replay: the real cellPadGetData and info calls, the real guest
 * serialisation, a synthetic trace and fake SDL devices. No title and no physical input.
 *
 *   test_pad_replay <control|replay|replay-info|record> <trace file>
 *
 * Each case is its own process: the injectors (PAD_SCRIPT, PAD_STICK, PAD_AUTOPRESS) read the environment
 * once, and the trace is opened once per process.
 *
 *   control      no trace: the injected press reaches the guest. This is what `replay` must suppress.
 *   replay       the same injectors, a fake host pad and a trace that says something else: the guest sees
 *                the trace, a neutral pad before its first record and after its end, and no drain loop
 *                runs forever.
 *   replay-info  the info calls report the recorded connection and flag its changes.
 *   record       what the guest was handed is what the file holds, injectors included, and reads back
 *                to the same words.
 */
#include <time.h>
#include <stdint.h>
static uint64_t fake_ms = 1000;
static int fixture_clock_gettime(clockid_t id, struct timespec* ts)
{ (void)id; ts->tv_sec = fake_ms / 1000; ts->tv_nsec = (fake_ms % 1000) * 1000000; return 0; }
#define clock_gettime fixture_clock_gettime
#include "../cellPad.c"
#undef clock_gettime
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t a) { (void)a; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static uint32_t g_flip;
u32 cellGcm_flip_request_count(void) { return g_flip; }   /* PAD_CLOCK=flips */
u32 cellGcm_guest_flip_count(void) { return g_flip; }     /* the pad trace's clock */

static ps3_config g_cfg;
const ps3_config* ps3_config_get(void) { return &g_cfg; }

static int device, attached;
static u16 device_buttons;
void SDL_GameControllerUpdate(void) {}
SDL_bool SDL_IsGameController(int n) { (void)n; return SDL_FALSE; }
SDL_bool SDL_GameControllerGetAttached(SDL_GameController* gc) { return gc == (SDL_GameController*)&device && attached; }
void SDL_GameControllerClose(SDL_GameController* gc) { (void)gc; }
int SDL_GameControllerRumble(SDL_GameController* gc, Uint16 lo, Uint16 hi, Uint32 ms)
{ (void)gc; (void)lo; (void)hi; (void)ms; return 0; }
Uint8 SDL_GameControllerGetButton(SDL_GameController* gc, SDL_GameControllerButton b)
{
    (void)gc;
    if (b == SDL_CONTROLLER_BUTTON_B) return !!(device_buttons & CELL_PAD_CTRL_CIRCLE);
    if (b == SDL_CONTROLLER_BUTTON_A) return !!(device_buttons & CELL_PAD_CTRL_CROSS);
    return 0;
}
Sint16 SDL_GameControllerGetAxis(SDL_GameController* gc, SDL_GameControllerAxis a)
{ (void)gc; return a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? 0 : 1; }

static int g_fail;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("[FAIL] %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            g_fail = 1;                                                        \
        }                                                                      \
    } while (0)

static CellPadData* guest = (CellPadData*)(uintptr_t)0x100;

static void publish(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    if (!f || fputs(text, f) < 0 || fclose(f)) { perror(path); exit(2); }
}

static s32 read_pad(unsigned port)
{
    CHECK(cellPadGetData(port, guest) == CELL_OK, "cellPadGetData(%u)", port);
    return (s32)vm_read32(0x100);
}
static u16 word(unsigned n) { return vm_read16(0x104 + n * 2); }
static u16 buttons(void) { return (u16)(word(CELL_PAD_BTN_OFFSET_DIGITAL1) | (word(CELL_PAD_BTN_OFFSET_DIGITAL2) << 8)); }

static void start(void)
{
    vm_base = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);
    ppu_vm_size = 0x10000;
    s_pad_initialized = 1;
    s_max_connect = 7;
    pad_trace_attach();
}

static u32 info_status(unsigned port)
{
    CHECK(cellPadGetInfo2((CellPadInfo2*)(uintptr_t)0x400) == CELL_OK, "cellPadGetInfo2");
    return vm_read32(0x400 + 12 + port * 4);
}

static void case_control(void)
{
    start();
    CHECK(read_pad(0) > 0, "a fresh packet");
    CHECK(buttons() == CELL_PAD_CTRL_CROSS, "without a trace the scripted press reaches the guest: 0x%04X", buttons());
}

static void case_replay(const char* path)
{
    /* Four recorded polls of port 0: a press with a packet, the same press on a poll the host had no
     * report for (len 0), then the release and its drained poll. */
    publish(path,
            "{\"format\":\"ps3recomp-pad-trace\",\"version\":3,\"clock\":\"guest-polls\",\"ports\":7}\n"
            "{\"seq\":0,\"port\":0,\"len\":8,\"words\":[0,116,0,32,128,128,128,128],\"flip\":2}\n"
            "{\"seq\":1,\"port\":0,\"len\":0,\"words\":[0,116,0,32,128,128,128,128],\"flip\":2}\n"
            "{\"seq\":2,\"port\":0,\"len\":8,\"words\":[0,116,0,0,128,128,128,128],\"flip\":4}\n"
            "{\"seq\":3,\"port\":0,\"len\":0,\"words\":[0,116,0,0,128,128,128,128],\"flip\":4}\n"
            "{\"end\":true,\"seq\":4}\n");
    snprintf(g_cfg.pad_replay, sizeof g_cfg.pad_replay, "%s", path);
    start();
    s_sdl_controllers[0] = (SDL_GameController*)&device;
    attached = 1;
    device_buttons = CELL_PAD_CTRL_CROSS;

    g_flip = 2;
    CHECK(read_pad(0) == 8, "poll 0: the recorded press, len 8");
    CHECK(buttons() == CELL_PAD_CTRL_CIRCLE, "the recorded CIRCLE and nothing else: 0x%04X", buttons());
    CHECK(read_pad(0) == 0, "poll 1: the recorded drained poll ends the loop, len 0");
    CHECK(buttons() == CELL_PAD_CTRL_CIRCLE, "with the same words");
    g_flip = 4;
    CHECK(read_pad(0) == 8 && buttons() == 0, "poll 2: the recorded release, len 8");
    CHECK(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) == 128, "recorded sticks");
    CHECK(read_pad(0) == 0, "poll 3: released and drained");
    g_flip = 5;
    CHECK(read_pad(0) > 0 && buttons() == 0, "poll 4 is past the end of the trace: a neutral pad");
    CHECK(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) == 128, "neutral sticks");
    CHECK(read_pad(1) == 0 && buttons() == 0, "a port the trace never mentions has no device");
    cellPad_poll();   /* the host side keeps running; it must not reach the guest */
    CHECK(read_pad(0) > 0 && buttons() == 0, "host polling changes nothing: still a neutral pad");
    {
        PadTraceStats s;
        pad_trace_stats(pad_trace_get(), &s);
        CHECK(s.records == 4 && s.records_reached == 4 && s.exact == 4 && s.held == 3,
              "four records, four reached at their own seq (polls %llu exact %llu held %llu)",
              (unsigned long long)s.polls, (unsigned long long)s.exact, (unsigned long long)s.held);
        CHECK(s.have_drift && s.drift_min == 0 && s.drift_max == 0,
              "the replay's flips are the recorded ones (%lld:%lld)", (long long)s.drift_min, (long long)s.drift_max);
        CHECK(s.ended, "the trace is reported as ended");
    }
}

static void case_replay_info(const char* path)
{
    /* The connection is keyed on the info-call ordinal: the trace was recorded with a change on its third
     * cellPadGetInfo2/GetInfo/PeriphGetInfo call. */
    publish(path,
            "{\"format\":\"ps3recomp-pad-trace\",\"version\":3,\"clock\":\"guest-polls\",\"ports\":7}\n"
            "{\"info\":0,\"conn\":[1,1,0,0,0,0,0]}\n"
            "{\"info\":2,\"conn\":[1,0,0,0,0,0,0]}\n"
            "{\"end\":true,\"seq\":0}\n");
    snprintf(g_cfg.pad_replay, sizeof g_cfg.pad_replay, "%s", path);
    start();

    CHECK(info_status(1) == CELL_PAD_STATUS_CONNECTED, "info call 0: port 1 is connected by the trace, no host device needed");
    CHECK(vm_read32(0x400 + 4) == 2, "now_connect counts the recorded ports");
    CHECK(info_status(1) == CELL_PAD_STATUS_CONNECTED, "info call 1: the baseline is not a change");
    CHECK(info_status(1) == CELL_PAD_STATUS_DISCONNECTED + CELL_PAD_STATUS_ASSIGN_CHANGES, "info call 2: the unplug is flagged once");
    CHECK(info_status(1) == CELL_PAD_STATUS_DISCONNECTED, "and only once");
    CHECK(vm_read32(0x400 + 4) == 1, "now_connect follows");

    CHECK(cellPadPeriphGetInfo((CellPadPeriphInfo*)(uintptr_t)0x800) == CELL_OK, "cellPadPeriphGetInfo");
    CHECK(vm_read32(0x800 + 12) == CELL_PAD_STATUS_CONNECTED && vm_read32(0x800 + 16) == CELL_PAD_STATUS_DISCONNECTED,
          "the peripheral view tells the same story");
    CHECK(cellPadGetInfo((CellPadInfo2*)(uintptr_t)0xC00) == CELL_OK, "cellPadGetInfo");
    CHECK(vm_read8(0xC00 + 0x208) == CELL_PAD_STATUS_CONNECTED && vm_read8(0xC00 + 0x209) == CELL_PAD_STATUS_DISCONNECTED,
          "and so does the v1 call");
}

static int count_sub(const char* hay, const char* needle)
{
    int n = 0;
    for (const char* p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

typedef struct { uint32_t flip; u16 words[CELL_PAD_MAX_CODES]; s32 len_seen; } Seen;

/* The recording session runs the guest's own drain loop, so the number of polls per flip is what the
 * live path handed out. 24 flips is enough for the scripted CROSS (held 40 polls, two polls a flip) to
 * be released at flip 20 and recorded. */
#define REC_FLIPS      24u
#define REC_POLL_MAX    6u

/* cellPadGetData until len == 0, capped: the guest's drain loop. */
static unsigned read_drain(unsigned port, Seen* out, unsigned cap)
{
    unsigned n = 0;
    while (n < cap) {
        Seen* s = &out[n];
        s->flip = g_flip;
        s->len_seen = read_pad(port);
        for (unsigned w = 0; w < CELL_PAD_MAX_CODES; w++) s->words[w] = word(w);
        n++;
        if (s->len_seen == 0) break;
    }
    return n;
}

static void case_record(const char* path)
{
    static Seen seen[REC_FLIPS * REC_POLL_MAX];
    unsigned per_flip[REC_FLIPS];
    unsigned nseen = 0;
    PadTrace* tr;
    PadTraceError e;
    PadTraceOptions o;
    char text[1 << 16];
    FILE* f;
    size_t n;

    unlink(path);
    snprintf(g_cfg.pad_record, sizeof g_cfg.pad_record, "%s", path);
    start();
    tr = pad_trace_get();
    CHECK(tr != NULL && !pad_trace_replaying(tr), "pad.record opens a recorder");

    g_flip = 0;
    CHECK(cellPadGetInfo2((CellPadInfo2*)(uintptr_t)0x400) == CELL_OK, "cellPadGetInfo2");
    for (g_flip = 0; g_flip < REC_FLIPS; g_flip++) {
        fake_ms += PAD_POLL_INTERVAL_MS;   /* one host report per flip: the second poll drains */
        per_flip[g_flip] = read_drain(0, &seen[nseen], REC_POLL_MAX);
        nseen += per_flip[g_flip];
    }
    CHECK(per_flip[0] == 2 && per_flip[REC_FLIPS - 1] == 2, "the live path drains in two polls a flip (%u, %u)",
          per_flip[0], per_flip[REC_FLIPS - 1]);
    CHECK(seen[0].len_seen > 0 && seen[1].len_seen == 0, "a packet on the flip's first poll, len 0 on the drain");
    CHECK(seen[0].words[CELL_PAD_BTN_OFFSET_DIGITAL2] == 0x40, "the scripted CROSS reached the guest");
    CHECK(seen[nseen - 1].words[CELL_PAD_BTN_OFFSET_DIGITAL2] == 0, "and was released after its hold");
    CHECK(seen[0].words[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] == 140, "PAD_STICK reached the guest");

    pad_trace_close(tr);   /* the exit hook does this; here the file is read back below */
    f = fopen(path, "rb");
    CHECK(f != NULL, "the recording exists");
    if (!f) return;
    n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);

    CHECK(strstr(text, "\"len\":8,\"words\":[0,116,0,64,160,170,140,150],\"flip\":0}") != NULL,
          "the first record is the guest's words and len, injectors included");
    CHECK(count_sub(text, "\"len\":0") > 0, "the drained polls are recorded with len 0");
    CHECK(count_sub(text, "\"polls\":") == 0, "no per-flip poll counts in version 3");
    CHECK(strstr(text, "{\"end\":true,\"seq\":48}") != NULL, "the end record is port 0's poll count");
    CHECK(strstr(text, "\"info\":0,\"conn\":[1,0,0,0,0,0,0]") != NULL, "GetInfo's connection is recorded, by call ordinal");

    /* The recording replays to the len and words the guest read, poll for poll: the same drain loops. */
    memset(&o, 0, sizeof o);
    o.replay_path = path;
    o.flip = cellGcm_guest_flip_count;
    memset(&e, 0, sizeof e);
    {
        PadTrace* rd = NULL;
        unsigned i = 0, rep_polls[REC_FLIPS];
        int len_same = 1, words_same = 1, counts_same = 1;
        CHECK(pad_trace_create(&o, &rd, &e) == 0 && rd, "the recording reads back: line %u %s %s", e.line, e.reason, e.detail);
        if (!rd) return;
        for (g_flip = 0; g_flip < REC_FLIPS; g_flip++) {
            unsigned k = 0;
            for (; k < REC_POLL_MAX; k++) {
                PadTraceState st, neutral;
                memset(&neutral, 0, sizeof neutral);
                memset(&st, 0, sizeof st);
                pad_trace_poll(rd, 0, &st, &neutral);
                CHECK(i < nseen && st.len == seen[i].len_seen && !memcmp(st.words, seen[i].words, sizeof st.words),
                      "poll %u (flip %u): the replay hands back the recorded len and words", i, seen[i].flip);
                if (i >= nseen || st.len != seen[i].len_seen) len_same = 0;
                if (i >= nseen || memcmp(st.words, seen[i].words, sizeof st.words)) words_same = 0;
                i++;
                if (st.len == 0) { k++; break; }
            }
            rep_polls[g_flip] = k;
            if (k != per_flip[g_flip]) counts_same = 0;
        }
        CHECK(i == nseen, "the replay drove every poll the recording made (%u of %u)", i, nseen);
        CHECK(len_same && words_same, "the recording's len sequence and words replay exactly");
        CHECK(counts_same, "the replay's drain loop is as long as the recording's in every flip");
        {
            PadTraceStats s;
            pad_trace_stats(rd, &s);
            CHECK(s.records > 0 && s.records_reached == s.records,
                  "every recorded change was reached at its own seq (%llu/%llu)",
                  (unsigned long long)s.records_reached, (unsigned long long)s.records);
            CHECK(s.have_drift && s.drift_min == 0 && s.drift_max == 0,
                  "the same flips drift by nothing (%lld:%lld)", (long long)s.drift_min, (long long)s.drift_max);
        }
        pad_trace_destroy(rd);
    }
}

int main(int argc, char** argv)
{
    if (argc != 3) { fprintf(stderr, "usage: test_pad_replay <case> <trace file>\n"); return 2; }
    if (!strcmp(argv[1], "control") || !strcmp(argv[1], "replay") || !strcmp(argv[1], "record")) {
        setenv("PAD_SCRIPT", "0:0x4000", 1);
        setenv("PAD_STICK", "140,150,160,170", 1);
        if (strcmp(argv[1], "record")) setenv("PAD_AUTOPRESS", "1", 1);
    }
    if      (!strcmp(argv[1], "control"))     case_control();
    else if (!strcmp(argv[1], "replay"))      case_replay(argv[2]);
    else if (!strcmp(argv[1], "replay-info")) case_replay_info(argv[2]);
    else if (!strcmp(argv[1], "record"))      case_record(argv[2]);
    else { fprintf(stderr, "unknown case %s\n", argv[1]); return 2; }
    if (!g_fail) printf("PASS test_pad_replay %s\n", argv[1]);
    return g_fail ? 1 : 0;
}
