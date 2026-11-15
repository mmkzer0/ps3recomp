/*
 * ps3recomp - the host stack of a thread, and how deep it went (PS3_HOST_STACK_HWM, docs/DIAGNOSTICS.md)
 *
 * Header-only and static, so the files that report a high-water mark (runtime/syscalls/sys_ppu_thread.c for guest
 * PPU threads, runtime/spu/spu_workload.c for the async-dispatch threads) build in fixtures that link neither.
 *
 * The stack grows down, so the high-water mark is the span from the top of the stack to the lowest page the
 * thread touched: the lowest page mincore reports resident (POSIX) or committed (Windows: VirtualQuery). That is
 * exact to one host page, except where the host can take a touched page away: iOS compresses cold pages, a page
 * that is no longer resident is not counted, and the report is then a lower bound. Nothing is pre-filled (that
 * would commit the whole reservation).
 *
 * Linux needs _GNU_SOURCE before the first system header (pthread_getattr_np).
 */
#ifndef PS3EMU_HOST_STACK_H
#define PS3EMU_HOST_STACK_H

#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <pthread.h>
#  include <sys/mman.h>
#  include <unistd.h>
#endif

#if defined(__linux__) && !defined(_GNU_SOURCE) && !defined(__cplusplus)
/* A C translation unit that did not ask for _GNU_SOURCE still includes this header; glibc hides the prototype.
 * (g++ always defines _GNU_SOURCE.) */
extern int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr);
#endif

/* The calling thread's host stack, as [lo, lo + size). 0 when the host will not say. */
static inline int host_stack_range_self(uintptr_t* lo, size_t* size)
{
#ifdef _WIN32
    /* The reservation starts at the allocation base of any address on the stack and ends at the TEB's StackBase. */
    MEMORY_BASIC_INFORMATION mbi;
    char here;
    if (!VirtualQuery(&here, &mbi, sizeof mbi)) return 0;
    const uintptr_t hi = (uintptr_t)((NT_TIB*)NtCurrentTeb())->StackBase;
    *lo = (uintptr_t)mbi.AllocationBase;
    if (hi <= *lo) return 0;
    *size = (size_t)(hi - *lo);
    return 1;
#elif defined(__APPLE__)
    const pthread_t self = pthread_self();
    const uintptr_t hi = (uintptr_t)pthread_get_stackaddr_np(self);   /* the top, not the base */
    *size = pthread_get_stacksize_np(self);
    *lo = hi - *size;
    return *size != 0;
#else
    pthread_attr_t attr;
    void* addr;
    size_t sz;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return 0;
    const int ok = pthread_attr_getstack(&attr, &addr, &sz) == 0;
    pthread_attr_destroy(&attr);
    if (!ok) return 0;
    *lo = (uintptr_t)addr;
    *size = sz;
    return 1;
#endif
}

/* The span from the top of [lo, lo + size) down to the lowest page that was touched, rounded to host pages.
 * Returns 0 (and leaves *high_water unset) when the host would not say. May run on any thread: only the range is
 * read, so the stack must still be mapped. */
static inline int host_stack_high_water(uintptr_t lo, size_t size, size_t* high_water)
{
#ifdef _WIN32
    const uintptr_t end = lo + size;
    for (uintptr_t a = lo; a < end;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((LPCVOID)a, &m, sizeof m)) return 0;
        /* Below the touched part: reserved and never committed, then the guard page, which is committed with
         * PAGE_GUARD and has not been touched either. */
        if (m.State == MEM_COMMIT && !(m.Protect & PAGE_GUARD)) {
            *high_water = (size_t)(end - (uintptr_t)m.BaseAddress);
            return 1;
        }
        a = (uintptr_t)m.BaseAddress + m.RegionSize;
    }
    *high_water = 0;
    return 1;
#else
#  if defined(__APPLE__)
    typedef char host_stack_vec_t;
#  else
    typedef unsigned char host_stack_vec_t;
#  endif
    const size_t pg  = (size_t)sysconf(_SC_PAGESIZE);
    const uintptr_t base = (lo + pg - 1) & ~(uintptr_t)(pg - 1);
    const uintptr_t end  = (lo + size) & ~(uintptr_t)(pg - 1);
    host_stack_vec_t vec[1024];
    for (uintptr_t a = base; a < end;) {
        size_t n = (size_t)((end - a) / pg);
        if (n > sizeof vec) n = sizeof vec;
        if (mincore((void*)a, n * pg, vec) != 0) return 0;
        for (size_t i = 0; i < n; i++) {
            if (vec[i] & 1) {                    /* MINCORE_INCORE */
                *high_water = (size_t)(end - (a + i * pg));
                return 1;
            }
        }
        a += n * pg;
    }
    *high_water = 0;
    return 1;
#endif
}

#ifndef _WIN32
/* A test may replace pthread_attr_init before the first include, to see what a refusal of it does (the callers
 * stop with host-thread-stack and never touch the attribute again). */
#  ifndef PS3_HOST_THREAD_ATTR_INIT
#    define PS3_HOST_THREAD_ATTR_INIT pthread_attr_init
#  endif

/* pthread_create with a reserved stack of `bytes`, joinable or detached. Returns 0 or the errno of the step that
 * failed: a failed pthread_attr_init leaves the attribute unusable, so nothing is set on it or destroyed; every
 * later step is checked, so a refused size is never a thread on the host's default stack. The caller ends the run
 * (ps3_stop_host_thread_stack, stop.h) on a non-zero result. */
static inline int ps3_host_pthread_create(pthread_t* t, size_t bytes, int detached, void* (*fn)(void*), void* arg)
{
    pthread_attr_t attr;
    int rc = PS3_HOST_THREAD_ATTR_INIT(&attr);
    if (rc != 0) return rc;
    rc = pthread_attr_setstacksize(&attr, bytes);
    if (rc == 0 && detached) rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (rc == 0) rc = pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}
#endif

/* Why CreateThread or _beginthreadex returned no thread: GetLastError() on Windows; on the POSIX shim
 * (runtime/platform/win32_compat.c) errno holds the host's answer for the step that failed. */
static inline int ps3_host_thread_error(void)
{
#ifdef _WIN32
    return (int)GetLastError();
#else
    return errno;
#endif
}

#endif /* PS3EMU_HOST_STACK_H */
