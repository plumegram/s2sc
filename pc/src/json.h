// Minimal JSON reader. Enough for the skin catalogue and the targets config:
// objects, arrays, strings with escapes, numbers, true/false/null.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value;
struct Member;

// Value holds std::vector<Member> while Member is still incomplete: std::vector
// is the one container the standard allows to be instantiated with an incomplete
// element type. Every member function that touches Member is defined below,
// after Member is complete.
struct Value {
    Type        type = Type::Null;
    bool        boolean = false;
    double      number  = 0.0;
    std::string string;
    std::vector<Value>  array;
    std::vector<Member> object;

    bool IsNull() const { return type == Type::Null; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }
    bool IsString() const { return type == Type::String; }
    bool IsNumber() const { return type == Type::Number; }

    const Value* Find(const std::string& key) const;
    std::string  Str(const std::string& key, const std::string& fallback = {}) const;
    double       Num(const std::string& key, double fallback = 0.0) const;
    int64_t      Int(const std::string& key, int64_t fallback = 0) const;
    bool         Bool(const std::string& key, bool fallback = false) const;
};

struct Member {
    std::string key;
    Value       value;
};

inline const Value* Value::Find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const Member& m : object)
        if (m.key == key) return &m.value;
    return nullptr;
}

inline std::string Value::Str(const std::string& key, const std::string& fallback) const {
    const Value* v = Find(key);
    return (v && v->IsString()) ? v->string : fallback;
}

inline double Value::Num(const std::string& key, double fallback) const {
    const Value* v = Find(key);
    return (v && v->IsNumber()) ? v->number : fallback;
}

inline int64_t Value::Int(const std::string& key, int64_t fallback) const {
    const Value* v = Find(key);
    return (v && v->IsNumber()) ? static_cast<int64_t>(v->number) : fallback;
}

inline bool Value::Bool(const std::string& key, bool fallback) const {
    const Value* v = Find(key);
    if (!v) return fallback;
    if (v->type == Type::Bool) return v->boolean;
    if (v->type == Type::String) return v->string == "true";
    return fallback;
}

class Parser {
public:
    static bool Parse(const std::string& text, Value& out) {
        Parser p(text);
        p.SkipWs();
        if (!p.ParseValue(out)) return false;
        p.SkipWs();
        return true;
    }

private:
    explicit Parser(const std::string& t) : m_s(t) {}

    const std::string& m_s;
    size_t m_i = 0;

    void SkipWs() {
        while (m_i < m_s.size()) {
            char c = m_s[m_i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++m_i; continue; }
            break;
        }
    }

    bool Eof() const { return m_i >= m_s.size(); }

    bool ParseValue(Value& out) {
        SkipWs();
        if (Eof()) return false;
        char c = m_s[m_i];
        switch (c) {
        case '{': return ParseObject(out);
        case '[': return ParseArray(out);
        case '"': {
            out.type = Type::String;
            return ParseString(out.string);
        }
        case 't':
            if (m_s.compare(m_i, 4, "true") == 0) { m_i += 4; out.type = Type::Bool; out.boolean = true; return true; }
            return false;
        case 'f':
            if (m_s.compare(m_i, 5, "false") == 0) { m_i += 5; out.type = Type::Bool; out.boolean = false; return true; }
            return false;
        case 'n':
            if (m_s.compare(m_i, 4, "null") == 0) { m_i += 4; out.type = Type::Null; return true; }
            return false;
        default:
            return ParseNumber(out);
        }
    }

    bool ParseNumber(Value& out) {
        size_t start = m_i;
        if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+')) ++m_i;
        bool any = false;
        while (m_i < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_i]))) { ++m_i; any = true; }
        if (m_i < m_s.size() && m_s[m_i] == '.') {
            ++m_i;
            while (m_i < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_i]))) { ++m_i; any = true; }
        }
        if (any && m_i < m_s.size() && (m_s[m_i] == 'e' || m_s[m_i] == 'E')) {
            ++m_i;
            if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+')) ++m_i;
            while (m_i < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_i]))) ++m_i;
        }
        if (!any) return false;
        out.type   = Type::Number;
        out.number = std::strtod(m_s.substr(start, m_i - start).c_str(), nullptr);
        return true;
    }

    bool ParseString(std::string& out) {
        if (Eof() || m_s[m_i] != '"') return false;
        ++m_i;
        out.clear();
        while (!Eof()) {
            char c = m_s[m_i++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (Eof()) return false;
            char e = m_s[m_i++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (m_i + 4 > m_s.size()) return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = m_s[m_i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                // UTF-8 encode; surrogate pairs are not expected in this data
                if (cp < 0x80) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool ParseArray(Value& out) {
        ++m_i; // '['
        out.type = Type::Array;
        SkipWs();
        if (!Eof() && m_s[m_i] == ']') { ++m_i; return true; }
        for (;;) {
            Value v;
            if (!ParseValue(v)) return false;
            out.array.push_back(std::move(v));
            SkipWs();
            if (Eof()) return false;
            if (m_s[m_i] == ',') { ++m_i; continue; }
            if (m_s[m_i] == ']') { ++m_i; return true; }
            return false;
        }
    }

    bool ParseObject(Value& out) {
        ++m_i; // '{'
        out.type = Type::Object;
        SkipWs();
        if (!Eof() && m_s[m_i] == '}') { ++m_i; return true; }
        for (;;) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (Eof() || m_s[m_i] != ':') return false;
            ++m_i;
            Value v;
            if (!ParseValue(v)) return false;
            out.object.push_back(Member{std::move(key), std::move(v)});
            SkipWs();
            if (Eof()) return false;
            if (m_s[m_i] == ',') { ++m_i; continue; }
            if (m_s[m_i] == '}') { ++m_i; return true; }
            return false;
        }
    }
};

} // namespace json
