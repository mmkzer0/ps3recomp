/* The loader's diagnostic registry of PPU thread contexts (ppu_ctx_register) and the watchdog report that
 * walks it (ppu_report_guest_lrs).
 *
 * tools/test_ppu_ctx_registry.py cuts that region out of runtime/ppu/ppu_loader.cpp into REGISTRY_BODY and
 * compiles it here, so the test runs the loader's own code without the rest of the loader. Contract:
 *   - the report names every registered context once, each followed by that thread's unwound guest stack
 *     (ppu_dump_guest_stack, tag "watchdog"), because lr names only the last direct call site (#767);
 *   - a context is registered once however often it is registered (sys_ppu_thread_create registers the
 *     context of every thread it creates, and a context that is reused would otherwise fill the table). */
#include "ppu_context.h"
#include "win32_compat.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

/* The report's log line and dumps, in the order the report makes them. */
static std::vector<std::string> g_events;
#define PS3_LOG_INFO 0
#define PS3_LOG(lvl, tag, ...) (void)(lvl), g_events.push_back("line")

extern "C" void ppu_dump_guest_stack(ppu_context* c, const char* tag)
{
    g_events.push_back(std::string("dump ") + std::to_string(c->thread_id) + " " + tag);
}
extern "C" void ppu_dump_bctrl_ring(uint32_t, const char*) {}

#include REGISTRY_BODY

static int failures;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static std::vector<std::string> report()
{
    g_events.clear();
    ppu_report_guest_lrs();
    return g_events;
}

int main()
{
    static ppu_context a, b;
    a.thread_id = 11;
    b.thread_id = 12;
    ppu_ctx_register(&a);
    ppu_ctx_register(&b);
    {
        const std::vector<std::string> expect = { "line", "dump 11 watchdog", "line", "dump 12 watchdog" };
        CHECK(report() == expect);
    }

    if (failures) return 1;
    std::puts("PASS ppu_ctx_registry");
    return 0;
}
