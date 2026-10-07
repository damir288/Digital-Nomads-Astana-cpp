// Логическая топология станции (аналог класса Topology из app/domain/topology.py).
//
// Граф: узлы — стрелки, входы и упоры; рёбра — пути, соединения стрелок («лестницы» горловин)
// и подходы. Маршрут движения — кратчайший путь (Дейкстра); стрелки маршрута резервируются на
// время движения, отсюда конфликты маршрутов в горловине. Транзит по путям, кроме главного,
// запрещён; проход по пути «стоит» на 2000 м больше его длины, чтобы маршрут шёл по стрелкам.
//
// Геометрия схемы (координаты точек для 3D) в C++-ядро не перенесена: решения от неё не зависят.
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "station/types.hpp"

namespace station {

struct Route {
    Strings node_ids;           // все узлы маршрута
    Strings switch_ids;         // стрелки маршрута (резервируются)
    Strings through_track_ids;  // пути, по которым маршрут проходит транзитом
    double length_m = 0;
};

using RoutePtr = std::shared_ptr<const Route>;  // nullptr — маршрута нет

class Topology {
public:
    Topology(std::string station_id, const std::vector<Node>& nodes, const std::vector<TrackRow>& tracks,
             const std::vector<Connection>& connections);

    std::string entry(const std::string& side) const;
    Strings track_switch_ends(const std::string& track_id) const;
    OptStr track_end_on_side(const std::string& track_id, const std::string& side) const;

    RoutePtr arrival_route(const std::string& side, const std::string& track_id) const;
    RoutePtr departure_route(const std::string& side, const std::string& track_id) const;
    RoutePtr shunting_route(const std::string& from_track, const std::string& to_track) const;

    const std::string& main_track_id() const { return main_track_id_; }

private:
    struct Step {
        std::string from, to;
        const Connection* edge;
    };
    bool dijkstra(const std::string& src, const std::set<std::string>& targets,
                  const std::set<std::string>& allowed_tracks, std::vector<Step>& path) const;
    RoutePtr route(const std::string& src, const std::set<std::string>& targets) const;

    std::string station_id_;
    std::unordered_map<std::string, Node> nodes_;
    std::unordered_map<std::string, TrackRow> tracks_;
    std::vector<Connection> edges_;
    std::unordered_map<std::string, std::vector<std::pair<std::string, const Connection*>>> adj_;
    std::string main_track_id_;
    mutable std::map<std::string, RoutePtr> cache_;
};

}  // namespace station
