/*
 * ps3recomp - RSX capture writer (ps3diff.rsx/1), see rsx_capture.h and docs/PS3DIFF.md.
 *
 * One event is written per FIFO step the walker took, in the order it took
 * them, with every guest-memory span the step consumed as a content-addressed
 * blob. What the step produced (draws, a completed frame) follows it as
 * output events that are not part of the capture hash: the capture hash is
 * over the inputs only, so a replay that reproduces the inputs reproduces it.
 *
 * Canonical form is what Python's json.dumps(ensure_ascii=True,
 * separators=(",", ":")) writes, so tools/ps3diff re-serialises each line and
 * refuses one that differs. Only integers, booleans, null and ASCII strings
 * are written; the one float is the constant `"t":0.0`, because a wall clock in
 * a capture would make two captures of the same inputs differ.
 */
#include "rsx_capture.h"

#include "rsx_dispatch.h"
#include "ps3emu/config.h"
#include "ps3emu/sha256.h"
#include "ps3emu/stop.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef PS3RECOMP_TOOLKIT_SHA
#define PS3RECOMP_TOOLKIT_SHA "unknown"
#endif
#ifndef PS3RECOMP_BUILD_TYPE
#define PS3RECOMP_BUILD_TYPE "unknown"
#endif

namespace {

constexpr const char* kSchema = "ps3diff.rsx/1";
constexpr u32 kNoFlip = 0xFFFFFFFFu;

const char* const kReadKind[RSX_CAP_READ_KIND_COUNT] = {
    "other", "fifo", "label", "vertex", "index", "texture", "program", "dma-source",
};
const char* const kWriteKind[RSX_CAP_WRITE_KIND_COUNT] = {
    "other", "label", "report", "dma-destination",
};

[[noreturn]] void fail(const char* reason, const std::string& detail)
{
    ps3_stop_rsx_capture("rsx_capture", reason, detail.c_str());
}

std::string hex8(u32 v)
{
    char b[16];
    snprintf(b, sizeof b, "0x%08x", v);
    return b;
}

std::string hex_of(const u8* d, size_t n)
{
    char h[PS3_SHA256_HEX_SIZE];
    ps3_sha256_hex(d, n, h);
    return h;
}

std::string hex_digest(const u8 d[32])
{
    static const char* x = "0123456789abcdef";
    std::string s(64, '0');
    for (int i = 0; i < 32; i++) { s[2 * i] = x[d[i] >> 4]; s[2 * i + 1] = x[d[i] & 15]; }
    return s;
}

std::string jstr(const char* s)
{
    std::string o = "\"";
    for (; s && *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c >= 0x20 && c < 0x7F) o += (char)c;
        else o += '?';
    }
    return o + "\"";
}

void be32(std::string& out, u32 v)
{
    out += (char)(v >> 24); out += (char)(v >> 16); out += (char)(v >> 8); out += (char)v;
}

struct Rd {
    u32 ea, loc, off, kind;
    u64 size, seq;
    std::string sha;
    bool write;
};

struct Frame {
    std::string prefix, suffix;   // the capture hash goes between them
};

struct Cap {
    bool open = false, started = false, finishing = false;
    std::string dir;
    FILE* f = nullptr;
    u64 seq = 0, consume_seq = 0, events = 0, frames = 0;
    u32 max_frames = 0;
    ps3_sha256_ctx in_hash;
    std::unordered_set<std::string> blobs;
    rsx_capture_environment env_copy;
    std::string env_strs[8];

    // The open step.
    bool in_step = false;
    rsx_cap_step step{};
    std::vector<u32> words;
    std::vector<Rd> reads, writes;
    std::deque<std::vector<u8>> arena;
    std::vector<std::pair<u32, u32>> binds;
    u32 bound_at_start = 0;
    u64 step_consume0 = 0;      // consume_seq when the open step began

    // A run of consecutive zero words, written as one event.
    bool nop = false;
    u32 nop_get = 0, nop_put = 0, nop_call = 0, nop_words = 0;
    u32 (*nop_io2ea)(u32) = nullptr;

