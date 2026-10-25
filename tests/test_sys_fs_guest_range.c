/*
 * ps3recomp - sys_fs_read/sys_fs_write bound the guest spans they dereference.
 *
 * RPCS3's lv2/sys_fs.cpp (sys_fs_read/sys_fs_write) answers, in this order:
 *   1. a null nread/nwrite is CELL_EFAULT;
 *   2. a null buffer writes 0 to that out-pointer and returns CELL_EFAULT;
 *   3. an fd that is not a file, or an access-mode mismatch (a read of more
 *      than zero bytes from O_WRONLY, a write of more than zero bytes to
 *      O_RDONLY), writes 0 and returns CELL_EBADF;
 *   4. a zero-byte call is CELL_OK with 0 reported.
 * Here the host kernel touches the buffer directly, so two range checks are
 * inserted around that order:
 *   - the out-pointer's 8-byte span is checked right after its null test and
 *     before the oracle's first write of 0, so that store is safe;
 *   - the buffer span is checked after the fd/mode checks and before the
 *     commit and the fread/fwrite, and a span that leaves guest memory stops
 *     (hle-guest-range);
 *   - a size whose low 32 bits are in range while its 64-bit span is not stops
 *     (the old code committed (uint32_t)size and let the bound inherit the
 *     truncation).
 *
 * The stop ends the process (PS3_STOP_EXIT_CODE), so every bad span runs in a
 * forked child; the parent checks the exit status and the machine line. The
 * arena has a PROT_NONE guard page after it, so a check that misses faults
 * instead of passing silently.
 *
 *   tests/test_sys_fs_guest_range.c, linked with runtime/syscalls/sys_fs.c and
 *   the real stop implementation (runtime/diag/stop.cpp).
 */
#include "sys_fs.h"
#include "ps3emu/stop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* The runtime globals sys_fs.c reads (runtime/memory/vm.h). */
uint8_t* vm_base;
uint32_t ppu_vm_size;

/* sys_fs.c holds a weak reference (Mach-O needs a definition, not a
 * weak_import declaration); the HDD-game path is not under test. */
const char* cellGame_get_hdd_title_id(void) { return NULL; }

static const uint32_t ARENA = 0x20000;
static int fails;

#define CHECK(c, ...) do { const int ok_ = (c); \
                           printf(ok_ ? "[PASS] " : "[FAIL] "); \
                           printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

/* Guest memory is big-endian; sys_fs.c stores through write_be32/64. */
static uint32_t be32_at(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64_at(uint32_t ea)
{
    return ((uint64_t)be32_at(ea) << 32) | be32_at(ea + 4);
}

static void put_be64(uint32_t ea, uint64_t v)
{
    for (int i = 0; i < 8; i++) vm_base[ea + i] = (uint8_t)(v >> (56 - i * 8));
}

static void arg(ppu_context* ctx, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->gpr[3] = a0; ctx->gpr[4] = a1; ctx->gpr[5] = a2; ctx->gpr[6] = a3;
}

static int32_t fs_open(const char* name, int32_t flags)
{
    ppu_context ctx;
    strncpy((char*)vm_base + 0x80, name, 0x60);
    arg(&ctx, 0x80, (uint64_t)(uint32_t)flags, 0x90, 0);
    if ((int32_t)sys_fs_open(&ctx) != CELL_OK) return -1;
    return (int32_t)be32_at(0x90);
}

static void fs_close(int32_t fd)
{
    ppu_context ctx;
    arg(&ctx, (uint64_t)(uint32_t)fd, 0, 0, 0);
    sys_fs_close(&ctx);
}

static int64_t call_read(int32_t fd, uint32_t buf, uint64_t size, uint32_t nread)
{
    ppu_context ctx;
    arg(&ctx, (uint64_t)(uint32_t)fd, buf, size, nread);
    return sys_fs_read(&ctx);
}

static int64_t call_write(int32_t fd, uint32_t buf, uint64_t size, uint32_t nwritten)
{
    ppu_context ctx;
    arg(&ctx, (uint64_t)(uint32_t)fd, buf, size, nwritten);
    return sys_fs_write(&ctx);
}

/* One refused call: the child makes it, the parent requires the stop. */
typedef void (*bad_call)(void);

static void expect_stop(bad_call fn, const char* func, const char* argname)
{
    int fds[2];
    if (pipe(fds) != 0) { CHECK(0, "pipe"); return; }
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        fn();
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
          "%s(%s) stops (status 0x%X)", func, argname, status);
    char want[64];
    snprintf(want, sizeof want, "func=%s arg=%s", func, argname);
    const char* line = strstr(out, "PS3STOP v=1");
    CHECK(line && strstr(line, "kind=hle-guest-range") && strstr(line, want),
          "%s(%s) names itself in the stop machine line%s%s", func, argname,
          line ? ": " : " (no machine line)", line ? line : "");
}

