// Перепланирование: сбор цепочек и стоянок, эвристика, проверка плана, сводки
// (аналог app/services/planner.py; модель CP-SAT — в planner_cpsat.cpp).
#include "station/planner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>

namespace station {

namespace {

json jopt(const OptStr& s) { return s ? json(*s) : json(nullptr); }

bool contains(const Strings& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

int prio_weight(int priority) { return 1 << (std::max(1, std::min(5, priority)) - 1); }

double elapsed_ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

json schedule_to_json(const Schedule& s) {
    json out = json::object();
    for (const auto& [id, e] : s) {
        json j{{"start", iso(e.start)}, {"end", iso(e.end)}};
        if (e.has_track) {
            j["track_id"] = jopt(e.track_id);
            j["from_track_id"] = jopt(e.from_track_id);
            j["resource_ids"] = e.resource_ids;
            j["route_nodes"] = e.route_nodes;
            j["fixed"] = e.fixed;
            if (e.kept) j["kept"] = true;
        }
        out[id] = j;
    }
    return out;
}

// ------------------------------------------------------------------ сбор цепочек
PlanBuilder::PlanBuilder(const StationModel& m, const Forecast& fc) : m_(m), fc_(fc) {
    t_base = floor_to(m.now, 1);  // now.replace(second=0, microsecond=0)
    buf_ = m.track_buffer_min();
    margin_ = m.processing("length_margin_m", 10).get<double>();
    collect();
}

void PlanBuilder::collect() {
    for (const auto& [tid, ids] : m_.ops_by_train) {
        const Train& t = m_.trains.at(tid);
        if (t.status == "departed" || t.status == "completed" || t.status == "cancelled") continue;
        std::vector<const Operation*> live;
        for (const auto& id : ids) {
            const Operation& o = m_.ops.at(id);
            if (o.status != "done" && o.status != "cancelled") live.push_back(&o);
        }
        if (!live.empty()) add_chain(live, &t);
    }
    for (const auto& [id, o] : m_.ops)
        if (!o.train_id && o.status != "done" && o.status != "cancelled") add_chain({&o}, nullptr);
}

void PlanBuilder::add_chain(const std::vector<const Operation*>& live, const Train* train) {
    const int ci_idx = static_cast<int>(chains.size());
    chains.emplace_back();
    auto& chain = chains.back();
    for (size_t i = 0; i < live.size(); ++i) {
        const Operation* o = live[i];
        OpInfo ci;
        ci.op = o;
        ci.idx = static_cast<int>(i);
        ci.fixed = o->status == "in_progress";
        ci.dur = o->duration_min + o->extra_delay_min;
        Time fs = fc_.contains(o->id) ? fc_.at(o->id).first : o->planned_start;
        long long lb = 0;
        if (o->not_before) lb = std::max(lb, to_min(*o->not_before));
        if (o->kind == "arrival" && train && train->expected_arrival) lb = std::max(lb, to_min(*train->expected_arrival));
        if (ci.fixed) lb = to_min(o->actual_start.value_or(o->planned_start));
        ci.lb = lb;
        ci.start0 = o->reserved ? to_min(o->planned_start) : to_min(fs);
        ci.confirmed = o->status == "confirmed";
        chain.push_back(ci);
    }
    // стоянки
    std::vector<OpDict> dicts;
    for (const auto& ci : chain) {
        OpDict d;
        d.id = ci.op->id;
        d.kind = ci.op->kind;
        d.track_id = ci.op->track_id;
        d.from_track_id = ci.op->from_track_id;
        dicts.push_back(d);
    }
    auto sts = stays_of(dicts);
    for (size_t k = 0; k < sts.size(); ++k) {
        const Stay& st = sts[k];
        StayInfo s;
        s.idx = static_cast<int>(stays.size());
        std::vector<const OpInfo*> infos;
        for (size_t p = 0; p < chain.size(); ++p)
            if (contains(st.ops, chain[p].op->id)) {
                s.ops.emplace_back(ci_idx, static_cast<int>(p));
                infos.push_back(&chain[p]);
            }
        // поезд уже стоит на пути или стоянка начата — путь фиксирован
        bool standing = train && k == 0 && train->status == "on_station" && train->current_track_id == st.track_id;
        bool fixed_track = standing || std::any_of(infos.begin(), infos.end(), [](const OpInfo* c) { return c->fixed; });
        s.track0 = st.track_id;
        s.cands = fixed_track ? Strings{st.track_id.value_or("")} : candidates(st.track_id, train, infos);
        if (s.cands.empty()) {
            s.cands = {st.track_id.value_or("")};
            s.unresolved = true;
            unresolved.push_back(json{{"track_id", jopt(st.track_id)},
                                      {"train", train ? json(train->number) : json(nullptr)},
                                      {"reason", "нет допустимого пути для стоянки (все совместимые пути закрыты, "
                                                 "без данных или недостаточной длины)"}});
        }
        s.fixed = fixed_track;
        s.train = train;
        s.confirmed = std::any_of(infos.begin(), infos.end(), [](const OpInfo* c) { return c->confirmed; });
        for (size_t q = 0; q < s.ops.size(); ++q) {
            // манёвр закрывает предыдущую стоянку и открывает новую: относится к пути назначения
            OpInfo& ci = chain[s.ops[q].second];
            if (!ci.stay || q == 0) ci.stay = s.idx;
        }
        stays.push_back(s);
    }
}

Strings PlanBuilder::candidates(const OptStr& track0, const Train* train, const std::vector<const OpInfo*>& infos) const {
    const TrackRow& row0 = m_.track_rows.at(track0.value_or(""));
    std::optional<double> L = train ? m_.train_length(*train) : std::nullopt;
    std::set<std::string> need_ops;
    for (const auto* ci : infos)
        if (ci->op->kind == "loading" || ci->op->kind == "unloading" || ci->op->kind == "repair") need_ops.insert(ci->op->kind);
    Strings out;
    for (const auto& [tid, t] : m_.track_rows) {
        if (t.kind != row0.kind) continue;
        if (train && !contains(t.allowed_train_kinds, train->kind)) continue;
        if (L && t.useful_length_m && *t.useful_length_m < *L + margin_) continue;
        if (!t.useful_length_m) continue;
        bool has_zone = t.zone_id && !t.zone_id->empty();
        if (has_zone && !need_ops.empty()) {
            Strings zops;
            for (const auto& z : m_.cfg.at("zones"))
                if (z.at("id").get<std::string>() == *t.zone_id) {
                    for (const auto& x : z.value("operations", json::array())) zops.push_back(x.get<std::string>());
                    break;
                }
            bool bad = false;
            for (const auto& k : need_ops)
                if (!contains(zops, k)) bad = true;
            if (bad) continue;
        }
        if (!need_ops.empty() && !has_zone) continue;
        std::string ds = m_.data_state(tid) ? m_.data_state_name(tid) : "";
        if (m_.data_state(tid) && ds != "actual" && ds != "not_monitored") continue;
        out.push_back(tid);
    }
    return out;
}

// ------------------------------------------------------------------ эвристика
PlanResult PlanBuilder::solve_greedy() {
    // Последовательное размещение цепочек (earliest-fit): сначала стоящие на путях поезда,
    // затем остальные по убыванию приоритета. Неразмещённые поезда оставляются как в плане.
    auto t0 = std::chrono::steady_clock::now();
    std::set<int> kept;
    PlanResult res;
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto [r, failed] = greedy_pass(kept);
        res = std::move(r);
        bool new_failures = false;
        for (int f : failed)
            if (!kept.count(f)) new_failures = true;
        if (!new_failures) break;
        kept.insert(failed.begin(), failed.end());  // бронируем их заранее и проходим заново
    }
    res.solve_ms = std::round(elapsed_ms(t0) * 10) / 10;
    if (!res.unresolved.empty()) res.status = "partial";
    return res;
}

std::pair<PlanResult, std::set<int>> PlanBuilder::greedy_pass(const std::set<int>& kept_chains) {
    std::set<int> failed;
    IntervalBook book;
    m_.add_blocks(book, true);
    PlanResult res;
    res.status = "heuristic";
    res.solver = "Эвристика earliest-fit";
    res.unresolved = unresolved;

    auto add_specs = [&](const std::vector<ReservationSpec>& specs) {
        for (const auto& sp : specs) {
            EntryMeta meta;
            meta.type = "reservation";
            meta.train_id = sp.train_id;
            book.add(sp.key, sp.start, sp.end, meta);
        }
    };

    // начатые операции не переносятся
    for (const auto& chain : chains)
        for (const auto& ci : chain) {
            if (!ci.fixed) continue;
            const Operation& o = *ci.op;
            ScheduleEntry e{to_time(ci.lb), to_time(ci.lb + ci.dur), o.track_id, o.from_track_id, o.resource_ids,
                            o.route_nodes, true, false, true};
            res.schedule[o.id] = e;
            OpDict d;
            d.id = o.id;
            d.kind = o.kind;
            d.track_id = o.track_id;
            d.from_track_id = o.from_track_id;
            d.start = e.start;
            d.end = e.end;
            d.resource_ids = o.resource_ids;
            d.route_nodes = o.route_nodes;
            d.side = o.side;
            d.status = "in_progress";
            add_specs(reservation_specs(m_, o.train_id, "", {d}));
        }

    auto planned_entry = [](const Operation& o) {
        return ScheduleEntry{o.planned_start, o.planned_end, o.track_id, o.from_track_id, o.resource_ids,
                             o.route_nodes, false, true, true};
    };

    // Оставить операции поезда как в текущем плане и занять ими книгу интервалов.
    auto keep = [&](const std::vector<const OpInfo*>& rest) {
        std::vector<OpDict> dicts;
        for (const auto* ci : rest) {
            const Operation& o = *ci->op;
            res.schedule[o.id] = planned_entry(o);
            OpDict d;
            d.id = o.id;
            d.kind = o.kind;
            d.track_id = o.track_id;
            d.from_track_id = o.from_track_id;
            d.start = o.planned_start;
            d.end = o.planned_end;
            d.side = o.side;
            d.resource_ids = o.resource_ids;
            d.route_nodes = o.route_nodes;
            d.status = "planned";
            dicts.push_back(d);
        }
        const Train* tr = m_.train(rest[0]->op->train_id);
        add_specs(reservation_specs(m_, tr ? OptStr(tr->id) : std::nullopt, tr ? tr->number : "",
                                    with_presence(m_, tr, dicts)));
    };

    // порядок: стоящие на путях, затем по убыванию приоритета и раннему началу
    std::vector<int> order(chains.size());
    for (size_t i = 0; i < chains.size(); ++i) order[i] = static_cast<int>(i);
    auto key = [&](int idx) {
        const auto& ch = chains[idx];
        const StayInfo& st = stays[*ch[0].stay];
        const Train* t = m_.train(ch[0].op->train_id);
        int pr = t ? t->priority : 0;
        return std::make_tuple(st.fixed ? 0 : 1, -pr, ch[0].lb);
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return key(a) < key(b); });

