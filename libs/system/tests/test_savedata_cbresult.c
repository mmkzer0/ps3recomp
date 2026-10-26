/*
 * ps3recomp - cellSaveData callback-result mapping, version/errDialog checks
 * and the statGet.fileList contract (ps3recomp #48).
 *
 * Contract under test, from FW 4.93 libsysutil (out/M3/SAVEDATA-FW-RE.md) with
 * RPCS3 d00d8023 cellSaveData.cpp as the oracle where the firmware RE is silent:
 *
 *   funcStat result   0 continues; 1 returns CELL_OK at once (no PARAM.SFO, no
 *                     funcFile); -1..-5 return CBRESULT, -4 included; 2 and
 *                     everything else return PARAM.
 *   funcFile result   (RPCS3) 1 and 2 end the sequence with CELL_OK; -1..-5
 *                     give CBRESULT; everything else PARAM.
 *   version > 1       PARAM at every entry that takes `version`; errDialog > 2
 *                     PARAM at every entry that has one. Both come before the
 *                     NULL checks and before any callback runs.
 *   statGet.fileList  the guest address of setBuf->buf, also for an empty list;
 *                     the entries are written into that buffer, bounded by
 *                     bufSize. A buffer span outside guest memory stops as
 *                     hle-guest-range before funcStat runs.
 *
 * The guest side is a g_ps3_guest_caller stub; the refused span runs in a forked
 * child because a stop ends the process, so this links the real stop
 * implementation.
 */
#include "../cellSaveData.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; }

enum {
    OPD_LIST = 0x700, OPD_FIXED = 0x800, OPD_STAT = 0x900, OPD_FILE = 0xA00,
    SETLIST_EA = 0x200, SETBUF_EA = 0x300, DIRNAME_EA = 0x340, PREFIX_EA = 0x360,
    PARAM_EA = 0x2000, NEWDATA_EA = 0x2100,
    BUF_EA = 0x10000, BUF_SIZE = 0x400,
};
static const uint32_t ARENA = 0x100000;
static const uint32_t SCRATCH_BASE = 0x40000, SCRATCH_SIZE = 0x40000;

static int fails;
#define CHECK(c, ...) do { const int ok_ = (c); \
                           printf(ok_ ? "[PASS] " : "[FAIL] "); \
                           printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

/* What the guest callbacks do, and what they saw. */
static s32 s_stat_result, s_file_result;
static int s_set_param;
static int s_stat_calls, s_file_calls;
static uint32_t s_seen_new, s_seen_file_num, s_seen_list_num, s_seen_list;
static char s_seen_first_name[16];
static volatile int* s_shared_stat_calls;   /* survives the fork */

static void callback(u32 opd, u64 cb, u64 get, u64 set,
                     u64 a, u64 b, u64 c, u64 d, u64 e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (opd == OPD_LIST) {                    /* funcList: a new save, by name */
        vm_write32(set + 16, NEWDATA_EA);
    } else if (opd == OPD_FIXED) {            /* funcFixed */
        vm_write32(set, DIRNAME_EA);
        vm_write32(set + 8, 0);
    } else if (opd == OPD_STAT) {
        s_stat_calls++;
        if (s_shared_stat_calls) (*s_shared_stat_calls)++;
        s_seen_new = vm_read32(get + 4);
        s_seen_file_num = vm_read32(get + 1628);
        s_seen_list_num = vm_read32(get + 1632);
        s_seen_list = vm_read32(get + 1636);
        s_seen_first_name[0] = 0;
        if (s_seen_list_num && s_seen_list)
            memcpy(s_seen_first_name, vm_base + s_seen_list + 40, 13);
        if (s_set_param) vm_write32(set, PARAM_EA);
        vm_write32(cb, (u32)s_stat_result);
    } else if (opd == OPD_FILE) {
        s_file_calls++;
        vm_write32(cb, (u32)s_file_result);
    }
}

static char s_root[64];

static void save_path(char* out, size_t n, const char* leaf)
{
    snprintf(out, n, "%s/TEST00000%s%s", s_root, leaf ? "/" : "", leaf ? leaf : "");
}

static void clear_save(void)
{
    static const char* const files[] = { "PARAM.SFO", "DATA.BIN", "ICON0.PNG" };
    char p[256];
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        save_path(p, sizeof p, files[i]);
        unlink(p);
    }
    save_path(p, sizeof p, NULL);
    rmdir(p);
}

