#include "station/forecast.hpp"

#include <algorithm>

namespace station {

Forecast forecast(const StationModel& m) {
    Forecast out;
    const Time now = m.now;
    for (const auto& [train_id, ids] : m.ops_by_train) {
        const Train* train = m.trains.find(train_id);
        OptTime t_prev;
        for (const auto& id : ids) {
            const Operation& o = m.ops.at(id);
            Duration dur = minutes(o.duration_min + o.extra_delay_min);
            if (o.status == "cancelled") continue;
            Time s, e;
            if (o.status == "done") {
                s = o.actual_start.value_or(o.planned_start);
                e = o.actual_end.value_or(o.planned_end);
            } else if (o.status == "in_progress") {
                s = o.actual_start.value_or(o.planned_start);
                e = std::max(s + dur, now);
            } else {
                s = std::max(o.planned_start, now);
                if (o.not_before) s = std::max(s, *o.not_before);
                if (o.kind == "arrival" && train && train->expected_arrival) s = std::max(s, *train->expected_arrival);
                if (t_prev) s = std::max(s, *t_prev);
                e = s + dur;
            }
            out[o.id] = {s, e};
            t_prev = e;
        }
    }
    for (const auto& [id, o] : m.ops) {  // операции без поезда
        if (o.train_id) continue;
        Duration dur = minutes(o.duration_min + o.extra_delay_min);
        if (o.status == "done") {
            out[id] = {o.actual_start.value_or(o.planned_start), o.actual_end.value_or(o.planned_end)};
        } else if (o.status == "in_progress") {
            Time s = o.actual_start.value_or(o.planned_start);
            out[id] = {s, std::max(s + dur, now)};
        } else if (o.status != "cancelled") {
            Time s = std::max(o.planned_start, now);
            if (o.not_before) s = std::max(s, *o.not_before);
            out[id] = {s, s + dur};
        }
    }
    return out;
}

OrderedMap<double> departure_delays(const StationModel& m, const Forecast& fc) {
    // Задержка отправления (или прибытия, если отправления нет) относительно расписания, мин.
    OrderedMap<double> out;
    for (const auto& [tid, ids] : m.ops_by_train) {
        const Train& t = m.trains.at(tid);
        const Operation* dep = nullptr;
        const Operation* arr = nullptr;
        for (const auto& id : ids) {
            const Operation& o = m.ops.at(id);
            if (!dep && o.kind == "departure" && o.status != "cancelled") dep = &o;
            if (!arr && o.kind == "arrival") arr = &o;
        }
        if (dep && t.scheduled_departure && fc.contains(dep->id))
            out[tid] = std::max(0.0, minutes_f(fc.at(dep->id).first - *t.scheduled_departure));
        if (!dep && arr && t.scheduled_arrival && fc.contains(arr->id))
            out[tid] = std::max(0.0, minutes_f(fc.at(arr->id).first - *t.scheduled_arrival));
    }
    return out;
}

}  // namespace station
