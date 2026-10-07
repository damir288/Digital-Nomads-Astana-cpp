// Размещение цепочки операций поезда: путь, время, ресурсы, маршрут (аналог placement.py).
//
// Детерминированный алгоритм «самое раннее допустимое размещение» (earliest-fit) с перебором
// путей в порядке best-fit (сначала самый короткий подходящий путь). Жёсткие ограничения
// никогда не нарушаются: совместимость и длина пути, пересечения резервов и закрытий,
// недостоверные данные, маршрут и занятость стрелок, смены и занятость ресурсов, порядок операций.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "station/model.hpp"

namespace station {

// Шаг технологического шаблона: {"kind", "duration", "requires", "group"?, "to_track"?}.
struct Step {
    std::string kind;
    int duration = 0;
    Strings requires;
    OptStr group;     // начало новой стоянки: вид парка (receiving_departure, sorting, cargo, …)
    OptStr to_track;  // для отцепки: путь депо
};

std::vector<Step> steps_from_json(const json& tpl);

struct TrainSpec {
    std::string train_id, number, kind;
    int priority = 1;
    std::optional<double> length_m;
    std::string side_in, side_out;
    std::vector<Step> templ;
    Time arrival = 0;
    OptTime departure_not_before;
    int wagons = 0;
};

struct PlacedOp {
    std::string kind, track_id;
    int seq = 0;
    Time start = 0, end = 0;
    int duration_min = 0;
    OptStr from_track_id, side;
    Strings requirements, resource_ids, route_nodes, through_tracks;

    json as_dict() const;
};

struct TrackReason {
    OptStr code;
    std::string message;
};

struct PlaceResult {
    bool ok = false;
    std::vector<PlacedOp> ops;
    OrderedMap<TrackReason> track_reasons;  // путь -> причина (первая записанная)
    std::set<std::string> failure_codes;
    Strings insufficient;
};

class Placer {
public:
    Placer(const StationModel& m, const IntervalBook& book, int wait_max_min = 90, OptStr ignore_train = std::nullopt);

    PlaceResult place(const TrainSpec& spec, bool arrival_exact = true);

    OptStr force_first;  // путь первой стоянки задан (поезд уже на нём)

private:
    struct TryResult {
        std::optional<PlacedOp> op;
        OptStr code;
        std::vector<Entry> blocks;
    };

    Strings candidates(const std::string& group, const TrainSpec& spec, const std::vector<Step>& steps,
                       PlaceResult& res) const;
    std::vector<Entry> blocking(const std::string& key, Time s, Time e) const;
    bool choose_resources(const Strings& reqs, const std::string& kind, const std::string& track_id, Time s, Time e,
                          Strings& chosen, std::vector<Entry>& blocks, std::string& why) const;
    RoutePtr route_for(const std::string& kind, const TrainSpec& spec, const OptStr& from_track,
                       const OptStr& to_track) const;
    TryResult try_op(const Step& step, const TrainSpec& spec, const std::string& track_id, const OptStr& from_track,
                     Time earliest, bool exact, int seq);
    void note(PlaceResult& res, const std::string& top_track, const std::string& track, OptStr code,
              const std::vector<Entry>& blocks, const Step& step) const;

    const StationModel& m_;
    const IntervalBook& book_;
    Duration wait_max_;
    OptStr ignore_train_;
    json margin_;   // запас по длине, м (число из конфигурации — печатается как в Python)
    Duration buf_;
    OptStr last_why_;
};

// Добавить размещённые операции в книгу (для последовательного размещения нескольких поездов).
void commit_to_book(const StationModel& m, IntervalBook& book, const std::string& train_id, const std::string& number,
                    const std::vector<PlacedOp>& ops);

}  // namespace station