static int has_param_sfo(void)
{
    char p[256];
    save_path(p, sizeof p, "PARAM.SFO");
    return access(p, F_OK) == 0;
}

static void make_save(int files)
{
    char p[256];
    save_path(p, sizeof p, NULL);
    mkdir(p, 0700);
    static const char* const names[] = { "PARAM.SFO", "DATA.BIN", "ICON0.PNG" };
    for (int i = 0; i < 1 + files; i++) {
        save_path(p, sizeof p, names[i]);
        FILE* f = fopen(p, "wb");
        if (!i) fwrite("\0PSF", 1, 4, f); else fwrite("x", 1, 1, f);
        fclose(f);
    }
}

static void reset_callbacks(s32 stat_result, s32 file_result)
{
    s_stat_result = stat_result; s_file_result = file_result;
    s_set_param = 0; s_stat_calls = s_file_calls = 0;
    s_seen_new = s_seen_file_num = s_seen_list_num = s_seen_list = 0;
    s_seen_first_name[0] = 0;
}

typedef s32 (*entry_fn)(u32 version, u32 err_dialog);

#define SETBUF   ((CellSaveDataSetBuf*)(uintptr_t)SETBUF_EA)
#define SETLIST  ((CellSaveDataSetList*)(uintptr_t)SETLIST_EA)
#define DIRNAME  ((const char*)(uintptr_t)DIRNAME_EA)
#define STAT     ((CellSaveDataStatCallback)(uintptr_t)OPD_STAT)
#define FILEFN   ((CellSaveDataFileCallback)(uintptr_t)OPD_FILE)
#define FIXEDFN  ((CellSaveDataFixedCallback)(uintptr_t)OPD_FIXED)
#define LISTFN   ((CellSaveDataListCallback)(uintptr_t)OPD_LIST)
#define UD       ((void*)(uintptr_t)7)

static s32 call_autoload2(u32 v, u32 d) { return cellSaveDataAutoLoad2(v, DIRNAME, d, SETBUF, STAT, FILEFN, 0, UD); }
static s32 call_autoload(u32 v, u32 d)  { return cellSaveDataAutoLoad(v, DIRNAME, d, SETBUF, STAT, FILEFN, 0, UD); }
static s32 call_autosave2(u32 v, u32 d) { return cellSaveDataAutoSave2(v, DIRNAME, d, SETBUF, STAT, FILEFN, 0, UD); }
static s32 call_autosave(u32 v, u32 d)  { return cellSaveDataAutoSave(v, DIRNAME, d, SETBUF, STAT, FILEFN, 0, UD); }
static s32 call_fixedload2(u32 v, u32 d)
{ (void)d; return cellSaveDataFixedLoad2(v, SETLIST, SETBUF, FIXEDFN, STAT, FILEFN, 0, UD); }
static s32 call_fixedsave2(u32 v, u32 d)
{ (void)d; return cellSaveDataFixedSave2(v, SETLIST, SETBUF, FIXEDFN, STAT, FILEFN, 0, UD); }
static s32 call_listsave2(u32 v, u32 d)
{ (void)d; return cellSaveDataListSave2(v, SETLIST, SETBUF, LISTFN, STAT, FILEFN, 0, UD); }
static s32 call_listautoload(u32 v, u32 d)
{ return cellSaveDataListAutoLoad(v, d, SETLIST, SETBUF, FIXEDFN, STAT, FILEFN, 0, UD); }

static const struct { const char* name; entry_fn fn; int has_err_dialog; } ENTRIES[] = {
    { "AutoLoad2",     call_autoload2,    1 },
    { "AutoLoad",      call_autoload,     1 },
    { "AutoSave2",     call_autosave2,    1 },
    { "AutoSave",      call_autosave,     1 },
    { "FixedLoad2",    call_fixedload2,   0 },
    { "FixedSave2",    call_fixedsave2,   0 },
    { "ListSave2",     call_listsave2,    0 },
    { "ListAutoLoad",  call_listautoload, 1 },
};
#define N_ENTRIES (sizeof ENTRIES / sizeof *ENTRIES)

