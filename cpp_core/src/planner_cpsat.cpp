// Модель CP-SAT для перепланирования (аналог PlanBuilder.solve_cpsat из planner.py).
// Собирается только с OR-Tools (макрос STATION_WITH_ORTOOLS).
#ifdef STATION_WITH_ORTOOLS

#include <chrono>
#include <cmath>
#include <map>

#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/sat/sat_parameters.pb.h"
#include "station/planner.hpp"

namespace station {

namespace sat = operations_research::sat;
using operations_research::Domain;

namespace {
constexpr long long H_MAX = 24 * 60;  // горизонт планирования, мин

std::string note_wagon(const OptStr& note) {  // (o.note or "").split("№")[-1].strip()
    std::string s = note.value_or("");
    const std::string mark = "№";
    auto p = s.rfind(mark);
    if (p != std::string::npos) s = s.substr(p + mark.size());
    size_t b = s.find_first_not_of(" \t\n"), e = s.find_last_not_of(" \t\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}
}  // namespace

PlanResult PlanBuilder::solve_cpsat(double time_limit_s, int workers, double deterministic_time) {
    const StationModel& m = m_;
    sat::CpModelBuilder mdl;
    std::map<std::string, sat::IntVar> S, E;

    // --- переменные времени и порядок операций
    for (const auto& chain : chains) {
        const OpInfo* prev = nullptr;
        for (const auto& ci : chain) {
            const std::string& id = ci.op->id;
            if (ci.fixed) {
                S.emplace(id, mdl.NewConstant(ci.lb));
                E.emplace(id, mdl.NewConstant(std::max(ci.lb + ci.dur, 0LL)));
            } else {
                long long lb = std::max(ci.lb, 0LL);
                auto s = mdl.NewIntVar(Domain(lb, H_MAX)).WithName("s_" + id);
                auto e = mdl.NewIntVar(Domain(lb + ci.dur, H_MAX + ci.dur)).WithName("e_" + id);
                mdl.AddEquality(e, sat::LinearExpr(s) + ci.dur);
                S.emplace(id, s);
                E.emplace(id, e);
            }
            if (prev && !ci.fixed) mdl.AddGreaterOrEqual(S.at(id), E.at(prev->op->id));
            prev = &ci;
        }
    }

    // --- выбор путей стоянок: на пути не более одной стоянки одновременно
    std::map<std::pair<int, std::string>, sat::BoolVar> X;
    std::map<std::string, std::vector<sat::IntervalVar>> track_iv, track_iv_free;
    for (const auto& st : stays) {
        const OpInfo& first = info(st.ops.front());
        const OpInfo& last = info(st.ops.back());
        sat::LinearExpr st_s = S.at(first.op->id);
        if (st.fixed && !first.fixed) st_s = mdl.NewConstant(0);  // поезд уже стоит: путь занят с текущего момента
        sat::IntVar st_e = E.at(last.op->id);
        auto size = mdl.NewIntVar(Domain(0, 2 * H_MAX));
        auto end_b = mdl.NewIntVar(Domain(-H_MAX, 3 * H_MAX));
        mdl.AddEquality(end_b, sat::LinearExpr(st_e) + (last.fixed ? 0 : buf_));
        mdl.AddEquality(size, sat::LinearExpr(end_b) - st_s);
        std::vector<sat::BoolVar> lits;
        for (const auto& t : st.cands) {
            auto x = mdl.NewBoolVar();
            X.emplace(std::make_pair(st.idx, t), x);
            lits.push_back(x);
            auto iv = mdl.NewOptionalIntervalVar(st_s, size, end_b, x);
            track_iv[t].push_back(iv);
            if (!st.fixed && !st.unresolved) track_iv_free[t].push_back(iv);
        }
        mdl.AddExactlyOne(lits);
    }
    // блокировки путей (закрытия, окна обслуживания, смены; без блоков «нет данных»)
    IntervalBook blocks;
    m.add_blocks(blocks, false);
    auto block_span = [&](const Entry& en, long long lo) {
        long long a = std::max(to_min(en.start), lo);
        long long b = std::min(en.end < FAR ? to_min(en.end) : 3 * H_MAX, 3 * H_MAX);
        return std::make_pair(a, b);
    };
    for (const auto& [t, row] : m.track_rows) {
        if (const auto* lst = blocks.entries("track:" + t))
            for (const auto& en : *lst) {
                auto [a, b] = block_span(en, -H_MAX);
                if (b > a && b > 0) track_iv_free[t].push_back(mdl.NewIntervalVar(a, b - a, b));
            }
    }
    // отцепка вагона занимает и путь депо; ремонт — только после отцепки
    std::map<std::string, std::string> unc_by_wagon;
    for (const auto& chain : chains)
        for (const auto& ci : chain) {
            const Operation& o = *ci.op;
            if (o.kind != "uncoupling" || !o.track_id) continue;
            auto ue = mdl.NewIntVar(Domain(-H_MAX, 3 * H_MAX));
            auto iv = mdl.NewIntervalVar(S.at(o.id), ci.dur + buf_, ue);
            mdl.AddEquality(iv.EndExpr(), sat::LinearExpr(E.at(o.id)) + buf_);
            track_iv[*o.track_id].push_back(iv);
            if (!ci.fixed) track_iv_free[*o.track_id].push_back(iv);
            unc_by_wagon[note_wagon(o.note)] = o.id;
        }
    for (const auto& chain : chains)
        for (const auto& ci : chain) {
            const Operation& o = *ci.op;
            if (o.kind != "repair" || ci.fixed) continue;
            auto it = unc_by_wagon.find(note_wagon(o.note));
            if (it != unc_by_wagon.end()) mdl.AddGreaterOrEqual(S.at(o.id), E.at(it->second));
        }
    for (auto& [t, ivs] : track_iv) mdl.AddNoOverlap(ivs);
    for (auto& [t, ivs] : track_iv_free) mdl.AddNoOverlap(ivs);

    // --- маршруты: через стрелку одновременно проходит одно движение
    struct RouteOption {
        sat::BoolVar p;
        RoutePtr r;
        std::string to;
        OptStr from;
    };
    std::map<std::string, std::vector<RouteOption>> route_choice;
    std::map<std::string, std::vector<sat::IntervalVar>> sw_iv, sw_iv_free;
    std::vector<std::string> sw_free_order;
    const Topology& topo = *m.topo;
    for (const auto& chain : chains)
        for (const auto& ci : chain) {
            const Operation& o = *ci.op;
            if (!is_movement(o.kind)) continue;
            if (ci.fixed) {
                for (const auto& sw : o.route_nodes) sw_iv[sw].push_back(mdl.NewIntervalVar(S.at(o.id), ci.dur, E.at(o.id)));
                continue;
            }
            const StayInfo& cur = stays[*ci.stay];
            std::vector<RouteOption> options;
            std::string side_w = o.side && !o.side->empty() ? *o.side : "west";
            std::string side_e = o.side && !o.side->empty() ? *o.side : "east";
            if (o.kind == "arrival") {
                for (const auto& t : cur.cands)
                    if (auto r = topo.arrival_route(side_w, t)) options.push_back({X.at({cur.idx, t}), r, t, std::nullopt});
            } else if (o.kind == "departure") {
                for (const auto& t : cur.cands)
                    if (auto r = topo.departure_route(side_e, t)) options.push_back({X.at({cur.idx, t}), r, t, std::nullopt});
            } else if (o.kind == "uncoupling") {
                for (const auto& t : cur.cands)
                    if (auto r = topo.shunting_route(t, *o.track_id)) options.push_back({X.at({cur.idx, t}), r, *o.track_id, t});
            } else {  // манёвры: из предыдущей стоянки в текущую
                std::optional<int> prev_idx;
                if (*ci.stay > 0 && stays[*ci.stay - 1].train == cur.train) prev_idx = *ci.stay - 1;
                Strings prev_cands = prev_idx ? stays[*prev_idx].cands : Strings{o.from_track_id.value_or("")};
                for (const auto& f : prev_cands)
                    for (const auto& t : cur.cands) {
                        auto r = topo.shunting_route(f, t);
                        if (!r) continue;
                        auto b = mdl.NewBoolVar();
                        auto xt = X.at({cur.idx, t});
                        if (prev_idx) {
                            auto xf = X.at({*prev_idx, f});
                            mdl.AddBoolAnd({xf, xt}).OnlyEnforceIf(b);
                            mdl.AddBoolOr({xf.Not(), xt.Not()}).OnlyEnforceIf(b.Not());
                        } else {
                            mdl.AddEquality(b, xt);
                        }
                        options.push_back({b, r, t, f});
                    }
            }
            if (options.empty()) {
                unresolved.push_back(json{{"train", cur.train ? json(cur.train->number) : json(nullptr)},
                                          {"reason", "нет маршрута для операции «" + kind_label(o.kind) + "»"}});
                continue;
            }
            std::vector<sat::BoolVar> ps;
            for (const auto& opt : options) ps.push_back(opt.p);
            mdl.AddExactlyOne(ps);
            for (const auto& opt : options)
                for (const auto& sw : opt.r->switch_ids) {
                    auto iv = mdl.NewOptionalIntervalVar(S.at(o.id), ci.dur, E.at(o.id), opt.p);
                    sw_iv[sw].push_back(iv);
                    if (!sw_iv_free.count(sw)) sw_free_order.push_back(sw);
                    sw_iv_free[sw].push_back(iv);
                }
            route_choice[o.id] = options;
        }
    for (auto& [sw, lst] : sw_iv) mdl.AddNoOverlap(lst);
    for (const auto& sw : sw_free_order) {
        auto& lst = sw_iv_free[sw];
        if (const auto* ens = blocks.entries("switch:" + sw))
            for (const auto& en : *ens) {
                auto [a, b] = block_span(en, -H_MAX);
                if (b > a && b > 0) lst.push_back(mdl.NewIntervalVar(a, b - a, b));
            }
        mdl.AddNoOverlap(lst);
    }

    // --- ресурсы: одна операция одновременно, время перехода между зонами, только в смене
    std::map<std::string, std::vector<sat::IntervalVar>> res_iv, res_iv_free;
    std::map<std::pair<std::string, int>, std::vector<std::pair<sat::BoolVar, std::string>>> res_choice;
    std::vector<std::pair<std::string, int>> res_choice_order;
    for (const auto& chain : chains)
        for (const auto& ci : chain) {
            const Operation& o = *ci.op;
            if (ci.fixed) {
                for (const auto& rid : o.resource_ids)
                    res_iv[rid].push_back(mdl.NewIntervalVar(S.at(o.id), ci.dur, E.at(o.id)));
                continue;
            }
            // путь операции может смениться — время перехода берём максимальным по допустимым путям
            std::vector<OptStr> zones;
            if (o.kind == "uncoupling" || !ci.stay) {
                zones.push_back(m.op_zone(o.kind, o.track_id));
            } else {
                for (const auto& t : stays[*ci.stay].cands) zones.push_back(m.op_zone(o.kind, t));
                if (zones.empty()) zones.push_back(m.op_zone(o.kind, o.track_id));
            }
            for (size_t k = 0; k < o.requirements.size(); ++k) {
                const std::string& rk = o.requirements[k];
                Strings cands;
                for (const auto& [rid, row] : m.resources)
                    if (row.kind == rk) cands.push_back(rid);
                if (cands.empty()) {
                    unresolved.push_back(json{{"train", nullptr}, {"reason", "на станции нет ресурса вида «" + rk + "»"}});
                    continue;
                }
                std::vector<std::pair<sat::BoolVar, std::string>> ys;
                for (const auto& rid : cands) {
                    auto y = mdl.NewBoolVar();
                    ys.push_back({y, rid});
                    int pad = 0;
                    bool first = true;
                    for (const auto& z : zones) {
                        int v = m.zone_travel(m.resources.at(rid).home_zone_id, z);
                        pad = first ? v : std::max(pad, v);
                        first = false;
                    }
                    auto st_pad = mdl.NewIntVar(Domain(-H_MAX, H_MAX + 1));
                    mdl.AddEquality(st_pad, sat::LinearExpr(S.at(o.id)) - pad);
                    auto iv = mdl.NewOptionalIntervalVar(st_pad, ci.dur + pad, E.at(o.id), y);
                    res_iv[rid].push_back(iv);
                    res_iv_free[rid].push_back(iv);
                }
                std::vector<sat::BoolVar> yb;
                for (const auto& [y, rid] : ys) yb.push_back(y);
                mdl.AddExactlyOne(yb);
                res_choice[{o.id, static_cast<int>(k)}] = ys;
                res_choice_order.push_back({o.id, static_cast<int>(k)});
            }
        }
    for (auto& [rid, lst] : res_iv) mdl.AddNoOverlap(lst);
    for (const auto& [rid, row] : m.resources) {
        auto it = res_iv_free.find(rid);
        if (it == res_iv_free.end() || it->second.empty()) continue;
        auto& free = it->second;
        if (const auto* ens = blocks.entries("res:" + rid))
            for (const auto& en : *ens) {
                auto [a, b] = block_span(en, -H_MAX);
                if (b > a && b > -H_MAX) free.push_back(mdl.NewIntervalVar(a, b - a, b));
            }
        mdl.AddNoOverlap(free);
    }

    // --- ограничения соседних станций для отправлений
    std::map<std::string, std::vector<sat::IntervalVar>> nb_iv;
    for (const auto& chain : chains)
        for (const auto& ci : chain) {
            const Operation& o = *ci.op;
            if (o.kind != "departure" || ci.fixed || !o.train_id) continue;
            const OptStr& dest = m.trains.at(*o.train_id).destination_station_id;
            if (dest && !dest->empty()) nb_iv[*dest].push_back(mdl.NewIntervalVar(S.at(o.id), ci.dur, E.at(o.id)));
        }
    for (auto& [dest, lst] : nb_iv) {
        const auto* ens = blocks.entries("neighbor:" + dest);
        if (!ens || ens->empty()) continue;
        for (const auto& en : *ens) {
            auto [a, b] = block_span(en, 0);
            if (b > a) lst.push_back(mdl.NewIntervalVar(a, b - a, b));
        }
        mdl.AddNoOverlap(lst);
    }

    // --- цель
    sat::LinearExpr obj;
    for (const auto& chain : chains) {
        if (chain.empty() || !chain[0].op->train_id) continue;
        const Train& train = m.trains.at(*chain[0].op->train_id);
        long long w = 1LL << (std::max(1, std::min(5, train.priority)) - 1);
        const OpInfo* dep = nullptr;
        const OpInfo* arr = nullptr;
        for (const auto& ci : chain) {
            if (!dep && ci.op->kind == "departure") dep = &ci;
            if (!arr && ci.op->kind == "arrival" && !ci.fixed) arr = &ci;
        }
        if (dep && train.scheduled_departure && !dep->fixed) {
            auto d = mdl.NewIntVar(Domain(0, 2 * H_MAX));
            mdl.AddGreaterOrEqual(d, sat::LinearExpr(S.at(dep->op->id)) - to_min(*train.scheduled_departure));
            obj += 10 * w * d;
        }
        if (arr) {
            auto h = mdl.NewIntVar(Domain(0, 2 * H_MAX));
            mdl.AddGreaterOrEqual(h, sat::LinearExpr(S.at(arr->op->id)) - std::max(arr->lb, 0LL));
            obj += 10 * w * h;
        }
        if (!dep) {  // цепочки без отправления (переработка): задержка окончания
            const OpInfo& last = chain.back();
            if (!last.fixed) {
                auto d2 = mdl.NewIntVar(Domain(0, 2 * H_MAX));
                mdl.AddGreaterOrEqual(d2, sat::LinearExpr(E.at(last.op->id)) - (std::max(last.start0, 0LL) + last.dur));
                obj += 5 * w * d2;
            }
        }
    }
    for (const auto& st : stays) {
        if (st.fixed) continue;
        for (const auto& t : st.cands)
            if (t != st.track0) obj += (st.confirmed ? 30 : 8) * 10 * sat::LinearExpr(X.at({st.idx, t}));
    }
    // смена назначенного ресурса без пользы — тоже изменение плана для людей: небольшой штраф
    for (const auto& key : res_choice_order) {
        const Operation& o = m.ops.at(key.first);
        size_t k = static_cast<size_t>(key.second);
        if (o.resource_ids.size() <= k || !o.reserved) continue;
        const std::string& cur = o.resource_ids[k];
        for (const auto& [y, rid] : res_choice.at(key))
            if (rid != cur) obj += 40 * sat::LinearExpr(y);
    }
    for (const auto& chain : chains)
        for (const auto& ci : chain) {
            if (ci.fixed || !ci.op->reserved) continue;
            auto dev = mdl.NewIntVar(Domain(0, 3 * H_MAX));
            mdl.AddAbsEquality(dev, sat::LinearExpr(S.at(ci.op->id)) - ci.start0);
            obj += (ci.confirmed ? 10 : 2) * dev;
        }
    mdl.Minimize(obj);
    // подсказка: текущий план
    for (const auto& chain : chains)
        for (const auto& ci : chain)
            if (!ci.fixed) mdl.AddHint(S.at(ci.op->id), std::max({ci.lb, ci.start0, 0LL}));
    for (const auto& st : stays)
        for (const auto& t : st.cands) mdl.AddHint(X.at({st.idx, t}), t == st.track0);

    // --- решение
    sat::SatParameters params;
    params.set_max_time_in_seconds(time_limit_s);
    params.set_num_workers(workers);
    params.set_random_seed(7);
    if (deterministic_time > 0) {  // воспроизводимый результат
        params.set_interleave_search(true);
        params.set_max_deterministic_time(deterministic_time);
    }
    auto t0 = std::chrono::steady_clock::now();
    const sat::CpSolverResponse resp = sat::SolveWithParameters(mdl.Build(), params);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string status;
    switch (resp.status()) {
        case sat::CpSolverStatus::OPTIMAL: status = "optimal"; break;
        case sat::CpSolverStatus::FEASIBLE: status = "feasible"; break;
        case sat::CpSolverStatus::INFEASIBLE:
        case sat::CpSolverStatus::MODEL_INVALID: status = "infeasible"; break;
        default: status = "timeout";
    }
    PlanResult res;
    res.status = status;
    res.solver = "CP-SAT";
    res.solve_ms = std::round(ms * 10) / 10;
    res.unresolved = unresolved;
    if (status != "optimal" && status != "feasible") return res;
    res.objective = resp.objective_value();
    for (auto& st : stays)
        for (const auto& t : st.cands)
            if (sat::SolutionBooleanValue(resp, X.at({st.idx, t}))) { st.chosen = t; break; }
    for (const auto& chain : chains)
        for (const auto& ci : chain) {
            const Operation& o = *ci.op;
            const StayInfo& stay = stays[*ci.stay];
            long long s = sat::SolutionIntegerValue(resp, S.at(o.id));
            ScheduleEntry e{to_time(s), to_time(s + ci.dur), o.track_id, o.from_track_id, o.resource_ids,
                            o.route_nodes, ci.fixed, false, true};
            if (!ci.fixed) {
                if (o.kind == "uncoupling") e.from_track_id = stay.chosen;
                else e.track_id = stay.chosen;  // прибытие, отправление, манёвры и операции на пути
                auto rc = route_choice.find(o.id);
                if (rc != route_choice.end())
                    for (const auto& opt : rc->second)
                        if (sat::SolutionBooleanValue(resp, opt.p)) {
                            e.route_nodes = opt.r->switch_ids;
                            if (o.kind == "shunting") e.from_track_id = opt.from;
                        }
                Strings rids;
                for (size_t k = 0; k < o.requirements.size(); ++k) {
                    auto it = res_choice.find({o.id, static_cast<int>(k)});
                    if (it == res_choice.end()) continue;
                    for (const auto& [y, rid] : it->second)
                        if (sat::SolutionBooleanValue(resp, y)) rids.push_back(rid);
                }
                e.resource_ids = rids;
            }
            res.schedule[o.id] = e;
        }
    return res;
}

}  // namespace station

#endif  // STATION_WITH_ORTOOLS
