// Снимок состояния станции для расчётов (аналог app/services/model.py).
//
// IntervalBook — «книга занятости»: по каждому ключу ресурса («track:…», «switch:…», «res:…»,
// «neighbor:…») хранит отсортированные интервалы резервов и блокировок (закрытия, окна
// обслуживания, вне смены, недостоверные данные). Все проверки пересечений идут через неё.
#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "station/topology.hpp"
#include "station/types.hpp"

namespace station {

extern const std::vector<std::pair<std::string, std::string>> KIND_LABEL_LIST;
const std::string& kind_label(const std::string& kind);       // «Прибытие», …; неизвестный вид — как есть
const std::string& res_kind_label(const std::string& kind);   // «маневровый локомотив», …
bool is_movement(const std::string& kind);                    // прибытие, отправление, манёвры, отцепка

// --------------------------------------------------------------- книга интервалов
struct EntryMeta {
    std::string type;  // reservation | closure | maintenance | shift | faulty | data | restriction
    OptStr train_id, op_id, label, data_state;
    bool fixed = false;
};

struct Entry {
    Time start = 0, end = 0;
    EntryMeta meta;
};

struct ConflictFilter {
    OptStr ignore_train;                 // резервы этого поезда не мешают
    const std::set<std::string>* ignore_ops = nullptr;
    const std::set<std::string>* kinds = nullptr;  // учитывать только эти типы
};

class IntervalBook {
public:
    void add(const std::string& key, Time start, Time end, EntryMeta meta);
    std::vector<Entry> conflicts(const std::string& key, Time start, Time end, const ConflictFilter& f = {}) const;
    const std::vector<Entry>* entries(const std::string& key) const;
    // Копия книги без интервалов указанного поезда (используется при «изменении очереди»).
    IntervalBook without_train(const std::string& train_id) const;

private:
    OrderedMap<std::vector<Entry>> data_;
};

std::string describe_block(const Entry& e);

// --------------------------------------------------------------- операции-словари
// В Python одни и те же функции принимают и строки БД, и словари; в C++ — одна структура.
struct OpDict {
    std::string id, kind;
    OptStr track_id, from_track_id, side, status, train_id;
    Time start = 0, end = 0;
    Strings resource_ids, route_nodes;
};

struct Stay {
    OptStr track_id;
    Time start = 0, end = 0;
    Strings ops;
};

struct ReservationSpec {
    std::string key, purpose, operation_id;
    OptStr train_id;
    Time start = 0, end = 0;
};

// --------------------------------------------------------------- модель станции
class StationModel {
public:
    explicit StationModel(const json& snapshot);

    Time now = 0;
    long long version = 0;
    json cfg;                     // конфигурация станции (processing, zones, zone_travel_min, …)
    std::string sid, station_name;
    OrderedMap<Neighbor> neighbors;
    std::unique_ptr<Topology> topo;
    OrderedMap<TrackRow> track_rows;
    OrderedMap<Resource> resources;
    OrderedMap<std::vector<std::pair<Time, Time>>> shifts;
    std::vector<MaintenanceWindow> maintenance;
    std::vector<Incident> incidents;
    OrderedMap<Train> trains;
    OrderedMap<Operation> ops;                    // по поезду и номеру в цепочке
    OrderedMap<std::vector<std::string>> ops_by_train;  // id поезда -> id операций
    OrderedMap<std::vector<Wagon>> wagons_by_train;
    OrderedMap<CapacityRule> rules;
    std::optional<PlanRow> plan;
    std::vector<Reservation> reservations;
    json data_states;             // путь -> {state, observed, message, device_id, …}
    json index_config;            // {version, config}
    std::vector<Request> requests;

    // справочные
    std::string track_label(const OptStr& tid) const;
    std::string track_label_lc(const OptStr& tid) const;
    std::string train_label(const OptStr& train_id) const;
    int zone_travel(const OptStr& a, const OptStr& b) const;
    OptStr op_zone(const std::string& kind, const OptStr& track_id) const;
    bool in_shift(const std::string& rid, Time s, Time e) const;
    std::optional<double> train_length(const Train& t) const;
    const Train* train(const OptStr& id) const;
    std::vector<const Operation*> train_ops(const std::string& train_id) const;
    const json* data_state(const std::string& track_id) const;
    std::string data_state_name(const std::string& track_id) const;  // "" если нет

    // книга интервалов
    IntervalBook build_book(bool include_reservations = true, bool include_data_blocks = true) const;
    void add_blocks(IntervalBook& b, bool include_data_blocks = true) const;

    // числа из конфигурации processing
    int track_buffer_min() const;
    json processing(const char* key, const json& def) const;
};

std::vector<Stay> stays_of(const std::vector<OpDict>& ops);
std::vector<ReservationSpec> reservation_specs(const StationModel& m, const OptStr& train_id,
                                               const std::string& train_number, const std::vector<OpDict>& ops);
std::vector<OpDict> with_presence(const StationModel& m, const Train* train, const std::vector<OpDict>& live);
Strings through_tracks(const StationModel& m, const OpDict& o);

Request request_from_json(const json& r);

}  // namespace station