    u32 bound[8] = {}, bound_before[8] = {};
    std::string program_sha;
    std::vector<std::pair<std::string, std::string>> out_lines;   // output events queued behind the open step
    std::vector<Frame> out_frames;

    // The frame in progress.
    u64 submitted = 0, executed = 0, fixed = 0, unknown = 0, resyncs = 0;
    std::map<std::string, u64> dropped;
    ps3_sha256_ctx chain;
    u32 last_flip = kNoFlip;
};

Cap C;

void write_line(const std::string& line, bool input)
{
    if (!C.f) return;
    if (fwrite(line.data(), 1, line.size(), C.f) != line.size() || fputc('\n', C.f) == EOF)
        fail("io", std::string("write ") + C.dir + "/capture.ndjson.partial: " + strerror(errno));
    if (input) {
        ps3_sha256_update(&C.in_hash, line.data(), line.size());
        ps3_sha256_update(&C.in_hash, "\n", 1);
    }
    C.events++;
}

std::string envelope(const char* event)
{
    char b[160];
    snprintf(b, sizeof b,
             "{\"schema\":\"%s\",\"seq\":%llu,\"ctx_seq\":%llu,\"t\":0.0,\"thread\":\"rsx\","
             "\"event\":\"%s\",\"ctx\":{\"kind\":\"ppu\",\"id\":\"rsx\"}",
             kSchema, (unsigned long long)C.seq, (unsigned long long)C.seq, event);
    C.seq++;
    return b;
}

void emit(const char* event, const std::string& fields, bool input)
{
    write_line(envelope(event) + fields + "}", input);
}

std::string hash_obj(const char* scope, const std::string& value)
{
    return std::string("{\"algo\":\"sha256\",\"scope\":\"") + scope + "\",\"value\":\"" + value + "\"}";
}

std::string current_input_hash()
{
    ps3_sha256_ctx copy = C.in_hash;
    u8 d[32];
    ps3_sha256_final(&copy, d);
    return hex_digest(d);
}

// ---- directories and blobs ----------------------------------------------------

void mkdir_p(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); i++) {
        if (i != path.size() && path[i] != '/') continue;
        const std::string part = path.substr(0, i);
        if (mkdir(part.c_str(), 0777) != 0 && errno != EEXIST)
            fail("io", "mkdir " + part + ": " + strerror(errno));
    }
}

bool exists(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

void store_blob(const std::string& sha, const u8* d, size_t n)
{
    if (!C.blobs.insert(sha).second) return;
    const std::string sub = C.dir + "/blobs/sha256/" + sha.substr(0, 2);
    mkdir_p(sub);
    const std::string path = sub + "/" + sha, tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) fail("io", "open " + tmp + ": " + strerror(errno));
    if (fwrite(d, 1, n, f) != n || fclose(f) != 0) fail("io", "write " + tmp + ": " + strerror(errno));
    if (rename(tmp.c_str(), path.c_str()) != 0) fail("io", "rename " + tmp + ": " + strerror(errno));
}

std::string rd_json(const Rd& r)
{
    char b[96];
    std::string s = "{\"ea\":\"" + hex8(r.ea) + "\",\"location\":\"" + (r.loc == RSX_CAP_LOC_LOCAL ? "local" : "main") + "\",";
    snprintf(b, sizeof b, "\"size\":%llu,", (unsigned long long)r.size);
    s += b;
    s += "\"sha256\":\"" + r.sha + "\",";
    snprintf(b, sizeof b, "\"consume_seq\":%llu,", (unsigned long long)r.seq);
    s += b;
    s += "\"offset\":\"" + hex8(r.off) + "\",\"kind\":\"" +
         (r.write ? kWriteKind[r.kind < RSX_CAP_WRITE_KIND_COUNT ? r.kind : 0]
                  : kReadKind[r.kind < RSX_CAP_READ_KIND_COUNT ? r.kind : 0]) + "\"}";
    return s;
}

