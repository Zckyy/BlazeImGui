#include "json.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace blaze::json {

// ---------------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------------
const Value* Value::find(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    for (auto& kv : obj_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

Value& Value::operator[](const std::string& key) {
    if (type_ != Type::Object) { *this = MakeObject(); }
    for (auto& kv : obj_)
        if (kv.first == key) return kv.second;
    obj_.emplace_back(key, Value());
    return obj_.back().second;
}

std::string Escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char buf[8]; snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
            else out += char(c);
        }
    }
    return out;
}

static void FormatNumber(std::string& out, double n) {
    if (!std::isfinite(n)) { out += "0"; return; }
    char buf[64];
    if (n == std::floor(n) && std::fabs(n) < 1e15) snprintf(buf, sizeof buf, "%.0f", n);
    else {
        // Shortest representation that round-trips well enough for configs.
        snprintf(buf, sizeof buf, "%.9g", n);
    }
    out += buf;
}

void Value::dumpTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent <= 0) return;
        out += '\n';
        out.append(size_t(indent * d), ' ');
    };
    switch (type_) {
    case Type::Null:   out += "null"; break;
    case Type::Bool:   out += b_ ? "true" : "false"; break;
    case Type::Number: FormatNumber(out, n_); break;
    case Type::String: out += '"'; out += Escape(s_); out += '"'; break;
    case Type::Array: {
        if (arr_.empty()) { out += "[]"; break; }
        // Short numeric arrays (colors, vectors) stay on one line.
        bool inlineArr = arr_.size() <= 4;
        for (auto& v : arr_) inlineArr &= v.isNumber();
        out += '[';
        for (size_t i = 0; i < arr_.size(); ++i) {
            if (i) out += inlineArr ? ", " : ",";
            if (!inlineArr) newline(depth + 1);
            arr_[i].dumpTo(out, indent, depth + 1);
        }
        if (!inlineArr) newline(depth);
        out += ']';
        break;
    }
    case Type::Object: {
        if (obj_.empty()) { out += "{}"; break; }
        out += '{';
        for (size_t i = 0; i < obj_.size(); ++i) {
            if (i) out += ',';
            newline(depth + 1);
            out += '"'; out += Escape(obj_[i].first); out += "\":";
            if (indent > 0) out += ' ';
            obj_[i].second.dumpTo(out, indent, depth + 1);
        }
        newline(depth);
        out += '}';
        break;
    }
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------------
namespace {
struct Parser {
    const char* p;
    const char* end;
    std::string err;

    void ws() {
        while (p < end) {
            if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
            else if (p + 1 < end && p[0] == '/' && p[1] == '/') { // tolerate // comments
                while (p < end && *p != '\n') ++p;
            } else break;
        }
    }
    bool fail(const char* msg) { if (err.empty()) err = msg; return false; }

    static void appendUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80) s += char(cp);
        else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
        else { s += char(0xF0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 0x3F)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    }

    bool hex4(unsigned& out) {
        if (end - p < 4) return fail("bad \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            out <<= 4;
            if (c >= '0' && c <= '9') out |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') out |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= unsigned(c - 'A' + 10);
            else return fail("bad hex digit");
        }
        return true;
    }

    bool str(std::string& s) {
        if (p >= end || *p != '"') return fail("expected string");
        ++p;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') { s += c; continue; }
            if (p >= end) return fail("unterminated escape");
            char e = *p++;
            switch (e) {
            case '"': s += '"'; break;  case '\\': s += '\\'; break; case '/': s += '/'; break;
            case 'b': s += '\b'; break; case 'f': s += '\f'; break;  case 'n': s += '\n'; break;
            case 'r': s += '\r'; break; case 't': s += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2; unsigned lo;
                    if (!hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                appendUtf8(s, cp);
                break;
            }
            default: return fail("unknown escape");
            }
        }
        if (p >= end) return fail("unterminated string");
        ++p;
        return true;
    }

    bool value(Value& v, int depth) {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (p >= end) return fail("unexpected end");
        char c = *p;
        if (c == '{') {
            ++p; v = Value::MakeObject();
            ws();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!str(key)) return false;
                ws();
                if (p >= end || *p != ':') return fail("expected ':'");
                ++p;
                Value child;
                if (!value(child, depth + 1)) return false;
                v.members().emplace_back(std::move(key), std::move(child));
                ws();
                if (p < end && *p == ',') { ++p; ws(); if (p < end && *p == '}') { ++p; return true; } continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p; v = Value::MakeArray();
            ws();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                Value child;
                if (!value(child, depth + 1)) return false;
                v.items().push_back(std::move(child));
                ws();
                if (p < end && *p == ',') { ++p; ws(); if (p < end && *p == ']') { ++p; return true; } continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { std::string s; if (!str(s)) return false; v = Value(std::move(s)); return true; }
        if (end - p >= 4 && strncmp(p, "true", 4) == 0)  { p += 4; v = Value(true);  return true; }
        if (end - p >= 5 && strncmp(p, "false", 5) == 0) { p += 5; v = Value(false); return true; }
        if (end - p >= 4 && strncmp(p, "null", 4) == 0)  { p += 4; v = Value();      return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            std::string num;
            while (p < end && (strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) num += *p++;
            char* stop = nullptr;
            double d = strtod(num.c_str(), &stop);
            if (!stop || *stop) return fail("bad number");
            v = Value(d);
            return true;
        }
        return fail("unexpected character");
    }
};
} // namespace

bool Parse(const std::string& text, Value& out, std::string* error) {
    Parser ps{ text.data(), text.data() + text.size(), {} };
    // Skip UTF-8 BOM
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        ps.p += 3;
    Value v;
    bool ok = ps.value(v, 0);
    if (ok) { ps.ws(); if (ps.p != ps.end) ok = ps.fail("trailing characters"); }
    if (!ok) {
        if (error) {
            size_t offset = size_t(ps.p - text.data());
            *error = ps.err + " at offset " + std::to_string(offset);
        }
        return false;
    }
    out = std::move(v);
    return true;
}

// ---------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool ReadFile(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? size_t(size) : 0);
    size_t read = size > 0 ? fread(out.data(), 1, out.size(), f) : 0;
    fclose(f);
    out.resize(read);
    return true;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    // Write to a temp file then rename, so a crash never leaves a half-written config.
    std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
    size_t written = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    if (written != data.size()) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

} // namespace blaze::json
