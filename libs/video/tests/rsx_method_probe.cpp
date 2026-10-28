/*
 * The method classification table, exercised from its header (issue #47).
 *
 * The python suite runs this probe instead of reading the table's source text: the four verdicts, the
 * value pinning that turns an allow-listed register into a missing state when the guest writes another
 * value, the table's own completeness/ordering rules, and the stop helper's machine line.
 *
 * Built by tools/test_rsx_methods.py:
 *   c++ -std=c++17 -I include -I libs/video rsx_method_probe.cpp runtime/diag/stop.cpp -o probe
 *
 * Prints one line per check and exits non-zero if any failed.
 */
#include "../rsx_method_class.h"
#include "ps3emu/stop.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

static unsigned checks, failures;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    checks++;
    failures += !ok;
}

static bool has(const char* haystack, const char* needle)
{ return haystack && std::strstr(haystack, needle) != nullptr; }

int main(int argc, char** argv)
{
    /* `--dump`: one machine-readable line per row, for tools/test_rsx_methods.py to compare with the
     * table in docs/RSX_GRAPHICS.md. The doc is documentation; the header is the classification. */
    if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) {
        for (unsigned i = 0; i < RSX_METHOD_RULE_COUNT; i++) {
            const rsx_method_rule* r = &rsx_method_rules[i];
            const char* v = r->verdict == RSX_METHOD_HANDLED ? "handled"
                          : r->verdict == RSX_METHOD_INERT ? "inert" : "missing";
            std::printf("ROW %s 0x%04X %u %s %s\n", rsx_class_name((rsx_class_id)r->cls), r->method,
                        r->count, v, rsx_evidence_name((rsx_evidence)r->evidence));
        }
        return 0;
    }
    /* 1. A handled pair names its consumer. */
    {
        const rsx_method_rule* r = rsx_method_rule_find(RSX_CLASS_NV4097, 0x0180);
        check(r && r->verdict == RSX_METHOD_HANDLED && has(r->consumer, "rsx_process_method"),
              "a handled pair returns handled and names its consumer (NV4097 0x0180 -> rsx_process_method)");
        const rsx_method_rule* q = nullptr;
        check(rsx_method_check(RSX_CLASS_NV4097, 0x0394, 0x3E800000u, &q) == RSX_METHOD_HANDLED && q &&
              has(q->consumer, "depth"),
              "the depth range is handled, not allow-listed (NV4097 0x0394)");
    }

    /* 2. An inert pair is inert only for the value it pins; another value is a missing state. */
    {
        const rsx_method_rule* r = nullptr;
        check(rsx_method_check(RSX_CLASS_NV4097, 0x0300, 1u, &r) == RSX_METHOD_INERT && r && r->mask == 1u,
              "an inert pair returns inert for its pinned value (NV4097 0x0300 = 1)");
        const rsx_method_rule* s = nullptr;
        check(rsx_method_check(RSX_CLASS_NV4097, 0x0300, 0u, &s) == RSX_METHOD_MISSING,
              "...and missing for a different value, because the row pins it");
        check(rsx_method_check(RSX_CLASS_NV4097, 0x0300, 1u, nullptr) == RSX_METHOD_INERT &&
              rsx_method_check(RSX_CLASS_NV4097, 0x0300, 0u, nullptr) == RSX_METHOD_MISSING,
              "the rule finder and the value check agree without an out-parameter");
        check(rsx_method_check(RSX_CLASS_NV4097, 0x1FD8, 0u, nullptr) == RSX_METHOD_MISSING &&
              rsx_method_check(RSX_CLASS_NV4097, 0x1FD8, 1u, nullptr) == RSX_METHOD_MISSING,
              "an S2 gap is missing for every value (NV4097 0x1FD8 invalidate L2, #64)");
    }

    /* 3. A pair with no row is unknown. */
    {
        const rsx_method_rule* r = nullptr;
        check(rsx_method_check(RSX_CLASS_NV4097, 0x1F80u, 0u, &r) == RSX_METHOD_UNKNOWN && !r,
              "an unclassified pair returns unknown and no rule");
        check(rsx_method_check(RSX_CLASS_NV406E, 0x0100u, 0u, &r) == RSX_METHOD_UNKNOWN,
              "...and the class is part of the key (NV406E 0x0100 is not NV4097 0x0100)");
    }

    /* 4. Table hygiene: every row names itself and a consumer/reason, INERT rows pin a value or cite the
     *    oracle's reset value, and no two rows overlap. */
    {
        unsigned bad_name = 0, bad_reason = 0, bad_pin = 0, overlaps = 0, unsorted = 0;
        for (unsigned i = 0; i < RSX_METHOD_RULE_COUNT; i++) {
            const rsx_method_rule* a = &rsx_method_rules[i];
            if (!a->name || !*a->name) bad_name++;
            if (!a->consumer || !*a->consumer) bad_reason++;
            if (a->verdict == RSX_METHOD_INERT && a->mask == 0 && a->evidence != RSX_EV_ORACLE_NULL &&
                a->evidence != RSX_EV_ORDER && a->evidence != RSX_EV_REDUNDANT &&
                a->evidence != RSX_EV_ABSENT_STAGE && a->evidence != RSX_EV_NO_TRANSFER)
                bad_pin++;
            if (i && (a->cls < rsx_method_rules[i - 1].cls ||
                      (a->cls == rsx_method_rules[i - 1].cls && a->method <= rsx_method_rules[i - 1].method)))
                unsorted++;
            for (unsigned k = 0; k < i; k++) {
                const rsx_method_rule* b = &rsx_method_rules[k];
                if (a->cls != b->cls) continue;
                const unsigned ae = a->method + a->count * 4u, be = b->method + b->count * 4u;
                if (a->method < be && b->method < ae) overlaps++;
            }
        }
        check(bad_name == 0, "every row has a register name");
        check(bad_reason == 0, "every row names its consumer or the reason it is inert/missing");
        check(bad_pin == 0, "every inert row pins a value or cites a structural reason (order/redundant/stage/no transfer)");
        check(overlaps == 0, "no two rows cover the same method");
        check(unsorted == 0, "rows are sorted by class then method (the lookup's contract)");
    }

    /* 5. The stop helper fills the machine line the walker raises: class, method, subchannel, fifo,
     *    word, data. Run in a child: a stop ends the process. */
    {
        int fds[2];
        if (pipe(fds) != 0) { check(false, "pipe for the stop probe"); }
        else {
            std::fflush(stdout);
            const pid_t pid = fork();
            if (pid == 0) {
                close(fds[0]);
                dup2(fds[1], 2);
                ps3_stop_rsx_unknown_method("NV4097", 0x1F80u, 3u, 0x00001234u, 0x00001238u, 0xDEADBEEFu);
                _exit(0);
            }
            close(fds[1]);
            std::string out;
            char buf[4096];
            for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;) out.append(buf, (size_t)n);
            close(fds[0]);
            int status = 0;
            waitpid(pid, &status, 0);
            const bool stopped = WIFEXITED(status) && WEXITSTATUS(status) == PS3_STOP_EXIT_CODE;
            const bool named = out.find("kind=rsx-unknown-method") != std::string::npos;
            const bool fields = out.find("class=NV4097") != std::string::npos &&
                                out.find("method=0x1F80") != std::string::npos &&
                                out.find("subchannel=3") != std::string::npos &&
                                out.find("fifo=0x00001234") != std::string::npos &&
                                out.find("word=0x00001238") != std::string::npos &&
                                out.find("data=0xDEADBEEF") != std::string::npos;
            check(stopped && named && fields,
                  "the stop helper fills class, method, subchannel, fifo, word and data (rsx-unknown-method)");
            if (!(stopped && named && fields))
                std::printf("  status=%d\n  child stderr: %.600s\n", status, out.c_str());
        }
    }

    std::printf("probe checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