std::string rd_array(const std::vector<Rd>& v)
{
    std::string s = "[";
    for (size_t i = 0; i < v.size(); i++) { if (i) s += ','; s += rd_json(v[i]); }
    return s + "]";
}

// A span two reads of one event disagree on cannot be replayed: the replay
// writes every blob of an event before it runs the event.
void check_conflict(const Rd& r, const u8* bytes, const std::vector<Rd>& prior,
                    const std::deque<std::vector<u8>>& copies, size_t first_copy)
{
    (void)first_copy;
    for (size_t i = 0; i < prior.size(); i++) {
        const Rd& p = prior[i];
        if (p.write) continue;
        const u64 lo = r.ea > p.ea ? r.ea : p.ea;
        const u64 hi = (u64)(r.ea + r.size < p.ea + p.size ? r.ea + r.size : p.ea + p.size);
        if (lo >= hi) continue;
        if (i >= copies.size() || copies[i].empty()) continue;
        if (memcmp(bytes + (lo - r.ea), copies[i].data() + (lo - p.ea), (size_t)(hi - lo)) != 0)
            fail("read-conflict", "two reads of one event disagree at ea " + hex8((u32)lo) +
                                  " (consume_seq " + std::to_string(p.seq) + " and " +
                                  std::to_string(C.consume_seq) + ")");
    }
}

std::deque<std::vector<u8>> g_read_copies;   // parallel to C.reads

const u8* record_read(u32 loc, u32 off, u32 ea, const u8* bytes, u32 size, u32 kind)
{
    if (!C.in_step) fail("read-outside-event", "a guest read at ea " + hex8(ea) + " has no FIFO step to belong to");
    if (!size) return bytes;
    const std::string sha = hex_of(bytes, size);
    for (size_t i = 0; i < C.reads.size(); i++)
        if (C.reads[i].ea == ea && C.reads[i].size == size && C.reads[i].sha == sha) {
            if (kind == RSX_CAP_READ_PROGRAM) C.program_sha = sha;
            return g_read_copies[i].data();
        }
    check_conflict(Rd{ea, loc, off, kind, size, 0, sha, false}, bytes, C.reads, g_read_copies, 0);
    Rd r{ea, loc, off, kind, size, C.consume_seq++, sha, false};
    g_read_copies.emplace_back(bytes, bytes + size);
    C.reads.push_back(r);
    store_blob(sha, bytes, size);
    if (kind == RSX_CAP_READ_PROGRAM) C.program_sha = sha;
    return g_read_copies.back().data();
}

// FIFO words, split into runs that are contiguous in guest memory.
void record_words(const std::vector<u32>& w, u32 get, u32 (*io2ea)(u32))
{
    size_t i = 0;
    while (i < w.size()) {
        const u32 ea0 = io2ea(get + 4u * (u32)i);
        if (!ea0) fail("fifo-unmapped", "FIFO word at IO offset " + hex8(get + 4u * (u32)i) + " has no mapping");
        size_t j = i + 1;
        while (j < w.size() && io2ea(get + 4u * (u32)j) == ea0 + 4u * (u32)(j - i)) j++;
        std::string bytes;
        for (size_t k = i; k < j; k++) be32(bytes, w[k]);
        record_read(RSX_CAP_LOC_MAIN, get + 4u * (u32)i, ea0, (const u8*)bytes.data(), (u32)bytes.size(),
                    RSX_CAP_READ_FIFO);
        i = j;
    }
}

// ---- events ---------------------------------------------------------------------

void flush_nop()
{
    if (!C.nop) return;
    C.nop = false;
    C.reads.clear(); g_read_copies.clear();
    C.in_step = true;
    std::vector<u32> z(C.nop_words, 0u);
    record_words(z, C.nop_get, C.nop_io2ea);
    C.in_step = false;
    char b[160];
    snprintf(b, sizeof b, ",\"get\":\"%s\",\"put\":\"%s\",\"call_ret\":\"%s\",\"words\":%u,\"reads\":",
             hex8(C.nop_get).c_str(), hex8(C.nop_put).c_str(), hex8(C.nop_call).c_str(), C.nop_words);
    emit("fifo-nop", std::string(b) + rd_array(C.reads) + ",\"writes\":[]", true);
    C.reads.clear(); g_read_copies.clear();
}

