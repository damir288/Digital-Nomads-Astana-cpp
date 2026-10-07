"""Выгрузка снимков состояния станции и эталонных результатов Python-ядра.

Запускается внутри контейнера backend (см. cpp_core/README.md):
    python /export/export_snapshot.py /export/data

Для каждого случая (сценарий + сдвиг модельного времени + датчики) пишет:
    <case>.snapshot.json — входные данные ядра (то, что StationModel читает из БД);
    <case>.golden.json   — результаты Python: прогноз, конфликты, проверка заявок,
                           индекс, эвристический план, проверка плана.
C++-реализация читает snapshot и должна выдать то же, что golden.
"""
import json
import os
import sys
import uuid
from datetime import timedelta
from types import SimpleNamespace

os.environ["ENGINE_ENABLED"] = "false"
os.environ["MQTT_ENABLED"] = "false"
os.environ["SEED_OPTIMIZE"] = "false"
os.environ.setdefault("ATTACHMENTS_DIR", "/tmp/ds_attachments")
BASE_URL = os.environ.get("DATABASE_URL", "postgresql+psycopg://station:station_dev_password@db:5432/station")
os.environ["DATABASE_URL"] = BASE_URL.rsplit("/", 1)[0] + "/station_cpp"

sys.path.insert(0, "/app")

from alembic import command  # noqa: E402
from alembic.config import Config  # noqa: E402
from sqlalchemy import create_engine, select, text  # noqa: E402

from app.config import get_settings  # noqa: E402

get_settings.cache_clear()


def prepare_db():
    admin = create_engine(BASE_URL, isolation_level="AUTOCOMMIT")
    with admin.connect() as c:
        if not c.execute(text("SELECT 1 FROM pg_database WHERE datname='station_cpp'")).first():
            c.execute(text("CREATE DATABASE station_cpp"))
    admin.dispose()
    url = os.environ["DATABASE_URL"]
    eng = create_engine(url)
    with eng.begin() as c:
        c.execute(text("DROP SCHEMA public CASCADE; CREATE SCHEMA public; CREATE EXTENSION IF NOT EXISTS btree_gist"))
    eng.dispose()
    cfg = Config("/app/alembic.ini")
    cfg.set_main_option("script_location", "/app/alembic")
    cfg.attributes["url"] = url
    command.upgrade(cfg, "head")
    from app.db import reset_engine
    reset_engine(url)


def fresh_observations(db, overrides=None, age_s=0.5):
    """Как в tests/conftest.py: актуальные показания рельсовых цепей, согласованные с учётной занятостью."""
    from app.core.timeutil import utcnow
    from app.iot.quality import _mismatch_since, expected_occupancy
    from app.models import Device, Observation
    from app.services.model import StationModel
    _mismatch_since.clear()
    model = StationModel(db, data_states={})
    occ = expected_occupancy(model)
    now = utcnow() - timedelta(seconds=age_s)
    for d in db.execute(select(Device).where(Device.kind == "track_circuit")).scalars():
        val = {"occupied": d.object_id in occ}
        if overrides and d.object_id in overrides:
            val = overrides[d.object_id]
            if val is None:
                continue
        o = db.get(Observation, (d.object_id, "occupancy", d.id))
        if o is None:
            db.add(Observation(object_id=d.object_id, attribute="occupancy", device_id=d.id, value=val,
                               observed_at=now, received_at=now, quality="good", event_id=uuid.uuid4().hex))
        else:
            o.value, o.observed_at, o.received_at, o.quality = val, now, now, "good"
        d.last_heartbeat_at = now
        d.last_seen_at = now
        d.health = {**(d.health or {}), "last_observed_at": now.isoformat()}
    db.commit()


# ---------------------------------------------------------------- сериализация
def iso(dt):
    from app.core.timeutil import iso as _iso
    return _iso(dt)


def jdefault(o):
    if hasattr(o, "isoformat"):
        return iso(o)
    if isinstance(o, set):
        return sorted(o)
    raise TypeError(type(o))


