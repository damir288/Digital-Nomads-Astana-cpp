// Перепланирование (аналог app/services/planner.py): эвристика earliest-fit, CP-SAT
// (если сборка с OR-Tools) и независимая проверка плана книгой интервалов.
//
// Модель CP-SAT (минуты от текущего модельного времени):
//   * переменные: начало каждой не начатой операции; выбор пути для каждой стоянки; выбор
//     ресурса для каждого требования операции;
//   * жёсткие ограничения: порядок операций, «не ранее», не более одной стоянки на пути
//     одновременно, закрытия и окна обслуживания, стрелки маршрута, ресурсы в смене, ограничения
//     соседей для отправлений;
//   * цель: взвешенная задержка (вес 2^(приоритет-1)) + штрафы за изменения плана.
// План, не прошедший независимую проверку, не предлагается.
#pragma once

#include <set>

#include "station/index.hpp"
#include "station/placement.hpp"

namespace station {

struct ScheduleEntry {
    Time start = 0, end = 0;
    OptStr track_id, from_track_id;
    Strings resource_ids, route_nodes;
    bool fixed = false;
    bool kept = false;      // оставлено как в текущем плане
    bool has_track = true;  // false — только время (прогноз «до» планирования)
};

using Schedule = OrderedMap<ScheduleEntry>;  // id операции -> размещение (порядок важен для проверки)

json schedule_to_json(const Schedule& s);

struct OpInfo {
    const Operation* op = nullptr;
    int idx = 0;
    bool fixed = false;
    long long lb = 0;      // нижняя граница начала, мин от t_base
    long long dur = 0;
    long long start0 = 0;  // текущее плановое начало (для штрафа сдвига)
    bool confirmed = false;
    std::optional<int> stay;
};

struct StayInfo {
    int idx = 0;
    std::vector<std::pair<int, int>> ops;  // (цепочка, позиция в цепочке)
    OptStr track0;
    Strings cands;
    bool fixed = false, unresolved = false, confirmed = false;
    const Train* train = nullptr;
    OptStr chosen;
};

struct PlanResult {
    std::string status, solver;
    double solve_ms = 0;
    Schedule schedule;
    std::optional<double> objective;
    json unresolved = json::array();
    Strings notes;
};

class PlanBuilder {
public:
    PlanBuilder(const StationModel& m, const Forecast& fc);

    PlanResult solve_greedy();
#ifdef STATION_WITH_ORTOOLS
    PlanResult solve_cpsat(double time_limit_s, int workers, double deterministic_time = 0);
#endif

    std::vector<std::vector<OpInfo>> chains;
    std::vector<StayInfo> stays;
    json unresolved = json::array();
    Time t_base = 0;

    long long to_min(Time t) const { return floor_div(t - t_base, MINUTE); }
    Time to_time(long long m) const { return t_base + minutes(m); }
    const OpInfo& info(const std::pair<int, int>& p) const { return chains[p.first][p.second]; }

private:
    void collect();
    void add_chain(const std::vector<const Operation*>& live, const Train* train);
    Strings candidates(const OptStr& track0, const Train* train, const std::vector<const OpInfo*>& infos) const;
    std::pair<PlanResult, std::set<int>> greedy_pass(const std::set<int>& kept_chains);

    const StationModel& m_;
    const Forecast& fc_;
    int buf_;
    double margin_;
};

Strings verify(const StationModel& m, const Schedule& schedule, const std::set<std::string>& unresolved_tracks);
json diff_schedule(const StationModel& m, const Schedule& schedule);
json plan_kpis(const StationModel& m, const Schedule& sched, size_t conflicts);

// Полный цикл перепланирования, как run_planner(): CP-SAT -> проверка -> эвристика при отказе.
json run_planner(const StationModel& m, double time_limit_s, int workers);

}  // namespace station
