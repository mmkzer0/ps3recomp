/*
 * ps3recomp - sysPrxForUser HLE
 *
 * Core PRX runtime: threads, process management, printf, heap allocation,
 * and TLS support. The lightweight mutex and condition variable are not here:
 * runtime/syscalls/sys_lwsync.cpp implements them behind their HLE imports.
 */

#ifndef PS3RECOMP_SYS_PRX_FOR_USER_H
#define PS3RECOMP_SYS_PRX_FOR_USER_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

/* Thread entry point */
typedef void (*sys_ppu_thread_entry_t)(u64 arg);

/* Thread attributes */
typedef struct sys_ppu_thread_attr {
    u32 entry;        /* guest entry address */
    u32 tls_addr;
} sys_ppu_thread_attr_t;

/* Sync protocol constants */
#define SYS_SYNC_FIFO              0x00000001
#define SYS_SYNC_PRIORITY          0x00000002
#define SYS_SYNC_PRIORITY_INHERIT  0x00000003
#define SYS_SYNC_RECURSIVE         0x00000010
#define SYS_SYNC_NOT_RECURSIVE     0x00000020

/* Thread priority range */
#define SYS_PPU_THREAD_PRIORITY_MAX  0
#define SYS_PPU_THREAD_PRIORITY_MIN  3071

/* ---------------------------------------------------------------------------
 * Thread management
 * -----------------------------------------------------------------------*/

s32 sys_ppu_thread_create(sys_ppu_thread_t* thread_id,
                          sys_ppu_thread_entry_t entry,
                          u64 arg,
                          s32 priority,
                          u32 stacksize,
                          u64 flags,
                          const char* threadname);
void sys_ppu_thread_exit(u64 exitcode);
s32 sys_ppu_thread_join(sys_ppu_thread_t thread_id, u64* exitstatus);
s32 sys_ppu_thread_detach(sys_ppu_thread_t thread_id);
s32 sys_ppu_thread_get_id(sys_ppu_thread_t* thread_id);
s32 sys_ppu_thread_yield(void);

/* ---------------------------------------------------------------------------
 * Process management
 * -----------------------------------------------------------------------*/

void sys_process_exit(s32 exitcode);
s32 sys_process_getpid(void);
s32 sys_process_get_number_of_object(u32 object_type, u32* count);
s32 sys_process_is_spu_lock_line_reservation_address(u32 addr, u64 flags);

/* ---------------------------------------------------------------------------
 * Printf / sprintf / snprintf
 * -----------------------------------------------------------------------*/

s32 _sys_printf(const char* fmt, ...);
/* NID 0x9FB6228E -- the abort half of the SDK internal assertion macro. */
s32 _sys_trap_process(const char* msg);
s32 _sys_sprintf(char* buf, const char* fmt, ...);
s32 _sys_snprintf(char* buf, u32 size, const char* fmt, ...);
s32 _sys_strlen(const char* str);
s32 _sys_strncpy(char* dst, const char* src, u32 size);
s32 _sys_strcat(char* dst, const char* src);
s32 _sys_strcmp(const char* s1, const char* s2);
void* _sys_memset(void* dst, s32 val, u32 size);
void* _sys_memcpy(void* dst, const void* src, u32 size);
s32 _sys_memcmp(const void* s1, const void* s2, u32 size);
s32 _sys_toupper(s32 c);
s32 _sys_tolower(s32 c);

/* ---------------------------------------------------------------------------
 * SPU printf server hooks (defined in sys_spu_printf.c, which documents the
 * semantics). The callback arguments are guest OPD addresses, not host
 * function pointers; attach/detach return CELL_ESTAT while the matching
 * callback is not registered, else the callback's own result.
 *
 * attach/detach return s64, not s32: they are reached through the generic HLE
 * adapter (ppu_hle.cpp), which copies the handler's full 64-bit return into
 * r3, and a PPC64 s32 result lives sign-extended in r3 (an s32 return leaves
 * the upper register half unspecified and zero-extended on arm64). The value
 * is always an s32 sign-extended to 64 bits.
 * -----------------------------------------------------------------------*/

s32 _sys_spu_printf_initialize(u32 agcb, u32 dgcb, u32 atcb, u32 dtcb);
s32 _sys_spu_printf_finalize(void);
s64 _sys_spu_printf_attach_group(u32 group);
s64 _sys_spu_printf_detach_group(u32 group);
s64 _sys_spu_printf_attach_thread(u32 thread);
s64 _sys_spu_printf_detach_thread(u32 thread);

/* ---------------------------------------------------------------------------
 * Heap management
 * -----------------------------------------------------------------------*/

typedef u32 sys_heap_t;

s32 sys_heap_create_heap(sys_heap_t* heap, u32 start_addr, u32 size,
                          u32 flags, void* alloc_func, void* free_func);
s32 sys_heap_destroy_heap(sys_heap_t heap);
void* sys_heap_malloc(sys_heap_t heap, u32 size);
s32 sys_heap_free(sys_heap_t heap, void* ptr);
void* sys_heap_memalign(sys_heap_t heap, u32 align, u32 size);

/* The underscore-prefixed names titles import; forwarders to the above. */
u32   sys_heap_create_heap_ret(u32 name, u32 type, u32 blocksize, u32 flags);
u32   _sys_heap_create_heap(u32 name, u32 type, u32 blocksize, u32 flags);
s32   _sys_heap_delete_heap(sys_heap_t heap);
/* The plain libc allocator (not the sys_heap_* family below). */
void* _sys_malloc(u32 size);
void* _sys_memalign(u32 align, u32 size);
s32   _sys_free(void* ptr);
void* _sys_realloc(void* ptr, u32 size);

void* _sys_heap_malloc(sys_heap_t heap, u32 size);
void* _sys_heap_memalign(sys_heap_t heap, u32 align, u32 size);
s32   _sys_heap_free(sys_heap_t heap, void* ptr);

/* ---------------------------------------------------------------------------
 * PRX utilities
 * -----------------------------------------------------------------------*/

s32 sys_prx_exitspawn_with_level(void);
s32 sys_prx_get_module_id_by_name(const char* name, u64 flags, u32* id);

/* ---------------------------------------------------------------------------
 * Random number generation
 * -----------------------------------------------------------------------*/

s32 sys_get_random_number(void* buf, u64 size);

/* ---------------------------------------------------------------------------
 * Console I/O (debug)
 * -----------------------------------------------------------------------*/

s32 console_putc(s32 ch);
s32 console_getc(void);
s32 console_write(const void* buf, u32 len);

/* ---------------------------------------------------------------------------
 * Process info
 * -----------------------------------------------------------------------*/

s32 sys_process_get_paramsfo(void* buf);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SYS_PRX_FOR_USER_H */
