/*
 * ps3recomp - the answer to deleting a host file, shared by the unlink paths.
 *
 * sys_fs_unlink (runtime/syscalls/sys_fs.c), cellFsUnlink on the PPU HLE path (runtime/ppu/ppu_fs.cpp) and the
 * libs/filesystem one all delete a host file for the guest, so they answer alike: a path that names nothing is
 * CELL_ENOENT, a directory is CELL_EISDIR, and any other refusal is CELL_EIO. Only a missing path is ENOENT: the
 * host errno for a directory differs per OS (EISDIR, EPERM, EACCES), so the host is asked with stat.
 */
#ifndef PS3RECOMP_FS_UNLINK_H
#define PS3RECOMP_FS_UNLINK_H

#include "error_codes.h"
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#endif

/* The host buffer a guest path is translated into, for the unlinks. A guest path may be CELL_FS_MAX_FS_PATH_LENGTH
 * (1024) characters, and the host path is the VFS root (or a mount's root) in front of it, so a host buffer of the
 * guest's size cannot hold a legal path once the root is longer than a few characters, and the translators stop
 * (host-path-limit) on a path that does not fit. 4096 is Linux's PATH_MAX, the longest path a host unlink takes
 * there: a root of up to 511 characters (g_sys_fs_root) plus a 1024-character path fits with room to spare. Where the
 * host's own limit is lower (macOS: 1024) the host refuses a path that long and the unlink answers as for any
 * refusal. Lives here, not in sys_fs.h, because ppu_fs.cpp cannot include that one (its CELL_FS_O_* spellings
 * differ) and this header is the one both unlinks already share. */
#define PS3_FS_HOST_PATH_MAX 4096

/* Delete `host_path`: CELL_OK, or the CELL_* code above. *host_errno (when given) is the host's errno for a
 * refusal that the code does not name (CELL_EIO), else 0, for the caller's log. */
static inline uint32_t ps3_fs_unlink_host(const char* host_path, int* host_errno)
{
    if (host_errno) *host_errno = 0;
#ifdef _WIN32
    if (_unlink(host_path) == 0) return CELL_OK;
#else
    if (unlink(host_path) == 0) return CELL_OK;
#endif
    const int e = errno;
    struct stat st;
    if (stat(host_path, &st) != 0) return CELL_ENOENT;
    if ((st.st_mode & S_IFMT) == S_IFDIR) return CELL_EISDIR;   /* S_ISDIR is not in every CRT */
    if (host_errno) *host_errno = e;
    return CELL_EIO;
}

#endif /* PS3RECOMP_FS_UNLINK_H */
