#ifndef PS3_HOST_FIBER_H
#define PS3_HOST_FIBER_H
#include "host_fiber_ctx.h"
#include "ps3emu/stop.h"
#include <pthread.h>

typedef enum ps3_host_fiber_state {
    PS3_HOST_FIBER_NEW = 1, PS3_HOST_FIBER_RUNNING,
    PS3_HOST_FIBER_SUSPENDED, PS3_HOST_FIBER_EXITED
} ps3_host_fiber_state;
typedef struct ps3_host_runner {
    ps3_host_ctx ctx;
    void* asan_fake;
    void* tsan_self;
} ps3_host_runner;
typedef struct ps3_host_fiber {
    ps3_host_ctx ctx;
    ps3_host_runner* runner;
    void (*entry)(void*);
    void* arg;
    unsigned char* map;
    size_t map_size, guard;
    pthread_t owner;
    uint32_t ea;
    ps3_host_fiber_state state;
    /* Unconditional: callers and sanitizer builds share one record layout. */
    void* asan_fake;
    const void* runner_bottom;
    size_t runner_size;
    void* tsan;
} ps3_host_fiber;

/* f is zeroed or destroyed. -1/ENOMEM leaves it untouched; other errors stop. */
int ps3_host_fiber_create(ps3_host_fiber* f, size_t stack_size,
                          void (*entry)(void*), void* arg, uint32_t ea);
/* Serialized by the consumer; first resume pins f to this host thread. */
void ps3_host_fiber_resume(ps3_host_runner* r, ps3_host_fiber* f);
/* Yield and exit require f's own stack. Exit abandons its live frames. */
void ps3_host_fiber_yield(ps3_host_fiber* f);
PS3_NORETURN void ps3_host_fiber_exit(ps3_host_fiber* f);
/* NEW or EXITED only; repeated destruction does nothing. */
void ps3_host_fiber_destroy(ps3_host_fiber* f);
void ps3_host_ctx_switch(ps3_host_ctx* from, const ps3_host_ctx* to)
    __attribute__((nonnull));
#endif
