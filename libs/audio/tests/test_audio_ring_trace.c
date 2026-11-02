/* PS3_FTRACE kinds 30 (PS3_FT_AUDIO_RING, the mixer's read) and 31 (PS3_FT_AUDIO_RING_VIEW, the guest's
 * cellAudioGetPortConfig) over the real mixer, stepped by hand: no device, no thread, so every run is the same.
 * The scripted producer models the guest protocol the trace has to explain: view the port, read the index the
 * view names, write block w while 0 < (w - r) mod n < lead. Equality (r == w) reads as a full ring.
 * macOS: clang -std=gnu17 -I include -I /opt/homebrew/include
 *   libs/audio/tests/test_audio_ring_trace.c -L /opt/homebrew/lib -lSDL2
 *   -Wl,-dead_strip -o /tmp/test_audio_ring_trace && /tmp/test_audio_ring_trace
 */
#include "../cellAudio.c"

uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
uint32_t sys_event_find_queue_by_key(uint64_t key) { (void)key; return 0; }
void sys_event_queue_push_from_kernel(uint32_t q, uint64_t s, uint64_t d1,
                                      uint64_t d2, uint64_t d3, const char* p)
{ (void)q; (void)s; (void)d1; (void)d2; (void)d3; (void)p; }

static int g_failures;
#define CHECK(c, ...) do { if (!(c)) { g_failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                       printf(__VA_ARGS__); printf("\n"); } } while (0)
#define REQUIRE(c, ...) do { if (!(c)) { CHECK(0, __VA_ARGS__); exit(1); } } while (0)

enum { PORT = 3, CFG = 0x300, LEAD = 3, PAT = 0x3F800000u };

/* Tracing is switched by the recorder's own state word (2 recording, 3 off), after one real init with the
 * environment set: the same init a title's first hook runs. */
static void trace_on(void)  { g_ps3_ft.n = 0; g_ps3_ft.dropped = 0; __atomic_store_n(&g_ps3_ft.on, 2, __ATOMIC_RELEASE); }
static void trace_off(void) { __atomic_store_n(&g_ps3_ft.on, 3, __ATOMIC_RELEASE); g_ps3_ft.n = 0; }

static uint32_t rec_count(void) { return g_ps3_ft.n; }
static const ps3_ft_rec* rec_at(uint32_t i) { return &g_ps3_ft.buf[i]; }
static uint32_t count_kind(uint32_t kind)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < rec_count(); i++) c += rec_at(i)->kind == kind;
    return c;
}

/* The k-th record of a kind, in time order (single-threaded: buffer order), or NULL. */
static const ps3_ft_rec* nth_kind(uint32_t kind, uint32_t k)
{
    for (uint32_t i = 0; i < rec_count(); i++)
        if (rec_at(i)->kind == kind && k-- == 0) return rec_at(i);
    return NULL;
}

static uint32_t a_tag(uint32_t a)    { return a >> 8; }
static uint32_t a_nblock(uint32_t a) { return ((a >> 3) & 31u) + 1u; }
static uint32_t a_port(uint32_t a)   { return a & 7u; }

/* Fresh ports 0..port opened through the real call (so the ring and its index live at real guest addresses);
 * only `port` runs, the others are skipped by the mixer. */
static AudioPortSlot* open_ring(unsigned port, unsigned nch, unsigned nblock)
{
    memset(s_ports, 0, sizeof s_ports);
    memset(s_audio_port_prev_non_silent, 0, sizeof s_audio_port_prev_non_silent);
    memset(s_audio_port_unrefill_run, 0, sizeof s_audio_port_unrefill_run);
    s_audio_initialized = 1;
    vm_write64(0x100, nch);
    vm_write64(0x108, nblock);
    for (unsigned i = 0; i <= port; i++)
        REQUIRE(cellAudioPortOpen((void*)0x100, (void*)0x200) == CELL_OK, "open port %u", i);
    s_ports[port].running = 1;
    return &s_ports[port];
}

