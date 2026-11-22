/*
 * ps3recomp - cellFs (ppu_fs.cpp) and sys_fs (runtime/syscalls/sys_fs.c) share one fd space.
 *
 * On hardware cellFsOpen / cellFsOpendir and the raw sys_fs_* calls draw from one kernel fd space and any call takes
 * any fd. This fixture links BOTH real tables and checks, with no title code or data:
 *   1. a file opened with cellFsOpen is read, seeked, fstat'ed, written, truncated and closed through sys_fs_*, and
 *      the other way round (a sys_fs_open fd through cellFsRead / Lseek / Fstat / Write / Ftruncate / Close);
 *   2. a directory opened with cellFsOpendir is read and closed through sys_fs_readdir / closedir, and a
 *      sys_fs_opendir directory through cellFsReaddir / cellFsGetDirectoryEntries / cellFsClosedir;
 *   3. no number is handed out twice, files and directories alike, whichever family opens them, and a closed
 *      number is free to both again;
 *   4. the same under concurrency: threads open and close through both families at once, and each number held is
 *      held by one thread. This is the case to run under TSan: cellFsOpen against sys_fs_open raced on the two
 *      tables' ownership checks before one lock guarded the space.
 *   5. cellFsRead / cellFsWrite answer in RPCS3's order (a null buffer is EFAULT before the fd is looked at, a null
 *      count pointer is a temporary, a count span outside guest memory is a stop before the file moves).
 *
 *   tools/test_ppu_fs_sys_bridge.py [--tsan]
 *   test_fs_sys_bridge [DIR]   (DIR: where the files go; default the temp dir)
 */
#include "ppu_fs.cpp"
#include "ps3emu/stop.h"
#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- link stubs: nothing on the paths this test drives calls them ---- */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ydkj_host_bt(const char* tag) { (void)tag; }
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
/* sys_fs.c's weak reference to cellGame.c; Mach-O will not link an undefined one. No HDD game here. */
const char* cellGame_get_hdd_title_id(void) { return nullptr; }
/* The runtime's stop for an HLE handler handed a guest span outside guest memory: nothing here passes one. */
PS3_NORETURN void ps3_stop_guest_range(const char* func, const char* arg,
                                       uint64_t ea, uint64_t size, uint64_t limit)
{
    fprintf(stderr, "kind=hle-guest-range func=%s arg=%s ea=0x%llX size=0x%llX limit=0x%llX\n",
            func, arg, (unsigned long long)ea, (unsigned long long)size, (unsigned long long)limit);
    fflush(stderr);
    _Exit(PS3_STOP_EXIT_CODE);
}
/* sys_fs_read/write's stop for a guest span the host could not commit: nothing here passes one. */
PS3_NORETURN void ps3_stop_commit_failed(const char* func, const char* arg, uint64_t ea, uint64_t size, int32_t rc)
{
    fprintf(stderr, "kind=hle-commit-failed func=%s arg=%s ea=0x%llX size=0x%llX rc=%d\n",
            func, arg, (unsigned long long)ea, (unsigned long long)size, (int)rc);
    fflush(stderr);
    _Exit(PS3_STOP_EXIT_CODE);
}
/* ps3_log.c references the stop runtime and the quick-exit table; this fixture configures no log and
 * takes no stop beyond the guest-range one above. */
void ps3_stop_set_mirror(ps3_stop_quiesce_fn, ps3_stop_mirror_fn) {}
/* The generic stop (host-path-limit): the same machine-line idea as the guest-range one, the kind as its number. */
PS3_NORETURN void ps3_stop(ps3_stop_kind kind, const char*, const char*, const ps3_stop_field* fields,
                           size_t nfields, const ps3_stop_site*)
{
    fprintf(stderr, "kind=%d", (int)kind);
    for (size_t i = 0; i < nfields; i++) fprintf(stderr, " %s=%s", fields[i].key, fields[i].value);
    fputc('\n', stderr);
    fflush(stderr);
    _Exit(PS3_STOP_EXIT_CODE);
}
int ps3_host_at_quick_exit(void (*fn)(void)) { (void)fn; return 0; }

/* The sys_fs side, from sys_fs.c (linked). */
int64_t sys_fs_open(ppu_context*);
int64_t sys_fs_read(ppu_context*);
int64_t sys_fs_write(ppu_context*);
int64_t sys_fs_close(ppu_context*);
int64_t sys_fs_lseek(ppu_context*);
int64_t sys_fs_fstat(ppu_context*);
int64_t sys_fs_opendir(ppu_context*);
int64_t sys_fs_readdir(ppu_context*);
int64_t sys_fs_closedir(ppu_context*);
int64_t sys_fs_ftruncate(ppu_context*);
int64_t sys_fs_unlink(ppu_context*);
extern char g_sys_fs_root[512];
}
/* ps3_dev_flash_path (runtime/platform), which both tables call for /dev_flash: the real one, it needs nothing. */
#include "../../platform/ps3_dev_flash.c"
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }

