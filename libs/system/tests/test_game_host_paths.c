/*
 * ps3recomp - cellGame keeps host paths whole and creates nothing in Permit.
 *
 * A title's VFS stage root lives under TMPDIR, so the host path of its game
 * directory is as long as the operator's directories make it. cellGame used to
 * format host paths into the 128-byte guest size: for some root lengths
 * "<root>/<id>/USRDIR" was cut inside "USRDIR", and cellGameContentPermit then
 * mkdir'd the cut name ("U", "US", "USR", ...) next to the real USRDIR. Permit
 * also created the directories of content that did not exist, which RPCS3
 * (cellGame.cpp:984-990, d00d8023) never does.
 *
 * Contract checked here:
 *   - an installed HDD game with a long root: Permit returns the guest paths
 *     and leaves the game directory exactly as it was;
 *   - a check session for content that does not exist: Permit returns empty
 *     paths with CELL_OK and creates nothing;
 *   - a NULL out-buffer is CELL_GAME_ERROR_PARAM;
 *   - a host path longer than the host buffer stops (host-path-limit), and an
 *     out-buffer outside guest memory stops (hle-guest-range), each in a child.
 */
#include "../cellGame.c"

#include <dirent.h>
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
const char* ppu_vfs_root;

static const uint32_t ARENA = 0x10000;
static const uint32_t CIP_EA = 0x1000, USR_EA = 0x1100, DIR_EA = 0x1200;
static int fails;

#define CHECK(c, ...) do { const int ok_ = (c); \
                           printf(ok_ ? "[PASS] " : "[FAIL] "); \
                           printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

/* Entries of a directory other than . and .., or -1 when it cannot be read. */
static int count_entries(const char* path)
{
    DIR* d = opendir(path);
    if (!d) return -1;
    int n = 0;
    for (struct dirent* e; (e = readdir(d)) != NULL;)
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) n++;
    closedir(d);
    return n;
}

static void make_dir(const char* path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        perror(path);
        exit(1);
    }
}

/* Run `call` in a forked child and require the named stop `kind` with `field`
 * on its machine line. */
static void expect_stop(const char* what, const char* kind, const char* field,
                        void (*call)(void))
{
    int fds[2];
    if (pipe(fds) != 0) { perror("pipe"); exit(1); }
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        call();
        _exit(1);   /* returned: nothing was refused */
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
    CHECK(line && strstr(line, want) && strstr(line, field),
          "%s names itself%s%s", what, line ? ": " : " (no machine line)", line ? line : "");
}

static void set_overlong_root(void)
{
    static char root[CELL_GAME_HOST_PATH_MAX + 64];
    memset(root, 'r', sizeof root - 1);
    root[0] = '/';
    root[sizeof root - 1] = '\0';
    cellGame_set_content_path(root);
}

static void permit_outside_guest_memory(void)
{
    cellGameContentPermit((char*)(uintptr_t)CIP_EA, (char*)(uintptr_t)(ARENA - 16));
}

