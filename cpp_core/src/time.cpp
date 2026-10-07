#include "station/time.hpp"

#include <cstdio>
#include <stdexcept>

namespace station {

namespace {

// Алгоритм Говарда Хиннанта: число дней от 1970-01-01 для григорианской даты.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yy + (m <= 2));
}

int g_offset_min = 300;  // Asia/Almaty (UTC+5) по умолчанию; переопределяется снимком

int parse_int(const std::string& s, size_t pos, size_t len) {
    if (pos + len > s.size()) throw std::runtime_error("bad ISO time: " + s);
    int v = 0;
    for (size_t i = pos; i < pos + len; ++i) {
        if (s[i] < '0' || s[i] > '9') throw std::runtime_error("bad ISO time: " + s);
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

}  // namespace

const Time FAR = from_civil({2100, 1, 1, 0, 0, 0, 0});

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

Time from_civil(const CivilTime& c) {
    std::int64_t days = days_from_civil(c.year, static_cast<unsigned>(c.month), static_cast<unsigned>(c.day));
    return days * DAY + c.hour * HOUR + c.minute * MINUTE + c.second * SECOND + c.micro;
}

CivilTime to_civil(Time t) {
    std::int64_t days = floor_div(t, DAY);
    std::int64_t rest = t - days * DAY;
    CivilTime c{};
    civil_from_days(days, c.year, c.month, c.day);
    c.hour = static_cast<int>(rest / HOUR);
    rest %= HOUR;
    c.minute = static_cast<int>(rest / MINUTE);
    rest %= MINUTE;
    c.second = static_cast<int>(rest / SECOND);
    c.micro = static_cast<int>(rest % SECOND);
    return c;
}

Time parse_iso(const std::string& s) {
    // YYYY-MM-DD[T ]HH:MM[:SS[.ffffff]][Z|±HH:MM]
    CivilTime c{};
    c.year = parse_int(s, 0, 4);
    c.month = parse_int(s, 5, 2);
    c.day = parse_int(s, 8, 2);
    size_t p = 10;
    if (p < s.size() && (s[p] == 'T' || s[p] == ' ')) {
        c.hour = parse_int(s, 11, 2);
        c.minute = parse_int(s, 14, 2);
        p = 16;
        if (p < s.size() && s[p] == ':') {
            c.second = parse_int(s, 17, 2);
            p = 19;
            if (p < s.size() && s[p] == '.') {
                ++p;
                int digits = 0, frac = 0;
                while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
                    if (digits < 6) { frac = frac * 10 + (s[p] - '0'); ++digits; }
                    ++p;
                }
                while (digits < 6) { frac *= 10; ++digits; }
                c.micro = frac;
            }
        }
    }
    Time t = from_civil(c);
    if (p < s.size()) {
        if (s[p] == 'Z') return t;
        if (s[p] == '+' || s[p] == '-') {
            int oh = parse_int(s, p + 1, 2);
            int om = parse_int(s, p + 4, 2);
            Duration off = oh * HOUR + om * MINUTE;
            return s[p] == '+' ? t - off : t + off;
        }
        throw std::runtime_error("bad ISO time: " + s);
    }
    return t;  // без пояса — считаем UTC (как aware() в Python)
}

static std::string fmt_civil(const CivilTime& c, const char* tail) {
    char buf[64];
    if (c.micro)
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%06d%s", c.year, c.month, c.day, c.hour,
                      c.minute, c.second, c.micro, tail);
    else
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d%s", c.year, c.month, c.day, c.hour, c.minute,
                      c.second, tail);
    return buf;
}

std::string iso(Time t) { return fmt_civil(to_civil(t), "Z"); }
std::string isoformat_utc(Time t) { return fmt_civil(to_civil(t), "+00:00"); }

void set_display_offset_min(int off) { g_offset_min = off; }
int display_offset_min() { return g_offset_min; }

CivilTime to_local(Time t) { return to_civil(t + g_offset_min * MINUTE); }
Time from_local(const CivilTime& c) { return from_civil(c) - g_offset_min * MINUTE; }

std::string local_hm(Time t) {
    CivilTime c = to_local(t);
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", c.hour, c.minute);
    return buf;
}

Time floor_to(Time t, int step_min) {
    // Как в Python: int(dt.timestamp()) отбрасывает доли секунды, затем округление вниз до шага.
    std::int64_t epoch = t >= 0 ? t / SECOND : -((-t) / SECOND);
    std::int64_t step = static_cast<std::int64_t>(step_min) * 60;
    std::int64_t mod = ((epoch % step) + step) % step;
    return (epoch - mod) * SECOND;
}

Time ceil_to(Time t, int step_min) {
    Time f = floor_to(t, step_min);
    return f == t ? f : f + minutes(step_min);
}

}  // namespace station
