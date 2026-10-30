/* Standalone unit tests: synthetic stacks and no runtime library. */
#include "host_fiber.h"
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <TargetConditionals.h>
#define STACK (4u * 1024u * 1024u)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
ps3_host_ctx hf_abi_runner, hf_abi_peer, hf_abi_a, hf_abi_b, hf_abi_c;
ps3_host_ctx hf_abi_got_a, hf_abi_got_b, hf_abi_got_c;
extern void hf_abi_run(void);
extern void hf_abi_peer_entry(void);
static uint64_t frames[3][2];
static void pattern(ps3_host_ctx* p, uint64_t seed, unsigned frame)
{
    for (unsigned i = 0; i < 10; ++i) p->x19_x28[i] = seed + i;
    for (unsigned i = 0; i < 8; ++i) p->d8_d15[i] = seed + 0x100 + i;
    p->fp = (uintptr_t)frames[frame];
}
static void same(const ps3_host_ctx* a, const ps3_host_ctx* b)
{
    CHECK(!memcmp(a->x19_x28, b->x19_x28, sizeof a->x19_x28));
    CHECK(a->fp == b->fp && a->sp == b->sp && !(b->sp & 15));
    CHECK(!memcmp(a->d8_d15, b->d8_d15, sizeof a->d8_d15));
}
static void abi(void)
{
    /* Assembly alone runs on the alternate stack, so no sanitizer C frames
     * exist there. It records registers before any C prologue can hide them. */
    void* stack = malloc(STACK); CHECK(stack);
    hf_abi_peer.sp = ((uintptr_t)stack + STACK) & ~(uintptr_t)15;
    hf_abi_peer.lr = (uintptr_t)hf_abi_peer_entry;
    pattern(&hf_abi_a, 0xA123456780000000ull, 0);
    pattern(&hf_abi_b, 0xB234567890000000ull, 1);
    pattern(&hf_abi_c, 0xC3456789A0000000ull, 2);
    hf_abi_b.sp = hf_abi_peer.sp;
    hf_abi_run();
    same(&hf_abi_a, &hf_abi_got_a); same(&hf_abi_b, &hf_abi_got_b);
    same(&hf_abi_c, &hf_abi_got_c);
    free(stack);
}
typedef struct Case { ps3_host_fiber f; ps3_host_runner r; unsigned step, id; } Case;
static void create(Case* c, void (*entry)(void*))
{ CHECK(ps3_host_fiber_create(&c->f, STACK, entry, c, 0x1000 + c->id) == 0); }
static void lifecycle_entry(void* arg)
{
    Case* c = arg; CHECK(c->step == 0 && c->f.state == PS3_HOST_FIBER_RUNNING);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(c->step == 2 * i); ++c->step;
        volatile unsigned sentinel = 0xC001 + i;
        ps3_host_fiber_yield(&c->f);
        CHECK(sentinel == 0xC001 + i && c->step == 2 * i + 2);
    }
    ++c->step; ps3_host_fiber_exit(&c->f);
}
static void lifecycle(void)
{
    for (unsigned round = 0; round < 10000; ++round) {
        Case c = {0}; create(&c, lifecycle_entry);
        CHECK(c.f.state == PS3_HOST_FIBER_NEW);
        for (unsigned i = 0; i < 3; ++i) {
            ps3_host_fiber_resume(&c.r, &c.f);
            CHECK(c.step == 2 * i + 1 && c.f.state == PS3_HOST_FIBER_SUSPENDED);
            ++c.step;
        }
        ps3_host_fiber_resume(&c.r, &c.f);
        CHECK(c.step == 7 && c.f.state == PS3_HOST_FIBER_EXITED);
        ps3_host_fiber_destroy(&c.f); ps3_host_fiber_destroy(&c.f);
    }
}
static unsigned sequence;
static void many_entry(void* arg)
{
    Case* c = arg;
    for (unsigned i = 0; i < 100; ++i) {
        CHECK(sequence++ == i * 64 + c->id);
        ps3_host_fiber_yield(&c->f);
    }
    ps3_host_fiber_exit(&c->f);
}
static void many(void)
{
    Case c[64] = {0};
    for (unsigned i = 0; i < 64; ++i) { c[i].id = i; create(&c[i], many_entry); }
    for (unsigned j = 0; j <= 100; ++j)
        for (unsigned i = 0; i < 64; ++i) ps3_host_fiber_resume(&c[i].r, &c[i].f);
    CHECK(sequence == 6400);
    for (unsigned i = 0; i < 64; ++i) ps3_host_fiber_destroy(&c[i].f);
}
static void frame_entry(void* arg)
{
    Case* c = arg; uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    uintptr_t low = (uintptr_t)c->f.map + c->f.guard;
    uintptr_t high = (uintptr_t)c->f.map + c->f.map_size;
    CHECK(!(sp & 15) && sp >= low && sp < high);
    uintptr_t* fp = __builtin_frame_address(0);
    unsigned depth = 0;
    while (fp[0]) {
        CHECK(++depth < 100 && (uintptr_t)fp >= low && (uintptr_t)fp + 16 <= high);
        CHECK(fp[0] > (uintptr_t)fp && fp[0] + 16 <= high);
        fp = (uintptr_t*)fp[0];
    }
    CHECK((uintptr_t)fp == high - 16 && fp[1] == 0);
    ps3_host_fiber_exit(&c->f);
}
__attribute__((noinline)) static unsigned deep_frames(Case* c, unsigned depth)
{
    volatile unsigned char bytes[16384];
    for (unsigned i = 0; i < sizeof bytes; ++i) bytes[i] = (unsigned char)(i + depth);
    if (depth) CHECK(deep_frames(c, depth - 1) == depth - 1);
    else ps3_host_fiber_yield(&c->f);
    for (unsigned i = 0; i < sizeof bytes; ++i) CHECK(bytes[i] == (unsigned char)(i + depth));
    return depth;
}
static void deep_entry(void* arg)
{ Case* c = arg; CHECK(deep_frames(c, 223) == 223); ps3_host_fiber_exit(&c->f); }
static void exit_entry(void* arg) { Case* c = arg; ++c->step; ps3_host_fiber_exit(&c->f); }
static void yield_entry(void* arg) { Case* c = arg; ps3_host_fiber_yield(&c->f); ps3_host_fiber_exit(&c->f); }
static void returned_entry(void* arg) { (void)arg; }
static void nested_entry(void* arg) { Case* c = arg; ps3_host_fiber_resume(&c->r, &c->f); }
static void* foreign(void* arg) { Case* c = arg; ps3_host_runner r = {0}; ps3_host_fiber_resume(&r, &c->f); return NULL; }
static void stop_mode(const char* mode)
{
    Case c = {0};
    if (!strcmp(mode, "stop_uncreated")) ps3_host_fiber_resume(&c.r, &c.f);
    if (!strcmp(mode, "stop_size")) {
        (void)ps3_host_fiber_create(&c.f, 1, exit_entry, &c, 0x1000); CHECK(0);
    }
    create(&c, !strcmp(mode, "stop_entry_returned") ? returned_entry :
               !strcmp(mode, "stop_resume_running") ? nested_entry :
               !strcmp(mode, "stop_resume_exited") ? exit_entry : yield_entry);
    ps3_host_fiber_resume(&c.r, &c.f);
    if (!strcmp(mode, "stop_resume_exited")) ps3_host_fiber_resume(&c.r, &c.f);
    if (!strcmp(mode, "stop_destroy_suspended")) ps3_host_fiber_destroy(&c.f);
    if (!strcmp(mode, "stop_yield_foreign_stack")) {
        c.f.state = PS3_HOST_FIBER_RUNNING; ps3_host_fiber_yield(&c.f);
    }
    if (!strcmp(mode, "stop_foreign_thread")) {
        pthread_attr_t attr; CHECK(!pthread_attr_init(&attr));
        CHECK(!pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));
        pthread_t t; CHECK(!pthread_create(&t, &attr, foreign, &c));
        CHECK(!pthread_attr_destroy(&attr));
        for (;;) pause();
    }
    CHECK(0);
}
static void handoff_entry(void* arg)
{
    Case* c = arg;
    for (unsigned i = 0; i < 10000; ++i) {
        CHECK(c->step == 2 * i); ++c->step; ps3_host_fiber_yield(&c->f);
    }
    ps3_host_fiber_exit(&c->f);
}
static void handoff(void)
{
    Case c = {0}; create(&c, handoff_entry);
    for (unsigned i = 0; i < 10000; ++i) {
        ps3_host_fiber_resume(&c.r, &c.f); CHECK(c.step == 2 * i + 1); ++c.step;
    }
    ps3_host_fiber_resume(&c.r, &c.f); ps3_host_fiber_destroy(&c.f);
}
static void guard_entry(void* arg)
{
    Case* c = arg;
    (void)deep_frames(c, 1024);
    CHECK(0);
}
static void guard(const char* executable)
{
#if !TARGET_OS_IPHONE
    int pipes[2]; CHECK(!pipe(pipes));
    posix_spawn_file_actions_t actions; CHECK(!posix_spawn_file_actions_init(&actions));
    CHECK(!posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO));
    CHECK(!posix_spawn_file_actions_addclose(&actions, pipes[0]));
    CHECK(!posix_spawn_file_actions_addclose(&actions, pipes[1]));
    extern char** environ;
    char* args[] = {(char*)executable, "guard_child", NULL};
    pid_t pid; CHECK(!posix_spawn(&pid, executable, &actions, NULL, args, environ));
    CHECK(!posix_spawn_file_actions_destroy(&actions));
    close(pipes[1]); char output[65536]; size_t used = 0; ssize_t n;
    char buffer[1024];
    while ((n = read(pipes[0], buffer, sizeof buffer)) > 0) {
        size_t keep = (size_t)n < sizeof output - 1 - used ? (size_t)n : sizeof output - 1 - used;
        memcpy(output + used, buffer, keep); used += keep;
    }
    close(pipes[0]); output[used] = 0;
    int status; CHECK(waitpid(pid, &status, 0) == pid);
    fprintf(stderr, "guard child status=%d\n%s", status, output);
    CHECK((WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS)) ||
          ((strstr(output, "AddressSanitizer: stack-overflow") ||
            strstr(output, "ThreadSanitizer: stack-overflow")) &&
           ((WIFEXITED(status) && WEXITSTATUS(status) != 0) ||
            (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT))));
