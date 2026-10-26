/* A minimal reader/writer for the tagged message container the firmware's sysutil client library
 * (libsysutil) uses to talk to the system process over its shared-memory rings. It exists so a
 * fake peer can answer the REAL firmware decoder with wire-accurate bytes; nothing here is a
 * description of any one command, and it models only the subset the lifted client reads and
 * writes (named int / string / blob elements below one root node).
 *
 * Layout (all big-endian), established from the firmware's own serializer, decoder and element
 * accessors (docs/DIFF_HARNESS.md, "Savedata: the IPC"):
 *   header   0x38 bytes: a 4-character message-group tag ("SAVE" for savedata), u32 version (0x110),
 *            then six (offset, length) section pairs;
 *            every section starts on a 16-byte boundary after the header
 *   sec0     element tree: root node {u32 name, u32 count, 5 x u32 -1} (0x1C bytes) followed by
 *            `count` entries of 0x10 bytes {u32 name, u32 type, u32 value, u32 aux}
 *   sec2     NUL-separated name/string table; `name`, and a string's `value`, are offsets into it
 *   sec5     blob bytes; a blob entry's value/aux are {offset, size} into it
 *   types    1 = int (value), 3 = string ({offset, length}), 6 = blob ({offset, size})
 * sec1, sec3 and sec4 are always empty for the messages the savedata client exchanges. */
#ifndef PS3RECOMP_DH_CXML_H
#define PS3RECOMP_DH_CXML_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dh { namespace cxml {

enum : uint32_t { T_INT = 1, T_STR = 3, T_BLOB = 6 };

struct Elem {
    std::string name;
    uint32_t type = T_INT;
    uint32_t ival = 0;                 /* T_INT */
    std::string sval;                  /* T_STR */
    std::vector<uint8_t> blob;         /* T_BLOB */
    uint32_t raw_value = 0, raw_aux = 0;   /* as found on the wire (parsed messages) */
};
struct Msg {
    std::string tag = "SAVE";          /* the header's first word: the message group */
    std::string root;
    std::vector<Elem> elems;
    const Elem* find(const char* n) const { for (auto& e : elems) if (e.name == n) return &e; return nullptr; }
    Msg& add_int(const char* n, uint32_t v) { Elem e; e.name = n; e.type = T_INT; e.ival = v; elems.push_back(e); return *this; }
    Msg& add_str(const char* n, const std::string& s) { Elem e; e.name = n; e.type = T_STR; e.sval = s; elems.push_back(e); return *this; }
    Msg& add_blob(const char* n, const std::vector<uint8_t>& b) { Elem e; e.name = n; e.type = T_BLOB; e.blob = b; elems.push_back(e); return *this; }
};

inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline void put32(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
inline size_t pad16(size_t n) { return (n + 15) & ~(size_t)15; }

/* Build the wire bytes of a message. */
inline std::vector<uint8_t> build(const Msg& m)
{
    std::string strs;
    auto intern = [&](const std::string& s) -> uint32_t {
        for (size_t i = 0; i < strs.size();) {
            const char* p = strs.c_str() + i;
            if (s == p) return (uint32_t)i;
            i += strlen(p) + 1;
        }
        uint32_t off = (uint32_t)strs.size();
        strs += s; strs.push_back('\0');
        return off;
    };
    std::vector<uint8_t> blobs, tree;
    put32(tree, intern(m.root)); put32(tree, (uint32_t)m.elems.size());
    for (int i = 0; i < 5; ++i) put32(tree, 0xFFFFFFFFu);
    for (auto& e : m.elems) {
        put32(tree, intern(e.name)); put32(tree, e.type);
        if (e.type == T_INT)       { put32(tree, e.ival); put32(tree, 0); }
        else if (e.type == T_STR)  { put32(tree, intern(e.sval)); put32(tree, (uint32_t)e.sval.size()); }
        else {
            put32(tree, (uint32_t)blobs.size()); put32(tree, (uint32_t)e.blob.size());
            blobs.insert(blobs.end(), e.blob.begin(), e.blob.end());
            blobs.resize(pad16(blobs.size()), 0);
        }
    }
    const size_t len[6] = {tree.size(), 0, strs.size(), 0, 0, blobs.size()};
    size_t off[6]; size_t at = pad16(0x38);
    for (int i = 0; i < 6; ++i) { off[i] = at; at += pad16(len[i]); }
    std::vector<uint8_t> out;
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(i < (int)m.tag.size() ? m.tag[i] : 0));
    put32(out, 0x110);
    for (int i = 0; i < 6; ++i) { put32(out, (uint32_t)off[i]); put32(out, (uint32_t)len[i]); }
    out.resize(at, 0);
    if (len[0]) memcpy(&out[off[0]], tree.data(), len[0]);
    if (len[2]) memcpy(&out[off[2]], strs.data(), len[2]);
    if (len[5]) memcpy(&out[off[5]], blobs.data(), len[5]);
    return out;
}

/* Parse wire bytes. False when the bytes are not a well-formed message of the subset above
 * (the reason is left in `why`); never reads outside [p, p+n). */
inline bool parse(const uint8_t* p, size_t n, Msg& m, std::string& why)
{
    if (n < 0x38) { why = "shorter than the header"; return false; }
    m.tag.assign((const char*)p, 4);
    const uint32_t ver = be32(p + 4);
    if (ver != 0x110 && ver != 0x100) { why = "unknown version"; return false; }
    uint32_t off[6], len[6];
    for (int i = 0; i < 6; ++i) { off[i] = be32(p + 8 + 8 * i); len[i] = be32(p + 12 + 8 * i); }
    for (int i = 0; i < 6; ++i)
        if (len[i] && ((uint64_t)off[i] + len[i] > n)) { why = "section outside the message"; return false; }
    if (len[0] < 0x1C || len[2] == 0 || p[off[2] + len[2] - 1] != 0) { why = "bad tree or string table"; return false; }
    const uint8_t* tree = p + off[0]; const char* strs = (const char*)p + off[2];
    auto str_at = [&](uint32_t o, std::string& s) { if (o >= len[2]) return false; s = strs + o; return true; };
    if (!str_at(be32(tree), m.root)) { why = "root name"; return false; }
    const uint32_t count = be32(tree + 4);
    if ((uint64_t)count * 0x10 + 0x1C > len[0]) { why = "entry table"; return false; }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = tree + 0x1C + 0x10 * i;
        Elem el;
        if (be32(e) == 0xFFFFFFFFu) continue;    /* a slot the sender reserved and did not fill */
        if (!str_at(be32(e), el.name)) { why = "entry name"; return false; }
        el.type = be32(e + 4); el.raw_value = be32(e + 8); el.raw_aux = be32(e + 12);
        if (el.type == T_INT) el.ival = el.raw_value;
        else if (el.type == T_STR) { if (!str_at(el.raw_value, el.sval)) { why = "string value"; return false; } }
        else if (el.type == T_BLOB) {
            if ((uint64_t)el.raw_value + el.raw_aux > len[5]) { why = "blob outside section"; return false; }
            el.blob.assign(p + off[5] + el.raw_value, p + off[5] + el.raw_value + el.raw_aux);
        }
        m.elems.push_back(std::move(el));
    }
    return true;
}

}}  // namespace dh::cxml
#endif