std::string machine_json()
{
    const rsx_capture_environment& e = C.env_copy;
    return std::string("{\"gpu\":") + jstr(e.gpu) + ",\"macos\":" + jstr(e.macos) + ",\"metal\":" + jstr(e.metal) +
           ",\"build\":" + jstr(e.build) + ",\"glslang\":" + jstr(e.glslang) +
           ",\"spirv_cross\":" + jstr(e.spirv_cross) + ",\"toolkit_sha\":" + jstr(e.toolkit_sha) +
           ",\"title_sha\":" + jstr(e.title_sha) + "}";
}

void emit_frame(const Frame& fr)
{
    emit("frame-complete", fr.prefix + hash_obj("input-events/v1", current_input_hash()) + fr.suffix, false);
}

void flush_outputs()
{
    std::vector<std::pair<std::string, std::string>> q;
    q.swap(C.out_lines);
    for (const auto& l : q) emit(l.first.c_str(), l.second, false);
}

void detach_and_close()
{
    if (!C.open) return;
    flush_nop();
    flush_outputs();
    char b[96];
    snprintf(b, sizeof b, ",\"frames\":%llu,\"events\":%llu,\"capture_hash\":",
             (unsigned long long)C.frames, (unsigned long long)C.events);
    emit("capture-end", std::string(b) + hash_obj("input-events/v1", current_input_hash()), false);
    const std::string part = C.dir + "/capture.ndjson.partial", fin = C.dir + "/capture.ndjson";
    if (fflush(C.f) != 0 || fclose(C.f) != 0) fail("io", "close " + part + ": " + strerror(errno));
    C.f = nullptr;
    if (rename(part.c_str(), fin.c_str()) != 0) fail("io", "rename " + part + ": " + strerror(errno));
    C.open = false;
    cellGcm_set_capture_sink(nullptr);
    rsx_draw_engine_set_capture_sink(nullptr);
}

// ---- the sink ---------------------------------------------------------------------

void s_gcm_init(u32 cmd_size, u32 io_size, u32 io_ea)
{
    flush_nop(); flush_outputs();
    C.started = true;
    char b[160];
    snprintf(b, sizeof b, ",\"cmd_size\":%u,\"io_size\":%u,\"io_ea\":\"%s\"", cmd_size, io_size, hex8(io_ea).c_str());
    emit("gcm-init", b, true);
}

void s_io_map(u32 mapped, u32 ea, u32 io, u32 size)
{
    flush_nop(); flush_outputs();
    char b[200];
    snprintf(b, sizeof b, ",\"op\":\"%s\",\"ea\":\"%s\",\"io\":\"%s\",\"size\":%u", mapped ? "io-map" : "io-unmap",
             hex8(ea).c_str(), hex8(io).c_str(), size);
    emit("gcm-state", b, true);
}

void s_display_buffer(u32 id, u32 offset, u32 pitch, u32 width, u32 height)
{
    flush_nop(); flush_outputs();
    char b[220];
    snprintf(b, sizeof b, ",\"op\":\"display-buffer\",\"id\":%u,\"offset\":\"%s\",\"pitch\":%u,\"width\":%u,\"height\":%u",
             id, hex8(offset).c_str(), pitch, width, height);
    emit("gcm-state", b, true);
}

