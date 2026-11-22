#!/usr/bin/env python3
"""The raw sys_fs_* calls and cellFs share one fd space.

On hardware cellFsOpen and the sys_fs syscalls share one kernel fd space.
MLB The Show 16's Bink SDK opens the .BIK with cellFsOpen and then reads it
with sys_fs_lseek/sys_fs_read. Our runtime kept two tables, so the read hit an
empty sys_fs slot, returned EBADF silently, and BinkOpen reported "Not a Bink
file." with no file read at all.

This fixture compiles the real runtime/syscalls/sys_fs.c and supplies the
cellFs side the way ppu_fs.cpp does (ppu_cellfs_file_for_fd, _fd_owned, _close,
_dir_next): a file at fd 12 and a directory at fd 13, backed by host files. It
checks, with no game run:
  - sys_fs_read / write / lseek / fstat / ftruncate / close reach the cellFs
    file; sys_fs_readdir / closedir reach the cellFs directory;
  - sys_fs_open and sys_fs_opendir never hand out 12 or 13, nor a number twice,
    and a directory's number comes from the same space as a file's;
  - what the cellFs side calls for a number sys_fs holds (sys_fs_file_for_fd,
    sys_fs_fd_active, sys_fs_dir_next, sys_fs_close_fd) answers for it.
tools/test_ppu_fs_sys_bridge.py runs both real tables together, in both
directions and across threads.
"""
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runtime_link  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: standalone fixture requires the POSIX C toolchain')
    raise SystemExit(0)