static std::atomic<int> fails{0};
#define CHECK(c, ...) do { const bool ok_ = (c); \
        if (!ok_) { printf("[FAIL] "); printf(__VA_ARGS__); printf("\n"); ++fails; } \
        else if (verbose) { printf("[PASS] "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static bool verbose = true;

static const uint32_t PAT = 256;                /* the pattern file: byte i holds i */
static const uint32_t MEM_BYTES = 1u << 20;
static std::string g_dir;

/* Guest scratch, one block per thread: path, out values, a data buffer, a dirent, a stat. */
struct Scratch {
    uint32_t base;
    uint32_t path() const { return base; }
    uint32_t out() const { return base + 0x200; }
    uint32_t buf() const { return base + 0x400; }
    uint32_t dirent() const { return base + 0x800; }
    uint32_t sb() const { return base + 0xA00; }
};
static void put(uint32_t a, const char* s) { strcpy((char*)vm_base + a, s); }

/* A cellFs handler, or a sys_fs syscall, with up to four arguments; the return code. */
static uint32_t cfs(void (*f)(ppu_context*), uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d;
    f(&ctx);
    return (uint32_t)ctx.gpr[3];
}
static uint32_t sfs(int64_t (*f)(ppu_context*), uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d;
    return (uint32_t)f(&ctx);
}
static const uint32_t EBADF_ = (uint32_t)CELL_FS_EBADF;   /* 0x8001002A, both families' code */

static int cfs_open(const Scratch& s, const char* name, uint32_t flags)
{
    char g[256]; snprintf(g, sizeof g, "/dev_hdd1/%s", name); put(s.path(), g);
    return cfs(cellFsOpen, s.path(), flags, s.out()) == 0 ? (int)rd32(s.out()) : -1;
}
static int sys_open(const Scratch& s, const char* name, uint32_t flags)
{
    char g[256]; snprintf(g, sizeof g, "/dev_bdvd/%s", name); put(s.path(), g);
    return sfs(sys_fs_open, s.path(), flags, s.out(), 0644) == 0 ? (int)rd32(s.out()) : -1;
}
static int cfs_opendir(const Scratch& s, const char* name)
{
    char g[256]; snprintf(g, sizeof g, "/dev_hdd1/%s", name); put(s.path(), g);
    return cfs(cellFsOpendir, s.path(), s.out()) == 0 ? (int)rd32(s.out()) : -1;
}
static int sys_opendir(const Scratch& s, const char* name)
{
    char g[256]; snprintf(g, sizeof g, "/dev_bdvd/%s", name); put(s.path(), g);
    return sfs(sys_fs_opendir, s.path(), s.out()) == 0 ? (int)rd32(s.out()) : -1;
}

/* Every byte i of the pattern file holds i & 0xFF: a buffer read from `off` is right iff byte i holds off + i. */
static bool holds(uint32_t buf, uint64_t off, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) if (vm_base[buf + i] != (uint8_t)(off + i)) return false;
    return true;
}
static std::string host(const char* name) { return g_dir + "/" + name; }
static void make_pattern(const char* name)
{
    FILE* f = fopen(host(name).c_str(), "wb");
    for (uint32_t i = 0; f && i < PAT; i++) fputc((int)(i & 0xFF), f);
    if (f) fclose(f);
}
static long host_size(const char* name)
{
    struct stat st;
    return stat(host(name).c_str(), &st) == 0 ? (long)st.st_size : -1;
}
static std::string host_bytes(const char* name, long off, size_t n)
{
    std::string out(n, '\0');
    FILE* f = fopen(host(name).c_str(), "rb");
    if (!f) return std::string();
    const bool ok = fseek(f, off, SEEK_SET) == 0 && fread(&out[0], 1, n, f) == n;
    fclose(f);
    return ok ? out : std::string();
}

#ifndef _WIN32
/* Run `body` in a forked child and report whether it ended in a stop (the fixture's stops exit with
 * PS3_STOP_EXIT_CODE) and what it wrote to stderr. */
struct ChildResult { bool stopped; std::string err; };
template <class F> static ChildResult run_child(F body)
{
    int fds[2];
    if (pipe(fds) != 0) return {false, {}};
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]); dup2(fds[1], STDERR_FILENO); close(fds[1]);
        body();
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
    return {WIFEXITED(status) && WEXITSTATUS(status) == PS3_STOP_EXIT_CODE, err};
}
static bool says(const ChildResult& r, const char* text) { return r.err.find(text) != std::string::npos; }
#endif

