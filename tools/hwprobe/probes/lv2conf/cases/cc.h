/* cc.h - helpers shared by the case files. Portable C99; includes only conf.h, so both backends
 * compile it unchanged. Nothing here knows about a backend.
 *
 * Syscall numbers: lv2 kernel numbers, checked against the RPCS3 syscall table at the oracle pin
 * (d00d80231af3f9b0286f38fd72f7513583f1a9d4, rpcs3/Emu/Cell/lv2/lv2.cpp:143-968). The macro name
 * is the firmware name of the call, in capitals.
 */
#ifndef LV2CONF_CASES_CC_H
#define LV2CONF_CASES_CC_H

#include "../conf.h"

/* ------------------------------------------------------------------ syscall numbers */
#define SC_SYS_PROCESS_GETPID 1
#define SC_SYS_PROCESS_GET_NUMBER_OF_OBJECT 12
#define SC_SYS_PROCESS_GET_ID 13
#define SC_SYS_PROCESS_GETPPID 18
#define SC_SYS_PROCESS_GET_SDK_VERSION 25

#define SC_SYS_PPU_THREAD_YIELD 43
#define SC_SYS_PPU_THREAD_JOIN 44
#define SC_SYS_PPU_THREAD_DETACH 45
#define SC_SYS_PPU_THREAD_GET_JOIN_STATE 46
#define SC_SYS_PPU_THREAD_SET_PRIORITY 47
#define SC_SYS_PPU_THREAD_GET_PRIORITY 48
#define SC_SYS_PPU_THREAD_GET_STACK_INFORMATION 49

#define SC_SYS_TIMER_CREATE 70
#define SC_SYS_TIMER_DESTROY 71
#define SC_SYS_TIMER_GET_INFORMATION 72
#define SC__SYS_TIMER_START 73
#define SC_SYS_TIMER_STOP 74
#define SC_SYS_TIMER_CONNECT_EVENT_QUEUE 75
#define SC_SYS_TIMER_DISCONNECT_EVENT_QUEUE 76

#define SC_SYS_EVENT_FLAG_CREATE 82
#define SC_SYS_EVENT_FLAG_DESTROY 83
#define SC_SYS_EVENT_FLAG_WAIT 85
#define SC_SYS_EVENT_FLAG_TRYWAIT 86
#define SC_SYS_EVENT_FLAG_SET 87

#define SC_SYS_SEMAPHORE_CREATE 90
#define SC_SYS_SEMAPHORE_DESTROY 91
#define SC_SYS_SEMAPHORE_WAIT 92
#define SC_SYS_SEMAPHORE_TRYWAIT 93
#define SC_SYS_SEMAPHORE_POST 94

#define SC_SYS_MUTEX_CREATE 100
#define SC_SYS_MUTEX_DESTROY 101
#define SC_SYS_MUTEX_LOCK 102
#define SC_SYS_MUTEX_TRYLOCK 103
#define SC_SYS_MUTEX_UNLOCK 104
#define SC_SYS_COND_CREATE 105
#define SC_SYS_COND_DESTROY 106
#define SC_SYS_COND_WAIT 107
#define SC_SYS_COND_SIGNAL 108
#define SC_SYS_COND_SIGNAL_ALL 109
#define SC_SYS_COND_SIGNAL_TO 110

#define SC_SYS_SEMAPHORE_GET_VALUE 114
#define SC_SYS_EVENT_FLAG_CLEAR 118
#define SC_SYS_RWLOCK_CREATE 120
#define SC_SYS_RWLOCK_DESTROY 121
#define SC_SYS_RWLOCK_RLOCK 122
#define SC_SYS_RWLOCK_TRYRLOCK 123
#define SC_SYS_RWLOCK_RUNLOCK 124
#define SC_SYS_RWLOCK_WLOCK 125
#define SC_SYS_RWLOCK_TRYWLOCK 126
#define SC_SYS_RWLOCK_WUNLOCK 127

