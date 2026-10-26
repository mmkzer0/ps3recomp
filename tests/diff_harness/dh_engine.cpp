/* See dh_engine.h. Each side of each case runs in its own forked child so that
 * a named stop (exit 70), a host fault or a hang is an outcome to compare, not
 * the end of the run, and so both sides start from byte-identical memory.
 *
 * The compared surface is the whole guest arena minus the guest stack: the
 * module image, the primed data window, TLS and every other byte a call can
 * touch. A write outside the arena does not exist in this model, and the 4 GiB
 * PROT_NONE reservation makes such an access a deterministic guest-fault. */
#include "dh_engine.h"
#include "dh_json.h"
#include "ppu/ppu_context.h"
#include "prx/prx_loader.h"
#include "ps3emu/hle_observer.h"
#include "ps3emu/sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <poll.h>
#include <set>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#ifndef PS3RECOMP_TOOLKIT_SHA
#define PS3RECOMP_TOOLKIT_SHA "unknown"
#endif
#ifndef PS3RECOMP_REPO_ROOT
#define PS3RECOMP_REPO_ROOT ""
#endif
/* The synthetic CI fixture is checked-in source, not firmware, and its image
 * and report necessarily live in the build tree inside the repository. Only
 * that binary is built with this define; every firmware-backed runner keeps the
 * repo-path refusal unconditional. */
#ifndef PS3RECOMP_DIFF_SYNTHETIC
#define PS3RECOMP_DIFF_SYNTHETIC 0
#endif

extern "C" {
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ppu_hle_init(void);
void ps3_hle_call(uint32_t, ppu_context*);
int  ps3_hle_has(uint32_t);
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern void (*g_ppu_thread_entry_trampoline)(ppu_context*);
void dh_entry_trampoline(ppu_context* ctx);
void dh_syscall(ppu_context* c);
uint32_t prx_resolve_export(uint32_t nid);
}

