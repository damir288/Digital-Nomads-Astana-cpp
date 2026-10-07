// Детектор конфликтов прогнозного плана (аналог app/services/conflicts.py).
#include <algorithm>
#include <map>
#include <set>

#include "station/forecast.hpp"
#include "station/sha1.hpp"

namespace station {

namespace {

const Duration HORIZON = hours(12);
const Duration DATA_HORIZON = hours(6);

int severity_rank(const std::string& s) {
    if (s == "critical") return 0;
    if (s == "high") return 1;
    if (s == "medium") return 2;
    return 3;
}

std::string severity_label(const std::string& s) {
    if (s == "critical") return "Критично";
    if (s == "high") return "Высокая";
    if (s == "medium") return "Средняя";
    return "Низкая";
}

std::string str_or_none(const OptStr& s) { return s ? *s : "None"; }

// Идентификатор конфликта: "C-" + первые 10 символов sha1("|".join(parts)).
std::string cid(std::initializer_list<std::string> parts) {
    std::string joined;
    bool first = true;
    for (const auto& p : parts) {
        if (!first) joined += "|";
        joined += p;
        first = false;
    }
    return "C-" + sha1_hex(joined).substr(0, 10);
}

json jstr(const OptStr& s) { return s ? json(*s) : json(nullptr); }

json obj(const char* type, const OptStr& id, const std::string& label) {
    return json{{"type", type}, {"id", jstr(id)}, {"label", label}};
}

std::string tail_after_dash(const std::string& s) {  // c.split('-')[-1]
    auto p = s.rfind('-');
    return p == std::string::npos ? s : s.substr(p + 1);
}

// Стоянка по прогнозу с данными поезда.
struct FStay {
    OptStr track_id;
    Time start = 0, end = 0;
    Strings ops;
    OptStr train_id;
    std::string number;
    int priority = 1;
    bool started = false;
};

json make_conflict(const std::string& id, const char* type, const std::string& severity, const std::string& title,
                   const std::string& explanation, json objects, json operations, json start, json end) {
    return json{{"id", id},           {"type", type},           {"severity", severity},
                {"title", title},     {"explanation", explanation}, {"objects", std::move(objects)},
                {"operations", std::move(operations)}, {"start", std::move(start)}, {"end", std::move(end)}};
}

}  // namespace

int route_conflict_count(const json& conflicts) {
    int n = 0;
    for (const auto& c : conflicts) {
        std::string t = c.at("type").get<std::string>();
        if (t == "route_conflict" || t == "track_overlap" || t == "track_closed") ++n;
    }
    return n;
}

Detection detect(const StationModel& m, const Forecast& fc) {
    const Time now = m.now;
    const Time horizon_end = now + HORIZON;
    std::vector<json> conflicts;
    auto tl = [&](const OptStr& t) { return m.track_label(t); };
    auto tlc = [&](const OptStr& t) { return m.track_label_lc(t); };

    auto op_dict = [&](const Operation& o) {
        OpDict d;
        d.id = o.id;
        d.kind = o.kind;
        d.track_id = o.track_id;
        d.from_track_id = o.from_track_id;
        if (const auto* p = fc.find(o.id)) { d.start = p->first; d.end = p->second; }
        else { d.start = o.planned_start; d.end = o.planned_end; }
        d.status = o.status;
        d.resource_ids = o.resource_ids;
        d.route_nodes = o.route_nodes;
        d.train_id = o.train_id;
        d.side = o.side;
        return d;
    };

    // --- стоянки (занятость путей) по прогнозу
    std::vector<FStay> stays;
    for (const auto& [tid, ids] : m.ops_by_train) {
        const Train& train = m.trains.at(tid);
        if (train.status == "departed" || train.status == "completed" || train.status == "cancelled") continue;
        std::vector<OpDict> live;
        for (const auto& id : ids) {
            const Operation& o = m.ops.at(id);
            if (o.status != "done" && o.status != "cancelled") live.push_back(op_dict(o));
        }
        live = with_presence(m, &train, live);
        auto sts = stays_of(live);
        for (size_t idx = 0; idx < sts.size(); ++idx) {
            const Stay& st = sts[idx];
            Strings ops;
            for (const auto& i : st.ops)
                if (i.rfind("presence-", 0) != 0) ops.push_back(i);
            if (st.start > horizon_end) continue;
            bool started = false;
            for (const auto& i : ops)
                if (const Operation* op = m.ops.find(i); op && op->status == "in_progress") started = true;
            if (!started && idx == 0 && train.status == "on_station" && train.current_track_id == st.track_id)
                started = true;
            stays.push_back({st.track_id, st.start, st.end, ops, tid, train.number, train.priority, started});
        }
    }
    for (const auto& [id, o] : m.ops) {
        if (o.train_id || o.status == "done" || o.status == "cancelled") continue;
        OpDict d = op_dict(o);
        stays.push_back({o.track_id, d.start, d.end, {o.id}, std::nullopt, o.note ? *o.note : "операция", 1,
                         o.status == "in_progress"});
    }

    // группировка по пути (ключ None — отдельная группа, как в Python)
    OrderedMap<std::vector<FStay>> by_track;
    std::map<std::string, OptStr> track_of_key;
    for (const auto& st : stays) {
        std::string key = st.track_id ? "T:" + *st.track_id : std::string("N:");
        by_track[key].push_back(st);
        track_of_key[key] = st.track_id;
    }
    for (auto& [key, lst] : by_track) {
        const OptStr track_id = track_of_key[key];
        std::stable_sort(lst.begin(), lst.end(), [](const FStay& a, const FStay& b) { return a.start < b.start; });
        for (size_t i = 0; i < lst.size(); ++i) {
            for (size_t j = i + 1; j < lst.size(); ++j) {
                const FStay &a = lst[i], &b = lst[j];
                if (b.start >= a.end) break;
                if (a.train_id && a.train_id == b.train_id) continue;
                Time s = std::max(a.start, b.start), e = std::min(a.end, b.end);
                json objects = json::array({obj("track", track_id, tl(track_id))});
                for (const FStay* x : {&a, &b})
                    if (x->train_id) objects.push_back(obj("train", x->train_id, "Поезд № " + x->number));
                json ops = json::array();
                for (const auto& v : a.ops) ops.push_back(v);
                for (const auto& v : b.ops) ops.push_back(v);
                conflicts.push_back(make_conflict(
                    cid({"TRACK", str_or_none(track_id), str_or_none(a.train_id), str_or_none(b.train_id)}),
                    "track_overlap", std::min(a.start, b.start) - now < HOUR ? "critical" : "high",
                    "Пересечение занятости: " + tlc(track_id),
                    "По прогнозу " + tlc(track_id) + " нужен одновременно поезду № " + a.number + " (до " +
                        local_hm(a.end) + ") и поезду № " + b.number + " (с " + local_hm(b.start) +
                        "). Пересечение " + local_hm(s) + "–" + local_hm(e) + ".",
                    objects, ops, iso(s), iso(e)));
            }
        }
        // закрытия и окна обслуживания
        for (const auto& st : lst) {
            json st_ops = st.ops;
            for (const auto& inc : m.incidents) {
                if (inc.kind != "track_closure" || inc.object_id != track_id) continue;
                Time ie = inc.end_at ? *inc.end_at : FAR;
                if (!(st.start < ie && inc.start_at < st.end)) continue;
                std::string sev, expl;
                if (st.started) {
                    sev = "medium";
                    expl = "Поезд № " + st.number + " находится на " + tlc(track_id) + ", который закрыт (" + inc.title +
                           "). Новые операции на путь запрещены; отправление — по решению дежурного.";
                } else {
                    sev = "critical";
                    expl = "Запланированное занятие " + tlc(track_id) + " поездом № " + st.number + " (" +
                           local_hm(st.start) + "–" + local_hm(st.end) + ") попадает на закрытие: " + inc.title + " (" +
                           local_hm(inc.start_at) + "–" + (inc.end_at ? local_hm(*inc.end_at) : "до отмены") + ").";
                }
                json objects = json::array({obj("track", track_id, tl(track_id)), obj("incident", inc.id, inc.title)});
                if (st.train_id) objects.push_back(obj("train", st.train_id, "Поезд № " + st.number));
                conflicts.push_back(make_conflict(cid({"CLOSED", str_or_none(track_id), str_or_none(st.train_id), inc.id}),
                                                  "track_closed", sev, "Операция на закрытом пути: " + tlc(track_id), expl,
                                                  objects, st_ops, iso(std::max(st.start, inc.start_at)),
                                                  ie < FAR ? json(iso(std::min(st.end, ie))) : json(nullptr)));
            }
            for (const auto& mw : m.maintenance) {
                if (mw.object_type == "track" && track_id && mw.object_id == *track_id && !st.started &&
                    st.start < mw.end_at && mw.start_at < st.end) {
                    conflicts.push_back(make_conflict(
                        cid({"MW", str_or_none(track_id), str_or_none(st.train_id), mw.id}), "maintenance", "high",
                        "Пересечение с окном обслуживания: " + tlc(track_id),
                        "Занятие поездом № " + st.number + " (" + local_hm(st.start) + "–" + local_hm(st.end) +
                            ") пересекается с окном «" + mw.reason + "» (" + local_hm(mw.start_at) + "–" +
                            local_hm(mw.end_at) + ").",
                        json::array({obj("track", track_id, tl(track_id))}), st_ops, iso(st.start), iso(st.end)));
                }
            }
            const json* ds = track_id ? m.data_state(*track_id) : nullptr;
            if (ds && !ds->is_null()) {
                std::string state = ds->contains("state") && !(*ds)["state"].is_null() ? (*ds)["state"].get<std::string>() : "None";
                if (state != "actual" && state != "not_monitored" && !st.started && st.start - now < DATA_HORIZON) {
                    auto s_or = [&](const char* k) -> OptStr {
                        return ds->contains(k) && !(*ds)[k].is_null() ? OptStr((*ds)[k].get<std::string>()) : std::nullopt;
                    };
                    std::string device_name = s_or("device_name").value_or("None");
                    json dev = json{{"type", "device"}, {"id", jstr(s_or("device_id"))},
                                    {"label", ds->contains("device_name") ? (*ds)["device_name"] : json(nullptr)}};
                    (void)device_name;
                    conflicts.push_back(make_conflict(
                        cid({"DATA", str_or_none(track_id), str_or_none(st.train_id)}), "data_unknown", "high",
                        "Нет достоверных данных: " + tlc(track_id),
                        s_or("message").value_or("None") + " Зависимая операция: занятие пути поездом № " + st.number +
                            " с " + local_hm(st.start) + " — подтверждение и начало блокируются до получения данных.",
                        json::array({obj("track", track_id, tl(track_id)), dev}), st_ops, iso(st.start), iso(st.end)));
                }
            }
        }
    }

    // --- маршруты (общие стрелки) и ресурсы
    std::vector<OpDict> live_ops;
    for (const auto& [id, o] : m.ops) {
        if (o.status == "done" || o.status == "cancelled") continue;
        const auto* p = fc.find(id);
        if (p && p->first < horizon_end) live_ops.push_back(op_dict(o));
    }
    std::vector<OpDict> moves;
    for (const auto& o : live_ops)
        if (is_movement(o.kind) && !o.route_nodes.empty()) moves.push_back(o);
    std::stable_sort(moves.begin(), moves.end(), [](const OpDict& a, const OpDict& b) { return a.start < b.start; });
    std::set<std::pair<std::string, std::string>> seen;
    for (size_t i = 0; i < moves.size(); ++i) {
        for (size_t j = i + 1; j < moves.size(); ++j) {
            const OpDict &a = moves[i], &b = moves[j];
            if (b.start >= a.end) break;
            if (a.train_id == b.train_id) continue;
            std::set<std::string> sa(a.route_nodes.begin(), a.route_nodes.end());
            Strings common;
            for (const auto& x : std::set<std::string>(b.route_nodes.begin(), b.route_nodes.end()))
                if (sa.count(x)) common.push_back(x);  // std::set уже отсортирован
            if (common.empty()) continue;
            if (!seen.insert({a.id, b.id}).second) continue;
            const std::string& na = m.trains.at(*a.train_id).number;
            const std::string& nb = m.trains.at(*b.train_id).number;
            std::string sw_list;
            json objects = json::array();
            for (size_t k = 0; k < common.size(); ++k) {
                sw_list += (k ? ", " : "") + tail_after_dash(common[k]);
                objects.push_back(obj("switch", common[k], "Стрелка " + tail_after_dash(common[k])));
            }
            objects.push_back(obj("train", a.train_id, "Поезд № " + na));
            objects.push_back(obj("train", b.train_id, "Поезд № " + nb));
            conflicts.push_back(make_conflict(
                cid({"ROUTE", a.id, b.id}), "route_conflict", "high", "Конфликт маршрутов в горловине",
                kind_label(a.kind) + " поезда № " + na + " (" + local_hm(a.start) + "–" + local_hm(a.end) + ") и " +
                    lower_utf8(kind_label(b.kind)) + " поезда № " + nb + " (" + local_hm(b.start) + "–" +
                    local_hm(b.end) + ") используют общие стрелки: " + sw_list + ".",
                objects, json::array({a.id, b.id}), iso(b.start), iso(std::min(a.end, b.end))));
        }
    }
    auto tn = [&](const OpDict& o) {
        const Train* t = m.train(o.train_id);
        return t ? "поезд № " + t->number : std::string("операция");
    };
    OrderedMap<std::vector<OpDict>> by_res;
    for (const auto& o : live_ops)
        for (const auto& r : o.resource_ids) by_res[r].push_back(o);
    for (auto& [rid, lst] : by_res) {
        const Resource* res = m.resources.find(rid);
        if (!res) continue;
        std::stable_sort(lst.begin(), lst.end(), [](const OpDict& a, const OpDict& b) { return a.start < b.start; });
        for (size_t i = 0; i < lst.size(); ++i) {
            for (size_t j = i + 1; j < lst.size(); ++j) {
                const OpDict &a = lst[i], &b = lst[j];
                if (b.start >= a.end) break;
                conflicts.push_back(make_conflict(
                    cid({"RES", rid, a.id, b.id}), "resource_conflict", "high", "Ресурс нужен одновременно: " + res->name,
                    res->name + ": " + lower_utf8(kind_label(a.kind)) + " (" + tn(a) + ", до " + local_hm(a.end) + ") и " +
                        lower_utf8(kind_label(b.kind)) + " (" + tn(b) + ", с " + local_hm(b.start) +
                        ") пересекаются. Один ресурс не может выполнять две операции одновременно.",
                    json::array({obj("resource", rid, res->name)}), json::array({a.id, b.id}), iso(b.start), iso(a.end)));
            }
        }
        for (const auto& o : lst) {
            for (const auto& inc : m.incidents) {
                if (inc.kind != "resource_failure" || inc.object_id != rid) continue;
                Time ie = inc.end_at ? *inc.end_at : FAR;
                if (o.start < ie && inc.start_at < o.end && o.status != "in_progress")
                    conflicts.push_back(make_conflict(
                        cid({"RESF", rid, o.id}), "resource_unavailable", "high", "Ресурс недоступен: " + res->name,
                        kind_label(o.kind) + " (" + tn(o) + ", " + local_hm(o.start) + ") назначена на " + res->name +
                            ", но ресурс неисправен (" + inc.title + ").",
                        json::array({obj("resource", rid, res->name), obj("incident", inc.id, inc.title)}),
                        json::array({o.id}), iso(o.start), iso(o.end)));
            }
            if (!m.in_shift(rid, o.start, o.end) && o.status != "in_progress")
                conflicts.push_back(make_conflict(
                    cid({"SHIFT", rid, o.id}), "resource_unavailable", "medium", "Операция вне смены: " + res->name,
                    kind_label(o.kind) + " (" + tn(o) + ", " + local_hm(o.start) + "–" + local_hm(o.end) +
                        ") выходит за пределы смены ресурса «" + res->name + "».",
                    json::array({obj("resource", rid, res->name)}), json::array({o.id}), iso(o.start), iso(o.end)));
        }
    }

    // --- соседние станции, неспланированные требования, неисправные вагоны
    for (const auto& o : live_ops) {
        if (o.kind != "departure" || o.status == "in_progress") continue;
        const Train& train = m.trains.at(*o.train_id);
        const OptStr& dest = train.destination_station_id;
        for (const auto& inc : m.incidents) {
            if (inc.kind != "neighbor_restriction" || inc.object_id != dest) continue;
            Time ie = inc.end_at ? *inc.end_at : FAR;
            if (o.start < ie && inc.start_at <= o.end) {
                const Neighbor* nb = dest ? m.neighbors.find(*dest) : nullptr;
                conflicts.push_back(make_conflict(
                    cid({"NB", o.id, inc.id}), "neighbor_restriction", "high", "Отправление при ограничении соседней станции",
                    "Отправление поезда № " + train.number + " в " + local_hm(o.start) + " на " +
                        (nb ? nb->name : str_or_none(dest)) + " попадает в ограничение «" + inc.title + "» до " +
                        (inc.end_at ? local_hm(*inc.end_at) : "отмены") + ".",
                    json::array({obj("train", train.id, "Поезд № " + train.number), obj("incident", inc.id, inc.title)}),
                    json::array({o.id}), iso(o.start), iso(o.end)));
            }
        }
    }
    for (const auto& [id, o] : m.ops) {
        if (o.reserved || (o.status != "planned" && o.status != "confirmed")) continue;
        json objects = json::array({obj("track", o.track_id, tl(o.track_id))});
        if (o.train_id) objects.push_back(obj("train", o.train_id, "Поезд № " + m.trains.at(*o.train_id).number));
        const auto* p = fc.find(id);
        conflicts.push_back(make_conflict(
            cid({"UNPL", id}), "unplanned", "high", "Требуется планирование: " + lower_utf8(kind_label(o.kind)),
            (o.note && !o.note->empty() ? *o.note : kind_label(o.kind)) +
                ": операция ещё не размещена во времени и ресурсах. Пока она не спланирована, зависящие операции не начнутся.",
            objects, json::array({id}), iso(p ? p->first : o.planned_start), nullptr));
    }
    for (const auto& [tid, wl] : m.wagons_by_train) {
        std::vector<const Wagon*> bad;
        for (const auto& w : wl)
            if (w.condition == "faulty" || w.condition == "restricted") bad.push_back(&w);
        const Train* t = m.trains.find(tid);
        if (bad.empty() || !t || t->status == "departed" || t->status == "completed") continue;
        bool has_unc = false;
        json dep_ops = json::array();
        for (const Operation* o : m.train_ops(tid)) {
            if (o->kind == "uncoupling" && o->status != "cancelled") has_unc = true;
            if (o->kind == "departure") dep_ops.push_back(o->id);
        }
        if (has_unc) continue;
        std::string expl =
            bad[0]->condition == "restricted"
                ? "Вагон № " + bad[0]->number +
                      ": временное ограничение до проверки сообщения о предположительно критическом дефекте. "
                      "Отправление состава до решения моделью не допускается."
                : "Вагон № " + bad[0]->number +
                      " неисправен. Отправление состава с неисправным вагоном моделью не допускается до устранения и "
                      "контрольного осмотра (ремонт без отцепки) или отцепки — требуется решение диспетчера.";
        conflicts.push_back(make_conflict(
            cid({"FW", tid}), "faulty_wagon", "critical", "Неисправный вагон в составе поезда № " + t->number, expl,
            json::array({obj("train", tid, "Поезд № " + t->number), obj("wagon", bad[0]->id, "Вагон № " + bad[0]->number)}),
            dep_ops, iso(now), nullptr));
    }

    // Уникальность по id: позиция — первого вхождения, значение — последнего (как dict в Python).
    OrderedMap<json> uniq;
    for (auto& c : conflicts) uniq[c.at("id").get<std::string>()] = c;
    std::vector<json> sorted;
    for (const auto& [k, c] : uniq) sorted.push_back(c);
    std::stable_sort(sorted.begin(), sorted.end(), [](const json& a, const json& b) {
        int ra = severity_rank(a.at("severity").get<std::string>()), rb = severity_rank(b.at("severity").get<std::string>());
        if (ra != rb) return ra < rb;
        std::string sa = a.at("start").is_null() ? "" : a.at("start").get<std::string>();
        std::string sb = b.at("start").is_null() ? "" : b.at("start").get<std::string>();
        return sa < sb;
    });
    Detection out;
    for (auto& c : sorted) {
        c["detected_at"] = iso(now);
        c["severity_label"] = severity_label(c.at("severity").get<std::string>());
        out.conflicts.push_back(c);
    }
    auto delays = departure_delays(m, fc);
    std::vector<std::pair<std::string, double>> dl(delays.begin(), delays.end());
    std::stable_sort(dl.begin(), dl.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [tid, d] : dl) {
        if (d < 5) continue;
        const Train& t = m.trains.at(tid);
        if (t.status == "departed" || t.status == "completed") continue;
        out.delays.push_back(json{{"train_id", tid}, {"number", t.number}, {"delay_min", py_round0(d)},
                                  {"priority", t.priority}});
    }
    return out;
}

}  // namespace station
