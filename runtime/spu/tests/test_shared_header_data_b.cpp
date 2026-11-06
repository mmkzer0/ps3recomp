/* Second object of test_shared_header_data.c: C++ reader of the header-defined data. */
#include "ps3emu/ps3_ftrace.h"

extern "C" const void* shared_b_ft(void) { return (const void*)&g_ps3_ft; }