FIXTURE = r'''
#include "runtime/syscalls/sys_fs.h"
#include "runtime/memory/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
const char* cellGame_get_hdd_title_id(void) { return NULL; }
const char* edat_resolve(const char* host_path, char* buf, size_t cap)
{ (void)buf; (void)cap; return host_path; }

/* ---- the cellFs table, as ppu_fs.cpp presents it to sys_fs.c: a file at fd 12, a directory at fd 13 ---- */
static FILE* g_cellfs_file;
static int   g_cellfs_dir_pos = -1;   /* -1: closed; else how many entries were read */
static char  g_dir_host[600];

FILE* ppu_cellfs_file_for_fd(int fd) { return fd == 12 ? g_cellfs_file : NULL; }
int ppu_cellfs_fd_owned(int fd)
{
    return (fd == 12 && g_cellfs_file) || (fd == 13 && g_cellfs_dir_pos >= 0);
}
int ppu_cellfs_close(int fd, int is_dir)
{
    if (!is_dir && fd == 12 && g_cellfs_file) { fclose(g_cellfs_file); g_cellfs_file = NULL; return 1; }
    if (is_dir && fd == 13 && g_cellfs_dir_pos >= 0) { g_cellfs_dir_pos = -1; return 1; }
    return 0;
}
int ppu_cellfs_dir_next(int fd, char* name, size_t cap, char* dir, size_t dir_cap)
{
    static const char* const entries[] = { ".", "inner.txt" };
    if (fd != 13 || g_cellfs_dir_pos < 0) return -1;
    snprintf(dir, dir_cap, "%s", g_dir_host);
    if (g_cellfs_dir_pos >= 2) return 0;
    snprintf(name, cap, "%s", entries[g_cellfs_dir_pos++]);
    return 1;
}

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint32_t be32(uint32_t ea) {
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(uint32_t ea) {
    return ((uint64_t)be32(ea) << 32) | be32(ea + 4);
}

/* sys_fs_open("/dev_bdvd/<name>", O_RDONLY) -> the fd, or -1. */
static int open_named(const char* name) {
    ppu_context c;
    memset(&c, 0, sizeof c);
    snprintf((char*)vm_base + 0x500, 64, "/dev_bdvd/%s", name);
    c.gpr[3] = 0x500; c.gpr[4] = 0; c.gpr[5] = 0x600;
    return sys_fs_open(&c) == CELL_OK ? (int)be32(0x600) : -1;
}
static int open_dir(void) {
    ppu_context c;
    memset(&c, 0, sizeof c);
    strcpy((char*)vm_base + 0x500, "/dev_bdvd/sub");
    c.gpr[3] = 0x500; c.gpr[4] = 0x600;
    return sys_fs_opendir(&c) == CELL_OK ? (int)be32(0x600) : -1;
}
static int64_t call1(int64_t (*f)(ppu_context*), uint64_t a) {
    ppu_context c;
    memset(&c, 0, sizeof c);
    c.gpr[3] = a;
    return f(&c);
}
/* sys_fs_readdir(fd): the type byte and name of the entry, or "" at the end; the return code. */
static int64_t readdir_of(int fd, int* type, char* name) {
    ppu_context c;
    memset(&c, 0, sizeof c);
    memset(vm_base + 0x700, 0, 0x102);
    c.gpr[3] = (uint64_t)fd; c.gpr[4] = 0x700; c.gpr[5] = 0x800;
    int64_t rc = sys_fs_readdir(&c);
    *type = vm_base[0x700];
    snprintf(name, 256, "%s", be64(0x800) ? (const char*)vm_base + 0x702 : "");
    return rc;
}

int main(void) {
    ppu_vm_size = 0x200000;
    vm_base = (uint8_t*)mmap(NULL, ppu_vm_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm_base == MAP_FAILED) return 2;

    const char payload[] = "BIKi-block-header-0123456789abcdef";
    char root[] = "/tmp/sysfs-bridge-root-XXXXXX";
    if (!mkdtemp(root)) return 2;
    snprintf(g_sys_fs_root, sizeof g_sys_fs_root, "%s", root);
    char sub[700], host_path[700], plain[700], inner[700], a_txt[700], b_txt[700];
    snprintf(sub, sizeof sub, "%s/sub", root);
    if (mkdir(sub, 0755) != 0) return 2;
    snprintf(g_dir_host, sizeof g_dir_host, "%s", sub);
    snprintf(host_path, sizeof host_path, "%s/movie.bik", root);
    snprintf(plain, sizeof plain, "%s/plain.bin", root);
    snprintf(inner, sizeof inner, "%s/inner.txt", sub);
    snprintf(a_txt, sizeof a_txt, "%s/a.txt", sub);
    snprintf(b_txt, sizeof b_txt, "%s/b.txt", sub);
    FILE* w = fopen(host_path, "wb");
    if (!w) return 2;
    fwrite(payload, 1, sizeof payload - 1, w);
    fclose(w);
    w = fopen(plain, "wb"); fputs("p", w); fclose(w);
    w = fopen(inner, "wb"); fputs("i", w); fclose(w);
    w = fopen(a_txt, "wb"); fputs("a", w); fclose(w);
    w = fopen(b_txt, "wb"); fputs("b", w); fclose(w);
    g_cellfs_file = fopen(host_path, "r+b");
    if (!g_cellfs_file) return 2;
    g_cellfs_dir_pos = 0;

    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);

    /* sys_fs_read(12, guest 0x100, 0x2c, &nread@0x80) on a cellFs fd. */
    ctx.gpr[3] = 12; ctx.gpr[4] = 0x100; ctx.gpr[5] = 0x20; ctx.gpr[6] = 0x80;
    CHECK(sys_fs_read(&ctx) == CELL_OK);
    CHECK(be64(0x80) == 0x20);
    CHECK(memcmp(vm_base + 0x100, payload, 0x20) == 0);

    /* lseek on the same fd: rewind, then 4 bytes. */
    ctx.gpr[3] = 12; ctx.gpr[4] = 0; ctx.gpr[5] = CELL_FS_SEEK_SET; ctx.gpr[6] = 0x88;
    CHECK(sys_fs_lseek(&ctx) == CELL_OK);
    CHECK(be64(0x88) == 0);
    ctx.gpr[3] = 12; ctx.gpr[4] = 4; ctx.gpr[5] = CELL_FS_SEEK_SET; ctx.gpr[6] = 0x90;
    CHECK(sys_fs_lseek(&ctx) == CELL_OK);
    CHECK(be64(0x90) == 4);

    /* And a read from the new offset sees the bridged stream position. */
    memset(vm_base + 0x200, 0, 8);
    ctx.gpr[3] = 12; ctx.gpr[4] = 0x200; ctx.gpr[5] = 4; ctx.gpr[6] = 0x98;
    CHECK(sys_fs_read(&ctx) == CELL_OK);
    CHECK(be64(0x98) == 4);
    CHECK(memcmp(vm_base + 0x200, payload + 4, 4) == 0);

    /* fstat on the bridged fd reports the real size at +0x24. */
    ctx.gpr[3] = 12; ctx.gpr[4] = 0x300;
    CHECK(sys_fs_fstat(&ctx) == CELL_OK);
    CHECK(be64(0x300 + 0x24) == (uint64_t)(sizeof payload - 1));

    /* sys_fs_write on a cellFs fd reaches the file, not the "pretend it
     * worked" path for a bad fd. */
    ctx.gpr[3] = 12; ctx.gpr[4] = 0; ctx.gpr[5] = CELL_FS_SEEK_SET; ctx.gpr[6] = 0x88;
    CHECK(sys_fs_lseek(&ctx) == CELL_OK);
    memcpy(vm_base + 0x400, "XXXX", 4);
    ctx.gpr[3] = 12; ctx.gpr[4] = 0x400; ctx.gpr[5] = 4; ctx.gpr[6] = 0xA0;
    CHECK(sys_fs_write(&ctx) == CELL_OK);
    CHECK(be64(0xA0) == 4);

    /* ftruncate on it shortens the host file. */
    ctx.gpr[3] = 12; ctx.gpr[4] = 8;
    CHECK(sys_fs_ftruncate(&ctx) == CELL_OK);
    { struct stat st; CHECK(stat(host_path, &st) == 0 && st.st_size == 8); }

    /* One fd space, allocation side: sys_fs hands out neither 12 (a cellFs file) nor 13 (a cellFs directory), and
     * no number twice. Slots 3..11 are free, 12 and 13 are skipped, and the next file is 14. */
    int fds[14];
    for (int i = 0; i < 14; i++) {
        fds[i] = open_named("plain.bin");
        CHECK(fds[i] >= 3 && fds[i] != 12 && fds[i] != 13);
        for (int j = 0; j < i; j++) CHECK(fds[i] != fds[j]);
    }
    CHECK(fds[8] == 11 && fds[9] == 14 && fds[13] == 18);

    /* A directory's number comes from the same space: the lowest nobody holds, here 19. */
    const int dfd = open_dir();
    CHECK(dfd == 19);
    /* A freed file number is taken by the next directory, and then no file may take it. */
    CHECK(call1(sys_fs_close, (uint64_t)fds[2]) == CELL_OK);          /* fd 5 */
    const int dfd2 = open_dir();
    CHECK(dfd2 == 5);
    const int next = open_named("plain.bin");
    CHECK(next == 20);

    /* What the cellFs side asks of the sys_fs table. */
    ps3_fs_fd_space_lock();
    CHECK(sys_fs_fd_active(fds[1]) && sys_fs_fd_active(dfd) && sys_fs_fd_active(dfd2));
    CHECK(!sys_fs_fd_active(12) && !sys_fs_fd_active(13) && !sys_fs_fd_active(2));
    ps3_fs_fd_space_unlock();
    CHECK(sys_fs_file_for_fd(fds[0]) != NULL && sys_fs_file_for_fd(12) == NULL && sys_fs_file_for_fd(dfd) == NULL);
    CHECK(sys_fs_dir_next(dfd2, NULL, 0, NULL, 0) == 1 && sys_fs_dir_next(fds[0], NULL, 0, NULL, 0) == -1);
    { char name[256], dir[600]; int a_seen = 0, b_seen = 0, entries = 0, rc;
      while ((rc = sys_fs_dir_next(dfd2, name, sizeof name, dir, sizeof dir)) == 1 && entries < 16) {
          CHECK(strstr(dir, "/sub") != NULL);   /* the host directory the entry is in */
          a_seen += strcmp(name, "a.txt") == 0; b_seen += strcmp(name, "b.txt") == 0; entries++;
      }
      CHECK(rc == 0 && a_seen == 1 && b_seen == 1 && entries == 5); }   /* ".", "..", a.txt, b.txt, inner.txt */
    CHECK(sys_fs_close_fd(dfd2, 1) == 1 && sys_fs_close_fd(dfd2, 1) == 0);
    CHECK(sys_fs_close_fd(fds[0], 0) == 1 && sys_fs_close_fd(fds[0], 0) == 0);
    CHECK(sys_fs_close_fd(12, 0) == 0 && sys_fs_close_fd(13, 1) == 0);   /* not sys_fs's to close */

    /* Directory side: sys_fs_readdir / closedir reach the cellFs directory at 13. */
    { int type; char name[256];
      CHECK(readdir_of(13, &type, name) == CELL_OK && type == 1 && strcmp(name, ".") == 0);
      CHECK(be64(0x800) == 0x102);
      CHECK(readdir_of(13, &type, name) == CELL_OK && type == 2 && strcmp(name, "inner.txt") == 0);
      CHECK(readdir_of(13, &type, name) == CELL_OK && be64(0x800) == 0);   /* the end */
      CHECK(call1(sys_fs_closedir, 13) == CELL_OK);
      CHECK(g_cellfs_dir_pos == -1);
      CHECK(call1(sys_fs_closedir, 13) == (int64_t)(int32_t)CELL_EBADF);
      CHECK(readdir_of(13, &type, name) == (int64_t)(int32_t)CELL_EBADF);
      CHECK(readdir_of(77, &type, name) == (int64_t)(int32_t)CELL_EBADF); }

    /* Close on the cellFs file: it is closed there, and the number is free again. */
    CHECK(call1(sys_fs_close, 12) == CELL_OK);
    CHECK(g_cellfs_file == NULL);
    CHECK(call1(sys_fs_close, 12) == (int64_t)(int32_t)CELL_EBADF);
    CHECK(call1(sys_fs_close, 250) == (int64_t)(int32_t)CELL_EBADF);
    { char back[4] = {0}; struct stat st;
      FILE* r = fopen(host_path, "rb");
      CHECK(r && fread(back, 1, 4, r) == 4 && memcmp(back, "XXXX", 4) == 0);
      fclose(r);
      CHECK(stat(host_path, &st) == 0 && st.st_size == 8); }
    /* Free again for sys_fs: 3 and 5 were released above, so the lowest numbers nobody holds are 3, 5, 12. */
    { const int fa = open_named("plain.bin"), fb = open_named("plain.bin"), fc = open_named("plain.bin");
      CHECK(fa == 3 && fb == 5 && fc == 12); }

    for (int fd = 3; fd < 300; fd++) (void)call1(sys_fs_close, (uint64_t)fd);
    unlink(host_path); unlink(plain); unlink(inner); unlink(a_txt); unlink(b_txt); rmdir(sub); rmdir(root);
    printf("PASS: %u sys_fs/cellFs fd bridge checks\n", checks);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='sysfs-bridge-') as name:
    tmp = Path(name)
    fixture = tmp / 'fixture.c'
    fixture.write_text(FIXTURE)
    binary = tmp / 'fixture'
    # sys_fs.c range-checks guest pointers (ps3_stop_guest_range) and logs: the real log, configuration and stops.
    # Its path translation maps /dev_flash through ps3_dev_flash.c.
    runtime_link.build([fixture, ROOT / 'runtime/syscalls/sys_fs.c', ROOT / 'runtime/platform/ps3_dev_flash.c'],
                       binary, tmp,
                       cflags=['-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
                               '-I' + str(ROOT), '-I' + str(ROOT / 'include')],
                       ldflags=(['-Wl,-dead_strip'] if platform.system() == 'Darwin'
                                else ['-Wl,--gc-sections']),
                       timeout=120)
    subprocess.run([str(binary)], check=True, timeout=10, cwd=tmp)
