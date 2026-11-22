/*
 * ps3recomp - cellFsReadWithOffset / cellFsWriteWithOffset (ppu_fs.cpp).
 *
 * Without a handler both imports take the unresolved-NID path: CELL_OK with
 * nothing read or written. This fixture compiles ppu_fs.cpp in, records each
 * ps3_hle_register_ctx() call, and checks:
 *   - each is registered under the NID a title imports it by;
 *   - ReadWithOffset reads at the offset and reports the count;
 *   - WriteWithOffset writes at the offset and reports the count;
 *   - neither moves the fd's own position (a plain cellFsRead continues from it);
 *   - a read past the end returns CELL_OK with a count of 0;
 *   - a bad fd returns CELL_FS_EBADF;
 *   - a write on a read-only fd returns CELL_FS_EIO with the count 0;
 *   - a buffer or count span that leaves guest memory is an hle-guest-range stop, with nothing clamped.
 *
 *   tools/test_ppu_fs_with_offset.py
 *   test_fs_with_offset [DIR]   (DIR: where the file goes; default the system temp directory)
 */
#include "ppu_fs.cpp"
#include <vector>
#ifdef _WIN32
#  include <windows.h>
#endif
#include <string>
#ifndef _WIN32
#  include <sys/wait.h>
#  include <unistd.h>
#endif

/* ---- link stubs: nothing on the paths this test drives calls them ---- */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
void ydkj_host_bt(const char* tag) { (void)tag; }
/* The runtime's stop for an HLE handler handed a guest span outside guest memory. The fixture has no stop
 * machinery, so it prints the same machine line and ends the process the same way: a refusal is then told from
 * the handler transferring a clamped prefix (the child of a fork below). */
PS3_NORETURN void ps3_stop_guest_range(const char* func, const char* arg,
                                       uint64_t ea, uint64_t size, uint64_t limit)
{
    fprintf(stderr, "kind=hle-guest-range func=%s arg=%s ea=0x%llX size=0x%llX limit=0x%llX\n",
            func, arg, (unsigned long long)ea, (unsigned long long)size, (unsigned long long)limit);
    fflush(stderr);
    _Exit(PS3_STOP_EXIT_CODE);
}
}
/* ps3_dev_flash_path (runtime/platform), which host_path calls for /dev_flash: the real one, it needs nothing. */
#include "../../platform/ps3_dev_flash.c"
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

struct Reg { uint32_t nid; const char* name; void (*fn)(ppu_context*); };
static std::vector<Reg> g_regs;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    g_regs.push_back({nid, name, fn});
}
static void (*find(uint32_t nid))(ppu_context*)
{
    for (auto& r : g_regs) if (r.nid == nid) return r.fn;
    return nullptr;
}
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

static uint8_t mem[1 << 16];

/* (fd, u64 offset, buf, u64 size, u64* count) */
static uint32_t io(void (*fn)(ppu_context*), uint32_t fd, uint64_t off, uint32_t buf, uint64_t size,
                   uint32_t count)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = fd; ctx.gpr[4] = off; ctx.gpr[5] = buf; ctx.gpr[6] = size; ctx.gpr[7] = count;
    fn(&ctx);
    return (uint32_t)ctx.gpr[3];
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = mem; ppu_vm_size = sizeof mem;
    ppu_fs_register();

    /* NIDs as they appear in a title's cellFs import table. */
    const uint32_t NID_READ_OFF = 0x0D5B4A14u, NID_WRITE_OFF = 0x8CB722D5u;
    CHECK(ps3_compute_nid("cellFsReadWithOffset") == NID_READ_OFF, "cellFsReadWithOffset: NID 0x%08X is the hash of its name", NID_READ_OFF);
    CHECK(ps3_compute_nid("cellFsWriteWithOffset") == NID_WRITE_OFF, "cellFsWriteWithOffset: NID 0x%08X is the hash of its name", NID_WRITE_OFF);
    void (*rd_off)(ppu_context*) = find(NID_READ_OFF);
    void (*wr_off)(ppu_context*) = find(NID_WRITE_OFF);
    CHECK(rd_off != nullptr, "cellFsReadWithOffset: handler registered");
    CHECK(wr_off != nullptr, "cellFsWriteWithOffset: handler registered");
    if (!rd_off || !wr_off) { printf("FAIL (%d failed)\n", fails); return 1; }

    const char* dir = argc > 1 ? argv[1] : NULL;
#ifdef _WIN32
    char tmp[MAX_PATH];
    if (!dir) { DWORD n = GetTempPathA(sizeof tmp, tmp); if (n && n < sizeof tmp) { tmp[n - 1] = 0; dir = tmp; } }
