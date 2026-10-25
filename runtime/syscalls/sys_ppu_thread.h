/*
 * ps3recomp - PPU thread management syscalls
 *
 * Real thread management using host OS threads.
 */

#ifndef SYS_PPU_THREAD_H
#define SYS_PPU_THREAD_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"
#include "../memory/vm.h"

#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <pthread.h>
  #include <sched.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum concurrent PPU threads */
#define PPU_THREAD_MAX  64   /* = PPU_THREAD_SLOTS (ppu_thread_id.h) */

/* Slot lifecycle, guarded by the table lock (sys_ppu_thread.c):
 *   FREE    -> RUNNING   sys_ppu_thread_create
 *   RUNNING -> ZOMBIE    the thread's epilogue, once its body has ended (a detached thread frees its slot there
 *                        instead; a ZOMBIE is reaped by exactly one of join / detach)
 *   ZOMBIE  -> FREE      the reaper
 * The main thread (slot 0) has no epilogue: sys_ppu_thread_exit makes it ZOMBIE and nothing reaps it. */
#define PPU_THREAD_STATE_FREE       0
#define PPU_THREAD_STATE_RUNNING    1
#define PPU_THREAD_STATE_ZOMBIE     2

/* Who may reap the slot (lv2 ppu_join_status), valid while the slot is not FREE:
 *   JOINABLE  nobody has asked yet
 *   JOINING   one sys_ppu_thread_join is waiting for, or reaping, the thread (ppu_thread_info.joiner_tid)
 *   DETACHED  nobody joins: the thread's own epilogue frees the slot (created without
 *             SYS_PPU_THREAD_CREATE_JOINABLE, or sys_ppu_thread_detach'ed) */
#define PPU_JOIN_JOINABLE   0
#define PPU_JOIN_JOINING    1
#define PPU_JOIN_DETACHED   2

/* Thread entry point type - recompiled function pointer */
typedef void (*ppu_thread_entry_fn)(ppu_context* ctx);

/* Internal thread descriptor */
typedef struct ppu_thread_info {
    int          state;
    ppu_context  ctx;
    char         name[64];
    int32_t      priority;
    int64_t      exit_status;
    int          joiner;       /* PPU_JOIN_*, under the table lock */
    uint64_t     joiner_tid;   /* the thread waiting in join, while joiner == PPU_JOIN_JOINING */
    uint32_t     stack_addr;   /* guest stack base */
    uint32_t     stack_size;
    uint64_t     entry_addr;   /* guest entry point */
    uint64_t     tls_addr;
    uint32_t     prof_pc;      /* last syscall/HLE callsite (sampling profiler) */

#ifdef _WIN32
    HANDLE       host_thread;
    DWORD        host_tid;
#else
    pthread_t    host_thread;
#endif

    /* For join synchronization. Only the epilogue of a thread that has a joiner signals it, under the table lock;
     * the joiner waits holding nothing else and takes the table lock only after it is woken (lock order: table
     * lock, then finish_mutex). Whoever reaps the slot destroys these once, and the epilogue never touches the
     * slot after it releases the table lock. */
#ifdef _WIN32
    HANDLE       finish_event;
#else
    pthread_mutex_t finish_mutex;
    pthread_cond_t  finish_cond;
    int             finished;  /* under finish_mutex */
#endif

} ppu_thread_info;

/* Global thread table */
extern ppu_thread_info g_ppu_threads[PPU_THREAD_MAX];

/* Stack allocator (shared) */
extern vm_stack_alloc g_vm_stack_alloc;

/* Callback to invoke recompiled code - must be set by the runtime */
extern ppu_thread_entry_fn g_ppu_thread_entry_trampoline;

/* ---------------------------------------------------------------------------
 * Syscall handlers
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_create(ppu_context* ctx);

/* Claim thread-table slot 0 (id 1) for the main guest thread; returns the id.
 * Call before the first sys_ppu_thread_create so no created thread aliases
 * main (thread_id 0 broke every owner-tracking sync primitive). */
uint64_t ppu_thread_register_main(void);
int64_t sys_ppu_thread_exit(ppu_context* ctx);
int64_t sys_ppu_thread_join(ppu_context* ctx);
int64_t sys_ppu_thread_detach(ppu_context* ctx);
int64_t sys_ppu_thread_yield(ppu_context* ctx);
int64_t sys_ppu_thread_get_priority(ppu_context* ctx);
int64_t sys_ppu_thread_set_priority(ppu_context* ctx);
int64_t sys_ppu_thread_rename(ppu_context* ctx);
int64_t sys_ppu_thread_get_join_state(ppu_context* ctx);
int64_t sys_ppu_thread_get_stack_information(ppu_context* ctx);

/* Test seam: called by the epilogue of a created thread once it has released the table lock for the last time, with
 * its own id and the joiner state it published under that lock (PPU_JOIN_JOINABLE: it is now a zombie;
 * PPU_JOIN_JOINING: its joiner was woken; PPU_JOIN_DETACHED: it freed its own slot). The thread touches no slot
 * afterwards. NULL (the default) calls nothing. Set it before creating the threads it concerns. */
typedef void (*ppu_thread_epilogue_hook_fn)(uint64_t tid, int joiner);
void ppu_thread_set_epilogue_hook(ppu_thread_epilogue_hook_fn fn);

/* Registration */
void sys_ppu_thread_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_PPU_THREAD_H */
