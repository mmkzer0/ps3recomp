/*
 * Header-defined shared data is one variable per program.
 *
 * ps3_ftrace.h defines g_ps3_ft in the header so that targets which link no
 * extra object still see it. Every translation unit that includes the header
 * gets a definition; the linker has to fold them into one. This test compares
 * the address a C object and a C++ object (test_shared_header_data_b.cpp) see.
 *
 * (Upstream's version also covered spu_coh_bitmap.h's g_spu_coh_bits; that
 * header is not in this tree, the reservation model in reservation.h is the one
 * authority for reserved lines.)
 *
 * Build (any toolchain; MinGW-w64 as well):
 *   cc -std=gnu17 -I include -c runtime/spu/tests/test_shared_header_data.c -o a.o
 *   c++ -std=gnu++17 -I include -c runtime/spu/tests/test_shared_header_data_b.cpp -o b.o
 *   c++ a.o b.o -o test_shared_header_data
 */
#include "ps3emu/ps3_ftrace.h"
#include <stdio.h>

const void* shared_b_ft(void);

int main(void)
{
    int fail = 0;
    if (shared_b_ft() != (const void*)&g_ps3_ft)
                              { printf("[FAIL] g_ps3_ft has two addresses\n"); fail++; }
    else                      printf("[PASS] g_ps3_ft is one variable\n");

    printf("\nResults: %d failed\n", fail);
    return fail ? 1 : 0;
}