/* A file opened with cellFsOpen, used through sys_fs_*. */
static void test_cellfs_fd_through_sys_fs()
{
    Scratch s{0x1000};
    make_pattern("bridge_a.bin");
    const int fd = cfs_open(s, "bridge_a.bin", CELL_FS_O_RDWR);
    CHECK(fd >= 3, "cellFsOpen returns an fd (%d)", fd);
    CHECK(sfs(sys_fs_read, fd, s.buf(), 16, s.out() + 8) == 0 && rd64(s.out() + 8) == 16 && holds(s.buf(), 0, 16),
          "sys_fs_read reads a cellFs fd");
    CHECK(sfs(sys_fs_lseek, fd, 100, CELL_FS_SEEK_SET, s.out() + 16) == 0 && rd64(s.out() + 16) == 100,
          "sys_fs_lseek moves a cellFs fd");
    CHECK(cfs(cellFsRead, fd, s.buf(), 8, s.out() + 8) == 0 && rd64(s.out() + 8) == 8 && holds(s.buf(), 100, 8),
          "cellFsRead continues from where sys_fs_lseek left the one stream");
    CHECK(sfs(sys_fs_fstat, fd, s.sb()) == 0 && rd64(s.sb() + 0x24) == PAT, "sys_fs_fstat sees the size");
    CHECK(sfs(sys_fs_lseek, fd, 0, CELL_FS_SEEK_SET, s.out() + 16) == 0, "rewind");
    memcpy(vm_base + s.buf(), "ZZZZ", 4);
    CHECK(sfs(sys_fs_write, fd, s.buf(), 4, s.out() + 8) == 0 && rd64(s.out() + 8) == 4 &&
          host_bytes("bridge_a.bin", 0, 4) == "ZZZZ", "sys_fs_write reaches the host file");
    CHECK(sfs(sys_fs_ftruncate, fd, 64) == 0 && host_size("bridge_a.bin") == 64, "sys_fs_ftruncate shortens it");
    CHECK(sfs(sys_fs_close, fd) == 0, "sys_fs_close closes a cellFs fd");
    CHECK(cfs(cellFsRead, fd, s.buf(), 4, s.out() + 8) == EBADF_, "the cellFs table no longer holds it");
    CHECK(sfs(sys_fs_close, fd) == EBADF_, "a second close is EBADF");
    remove(host("bridge_a.bin").c_str());
}

/* A file opened with sys_fs_open, used through cellFs. */
static void test_sys_fs_fd_through_cellfs()
{
    Scratch s{0x1000};
    make_pattern("bridge_a.bin");
    const int fd = sys_open(s, "bridge_a.bin", CELL_FS_O_RDWR);
    CHECK(fd >= 3, "sys_fs_open returns an fd (%d)", fd);
    CHECK(cfs(cellFsRead, fd, s.buf(), 8, s.out() + 8) == 0 && rd64(s.out() + 8) == 8 && holds(s.buf(), 0, 8),
          "cellFsRead reads a sys_fs fd");
    CHECK(cfs(cellFsLseek, fd, 40, CELL_FS_SEEK_SET, s.out() + 16) == 0 && rd64(s.out() + 16) == 40,
          "cellFsLseek moves a sys_fs fd");
    CHECK(sfs(sys_fs_read, fd, s.buf(), 4, s.out() + 8) == 0 && holds(s.buf(), 40, 4),
          "sys_fs_read continues from where cellFsLseek left the one stream");
    CHECK(cfs(cellFsFstat, fd, s.sb()) == 0 && rd64(s.sb() + 0x24) == PAT, "cellFsFstat sees the size");
    memcpy(vm_base + s.buf(), "QQQQ", 4);
    CHECK(cfs(cellFsWrite, fd, s.buf(), 4, s.out() + 8) == 0 && rd64(s.out() + 8) == 4 &&
          host_bytes("bridge_a.bin", 44, 4) == "QQQQ", "cellFsWrite reaches the host file at the position");
    CHECK(cfs(cellFsFtruncate, fd, 100) == 0 && host_size("bridge_a.bin") == 100, "cellFsFtruncate shortens it");
    CHECK(cfs(cellFsClose, fd) == 0, "cellFsClose closes a sys_fs fd");
    CHECK(sfs(sys_fs_read, fd, s.buf(), 4, s.out() + 8) == EBADF_, "the sys_fs table no longer holds it");
    remove(host("bridge_a.bin").c_str());
}

/* Directory entries (name, type byte) read through `next` until the end. */
template <class F> static std::set<std::string> list_dir(const Scratch& s, F next, std::vector<int>* types = nullptr)
{
    std::set<std::string> names;
    for (int i = 0; i < 16; i++) {
        memset(vm_base + s.dirent(), 0, 0x102);
        if (next() != 0 || rd64(s.out() + 8) == 0) break;
        names.insert((const char*)vm_base + s.dirent() + 2);
        if (types) types->push_back(vm_base[s.dirent()]);
    }
    return names;
}

/* The access mode of a file is the flags it was opened with, whichever family opens it and whichever reads or writes
 * it (RPCS3 6e4b784d1 sys_fs.cpp:1498-1502 and :1593-1597; cellFsRead / cellFsWrite wrap them, cellFs.cpp:57-69): a
 * read on a write-only file and a write on a read-only file are EBADF with the count 0, where the permitted ones
 * transfer, and a zero-byte call is not refused. All four open/use pairings: the cellFs table's own fds, the
 * sys_fs table's own fds, and the two bridged ways. */
