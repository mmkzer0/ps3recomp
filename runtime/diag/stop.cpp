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

} // namespace

/* runtime/spu/spu_dma.h is a C header inlined into header-only SPU fixtures,
 * so it cannot call ps3_stop() itself without making every fixture link this
 * file. It calls through this pointer instead (see the hook block there); a
 * title links both, and the PPU scaffold already references ps3_stop(). */
static void spu_dma_repeat_stop(uint32_t image, uint32_t pc, uint64_t ea, uint32_t lsa,
                                uint32_t cmd, uint32_t size, uint32_t count, uint64_t secs,
                                uint32_t tid)
{
    char image_s[16], pc_s[16], ea_s[24], lsa_s[16], cmd_s[8], size_s[16], count_s[16], secs_s[24];
    std::snprintf(image_s, sizeof image_s, "%u", image);
    std::snprintf(pc_s, sizeof pc_s, "0x%05X", pc);
    std::snprintf(ea_s, sizeof ea_s, "0x%08X", (uint32_t)ea);
    std::snprintf(lsa_s, sizeof lsa_s, "0x%05X", lsa);
    std::snprintf(cmd_s, sizeof cmd_s, "0x%02X", cmd);
    std::snprintf(size_s, sizeof size_s, "%u", size);
    std::snprintf(count_s, sizeof count_s, "%u", count);
    std::snprintf(secs_s, sizeof secs_s, "%llu", (unsigned long long)secs);
    ps3_stop_field f[] = {
        {"image", image_s}, {"ea", ea_s}, {"lsa", lsa_s}, {"cmd", cmd_s},
        {"size", size_s}, {"count", count_s}, {"secs", secs_s},
    };
    /* pc and tid are the site, so they stay the reserved pc=/tid= keys. */
    ps3_stop_site site = { pc, 0, tid };
    ps3_stop(PS3_STOP_SPU_DMA_REPEAT,
             "an SPU has issued the same MFC transfer continuously past spu.dma_repeat_stop_secs",
             "If the job is legitimately polling memory another agent writes (ring space, a "
             "label), raise spu.dma_repeat_stop_secs or set it to 0, which disables this "
             "watchdog; otherwise the protocol this transfer belongs to is wedged.",
             f, sizeof f / sizeof f[0], &site);
}

extern "C" {

void (*g_spu_dma_repeat_stop)(uint32_t, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t,
                              uint32_t, uint64_t, uint32_t) = spu_dma_repeat_stop;

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
    std::_Exit(PS3_STOP_EXIT_CODE);
}

void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy,
              const ps3_stop_field* fields, size_t nfields,
              const ps3_stop_site* site)
{
    const char* id = ps3_stop_kind_id(kind);

    if (t_stopping) {
        write_all(std::string("PS3STOP v=1 kind=") + id + " nested=1\n");
        std::_Exit(PS3_STOP_EXIT_CODE);
    }
    t_stopping = true;
    g_stop_lock.lock();

    // Whatever the guest or the runtime had buffered goes out first, so the
    // log leading up to the stop is complete when the banner lands after it.
    std::fflush(stdout);

    ps3_stop_frontier fr = {};
    for (auto& slot : g_providers) {
        ps3_stop_frontier_fn fn = slot.load(std::memory_order_acquire);
        if (fn) fn(site ? site->thread_id : 0, &fr);
    }

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
    std::_Exit(PS3_STOP_EXIT_CODE);
}

} // extern "C"
