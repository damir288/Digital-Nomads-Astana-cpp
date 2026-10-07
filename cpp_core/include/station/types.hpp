// Данные станции (то, что Python-версия читает из таблиц PostgreSQL через SQLAlchemy).
// Здесь они загружаются из JSON-снимка (см. tools/export_snapshot.py).
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "station/pyfmt.hpp"
#include "station/time.hpp"

namespace station {

using OptStr = std::optional<std::string>;
using OptTime = std::optional<Time>;
using Strings = std::vector<std::string>;

// Словарь, который помнит порядок вставки ключей — как dict в Python. От порядка обхода
// зависят результаты (например, порядок перебора путей и ресурсов), поэтому он важен.
template <class V>
class OrderedMap {
public:
    V& operator[](const std::string& key) {
        auto it = index_.find(key);
        if (it != index_.end()) return items_[it->second].second;
        index_[key] = items_.size();
        items_.emplace_back(key, V{});
        return items_.back().second;
    }
    const V* find(const std::string& key) const {
        auto it = index_.find(key);
        return it == index_.end() ? nullptr : &items_[it->second].second;
    }
    V* find(const std::string& key) {
        auto it = index_.find(key);
        return it == index_.end() ? nullptr : &items_[it->second].second;
    }
    bool contains(const std::string& key) const { return index_.count(key) != 0; }
    const V& at(const std::string& key) const { return items_.at(index_.at(key)).second; }
    V& at(const std::string& key) { return items_.at(index_.at(key)).second; }
    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    auto begin() const { return items_.begin(); }
    auto end() const { return items_.end(); }
    auto begin() { return items_.begin(); }
    auto end() { return items_.end(); }

private:
    std::vector<std::pair<std::string, V>> items_;
    std::unordered_map<std::string, size_t> index_;
};

struct Node {
    std::string id, kind, name;
    OptStr side;
};

struct TrackRow {
    std::string id, number, kind, from_node, to_node;
    std::optional<double> useful_length_m;
    Strings allowed_train_kinds;
    OptStr zone_id;
};

struct Connection {
    std::string id, from_node, to_node, kind;
    OptStr track_id;
    double length_m = 0;
};

struct Resource {
    std::string id, kind, name, status;
    OptStr home_zone_id;
};

struct Train {
    std::string id, number, kind, arrival_side, departure_side, status;
    int priority = 1;
    int wagons_count = 0;
    std::optional<double> length_m;
    double loco_length_m = 0;
    OptStr destination_station_id, current_track_id;
    OptTime scheduled_arrival, expected_arrival, scheduled_departure;
};

struct Operation {
    std::string id, kind, status;
    OptStr train_id, track_id, from_track_id, side, note;
    int seq = 0;
    int duration_min = 0;
    int extra_delay_min = 0;
    bool reserved = false;
    Strings requirements, resource_ids, route_nodes;
    Time planned_start = 0, planned_end = 0;
    OptTime not_before, forecast_start, forecast_end, actual_start, actual_end;
};

struct Wagon {
    std::string id, number, condition;
    OptStr train_id;
    std::optional<double> length_m;
};

struct Incident {
    std::string id, kind, title;
    OptStr object_id;
    Time start_at = 0;
    OptTime end_at;
};

struct MaintenanceWindow {
    std::string id, object_type, object_id, reason;
    Time start_at = 0, end_at = 0;
};

struct CapacityRule {
    std::string code, name, unit, period, source, rule_text, policy;
    std::optional<double> value;
    bool active = true;
};

struct PlanRow {
    int target_wagons = 0;
    int actual_base_wagons = 0;
    std::string policy;
};

struct Reservation {
    std::string resource_key, purpose;
    OptStr train_id, operation_id, request_id;
    long long id = 0;
    Time start_at = 0, end_at = 0;
};

struct Neighbor {
    std::string id, name;
    json config;
};

// Заявка на передачу состава со станции-соседа (TransferRequest).
struct Request {
    std::string id, number, from_station_id;
    int wagons_count = 0;
    OptStr wagon_kind, train_id;
    std::optional<double> train_length_m;
    int priority = 2;          // 0 в данных = «не задан» (в Python: req.priority or 2)
    bool split_allowed = false;
    Time desired_departure = 0;
};

}  // namespace station