def snapshot(model, db):
    from app.models import IndexConfig, TransferRequest
    from app.services.index import active_config
    from zoneinfo import ZoneInfo
    tzi = ZoneInfo(get_settings().display_timezone)
    off = model.now.astimezone(tzi).utcoffset()
    for d in (-3, -1, 1, 3):  # в часовом поясе станции нет перехода на летнее время
        assert (model.now + timedelta(days=d)).astimezone(tzi).utcoffset() == off
    st = model.station
    ic = active_config(db)
    return {
        "now": iso(model.now), "version": model.version, "tz_offset_min": int(off.total_seconds() // 60),
        "station": {"id": st.id, "name": st.name, "config": st.config},
        "neighbors": [{"id": n.id, "name": n.name, "config": n.config} for n in model.neighbors.values()],
        "nodes": [dict(id=n["id"], kind=n["kind"], name=n["name"], x=n["x"], y=n["y"], side=n["side"])
                  for n in model.topo.nodes.values()],
        "tracks": [{"id": t.id, "number": t.number, "kind": t.kind, "useful_length_m": t.useful_length_m,
                    "allowed_train_kinds": t.allowed_train_kinds, "from_node": t.from_node, "to_node": t.to_node,
                    "zone_id": t.zone_id} for t in model.track_rows.values()],
        "connections": [{"id": c["id"], "from_node": c["from_node"], "to_node": c["to_node"], "kind": c["kind"],
                         "track_id": c["track_id"], "length_m": c["length_m"]} for c in model.topo.edges.values()],
        "resources": [{"id": r.id, "kind": r.kind, "name": r.name, "home_zone_id": r.home_zone_id, "status": r.status}
                      for r in model.resources.values()],
        "shifts": {rid: [[iso(a), iso(b)] for a, b in sh] for rid, sh in model.shifts.items()},
        "maintenance": [{"id": m.id, "object_type": m.object_type, "object_id": m.object_id,
                         "start_at": iso(m.start_at), "end_at": iso(m.end_at), "reason": m.reason}
                        for m in model.maintenance],
        "incidents": [{"id": i.id, "kind": i.kind, "title": i.title, "object_id": i.object_id,
                       "start_at": iso(i.start_at), "end_at": iso(i.end_at)} for i in model.incidents],
        "trains": [{k: (iso(v) if hasattr(v, "isoformat") else v) for k, v in {
            "id": t.id, "number": t.number, "kind": t.kind, "priority": t.priority,
            "destination_station_id": t.destination_station_id, "arrival_side": t.arrival_side,
            "departure_side": t.departure_side, "wagons_count": t.wagons_count, "length_m": t.length_m,
            "loco_length_m": t.loco_length_m, "status": t.status, "scheduled_arrival": t.scheduled_arrival,
            "expected_arrival": t.expected_arrival, "scheduled_departure": t.scheduled_departure,
            "current_track_id": t.current_track_id}.items()} for t in model.trains.values()],
        # порядок как в StationModel: по поезду и номеру в цепочке
        "operations": [{k: (iso(v) if hasattr(v, "isoformat") else v) for k, v in {
            "id": o.id, "train_id": o.train_id, "kind": o.kind, "seq": o.seq, "track_id": o.track_id,
            "from_track_id": o.from_track_id, "side": o.side, "duration_min": o.duration_min,
            "requirements": o.requirements or [], "resource_ids": o.resource_ids or [],
            "route_nodes": o.route_nodes or [], "planned_start": o.planned_start, "planned_end": o.planned_end,
            "not_before": o.not_before, "forecast_start": o.forecast_start, "forecast_end": o.forecast_end,
            "actual_start": o.actual_start, "actual_end": o.actual_end, "status": o.status,
            "extra_delay_min": o.extra_delay_min, "reserved": o.reserved, "note": o.note}.items()}
            for o in model.ops.values()],
        "wagons": [{"id": w.id, "train_id": w.train_id, "number": w.number, "length_m": w.length_m,
                    "condition": w.condition} for ws in model.wagons_by_train.values() for w in ws],
        "rules": [{"code": r.code, "name": r.name, "unit": r.unit, "period": r.period, "source": r.source,
                   "rule_text": r.rule_text, "value": r.value, "policy": r.policy, "active": r.active}
                  for r in model.rules.values()],
        "plan": None if model.plan is None else {"target_wagons": model.plan.target_wagons,
                                                  "actual_base_wagons": model.plan.actual_base_wagons,
                                                  "policy": model.plan.policy},
        "reservations": [{"id": r.id, "resource_key": r.resource_key, "start_at": iso(r.start_at),
                          "end_at": iso(r.end_at), "purpose": r.purpose, "train_id": r.train_id,
                          "operation_id": r.operation_id, "request_id": r.request_id}
                         for r in model.reservations],
        "data_states": model.data_states,
        "index_config": {"version": ic.version, "config": ic.config},
        "requests": [],  # заполняется ниже
    }


def request_cases(model, db):
    """Заявки из БД + синтетические варианты (разное время, длина, станция, в прошлом)."""
    from app.models import TransferRequest
    out = []
    for r in db.execute(select(TransferRequest).order_by(TransferRequest.id)).scalars():
        out.append({"id": r.id, "number": r.number, "from_station_id": r.from_station_id,
                    "wagons_count": r.wagons_count, "wagon_kind": r.wagon_kind, "train_length_m": r.train_length_m,
                    "priority": r.priority, "split_allowed": r.split_allowed,
                    "desired_departure": iso(r.desired_departure), "train_id": r.train_id})
    now = model.now
    variants = [
        ("S1", "OTR", 40, "gondola", None, 30, True), ("S2", "OTR", 57, "covered", None, 120, True),
        ("S3", "ZHT", 30, "tank", None, 200, False), ("S4", "OTR", 70, "gondola", None, 45, True),
        ("S5", "ZHT", 20, None, 400.0, 360, True), ("S6", "OTR", 40, None, None, 60, True),
        ("S7", "OTR", 40, "gondola", None, -30, True), ("S8", "ZHT", 64, "hopper", None, 15, True),
        ("S9", "XXX", 10, "flat", None, 90, False),
    ]
    for num, frm, wag, kind, length, dep_min, split in variants:
        out.append({"id": f"RQ-{num}", "number": num, "from_station_id": frm, "wagons_count": wag, "wagon_kind": kind,
                    "train_length_m": length, "priority": 2, "split_allowed": split,
                    "desired_departure": iso(now + timedelta(minutes=dep_min)), "train_id": None})
    return out


def golden(model, db, requests):
    from datetime import datetime
    from app.services.checker import check_request
    from app.services.conflicts import detect
    from app.services.forecast import forecast
    from app.services.index import compute_current, compute_plan_index
    from app.services.planner import PlanBuilder, diff_schedule, plan_kpis, verify
    fc = forecast(model)
    det = detect(model, fc)
    checks = {}
    for r in requests:
        req = SimpleNamespace(**{**r, "desired_departure": datetime.fromisoformat(r["desired_departure"].replace("Z", "+00:00"))})
        res = check_request(model, req, book=model.build_book())
        res.pop("calc_ms", None)
        res.pop("computed_real_at", None)
        checks[r["id"]] = res
    idx = compute_current(db, model, det["conflicts"])
    idx.pop("computed_real_at", None)
    pb = PlanBuilder(model, fc)
    greedy = pb.solve_greedy()
    unresolved_tracks = {u["track_id"] for u in pb.unresolved if u.get("track_id")}
    errs = verify(model, greedy.schedule, unresolved_tracks)
    kpi_before = plan_kpis(model, {oid: {"start": fc[oid][0], "end": fc[oid][1]} for oid in fc
                                   if oid in model.ops and model.ops[oid].status not in ("done", "cancelled")},
                           det["conflicts"])
    kpi_after = plan_kpis(model, greedy.schedule, [])
    # CP-SAT: статус, цель и независимая проверка (результат зависит от версии решателя и времени,
    # поэтому сравнивается не расписание, а оптимальное значение цели и прохождение проверки)
    pb_cp = PlanBuilder(model, fc)
    cp = pb_cp.solve_cpsat(20.0, 8)
    cp_verify = verify(model, cp.schedule, {u["track_id"] for u in pb_cp.unresolved if u.get("track_id")}) \
        if cp.status in ("optimal", "feasible") else []
    return {
        "cpsat": {"status": cp.status, "objective": cp.objective, "verify": cp_verify},
        "forecast": {k: [iso(a), iso(b)] for k, (a, b) in fc.items()},
        "conflicts": det["conflicts"], "delays": det["delays"],
        "checks": checks,
        "index": idx,
        "plan_builder": {"stays": [{"track0": s["track0"], "cands": s["cands"], "fixed": s["fixed"],
                                    "ops": [ci.op.id for ci in s["ops"]]} for s in pb.stays],
                         "unresolved": pb.unresolved},
        "greedy": {"status": greedy.status, "schedule": greedy.schedule, "unresolved": greedy.unresolved,
                   "verify": errs, "changes": diff_schedule(model, greedy.schedule),
                   "kpi_before": kpi_before, "kpi_after": kpi_after,
                   "index_before": compute_plan_index(model, None, kpi_before),
                   "index_after": compute_plan_index(model, greedy.schedule, kpi_after)},
    }


CASES = [
    # (имя, сценарий, минут вперёд, шагов движка, датчики: путь -> значение / None = нет данных)
    ("normal_t0", "normal", 0, 0, None),
    ("normal_t90", "normal", 90, 6, None),
    ("tracks_busy", "demo_tracks_busy", 0, 0, None),
    ("track_frees", "demo_track_frees", 0, 0, None),
    ("plan_exceeded", "demo_plan_exceeded", 0, 0, None),
    ("peak", "peak_arrivals", 20, 2, None),
    ("closure", "track_closure", 30, 3, None),
    ("cargo_delay", "cargo_delay", 40, 4, None),
    ("faulty_wagon", "faulty_wagon", 60, 6, None),
    ("neighbor", "neighbor_restriction", 30, 3, None),
    ("sensor_loss", "normal", 10, 1, {"ALM-T3": None, "ALM-T4": None}),
    ("multi_10", "multi_10", 30, 3, None),
    ("faulty_t150", "faulty_wagon", 150, 10, None),
    ("hard_quota", "demo_plan_exceeded", 0, 0, None),
    ("limits_hard", "normal", 0, 0, None),
    ("limits_soft", "normal", 0, 0, None),
    ("no_loco", "normal", 0, 0, None),
    ("maintenance", "normal", 30, 3, None),
    ("faulty_flag", "normal", 60, 4, None),
]


def mutate(name, db):
    """Изменения данных, чтобы задеть редкие ветки алгоритмов."""
    from app.models import CapacityRule, MaintenanceWindow, Plan, SimState, Station
    from app.core.timeutil import aware
    if name == "hard_quota":
        for p in db.execute(select(Plan)).scalars():
            p.policy = "hard_quota"
    elif name in ("limits_hard", "limits_soft"):
        for r in db.execute(select(CapacityRule)).scalars():
            if r.code == "DAILY_PROCESSING":
                r.value, r.policy = 300, ("hard" if name == "limits_hard" else "soft")
            if r.code.startswith("ENTRY_THROUGHPUT") and name == "limits_hard":
                r.value = 1
    elif name == "no_loco":
        st = db.get(Station, "OTR")
        st.config = {**st.config, "locomotives_available": 0}
    elif name == "maintenance":
        # окно обслуживания — на путь ближайшего планового прибытия, поверх его стоянки
        from app.models import Operation
        now = aware(db.get(SimState, 1).model_time)
        arr = db.execute(select(Operation).where(Operation.kind == "arrival", Operation.status == "planned",
                                                 Operation.planned_start > now + timedelta(minutes=15))
                         .order_by(Operation.planned_start)).scalars().first()
        db.add(MaintenanceWindow(id="MW-CPP-1", station_id="ALM", object_type="track", object_id=arr.track_id,
                                 start_at=aware(arr.planned_start) - timedelta(minutes=10),
                                 end_at=aware(arr.planned_start) + timedelta(minutes=50),
                                 reason="Ремонт пути (тест C++)"))
        db.add(MaintenanceWindow(id="MW-CPP-2", station_id="ALM", object_type="switch", object_id="ALM-W2",
                                 start_at=now + timedelta(minutes=60), end_at=now + timedelta(minutes=90),
                                 reason="Осмотр стрелки (тест C++)"))
    elif name == "faulty_flag":
        # неисправный вагон в составе стоящего поезда без отцепки
        from app.models import Train, Wagon
        t = db.execute(select(Train).where(Train.status == "on_station").order_by(Train.id)).scalars().first()
        w = db.execute(select(Wagon).where(Wagon.train_id == t.id).order_by(Wagon.position)).scalars().first()
        w.condition = "faulty"
        t2 = db.execute(select(Train).where(Train.status == "on_station", Train.id != t.id).order_by(Train.id)).scalars().first()
        if t2:
            w2 = db.execute(select(Wagon).where(Wagon.train_id == t2.id).order_by(Wagon.position)).scalars().first()
            w2.condition = "restricted"
    db.commit()


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    prepare_db()
    from app.db import SessionLocal
    from app.models import SimState
    from app.services.model import StationModel
    from app.sim.engine import Engine
    from app.sim.seed import reset_world
    from app.core.timeutil import aware
    only = set(sys.argv[2:])
    for name, scenario, minutes, steps, overrides in CASES:
        if only and name not in only:
            continue
        with SessionLocal() as db:
            reset_world(db, "large", scenario, 42)
            db.commit()
            fresh_observations(db)
        eng = Engine()
        for _ in range(steps):
            with SessionLocal() as db:
                s = db.get(SimState, 1)
                s.model_time = aware(s.model_time) + timedelta(minutes=minutes / steps)
                db.commit()
                fresh_observations(db)
            eng.step(advance=False)
        with SessionLocal() as db:
            mutate(name, db)
        with SessionLocal() as db:
            if overrides:
                ids = {k for k in overrides}
                fresh_observations(db, overrides={k: v for k, v in overrides.items()})
                from app.models import Observation
                for o in db.execute(select(Observation)).scalars():
                    if o.object_id in ids:
                        o.observed_at = o.received_at = aware(o.observed_at) - timedelta(minutes=10)
                db.commit()
            else:
                fresh_observations(db)
            model = StationModel(db)
            snap = snapshot(model, db)
            snap["requests"] = request_cases(model, db)
            snap["case"] = name
            gold = golden(model, db, snap["requests"])
            db.rollback()
        with open(os.path.join(out_dir, f"{name}.snapshot.json"), "w", encoding="utf-8") as f:
            json.dump(snap, f, ensure_ascii=False, default=jdefault, indent=1)
        with open(os.path.join(out_dir, f"{name}.golden.json"), "w", encoding="utf-8") as f:
            json.dump(gold, f, ensure_ascii=False, default=jdefault, indent=1)
        print(f"{name}: ops={len(snap['operations'])} trains={len(snap['trains'])} "
              f"conflicts={len(gold['conflicts'])} greedy={gold['greedy']['status']} verify_errors={len(gold['greedy']['verify'])} "
              f"cpsat={gold['cpsat']['status']} obj={gold['cpsat']['objective']} cpsat_verify={len(gold['cpsat']['verify'])}",
              flush=True)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/export/data")
