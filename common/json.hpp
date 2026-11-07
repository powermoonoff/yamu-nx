// Минимальный JSON-парсер: без исключений, только чтение.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    static Json parse(const std::string &text);

    const Json &operator[](const char *key) const;
    const Json &operator[](size_t i) const;
    size_t size() const { return type == Array ? arr.size() : type == Object ? obj.size() : 0; }
    bool isNull() const { return type == Null; }

    std::string asString(const std::string &def = "") const;
    long long asInt(long long def = 0) const;
    bool asBool(bool def = false) const { return type == Bool ? b : def; }
};