static void test_access_modes()
{
    Scratch s{0x1000};
    make_pattern("bridge_a.bin");
    const struct { const char* name; uint32_t flags; bool can_read, can_write; } modes[] = {
        {"read-only", CELL_FS_O_RDONLY, true, false},
        {"write-only", CELL_FS_O_WRONLY, false, true},
        {"read-write", CELL_FS_O_RDWR, true, true},
    };
    for (int opened_by_cellfs = 0; opened_by_cellfs < 2; ++opened_by_cellfs) {
        for (const auto& m : modes) {
            const int fd = opened_by_cellfs ? cfs_open(s, "bridge_a.bin", m.flags) : sys_open(s, "bridge_a.bin", m.flags);
            CHECK(fd >= 3, "%s open of a %s file returns an fd (%d)", opened_by_cellfs ? "cellFs" : "sys_fs", m.name, fd);
            for (int used_by_cellfs = 0; used_by_cellfs < 2; ++used_by_cellfs) {
                const char* how = opened_by_cellfs ? (used_by_cellfs ? "cellFs fd through cellFs" : "cellFs fd through sys_fs")
                                                   : (used_by_cellfs ? "sys_fs fd through cellFs" : "sys_fs fd through sys_fs");
                /* read 4 bytes from offset 4 */
                sfs(sys_fs_lseek, fd, 4, CELL_FS_SEEK_SET, s.out() + 16);
                memset(vm_base + s.out() + 8, 0xEE, 8);
                uint32_t rc = used_by_cellfs ? cfs(cellFsRead, fd, s.buf(), 4, s.out() + 8)
                                             : sfs(sys_fs_read, fd, s.buf(), 4, s.out() + 8);
                if (m.can_read)
                    CHECK(rc == 0 && rd64(s.out() + 8) == 4 && holds(s.buf(), 4, 4), "read on a %s %s", m.name, how);
                else
                    CHECK(rc == EBADF_ && rd64(s.out() + 8) == 0, "read on a %s %s is EBADF, count 0 (0x%08X, %llu)",
                          m.name, how, rc, (unsigned long long)rd64(s.out() + 8));
                /* write the same four bytes back at offset 4, so the file is unchanged */
                for (int i = 0; i < 4; i++) vm_base[s.buf() + i] = (uint8_t)(4 + i);
                sfs(sys_fs_lseek, fd, 4, CELL_FS_SEEK_SET, s.out() + 16);
                memset(vm_base + s.out() + 8, 0xEE, 8);
                rc = used_by_cellfs ? cfs(cellFsWrite, fd, s.buf(), 4, s.out() + 8)
                                    : sfs(sys_fs_write, fd, s.buf(), 4, s.out() + 8);
                if (m.can_write)
                    CHECK(rc == 0 && rd64(s.out() + 8) == 4, "write on a %s %s", m.name, how);
                else
                    CHECK(rc == EBADF_ && rd64(s.out() + 8) == 0, "write on a %s %s is EBADF, count 0 (0x%08X, %llu)",
                          m.name, how, rc, (unsigned long long)rd64(s.out() + 8));
                /* nothing to transfer: not refused on any mode */
                memset(vm_base + s.out() + 8, 0xEE, 8);
                rc = used_by_cellfs ? cfs(cellFsRead, fd, s.buf(), 0, s.out() + 8) : sfs(sys_fs_read, fd, s.buf(), 0, s.out() + 8);
                CHECK(rc == 0 && rd64(s.out() + 8) == 0, "a zero-byte read on a %s %s is CELL_OK", m.name, how);
                memset(vm_base + s.out() + 8, 0xEE, 8);
                rc = used_by_cellfs ? cfs(cellFsWrite, fd, s.buf(), 0, s.out() + 8) : sfs(sys_fs_write, fd, s.buf(), 0, s.out() + 8);
                CHECK(rc == 0 && rd64(s.out() + 8) == 0, "a zero-byte write on a %s %s is CELL_OK", m.name, how);
            }
            CHECK((opened_by_cellfs ? sfs(sys_fs_close, fd) : cfs(cellFsClose, fd)) == 0, "closed");
        }
    }
    CHECK(host_size("bridge_a.bin") == (long)PAT, "the file was not changed");
    remove(host("bridge_a.bin").c_str());
}

/* cellFsRead / cellFsWrite answer in sys_fs_read / sys_fs_write's order (RPCS3 6e4b784d1 sys_fs.cpp:1504-1520 and
 * :1585-1601, behind the wrapper of cellFs.cpp:57-69): a null buffer is EFAULT with the count 0 before the fd or the
 * access mode is looked at, then a bad fd or a refused mode is EBADF with the count 0. The wrapper hands the syscall
 * a temporary when the count pointer is null, so that is no error and nothing is stored (the syscalls themselves
 * answer EFAULT to a null pointer, and are not asked here). A count span outside guest memory is a stop before the
 * file moves. Fork issue #154, item 3: a write with a null buffer and a null count pointer stored to page 0. */