void s_step_begin(const rsx_cap_step* s)
{
    if (!C.started)
        fail("midstream", "the first RSX command (GET " + hex8(s->get) + ") came before any cellGcmInit; "
                          "only a clean reset is a capture");
    if (C.in_step) fail("io", "nested FIFO step");
    const bool zero = s->kind == RSX_CAP_STEP_METHOD && s->count == 0 && s->nwords == 1 && s->words[0] == 0;
    if (zero) {
        if (C.nop && C.nop_put == s->put && C.nop_call == s->call_ret && C.nop_io2ea == s->io2ea &&
            C.nop_get + 4u * C.nop_words == s->get) {
            C.nop_words++;
        } else {
            flush_nop(); flush_outputs();
            C.nop = true; C.nop_get = s->get; C.nop_put = s->put; C.nop_call = s->call_ret;
            C.nop_words = 1; C.nop_io2ea = s->io2ea;
        }
        C.in_step = false;
        C.step.kind = ~0u;          // marks "a coalesced zero word": step_end has nothing to write
        C.step.nwords = 0;
        return;
    }
    flush_nop(); flush_outputs();
    C.step = *s;
    C.words.assign(s->words, s->words + s->nwords);
    C.step.words = C.words.data();
    C.reads.clear(); C.writes.clear(); C.arena.clear(); g_read_copies.clear(); C.binds.clear();
    C.in_step = true;
    C.step_consume0 = C.consume_seq;
    C.program_sha.clear();
    memcpy(C.bound_before, C.bound, sizeof C.bound);
    C.bound_at_start = s->subch < 8 ? C.bound[s->subch] : 0;
    record_words(C.words, s->get, s->io2ea);
    if (s->kind == RSX_CAP_STEP_METHOD) {
        const u32 method = C.words[0] & 0x1FFCu;
        for (u32 i = 0; i < s->count && 1 + i < s->nwords; i++) {
            const u32 m = s->incrementing ? method + i * 4u : method;
            if (m == 0 && s->subch < 8) { C.bound[s->subch] = C.words[1 + i]; C.binds.push_back({s->subch, C.words[1 + i]}); }
        }
    }
}

const u8* s_read(u32 loc, u32 off, u32 ea, const u8* bytes, u32 size, u32 kind)
{
    return record_read(loc, off, ea, bytes, size, kind);
}

void s_write(u32 loc, u32 off, u32 ea, const u8* bytes, u32 size, u32 kind)
{
    if (!C.in_step) fail("read-outside-event", "a guest write at ea " + hex8(ea) + " has no FIFO step to belong to");
    if (!size) return;
    Rd w{ea, loc, off, kind, size, C.consume_seq++, hex_of(bytes, size), true};
    store_blob(w.sha, bytes, size);
    C.writes.push_back(w);
}

void s_note(u32 what, u32, u32, u32)
{
    if (what == RSX_CAP_NOTE_RESYNC) C.resyncs++;
    else if (what == RSX_CAP_NOTE_UNKNOWN_METHOD) C.unknown++;
}

void s_step_cancel()
{
    if (!C.in_step) return;
    C.in_step = false;
    // A wait the walker retried leaves nothing behind, the read ordinals included: how many passes sat on an
    // ACQUIRE is host timing, and consume_seq is part of the capture hash.
    C.consume_seq = C.step_consume0;
    memcpy(C.bound, C.bound_before, sizeof C.bound);
    C.reads.clear(); C.writes.clear(); C.arena.clear(); g_read_copies.clear(); C.binds.clear();
}