static const char* rc_name(s32 rc)
{
    static char buf[4][24];
    static int k;
    if (rc == CELL_OK) return "CELL_OK";
    if (rc == CELL_SAVEDATA_ERROR_PARAM) return "PARAM";
    if (rc == CELL_SAVEDATA_ERROR_CBRESULT) return "CBRESULT";
    if (rc == CELL_SAVEDATA_ERROR_NODATA) return "NODATA";
    snprintf(buf[k & 3], sizeof buf[0], "0x%08X", (unsigned)rc);
    return buf[k++ & 3];
}

/* Every entry point that runs funcStat gets the same result mapping. */
static void stat_result_matrix(void)
{
    static const struct { s32 r; s32 want; const char* why; } CASES[] = {
        { -6, CELL_SAVEDATA_ERROR_PARAM,    "-6 is below ERR_INVALID" },
        { -5, CELL_SAVEDATA_ERROR_CBRESULT, "-5 ERR_INVALID" },
        { -4, CELL_SAVEDATA_ERROR_CBRESULT, "-4 ERR_NODATA" },
        { -3, CELL_SAVEDATA_ERROR_CBRESULT, "-3 ERR_BROKEN" },
        { -2, CELL_SAVEDATA_ERROR_CBRESULT, "-2 ERR_FAILURE" },
        { -1, CELL_SAVEDATA_ERROR_CBRESULT, "-1 ERR_NOSPACE" },
        {  1, CELL_OK,                      "1 OK_LAST" },
        {  2, CELL_SAVEDATA_ERROR_PARAM,    "2 OK_LAST_NOCONFIRM" },
        {  3, CELL_SAVEDATA_ERROR_PARAM,    "3 is above OK_LAST_NOCONFIRM" },
    };
    for (size_t e = 0; e < N_ENTRIES; e++) {
        for (size_t c = 0; c < sizeof CASES / sizeof *CASES; c++) {
            clear_save();
            reset_callbacks(CASES[c].r, CELL_SAVEDATA_CBRESULT_OK_LAST);
            const s32 rc = ENTRIES[e].fn(0, 1);
            CHECK(rc == CASES[c].want && s_stat_calls == 1 && s_file_calls == 0,
                  "%s: funcStat %s -> %s (rc=%s, stat=%d, file=%d)", ENTRIES[e].name,
                  CASES[c].why, rc_name(CASES[c].want), rc_name(rc), s_stat_calls, s_file_calls);
        }
        /* 0 continues: funcFile runs, and its OK_LAST ends the call with CELL_OK. */
        clear_save();
        reset_callbacks(CELL_SAVEDATA_CBRESULT_OK_NEXT, CELL_SAVEDATA_CBRESULT_OK_LAST);
        const s32 rc = ENTRIES[e].fn(0, 1);
        CHECK(rc == CELL_OK && s_stat_calls == 1 && s_file_calls == 1,
              "%s: funcStat 0 continues into funcFile (rc=%s, stat=%d, file=%d)",
              ENTRIES[e].name, rc_name(rc), s_stat_calls, s_file_calls);
    }
    clear_save();
}

/* Negative test: AutoLoad on a fresh profile with funcStat ERR_NODATA must never
 * report success -- the toolkit used to turn that into CELL_OK for one title. */
static void autoload_nodata_is_never_ok(void)
{
    clear_save();
    reset_callbacks(CELL_SAVEDATA_CBRESULT_ERR_NODATA, CELL_SAVEDATA_CBRESULT_OK_LAST);
    const s32 rc2 = call_autoload2(0, 1);
    CHECK(rc2 != CELL_OK && s_seen_new == 1, "AutoLoad2 on a fresh profile with -4 is not CELL_OK (rc=%s, isNew=%u)",
          rc_name(rc2), s_seen_new);
    reset_callbacks(CELL_SAVEDATA_CBRESULT_ERR_NODATA, CELL_SAVEDATA_CBRESULT_OK_LAST);
    const s32 rc1 = call_autoload(0, 1);
    CHECK(rc1 != CELL_OK && s_seen_new == 1, "AutoLoad on a fresh profile with -4 is not CELL_OK (rc=%s, isNew=%u)",
          rc_name(rc1), s_seen_new);
    for (u32 dialog = 0; dialog <= 2; dialog++) {
        reset_callbacks(CELL_SAVEDATA_CBRESULT_ERR_NODATA, CELL_SAVEDATA_CBRESULT_OK_LAST);
        const s32 rc = call_autoload2(0, dialog);
        CHECK(rc == CELL_SAVEDATA_ERROR_CBRESULT, "AutoLoad2 -4 with errDialog=%u -> CBRESULT (rc=%s)",
              dialog, rc_name(rc));
    }
}