static void test_read_write_order()
{
    Scratch s{0x1000};
    make_pattern("bridge_a.bin");
    const int rofd = cfs_open(s, "bridge_a.bin", CELL_FS_O_RDONLY);
    const int wofd = cfs_open(s, "bridge_a.bin", CELL_FS_O_WRONLY);
    const int rwfd = cfs_open(s, "bridge_a.bin", CELL_FS_O_RDWR);
    CHECK(rofd >= 3 && wofd >= 3 && rwfd >= 3, "three fds open (%d %d %d)", rofd, wofd, rwfd);
    const uint32_t EFAULT_ = (uint32_t)CELL_EFAULT;
    const uint32_t cnt = s.out() + 8;
    auto arm = [&] { memset(vm_base + cnt, 0xEE, 8); memset(vm_base, 0xEE, 16); };   /* the count, and guest page 0 */
    auto page0_kept = [&] { for (int i = 0; i < 16; i++) if (vm_base[i] != 0xEE) return false; return true; };
    auto read_via = [&](int cellfs, int fd, uint32_t buf, uint64_t n, uint32_t cp) {
        return cellfs ? cfs(cellFsRead, fd, buf, n, cp) : sfs(sys_fs_read, fd, buf, n, cp);
    };
    auto write_via = [&](int cellfs, int fd, uint32_t buf, uint64_t n, uint32_t cp) {
        return cellfs ? cfs(cellFsWrite, fd, buf, n, cp) : sfs(sys_fs_write, fd, buf, n, cp);
    };
    for (int cellfs = 1; cellfs >= 0; --cellfs) {
        const char* via = cellfs ? "cellFs" : "sys_fs";
        arm();
        uint32_t rc = read_via(cellfs, wofd, 0, 4, cnt);
        CHECK(rc == EFAULT_ && rd64(cnt) == 0, "%s read on a write-only fd with a null buffer is EFAULT, count 0 (0x%08X)", via, rc);
        arm();
        rc = write_via(cellfs, rofd, 0, 4, cnt);
        CHECK(rc == EFAULT_ && rd64(cnt) == 0, "%s write on a read-only fd with a null buffer is EFAULT, count 0 (0x%08X)", via, rc);
        arm();
        rc = read_via(cellfs, 1000, 0, 4, cnt);
        CHECK(rc == EFAULT_ && rd64(cnt) == 0, "%s read on a bad fd with a null buffer is EFAULT, count 0 (0x%08X)", via, rc);
        arm();
        rc = write_via(cellfs, 1000, 0, 4, cnt);
        CHECK(rc == EFAULT_ && rd64(cnt) == 0, "%s write on a bad fd with a null buffer is EFAULT, count 0 (0x%08X)", via, rc);
    }

    /* A null count pointer: the wrapper's temporary, so the call is judged on its other arguments alone, and no store
     * reaches page 0 (it used to: fs_range_ok(0, 8) holds). */
    arm();
    uint32_t rc = cfs(cellFsRead, rwfd, 0, 4, 0);
    CHECK(rc == EFAULT_ && page0_kept(), "cellFsRead with a null buffer and a null count pointer is EFAULT, page 0 untouched (0x%08X)", rc);
    arm();
    rc = cfs(cellFsWrite, rwfd, 0, 4, 0);
    CHECK(rc == EFAULT_ && page0_kept(), "cellFsWrite with a null buffer and a null count pointer is EFAULT, page 0 untouched (0x%08X)", rc);
    arm();
    rc = cfs(cellFsRead, 1000, s.buf(), 4, 0);
    CHECK(rc == EBADF_ && page0_kept(), "cellFsRead on a bad fd with a null count pointer is EBADF, page 0 untouched (0x%08X)", rc);
    arm();
    rc = cfs(cellFsWrite, rofd, s.buf(), 4, 0);
    CHECK(rc == EBADF_ && page0_kept(), "cellFsWrite on a read-only fd with a null count pointer is EBADF, page 0 untouched (0x%08X)", rc);
    /* ... and a call that succeeds moves the file: the 4 bytes at 8, then the position is 12. */
    sfs(sys_fs_lseek, rwfd, 8, CELL_FS_SEEK_SET, s.out() + 16);
    memset(vm_base + s.buf(), 0, 4);
    rc = cfs(cellFsRead, rwfd, s.buf(), 4, 0);
    CHECK(rc == 0 && holds(s.buf(), 8, 4), "cellFsRead with a null count pointer reads (0x%08X)", rc);
    CHECK(sfs(sys_fs_lseek, rwfd, 0, CELL_FS_SEEK_CUR, s.out() + 16) == 0 && rd64(s.out() + 16) == 12,
          "... and the position moved");
    sfs(sys_fs_lseek, rwfd, 8, CELL_FS_SEEK_SET, s.out() + 16);
    memcpy(vm_base + s.buf(), "WXYZ", 4);   /* bytes the file does not hold at 8, so the check sees the write */
    rc = cfs(cellFsWrite, rwfd, s.buf(), 4, 0);
    CHECK(rc == 0 && host_bytes("bridge_a.bin", 8, 4) == "WXYZ",
          "cellFsWrite with a null count pointer writes (0x%08X)", rc);
    CHECK(sfs(sys_fs_lseek, rwfd, 0, CELL_FS_SEEK_CUR, s.out() + 16) == 0 && rd64(s.out() + 16) == 12,
          "... and the position moved");

    /* A count span outside guest memory is a stop before the file moves: the write below would have changed the
     * file's first four bytes, and the host stream is flushed by the handler, so the file shows it. */
    sfs(sys_fs_lseek, rwfd, 0, CELL_FS_SEEK_SET, s.out() + 16);
    memcpy(vm_base + s.buf(), "ZZZZ", 4);
    const struct { const char* name; const char* arg; bool write; } outside[] = {
        {"cellFsRead", "nread", false},
        {"cellFsWrite", "nwrite", true},
    };
    for (const auto& o : outside) {
        const auto r = run_child([&] {
            if (o.write) cfs(cellFsWrite, rwfd, s.buf(), 4, MEM_BYTES - 4);
            else cfs(cellFsRead, rwfd, s.buf(), 4, MEM_BYTES - 4);
        });
        CHECK(r.stopped && says(r, "kind=hle-guest-range") && says(r, o.name) && says(r, o.arg),
              "%s with a count span outside guest memory stops, arg %s (stopped %d: %s)", o.name, o.arg, (int)r.stopped,
              r.err.c_str());
    }
    CHECK(host_bytes("bridge_a.bin", 0, 4) == std::string("\x00\x01\x02\x03", 4), "... and the file did not move");

    cfs(cellFsClose, rofd); cfs(cellFsClose, wofd); cfs(cellFsClose, rwfd);
    remove(host("bridge_a.bin").c_str());
}