#endif
    if (!dir) dir = getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    ppu_vfs_root = dir;
    char host[1100];
    snprintf(host, sizeof host, "%s/ps3recomp_fs_with_offset.bin", dir);
    {
        FILE* f = fopen(host, "wb");
        if (!f || fwrite("0123456789ABCDEF", 1, 16, f) != 16) { printf("FAIL: could not create %s\n", host); return 1; }
        fclose(f);
    }

    ppu_context ctx;
    strcpy((char*)mem + 0x100, "/dev_hdd1/ps3recomp_fs_with_offset.bin");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = CELL_FS_O_RDWR; ctx.gpr[5] = 0x500;
    cellFsOpen(&ctx);
    const uint32_t fd = rd32(0x500);
    CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsOpen(O_RDWR) returns CELL_OK (fd %u)", fd);
    if ((uint32_t)ctx.gpr[3] != 0) { remove(host); printf("FAIL (%d failed)\n", fails); return 1; }

    /* Read at an offset. */
    uint32_t rc = io(rd_off, fd, 10, 0x600, 4, 0x508);
    CHECK(rc == 0 && rd64(0x508) == 4 && memcmp(mem + 0x600, "ABCD", 4) == 0,
          "ReadWithOffset(10, 4) reads \"ABCD\", count %llu", (unsigned long long)rd64(0x508));

    /* The fd's own position is still 0. */
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x610; ctx.gpr[5] = 2; ctx.gpr[6] = 0x510;
    cellFsRead(&ctx);
    CHECK(rd64(0x510) == 2 && memcmp(mem + 0x610, "01", 2) == 0, "a plain cellFsRead after it starts at 0");

    /* Write at an offset. */
    memcpy(mem + 0x620, "WXYZ", 4);
    rc = io(wr_off, fd, 4, 0x620, 4, 0x518);
    CHECK(rc == 0 && rd64(0x518) == 4, "WriteWithOffset(4, \"WXYZ\") returns CELL_OK, count %llu",
          (unsigned long long)rd64(0x518));

    /* The plain read continues from 2, and sees the write. */
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x630; ctx.gpr[5] = 4; ctx.gpr[6] = 0x520;
    cellFsRead(&ctx);
    CHECK(rd64(0x520) == 4 && memcmp(mem + 0x630, "23WX", 4) == 0,
          "a plain cellFsRead continues at 2 and sees the write (\"%.4s\")", (const char*)mem + 0x630);

    /* Past the end: no data, still CELL_OK. */
    memset(mem + 0x508, 0xEE, 8);
    rc = io(rd_off, fd, 100, 0x640, 4, 0x508);
    CHECK(rc == 0 && rd64(0x508) == 0, "ReadWithOffset past the end returns CELL_OK, count %llu",
          (unsigned long long)rd64(0x508));

    /* A bad fd. */
    rc = io(rd_off, 1000, 0, 0x640, 4, 0x508);
    CHECK(rc == (uint32_t)CELL_FS_EBADF, "ReadWithOffset on a bad fd returns 0x%08X (want CELL_FS_EBADF)", rc);
    rc = io(wr_off, 1000, 0, 0x640, 4, 0x508);
    CHECK(rc == (uint32_t)CELL_FS_EBADF, "WriteWithOffset on a bad fd returns 0x%08X (want CELL_FS_EBADF)", rc);

    /* A write on an fd opened read-only fails in stdio: CELL_FS_EIO with the count 0, not CELL_OK and a short
     * count the title cannot tell from success (ps3_fs_write_failed). */
    {
        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = CELL_FS_O_RDONLY; ctx.gpr[5] = 0x530;
        cellFsOpen(&ctx);
        const uint32_t rfd = rd32(0x530);
        CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsOpen(O_RDONLY) returns CELL_OK (fd %u)", rfd);
        memset(mem + 0x538, 0xEE, 8);
        rc = io(wr_off, rfd, 0, 0x620, 4, 0x538);
        CHECK(rc == (uint32_t)CELL_FS_EIO && rd64(0x538) == 0,
              "WriteWithOffset on a read-only fd returns 0x%08X, count %llu (want CELL_FS_EIO, 0)", rc,
              (unsigned long long)rd64(0x538));
        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = rfd;
        cellFsClose(&ctx);
    }

#ifndef _WIN32
    /* A span that leaves the arena is a named stop before the file or the count is touched, never a clamped
     * transfer of the part that fits: the buffer's whole span, and the count's 8 bytes. */
    {
        const struct { const char* name; uint64_t buf; uint64_t size; uint32_t n_ptr; const char* arg; } cases[] = {
            {"a buffer straddling the arena's end", sizeof mem - 2, 4, 0x508, "buf"},
            {"a buffer at the arena's end", sizeof mem, 4, 0x508, "buf"},
            {"a size that wraps the span", 0x600, ~0ull, 0x508, "buf"},
            {"a count straddling the arena's end", 0x600, 4, (uint32_t)sizeof mem - 4, "n"},
        };
        for (const auto& c : cases) {
            for (int op = 0; op < 2; ++op) {
                const char* fn = op == 0 ? "cellFsReadWithOffset" : "cellFsWriteWithOffset";
                int fds[2];
                CHECK(pipe(fds) == 0, "pipe for %s", c.name);
                const pid_t pid = fork();
                if (pid == 0) {
                    close(fds[0]); dup2(fds[1], STDERR_FILENO); close(fds[1]);
                    io(op == 0 ? rd_off : wr_off, fd, 4, (uint32_t)c.buf, c.size, c.n_ptr);
                    _exit(1);   /* returned: nothing was refused */
                }
                close(fds[1]);
                std::string err;
                char bytes[512];
                ssize_t got;
                while ((got = read(fds[0], bytes, sizeof bytes)) > 0) err.append(bytes, (size_t)got);
                close(fds[0]);
                int status = 0;
                waitpid(pid, &status, 0);
                CHECK(WIFEXITED(status) && WEXITSTATUS(status) == PS3_STOP_EXIT_CODE,
                      "%s with %s stops instead of transferring (status 0x%X)", fn, c.name, status);
                CHECK(err.find("kind=hle-guest-range") != std::string::npos &&
                      err.find(std::string("func=") + fn) != std::string::npos &&
                      err.find(std::string("arg=") + c.arg) != std::string::npos,
                      "%s names itself and its %s in the stop (got: %s)", fn, c.arg, err.c_str());
            }
        }
    }
#endif

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd;
    cellFsClose(&ctx);

    /* And the write reached the file. */
    {
        char got[17] = {0};
        FILE* f = fopen(host, "rb");
        const int ok = f && fread(got, 1, 16, f) == 16;
        if (f) fclose(f);
        CHECK(ok && memcmp(got, "0123WXYZ89ABCDEF", 16) == 0, "the file holds \"%s\"", got);
    }
    remove(host);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
