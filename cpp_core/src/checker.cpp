// Проверка заявки на приём (аналог app/services/checker.py).
#include "station/checker.hpp"

#include <algorithm>
#include <cmath>

namespace station {

namespace {

int status_rank(const std::string& s) {
    if (s == "ok") return 0;
    if (s == "warning") return 1;
    if (s == "insufficient_data") return 2;
    return 3;  // fail
}

json jopt(const OptStr& s) { return s ? json(*s) : json(nullptr); }

// Пункт проверки: {code, title, status, message, unit, period, source, rule, values}.
json item(const std::string& code, const std::string& title, const std::string& status, const std::string& message,
          json values = json::object(), json meta = json::object()) {
    json j{{"code", code},     {"title", title},     {"status", status}, {"message", message},
           {"unit", nullptr},  {"period", nullptr},  {"source", nullptr}, {"rule", nullptr},
           {"values", values.is_null() ? json::object() : values}};
    for (auto it = meta.begin(); it != meta.end(); ++it) j[it.key()] = it.value();
    return j;
}

json rule_meta(const StationModel& m, const std::string& code) {
    const CapacityRule* r = m.rules.find(code);
    if (!r) return json::object();
    return json{{"unit", r->unit}, {"period", r->period}, {"source", r->source}, {"rule", r->rule_text}};
}

std::string side_of(const StationModel& m, const std::string& from_id) {
    const Neighbor* n = m.neighbors.find(from_id);
    return n ? n->config.value("side", "west") : std::string("west");
}

std::string ddmm(const CivilTime& c, bool with_year) {
    char buf[16];
    if (with_year) std::snprintf(buf, sizeof buf, "%02d.%04d", c.month, c.year);
    else std::snprintf(buf, sizeof buf, "%02d.%02d", c.day, c.month);
    return buf;
}

std::string upper_ascii(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

struct PlanFigures {
    std::optional<int> target;
    int fact = 0, agreed = 0;
    std::string policy;
};

PlanFigures plan_figures(const StationModel& m, Time month_start, Time month_end) {
    // Факт и согласованные будущие объёмы за месяц, вагонов (по окончанию техосмотра).
    int done = 0, agreed = 0;
    for (const auto& [tid, t] : m.trains) {
        if ((t.kind != "freight" && t.kind != "transfer") || t.status == "cancelled") continue;
        const Operation* insp = nullptr;
        for (const Operation* o : m.train_ops(tid))
            if (o->kind == "inspection") { insp = o; break; }
        if (!insp) continue;
        Time when = insp->actual_end.value_or(insp->planned_end);
        if (!(month_start <= when && when < month_end)) continue;
        (insp->status == "done" ? done : agreed) += t.wagons_count;
    }
    PlanFigures pf;
    if (m.plan) pf.target = m.plan->target_wagons;
    pf.fact = (m.plan ? m.plan->actual_base_wagons : 0) + done;
    pf.agreed = agreed;
    pf.policy = m.plan ? m.plan->policy : "soft";
    return pf;
}

std::string summarize_failure(Time arrival, int wagons, const PlaceResult& res, const StationModel& m) {
    // Сводка отказа из причин по путям-кандидатам (вычисляется, а не задаётся текстом).
    const std::string NONE = std::string("\0None", 5);
    OrderedMap<Strings> by_code;
    for (const auto& [tid, r] : res.track_reasons) by_code[r.code ? *r.code : NONE].push_back(tid);
    std::string head = "Приём " + std::to_string(wagons) + " ваг. в " + local_hm(arrival) + " недоступен";
    OrderedMap<Strings> usable;
    for (const auto& [c, v] : by_code)
        if (c != "LENGTH" && c != "INCOMPATIBLE") usable[c] = v;
    if (usable.empty()) return head + ": нет пути достаточной полезной длины.";
    bool only_busy = true;
    for (const auto& [c, v] : usable)
        if (c != "OCCUPIED" && c != "CLOSED" && c != "DATA") only_busy = false;
    if (only_busy && !usable.contains("DATA")) return head + ": нет подходящего свободного пути на время приёма и обработки.";

    auto get = [&](const char* c) { const Strings* v = usable.find(c); return v ? *v : Strings{}; };
    auto nums = [&](Strings ids) {
        std::stable_sort(ids.begin(), ids.end(), [&](const std::string& a, const std::string& b) {
            return zfill(m.track_rows.at(a).number, 3) < zfill(m.track_rows.at(b).number, 3);
        });
        std::string s;
        for (size_t i = 0; i < ids.size(); ++i) s += (i ? ", " : "") + m.track_rows.at(ids[i]).number;
        return s;
    };
    Strings parts;
    if (usable.contains("RESOURCE") || usable.contains("RESOURCE_NONE")) {
        Strings ids = get("RESOURCE");
        for (const auto& x : get("RESOURCE_NONE")) ids.push_back(x);
        parts.push_back("на путях " + nums(ids) + " нет свободных ресурсов обработки на нужный интервал");
    }
    if (usable.contains("OCCUPIED") || usable.contains("CLOSED")) {
        Strings ids = get("OCCUPIED");
        for (const auto& x : get("CLOSED")) ids.push_back(x);
        parts.push_back("пути " + nums(ids) + " заняты или закрыты");
    }
    if (usable.contains("DATA")) parts.push_back("по путям " + nums(get("DATA")) + " нет достоверных данных датчиков");
    if (usable.contains("ROUTE")) parts.push_back("маршрут приёма на пути " + nums(get("ROUTE")) + " занят");
    std::string s = head + ": ";
    for (size_t i = 0; i < parts.size(); ++i) s += (i ? "; " : "") + parts[i];
    return s + ".";
}

Time stay_end(const std::vector<PlacedOp>& ops, const std::string& track) {
    OptTime end;
    for (const auto& o : ops)
        if (o.track_id == track || o.from_track_id == track) end = o.end;
    return end ? *end : ops.back().end;
}

}  // namespace

std::pair<std::optional<double>, std::string> train_length_for_request(const StationModel& m, const Request& req) {
    if (req.train_length_m && *req.train_length_m != 0.0) return {*req.train_length_m, "указана в заявке"};
    json lengths = m.processing("wagon_lengths_m", json::object());
    if (req.wagon_kind && !req.wagon_kind->empty() && lengths.contains(*req.wagon_kind)) {
        double loco = m.processing("default_loco_length_m", 34.0).get<double>();
        double per = lengths[*req.wagon_kind].get<double>();
        double L = req.wagons_count * per + loco;
        return {py_round(L, 1), std::to_string(req.wagons_count) + " ваг. × " + py_str(lengths[*req.wagon_kind]) +
                                    " м + локомотив " + fmt_g(loco) + " м"};
    }
    return {std::nullopt, "нет общей длины и длины вагона"};
}

json check_static_rules(const StationModel& m, const Request& req, Time arrival, std::optional<double> length,
                        int wagons) {
    json items = json::array();
    // 1. месячный план — целевой показатель или жёсткая квота (месяц — в поясе станции)
    CivilTime loc = to_local(arrival);
    CivilTime ms_c{loc.year, loc.month, 1, 0, 0, 0, 0};
    CivilTime me_c = ms_c.month == 12 ? CivilTime{loc.year + 1, 1, 1, 0, 0, 0, 0} : CivilTime{loc.year, loc.month + 1, 1, 0, 0, 0, 0};
    Time ms = from_local(ms_c), me = from_local(me_c);
    PlanFigures pf = plan_figures(m, ms, me);
    json meta = rule_meta(m, "MONTHLY_PLAN");
    const std::string title = "Месячный план обработки";
    if (!pf.target) {
        items.push_back(item("MONTHLY_PLAN", title, "insufficient_data",
                             "План месяца не задан — оценить выполнение невозможно.", json::object(), meta));
    } else {
        int target = *pf.target;
        int projected = pf.fact + pf.agreed + wagons;
        int over = projected - target;
        json vals{{"target", target}, {"fact", pf.fact},     {"agreed", pf.agreed}, {"request", wagons},
                  {"projected", projected}, {"policy", pf.policy}, {"month", ddmm(ms_c, true)}};
        std::string P = std::to_string(projected), T = std::to_string(target), O = std::to_string(over);
        if (over <= 0)
            items.push_back(item("MONTHLY_PLAN", title, "ok",
                                 "С учётом заявки: " + P + " из " + T + " ваг. (факт " + std::to_string(pf.fact) +
                                     ", согласовано " + std::to_string(pf.agreed) + "). Превышения нет.",
                                 vals, meta));
        else if (pf.policy == "hard_quota")
            items.push_back(item("MONTHLY_PLAN", title, "fail",
                                 "Жёсткая квота: с заявкой будет " + P + " ваг. при квоте " + T + " (превышение на " +
                                     O + "). Политика «жёсткая квота» запрещает новые заявки.",
                                 vals, meta));
        else
            items.push_back(item("MONTHLY_PLAN", title, "warning",
                                 std::string("План месяца ") + (pf.fact >= target ? "уже выполнен" : "будет превышен") +
                                     ": с заявкой " + P + " из " + T + " ваг. (превышение на " + O +
                                     "). Политика «целевой показатель»: заявка допускается с предупреждением.",
                                 vals, meta));
    }
    // 2. перерабатывающая способность за сутки (местные сутки)
    const CapacityRule* rule = m.rules.find("DAILY_PROCESSING");
    if (rule && rule->active && rule->value && *rule->value != 0.0) {
        CivilTime d0c{loc.year, loc.month, loc.day, 0, 0, 0, 0};
        Time d0 = from_local(d0c), d1 = d0 + DAY;
        int day_wagons = 0;
        for (const auto& [tid, t] : m.trains)
            if ((t.kind == "freight" || t.kind == "transfer") && t.status != "cancelled" && t.expected_arrival &&
                d0 <= *t.expected_arrival && *t.expected_arrival < d1)
                day_wagons += t.wagons_count;
        int total = day_wagons + wagons;
        std::string st = total <= *rule->value ? "ok" : (rule->policy == "hard" ? "fail" : "warning");
        items.push_back(item("DAILY_PROCESSING", rule->name, st,
                             "За сутки " + ddmm(d0c, false) + " в переработку: " + std::to_string(day_wagons) +
                                 " ваг. + заявка " + std::to_string(wagons) + " = " + std::to_string(total) + " из " +
                                 fmt_f(*rule->value, 0) + " ваг./сут." +
                                 (st == "ok" ? "" : " Превышение перерабатывающей способности."),
                             json{{"planned", day_wagons}, {"request", wagons}, {"limit", *rule->value}},
                             rule_meta(m, "DAILY_PROCESSING")));
    }
    // 3. пропускная способность входа (поездов в час)
    std::string side = side_of(m, req.from_station_id);
    rule = m.rules.find("ENTRY_THROUGHPUT_" + upper_ascii(side));
    if (rule && rule->active && rule->value && *rule->value != 0.0) {
        Time w0 = arrival - minutes(30), w1 = arrival + minutes(30);
        int cnt = 0;
        for (const auto& [tid, t] : m.trains) {
            if (t.status == "cancelled" || t.arrival_side != side) continue;
            const Operation* arr = nullptr;
            for (const Operation* o : m.train_ops(tid))
                if (o->kind == "arrival") { arr = o; break; }
            if (!arr || arr->status == "done") continue;
            Time when = arr->forecast_start.value_or(arr->planned_start);
            if (w0 <= when && when < w1) ++cnt;
        }
        int total = cnt + 1;
        std::string st = total <= *rule->value ? "ok" : "fail";
        items.push_back(item(rule->code, rule->name, st,
                             "Прибытий с этой стороны в окне " + local_hm(w0) + "–" + local_hm(w1) + ": " +
                                 std::to_string(cnt) + " + заявка = " + std::to_string(total) + " при норме " +
                                 fmt_f(*rule->value, 0) + " поезд./ч." +
                                 (st == "ok" ? "" : " Пропускная способность входа исчерпана."),
                             json{{"count", cnt}, {"limit", *rule->value}}, rule_meta(m, rule->code)));
    }
    // 4. данные о длине
    if (!length)
        items.push_back(item("TRAIN_LENGTH", "Длина состава", "insufficient_data",
                             "Недостаточно данных: в заявке нет длины состава и вида вагонов с известной длиной. "
                             "Проверка соответствия полезной длине путей невозможна.",
                             json::object(),
                             json{{"unit", "м"}, {"period", "на момент прибытия"}, {"source", "Заявка"},
                                  {"rule", "Длина состава + запас ≤ полезной длины пути. Количество вагонов не заменяет длину."}}));
    return items;
}

json neighbor_checks(const StationModel& m, const IntervalBook& book, const Request& req, Time departure, Time) {
    json items = json::array();
    const Neighbor* origin = m.neighbors.find(req.from_station_id);
    if (!origin) {
        items.push_back(item("ORIGIN", "Станция отправления", "insufficient_data",
                             "Станция " + req.from_station_id + " не найдена в модели.", json::object(),
                             json{{"source", "Справочник станций"}}));
        return items;
    }
    auto restr = book.conflicts("neighbor:" + origin->id, departure, departure + minutes(10));
    if (!restr.empty())
        items.push_back(item("ORIGIN_RESTRICTION", "Ограничение станции " + origin->name, "fail",
                             "Отправление со станции " + origin->name + " в " + local_hm(departure) +
                                 " невозможно: " + describe_block(restr[0]) + ".",
                             json::object(), json{{"source", "Состояние соседней станции (упрощённое)"}}));
    json locos = origin->config.value("locomotives_available", json(1));
    if (locos.get<double>() <= 0)
        items.push_back(item("ORIGIN_LOCO", "Поездной локомотив", "fail",
                             "На станции " + origin->name + " нет свободного поездного локомотива.", json::object(),
                             json{{"unit", "локомотивов"}, {"source", "Состояние соседней станции (упрощённое)"}}));
    return items;
}

static TrainSpec spec_for_request(const StationModel& m, const Request& req, Time arrival, std::optional<double> length,
                                  int wagons) {
    const json& proc = m.cfg.at("processing");
    const json& tpl = proc.at("templates").at(proc.at("transfer_template").get<std::string>());
    std::string side = side_of(m, req.from_station_id);
    TrainSpec s;
    s.train_id = "REQ-" + req.id;
    s.number = "заявка " + req.number;
    s.kind = "transfer";
    s.priority = req.priority ? req.priority : 2;
    s.length_m = length;
    s.side_in = side;
    s.side_out = side == "west" ? "east" : "west";
    s.templ = steps_from_json(tpl);
    s.arrival = arrival;
    s.wagons = wagons;
    return s;
}

PlaceResult place_request(const StationModel& m, const IntervalBook& book, const Request& req, Time arrival,
                          std::optional<double> length, int wagons, const OptStr& ignore_train) {
    Placer placer(m, book, 60, ignore_train);
    return placer.place(spec_for_request(m, req, arrival, length, wagons), true);
}

int travel_min(const StationModel& m, const std::string& from_id) {
    const Neighbor* n = m.neighbors.find(from_id);
    return n ? static_cast<int>(n->config.value("travel_min", json(90)).get<double>()) : 90;
}

json check_request(const StationModel& m, const Request& req, const IntervalBook& book, bool with_alternatives) {
    Time departure = req.desired_departure;
    int tmin = travel_min(m, req.from_station_id);
    Time arrival = departure + minutes(tmin);
    auto [length, length_src] = train_length_for_request(m, req);
    const OptStr& ignore = req.train_id;  // собственные резервы подтверждённой заявки не мешают
    json items = check_static_rules(m, req, arrival, length, req.wagons_count);
    for (auto& i : neighbor_checks(m, book, req, departure, arrival)) items.push_back(i);
    if (departure < m.now)
        items.push_back(item("TIME_PAST", "Время отправления", "fail",
                             "Время отправления " + local_hm(departure) + " уже прошло (модельное время " +
                                 local_hm(m.now) + ").",
                             json::object(), json{{"unit", "время"}, {"source", "Модельные часы"}}));
    PlaceResult placement = place_request(m, book, req, arrival, length, req.wagons_count, ignore);
    json track_items = json::array();
    for (const auto& [tid, r] : placement.track_reasons)
        track_items.push_back(json{{"track_id", tid}, {"label", m.track_label(tid)}, {"code", jopt(r.code)},
                                   {"message", r.message}});
    json assignment = nullptr;
    if (placement.ok) {
        const auto& ops = placement.ops;
        const std::string& main_track = ops[0].track_id;
        ConflictFilter f;
        f.ignore_train = ignore;
        bool free_now = book.conflicts("track:" + main_track, m.now, m.now + MINUTE, f).empty();
        auto occ = book.conflicts("track:" + main_track, m.now, ops[0].start, f);
        std::string note;
        if (!occ.empty()) {
            const Entry* last = &occ[0];
            for (const auto& e : occ)
                if (e.end > last->end) last = &e;
            note = " Сейчас путь занят (" + describe_block(*last) + "), освободится к " + local_hm(last->end) +
                   " — до начала приёма.";
        }
        items.push_back(item("TIME_WINDOW", "Путь и интервал приёма", "ok",
                             m.track_label(main_track) + " свободен на весь интервал приёма и обработки " +
                                 local_hm(ops[0].start) + "–" + local_hm(stay_end(ops, main_track)) + "." + note,
                             json{{"track_id", main_track}, {"free_now", free_now}}, rule_meta(m, "TIME_WINDOW")));
        std::set<std::string> rs;
        for (const auto& o : ops) rs.insert(o.resource_ids.begin(), o.resource_ids.end());
        Strings res_list(rs.begin(), rs.end());
        std::string msg = "Ресурсы не требуются.";
        if (!res_list.empty()) {
            msg = "Назначены: ";
            for (size_t i = 0; i < res_list.size(); ++i) msg += (i ? ", " : "") + m.resources.at(res_list[i]).name;
            msg += ".";
        }
        items.push_back(item("RESOURCES", "Ресурсы обработки", "ok", msg, json{{"resources", res_list}},
                             rule_meta(m, "RESOURCES")));
        json jops = json::array();
        Strings tracks;
        for (const auto& o : ops) {
            jops.push_back(o.as_dict());
            if (o.kind == "arrival" || o.kind == "shunting") tracks.push_back(o.track_id);
        }
        assignment = json{{"ops", jops}, {"tracks", tracks}, {"resources", res_list}};
    } else {
        std::string msg = summarize_failure(arrival, req.wagons_count, placement, m);
        bool only_insufficient = placement.failure_codes.size() == 1 && placement.failure_codes.count("INSUFFICIENT_DATA");
        std::string st = (!length || only_insufficient) ? "insufficient_data" : "fail";
        items.push_back(item("TIME_WINDOW", "Путь и интервал приёма", st, msg, json{{"tracks", track_items}},
                             rule_meta(m, "TIME_WINDOW")));
    }

    int worst = 0;
    for (const auto& i : items) worst = std::max(worst, status_rank(i.at("status").get<std::string>()));
    static const char* DEC[] = {"available", "available_with_warnings", "insufficient_data", "unavailable"};
    std::string decision = DEC[worst];
    if (decision == "insufficient_data")
        for (const auto& i : items)
            if (i.at("status") == "fail") decision = "unavailable";

    std::optional<Window> window;
    json alternatives = json::array();
    if (decision == "unavailable" && length && with_alternatives) {
        auto alt = build_alternatives(m, book, req, departure, arrival, *length, items);
        alternatives = alt.first;
        window = alt.second;
    }

    // сводка
    std::string summary;
    if (decision == "available" || decision == "available_with_warnings") {
        const std::string& tr = placement.ops[0].track_id;
        summary = "Приём " + std::to_string(req.wagons_count) + " ваг. в " + local_hm(arrival) + " возможен: " +
                  m.track_label_lc(tr) + ", обработка до " + local_hm(stay_end(placement.ops, tr)) + ".";
        for (const auto& i : items)
            if (i.at("status") == "warning") {
                summary += " Предупреждение: " + i.at("message").get<std::string>();
                break;
            }
    } else {
        std::vector<const json*> fails;
        for (const auto& i : items)
            if (i.at("status") == "fail") fails.push_back(&i);
        if (fails.empty()) {
            const json* miss = nullptr;
            for (const auto& i : items)
                if (i.at("status") == "insufficient_data") { miss = &i; break; }
            summary = "Недостаточно данных для решения: " +
                      (miss ? miss->at("message").get<std::string>() : std::string("проверьте исходные данные."));
        } else {
            const json* main = fails[0];
            for (const json* f : fails)
                if (f->at("code") == "TIME_WINDOW") { main = f; break; }
            summary = main->at("message").get<std::string>();
            if (window) {
                while (!summary.empty() && summary.back() == '.') summary.pop_back();
                summary += ". Ближайшее допустимое окно — " + local_hm(window->arrival) + ".";
            } else if (main->at("code") == "TIME_WINDOW") {
                summary += " Допустимого окна в ближайшие 12 ч не найдено.";
            }
        }
    }

    json missing = json::array();
    for (const auto& i : items)
        if (i.at("status") == "insufficient_data") missing.push_back(i.at("message"));
    for (const auto& s : placement.insufficient) missing.push_back(s);

    static const std::map<std::string, std::string> LABEL = {
        {"available", "Приём возможен"}, {"available_with_warnings", "Приём возможен с предупреждениями"},
        {"unavailable", "Приём недоступен"}, {"insufficient_data", "Недостаточно данных"}};
    json out;
    out["decision"] = decision;
    out["decision_label"] = LABEL.at(decision);
    out["summary"] = summary;
    out["departure"] = iso(departure);
    out["arrival"] = iso(arrival);
    out["travel_min"] = tmin;
    out["train_length_m"] = length ? json(*length) : json(nullptr);
    out["train_length_source"] = length_src;
    out["wagons"] = req.wagons_count;
    out["items"] = items;
    out["tracks"] = track_items;
    out["assignment"] = assignment;
    out["nearest_window"] = window ? json{{"arrival", iso(window->arrival)}, {"departure", iso(window->departure)},
                                          {"track_id", window->track_id}, {"delay_min", window->delay_min}}
                                   : json(nullptr);
    out["alternatives"] = alternatives;
    out["missing_data"] = missing;
    out["computed_at"] = iso(m.now);
    out["state_version"] = m.version;
    out["assumptions"] = json::array({"Время хода от станции отправления — " + std::to_string(tmin) + " мин (норматив модели).",
                                      "Интервал занятия пути: от прибытия до окончания обработки и вытягивания + технологический запас.",
                                      "Схема станции, нормы и показатели демонстрационные."});
    return out;
}

}  // namespace station