/* "/dev_bdvd/" + "./" repeated + a name of 'n's: a guest path `total` characters long (1024 or 1025). The path names
 * the file "nn" (1024) or "nnn" (1025), and its first 1023 characters name "n". */
static std::string long_guest_path(size_t total)
{
    const size_t name = total % 2 == 0 ? 2 : 3;
    std::string p = "/dev_bdvd/";
    for (size_t pairs = (total - 10 - name) / 2; pairs; --pairs) p += "./";
    return p + std::string(name, 'n');
}
static uint32_t child_rc(const ChildResult& r)
{
    const size_t at = r.err.find("rc=");
    unsigned v = 0xFFFFFFFFu;
    if (at != std::string::npos) sscanf(r.err.c_str() + at + 3, "%x", &v);
    return v;
}

/* The unlinks' guest path limit and their host buffers. A guest path is up to 1024 characters (RPCS3 6e4b784d1
 * translate_to_str, sys_fs.cpp:1107-1130) and the host path is a root in front of it: the unlinks translate into a
 * host-sized buffer (PS3_FS_HOST_PATH_MAX), so a legal path under a long root is not a host-path-limit stop, while a
 * host path past that buffer still is. Both families, cellFsUnlink and sys_fs_unlink (which had no fixture). */
static void test_unlink_limits()
{
    const uint32_t PATH = 0x40000;
    for (const char* name : {"n", "nn", "nnn"}) {
        FILE* f = fopen(host(name).c_str(), "wb");
        if (f) { fputs("x", f); fclose(f); }
    }
    /* 1025 characters: ENAMETOOLONG, and nothing is unlinked (the 1023-character prefix names "n"). */
    put(PATH, long_guest_path(1025).c_str());
    for (int sys = 0; sys < 2; ++sys) {
        const uint32_t rc = sys ? sfs(sys_fs_unlink, PATH) : cfs(cellFsUnlink, PATH);
        CHECK(rc == (uint32_t)CELL_ENAMETOOLONG, "%s: a 1025-character path is ENAMETOOLONG (0x%08X)",
              sys ? "sys_fs_unlink" : "cellFsUnlink", rc);
        CHECK(host_size("n") == 1 && host_size("nn") == 1 && host_size("nnn") == 1, "%s: ... and no file went",
              sys ? "sys_fs_unlink" : "cellFsUnlink");
    }

#ifndef _WIN32
    /* 1024 characters under a root of 500 (sys_fs) or 1500 (cellFs) characters is a host path past the 1024 and 1100
     * bytes the buffers used to be: it reaches the host, which has no such file: ENOENT, not a stop. */
    put(PATH, long_guest_path(1024).c_str());
    for (int sys = 0; sys < 2; ++sys) {
        const std::string root(sys ? 500 : 1500, 'r');
        const auto r = run_child([&] {
            uint32_t rc;
            if (sys) { snprintf(g_sys_fs_root, sizeof g_sys_fs_root, "%s", root.c_str()); rc = sfs(sys_fs_unlink, PATH); }
            else { ppu_vfs_root = root.c_str(); rc = cfs(cellFsUnlink, PATH); }
            fprintf(stderr, "rc=%08X\n", rc);
        });
        CHECK(!r.stopped && child_rc(r) == (uint32_t)CELL_ENOENT, "%s: a 1024-character path under a %zu-character root is not a stop (rc 0x%08X, stopped %d: %s)",
              sys ? "sys_fs_unlink" : "cellFsUnlink", root.size(), child_rc(r), (int)r.stopped, r.err.c_str());
    }

    /* A host path past the host buffer is still the named stop, never a cut path: the mount root is 4100 characters
     * (PS3_HDD0_ROOT for sys_fs, set in main; ppu_vfs_root for cellFs). */
    put(PATH, "/dev_hdd0/f");
    auto r = run_child([&] { sfs(sys_fs_unlink, PATH); });
    const std::string kind = "kind=" + std::to_string((int)PS3_STOP_HOST_PATH_LIMIT);
    CHECK(r.stopped && says(r, kind.c_str()) && says(r, "func=sys_fs_translate_path"),
          "sys_fs_unlink: a host path past its buffer is host-path-limit (stopped %d: %s)", (int)r.stopped, r.err.c_str());
    put(PATH, "/dev_bdvd/f");
    const std::string big(4100, 'v');
    r = run_child([&] { ppu_vfs_root = big.c_str(); cfs(cellFsUnlink, PATH); });
    CHECK(r.stopped && says(r, kind.c_str()) && says(r, "func=host_path"),
          "cellFsUnlink: a host path past its buffer is host-path-limit (stopped %d: %s)", (int)r.stopped, r.err.c_str());
#endif
    for (const char* name : {"n", "nn", "nnn"}) remove(host(name).c_str());
}

