/* Raises one named stop per invocation; test_stop.py runs it and inspects the
 * exit status and stderr. Each scenario is a separate process because a stop
 * ends the process, which is the contract under test. */

#include "ps3emu/stop.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static void hle_provider(uint64_t, ps3_stop_frontier* out)
{
    out->has_hle = 1;
    out->hle_nid = 0xDEADBEEFu;
    out->hle_name = "cellProbeLastCall";
}

static void syscall_provider(uint64_t thread_id, ps3_stop_frontier* out)
{
    if (thread_id != 7) return;   // only the stopping thread's own in-flight call counts
    out->has_syscall = 1;
    out->syscall_num = 141;
}

static void stopping_provider(uint64_t, ps3_stop_frontier*)
{
    ps3_stop(PS3_STOP_CONFIG_INVALID, "provider stopped", "none", nullptr, 0, nullptr);
}

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    const char* scenario = argv[1];

    if (!std::strcmp(scenario, "plain")) {
        ps3_stop_field f[] = { {"path", "/tmp/x.elf"}, {"expected", "aa"}, {"actual", "bb"} };
        ps3_stop(PS3_STOP_IDENTITY_MISMATCH, "the executable differs from the one lifted",
                 "re-lift from this file", f, 3, nullptr);
    }
    if (!std::strcmp(scenario, "site")) {
        ps3_stop_register_frontier(hle_provider);
        ps3_stop_register_frontier(hle_provider);   // idempotent
        ps3_stop_register_frontier(syscall_provider);
        ps3_stop_field f[] = { {"nid", "0x12345678"}, {"name", "cellFooBar"} };
        ps3_stop_site s = { 0x00123456, 0x00123450, 7 };
        ps3_stop(PS3_STOP_UNRESOLVED_NID, "import has no HLE handler",
                 "implement it or allow-list it", f, 2, &s);
    }
    if (!std::strcmp(scenario, "other-thread")) {
        // syscall_provider ignores every thread but 7: the frontier must not
        // claim a syscall the stopping thread was not inside.
        ps3_stop_register_frontier(syscall_provider);
        ps3_stop_site s = { 0x10, 0x20, 9 };
        ps3_stop(PS3_STOP_UNKNOWN_INSTRUCTION, "x", "y", nullptr, 0, &s);
    }
    if (!std::strcmp(scenario, "escape")) {
        ps3_stop_field f[] = {
            {"Mixed-Key", "has space"},
            {"kind", "collides"},
            {"quote", "a\"b\\c"},
            {"multi", "line1\nline2"},
            {"empty", ""},
            {"ctl", "tab\there\x01"},
        };
        ps3_stop(PS3_STOP_CONFIG_INVALID, "w", "r", f, 6, nullptr);
    }
    if (!std::strcmp(scenario, "kind") && argc == 3) {
        int k = std::atoi(argv[2]);
        ps3_stop((ps3_stop_kind)k, "a kind probe", "none", nullptr, 0, nullptr);
    }
    if (!std::strcmp(scenario, "kinds")) {
        for (int k = 0; k < PS3_STOP_KIND_COUNT; ++k) std::printf("%s\n", ps3_stop_kind_id((ps3_stop_kind)k));
        return 0;
    }
    if (!std::strcmp(scenario, "bare")) {
        ps3_stop(PS3_STOP_UNLIFTED_FUNCTION, nullptr, nullptr, nullptr, 0, nullptr);
    }
    if (!std::strcmp(scenario, "nested")) {
        ps3_stop_register_frontier(stopping_provider);
        ps3_stop(PS3_STOP_UNRESOLVED_INDIRECT_CALL, "outer", "outer", nullptr, 0, nullptr);
    }
    if (!std::strcmp(scenario, "buffered-stdout")) {
        std::printf("log line before the stop\n");   // stdout is a pipe here: block-buffered
        ps3_stop(PS3_STOP_UNIMPLEMENTED_SYSCALL, "w", "r", nullptr, 0, nullptr);
    }
    if (!std::strcmp(scenario, "concurrent")) {
        std::atomic<int> ready{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 16; ++i)
            ts.emplace_back([i, &ready] {
                char tid[16];
                std::snprintf(tid, sizeof tid, "%d", i);
                ps3_stop_field f[] = { {"worker", tid} };
                ps3_stop_site s = { (uint64_t)i, 0, (uint64_t)i };
                ready.fetch_add(1);
                while (ready.load() < 16) std::this_thread::yield();
                ps3_stop(PS3_STOP_UNRESOLVED_NID, "concurrent stop", "none", f, 1, &s);
            });
        for (auto& t : ts) t.join();   // never reached: the first stop exits the process
        return 3;
    }
    std::fprintf(stderr, "unknown scenario %s\n", scenario);
    return 2;
}