static void func_file_matrix(void)
{
    static const struct { s32 r; s32 want; const char* why; } CASES[] = {
        { -6, CELL_SAVEDATA_ERROR_PARAM,    "-6" },
        { -5, CELL_SAVEDATA_ERROR_CBRESULT, "-5" },
        { -4, CELL_SAVEDATA_ERROR_CBRESULT, "-4 ERR_NODATA" },
        { -1, CELL_SAVEDATA_ERROR_CBRESULT, "-1" },
        {  1, CELL_OK,                      "1 OK_LAST" },
        {  2, CELL_OK,                      "2 OK_LAST_NOCONFIRM" },
        {  3, CELL_SAVEDATA_ERROR_PARAM,    "3" },
    };
    for (size_t c = 0; c < sizeof CASES / sizeof *CASES; c++) {
        clear_save();
        reset_callbacks(CELL_SAVEDATA_CBRESULT_OK_NEXT, CASES[c].r);
        const s32 rc = call_autoload2(0, 1);
        CHECK(rc == CASES[c].want && s_file_calls == 1,
              "AutoLoad2: funcFile %s -> %s (rc=%s, file=%d)", CASES[c].why,
              rc_name(CASES[c].want), rc_name(rc), s_file_calls);
    }
    clear_save();
}

static void ok_last_writes_no_param_sfo(void)
{
    /* Control: the same save with funcStat 0 writes PARAM.SFO. */
    clear_save();
    reset_callbacks(CELL_SAVEDATA_CBRESULT_OK_NEXT, CELL_SAVEDATA_CBRESULT_OK_LAST);
    s_set_param = 1;
    strcpy((char*)vm_base + PARAM_EA, "A TITLE");
    CHECK(call_autosave2(0, 1) == CELL_OK && has_param_sfo(),
          "AutoSave2: funcStat 0 with a setParam writes PARAM.SFO");

    clear_save();
    reset_callbacks(CELL_SAVEDATA_CBRESULT_OK_LAST, CELL_SAVEDATA_CBRESULT_OK_LAST);
    s_set_param = 1;
    const s32 rc = call_autosave2(0, 1);
    CHECK(rc == CELL_OK && !has_param_sfo() && s_file_calls == 0,
          "AutoSave2: funcStat OK_LAST returns CELL_OK without PARAM.SFO or funcFile "
          "(rc=%s, sfo=%d, file=%d)", rc_name(rc), has_param_sfo(), s_file_calls);

    clear_save();
    reset_callbacks(CELL_SAVEDATA_CBRESULT_OK_LAST, CELL_SAVEDATA_CBRESULT_OK_LAST);
    s_set_param = 1;
    const s32 rcf = call_fixedsave2(0, 1);
    CHECK(rcf == CELL_OK && !has_param_sfo() && s_file_calls == 0,
          "FixedSave2: funcStat OK_LAST returns CELL_OK without PARAM.SFO (rc=%s, sfo=%d)",
          rc_name(rcf), has_param_sfo());
    clear_save();
}