namespace dh {

uint8_t* mem;
void wr8(uint32_t ea, uint8_t v)   { mem[ea] = v; }
void wr16(uint32_t ea, uint16_t v) { v = __builtin_bswap16(v); memcpy(mem + ea, &v, 2); }
void wr32(uint32_t ea, uint32_t v) { v = __builtin_bswap32(v); memcpy(mem + ea, &v, 4); }
void wr64(uint32_t ea, uint64_t v) { v = __builtin_bswap64(v); memcpy(mem + ea, &v, 8); }
uint32_t rd32(uint32_t ea) { uint32_t v; memcpy(&v, mem + ea, 4); return __builtin_bswap32(v); }
uint16_t rd16(uint32_t ea) { uint16_t v; memcpy(&v, mem + ea, 2); return __builtin_bswap16(v); }
void fill(uint32_t ea, uint32_t n, uint8_t v) { memset(mem + ea, v, n); }

Step step(const char* func, uint32_t nid, std::initializer_list<Arg> args)
{
    Step s;
    s.func = func; s.nid = nid;
    int i = 0;
    for (const Arg& a : args) if (i < 12) s.args[i++] = a;
    return s;
}

/* ---- lifted-code dispatch ----------------------------------------------- */
typedef void (*host_fn)(void*);
static std::unordered_map<uint32_t, host_fn> g_registry;   /* guest code EA -> lifted function */
static const dh_module*  g_mod;
static const ImportPolicy* g_pol;
static std::vector<ImportRec>* g_lifted_imports;            /* the lifted side's import log */
static std::vector<ImportRec>* g_hle_imports;               /* the HLE side's nested HLE calls */
static std::vector<size_t>     g_obs_stack;
static bool     g_top_call;
static uint32_t g_tramp_target;
static uint32_t g_cur_step;
static const std::vector<std::pair<uint32_t, uint32_t>>* g_mask;
static const std::vector<std::pair<uint32_t, uint32_t>>* g_mask_image;
static bool g_hle_side = false;

bool on_hle_side() { return g_hle_side; }

/* r14..r31 seed; see the ABI check in child_run/evaluate. */
constexpr uint64_t ABI_SENTINEL_BASE = 0xA5A5A5A500000000ull;
/* A case may capture at most this many values per step; the wire reader rejects
 * more instead of silently truncating, so a capture cannot hide a divergence. */
constexpr uint32_t kMaxCaptures = 64;

/* A harness gap inside a lifted call: no policy entry, or lifted code the host
 * cannot execute. Thrown rather than longjmp'd so live std::string/vector
 * locals unwind normally; caught at the step boundary in child_run. */
struct LiftBlock {
    std::string blocker;
    std::string detail;
};
[[noreturn]] static void lift_block(const char* blocker, const std::string& detail)
{
    throw LiftBlock{blocker, detail};
}

[[noreturn]] void model_stop(const char* reason)
{
    throw LiftBlock{"model-stop", reason};
}
[[noreturn]] void model_block(const char* what)
{
    /* A stand-in that must wait on state this harness cannot provide: block for
     * good, so the per-side alarm turns it into a timeout outcome. */
    char t[256];
    int n = snprintf(t, sizeof t, "kind=dh-blocked %s\n", what);
    if (write(STDERR_FILENO, t, (size_t)n) < 0) {}
    for (;;) pause();
}

static void register_fn(uint32_t addr, host_fn fn) { g_registry[addr] = fn; }
void register_guest_fn(uint32_t addr, void (*fn)(void*)) { g_registry[addr] = fn; }

static std::string hex(uint64_t v)
{
    char b[24];
    snprintf(b, sizeof b, "0x%llX", (unsigned long long)v);
    return b;
}

static std::string import_name(uint32_t nid)
{
    if (g_mod)
        for (uint32_t i = 0; i < g_mod->n_imports; ++i)
            if (g_mod->imports[i].nid == nid && g_mod->imports[i].name && *g_mod->imports[i].name)
                return g_mod->imports[i].name;
    return hex(nid);
}

/* How the HLE starts guest code on a fiber/thread context: ctx->cia names the OPD. */
extern "C" void dh_entry_trampoline(ppu_context* ctx)
{
    const uint32_t opd = (uint32_t)ctx->cia, code = rd32(opd), toc = rd32(opd + 4);
    auto it = g_registry.find(code);
    if (it == g_registry.end()) {
        fprintf(stderr, "kind=dh-unlifted-function addr=0x%X\n", code);
        _exit(72);
    }
    ctx->gpr[2] = toc; ctx->lr = SENTINEL;
    it->second(ctx);
}

/* Every import the lifted code makes lands here (its stub slot points at a
 * harness-owned descriptor whose code is TRAMP_BASE + 8*i). Log it, then follow
 * the module's policy for that NID. */
static void import_trampoline(ppu_context* c)
{
    const uint32_t idx = (g_tramp_target - TRAMP_BASE) / 8;
    const dh_import& im = g_mod->imports[idx];
    ImportRec r;
    r.nid = im.nid; r.name = im.name && *im.name ? im.name : "?";
    r.arg[0] = c->gpr[3]; r.arg[1] = c->gpr[4]; r.arg[2] = c->gpr[5]; r.arg[3] = c->gpr[6];
    r.policy = g_pol->of(im.nid); r.step = g_cur_step;
    g_lifted_imports->push_back(r);          /* keep the call even if it stops the side */
    if (r.policy == Policy::Stop)
        lift_block("import-policy", "import " + r.name + " (" + hex(im.nid) + ") is not allowed by the import policy");
    if (r.policy == Policy::Model) {
        auto mi = g_pol->models.find(im.nid);
        if (mi == g_pol->models.end())
            lift_block("import-model-missing", "import " + r.name + " (" + hex(im.nid) + ") has no harness model");
        mi->second(c);
        auto& back = g_lifted_imports->back();
        back.ret = c->gpr[3]; back.has_ret = true;
        return;
    }
    if (!ps3_hle_has(im.nid))
        lift_block("import-not-registered", "import " + r.name + " (" + hex(im.nid) + ") is forwarded but has no HLE handler");
    const uint64_t r2 = c->gpr[2], sp = c->gpr[1];
    uint64_t slot; memcpy(&slot, mem + sp + 0x28, 8);
    ps3_hle_call(im.nid, c);
    c->gpr[2] = r2; memcpy(mem + sp + 0x28, &slot, 8);   /* the stub's TOC spill belongs to the caller */
    auto& back = g_lifted_imports->back();
    back.ret = c->gpr[3]; back.has_ret = true;
}

/* Called by the lifted code's `sc` (via lv2_syscall in dh_runtime.cpp). */
extern "C" void dh_syscall(ppu_context* c)
{
    const uint64_t num = c->gpr[11];
    ImportRec r; r.nid = 0xFFFF0000u | (uint32_t)num; r.name = "lv2_syscall_" + std::to_string(num); r.step = g_cur_step;
    r.arg[0] = c->gpr[3]; r.arg[1] = c->gpr[4]; r.arg[2] = c->gpr[5]; r.arg[3] = c->gpr[6];
    auto it = g_pol->syscalls.find(num);
    if (it == g_pol->syscalls.end()) {
        r.policy = Policy::Stop; g_lifted_imports->push_back(r);
        lift_block("syscall-unmodelled", "lv2 syscall " + std::to_string(num) + " is not modelled by the harness");
    }
    r.policy = Policy::Model;
    it->second(c);
    r.ret = c->gpr[3]; r.has_ret = true; g_lifted_imports->push_back(r);
}

extern "C" void ps3_indirect_call(ppu_context* ctx)
{
    const uint32_t t = (uint32_t)ctx->ctr;
    if (t >= TRAMP_BASE && t < TRAMP_BASE + 8 * g_mod->n_imports && ((t - TRAMP_BASE) & 7) == 0) {
        g_tramp_target = t;
        import_trampoline(ctx);
        return;
    }
    auto it = g_registry.find(t);
    if (it == g_registry.end())
        lift_block("unlifted-function", "indirect call to unlifted guest address " + hex(t));
    it->second(ctx);
}

/* HLE-side nested calls: the observer sees the top-level case call too, which
 * child_run marks as skipped; everything the handler then calls through the
 * normal dispatcher is an imported call. */
static void obs_call(void*, uint32_t nid, const ppu_context* c)
{
    if (g_top_call) { g_top_call = false; return; }
    if (!g_hle_imports) return;
    ImportRec r;
    r.nid = nid; r.name = import_name(nid); r.policy = Policy::Forward; r.step = g_cur_step;
    r.arg[0] = c->gpr[3]; r.arg[1] = c->gpr[4]; r.arg[2] = c->gpr[5]; r.arg[3] = c->gpr[6];
    g_obs_stack.push_back(g_hle_imports->size());
    g_hle_imports->push_back(r);
}
static void obs_return(void*, uint32_t, const ppu_context* c)
{
    if (g_obs_stack.empty()) return;
    const size_t idx = g_obs_stack.back();
    g_obs_stack.pop_back();
    if (idx < g_hle_imports->size()) {
        (*g_hle_imports)[idx].ret = c->gpr[3];
        (*g_hle_imports)[idx].has_ret = true;
    }
}

/* ---- compared surface --------------------------------------------------- */
struct Span { uint32_t lo, hi; };
/* The whole arena except the guest stack (see dh_engine.h). */
static const Span kSpans[] = {{0, STACK_SCAN_LO}, {STACK_SCAN_HI, ARENA_SIZE}};

static bool masked(uint32_t a)
{
    if (g_mask) for (auto& m : *g_mask) if (a >= m.first && a < m.first + m.second) return true;
    if (g_mask_image)
        for (auto& m : *g_mask_image) {
            const uint32_t lo = g_mod->base + m.first;
            if (a >= lo && a < lo + m.second) return true;
        }
    return false;
}

static void collect_diff(const std::vector<uint8_t>& pristine, ByteDiff& out)
{
    out.clear();
    for (const Span& s : kSpans) {
        if (memcmp(mem + s.lo, pristine.data() + s.lo, s.hi - s.lo) == 0) continue;
        for (uint32_t a = s.lo; a < s.hi; ++a)
            if (mem[a] != pristine[a] && !masked(a)) out.push_back({a, mem[a]});
    }
}

static void on_fault(int sig, siginfo_t* si, void*)
{
    const uintptr_t a = (uintptr_t)si->si_addr, b = (uintptr_t)mem;
    if (a >= b && a - b < GUEST_SPACE) {
        char t[96];
        int n = snprintf(t, sizeof t, "kind=dh-guest-fault ea=0x%llX sig=%d\n", (unsigned long long)(a - b), sig);
        if (write(STDERR_FILENO, t, (size_t)n) < 0) {}
        _exit(75);
    }
    /* SA_RESETHAND: returning re-executes the access under the default action. */
}

/* ---- wire --------------------------------------------------------------- */
struct Wire {
    std::vector<uint8_t> b;
    void put(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    template <class T> void pod(const T& v) { put(&v, sizeof v); }
    void str(const std::string& s) { uint32_t n = (uint32_t)s.size(); pod(n); put(s.data(), n); }
};
struct Reader {
    const uint8_t* p; const uint8_t* e; bool ok = true;
    template <class T> T pod() { T v{}; if ((size_t)(e - p) < sizeof v) { ok = false; return v; } memcpy(&v, p, sizeof v); p += sizeof v; return v; }
    std::string str() { uint32_t n = pod<uint32_t>(); if (!ok || (size_t)(e - p) < n) { ok = false; return ""; } std::string s((const char*)p, n); p += n; return s; }
};
static void write_all(int fd, const uint8_t* p, size_t n)
{
    while (n) { ssize_t w = write(fd, p, n); if (w <= 0) _exit(99); p += w; n -= (size_t)w; }
}
static void send_rec(int fd, char tag, const Wire& w)
{
    uint32_t n = (uint32_t)w.b.size();
    write_all(fd, (const uint8_t*)&tag, 1); write_all(fd, (const uint8_t*)&n, 4);
    if (n) write_all(fd, w.b.data(), n);
}
static void enc_imports(Wire& w, const std::vector<ImportRec>& v)
{
    uint32_t n = (uint32_t)v.size(); w.pod(n);
    for (auto& i : v) {
        w.pod(i.nid); w.str(i.name); uint8_t p = (uint8_t)i.policy; w.pod(p);
        for (int k = 0; k < 4; ++k) w.pod(i.arg[k]);
        w.pod(i.ret); uint8_t h = i.has_ret; w.pod(h); w.pod(i.step);
    }
}
static void dec_imports(Reader& rd, std::vector<ImportRec>& v)
{
    uint32_t n = rd.pod<uint32_t>(); if (!rd.ok || n > 100000) { rd.ok = false; return; }
    v.resize(n);
    for (auto& i : v) {
        i.nid = rd.pod<uint32_t>(); i.name = rd.str(); i.policy = (Policy)rd.pod<uint8_t>();
        for (int k = 0; k < 4; ++k) i.arg[k] = rd.pod<uint64_t>();
        i.ret = rd.pod<uint64_t>(); i.has_ret = rd.pod<uint8_t>(); i.step = rd.pod<uint32_t>();
    }
}
static void enc_diff(Wire& w, const ByteDiff& d)
{
    uint32_t n = (uint32_t)d.size(); w.pod(n);
    for (auto& e : d) { w.pod(e.first); w.pod(e.second); }
}
static bool dec_diff(Reader& rd, ByteDiff& d)
{
    uint32_t n = rd.pod<uint32_t>();
    if (!rd.ok || n > (1u << 22)) { rd.ok = false; return false; }
    d.resize(n);
    for (auto& x : d) { x.first = rd.pod<uint32_t>(); x.second = rd.pod<uint8_t>(); }
    return rd.ok;
}

enum class Side { Hle, Lifted };

/* One side runs the WHOLE sequence, streaming a record per step. */
static void child_run(Side side, const Case& c, const std::vector<Step>& steps, int fd, bool normalise)
{
    std::vector<uint8_t> pristine(mem, mem + ARENA_SIZE);
    std::vector<ImportRec> imports;
    g_lifted_imports = &imports;
    g_hle_imports = side == Side::Hle ? &imports : nullptr;
    g_obs_stack.clear();
    g_top_call = false;
    if (side == Side::Hle) {
        static ps3_hle_observer obs{nullptr, obs_call, obs_return};
        ps3_hle_set_observer(&obs);
    } else {
        ps3_hle_set_observer(nullptr);
    }
    g_mask = &c.mask;
    g_mask_image = &c.mask_image;
    if (normalise) {
        struct sigaction sa{};
        sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER;
        sigemptyset(&sa.sa_mask); sigaction(SIGSEGV, &sa, nullptr); sigaction(SIGBUS, &sa, nullptr);
    }
    std::map<int, uint64_t> slots;
    g_hle_side = side == Side::Hle;
    std::string final_status = "returned", final_blocker, final_detail;
    for (uint32_t si = 0; si < steps.size(); ++si) {
        const Step& st = steps[si];
        g_cur_step = si;
        if (st.pre) st.pre();
        ppu_context ctx{};
        for (int i = 0; i < 12; ++i) {
            const Arg& a = st.args[i];
            const uint64_t v = a.slot >= 0 ? (uint64_t)(slots[a.slot] + (uint64_t)a.add) : a.lit;
            if (i < 8) ctx.gpr[3 + i] = v;
            else wr64(STACK_TOP + 0x70 + 8 * (i - 8), v);   /* ELFv1 parameter save area, r1 + 48 + 8*i */
        }
        ctx.gpr[1] = STACK_TOP; ctx.gpr[13] = TLS_R13; ctx.thread_id = 1;
        /* r14..r31 are the ELFv1 nonvolatile set. Distinct sentinels, not
         * zero, so a lifted callee that clears one is caught too. Both sides
         * get the same values. */
        for (int r = 14; r < 32; ++r) ctx.gpr[r] = ABI_SENTINEL_BASE | (uint64_t)r;
        ctx.lr = SENTINEL;
        g_active_ctx = &ctx;
        StepRes sr;
        sr.status = "returned";
        if (st.caps.size() > kMaxCaptures) {
            sr.status = "blocked"; sr.blocker = "captures-over-cap";
            sr.detail = "the case declares more than " + std::to_string(kMaxCaptures) + " captures";
        } else if (side == Side::Hle) {
            if (!ps3_hle_has(st.nid)) {
                sr.status = "no-hle"; sr.blocker = "no-hle-handler";
                sr.detail = "no HLE handler is registered for this NID";
            } else if (prx_resolve_export(st.nid)) {
                sr.status = "blocked"; sr.blocker = "prx-bound";
                sr.detail = "NID is bound to a loaded PRX; the HLE side would not be HLE";
            } else {
                g_top_call = true;
                try { ps3_hle_call(st.nid, &ctx); }
                catch (const LiftBlock& e) { sr.status = "blocked"; sr.blocker = e.blocker; sr.detail = e.detail; }
            }
        } else {
            const dh_export* ex = nullptr;
            for (uint32_t i = 0; i < g_mod->n_exports; ++i) if (g_mod->exports[i].nid == st.nid) ex = &g_mod->exports[i];
            if (!ex) { sr.status = "blocked"; sr.blocker = "no-export"; sr.detail = "the module does not export this NID"; }
            else {
                const uint32_t code = rd32(ex->opd), toc = rd32(ex->opd + 4);
                auto it = g_registry.find(code);
                if (it == g_registry.end()) { sr.status = "blocked"; sr.blocker = "unlifted-export"; sr.detail = "export OPD points at an unlifted address"; }
                else {
                    ctx.gpr[2] = toc;
                    try {
                        it->second(&ctx);
                        while (g_trampoline_fn) { auto f = g_trampoline_fn; g_trampoline_fn = nullptr; f(&ctx); }
                    } catch (const LiftBlock& e) { sr.status = "blocked"; sr.blocker = e.blocker; sr.detail = e.detail; }
                }
            }
        }
        sr.r3 = ctx.gpr[3]; sr.lr = ctx.lr; sr.r1 = ctx.gpr[1]; sr.r2 = ctx.gpr[2];
        if (side == Side::Lifted && sr.status == "returned")
            for (int r = 14; r < 32; ++r)
                if (ctx.gpr[r] != (ABI_SENTINEL_BASE | (uint64_t)r)) sr.abi_bad |= 1u << (r - 14);
        if (sr.status == "returned" && st.post) st.post();
        if (sr.status == "returned")
            for (const Capture& cp : st.caps) {
                uint64_t v = cp.kind == Capture::Ret ? ctx.gpr[3] : cp.kind == Capture::Mem32 ? rd32(cp.ea)
                           : ((uint64_t)rd32(cp.ea) << 32 | rd32(cp.ea + 4));
                slots[cp.slot] = v; sr.caps.push_back({cp.slot, v});
            }
        collect_diff(pristine, sr.diff);
        if (sr.diff.size() > (1u << 22)) {
            sr.diff.clear();
            sr.status = "blocked"; sr.blocker = "step-diff-oversize";
            sr.detail = "more than 4 MiB changed in one step; the record is refused, never truncated";
        }
        Wire w;
        w.str(sr.status); w.str(sr.blocker); w.str(sr.detail);
        w.pod(sr.r3); w.pod(sr.lr); w.pod(sr.r1); w.pod(sr.r2); w.pod(sr.abi_bad);
        uint32_t nc = (uint32_t)sr.caps.size(); w.pod(nc);
        for (auto& k : sr.caps) { w.pod(k.first); w.pod(k.second); }
        enc_diff(w, sr.diff);
        send_rec(fd, 'S', w);
        if (sr.status != "returned") {
            final_status = sr.status; final_blocker = sr.blocker; final_detail = sr.detail;
            break;
        }
    }
    Wire w;
    w.str(final_status); w.str(final_blocker); w.str(final_detail);
    enc_imports(w, imports);
    send_rec(fd, 'F', w);
}

/* ---- result parsing ----------------------------------------------------- */
/* Only the harness's own marker lines are machine-readable. A forwarded HLE
 * can print guest-controlled text (sys_prx_get_module_id_by_name prints the
 * module name), so a free search for "kind=" over the combined output could
 * pair a fake line with a real fault. The marker is written directly to fd 2
 * and the process exits immediately afterwards, so the LAST line with the
 * exact prefix is the harness's. */
static std::string last_line_starting(const std::string& s, const char* prefix)
{
    const size_t plen = strlen(prefix);
    std::string found;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t nl = s.find('\n', pos);
        const size_t end = nl == std::string::npos ? s.size() : nl;
        if (end - pos >= plen && s.compare(pos, plen, prefix) == 0)
            found = s.substr(pos, end - pos);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return found;
}

static std::vector<std::string> stop_tokens(const std::string& line);

static std::string kv(const std::string& line, const char* key)
{
    for (const std::string& t : stop_tokens(line)) {
        const size_t eq = t.find('=');
        if (eq != std::string::npos && t.compare(0, eq, key) == 0) return t.substr(eq + 1);
    }
    return "";
}

/* Tokenise a PS3STOP machine line, honouring the quoting include/ps3emu/stop.h defines. */
static std::vector<std::string> stop_tokens(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    bool quote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];
        if (quote) {
            if (ch == '\\' && i + 1 < line.size()) {
                char n = line[++i];
                if (n == 'n') cur += '\n'; else if (n == 'r') cur += '\r'; else if (n == 't') cur += '\t';
                else if (n == 'x' && i + 2 < line.size()) {
                    auto hv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
                    cur += (char)((hv(line[i + 1]) << 4) | hv(line[i + 2]));
                    i += 2;
                } else cur += n;
            } else if (ch == '"') quote = false;
            else cur += ch;
        } else if (ch == '"') quote = true;
        else if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool parse_stop(const std::string& errs, SideState& s)
{
    const std::string line = last_line_starting(errs, "PS3STOP v=1 ");
    if (line.empty()) return false;
    const auto tok = stop_tokens(line);
    static const std::set<std::string> volatile_keys = {"pc", "lr", "tid", "last_hle_nid", "last_hle"};
    for (const std::string& t : tok) {
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = t.substr(0, eq), v = t.substr(eq + 1);
        if (k == "kind") { s.stop_kind = v; continue; }
        if (k == "last_hle_nid") { s.last_hle_nid = v; continue; }
        if (!volatile_keys.count(k)) s.stop_fields[k] = v;
    }
    s.has_span = s.stop_kind == "hle-guest-range";
    if (s.has_span) {
        auto ea = s.stop_fields.find("ea"), sz = s.stop_fields.find("size");
        if (ea != s.stop_fields.end()) s.span_ea = strtoull(ea->second.c_str(), nullptr, 16);
        if (sz != s.stop_fields.end()) s.span_size = strtoull(sz->second.c_str(), nullptr, 16);
        /* A zero-length range covers nothing; overlap() refuses to pair one. */
    }
    return true;
}

/* A fork/pipe/waitpid failure is a harness error, never a case outcome. */
static std::string g_harness_error;

/* Fork, run one side's whole sequence, collect the stream and stderr. Death is a result. */
static SideResult run_side(Side side, const Case& c, const std::vector<Step>& steps, bool normalise)
{
    SideResult r;
    int rp[2], ep[2];
    if (pipe(rp) || pipe(ep)) { g_harness_error = std::string("pipe: ") + strerror(errno); return r; }
    fflush(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        g_harness_error = std::string("fork: ") + strerror(errno);
        close(rp[0]); close(rp[1]); close(ep[0]); close(ep[1]);
        return r;
    }
    if (pid == 0) {
        close(rp[0]); close(ep[0]);
        dup2(ep[1], STDERR_FILENO); dup2(ep[1], STDOUT_FILENO); close(ep[1]);
        alarm((unsigned)c.timeout_s);
        child_run(side, c, steps, rp[1], normalise);
        _exit(0);
    }
    close(rp[1]); close(ep[1]);
    std::vector<uint8_t> res, err;
    bool res_truncated = false, err_truncated = false;
    struct pollfd fds[2] = {{rp[0], POLLIN, 0}, {ep[0], POLLIN, 0}};
    int open_fds = 2;
    static uint8_t buf[65536];
    while (open_fds) {
        if (poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(fds[i].fd, buf, sizeof buf);
            if (n > 0) {
                auto& v = i == 0 ? res : err;
                const size_t cap = i == 0 ? (64u << 20) : (4u << 20);
                if (v.size() + (size_t)n > cap) { (i == 0 ? res_truncated : err_truncated) = true; }
                else v.insert(v.end(), buf, buf + n);
            } else { close(fds[i].fd); fds[i].fd = -1; --open_fds; }
        }
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) { g_harness_error = std::string("waitpid: ") + strerror(errno); return r; }
    const std::string errs(err.begin(), err.end());
    bool have_final = false, parse_ok = true;
    std::string final_status, final_blocker, final_detail;
    const uint8_t* p = res.data(); const uint8_t* e = p + res.size();
    while (e - p >= 5) {
        char tag = (char)*p; uint32_t n; memcpy(&n, p + 1, 4);
        if ((size_t)(e - p) < 5 + (size_t)n) { parse_ok = false; break; }
        Reader rd{p + 5, p + 5 + n};
        if (tag == 'S') {
            StepRes s; s.status = rd.str(); s.blocker = rd.str(); s.detail = rd.str();
            s.r3 = rd.pod<uint64_t>(); s.lr = rd.pod<uint64_t>(); s.r1 = rd.pod<uint64_t>(); s.r2 = rd.pod<uint64_t>();
            s.abi_bad = rd.pod<uint32_t>();
            uint32_t nc = rd.pod<uint32_t>();
            if (!rd.ok || nc > kMaxCaptures) { parse_ok = false; break; }
            for (uint32_t i = 0; i < nc; ++i) { int sl = rd.pod<int>(); uint64_t v = rd.pod<uint64_t>(); s.caps.push_back({sl, v}); }
            if (rd.ok) dec_diff(rd, s.diff);
            if (!rd.ok) { parse_ok = false; break; }
            r.steps.push_back(std::move(s));
        } else if (tag == 'F') {
            final_status = rd.str(); final_blocker = rd.str(); final_detail = rd.str();
            if (rd.ok) dec_imports(rd, r.imports);
            have_final = rd.ok;
            if (!rd.ok) parse_ok = false;
        } else {
            parse_ok = false;
        }
        p += 5 + n;
    }
    if (!r.steps.empty()) {
        const StepRes& l = r.steps.back();
        r.r3 = l.r3; r.lr = l.lr; r.r1 = l.r1; r.r2 = l.r2; r.diff = l.diff;
    }
    r.state = SideState{};

    if (res_truncated || err_truncated) {
        r.state.kind = SideKind::HarnessBlocked;
        r.state.blocker = res_truncated ? "wire-truncated" : "stderr-truncated";
        r.state.detail = "the child produced more output than the harness retains; no verdict can rest on it";
        r.state.term_step = r.steps.empty() ? 0 : (uint32_t)r.steps.size() - 1;
        return r;
    }
    if (WIFSIGNALED(st)) {
        const int sig = WTERMSIG(st);
        r.state.kind = sig == SIGALRM ? SideKind::Timeout : SideKind::HostCrash;
        r.state.signal = sig;
        r.state.term_step = (uint32_t)r.steps.size();
        r.state.detail = std::string("signal ") + std::to_string(sig);
        return r;
    }
    if (!WIFEXITED(st)) {
        g_harness_error = "child ended without an exit status";
        return r;
    }
    const int code = WEXITSTATUS(st);
    if (code == 0 && have_final && parse_ok) {
        r.wire_ok = true;
        r.state.detail = final_detail;
        if (final_status == "returned") {
            r.state.kind = SideKind::Returned;
            r.state.term_step = (uint32_t)r.steps.size();
        } else {
            r.state.kind = SideKind::HarnessBlocked;
            r.state.blocker = final_blocker.empty() ? final_status : final_blocker;
            r.state.term_step = r.steps.empty() ? 0 : (uint32_t)r.steps.size() - 1;
        }
        return r;
    }
    if (code == 75) {
        const std::string line = last_line_starting(errs, "kind=dh-guest-fault ");
        r.state.kind = SideKind::GuestFault;
        r.state.term_step = (uint32_t)r.steps.size();
        r.state.has_span = true;
        r.state.span_ea = line.empty() ? 0 : strtoull(kv(line, "ea").c_str(), nullptr, 16);
        r.state.span_size = 1;
        r.state.detail = "guest-fault at " + hex(r.state.span_ea);
        return r;
    }
    if (code == 70) {
        r.state.term_step = (uint32_t)r.steps.size();
        if (!parse_stop(errs, r.state)) {
            r.state.kind = SideKind::HarnessBlocked;
            r.state.blocker = "unparsed-stop";
            r.state.detail = "a stop exited 70 without a PS3STOP machine line";
            return r;
        }
        r.state.kind = SideKind::NamedStop;
        r.state.detail = "kind=" + r.state.stop_kind;
        return r;
    }
    if (code == 72 || code == 73 || code == 74) {
        const std::string line = last_line_starting(errs, "kind=dh-");
        std::string kind = kv(line, "kind");
        if (kind.rfind("dh-", 0) == 0) kind = kind.substr(3);
        if (kind.empty()) kind = "child-exit-" + std::to_string(code);
        r.state.kind = SideKind::HarnessBlocked;
        r.state.blocker = kind;
        r.state.term_step = (uint32_t)r.steps.size();
        r.state.detail = line;
        return r;
    }
    if (code == 99) {
        r.state.kind = SideKind::HarnessBlocked;
        r.state.blocker = "wire-write-failed";
        r.state.detail = "the child could not write its result record";
        r.state.term_step = (uint32_t)r.steps.size();
        return r;
    }
    r.state.kind = SideKind::HarnessBlocked;
    r.state.blocker = parse_ok ? ("child-exit-" + std::to_string(code)) : "invalid-result-record";
    r.state.detail = parse_ok ? "the child exited without a final record" : "a result record was truncated or malformed";
    r.state.term_step = r.steps.empty() ? 0 : (uint32_t)r.steps.size() - 1;
    return r;
}

/* ---- comparison --------------------------------------------------------- */
struct Cmp {
    size_t missing = 0;      /* the lifted side wrote a byte the HLE side did not (or a different one) */
    size_t extra = 0;        /* the HLE side wrote a byte the lifted side did not (or a different one) */
    size_t common_mismatch = 0;
    size_t bytes_differing = 0;
    bool has_first_missing = false, has_first_extra = false;
    uint32_t first_missing = 0, first_extra = 0;
    std::string ranges;
};

static Cmp compare_diffs(const ByteDiff& a, const ByteDiff& b)
{
    Cmp c;
    size_t i = 0, j = 0, nr = 0;
    uint32_t rs = 0, re = 0;
    auto flush = [&] {
        if (re > rs && nr < 16) {
            char t[40];
            snprintf(t, sizeof t, "%s%X+%X", c.ranges.empty() ? "" : ",", rs, re - rs);
            c.ranges += t;
        }
    };
    auto note = [&](uint32_t ea) {
        ++c.bytes_differing;
        if (re == 0 || ea > re) { flush(); if (re) ++nr; rs = ea; }
        re = ea + 1;
    };
    while (i < a.size() || j < b.size()) {
        if (j == b.size() || (i < a.size() && a[i].first < b[j].first)) {
            ++c.extra;
            if (!c.has_first_extra) { c.has_first_extra = true; c.first_extra = a[i].first; }
            note(a[i].first); ++i;
        } else if (i == a.size() || b[j].first < a[i].first) {
            ++c.missing;
            if (!c.has_first_missing) { c.has_first_missing = true; c.first_missing = b[j].first; }
            note(b[j].first); ++j;
        } else {
            if (a[i].second != b[j].second) {
                ++c.common_mismatch; ++c.missing; ++c.extra;
                if (!c.has_first_extra) { c.has_first_extra = true; c.first_extra = a[i].first; }
                if (!c.has_first_missing) { c.has_first_missing = true; c.first_missing = a[i].first; }
                note(a[i].first);
            }
            ++i; ++j;
        }
    }
    flush();
    if (nr >= 16) c.ranges += ",...";
    return c;
}

static bool is_fault(const SideResult& r)
{
    return r.state.kind == SideKind::GuestFault ||
           (r.state.kind == SideKind::NamedStop && r.state.stop_kind == "hle-guest-range" && r.state.has_span);
}
static bool overlap(const SideState& a, const SideState& b)
{
    /* Half-open spans; a zero-length span covers nothing and can never pair.
     * Subtraction-based: ea + size can wrap for a kernel-supplied uint64. */
    if (!a.span_size || !b.span_size) return false;
    return a.span_ea - b.span_ea < b.span_size || b.span_ea - a.span_ea < a.span_size;
}

static std::set<uint32_t> module_import_nids(const dh_module& m)
{
    std::set<uint32_t> s;
    for (uint32_t i = 0; i < m.n_imports; ++i) s.insert(m.imports[i].nid);
    return s;
}
static std::vector<ImportRec> filtered_imports(const std::vector<ImportRec>& v, const std::set<uint32_t>& nids)
{
    std::vector<ImportRec> out;
    for (auto& i : v) if (nids.count(i.nid)) out.push_back(i);
    return out;
}
static bool imports_equal(const std::vector<ImportRec>& a, const std::vector<ImportRec>& b,
                          const std::set<uint32_t>& nids, std::string& cause)
{
    const auto fa = filtered_imports(a, nids), fb = filtered_imports(b, nids);
    if (fa.empty()) return true;   /* the HLE implementation does not re-enter the dispatcher */
    if (fa.size() != fb.size()) {
        cause = "import call count " + std::to_string(fa.size()) + " (hle) vs " + std::to_string(fb.size()) + " (lifted)";
        return false;
    }
    for (size_t i = 0; i < fa.size(); ++i) {
        const ImportRec &x = fa[i], &y = fb[i];
        bool same = x.nid == y.nid && x.step == y.step &&
                    x.arg[0] == y.arg[0] && x.arg[1] == y.arg[1] && x.arg[2] == y.arg[2] && x.arg[3] == y.arg[3] &&
                    x.has_ret == y.has_ret && (!x.has_ret || x.ret == y.ret);
        if (!same) {
            cause = "import call #" + std::to_string(i) + " at step " + std::to_string(x.step) + " (" +
                    import_name(x.nid) + ") differs: hle(" + hex(x.arg[0]) + "," + hex(x.arg[1]) + ") lifted(" +
                    hex(y.arg[0]) + "," + hex(y.arg[1]) + ")";
            return false;
        }
    }
    return true;
}

struct CaseOut {
    const Case* c = nullptr;
    std::vector<Step> steps;
    SideResult a, b;
    std::string verdict, cause, cause_kind, expected;
    int first_bad_step = -1;
    bool unexpected = false, common_mode = false, abi_ok = true, compared = false;
    std::string abi_note;
    std::vector<uint32_t> forwarded;
    std::vector<char> step_equal;
    Cmp mc;
    /* observed witness, compared against Case::expect_{step,cause,range,stop_kind} */
    int witness_step = -1;
    bool witness_has_ea = false;
    uint32_t witness_ea = 0;
    std::string witness_stop;
};

static const char* side_state_name(const SideState& s);

static std::string expect_name(Expect e)
{
    switch (e) {
    case Expect::Match: return "match";
    case Expect::Diverge: return "diverge";
    case Expect::NoHle: return "no-hle";
    case Expect::Inconclusive: return "inconclusive";
    case Expect::Blocked: return "lifted-blocked";
    }
    return "?";
}

static CaseOut evaluate(const Case& c, const std::vector<Step>& steps,
                        const SideResult& a, const SideResult& b, uint32_t dims)
{
    CaseOut o; o.c = &c; o.steps = steps; o.a = a; o.b = b;
    o.expected = expect_name(c.expect);
    const auto& mod = *g_mod;

    const size_t common_steps = std::min(a.steps.size(), b.steps.size());
    o.step_equal.assign(steps.size(), 0);
    for (size_t i = 0; i < common_steps; ++i) {
        if (a.steps[i].status != "returned" || b.steps[i].status != "returned") break;
        bool eq = a.steps[i].status == b.steps[i].status && a.steps[i].caps == b.steps[i].caps;
        if (dims & DIM_RETURN)
            eq = eq && (steps[i].ret64 ? a.steps[i].r3 == b.steps[i].r3 : (uint32_t)a.steps[i].r3 == (uint32_t)b.steps[i].r3);
        if ((dims & (DIM_WRITES | DIM_EXTRA)) == (DIM_WRITES | DIM_EXTRA))
            eq = eq && a.steps[i].diff == b.steps[i].diff;
        o.step_equal[i] = eq ? 1 : 0;
        if (!eq && o.first_bad_step < 0) o.first_bad_step = (int)i;
    }

    /* The final-surface comparison is reported for every verdict, including a
     * divergence already located at an intermediate step. */
    o.mc = compare_diffs(a.diff, b.diff);

    const bool a_nohle = a.state.kind == SideKind::HarnessBlocked && a.state.blocker == "no-hle-handler";
    const bool b_blocked = b.state.kind == SideKind::HarnessBlocked;

    if (a_nohle) {
        o.verdict = "no-hle"; o.cause_kind = "no-hle"; o.cause = "HLE has no handler";
        o.witness_step = (int)a.state.term_step;
    } else if (b_blocked) {
        o.verdict = "lifted-blocked"; o.cause_kind = "blocked";
        o.cause = "lifted side blocked at step " + std::to_string(b.state.term_step) + ": " +
                  b.state.blocker + (b.state.detail.empty() ? "" : " (" + b.state.detail + ")");
        o.witness_step = (int)b.state.term_step;
        o.witness_stop = b.state.blocker;
    } else if (a.state.kind == SideKind::HarnessBlocked) {
        o.verdict = "inconclusive"; o.cause_kind = "hle-blocked";
        o.cause = "HLE side could not run: " + a.state.blocker + " (" + a.state.detail + ")";
        o.witness_step = (int)a.state.term_step;
    } else if (o.first_bad_step >= 0) {
        const size_t i = (size_t)o.first_bad_step;
        const StepRes &x = a.steps[i], &y = b.steps[i];
        o.verdict = "diverge";
        o.witness_step = (int)i;
        if ((dims & DIM_RETURN) && (steps[i].ret64 ? x.r3 != y.r3 : (uint32_t)x.r3 != (uint32_t)y.r3)) {
            o.cause_kind = "return";
            o.cause = "step " + std::to_string(i) + " (" + steps[i].func + ") return " +
                      hex(steps[i].ret64 ? x.r3 : (uint64_t)(uint32_t)x.r3) + " vs " +
                      hex(steps[i].ret64 ? y.r3 : (uint64_t)(uint32_t)y.r3);
        } else if (x.diff != y.diff) {
            o.cause_kind = "memory";
            Cmp sc = compare_diffs(x.diff, y.diff);
            o.cause = "step " + std::to_string(i) + " (" + steps[i].func + ") memory differs at " +
                      hex(sc.has_first_missing ? sc.first_missing : sc.first_extra);
            o.witness_has_ea = true;
            o.witness_ea = sc.has_first_missing ? sc.first_missing : sc.first_extra;
        } else {
            o.cause_kind = "captures";
            o.cause = "step " + std::to_string(i) + " (" + steps[i].func + ") captured values differ";
        }
    } else if (a.state.kind == SideKind::Returned && b.state.kind == SideKind::Returned) {
        bool ret_eq = true;
        if (dims & DIM_RETURN)
            ret_eq = steps.back().ret64 ? a.r3 == b.r3 : (uint32_t)a.r3 == (uint32_t)b.r3;
        const bool mem_ok = (!(dims & DIM_WRITES) || o.mc.missing == 0) &&
                            (!(dims & DIM_EXTRA) || o.mc.extra == 0);
        std::string icause;
        bool imp_ok = true;
        if (dims & DIM_IMPORTS) imp_ok = imports_equal(a.imports, b.imports, module_import_nids(mod), icause);
        o.witness_step = (int)steps.size() - 1;
        if (ret_eq && mem_ok && imp_ok) {
            o.verdict = "match";
            o.cause_kind = "return";
            o.cause = "return and memory agree";
        } else {
            o.verdict = "diverge";
            if (!ret_eq) {
                o.cause_kind = "return";
                o.cause = "return value " + hex(steps.back().ret64 ? a.r3 : (uint64_t)(uint32_t)a.r3) +
                          " vs " + hex(steps.back().ret64 ? b.r3 : (uint64_t)(uint32_t)b.r3);
            } else if (!mem_ok) {
                o.cause_kind = "memory";
                o.cause = "memory differs at " + hex(o.mc.has_first_missing ? o.mc.first_missing : o.mc.first_extra) +
                          " (missing " + std::to_string(o.mc.missing) + ", extra " + std::to_string(o.mc.extra) + ")";
                o.witness_has_ea = true;
                o.witness_ea = o.mc.has_first_missing ? o.mc.first_missing : o.mc.first_extra;
            } else {
                o.cause_kind = "imports"; o.cause = icause;
            }
        }
    } else {
        /* At least one side ended abnormally. */
        const bool same_abnormal = a.state.kind == b.state.kind &&
                                   (a.state.kind == SideKind::Timeout || a.state.kind == SideKind::HostCrash);
        o.witness_step = (int)(a.state.kind != SideKind::Returned ? a.state.term_step : b.state.term_step);
        if (a.state.kind == SideKind::NamedStop) o.witness_stop = a.state.stop_kind;
        else if (b.state.kind == SideKind::NamedStop) o.witness_stop = b.state.stop_kind;
        if (same_abnormal) {
            o.cause_kind = "hang";
            if (dims & DIM_HANG) {
                o.verdict = "inconclusive";
                o.cause = "both sides " + std::string(a.state.kind == SideKind::Timeout ? "timed out" : "crashed") +
                          " at step " + std::to_string(a.state.term_step);
            } else {
                /* Test-only: the pre-V1 rule that accepted two equal abnormal
                 * outcomes as a match. The synthetic fixture proves this is
                 * what DIM_HANG exists to prevent. */
                o.verdict = "match";
                o.cause = "both sides " + std::string(a.state.kind == SideKind::Timeout ? "timed out" : "crashed") +
                          " at step " + std::to_string(a.state.term_step) + " (hang dimension disabled)";
            }
        } else if ((dims & DIM_FAULTS) && is_fault(a) && is_fault(b)) {
            if (a.state.term_step != b.state.term_step) {
                o.verdict = "diverge"; o.cause_kind = "status";
                o.cause = "fault at step " + std::to_string(a.state.term_step) + " (hle) vs " +
                          std::to_string(b.state.term_step) + " (lifted)";
            } else if (!(dims & DIM_FAULT_SPAN) || overlap(a.state, b.state)) {
                o.verdict = "match"; o.cause_kind = "fault";
                o.cause = "both guest-fault at step " + std::to_string(a.state.term_step) + ": hle " +
                          hex(a.state.span_ea) + "+" + hex(a.state.span_size) + ", lifted " +
                          hex(b.state.span_ea) + "+" + hex(b.state.span_size);
            } else {
                o.verdict = "inconclusive"; o.cause_kind = "fault-span";
                o.cause = "guest faults that cannot be paired: hle " + hex(a.state.span_ea) + "+" +
                          hex(a.state.span_size) + ", lifted " + hex(b.state.span_ea) + "+" + hex(b.state.span_size);
            }
        } else if (a.state.kind == SideKind::NamedStop && b.state.kind == SideKind::NamedStop &&
                   a.state.term_step == b.state.term_step && a.state.stop_kind == b.state.stop_kind) {
            if (a.state.stop_fields == b.state.stop_fields) {
                o.verdict = "match"; o.cause_kind = "stop";
                o.cause = "both stopped with " + a.state.stop_kind + " at step " + std::to_string(a.state.term_step);
            } else {
                o.verdict = "inconclusive"; o.cause_kind = "stop-fields";
                o.cause = "both stopped with " + a.state.stop_kind + " at step " + std::to_string(a.state.term_step) +
                          ", but their machine-line fields differ";
            }
        } else {
            o.verdict = "diverge"; o.cause_kind = "status";
            o.cause = std::string("status: hle=") + side_state_name(a.state) + " (" + a.state.detail + ") at step " +
                      std::to_string(a.state.term_step) + ", lifted=" + side_state_name(b.state) + " (" +
                      b.state.detail + ") at step " + std::to_string(b.state.term_step);
        }
    }

    /* The lifted result is only usable if it handed r1/lr back. */
    o.abi_ok = true;
    for (size_t i = 0; i < b.steps.size(); ++i) {
        if (b.steps[i].status != "returned") continue;
        if (b.steps[i].abi_bad) {
            o.abi_ok = false;
            o.witness_step = (int)i;
            o.abi_note += " step " + std::to_string(i) + " (" + steps[i].func + "): nonvolatile register(s) clobbered (mask " +
                          hex(b.steps[i].abi_bad) + ")";
            break;
        }
        if (!(b.steps[i].lr == SENTINEL && b.steps[i].r1 == STACK_TOP)) {
            o.abi_ok = false;
            o.witness_step = (int)i;
            o.abi_note += " step " + std::to_string(i) + " (" + steps[i].func + "): lr=" + hex(b.steps[i].lr) +
                          " r1=" + hex(b.steps[i].r1);
            break;
        }
    }
    if (!o.abi_ok && (o.verdict == "match" || o.verdict == "diverge") && !b_blocked) {
        o.verdict = "inconclusive"; o.cause_kind = "abi";
        o.cause = "lifted code returned with the guest LR/SP not restored (" + o.abi_note +
                  " ); its result cannot be trusted";
    }

    /* Common mode: a match (or an equal named stop) reached through an import
     * the lifted side forwards into the same HLE. */
    if (o.verdict == "match") {
        const auto nids = module_import_nids(mod);
        std::set<uint32_t> fwd;
        for (auto& im : b.imports)
            if (im.policy == Policy::Forward && nids.count(im.nid)) fwd.insert(im.nid);
        for (const SideState* s : {&a.state, &b.state}) {
            if (s->kind != SideKind::NamedStop || s->last_hle_nid.empty()) continue;
            const uint32_t nid = (uint32_t)strtoul(s->last_hle_nid.c_str(), nullptr, 16);
            if (nids.count(nid) && g_pol->of(nid) == Policy::Forward) fwd.insert(nid);
        }
        o.forwarded.assign(fwd.begin(), fwd.end());
        o.common_mode = !fwd.empty();
        if (o.common_mode) o.cause += " (common mode through " + std::to_string(fwd.size()) + " forwarded import(s))";
    }

    o.compared = (o.verdict == "match" || o.verdict == "diverge" || o.verdict == "inconclusive") &&
                 a.state.kind != SideKind::HarnessBlocked && b.state.kind != SideKind::HarnessBlocked;

    o.unexpected = o.verdict != o.expected;
    if (!o.unexpected && !c.expect_cause.empty() && o.cause_kind != c.expect_cause) {
        o.unexpected = true;
        o.cause += " [declared cause " + c.expect_cause + ", observed " + o.cause_kind + "]";
    }
    if (!o.unexpected && c.expect_step >= 0 && o.witness_step != c.expect_step) {
        o.unexpected = true;
        o.cause += " [declared step " + std::to_string(c.expect_step) +
                   ", observed " + std::to_string(o.witness_step) + "]";
    }
    if (!o.unexpected && c.expect_range_hi > c.expect_range_lo) {
        if (!o.witness_has_ea || o.witness_ea < c.expect_range_lo || o.witness_ea >= c.expect_range_hi) {
            o.unexpected = true;
            o.cause += " [declared memory range " + hex(c.expect_range_lo) + "+" +
                       hex(c.expect_range_hi - c.expect_range_lo) + ", observed " +
                       (o.witness_has_ea ? hex(o.witness_ea) : std::string("none")) + "]";
        }
    }
    if (!o.unexpected && !c.expect_stop_kind.empty() && o.witness_stop != c.expect_stop_kind) {
        o.unexpected = true;
        o.cause += " [declared stop " + c.expect_stop_kind + ", observed " +
                   (o.witness_stop.empty() ? std::string("none") : o.witness_stop) + "]";
    }
    if (!o.unexpected && c.expect == Expect::Blocked && !c.blocker.empty()) {
        const bool reason_ok = b.state.blocker == c.blocker;
        const bool step_ok = c.blocker_step < 0 || (int)b.state.term_step == c.blocker_step;
        if (!reason_ok || !step_ok) {
            o.unexpected = true;
            o.cause += " [declared blocker " + c.blocker + "@" + std::to_string(c.blocker_step) +
                       ", observed " + b.state.blocker + "@" + std::to_string(b.state.term_step) + "]";
        }
    }
    return o;
}

static const char* side_state_name(const SideState& s)
{
    switch (s.kind) {
    case SideKind::Returned: return "returned";
    case SideKind::GuestFault: return "guest-fault";
    case SideKind::NamedStop: return "named-stop";
    case SideKind::HostCrash: return "host-crash";
    case SideKind::Timeout: return "timeout";
    case SideKind::HarnessBlocked: return "harness-blocked";
    }
    return "?";
}

/* ---- report ------------------------------------------------------------- */
static void write_side(JsonWriter& w, const SideResult& r, int timeout_s, bool ret64)
{
    w.begin_object();
    w.field("state", side_state_name(r.state));
    switch (r.state.kind) {
    case SideKind::Returned:
        w.field("r3", hex(ret64 ? r.r3 : (uint64_t)(uint32_t)r.r3));
        w.field("lr", hex(r.lr));
        w.field("r1", hex(r.r1));
        break;
    case SideKind::GuestFault:
        w.field("ea", hex(r.state.span_ea));
        w.field("size", hex(r.state.span_size));
        w.field("step", (int)r.state.term_step);
        w.field("detail", r.state.detail);
        break;
    case SideKind::NamedStop: {
        w.field("kind", r.state.stop_kind);
        w.field("step", (int)r.state.term_step);
        if (r.state.has_span) { w.field("ea", hex(r.state.span_ea)); w.field("size", hex(r.state.span_size)); }
        w.field("detail", r.state.detail);
        w.key("fields");
        w.begin_object();
        for (auto& kv : r.state.stop_fields) w.field(kv.first, kv.second);
        w.end_object();
        if (!r.state.last_hle_nid.empty()) w.field("last_hle_nid", r.state.last_hle_nid);
        break;
    }
    case SideKind::HostCrash:
        w.field("signal", r.state.signal);
        w.field("step", (int)r.state.term_step);
        w.field("detail", r.state.detail);
        break;
    case SideKind::Timeout:
        w.field("timeout_s", timeout_s);
        w.field("step", (int)r.state.term_step);
        break;
    case SideKind::HarnessBlocked:
        w.field("reason", r.state.blocker);
        w.field("step", (int)r.state.term_step);
        w.field("detail", r.state.detail);
        break;
    }
    w.end_object();
}

static void write_case(JsonWriter& w, const CaseOut& o, int timeout_s)
{
    const Case& c = *o.c;
    w.begin_object();
    w.field("id", c.id);
    w.field("func", o.steps.back().func);
    w.field("nid", hex(o.steps.back().nid));
    w.field("row", c.row);
    w.key("args");
    w.begin_array();
    for (int i = 0; i < 6; ++i) {
        const Arg& a = o.steps.back().args[i];
        w.value(a.slot >= 0 ? ("slot" + std::to_string(a.slot)) : hex(a.lit));
    }
    w.end_array();
    w.field("expect", o.expected);
    w.field("verdict", o.verdict);
    w.field("unexpected", o.unexpected);
    w.field("cause_kind", o.cause_kind);
    w.field("cause", o.cause);
    if (!c.note.empty()) w.field("note", c.note);
    w.field("common_mode", o.common_mode);
    w.key("forwarded_nids");
    w.begin_array();
    for (uint32_t nid : o.forwarded) w.value(hex(nid));
    w.end_array();
    if (!c.mask.empty() || !c.mask_image.empty()) {
        w.key("masked");
        w.begin_array();
        for (auto& m : c.mask) w.value(hex(m.first) + "+" + hex(m.second));
        for (auto& m : c.mask_image) w.value("image+" + hex(m.first) + "+" + hex(m.second));
        w.end_array();
    }
    w.field("compared", o.compared);
    w.field("ret64", o.steps.back().ret64);
    w.key("sides");
    w.begin_object();
    w.key("hle"); write_side(w, o.a, timeout_s, o.steps.back().ret64);
    w.key("lifted"); write_side(w, o.b, timeout_s, o.steps.back().ret64);
    w.end_object();
    if (o.steps.size() > 1) {
        w.key("first_diff_step");
        w.value(o.first_bad_step);
        w.key("steps");
        w.begin_array();
        for (size_t i = 0; i < o.steps.size(); ++i) {
            w.begin_object();
            w.field("func", o.steps[i].func);
            w.field("a", i < o.a.steps.size() ? hex(o.steps[i].ret64 ? o.a.steps[i].r3
                                                                      : (uint64_t)(uint32_t)o.a.steps[i].r3) : std::string("-"));
            w.field("b", i < o.b.steps.size() ? hex(o.steps[i].ret64 ? o.b.steps[i].r3
                                                                      : (uint64_t)(uint32_t)o.b.steps[i].r3) : std::string("-"));
            w.field("a_status", i < o.a.steps.size() ? o.a.steps[i].status
                       : (i == o.a.state.term_step ? side_state_name(o.a.state) : "not-run"));
            w.field("b_status", i < o.b.steps.size() ? o.b.steps[i].status
                       : (i == o.b.state.term_step ? side_state_name(o.b.state) : "not-run"));
            w.field("equal", i < o.step_equal.size() && o.step_equal[i]);
            w.end_object();
        }
        w.end_array();
    }
    w.field("abi_ok", o.abi_ok);
    if (!o.abi_ok) w.field("abi_note", o.abi_note);
    w.field("missing_bytes", (uint64_t)o.mc.missing);
    w.field("extra_bytes", (uint64_t)o.mc.extra);
    w.field("common_mismatch", (uint64_t)o.mc.common_mismatch);
    w.field("bytes_differing", (uint64_t)o.mc.bytes_differing);
    if (o.mc.has_first_missing) w.field("first_missing", hex(o.mc.first_missing));
    if (o.mc.has_first_extra) w.field("first_extra", hex(o.mc.first_extra));
    w.field("diff_ranges", o.mc.ranges);
    w.field("hle_bytes_changed", (uint64_t)o.a.diff.size());
    w.field("lifted_bytes_changed", (uint64_t)o.b.diff.size());
    w.field("hle_image_writes", (uint64_t)o.a.img_diff.size());
    w.field("image_writes", (uint64_t)o.b.img_diff.size());
    w.key("imports");
    w.begin_array();
    for (auto& im : o.b.imports) {
        w.begin_object();
        w.field("step", (int)im.step);
        w.field("nid", hex(im.nid));
        w.field("name", im.name);
        w.field("policy", im.policy == Policy::Forward ? "forward" : im.policy == Policy::Model ? "model" : "stop");
        w.key("args");
        w.begin_array();
        for (int k = 0; k < 4; ++k) w.value(hex(im.arg[k]));
        w.end_array();
        if (im.has_ret) w.field("ret", hex(im.ret));
        w.end_object();
    }
    w.end_array();
    w.end_object();
}

/* ---- paths / report file ------------------------------------------------ */
static std::string realish(const std::string& p)
{
    std::error_code ec;
    auto abs = std::filesystem::absolute(p, ec);
    if (ec) return p;
    auto parent = std::filesystem::weakly_canonical(abs.parent_path(), ec);
    if (ec) return abs.string();
    return (parent / abs.filename()).string();
}
static bool inside(const std::string& path, const std::string& root)
{
    if (root.empty()) return false;
    const std::string r = realish(root), p = realish(path);
    return p == r || (p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '/');
}

static bool write_atomic(const std::string& path, const std::string& text)
{
    const std::string part = path + ".partial";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = (fflush(f) == 0) && ok;
    ok = (fsync(fileno(f)) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(part.c_str()); return false; }
    if (rename(part.c_str(), path.c_str()) != 0) { remove(part.c_str()); return false; }
    return true;
}

/* ---- runner ------------------------------------------------------------- */
int run(const dh_module& mod, std::vector<Case>& cases, const ImportPolicy& pol, const Options& opt)
{
    g_mod = &mod; g_pol = &pol;
    const uint32_t dims = opt.disable_dimensions ? (DIM_ALL & ~opt.disable_dimensions) : DIM_ALL;
    const bool normalise = (dims & DIM_FAULTS) != 0;

    if (opt.out.empty()) {
        fprintf(stderr, "[dh] --out is required: a run writes one JSON report (schema ps3recomp.diffharness/1)\n");
        return 2;
    }
    if (!PS3RECOMP_DIFF_SYNTHETIC && inside(opt.out, PS3RECOMP_REPO_ROOT)) {
        fprintf(stderr, "[dh] refusing a report path inside the repository: %s\n", realish(opt.out).c_str());
        return 2;
    }
    const std::string path = !opt.image.empty() ? opt.image : mod.default_image;
    if (!PS3RECOMP_DIFF_SYNTHETIC && inside(path, PS3RECOMP_REPO_ROOT)) {
        fprintf(stderr, "[dh] refusing an image inside the repository: %s\n", realish(path).c_str());
        return 2;
    }
    if (access(path.c_str(), R_OK) != 0) {
        printf("SKIP %s: image %s is missing or unreadable\n", mod.name, path.c_str());
        return 77;
    }

    /* The lifted code was compiled against its own copy of ppu_context. */
    {
        const uint32_t* L = mod.ctx_layout;
        const uint32_t mine[8] = {(uint32_t)sizeof(ppu_context), (uint32_t)offsetof(ppu_context, gpr), (uint32_t)offsetof(ppu_context, fpr),
                                  (uint32_t)offsetof(ppu_context, cr), (uint32_t)offsetof(ppu_context, lr), (uint32_t)offsetof(ppu_context, ctr),
                                  (uint32_t)offsetof(ppu_context, xer), (uint32_t)offsetof(ppu_context, thread_id)};
        for (int i = 0; i < 8; ++i) if (L[i] != mine[i]) {
            fprintf(stderr, "[dh] ppu_context layout mismatch at field %d: lifted %u, runtime %u\n", i, L[i], mine[i]);
            return 2;
        }
    }

    /* Reserve the whole guest space PROT_NONE, then put the arena inside it:
     * an access outside the arena is a deterministic guest-fault, not whatever
     * host mapping happens to follow vm_base. */
    void* guard = mmap(nullptr, GUEST_SPACE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (guard == MAP_FAILED) { perror("[dh] mmap guest reservation"); return 2; }
    void* arena = mmap(guard, ARENA_SIZE, PROT_READ | PROT_WRITE,
                       MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (arena == MAP_FAILED) { perror("[dh] mmap arena"); return 2; }
    vm_base = mem = (uint8_t*)arena;
    ppu_vm_size = ARENA_SIZE;
    ppu_hle_init();
    g_ppu_thread_entry_trampoline = dh_entry_trampoline;

    /* Load the relocated image at its base and register the lifted functions.
     * Exports are deliberately NOT handed to prx_load_module: its registry makes
     * ps3_hle_call() divert every bound NID into the lifted code, which would turn
     * the HLE side into the lifted side. The harness binds exports itself. */
    uint32_t image_size = 0;
    uint8_t* img = prx_image_load_file(path.c_str(), &image_size);
    if (!img) { fprintf(stderr, "[dh] cannot read image %s\n", path.c_str()); return 2; }
    char image_sha[PS3_SHA256_HEX_SIZE];
    ps3_sha256_hex(img, image_size, image_sha);
    prx_module pm{};
    pm.name = mod.name; pm.base = mod.base; pm.image = img; pm.image_size = image_size;
    pm.funcs = mod.funcs; pm.func_count = mod.func_count; pm.exports = nullptr; pm.export_count = 0;
    prx_load_result lr = prx_load_module(&pm, register_fn);
    free(img);
    if (!lr.ok) return 2;

    /* Patch every import slot: slot -> descriptor {TRAMP_BASE+8i, module TOC}. */
    uint32_t patched = 0;
    for (uint32_t i = 0; i < mod.n_imports; ++i) {
        const dh_import& im = mod.imports[i];
        if (!im.slot_ea) continue;
        wr32(OPD_BASE + 8 * i, TRAMP_BASE + 8 * i);
        wr32(OPD_BASE + 8 * i + 4, mod.toc);
        wr32(im.slot_ea, OPD_BASE + 8 * i);
        ++patched;
    }
    fprintf(stderr, "[dh] %s: image 0x%X bytes @0x%08X, %llu lifted functions, %u/%u import slots patched\n",
            mod.name, image_size, mod.base, (unsigned long long)lr.funcs_registered, patched, mod.n_imports);

    JsonWriter w;
    w.begin_object();
    w.field("schema", "ps3recomp.diffharness/1");
    w.key("environment");
    w.begin_object();
    w.field("toolkit_sha", PS3RECOMP_TOOLKIT_SHA);
    w.field("module", mod.name);
    w.field("image", realish(path));
    w.field("image_sha256", image_sha);
    w.field("image_size", (uint64_t)image_size);
    w.field("base", hex(mod.base));
    w.field("toc", hex(mod.toc));
    w.field("harness", "dh_diff");
    w.end_object();
    w.key("options");
    w.begin_object();
    w.field("filter", opt.filter);
    w.field("fault_normalisation", normalise);
    w.key("disabled_dimensions");
    w.begin_array();
    static const std::pair<uint32_t, const char*> dim_names[] = {
        {DIM_RETURN, "return"}, {DIM_WRITES, "writes"}, {DIM_EXTRA, "extra"}, {DIM_IMPORTS, "imports"},
        {DIM_FAULTS, "faults"}, {DIM_FAULT_SPAN, "fault-span"}, {DIM_HANG, "hang"}};
    for (auto& d : dim_names) if (opt.disable_dimensions & d.first) w.value(d.second);
    w.end_array();
    w.end_object();

    int n_match = 0, n_div = 0, n_nohle = 0, n_blocked = 0, n_inconc = 0, n_unexpected = 0, n_run = 0, n_compared = 0;
    int n_common = 0;
    std::map<std::string, int> import_tally;
    struct FnStat { int match = 0, diverge = 0, nohle = 0, blocked = 0, inconc = 0, cases = 0, common = 0; };
    std::map<std::pair<std::string, uint32_t>, FnStat> per_fn;

    w.key("cases");
    w.begin_array();
    for (Case& c : cases) {
        if (!opt.filter.empty() && c.id.find(opt.filter) == std::string::npos) continue;
        std::vector<Step> steps = c.steps;
        if (steps.empty()) {
            Step s; s.func = c.func; s.nid = c.nid;
            for (int i = 0; i < 8; ++i) s.args[i] = Arg(c.args[i]);
            steps.push_back(s);
        }
        fill(WIN_LO, WIN_HI - WIN_LO, 0xA5);
        fill(TLS_R13, TLS_LEN, 0);
        if (c.setup) c.setup();
        SideResult a = run_side(Side::Hle, c, steps, normalise);
        if (!g_harness_error.empty()) { fprintf(stderr, "[dh] harness error: %s\n", g_harness_error.c_str()); return 2; }
        SideResult b = run_side(Side::Lifted, c, steps, normalise);
        if (!g_harness_error.empty()) { fprintf(stderr, "[dh] harness error: %s\n", g_harness_error.c_str()); return 2; }
        for (auto& e : a.diff)
            if (e.first >= mod.base && e.first < mod.base + image_size) a.img_diff.push_back(e);
        for (auto& e : b.diff)
            if (e.first >= mod.base && e.first < mod.base + image_size) b.img_diff.push_back(e);

        CaseOut o = evaluate(c, steps, a, b, dims);
        ++n_run;
        if (o.compared) ++n_compared;
        if (o.verdict == "match") ++n_match; else if (o.verdict == "diverge") ++n_div;
        else if (o.verdict == "no-hle") ++n_nohle; else if (o.verdict == "inconclusive") ++n_inconc;
        else ++n_blocked;
        if (o.unexpected) ++n_unexpected;
        if (o.common_mode) ++n_common;
        for (auto& i : b.imports)
            import_tally[i.name + (i.policy == Policy::Forward ? " [forward]" : i.policy == Policy::Model ? " [model]" : " [STOP]")]++;
        FnStat& fs = per_fn[{steps.back().func, steps.back().nid}];
        ++fs.cases;
        if (o.verdict == "match") ++fs.match; else if (o.verdict == "diverge") ++fs.diverge;
        else if (o.verdict == "no-hle") ++fs.nohle; else if (o.verdict == "inconclusive") ++fs.inconc;
        else ++fs.blocked;
        if (o.common_mode) ++fs.common;

        write_case(w, o, c.timeout_s);
        printf("%-34s %-14s %s%s\n", c.id.c_str(), o.verdict.c_str(), o.cause.c_str(),
               o.unexpected ? "   <== UNEXPECTED" : "");
    }
    w.end_array();

    w.key("functions");
    w.begin_object();
    for (auto& kv : per_fn) {
        w.key(kv.first.first);
        w.begin_object();
        w.key(hex(kv.first.second));
        w.begin_object();
        w.field("cases", kv.second.cases);
        w.field("match", kv.second.match);
        w.field("diverge", kv.second.diverge);
        w.field("no-hle", kv.second.nohle);
        w.field("inconclusive", kv.second.inconc);
        w.field("lifted-blocked", kv.second.blocked);
        w.field("common_mode", kv.second.common);
        w.end_object();
        w.end_object();
    }
    w.end_object();

    w.key("summary");
    w.begin_object();
    w.field("cases", n_run);
    w.field("compared", n_compared);
    w.field("match", n_match);
    w.field("diverge", n_div);
    w.field("no-hle", n_nohle);
    w.field("inconclusive", n_inconc);
    w.field("lifted-blocked", n_blocked);
    w.field("unexpected", n_unexpected);
    w.field("common_mode", n_common);
    w.field("independent_match", n_match - n_common);
    w.end_object();
    w.end_object();

    const std::string out = realish(opt.out);
    if (!write_atomic(out, w.str())) {
        fprintf(stderr, "[dh] cannot write report %s: %s\n", out.c_str(), strerror(errno));
        return 2;
    }
    printf("\ncases run %d: match %d, diverge %d, no-hle %d, inconclusive %d, lifted-blocked %d; "
           "compared %d; common-mode %d; unexpected %d\n",
           n_run, n_match, n_div, n_nohle, n_inconc, n_blocked, n_compared, n_common, n_unexpected);
    for (auto& kv : import_tally) printf("  import seen from lifted code: %-36s x%d\n", kv.first.c_str(), kv.second);
    printf("\nper-function coverage (case's final call):\n  %-52s cases match diverge no-hle inconcl blocked common\n", "function");
    for (auto& kv : per_fn)
        printf("  %-52s %5d %5d %7d %6d %7d %7d %6d\n", kv.first.first.c_str(), kv.second.cases, kv.second.match,
               kv.second.diverge, kv.second.nohle, kv.second.inconc, kv.second.blocked, kv.second.common);
    int exported = 0, hit = 0;
    for (uint32_t i = 0; i < mod.n_exports; ++i) {
        const dh_export& e = mod.exports[i];
        if (e.lib && e.lib[0] == '!') continue;     /* module-entry bookkeeping exports */
        ++exported;
        bool in = false;
        for (Case& c : cases) {
            if (!opt.filter.empty() && c.id.find(opt.filter) == std::string::npos) continue;
            if (c.steps.empty() ? c.nid == e.nid : [&] { for (auto& s : c.steps) if (s.nid == e.nid) return true; return false; }()) { in = true; break; }
        }
        if (in) ++hit;
    }
    printf("  exports of %s with at least one case: %d of %d\n", mod.name, hit, exported);

    if (n_run == 0) {
        fprintf(stderr, "[dh] no case matched the filter '%s'; nothing was compared\n", opt.filter.c_str());
        return 2;
    }
    if (n_compared == 0) {
        fprintf(stderr, "[dh] no case compared the HLE against the lifted code; a run without a comparison is not evidence\n");
        return 2;
    }
    if (n_run < opt.min_cases) {
        fprintf(stderr, "[dh] only %d case(s) ran, at least %d required\n", n_run, opt.min_cases);
        return 1;
    }
    if (opt.expect_diverge >= 0 && n_div != opt.expect_diverge) {
        fprintf(stderr, "[dh] %d case(s) diverged, %d declared\n", n_div, opt.expect_diverge);
        return 1;
    }
    return n_unexpected ? 1 : 0;
}

}  // namespace dh