static void test_directories()
{
    Scratch s{0x1000};
    mkdir(host("bridge_dir").c_str(), 0755);
    FILE* f = fopen(host("bridge_dir/e1").c_str(), "wb"); if (f) { fputs("x", f); fclose(f); }
    f = fopen(host("bridge_dir/e2").c_str(), "wb"); if (f) { fputs("y", f); fclose(f); }
    const std::set<std::string> want = {".", "..", "e1", "e2"};

    /* cellFsOpendir, read and closed through sys_fs. */
    const int cd = cfs_opendir(s, "bridge_dir");
    CHECK(cd >= 3, "cellFsOpendir returns an fd (%d)", cd);
    std::vector<int> types;
    const auto a = list_dir(s, [&] { return sfs(sys_fs_readdir, cd, s.dirent(), s.out() + 8); }, &types);
    CHECK(a == want, "sys_fs_readdir lists a cellFs directory");
    bool typed = types.size() == 4;
    int dirs = 0; for (int t : types) dirs += t == CELL_FS_TYPE_DIR;
    CHECK(typed && dirs == 2, "its entries are typed (\".\" and \"..\" directories, e1 and e2 files)");
    CHECK(sfs(sys_fs_closedir, cd) == 0, "sys_fs_closedir closes a cellFs directory");
    CHECK(cfs(cellFsReaddir, cd, s.dirent(), s.out() + 8) == EBADF_, "the cellFs table no longer holds it");
    CHECK(sfs(sys_fs_closedir, cd) == EBADF_, "a second closedir is EBADF");

    /* sys_fs_opendir, read and closed through cellFs. */
    const int sd = sys_opendir(s, "bridge_dir");
    CHECK(sd >= 3, "sys_fs_opendir returns an fd (%d)", sd);
    types.clear();
    const auto b = list_dir(s, [&] { return cfs(cellFsReaddir, sd, s.dirent(), s.out() + 8); }, &types);
    CHECK(b == want, "cellFsReaddir lists a sys_fs directory");
    dirs = 0; for (int t : types) dirs += t == CELL_FS_TYPE_DIR;
    CHECK(types.size() == 4 && dirs == 2, "its entries are typed");
    CHECK(cfs(cellFsClosedir, sd) == 0, "cellFsClosedir closes a sys_fs directory");
    CHECK(sfs(sys_fs_readdir, sd, s.dirent(), s.out() + 8) == EBADF_, "the sys_fs table no longer holds it");

    /* cellFsGetDirectoryEntries on a sys_fs directory: 312-byte entries, a count of whole ones. */
    const int sd2 = sys_opendir(s, "bridge_dir");
    const uint32_t entries = 0x80000, count_ptr = s.out() + 16;
    CHECK(cfs(cellFsGetDirectoryEntries, sd2, entries, 312 * 8, count_ptr) == 0 && rd32(count_ptr) == 4,
          "cellFsGetDirectoryEntries reads a sys_fs directory (%u entries)", rd32(count_ptr));
    std::set<std::string> got;
    for (uint32_t i = 0; i < rd32(count_ptr) && i < 8; i++) got.insert((const char*)vm_base + entries + i * 312 + 54);
    CHECK(got == want, "its entries carry the names");
    CHECK(cfs(cellFsClosedir, sd2) == 0, "closed");

    remove(host("bridge_dir/e1").c_str());
    remove(host("bridge_dir/e2").c_str());
    rmdir(host("bridge_dir").c_str());
}