#define SC_SYS_EVENT_QUEUE_CREATE 128
#define SC_SYS_EVENT_QUEUE_DESTROY 129
#define SC_SYS_EVENT_QUEUE_RECEIVE 130
#define SC_SYS_EVENT_QUEUE_TRYRECEIVE 131
#define SC_SYS_EVENT_FLAG_CANCEL 132
#define SC_SYS_EVENT_QUEUE_DRAIN 133
#define SC_SYS_EVENT_PORT_CREATE 134
#define SC_SYS_EVENT_PORT_DESTROY 135
#define SC_SYS_EVENT_PORT_CONNECT_LOCAL 136
#define SC_SYS_EVENT_PORT_DISCONNECT 137
#define SC_SYS_EVENT_PORT_SEND 138
#define SC_SYS_EVENT_FLAG_GET 139
#define SC_SYS_TIMER_USLEEP 141
#define SC_SYS_TIMER_SLEEP 142
#define SC__SYS_RWLOCK_TRYWLOCK 148

#define SC_SYS_MEMORY_CONTAINER_CREATE_OLD 324
#define SC_SYS_MEMORY_CONTAINER_DESTROY_OLD 325
#define SC_SYS_MEMORY_CONTAINER_CREATE 341
#define SC_SYS_MEMORY_CONTAINER_DESTROY 342
#define SC_SYS_MEMORY_CONTAINER_GET_SIZE 343
#define SC_SYS_MEMORY_ALLOCATE 348
#define SC_SYS_MEMORY_FREE 349
#define SC_SYS_MEMORY_ALLOCATE_FROM_CONTAINER 350
#define SC_SYS_MEMORY_GET_PAGE_ATTRIBUTE 351
#define SC_SYS_MEMORY_GET_USER_MEMORY_SIZE 352

/* ------------------------------------------------------------------ syscall wrappers */
static inline int64_t sc0(uint32_t n)
{
	return conf_sc(n, 0, 0);
}

static inline int64_t sc1(uint32_t n, uint64_t a)
{
	uint64_t v[1];
	v[0] = a;
	return conf_sc(n, 1, v);
}

static inline int64_t sc2(uint32_t n, uint64_t a, uint64_t b)
{
	uint64_t v[2];
	v[0] = a;
	v[1] = b;
	return conf_sc(n, 2, v);
}

static inline int64_t sc3(uint32_t n, uint64_t a, uint64_t b, uint64_t c)
{
	uint64_t v[3];
	v[0] = a;
	v[1] = b;
	v[2] = c;
	return conf_sc(n, 3, v);
}

static inline int64_t sc4(uint32_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
	uint64_t v[4];
	v[0] = a;
	v[1] = b;
	v[2] = c;
	v[3] = d;
	return conf_sc(n, 4, v);
}

static inline int64_t sc5(uint32_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
	uint64_t v[5];
	v[0] = a;
	v[1] = b;
	v[2] = c;
	v[3] = d;
	v[4] = e;
	return conf_sc(n, 5, v);
}

/* ------------------------------------------------------------------ recording helpers */

/* A boolean relation as 0/1. */
static inline void rec_bool(const char *key, int cond)
{
	conf_rec(key, cond ? 1 : 0);
}

/* Sentinel written into an out parameter before a call, to tell "written" from "left alone". */
#define SENT32 0xDEADBEEFu
#define SENT64 0xDEADBEEFCAFEF00Dull

/* A finite wait for calls that must not block the probe: 20 ms in microseconds. */
#define WAIT_US 20000ull

/* Write the 8-byte NUL-padded name field at a + off. */
static inline void put_name8(conf_addr a, uint32_t off)
{
	static const char nm[8] = { 'h', 'w', 'p', 'r', 'o', 'b', 'e', 0 };
	uint32_t i;
	for (i = 0; i < 8; i++)
		conf_w8(a + off + i, (uint8_t)nm[i]);
}

/* ------------------------------------------------------------------ attribute constants
 * sys_sync.h at the RPCS3 pin, lines 12-57. */
#define SYNC_FIFO 0x1u
#define SYNC_PRIO 0x2u
#define SYNC_PRIO_INHERIT 0x3u
#define SYNC_RETRY 0x4u
#define SYNC_RECURSIVE 0x10u
#define SYNC_NOT_RECURSIVE 0x20u
#define SYNC_PROCESS_SHARED 0x100u
#define SYNC_NOT_PROCESS_SHARED 0x200u
#define SYNC_ADAPTIVE 0x1000u
#define SYNC_NOT_ADAPTIVE 0x2000u