void s_step_end()
{
    if (C.step.kind == ~0u) { C.step.kind = 0; return; }   // a coalesced zero word
    if (!C.in_step) return;
    C.in_step = false;
    const rsx_cap_step& s = C.step;
    char b[256];
    std::string f;
    snprintf(b, sizeof b, ",\"get\":\"%s\",\"put\":\"%s\",\"call_ret\":\"%s\"", hex8(s.get).c_str(),
             hex8(s.put).c_str(), hex8(s.call_ret).c_str());
    f = b;
    const char* ev = "fifo-word";
    if (s.kind == RSX_CAP_STEP_METHOD) {
        ev = "fifo-method";
        const u32 bound = C.bound_at_start;
        snprintf(b, sizeof b, ",\"subchannel\":%u,\"type\":%u,\"count\":%u,\"method\":\"0x%04x\",\"bound\":", s.subch,
                 s.incrementing ? 0u : 2u, s.count, C.words[0] & 0x1FFCu);
        f += b;
        f += bound ? "\"" + hex8(bound) + "\"" : std::string("null");
        f += ",\"binds\":[";
        for (size_t i = 0; i < C.binds.size(); i++) {
            if (i) f += ',';
            snprintf(b, sizeof b, "{\"subchannel\":%u,\"handle\":\"%s\"}", C.binds[i].first, hex8(C.binds[i].second).c_str());
            f += b;
        }
        f += "]";
    } else if (s.kind != RSX_CAP_STEP_WORD) {
        ev = "fifo-branch";
        const char* k = s.kind == RSX_CAP_STEP_JUMP ? "jump" : s.kind == RSX_CAP_STEP_CALL ? "call" : "ret";
        snprintf(b, sizeof b, ",\"kind\":\"%s\",\"target\":\"%s\"", k, hex8(s.target).c_str());
        f += b;
    }
    f += ",\"reads\":" + rd_array(C.reads) + ",\"writes\":" + rd_array(C.writes);
    emit(ev, f, true);
    C.reads.clear(); C.writes.clear(); C.arena.clear(); g_read_copies.clear();
    flush_outputs();
    for (const Frame& fr : C.out_frames) emit_frame(fr);
    C.out_frames.clear();
    if (C.finishing) detach_and_close();
}

void digest_state(ps3_sha256_ctx* h, const rsx_dispatch* r)
{
    auto feed = [&](const u32* p, size_t n) {
        u8 buf[4096];
        while (n) {
            const size_t k = n < 1024 ? n : 1024;
            for (size_t i = 0; i < k; i++) {
                buf[4 * i] = (u8)(p[i] >> 24); buf[4 * i + 1] = (u8)(p[i] >> 16);
                buf[4 * i + 2] = (u8)(p[i] >> 8); buf[4 * i + 3] = (u8)p[i];
            }
            ps3_sha256_update(h, buf, 4 * k);
            p += k; n -= k;
        }
    };
    feed(r->regs, RSX_DSP_NUM_REGS);
    feed(r->vp, RSX_DSP_VP_WORDS);
    feed(&r->constants[0][0], RSX_DSP_NUM_CONSTANTS * 4u);
    feed(C.bound, 8);
}

void s_draw(const rsx_dispatch* r, u32 executed, u32 reason, const char* name, u32 fixed)
{
    C.submitted++;
    if (executed) { C.executed++; if (fixed) C.fixed++; }
    else C.dropped[name]++;
    ps3_sha256_ctx h;
    ps3_sha256_init(&h);
    static const char tag[] = "ps3diff.rsx/1 draw-state/v1";
    ps3_sha256_update(&h, tag, sizeof tag);
    digest_state(&h, r);
    const std::string prog = C.program_sha.empty() ? std::string(64, '0') : C.program_sha;
    ps3_sha256_update(&h, prog.data(), prog.size());
    const u8 tail[2] = { (u8)executed, (u8)(executed ? 0 : reason) };
    ps3_sha256_update(&h, tail, 2);
    u8 d[32];
    ps3_sha256_final(&h, d);
    ps3_sha256_update(&C.chain, d, 32);
    std::string f = std::string(",\"outcome\":\"") + (executed ? "executed" : "dropped") + "\",\"reason\":" +
                    (executed ? std::string("null") : jstr(name)) + ",\"fixed_function\":" +
                    (executed && fixed ? "true" : "false") + ",\"state_hash\":" +
                    hash_obj("draw-state/v1", hex_digest(d));
    // Queue behind the step that triggered the draw, so a line never lands before its own input.
    if (C.in_step) {
        C.out_lines.push_back({"draw", f});
    } else {
        flush_nop();
        emit("draw", f, false);
    }
}

