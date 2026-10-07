#include "station/model.hpp"

#include <algorithm>
#include <stdexcept>

namespace station {

// ------------------------------------------------------------------ справочники
const std::vector<std::pair<std::string, std::string>> KIND_LABEL_LIST = {
    {"arrival", "Прибытие"},   {"inspection", "Техосмотр"}, {"shunting", "Манёвры"},
    {"loading", "Погрузка"},   {"unloading", "Выгрузка"},   {"repair", "Ремонт"},
    {"departure", "Отправление"}, {"dwell", "Стоянка"},     {"sorting", "Расформирование"},
    {"uncoupling", "Отцепка вагона"},
};

const std::string& kind_label(const std::string& kind) {
    for (const auto& [k, v] : KIND_LABEL_LIST)
        if (k == kind) return v;
    return kind;
}

const std::string& res_kind_label(const std::string& kind) {
    static const std::vector<std::pair<std::string, std::string>> L = {
        {"shunting_loco", "маневровый локомотив"}, {"loco_crew", "локомотивная бригада"},
        {"shunting_crew", "составительская бригада"}, {"inspection_team", "бригада осмотрщиков"},
        {"cargo_equipment", "погрузочно-разгрузочный механизм"}, {"repair_team", "ремонтная бригада"},
    };
    for (const auto& [k, v] : L)
        if (k == kind) return v;
    return kind;
}

bool is_movement(const std::string& kind) {
    return kind == "arrival" || kind == "departure" || kind == "shunting" || kind == "uncoupling";
}

// ------------------------------------------------------------------ книга интервалов
void IntervalBook::add(const std::string& key, Time start, Time end, EntryMeta meta) {
    if (end <= start) return;
    auto& lst = data_[key];
    // bisect.insort: сортировка по (начало, конец), новый интервал — после равных.
    Entry e{start, end, std::move(meta)};
    auto pos = std::upper_bound(lst.begin(), lst.end(), e, [](const Entry& a, const Entry& b) {
        return a.start != b.start ? a.start < b.start : a.end < b.end;
    });
    lst.insert(pos, std::move(e));
}

std::vector<Entry> IntervalBook::conflicts(const std::string& key, Time start, Time end, const ConflictFilter& f) const {
    std::vector<Entry> out;
    const auto* lst = data_.find(key);
    if (!lst) return out;
    for (const auto& e : *lst) {
        if (e.start >= end) break;  // список отсортирован по началу
        if (e.end <= start) continue;
        if (f.ignore_train && !f.ignore_train->empty() && e.meta.train_id == f.ignore_train) continue;
        if (f.ignore_ops && e.meta.op_id && f.ignore_ops->count(*e.meta.op_id)) continue;
        if (f.kinds && !f.kinds->count(e.meta.type)) continue;
        out.push_back(e);
    }
    return out;
}

const std::vector<Entry>* IntervalBook::entries(const std::string& key) const { return data_.find(key); }

IntervalBook IntervalBook::without_train(const std::string& train_id) const {
    IntervalBook b;
    for (const auto& [k, v] : data_) {
        auto& dst = b.data_[k];
        for (const auto& e : v)
            if (e.meta.train_id != train_id) dst.push_back(e);
    }
    return b;
}

std::string describe_block(const Entry& e) {
    const auto& m = e.meta;
    const std::string& t = m.type;
    std::string span = local_hm(e.start) + "–" + (e.end < FAR ? local_hm(e.end) : "…");
    auto label_or = [&](const std::string& def) { return m.label ? *m.label : def; };
    if (t == "reservation") return "резерв «" + label_or("операция") + "» " + span;
    if (t == "closure") return "закрытие: " + label_or("инцидент") + " (" + span + ")";
    if (t == "maintenance") return "окно обслуживания: " + label_or("") + " (" + span + ")";
    if (t == "shift") return "вне смены (" + span + ")";
    if (t == "faulty") return "неисправность ресурса (" + span + ")";
    if (t == "data") return label_or("нет достоверных данных о состоянии");
    if (t == "restriction") return "ограничение: " + label_or("") + " (" + span + ")";
    return span;
}

// ------------------------------------------------------------------ загрузка из JSON
namespace {

OptStr opt_str(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    return it->get<std::string>();
}
OptTime opt_time(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    return parse_iso(it->get<std::string>());
}
std::optional<double> opt_num(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    return it->get<double>();
}
Strings str_list(const json& j, const char* key) {
    Strings out;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return out;
    for (const auto& v : *it) out.push_back(v.get<std::string>());
    return out;
}
int int_or(const json& j, const char* key, int def) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    return it->get<int>();
}

}  // namespace

