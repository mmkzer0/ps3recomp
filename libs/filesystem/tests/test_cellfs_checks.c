/* cellFs HLE argument/error checks, compared with RPCS3 cellFs.cpp/sys_fs.cpp.
 *
 *   cc -std=gnu17 -I include -o /tmp/test_cellfs_checks \
 *      libs/filesystem/tests/test_cellfs_checks.c -Wl,-dead_strip
 */
#include "../cellFs.c"
#include <assert.h>
#include <sys/mman.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
unsigned int ppu_prof_resolve_host(void* p) { (void)p; return 0; }
const char* cellGame_get_hdd_title_id(void) { return 0; }
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
void ps3_stop_guest_range(const char* f, const char* a, uint64_t ea,
                          uint64_t size, uint64_t limit)
{ (void)f; (void)a; (void)ea; (void)size; (void)limit; abort(); }

int main(void)
{
    char root[] = "/tmp/ps3recomp-fs-checks-XXXXXX";
    assert(mkdtemp(root));
    char host[512];
    snprintf(host, sizeof(host), "%s/f", root);
    FILE* h = fopen(host, "wb"); assert(h);
    assert(fwrite("abc", 1, 3, h) == 3);
    fclose(h);

    vm_base = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);
    ppu_vm_size = 0x100000;
    cellfs_set_root_path(root);
    cellfs_add_path_mapping("/dev_hdd0/", root);
    strcpy((char*)vm_base + 0x100, "/dev_hdd0/f");
    strcpy((char*)vm_base + 0x120, "/dev_hdd0/missing");

    /* RPCS3 cellFs.cpp cellFsOpen: !fd -> EFAULT; a null path is EFAULT in
     * sys_fs.cpp translate_to_str. */
    assert(cellFsOpen(NULL, CELL_FS_O_RDONLY, (CellFsFd*)(uintptr_t)0x200, NULL, 0)
           == CELL_EFAULT);
    assert(cellFsOpen((void*)0x100, CELL_FS_O_RDONLY, NULL, NULL, 0) == CELL_EFAULT);

    assert(cellFsOpen((void*)0x100, CELL_FS_O_RDONLY,
                      (CellFsFd*)(uintptr_t)0x200, NULL, 0) == CELL_OK);
    CellFsFd fd = (CellFsFd)vm_read32(0x200);
    assert(cellFsOpen((void*)0x120, CELL_FS_O_RDONLY,
                      (CellFsFd*)(uintptr_t)0x200, NULL, 0) == CELL_ENOENT);

    /* RPCS3 sys_fs_read: unknown fd / null buf -> *nread = 0. */
    vm_write64(0x300, 0xDEADBEEF);
    assert(cellFsRead(-1, (void*)0x400, 4, (u64*)(uintptr_t)0x300) ==
           CELL_FS_ERROR_EBADF);
    assert(vm_read64(0x300) == 0);
    assert(cellFsRead(fd, NULL, 4, (u64*)(uintptr_t)0x300) == CELL_EFAULT);
    assert(vm_read64(0x300) == 0);
    assert(cellFsRead(fd, (void*)0x400, 3, (u64*)(uintptr_t)0x300) == CELL_OK);
    assert(vm_read64(0x300) == 3);
    assert(memcmp(vm_base + 0x400, "abc", 3) == 0);

    /* RPCS3 sys_fs_lseek/fstat: bad fd -> EBADF. */
    assert(cellFsLseek(-1, 0, CELL_FS_SEEK_SET, (u64*)(uintptr_t)0x308) ==
           CELL_FS_ERROR_EBADF);
    assert(cellFsFstat(-1, (CellFsStat*)(uintptr_t)0x500) == CELL_FS_ERROR_EBADF);

    struct { u64 pos; } out;
    assert(cellFsLseek(fd, 0, CELL_FS_SEEK_SET, (u64*)(uintptr_t)0x308) == CELL_OK);
    assert(vm_read64(0x308) == 0);
    assert(cellFsFstat(fd, (CellFsStat*)(uintptr_t)0x500) == CELL_OK);
    assert(cellFsClose(fd) == CELL_OK);
    assert(cellFsClose(fd) == CELL_FS_ERROR_EBADF);

    munmap(vm_base, 0x100000);
    unlink(host);
    rmdir(root);
    puts("test_cellfs_checks: OK");
    return 0;
}
