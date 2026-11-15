/* The configuration a fixture that includes runtime/syscalls/sys_ppu_thread.c runs on: the runtime's config
 * module is not linked into those fixtures, and sys_ppu_thread_create reads ppu.host_stack_mb from it. Only
 * that key is set; a fixture that needs another size writes g_stub_config.ppu_host_stack_mb before it creates
 * the thread (the key is read at each create, from the creating thread). */
#ifndef PS3_TESTS_PPU_THREAD_CONFIG_STUB_H
#define PS3_TESTS_PPU_THREAD_CONFIG_STUB_H

#include "ps3emu/config.h"

static ps3_config g_stub_config = { .ppu_host_stack_mb = 256 };

const ps3_config* ps3_config_get(void) { return &g_stub_config; }

#endif
