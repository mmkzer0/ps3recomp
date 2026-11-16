/*
 * ps3_host_at_quick_exit() on the toolkit-owned table (runtime/diag/host_exit.c).
 *
 * iOS 16's libSystem has no at_quick_exit/quick_exit, so a deployment-16.0 app has to keep its own LIFO
 * table and end the process itself. This fixture compiles host_exit.c with PS3_HOST_EXIT_FORCE_TABLE
 * (CMakeLists.txt), which takes the table path on a host whose libc does have both, and checks the
 * contract a handler relies on from C11 7.22.4.3 / 7.22.4.7:
 *
 *   - ps3_host_exit(status) runs the handlers last-registered first and ends the process with status;
 *   - stdio is flushed before the first handler, and no atexit handler runs;
 *   - a handler that registers another handler gets it run next;
 *   - the table holds at least 32 and the 33rd registration returns nonzero (like at_quick_exit);
 *   - concurrent registrations lose none of the 32 slots and none is granted twice.
 *
 * If the table were skipped, ps3_host_exit would call libc's quick_exit with no handler registered with
 * it, and every handler check below would see an empty output.
 *
 * Each exit runs in a forked child: ps3_host_exit never returns. The child's stdout is a pipe and the
 * handlers write to it with write(2), so the output order is the order of the calls.
 */
#ifndef PS3_HOST_EXIT_FORCE_TABLE
#error "this fixture checks the table path: build it with PS3_HOST_EXIT_FORCE_TABLE"
#endif

#include "ps3emu/host_exit.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "[FAIL] %s:%d: %s: ", __FILE__, __LINE__, #cond); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); ++g_failures; } } while (0)

static void emit(char c) { (void)!write(STDOUT_FILENO, &c, 1); }

static void handler_a(void) { emit('A'); }
static void handler_b(void) { emit('B'); }
static void handler_d(void) { emit('D'); }
static void handler_c(void)
{
    emit('C');
    /* Registered while the table is running: it goes on top and runs next. */
    if (ps3_host_at_quick_exit(handler_d)) emit('!');
}
static void handler_dot(void) { emit('.'); }
static void noop(void) {}
static void atexit_must_not_run(void) { emit('X'); }

/* Runs `child` in a forked process whose stdout is a pipe; returns what it wrote and its wait status. */
static int run_child(void (*child)(void), char* out, size_t cap, size_t* len)
{
    int fds[2];
    if (pipe(fds)) { perror("pipe"); exit(2); }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }
    if (pid == 0) {
        alarm(30);                         /* a hang is a failure, not a stuck suite */
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        child();
        _exit(99);                         /* ps3_host_exit returned */
    }
    close(fds[1]);
    size_t n = 0;
    ssize_t r;
    while (n < cap && (r = read(fds[0], out + n, cap - n)) > 0) n += (size_t)r;
    close(fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) { perror("waitpid"); exit(2); }
    *len = n;
    return status;
}

static void child_lifo(void)
{
    atexit(atexit_must_not_run);
    if (ps3_host_at_quick_exit(handler_a) || ps3_host_at_quick_exit(handler_b) || ps3_host_at_quick_exit(handler_c))
        _exit(98);
    fputs("S", stdout);                    /* buffered: ps3_host_exit flushes it before any handler */
    ps3_host_exit(42);
}

static void check_lifo_flush_and_status(void)
{
    char out[64];
    size_t n;
    int status = run_child(child_lifo, out, sizeof out - 1, &n);
    out[n] = 0;
    CHECK(WIFEXITED(status), "the child did not exit normally (status 0x%x)", status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42, "exit status %d, want 42",
          WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    /* S: the flush; C then its D (registered during the run), then B, A. No X: no atexit handler. */
    CHECK(strcmp(out, "SCDBA") == 0, "handler output \"%s\", want \"SCDBA\"", out);
}

#define THREADS 8
#define PER_THREAD 8   /* 64 attempts for 32 slots */

static int g_granted[THREADS];

static void* register_many(void* arg)
{
    int* granted = arg;
    for (int i = 0; i < PER_THREAD; ++i)
        if (ps3_host_at_quick_exit(handler_dot) == 0) ++*granted;
    return NULL;
}

static void child_concurrent(void)
{
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; ++i)
        if (pthread_create(&t[i], NULL, register_many, &g_granted[i])) _exit(98);
    int total = 0;
    for (int i = 0; i < THREADS; ++i) { pthread_join(t[i], NULL); total += g_granted[i]; }
    emit((char)('a' + total / 2));         /* 32 granted -> 'q' */
    ps3_host_exit(0);
}

static void check_concurrent_registration(void)
{
    char out[128];
    size_t n;
    int status = run_child(child_concurrent, out, sizeof out - 1, &n);
    out[n] = 0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child status 0x%x", status);
    /* One byte for the grant count, then one '.' per handler that ran. */
    CHECK(n == 33 && out[0] == 'a' + 16, "granted/run output \"%s\" (%zu bytes), want 'q' then 32 dots", out, n);
    for (size_t i = 1; i < n; ++i) CHECK(out[i] == '.', "byte %zu is 0x%02x", i, (unsigned char)out[i]);
}

static void check_table_capacity(void)
{
    /* In the parent: nothing here calls ps3_host_exit, and the children above forked before it ran. */
    for (int i = 0; i < 32; ++i)
        CHECK(ps3_host_at_quick_exit(noop) == 0, "registration %d of 32 failed", i + 1);
    CHECK(ps3_host_at_quick_exit(noop) != 0, "the 33rd registration succeeded");
    CHECK(ps3_host_at_quick_exit(noop) != 0, "the 34th registration succeeded");
}

int main(void)
{
    check_lifo_flush_and_status();
    check_concurrent_registration();
    check_table_capacity();
    if (g_failures) return 1;
    puts("PASS: test_host_at_quick_exit");
    return 0;
}
