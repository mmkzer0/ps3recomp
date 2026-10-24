/*
 * ps3recomp - cellFsAioRead on a shared fd (ppu_fs.cpp).
 *
 * An AIO read names its own offset. It must not move the fd's position for
 * the plain cellFsRead that follows, and several guest threads (FIOS media
 * threads, audio streamers) issue them on one fd at once. The fixture writes
 * a file whose every 4-byte word holds its own offset, then checks both:
 * a cellFsRead after an AIO read continues from the fd's position, and
 * reads racing from four threads each get the words they asked for.
 *
 *   tools/test_ppu_fs_aio.py
 *   test_fs_aio [DIR]     (DIR: where the file goes; default the temp dir)
 */
#include "ppu_fs.cpp"
#include "ps3emu/stop.h"
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- link stubs: nothing on the paths this test drives calls them ---- */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
void ydkj_host_bt(const char* tag) { (void)tag; }
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
/* The runtime's stop for an HLE handler handed a guest span outside guest
 * memory. The fixture has no stop machinery, so it prints the same machine line
 * and ends the process the same way: a refusal must be distinguishable from the
 * handler writing past the arena and faulting. */
PS3_NORETURN void ps3_stop_guest_range(const char* func, const char* arg,
                                       uint64_t ea, uint64_t size, uint64_t limit)
{
    fprintf(stderr, "kind=hle-guest-range func=%s arg=%s ea=0x%llX size=0x%llX limit=0x%llX\n",
            func, arg, (unsigned long long)ea, (unsigned long long)size, (unsigned long long)limit);
    fflush(stderr);
    _Exit(PS3_STOP_EXIT_CODE);
}
}
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }
static void wr32(uint32_t a, uint32_t v) { vm_write32(a, v); }
static void wr64(uint32_t a, uint64_t v) { vm_write64(a, v); }

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

static const uint32_t WORDS = 1u << 16;   /* 256 KiB file */
static const uint32_t MEM_BYTES = 1u << 20;

/* The guest arena, with one PROT_NONE page immediately after it: a handler that
 * writes past ppu_vm_size faults in the guard page instead of corrupting some
 * other static (which is what cellFsRead did for a buffer past the arena). */
static uint8_t* arena(size_t bytes)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t size = (bytes + page - 1) / page * page;
    void* p = mmap(nullptr, size + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    if (mprotect((uint8_t*)p + size, page, PROT_NONE) != 0) return nullptr;
    return (uint8_t*)p;
}

/* One AIO read of `len` bytes at `off` into guest `buf`, through a CellFsAio
 * at guest `aio`: {fd u32, pad, offset u64, buf u32, pad, size u64}. */
static void aio_read(uint32_t aio, uint32_t fd, uint64_t off, uint32_t buf, uint64_t len)
{
    wr32(aio + 0x00, fd); wr64(aio + 0x08, off); wr32(aio + 0x10, buf); wr64(aio + 0x18, len);
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = aio; ctx.gpr[4] = 0; ctx.gpr[5] = 0;
    cellFsAioRead(&ctx);
}

/* Every word at file offset o holds o (big-endian), so a buffer read from
 * offset `off` is right iff word i holds off + 4*i. */
