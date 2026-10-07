# Ядро «Цифровой станции» на C++

Перенос алгоритмического ядра backend (Python) на C++17. Вход и результаты те же, что у
Python-версии; совпадение проверяется автоматически на 19 сценариях (59 342 значения, включая
каждую строку объяснения на русском).

## Что перенесено

| Python (`backend/app/…`) | C++ (`cpp_core/…`) | Что делает |
|---|---|---|
| `core/timeutil.py` | `src/time.cpp` | время в UTC, «ЧЧ:ММ» в поясе станции, округление до минут |
| `domain/topology.py` (класс `Topology`) | `src/topology.cpp` | граф станции, маршруты (Дейкстра), стрелки и транзитные пути |
| `services/model.py` | `src/model.cpp` | снимок станции, книга интервалов, стоянки, резервы |
| `services/placement.py` | `src/placement.cpp` | размещение цепочки операций (earliest-fit, best-fit по путям) |
| `services/checker.py` | `src/checker.cpp` | проверка заявки на приём: план, переработка, вход, сосед, путь, ресурсы |
| `services/alternatives.py` | `src/alternatives.cpp` | перенос, другая станция, изменение очереди, разделение партии |
| `services/forecast.py` | `src/forecast.cpp` | прогноз операций, задержки |
| `services/conflicts.py` | `src/conflicts.cpp` | детектор конфликтов (пути, маршруты, ресурсы, закрытия, вагоны…) |
| `services/index.py` | `src/index.cpp` | индекс эффективности 0–100 |
| `services/planner.py` | `src/planner.cpp`, `src/planner_cpsat.cpp` | эвристика, модель CP-SAT (OR-Tools), независимая проверка плана, KPI |

**Не перенесено** (это не алгоритмы, а инфраструктура вокруг них): HTTP API и WebSocket (FastAPI),
работа с PostgreSQL, приём MQTT-телеметрии, PDF-отчёты, симулятор, 3D-геометрия схемы. Ядро
получает состояние станции JSON-снимком — тем же набором данных, который Python читает из БД.

## Сборка (macOS / Linux)

```bash
brew install cmake nlohmann-json or-tools pkgconf    # Linux: пакеты cmake, nlohmann-json, OR-Tools
cd cpp_core
cmake -S . -B build
cmake --build build -j8
```

Без OR-Tools проект тоже собирается (`-DSTATION_WITH_ORTOOLS=OFF`), тогда планировщик —
только эвристика.

## Запуск

```bash
./build/station_core check     tools/data/normal_t0.snapshot.json RQ-Z-0001   # проверка заявки
./build/station_core conflicts tools/data/closure.snapshot.json              # конфликты и задержки
./build/station_core index     tools/data/normal_t90.snapshot.json           # индекс эффективности
./build/station_core plan      tools/data/peak.snapshot.json 3.5 8           # перепланирование
./build/station_core golden    tools/data/peak.snapshot.json                 # всё сразу (для сравнения)
```

Результат — JSON в stdout.

## Тесты

```bash
./build/unit_tests                                          # модульные тесты базовых частей
python3 tests/compare.py build/station_core tools/data      # сравнение с Python на всех сценариях
python3 tests/compare.py build/station_core tools/data --cpsat=20   # плюс сравнение CP-SAT
```

Сравнение проходит по каждому полю: решения, тексты объяснений, причины по путям, альтернативы,
конфликты и их идентификаторы, индекс, расписание эвристики, KPI. Для CP-SAT сравниваются
доказанные оптимальные значения цели и результат независимой проверки плана (само расписание у
равноценных оптимумов может отличаться).

### Эталонные данные

`tools/data/*.snapshot.json` (вход) и `*.golden.json` (результат Python) получены скриптом
`tools/export_snapshot.py` внутри контейнера backend, на тех же сценариях, что и тесты проекта
(`normal`, `peak_arrivals`, `track_closure`, `faulty_wagon`, `multi_10` и др.), плюс изменения
данных для редких веток (жёсткая квота, лимиты переработки, нет локомотива у соседа, окно
обслуживания, неисправный вагон, потеря датчиков). Перегенерация:

```bash
docker compose up -d db && docker compose build backend
docker compose run --rm --no-deps -v "$PWD/cpp_core/tools:/export" backend \
    python /export/export_snapshot.py /export/data
```

## Тонкие места переноса

Чтобы тексты и числа совпадали с Python до символа, повторены его правила:

- **Порядок словарей.** `dict` в Python помнит порядок вставки, а от него зависит порядок
  перебора путей и ресурсов. В C++ для этого `OrderedMap` и `nlohmann::ordered_json`.
- **`sum()` в Python 3.12+** складывает дробные числа с компенсацией ошибки (алгоритм Ноймайера).
  Обычный цикл даёт другой последний бит, например 49.65000000000001 вместо 49.64999999999999,
  и после `round(…, 1)` индекс выходит 49.7 вместо 49.6. Функция `py_sum` повторяет алгоритм CPython.
- **FMA.** Компилятор может объединять `a*b+c` в одну инструкцию, и это тоже меняет последний бит.
  Сборка идёт с `-ffp-contract=off`.
- **Форматирование.** `f"{x:.1f}"`, `f"{u:.0%}"`, `str(10.0)`, `round()` с округлением к чётному —
  `src/pyfmt.cpp`.
- **Идентификаторы конфликтов** — `sha1("|".join(parts))[:10]`, своя реализация SHA-1
  (`src/sha1.cpp`).
- **`heapq` с кортежами (расстояние, узел)**: при равных расстояниях Дейкстра выбирает узел с
  меньшим id. В C++ это тот же `std::priority_queue` по паре (расстояние, строка).