    auto rest_of = [&](int idx) {
        std::vector<const OpInfo*> rest;
        for (const auto& ci : chains[idx])
            if (!ci.fixed) rest.push_back(&ci);
        return rest;
    };

    for (int idx : order) {  // сначала заранее оставленные поезда — они занимают книгу первыми
        if (!kept_chains.count(idx)) continue;
        auto rest = rest_of(idx);
        if (rest.empty()) continue;
        keep(rest);
        const Train* t = m_.train(rest[0]->op->train_id);
        res.unresolved.push_back(json{{"train", t ? json(t->number) : json(nullptr)},
                                      {"reason", "эвристика не нашла размещения; операции оставлены как в текущем плане"}});
    }
    for (int idx : order) {
        if (kept_chains.count(idx)) continue;
        auto rest = rest_of(idx);
        if (rest.empty()) continue;
        const auto& chain = chains[idx];
        const Operation& o0 = *rest[0]->op;
        const Train* train = m_.train(o0.train_id);
        const StayInfo& first_stay = stays[*rest[0]->stay];
        std::vector<const OpInfo*> fixed_ops;
        for (const auto& ci : chain)
            if (ci.fixed) fixed_ops.push_back(&ci);
        bool on_station = train && train->status == "on_station" && train->current_track_id && !train->current_track_id->empty();
        bool standing = first_stay.fixed || !fixed_ops.empty() || on_station;
        OptStr src;
        if (standing) {
            const Operation* last_fixed = fixed_ops.empty() ? nullptr : fixed_ops.back()->op;
            if (last_fixed && (last_fixed->kind == "arrival" || last_fixed->kind == "shunting")) src = last_fixed->track_id;
            else if (on_station) src = train->current_track_id;
            else if (rest[0]->op->kind == "shunting" && rest[0]->op->from_track_id) src = rest[0]->op->from_track_id;
            else src = first_stay.track0;
        }
        std::vector<Step> steps;
        if (standing) steps.push_back(Step{"dwell", 0, {}, m_.track_rows.at(*src).kind, std::nullopt});
        for (const auto* ci : rest) {
            Step st;
            st.kind = ci->op->kind;
            st.duration = static_cast<int>(ci->dur);
            st.requires = ci->op->requirements;
            if (st.kind == "arrival" || st.kind == "shunting" || steps.empty())
                st.group = m_.track_rows.at(ci->op->track_id.value_or("")).kind;
            if (st.kind == "uncoupling") st.to_track = ci->op->track_id;
            steps.push_back(st);
        }
        long long start_after = std::max(0LL, rest[0]->lb);
        std::optional<long long> prev_end;
        for (const auto* ci : fixed_ops) prev_end = std::max(prev_end.value_or(ci->lb + ci->dur), ci->lb + ci->dur);
        if (prev_end) start_after = std::max(start_after, *prev_end);
        if (standing) start_after = std::max(0LL, prev_end.value_or(0));  // стоянка — с окончания начатой операции

        TrainSpec spec;
        spec.train_id = train ? train->id : o0.id;
        spec.number = train ? train->number : o0.note.value_or("");
        spec.kind = train ? train->kind : "freight";
        spec.priority = train ? train->priority : 1;
        spec.length_m = train ? m_.train_length(*train) : std::optional<double>(20.0);
        spec.side_in = train ? train->arrival_side : "east";
        spec.side_out = train ? train->departure_side : "east";
        spec.templ = steps;
        spec.arrival = to_time(start_after);
        if (train && train->scheduled_departure) spec.departure_not_before = train->scheduled_departure;

        Placer placer(m_, book, 600, spec.train_id);  // свои резервы не мешают
        if (standing) placer.force_first = src;
        else if (rest[0]->op->kind != "arrival" && rest[0]->op->kind != "shunting") placer.force_first = first_stay.track0;
        PlaceResult pr = placer.place(spec, false);
        if (!pr.ok) {
            failed.insert(idx);
            for (const auto* ci : rest) res.schedule[ci->op->id] = planned_entry(*ci->op);  // до следующего прохода
            res.unresolved.push_back(json{{"train", spec.number}, {"reason", "эвристика не нашла размещения в горизонте"}});
            continue;
        }
        commit_to_book(m_, book, spec.train_id, spec.number, pr.ops);
        size_t off = standing ? 1 : 0;
        for (size_t k = 0; k < rest.size() && k + off < pr.ops.size(); ++k) {
            const PlacedOp& po = pr.ops[k + off];
            const Operation& o = *rest[k]->op;
            res.schedule[o.id] = ScheduleEntry{po.start, po.end, po.track_id,
                                               po.from_track_id ? po.from_track_id : o.from_track_id,
                                               po.resource_ids, po.route_nodes, false, false, true};
        }
    }
    return {std::move(res), failed};
}

// ------------------------------------------------------------------ независимая проверка
Strings verify(const StationModel& m, const Schedule& schedule, const std::set<std::string>& unresolved_tracks) {
    // Все резервы плана не пересекаются друг с другом и с блокировками. Блокировка «нет данных»
    // на путях, честно отмеченных планировщиком как нерешаемые, план не отклоняет.
    IntervalBook book;
    m.add_blocks(book, true);
    Strings errors;
    OrderedMap<std::vector<std::pair<const Operation*, const ScheduleEntry*>>> by_train;
    for (const auto& [oid, e] : schedule) {
        const Operation& o = m.ops.at(oid);
        by_train[o.train_id ? *o.train_id : oid].push_back({&o, &e});
    }
    for (auto& [key, items] : by_train) {
        std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first->seq < b.first->seq; });
        const Train* train = m.train(items[0].first->train_id);
        std::vector<OpDict> dicts;
        std::set<std::string> fixed_ops;
        for (const auto& [o, e] : items) {
            OpDict d;
            d.id = o->id;
            d.kind = o->kind;
            d.track_id = e->track_id;
            d.from_track_id = e->from_track_id;
            d.start = e->start;
            d.end = e->end;
            d.resource_ids = e->resource_ids;
            d.route_nodes = e->route_nodes;
            d.side = o->side;
            d.status = e->fixed ? "in_progress" : "planned";
            dicts.push_back(d);
            if (e->fixed || e->kept) fixed_ops.insert(o->id);
        }
        dicts = with_presence(m, train, dicts);
        for (const auto& d : dicts)
            if (d.id.rfind("presence-", 0) == 0) fixed_ops.insert(d.id);
        OptTime prev_end;
        for (const auto& [o, e] : items) {
            if (prev_end && e->start < *prev_end && !e->fixed)
                errors.push_back("нарушена последовательность операций поезда № " + (train ? train->number : key));
            prev_end = e->end;
        }
        for (const auto& sp : reservation_specs(m, items[0].first->train_id, train ? train->number : "", dicts)) {
            bool fixed = fixed_ops.count(sp.operation_id) > 0;
            for (const auto& h : book.conflicts(sp.key, sp.start, sp.end)) {
                if (fixed && h.meta.fixed) continue;
                if (h.meta.type == "reservation" || !fixed) {
                    if (h.meta.type == "data" && (fixed || unresolved_tracks.count(sp.key.size() > 6 ? sp.key.substr(6) : "")))
                        continue;
                    if (h.meta.type == "shift" && fixed) continue;
                    errors.push_back(sp.key + ": " + sp.purpose + " " + local_hm(sp.start) + "–" + local_hm(sp.end) +
                                     " пересекается с «" + (h.meta.label ? *h.meta.label : h.meta.type) + "»");
                    break;
                }
            }
            EntryMeta meta;
            meta.type = "reservation";
            meta.label = sp.purpose;
            meta.train_id = sp.train_id;
            meta.fixed = fixed;
            book.add(sp.key, sp.start, sp.end, meta);
        }
    }
    return errors;
}