static void read_crossing_arena(void)
{
    const int32_t fd = fs_open("in.bin", CELL_FS_O_RDONLY);
    if (fd < 0) _exit(2);
    call_read(fd, ARENA - 4, 8, 0x3000);
}

static void read_size_not_fitting_u32(void)
{
    const int32_t fd = fs_open("in.bin", CELL_FS_O_RDONLY);
    if (fd < 0) _exit(2);
    /* (uint32_t)size is 8, which would pass the old truncated commit/check;
     * the 64-bit span is 4 GiB past the arena. */
    call_read(fd, 0x1000, 0x100000008ull, 0x3000);
}

static void read_nread_crossing_arena(void)
{
    const int32_t fd = fs_open("in.bin", CELL_FS_O_RDONLY);
    if (fd < 0) _exit(2);
    call_read(fd, 0x1000, 4, ARENA - 4);
}

static void write_crossing_arena(void)
{
    const int32_t fd = fs_open("out.bin",
                               CELL_FS_O_CREAT | CELL_FS_O_TRUNC | CELL_FS_O_RDWR);
    if (fd < 0) _exit(2);
    call_write(fd, ARENA - 4, 8, 0x3000);
}

static void write_nwrite_crossing_arena(void)
{
    const int32_t fd = fs_open("out.bin",
                               CELL_FS_O_CREAT | CELL_FS_O_TRUNC | CELL_FS_O_RDWR);
    if (fd < 0) _exit(2);
    call_write(fd, 0x2000, 4, ARENA - 4);
}

/* The out-pointer span is bounded before the oracle's null-buffer write of 0:
 * the null buffer must not shelter a wild out-pointer (fd 3 need not exist at
 * all, since the null-nread/nwrite check and its span check come first). */
static void read_null_buf_crossing_nread(void)
{
    call_read(3, 0, 4, ARENA - 4);
}

