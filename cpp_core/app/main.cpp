// station_core — консольная программа ядра «Цифровой станции» на C++.
//
//   station_core golden <snapshot.json>                 все расчёты в формате эталона Python (для сравнения)
//   station_core check  <snapshot.json> [id заявки]     проверка заявки(ок) на приём состава
//   station_core conflicts <snapshot.json>              прогноз конфликтов и задержек
//   station_core index  <snapshot.json>                 индекс эффективности
//   station_core plan   <snapshot.json> [секунды] [потоки]  перепланирование (CP-SAT + проверка + эвристика)
//   station_core cpsat  <snapshot.json> [секунды] [потоки]  только CP-SAT: статус, цель, независимая проверка
//
// Результат печатается в stdout как JSON (UTF-8).
#include <fstream>
#include <iostream>
#include <set>

#include "station/checker.hpp"
#include "station/forecast.hpp"
#include "station/index.hpp"
#include "station/planner.hpp"

using namespace station;

namespace {

json load(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("не удалось открыть " + path);
    return json::parse(f);
}

json forecast_json(const Forecast& fc) {
    json out = json::object();
    for (const auto& [id, se] : fc) out[id] = json::array({iso(se.first), iso(se.second)});
    return out;
}

json checks_json(const StationModel& m, const std::string& only = "") {
    json out = json::object();
    for (const auto& req : m.requests) {
        if (!only.empty() && req.id != only) continue;
        IntervalBook book = m.build_book();
        out[req.id] = check_request(m, req, book);
    }
    return out;
}

json golden(const StationModel& m) {
    Forecast fc = forecast(m);
    Detection det = detect(m, fc);
    json out;
    out["forecast"] = forecast_json(fc);
    out["conflicts"] = det.conflicts;
    out["delays"] = det.delays;
    out["checks"] = checks_json(m);
    out["index"] = compute_current(m, fc, det.conflicts);

    PlanBuilder pb(m, fc);
    json stays = json::array();
    for (const auto& st : pb.stays) {
        json ops = json::array();
        for (const auto& p : st.ops) ops.push_back(pb.info(p).op->id);
        stays.push_back(json{{"track0", st.track0 ? json(*st.track0) : json(nullptr)},
                             {"cands", st.cands},
                             {"fixed", st.fixed},
                             {"ops", ops}});
    }
    out["plan_builder"] = json{{"stays", stays}, {"unresolved", pb.unresolved}};

    PlanResult g = pb.solve_greedy();
    std::set<std::string> unresolved_tracks;
    for (const auto& u : pb.unresolved)
        if (u.contains("track_id") && !u["track_id"].is_null()) unresolved_tracks.insert(u["track_id"].get<std::string>());
    Schedule before;
    for (const auto& [oid, se] : fc) {
        const Operation* o = m.ops.find(oid);
        if (!o || o->status == "done" || o->status == "cancelled") continue;
        ScheduleEntry e;
        e.start = se.first;
        e.end = se.second;
        e.has_track = false;
        before[oid] = e;
    }
    json kpi_before = plan_kpis(m, before, det.conflicts.size());
    json kpi_after = plan_kpis(m, g.schedule, 0);
    out["greedy"] = json{{"status", g.status},
                         {"schedule", schedule_to_json(g.schedule)},
                         {"unresolved", g.unresolved},
                         {"verify", verify(m, g.schedule, unresolved_tracks)},
                         {"changes", diff_schedule(m, g.schedule)},
                         {"kpi_before", kpi_before},
                         {"kpi_after", kpi_after},
                         {"index_before", compute_plan_index(m, nullptr, kpi_before)},
                         {"index_after", compute_plan_index(m, &g.schedule, kpi_after)}};
    return out;
}

void usage() {
    std::cerr << "Использование:\n"
                 "  station_core golden <snapshot.json>\n"
                 "  station_core check <snapshot.json> [id заявки]\n"
                 "  station_core conflicts <snapshot.json>\n"
                 "  station_core index <snapshot.json>\n"
                 "  station_core plan <snapshot.json> [лимит, с] [потоки]\n"
                 "  station_core cpsat <snapshot.json> [лимит, с] [потоки]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    try {
        std::string cmd = argv[1];
        StationModel m(load(argv[2]));
        json out;
        if (cmd == "golden") {
            out = golden(m);
        } else if (cmd == "check") {
            out = checks_json(m, argc > 3 ? argv[3] : "");
        } else if (cmd == "conflicts") {
            Forecast fc = forecast(m);
            Detection d = detect(m, fc);
            out = json{{"conflicts", d.conflicts}, {"delays", d.delays}};
        } else if (cmd == "index") {
            Forecast fc = forecast(m);
            out = compute_current(m, fc, detect(m, fc).conflicts);
        } else if (cmd == "cpsat") {
#ifdef STATION_WITH_ORTOOLS
            double limit = argc > 3 ? std::stod(argv[3]) : 20.0;
            int workers = argc > 4 ? std::stoi(argv[4]) : 8;
            Forecast fc = forecast(m);
            PlanBuilder pb(m, fc);
            PlanResult r = pb.solve_cpsat(limit, workers);
            std::set<std::string> unresolved_tracks;
            for (const auto& u : pb.unresolved)
                if (u.contains("track_id") && !u["track_id"].is_null()) unresolved_tracks.insert(u["track_id"].get<std::string>());
            Strings errs;
            if (r.status == "optimal" || r.status == "feasible") errs = verify(m, r.schedule, unresolved_tracks);
            out = json{{"status", r.status}, {"objective", r.objective ? json(*r.objective) : json(nullptr)},
                       {"solve_ms", r.solve_ms}, {"verify", errs}};
#else
            throw std::runtime_error("программа собрана без OR-Tools");
#endif
        } else if (cmd == "plan") {
            double limit = argc > 3 ? std::stod(argv[3]) : 3.5;
            int workers = argc > 4 ? std::stoi(argv[4]) : 8;
            out = run_planner(m, limit, workers);
        } else {
            usage();
            return 2;
        }
        std::cout << out.dump(1) << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
