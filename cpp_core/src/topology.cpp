#include "station/topology.hpp"

#include <algorithm>
#include <limits>
#include <queue>
#include <utility>

namespace station {

Topology::Topology(std::string station_id, const std::vector<Node>& nodes, const std::vector<TrackRow>& tracks,
                   const std::vector<Connection>& connections)
    : station_id_(std::move(station_id)), edges_(connections) {
    for (const auto& n : nodes) nodes_[n.id] = n;
    for (const auto& t : tracks) {
        tracks_[t.id] = t;
        if (main_track_id_.empty() && t.kind == "main") main_track_id_ = t.id;
    }
    // Рёбра неориентированные: каждое попадает в списки смежности обоих концов (в порядке данных).
    for (const auto& c : edges_) {
        adj_[c.from_node].emplace_back(c.to_node, &c);
        adj_[c.to_node].emplace_back(c.from_node, &c);
    }
}

std::string Topology::entry(const std::string& side) const {
    return station_id_ + "-" + (side == "west" ? "WENT" : "EENT");
}

Strings Topology::track_switch_ends(const std::string& track_id) const {
    const TrackRow& t = tracks_.at(track_id);
    Strings out;
    for (const auto& n : {t.from_node, t.to_node})
        if (nodes_.at(n).kind == "switch") out.push_back(n);
    return out;
}

OptStr Topology::track_end_on_side(const std::string& track_id, const std::string& side) const {
    const TrackRow& t = tracks_.at(track_id);
    const std::string& n = side == "west" ? t.from_node : t.to_node;
    if (nodes_.at(n).kind == "switch") return n;
    return std::nullopt;
}

bool Topology::dijkstra(const std::string& src, const std::set<std::string>& targets,
                        const std::set<std::string>& allowed_tracks, std::vector<Step>& path) const {
    const double INF = std::numeric_limits<double>::infinity();
    std::unordered_map<std::string, double> dist{{src, 0.0}};
    std::unordered_map<std::string, std::pair<std::string, const Connection*>> prev;
    // Очередь с приоритетом (расстояние, узел): при равных расстояниях раньше идёт меньший id —
    // так же упорядочивает кортежи heapq в Python.
    using Item = std::pair<double, std::string>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    pq.emplace(0.0, src);
    while (!pq.empty()) {
        auto [d, u] = pq.top();
        pq.pop();
        if (targets.count(u)) {
            path.clear();
            while (u != src) {
                const auto& [p, e] = prev.at(u);
                path.push_back({p, u, e});
                u = p;
            }
            std::reverse(path.begin(), path.end());
            return true;
        }
        auto du = dist.find(u);
        if (d > (du == dist.end() ? INF : du->second)) continue;
        auto it = adj_.find(u);
        if (it == adj_.end()) continue;
        for (const auto& [v, e] : it->second) {
            if (e->kind == "track" && !(e->track_id && allowed_tracks.count(*e->track_id))) continue;
            double w = e->length_m + (e->kind == "track" ? 2000.0 : 0.0);
            double nd = d + w;
            auto dv = dist.find(v);
            if (nd < (dv == dist.end() ? INF : dv->second)) {
                dist[v] = nd;
                prev[v] = {u, e};
                pq.emplace(nd, v);
            }
        }
    }
    return false;
}

RoutePtr Topology::route(const std::string& src, const std::set<std::string>& targets) const {
    std::set<std::string> allowed;
    if (!main_track_id_.empty()) allowed.insert(main_track_id_);
    std::vector<Step> path;
    if (!dijkstra(src, targets, allowed, path)) return nullptr;
    auto r = std::make_shared<Route>();
    r->node_ids.push_back(src);
    for (const auto& s : path) {
        r->node_ids.push_back(s.to);
        r->length_m += s.edge->length_m;
        if (s.edge->kind == "track") r->through_track_ids.push_back(*s.edge->track_id);
    }
    for (const auto& n : r->node_ids)
        if (nodes_.at(n).kind == "switch") r->switch_ids.push_back(n);
    return r;
}

RoutePtr Topology::arrival_route(const std::string& side, const std::string& track_id) const {
    std::string key = "arr|" + side + "|" + track_id;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    RoutePtr r;
    if (track_id == main_track_id_) {
        OptStr end = track_end_on_side(track_id, side);
        // В Python цель {None} недостижима — маршрута нет.
        r = end ? route(entry(side), {*end}) : nullptr;
    } else {
        Strings ends = track_switch_ends(track_id);
        r = ends.empty() ? nullptr : route(entry(side), std::set<std::string>(ends.begin(), ends.end()));
    }
    cache_[key] = r;
    return r;
}

RoutePtr Topology::departure_route(const std::string& side, const std::string& track_id) const {
    std::string key = "dep|" + side + "|" + track_id;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    RoutePtr arr = arrival_route(side, track_id);
    RoutePtr r;
    if (arr) {
        // Отправление — тот же маршрут в обратную сторону.
        auto rev = std::make_shared<Route>(*arr);
        std::reverse(rev->node_ids.begin(), rev->node_ids.end());
        std::reverse(rev->switch_ids.begin(), rev->switch_ids.end());
        r = rev;
    }
    cache_[key] = r;
    return r;
}

RoutePtr Topology::shunting_route(const std::string& from_track, const std::string& to_track) const {
    std::string key = "sh|" + from_track + "|" + to_track;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    RoutePtr best;
    Strings to_ends = track_switch_ends(to_track);
    std::set<std::string> targets(to_ends.begin(), to_ends.end());
    for (const auto& a : track_switch_ends(from_track)) {
        RoutePtr r = route(a, targets);
        if (r && (!best || r->length_m < best->length_m)) best = r;
    }
    cache_[key] = best;
    return best;
}

}  // namespace station