static uint32_t block_words(const AudioPortSlot* p) { return CELL_AUDIO_BLOCK_SAMPLES * (uint32_t)p->param.nChannel; }

static void fill_block(const AudioPortSlot* p, uint32_t b)
{
    for (uint32_t i = 0; i < block_words(p); i++)
        vm_write32((uint32_t)p->port_addr + (b * block_words(p) + i) * 4u, PAT | (b + 1u));
}

/* ---- 1 and 2: the exact records ------------------------------------------------------------------------ */

static void fill_exact_ring(const AudioPortSlot* p)
{
    const uint32_t pa = (uint32_t)p->port_addr, w = block_words(p);
    vm_write32(pa + (0 * w + 0) * 4u, PAT);               /* first word of block 0 */
    vm_write32(pa + (1 * w + 77) * 4u, 0x00000001u);      /* a denormal in the middle of block 1 */
    vm_write32(pa + (5 * w + w - 1) * 4u, 0x80000000u);   /* -0.0f as the last word of block 5: raw, so filled */
}

static void test_exact_read_record(void)
{
    AudioPortSlot* p = open_ring(2, 2, 8);
    fill_exact_ring(p);
    p->read_index = 3;
    trace_on();
    audio_mix_one_block();
    REQUIRE(rec_count() == 1 && rec_at(0)->kind == PS3_FT_AUDIO_RING, "one kind 30, got %u records", rec_count());
    const ps3_ft_rec* r = rec_at(0);
    CHECK(r->who == 2, "who %u", r->who);
    CHECK(r->a == (3u << 8 | 7u << 3 | 2u), "a %#x", r->a);
    CHECK(r->b == 0x23u, "b %#x, want blocks 0,1,5", r->b);
    CHECK(r->t0 == r->t1, "not an instant: %llu..%llu", (unsigned long long)r->t0, (unsigned long long)r->t1);

    /* The mask is the ring before the read: block 5 still shows at tag 5 and is gone at tag 6. Kind 21 is not
     * counted here: it compares host floats, and the byte-swapped -0.0f in block 5 is a non-zero host float. */
    audio_mix_one_block();
    audio_mix_one_block();
    audio_mix_one_block();
    const uint32_t want_b[4] = { 0x23, 0x23, 0x23, 0x03 };
    REQUIRE(count_kind(PS3_FT_AUDIO_RING) == 4, "%u kind 30 records after 4 reads", count_kind(PS3_FT_AUDIO_RING));
    uint32_t k = 0;
    for (uint32_t i = 0; i < rec_count(); i++) {
        if (rec_at(i)->kind != PS3_FT_AUDIO_RING) continue;
        CHECK(a_tag(rec_at(i)->a) == 3 + k && rec_at(i)->b == want_b[k], "read %u: tag %u b %#x",
              k, a_tag(rec_at(i)->a), rec_at(i)->b);
        k++;
    }
}

