/*
 * ps3recomp - named stop (see include/ps3emu/stop.h for the contract)
 *
 * Everything is composed into one string first and written with a single
 * fwrite, so no other thread's stderr output can land inside the banner and a
 * parser reading the stream sees the machine line whole.
 */

#include "ps3emu/stop.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* const kKindIds[] = {
#define PS3_STOP_KIND_ID(name, id) id,
    PS3_STOP_KINDS(PS3_STOP_KIND_ID)
#undef PS3_STOP_KIND_ID
};
static_assert(sizeof kKindIds / sizeof kKindIds[0] == PS3_STOP_KIND_COUNT,
              "one id per kind");

constexpr size_t kMaxProviders = 8;

/* Constant-initialised, so a provider may register from a static initialiser in
 * any translation unit regardless of initialisation order. */
std::atomic<ps3_stop_frontier_fn> g_providers[kMaxProviders];

/* Serialises stops. Never released: the winner exits the process, and every
 * other stopping thread parking here is the intended outcome. */
std::mutex g_stop_lock;
thread_local bool t_stopping = false;

const char* const kReservedKeys[] = {
    "v", "kind", "pc", "lr", "tid", "last_hle_nid", "last_hle", "syscall",
};

bool is_reserved(const std::string& key)
{
    for (const char* r : kReservedKeys)
        if (key == r) return true;
    return false;
}

/* [a-z][a-z0-9_]*, with anything else rewritten so the line stays parseable. */
std::string sanitize_key(const char* key)
{
    std::string out;
    if (key) for (const char* p = key; *p; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        bool ok = (c >= 'a' && c <= 'z') || c == '_' || (!out.empty() && c >= '0' && c <= '9');
        out += ok ? c : '_';
    }
    if (out.empty()) out = "_";
    if (is_reserved(out)) out.insert(0, "f_");
    return out;
}

bool is_bare(const char* v)
{
    if (!*v) return false;
    for (const char* p = v; *p; ++p) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == ':' || c == '/' || c == '+' || c == '@' ||
                  c == ',' || c == '-';
        if (!ok) return false;
    }
    return true;
}

void append_value(std::string& out, const char* v)
{
    if (!v) v = "";
    if (is_bare(v)) { out += v; return; }
    out += '"';
    for (const unsigned char* p = (const unsigned char*)v; *p; ++p) {
        switch (*p) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (*p < 0x20 || *p >= 0x7F) {
                char esc[5];
                std::snprintf(esc, sizeof esc, "\\x%02X", *p);
                out += esc;
            } else {
                out += char(*p);
            }
        }
    }
    out += '"';
}

std::string hex(uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof b, "0x%llX", (unsigned long long)v);
    return b;
}

void kv(std::string& line, const std::string& key, const char* value)
{
    line += ' ';
    line += key;
    line += '=';
    append_value(line, value);
}

void write_all(const std::string& s)
{
    std::fwrite(s.data(), 1, s.size(), stderr);
    std::fflush(stderr);
}

// Every way out of a stop ends here. std::_Exit runs no atexit handler and
// flushes no stream, and ps3_stdio fully buffers stdout and stderr (#54), so
// what the runtime or the guest wrote before the stop is pushed out first on
// every path; a nested stop or a full provider table would otherwise drop it.
[[noreturn]] void exit_stopped()
{
    std::fflush(nullptr);
    std::_Exit(PS3_STOP_EXIT_CODE);
}

} // namespace