static bool holds(uint32_t buf, uint64_t off, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4) if (rd32(buf + i) != (uint32_t)(off + i)) return false;
    return true;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    uint8_t* mem = arena(MEM_BYTES);
    if (!mem) { printf("FAIL: could not map the guest arena\n"); return 1; }
    vm_base = mem; ppu_vm_size = MEM_BYTES;   /* the whole mapped arena, no more */

    const char* dir = argc > 1 ? argv[1] : getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    char host[1100];
    snprintf(host, sizeof host, "%s/ps3recomp_fs_aio.bin", dir);
    {
        FILE* f = fopen(host, "wb");
        if (!f) { printf("FAIL: could not create %s\n", host); return 1; }
        for (uint32_t i = 0; i < WORDS; i++) { uint32_t be = __builtin_bswap32(i * 4); fwrite(&be, 4, 1, f); }
        fclose(f);
    }

    ppu_vfs_root = dir;
    strcpy((char*)mem + 0x100, "/dev_hdd1/ps3recomp_fs_aio.bin");
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = 0 /* O_RDONLY */; ctx.gpr[5] = 0x200;
    cellFsOpen(&ctx);
    const uint32_t fd = rd32(0x200);
    CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsOpen returns CELL_OK (fd %u)", fd);
    if ((uint32_t)ctx.gpr[3] != 0) { remove(host); return 1; }

    /* 1. The fd's position survives an AIO read. */
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x40; ctx.gpr[5] = 0 /* SET */; ctx.gpr[6] = 0x208;
    cellFsLseek(&ctx);
    aio_read(0x300, fd, 0x8000, 0x1000, 0x100);
    CHECK(holds(0x1000, 0x8000, 0x100), "the AIO read returns the words at its own offset");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x2000; ctx.gpr[5] = 16; ctx.gpr[6] = 0x210;
    cellFsRead(&ctx);
    CHECK(rd64(0x210) == 16 && holds(0x2000, 0x40, 16),
          "cellFsRead after it continues from the fd's position 0x40 (got word 0x%08X)", rd32(0x2000));

    /* 2. AIO reads racing on the same fd each get their own bytes. */
    const int THREADS = 4, ROUNDS = 2000;
    const uint32_t LEN = 0x400;
    std::vector<int> bad(THREADS, 0);
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; t++)
        ts.emplace_back([&, t] {
            const uint32_t aio = 0x400 + 0x40 * t, buf = 0x10000 + 0x10000 * t;
            uint32_t seed = 0x9E3779B9u * (t + 1);
            for (int r = 0; r < ROUNDS; r++) {
                seed = seed * 1664525u + 1013904223u;
                const uint64_t off = (uint64_t)(seed % (WORDS * 4 - LEN)) & ~3ull;
                aio_read(aio, fd, off, buf, LEN);
                if (!holds(buf, off, LEN)) bad[t]++;
            }
        });
    for (auto& th : ts) th.join();
    int total = 0; for (int b : bad) total += b;
    CHECK(total == 0, "%d racing AIO reads over %d threads, %d returned the wrong bytes",
          THREADS * ROUNDS, THREADS, total);

    /* 3. A buffer that leaves the arena is refused before anything is copied,
     *    on every cellFs path that copies through one. `ppu_vm_size - buf`
     *    underflowed for a buf past the arena, so the old read code clamped to
     *    ~2^64 bytes and fread wrote into the guard page; a buf that merely
     *    ended the arena was silently truncated to a zero-byte read (EOF); and
     *    cellFsWrite had no check at all. A straddling buffer is the same case:
     *    the part that fits is not read or written either. */
    {
        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = 1 /* O_WRONLY */; ctx.gpr[5] = 0x204;
        cellFsOpen(&ctx);
        const uint32_t fdw = rd32(0x204);
        CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsOpen(O_WRONLY) returns CELL_OK (fd %u)", fdw);

        const struct { const char* name; uint32_t buf; } cases[] = {
            {"a buffer past the arena", MEM_BYTES + 0x1000},
            {"a buffer at the arena's end", MEM_BYTES},
            {"a buffer straddling the arena's end", MEM_BYTES - 0x10},
        };
        for (const auto& c : cases) {
            for (int op = 0; op < 3; ++op) {
                int fds[2];
                CHECK(pipe(fds) == 0, "pipe for %s", c.name);
                const pid_t pid = fork();
                if (pid == 0) {
                    close(fds[0]); dup2(fds[1], STDERR_FILENO); close(fds[1]);
                    ppu_context cc;
                    memset(&cc, 0, sizeof cc);
                    if (op == 0) {           /* cellFsRead */
                        cc.gpr[3] = fd; cc.gpr[4] = c.buf; cc.gpr[5] = 0x1000; cc.gpr[6] = 0x210;
                        cellFsRead(&cc);
                    } else if (op == 1) {    /* cellFsWrite */
                        cc.gpr[3] = fdw; cc.gpr[4] = c.buf; cc.gpr[5] = 0x1000; cc.gpr[6] = 0x210;
                        cellFsWrite(&cc);
                    } else {                 /* cellFsAioRead */
                        aio_read(0x300, fd, 0, c.buf, 0x1000);
                    }
                    _exit(1);   /* returned: nothing was refused */
                }
                close(fds[1]);
                std::string err;
                char bytes[512];
                ssize_t n;
                while ((n = read(fds[0], bytes, sizeof bytes)) > 0) err.append(bytes, (size_t)n);
                close(fds[0]);
                int status = 0;
                waitpid(pid, &status, 0);
                const char* fn = op == 0 ? "cellFsRead" : op == 1 ? "cellFsWrite" : "cellFsAioRead";
                CHECK(WIFEXITED(status) && WEXITSTATUS(status) == PS3_STOP_EXIT_CODE,
                      "%s with %s stops instead of copying through it (status 0x%X)", fn, c.name, status);
                CHECK(err.find("kind=hle-guest-range") != std::string::npos &&
                      err.find(std::string("func=") + fn) != std::string::npos &&
                      err.find("arg=buf") != std::string::npos,
                      "%s names itself and the buffer in the stop (got: %s)", fn, err.c_str());
            }
        }
        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fdw;
        cellFsClose(&ctx);
    }

    /* 4. The null buffer is the one bad pointer the firmware itself answers:
     *    CELL_EFAULT, and the byte count is left at 0 (oracle RPCS3
     *    sys_fs_read/sys_fs_write). It is not a stop. */
    {
        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0; ctx.gpr[5] = 0x100; ctx.gpr[6] = 0x210;
        wr64(0x210, 0xDEADBEEF);
        cellFsRead(&ctx);
        CHECK((uint32_t)ctx.gpr[3] == 0x8001000Du && rd64(0x210) == 0,
              "cellFsRead(null buffer) returns CELL_EFAULT and sets the byte count to 0");
        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x100; ctx.gpr[5] = 4; ctx.gpr[6] = 0;
        cellFsRead(&ctx);
        CHECK((uint32_t)ctx.gpr[3] == 0x8001000Du,
              "cellFsRead(null byte-count pointer) returns CELL_EFAULT");
    }

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd;
    cellFsClose(&ctx);
    remove(host);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
