/* sys_gpio_set (630) / sys_gpio_get (631): the answers of the loader's own syscall cases.
 *
 * tools/test_sys_gpio.py cuts the two case bodies out of runtime/ppu/ppu_loader.cpp into GPIO_BODY and
 * compiles them against a small guest memory here, so the test runs what lv2_syscall runs without the
 * rest of the loader. Contract (RPCS3 6e4b784d1 rpcs3/Emu/Cell/lv2/sys_gpio.cpp:8-39):
 *   device 1 (LED)         set -> CELL_OK,     get -> CELL_OK and *value = 0
 *   device 2 (DIP switch)  set -> CELL_EINVAL, get -> CELL_OK and *value = 0
 *   any other device id    set/get -> CELL_ESRCH, *value untouched
 * a null value pointer is CELL_EFAULT (device id first), and one whose 8-byte span leaves guest memory ends in the
 * hle-guest-range stop. */
#include "../runtime/memory/vm.h"
#include "ppu_context.h"
#include "guest_range_catch.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base = 0x20000000u;
}
void vm_write64(uint64_t a, uint64_t x)
{
    for (int i = 0; i < 8; i++) vm_base[a + i] = (uint8_t)(x >> (56 - 8 * i));
}

static void gpio_cases(ppu_context* ctx)
{
    switch (ctx->gpr[11]) {
#include GPIO_BODY
    default: std::abort();
    }
}

static const uint64_t k_ok = 0;
static const uint64_t k_einval = (uint64_t)(int64_t)(int32_t)CELL_EINVAL;
static const uint64_t k_esrch = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;

static int failures;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static uint64_t set_call(uint64_t dev, uint64_t mask, uint64_t value)
{
    ppu_context c{}; c.gpr[11] = 630; c.gpr[3] = dev; c.gpr[4] = mask; c.gpr[5] = value;
    gpio_cases(&c);
    return c.gpr[3];
}

static uint64_t get_call(uint64_t dev, uint64_t ptr)
{
    ppu_context c{}; c.gpr[11] = 631; c.gpr[3] = dev; c.gpr[4] = ptr;
    gpio_cases(&c);
    return c.gpr[3];
}

int main()
{
    static uint8_t mem[0x10000];
    vm_base = mem;
    ppu_vm_size = sizeof mem;

    /* set: the LED accepts, the DIP switch refuses, anything else has no such device. */
    CHECK(set_call(1, ~0ull, ~0ull) == k_ok);
    CHECK(set_call(2, 0, 0) == k_einval);
    CHECK(set_call(0, 1, 1) == k_esrch);
    CHECK(set_call(3, 1, 1) == k_esrch);
    CHECK(set_call(0x100000001ull, 1, 1) == k_esrch);   /* the id is 64-bit: 1 in the low word is not the LED */

    /* get: a read is 0 whatever was there, for both devices, and only the 8 bytes change. */
    for (uint64_t dev = 1; dev <= 2; ++dev) {
        std::memset(mem, 0xA5, sizeof mem);
        CHECK(get_call(dev, 0x2000) == k_ok);
        for (int i = 0; i < 8; ++i) CHECK(mem[0x2000 + i] == 0);
        CHECK(mem[0x1FFF] == 0xA5 && mem[0x2008] == 0xA5);
    }

    /* an unknown id answers ESRCH before the pointer is looked at, and writes nothing. */
    std::memset(mem, 0xA5, sizeof mem);
    CHECK(get_call(0, 0x2000) == k_esrch);
    CHECK(get_call(3, 0x2000) == k_esrch);
    CHECK(get_call(0, 0xFFFFFFF0u) == k_esrch);
    CHECK(mem[0x2000] == 0xA5 && mem[0x2007] == 0xA5);

    /* a null value pointer is CELL_EFAULT for a real device (try_write fails on page 0 in RPCS3), after the id test:
     * nothing is stored to page 0, and an unknown id still answers ESRCH. It used to store 8 zero bytes there. */
    static const uint64_t k_efault = (uint64_t)(int64_t)(int32_t)CELL_EFAULT;
    for (uint64_t dev = 1; dev <= 2; ++dev) {
        std::memset(mem, 0xA5, sizeof mem);
        CHECK(get_call(dev, 0) == k_efault);
        for (int i = 0; i < 8; ++i) CHECK(mem[i] == 0xA5);
    }
    CHECK(get_call(0, 0) == k_esrch);

    /* the pointer's whole 8-byte span must be in guest memory: the last full slot is fine, one byte
     * past it and the limit itself stop, and the stop leaves memory alone. */
    std::memset(mem, 0xA5, sizeof mem);
    CHECK(get_call(1, sizeof mem - 8) == k_ok);
    CHECK(mem[sizeof mem - 9] == 0xA5 && mem[sizeof mem - 8] == 0 && mem[sizeof mem - 1] == 0);
    std::memset(mem, 0xA5, sizeof mem);
    const uint64_t bad[] = { sizeof mem - 7, sizeof mem - 1, sizeof mem, 0xFFFFFFF8u };
    for (uint64_t ea : bad) {
        GR_TRY(get_call(2, ea));
        CHECK(gr_named("sys_gpio_get", "value"));
        CHECK(g_gr_ea == ea && g_gr_size == 8 && g_gr_limit == sizeof mem);
        CHECK(mem[sizeof mem - 1] == 0xA5 && mem[sizeof mem - 8] == 0xA5);
    }

    if (failures) return 1;
    std::puts("PASS sys_gpio answers and range stop");
    return 0;
}
