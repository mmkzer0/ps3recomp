/*
 * ps3recomp - cellFs VFS (sys_fs HLE)
 *
 * Backs the game's file I/O with the real host filesystem so it can load its
 * data/config/assets. Guest PS3 paths (/dev_bdvd/..., /app_home/..., /dev_hdd0,
 * etc.) are translated to a host root (the game directory that contains
 * PS3_GAME), set by the boot harness from the EBOOT path (or $PS3_VFS_ROOT).
 *
 * Only the calls the boot actually imports are implemented:
 *   cellFsOpen/Close/Read/Write/Lseek/Stat/Fstat/Opendir/Readdir/Closedir/
 *   Mkdir/Rmdir/Unlink/Fsync.
 *
 * Context-aware HLE: guest pointers (path, buffers, out params) are read/written
 * through vm_base in big-endian.
 */
#define PPU_NO_INLINE_STORES   /* declares its own out-of-line vm_write32/64 below */
#include "../memory/vm_watch_hooks.h"
#include <errno.h>
#include "ppu_recomp.h"      /* ppu_context */
#include "../../libs/filesystem/edat.h"
#include "../memory/vm.h"    /* vm_range_ok, vm_guest_limit: the one guest-range rule */
#include "ps3emu/stop.h"     /* ps3_stop_guest_range */
#include "ps3emu/ps3_log.h"  /* PS3_LOG: tagged lines, stderr bytes unchanged */
#include "ps3emu/nid.h"      /* ps3_compute_nid */
#include "ps3emu/error_codes.h"
#include "ps3emu/guest_call.h" /* ps3_invoke_guest: AIO completion is a guest OPD */
#include "ps3emu/fs_write_check.h"
#include "ps3emu/fs_unlink.h"
#include "sdata_decrypt.h"   /* SDATA/EDAT (NPD) decryption for cellFsSdataOpen */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <mutex>
#include <atomic>
#include <sys/stat.h>
#include "../platform/win32_dirent.h"   /* <dirent.h>, or a Win32 stand-in */
#include "../platform/ps3_dev_flash.h"   /* ps3_dev_flash_path: PS3_DEV_FLASH may name several roots */

/* Guest-resolving host backtrace (ppu_loader.cpp). Must be declared at file scope:
 * `extern "C"` inside a function body is ill-formed in C++. */
extern "C" void ydkj_host_bt(const char* tag);
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>          /* open/close on MinGW */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>     /* SetEndOfFile: cellFsAllocateFileAreaWithoutZeroFill */
#else
#include <unistd.h>
#endif
#ifndef O_BINARY
#define O_BINARY 0       /* POSIX has no text/binary distinction */
#endif

/* 64-bit file offsets and sizes. Windows is LLP64: `long` is 32 bits, so
 * fseek(long)/ftell and `struct stat` (st_size is a 32-bit _off_t on MinGW, and
 * stat() is stat64i32) cannot express a file past 2 GiB. Disc archives are
 * routinely bigger: Lseek truncated the offset, Fstat's size came from ftell's
 * overflow result and Stat's from the 32-bit wrapper, and what those return
 * differs between CRTs: MinGW-w64's own stat() wrapper (msvcrt builds) fails
 * with EOVERFLOW, UCRT's _stat64i32 (llvm-mingw) returns the size truncated, so
 * the same title took different paths depending on the toolchain. POSIX LP64
 * is unaffected. */
#ifdef _WIN32
#  define FS_SEEK64(f, o, w) _fseeki64((f), (int64_t)(o), (w))
#  define FS_TELL64(f)       ((int64_t)_ftelli64(f))
typedef struct _stat64 fs_stat64_t;
/* _stat64 rejects a trailing slash (MinGW's stat wrapper stripped it). */
static int FS_STAT64(const char* p, fs_stat64_t* s)
{
    char t[1100]; size_t n = strlen(p);
    if (n >= sizeof t) return _stat64(p, s);
    memcpy(t, p, n + 1);
    while (n > 1 && (t[n - 1] == '/' || t[n - 1] == '\\') && t[n - 2] != ':') t[--n] = 0;
    return _stat64(t, s);
}
#else
#  define FS_SEEK64(f, o, w) fseeko((f), (off_t)(o), (w))
#  define FS_TELL64(f)       ((int64_t)ftello(f))
#  define FS_STAT64(p, s)    stat((p), (s))
typedef struct stat fs_stat64_t;
#endif

extern "C" uint8_t* vm_base;
extern "C" void ppu_guest_caller(char* out, size_t n);
extern "C" void     ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);
#include "../platform/win32_backtrace.h"   /* RtlCaptureStackBackTrace / GetModuleHandleA on POSIX */
#include "ps3emu/log_first.h"
#include "ps3emu/ps3_env_once.h"   /* ps3_env_once: each switch looked up once */

/* Host root that the PS3 mount points map into (dir containing PS3_GAME). */
extern "C" const char* ppu_vfs_root = ".";

/* CELL_FS return / mode / flag constants. (CELL_OK comes from
 * ps3emu/error_codes.h, which runtime/memory/vm.h includes.) */
#define CELL_FS_ENOENT       (-2147418106)   /* 0x80010006 -- was -2147418090, which is 0x80010016 = CELL_ENOTCONN, not ENOENT */
#define CELL_FS_EISDIR       (-2147418094)   /* 0x80010012 */
/* 0x8001002B. This was -2147418111 = 0x80010001, which is CELL_EAGAIN ("try
 * again"), not EIO: a title that retries on EAGAIN spun on a hard I/O failure. */
#define CELL_FS_EIO          (-2147418069)
#define CELL_FS_EBADF        (-2147418070)   /* 0x8001002A: not an open fd */
#define CELL_FS_EMFILE       (-2147418068)   /* 0x8001002C: fd table full */
#define CELL_FS_EINVAL       (-2147418110)   /* 0x80010002 */
#define CELL_FS_ENOSYS       (-2147418109)   /* 0x80010003 */
#define CELL_FS_S_IFDIR      0x4000u
#define CELL_FS_S_IFREG      0x8000u
#define CELL_FS_O_RDONLY     0
#define CELL_FS_O_WRONLY     1
#define CELL_FS_O_RDWR       2
/* Open flags: the PS3 SDK values (octal), byte for byte the set
 * libs/filesystem/cellFs.h declares. The hex guesses that used to be here
 * (O_CREAT 0x0200, O_TRUNC 0x0400, O_APPEND 0x0100) put the right numbers on
 * the wrong names -- 0x200 and 0x400 are TRUNC and APPEND, not CREAT and
 * TRUNC -- so a title opening a file with O_RDWR|O_CREAT (flags 0x42 =
 * 000002 | 000100) hit an unnamed bit and got a plain read-only open; the file
 * was never created. */
#define CELL_FS_O_CREAT      0x0040   /* 000100 */
#define CELL_FS_O_TRUNC      0x0200   /* 001000 */
#define CELL_FS_O_APPEND     0x0400   /* 002000 */
#define CELL_FS_O_EXCL       0x0080   /* 000200 */
#define CELL_FS_SEEK_SET     0
#define CELL_FS_SEEK_CUR     1
#define CELL_FS_SEEK_END     2
#define CELL_FS_TYPE_DIR     1
#define CELL_FS_TYPE_REG     2

/* ---- guest memory string helpers ---- */

/* Is [ea, ea+size) inside guest memory? runtime/memory/vm.h owns the rule; this
 * refuses a 64-bit EA above the 32-bit guest space that a narrowing cast would
 * wrap back into it. */
static inline bool fs_range_ok(uint64_t ea, uint64_t size)
{
    return ea <= UINT32_MAX && vm_range_ok((uint32_t)ea, size);
}

/* The span a handler was given leaves guest memory: the firmware would have
 * handed the buffer to the kernel, so there is no error code that is faithful,
 * and truncating it would read a different file than the caller asked for. */
[[noreturn]] static void fs_range_stop(const char* api, const char* arg, uint64_t ea, uint64_t size)
{
    ps3_stop_guest_range(api, arg, ea, size, vm_guest_limit());
}

static void guest_strcpy(char* dst, uint32_t gaddr, size_t cap)
{
    size_t i = 0;
    for (; i < cap - 1; i++) {
        /* Stop at the end of guest memory: a path with no NUL before it is not a
         * guest string, and the prefix written so far would name another file. */
        if (!fs_range_ok((uint64_t)gaddr + i, 1)) fs_range_stop("cellFs", "path", (uint64_t)gaddr + i, 1);
        char c = (char)vm_base[gaddr + i];
        if (!c) break;
        dst[i] = c;
    }
    dst[i] = 0;
}

/* Defined in runtime/syscalls/sys_fs.c -- shared by all three path
 * translators. (extern "C" is ill-formed at block scope.) */
extern "C" void ps3_vfs_ps3game_fallback(char* path, size_t cap);

/* A host path that does not fit its buffer is a different path: unlink, open and mkdir would act on the cut one.
 * There is no truncation that is safe, so it is the named stop (host-path-limit), as cellGame.c makes of its own.
 * Every handler builds its host path through host_path below, so every handler is covered. */
[[noreturn]] static void host_path_stop(int need, size_t cap)
{
    char need_s[16], limit_s[16];
    snprintf(need_s, sizeof need_s, "%d", need);
    snprintf(limit_s, sizeof limit_s, "%zu", cap - 1);
    const ps3_stop_field f[] = { {"func", "host_path"}, {"need", need_s}, {"limit", limit_s} };
    ps3_stop(PS3_STOP_HOST_PATH_LIMIT, "a cellFs host path does not fit its buffer",
             "shorten the VFS root or the guest path", f, 3, nullptr);
}
/* `need` is what snprintf wanted: the path fits iff it is below the buffer size. */
static inline void host_path_fit(int need, size_t cap)
{
    if (need < 0 || (size_t)need >= cap) host_path_stop(need, cap);
}

/* RPCS3's CELL_FS_MAX_FS_PATH_LENGTH: the longest guest path, in characters. */
#define FS_PATH_LENGTH_MAX 1024

/* Translate a guest path to a host path under ppu_vfs_root. Known PS3 mount
 * prefixes are stripped; the rest is appended to the root. */
