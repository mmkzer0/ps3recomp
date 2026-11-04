/* SPU_INTERP_OPSTATS: the per-op execution counts of a synthetic program with
 * known counts, folded exactly across thread-local counters, a run's end and
 * several interpreting threads, and the one-printer election. The fixture
 * includes the interpreter so it can reach the report and the clock check; the
 * counts themselves come only from interpreting.
 *
 * argv[1] = "off": SPU_INTERP_OPSTATS is unset and nothing may be counted;
 * otherwise it is set before the first run. */
#include "../spu_interp.c"
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

/* The programs here are pure integer code: the external hooks must never run. */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c; (void)a; abort(); }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

/* ISA layouts (IBM SPU ISA v1.2, ch. 2). */
static uint32_t ri16(uint32_t op9, uint32_t i16, uint32_t rt) {
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt;
}
static uint32_t ri10(uint32_t op8, uint32_t i10, uint32_t ra, uint32_t rt) {
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt;
}
static void be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* Counts per execution: il 2, ai LOOPS, brnz LOOPS, stop 1. LOOPS is above
 * OPS_FOLD so a run folds in the middle and again when it returns. */
enum { LOOPS = 10000, EXEC_TOTAL = 2 * LOOPS + 3 };
static void load_program(spu_context* ctx) {
    be32(ctx->ls + 0x100, ri16(0x081, LOOPS, 3));       /* il   $3, LOOPS */
    be32(ctx->ls + 0x104, ri16(0x081, 0, 4));           /* il   $4, 0     */
    be32(ctx->ls + 0x108, ri10(0x1C, 0x3FF, 3, 3));     /* ai   $3, $3, -1 */
    be32(ctx->ls + 0x10C, ri16(0x042, 0xFFFF, 3));      /* brnz $3, 0x108 */
    be32(ctx->ls + 0x110, 0);                           /* stop 0         */
}
static void run_once(spu_context* ctx) {
    memset(ctx, 0, sizeof *ctx);
    spu_context_init(ctx, 0);
    ctx->image_id = -1;   /* pure interpretation: no registry lookup */
    load_program(ctx);
    spu_interp_run(ctx, 0x100);
    CHECK(ctx->status == SPU_STATUS_STOPPED_BY_STOP && ctx->pc == 0x114);
    CHECK(ctx->gpr[3]._u32[0] == 0);
}

/* The report line as text, through a temporary file. */
static void report_line(char* line, size_t cap) {
    FILE* f = tmpfile();
    CHECK(f);
    interp_opstats_report(f);
    rewind(f);
    CHECK(fgets(line, (int)cap, f));
    CHECK(fgetc(f) == EOF);   /* exactly one line */
    fclose(f);
}
/* The count the line gives an op, or -1 when the op is not listed. */
static long long line_count(const char* line, const char* op) {
    char key[32];
    snprintf(key, sizeof key, " %s=", op);
    const char* p = strstr(line, key);
    return p ? strtoll(p + strlen(key), NULL, 10) : -1;
}
static int line_ops(const char* line) {
    int n = 0;
    for (const char* p = strchr(line, ' '); p; p = strchr(p + 1, ' ')) n++;
    return n - 1;   /* the first field is total= */
}

static spu_context g_ctx[4];   /* a context is 256 KiB of local store: not on the stack */
enum { THREADS = 4, REPS = 5 };
static void* worker(void* arg) {
    spu_context* c = (spu_context*)arg;
    for (int i = 0; i < REPS; i++) run_once(c);
    return NULL;
}
static void* ticker(void* arg) { (void)arg; interp_opstats_tick(3600); return NULL; }

static int check_on(void) {
    char line[4096];

    /* One run: the counts are exact and the line lists exactly the executed
     * ops, busiest first (ai and brnz tie, then il, then stop). */
    run_once(&g_ctx[0]);
    report_line(line, sizeof line);
    CHECK(!strncmp(line, "[spu-ops] total=", 16));
    CHECK(line_count(line, "ai") == LOOPS && line_count(line, "brnz") == LOOPS);
    CHECK(line_count(line, "il") == 2 && line_count(line, "stop") == 1);
    CHECK(strtoll(line + 16, NULL, 10) == EXEC_TOTAL);
    CHECK(line_ops(line) == 4);
    CHECK(strstr(line, " ai=10000 brnz=10000 il=2 stop=1\n"));
    fputs(line, stdout);

    /* Totals are cumulative: a second run adds to the first. */
    run_once(&g_ctx[0]);
    report_line(line, sizeof line);
    CHECK(line_count(line, "ai") == 2 * LOOPS && line_count(line, "il") == 4);
    CHECK(strtoll(line + 16, NULL, 10) == 2 * EXEC_TOTAL);

    /* Several interpreting threads: nothing lost or doubled in the folds. */
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) CHECK(!pthread_create(&t[i], NULL, worker, &g_ctx[i]));
    for (int i = 0; i < THREADS; i++) CHECK(!pthread_join(t[i], NULL));
    report_line(line, sizeof line);
    long long runs = 2 + THREADS * REPS;
    CHECK(line_count(line, "ai") == runs * LOOPS && line_count(line, "brnz") == runs * LOOPS);
    CHECK(line_count(line, "il") == runs * 2 && line_count(line, "stop") == runs);
    CHECK(strtoll(line + 16, NULL, 10) == runs * EXEC_TOTAL);
    CHECK(line_ops(line) == 4);

    /* The printer is elected: of several threads past the period at once,
     * exactly one prints. stderr is redirected to a file for the check. */
    fflush(stderr);
    int saved = dup(2);
    FILE* cap = tmpfile();
    CHECK(saved >= 0 && cap && dup2(fileno(cap), 2) >= 0);
    atomic_store(&s_ops_last_ms, 1);   /* the last print was long ago */
    for (int i = 0; i < THREADS; i++) CHECK(!pthread_create(&t[i], NULL, ticker, NULL));
    for (int i = 0; i < THREADS; i++) CHECK(!pthread_join(t[i], NULL));
    fflush(stderr);
    CHECK(dup2(saved, 2) >= 0);
    close(saved);
    rewind(cap);
    int printed = 0;
    while (fgets(line, sizeof line, cap)) {
        CHECK(!strncmp(line, "[spu-ops] total=", 16));
        CHECK(line_count(line, "ai") == runs * LOOPS);   /* cumulative, not per period */
        printed++;
    }
    fclose(cap);
    CHECK(printed == 1);
    printf("opstats: exact over %lld runs on %d threads, one printer of %d\n", runs, THREADS, THREADS);
    return 0;
}

/* Env unset: the same program counts nothing and the report is empty. */
static int check_off(void) {
    char line[256];
    run_once(&g_ctx[0]);
    report_line(line, sizeof line);
    CHECK(!strcmp(line, "[spu-ops] total=0\n"));
    printf("opstats: off counts nothing\n");
    return 0;
}

int main(int argc, char** argv) {
    int off = argc > 1 && !strcmp(argv[1], "off");
    if (off) unsetenv("SPU_INTERP_OPSTATS");
    else setenv("SPU_INTERP_OPSTATS", "3600", 1);   /* no periodic print inside the test's run */
    return off ? check_off() : check_on();
}
