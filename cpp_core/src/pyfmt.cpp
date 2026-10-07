#include "station/pyfmt.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace station {

std::string fmt_f(double x, int prec) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", prec, x);
    return buf;
}

std::string fmt_pct(double x, int prec) { return fmt_f(x * 100.0, prec) + "%"; }

std::string fmt_g(double x) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", x);
    return buf;
}

std::string py_repr(double x) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
    char buf[64];
    // Кратчайшая запись, которая читается обратно в то же число (так делает repr в Python).
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, x);
        if (std::strtod(buf, nullptr) == x) break;
    }
    std::string s = buf;
    double ax = std::fabs(x);
    if (s.find('e') != std::string::npos && ax >= 1e-4 && ax < 1e16) {
        std::snprintf(buf, sizeof buf, "%.17f", x);  // Python не переходит к экспоненте в этом диапазоне
        s = buf;
        while (!s.empty() && s.back() == '0') s.pop_back();
    }
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    if (!s.empty() && s.back() == '.') s += "0";
    return s;
}

std::string py_str(const json& v) {
    if (v.is_number_integer() || v.is_number_unsigned()) return std::to_string(v.get<long long>());
    if (v.is_number_float()) return py_repr(v.get<double>());
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    if (v.is_null()) return "None";
    return v.dump();
}

double py_round(double x, int ndigits) {
    // printf округляет точное двоичное значение до ближайшего (при равенстве — к чётному),
    // как и round() в Python.
    return std::strtod(fmt_f(x, ndigits).c_str(), nullptr);
}

long long py_round0(double x) { return static_cast<long long>(std::nearbyint(x)); }

long long py_int(const std::string& s, bool& ok) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    std::string t = s.substr(b, e - b);
    ok = false;
    if (t.empty()) return 0;
    size_t i = (t[0] == '+' || t[0] == '-') ? 1 : 0;
    if (i == t.size()) return 0;
    for (size_t k = i; k < t.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(t[k])) && t[k] != '_') return 0;
    std::string digits;
    for (char ch : t) if (ch != '_') digits += ch;
    ok = true;
    return std::strtoll(digits.c_str(), nullptr, 10);
}

std::string zfill(const std::string& s, size_t width) {
    // Ширина в символах; номера путей — ASCII или однобайтные цифры/латиница.
    if (s.size() >= width) return s;
    size_t pad = width - s.size();
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) return s[0] + std::string(pad, '0') + s.substr(1);
    return std::string(pad, '0') + s;
}

// Строчная буква для одного символа UTF-8 (латиница и русская кириллица); длина символа — в len.
static std::string lower_char(const std::string& s, size_t i, size_t& len) {
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) { len = 1; return std::string(1, static_cast<char>(std::tolower(c0))); }
    len = (c0 >= 0xF0) ? 4 : (c0 >= 0xE0) ? 3 : (c0 >= 0xC0) ? 2 : 1;
    if (c0 == 0xD0 && i + 1 < s.size()) {
        unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        // А–П (D0 90–9F) -> а–п (D0 B0–BF); Р–Я (D0 A0–AF) -> р–я (D1 80–8F); Ё (D0 81) -> ё (D1 91)
        if (c1 >= 0x90 && c1 <= 0x9F) return {static_cast<char>(0xD0), static_cast<char>(c1 + 0x20)};
        if (c1 >= 0xA0 && c1 <= 0xAF) return {static_cast<char>(0xD1), static_cast<char>(c1 - 0x20)};
        if (c1 == 0x81) return {static_cast<char>(0xD1), static_cast<char>(0x91)};
    }
    return s.substr(i, len);
}

std::string lower_utf8(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        size_t len = 1;
        out += lower_char(s, i, len);
        i += len;
    }
    return out;
}

std::string lower_first(const std::string& s) {
    if (s.empty()) return s;
    size_t len = 1;
    std::string head = lower_char(s, 0, len);
    return head + s.substr(len);
}

double py_sum(const std::vector<double>& xs) {
    // Как builtin_sum_impl в CPython 3.12: первое слагаемое прибавляется к целому 0 обычным
    // сложением, остальные — с накоплением поправки c, которая добавляется в конце.
    if (xs.empty()) return 0.0;
    double f = 0.0 + xs[0];
    double c = 0.0;
    for (size_t i = 1; i < xs.size(); ++i) {
        double x = xs[i];
        double t = f + x;
        if (std::fabs(f) >= std::fabs(x)) c += (f - t) + x;
        else c += (x - t) + f;
        f = t;
    }
    if (c != 0.0 && std::isfinite(c)) f += c;
    return f;
}

}  // namespace station