int main(void)
{
    vm_base = calloc(1, ARENA);
    ppu_vm_size = ARENA;
    if (!vm_base) return 1;
    /* base/game is the content root, as the title stages it ("<stage>/game").
     * A 109-character base makes "<root>/TEST00000/" 125 characters, so the old
     * 128-byte buffer kept "US" of "USRDIR". */
    char tmpdir[] = "/tmp/ps3recomp-game-host-XXXXXX";
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); return 1; }
    char base[CELL_GAME_HOST_PATH_MAX];
    size_t len = (size_t)snprintf(base, sizeof base, "%s/", tmpdir);
    while (len < 99) base[len++] = 'p';
    base[len] = '\0';
    make_dir(base);
    strcat(base, "/stagepath");
    make_dir(base);
    ppu_vfs_root = base;

    char root[CELL_GAME_HOST_PATH_MAX], game[CELL_GAME_HOST_PATH_MAX];
    char usrdir[CELL_GAME_HOST_PATH_MAX], missing[CELL_GAME_HOST_PATH_MAX];
    snprintf(root, sizeof root, "%s/game", base);
    snprintf(game, sizeof game, "%s/TEST00000", root);
    snprintf(usrdir, sizeof usrdir, "%s/USRDIR", game);
    snprintf(missing, sizeof missing, "%s/NODATA01", root);
    make_dir(root);
    make_dir(game);
    make_dir(usrdir);
    cellGame_set_content_path(root);
    cellGame_set_title_id("TEST00000");
    s_game_type = CELL_GAME_GAMETYPE_HDD;
    CHECK(strlen(usrdir) > CELL_GAME_PATH_MAX, "the fixture's USRDIR host path (%zu) "
          "exceeds the 128-byte guest size", strlen(usrdir));

    /* An installed HDD game: Permit names it and touches nothing. */
    CHECK(cellGameBootCheck(NULL, NULL, NULL, NULL) == CELL_OK, "BootCheck opens the session");
    CHECK(cellGameContentPermit((char*)(uintptr_t)CIP_EA, (char*)(uintptr_t)USR_EA) == CELL_OK,
          "Permit on the installed game returns CELL_OK");
    CHECK(strcmp((char*)vm_base + CIP_EA, "/dev_hdd0/game/TEST00000") == 0,
          "contentInfoPath is '%s'", (char*)vm_base + CIP_EA);
    CHECK(strcmp((char*)vm_base + USR_EA, "/dev_hdd0/game/TEST00000/USRDIR") == 0,
          "usrdirPath is '%s'", (char*)vm_base + USR_EA);
    CHECK(count_entries(game) == 1, "the game directory still holds only USRDIR (%d entries)",
          count_entries(game));

    /* Content that does not exist: empty paths, nothing created. */
    memcpy(vm_base + DIR_EA, "NODATA01", sizeof "NODATA01");
    CHECK(cellGameDataCheck(CELL_GAME_GAMETYPE_GAMEDATA, (char*)(uintptr_t)DIR_EA, NULL)
          == CELL_GAME_RET_NONE, "DataCheck reports the missing data as NONE");
    memset(vm_base + CIP_EA, 'x', 8);
    memset(vm_base + USR_EA, 'x', 8);
    CHECK(cellGameContentPermit((char*)(uintptr_t)CIP_EA, (char*)(uintptr_t)USR_EA) == CELL_OK,
          "Permit on missing data returns CELL_OK");
    CHECK(vm_base[CIP_EA] == 0 && vm_base[USR_EA] == 0, "Permit on missing data returns empty paths");
    CHECK(!dir_exists(missing), "Permit did not create the missing data directory");
    CHECK(count_entries(root) == 1, "the content root still holds only the game (%d entries)",
          count_entries(root));

    CHECK(cellGameContentPermit(NULL, (char*)(uintptr_t)USR_EA) == CELL_GAME_ERROR_PARAM,
          "a NULL contentInfoPath is CELL_GAME_ERROR_PARAM");
    CHECK(cellGameContentPermit((char*)(uintptr_t)CIP_EA, NULL) == CELL_GAME_ERROR_PARAM,
          "a NULL usrdirPath is CELL_GAME_ERROR_PARAM");

    expect_stop("an overlong content root", "host-path-limit",
                "func=cellGame_set_content_path", set_overlong_root);
    expect_stop("a usrdirPath crossing the arena end", "hle-guest-range",
                "arg=usrdirPath", permit_outside_guest_memory);

    rmdir(usrdir); rmdir(game); rmdir(root); rmdir(base);
    *strrchr(base, '/') = '\0';
    rmdir(base);
    rmdir(tmpdir);
    free(vm_base);
    printf(fails ? "FAILED: %d check(s)\n" : "cellGame host paths: all checks passed\n", fails);
    return fails ? 1 : 0;
}
