/*
 * rsx_replay <capture-dir> [--frames a-b] [--out report.json] [--manifest dir] [--accept-unknown-methods]
 *
 * Replays a ps3diff.rsx/1 capture (docs/PS3DIFF.md) through the same FIFO walker, register-file draw engine
 * and headless Metal backend the title ran, without the title. Per recorded step: write the step's blobs to
 * their guest addresses, run the walker over exactly that step (cellGcm_rsx_replay_step), and check what the
 * RSX wrote back (label RELEASE, report, NV0039 destination) against the recording. The replay is itself
 * captured by the same writer, so its manifest is a ps3diff.rsx/1 file `ps3diff rsx-compare` accepts.
 *
 * Determinism: one thread does everything (the title runs the walker, the engine and the backend on one
 * thread too); the host clock reaches the replay nowhere (GET_REPORT timers are the recorded ones, see
 * cellGcmSys.c s_replay); the readback is taken after the command buffer has completed.
 *
 * Exit statuses, named in docs/PS3DIFF.md:
 *   0 every compared frame is equal and valid       1 a compared frame differs or is invalid
 *   2 a named error: the capture cannot be replayed 3 zero frames compared
 *   4 not comparable: the environment records differ
 *   5 diagnostic only (--accept-unknown-methods): equal, but not gate-valid
 *  70 a stop raised by the walker, the engine or the capture writer
 */
#include "cellGcmSys.h"
#include "rsx_capture.h"
#include "rsx_metal_backend.h"
#include "ps3emu/config.h"
#include "ps3emu/sha256.h"

#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
uint8_t* vm_base = nullptr;                 /* the guest address space this host owns */
extern uint32_t ppu_hle_inject_base;        /* defined with the walker; the live value is inferred from its label blobs */
extern uint32_t ppu_vm_size;
void vm_write32(uint64_t addr, uint32_t v);
}

namespace {

constexpr u32 kVmSize = 0xE0000000u;        /* what the title maps: local memory ends at 0xD0000000 */
constexpr u64 kBlobCacheCap = 512ull << 20;

struct Err {
    std::string name, detail;
};
[[noreturn]] void fail(const std::string& name, const std::string& detail) { throw Err{name, detail}; }

// ---- JSON: the subset the capture writer emits (integers, booleans, null, ASCII strings) -------------------

struct J {
    enum T { Null, Bool, Int, Str, Arr, Obj } t = Null;
    bool b = false;
    long long i = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;

