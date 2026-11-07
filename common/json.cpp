#include "json.hpp"
#include <cstdlib>

static const Json NULL_JSON;

namespace {
struct Parser {
    const std::string &s;
    size_t p = 0;
    bool ok = true;

    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) p++; }

    static void putUtf8(std::string &out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }

    unsigned hex4() {
        if (p + 4 > s.size()) { ok = false; return 0; }
        unsigned v = strtoul(s.substr(p, 4).c_str(), nullptr, 16);
        p += 4;
        return v;
    }

    std::string str() {
        std::string out;
        p++; // "
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c != '\\') { out += c; continue; }
            if (p >= s.size()) break;
            char e = s[p++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                        p += 2;
                        unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: out += e;
            }
        }
        p++; // "
        return out;
    }

    Json value() {
        Json j;
        ws();
        if (p >= s.size()) { ok = false; return j; }
        char c = s[p];
        if (c == '{') {
            j.type = Json::Object;
            p++; ws();
            if (s[p] == '}') { p++; return j; }
            while (ok && p < s.size()) {
                ws();
                if (s[p] != '"') { ok = false; break; }
                std::string k = str();
                ws();
                if (s[p] != ':') { ok = false; break; }
                p++;
                j.obj[k] = value();
                ws();
                if (s[p] == ',') { p++; continue; }
                if (s[p] == '}') { p++; break; }
                ok = false;
            }
        } else if (c == '[') {
            j.type = Json::Array;
            p++; ws();
            if (s[p] == ']') { p++; return j; }
            while (ok && p < s.size()) {
                j.arr.push_back(value());
                ws();
                if (s[p] == ',') { p++; continue; }
                if (s[p] == ']') { p++; break; }
                ok = false;
            }
        } else if (c == '"') {
            j.type = Json::String;
            j.str = str();
        } else if (s.compare(p, 4, "true") == 0) {
            j.type = Json::Bool; j.b = true; p += 4;
        } else if (s.compare(p, 5, "false") == 0) {
            j.type = Json::Bool; p += 5;
        } else if (s.compare(p, 4, "null") == 0) {
            p += 4;
        } else {
            char *end = nullptr;
            j.type = Json::Number;
            j.num = strtod(s.c_str() + p, &end);
            if (end == s.c_str() + p) { ok = false; return Json(); }
            j.str = s.substr(p, end - (s.c_str() + p));
            p = end - s.c_str();
        }
        return j;
    }
};
}

Json Json::parse(const std::string &text) {
    Parser ps{text};
    Json j = ps.value();
    return ps.ok ? j : Json();
}

const Json &Json::operator[](const char *key) const {
    if (type != Object) return NULL_JSON;
    auto it = obj.find(key);
    return it == obj.end() ? NULL_JSON : it->second;
}

const Json &Json::operator[](size_t i) const {
    return type == Array && i < arr.size() ? arr[i] : NULL_JSON;
}

std::string Json::asString(const std::string &def) const {
    if (type == String || type == Number) return str;
    return def;
}

long long Json::asInt(long long def) const {
    if (type == Number) return (long long)num;
    if (type == String) return strtoll(str.c_str(), nullptr, 10);
    return def;
}