extern "C" {

const char* ps3_stop_kind_id(ps3_stop_kind kind)
{
    return (unsigned)kind < (unsigned)PS3_STOP_KIND_COUNT ? kKindIds[kind] : "invalid-kind";
}

void ps3_stop_register_frontier(ps3_stop_frontier_fn fn)
{
    if (!fn) return;
    for (auto& slot : g_providers) {
        ps3_stop_frontier_fn cur = slot.load(std::memory_order_acquire);
        if (cur == fn) return;
        if (cur) continue;
        if (slot.compare_exchange_strong(cur, fn, std::memory_order_acq_rel)) return;
        if (cur == fn) return;   // lost the race to the same provider
    }
    // Eight providers is far more than the runtime has; running out is a bug in
    // whoever added the ninth, and dropping its frontier silently would hide it.
    write_all("PS3STOP v=1 kind=invalid-kind detail=frontier-provider-table-full\n");
    exit_stopped();
}

void ps3_stop_query_frontier(uint64_t thread_id, ps3_stop_frontier* out)
{
    for (auto& slot : g_providers) {
        ps3_stop_frontier_fn fn = slot.load(std::memory_order_acquire);
        if (fn) fn(thread_id, out);
    }
}

void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy,
              const ps3_stop_field* fields, size_t nfields,
              const ps3_stop_site* site)
{
    const char* id = ps3_stop_kind_id(kind);

    if (t_stopping) {
        write_all(std::string("PS3STOP v=1 kind=") + id + " nested=1\n");
        exit_stopped();
    }
    t_stopping = true;
    g_stop_lock.lock();

    // Whatever the guest or the runtime had buffered, on either stream, goes
    // out before the frontier providers run, so the log leading up to the stop
    // is complete even if a provider stops again, and the banner lands after it.
    std::fflush(nullptr);

    ps3_stop_frontier fr = {};
    ps3_stop_query_frontier(site ? site->thread_id : 0, &fr);

    if (!what || !*what) what = "(no description: this is a bug in the caller)";
    if (!remedy || !*remedy) remedy = "(none given: this is a bug in the caller)";

    std::string line = "PS3STOP v=1";
    kv(line, "kind", id);

    std::vector<std::pair<std::string, std::string>> rows;
    rows.emplace_back("what", what);
    for (size_t i = 0; i < nfields; ++i) {
        std::string key = sanitize_key(fields[i].key);
        const char* value = fields[i].value ? fields[i].value : "";
        rows.emplace_back(key, value);
        kv(line, key, value);
    }
    if (site) {
        rows.emplace_back("guest", "pc=" + hex(site->pc) + " lr=" + hex(site->lr) +
                                   " tid=" + std::to_string(site->thread_id));
        kv(line, "pc", hex(site->pc).c_str());
        kv(line, "lr", hex(site->lr).c_str());
        kv(line, "tid", std::to_string(site->thread_id).c_str());
    }
    if (fr.has_hle || fr.has_syscall) {
        std::string text;
        if (fr.has_hle) {
            text = "last HLE import " + hex(fr.hle_nid);
            if (fr.hle_name && *fr.hle_name) text += std::string(" (") + fr.hle_name + ")";
            kv(line, "last_hle_nid", hex(fr.hle_nid).c_str());
            if (fr.hle_name && *fr.hle_name) kv(line, "last_hle", fr.hle_name);
        }
        if (fr.has_syscall) {
            if (!text.empty()) text += "; ";
            text += "inside lv2 syscall " + std::to_string(fr.syscall_num);
            kv(line, "syscall", std::to_string(fr.syscall_num).c_str());
        }
        rows.emplace_back("frontier", text);
    }
    rows.emplace_back("remedy", remedy);
    rows.emplace_back("exit", "status " + std::to_string(PS3_STOP_EXIT_CODE));

    size_t width = 8;
    for (auto& r : rows) if (r.first.size() > width) width = r.first.size();

    const std::string rule(78, '=');
    std::string out = "\n" + rule + "\nPS3RECOMP STOP: " + id + "\n" + rule + "\n";
    for (auto& r : rows) {
        out += "  " + r.first + std::string(width - r.first.size() + 2, ' ');
        for (char c : r.second) {
            out += c;
            if (c == '\n') out += std::string(width + 4, ' ');
        }
        out += '\n';
    }
    out += rule + "\n" + line + "\n";

    write_all(out);
    exit_stopped();
}

