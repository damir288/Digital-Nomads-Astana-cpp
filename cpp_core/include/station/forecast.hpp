// Прогноз операций и детектор конфликтов (аналог forecast.py и conflicts.py).
//
// Прогноз: операция не начинается раньше планового начала, раньше «не ранее», раньше окончания
// предыдущей операции и раньше текущего модельного времени; длительность растёт на задержку от
// инцидентов. Конфликт — нарушение жёсткого ограничения в прогнозе.
#pragma once

#include <utility>

#include "station/model.hpp"

namespace station {

using Forecast = OrderedMap<std::pair<Time, Time>>;  // id операции -> (начало, конец)

Forecast forecast(const StationModel& m);
OrderedMap<double> departure_delays(const StationModel& m, const Forecast& fc);  // id поезда -> минуты

struct Detection {
    json conflicts = json::array();
    json delays = json::array();
};

Detection detect(const StationModel& m, const Forecast& fc);
int route_conflict_count(const json& conflicts);

}  // namespace station
