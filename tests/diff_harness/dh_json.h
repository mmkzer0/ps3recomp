/* A small JSON writer: the differential report is the only consumer.
 *
 * Hand-built JSON hides escaping bugs until a case id or a stop detail carries
 * a quote; one writer owns the escaping, the comma and the indentation. */
#ifndef PS3RECOMP_DH_JSON_H
#define PS3RECOMP_DH_JSON_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dh {

class JsonWriter {
public:
    explicit JsonWriter(bool pretty = true) : pretty_(pretty) {}

    void begin_object() { open('{'); }
    void end_object() { close('}'); }
    void begin_array() { open('['); }
    void end_array() { close(']'); }

    void key(const std::string& k)
    {
        separators();
        string(k);
        out_ += pretty_ ? ": " : ":";
        after_key_ = true;
    }
    void value(const std::string& s) { separators(); string(s); }
    void value(const char* s) { separators(); string(std::string(s)); }
    void value(uint64_t v) { separators(); out_ += std::to_string(v); }
    void value(int64_t v) { separators(); out_ += std::to_string(v); }
    void value(int v) { separators(); out_ += std::to_string(v); }
    void value(bool v) { separators(); out_ += v ? "true" : "false"; }
    void null() { separators(); out_ += "null"; }

    void field(const std::string& k, const std::string& v) { key(k); value(v); }
    void field(const std::string& k, const char* v) { key(k); value(v); }
    void field(const std::string& k, uint64_t v) { key(k); value(v); }
    void field(const std::string& k, int64_t v) { key(k); value(v); }
    void field(const std::string& k, int v) { key(k); value(v); }
    void field(const std::string& k, bool v) { key(k); value(v); }

    const std::string& str() const { return out_; }

    static std::string hex(uint64_t v)
    {
        char b[24];
        std::snprintf(b, sizeof b, "0x%llX", (unsigned long long)v);
        return b;
    }

private:
    void open(char c)
    {
        separators();
        out_ += c;
        stack_.push_back(c);
        first_.push_back(true);
        after_key_ = false;
    }
    void close(char c)
    {
        const bool empty = first_.back();
        const char open_c = stack_.back();
        stack_.pop_back();
        first_.pop_back();
        if (pretty_ && !empty) newline();
        out_ += c;
        after_key_ = false;
        (void)open_c;
    }
    void separators()
    {
        if (after_key_) { after_key_ = false; return; }
        if (stack_.empty()) return;
        if (!first_.back()) out_ += ',';
        first_.back() = false;
        newline();
    }
    void newline()
    {
        if (!pretty_) return;
        out_ += '\n';
        out_.append(stack_.size() * 2, ' ');
    }
    void string(const std::string& s)
    {
        out_ += '"';
        for (unsigned char c : s) {
            switch (c) {
            case '"': out_ += "\\\""; break;
            case '\\': out_ += "\\\\"; break;
            case '\b': out_ += "\\b"; break;
            case '\f': out_ += "\\f"; break;
            case '\n': out_ += "\\n"; break;
            case '\r': out_ += "\\r"; break;
            case '\t': out_ += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04X", c);
                    out_ += b;
                } else {
                    out_ += (char)c;   /* UTF-8 passes through */
                }
            }
        }
        out_ += '"';
    }

    bool pretty_;
    std::string out_;
    std::vector<char> stack_;
    std::vector<bool> first_;
    bool after_key_ = false;
};

}  // namespace dh
#endif