/* Neither family hands out a number the other holds, files and directories alike. */
static void test_distinct_numbers()
{
    Scratch s{0x1000};
    make_pattern("bridge_a.bin");
    mkdir(host("bridge_dir").c_str(), 0755);
    struct Held { int fd; bool by_cellfs; bool dir; };
    std::vector<Held> held;
    for (int i = 0; i < 20; i++) {
        held.push_back({cfs_open(s, "bridge_a.bin", CELL_FS_O_RDONLY), true, false});
        held.push_back({sys_open(s, "bridge_a.bin", CELL_FS_O_RDONLY), false, false});
    }
    for (int i = 0; i < 5; i++) {
        held.push_back({cfs_opendir(s, "bridge_dir"), true, true});
        held.push_back({sys_opendir(s, "bridge_dir"), false, true});
    }
    std::set<int> seen;
    bool all_ok = true;
    for (const Held& h : held) { all_ok = all_ok && h.fd >= 3; seen.insert(h.fd); }
    CHECK(all_ok, "every open succeeded");
    CHECK(seen.size() == held.size(), "%zu opens through both families hold %zu distinct numbers", held.size(),
          seen.size());
    /* Each is closed through the OTHER family. */
    bool closed = true;
    for (const Held& h : held) {
        const uint32_t rc = h.dir ? (h.by_cellfs ? sfs(sys_fs_closedir, h.fd) : cfs(cellFsClosedir, h.fd))
                                  : (h.by_cellfs ? sfs(sys_fs_close, h.fd) : cfs(cellFsClose, h.fd));
        closed = closed && rc == 0;
    }
    CHECK(closed, "each closes through the other family");
    CHECK(cfs_open(s, "bridge_a.bin", CELL_FS_O_RDONLY) == 3, "with everything closed the lowest number is free again");
    cfs(cellFsClose, 3);
    remove(host("bridge_a.bin").c_str());
    rmdir(host("bridge_dir").c_str());
}

/* Threads open and close through both families at once; a number held is held by one thread. */
static std::atomic<int> g_held[1024];
static std::atomic<int> g_dups{0}, g_bad{0};
static void claim(int fd) { if (fd < 0 || fd >= 1024 || g_held[fd].exchange(1)) ++g_dups; }
static void release(int fd) { if (fd >= 0 && fd < 1024) g_held[fd].store(0); }

static void worker(int t)
{
    Scratch s{0x40000u + 0x1000u * (uint32_t)t};
    for (int it = 0; it < 300; it++) {
        const int a = cfs_open(s, "bridge_b.bin", CELL_FS_O_RDONLY);
        const int b = sys_open(s, "bridge_b.bin", CELL_FS_O_RDONLY);
        claim(a); claim(b);
        /* Each is read through the other family's call. */
        if (sfs(sys_fs_read, a, s.buf(), 8, s.out() + 8) != 0 || !holds(s.buf(), 0, 8)) ++g_bad;
        if (cfs(cellFsRead, b, s.buf(), 8, s.out() + 8) != 0 || !holds(s.buf(), 0, 8)) ++g_bad;
        int da = -1, db = -1;
        if ((it & 3) == 0) {
            da = cfs_opendir(s, "bridge_dir"); db = sys_opendir(s, "bridge_dir");
            claim(da); claim(db);
        }
        /* The number is cleared before the close: once closed, another thread may be handed it. */
        release(a); release(b);
        if (sfs(sys_fs_close, a) != 0) ++g_bad;
        if (cfs(cellFsClose, b) != 0) ++g_bad;
        if (da >= 0 || db >= 0) {
            release(da); release(db);
            if (sfs(sys_fs_closedir, da) != 0) ++g_bad;
            if (cfs(cellFsClosedir, db) != 0) ++g_bad;
        }
    }
}

static void test_concurrent()
{
    make_pattern("bridge_b.bin");
    mkdir(host("bridge_dir").c_str(), 0755);
    const int THREADS = 8;
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; t++) ts.emplace_back(worker, t);
    for (auto& th : ts) th.join();
    CHECK(g_dups == 0, "%d concurrent open/close rounds on %d threads handed out a number twice %d times",
          THREADS * 300, THREADS, (int)g_dups);
    CHECK(g_bad == 0, "and %d cross-family reads or closes failed", (int)g_bad);
    remove(host("bridge_b.bin").c_str());
    rmdir(host("bridge_dir").c_str());
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    uint8_t* mem = (uint8_t*)mmap(nullptr, MEM_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) { printf("FAIL: could not map the guest arena\n"); return 1; }
    vm_base = mem; ppu_vm_size = MEM_BYTES;

    const char* dir = argc > 1 ? argv[1] : getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    g_dir = dir;
    ppu_vfs_root = dir;
    snprintf(g_sys_fs_root, sizeof g_sys_fs_root, "%s", dir);
    /* sys_fs_translate_path reads PS3_HDD0_ROOT once, at its first call, so the over-long mount root of
     * test_unlink_limits is set before any test runs. Nothing else here opens under /dev_hdd0. */
    setenv("PS3_HDD0_ROOT", std::string(4100, 'h').c_str(), 1);

    test_cellfs_fd_through_sys_fs();
    test_sys_fs_fd_through_cellfs();
    test_access_modes();
    test_read_write_order();
    test_unlink_limits();
    test_directories();
    test_distinct_numbers();
    verbose = false;   /* the concurrent rounds report failures only */
    test_concurrent();
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", (int)fails);
    return fails ? 1 : 0;
}