// ------------------------------------------------------------------ сводки
json diff_schedule(const StationModel& m, const Schedule& schedule) {
    std::vector<json> out;
    for (const auto& [oid, e] : schedule) {
        if (e.fixed) continue;
        const Operation& o = m.ops.at(oid);
        long long moved = py_round0(minutes_f(e.start - o.planned_start));
        bool track_changed = e.track_id != o.track_id ||
                             (e.from_track_id && !e.from_track_id->empty() ? e.from_track_id : std::nullopt) !=
                                 (o.from_track_id && !o.from_track_id->empty() ? o.from_track_id : std::nullopt);
        Strings a = e.resource_ids, b = o.resource_ids;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        bool res_changed = a != b;
        if (moved == 0 && !track_changed && !res_changed && o.reserved) continue;
        const Train* t = m.train(o.train_id);
        std::set<std::string> tids;
        for (const OptStr& x : {o.track_id, e.track_id, o.from_track_id, e.from_track_id})
            if (x && !x->empty()) tids.insert(*x);
        json from_res = json::array(), to_res = json::array();
        for (const auto& r : o.resource_ids)
            if (const Resource* rr = m.resources.find(r)) from_res.push_back(rr->name);
        for (const auto& r : e.resource_ids)
            if (const Resource* rr = m.resources.find(r)) to_res.push_back(rr->name);
        out.push_back(json{{"operation_id", oid},
                           {"train", t ? json(t->number) : json(nullptr)},
                           {"train_id", jopt(o.train_id)},
                           {"kind", o.kind},
                           {"track_ids", Strings(tids.begin(), tids.end())},
                           {"kind_label", kind_label(o.kind)},
                           {"status", o.status},
                           {"from", json{{"track", m.track_label(o.track_id)}, {"start", iso(o.planned_start)}, {"resources", from_res}}},
                           {"to", json{{"track", m.track_label(e.track_id)}, {"start", iso(e.start)}, {"resources", to_res}}},
                           {"shift_min", moved},
                           {"track_changed", track_changed},
                           {"resources_changed", res_changed},
                           {"newly_planned", !o.reserved},
                           {"was_confirmed", o.status == "confirmed"}});
    }
    std::stable_sort(out.begin(), out.end(), [](const json& a, const json& b) {
        std::string ta = a["train"].is_null() ? "" : a["train"].get<std::string>();
        std::string tb = b["train"].is_null() ? "" : b["train"].get<std::string>();
        if (ta != tb) return ta < tb;
        return a["to"]["start"].get<std::string>() < b["to"]["start"].get<std::string>();
    });
    return json(out);
}

