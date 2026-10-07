#include "station/placement.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace station {

namespace {

long long track_num(const std::string& s) {  // _num(): int(номер) или 0 для «I», «II»
    bool ok = false;
    long long v = py_int(s, ok);
    return ok ? v : 0;
}

bool contains(const Strings& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

}  // namespace

std::vector<Step> steps_from_json(const json& tpl) {
    std::vector<Step> out;
    for (const auto& j : tpl) {
        Step s;
        s.kind = j.at("kind").get<std::string>();
        s.duration = static_cast<int>(j.at("duration").get<double>());
        if (j.contains("requires"))
            for (const auto& r : j["requires"]) s.requires.push_back(r.get<std::string>());
        if (j.contains("group") && !j["group"].is_null() && !j["group"].get<std::string>().empty())
            s.group = j["group"].get<std::string>();
        if (j.contains("to_track") && !j["to_track"].is_null()) s.to_track = j["to_track"].get<std::string>();
        out.push_back(s);
    }
    return out;
}

json PlacedOp::as_dict() const {
    json j;
    j["kind"] = kind;
    j["kind_label"] = kind_label(kind);
    j["seq"] = seq;
    j["track_id"] = track_id;
    j["from_track_id"] = from_track_id ? json(*from_track_id) : json(nullptr);
    j["side"] = side ? json(*side) : json(nullptr);
    j["start"] = isoformat_utc(start);  // datetime.isoformat(): «+00:00», а не «Z»
    j["end"] = isoformat_utc(end);
    j["duration_min"] = duration_min;
    j["requirements"] = requirements;
    j["resource_ids"] = resource_ids;
    j["route_nodes"] = route_nodes;
    return j;
}

Placer::Placer(const StationModel& m, const IntervalBook& book, int wait_max_min, OptStr ignore_train)
    : m_(m), book_(book), wait_max_(minutes(wait_max_min)), ignore_train_(std::move(ignore_train)) {
    margin_ = m.processing("length_margin_m", 10);
    buf_ = minutes(m.track_buffer_min());
}

// ------------------------------------------------------------------ пути-кандидаты
Strings Placer::candidates(const std::string& group, const TrainSpec& spec, const std::vector<Step>& steps,
                           PlaceResult& res) const {
    Strings out;
    auto reason = [&](const std::string& tid, const char* code, const std::string& msg) {
        if (!res.track_reasons.contains(tid)) res.track_reasons[tid] = {std::string(code), msg};
    };
    for (const auto& [tid, t] : m_.track_rows) {
        if (t.kind != group) continue;
        std::string label = m_.track_label(tid);
        if (!contains(t.allowed_train_kinds, spec.kind)) {
            reason(tid, "INCOMPATIBLE", label + ": не предназначен для поездов этого вида");
            continue;
        }
        if (t.zone_id && !t.zone_id->empty()) {
            Strings zone_ops;
            for (const auto& z : m_.cfg.at("zones"))
                if (z.at("id").get<std::string>() == *t.zone_id) {
                    if (z.contains("operations"))
                        for (const auto& op : z["operations"]) zone_ops.push_back(op.get<std::string>());
                    break;
                }
            bool bad = false;
            for (const auto& s : steps)
                if ((s.kind == "loading" || s.kind == "unloading" || s.kind == "repair") && !contains(zone_ops, s.kind))
                    bad = true;
            if (bad) {
                reason(tid, "INCOMPATIBLE", label + ": грузовой фронт не выполняет нужную операцию");
                continue;
            }
        }
        if (!t.useful_length_m) {
            reason(tid, "INSUFFICIENT_DATA", label + ": не задана полезная длина — недостаточно данных");
            res.insufficient.push_back("полезная длина: " + lower_first(label));
            continue;
        }
        if (!spec.length_m) {
            reason(tid, "INSUFFICIENT_DATA", label + ": длина состава неизвестна — недостаточно данных");
            continue;
        }
        double need_len = *spec.length_m + margin_.get<double>();
        if (*t.useful_length_m < need_len) {
            reason(tid, "LENGTH",
                   label + ": полезная длина " + fmt_f(*t.useful_length_m, 0) + " м меньше требуемой " + fmt_f(need_len, 1) +
                       " м (состав " + fmt_f(*spec.length_m, 1) + " м + запас " + py_str(margin_) + " м)");
            continue;
        }
        out.push_back(tid);
    }
    // best-fit: сначала самый короткий подходящий путь, при равной длине — по номеру
    std::stable_sort(out.begin(), out.end(), [&](const std::string& a, const std::string& b) {
        const TrackRow& ta = m_.track_rows.at(a);
        const TrackRow& tb = m_.track_rows.at(b);
        if (*ta.useful_length_m != *tb.useful_length_m) return *ta.useful_length_m < *tb.useful_length_m;
        return track_num(ta.number) < track_num(tb.number);
    });
    return out;
}

// ------------------------------------------------------------------ проверки интервалов
std::vector<Entry> Placer::blocking(const std::string& key, Time s, Time e) const {
    ConflictFilter f;
    f.ignore_train = ignore_train_;
    return book_.conflicts(key, s, e, f);
}

bool Placer::choose_resources(const Strings& reqs, const std::string& kind, const std::string& track_id, Time s,
                              Time e, Strings& chosen, std::vector<Entry>& blocks, std::string& why) const {
    // Как в Python: блокировки копятся по всем видам ресурсов, пока не найдётся отказ.
    std::set<std::string> used;
    std::vector<Entry> acc;
    chosen.clear();
    blocks.clear();
    for (const auto& rk : reqs) {
        Strings cands;
        for (const auto& [rid, row] : m_.resources)
            if (row.kind == rk && !used.count(rid)) cands.push_back(rid);
        std::sort(cands.begin(), cands.end());
        if (cands.empty()) {
            why = "на станции нет ресурса «" + res_kind_label(rk) + "»";
            blocks.clear();
            return false;
        }
        OptStr ok;
        for (const auto& rid : cands) {
            const Resource& row = m_.resources.at(rid);
            Duration pad = minutes(m_.zone_travel(row.home_zone_id, m_.op_zone(kind, track_id)));
            auto b = blocking("res:" + rid, s - pad, e);
            if (b.empty()) {
                ok = rid;
                break;
            }
            acc.insert(acc.end(), b.begin(), b.end());
        }
        if (!ok) {
            why = "нет свободного ресурса «" + res_kind_label(rk) + "»";
            blocks = acc;
            return false;
        }
        chosen.push_back(*ok);
        used.insert(*ok);
    }
    return true;
}

RoutePtr Placer::route_for(const std::string& kind, const TrainSpec& spec, const OptStr& from_track,
                           const OptStr& to_track) const {
    const Topology& topo = *m_.topo;
    auto need = [](const OptStr& v) -> const std::string& {
        if (!v) throw std::runtime_error("операция движения без пути");
        return *v;
    };
    if (kind == "arrival") return topo.arrival_route(spec.side_in, need(to_track));
    if (kind == "departure") return topo.departure_route(spec.side_out, need(from_track));
    if (kind == "shunting" || kind == "uncoupling") return topo.shunting_route(need(from_track), need(to_track));
    return nullptr;
}

Placer::TryResult Placer::try_op(const Step& step, const TrainSpec& spec, const std::string& track_id,
                                 const OptStr& from_track, Time earliest, bool exact, int seq) {
    // Самое раннее допустимое начало операции (или ровно earliest при exact).
    Duration dur = minutes(step.duration);
    const std::string& kind = step.kind;
    RoutePtr route;
    if (is_movement(kind)) {
        route = route_for(kind, spec, kind != "arrival" ? from_track : std::nullopt,
                          kind != "departure" ? OptStr(track_id) : std::nullopt);
        if (!route) return {std::nullopt, std::string("ROUTE"), {}};
    }
    Time s = ceil_to(earliest, 1);
    Time limit = s + (exact ? 0 : wait_max_);
    std::vector<Entry> last_blocks;
    OptStr last_code;
    last_why_.reset();
    while (s <= limit) {
        Time e = s + dur;
        std::vector<Entry> blocks;
        OptStr code;
        if (route) {
            for (const auto& sw : route->switch_ids) {
                auto b = blocking("switch:" + sw, s, e);
                blocks.insert(blocks.end(), b.begin(), b.end());
            }
            for (const auto& tt : route->through_track_ids) {
                if (tt != track_id && tt != from_track) {
                    auto b = blocking("track:" + tt, s, e);
                    blocks.insert(blocks.end(), b.begin(), b.end());
                }
            }
            if (!blocks.empty()) code = "ROUTE";
            if (blocks.empty() && (kind == "arrival" || kind == "shunting" || kind == "uncoupling")) {
                // заезд возможен только на путь, свободный в момент заезда
                blocks = blocking("track:" + track_id, s, e + buf_);
                if (!blocks.empty()) code = "OCCUPIED";
            }
        }
        Strings res_ids;
        if (blocks.empty() && !step.requires.empty()) {
            Strings chosen;
            std::vector<Entry> rb;
            std::string why;
            if (!choose_resources(step.requires, kind, track_id, s, e, chosen, rb, why)) {
                blocks = rb;
                code = "RESOURCE";
                last_why_ = why;
                if (rb.empty()) return {std::nullopt, std::string("RESOURCE_NONE"), {}};  // ресурса нет вовсе
            } else {
                res_ids = chosen;
            }
        }
        if (blocks.empty()) {
            PlacedOp op;
            op.kind = kind;
            op.seq = seq;
            op.track_id = track_id;
            op.start = s;
            op.end = e;
            op.duration_min = step.duration;
            op.from_track_id = from_track;
            op.requirements = step.requires;
            op.resource_ids = res_ids;
            if (kind == "arrival") op.side = spec.side_in;
            else if (kind == "departure") op.side = spec.side_out;
            if (route) {
                op.route_nodes = route->switch_ids;
                op.through_tracks = route->through_track_ids;
            }
            return {op, std::nullopt, {}};
        }
        last_blocks = blocks;
        last_code = code;
        Time nxt = blocks[0].end;
        for (const auto& b : blocks) nxt = std::min(nxt, b.end);
        if (nxt >= FAR) break;
        s = ceil_to(std::max(nxt, s + MINUTE), 1);
    }
    return {std::nullopt, last_code, last_blocks};
}

// ------------------------------------------------------------------ размещение цепочки
PlaceResult Placer::place(const TrainSpec& spec, bool arrival_exact) {
    PlaceResult res;
    if (!spec.length_m) res.insufficient.push_back("длина состава (нет длины вагонов и общей длины)");
    // Шаблон делится на стоянки: шаг с group начинает новую стоянку.
    std::vector<std::pair<std::string, std::vector<Step>>> stays;
    for (const auto& st : spec.templ) {
        if (st.group) stays.push_back({*st.group, {st}});
        else if (stays.empty()) throw std::runtime_error("шаблон начинается без вида парка");
        else stays.back().second.push_back(st);
    }
    std::vector<PlacedOp> best;
    const bool forced = force_first.has_value();

    auto finish_stay = [&](const std::string& track, Time stay_start, Time stay_end, bool standing) {
        auto b = blocking("track:" + track, stay_start, stay_end + buf_);
        if (standing) {  // поезд уже стоит на пути: закрытие запрещает новые заезды, но не его стоянку
            std::vector<Entry> keep;
            for (const auto& e : b)
                if (e.meta.type != "closure" && e.meta.type != "maintenance" && e.meta.type != "data") keep.push_back(e);
            b = keep;
        }
        return b;
    };

    std::function<bool(size_t, const OptStr&, Time, const std::vector<PlacedOp>&, int, Time, const OptStr&)> dfs =
        [&](size_t i, const OptStr& prev_track, Time t_ready, const std::vector<PlacedOp>& acc, int seq,
            Time prev_stay_start, const OptStr& top_track) -> bool {
        const auto& [group, steps] = stays[i];
        Strings cands = (i == 0 && force_first && !force_first->empty()) ? Strings{*force_first}
                                                                         : candidates(group, spec, steps, res);
        for (const auto& track : cands) {
            std::string tt = top_track ? *top_track : track;
            const Step& first = steps[0];
            TryResult r = try_op(first, spec, track, prev_track, t_ready, first.kind == "arrival" && arrival_exact, seq);
            if (!r.op) {
                note(res, tt, track, r.code, r.blocks, first);
                continue;
            }
            const PlacedOp& op = *r.op;
            // предыдущая стоянка занята до окончания вытягивания
            if (prev_track) {
                auto b = finish_stay(*prev_track, prev_stay_start, op.end, i == 1 && forced);
                if (!b.empty()) {
                    note(res, tt, *prev_track, std::string("OCCUPIED"), b, first);
                    continue;
                }
            }
            std::vector<PlacedOp> ops{op};
            Time t = op.end;
            bool failed = false;
            for (size_t k = 1; k < steps.size(); ++k) {
                const Step& st = steps[k];
                Time earliest = t;
                if (st.kind == "departure" && spec.departure_not_before) earliest = std::max(t, *spec.departure_not_before);
                TryResult n;
                if (st.kind == "uncoupling") {  // вагон подаётся с пути стоянки на путь депо
                    if (!st.to_track) throw std::runtime_error("отцепка без пути депо");
                    n = try_op(st, spec, *st.to_track, track, earliest, false, seq + static_cast<int>(k));
                } else {
                    n = try_op(st, spec, track, st.kind == "departure" ? OptStr(track) : std::nullopt, earliest, false,
                               seq + static_cast<int>(k));
                }
                if (!n.op) {
                    note(res, tt, track, n.code, n.blocks, st);
                    failed = true;
                    break;
                }
                ops.push_back(*n.op);
                t = n.op->end;
            }
            if (failed) continue;
            Time stay_start = op.start;
            std::vector<PlacedOp> next_acc = acc;
            next_acc.insert(next_acc.end(), ops.begin(), ops.end());
            if (i + 1 < stays.size()) {
                if (dfs(i + 1, track, t, next_acc, seq + static_cast<int>(steps.size()), stay_start, tt)) return true;
                continue;
            }
            auto b = finish_stay(track, stay_start, t, i == 0 && forced);
            if (!b.empty()) {
                note(res, tt, track, std::string("OCCUPIED"), b, steps.back());
                continue;
            }
            best = next_acc;
            return true;
        }
        return false;
    };

    if (!stays.empty() && dfs(0, std::nullopt, spec.arrival, {}, 1, 0, std::nullopt)) {
        res.ok = true;
        res.ops = best;
    }
    return res;
}

void Placer::note(PlaceResult& res, const std::string& top_track, const std::string& track, OptStr code,
                  const std::vector<Entry>& blocks, const Step& step) const {
    std::string label = m_.track_label(track);
    std::string what = lower_utf8(kind_label(step.kind));
    std::string msg;
    if (code == "ROUTE" && blocks.empty()) {
        msg = label + ": нет маршрута по топологии для операции «" + what + "»";
    } else if (code == "RESOURCE_NONE") {
        msg = "для операции «" + what + "» на станции нет нужного ресурса";
    } else if (!blocks.empty()) {
        const Entry& b = blocks[0];
        std::string who = describe_block(b);
        if (b.meta.type == "data") {
            code = "DATA";
            msg = who;
        } else if (b.meta.type == "closure" || b.meta.type == "maintenance") {
            code = "CLOSED";
            msg = label + ": " + who;
        } else if (code == "RESOURCE") {
            Time busy_until = blocks[0].end;
            for (const auto& x : blocks) busy_until = std::min(busy_until, x.end);
            msg = label + ": " + (last_why_ && !last_why_->empty() ? *last_why_ : std::string("нет свободного ресурса")) +
                  " для операции «" + what + "» в пределах допустимого ожидания (ближайшее освобождение — " +
                  local_hm(busy_until) + ")";
        } else if (code == "ROUTE") {
            msg = "маршрут для операции «" + what + "» на " + lower_first(label) + " занят: " + who;
        } else {
            code = "OCCUPIED";
            msg = label + ": " + who;
        }
    } else {
        msg = label + ": размещение невозможно";
    }
    res.failure_codes.insert(code ? *code : "UNKNOWN");
    if (!res.track_reasons.contains(top_track)) res.track_reasons[top_track] = {code, msg};
}

void commit_to_book(const StationModel& m, IntervalBook& book, const std::string& train_id, const std::string& number,
                    const std::vector<PlacedOp>& ops) {
    std::vector<OpDict> dicts;
    for (const auto& o : ops) {
        OpDict d;
        d.id = "tmp-" + train_id + "-" + std::to_string(o.seq);
        d.kind = o.kind;
        d.track_id = o.track_id;
        d.from_track_id = o.from_track_id;
        d.side = o.side;
        d.start = o.start;
        d.end = o.end;
        d.resource_ids = o.resource_ids;
        d.route_nodes = o.route_nodes;
        d.status = "planned";
        dicts.push_back(d);
    }
    for (const auto& sp : reservation_specs(m, train_id, number, dicts)) {
        EntryMeta meta;
        meta.type = "reservation";
        meta.train_id = train_id;
        meta.op_id = sp.operation_id;
        meta.label = sp.purpose;
        book.add(sp.key, sp.start, sp.end, meta);
    }
}

}  // namespace station
