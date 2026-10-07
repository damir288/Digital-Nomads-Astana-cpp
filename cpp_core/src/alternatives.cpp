// Проверенные альтернативы при отказе в приёме (аналог app/services/alternatives.py):
//   * перенос отправления на ближайшее допустимое окно;
//   * другая совместимая станция (по упрощённой модели соседа);
//   * изменение очереди (менее приоритетный не начатый поезд — на другой путь);
//   * разделение партии (если разрешено заявкой и хватает локомотивов).
#include <algorithm>
#include <cmath>
#include <set>

#include "station/checker.hpp"

namespace station {

namespace {

const Duration STEP = minutes(5);

bool static_ok(const StationModel& m, const Request& req, Time arrival, std::optional<double> length, int wagons) {
    for (const auto& i : check_static_rules(m, req, arrival, length, wagons))
        if (i.at("status") == "fail") return false;
    return true;
}

long long occupancy_minutes(const std::vector<PlacedOp>& ops, const std::string& track) {
    Time s = FAR, e = 0;
    bool any_s = false, any_e = false;
    for (const auto& o : ops) {
        if (o.track_id == track) { s = any_s ? std::min(s, o.start) : o.start; any_s = true; }
        if (o.track_id == track || o.from_track_id == track) { e = any_e ? std::max(e, o.end) : o.end; any_e = true; }
    }
    return whole_minutes(e - s);
}

struct Found {
    Time t = 0, dep = 0;
    PlaceResult r;
};

std::optional<Found> nearest_window(const StationModel& m, const IntervalBook& book, const Request& req, Time arrival,
                                    double length, int wagons, int horizon_h = 12) {
    int tmin = travel_min(m, req.from_station_id);
    Time t = floor_to(arrival, 1);  // arrival.replace(second=0, microsecond=0)
    int minute = to_civil(t).minute;
    t += minutes(((-minute) % 5 + 5) % 5);  // до ближайшей «круглой» пятиминутки
    if (t <= arrival) t += STEP;
    Time end = arrival + hours(horizon_h);
    for (; t <= end; t += STEP) {
        Time dep = t - minutes(tmin);
        if (dep < m.now) continue;
        PlaceResult r = place_request(m, book, req, t, length, wagons, req.train_id);
        if (!r.ok) continue;
        bool nb_fail = false;
        for (const auto& i : neighbor_checks(m, book, req, dep, t))
            if (i.at("status") == "fail") nb_fail = true;
        if (!nb_fail && static_ok(m, req, t, length, wagons)) return Found{t, dep, r};
    }
    return std::nullopt;
}

json other_station(const StationModel& m, const IntervalBook& book, const Request& req, Time departure, double length) {
    json out = json::array();
    const Neighbor* origin = m.neighbors.find(req.from_station_id);
    if (!origin) return out;
    for (const auto& [nid, n] : m.neighbors) {
        if (nid == req.from_station_id) continue;
        const json& c = n.config;
        json travel_to = origin->config.value("travel_min_to", json::object());
        if (travel_to.is_null()) travel_to = json::object();
        if (!travel_to.contains(nid) || travel_to[nid].is_null()) continue;
        bool accepts = false;
        for (const auto& a : c.value("accepts", json::array()))
            if (a == "transfer") accepts = true;
        if (!accepts) continue;
        int travel = travel_to[nid].get<int>();
        Strings reasons;
        if (c.contains("max_train_length_m") && !c["max_train_length_m"].is_null() &&
            c["max_train_length_m"].get<double>() != 0.0 && length > c["max_train_length_m"].get<double>())
            reasons.push_back("длина состава " + fmt_f(length, 0) + " м больше допустимой " +
                              py_str(c["max_train_length_m"]) + " м");
        Time arr = departure + minutes(travel);
        Duration proc = minutes(static_cast<long long>(c.value("processing_min", json(90)).get<double>()));
        if (!book.conflicts("neighbor:" + nid, arr, arr + proc).empty()) reasons.push_back("действует ограничение приёма");
        long long free_tracks = 0;
        json occupancy = c.value("occupancy", json::array());
        if (occupancy.is_null()) occupancy = json::array();
        for (const auto& occ : occupancy) {
            bool busy = false;
            for (const auto& p : occ)
                if (parse_iso(p[0].get<std::string>()) < arr + proc && arr < parse_iso(p[1].get<std::string>())) busy = true;
            if (!busy) ++free_tracks;
        }
        if (occupancy.empty()) free_tracks = c.value("receiving_tracks", json(0)).get<long long>();
        if (free_tracks == 0) reasons.push_back("нет свободного приёмного пути на интервал");
        if (!reasons.empty()) continue;
        int own_travel = static_cast<int>(origin->config.value("travel_min", json(90)).get<double>());
        out.push_back(json{
            {"type", "other_station"},
            {"title", "Направить на станцию " + n.name},
            {"description", "Прибытие на " + n.name + " в " + local_hm(arr) +
                                "; свободных приёмных путей на интервал обработки: " + std::to_string(free_tracks) +
                                ". Проверено по упрощённой модели соседней станции."},
            {"verified", true},
            {"effect", json{{"delay_min", 0},
                            {"extra_travel_min", travel - own_travel},
                            {"load_change", "Загрузка " + m.station_name + " не меняется; у " + n.name + ": +1 состав на " +
                                                std::to_string(whole_minutes(proc)) + " мин"},
                            {"affected_operations", json::array()}}},
            {"action", json{{"type", "other_station"}, {"station_id", nid}}},
        });
    }
    return out;
}

// Шаблон поезда из его текущих операций (для повторного размещения при «изменении очереди»).
TrainSpec train_spec_from_ops(const StationModel& m, const Train& train) {
    auto ops = m.train_ops(train.id);
    TrainSpec s;
    for (const Operation* o : ops) {
        Step st;
        st.kind = o->kind;
        st.duration = o->duration_min;
        st.requires = o->requirements;
        if (o->kind == "arrival" || o->kind == "shunting") st.group = m.track_rows.at(*o->track_id).kind;
        s.templ.push_back(st);
    }
    bool has_dep = std::any_of(ops.begin(), ops.end(), [](const Operation* o) { return o->kind == "departure"; });
    s.train_id = train.id;
    s.number = train.number;
    s.kind = train.kind;
    s.priority = train.priority;
    s.length_m = m.train_length(train);
    s.side_in = train.arrival_side;
    s.side_out = train.departure_side;
    s.arrival = ops.empty() ? m.now : ops[0]->planned_start;
    if (has_dep) s.departure_not_before = train.scheduled_departure;
    s.wagons = train.wagons_count;
    return s;
}

json reorder(const StationModel& m, const IntervalBook& book, const Request& req, Time arrival, double length) {
    // Освободить путь, переставив менее приоритетный не начатый поезд на другой путь.
    json out = json::array();
    std::set<std::string> tried;
    const std::set<std::string> only_res{"reservation"};
    const int req_priority = req.priority ? req.priority : 2;
    for (const auto& [tid, t] : m.track_rows) {
        if (t.kind != "receiving_departure") continue;
        ConflictFilter f;
        f.kinds = &only_res;
        for (const auto& e : book.conflicts("track:" + tid, arrival, arrival + hours(3), f)) {
            const Train* x = m.train(e.meta.train_id);
            if (!x || tried.count(x->id) || x->priority > req_priority) continue;
            auto ops = m.train_ops(x->id);
            if (ops.empty() || std::any_of(ops.begin(), ops.end(), [](const Operation* o) {
                    return o->status == "in_progress" || o->status == "done";
                }))
                continue;
            tried.insert(x->id);
            IntervalBook b2 = book.without_train(x->id);
            PlaceResult r = place_request(m, b2, req, arrival, length, req.wagons_count, req.train_id);
            if (!r.ok) continue;
            commit_to_book(m, b2, "REQ-" + req.id, req.number, r.ops);
            TrainSpec spec = train_spec_from_ops(m, *x);
            PlaceResult r2 = Placer(m, b2, 45).place(spec, true);
            if (!r2.ok) continue;
            const Operation* old_dep = nullptr;
            for (const Operation* o : ops)
                if (o->kind == "departure") { old_dep = o; break; }
            const PlacedOp* new_dep = nullptr;
            for (const auto& o : r2.ops)
                if (o.kind == "departure") { new_dep = &o; break; }
            long long delay = (old_dep && new_dep) ? whole_minutes(new_dep->start - old_dep->planned_start) : 0;
            if (delay > 45) continue;
            OptStr old_track = ops[0]->track_id;
            const std::string& new_track = r2.ops[0].track_id;
            json changed = json::array();
            json action_ops = json::array();
            for (size_t k = 0; k < std::min(ops.size(), r2.ops.size()); ++k) {
                const Operation& o_old = *ops[k];
                const PlacedOp& o_new = r2.ops[k];
                if (o_old.track_id != o_new.track_id || o_old.planned_start != o_new.start)
                    changed.push_back(json{{"operation_id", o_old.id},
                                           {"train", x->number},
                                           {"kind", o_old.kind},
                                           {"from", m.track_label(o_old.track_id) + " " + local_hm(o_old.planned_start)},
                                           {"to", m.track_label(o_new.track_id) + " " + local_hm(o_new.start)}});
                action_ops.push_back(json{{"operation_id", o_old.id},
                                          {"track_id", o_new.track_id},
                                          {"from_track_id", o_new.from_track_id ? json(*o_new.from_track_id) : json(nullptr)},
                                          {"start", iso(o_new.start)},
                                          {"resource_ids", o_new.resource_ids},
                                          {"route_nodes", o_new.route_nodes}});
            }
            out.push_back(json{
                {"type", "reorder"},
                {"title", "Изменить очередь: поезд № " + x->number + " — с " + m.track_label_lc(old_track) + " на " +
                              m.track_label_lc(new_track)},
                {"description", "Освобождает " + m.track_label_lc(r.ops[0].track_id) + " для приёма в " +
                                    local_hm(arrival) + ". Поезд № " + x->number + " (приоритет " +
                                    std::to_string(x->priority) + ") не начат; обе цепочки операций проверены."},
                {"verified", true},
                {"effect", json{{"delay_min", std::max<long long>(0, delay)},
                                {"affected_train", x->number},
                                {"load_change", m.track_label(new_track) + " занят поездом № " + x->number + " вместо " +
                                                    m.track_label_lc(old_track)},
                                {"affected_operations", changed}}},
                {"action", json{{"type", "reorder"}, {"train_id", x->id}, {"ops", action_ops}}},
                {"requires_role", "station_dispatcher"},
            });
            if (out.size() >= 2) return out;
        }
    }
    return out;
}

json split(const StationModel& m, const IntervalBook& book, const Request& req, Time arrival, double length) {
    json out = json::array();
    const Neighbor* origin = m.neighbors.find(req.from_station_id);
    json locos = origin ? origin->config.value("locomotives_available", json(1)) : json(nullptr);
    if (origin && locos.get<double>() < 2) return out;
    int n1 = static_cast<int>(std::ceil(req.wagons_count / 2.0));
    int n2 = req.wagons_count - n1;
    double loco = m.processing("default_loco_length_m", 34).get<double>();
    double per_wagon = (length - loco) / req.wagons_count;
    double l1 = py_round(n1 * per_wagon + loco, 1), l2 = py_round(n2 * per_wagon + loco, 1);
    IntervalBook b = book;
    PlaceResult r1 = place_request(m, b, req, arrival, l1, n1, req.train_id);
    if (!r1.ok) return out;
    commit_to_book(m, b, "REQ-" + req.id + "-1", req.number + "/1", r1.ops);
    for (Time t = arrival + minutes(15); t <= arrival + hours(4); t += STEP) {
        PlaceResult r2 = place_request(m, b, req, t, l2, n2, req.train_id);
        if (!r2.ok) continue;
        long long delay = whole_minutes(t - arrival);
        out.push_back(json{
            {"type", "split"},
            {"title", "Разделить партию: " + std::to_string(n1) + " ваг. в " + local_hm(arrival) + " и " +
                          std::to_string(n2) + " ваг. в " + local_hm(t)},
            {"description", "Часть 1 (" + fmt_f(l1, 0) + " м) — " + m.track_label_lc(r1.ops[0].track_id) +
                                ", часть 2 (" + fmt_f(l2, 0) + " м) — " + m.track_label_lc(r2.ops[0].track_id) +
                                ". Требуется 2 поездных локомотива на станции отправления (доступно: " + py_str(locos) +
                                ")."},
            {"verified", true},
            {"effect", json{{"delay_min", delay},
                            {"load_change", "Две операции приёма вместо одной"},
                            {"affected_operations", json::array()}}},
            {"action", json{{"type", "split"},
                            {"parts", json::array({json{{"wagons", n1}, {"departure", iso(req.desired_departure)}},
                                                   json{{"wagons", n2},
                                                        {"departure", iso(req.desired_departure + (t - arrival))}}})}}},
        });
        return out;
    }
    return out;
}

}  // namespace

std::pair<json, std::optional<Window>> build_alternatives(const StationModel& m, const IntervalBook& book,
                                                          const Request& req, Time departure, Time arrival,
                                                          double length, const json& items) {
    json alts = json::array();
    std::optional<Window> window;
    bool hard_static_fail = false;  // жёсткая квота: время не поможет — только другая станция
    for (const auto& i : items)
        if (i.at("status") == "fail" && i.at("code") == "MONTHLY_PLAN") hard_static_fail = true;
    if (!hard_static_fail) {
        if (auto f = nearest_window(m, book, req, arrival, length, req.wagons_count)) {
            long long delay = whole_minutes(f->t - arrival);
            const std::string& tr = f->r.ops[0].track_id;
            window = Window{f->t, f->dep, tr, delay};
            alts.push_back(json{
                {"type", "postpone"},
                {"title", "Перенести отправление на " + local_hm(f->dep)},
                {"description", "Прибытие в " + local_hm(f->t) + " на " + m.track_label_lc(tr) +
                                    "; весь интервал приёма и обработки проверен."},
                {"verified", true},
                {"effect", json{{"delay_min", delay},
                                {"load_change", m.track_label(tr) + ": +" + std::to_string(occupancy_minutes(f->r.ops, tr)) +
                                                    " мин занятости с " + local_hm(f->r.ops[0].start)},
                                {"affected_operations", json::array()}}},
                {"action", json{{"type", "postpone"}, {"departure", iso(f->dep)}}},
            });
        }
    }
    for (auto& a : other_station(m, book, req, departure, length)) alts.push_back(a);
    if (!hard_static_fail) {
        for (auto& a : reorder(m, book, req, arrival, length)) alts.push_back(a);
        if (req.split_allowed && req.wagons_count >= 2)
            for (auto& a : split(m, book, req, arrival, length)) alts.push_back(a);
    }
    return {alts, window};
}

}  // namespace station