static void test_exact_view_record(void)
{
    AudioPortSlot* p = open_ring(2, 2, 8);
    fill_exact_ring(p);
    p->read_index = 3;
    trace_on();

    static ppu_context ctx;
    ctx.thread_id = ppu_tid_make(5, 9);   /* table slot 9: trace tid 10 */
    g_active_ctx = &ctx;
    CHECK(cellAudioGetPortConfig(2, (void*)CFG) == CELL_OK, "view");
    CHECK(vm_read32(CFG + 4) == CELL_AUDIO_STATUS_RUN, "config status %u", vm_read32(CFG + 4));
    REQUIRE(rec_count() == 1 && rec_at(0)->kind == PS3_FT_AUDIO_RING_VIEW, "one kind 31, got %u records", rec_count());
    const ps3_ft_rec* v = rec_at(0);
    CHECK(v->who == 10, "who %u", v->who);
    CHECK(v->a == (3u << 8 | 7u << 3 | 2u), "a %#x", v->a);
    CHECK(v->b == 0x23u, "b %#x", v->b);
    CHECK(v->t1 >= v->t0 && v->t0 != 0, "span %llu..%llu", (unsigned long long)v->t0, (unsigned long long)v->t1);

    g_active_ctx = NULL;   /* a host thread: tid 0, as the label-wait records it */
    CHECK(cellAudioGetPortConfig(2, (void*)CFG) == CELL_OK, "view without a context");
    REQUIRE(rec_count() == 2, "%u records", rec_count());
    CHECK(rec_at(1)->kind == PS3_FT_AUDIO_RING_VIEW && rec_at(1)->who == 0, "who %u without a context", rec_at(1)->who);

    /* The index as of the call: after a mixer step the view carries tag 4 and the block read is gone. */
    audio_mix_one_block();
    CHECK(cellAudioGetPortConfig(2, (void*)CFG) == CELL_OK, "view after a step");
    REQUIRE(rec_count() == 4 && rec_at(3)->kind == PS3_FT_AUDIO_RING_VIEW, "%u records", rec_count());
    CHECK(a_tag(rec_at(3)->a) == 4 && rec_at(3)->b == 0x23, "tag %u b %#x", a_tag(rec_at(3)->a), rec_at(3)->b);

    /* No view for a call that does not describe a running ring. */
    const uint32_t before = rec_count();
    p->running = 0;
    CHECK(cellAudioGetPortConfig(2, (void*)CFG) == CELL_OK && vm_read32(CFG + 4) == CELL_AUDIO_STATUS_READY, "ready port");
    p->running = 1;
    CHECK(cellAudioGetPortConfig(5, (void*)CFG) == CELL_OK && vm_read32(CFG + 4) == CELL_AUDIO_STATUS_CLOSE, "closed port");
    CHECK((u32)cellAudioGetPortConfig(CELL_AUDIO_PORT_MAX, (void*)CFG) == CELL_AUDIO_ERROR_PARAM, "range");
    CHECK((u32)cellAudioGetPortConfig(2, NULL) == CELL_AUDIO_ERROR_PARAM, "null config");
    CHECK(rec_count() == before, "%u records for calls that are not a view of a running ring", rec_count() - before);
}

/* ---- 3: the 24-bit tag boundary ------------------------------------------------------------------------- */