/* Object type codes of sys_process_get_number_of_object (sys_process.h:10-29). */
#define OBJ_MUTEX 0x85u
#define OBJ_COND 0x86u
#define OBJ_RWLOCK 0x88u
#define OBJ_EVENT_QUEUE 0x8Du
#define OBJ_EVENT_PORT 0x0Eu
#define OBJ_TIMER 0x11u
#define OBJ_SEMAPHORE 0x96u
#define OBJ_EVENT_FLAG 0x98u
#define OBJ_MEM 0x08u

/* Number of live kernel objects of one type, or -1 when the call failed. The word is written
 * big endian by the kernel and read back with conf_r32, so it is host-independent. */
static inline int64_t obj_count(uint32_t type)
{
	conf_addr p = conf_alloc(4, 4);
	conf_w32(p, SENT32);
	if (sc2(SC_SYS_PROCESS_GET_NUMBER_OF_OBJECT, type, p) != 0)
		return -1;
	return (int64_t)conf_r32(p);
}

/* ------------------------------------------------------------------ attribute builders
 * Each struct is written byte by byte at the offsets of the RPCS3 definition (rpcs3/Emu/Cell/lv2/
 * sys_*.h at the pin); the name field is "hwprobe". */

/* sys_mutex_attribute_t (sys_mutex.h:9-24), 40 bytes. */
static inline conf_addr attr_mutex(uint32_t protocol, uint32_t recursive, uint32_t pshared, uint32_t adaptive)
{
	conf_addr a = conf_alloc(40, 8);
	conf_w32(a + 0, protocol);
	conf_w32(a + 4, recursive);
	conf_w32(a + 8, pshared);
	conf_w32(a + 12, adaptive);
	conf_w64(a + 16, 0); /* ipc_key */
	conf_w32(a + 24, 0); /* flags */
	conf_w32(a + 28, 0); /* pad */
	put_name8(a, 32);
	return a;
}

/* A valid mutex: returns its id, 0 when creation failed. */
static inline uint32_t mutex_new(int recursive)
{
	conf_addr idp = conf_alloc(4, 4);
	conf_addr at = attr_mutex(SYNC_PRIO, recursive ? SYNC_RECURSIVE : SYNC_NOT_RECURSIVE, SYNC_NOT_PROCESS_SHARED,
	                          SYNC_NOT_ADAPTIVE);
	conf_w32(idp, 0);
	if (sc2(SC_SYS_MUTEX_CREATE, idp, at) != 0)
		return 0;
	return conf_r32(idp);
}

/* sys_cond_attribute_t (sys_cond.h:8-19), 24 bytes. */
static inline conf_addr attr_cond(uint32_t pshared)
{
	conf_addr a = conf_alloc(24, 8);
	conf_w32(a + 0, pshared);
	conf_w32(a + 4, 0); /* flags */
	conf_w64(a + 8, 0); /* ipc_key */
	put_name8(a, 16);
	return a;
}

/* sys_semaphore_attribute_t / sys_rwlock_attribute_t (sys_semaphore.h:7-20, sys_rwlock.h:7-20,
 * the two are identical), 32 bytes. */
static inline conf_addr attr_sema_rw(uint32_t protocol, uint32_t pshared)
{
	conf_addr a = conf_alloc(32, 8);
	conf_w32(a + 0, protocol);
	conf_w32(a + 4, pshared);
	conf_w64(a + 8, 0); /* ipc_key */
	conf_w32(a + 16, 0); /* flags */
	conf_w32(a + 20, 0); /* pad */
	put_name8(a, 24);
	return a;
}

/* sys_event_queue_attribute_t (sys_event.h:58-68), 16 bytes. */
static inline conf_addr attr_equeue(uint32_t protocol, uint32_t type)
{
	conf_addr a = conf_alloc(16, 8);
	conf_w32(a + 0, protocol);
	conf_w32(a + 4, type);
	put_name8(a, 8);
	return a;
}

/* sys_event_flag_attribute_t (sys_event_flag.h:19-32), 32 bytes. */
static inline conf_addr attr_eflag(uint32_t protocol, uint32_t type)
{
	conf_addr a = conf_alloc(32, 8);
	conf_w32(a + 0, protocol);
	conf_w32(a + 4, SYNC_NOT_PROCESS_SHARED);
	conf_w64(a + 8, 0); /* ipc_key */
	conf_w32(a + 16, 0); /* flags */
	conf_w32(a + 20, type);
	put_name8(a, 24);
	return a;
}

#endif