static void host_path(char* out, size_t cap, const char* guest)
{
    if (guest && (strcmp(guest, "/dev/null") == 0 || strcmp(guest, "dev/null") == 0)) {
#ifdef _WIN32
        snprintf(out, cap, "NUL");
#else
        snprintf(out, cap, "/dev/null");
#endif
        return;
    }

    const char* rel = guest;
    /* /dev_hdd0 overlays the installed game-update dir (a disc title patched to
     * e.g. v1.30 runs the update's EBOOT and reads its patchN.farc from
     * /dev_hdd0/game/<title>/), mirroring the PS3/RPCS3 layout: base data on
     * /dev_bdvd (disc), update data on /dev_hdd0. Set PS3_HDD0_ROOT to the host
     * dir that /dev_hdd0 maps into (the one containing game/<title>/). */
    static const char* hdd0_root = nullptr; static int hdd0_init = 0;
    if (!hdd0_init) { hdd0_root = ps3_env_once("PS3_HDD0_ROOT"); hdd0_init = 1; }
    if (hdd0_root && strncmp(guest, "/dev_hdd0/", 10) == 0) {
        host_path_fit(snprintf(out, cap, "%s/%s", hdd0_root, guest + 10), cap);
        for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
        return;
    }

    /* /dev_hdd1 is the CACHE partition (cellSysCacheMount mounts it), and it
     * must be WRITABLE and separate from the disc mount: without its own root
     * the mount table below resolves /dev_hdd1 to <ppu_vfs_root>/cache/..., i.e.
     * into the read-only disc tree, so a title's first cache open fails and it
     * never creates its media threads. PS3_HDD1_ROOT selects the host dir, the
     * same convention as PS3_HDD0_ROOT. */
    static const char* hdd1_root = nullptr; static int hdd1_init = 0;
    if (!hdd1_init) { hdd1_root = ps3_env_once("PS3_HDD1_ROOT"); hdd1_init = 1; }
    if (hdd1_root && *hdd1_root && strncmp(guest, "/dev_hdd1/", 10) == 0) {
        host_path_fit(snprintf(out, cap, "%s/%s", hdd1_root, guest + 10), cap);
        for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
        return;
    }

    /* /dev_flash is FIRMWARE, not game data — mapping it into the game root (the
     * old behaviour) makes every firmware lookup miss. YDKJ's FMOD asks for
     * /dev_flash/sys/external/flashMP3.pic and, on a miss, prints "mp3 failed to
     * load MP3 codec (are you using the correct flash?)" and then crashes.
     * Serve it from a real dev_flash tree instead ($PS3_DEV_FLASH, else RPCS3's). */
    if (strncmp(guest, "/dev_flash/", 11) == 0) {
        const char* fw = ps3_env_once("PS3_DEV_FLASH");
        if (!fw || !*fw) fw = "D:/recomp/tools/rpcs3/dev_flash";
        ps3_dev_flash_path(out, cap, fw, guest + 11);
        /* The helper formats the path itself and does not say how long it wanted: a result that fills the buffer
         * may be cut, so it is refused too (one character short of the limit, never a different path). */
        host_path_fit((int)strlen(out) + 1, cap);
        for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
        return;
    }

    static const char* mounts[] = {
        "/dev_bdvd/", "/app_home/", "/dev_hdd0/", "/dev_hdd1/",
        "/dev_flash/", "/host_root/", "/dev_usb000/", "/dev_usb/"
    };
    for (size_t i = 0; i < sizeof(mounts)/sizeof(mounts[0]); i++) {
        size_t n = strlen(mounts[i]);
        if (strncmp(guest, mounts[i], n) == 0) { rel = guest + n; break; }
        /* Every mount above ends in '/', so a BARE device root never matched
         * its own mount: "/dev_bdvd" is not a prefix-match for "/dev_bdvd/".
         * It fell through to the strip-leading-slash case and resolved to
         * <root>/dev_bdvd, which does not exist -- so a title that opendirs or
         * stats the device to check the medium is present reads it as "no
         * disc". YDKJ does exactly that, 27 times a boot, and consequently
         * loaded none of its Scaleform UI or FMOD banks: no logos, no legal
         * screen, just a clear colour.
         *
         * libs/filesystem/cellFs.c already carries this fix (Tokyo Jungle hit
         * the same wall and retried forever); ppu_fs is the other half of the
         * split filesystem and never got it. Map the bare root to the mount
         * directory itself. */
        if (n > 1 && strncmp(guest, mounts[i], n - 1) == 0 && guest[n - 1] == '\0') {
            rel = guest + n - 1;   /* points at the NUL -> <root>/ */
            break;
        }
    }
    if (rel == guest && guest[0] == '/') rel = guest + 1;   /* strip leading '/' */
    host_path_fit(snprintf(out, cap, "%s/%s", ppu_vfs_root, rel), cap);
    for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
    ps3_vfs_ps3game_fallback(out, cap);
}

/* ---- fd / dir handle tables ---- */
#define FS_MAX 256
static FILE* g_files[FS_MAX];
static uint8_t g_fd_usm[FS_MAX];
/* The open flags each file was opened with: the access mode cellFsRead / cellFsWrite / the WithOffset pair check
 * (RPCS3's lv2_file::flags), and what sys_fs_read / sys_fs_write read for a file opened here. Written before the
 * file's slot is published (release) and read after it is loaded (acquire), so a reader sees its file's flags; the
 * accesses are atomic because a number closed and reopened by another thread is written while a stale reader may
 * still load it. */
#ifndef CELL_FS_O_ACCMODE
#  define CELL_FS_O_ACCMODE  3
#endif
static uint32_t g_fd_flags[FS_MAX];
static char    g_fd_path[FS_MAX][192];   /* guest path per fd (diagnostics) */   /* 1 if this fd is an open .usm movie (read tracker) */
static DIR*  g_dirs[FS_MAX];
/* Last transfer direction on each fd's stream: 0 none, 1 read, 2 write.
 *
 * C allows a read after a write on one stream only once it has been flushed or
 * repositioned, and a write after a read only once it has been repositioned;
 * otherwise the result is undefined. cellFs has no such rule, so a title may
 * write and then read on one fd with nothing in between. FIOS does exactly
 * that with its HDD cache: it writes a 512-byte header into a new cache.idx
 * and reads on. On the MSVC CRT that read returned 3584 bytes of stale buffer
 * and the header never reached the file (size still 0), so the index never
 * validated and FIOS rebuilt it for good -- ICO stayed on a black screen.
 * fs_set_dir() repositions in place (SEEK_CUR 0) when the direction changes. */
static uint8_t g_fd_dir[FS_MAX];
static void fs_set_dir(int fd, FILE* f, uint8_t dir)
{
    if (g_fd_dir[fd] && g_fd_dir[fd] != dir) FS_SEEK64(f, 0, SEEK_CUR);
    g_fd_dir[fd] = dir;
}
static char  g_dir_path[FS_MAX][1024];   /* host path per open dir (for readdir stat) */

/* Guest threads open, read and close concurrently (FIOS media threads, audio
 * streamers). Allocation and release take the fd-space lock, so two opens can
 * no longer claim the same slot, and every handler reads its slot once
 * (fd_file/fd_dir) rather than re-reading a pointer another thread's close may
 * have cleared.
 *
 * runtime/syscalls/sys_fs.c keeps its own descriptor table, but on hardware
 * cellFs and the raw sys_fs_* calls share ONE kernel fd space (the contract is
 * in sys_fs.h): any call takes any fd, and a number is held by exactly one
 * table. The lock is sys_fs.c's, so one lock guards the whole space and the
 * two allocators cannot both pick a number; g_fd_lock is only the default for
 * a build without that file. The functions below are defaults too, replaced by
 * sys_fs.c's: a fixture that links ppu_fs.cpp alone sees an empty sys_fs table. */
