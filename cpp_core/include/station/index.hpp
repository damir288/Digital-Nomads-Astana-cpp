// Интегральный индекс эффективности станции 0–100 (аналог app/services/index.py).
//
// I = 100 × Σ(wᵢ·sᵢ) / Σwᵢ по составляющим с доступными данными; sᵢ ∈ [0, 1]. Составляющие без
// данных исключаются из числителя и знаменателя, а качество оценки (доля веса с данными)
// показывается явно — благоприятные значения вместо отсутствующих не подставляются.
#pragma once

#include "station/forecast.hpp"

namespace station {

struct ScheduleEntry;  // planner.hpp
template <class V> class OrderedMap;

double utilization_score(double u, const json& p);
std::string index_category(std::optional<double> value, const json& thresholds);

json compute_current(const StationModel& m, const Forecast& fc, const json& conflicts);
// Прогноз индекса для плана на ближайшие 4 ч. schedule == nullptr — «без плана».
json compute_plan_index(const StationModel& m, const OrderedMap<ScheduleEntry>* schedule, const json& kpis);

}  // namespace station