static void test_tag_wrap(void)
{
    const unsigned sizes[3] = { 2, 8, 32 };
    const uint64_t starts[2] = { (1ull << 24) - 2, 5 * (1ull << 24) - 1 };
    for (unsigned s = 0; s < 3; s++) {
        for (unsigned t = 0; t < 2; t++) {
            const unsigned n = sizes[s];
            AudioPortSlot* p = open_ring(0, 2, n);
            for (unsigned b = 0; b < n; b++) fill_block(p, b);
            p->read_index = starts[t];
            trace_on();
            const unsigned steps = n + 3;
            uint32_t expect = n == 32 ? 0xFFFFFFFFu : (1u << n) - 1u;
            for (unsigned i = 0; i < steps; i++) {
                const uint64_t idx = p->read_index;
                audio_mix_one_block();
                const ps3_ft_rec* r = nth_kind(PS3_FT_AUDIO_RING, i);   /* kind 21 joins once the ring runs dry */
                REQUIRE(r && count_kind(PS3_FT_AUDIO_RING) == i + 1, "n=%u step %u: %u kind 30 records", n, i, count_kind(PS3_FT_AUDIO_RING));
                CHECK(a_tag(r->a) == (uint32_t)(idx & 0xFFFFFFu), "n=%u step %u: tag %u, read_index %llu", n, i, a_tag(r->a), (unsigned long long)idx);
                CHECK(a_tag(r->a) % n == idx % n, "n=%u step %u: tag %% n %u != read_index %% n %llu", n, i, a_tag(r->a) % n, (unsigned long long)(idx % n));
                CHECK(a_nblock(r->a) == n && a_port(r->a) == 0 && r->who == 0, "n=%u step %u: a %#x who %u", n, i, r->a, r->who);
                CHECK(r->b == expect, "n=%u step %u: b %#x, want %#x", n, i, r->b, expect);
                expect &= ~(1u << (idx % n));   /* the block just read is cleared; nothing refills it */
            }
            CHECK(a_tag(nth_kind(PS3_FT_AUDIO_RING, steps - 1)->a) < steps, "n=%u: the tag never wrapped", n);
        }
    }
    /* The 32-block ring with only block 31 filled: bit 31 and no other, and the read of block 31 itself shows it. */
    AudioPortSlot* p = open_ring(0, 2, 32);
    fill_block(p, 31);
    p->read_index = 30;
    trace_on();
    audio_mix_one_block();
    audio_mix_one_block();
    audio_mix_one_block();
    REQUIRE(count_kind(PS3_FT_AUDIO_RING) == 3, "%u kind 30 records", count_kind(PS3_FT_AUDIO_RING));
    const uint32_t b30 = nth_kind(PS3_FT_AUDIO_RING, 0)->b, b31 = nth_kind(PS3_FT_AUDIO_RING, 1)->b,
                   b0 = nth_kind(PS3_FT_AUDIO_RING, 2)->b;
    CHECK(b30 == 0x80000000u && b31 == 0x80000000u && b0 == 0, "block 31: %#x %#x %#x", b30, b31, b0);
}

/* ---- 4 and 5: the scripted producer --------------------------------------------------------------------- */

typedef enum { MODE_STEADY, MODE_HOLD, MODE_PAUSE } Mode;
typedef struct { Mode mode; unsigned nblock, warm, pause, periods; } Script;
typedef struct { uint64_t ridx, ring, cfg; } Obs;