void ps3_stop_fiber_host_context(const char* backend, const char* op, const char* reason,
                                 uint32_t ea, uint32_t error, const char* error_domain)
{
    char address[16], code[16];
    std::snprintf(address, sizeof address, "0x%X", ea);
    std::snprintf(code, sizeof code, "%u", error);
    const ps3_stop_field fields[] = {
        {"backend", backend}, {"op", op}, {"reason", reason},
        {"ea", address}, {"error", code}, {"error_domain", error_domain},
    };
    ps3_stop(PS3_STOP_FIBER_HOST_CONTEXT,
             "the host could not create or switch a guest fiber execution context",
             "For a failed host call, select or port PS3RECOMP_FIBER_BACKEND "
             "(docs/PLATFORM_ABSTRACTION.md). A broken invariant is a runtime defect: "
             "report it with this line.",
             fields, sizeof fields / sizeof fields[0], nullptr);
}

void ps3_stop_guest_range(const char* func, const char* arg,
                          uint64_t ea, uint64_t size, uint64_t limit)
{
    char ea_s[24], sz_s[24], lim_s[24];
    std::snprintf(ea_s, sizeof ea_s, "0x%llX", (unsigned long long)ea);
    std::snprintf(sz_s, sizeof sz_s, "0x%llX", (unsigned long long)size);
    std::snprintf(lim_s, sizeof lim_s, "0x%llX", (unsigned long long)limit);
    const ps3_stop_field f[] = {
        {"func", func ? func : "unknown"},
        {"arg", arg ? arg : "unknown"},
        {"ea", ea_s},
        {"size", sz_s},
        {"limit", lim_s},
    };
    ps3_stop(PS3_STOP_HLE_GUEST_RANGE,
             "an HLE handler was given a guest EA or span outside guest memory",
             "Fix the caller to pass an address inside guest memory, or enlarge ppu.vm_size.",
             f, sizeof f / sizeof f[0], nullptr);
}

void ps3_stop_commit_failed(const char* func, const char* arg,
                            uint64_t ea, uint64_t size, int32_t rc)
{
    char ea_s[24], sz_s[24], rc_s[24];
    std::snprintf(ea_s, sizeof ea_s, "0x%llX", (unsigned long long)ea);
    std::snprintf(sz_s, sizeof sz_s, "0x%llX", (unsigned long long)size);
    std::snprintf(rc_s, sizeof rc_s, "0x%08X", (unsigned)rc);
    const ps3_stop_field f[] = {
        {"func", func ? func : "unknown"},
        {"arg", arg ? arg : "unknown"},
        {"ea", ea_s},
        {"size", sz_s},
        {"rc", rc_s},
    };
    ps3_stop(PS3_STOP_HLE_COMMIT_FAILED,
             "a guest span inside guest memory could not be committed for a host access",
             "A host fault (mprotect/VirtualAlloc); check the host's memory limits and the "
             "arena geometry, then retry. The guest span itself was in range.",
             f, sizeof f / sizeof f[0], nullptr);
}

void ps3_stop_hle_uninitialized(const char* func, const char* module, const char* reason)
{
    const ps3_stop_field f[] = {
        {"func", func ? func : "unknown"},
        {"module", module ? module : "unknown"},
        {"reason", reason ? reason : "unknown"},
    };
    ps3_stop(PS3_STOP_HLE_UNINITIALIZED,
             "an HLE function was called before its module was initialised and the firmware's behaviour there is undefined",
             "Fix the caller to initialise the module first, or implement the firmware's behaviour for this call.",
             f, sizeof f / sizeof f[0], nullptr);
}

void ps3_stop_gcm_unmodelled(const char* func, const char* feature, const char* reason)
{
    const ps3_stop_field f[] = {
        {"func", func ? func : "unknown"},
        {"feature", feature ? feature : "unknown"},
        {"reason", reason ? reason : "unknown"},
    };
    ps3_stop(PS3_STOP_GCM_UNMODELLED,
             "the firmware's command-stream path for this configuration is not reproduced, and a stream built without it would be a guess",
             "Model the firmware path (libresc/libgcm_sys), or run a configuration that does not reach it.",
             f, sizeof f / sizeof f[0], nullptr);
}