#else
    (void)executable;
    Case c = {0}; create(&c, guard_entry); ps3_host_fiber_resume(&c.r, &c.f);
#endif
}
int main(int argc, char** argv)
{
    CHECK(argc == 2); const char* mode = argv[1];
    if (!strncmp(mode, "stop_", 5)) stop_mode(mode);
    else if (!strcmp(mode, "abi")) abi();
    else if (!strcmp(mode, "lifecycle")) lifecycle();
    else if (!strcmp(mode, "many")) many();
    else if (!strcmp(mode, "tsan_handoff")) handoff();
    else if (!strcmp(mode, "frame") || !strcmp(mode, "deep")) {
        Case c = {0}; create(&c, !strcmp(mode, "frame") ? frame_entry : deep_entry);
        ps3_host_fiber_resume(&c.r, &c.f);
        if (c.f.state == PS3_HOST_FIBER_SUSPENDED) ps3_host_fiber_resume(&c.r, &c.f);
        CHECK(c.f.state == PS3_HOST_FIBER_EXITED); ps3_host_fiber_destroy(&c.f);
    } else if (!strcmp(mode, "enomem")) {
        Case c = {0}; ps3_host_fiber before = c.f; errno = 0;
        CHECK(ps3_host_fiber_create(&c.f, SIZE_MAX / 4, exit_entry, &c, 0) == -1);
        CHECK(errno == ENOMEM && !memcmp(&before, &c.f, sizeof before));
    } else if (!strcmp(mode, "guard")) guard(argv[0]);
    else if (!strcmp(mode, "guard_child")) {
        Case c = {0}; create(&c, guard_entry); ps3_host_fiber_resume(&c.r, &c.f);
        CHECK(0);
    }
    else CHECK(0);
    printf("PASS %s\n", mode); return 0;
}