static uint64_t fnv(uint64_t h, const void* data, size_t n)
{
    const uint8_t* b = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static int guest_block_empty(uint32_t port_addr, uint32_t nch, uint32_t b)
{
    for (uint32_t i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * nch; i++)
        if (vm_read32(port_addr + (b * CELL_AUDIO_BLOCK_SAMPLES * nch + i) * 4u)) return 0;
    return 1;
}

/* One producer visit: as many write steps as the protocol allows. A step is a view, a guest read of the index
 * the view names, and a write of block w when 0 < (w - r) mod n < LEAD. In `hold` it writes nothing until its
 * view shows r == w with block w empty; the protocol then refuses (ahead 0 is a full ring) like any other. */
static void producer_visit(unsigned* w, int* hold)
{
    for (;;) {
        if (cellAudioGetPortConfig(PORT, (void*)CFG) != CELL_OK) return;
        if (vm_read32(CFG + 4) != CELL_AUDIO_STATUS_RUN) return;
        const uint32_t ridx_ea = vm_read32(CFG + 0), port_addr = vm_read32(CFG + 28);
        const uint32_t nch = (uint32_t)vm_read64(CFG + 8), n = (uint32_t)vm_read64(CFG + 16);
        const uint32_t r = (uint32_t)vm_read64(ridx_ea);
        const uint32_t ahead = (*w + n - r) % n;
        if (*hold) {
            if (r == *w && guest_block_empty(port_addr, nch, *w)) *hold = 0;
            else return;
        }
        if (!(ahead > 0 && ahead < LEAD)) return;
        for (uint32_t i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * nch; i++)
            vm_write32(port_addr + (*w * CELL_AUDIO_BLOCK_SAMPLES * nch + i) * 4u, PAT | (*w + 1u));
        *w = (*w + 1) % n;
    }
}

/* Period k: the producer's visit, then one mixer step (which reads tag k). The ring starts primed with LEAD
 * blocks, as a producer that ran before the port was started would leave it. */
static void run_script(const Script* sc, Obs* obs)
{
    AudioPortSlot* p = open_ring(PORT, 2, sc->nblock);
    for (uint32_t b = 0; b < LEAD; b++) fill_block(p, b);
    unsigned w = LEAD;
    int hold = 0;
    for (unsigned k = 0; k < sc->periods; k++) {
        if (sc->mode == MODE_HOLD && k == sc->warm) hold = 1;
        const int paused = sc->mode == MODE_PAUSE && k >= sc->warm && k < sc->warm + sc->pause;
        if (!paused) producer_visit(&w, &hold);
        audio_mix_one_block();
        obs[k].ridx = vm_read64((uint32_t)p->read_idx_addr);
        obs[k].ring = fnv(0xCBF29CE484222325ull, p->buffer, p->buf_size);
        uint8_t cfg[32];
        memcpy(cfg, vm_base + CFG, sizeof cfg);
        obs[k].cfg = fnv(0xCBF29CE484222325ull, cfg, sizeof cfg);
    }
}

typedef struct { uint32_t first_idx, first_tag, len; int closed; } Run;

/* Runs of zero reads after a non-zero read, as kind 21 counts them, from kind 30 alone. */
static unsigned find_runs(unsigned port, unsigned nblock, Run* runs, unsigned max)
{
    unsigned nr = 0;
    int seen_filled = 0, open = 0;
    uint32_t last_tag = 0;
    for (uint32_t i = 0; i < rec_count(); i++) {
        const ps3_ft_rec* r = rec_at(i);
        if (r->kind != PS3_FT_AUDIO_RING || a_port(r->a) != port) continue;
        const uint32_t tag = a_tag(r->a);
        const int zero = !((r->b >> (tag % nblock)) & 1u);
        if (!zero) {
            if (open) runs[nr - 1].closed = 1;
            open = 0;
            seen_filled = 1;
        } else if (seen_filled) {
            if (open && tag == last_tag + 1) {
                runs[nr - 1].len++;
            } else if (nr < max) {
                runs[nr].first_idx = i; runs[nr].first_tag = tag; runs[nr].len = 1; runs[nr].closed = 0;
                nr++;
                open = 1;
            }
            last_tag = tag;
        }
    }
    return nr;
}

/* Index of the kind 30 read of `tag` on PORT, or UINT32_MAX. */
static uint32_t read_of_tag(uint32_t tag)
{
    for (uint32_t i = 0; i < rec_count(); i++)
        if (rec_at(i)->kind == PS3_FT_AUDIO_RING && a_port(rec_at(i)->a) == PORT && a_tag(rec_at(i)->a) == tag) return i;
    return UINT32_MAX;
}

/* A view of PORT between records `lo` and `hi` (exclusive) that carries `tag` with block tag % n clear. */
static int boundary_view_between(uint32_t lo, uint32_t hi, uint32_t tag, unsigned n)
{
    for (uint32_t i = lo + 1; i < hi; i++) {
        const ps3_ft_rec* v = rec_at(i);
        if (v->kind != PS3_FT_AUDIO_RING_VIEW || a_port(v->a) != PORT) continue;
        if (a_tag(v->a) == tag && !((v->b >> (tag % n)) & 1u)) return 1;
    }
    return 0;
}

static unsigned views_between(uint32_t lo, uint32_t hi)
{
    unsigned c = 0;
    for (uint32_t i = lo + 1; i < hi; i++) c += rec_at(i)->kind == PS3_FT_AUDIO_RING_VIEW;
    return c;
}

/* The same script with tracing off, then on. Returns the traced run's records in the recorder. */
static void run_both(const Script* sc, const char* name)
{
    enum { MAXP = 512 };
    static Obs off[MAXP], on[MAXP];
    REQUIRE(sc->periods <= MAXP, "script too long");

    g_active_ctx = NULL;
    trace_off();
    run_script(sc, off);
    CHECK(g_ps3_ft.n == 0, "%s: tracing off recorded %u spans (%u kind 30, %u kind 31)", name, g_ps3_ft.n,
          count_kind(PS3_FT_AUDIO_RING), count_kind(PS3_FT_AUDIO_RING_VIEW));

    static ppu_context ctx;
    ctx.thread_id = ppu_tid_make(2, 6);
    g_active_ctx = &ctx;
    trace_on();
    run_script(sc, on);
    g_active_ctx = NULL;
    CHECK(g_ps3_ft.dropped == 0, "%s: %u dropped", name, g_ps3_ft.dropped);

    unsigned diverged = 0;
    for (unsigned k = 0; k < sc->periods; k++) {
        if (off[k].ridx != on[k].ridx || off[k].ring != on[k].ring || off[k].cfg != on[k].cfg) {
            if (!diverged) printf("FAIL %s: period %u differs with tracing on: ridx %llu/%llu ring %016llx/%016llx cfg %016llx/%016llx\n",
                                  name, k, (unsigned long long)off[k].ridx, (unsigned long long)on[k].ridx,
                                  (unsigned long long)off[k].ring, (unsigned long long)on[k].ring,
                                  (unsigned long long)off[k].cfg, (unsigned long long)on[k].cfg);
            diverged++;
        }
    }
    CHECK(!diverged, "%s: %u of %u periods differ between tracing off and on", name, diverged, sc->periods);
}

static void check_views_are_the_producers(const char* name)
{
    for (uint32_t i = 0; i < rec_count(); i++)
        if (rec_at(i)->kind == PS3_FT_AUDIO_RING_VIEW)
            CHECK(rec_at(i)->who == 7, "%s: view %u who %u, want the producer's trace tid 7", name, i, rec_at(i)->who);
}

static void test_steady_no_run(unsigned n)
{
    const Script sc = { MODE_STEADY, n, 0, 0, 4 * n };
    char name[48];
    snprintf(name, sizeof name, "steady n=%u", n);
    run_both(&sc, name);
    Run runs[4];
    CHECK(find_runs(PORT, n, runs, 4) == 0, "%s: a run in a producer that is never late", name);
    CHECK(count_kind(PS3_FT_AUDIO_STALE) == 0, "%s: kind 21 fired", name);
    CHECK(count_kind(PS3_FT_AUDIO_RING) == 4 * n, "%s: %u reads", name, count_kind(PS3_FT_AUDIO_RING));
    CHECK(count_kind(PS3_FT_AUDIO_RING_VIEW) > 0, "%s: no views", name);
    check_views_are_the_producers(name);
}

/* 4.1 Late by exactly the boundary: the view sees r == w with block w empty, and the protocol then refuses
 * to write until r has nearly lapped. */
static void test_late_by_the_boundary(unsigned n)
{
    const Script sc = { MODE_HOLD, n, 5, 0, 5 + 2 * n + 12 };
    char name[48];
    snprintf(name, sizeof name, "boundary n=%u", n);
    run_both(&sc, name);
    check_views_are_the_producers(name);

    Run runs[8];
    const unsigned nr = find_runs(PORT, n, runs, 8);
    REQUIRE(nr == 1, "%s: %u runs, want 1", name, nr);
    CHECK(runs[0].closed, "%s: the run never ended", name);
    CHECK(runs[0].len == n, "%s: run length %u, want exactly nblock %u", name, runs[0].len, n);
    const uint32_t T = runs[0].first_tag;
    CHECK(T == sc.warm + 2, "%s: first zero read at tag %u, want %u (LEAD blocks held at the hold, 2 still unread)", name, T, sc.warm + 2);
    for (uint32_t j = 0; j < n; j++) {
        const uint32_t i = read_of_tag(T + j);
        REQUIRE(i != UINT32_MAX, "%s: no read at tag %u", name, T + j);
        CHECK(!((rec_at(i)->b >> ((T + j) % n)) & 1u), "%s: the read at tag %u was not a zero read", name, T + j);
    }
    const uint32_t before = read_of_tag(T - 1), first = read_of_tag(T), after = read_of_tag(T + n);
    REQUIRE(before != UINT32_MAX && after != UINT32_MAX, "%s: reads around the run", name);
    CHECK((rec_at(before)->b >> ((T - 1) % n)) & 1u, "%s: the read before the run was empty", name);
    CHECK((rec_at(after)->b >> ((T + n) % n)) & 1u, "%s: the read after the run was empty", name);
    CHECK(boundary_view_between(before, first, T, n), "%s: no view with tag %u and block %u clear between the last good read and the first zero read",
          name, T, T % n);

    /* Kind 21 counts the same run: nblock records, lengths 1..n. */
    unsigned stale = 0;
    for (uint32_t i = 0; i < rec_count(); i++) {
        if (rec_at(i)->kind != PS3_FT_AUDIO_STALE) continue;
        stale++;
        CHECK(rec_at(i)->b == stale && rec_at(i)->a == (T + stale - 1) % n, "%s: kind 21 #%u a %u b %u", name, stale, rec_at(i)->a, rec_at(i)->b);
    }
    CHECK(stale == n, "%s: %u kind 21 records, want %u", name, stale, n);
}

/* 4.2 Paused longer than a lap: no view at all while the ring drains, and the first one lands on r == w
 * again, so the run is a lap longer. */
static void test_paused_longer_than_a_lap(unsigned n)
{
    const Script sc = { MODE_PAUSE, n, 5, n + 2, 5 + (n + 2) + 2 * n + 12 };
    char name[48];
    snprintf(name, sizeof name, "pause n=%u", n);
    run_both(&sc, name);
    check_views_are_the_producers(name);

    Run runs[8];
    const unsigned nr = find_runs(PORT, n, runs, 8);
    REQUIRE(nr == 1, "%s: %u runs, want 1", name, nr);
    CHECK(runs[0].closed, "%s: the run never ended", name);
    CHECK(runs[0].len == 2 * n, "%s: run length %u, want 2 * nblock = %u", name, runs[0].len, 2 * n);
    const uint32_t T = runs[0].first_tag;
    CHECK(T == sc.warm + 2, "%s: first zero read at tag %u, want %u", name, T, sc.warm + 2);
    const uint32_t before = read_of_tag(T - 1), first = read_of_tag(T);
    REQUIRE(before != UINT32_MAX && first != UINT32_MAX, "%s: reads around the run", name);
    CHECK(views_between(before, first) == 0, "%s: %u views while the producer was paused", name, views_between(before, first));
    CHECK(!boundary_view_between(before, first, T, n), "%s: a boundary view in a run the producer did not see coming", name);
}

int main(void)
{
    vm_base = mmap(NULL, 0x100000000ULL, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(vm_base != MAP_FAILED, "mmap");
    mutex_init(&s_audio_mutex);

    setenv("PS3_FTRACE", "0:100000:/dev/null", 1);
    setenv("PS3_FTRACE_CAP", "65536", 1);
    REQUIRE(PS3_FT_ON(), "PS3_FTRACE did not start recording");

    test_exact_read_record();
    test_exact_view_record();
    test_tag_wrap();
    const unsigned sizes[3] = { 8, 16, 32 };
    for (unsigned i = 0; i < 3; i++) {
        test_steady_no_run(sizes[i]);
        test_late_by_the_boundary(sizes[i]);
        test_paused_longer_than_a_lap(sizes[i]);
    }

    munmap(vm_base, 0x100000000ULL);
    if (g_failures) { printf("audio ring trace: %d failures\n", g_failures); return 1; }
    puts("audio ring trace: kinds 30/31 exact, tag wrap, boundary and lap-behind runs, no behaviour change");
    return 0;
}