Request request_from_json(const json& r) {
    Request q;
    q.id = r.at("id").get<std::string>();
    q.number = r.at("number").get<std::string>();
    q.from_station_id = r.at("from_station_id").get<std::string>();
    q.wagons_count = r.at("wagons_count").get<int>();
    q.wagon_kind = opt_str(r, "wagon_kind");
    q.train_id = opt_str(r, "train_id");
    q.train_length_m = opt_num(r, "train_length_m");
    q.priority = int_or(r, "priority", 0);
    q.split_allowed = r.value("split_allowed", false);
    q.desired_departure = parse_iso(r.at("desired_departure").get<std::string>());
    return q;
}

StationModel::StationModel(const json& s) {
    set_display_offset_min(s.at("tz_offset_min").get<int>());
    now = parse_iso(s.at("now").get<std::string>());
    version = s.at("version").get<long long>();
    cfg = s.at("station").at("config");
    sid = s.at("station").at("id").get<std::string>();
    station_name = s.at("station").at("name").get<std::string>();
    for (const auto& n : s.at("neighbors")) {
        Neighbor nb{n.at("id").get<std::string>(), n.at("name").get<std::string>(), n.at("config")};
        neighbors[nb.id] = nb;
    }
    std::vector<Node> nodes;
    for (const auto& n : s.at("nodes"))
        nodes.push_back({n.at("id").get<std::string>(), n.at("kind").get<std::string>(),
                         n.at("name").get<std::string>(), opt_str(n, "side")});
    std::vector<TrackRow> tracks;
    for (const auto& t : s.at("tracks")) {
        TrackRow r;
        r.id = t.at("id").get<std::string>();
        r.number = t.at("number").get<std::string>();
        r.kind = t.at("kind").get<std::string>();
        r.from_node = t.at("from_node").get<std::string>();
        r.to_node = t.at("to_node").get<std::string>();
        r.useful_length_m = opt_num(t, "useful_length_m");
        r.allowed_train_kinds = str_list(t, "allowed_train_kinds");
        r.zone_id = opt_str(t, "zone_id");
        track_rows[r.id] = r;
        tracks.push_back(r);
    }
    std::vector<Connection> conns;
    for (const auto& c : s.at("connections"))
        conns.push_back({c.at("id").get<std::string>(), c.at("from_node").get<std::string>(),
                         c.at("to_node").get<std::string>(), c.at("kind").get<std::string>(), opt_str(c, "track_id"),
                         c.at("length_m").get<double>()});
    topo = std::make_unique<Topology>(sid, nodes, tracks, conns);
    for (const auto& r : s.at("resources")) {
        Resource x{r.at("id").get<std::string>(), r.at("kind").get<std::string>(), r.at("name").get<std::string>(),
                   r.at("status").get<std::string>(), opt_str(r, "home_zone_id")};
        resources[x.id] = x;
    }
    for (const auto& [rid, lst] : s.at("shifts").items()) {
        auto& v = shifts[rid];
        for (const auto& p : lst) v.emplace_back(parse_iso(p[0].get<std::string>()), parse_iso(p[1].get<std::string>()));
    }
    for (const auto& w : s.at("maintenance"))
        maintenance.push_back({w.at("id").get<std::string>(), w.at("object_type").get<std::string>(),
                               w.at("object_id").get<std::string>(), w.at("reason").get<std::string>(),
                               parse_iso(w.at("start_at").get<std::string>()), parse_iso(w.at("end_at").get<std::string>())});
    for (const auto& i : s.at("incidents"))
        incidents.push_back({i.at("id").get<std::string>(), i.at("kind").get<std::string>(),
                             i.at("title").get<std::string>(), opt_str(i, "object_id"),
                             parse_iso(i.at("start_at").get<std::string>()), opt_time(i, "end_at")});
    for (const auto& t : s.at("trains")) {
        Train x;
        x.id = t.at("id").get<std::string>();
        x.number = t.at("number").get<std::string>();
        x.kind = t.at("kind").get<std::string>();
        x.priority = int_or(t, "priority", 1);
        x.destination_station_id = opt_str(t, "destination_station_id");
        x.arrival_side = t.at("arrival_side").get<std::string>();
        x.departure_side = t.at("departure_side").get<std::string>();
        x.wagons_count = int_or(t, "wagons_count", 0);
        x.length_m = opt_num(t, "length_m");
        x.loco_length_m = opt_num(t, "loco_length_m").value_or(0.0);
        x.status = t.at("status").get<std::string>();
        x.scheduled_arrival = opt_time(t, "scheduled_arrival");
        x.expected_arrival = opt_time(t, "expected_arrival");
        x.scheduled_departure = opt_time(t, "scheduled_departure");
        x.current_track_id = opt_str(t, "current_track_id");
        trains[x.id] = x;
    }
    for (const auto& o : s.at("operations")) {
        Operation x;
        x.id = o.at("id").get<std::string>();
        x.train_id = opt_str(o, "train_id");
        x.kind = o.at("kind").get<std::string>();
        x.seq = int_or(o, "seq", 0);
        x.track_id = opt_str(o, "track_id");
        x.from_track_id = opt_str(o, "from_track_id");
        x.side = opt_str(o, "side");
        x.duration_min = int_or(o, "duration_min", 0);
        x.requirements = str_list(o, "requirements");
        x.resource_ids = str_list(o, "resource_ids");
        x.route_nodes = str_list(o, "route_nodes");
        x.planned_start = parse_iso(o.at("planned_start").get<std::string>());
        x.planned_end = parse_iso(o.at("planned_end").get<std::string>());
        x.not_before = opt_time(o, "not_before");
        x.forecast_start = opt_time(o, "forecast_start");
        x.forecast_end = opt_time(o, "forecast_end");
        x.actual_start = opt_time(o, "actual_start");
        x.actual_end = opt_time(o, "actual_end");
        x.status = o.at("status").get<std::string>();
        x.extra_delay_min = int_or(o, "extra_delay_min", 0);
        x.reserved = o.value("reserved", false);
        x.note = opt_str(o, "note");
        ops[x.id] = x;
        if (x.train_id) ops_by_train[*x.train_id].push_back(x.id);
    }
    for (const auto& w : s.at("wagons")) {
        Wagon x{w.at("id").get<std::string>(), w.at("number").get<std::string>(), w.at("condition").get<std::string>(),
                opt_str(w, "train_id"), opt_num(w, "length_m")};
        if (x.train_id) wagons_by_train[*x.train_id].push_back(x);
    }
    for (const auto& r : s.at("rules")) {
        CapacityRule x;
        x.code = r.at("code").get<std::string>();
        x.name = r.at("name").get<std::string>();
        x.unit = r.value("unit", "");
        x.period = r.value("period", "");
        x.source = r.value("source", "");
        x.rule_text = r.value("rule_text", "");
        x.policy = r.value("policy", "");
        x.value = opt_num(r, "value");
        x.active = r.value("active", true);
        rules[x.code] = x;
    }
    if (!s.at("plan").is_null()) {
        const auto& p = s.at("plan");
        plan = PlanRow{p.at("target_wagons").get<int>(), p.at("actual_base_wagons").get<int>(),
                       p.at("policy").get<std::string>()};
    }
    for (const auto& r : s.at("reservations")) {
        Reservation x;
        x.id = r.at("id").get<long long>();
        x.resource_key = r.at("resource_key").get<std::string>();
        x.purpose = r.at("purpose").get<std::string>();
        x.train_id = opt_str(r, "train_id");
        x.operation_id = opt_str(r, "operation_id");
        x.request_id = opt_str(r, "request_id");
        x.start_at = parse_iso(r.at("start_at").get<std::string>());
        x.end_at = parse_iso(r.at("end_at").get<std::string>());
        reservations.push_back(x);
    }
    data_states = s.at("data_states");
    index_config = s.at("index_config");
    for (const auto& r : s.at("requests")) requests.push_back(request_from_json(r));
}

