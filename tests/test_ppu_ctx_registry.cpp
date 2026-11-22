/* The loader's diagnostic registry of PPU thread contexts (ppu_ctx_register) and the watchdog report that
 * walks it (ppu_report_guest_lrs).
 *
 * tools/test_ppu_ctx_registry.py cuts that region out of runtime/ppu/ppu_loader.cpp into REGISTRY_BODY and
 * compiles it here, so the test runs the loader's own code without the rest of the loader. Contract:
 *   - the report names every registered context once, each followed by that thread's unwound guest stack
 *     (ppu_dump_guest_stack, tag "watchdog"), because lr names only the last direct call site (#767);
 *   - a context is registered once however often it is registered (sys_ppu_thread_create registers the
 *     context of every thread it creates, and a reused context would otherwise fill the table), also when
 *     several threads register at once;
 *   - a context past the table's capacity is dropped with a warning, never stored out of bounds. */
#include "ppu_context.h"
#include "win32_compat.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* The report's log line and dumps, in the order the report makes them. */
static std::vector<std::string> g_events;
static int g_full_warnings;
#define PS3_LOG_INFO 0
#define PS3_LOG_WARN 1
#define PS3_LOG(lvl, tag, ...) (void)(lvl), g_events.push_back("line")
/* The real PS3_LOG_N admits the first line only; this one counts every call, so the test sees each drop. */
#define PS3_LOG_N(limit, lvl, tag, ...) (void)(lvl), ++g_full_warnings

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
    const std::vector<std::string> two = { "line", "dump 11 watchdog", "line", "dump 12 watchdog" };
    CHECK(report() == two);

    /* The same context registered again, and again, is still one entry; so is a null context, none. */
    for (int i = 0; i < 1000; ++i) ppu_ctx_register(&a);
    ppu_ctx_register(&b);
    ppu_ctx_register(nullptr);
    CHECK(report() == two);

    /* Four threads registering the same four contexts at once: one entry each. */
    static ppu_context shared[4];
    for (int i = 0; i < 4; ++i) shared[i].thread_id = 20 + i;
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([] { for (int r = 0; r < 200; ++r) for (auto& c : shared) ppu_ctx_register(&c); });
        for (auto& th : threads) th.join();
    }
    CHECK(report().size() == 2u * 6u);

    /* Repeats used no slots: distinct contexts still fit up to the cap, and the ones past it are dropped
     * with the warning instead of being stored. */
    static ppu_context many[PPU_CTX_MAX];
    int used = 6;
    for (int i = 0; used < PPU_CTX_MAX; ++i, ++used) {
        many[i].thread_id = 100 + i;
        ppu_ctx_register(&many[i]);
        ppu_ctx_register(&many[i]);
    }
    CHECK(g_full_warnings == 0);
    CHECK(report().size() == 2u * PPU_CTX_MAX);
    static ppu_context extra[2];
    ppu_ctx_register(&extra[0]);
    ppu_ctx_register(&extra[1]);
    CHECK(g_full_warnings == 2);
    CHECK(report().size() == 2u * PPU_CTX_MAX);

    if (failures) return 1;
    std::puts("PASS ppu_ctx_registry");
    return 0;
}
