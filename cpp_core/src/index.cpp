// Индекс эффективности станции (аналог app/services/index.py).
#include "station/index.hpp"

#include <algorithm>

#include "station/planner.hpp"

namespace station {

namespace {

struct ComponentInfo {
    const char *key, *title, *unit, *source;
};

const ComponentInfo COMPONENTS[] = {
    {"throughput", "Пропускная способность", "ваг./ч", "Журнал операций: завершённые техосмотры (обработанные вагоны)"},
    {"schedule_deviation", "Среднее отклонение от графика", "мин", "Отправления: факт и прогноз относительно расписания"},
    {"track_utilization", "Загрузка путей приёмо-отправочного парка", "доля путей",
     "Телеметрия рельсовых цепей (только актуальные данные)"},
    {"route_conflicts", "Конфликты маршрутов и занятости", "шт.", "Детектор конфликтов прогнозного плана"},
    {"idle", "Простой локомотивов и бригад сверх резерва", "доля смены", "Журнал операций и смены ресурсов"},
};

const ComponentInfo& comp_info(const std::string& key) {
    for (const auto& c : COMPONENTS)
        if (key == c.key) return c;
    throw std::runtime_error("unknown index component " + key);
}

// Составляющая индекса: {key, title, unit, source, raw, score, explanation, window_min}.
json component(const std::string& key, json raw, std::optional<double> score, const std::string& explanation,
               json window_min) {
    const auto& ci = comp_info(key);
    return json{{"key", key},       {"title", ci.title},
                {"unit", ci.unit},  {"source", ci.source},
                {"raw", raw},       {"score", score ? json(py_round(*score, 3)) : json(nullptr)},
                {"explanation", explanation}, {"window_min", window_min}};
}

json aggregate(const json& cfg, const json& comps) {
    const json& w = cfg.at("weights");
    std::vector<double> ws, aws, nums;
    for (const auto& [k, v] : w.items()) ws.push_back(v.get<double>());
    std::vector<std::pair<std::string, double>> factors;
    json missing = json::array();
    for (const auto& [k, c] : comps.items()) {
        if (c.at("score").is_null()) {
            missing.push_back(comp_info(k).title);
            continue;
        }
        double wk = w.at(k).get<double>();
        aws.push_back(wk);
        nums.push_back(wk * c.at("score").get<double>());
    }
    // суммы — как sum() в Python, в порядке словаря составляющих
    double total_w = py_sum(ws), aw = py_sum(aws);
    std::optional<double> value;
    if (aw > 0) value = py_round(100 * py_sum(nums) / aw, 1);
    double coverage = total_w ? aw / total_w : 0;
    for (const auto& [k, c] : comps.items())
        if (!c.at("score").is_null())
            factors.emplace_back(k, w.at(k).get<double>() * (1 - c.at("score").get<double>()) / total_w * 100);
    std::stable_sort(factors.begin(), factors.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string cat = index_category(value, cfg.at("thresholds"));
    std::string quality = coverage >= 0.999 ? "full" : (coverage >= 0.6 ? "partial" : "low");
    static const std::map<std::string, std::string> CAT = {
        {"normal", "Норма"}, {"attention", "Внимание"}, {"critical", "Критично"}, {"unknown", "Нет оценки"}};
    static const std::map<std::string, std::string> QL = {{"full", "Полная"}, {"partial", "Частичная"}, {"low", "Низкая"}};
    json jf = json::array();
    for (const auto& [k, v] : factors) {
        if (v < 0.5) continue;
        if (jf.size() >= 3) break;
        jf.push_back(json{{"key", k}, {"title", comp_info(k).title}, {"loss_points", py_round(v, 1)},
                          {"explanation", comps.at(k).at("explanation")}});
    }
    return json{{"value", value ? json(*value) : json(nullptr)},
                {"category", cat},
                {"category_label", CAT.at(cat)},
                {"quality", json{{"level", quality}, {"coverage", py_round(coverage, 3)}, {"label", QL.at(quality)},
                                 {"missing", missing}}},
                {"components", comps},
                {"factors", jf}};
}

}  // namespace

double utilization_score(double u, const json& p) {
    double lo = p.at("target_low").get<double>(), hi = p.at("target_high").get<double>();
    double z0 = p.value("zero_at_low", 0.0), z1 = p.value("zero_at_high", 1.0);
    if (lo <= u && u <= hi) return 1.0;
    if (u < lo) return lo > z0 ? std::max(0.0, (u - z0) / (lo - z0)) : 0.0;
    return z1 > hi ? std::max(0.0, (z1 - u) / (z1 - hi)) : 0.0;
}

std::string index_category(std::optional<double> value, const json& th) {
    if (!value) return "unknown";
    if (*value >= th.at("normal").get<double>()) return "normal";
    if (*value >= th.at("attention").get<double>()) return "attention";
    return "critical";
}

json compute_current(const StationModel& m, const Forecast& fc, const json& conflicts) {
    const json& cfg = m.index_config.at("config");
    const json& p = cfg.at("params");
    const Time now = m.now;
    json comps = json::object();
    // 1. пропускная способность: вагоны, прошедшие техосмотр за окно
    const json& pt = p.at("throughput");
    long long win_min = pt.at("window_min").get<long long>();
    Duration win = minutes(win_min);
    long long wagons = 0;
    for (const auto& [tid, ids] : m.ops_by_train) {
        const Train& t = m.trains.at(tid);
        for (const auto& id : ids) {
            const Operation& o = m.ops.at(id);
            if (o.kind == "inspection" && o.status == "done" && o.actual_end && now - win <= *o.actual_end &&
                *o.actual_end <= now)
                wagons += t.wagons_count;
        }
    }
    double rate = wagons / (win_min / 60.0);
    double target = pt.at("target_wagons_per_hour").get<double>();
    std::optional<double> s1;
    if (target) s1 = std::min(1.0, rate / target);
    comps["throughput"] = component("throughput", py_round(rate, 1), s1,
                                    fmt_f(rate, 0) + " ваг./ч при цели " + py_str(pt.at("target_wagons_per_hour")) +
                                        " ваг./ч за последние " + py_str(pt.at("window_min")) + " мин",
                                    pt.at("window_min"));
    // 2. отклонение от графика: факт за окно и прогноз на 1 ч
    const json& ps = p.at("schedule_deviation");
    Duration win2 = minutes(ps.at("window_min").get<long long>());
    std::vector<double> delays;
    for (const auto& [tid, ids] : m.ops_by_train) {
        const Train& t = m.trains.at(tid);
        const Operation* dep = nullptr;
        for (const auto& id : ids)
            if (m.ops.at(id).kind == "departure") { dep = &m.ops.at(id); break; }
        if (!dep || !t.scheduled_departure) continue;
        Time sched = *t.scheduled_departure;
        if (dep->status == "done" && dep->actual_start && now - win2 <= *dep->actual_start && *dep->actual_start <= now)
            delays.push_back(std::max(0.0, minutes_f(*dep->actual_start - sched)));
        else if (dep->status != "done" && fc.contains(dep->id) && fc.at(dep->id).first <= now + HOUR)
            delays.push_back(std::max(0.0, minutes_f(fc.at(dep->id).first - sched)));
    }
    if (!delays.empty()) {
        double avg = py_sum(delays) / delays.size();
        double s2 = std::max(0.0, 1 - avg / ps.at("max_avg_delay_min").get<double>());
        comps["schedule_deviation"] = component("schedule_deviation", py_round(avg, 1), s2,
                                                "среднее " + fmt_f(avg, 0) + " мин по " + std::to_string(delays.size()) +
                                                    " отправлениям (факт и прогноз на 1 ч)",
                                                ps.at("window_min"));
    } else {
        comps["schedule_deviation"] =
            component("schedule_deviation", nullptr, std::nullopt, "нет отправлений в окне — нет данных", ps.at("window_min"));
    }
    // 3. загрузка путей по актуальной телеметрии
    Strings rd, known;
    for (const auto& [tid, t] : m.track_rows)
        if (t.kind == "receiving_departure") rd.push_back(tid);
    for (const auto& tid : rd)
        if (m.data_state_name(tid) == "actual") known.push_back(tid);
    const json& pu = p.at("track_utilization");
    json max_age = cfg.value("max_data_age_s", json(30));
    if (!known.empty() && known.size() >= rd.size() * 0.5) {
        long long occ = 0;
        for (const auto& tid : known) {
            const json* ds = m.data_state(tid);
            if (ds->contains("observed") && (*ds)["observed"] == "occupied") ++occ;
        }
        double u = static_cast<double>(occ) / known.size();
        std::string note = known.size() == rd.size()
                               ? ""
                               : "; без данных: " + std::to_string(rd.size() - known.size()) + " путь(и) исключены";
        comps["track_utilization"] = component(
            "track_utilization", py_round(u, 3), utilization_score(u, pu),
            "занято " + std::to_string(occ) + " из " + std::to_string(known.size()) + " путей (" + fmt_pct(u, 0) +
                "), целевой диапазон " + fmt_pct(pu.at("target_low").get<double>(), 0) + "–" +
                fmt_pct(pu.at("target_high").get<double>(), 0) + note,
            0);
    } else {
        comps["track_utilization"] =
            component("track_utilization", nullptr, std::nullopt,
                      "актуальные данные телеметрии есть только по " + std::to_string(known.size()) + " из " +
                          std::to_string(rd.size()) + " путей (порог устаревания до " + py_str(max_age) +
                          " с) — оценка не выполняется",
                      0);
    }
    // 4. конфликты
    int n = route_conflict_count(conflicts);
    const json& mx = p.at("route_conflicts").at("max_conflicts");
    comps["route_conflicts"] = component("route_conflicts", n, std::max(0.0, 1 - n / mx.get<double>()),
                                         std::to_string(n) + " конфликт(ов) в прогнозе (0 баллов при " + py_str(mx) + ")", 0);
    // 5. простой локомотивов и бригад сверх резерва
    const json& pi = p.at("idle");
    Duration win5 = minutes(pi.at("window_min").get<long long>());
    Time t0 = now - win5;
    double shift_min = 0, busy_min = 0;
    for (const auto& [rid, r] : m.resources) {
        if (r.kind != "shunting_loco" && r.kind != "loco_crew" && r.kind != "shunting_crew") continue;
        std::vector<std::pair<Time, Time>> sh{{t0, now}};
        if (const auto* s = m.shifts.find(rid); s && !s->empty()) sh = *s;
        for (const auto& [a, b] : sh) {
            Time a2 = std::max(a, t0), b2 = std::min(b, now);
            if (b2 > a2) shift_min += minutes_f(b2 - a2);
        }
        for (const auto& [oid, o] : m.ops) {
            if (!o.actual_start || std::find(o.resource_ids.begin(), o.resource_ids.end(), rid) == o.resource_ids.end())
                continue;
            Time a2 = std::max(*o.actual_start, t0);
            Time b2 = std::min(o.actual_end ? *o.actual_end : now, now);
            if (b2 > a2) busy_min += minutes_f(b2 - a2);
        }
    }
    if (shift_min > 0) {
        double idle = std::max(0.0, 1 - busy_min / shift_min);
        double rs = pi.at("reserve_share").get<double>();
        double excess = std::max(0.0, idle - rs);
        double s5 = std::max(0.0, 1 - excess / (1 - rs));
        comps["idle"] = component("idle", py_round(idle, 3), s5,
                                  "простой " + fmt_pct(idle, 0) + " смены при необходимом резерве " + fmt_pct(rs, 0) +
                                      " (сверх резерва " + fmt_pct(excess, 0) + ")",
                                  pi.at("window_min"));
    } else {
        comps["idle"] = component("idle", nullptr, std::nullopt, "нет данных о сменах ресурсов в окне", pi.at("window_min"));
    }
    json out = aggregate(cfg, comps);
    out["computed_at"] = iso(now);
    out["config_version"] = m.index_config.at("version");
    out["mode"] = "current";
    out["formula"] = "I = 100 × Σ(wᵢ·sᵢ) / Σwᵢ";
    out["weights"] = cfg.at("weights");
    out["thresholds"] = cfg.at("thresholds");
    return out;
}

json compute_plan_index(const StationModel& m, const Schedule* schedule, const json& kpis) {
    const json& cfg = m.index_config.at("config");
    const json& p = cfg.at("params");
    const Time now = m.now;
    const Duration h = hours(4);
    bool use_sched = schedule && !schedule->empty();
    json comps = json::object();
    long long wagons = 0;
    for (const auto& [tid, ids] : m.ops_by_train) {
        const Train& t = m.trains.at(tid);
        for (const auto& id : ids) {
            const Operation& o = m.ops.at(id);
            if (o.kind != "inspection" || o.status == "done") continue;
            const ScheduleEntry* e = use_sched ? schedule->find(id) : nullptr;
            Time end = e ? e->end : o.forecast_end.value_or(o.planned_end);
            if (now <= end && end <= now + h) wagons += t.wagons_count;
        }
    }
    double rate = wagons / 4.0;
    double target = p.at("throughput").at("target_wagons_per_hour").get<double>();
    std::optional<double> s1;
    if (target) s1 = std::min(1.0, rate / target);
    comps["throughput"] = component("throughput", py_round(rate, 1), s1, "план: " + fmt_f(rate, 0) + " ваг./ч на 4 ч", 240);
    // число поездов с ещё не выполненным отправлением в плане
    long long n_dep = 0;
    for (const auto& [tid, ids] : m.ops_by_train)
        for (const auto& id : ids) {
            const Operation& o = m.ops.at(id);
            if (o.kind == "departure" && o.status != "done" && (!schedule || schedule->contains(id))) {
                ++n_dep;
                break;
            }
        }
    double avg = kpis.value("total_delay_min", 0.0) / std::max<long long>(1, n_dep);
    comps["schedule_deviation"] =
        component("schedule_deviation", py_round(avg, 1),
                  std::max(0.0, 1 - avg / p.at("schedule_deviation").at("max_avg_delay_min").get<double>()),
                  "план: средняя задержка " + fmt_f(avg, 0) + " мин", 240);
    double u = kpis.value("rd_utilization_4h", 0.0);
    comps["track_utilization"] = component("track_utilization", kpis.value("rd_utilization_4h", json(0)),
                                           utilization_score(u, p.at("track_utilization")),
                                           "план: загрузка парка " + fmt_pct(u, 0) + " за 4 ч", 240);
    long long n = kpis.value("conflicts", 0LL);
    comps["route_conflicts"] =
        component("route_conflicts", n, std::max(0.0, 1 - n / p.at("route_conflicts").at("max_conflicts").get<double>()),
                  "план: " + std::to_string(n) + " конфликт(ов)", 0);
    comps["idle"] = component("idle", nullptr, std::nullopt, "простой в прогнозе не оценивается", 0);
    json out = aggregate(cfg, comps);
    out["mode"] = "forecast";
    out["config_version"] = m.index_config.at("version");
    out["computed_at"] = iso(now);
    return out;
}

}  // namespace station