static void version_and_err_dialog(void)
{
    for (size_t e = 0; e < N_ENTRIES; e++) {
        clear_save();
        reset_callbacks(CELL_SAVEDATA_CBRESULT_OK_LAST, CELL_SAVEDATA_CBRESULT_OK_LAST);
        s32 rc = ENTRIES[e].fn(2, 1);
        CHECK(rc == CELL_SAVEDATA_ERROR_PARAM && s_stat_calls == 0,
              "%s: version=2 -> PARAM, no callback (rc=%s, stat=%d)", ENTRIES[e].name,
              rc_name(rc), s_stat_calls);
        rc = ENTRIES[e].fn(0xFFFFFFFFu, 1);
        CHECK(rc == CELL_SAVEDATA_ERROR_PARAM && s_stat_calls == 0,
              "%s: version=0xFFFFFFFF -> PARAM (rc=%s)", ENTRIES[e].name, rc_name(rc));
        rc = ENTRIES[e].fn(1, 1);
        CHECK(rc == CELL_OK && s_stat_calls == 1,
              "%s: version=1 is accepted (rc=%s, stat=%d)", ENTRIES[e].name, rc_name(rc), s_stat_calls);
        if (!ENTRIES[e].has_err_dialog) continue;
        reset_callbacks(CELL_SAVEDATA_CBRESULT_OK_LAST, CELL_SAVEDATA_CBRESULT_OK_LAST);
        rc = ENTRIES[e].fn(0, 3);
        CHECK(rc == CELL_SAVEDATA_ERROR_PARAM && s_stat_calls == 0,
              "%s: errDialog=3 -> PARAM, no callback (rc=%s, stat=%d)", ENTRIES[e].name,
              rc_name(rc), s_stat_calls);
        rc = ENTRIES[e].fn(0, 2);
        CHECK(rc == CELL_OK && s_stat_calls == 1,
              "%s: errDialog=2 is accepted (rc=%s, stat=%d)", ENTRIES[e].name, rc_name(rc), s_stat_calls);
    }
    /* version is checked before the NULL checks, but PARAM is PARAM either way:
     * what is observable is that no callback ran for a bad version with good
     * pointers, shown above. Delete has no callbacks, so it shows the check alone. */
    CHECK(cellSaveDataDelete(2, DIRNAME, 0) == CELL_SAVEDATA_ERROR_PARAM,
          "Delete: version=2 -> PARAM");
    CHECK(cellSaveDataDelete(1, DIRNAME, 0) == CELL_OK, "Delete: version=1 -> CELL_OK");
    clear_save();
}

static void file_list_contract(void)
{
    /* An empty list: fileList still points at setBuf->buf. */
    clear_save();
    reset_callbacks(CELL_SAVEDATA_CBRESULT_OK_LAST, CELL_SAVEDATA_CBRESULT_OK_LAST);
    call_autoload2(0, 1);
    CHECK(s_seen_new == 1 && s_seen_list_num == 0 && s_seen_file_num == 0 && s_seen_list == BUF_EA,
          "empty list: fileList == setBuf->buf (0x%X), fileListNum=%u (isNew=%u)",
          s_seen_list, s_seen_list_num, s_seen_new);

    /* A save with a file: the entry lands in the title's buffer, not in scratch. */
    make_save(1);
    memset(vm_base + BUF_EA, 0xAA, 56);
    reset_callbacks(CELL_SAVEDATA_CBRESULT_OK_LAST, CELL_SAVEDATA_CBRESULT_OK_LAST);
    call_autoload2(0, 1);
    CHECK(s_seen_new == 0 && s_seen_list_num == 1 && s_seen_file_num == 1 && s_seen_list == BUF_EA &&
          !strcmp(s_seen_first_name, "DATA.BIN"),
          "one file: fileList == setBuf->buf, entry written there (list=0x%X num=%u name='%s')",
          s_seen_list, s_seen_list_num, s_seen_first_name);
    CHECK(vm_read32(BUF_EA) == CELL_SAVEDATA_FILETYPE_NORMALFILE && vm_read64(BUF_EA + 8) == 1 &&
          vm_base[BUF_EA + 40 + 13] == 0 && vm_base[BUF_EA + 55] == 0,
          "the entry is a whole CellSaveDataFileStat: type, size, name, zero padding");
    CHECK(s_seen_list < SCRATCH_BASE || s_seen_list >= SCRATCH_BASE + SCRATCH_SIZE,
          "the list is not in HLE scratch");

    /* bufSize bounds the entries; fileNum still counts them all. */
    make_save(2);
    vm_write32(SETBUF_EA + 32, 56);
    reset_callbacks(CELL_SAVEDATA_CBRESULT_OK_LAST, CELL_SAVEDATA_CBRESULT_OK_LAST);
    call_autoload2(0, 1);
    CHECK(s_seen_list == BUF_EA && s_seen_list_num == 1 && s_seen_file_num == 2,
          "bufSize 56: one entry fits (num=%u, fileNum=%u)", s_seen_list_num, s_seen_file_num);
    vm_write32(SETBUF_EA + 32, 55);
    reset_callbacks(CELL_SAVEDATA_CBRESULT_OK_LAST, CELL_SAVEDATA_CBRESULT_OK_LAST);
    call_autoload2(0, 1);
    CHECK(s_seen_list == BUF_EA && s_seen_list_num == 0,
          "bufSize 55: no entry fits, fileList still setBuf->buf (num=%u)", s_seen_list_num);
    vm_write32(SETBUF_EA + 32, BUF_SIZE);
    clear_save();
}

