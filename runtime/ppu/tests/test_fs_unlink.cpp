/* Synthetic stale-cache recovery through the actual PPU cellFs HLE: the file goes, a missing or null path answers
 * as the console does (ENOENT, EFAULT), a directory is EISDIR, and a path outside guest memory is a named stop. */
#include "ppu_fs.cpp"
#include "ps3emu/error_codes.h"
#include <filesystem>
#include <fstream>
#include <string>
#ifndef _WIN32
#  include <sys/wait.h>
#  include <unistd.h>
#endif

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size;
/* No other I/O operation is exercised by this fixture. */
const char* edat_resolve(const char*, char*, size_t) { abort(); }
void vm_write32(uint64_t, uint32_t) { abort(); }
void vm_write64(uint64_t, uint64_t) { abort(); }
void ppu_guest_caller(char*, size_t) { abort(); }
void ydkj_host_bt(const char*) { abort(); }
void ps3_vfs_ps3game_fallback(char*, size_t) { abort(); }
ps3_guest_caller_fn g_ps3_guest_caller;
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
/* The runtime's stop for an HLE handler handed a guest span outside guest memory: the fixture prints the same
 * machine line and ends the process the same way, so a refusal is told from an error code (the child of a fork). */
PS3_NORETURN void ps3_stop_guest_range(const char* func, const char* arg,
                                       uint64_t ea, uint64_t size, uint64_t limit)
{
    fprintf(stderr, "kind=hle-guest-range func=%s arg=%s ea=0x%llX size=0x%llX limit=0x%llX\n",
            func, arg, (unsigned long long)ea, (unsigned long long)size, (unsigned long long)limit);
    fflush(stderr);
    _Exit(PS3_STOP_EXIT_CODE);
}
}

int main(int argc, char** argv)
{
    if (argc != 2) return 90;
    const auto root = std::filesystem::path(argv[1]);
#ifdef _WIN32
    _putenv_s("PS3_HDD1_ROOT", root.string().c_str());
#else
    setenv("PS3_HDD1_ROOT", root.string().c_str(), 1);
#endif
    std::filesystem::create_directories(root / "cache");
    const auto stale = root / "cache/stale.idx";
    std::ofstream(stale) << "synthetic invalid cache";
    const auto original = root / "original.dat";
    std::ofstream(original) << "preserve original";
    uint8_t memory[4096]{};
    vm_base = memory; ppu_vm_size = sizeof memory;
    strcpy((char*)memory + 256, "/dev_hdd1/cache/stale.idx");
    ppu_context ctx{};
    ctx.gpr[3] = 256; cellFsUnlink(&ctx);
    if (ctx.gpr[3] != CELL_OK || std::filesystem::exists(stale)) return 41;
    ctx.gpr[3] = 256; cellFsUnlink(&ctx);
    if ((int32_t)ctx.gpr[3] != (int32_t)CELL_ENOENT) return 42;
    /* RPCS3: a null path is CELL_EFAULT (it was EINVAL). */
    ctx.gpr[3] = 0; cellFsUnlink(&ctx);
    if ((int32_t)ctx.gpr[3] != (int32_t)CELL_EFAULT) return 43;
#ifndef _WIN32
    /* A pointer outside guest memory, and a string that never ends inside it, are named stops: the console
     * faults on them, so there is no code to answer with (it was EFAULT for the first). */
    const auto stops = [&](uint32_t address, const char* arg) {
        int fds[2];
        if (pipe(fds) != 0) return false;
        const pid_t pid = fork();
        if (pid == 0) {
            close(fds[0]); dup2(fds[1], STDERR_FILENO); close(fds[1]);
            ppu_context child{};
            child.gpr[3] = address; cellFsUnlink(&child);
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
        return WIFEXITED(status) && WEXITSTATUS(status) == PS3_STOP_EXIT_CODE &&
               err.find("kind=hle-guest-range") != std::string::npos &&
               err.find(std::string("arg=") + arg) != std::string::npos;
    };
    if (!stops(sizeof memory, "path")) return 45;
    memset(memory + sizeof memory - 8, 'a', 8);   /* 8 bytes up to the end of memory, no NUL */
    if (!stops(sizeof memory - 8, "path")) return 47;
    memset(memory + sizeof memory - 8, 0, 8);
#endif
    /* A directory is CELL_EISDIR (sys_fs_unlink's mapping), and it is left alone. */
    strcpy((char*)memory + 256, "/dev_hdd1/cache");
    ctx.gpr[3] = 256; cellFsUnlink(&ctx);
    if ((int32_t)ctx.gpr[3] != (int32_t)CELL_EISDIR || !std::filesystem::is_directory(root / "cache")) return 46;
    if (!std::filesystem::exists(original)) return 44;
    puts("PASS: stale cache removed; missing/null paths report errors; unrelated file preserved");
    return 0;
}
