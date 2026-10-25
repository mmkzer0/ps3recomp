/*
 * ps3recomp - cellSaveData bounds the funcFile fileBuf span before it becomes
 * a host pointer.
 *
 * The funcFile callback reports its data buffer as a guest EA in
 * CellSaveDataFileSet.fileBuf (set+44) plus fileBufSize (set+40).
 * dispatch_func_file turns that into `vm_base + buffer` and process_file_op
 * then fread/fwrite's through it. The host kernel cannot fault a reserved
 * guest page in, so the span is checked first: a span that leaves guest memory
 * is a named stop (hle-guest-range, naming dispatch_func_file and fileBuf),
 * not a host pointer into whatever follows the arena.
 *
 * The savedata APIs end the process on a stop, so each refused case runs in a
 * forked child; the parent checks the exit status and the machine line.
 * The arena has a PROT_NONE guard page after it, so a transfer the check
 * misses faults instead of passing silently. An in-arena fileBuf still makes
 * the same save round trip. An in-arena fileBuf whose pages the host cannot
 * commit (here: unmapped under the arena) stops with hle-commit-failed; the
 * commit result used to be dropped, and the fwrite then faulted.
 *
 *   libs/system/tests/test_savedata_filebuf_range.c, linked with the real stop
 *   implementation (it includes ../cellSaveData.c, as the other savedata
 *   fixtures do).
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

/* cellSaveData_register_ctx_handlers is not on the path under test; the other
 * savedata fixtures drop it with dead_strip. */
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; }

static const uint32_t ARENA = 0x100000;
static const char payload[] = "hello";

static uint32_t s_file_buf_ea;
static uint32_t s_file_buf_size = sizeof(payload);
static int s_file_ops;
static int fails;

#define CHECK(c, ...) do { const int ok_ = (c); \
                           printf(ok_ ? "[PASS] " : "[FAIL] "); \
                           printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

/* funcFixed / funcStat / funcFile: the guest side of the callback contract. */
static void callback(u32 opd, u64 cb_ea, u64 get, u64 set,
                     u64 a, u64 b, u64 c, u64 d, u64 e)
{
    (void)get; (void)a; (void)b; (void)c; (void)d; (void)e;
    if (opd == 0x800) {                       /* funcFixed */
        vm_write32(set, 0x340);               /* dirName: guest EA of "TEST00000" */
        vm_write32(set + 8, 0);               /* option */
    } else if (opd == 0x900) {                /* funcStat: leave OK_NEXT */
    } else if (opd == 0xA00) {                /* funcFile */
        if (s_file_ops++ > 0) {               /* one operation, then LAST */
            vm_write32(cb_ea, CELL_SAVEDATA_CBRESULT_OK_LAST);
            return;
        }
        vm_write32(set, CELL_SAVEDATA_FILEOP_WRITE);
        vm_write32(set + 8, CELL_SAVEDATA_FILETYPE_NORMALFILE);
        vm_write32(set + 28, 0x360);          /* fileName: "DATA.BIN" */
        vm_write32(set + 32, 0);              /* fileOffset */
        vm_write32(set + 36, sizeof(payload));/* fileSize */
        vm_write32(set + 40, s_file_buf_size);
        vm_write32(set + 44, s_file_buf_ea);  /* fileBuf: guest EA */
    }
}

static s32 run_save(void);

/* Run one save in a forked child and require it to end in the named stop
 * `kind` with `func=cellSaveData arg=fileBuf`. `prep` runs in the child first. */
static void expect_stop(const char* what, const char* kind, void (*prep)(void))
{
    int fds[2];
    if (pipe(fds) != 0) { printf("FAIL: pipe\n"); exit(1); }
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        if (prep) prep();
        run_save();
        _exit(1);   /* returned: the span was not refused */
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
          "%s stops (status 0x%X)", what, status);
    const char* line = strstr(out, "PS3STOP v=1");
    char want[64];
    snprintf(want, sizeof want, "kind=%s", kind);
    CHECK(line && strstr(line, want) &&
          strstr(line, "func=cellSaveData") && strstr(line, "arg=fileBuf"),
          "%s names itself in the machine line%s%s", what,
          line ? ": " : " (no machine line)", line ? line : "");
}