// ------------------------------------------------------------------ справочные
std::string StationModel::track_label(const OptStr& tid) const {
    if (!tid) return "—";
    const TrackRow* t = track_rows.find(*tid);
    if (!t) return "—";
    return (t->kind == "main" ? "Главный путь " : "Путь ") + t->number;
}

std::string StationModel::track_label_lc(const OptStr& tid) const { return lower_first(track_label(tid)); }

std::string StationModel::train_label(const OptStr& train_id) const {
    const Train* t = train(train_id);
    return t ? "поезд № " + t->number : "—";
}

int StationModel::zone_travel(const OptStr& a, const OptStr& b) const {
    if (!a || !b || a->empty() || b->empty() || *a == *b) return 0;
    const json tm = cfg.value("zone_travel_min", json::object());
    std::string ab = *a + "-" + *b, ba = *b + "-" + *a;
    if (tm.contains(ab)) return static_cast<int>(tm[ab].get<double>());
    if (tm.contains(ba)) return static_cast<int>(tm[ba].get<double>());
    if (tm.contains("default")) return static_cast<int>(tm["default"].get<double>());
    return 5;
}

OptStr StationModel::op_zone(const std::string& kind, const OptStr& track_id) const {
    if (kind == "inspection") return std::string("PTO");
    if (track_id) {
        const TrackRow* t = track_rows.find(*track_id);
        if (t && t->zone_id && !t->zone_id->empty()) return t->zone_id;
    }
    return std::nullopt;
}