void ps3_stop_gcm_wait_timeout(const char* func, const char* what, uint32_t offset, uint32_t value,
                               uint32_t current, uint32_t waited_ms)
{
    char off_s[24], val_s[24], cur_s[24], ms_s[24];
    std::snprintf(off_s, sizeof off_s, "0x%08X", offset);
    std::snprintf(val_s, sizeof val_s, "0x%08X", value);
    std::snprintf(cur_s, sizeof cur_s, "0x%08X", current);
    std::snprintf(ms_s, sizeof ms_s, "%u", waited_ms);
    const ps3_stop_field f[] = {
        {"func", func ? func : "unknown"},
        {"what", what ? what : "unknown"},
        {"offset", off_s},
        {"value", val_s},
        {"current", cur_s},
        {"waited_ms", ms_s},
    };
    ps3_stop(PS3_STOP_GCM_WAIT_TIMEOUT,
             "an RSX wait stood unsatisfied past gcm.wait_timeout_ms; the agent that should end it never ran",
             "Find the producer of the label (SPU job, driver) or raise gcm.wait_timeout_ms if the producer is only slow.",
             f, sizeof f / sizeof f[0], nullptr);
}

void ps3_stop_spu_label_window(const char* func, uint32_t cmd, uint64_t ea,
                               uint32_t size, uint32_t tag, uint64_t count,
                               const ps3_stop_site* site)
{
    char cmd_s[8], ea_s[24], size_s[16], tag_s[8], count_s[24];
    std::snprintf(cmd_s, sizeof cmd_s, "0x%02X", (unsigned)(cmd & 0xFFu));
    std::snprintf(ea_s, sizeof ea_s, "0x%llX", (unsigned long long)ea);
    std::snprintf(size_s, sizeof size_s, "%u", (unsigned)size);
    std::snprintf(tag_s, sizeof tag_s, "%u", (unsigned)tag);
    std::snprintf(count_s, sizeof count_s, "%llu", (unsigned long long)count);
    const ps3_stop_field f[] = {
        {"func", func ? func : "unknown"},
        {"cmd", cmd_s},
        {"ea", ea_s},
        {"size", size_s},
        {"tag", tag_s},
        {"count", count_s},
    };
    ps3_stop(PS3_STOP_SPU_LABEL_WINDOW,
             "an SPU MFC PUT/PUTL writes into the guest store window a device model registered",
             "The runtime cannot tell whether this PUT produces the value an RSX ACQUIRE waits for or "
             "overwrites a label the walker has not read: order the title's protocol explicitly (a PPU "
             "store, or a label outside the window) instead of letting the SPU write it.",
             f, sizeof f / sizeof f[0], site);
}

void ps3_stop_lifter_abi(uint32_t expected, uint32_t got)
{
    char exp_s[24], got_s[24];
    std::snprintf(exp_s, sizeof exp_s, "%u", expected);
    std::snprintf(got_s, sizeof got_s, "%u", got);
    const ps3_stop_field f[] = { {"expected", exp_s}, {"got", got_s} };
    ps3_stop(PS3_STOP_LIFTER_ABI,
             "the lifted code was generated for a different lifted-code ABI than this runtime implements; "
             "running it would silently skip hooks the runtime assumes (a changed store-hook ABI, for example)",
             "Re-lift the executable with this runtime's tools/ppu_lifter.py (and regenerate its header), "
             "or build the runtime that matches the lift.",
             f, sizeof f / sizeof f[0], nullptr);
}

void ps3_stop_spurs_task_halt(const char* api, uint32_t taskset_ea, uint32_t task,
                              uint32_t elf_ea, const char* reason, const ps3_stop_site* site)
{
    char ts_s[24], task_s[24], elf_s[24];
    std::snprintf(ts_s, sizeof ts_s, "0x%08X", taskset_ea);
    std::snprintf(task_s, sizeof task_s, "%u", task);
    std::snprintf(elf_s, sizeof elf_s, "0x%08X", elf_ea);
    const ps3_stop_field f[] = {
        {"api", api ? api : "unknown"},
        {"taskset", ts_s},
        {"task", task_s},
        {"elf", elf_s},
        {"reason", reason ? reason : "unknown"},
    };
    ps3_stop(PS3_STOP_SPURS_TASK_HALT,
             "a SPURS task's ELF cannot be loaded by the taskset policy module, which halts the SPU on the console",
             "Pass cellSpursCreateTask a 16-byte aligned 32-bit big-endian SPU ELF whose load segments lie in the task area "
             "(local store 0x3000..0x40000); a task whose TaskInfo.elf is 0 or not an ELF never runs on the console either.",
             f, sizeof f / sizeof f[0], site);
}

