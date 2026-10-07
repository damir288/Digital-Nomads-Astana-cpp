// Проверка возможности приёма состава по заявке (аналог checker.py) и проверенные
// альтернативы при отказе (аналог alternatives.py).
//
// Каждое ограничение проверяется отдельно и даёт пункт со статусом ok | warning | fail |
// insufficient_data. Альтернативы предлагаются, только если прошли ту же проверку.
#pragma once

#include <optional>
#include <utility>

#include "station/placement.hpp"

namespace station {

std::pair<std::optional<double>, std::string> train_length_for_request(const StationModel& m, const Request& req);
json check_static_rules(const StationModel& m, const Request& req, Time arrival, std::optional<double> length,
                        int wagons);
json neighbor_checks(const StationModel& m, const IntervalBook& book, const Request& req, Time departure, Time arrival);
PlaceResult place_request(const StationModel& m, const IntervalBook& book, const Request& req, Time arrival,
                          std::optional<double> length, int wagons, const OptStr& ignore_train);
int travel_min(const StationModel& m, const std::string& from_id);

json check_request(const StationModel& m, const Request& req, const IntervalBook& book, bool with_alternatives = true);

// Ближайшее допустимое окно (результат альтернативы «перенос»).
struct Window {
    Time arrival = 0, departure = 0;
    std::string track_id;
    long long delay_min = 0;
};

std::pair<json, std::optional<Window>> build_alternatives(const StationModel& m, const IntervalBook& book,
                                                          const Request& req, Time departure, Time arrival,
                                                          double length, const json& items);

}  // namespace station