bool StationModel::in_shift(const std::string& rid, Time s, Time e) const {
    const auto* sh = shifts.find(rid);
    if (!sh) return true;
    for (const auto& [a, b] : *sh)
        if (a <= s && e <= b) return true;
    return false;
}

std::optional<double> StationModel::train_length(const Train& t) const {
    if (t.length_m && *t.length_m != 0.0) return t.length_m;
    const auto* ws = wagons_by_train.find(t.id);
    if (!ws || ws->empty()) return std::nullopt;
    std::vector<double> lengths;
    for (const auto& w : *ws) {
        if (!w.length_m) return std::nullopt;
        lengths.push_back(*w.length_m);
    }
    return py_sum(lengths) + t.loco_length_m;
}

const Train* StationModel::train(const OptStr& id) const { return id ? trains.find(*id) : nullptr; }

std::vector<const Operation*> StationModel::train_ops(const std::string& train_id) const {
    std::vector<const Operation*> out;
    if (const auto* ids = ops_by_train.find(train_id))
        for (const auto& i : *ids) out.push_back(&ops.at(i));
    return out;
}

const json* StationModel::data_state(const std::string& track_id) const {
    auto it = data_states.find(track_id);
    return it == data_states.end() ? nullptr : &*it;
}

std::string StationModel::data_state_name(const std::string& track_id) const {
    const json* ds = data_state(track_id);
    if (!ds || !ds->contains("state") || (*ds)["state"].is_null()) return "";
    return (*ds)["state"].get<std::string>();
}

json StationModel::processing(const char* key, const json& def) const {
    const json& p = cfg.at("processing");
    auto it = p.find(key);
    return it == p.end() ? def : *it;
}

int StationModel::track_buffer_min() const { return processing("track_buffer_min", 5).get<int>(); }