    const J* find(const char* k) const
    {
        for (const auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    const J& at(const char* k) const
    {
        const J* p = find(k);
        if (!p) fail("capture-format", std::string("a record lacks the field \"") + k + "\"");
        return *p;
    }
    u32 u(const char* k) const
    {
        const J& v = at(k);
        if (v.t != Int || v.i < 0 || v.i > 0xFFFFFFFFll) fail("capture-format", std::string("field ") + k + " is not a u32");
        return (u32)v.i;
    }
    const std::string& str(const char* k) const
    {
        const J& v = at(k);
        if (v.t != Str) fail("capture-format", std::string("field ") + k + " is not a string");
        return v.s;
    }
    u32 hex(const char* k) const
    {
        const std::string& s = str(k);
        char* end = nullptr;
        const unsigned long long v = strtoull(s.c_str(), &end, 16);
        if (s.compare(0, 2, "0x") != 0 || *end || v > 0xFFFFFFFFull)
            fail("capture-format", std::string("field ") + k + " is not a 0x u32");
        return (u32)v;
    }
};

struct Parser {
    const char* p;
    const char* e;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r')) p++; }
    [[noreturn]] void bad(const char* what) { fail("capture-format", std::string("malformed JSON: ") + what); }
    J value()
    {
        ws();
        if (p >= e) bad("unexpected end");
        J v;
        const char c = *p;
        if (c == '{') {
            p++; v.t = J::Obj; ws();
            if (p < e && *p == '}') { p++; return v; }
            for (;;) {
                ws();
                J k = value();
                if (k.t != J::Str) bad("object key");
                ws();
                if (p >= e || *p++ != ':') bad("missing ':'");
                J val = value();
                v.o.emplace_back(std::move(k.s), std::move(val));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                bad("object end");
            }
        } else if (c == '[') {
            p++; v.t = J::Arr; ws();
            if (p < e && *p == ']') { p++; return v; }
            for (;;) {
                v.a.push_back(value());
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                bad("array end");
            }
        } else if (c == '"') {
            p++; v.t = J::Str;
            while (p < e && *p != '"') {
                if (*p == '\\') {
                    p++;
                    if (p >= e) bad("escape");
                    if (*p == 'u') {
                        if (e - p < 5) bad("\\u");
                        v.s += (char)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16);
                        p += 4;
                    } else v.s += *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
                    p++;
                } else v.s += *p++;
            }
            if (p >= e) bad("unterminated string");
            p++;
        } else if (!strncmp(p, "true", 4)) { p += 4; v.t = J::Bool; v.b = true; }
        else if (!strncmp(p, "false", 5)) { p += 5; v.t = J::Bool; }
        else if (!strncmp(p, "null", 4)) { p += 4; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            // Integers; the writer's one float (the constant "t":0.0) is read and dropped.
            const char* q = p;
            if (*q == '-') q++;
            while (q < e && *q >= '0' && *q <= '9') q++;
            v.t = J::Int;
            v.i = strtoll(std::string(p, q).c_str(), nullptr, 10);
            if (q < e && (*q == '.' || *q == 'e' || *q == 'E'))
                while (q < e && (*q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-' || (*q >= '0' && *q <= '9'))) q++;
            p = q;
        } else bad("unexpected character");
        return v;
    }
};

J parse_line(const std::string& line)
{
    Parser ps{line.data(), line.data() + line.size()};
    J v = ps.value();
    ps.ws();
    if (ps.p != ps.e || v.t != J::Obj) fail("capture-format", "a line is not one JSON object");
    return v;
}

std::string quote(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) o += c;
        else o += '?';
    }
    return o + "\"";
}

std::string dump(const J& v)
{
    switch (v.t) {
    case J::Null: return "null";
    case J::Bool: return v.b ? "true" : "false";
    case J::Int: return std::to_string(v.i);
    case J::Str: return quote(v.s);
    case J::Arr: {
        std::string s = "[";
        for (size_t i = 0; i < v.a.size(); i++) { if (i) s += ','; s += dump(v.a[i]); }
        return s + "]";
    }
    case J::Obj: {
        std::string s = "{";
        for (size_t i = 0; i < v.o.size(); i++) { if (i) s += ','; s += quote(v.o[i].first) + ":" + dump(v.o[i].second); }
        return s + "}";
    }
    }
    return "null";
}

std::string event_of(const std::string& line)
{
    static const char key[] = "\"event\":\"";
    const size_t at = line.find(key);
    if (at == std::string::npos) return "";
    const size_t from = at + sizeof key - 1;
    const size_t to = line.find('"', from);
    return to == std::string::npos ? "" : line.substr(from, to - from);
}

std::string hex8(u32 v)
{
    char b[16];
    snprintf(b, sizeof b, "0x%08x", v);
    return b;
}

std::string digest_hex(ps3_sha256_ctx ctx)
{
    u8 d[32];
    ps3_sha256_final(&ctx, d);
    static const char* x = "0123456789abcdef";
    std::string s(64, '0');
    for (int i = 0; i < 32; i++) { s[2 * i] = x[d[i] >> 4]; s[2 * i + 1] = x[d[i] & 15]; }
    return s;
}

// ---- the capture -------------------------------------------------------------------------------------------

class BlobStore {
public:
    explicit BlobStore(std::string dir) : dir_(std::move(dir)) {}
    // The verified bytes of one blob: present, the declared size, and hashing to its own name.
    const std::vector<u8>& get(const J& ref, u64 seq)
    {
        const std::string& sha = ref.str("sha256");
        const u32 size = ref.u("size");
        auto it = cache_.find(sha);
        if (it == cache_.end()) {
            const std::string path = dir_ + "/blobs/sha256/" + sha.substr(0, 2) + "/" + sha;
            std::ifstream f(path, std::ios::binary);
            if (sha.size() != 64 || !f)
                fail("missing-blob", "event seq " + std::to_string(seq) + " names blob " + sha + " (ea " +
                                         hex8(ref.hex("ea")) + "), which is not in " + dir_ + "/blobs/sha256");
            std::vector<u8> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            char h[PS3_SHA256_HEX_SIZE];
            ps3_sha256_hex(d.data(), d.size(), h);
            if (sha != h)
                fail("altered-blob", "blob " + sha + " (event seq " + std::to_string(seq) + ") hashes to " + h);
            if (bytes_ + d.size() > kBlobCacheCap) { cache_.clear(); bytes_ = 0; }
            bytes_ += d.size();
            it = cache_.emplace(sha, std::move(d)).first;
        }
        if (it->second.size() != size)
            fail("altered-blob", "blob " + sha + " is " + std::to_string(it->second.size()) + " bytes, the event (seq " +
                                     std::to_string(seq) + ") declares " + std::to_string(size));
        return it->second;
    }

private:
    std::string dir_;
    std::unordered_map<std::string, std::vector<u8>> cache_;
    u64 bytes_ = 0;
};

struct Capture {
    std::string dir;
    J machine;                                  /* the environment record */
    bool have_env = false, have_end = false;
    std::vector<J> frames;                      /* every frame-complete record, in order */
    std::string end_hash;
    bool have_label = false;
    u32 hle_base = 0;
};

// Lines of a capture file, one at a time; false at the end.
bool next_line(std::ifstream& f, std::string& line)
{
    if (!std::getline(f, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

const std::string& hash_value(const J& rec, const char* name)
{
    static const std::string none;
    const J& h = rec.at(name);
    if (h.t == J::Null) return none;
    return h.str("value");
}

// The whole file once, parsing only what a replay needs before it starts: the environment, the frame records,
// and the guest address the live process kept its labels at.
Capture scan(const std::string& dir)
{
    Capture c;
    c.dir = dir;
    const std::string path = dir + "/capture.ndjson";
    std::ifstream f(path);
    if (!f) {
        std::ifstream part(path + ".partial");
        if (part) fail("unfinished-capture", dir + " holds capture.ndjson.partial: the process that wrote it did not close it");
        fail("no-capture", "no capture.ndjson in " + dir);
    }
    std::string line;
    bool first = true;
    while (next_line(f, line)) {
        const std::string ev = event_of(line);
        if (first && ev != "capture-start") fail("capture-format", "the first record is not capture-start");
        first = false;
        if (ev == "environment") {
            c.machine = parse_line(line).at("machine");
            c.have_env = true;
        } else if (ev == "frame-complete") {
            c.frames.push_back(parse_line(line));
        } else if (ev == "capture-end") {
            c.have_end = true;
            c.end_hash = hash_value(parse_line(line), "capture_hash");
        } else if (!c.have_label && line.find("\"kind\":\"label\"") != std::string::npos) {
            // A label's guest address is hle_base + its offset, and the offset is in the blob record.
            const J rec = parse_line(line);
            for (const char* list : {"reads", "writes"}) {
                const J* l = rec.find(list);
                if (!l) continue;
                for (const J& r : l->a) {
                    if (r.str("kind") != "label") continue;
                    const u32 base = r.hex("ea") - (r.hex("offset") & 0xFFFFu);
                    if (c.have_label && base != c.hle_base)
                        fail("hle-base", "label blobs disagree on where the label window is (" + hex8(base) + " vs " +
                                             hex8(c.hle_base) + ")");
                    c.have_label = true;
                    c.hle_base = base;
                }
            }
        }
    }
    if (first) fail("capture-format", "an empty capture");
    if (!c.have_end) fail("truncated-capture", "capture.ndjson has no capture-end");
    if (!c.have_env) fail("capture-format", "no environment record: a capture without one cannot be compared");
    return c;
}

// ---- guest side ----------------------------------------------------------------------------------------------

u32 g_scratch;
u32 scratch_alloc(u32 size, u32 align)
{
    g_scratch = (g_scratch + align - 1u) & ~(align - 1u);
    const u32 r = g_scratch;
    g_scratch += size;
    return r;
}
void guest_write32(u32 ea, u32 v) { vm_write32(ea, v); }

void put_bytes(const std::vector<u8>& d, u32 ea, u64 seq)
{
    if ((u64)ea + d.size() > ppu_vm_size)
        fail("blob-range", "event seq " + std::to_string(seq) + ": a " + std::to_string(d.size()) + "-byte blob at ea " +
                               hex8(ea) + " leaves guest memory");
    memcpy(vm_base + ea, d.data(), d.size());
}

u64 be64(const u8* p)
{
    u64 v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

// ---- one replay ------------------------------------------------------------------------------------------------

struct Options {
    std::string dir, out, manifest;
    bool have_range = false;
    long long a = 0, b = 0;
    bool accept_unknown = false;
};

const char* const kMachineFields[8] = {"gpu", "macos", "metal", "build", "glslang", "spirv_cross", "toolkit_sha", "title_sha"};

struct Row {
    J live, replay;
    std::string result, field;
    bool gate_valid = true;
};

struct Report {
    Options opt;
    std::string status = "error";
    int exit_code = 2;
    std::string capture_hash, manifest;
    bool gate_valid = true;
    long long from = 0, to = 0;
    std::vector<std::pair<std::string, std::string>> env_live, env_replay;
    std::vector<std::string> env_differs;
    std::vector<Row> rows;
    std::string err_name, err_detail;
    std::string first;
    u32 steps = 0, checked_writes = 0;
};

std::string frame_id(const J& f)
{
    const J& id = f.at("frame");
    return std::to_string(id.at("flip_id").i);
}

// The frame identity of M3-SCOPE H1: {flip_id, guest_frame, display_buffer}.
std::string contract_id(const J& id)
{
    return "{\"flip_id\":" + dump(id.at("flip_id")) + ",\"guest_frame\":" + dump(id.at("guest_frame")) +
           ",\"display_buffer\":" + dump(id.at("display_buffer")) + "}";
}

// frame-complete records as the replay's own writer produced them.
std::vector<J> read_frames(const std::string& path)
{
    std::ifstream f(path);
    if (!f) fail("replay-manifest", "the replay left no " + path);
    std::vector<J> v;
    std::string line;
    while (next_line(f, line))
        if (event_of(line) == "frame-complete") v.push_back(parse_line(line));
    return v;
}

bool unknown_only(const J& f)
{
    const J& inv = f.at("invalid");
    for (const J& r : inv.a) if (r.s != "unknown-methods") return false;
    return true;
}

void run(Report& rep)
{
    const Options& opt = rep.opt;
    Capture cap = scan(opt.dir);

    // ---- which frames
    long long a = 0, b = (long long)0x7FFFFFFF;
    if (opt.have_range) { a = opt.a; b = opt.b; }
    rep.from = a;
    size_t last = (size_t)-1;
    for (size_t i = 0; i < cap.frames.size(); i++) {
        const long long id = cap.frames[i].at("frame").at("flip_id").i;
        if (id >= a && id <= b) last = i;
    }
    if (last == (size_t)-1) {
        rep.status = "no-frames";
        rep.exit_code = 3;
        rep.err_name = "zero-frames";
        rep.err_detail = "no frame of the capture has a flip_id in " + std::to_string(a) + ".." + std::to_string(b) +
                         ": a replay that compares nothing proves nothing";
        rep.to = b;
        return;
    }
    rep.to = cap.frames[last].at("frame").at("flip_id").i;
    rep.capture_hash = cap.end_hash;

    // ---- the refusals that need no GPU
    if (ps3_cfg_video_rsx_capture())
        fail("config", "video.rsx_capture is on: the replay writes its own manifest and must not also capture");
    for (size_t i = 0; i <= last; i++) {
        const u32 n = cap.frames[i].u("unknown_methods");
        if (n && !opt.accept_unknown)
            fail("unknown-method", "flip " + frame_id(cap.frames[i]) + " consumed " + std::to_string(n) +
                                       " method(s) no object class handles; classify them, or pass "
                                       "--accept-unknown-methods for a diagnostic replay that is not gate-valid");
    }
    if (opt.accept_unknown) rep.gate_valid = false;

    // ---- guest memory and the backend
    void* p = mmap(nullptr, kVmSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) fail("guest-memory", std::string("mmap: ") + strerror(errno));
    vm_base = (uint8_t*)p;
    ppu_vm_size = kVmSize;
    if (cap.have_label) ppu_hle_inject_base = cap.hle_base;
    g_scratch = ppu_hle_inject_base + 0x4000u;

    setenv("PS3RECOMP_METAL_HEADLESS", "1", 1);
    if (rsx_metal_backend_init(1280, 720, "rsx_replay") != 0)
        fail("backend", "the Metal backend did not initialise");
    const u32 st = rsx_metal_backend_status();
    if (!(st & RSX_METAL_ENGINE_PATH))
        fail("backend", "the register-file draw engine is not driving the backend (PS3RECOMP_RSX_ENGINE?)");
    if (!(st & RSX_METAL_GUEST_SHADERS))
        fail("fixed-function", "guest shaders are off (translator missing, or PS3RECOMP_METAL_FIXED_FUNCTION set): "
                               "a fixed-function replay is not a baseline");

    rsx_capture_environment env;
    if (rsx_metal_backend_capture_environment(&env) != 0) fail("backend", "no machine record");
    // The title that produced the inputs is provenance, not a rendering input: the manifest carries it on.
    const std::string title_sha = cap.machine.str("title_sha");
    env.title_sha = title_sha.c_str();

    // ---- comparability
    rsx_capture_options copt;
    std::string mdir = opt.manifest;
    if (mdir.empty()) {
        if (!opt.out.empty()) mdir = opt.out + ".manifest";
        else {
            const char* t = getenv("TMPDIR");
            std::string tmpl = std::string(t && *t ? t : "/tmp") + "/rsx_replay.XXXXXX";
            std::vector<char> b2(tmpl.begin(), tmpl.end());
            b2.push_back(0);
            if (!mkdtemp(b2.data())) fail("manifest", std::string("mkdtemp: ") + strerror(errno));
            mdir = b2.data();
            rmdir(mdir.c_str());   // the writer creates it; mkdtemp only chose a free name
        }
    }
    rep.manifest = mdir;
    copt.dir = mdir.c_str();
    copt.max_frames = (u32)(last + 1);
    if (rsx_capture_install(&env, &copt) != 1) fail("manifest", "the replay's capture did not open");
    {
        // What this build and machine write into the manifest is what the capture must have been made on.
        const rsx_capture_environment* e = rsx_capture_environment_record();
        const char* mine[8] = {e->gpu, e->macos, e->metal, e->build, e->glslang, e->spirv_cross, e->toolkit_sha, e->title_sha};
        for (int i = 0; i < 8; i++) {
            rep.env_live.emplace_back(kMachineFields[i], cap.machine.str(kMachineFields[i]));
            rep.env_replay.emplace_back(kMachineFields[i], mine[i]);
            if (rep.env_live.back().second != mine[i]) rep.env_differs.push_back(kMachineFields[i]);
        }
    }
    if (!rep.env_differs.empty()) {
        rep.status = "not-comparable";
        rep.exit_code = 4;
        rsx_capture_finish();
        return;
    }

    // ---- replay
    BlobStore blobs(opt.dir);
    std::ifstream f(opt.dir + "/capture.ndjson");
    std::string line;
    ps3_sha256_ctx running;
    ps3_sha256_init(&running);
    size_t frame_ix = 0;
    bool gcm_ready = false;
    while (next_line(f, line)) {
        const std::string ev = event_of(line);
        if (ev == "environment" || ev == "draw") continue;
        const bool input = ev == "capture-start" || ev == "gcm-init" || ev == "gcm-state" || ev == "fifo-method" ||
                           ev == "fifo-branch" || ev == "fifo-word" || ev == "fifo-nop";
        const J rec = parse_line(line);
        const u64 seq = (u64)rec.at("seq").i;
        if (input) {
            ps3_sha256_update(&running, line.data(), line.size());
            ps3_sha256_update(&running, "\n", 1);
        }
        if (ev == "capture-start") {
            if (rec.at("bootstrap").str("mode") != "clean-reset")
                fail("bootstrap", "only a clean-reset capture can be replayed");
        } else if (ev == "gcm-init") {
            const u32 ctx_out = scratch_alloc(4, 4);
            if (!cellGcmSetupContext(ctx_out, rec.u("cmd_size"), rec.u("io_size"), rec.hex("io_ea"), scratch_alloc, guest_write32))
                fail("gcm-init", "cellGcmSetupContext refused the recorded parameters (event seq " + std::to_string(seq) + ")");
            gcm_ready = true;
        } else if (ev == "gcm-state") {
            const std::string& op = rec.str("op");
            if (op == "io-map") {
                if (cellGcmMapEaIoAddress(rec.hex("ea"), rec.hex("io"), rec.u("size")) != 0)
                    fail("gcm-state", "cellGcmMapEaIoAddress refused event seq " + std::to_string(seq));
            } else if (op == "io-unmap") {
                if (cellGcmUnmapEaIoAddress(rec.hex("ea")) != 0)
                    fail("gcm-state", "cellGcmUnmapEaIoAddress refused event seq " + std::to_string(seq));
            } else if (op == "display-buffer") {
                if (cellGcmSetDisplayBuffer(rec.u("id"), rec.hex("offset"), rec.u("pitch"), rec.u("width"), rec.u("height")) != 0)
                    fail("gcm-state", "cellGcmSetDisplayBuffer refused event seq " + std::to_string(seq));
            } else fail("gcm-state", "an unknown op \"" + op + "\" at event seq " + std::to_string(seq));
        } else if (ev == "fifo-method" || ev == "fifo-branch" || ev == "fifo-word" || ev == "fifo-nop") {
            if (!gcm_ready) fail("capture-format", "a FIFO step before gcm-init (event seq " + std::to_string(seq) + ")");
            // Every input first, the way the RSX had them when it consumed the step.
            for (const J& r : rec.at("reads").a) put_bytes(blobs.get(r, seq), r.hex("ea"), seq);
            std::vector<u64> ts;
            for (const J& w : rec.at("writes").a) {
                const std::vector<u8>& d = blobs.get(w, seq);      // present and intact, or a named error before the step
                if (w.str("kind") == "report") {
                    if (d.size() < 8) fail("capture-format", "a report write shorter than its timer (event seq " + std::to_string(seq) + ")");
                    ts.push_back(be64(d.data()));
                }
            }
            GcmReplayStep s;
            s.get = rec.hex("get"); s.put = rec.hex("put"); s.call_ret = rec.hex("call_ret");
            if (ev == "fifo-method") s.end = s.get + 4u * (1u + rec.u("count"));
            else if (ev == "fifo-word") s.end = s.get + 4u;
            else if (ev == "fifo-nop") s.end = s.get + 4u * rec.u("words");
            else s.end = rec.hex("target");
            s.report_ts = ts.data(); s.n_report_ts = (u32)ts.size();
            u32 got[2];
            switch (cellGcm_rsx_replay_step(&s, got)) {
            case 0: break;
            case GCM_REPLAY_NOT_AT_STEP:
                fail("step-discontinuity", "event seq " + std::to_string(seq) + " starts at get " + hex8(s.get) + " (call_ret " +
                                               hex8(s.call_ret) + ") but the walker stands at " + hex8(got[0]) + " (" + hex8(got[1]) + ")");
            case GCM_REPLAY_LANDED_ELSEWHERE:
                fail("step-landing", "event seq " + std::to_string(seq) + " ends at " + hex8(s.end) + " but the walker left it at " +
                                         hex8(got[0]));
            default:
                fail("write-mismatch", "event seq " + std::to_string(seq) + " recorded " + std::to_string(ts.size()) +
                                           " report write(s); the replay wrote " + std::to_string(got[0]));
            }
            rep.steps++;
            // What the RSX wrote, against what the recording says it wrote.
            for (const J& w : rec.at("writes").a) {
                const std::vector<u8>& d = blobs.get(w, seq);
                const u32 ea = w.hex("ea");
                if ((u64)ea + d.size() > ppu_vm_size) fail("blob-range", "a recorded write leaves guest memory (event seq " + std::to_string(seq) + ")");
                rep.checked_writes++;
                if (memcmp(vm_base + ea, d.data(), d.size()) != 0) {
                    size_t k = 0;
                    while (vm_base[ea + k] == d[k]) k++;
                    char hb[48];
                    snprintf(hb, sizeof hb, "replay 0x%02x, recorded 0x%02x", vm_base[ea + k], d[k]);
                    fail("write-mismatch", "event seq " + std::to_string(seq) + ": the " + w.str("kind") + " write at ea " + hex8(ea) +
                                               " differs from the recording at byte " + std::to_string(k) + " (" + hb + ")");
                }
            }
        } else if (ev == "frame-complete") {
            // The recorded hash of everything consumed so far: the manifest was not edited since it was written.
            if (hash_value(rec, "capture_hash") != digest_hex(running))
                fail("capture-hash", "frame-complete at event seq " + std::to_string(seq) +
                                         " records an input hash the capture's own events do not give: the manifest was altered");
            if (frame_ix++ == last) break;
        } else if (ev == "capture-end") {
            fail("capture-format", "capture-end before the last compared frame");
        } else {
            fail("capture-format", "an unknown event \"" + ev + "\" at seq " + std::to_string(seq));
        }
    }
    rsx_capture_finish();
    rsx_metal_backend_shutdown();

    // ---- what the replay produced, against what the capture recorded
    const std::vector<J> mine = read_frames(mdir + "/capture.ndjson");
    if (mine.size() != last + 1)
        fail("frame-identity", "the replay produced " + std::to_string(mine.size()) + " frame(s) where the capture has " +
                                   std::to_string(last + 1) + " up to flip " + std::to_string(rep.to));
    bool any_differs = false, any_invalid = false;
    for (size_t i = 0; i <= last; i++) {
        const J& L = cap.frames[i];
        const J& R = mine[i];
        // present_seq counts the engine's presents, and the title's frame clock adds presents of its own between
        // flips (host timing), so it is reported and never compared; the contract identity is the other three.
        if (contract_id(L.at("frame")) != contract_id(R.at("frame")))
            fail("frame-identity", "frame " + std::to_string(i) + ": the capture has " + contract_id(L.at("frame")) + ", the replay " +
                                       contract_id(R.at("frame")));
        const long long id = L.at("frame").at("flip_id").i;
        if (id < a || id > b) continue;
        Row row;
        row.live = L; row.replay = R;
        row.result = "equal";
        const char* fields[] = {"capture_hash", "state_hash", "frame_hash", "readback", "draws", "unknown_methods", "resyncs"};
        for (const char* k : fields) {
            if (dump(L.at(k)) != dump(R.at(k))) {
                row.result = "differs";
                row.field = k;
                if (rep.first.empty()) rep.first = "flip " + std::to_string(id) + ": " + k + ": " + dump(L.at(k)) + " vs " + dump(R.at(k));
                break;
            }
        }
        const bool valid = R.at("valid").b;
        row.gate_valid = valid;
        if (!valid && !(opt.accept_unknown && unknown_only(R))) {
            if (row.result == "equal") row.result = "invalid";
            if (rep.first.empty()) rep.first = "flip " + std::to_string(id) + ": invalid: " + dump(R.at("invalid"));
        }
        if (!valid) rep.gate_valid = false;
        any_differs |= row.result == "differs";
        any_invalid |= row.result == "invalid";
        rep.rows.push_back(std::move(row));
    }
    if (rep.rows.empty()) {
        rep.status = "no-frames"; rep.exit_code = 3;
        rep.err_name = "zero-frames"; rep.err_detail = "no frame was compared";
    } else if (any_differs) { rep.status = "differ"; rep.exit_code = 1; }
    else if (any_invalid) { rep.status = "invalid"; rep.exit_code = 1; }
    else if (!rep.gate_valid) { rep.status = "diagnostic-equal"; rep.exit_code = 5; }
    else { rep.status = "equal"; rep.exit_code = 0; }
}

// ---- the report ------------------------------------------------------------------------------------------------

std::string hashes(const J& f)
{
    std::string s = "{\"capture_hash\":" + quote(hash_value(f, "capture_hash")) + ",\"state_hash\":" + quote(hash_value(f, "state_hash"));
    const J& fh = f.at("frame_hash");
    s += ",\"frame_hash\":" + (fh.t == J::Null ? std::string("null") : quote(fh.str("value")));
    s += ",\"valid\":" + dump(f.at("valid")) + ",\"invalid\":" + dump(f.at("invalid")) + "}";
    return s;
}

std::string env_json(const std::vector<std::pair<std::string, std::string>>& v)
{
    std::string s = "{";
    for (size_t i = 0; i < v.size(); i++) { if (i) s += ','; s += quote(v[i].first) + ":" + quote(v[i].second); }
    return s + "}";
}

std::string report_json(const Report& r)
{
    std::string s = "{\"schema\":\"ps3diff.rsx.replay/1\",\"status\":" + quote(r.status) + ",\"exit\":" + std::to_string(r.exit_code);
    s += ",\"capture\":" + quote(r.opt.dir) + ",\"capture_hash\":" + quote(r.capture_hash) + ",\"manifest\":" + quote(r.manifest);
    s += std::string(",\"gate_valid\":") + (r.gate_valid && r.status == "equal" ? "true" : "false");
    s += std::string(",\"accept_unknown_methods\":") + (r.opt.accept_unknown ? "true" : "false");
    s += ",\"frames\":{\"from\":" + std::to_string(r.from) + ",\"to\":" + std::to_string(r.to) + "}";
    s += ",\"steps\":" + std::to_string(r.steps) + ",\"checked_writes\":" + std::to_string(r.checked_writes);
    s += ",\"environment\":{\"live\":" + env_json(r.env_live) + ",\"replay\":" + env_json(r.env_replay) + ",\"differs\":[";
    for (size_t i = 0; i < r.env_differs.size(); i++) { if (i) s += ','; s += quote(r.env_differs[i]); }
    s += "]},\"compared\":" + std::to_string(r.rows.size());
    s += ",\"error\":" + (r.err_name.empty() ? std::string("null") : "{\"name\":" + quote(r.err_name) + ",\"detail\":" + quote(r.err_detail) + "}");
    s += ",\"first\":" + (r.first.empty() ? std::string("null") : quote(r.first)) + ",\"results\":[";
    for (size_t i = 0; i < r.rows.size(); i++) {
        const Row& w = r.rows[i];
        const J& id = w.live.at("frame");
        if (i) s += ',';
        // gate_valid is per frame: a frame that consumed an unknown method, or was replayed on request despite one, is not.
        const J& rid = w.replay.at("frame");
        s += "{\"frame\":" + contract_id(id) + ",\"present_seq\":{\"live\":" + dump(id.at("present_seq")) + ",\"replay\":" +
             dump(rid.at("present_seq")) + "},\"result\":" + quote(w.result) + ",\"field\":" + (w.field.empty() ? std::string("null") : quote(w.field));
        s += std::string(",\"gate_valid\":") + (w.gate_valid && !r.opt.accept_unknown ? "true" : "false");
        s += ",\"live\":" + hashes(w.live) + ",\"replay\":" + hashes(w.replay) + "}";
    }
    return s + "]}\n";
}

void write_report(const Report& r)
{
    const std::string text = report_json(r);
    if (r.opt.out.empty()) return;
    FILE* f = fopen(r.opt.out.c_str(), "wb");
    if (!f || fwrite(text.data(), 1, text.size(), f) != text.size() || fclose(f) != 0)
        fprintf(stderr, "rsx_replay: cannot write %s: %s\n", r.opt.out.c_str(), strerror(errno));
}

bool parse_range(const char* s, long long* a, long long* b)
{
    char* e = nullptr;
    *a = strtoll(s, &e, 10);
    if (e == s || *a < 0) return false;
    if (!*e) { *b = *a; return true; }
    if (*e != '-') return false;
    const char* t = e + 1;
    *b = strtoll(t, &e, 10);
    return e != t && !*e && *b >= *a;
}

}  // namespace

int main(int argc, char** argv)
{
    Report rep;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto value = [&](std::string* into) {
            if (i + 1 >= argc) { fprintf(stderr, "rsx_replay: %s needs a value\n", arg.c_str()); exit(2); }
            *into = argv[++i];
        };
        if (arg == "--frames") {
            std::string v;
            value(&v);
            if (!parse_range(v.c_str(), &rep.opt.a, &rep.opt.b)) { fprintf(stderr, "rsx_replay: --frames wants a or a-b\n"); return 2; }
            rep.opt.have_range = true;
        } else if (arg == "--out") value(&rep.opt.out);
        else if (arg == "--manifest") value(&rep.opt.manifest);
        else if (arg == "--accept-unknown-methods") rep.opt.accept_unknown = true;
        else if (!arg.empty() && arg[0] == '-') { fprintf(stderr, "rsx_replay: unknown option %s\n", arg.c_str()); return 2; }
        else if (rep.opt.dir.empty()) rep.opt.dir = arg;
        else { fprintf(stderr, "rsx_replay: one capture directory only\n"); return 2; }
    }
    if (rep.opt.dir.empty()) {
        fprintf(stderr, "usage: rsx_replay <capture-dir> [--frames a-b] [--out report.json] [--manifest dir] [--accept-unknown-methods]\n");
        return 2;
    }
    while (rep.opt.dir.size() > 1 && rep.opt.dir.back() == '/') rep.opt.dir.pop_back();
    try {
        run(rep);
    } catch (const Err& e) {
        rep.status = "error";
        rep.exit_code = 2;
        rep.err_name = e.name;
        rep.err_detail = e.detail;
        rep.gate_valid = false;
    }
    write_report(rep);
    fflush(stdout);
    if (rep.status == "error")
        fprintf(stderr, "rsx_replay: error %s: %s\n", rep.err_name.c_str(), rep.err_detail.c_str());
    else if (rep.status == "no-frames")
        fprintf(stderr, "rsx_replay: error %s: %s\n", rep.err_name.c_str(), rep.err_detail.c_str());
    else if (rep.status == "not-comparable") {
        fprintf(stderr, "rsx_replay: not comparable: the environment records differ in");
        for (const auto& k : rep.env_differs) fprintf(stderr, " %s", k.c_str());
        fprintf(stderr, "\n");
    } else
        fprintf(stderr, "rsx_replay: %s: %zu frame(s) compared over %u steps, %u recorded writes checked%s%s\n", rep.status.c_str(),
                rep.rows.size(), rep.steps, rep.checked_writes, rep.first.empty() ? "" : "; first: ", rep.first.c_str());
    // A stop may have raised on the way; an exit status of 0 is only ever the clean verdict above.
    return rep.exit_code;
}