json plan_kpis(const StationModel& m, const Schedule& sched, size_t conflicts) {
    std::vector<double> delays;
    double weighted = 0;
    for (const auto& [tid, ids] : m.ops_by_train) {
        const Train& t = m.trains.at(tid);
        const Operation* dep = nullptr;
        for (const auto& id : ids)
            if (m.ops.at(id).kind == "departure" && sched.contains(id)) { dep = &m.ops.at(id); break; }
        if (dep && t.scheduled_departure) {
            double d = std::max(0.0, minutes_f(sched.at(dep->id).start - *t.scheduled_departure));
            delays.push_back(d);
            weighted += d * prio_weight(t.priority);
        }
    }
    OrderedMap<double> busy;
    Time h0 = m.now, h1 = m.now + hours(4);
    for (const auto& [oid, e] : sched) {
        if (!m.ops.contains(oid) || !e.has_track || !e.track_id) continue;
        const TrackRow* tr = m.track_rows.find(*e.track_id);
        if (!tr || tr->kind != "receiving_departure") continue;
        Time a = std::max(e.start, h0), b = std::min(e.end, h1);
        if (b > a) busy[*e.track_id] += minutes_f(b - a);
    }
    size_t rd = 0;
    for (const auto& [tid, t] : m.track_rows)
        if (t.kind == "receiving_departure") ++rd;
    std::vector<double> busy_v;
    for (const auto& [k, v] : busy) busy_v.push_back(v);
    double util = rd ? py_sum(busy_v) / (rd * 240.0) : 0;
    double sum = py_sum(delays), mx = 0;
    long long delayed = 0;
    for (double d : delays) {
        mx = std::max(mx, d);
        if (d >= 5) ++delayed;
    }
    return json{{"delayed_trains", delayed},
                {"total_delay_min", py_round(sum, 1)},
                {"max_delay_min", delays.empty() ? json(0) : json(py_round(mx, 1))},
                {"weighted_delay", py_round(weighted, 1)},
                {"conflicts", conflicts},
                {"rd_utilization_4h", py_round(std::min(util, 1.5), 3)}};
}