// ------------------------------------------------------------------ книга интервалов модели
IntervalBook StationModel::build_book(bool include_reservations, bool include_data_blocks) const {
    IntervalBook b;
    if (include_reservations) {
        for (const auto& r : reservations) {
            EntryMeta m;
            m.type = "reservation";
            m.train_id = r.train_id;
            m.op_id = r.operation_id;
            m.label = r.purpose;
            b.add(r.resource_key, r.start_at, r.end_at, m);
        }
    }
    add_blocks(b, include_data_blocks);
    return b;
}

void StationModel::add_blocks(IntervalBook& b, bool include_data_blocks) const {
    for (const auto& inc : incidents) {
        Time s = inc.start_at, e = inc.end_at ? *inc.end_at : FAR;
        if (!inc.object_id || inc.object_id->empty()) continue;
        EntryMeta m;
        m.label = inc.title;
        std::string key;
        if (inc.kind == "track_closure") { key = "track:"; m.type = "closure"; }
        else if (inc.kind == "switch_failure") { key = "switch:"; m.type = "closure"; }
        else if (inc.kind == "resource_failure") { key = "res:"; m.type = "faulty"; }
        else if (inc.kind == "neighbor_restriction") { key = "neighbor:"; m.type = "restriction"; }
        else continue;
        b.add(key + *inc.object_id, s, e, m);
    }
    for (const auto& mw : maintenance) {
        std::string key = mw.object_type == "track" ? "track:" + mw.object_id
                          : mw.object_type == "resource" ? "res:" + mw.object_id
                                                          : "switch:" + mw.object_id;
        EntryMeta m;
        m.type = "maintenance";
        m.label = mw.reason;
        b.add(key, mw.start_at, mw.end_at, m);
    }
    for (const auto& [rid, r] : resources) {
        if (r.status == "faulty") {
            EntryMeta m;
            m.type = "faulty";
            m.label = r.name + " неисправен";
            b.add("res:" + rid, now - DAY, FAR, m);
        }
        const auto* shp = shifts.find(rid);
        if (shp && !shp->empty()) {
            auto sh = *shp;
            std::sort(sh.begin(), sh.end());
            Time cur = now - 2 * DAY;
            EntryMeta m;
            m.type = "shift";
            m.label = r.name + ": вне смены";
            for (const auto& [a, z] : sh) {
                if (a > cur) b.add("res:" + rid, cur, a, m);
                cur = std::max(cur, z);
            }
            b.add("res:" + rid, cur, FAR, m);
        }
    }
    if (include_data_blocks) {
        for (const auto& [tid, ds] : data_states.items()) {
            std::string st = ds.value("state", "");
            if (st == "stale" || st == "missing" || st == "contradictory" || st == "invalid") {
                EntryMeta m;
                m.type = "data";
                m.label = ds.contains("message") && !ds["message"].is_null() ? ds["message"].get<std::string>() : "None";
                m.data_state = st;
                b.add("track:" + tid, now - HOUR, FAR, m);
            }
        }
    }
}

// ------------------------------------------------------------------ стоянки и резервы
std::vector<Stay> stays_of(const std::vector<OpDict>& ops) {
    // Стоянка — непрерывный интервал занятия одного пути поездом.
    std::vector<Stay> stays;
    std::optional<Stay> cur;
    for (const auto& o : ops) {
        OptStr track = o.kind == "uncoupling" ? o.from_track_id : o.track_id;  // отцепка: состав остаётся на пути
        if (o.kind == "arrival" || o.kind == "shunting") {
            if (cur && o.kind == "shunting") {
                cur->end = o.end;  // путь отправления занят до окончания вытягивания состава
                cur->ops.push_back(o.id);
                stays.push_back(*cur);
            } else if (o.kind == "shunting" && o.from_track_id) {
                stays.push_back({o.from_track_id, o.start, o.end, {o.id}});
            }
            cur = Stay{track, o.start, o.end, {o.id}};
        } else {
            if (!cur) cur = Stay{track, o.start, o.end, {}};  // предыдущие операции уже выполнены
            cur->end = o.end;
            cur->ops.push_back(o.id);
            if (o.kind == "departure") {
                stays.push_back(*cur);
                cur.reset();
            }
        }
    }
    if (cur) stays.push_back(*cur);
    return stays;
}