void s_frame(const rsx_cap_frame* fr)
{
    if (fr->readback == 1)
        fail("readback-format", "the presented surface (rsx_be_format " + std::to_string(fr->surface_format) +
                                ") has no readback format the capture defines");
    std::vector<std::string> invalid;
    u64 dropped = 0;
    for (auto& kv : C.dropped) dropped += kv.second;
    if (dropped) invalid.push_back("dropped-draws");
    if (C.fixed) invalid.push_back("fixed-function");
    if (C.resyncs) invalid.push_back("fifo-resync");
    if (fr->guest_frame == kNoFlip) invalid.push_back("no-queued-flip");
    if (C.unknown) invalid.push_back("unknown-methods");
    if (fr->readback == 2) invalid.push_back("readback-unavailable");
    if (fr->readback == 3) invalid.push_back("readback-incomplete");
    std::sort(invalid.begin(), invalid.end());

    // The frame's state hash: every draw's, then the register file as it stands at the flip.
    ps3_sha256_ctx h = C.chain;
    digest_state(&h, fr->rsx);
    u8 d[32];
    ps3_sha256_final(&h, d);
    ps3_sha256_init(&C.chain);
    static const char ctag[] = "ps3diff.rsx/1 frame-state/v1";
    ps3_sha256_update(&C.chain, ctag, sizeof ctag);

    Frame out;
    char b[320];
    snprintf(b, sizeof b, ",\"frame\":{\"flip_id\":%u,\"guest_frame\":", fr->flip_id);
    out.prefix = b;
    if (fr->guest_frame == kNoFlip) out.prefix += "null";
    else out.prefix += std::to_string(fr->guest_frame);
    snprintf(b, sizeof b, ",\"display_buffer\":%u,\"present_seq\":%u},\"capture_hash\":", fr->display_buffer, fr->present_seq);
    out.prefix += b;

    out.suffix = ",\"state_hash\":" + hash_obj("frame-state/v1", hex_digest(d)) + ",\"frame_hash\":";
    if (fr->readback == 0) {
        out.suffix += hash_obj("raw-readback/v1", hex_of(fr->bytes, (size_t)fr->row_pitch * fr->height));
        snprintf(b, sizeof b, ",\"readback\":{\"format\":\"%s\",\"channel_order\":\"%s\",\"row_pitch\":%u,"
                              "\"alpha\":\"kept\",\"colorspace\":\"%s\",\"clamp\":\"none\",\"width\":%u,\"height\":%u}",
                 fr->format, fr->channel_order, fr->row_pitch, fr->colorspace, fr->width, fr->height);
        out.suffix += b;
    } else {
        out.suffix += "null,\"readback\":null";
    }
    snprintf(b, sizeof b, ",\"draws\":{\"submitted\":%llu,\"executed\":%llu,\"dropped\":{",
             (unsigned long long)C.submitted, (unsigned long long)C.executed);
    out.suffix += b;
    bool first = true;
    for (auto& kv : C.dropped) {
        if (!first) out.suffix += ',';
        first = false;
        out.suffix += jstr(kv.first.c_str()) + ":" + std::to_string(kv.second);
    }
    snprintf(b, sizeof b, "},\"fixed_function\":%llu},\"unknown_methods\":%llu,\"resyncs\":%llu,\"valid\":%s,\"invalid\":[",
             (unsigned long long)C.fixed, (unsigned long long)C.unknown, (unsigned long long)C.resyncs,
             invalid.empty() ? "true" : "false");
    out.suffix += b;
    for (size_t i = 0; i < invalid.size(); i++) { if (i) out.suffix += ','; out.suffix += jstr(invalid[i].c_str()); }
    out.suffix += "]";

    C.submitted = C.executed = C.fixed = C.unknown = C.resyncs = 0;
    C.dropped.clear();
    C.frames++;
    if (C.in_step) C.out_frames.push_back(out);
    else { flush_nop(); emit_frame(out); }
    if (C.max_frames && C.frames >= C.max_frames) {
        C.finishing = true;
        if (!C.in_step) detach_and_close();
    }
}

void s_fail(const char* reason, const char* detail) { fail(reason, detail ? detail : ""); }