static std::mutex g_fd_lock;
#ifdef _MSC_VER
#  ifdef _M_IX86
#    define FS_PEER_SYM(x) "_" #x
#  else
#    define FS_PEER_SYM(x) #x
#  endif
#  define FS_PEER_FN(ret, name, params, ...) \
     extern "C" ret name##_absent params { __VA_ARGS__ } \
     extern "C" ret name params; \
     __pragma(comment(linker, "/alternatename:" FS_PEER_SYM(name) "=" FS_PEER_SYM(name##_absent)))
#else
#  define FS_PEER_FN(ret, name, params, ...) extern "C" __attribute__((weak)) ret name params { __VA_ARGS__ }
#endif
FS_PEER_FN(void, ps3_fs_fd_space_lock, (void), g_fd_lock.lock();)
FS_PEER_FN(void, ps3_fs_fd_space_unlock, (void), g_fd_lock.unlock();)
/* Does sys_fs hold the number, as a file or a directory. Called under the lock. */
FS_PEER_FN(int, sys_fs_fd_active, (int fd), (void)fd; return 0;)
/* The FILE* of a file sys_fs holds, or null. */
FS_PEER_FN(FILE*, sys_fs_file_for_fd, (int fd), (void)fd; return nullptr;)
/* The open flags of a file sys_fs holds, or -1. */
FS_PEER_FN(int, sys_fs_file_flags, (int fd), (void)fd; return -1;)
/* Close a file (is_dir 0) or directory (1) sys_fs holds: 1 closed, 0 not its. Takes the lock itself. */
FS_PEER_FN(int, sys_fs_close_fd, (int fd, int is_dir), (void)fd; (void)is_dir; return 0;)
/* One entry of a directory sys_fs holds: 1 an entry (name, and the host directory it is in), 0 end, -1 not its.
 * A null name only asks whether fd is such a directory (1) or not (-1). */
FS_PEER_FN(int, sys_fs_dir_next, (int fd, char* name, size_t name_cap, char* dir, size_t dir_cap),
           (void)fd; (void)name; (void)name_cap; (void)dir; (void)dir_cap; return -1;)

struct FdSpaceLock {
    FdSpaceLock()  { ps3_fs_fd_space_lock(); }
    ~FdSpaceLock() { ps3_fs_fd_space_unlock(); }
    FdSpaceLock(const FdSpaceLock&) = delete;
    FdSpaceLock& operator=(const FdSpaceLock&) = delete;
};
static int fd_alloc_file(FILE* f, uint32_t flags)
{
    FdSpaceLock hold;
    for (int i = 3; i < FS_MAX; i++)
        if (!g_files[i] && !g_dirs[i] && !sys_fs_fd_active(i)) {
            g_fd_dir[i] = 0;
            __atomic_store_n(&g_fd_flags[i], flags, __ATOMIC_RELAXED);   /* published by the release below */
            __atomic_store_n(&g_files[i], f, __ATOMIC_RELEASE);
            return i;
        }
    return -1;
}
static int fd_alloc_dir(DIR* d)
{
    FdSpaceLock hold;
    for (int i = 3; i < FS_MAX; i++)
        if (!g_files[i] && !g_dirs[i] && !sys_fs_fd_active(i)) {
            __atomic_store_n(&g_dirs[i], d, __ATOMIC_RELEASE);
            return i;
        }
    return -1;
}
/* The file this table holds under `fd`. */
static FILE* fd_file_own(int fd)
{
    return fd >= 0 && fd < FS_MAX ? __atomic_load_n(&g_files[fd], __ATOMIC_ACQUIRE) : nullptr;
}
/* The file under `fd`, whichever table holds it: one kernel fd space on hardware, so a handler may be given an fd
 * that was opened through sys_fs (MLB's Bink SDK opens with cellFsOpen and reads with sys_fs_read, and a title
 * may as well do the reverse). An fd outside this table's range is cellFs's EBADF, as on the console. */
static FILE* fd_file(int fd)
{
    if (fd < 0 || fd >= FS_MAX) return nullptr;
    FILE* f = fd_file_own(fd);
    return f ? f : sys_fs_file_for_fd(fd);
}
/* The open flags of the file under `fd`, whichever table holds it, or -1: the access mode the read and write
 * handlers check. */
static int fd_flags(int fd)
{
    if (fd < 0 || fd >= FS_MAX) return -1;
    return fd_file_own(fd) ? (int)__atomic_load_n(&g_fd_flags[fd], __ATOMIC_RELAXED) : sys_fs_file_flags(fd);
}
static DIR* fd_dir(int fd)
{
    return fd >= 0 && fd < FS_MAX ? __atomic_load_n(&g_dirs[fd], __ATOMIC_ACQUIRE) : nullptr;
}
/* Release the file / directory this table holds under `fd`: true when it held one. The number is free once the
 * slot is cleared, under the lock; the stream or handle is closed after it. */
static bool fd_release_file(int fd)
{
    FILE* f = nullptr;
    if (fd >= 0 && fd < FS_MAX) {
        FdSpaceLock hold;
        f = g_files[fd];
        if (f) {
            if (g_fd_usm[fd] && ps3_env_once("PS3_FSLOG_READS")) PS3_LOG(PS3_LOG_INFO, "USMRD", "CLOSE usm fd=%d\n", fd);
            __atomic_store_n(&g_files[fd], (FILE*)nullptr, __ATOMIC_RELEASE);
            g_fd_usm[fd] = 0; g_fd_dir[fd] = 0;
        }
    }
    if (f) fclose(f);
    return f != nullptr;
}
static bool fd_release_dir(int fd)
{
    DIR* d = nullptr;
    if (fd >= 0 && fd < FS_MAX) {
        FdSpaceLock hold;
        d = g_dirs[fd];
        __atomic_store_n(&g_dirs[fd], (DIR*)nullptr, __ATOMIC_RELEASE);
    }
    if (d) closedir(d);
    return d != nullptr;
}
/* What runtime/syscalls/sys_fs.c reaches for an fd this table holds (see sys_fs.h). */
extern "C" FILE* ppu_cellfs_file_for_fd(int fd)
{
    return fd_file_own(fd);
}
extern "C" int ppu_cellfs_fd_owned(int fd)
{
    return fd >= 0 && fd < FS_MAX &&
           (__atomic_load_n(&g_files[fd], __ATOMIC_ACQUIRE) || __atomic_load_n(&g_dirs[fd], __ATOMIC_ACQUIRE));
}
extern "C" int ppu_cellfs_close(int fd, int is_dir)
{
    return is_dir ? fd_release_dir(fd) : fd_release_file(fd);
}
extern "C" int ppu_cellfs_dir_next(int fd, char* name, size_t name_cap, char* dir, size_t dir_cap)
{
    DIR* d = fd_dir(fd);
    if (!d) return -1;
    if (!name) return 1;
    struct dirent* e = readdir(d);
    if (dir && dir_cap) snprintf(dir, dir_cap, "%s", g_dir_path[fd]);
    if (!e) return 0;
    snprintf(name, name_cap, "%s", e->d_name);
    return 1;
}
/* The open flags this table recorded for a file it holds under `fd`, or -1. */
extern "C" int ppu_cellfs_file_flags(int fd)
{
    return fd_file_own(fd) ? (int)__atomic_load_n(&g_fd_flags[fd], __ATOMIC_RELAXED) : -1;
}

/* ---- handlers ---- */
static void cellFsOpen(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t flags  = (uint32_t)ctx->gpr[4];
    uint32_t fd_ptr = (uint32_t)ctx->gpr[5];
    host_path(hpath, sizeof hpath, gpath);

    /* NPDRM: see the note in sys_fs.c -- an EDAT is decrypted once into a cache
     * file and that is opened in its place. */
    { char dec_path[1200];
      const char* use = edat_resolve(hpath, dec_path, sizeof dec_path);
      if (use != hpath) snprintf(hpath, sizeof hpath, "%s", use); }

    /* fopen() mode strings can't express the PS3/POSIX open semantics (e.g.
     * O_WRONLY without create+truncate, or O_CREAT without O_TRUNC), so build
     * real open() flags from the access mode (low 2 bits) + the modifiers, then
     * wrap the fd in a FILE* with fdopen() (which neither creates nor truncates
     * -- that was already decided by open()). */
    int acc = flags & 0x3;
    int oflags = (acc == CELL_FS_O_RDWR)   ? O_RDWR
               : (acc == CELL_FS_O_WRONLY) ? O_WRONLY : O_RDONLY;
    if (flags & CELL_FS_O_CREAT)  oflags |= O_CREAT;
    if (flags & CELL_FS_O_TRUNC)  oflags |= O_TRUNC;
    if (flags & CELL_FS_O_APPEND) oflags |= O_APPEND;

    /* A bare device root ("/dev_bdvd") resolves to a DIRECTORY, and open() on a
     * directory fails on Windows. Real cellFsOpen answers EISDIR there, and a
     * title reads that as "the medium is mounted" -- YDKJ probes the disc this
     * way before loading its Scaleform UI and FMOD banks, so the ENOENT it got
     * instead read as "no disc" and it drew nothing but a clear colour.
     * S_IFMT/S_IFDIR rather than S_ISDIR: MSVC's CRT does not define the macro. */
    {
        struct stat _dst;
        if (stat(hpath, &_dst) == 0 && (_dst.st_mode & S_IFMT) == S_IFDIR) {
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EISDIR;
            return;
        }
    }
    int hfd = open(hpath, oflags | O_BINARY, 0666);
    if (hfd < 0) {
        PS3_LOG(PS3_LOG_WARN, "fs", "open FAIL '%s' -> '%s' (flags=0x%X)\n", gpath, hpath, flags);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    const char* fmode = (acc == CELL_FS_O_RDWR)
                        ? ((flags & CELL_FS_O_APPEND) ? "ab+" : "rb+")
                        : (acc == CELL_FS_O_WRONLY)
                          ? ((flags & CELL_FS_O_APPEND) ? "ab" : "wb")
                          : "rb";
    FILE* f = fdopen(hfd, fmode);
    if (!f) {
        close(hfd);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return;
    }
    int fd = fd_alloc_file(f, flags);
    if (fd < 0) { fclose(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EMFILE; return; }
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    g_fd_usm[fd] = (strstr(gpath, ".usm") != nullptr) ? 1 : 0;
    /* FS_CALLER=1: name the guest function that opened each file. A title whose
     * streaming layer opens a file and never reads it (You Don't Know Jack's
     * movies) is diagnosed from the opener: its sibling read path is the one
     * that is not running. */
    if (fd >= 0 && fd < FS_MAX) { strncpy(g_fd_path[fd], gpath, sizeof g_fd_path[fd]-1); g_fd_path[fd][sizeof g_fd_path[fd]-1]=0; }
    { char who[64] = "?";
      if (ps3_env_once("FS_CALLER")) { ppu_guest_caller(who, sizeof who);
          PS3_LOG(PS3_LOG_INFO, "fs", "open '%s' -> fd %d  (opened by %s)\n", gpath, fd, who);
          /* FS_STACK=<substr>: every open funnels through one guest wrapper, so
           * one caller level says nothing about WHICH subsystem wanted the file.
           * Dump the guest call chain for the opens that matter. */
          { const char* want = ps3_env_once("FS_STACK");
            if (want && *want && strstr(gpath, want)) ydkj_host_bt("fs-open");
          } }
      else PS3_LOG(PS3_LOG_INFO, "fs", "open '%s' -> fd %d\n", gpath, fd); }
    if (ps3_env_once("PS3_FSLOG_BT") && strstr(gpath, ".usm")) {
        /* Resolve to GUEST functions (raw host RVAs are useless here): this tells us
         * which criMv/criFs function opened the movie, so the reader-attach path
         * (stream+0x10, never wired => movie opened but never read) can be found. */
        PS3_LOG(PS3_LOG_INFO, "USMBT", "open '%s' -> fd %d\n", gpath, fd); fflush(stderr);
        ydkj_host_bt("usm-open");
    }
    ctx->gpr[3] = CELL_OK;
}

/* cellFsSdataOpen(path, flags, fd*, arg*, size): opens a PS3 SDATA/EDAT
 * encrypted-data file and returns an fd whose reads yield the DECRYPTED
 * plaintext. LBP 1.30 stores its SPU job code modules inside patch.sdat (an
 * FSHb container, zlib-compressed C0DEC0DE modules) and opens it via this call;
 * leaving it faked (default CELL_OK, no handle) meant the container was never
 * read -> zero job modules loaded -> every SPU physics job dispatched into an
 * empty code buffer -> the loading freeze.
 *
 * Our run env's PS3_HDD0_ROOT (RPCS3 dev_hdd0) already holds patch.sdat in
 * DECRYPTED form (magic "FSHb", not "NPD"), so we just open it read-only --
 * no EDAT decryption needed. (A title supplying a still-encrypted NPD .sdat
 * would need the SDATA block cipher; not required here.) The license `arg`
 * (gpr[6]) and its size (gpr[7]) are ignored. */
static void cellFsSdataOpen(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t fd_ptr = (uint32_t)ctx->gpr[5];
    host_path(hpath, sizeof hpath, gpath);

    /* Same NPDRM step cellFsOpen does: an encrypted container is decrypted once
     * into a cache file and that is opened in its place. This used to be a
     * second, weaker decryptor local to this function (sdata_decrypt below),
     * which handled the self-keyed SDATA form only and refused every real EDAT
     * -- so a title whose data file was EDAT rather than SDATA got "success
     * without a handle" here while the very same file opened fine through
     * cellFsOpen. One decryptor, reached from both doors. */
    { char dec_path[1200];
      const char* use = edat_resolve(hpath, dec_path, sizeof dec_path);
      if (use != hpath) snprintf(hpath, sizeof hpath, "%s", use); }

    int hfd = open(hpath, O_RDONLY | O_BINARY, 0666);
    if (hfd < 0) {
        PS3_LOG(PS3_LOG_WARN, "fs", "SdataOpen FAIL '%s' -> '%s'\n", gpath, hpath);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    FILE* f = fdopen(hfd, "rb");
    if (!f) { close(hfd); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    /* If the file IS still NPD-encrypted, we can't serve it plaintext here --
     * warn loudly rather than hand back ciphertext the title will choke on.
     * Use portable fread/fseek (POSIX read()/ssize_t aren't available under the
     * exe's compiler). */
    unsigned char magic[4] = {0};
    size_t got = fread(magic, 1, 4, f);
    fseek(f, 0, SEEK_SET);
    if (got == 4 && magic[0]=='N' && magic[1]=='P' && magic[2]=='D') {
        /* NPD-encrypted SDATA: decrypt it in full and serve the plaintext from
         * an anonymous tmpfile so the title's reads see the real container. */
        fseek(f, 0, SEEK_END); long enc_sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t* enc = (enc_sz > 0) ? (uint8_t*)malloc((size_t)enc_sz) : nullptr;
        size_t rd = enc ? fread(enc, 1, (size_t)enc_sz, f) : 0;
        fclose(f);
        size_t dec_sz = 0;
        uint8_t* dec = (enc && rd == (size_t)enc_sz) ? sdata_decrypt(enc, rd, &dec_sz) : nullptr;
        free(enc);
        if (!dec) {
            PS3_LOG(PS3_LOG_WARN, "fs", "SdataOpen '%s' NPD decrypt FAILED (unsupported "
                    "EDAT/needs license); returning success without a handle\n", gpath);
            ctx->gpr[3] = CELL_OK; return;   /* don't feed the title ciphertext */
        }
        char dmagic[4] = {0};
        if (dec_sz >= 4) memcpy(dmagic, dec, 4);
        FILE* tf = tmpfile();
        if (!tf || fwrite(dec, 1, dec_sz, tf) != dec_sz) {
            if (tf) fclose(tf); free(dec);
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return;
        }
        free(dec);
        rewind(tf);
        int fd = fd_alloc_file(tf, CELL_FS_O_RDONLY);   /* a decrypted copy to read, never to write */
        if (fd < 0) { fclose(tf); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EMFILE; return; }
        if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
        PS3_LOG(PS3_LOG_INFO, "fs", "SdataOpen '%s' -> fd %d (NPD decrypted, 0x%zX bytes, magic '%c%c%c%c')\n",
                gpath, fd, dec_sz,
                dmagic[0]?dmagic[0]:'?', dmagic[1]?dmagic[1]:'?',
                dmagic[2]?dmagic[2]:'?', dmagic[3]?dmagic[3]:'?');
        ctx->gpr[3] = CELL_OK;
        return;
    }
    int fd = fd_alloc_file(f, CELL_FS_O_RDONLY);   /* opened read-only above */
    if (fd < 0) { fclose(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EMFILE; return; }
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    PS3_LOG(PS3_LOG_INFO, "fs", "SdataOpen '%s' -> fd %d (magic %c%c%c%c)\n", gpath, fd,
            magic[0]?magic[0]:'?', magic[1]?magic[1]:'?', magic[2]?magic[2]:'?', magic[3]?magic[3]:'?');
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClose(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (ps3_env_once("FS_CALLER")) { char w[64]="?"; ppu_guest_caller(w,sizeof w);
        PS3_LOG(PS3_LOG_INFO, "fs", "close fd=%d  (by %s)\n", fd, w); }
    /* One fd space: a number this table does not hold may be a file opened through sys_fs. */
    if (!fd_release_file(fd) && fd >= 0 && fd < FS_MAX) sys_fs_close_fd(fd, 0);
    ctx->gpr[3] = CELL_OK;
}

/* Pre-fault a guest range so the kernel can write into it.
 *
 * The flat VM is MEM_RESERVEd and each page is committed on FIRST ACCESS by a
 * vectored exception handler. That covers CPU access from lifted code, but
 * fread/fwrite move data through the kernel, and a kernel write to a reserved
 * page does not raise a user-mode exception -- the I/O just fails, returning 0
 * with ferror set. Twisted Metal hit this on every read whose destination the
 * guest had not touched yet: read 1 of a file succeeded, read 2 into a fresh
 * page came back n=0 eof=0 err=1, and the title logged its own
 * "Short read ... Possible reasons include disc eject" and gave up.
 *
 * Touch each page read-then-write so the fault happens here, in user mode,
 * where the handler can commit it. Read-then-write rather than a plain store so
 * nothing already in the buffer is disturbed. */
static inline void fs_prefault(uint32_t buf, uint64_t len)
{
    if (!vm_base || !len) return;
    volatile uint8_t* p = (volatile uint8_t*)(vm_base + buf);
    for (uint64_t o = 0; o < len; o += 0x1000) p[o] = p[o];
    p[len - 1] = p[len - 1];
}

static void cellFsRead(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[6];
    FILE* f = fd_file(fd);
    const int flags = f ? fd_flags(fd) : -1;
    /* RPCS3 6e4b784d1 sys_fs_read (:1498-1502), which cellFsRead wraps (cellFs.cpp:57-62): a read on a write-only
     * file is EBADF with *nread = 0. The cellFs table kept no flags to check. */
    if (!f || (nbytes && flags >= 0 && (flags & CELL_FS_O_WRONLY))) {
        if (nread_ptr && fs_range_ok(nread_ptr, 8)) vm_write64(nread_ptr, 0);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF;
        return;
    }
    uint64_t raw_nbytes = ctx->gpr[5];
    long fpos_before = ftell(f);
    /* The firmware checks the null out pointer and the null buffer here and
     * nowhere else (oracle RPCS3 sys_fs_read: `!nread` or `!buf` -> CELL_EFAULT),
     * so both keep their code; the byte count is left at 0, which is a value a
     * caller can act on. A buffer that leaves the arena is a different matter:
     * see below. */
    if (!nread_ptr || !buf) {
        if (nread_ptr && fs_range_ok(nread_ptr, 8)) vm_write64(nread_ptr, 0);
        ctx->gpr[3] = 0x8001000Du; /* CELL_EFAULT */
        return;
    }
    /* A buffer that is partly or wholly outside guest memory is NOT clamped to
     * the part inside it and NOT reported as "0 bytes, CELL_OK": the old
     * `ppu_vm_size - buf` underflowed when buf already lay past the arena, and
     * fread wrote past the mapping. lv2's own answer to a copyout fault is
     * unverified, so there is no faithful error code to invent -- the span is
     * refused before any host pointer is formed. */
    if (!fs_range_ok(buf, nbytes)) fs_range_stop("cellFsRead", "buf", buf, nbytes);
    fs_prefault(buf, nbytes);
    fs_set_dir(fd, f, 1);
    vm_note_write(buf, (size_t)nbytes);   /* kernel write into guest memory */
    size_t n = fread(vm_base + buf, 1, (size_t)nbytes, f);   /* raw bytes, no swap */
    vm_note_write_check(n < (size_t)nbytes && ferror(f) && errno == EFAULT, "cellFsRead");
    /* PS3_FSLOG_EOF=<fd>,<bytes>: report end-of-file past <bytes> on one descriptor.
     * The intro cinematic is 28 seconds and this port renders it at a few frames
     * a second, so it cannot be watched to its end -- truncating the stream makes
     * the demuxer see EOF and the movie finish, which is what advances the title
     * to whatever follows the intro. Deliberately a testing knob, not a fix. */
    { static int efd = -2; static long elim = 0;
      if (efd == -2) { const char* e = ps3_env_once("PS3_FSLOG_EOF");
          if (e) { efd = atoi(e); const char* c = strchr(e, 44); elim = c ? atol(c + 1) : 0; }
          else efd = -1; }
      if (efd >= 0 && fd == efd && elim > 0 && fpos_before >= elim) {
          static int once = 0;
          if (!once++) PS3_LOG(PS3_LOG_WARN, "fs", "PS3_FSLOG_EOF: fd=%d truncated at %ld bytes\n", fd, elim);
          n = 0;
      } }
    if (g_fd_usm[fd] && ps3_env_once("PS3_FSLOG_READS")) PS3_LOG(PS3_LOG_INFO, "USMRD", "READ usm fd=%d nbytes=%llu -> %zu magic=%02X%02X%02X%02X pos=%ld lr=0x%08X\n", fd, (unsigned long long)nbytes, n, vm_base[buf], vm_base[buf+1], vm_base[buf+2], vm_base[buf+3], fpos_before, (uint32_t)ctx->lr);
    /* PS3_FSLOG_READS=<fd>: log every read on one descriptor. PS3_FSLOG caps at 20
     * lines and they are all spent before a movie ever opens, so it cannot
     * answer "is the streamer reading the .avi". */
    { static int wfd = -2;
      if (wfd == -2) { const char* e = ps3_env_once("PS3_FSLOG_READS"); wfd = e ? atoi(e) : -1; }
      if (wfd >= 0 && fd == wfd)
          PS3_LOG(PS3_LOG_INFO, "fsread", "fd=%d want=%llu got=%zu pos=%ld\n",
                  fd, (unsigned long long)nbytes, n, fpos_before); }
    if (ps3_env_once("PS3_FSLOG")) PS3_LOG_N(20, PS3_LOG_INFO, "FSDBG", "fd=%d raw_nbytes=0x%llX clamped=0x%llX buf=0x%08X fpos_before=%ld n=%zu eof=%d err=%d\n", fd,(unsigned long long)raw_nbytes,(unsigned long long)nbytes,buf,fpos_before,n,feof(f),ferror(f));
#ifdef _WIN32
    if (ps3_env_once("PS3_FSLOG") && buf==0 && raw_nbytes>0x10000) { static int _b=0; if(_b++<2){ void* fr[30]; unsigned short nn=RtlCaptureStackBackTrace(0,30,fr,0); uintptr_t mb=(uintptr_t)GetModuleHandleA(0); fprintf(stderr,"[FSBT] null-buf read caller rvas:"); for(unsigned short i=0;i<nn&&i<16;i++) fprintf(stderr," %llX",(unsigned long long)((uintptr_t)fr[i]-mb)); fprintf(stderr,"\n"); } }
#endif
    /* Per-fd totals, not just the first 50 lines. The flat cap made "this file is
     * opened and never read" unfalsifiable: reads on a later-opened fd fall off
     * the end of the log and look identical to reads that never happen.
     * FS_READ_ALL=1 logs every read; otherwise a per-fd first-read line plus a
     * periodic summary is enough to tell the two apart. */
    /* Any guest thread reads, so the counters are relaxed atomics: a diagnostic, but a plain static here is a data
     * race between two reading threads (TSan, test_ppu_fs_sys_bridge --tsan). */
    { static std::atomic<uint64_t> tot{0}; static std::atomic<uint32_t> reads{0};
      static std::atomic<uint64_t> per_fd[64]; static std::atomic<uint32_t> cnt_fd[64];
      const uint64_t total = tot.fetch_add(n, std::memory_order_relaxed) + n;
      const uint32_t nread = reads.fetch_add(1, std::memory_order_relaxed) + 1;
      uint32_t fd_reads = 0;
      if (fd>=0 && fd<64) { per_fd[fd].fetch_add(n, std::memory_order_relaxed);
                            fd_reads = cnt_fd[fd].fetch_add(1, std::memory_order_relaxed) + 1; }
      const int first_for_fd = fd_reads == 1;
      if(nread <= 50 || first_for_fd || ps3_env_once("FS_READ_ALL"))
        PS3_LOG(PS3_LOG_INFO, "fs", "read fd=%d nbytes=%llu -> %zu (magic=%02X%02X%02X%02X, total=%llu)%s\n",
                fd,(unsigned long long)nbytes,n,vm_base[buf],vm_base[buf+1],vm_base[buf+2],vm_base[buf+3],
                (unsigned long long)total, first_for_fd?"  <= FIRST READ ON THIS FD":"");
      if (ps3_env_once("FS_READ_PATH") && fd>=0 && fd<64 && g_fd_path[fd][0])
          fprintf(stderr, "        from %s\n", g_fd_path[fd]);
      if ((nread % 2000)==0) { fprintf(stderr,"[fs] read summary after %u reads:",nread);
          for (int i=0;i<64;i++) { const uint32_t c = cnt_fd[i].load(std::memory_order_relaxed);
              if (c) fprintf(stderr," fd%d=%ux/%lluB",i,c,
                             (unsigned long long)per_fd[i].load(std::memory_order_relaxed)); }
          fprintf(stderr,"\n"); } }
    if (ps3_env_once("PS3_FSLOG_TOC") && nbytes >= 50000) {  /* data.toc read -> who parses it? */
        PS3_LOG(PS3_LOG_INFO, "TOC", "data.toc read into buf=0x%08X n=%zu; lr=0x%08llX; guest-stack RAs:\n", buf, n, (unsigned long long)ctx->lr);
        uint32_t sp = (uint32_t)ctx->gpr[1];
        for (uint32_t i = 0; i < 128 && fs_range_ok((uint64_t)sp + i*4, 4); i++) {
            uint32_t a = sp + i*4; uint32_t w = (vm_base[a]<<24)|(vm_base[a+1]<<16)|(vm_base[a+2]<<8)|vm_base[a+3];
            if (w >= 0x10000u && w < 0x600000u) PS3_LOG(PS3_LOG_INFO, "TOC", "  ra 0x%08X (@sp+0x%X)\n", w, i*4);
        }
    }
    if (nread_ptr) vm_write64(nread_ptr, n);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsWrite(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nwr_ptr = (uint32_t)ctx->gpr[6];
    FILE* f = fd_file(fd);
    const int flags = f ? fd_flags(fd) : -1;
    /* RPCS3 6e4b784d1 sys_fs_write (:1593-1597), which cellFsWrite wraps (cellFs.cpp:63-69): a file not opened for
     * writing is EBADF with *nwrite = 0. It was EIO, from the host stream refusing the write; the cellFs table kept
     * no flags to check. Read-only mounts (EROFS) are not modelled. */
    if (!f || (nbytes && flags >= 0 && !(flags & CELL_FS_O_ACCMODE))) {
        if (nwr_ptr && fs_range_ok(nwr_ptr, 8)) vm_write64(nwr_ptr, 0);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF;
        return;
    }
    /* Oracle RPCS3 sys_fs_write: `!buf` -> CELL_EFAULT with bytes written 0. */
    if (!buf) {
        if (fs_range_ok(nwr_ptr, 8)) vm_write64(nwr_ptr, 0);
        ctx->gpr[3] = 0x8001000Du; /* CELL_EFAULT */
        return;
    }
    /* Same rule as the read path: lv2 would read this span, so it must be guest
     * memory in full. Neither a clamped prefix nor "0 bytes, CELL_OK" is a
     * faithful answer to a span that leaves the arena. */
    if (!fs_range_ok(buf, nbytes)) fs_range_stop("cellFsWrite", "buf", buf, nbytes);
    fs_prefault(buf, nbytes);   /* kernel READS the buffer; same reserved-page trap */
    fs_set_dir(fd, f, 2);
    clearerr(f);    /* ferror() is sticky: an old failure would look like this write's */
    size_t n = fwrite(vm_base + buf, 1, (size_t)nbytes, f);
    /* Flush here: a small write only reaches the kernel at the flush, so an
     * unflushed one always looks like it worked. */
    const int write_failed = ps3_fs_write_failed(n, nbytes, fflush(f) != 0 || ferror(f));
    /* DIAGNOSTIC (PS3_FSLOG_BT=1): dump the guest back-chain when the game logs the
     * render-config failure, to locate setScreenRenderTargetInternal & the config obj. */
    if (ps3_env_once("PS3_FSLOG_BT") && buf && nbytes > 0 && nbytes < 4096 && vm_base) {
        char tmp[256]; uint32_t nn = (uint32_t)(nbytes < 255 ? nbytes : 255);
        memcpy(tmp, vm_base + buf, nn); tmp[nn] = 0;
        if (strstr(tmp,"config") || strstr(tmp,"Config") || strstr(tmp,"Mystery") ||
            strstr(tmp,"downsample") || strstr(tmp,"RenderTarget")) {
            uint32_t sp = (uint32_t)ctx->gpr[1];
            PS3_LOG(PS3_LOG_INFO, "cfgbt", "write \"%.60s\" lr=0x%08X sp=0x%08X\n", tmp, (uint32_t)ctx->lr, sp);
            for (int i = 0; i < 24 && sp && sp < 0x10000000u; i++) {
                uint32_t nsp; memcpy(&nsp, vm_base + sp, 4);
                nsp = ((nsp>>24)&0xFF)|((nsp>>8)&0xFF00)|((nsp<<8)&0xFF0000)|((nsp<<24)&0xFF000000);
                if (nsp <= sp || nsp >= 0x10000000u) break;
                uint32_t lr; memcpy(&lr, vm_base + nsp + 0x10, 4);
                lr = ((lr>>24)&0xFF)|((lr>>8)&0xFF00)|((lr<<8)&0xFF0000)|((lr<<24)&0xFF000000);
                PS3_LOG(PS3_LOG_INFO, "cfgbt", "  #%d lr=0x%08X\n", i, lr);
                sp = nsp;
            }
            fflush(stderr);
        }
    }
    if (nwr_ptr) vm_write64(nwr_ptr, n);
    ctx->gpr[3] = write_failed ? (uint64_t)(int64_t)CELL_FS_EIO : CELL_OK;
}

/* cellFsFGetBlockSize(int fd, u64* sector_size, u64* block_size)
 * cellFsGetBlockSize(const char* path, u64* sector_size, u64* block_size)
 *
 * Titles ask the filesystem what granularity it wants and then size and ALIGN
 * their reads to the answer. Left unregistered, the call fell through to the
 * unresolved-NID path: the out-params were never written, so the title read
 * whatever was already in them.
 *
 * The Orange Box's chooser does exactly this. With no answer it re-seeks to
 * offset 0 and re-reads the same 64 KB of APP_CHOOSER.GRP forever -- 241,997
 * reads and 15.86 GB of I/O against a 4 MB file in one run -- because the
 * offset arithmetic it derives from the block size collapses to zero.
 *
 * Both out-params are u64 and big-endian, so they go through vm_write64.
 * PS3_FS_BLOCK_SIZE overrides the value for a title that wants a different
 * granularity. */
static void fs_report_block_size(ppu_context* ctx, uint32_t sec_ptr, uint32_t blk_ptr)
{
    static uint64_t bs = 0;
    if (!bs) {
        const char* e = ps3_env_once("PS3_FS_BLOCK_SIZE");
        bs = (e && *e) ? (uint64_t)strtoull(e, nullptr, 0) : 4096ull;
        if (!bs) bs = 4096ull;
    }
    if (sec_ptr) vm_write64(sec_ptr, bs);
    if (blk_ptr) vm_write64(blk_ptr, bs);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsFGetBlockSize(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (!fd_file(fd)) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF;
        return;
    }
    fs_report_block_size(ctx, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);
}

static void cellFsGetBlockSize(ppu_context* ctx)
{
    /* Path variant: the answer does not depend on the file, only the device,
     * and every mount we serve is backed by the same host filesystem. */
    fs_report_block_size(ctx, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);
}

static void cellFsLseek(ppu_context* ctx)
{
    int fd        = (int)(uint32_t)ctx->gpr[3];
    int64_t off   = (int64_t)ctx->gpr[4];
    uint32_t wh   = (uint32_t)ctx->gpr[5];
    uint32_t pos_ptr = (uint32_t)ctx->gpr[6];
    FILE* f = fd_file(fd);
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    if (ps3_env_once("FS_CALLER")) { char w[64]="?"; ppu_guest_caller(w,sizeof w);
        PS3_LOG(PS3_LOG_INFO, "fs", "lseek fd=%d off=%lld whence=%u  (by %s)\n", fd, (long long)off, wh, w); }
    /* An unknown whence, or a seek that would land before the start of the
     * file, is CELL_EINVAL and leaves the offset where it was. fseek failing
     * is not harmless on the MSVC CRT: it drops the stream's buffer and the
     * stream then reports the OS position, which is past the buffered data,
     * so the old position is put back explicitly. */
    if (wh > CELL_FS_SEEK_END) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EINVAL; return; }
    int worigin = (wh == CELL_FS_SEEK_END) ? SEEK_END : (wh == CELL_FS_SEEK_CUR) ? SEEK_CUR : SEEK_SET;
    const int64_t before = FS_TELL64(f);
    if (FS_SEEK64(f, off, worigin) != 0) {
        if (before >= 0) FS_SEEK64(f, before, SEEK_SET);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EINVAL; return;
    }
    int64_t p = FS_TELL64(f);
    if (pos_ptr) vm_write64(pos_ptr, (uint64_t)p);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsStat is 0x34 (52) bytes, 4-byte aligned -- the s64/u64 members are
 * be_t<...,4> so there is NO 4-byte pad after gid (verified vs RPCS3:
 * CHECK_SIZE_ALIGN(CellFsStat, 52, 4)). Laying it out 8-byte-aligned (0x38,
 * pad@0x0C) shifts size/blksize +4 and overruns the struct by 4 bytes -- games
 * that embed a CellFsStat inside a larger object (e.g. Dantelion's
 * DLFileDeviceStream, stat@obj+0xD8) then have the trailing blksize clobber the
 * field right after the stat (the fd at obj+0x10c), which later fails lseek.
 * Layout: mode@0 uid@4 gid@8 atime@0x0C mtime@0x14 ctime@0x1C size@0x24 blksize@0x2C. */
static void write_stat(uint32_t sb, uint32_t mode, uint64_t size,
                       int64_t atime = 0, int64_t mtime = 0, int64_t ctime = 0)
{
    vm_write32(sb + 0x00, mode);
    vm_write32(sb + 0x04, 0);            /* uid */
    vm_write32(sb + 0x08, 0);            /* gid */
    vm_write64(sb + 0x0C, (uint64_t)atime);   /* seconds since the epoch */
    vm_write64(sb + 0x14, (uint64_t)mtime);
    vm_write64(sb + 0x1C, (uint64_t)ctime);
    vm_write64(sb + 0x24, size);         /* size */
    vm_write64(sb + 0x2C, 0x200);        /* blksize */
}

static void cellFsStat(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t sb = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    fs_stat64_t st;
    if (FS_STAT64(hpath, &st) != 0) {
        if (ps3_env_once("PS3_FSLOG")) PS3_LOG(PS3_LOG_INFO, "fs", "stat '%s' -> ENOENT\n", gpath);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    if (ps3_env_once("PS3_FSLOG")) PS3_LOG(PS3_LOG_INFO, "fs", "stat '%s' -> OK (size=%lld)\n", gpath, (long long)st.st_size);
    uint32_t mode = (st.st_mode & S_IFDIR) ? (CELL_FS_S_IFDIR | 0x1FF)
                                           : (CELL_FS_S_IFREG | 0x1B6);
    /* The file's own times; they used to be written as 0 (1970), and a title
     * that lists a directory and shows or compares file dates saw all of them
     * equal. */
    if (sb) write_stat(sb, mode, (uint64_t)st.st_size,
                       (int64_t)st.st_atime, (int64_t)st.st_mtime, (int64_t)st.st_ctime);
    if (ps3_env_once("PS3_FSLOG") && strstr(gpath,".toc")) PS3_LOG(PS3_LOG_INFO, "FSDBG", "cellFsStat('%s') -> size=0x%llX\n",gpath,(unsigned long long)st.st_size);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsFstat(ppu_context* ctx)
{
    int fd      = (int)(uint32_t)ctx->gpr[3];
    uint32_t sb = (uint32_t)ctx->gpr[4];
    FILE* f = fd_file(fd);
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    if (ps3_env_once("FS_CALLER")) { char w[64]="?"; ppu_guest_caller(w,sizeof w);
        PS3_LOG(PS3_LOG_INFO, "fs", "fstat fd=%d  (by %s)\n", fd, w); }
    int64_t cur = FS_TELL64(f);
    FS_SEEK64(f, 0, SEEK_END);
    int64_t sz = FS_TELL64(f);
    FS_SEEK64(f, cur, SEEK_SET);
    /* FS_FSTAT_CAP=<bytes>: DIAGNOSTIC. Report a smaller size than the file
     * has. You Don't Know Jack reads a 100 KB archive whole but only probes
     * (open/fstat/close) its 500 KB and 2.4 MB ones; if that branch is driven
     * by the size it just asked for, capping it makes the title take the read
     * path. Answers whether the split is size-based -- nothing more. */
    { const char* cap = ps3_env_once("FS_FSTAT_CAP");
      const char* only = ps3_env_once("FS_FSTAT_CAP_PATH");
      if (cap && only && *only && !(g_fd_path[fd][0] && strstr(g_fd_path[fd], only))) cap = 0;
      if (cap) { long long c = strtoll(cap, 0, 0);
          if (c > 0 && sz > c) {
              PS3_LOG(PS3_LOG_WARN, "fs", "fstat fd=%d size %lld -> capped %lld (FS_FSTAT_CAP)\n", fd, (long long)sz, c);
              sz = c; } } }
    if (sb) {
        /* Times only from the host: the size above also counts unflushed writes. */
        fs_stat64_t fst;   /* 64-bit: a 32-bit fstat fails on a file past 2 GiB */
#ifdef _WIN32
        const int have_t = _fstat64(_fileno(f), &fst) == 0;
#else
        const int have_t = fstat(fileno(f), &fst) == 0;
#endif
        write_stat(sb, CELL_FS_S_IFREG | 0x1B6, (uint64_t)sz,
                   have_t ? (int64_t)fst.st_atime : 0, have_t ? (int64_t)fst.st_mtime : 0,
                   have_t ? (int64_t)fst.st_ctime : 0);
    }
    if (ps3_env_once("PS3_FSLOG")) PS3_LOG_N(12, PS3_LOG_INFO, "FSDBG", "cellFsFstat(fd=%d) -> size=0x%llX\n",fd,(unsigned long long)sz);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsOpendir(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t fd_ptr = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    DIR* d = opendir(hpath);
    if (ps3_env_once("PS3_FSLOG")) PS3_LOG(PS3_LOG_INFO, "fs", "opendir '%s' -> %s\n", gpath, d ? "OK" : "ENOENT");
    if (!d) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return; }
    int fd = fd_alloc_dir(d);
    if (fd < 0) { closedir(d); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EMFILE; return; }
    strncpy(g_dir_path[fd], hpath, sizeof g_dir_path[fd] - 1);
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsDirent: d_type(1) d_namlen(1) d_name[256]; total 0x102. */
static void cellFsReaddir(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t dirent = (uint32_t)ctx->gpr[4];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[5];
    DIR* d = fd_dir(fd);
    char bname[256], bdir[1100];
    const char* name;
    const char* dir_path;
    if (d) {
        struct dirent* e = readdir(d);
        if (!e) { if (nread_ptr) vm_write64(nread_ptr, 0); ctx->gpr[3] = CELL_OK; return; }
        name = e->d_name;
        dir_path = g_dir_path[fd];
    } else {
        /* One fd space: a directory opened through sys_fs is read through its table. */
        const int r = fd >= 3 && fd < FS_MAX ? sys_fs_dir_next(fd, bname, sizeof bname, bdir, sizeof bdir) : -1;
        if (r < 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
        if (r == 0) { if (nread_ptr) vm_write64(nread_ptr, 0); ctx->gpr[3] = CELL_OK; return; }
        name = bname;
        dir_path = bdir;
    }
    char full[1300]; struct stat st;
    snprintf(full, sizeof full, "%s/%s", dir_path, name);
    /* "." and ".." are always directories: a title that walks a tree skips the
     * dot entries by type and name, and one typed as a file would be handed
     * to its file handler. Not left to stat("<dir>//.") resolving. */
    const int dot = name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]));
    uint8_t type = (dot || (stat(full, &st) == 0 && (st.st_mode & S_IFDIR)))
                   ? CELL_FS_TYPE_DIR : CELL_FS_TYPE_REG;
    size_t nl = strlen(name); if (nl > 255) nl = 255;
    vm_base[dirent + 0] = type;
    vm_base[dirent + 1] = (uint8_t)nl;
    for (size_t i = 0; i < nl; i++) vm_base[dirent + 2 + i] = (uint8_t)name[i];
    vm_base[dirent + 2 + nl] = 0;
    if (nread_ptr) vm_write64(nread_ptr, 0x102);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsDirectoryEntry is a 52-byte stat followed by a 258-byte dirent,
 * rounded to four-byte alignment: 312 bytes. entries_size is bytes, while
 * data_count is a big-endian u32 count of complete entries. The stream is
 * shared with cellFsReaddir and cellFsClosedir. */
static void cellFsGetDirectoryEntries(ppu_context* ctx)
{
    const uint32_t fd = (uint32_t)ctx->gpr[3];
    const uint32_t entries = (uint32_t)ctx->gpr[4];
    const uint32_t bytes = (uint32_t)ctx->gpr[5];
    const uint32_t count_ptr = (uint32_t)ctx->gpr[6];
    if (!entries || !count_ptr || !fs_range_ok(entries, bytes) || !fs_range_ok(count_ptr, 4)) {
        ctx->gpr[3] = 0x8001000Du; // CELL_EFAULT
        return;
    }
    DIR* d = fd < 3 ? nullptr : fd_dir(fd);
    /* One fd space: a directory opened through sys_fs is read through its table. */
    const bool bridged = !d && fd >= 3 && fd < FS_MAX && sys_fs_dir_next((int)fd, nullptr, 0, nullptr, 0) > 0;
    if (!d && !bridged) {
        ctx->gpr[3] = 0x8001002Au; // CELL_EBADF
        return;
    }
    uint32_t count = 0;
    ctx->gpr[3] = CELL_OK;
    for (; count < bytes / 312; ++count) {
        errno = 0;
        char bname[256], bdir[1100];
        const char* name;
        const char* dir_path;
        if (d) {
            struct dirent* entry = readdir(d);
            if (!entry) {
                if (errno) ctx->gpr[3] = 0x8001002Bu; // CELL_EIO
                break;
            }
            name = entry->d_name;
            dir_path = g_dir_path[fd];
        } else {
            if (sys_fs_dir_next((int)fd, bname, sizeof bname, bdir, sizeof bdir) <= 0) break;
            name = bname;
            dir_path = bdir;
        }
        char path[1300];
        snprintf(path, sizeof path, "%s/%s", dir_path, name);
        struct stat st;
        if (stat(path, &st) != 0) {
            ctx->gpr[3] = 0x8001002Bu; // entry disappeared or cannot be inspected
            break;
        }
        uint32_t out = entries + count * 312;
        memset(vm_base + out, 0, 312);
        bool directory = (st.st_mode & S_IFDIR) != 0;
        write_stat(out, (directory ? CELL_FS_S_IFDIR : CELL_FS_S_IFREG) |
                        (uint32_t(st.st_mode) & 0777), uint64_t(st.st_size));
        vm_write64(out + 0x0C, uint64_t(st.st_atime));
        vm_write64(out + 0x14, uint64_t(st.st_mtime));
        vm_write64(out + 0x1C, uint64_t(st.st_ctime));
        size_t length = strlen(name);
        if (length > 255) length = 255;
        vm_base[out + 52] = directory ? CELL_FS_TYPE_DIR : CELL_FS_TYPE_REG;
        vm_base[out + 53] = uint8_t(length);
        memcpy(vm_base + out + 54, name, length);
    }
    vm_write32(count_ptr, count);
}

static void cellFsClosedir(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    /* One fd space: a number this table does not hold may be a directory opened through sys_fs. */
    if (!fd_release_dir(fd) && fd >= 0 && fd < FS_MAX) sys_fs_close_fd(fd, 1);
    ctx->gpr[3] = CELL_OK;
}

#ifdef _WIN32
#include <direct.h>
#define HOST_MKDIR(p) _mkdir(p)
#else
#define HOST_MKDIR(p) mkdir((p), 0777)
#endif

/* Create `path` and any missing parent dirs on the host. Returns 0 on
 * success or if the directory already exists. */
static int host_mkdir_p(const char* path)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    while (len > 1 && (tmp[len-1] == '/' || tmp[len-1] == '\\')) tmp[--len] = 0;
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') { char c = *p; *p = 0; HOST_MKDIR(tmp); *p = c; }
    }
    int r = HOST_MKDIR(tmp);
    if (r != 0) { struct stat st; if (stat(tmp, &st) == 0 && (st.st_mode & S_IFDIR)) r = 0; }
    return r;
}

/* cellFsMkdir(path, mode): create the guest directory on the host. A no-op stub
 * here silently breaks games that create then poll a dir (e.g. LBP's boot waits
 * for /dev_hdd0/game/<title>/USRDIR to appear). Create parents too. */
static void cellFsMkdir(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    host_path(hpath, sizeof hpath, gpath);
    int r = host_mkdir_p(hpath);
    if (ps3_env_once("PS3_FSLOG")) PS3_LOG(PS3_LOG_INFO, "fs", "mkdir '%s' -> '%s' (%s)\n", gpath, hpath, r == 0 ? "OK" : "FAIL");
    ctx->gpr[3] = CELL_OK;   /* existing dir is benign for boot */
}
static void cellFsRmdir(ppu_context* ctx)  { ctx->gpr[3] = CELL_OK; }
static void cellFsUnlink(ppu_context* ctx)
{
    const uint32_t address = (uint32_t)ctx->gpr[3];
    /* RPCS3 6e4b784d1 translate_to_str: a null path is CELL_EFAULT. A pointer outside guest memory has no code the
     * console gives (the kernel faults on it), so it is the named stop, as guest_strcpy makes of a string that
     * never ends inside guest memory. */
    if (!address) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EFAULT; return; }
    if (!fs_range_ok(address, 1)) fs_range_stop("cellFsUnlink", "path", address, 1);
    /* RPCS3 translate_to_str (sys_fs.cpp:1107-1130): up to CELL_FS_MAX_FS_PATH_LENGTH (1024) characters, and
     * CELL_ENAMETOOLONG when no NUL is found in the first 1025 bytes. cellFsUnlink is sys_fs_unlink
     * (cellFs.cpp:160-166). guest_strcpy would cut the string at its buffer and unlink a different file. */
    char guest[FS_PATH_LENGTH_MAX + 1], host[1100];
    for (size_t i = 0;; i++) {
        if (i > FS_PATH_LENGTH_MAX) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENAMETOOLONG; return; }
        if (!fs_range_ok((uint64_t)address + i, 1)) fs_range_stop("cellFsUnlink", "path", (uint64_t)address + i, 1);
        guest[i] = (char)vm_base[address + i];
        if (!guest[i]) break;
    }
    host_path(host, sizeof host, guest);
    /* The answer is sys_fs_unlink's (include/ps3emu/fs_unlink.h): ENOENT for a missing path, EISDIR for a
     * directory, EIO for any other refusal. */
    int host_errno = 0;
    const uint32_t error = ps3_fs_unlink_host(host, &host_errno);
    if (error == CELL_EIO)
        PS3_LOG(PS3_LOG_INFO, "fs", "unlink '%s' failed (host errno %d) -> EIO\n", host, host_errno);
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)error;
}
static void cellFsFsync(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (FILE* f = fd_file(fd)) fflush(f);
    ctx->gpr[3] = CELL_OK;
}
/* (fd, u64 size). The libs/filesystem version looks fds up in its own table,
 * which cellFsOpen here never fills, so it always returned EBADF. */
static void cellFsFtruncate(ppu_context* ctx)
{
    FILE* f = fd_file((int)(uint32_t)ctx->gpr[3]);
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    fflush(f);
#ifdef _WIN32
    int rc = _chsize_s(_fileno(f), (long long)ctx->gpr[4]);
#else
    int rc = ftruncate(fileno(f), (off_t)ctx->gpr[4]);
#endif
    if (ps3_env_once("PS3_FSLOG")) PS3_LOG(PS3_LOG_INFO, "FSDBG", "cellFsFtruncate(fd=%d, size=0x%llX) -> %d\n",
                                     (int)(uint32_t)ctx->gpr[3], (unsigned long long)ctx->gpr[4], rc);
    ctx->gpr[3] = rc == 0 ? CELL_OK : (uint64_t)(int64_t)CELL_FS_EIO;
}
/* Grow an existing host file to `size` bytes without writing the new space.
 * SetEndOfFile (Windows) and truncate (POSIX) move the end of file, and the
 * filesystem reads the gap back as zeroes. The CRT's _chsize_s writes those
 * zeroes itself: about 5 s for a 1.75 GB file, which is what one title asks
 * for. Returns 0 when the file is now at least `size` long (a longer file is
 * left alone), 1 when there is no such file, -1 when it cannot be grown. */
static int host_grow_file(const char* path, uint64_t size)
{
    fs_stat64_t st;
    if (FS_STAT64(path, &st) != 0) return 1;
    if ((uint64_t)st.st_size >= size) return 0;
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER end;
    end.QuadPart = (LONGLONG)size;
    const BOOL ok = SetFilePointerEx(h, end, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    return ok ? 0 : -1;
#else
    return truncate(path, (off_t)size) == 0 ? 0 : -1;
#endif
}

/* cellFsAllocateFileAreaWithoutZeroFill(path, u64 size): reserve `size` bytes
 * for the file without clearing them, so that afterwards the file is that long.
 *
 * Accepting the call and doing nothing left the file at whatever length its
 * writes had reached, and a title that checks the length when it next opens
 * the file finds it short. Red Dead Redemption sizes its disk cache this way
 * (cache.dat under /dev_hdd1, 0x6FE02000 bytes), reads the size back on the
 * next boot, and rebuilt the whole cache on every boot ("Cache file is wrong
 * size").
 *
 * This allocates; it never shrinks, and a file already long enough costs one
 * stat (LBP calls it repeatedly while it warms its cache). A missing file is
 * not created and answers CELL_ENOENT: RPCS3 6e4b784d1 returns sys_fs_truncate's
 * answer for the path (cellFs.cpp:629-660, sys_fs.cpp:3038); CELL_OK here told
 * the title its cache file existed. */
static void cellFsAllocateFileAreaWithoutZeroFill(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    host_path(hpath, sizeof hpath, gpath);
    const uint64_t size = ctx->gpr[4];
    const int rc = host_grow_file(hpath, size);
    if (ps3_env_once("PS3_FSLOG"))
        fprintf(stderr, "[fs] AllocateFileAreaWithoutZeroFill '%s' -> '%s' size=0x%llX (%s)\n",
                gpath, hpath, (unsigned long long)size,
                rc == 0 ? "OK" : rc > 0 ? "no such file" : "FAIL");
    ctx->gpr[3] = rc == 0 ? CELL_OK : (uint64_t)(int64_t)(rc > 0 ? CELL_FS_ENOENT : CELL_FS_EIO);
}

/* Free space for (path, u32* block_size, u64* free_block_count). Unregistered,
 * the call took the unresolved-NID path: CELL_OK with both out-params left as
 * unwritten, so a title sizing a cache read whatever was there. Report a
 * 4 KiB block and 32 GiB free: enough for any cache, and the same on every host. */
static void cellFsGetFreeSize(ppu_context* ctx)
{
    uint32_t bs = (uint32_t)ctx->gpr[4], fb = (uint32_t)ctx->gpr[5];
    if (bs) vm_write32(bs, 4096);
    if (fb) vm_write64(fb, (32ULL << 30) / 4096);
    ctx->gpr[3] = CELL_OK;
}
static void cellFsChmod(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }   /* host ignores PS3 mode bits */
static void cellFsUtime(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }   /* timestamps are not modelled */
/* SDATA opened from an already-open fd is not implemented: fail visibly rather
 * than return CELL_OK with *sdata_fd never written. */
static void cellFsSdataOpenByFd(ppu_context* ctx)
{
    PS3_LOG_N(4, PS3_LOG_WARN, "fs", "cellFsSdataOpenByFd(fd=%d) not implemented -> CELL_ENOSYS\n",
                         (int)(uint32_t)ctx->gpr[3]);
    ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOSYS;
}

/* ---- cellFs AIO -----------------------------------------------------------
 *
 * Scott Pilgrim loads everything through cellFsAioRead: it opens gamedata.fat
 * with cellFsOpen and then never calls cellFsRead again. With the AIO NIDs
 * unresolved, its loader thread submitted requests that nothing ever completed
 * and the whole boot parked -- one open, no reads, and the main thread spinning.
 *
 * CellFsAio, big-endian, 0x28 bytes:
 *   0x00 u32 fd    0x08 u64 offset    0x10 u32 buf    0x18 u64 size    0x20 u64 user_data
 *
 * The completion callback is (CellFsAio* aio, s32 error, s32 id, u64 size).
 *
 * ponytail: the read runs synchronously and the callback fires before the
 * submit call returns. A title that submits, THEN arms the thing the callback
 * signals, would miss it -- move completion to a worker thread if one shows up.
 */
#ifdef _WIN32
#  define HOST_FSEEK64(f,off) _fseeki64((f), (__int64)(off), SEEK_SET)
#  define HOST_FTELL64(f)     _ftelli64(f)
#  define HOST_LOCKFILE(f)    _lock_file(f)
#  define HOST_UNLOCKFILE(f)  _unlock_file(f)
#else
#  define HOST_FSEEK64(f,off) fseeko((f), (off_t)(off), SEEK_SET)
#  define HOST_FTELL64(f)     ftello(f)
#  define HOST_LOCKFILE(f)    flockfile(f)
#  define HOST_UNLOCKFILE(f)  funlockfile(f)
#endif

static uint32_t guest_be32(uint32_t a)
{
    if (!fs_range_ok(a, 4)) return 0;
    return ((uint32_t)vm_base[a] << 24) | ((uint32_t)vm_base[a+1] << 16) |
           ((uint32_t)vm_base[a+2] << 8) | (uint32_t)vm_base[a+3];
}
static uint64_t guest_be64(uint32_t a)
{
    return ((uint64_t)guest_be32(a) << 32) | guest_be32(a + 4);
}

static void cellFsAioInit(ppu_context* ctx)   { ctx->gpr[3] = CELL_OK; }
static void cellFsAioFinish(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }
/* Nothing is ever outstanding, so a cancel has nothing to find. Real cellFs
 * answers CELL_FS_ENOENT for an unknown id and callers treat that as "already
 * done", which is exactly true here. */
static void cellFsAioCancel(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }

/* One id space for reads and writes: the completion callback and cellFsAioCancel tell requests apart by id alone,
 * and with a counter each a read and a write in flight together could both be id 1 (upstream #700, c66cec6f). */
static std::atomic<int32_t> s_aio_next_id{1};

static void cellFsAioRead(ppu_context* ctx)
{
    uint32_t aio    = (uint32_t)ctx->gpr[3];
    uint32_t id_ptr = (uint32_t)ctx->gpr[4];
    uint32_t cb_opd = (uint32_t)ctx->gpr[5];

    int      fd     = (int)guest_be32(aio + 0x00);
    uint64_t offset = guest_be64(aio + 0x08);
    uint32_t buf    = guest_be32(aio + 0x10);
    uint64_t size   = guest_be64(aio + 0x18);

    const int32_t id = s_aio_next_id++;
    if (id_ptr) vm_write32(id_ptr, (uint32_t)id);

    size_t n = 0;
    int32_t err = CELL_FS_ENOENT;
    if (FILE* f = fd_file(fd)) {
        if (!buf) {
            /* No copyout at guest address 0 (the null page is not a buffer). */
            err = (int32_t)0x8001000Du; /* CELL_EFAULT */
            ctx->gpr[3] = CELL_OK;
            if (getenv("PS3_FSLOG"))
                PS3_LOG(PS3_LOG_WARN, "fs", "aio read id=%d fd=%d off=%llu size=%llu -> EFAULT (null buffer)\n",
                        id, fd, (unsigned long long)offset, (unsigned long long)size);
            if (cb_opd) ps3_invoke_guest(cb_opd, aio, (uint64_t)(int64_t)err,
                                         (uint64_t)(int64_t)id, (uint64_t)n, 0, 0, 0, 0);
            return;
        }
        /* As in cellFsRead: a span that leaves the arena is a named stop, never
         * a clamped read of the part that fits. */
        if (!fs_range_ok(buf, size)) fs_range_stop("cellFsAioRead", "buf", buf, size);
        fs_prefault(buf, size);
        /* AIO reads are absolute: they must not move the fd's own position,
         * and the guest issues them from several threads at once (FIOS media
         * threads, audio streamers) on the same fd as its plain reads. A bare
         * seek + read on the shared FILE* did both: another thread's seek
         * could land between the two, and the next cellFsRead continued from
         * wherever the AIO stopped. Hold the stream lock across seek, read
         * and the seek back (stdio's own calls take the same lock). */
        HOST_LOCKFILE(f);
        const int64_t pos = (int64_t)HOST_FTELL64(f);
        if (HOST_FSEEK64(f, offset) == 0) {
            vm_note_write(buf, (size_t)size);   /* kernel write into guest memory (#727) */
            n = fread(vm_base + buf, 1, (size_t)size, f);
            vm_note_write_check(n < (size_t)size && ferror(f) && errno == EFAULT, "cellFsAioRead");
        }
        if (pos >= 0) HOST_FSEEK64(f, pos);
        HOST_UNLOCKFILE(f);
        err = CELL_OK;
    }
    if (ps3_env_once("PS3_FSLOG"))
        PS3_LOG(PS3_LOG_INFO, "fs", "aio read id=%d fd=%d off=%llu size=%llu -> %zu\n",
                id, fd, (unsigned long long)offset, (unsigned long long)size, n);

    ctx->gpr[3] = CELL_OK;
    if (cb_opd) ps3_invoke_guest(cb_opd, aio, (uint64_t)(int64_t)err,
                                 (uint64_t)(int64_t)id, (uint64_t)n, 0, 0, 0, 0);
}

/* MLB The Show 16 copies files into its /dev_hdd1 cache in 64 KB chunks, each
 * written with cellFsAioWrite, and polls a status word its completion callback
 * sets. With the NID unresolved the submit "succeeded", nothing was written, the
 * callback never ran, and the boot parked on the first screen. Same contract and
 * the same synchronous-completion shortcut as cellFsAioRead above. */
static void cellFsAioWrite(ppu_context* ctx)
{
    uint32_t aio    = (uint32_t)ctx->gpr[3];
    uint32_t id_ptr = (uint32_t)ctx->gpr[4];
    uint32_t cb_opd = (uint32_t)ctx->gpr[5];

    /* The CellFsAio fields read below (0x20 bytes) and the id written back are guest spans the title chose; guest_be32
     * answers 0 for a bad one and vm_write32 does not look, so the span is bounded here, before either is touched. */
    if (!fs_range_ok(aio, 0x20)) fs_range_stop("cellFsAioWrite", "aio", aio, 0x20);
    if (id_ptr && !fs_range_ok(id_ptr, 4)) fs_range_stop("cellFsAioWrite", "id", id_ptr, 4);

    int      fd     = (int)guest_be32(aio + 0x00);
    uint64_t offset = guest_be64(aio + 0x08);
    uint32_t buf    = guest_be32(aio + 0x10);
    uint64_t size   = guest_be64(aio + 0x18);

    const int32_t id = s_aio_next_id++;
    if (id_ptr) vm_write32(id_ptr, (uint32_t)id);

    size_t n = 0;
    int32_t err = CELL_FS_ENOENT;
    if (FILE* f = fd_file(fd)) {
        /* As in cellFsAioRead / cellFsWrite: no copyin at guest address 0, and a
         * span that leaves the arena is a named stop, never a clamped write. */
        if (!buf) {
            err = (int32_t)0x8001000Du; /* CELL_EFAULT */
            ctx->gpr[3] = CELL_OK;
            if (cb_opd) ps3_invoke_guest(cb_opd, aio, (uint64_t)(int64_t)err,
                                         (uint64_t)(int64_t)id, (uint64_t)n, 0, 0, 0, 0);
            return;
        }
        if (!fs_range_ok(buf, size)) fs_range_stop("cellFsAioWrite", "buf", buf, size);
        /* The kernel reads the source here, and a read of a reserved page fails where a guest load would fault it
         * in: fwrite from pages the title has not touched wrote nothing (as cellFsWrite and cellFsAioRead). */
        fs_prefault(buf, size);
        /* Absolute, like the read: leave the fd's own position where it was. */
        HOST_LOCKFILE(f);
        const int64_t pos = (int64_t)HOST_FTELL64(f);
        if (HOST_FSEEK64(f, offset) == 0)
            n = fwrite(vm_base + buf, 1, (size_t)size, f);
        fflush(f);
        if (pos >= 0) HOST_FSEEK64(f, pos);
        HOST_UNLOCKFILE(f);
        err = (n == size) ? CELL_OK : CELL_FS_EIO;
    }
    if (ps3_env_once("PS3_FSLOG"))
        PS3_LOG(PS3_LOG_INFO, "fs", "aio write id=%d fd=%d off=%llu size=%llu -> %zu\n",
                id, fd, (unsigned long long)offset, (unsigned long long)size, n);

    ctx->gpr[3] = CELL_OK;
    if (cb_opd) ps3_invoke_guest(cb_opd, aio, (uint64_t)(int64_t)err,
                                 (uint64_t)(int64_t)id, (uint64_t)n, 0, 0, 0, 0);
}

/* cellFsReadWithOffset(fd, u64 offset, void* buf, u64 size, u64* nread) and
 * cellFsWriteWithOffset(fd, u64 offset, const void* buf, u64 size, u64* nwrite):
 * the synchronous form of the AIO pair above, I/O at an absolute offset.
 *
 * Unregistered, both took the unresolved-NID path: CELL_OK with nothing read or
 * written and the count left unwritten. Red Dead Redemption reads its RPF
 * archives with the first -- the header came back empty and the title stopped
 * at "Cannot load archive" -- and writes its disk cache with the second, which
 * stayed at 0 bytes. Like the AIO pair they leave the fd's own position where it
 * was, under the stream lock, since a title issues them from several threads on
 * one fd alongside its plain reads. */
static void fs_io_with_offset(ppu_context* ctx, int write)
{
    const char* api = write ? "cellFsWriteWithOffset" : "cellFsReadWithOffset";
    int      fd     = (int)(uint32_t)ctx->gpr[3];
    uint64_t offset = ctx->gpr[4];
    uint32_t buf    = (uint32_t)ctx->gpr[5];
    uint64_t size   = ctx->gpr[6];
    uint32_t n_ptr  = (uint32_t)ctx->gpr[7];
    /* The count is stored by every outcome below (0 for a refusal, RPCS3 6e4b784d1 cellFs.cpp:471-474), so its span
     * is checked before the first store. */
    if (n_ptr && !fs_range_ok(n_ptr, 8)) fs_range_stop(api, "n", n_ptr, 8);
    /* RPCS3 cellFs.cpp:488-492: a null write buffer is CELL_EFAULT with *nwrite = 0, before the fd is looked at (a
     * bad fd with a null buffer is EFAULT, not EBADF). The write used to go through, copying guest page 0 into the
     * file and answering CELL_OK. */
    if (write && !buf) {
        if (n_ptr) vm_write64(n_ptr, 0);
        ctx->gpr[3] = 0x8001000Du;   /* CELL_EFAULT */
        return;
    }
    FILE* f = fd_file(fd);
    const int flags = f ? fd_flags(fd) : -1;
    /* RPCS3 sys_fs.cpp:2498-2511 (the fcntl both calls are): a read on a write-only file, a write on a file not
     * opened for writing and a write on an append-mode file are EBADF, count 0. A read-only mount (EROFS, :2513) is
     * not modelled. */
    const bool refused = !f || (flags >= 0 && (write ? (!(flags & CELL_FS_O_ACCMODE) || (flags & CELL_FS_O_APPEND))
                                                      : (flags & CELL_FS_O_WRONLY) != 0));
    if (refused) {
        if (n_ptr) vm_write64(n_ptr, 0);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF;
        return;
    }
    /* The same rule as cellFsRead / cellFsWrite: every span the title chose is guest memory in full, or a named
     * stop before anything is touched. `size` is never clamped to the arena: that read or wrote a prefix of what
     * the title asked for and reported success, and a span past the arena would have underflowed. The count is
     * written after the transfer, so its span is checked first too, not left to fault once the file has changed. */
    /* A null read buffer with bytes to read: RPCS3 has no answer for it. cellFsReadWithOffset does no null check
     * (cellFs.cpp:448-482) and the fcntl hands the buffer to lv2_file::op_read (sys_fs.cpp:756-786), whose fast path
     * needs a writable page and whose fallback memcpy's into buf.get_ptr(): guest page 0 is not mapped on the
     * console, so the read faults. There is no code to answer with, and a read into page 0 here would write guest
     * memory the console never has: the named stop. With size 0 nothing is stored and the call succeeds with
     * count 0, as RPCS3's loop does. */
    if (!write && !buf && size) fs_range_stop(api, "buf", 0, size);
    if (!fs_range_ok(buf, size)) fs_range_stop(api, "buf", buf, size);
    fs_prefault(buf, size);   /* written by a read, read by a write: same reserved-page trap */
    size_t n = 0;
    int write_failed = 0;
    HOST_LOCKFILE(f);
    const int64_t pos = (int64_t)HOST_FTELL64(f);
    if (write) clearerr(f);   /* ferror() is sticky: an old failure would look like this write's */
    if (HOST_FSEEK64(f, offset) == 0) {
        if (write) {
            n = fwrite(vm_base + buf, 1, (size_t)size, f);
            /* Flushed under the lock so the failure is this write's, and before the seek back. */
            write_failed = ps3_fs_write_failed(n, size, fflush(f) != 0 || ferror(f));
        } else {
            vm_note_write(buf, (size_t)size);   /* kernel write into guest memory */
            n = fread(vm_base + buf, 1, (size_t)size, f);
            vm_note_write_check(n < (size_t)size && ferror(f) && errno == EFAULT, api);
        }
    } else if (write) {
        write_failed = ps3_fs_write_failed(0, size, 0);
    }
    if (pos >= 0) HOST_FSEEK64(f, pos);
    HOST_UNLOCKFILE(f);
    if (ps3_env_once("PS3_FSLOG"))
        PS3_LOG(PS3_LOG_INFO, "fs", "%s@ fd=%d off=%llu size=%llu -> %zu\n", write ? "write" : "read",
                fd, (unsigned long long)offset, (unsigned long long)size, n);
    if (n_ptr) vm_write64(n_ptr, n);
    ctx->gpr[3] = write_failed ? (uint64_t)(int64_t)CELL_FS_EIO : CELL_OK;
}
static void cellFsReadWithOffset(ppu_context* ctx)  { fs_io_with_offset(ctx, 0); }
static void cellFsWriteWithOffset(ppu_context* ctx) { fs_io_with_offset(ctx, 1); }

extern "C" void ppu_fs_register(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("cellFsOpen"),     "cellFsOpen",     cellFsOpen);
    /* Register by the literal import NID: LBP imports 0xB1840B53 for
     * cellFsSdataOpen, which ps3_compute_nid("cellFsSdataOpen") does NOT match
     * (the real exported symbol name differs from the friendly name). */
    ps3_hle_register_ctx(0xB1840B53u, "cellFsSdataOpen", cellFsSdataOpen);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsClose"),    "cellFsClose",    cellFsClose);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRead"),     "cellFsRead",     cellFsRead);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsWrite"),    "cellFsWrite",    cellFsWrite);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsReadWithOffset"),  "cellFsReadWithOffset",  cellFsReadWithOffset);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsWriteWithOffset"), "cellFsWriteWithOffset", cellFsWriteWithOffset);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsLseek"),    "cellFsLseek",    cellFsLseek);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFGetBlockSize"), "cellFsFGetBlockSize", cellFsFGetBlockSize);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsGetBlockSize"),  "cellFsGetBlockSize",  cellFsGetBlockSize);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsStat"),     "cellFsStat",     cellFsStat);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFstat"),    "cellFsFstat",    cellFsFstat);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsOpendir"),  "cellFsOpendir",  cellFsOpendir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsGetDirectoryEntries"), "cellFsGetDirectoryEntries", cellFsGetDirectoryEntries);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsReaddir"),  "cellFsReaddir",  cellFsReaddir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsClosedir"), "cellFsClosedir", cellFsClosedir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsMkdir"),    "cellFsMkdir",    cellFsMkdir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRmdir"),    "cellFsRmdir",    cellFsRmdir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsUnlink"),   "cellFsUnlink",   cellFsUnlink);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFsync"),    "cellFsFsync",    cellFsFsync);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFtruncate"), "cellFsFtruncate", cellFsFtruncate);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsGetFreeSize"),   "cellFsGetFreeSize",   cellFsGetFreeSize);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsChmod"),         "cellFsChmod",         cellFsChmod);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsUtime"),         "cellFsUtime",         cellFsUtime);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsSdataOpenByFd"), "cellFsSdataOpenByFd", cellFsSdataOpenByFd);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsAllocateFileAreaWithoutZeroFill"),
                         "cellFsAllocateFileAreaWithoutZeroFill", cellFsAllocateFileAreaWithoutZeroFill);
    /* AIO by literal import NID -- ps3_compute_nid() of the friendly name does
     * not match the exported symbols. */
    ps3_hle_register_ctx(0xDB869F20u, "cellFsAioInit",   cellFsAioInit);
    ps3_hle_register_ctx(0x9F951810u, "cellFsAioFinish", cellFsAioFinish);
    ps3_hle_register_ctx(0xC1C507E7u, "cellFsAioRead",   cellFsAioRead);
    ps3_hle_register_ctx(0x4CEF342Eu, "cellFsAioWrite",  cellFsAioWrite);
    ps3_hle_register_ctx(0x7F13FC8Cu, "cellFsAioCancel", cellFsAioCancel);
}