void ps3_stop_spurs_dispatch_miss(const char* api, uint32_t owner_ea, int32_t task,
                                  uint32_t image_ea, uint32_t size, uint64_t fingerprint,
                                  const ps3_stop_site* site)
{
    char owner_s[24], task_s[24], img_s[24], size_s[24], fp_s[24];
    std::snprintf(owner_s, sizeof owner_s, "0x%08X", owner_ea);
    std::snprintf(task_s, sizeof task_s, "%d", task);
    std::snprintf(img_s, sizeof img_s, "0x%08X", image_ea);
    std::snprintf(size_s, sizeof size_s, "0x%X", size);
    std::snprintf(fp_s, sizeof fp_s, "0x%016llX", (unsigned long long)fingerprint);
    /* A job has no task id: leave the field out rather than print -1. */
    ps3_stop_field f[6];
    size_t n = 0;
    f[n++] = {"api", api ? api : "unknown"};
    f[n++] = {task >= 0 ? "taskset" : "job", owner_s};
    if (task >= 0) f[n++] = {"task", task_s};
    f[n++] = {"image", img_s};
    f[n++] = {"size", size_s};
    f[n++] = {"fingerprint", fp_s};
    ps3_stop(PS3_STOP_SPURS_DISPATCH_MISS,
             "a SPURS task or job names an SPU program the runtime has no way to run, and reporting it started or done would be success for work that never ran",
             "Lift the SPU image and register it (spu_workload_register; SPU_DUMP_MISS=<dir> writes the unrecognised image out), or run a configuration that does not reach it.",
             f, n, site);
}

void ps3_stop_rsx_capture(const char* func, const char* reason, const char* detail)
{
    const ps3_stop_field f[] = {
        {"func", func ? func : "unknown"},
        {"reason", reason ? reason : "unknown"},
        {"detail", detail ? detail : ""},
    };
    ps3_stop(PS3_STOP_RSX_CAPTURE,
             "an RSX capture or replay cannot be trusted: continuing would write or replay a capture that only looks complete",
             "Fix the named cause (docs/PS3DIFF.md, \"ps3diff.rsx/1\") and capture again; a capture that stopped is never a baseline.",
             f, sizeof f / sizeof f[0], nullptr);
}

void ps3_stop_rsx_unknown_method(const char* cls, uint32_t method, uint32_t subchannel,
                                 uint32_t fifo, uint32_t word, uint32_t data)
{
    char method_s[16], sub_s[16], fifo_s[16], word_s[16], data_s[16];
    snprintf(method_s, sizeof method_s, "0x%04X", method);
    snprintf(sub_s, sizeof sub_s, "%u", subchannel);
    snprintf(fifo_s, sizeof fifo_s, "0x%08X", fifo);
    snprintf(word_s, sizeof word_s, "0x%08X", word);
    snprintf(data_s, sizeof data_s, "0x%08X", data);
    const ps3_stop_field f[] = {
        {"class", cls ? cls : "invalid"},
        {"method", method_s},
        {"subchannel", sub_s},
        {"fifo", fifo_s},
        {"word", word_s},
        {"data", data_s},
    };
    ps3_stop(PS3_STOP_RSX_UNKNOWN_METHOD,
             "an RSX FIFO method has no consumer and no allow-list entry: the console applied it and this runtime would drop it",
             "Classify the (class, method) pair in libs/video/rsx_method_class.h and docs/RSX_GRAPHICS.md: name the consumer, cite why it is inert, or record it as a missing state (an S2 row) — never widen the allow-list without evidence.",
             f, sizeof f / sizeof f[0], nullptr);
}

} // extern "C"
