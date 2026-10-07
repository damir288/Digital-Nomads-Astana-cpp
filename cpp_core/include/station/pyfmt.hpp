// Форматирование чисел «как в Python».
//
// Тексты объяснений собираются из чисел (f"{x:.1f}", f"{u:.0%}", str(10), round(x, 1)), и чтобы
// C++-версия выдавала те же строки до символа, правила форматирования повторены здесь.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace station {

using json = nlohmann::ordered_json;  // порядок ключей как у dict в Python

std::string fmt_f(double x, int prec);      // f"{x:.{prec}f}"
std::string fmt_pct(double x, int prec);    // f"{x:.{prec}%}"
std::string fmt_g(double x);                // f"{x:g}"
std::string py_repr(double x);              // repr(float): кратчайшее представление, «10.0»
std::string py_str(const json& v);          // str(значение из JSON-конфига): 10 -> «10», 10.0 -> «10.0»
double py_round(double x, int ndigits);     // round(x, n) — к ближайшему, при равенстве к чётному
long long py_round0(double x);              // round(x) -> int
long long py_int(const std::string& s, bool& ok);  // int(s) с проверкой
std::string zfill(const std::string& s, size_t width);  // str.zfill
std::string lower_first(const std::string& s);          // s[:1].lower() + s[1:] (UTF-8, кириллица)
std::string lower_utf8(const std::string& s);           // s.lower() для латиницы и кириллицы

// sum() из Python 3.12+: сложение дробных чисел с компенсацией ошибки (алгоритм Ноймайера).
// Обычное сложение в цикле может отличаться в последнем бите, а это меняет округления.
double py_sum(const std::vector<double>& xs);

}  // namespace station
