/*
 * ps3recomp - typed configuration (contract: include/ps3emu/config.h, format:
 * docs/CONFIG.md)
 *
 * Loading is strict and all-or-nothing: the first problem found stops the
 * process (config-invalid) naming where it is, so a configuration that loads is
 * one in which every key was understood.
 */

#define TOML_EXCEPTIONS 0                    /* parse errors come back as values */
#define TOML_ENABLE_FORMATTERS 0             /* the dump is written here */
#define TOML_ENABLE_UNRELEASED_FEATURES 0
#define TOML_ENABLE_WINDOWS_COMPAT 0
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#endif
#include "../../third_party/tomlplusplus/toml.hpp"
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

#include "ps3emu/config.h"
#include "ps3emu/stop.h"
#include "../ppu/ppu_context.h"   /* PPU_VSCR_NJ */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <stdlib.h>   /* _environ */
#elif defined(__APPLE__)
#  include <crt_externs.h>
#else
extern "C" char** environ;
#endif

namespace {

constexpr const char* kEnvPrefix = "PS3RECOMP_";
constexpr const char* kConfigEnv = "PS3RECOMP_CONFIG";
constexpr const char* kRemedy =
    "Correct the setting named above (docs/CONFIG.md lists every key with its "
    "type and range), or remove it to take the default.";

char** environment()
{
#if defined(_WIN32)
    return _environ;
#elif defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

/* ---- the registry, as data -------------------------------------------- */

struct Option {
    const char*     section;
    const char*     key;
    ps3_config_type type;
    bool            default_bool;
    const char*     default_str;
    size_t          max_length;
    int             validator;
    const char*     doc;
    size_t          offset;      /* of the member in ps3_config */
    uint32_t        default_u32;
    uint32_t        min_u32;
    uint32_t        max_u32;
    std::string     env;         /* PS3RECOMP_<SECTION>_<KEY> */
    std::string     default_text;
};

std::string upper(std::string s)
{
    for (char& c : s) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}

/* Built on first use so that a reader in a static initialiser sees it whatever
 * the translation-unit order. Never modified afterwards. */
const std::vector<Option>& options()
{
    static const std::vector<Option> table = [] {
        std::vector<Option> t;
#define PS3_CFG_BOOL(sec, k, def, doc_)                                                      \
        t.push_back({#sec, #k, PS3_CFG_TYPE_BOOL, (def) != 0, "", 0, PS3_CFG_V_NONE, doc_,   \
                     offsetof(ps3_config, sec##_##k), 0u, 0u, 0u, {}, {}});
#define PS3_CFG_U32(sec, k, def, lo, hi, doc_)                                               \
        t.push_back({#sec, #k, PS3_CFG_TYPE_U32, false, "", 0, PS3_CFG_V_NONE, doc_,         \
                     offsetof(ps3_config, sec##_##k), (uint32_t)(def), (uint32_t)(lo),       \
                     (uint32_t)(hi), {}, {}});
#define PS3_CFG_STR(sec, k, def, max_length, validator, doc_)                                \
        t.push_back({#sec, #k, PS3_CFG_TYPE_STR, false, def, max_length, validator, doc_,    \
                     offsetof(ps3_config, sec##_##k), 0u, 0u, 0u, {}, {}});
#include "ps3emu/config_options.def"
#undef PS3_CFG_BOOL
#undef PS3_CFG_U32
#undef PS3_CFG_STR
        for (Option& o : t) {
            o.env = std::string(kEnvPrefix) + upper(o.section) + "_" + upper(o.key);
            if (o.type == PS3_CFG_TYPE_BOOL) o.default_text = o.default_bool ? "true" : "false";
            else if (o.type == PS3_CFG_TYPE_U32) o.default_text = std::to_string(o.default_u32);
            else o.default_text = o.default_str;
        }
        return t;
    }();
    return table;
}

const std::vector<ps3_config_option>& public_options()
{
    static const std::vector<ps3_config_option> table = [] {
        std::vector<ps3_config_option> t;
        for (const Option& o : options())
            t.push_back({o.section, o.key, o.type, o.default_text.c_str(), o.env.c_str(), o.doc});
        return t;
    }();
    return table;
}

bool is_legacy_env(const std::string& name)
{
    static const char* const legacy[] = {
#define PS3_CFG_LEGACY_ENV(n) n,
#include "ps3emu/config_options.def"
#undef PS3_CFG_LEGACY_ENV
    };
    for (const char* l : legacy)
        if (name == l) return true;
    return false;
}

/* ---- the loaded state --------------------------------------------------- */

struct State {
    ps3_config                cfg;
    std::vector<std::string>  source;      /* per option: "default" | "file:.." | "env:.." */
    std::string               file;        /* empty: no file */
    std::deque<std::string>   reasons;     /* stable storage for stub reason text */
    std::vector<ps3_cfg_hle_stub> hle;     /* sorted by nid */
    std::vector<ps3_cfg_lv2_stub> lv2;     /* sorted by num */
};

std::mutex g_load_lock;
std::atomic<State*> g_published{nullptr};   /* set once, release; never reset */

/* ---- stopping ----------------------------------------------------------- */

struct Where {
    const char* file = nullptr;
    uint32_t    line = 0;
    uint32_t    col  = 0;
    const char* env  = nullptr;
};

[[noreturn]] void invalid(const Where& w, const std::string& key, const std::string& detail)
{
    std::string line_s = std::to_string(w.line), col_s = std::to_string(w.col);
    ps3_stop_field f[6];
    size_t n = 0;
    if (w.file) f[n++] = {"file", w.file};
    if (w.file && w.line) f[n++] = {"line", line_s.c_str()};
    if (w.file && w.line && w.col) f[n++] = {"col", col_s.c_str()};
    if (w.env) f[n++] = {"env", w.env};
    if (!key.empty()) f[n++] = {"key", key.c_str()};
    f[n++] = {"detail", detail.c_str()};

    std::string what = "invalid configuration: ";
    if (w.file) {
        what += w.file;
        if (w.line) what += ":" + line_s;
        what += ": ";
    }
    if (w.env) what += std::string(w.env) + ": ";
    if (!key.empty()) what += "key '" + key + "': ";
    what += detail;
    ps3_stop(PS3_STOP_CONFIG_INVALID, what.c_str(), kRemedy, f, n, nullptr);
}

size_t edit_distance(const std::string& a, const std::string& b)
{
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1])});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

/* " (did you mean 'x'?)" when something close exists. */
std::string suggestion(const std::string& got, const std::vector<std::string>& candidates)
{
    const std::string* best = nullptr;
    size_t best_d = 3;
    for (const std::string& c : candidates) {
        size_t d = edit_distance(got, c);
        if (d < best_d) { best_d = d; best = &c; }
    }
    return best ? " (did you mean '" + *best + "'?)" : std::string();
}

/* ---- applying one value ------------------------------------------------- */

const Option* find_option(const std::string& section, const std::string& key)
{
    for (const Option& o : options())
        if (section == o.section && key == o.key) return &o;
    return nullptr;
}

size_t option_index(const Option* o) { return size_t(o - options().data()); }

/* Checks `s` against the option's length and validator; normalises it. Returns
 * an empty string when fine, otherwise what is wrong. */
std::string check_string(const Option& o, std::string& s)
{
    if (s.size() > o.max_length)
        return "longer than the " + std::to_string(o.max_length) + " characters allowed";
    switch (o.validator) {
    case PS3_CFG_V_SHA256_OR_EMPTY:
        if (s.empty()) return {};
        if (s.size() != 64)
            return "expected 64 hex digits (a SHA-256), found " + std::to_string(s.size()) + " characters";
        for (char& c : s) {
            if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a');
            else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return "expected 64 hex digits (a SHA-256), found a non-hex character";
        }
        return {};
    case PS3_CFG_V_VIDEO_RESOLUTIONS: {
        /* The cellVideoOut resolution ids (libs/video/cellVideoOut.h). */
        static const unsigned kIds[] = {1, 2, 4, 5, 10, 11, 12, 13};
        if (s.empty()) return {};
        bool want[14] = {false};
        size_t pos = 0;
        for (;;) {
            size_t end = s.find(',', pos);
            std::string tok = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
            while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
            bool ok = !tok.empty() && tok.size() <= 2;
            unsigned v = 0;
            for (char c : tok) {
                if (c < '0' || c > '9') { ok = false; break; }
                v = v * 10 + unsigned(c - '0');
            }
            bool known = false;
            for (unsigned id : kIds) known = known || (ok && id == v);
            if (!known)
                return "'" + tok + "' is not a resolution id; expected a comma-separated list from "
                       "1 (1080), 2 (720), 4 (480), 5 (576), 10, 11, 12, 13";
            want[v] = true;
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        std::string norm;
        for (unsigned id : kIds)
            if (want[id]) norm += (norm.empty() ? "" : ",") + std::to_string(id);
        s = norm;
        return {};
    }
    default:
        return {};
    }
}

void store_bool(State& st, const Option& o, bool v, const std::string& source)
{
    *reinterpret_cast<bool*>(reinterpret_cast<char*>(&st.cfg) + o.offset) = v;
    st.source[option_index(&o)] = source;
}

void store_u32(State& st, const Option& o, uint32_t v, const std::string& source)
{
    *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(&st.cfg) + o.offset) = v;
    st.source[option_index(&o)] = source;
}

void store_string(State& st, const Option& o, const std::string& v, const std::string& source)
{
    char* dst = reinterpret_cast<char*>(&st.cfg) + o.offset;
    std::memcpy(dst, v.c_str(), v.size() + 1);   /* check_string bounded it */
    st.source[option_index(&o)] = source;
}

void set_defaults(State& st)
{
    std::memset(&st.cfg, 0, sizeof st.cfg);
    st.source.assign(options().size(), "default");
    for (const Option& o : options()) {
        char* base = reinterpret_cast<char*>(&st.cfg) + o.offset;
        switch (o.type) {
        case PS3_CFG_TYPE_BOOL: *reinterpret_cast<bool*>(base) = o.default_bool; break;
        case PS3_CFG_TYPE_U32:  *reinterpret_cast<uint32_t*>(base) = o.default_u32; break;
        case PS3_CFG_TYPE_STR:
            std::memcpy(base, o.default_str, std::strlen(o.default_str) + 1);
            break;
        }
    }
}

/* ---- the file ----------------------------------------------------------- */

const char* type_name(toml::node_type t)
{
    switch (t) {
    case toml::node_type::table:          return "table";
    case toml::node_type::array:          return "array";
    case toml::node_type::string:         return "string";
    case toml::node_type::integer:        return "integer";
    case toml::node_type::floating_point: return "float";
    case toml::node_type::boolean:        return "boolean";
    case toml::node_type::date:           return "date";
    case toml::node_type::time:           return "time";
    case toml::node_type::date_time:      return "date-time";
    default:                              return "nothing";
    }
}

/* The position of a node (a value, table or array) or of a key. */
template <class NodeOrKey>
Where at(const std::string& file, const NodeOrKey& n)
{
    Where w;
    w.file = file.c_str();
    w.line = n.source().begin.line;
    w.col  = n.source().begin.column;
    return w;
}

std::vector<std::string> section_names()
{
    std::vector<std::string> s;
    for (const Option& o : options())
        if (std::find(s.begin(), s.end(), o.section) == s.end()) s.push_back(o.section);
    return s;
}

std::vector<std::string> keys_of(const std::string& section)
{
    std::vector<std::string> k;
    for (const Option& o : options())
        if (section == o.section) k.push_back(o.key);
    return k;
}

void read_option_section(State& st, const std::string& file, const std::string& section,
                         const toml::table& tbl)
{
    for (auto&& [k, v] : tbl) {
        const std::string key(k.str());
        const std::string path = section + "." + key;
        const Option* o = find_option(section, key);
        if (!o)
            invalid(at(file, k), path, "unknown key" + suggestion(key, keys_of(section)));
        const std::string source = "file:" + file;
        if (o->type == PS3_CFG_TYPE_BOOL) {
            const toml::value<bool>* b = v.as_boolean();
            if (!b)
                invalid(at(file, v), path, std::string("expected boolean, found ") + type_name(v.type()));
            store_bool(st, *o, b->get(), source);
        } else if (o->type == PS3_CFG_TYPE_U32) {
            const toml::value<int64_t>* i = v.as_integer();
            if (!i)
                invalid(at(file, v), path, std::string("expected integer, found ") + type_name(v.type()));
            const int64_t n = i->get();
            /* A negative value is below every minimum (all are >= 0), so one
             * range check reports both "too small" and "negative". */
            if (n < (int64_t)o->min_u32 || n > (int64_t)o->max_u32)
                invalid(at(file, v), path,
                        "out of range: " + std::to_string(n) + " is not within " +
                        std::to_string(o->min_u32) + ".." + std::to_string(o->max_u32));
            store_u32(st, *o, uint32_t(n), source);
        } else {
            const toml::value<std::string>* s = v.as_string();
            if (!s)
                invalid(at(file, v), path, std::string("expected string, found ") + type_name(v.type()));
            std::string value = s->get();
            std::string why = check_string(*o, value);
            if (!why.empty()) invalid(at(file, v), path, why);
            store_string(st, *o, value, source);
        }
    }
}

/* A required integer field of a stub entry, range-checked. */
int64_t stub_int(const std::string& file, const toml::table& e, const std::string& entry,
                 const char* field, int64_t lo, int64_t hi)
{
    const std::string path = entry + "." + field;
    const toml::node* n = e.get(field);
    if (!n) invalid(at(file, e), path, "missing required field");
    const toml::value<int64_t>* i = n->as_integer();
    if (!i) invalid(at(file, *n), path, std::string("expected integer, found ") + type_name(n->type()));
    if (i->get() < lo || i->get() > hi)
        invalid(at(file, *n), path, "out of range: " + std::to_string(i->get()) + " is not within " +
                                    std::to_string(lo) + ".." + std::to_string(hi));
    return i->get();
}

std::string stub_reason(const std::string& file, const toml::table& e, const std::string& entry)
{
    const std::string path = entry + ".reason";
    const toml::node* n = e.get("reason");
    if (!n) invalid(at(file, e), path, "missing required field");
    const toml::value<std::string>* s = n->as_string();
    if (!s) invalid(at(file, *n), path, std::string("expected string, found ") + type_name(n->type()));
    const std::string& r = s->get();
    if (r.find_first_not_of(" \t\r\n") == std::string::npos)
        invalid(at(file, *n), path, "reason is blank; say why the gap may be skipped");
    return r;
}

void reject_other_fields(const std::string& file, const toml::table& e, const std::string& entry,
                         std::initializer_list<const char*> known)
{
    for (auto&& [k, v] : e) {
        bool ok = false;
        for (const char* f : known) ok = ok || k.str() == f;
        if (!ok) {
            std::vector<std::string> cand(known.begin(), known.end());
            invalid(at(file, k), entry + "." + std::string(k.str()),
                    "unknown field" + suggestion(std::string(k.str()), cand));
        }
    }
}

/* [[hle.stub]] / [[lv2.stub]]: `kind` is "hle" or "lv2". */
void read_stubs(State& st, const std::string& file, const std::string& kind, const toml::node& n)
{
    const toml::table* t = n.as_table();
    if (!t)
        invalid(at(file, n), kind, std::string("expected a table holding [[") + kind +
                                   ".stub]], found " + type_name(n.type()));
    const bool hle = kind == "hle";
    std::map<uint32_t, uint32_t> seen;   /* nid/num -> line of its first entry */
    for (auto&& [k, v] : *t) {
        if (k.str() != "stub")
            invalid(at(file, k), kind + "." + std::string(k.str()),
                    "unknown key" + suggestion(std::string(k.str()), {"stub"}));
        const toml::array* arr = v.as_array();
        if (!arr || !arr->is_array_of_tables())
            invalid(at(file, v), kind + ".stub",
                    std::string("expected an array of tables ([[") + kind + ".stub]], found " + type_name(v.type()));
        size_t idx = 0;
        for (auto&& el : *arr) {
            const toml::table& e = *el.as_table();
            const std::string entry = kind + ".stub[" + std::to_string(idx++) + "]";
            if (hle) reject_other_fields(file, e, entry, {"nid", "ret", "reason"});
            else     reject_other_fields(file, e, entry, {"num", "ret", "reason"});
            const char* idname = hle ? "nid" : "num";
            const uint32_t id = uint32_t(stub_int(file, e, entry, idname, 0, hle ? 0xFFFFFFFFll : 1023));
            const uint32_t ret = uint32_t(stub_int(file, e, entry, "ret", -0x80000000ll, 0xFFFFFFFFll));
            std::string reason = stub_reason(file, e, entry);
            const uint32_t line = e.source().begin.line;
            auto dup = seen.emplace(id, line);
            if (!dup.second)
                invalid(at(file, e), entry + "." + idname,
                        std::string("duplicate: ") + idname + " " + std::to_string(id) +
                        " is already allow-listed at line " + std::to_string(dup.first->second));
            st.reasons.push_back(std::move(reason));
            if (hle) st.hle.push_back({id, ret, st.reasons.back().c_str(), line});
            else     st.lv2.push_back({id, ret, st.reasons.back().c_str(), line});
        }
    }
}

void read_file(State& st, const std::string& path)
{
    st.file = path;
    toml::parse_result parsed = toml::parse_file(path);
    if (!parsed) {
        Where w;
        w.file = path.c_str();
        w.line = parsed.error().source().begin.line;
        w.col  = parsed.error().source().begin.column;
        invalid(w, "", std::string("cannot read as TOML: ") + std::string(parsed.error().description()));
    }
    const toml::table& root = parsed.table();

    Where whole;
    whole.file = path.c_str();
    const toml::node* schema = root.get("schema");
    if (!schema)
        invalid(whole, "schema", "missing; the file must begin with `schema = " +
                                 std::to_string(PS3_CONFIG_SCHEMA_VERSION) + "`");
    const toml::value<int64_t>* sv = schema->as_integer();
    if (!sv)
        invalid(at(path, *schema), "schema", std::string("expected integer, found ") + type_name(schema->type()));
    if (sv->get() != PS3_CONFIG_SCHEMA_VERSION)
        invalid(at(path, *schema), "schema",
                "unsupported schema " + std::to_string(sv->get()) + "; this runtime reads schema " +
                std::to_string(PS3_CONFIG_SCHEMA_VERSION));

    const std::vector<std::string> sections = section_names();
    for (auto&& [k, v] : root) {
        const std::string key(k.str());
        if (key == "schema") continue;
        if (key == "hle" || key == "lv2") { read_stubs(st, path, key, v); continue; }
        if (std::find(sections.begin(), sections.end(), key) == sections.end()) {
            std::vector<std::string> cand = sections;
            cand.insert(cand.end(), {"hle", "lv2", "schema"});
            invalid(at(path, k), key, "unknown section or key" + suggestion(key, cand));
        }
        const toml::table* tbl = v.as_table();
        if (!tbl)
            invalid(at(path, v), key, std::string("expected a [") + key + "] table, found " + type_name(v.type()));
        read_option_section(st, path, key, *tbl);
    }
}

/* ---- the environment ---------------------------------------------------- */

bool parse_env_bool(const std::string& v, bool& out)
{
    std::string l = v;
    for (char& c : l) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (l == "1" || l == "true")  { out = true;  return true; }
    if (l == "0" || l == "false") { out = false; return true; }
    return false;
}

/* A u32 environment value: decimal, or 0x/0X followed by hex digits. No sign,
 * no whitespace, no trailing text: an operator who writes "-1" or "300ms" made
 * a mistake the stop should name rather than wrap into a large number. */
bool parse_env_u32(const std::string& v, uint64_t& out)
{
    if (v.empty()) return false;
    size_t i = 0, base = 10;
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) { base = 16; i = 2; }
    uint64_t n = 0;
    for (; i < v.size(); ++i) {
        const char c = v[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = unsigned(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = unsigned(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = unsigned(c - 'A' + 10);
        else return false;
        if (n > (UINT64_MAX - d) / base) return false;
        n = n * base + d;
    }
    out = n;
    return true;
}

void read_environment(State& st)
{
    std::vector<std::pair<std::string, std::string>> vars;   /* PS3RECOMP_* only */
    for (char** e = environment(); e && *e; ++e) {
        if (std::strncmp(*e, kEnvPrefix, std::strlen(kEnvPrefix)) != 0) continue;
        const char* eq = std::strchr(*e, '=');
        if (!eq) continue;
        vars.emplace_back(std::string(*e, size_t(eq - *e)), std::string(eq + 1));
    }
    std::sort(vars.begin(), vars.end());   /* the first problem reported is deterministic */

    std::vector<std::string> names;
    for (const Option& o : options()) names.push_back(o.env);
    names.push_back(kConfigEnv);

    for (const auto& [name, value] : vars) {
        if (name == kConfigEnv || is_legacy_env(name)) continue;
        const Option* opt = nullptr;
        for (const Option& o : options()) if (o.env == name) opt = &o;
        Where w;
        w.env = name.c_str();
        if (!opt)
            invalid(w, "", "not a ps3recomp option; PS3RECOMP_* names are reserved for configuration" +
                           suggestion(name, names));
        const std::string key = std::string(opt->section) + "." + opt->key;
        const std::string source = "env:" + name;
        if (opt->type == PS3_CFG_TYPE_BOOL) {
            bool b = false;
            if (!parse_env_bool(value, b))
                invalid(w, key, "expected 1, 0, true or false, found '" + value + "'");
            store_bool(st, *opt, b, source);
        } else if (opt->type == PS3_CFG_TYPE_U32) {
            uint64_t n = 0;
            if (!parse_env_u32(value, n))
                invalid(w, key, "expected an unsigned decimal or 0x hexadecimal integer, found '" +
                                value + "'");
            if (n < opt->min_u32 || n > opt->max_u32)
                invalid(w, key, "out of range: " + std::to_string(n) + " is not within " +
                                std::to_string(opt->min_u32) + ".." + std::to_string(opt->max_u32));
            store_u32(st, *opt, uint32_t(n), source);
        } else {
            std::string s = value;
            std::string why = check_string(*opt, s);
            if (!why.empty()) invalid(w, key, why);
            store_string(st, *opt, s, source);
        }
    }
}

/* ---- the dump ----------------------------------------------------------- */

void toml_string(std::string& out, const std::string& s)
{
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20 || c == 0x7F) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04X", c);
                out += b;
            } else {
                out += char(c);
            }
        }
    }
    out += '"';
}

std::string hex32(uint32_t v)
{
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

std::string dump_string(const State& st)
{
    std::string out = "# ps3recomp effective configuration\n# config file: ";
    out += st.file.empty() ? std::string("none") : st.file;
    out += "\nschema = " + std::to_string(PS3_CONFIG_SCHEMA_VERSION) + "\n";

    std::string current;
    for (size_t i = 0; i < options().size(); ++i) {
        const Option& o = options()[i];
        if (current != o.section) {
            current = o.section;
            out += "\n[" + current + "]\n";
        }
        out += std::string(o.key) + " = ";
        const char* base = reinterpret_cast<const char*>(&st.cfg) + o.offset;
        if (o.type == PS3_CFG_TYPE_BOOL) out += *reinterpret_cast<const bool*>(base) ? "true" : "false";
        else if (o.type == PS3_CFG_TYPE_U32) out += std::to_string(*reinterpret_cast<const uint32_t*>(base));
        else toml_string(out, base);
        out += "  # source: " + st.source[i] + "\n";
    }
    const std::string where = st.file.empty() ? std::string() : st.file;
    for (const ps3_cfg_hle_stub& s : st.hle) {
        out += "\n# source: file:" + where + ":" + std::to_string(s.line) + "\n[[hle.stub]]\n";
        out += "nid = " + hex32(s.nid) + "\nret = " + hex32(s.ret) + "\nreason = ";
        toml_string(out, s.reason);
        out += "\n";
    }
    for (const ps3_cfg_lv2_stub& s : st.lv2) {
        out += "\n# source: file:" + where + ":" + std::to_string(s.line) + "\n[[lv2.stub]]\n";
        out += "num = " + std::to_string(s.num) + "\nret = " + hex32(s.ret) + "\nreason = ";
        toml_string(out, s.reason);
        out += "\n";
    }
    return out;
}

/* ---- loading ------------------------------------------------------------ */

/* Caller holds g_load_lock and has seen g_published == nullptr. */
State* load_locked(const char* title_path)
{
    auto st = std::make_unique<State>();
    set_defaults(*st);

    /* The environment beats the path the title built in: both name a file, and
     * the operator's choice of file is the more specific one. */
    std::string path;
    if (const char* env = std::getenv(kConfigEnv)) {
        Where w;
        w.env = kConfigEnv;
        if (!*env) invalid(w, "", "is set but empty; name a config file or unset it");
        path = env;
    } else if (title_path && *title_path) {
        path = title_path;
    }
    if (!path.empty()) read_file(*st, path);
    read_environment(*st);

    if (st->cfg.diag_dump_config) {
        const std::string text = dump_string(*st);
        std::fwrite(text.data(), 1, text.size(), stderr);
        std::fflush(stderr);
    }

    std::sort(st->hle.begin(), st->hle.end(),
              [](const ps3_cfg_hle_stub& a, const ps3_cfg_hle_stub& b) { return a.nid < b.nid; });
    std::sort(st->lv2.begin(), st->lv2.end(),
              [](const ps3_cfg_lv2_stub& a, const ps3_cfg_lv2_stub& b) { return a.num < b.num; });

    State* published = st.release();   /* intentionally never freed: valid for the process */
    g_published.store(published, std::memory_order_release);
    return published;
}

const State& loaded()
{
    ps3_config_get();
    return *g_published.load(std::memory_order_acquire);
}

} // namespace

extern "C" {

const ps3_config* ps3_config_get(void)
{
    State* s = g_published.load(std::memory_order_acquire);
    if (s) return &s->cfg;
    std::lock_guard<std::mutex> lock(g_load_lock);
    s = g_published.load(std::memory_order_relaxed);   /* another thread may have won */
    if (!s) s = load_locked(nullptr);
    return &s->cfg;
}

void ps3_config_load(const char* path)
{
    std::lock_guard<std::mutex> lock(g_load_lock);
    if (g_published.load(std::memory_order_relaxed))
        invalid(Where{}, "", "ps3_config_load() was called after the configuration was already "
                             "loaded or in use; it must run first, before the executable is loaded");
    load_locked(path);
}

const char* ps3_config_file(void)
{
    const State& s = loaded();
    return s.file.empty() ? nullptr : s.file.c_str();
}

size_t ps3_config_option_count(void) { return options().size(); }

const ps3_config_option* ps3_config_option_at(size_t index)
{
    return index < public_options().size() ? &public_options()[index] : nullptr;
}

const char* ps3_config_option_source(size_t index)
{
    const State& s = loaded();
    return index < s.source.size() ? s.source[index].c_str() : nullptr;
}

void ps3_config_dump(FILE* out)
{
    const std::string text = dump_string(loaded());
    std::fwrite(text.data(), 1, text.size(), out);
    std::fflush(out);
}

const ps3_cfg_hle_stub* ps3_config_find_hle_stub(uint32_t nid)
{
    const auto& v = loaded().hle;
    auto it = std::lower_bound(v.begin(), v.end(), nid,
                               [](const ps3_cfg_hle_stub& s, uint32_t n) { return s.nid < n; });
    return it != v.end() && it->nid == nid ? &*it : nullptr;
}

const ps3_cfg_lv2_stub* ps3_config_find_lv2_stub(uint32_t num)
{
    const auto& v = loaded().lv2;
    auto it = std::lower_bound(v.begin(), v.end(), num,
                               [](const ps3_cfg_lv2_stub& s, uint32_t n) { return s.num < n; });
    return it != v.end() && it->num == num ? &*it : nullptr;
}

uint32_t ps3_ppu_initial_vscr(void)
{
    return ps3_cfg_ppu_vscr_nj() ? PPU_VSCR_NJ : 0u;
}

} // extern "C"