const rsx_capture_sink kSink = {
    s_gcm_init, s_io_map, s_display_buffer, s_step_begin, s_step_end, s_step_cancel, s_read, s_write, s_note,
    s_fail, s_draw, s_frame,
};

}  // namespace

extern "C" {

const char* rsx_capture_toolkit_sha(void) { return PS3RECOMP_TOOLKIT_SHA; }

const rsx_capture_environment* rsx_capture_environment_record(void) { return C.open ? &C.env_copy : nullptr; }

int rsx_capture_install(const rsx_capture_environment* env, const rsx_capture_options* opt)
{
    if (C.open) return 1;
    if (!opt || !opt->dir || !*opt->dir)
        fail("output", "no output directory");
    C.dir = opt->dir;
    while (C.dir.size() > 1 && C.dir.back() == '/') C.dir.pop_back();
    mkdir_p(C.dir);
    if (exists(C.dir + "/capture.ndjson") || exists(C.dir + "/capture.ndjson.partial"))
        fail("output-exists", C.dir + " already holds a capture");
    C.f = fopen((C.dir + "/capture.ndjson.partial").c_str(), "wb");
    if (!C.f) fail("io", "open " + C.dir + "/capture.ndjson.partial: " + strerror(errno));
    setvbuf(C.f, nullptr, _IOFBF, 1 << 20);
    C.max_frames = opt->max_frames;
    ps3_sha256_init(&C.in_hash);
    ps3_sha256_init(&C.chain);
    static const char ctag[] = "ps3diff.rsx/1 frame-state/v1";
    ps3_sha256_update(&C.chain, ctag, sizeof ctag);

    const char* src[8] = { env ? env->gpu : 0, env ? env->macos : 0, env ? env->metal : 0, env ? env->build : 0,
                           env ? env->glslang : 0, env ? env->spirv_cross : 0, env ? env->toolkit_sha : 0,
                           env ? env->title_sha : 0 };
    const char* dflt[8] = { "n/a", "n/a", "n/a", PS3RECOMP_BUILD_TYPE, "n/a", "n/a", PS3RECOMP_TOOLKIT_SHA, "n/a" };
    for (int i = 0; i < 8; i++) C.env_strs[i] = (src[i] && *src[i]) ? src[i] : dflt[i];
    C.env_copy = { C.env_strs[0].c_str(), C.env_strs[1].c_str(), C.env_strs[2].c_str(), C.env_strs[3].c_str(),
                   C.env_strs[4].c_str(), C.env_strs[5].c_str(), C.env_strs[6].c_str(), C.env_strs[7].c_str() };
    C.open = true;
    emit("capture-start", ",\"bootstrap\":{\"mode\":\"clean-reset\"},\"producer\":\"ps3recomp\"", true);
    emit("environment", ",\"machine\":" + machine_json(), false);
    cellGcm_set_capture_sink(&kSink);
    rsx_draw_engine_set_capture_sink(&kSink);
    static bool registered;
    if (!registered) { registered = true; atexit(rsx_capture_finish); }
    return 1;
}

int rsx_capture_install_from_config(const rsx_capture_environment* env)
{
    if (!ps3_cfg_video_rsx_capture()) return 0;
    const char* dir = ps3_cfg_video_rsx_capture_dir();
    if (!dir || !*dir) {
        const ps3_stop_field f[] = {
            {"key", "video.rsx_capture_dir"},
            {"detail", "video.rsx_capture is on and no directory was named"},
        };
        ps3_stop(PS3_STOP_CONFIG_INVALID, "video.rsx_capture needs video.rsx_capture_dir",
                 "Name an empty directory in video.rsx_capture_dir (docs/CONFIG.md), or turn capture off.",
                 f, 2, nullptr);
    }
    rsx_capture_options o = { dir, ps3_cfg_video_rsx_capture_frames() };
    return rsx_capture_install(env, &o);
}

void rsx_capture_finish(void)
{
    if (!C.open) return;
    if (C.in_step) C.finishing = true;
    else detach_and_close();
}

}  // extern "C"