static std::string key_of(const char* prefix, const OptStr& id) { return std::string(prefix) + (id ? *id : "None"); }

Strings through_tracks(const StationModel& m, const OpDict& o) {
    RoutePtr r;
    auto side_or = [&](const char* def) { return o.side && !o.side->empty() ? *o.side : std::string(def); };
    if (o.kind == "arrival") r = m.topo->arrival_route(side_or("west"), *o.track_id);
    else if (o.kind == "departure") r = m.topo->departure_route(side_or("east"), *o.track_id);
    else if (o.from_track_id) r = m.topo->shunting_route(*o.from_track_id, *o.track_id);
    Strings out;
    if (!r) return out;
    for (const auto& t : r->through_track_ids)
        if (t != o.track_id && t != o.from_track_id) out.push_back(t);
    return out;
}

std::vector<ReservationSpec> reservation_specs(const StationModel& m, const OptStr& train_id,
                                               const std::string& train_number, const std::vector<OpDict>& ops) {
    // Стоянки считаются по всей цепочке (включая выполненные операции); полностью завершённые
    // стоянки не резервируются.
    Duration buf = minutes(m.track_buffer_min());
    std::vector<ReservationSpec> specs;
    std::vector<OpDict> norm;
    for (const auto& o : ops)
        if (o.status != "cancelled") norm.push_back(o);
    std::unordered_map<std::string, OptStr> status;
    for (const auto& o : norm) status[o.id] = o.status;
    auto is_done = [&](const std::string& id) {
        auto it = status.find(id);
        return it != status.end() && it->second == "done";
    };
    for (const auto& st : stays_of(norm)) {
        if (std::all_of(st.ops.begin(), st.ops.end(), is_done)) continue;
        std::string op_id = st.ops[0];
        for (const auto& i : st.ops)
            if (!is_done(i)) { op_id = i; break; }
        specs.push_back({key_of("track:", st.track_id),
                         "Занятие " + m.track_label_lc(st.track_id) + " поездом № " + train_number, op_id, train_id,
                         st.start, st.end + buf});
    }
    for (const auto& o : norm) {
        if (o.status == "done") continue;
        std::string label = kind_label(o.kind) + " п. № " + train_number;
        if (o.kind == "uncoupling" && o.track_id)
            specs.push_back({"track:" + *o.track_id, "Подача неисправного вагона п. № " + train_number + " в депо", o.id,
                             train_id, o.start, o.end + buf});
        if (is_movement(o.kind)) {
            for (const auto& tt : through_tracks(m, o))
                specs.push_back({"track:" + tt, "Проследование: " + label, o.id, train_id, o.start, o.end});
            for (const auto& sw : o.route_nodes)
                specs.push_back({"switch:" + sw, label, o.id, train_id, o.start, o.end});
        }
        for (const auto& rid : o.resource_ids) {
            const Resource* r = m.resources.find(rid);
            Duration pad = minutes(m.zone_travel(r ? r->home_zone_id : std::nullopt, m.op_zone(o.kind, o.track_id)));
            specs.push_back({"res:" + rid, label, o.id, train_id, o.start - pad, o.end});
        }
    }
    return specs;
}

std::vector<OpDict> with_presence(const StationModel& m, const Train* train, const std::vector<OpDict>& live) {
    // Поезд уже стоит на пути: занятость начинается сейчас, а не с его следующей операции.
    if (!train || train->status != "on_station" || !train->current_track_id || train->current_track_id->empty() ||
        live.empty())
        return live;
    const OpDict& first = live[0];
    if (first.kind == "arrival") return live;
    OpDict p;
    p.id = "presence-" + train->id;
    p.kind = "dwell";
    p.track_id = train->current_track_id;
    p.start = p.end = std::min(m.now, first.start);
    p.status = "in_progress";
    p.train_id = train->id;
    std::vector<OpDict> out{p};
    out.insert(out.end(), live.begin(), live.end());
    return out;
}

}  // namespace station