static const uint32_t UNCOMMITTABLE_EA = 0x8000;   /* host-page aligned on 4K and 16K hosts */

/* Unmap one host page inside the arena: mprotect on it fails (ENOMEM), which is
 * what a host that cannot back a guest page looks like to vm_commit. */
static void unmap_buffer_page(void)
{
    munmap(vm_base + UNCOMMITTABLE_EA, (size_t)sysconf(_SC_PAGESIZE));
}

/* One save attempt: returns the HLE result, or exits the process on a stop. */
static s32 run_save(void)
{
    s_file_ops = 0;
    return cellSaveDataFixedSave2(0, (CellSaveDataSetList*)(uintptr_t)0x200,
                                  (CellSaveDataSetBuf*)(uintptr_t)0x300,
                                  (CellSaveDataFixedCallback)(uintptr_t)0x800,
                                  (CellSaveDataStatCallback)(uintptr_t)0x900,
                                  (CellSaveDataFileCallback)(uintptr_t)0xA00,
                                  0, (void*)(uintptr_t)7);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* forked children must not repeat it */
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t* p = mmap(NULL, (size_t)ARENA + page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { printf("FAIL: could not map the arena\n"); return 1; }
    if (mprotect(p + ARENA, page, PROT_NONE) != 0) { printf("FAIL: guard page\n"); return 1; }
    vm_base = p;
    ppu_vm_size = ARENA;

    char root[] = "/tmp/ps3recomp-savedata-filebuf-XXXXXX";
    if (!mkdtemp(root)) { printf("FAIL: mkdtemp\n"); return 1; }
    snprintf(s_save_root, sizeof s_save_root, "%s", root);
    if (cellSaveData_set_scratch_region(0x40000, 0x40000) != CELL_OK) {
        printf("FAIL: scratch region\n"); return 1;
    }
    strcpy((char*)vm_base + 0x340, "TEST00000");
    strcpy((char*)vm_base + 0x360, "DATA.BIN");
    memcpy(vm_base + 0x4000, payload, sizeof(payload));
    vm_write32(0x200, 4);   /* setList: dirListMax */
    vm_write32(0x300, 2);   /* setBuf: fileListMax */
    g_ps3_guest_caller = callback;

    /* An in-arena fileBuf still makes the round trip. */
    s_file_buf_ea = 0x4000;
    const s32 rc = run_save();
    CHECK(rc == CELL_OK, "an in-arena fileBuf saves (rc=0x%08X)", (unsigned)rc);
    char path[512], got[sizeof(payload)] = { 0 };
    snprintf(path, sizeof path, "%s/TEST00000/DATA.BIN", root);
    FILE* f = fopen(path, "rb");
    CHECK(f && fread(got, 1, sizeof got, f) == sizeof got &&
          memcmp(got, payload, sizeof payload) == 0,
          "an in-arena fileBuf reaches the file");
    if (f) fclose(f);

    /* A fileBuf span that leaves guest memory stops, before fwrite can read it
     * through the guard page. */
    s_file_buf_ea = ARENA - 2;
    s_file_buf_size = 0x10;
    expect_stop("an out-of-arena fileBuf", "hle-guest-range", NULL);

    /* An in-arena fileBuf the host cannot commit stops before the fwrite. */
    s_file_buf_ea = UNCOMMITTABLE_EA;
    s_file_buf_size = 0x10;
    expect_stop("an uncommittable fileBuf", "hle-commit-failed", unmap_buffer_page);

    munmap(p, (size_t)ARENA + page);
    snprintf(path, sizeof path, "%s/TEST00000/DATA.BIN", root); unlink(path);
    snprintf(path, sizeof path, "%s/TEST00000", root); rmdir(path);
    rmdir(root);
    printf("%s (%d failure%s)\n", fails ? "test_savedata_filebuf_range: FAIL"
                                        : "test_savedata_filebuf_range: OK",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