/* The span check runs before funcStat, for an empty list too. */
static void guest_range_child(void)
{
    vm_write32(SETBUF_EA + 36, ARENA - 16);
    vm_write32(SETBUF_EA + 32, 0x40);
    reset_callbacks(CELL_SAVEDATA_CBRESULT_OK_LAST, CELL_SAVEDATA_CBRESULT_OK_LAST);
    call_autoload2(0, 1);
    _exit(1);   /* returned: the span was not refused */
}

static void expect_guest_range_stop(void)
{
    int fds[2];
    if (pipe(fds) != 0) { printf("FAIL: pipe\n"); exit(1); }
    *s_shared_stat_calls = 0;
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        guest_range_child();
    }
    close(fds[1]);
    char out[4096];
    size_t used = 0;
    ssize_t n;
    while (used < sizeof out - 1 && (n = read(fds[0], out + used, sizeof out - 1 - used)) > 0)
        used += (size_t)n;
    out[used] = 0;
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == PS3_STOP_EXIT_CODE,
          "a setBuf->buf span outside guest memory stops (status 0x%X)", status);
    const char* line = strstr(out, "PS3STOP v=1");
    CHECK(line && strstr(line, "kind=hle-guest-range") && strstr(line, "func=cellSaveData") &&
          strstr(line, "arg=\"setBuf->buf\""),
          "the stop is hle-guest-range naming setBuf->buf%s%s", line ? ": " : " (no machine line)",
          line ? line : "");
    CHECK(*s_shared_stat_calls == 0, "funcStat never ran for the refused span");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* forked children must not repeat it */
    CHECK((u32)CELL_SAVEDATA_ERROR_NODATA == 0x8002B40Bu, "CELL_SAVEDATA_ERROR_NODATA is 0x8002B40B");
    CHECK((u32)CELL_SAVEDATA_ERROR_CBRESULT == 0x8002B401u, "CELL_SAVEDATA_ERROR_CBRESULT is 0x8002B401");

    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t* p = mmap(NULL, (size_t)ARENA + page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { printf("FAIL: could not map the arena\n"); return 1; }
    if (mprotect(p + ARENA, page, PROT_NONE) != 0) { printf("FAIL: guard page\n"); return 1; }
    s_shared_stat_calls = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (s_shared_stat_calls == (volatile int*)MAP_FAILED) { printf("FAIL: shared page\n"); return 1; }
    vm_base = p;
    ppu_vm_size = ARENA;

    snprintf(s_root, sizeof s_root, "/tmp/ps3recomp-savedata-cbr-XXXXXX");
    if (!mkdtemp(s_root)) { printf("FAIL: mkdtemp\n"); return 1; }
    snprintf(s_save_root, sizeof s_save_root, "%s", s_root);
    if (cellSaveData_set_scratch_region(SCRATCH_BASE, SCRATCH_SIZE) != CELL_OK) {
        printf("FAIL: scratch region\n"); return 1;
    }
    strcpy((char*)vm_base + DIRNAME_EA, "TEST00000");
    strcpy((char*)vm_base + PREFIX_EA, "TEST");
    vm_write32(SETLIST_EA + 8, PREFIX_EA);                /* setList: dirNamePrefix */
    vm_write32(SETBUF_EA + 0, 4);                         /* setBuf: dirListMax */
    vm_write32(SETBUF_EA + 4, 4);                         /* fileListMax */
    vm_write32(SETBUF_EA + 32, BUF_SIZE);                 /* bufSize */
    vm_write32(SETBUF_EA + 36, BUF_EA);                   /* buf */
    vm_write32(NEWDATA_EA + 4, DIRNAME_EA);               /* newData.dirName */
    g_ps3_guest_caller = callback;

    stat_result_matrix();
    autoload_nodata_is_never_ok();
    func_file_matrix();
    ok_last_writes_no_param_sfo();
    version_and_err_dialog();
    file_list_contract();
    expect_guest_range_stop();

    clear_save();
    rmdir(s_root);
    munmap(p, (size_t)ARENA + page);
    printf("%s (%d failure%s)\n", fails ? "test_savedata_cbresult: FAIL" : "test_savedata_cbresult: OK",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