// ------------------------------------------------------------------ полный цикл
json run_planner(const StationModel& m, double time_limit_s, int workers) {
    static const std::map<std::string, std::string> STATUS = {
        {"optimal", "Оптимальность доказана"},
        {"feasible", "Найден допустимый план (оптимальность не доказана за отведённое время)"},
        {"timeout", "Допустимый план не найден за отведённое время"},
        {"infeasible", "Доказано отсутствие решения"},
        {"heuristic", "Эвристика: найден допустимый план (оптимальность не оценивалась)"},
        {"partial", "Полный допустимый план не найден: сохранены допустимые части прежнего плана"},
    };
    auto t_all = std::chrono::steady_clock::now();
    Forecast fc = forecast(m);
    Detection before = detect(m, fc);
    PlanBuilder pb(m, fc);
    std::set<std::string> unresolved_tracks;
    for (const auto& u : pb.unresolved)
        if (u.contains("track_id") && !u["track_id"].is_null()) unresolved_tracks.insert(u["track_id"].get<std::string>());
    Strings notes;
    std::optional<PlanResult> res;
#ifdef STATION_WITH_ORTOOLS
    PlanResult cp = pb.solve_cpsat(time_limit_s, workers);
    if (cp.status == "optimal" || cp.status == "feasible") {
        Strings errs = verify(m, cp.schedule, unresolved_tracks);
        if (!errs.empty()) {
            std::string s = "Решение CP-SAT не прошло независимую проверку и отклонено: ";
            for (size_t i = 0; i < std::min<size_t>(3, errs.size()); ++i) s += (i ? "; " : "") + errs[i];
            notes.push_back(s);
        } else {
            res = std::move(cp);
        }
    } else {
        notes.push_back("CP-SAT: " + STATUS.at(cp.status) + " (" + fmt_f(cp.solve_ms, 0) + " мс).");
    }
#else
    (void)time_limit_s;
    (void)workers;
    notes.push_back("Сборка без OR-Tools: используется эвристика.");
#endif
    json verify_errors = json::array();
    if (!res) {
        PlanBuilder pb2(m, fc);
        PlanResult g = pb2.solve_greedy();
        Strings errs = verify(m, g.schedule, unresolved_tracks);  // весь план, включая оставленные операции
        if (!errs.empty()) {
            std::string s = "Эвристика: часть плана не прошла проверку — ";
            for (size_t i = 0; i < std::min<size_t>(3, errs.size()); ++i) s += (i ? "; " : "") + errs[i];
            notes.push_back(s);
            g.status = "partial";
        }
        res = std::move(g);
    }
    Schedule before_sched;
    for (const auto& [oid, se] : fc) {
        const Operation* o = m.ops.find(oid);
        if (!o || o->status == "done" || o->status == "cancelled") continue;
        ScheduleEntry e;
        e.start = se.first;
        e.end = se.second;
        e.has_track = false;
        before_sched[oid] = e;
    }
    json kpi_before = plan_kpis(m, before_sched, before.conflicts.size());
    json kpi_after = plan_kpis(m, res->schedule, 0);
    size_t remaining = 0;
    for (const auto& c : before.conflicts)
        if (c["type"] == "faulty_wagon") ++remaining;
    kpi_after["conflicts"] = remaining + res->unresolved.size();
    double total_ms = elapsed_ms(t_all);
    return json{{"status", res->status},
                {"status_label", STATUS.at(res->status)},
                {"solver", res->solver},
                {"solve_ms", res->solve_ms},
                {"total_ms", std::round(total_ms * 10) / 10},
                {"objective", res->objective ? json(*res->objective) : json(nullptr)},
                {"schedule", schedule_to_json(res->schedule)},
                {"changes", diff_schedule(m, res->schedule)},
                {"unresolved", res->unresolved},
                {"notes", notes},
                {"verify_errors", verify(m, res->schedule, unresolved_tracks)},
                {"summary", json{{"before", kpi_before},
                                 {"after", kpi_after},
                                 {"index_before", compute_plan_index(m, nullptr, kpi_before)},
                                 {"index_after", compute_plan_index(m, &res->schedule, kpi_after)},
                                 {"conflicts_before", before.conflicts.size()}}}};
}

}  // namespace station
