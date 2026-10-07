// Работа со временем (аналог app/core/timeutil.py).
//
// Момент времени хранится как целое число микросекунд от 1970-01-01 UTC (как datetime с
// tzinfo=UTC в Python). Отображение «ЧЧ:ММ» — в часовом поясе станции; у пояса станции нет
// перехода на летнее время, поэтому достаточно постоянного смещения в минутах.
#pragma once

#include <cstdint>
#include <string>

namespace station {

using Time = std::int64_t;      // микросекунды UTC
using Duration = std::int64_t;  // микросекунды

constexpr Duration US = 1;
constexpr Duration SECOND = 1000000 * US;
constexpr Duration MINUTE = 60 * SECOND;
constexpr Duration HOUR = 60 * MINUTE;
constexpr Duration DAY = 24 * HOUR;

inline Duration minutes(std::int64_t m) { return m * MINUTE; }
inline Duration hours(std::int64_t h) { return h * HOUR; }

// Дата/время по частям (в UTC или в местном времени — зависит от вызова).
struct CivilTime {
    int year, month, day, hour, minute, second, micro;
};

Time from_civil(const CivilTime& c);   // части считаются UTC
CivilTime to_civil(Time t);            // в UTC

// FAR — «бесконечно далёкое будущее» (открытый конец закрытия, смены и т. п.).
extern const Time FAR;

// Разбор ISO-8601: «2026-10-01T07:10:00Z», «…T07:10:00.123456+00:00», «…+05:00».
Time parse_iso(const std::string& s);

// iso() из Python: «2026-10-01T07:10:00Z» (микросекунды — только если не ноль).
std::string iso(Time t);
// datetime.isoformat() для времени в UTC: «2026-10-01T07:10:00+00:00».
std::string isoformat_utc(Time t);

// Часовой пояс отображения (смещение от UTC в минутах), задаётся при загрузке снимка.
void set_display_offset_min(int off);
int display_offset_min();

std::string local_hm(Time t);                       // «14:30» в поясе станции
CivilTime to_local(Time t);                         // части местного времени
Time from_local(const CivilTime& c);                // местные части -> момент UTC

Time floor_to(Time t, int step_min);
Time ceil_to(Time t, int step_min);

// Деление с округлением вниз (как // в Python) — для «минут между моментами».
std::int64_t floor_div(std::int64_t a, std::int64_t b);
inline std::int64_t whole_minutes(Duration d) { return floor_div(d, MINUTE); }
inline double minutes_f(Duration d) { return static_cast<double>(d) / 1e6 / 60.0; }

}  // namespace station