static void write_null_buf_crossing_nwrite(void)
{
    call_write(3, 0, 4, ARENA - 4);
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

    char root[] = "/tmp/ps3recomp-sysfs-range-XXXXXX";
    if (!mkdtemp(root)) { printf("FAIL: mkdtemp\n"); return 1; }
    snprintf(g_sys_fs_root, sizeof g_sys_fs_root, "%s", root);

    char in_path[512], out_path[512];
    snprintf(in_path, sizeof in_path, "%s/in.bin", root);
    snprintf(out_path, sizeof out_path, "%s/out.bin", root);
    FILE* h = fopen(in_path, "wb");
    if (!h || fwrite("ABCDEFGH", 1, 8, h) != 8) { printf("FAIL: in.bin\n"); return 1; }
    fclose(h);

    /* In-arena calls still work and report through the bounded out-pointer. */
    int32_t fd = fs_open("in.bin", CELL_FS_O_RDONLY);
    CHECK(fd == 3, "open returns fd 3 (got %d)", fd);
    put_be64(0x3000, 0xDEADBEEFCAFEF00Dull);
    int64_t rc = call_read(fd, 0x1000, 8, 0x3000);
    CHECK((int32_t)rc == CELL_OK, "an in-arena read returns CELL_OK (0x%llX)",
          (unsigned long long)rc);
    CHECK(be64_at(0x3000) == 8, "an in-arena read reports 8 bytes (got %llu)",
          (unsigned long long)be64_at(0x3000));
    CHECK(memcmp(vm_base + 0x1000, "ABCDEFGH", 8) == 0, "an in-arena read moves the bytes");

    /* RPCS3 sys_fs_read 1: !nread -> CELL_EFAULT, before the fd or the buffer
     * is examined (the second call proves the fd cannot pre-empt it). */
    rc = call_read(fd, 0x1000, 4, 0);
    CHECK((int32_t)rc == (int32_t)CELL_EFAULT,
          "read with a null nread is CELL_EFAULT (0x%llX)", (unsigned long long)rc);
    rc = call_read(3 + SYS_FS_FD_MAX, 0x1000, 4, 0);
    CHECK((int32_t)rc == (int32_t)CELL_EFAULT,
          "a null nread is CELL_EFAULT even for a bad fd (0x%llX)",
          (unsigned long long)rc);

    /* RPCS3 sys_fs_read 2: !buf -> *nread = 0, CELL_EFAULT. */
    put_be64(0x3000, 0xDEADBEEFCAFEF00Dull);
    rc = call_read(fd, 0, 8, 0x3000);
    CHECK((int32_t)rc == (int32_t)CELL_EFAULT,
          "read with a null buffer is CELL_EFAULT (0x%llX)", (unsigned long long)rc);
    CHECK(be64_at(0x3000) == 0, "a null read buffer zeroes *nread");
    fs_close(fd);

    fd = fs_open("out.bin", CELL_FS_O_CREAT | CELL_FS_O_TRUNC | CELL_FS_O_RDWR);
    CHECK(fd >= 3, "open for write returns a slot fd (got %d)", fd);
    memcpy(vm_base + 0x2000, "write!", 6);
    rc = call_write(fd, 0x2000, 6, 0x3008);
    CHECK((int32_t)rc == CELL_OK, "an in-arena write returns CELL_OK (0x%llX)",
          (unsigned long long)rc);
    CHECK(be64_at(0x3008) == 6, "an in-arena write reports 6 bytes");

    /* RPCS3 sys_fs_write 1 and 2, the same shape as read. */
    rc = call_write(fd, 0x2000, 4, 0);
    CHECK((int32_t)rc == (int32_t)CELL_EFAULT,
          "write with a null nwrite is CELL_EFAULT (0x%llX)", (unsigned long long)rc);
    rc = call_write(3 + SYS_FS_FD_MAX, 0x2000, 4, 0);
    CHECK((int32_t)rc == (int32_t)CELL_EFAULT,
          "a null nwrite is CELL_EFAULT even for a bad fd (0x%llX)",
          (unsigned long long)rc);
    put_be64(0x3008, 0x1122334455667788ull);
    rc = call_write(fd, 0, 4, 0x3008);
    CHECK((int32_t)rc == (int32_t)CELL_EFAULT,
          "write with a null buffer is CELL_EFAULT (0x%llX)", (unsigned long long)rc);
    CHECK(be64_at(0x3008) == 0, "a null write buffer zeroes *nwrite");
    fs_close(fd);
    h = fopen(out_path, "rb");
    char got[8] = { 0 };
    CHECK(h && fread(got, 1, 6, h) == 6 && memcmp(got, "write!", 6) == 0,
          "an in-arena write reaches the file");
    if (h) fclose(h);

    /* RPCS3 sys_fs_write 3: a bad fd reports 0, never the requested size. It
     * used to answer CELL_OK and write `size` here ("pretend write
     * succeeded"). The wild-buffer call proves the fd check precedes the
     * buffer span check. */
    const int32_t bad_fds[2] = { 3 + SYS_FS_FD_MAX, 3 + 200 };
    for (unsigned i = 0; i < 2; i++) {
        put_be64(0x3008, 0x1122334455667788ull);
        rc = call_write(bad_fds[i], 0x2000, 6, 0x3008);
        CHECK((int32_t)rc == (int32_t)CELL_EBADF,
              "write to fd %d is CELL_EBADF (0x%llX)",
              bad_fds[i], (unsigned long long)rc);
        CHECK(be64_at(0x3008) == 0, "write to fd %d reports 0 bytes", bad_fds[i]);
        rc = call_write(bad_fds[i], ARENA - 4, 8, 0x3008);
        CHECK((int32_t)rc == (int32_t)CELL_EBADF,
              "write to fd %d with a wild buffer is still CELL_EBADF", bad_fds[i]);
        CHECK(be64_at(0x3008) == 0,
              "write to fd %d with a wild buffer still reports 0 bytes", bad_fds[i]);
    }

    /* RPCS3 sys_fs_read 3: O_WRONLY with nbytes != 0 is CELL_EBADF and *nread
     * is 0; with nbytes == 0 the access-mode test is skipped (step 4). */
    fd = fs_open("in.bin", CELL_FS_O_WRONLY);
    CHECK(fd >= 3, "open O_WRONLY returns a slot fd (got %d)", fd);
    put_be64(0x3000, 0xDEADBEEFCAFEF00Dull);
    rc = call_read(fd, 0x1000, 4, 0x3000);
    CHECK((int32_t)rc == (int32_t)CELL_EBADF,
          "read of 4 bytes on an O_WRONLY fd is CELL_EBADF (0x%llX)",
          (unsigned long long)rc);
    CHECK(be64_at(0x3000) == 0, "the O_WRONLY read reports 0 bytes");
    rc = call_read(fd, 0x1000, 0, 0x3000);
    CHECK((int32_t)rc == CELL_OK,
          "a zero-byte read on an O_WRONLY fd is CELL_OK (0x%llX)",
          (unsigned long long)rc);
    CHECK(be64_at(0x3000) == 0, "the zero-byte read reports 0 bytes");
    fs_close(fd);

    /* RPCS3 sys_fs_write 3: O_RDONLY with nbytes != 0 is CELL_EBADF and
     * *nwrite is 0; zero bytes is CELL_OK. */
    fd = fs_open("in.bin", CELL_FS_O_RDONLY);
    CHECK(fd >= 3, "open O_RDONLY returns a slot fd (got %d)", fd);
    put_be64(0x3008, 0x1122334455667788ull);
    rc = call_write(fd, 0x1000, 4, 0x3008);
    CHECK((int32_t)rc == (int32_t)CELL_EBADF,
          "write of 4 bytes on an O_RDONLY fd is CELL_EBADF (0x%llX)",
          (unsigned long long)rc);
    CHECK(be64_at(0x3008) == 0, "the O_RDONLY write reports 0 bytes");
    rc = call_write(fd, 0x1000, 0, 0x3008);
    CHECK((int32_t)rc == CELL_OK,
          "a zero-byte write on an O_RDONLY fd is CELL_OK (0x%llX)",
          (unsigned long long)rc);
    CHECK(be64_at(0x3008) == 0, "the zero-byte write reports 0 bytes");
    fs_close(fd);

    /* Every span that leaves the arena is a named stop, raised before the
     * host touches it. The null-buffer cases prove the out-pointer span is
     * bounded before the oracle's first write of 0. */
    expect_stop(read_crossing_arena, "sys_fs_read", "buf");
    expect_stop(read_size_not_fitting_u32, "sys_fs_read", "buf");
    expect_stop(read_nread_crossing_arena, "sys_fs_read", "nread");
    expect_stop(read_null_buf_crossing_nread, "sys_fs_read", "nread");
    expect_stop(write_crossing_arena, "sys_fs_write", "buf");
    expect_stop(write_nwrite_crossing_arena, "sys_fs_write", "nwrite");
    expect_stop(write_null_buf_crossing_nwrite, "sys_fs_write", "nwrite");

    munmap(p, (size_t)ARENA + page);
    unlink(in_path);
    unlink(out_path);
    rmdir(root);
    printf("%s (%d failure%s)\n", fails ? "test_sys_fs_guest_range: FAIL"
                                        : "test_sys_fs_guest_range: OK",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
