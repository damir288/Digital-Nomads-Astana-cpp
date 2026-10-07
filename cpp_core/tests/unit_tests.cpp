// Модульные тесты базовых частей ядра (без внешних фреймворков).
// Полная проверка алгоритмов — сравнение с эталоном Python: tests/compare.py.
#include <iostream>
#include <string>

#include "station/model.hpp"
#include "station/pyfmt.hpp"
#include "station/sha1.hpp"
#include "station/time.hpp"

using namespace station;

static int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++failures;                                                             \
            std::cerr << __FILE__ << ":" << __LINE__ << ": не выполнено: " #cond "\n"; \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        auto va = (a);                                                                              \
        auto vb = (b);                                                                              \
        if (!(va == vb)) {                                                                          \
            ++failures;                                                                             \
            std::cerr << __FILE__ << ":" << __LINE__ << ": " #a " = «" << va << "», ожидалось «" << vb << "»\n"; \
        }                                                                                           \
    } while (0)

static void test_time() {
    set_display_offset_min(300);
    Time t = parse_iso("2026-10-01T07:10:00Z");
    CHECK_EQ(iso(t), std::string("2026-10-01T07:10:00Z"));
    CHECK_EQ(isoformat_utc(t), std::string("2026-10-01T07:10:00+00:00"));
    CHECK_EQ(local_hm(t), std::string("12:10"));
    CHECK_EQ(parse_iso("2026-10-01T12:10:00+05:00"), t);
    CHECK_EQ(iso(parse_iso("2026-10-01T07:10:00.123456Z")), std::string("2026-10-01T07:10:00.123456Z"));
    CHECK_EQ(ceil_to(t + SECOND, 1), t + MINUTE);
    CHECK_EQ(ceil_to(t, 1), t);
    CHECK_EQ(floor_div(-1, 60), -1LL);
    CHECK_EQ(iso(from_civil({2100, 1, 1, 0, 0, 0, 0})), std::string("2100-01-01T00:00:00Z"));
}

static void test_format() {
    CHECK_EQ(py_repr(10.0), std::string("10.0"));
    CHECK_EQ(py_repr(13.92), std::string("13.92"));
    CHECK_EQ(py_repr(0.1), std::string("0.1"));
    CHECK_EQ(py_str(json(10)), std::string("10"));
    CHECK_EQ(fmt_f(780.2, 1), std::string("780.2"));
    CHECK_EQ(fmt_pct(0.45, 0), std::string("45%"));
    CHECK_EQ(fmt_g(34.0), std::string("34"));
    CHECK_EQ(py_round(2.675, 2), 2.67);  // как в Python: двоичное 2.675 чуть меньше
    CHECK_EQ(py_round0(2.5), 2LL);       // округление к чётному
    CHECK_EQ(lower_first("Путь 3"), std::string("путь 3"));
    CHECK_EQ(lower_first("Главный путь I"), std::string("главный путь I"));
    CHECK_EQ(lower_utf8("Отцепка вагона"), std::string("отцепка вагона"));
    CHECK_EQ(zfill("I", 3), std::string("00I"));
}

static void test_sha1() {
    CHECK_EQ(sha1_hex("abc"), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ(sha1_hex(""), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
}

static void test_interval_book() {
    IntervalBook b;
    Time t0 = parse_iso("2026-10-01T07:00:00Z");
    EntryMeta a;
    a.type = "reservation";
    a.train_id = std::string("T1");
    a.label = std::string("первый");
    EntryMeta c = a;
    c.label = std::string("второй");
    b.add("track:X", t0, t0 + HOUR, a);
    b.add("track:X", t0, t0 + HOUR, c);       // равный интервал — после первого
    b.add("track:X", t0 + HOUR, t0, a);       // пустой интервал не добавляется
    auto hits = b.conflicts("track:X", t0 + 30 * MINUTE, t0 + 2 * HOUR);
    CHECK_EQ(hits.size(), size_t(2));
    CHECK_EQ(*hits[0].meta.label, std::string("первый"));
    ConflictFilter f;
    f.ignore_train = std::string("T1");
    CHECK(b.conflicts("track:X", t0, t0 + HOUR, f).empty());
    CHECK(b.conflicts("track:X", t0 + HOUR, t0 + 2 * HOUR).empty());  // полуоткрытые интервалы
}

static void test_stays() {
    // прибытие -> осмотр -> манёвры -> сортировка: две стоянки, манёвры в обеих
    auto op = [](const char* id, const char* kind, const char* track, const char* from, int s, int e) {
        OpDict d;
        d.id = id;
        d.kind = kind;
        d.track_id = std::string(track);
        if (from) d.from_track_id = std::string(from);
        d.start = s * MINUTE;
        d.end = e * MINUTE;
        return d;
    };
    auto st = stays_of({op("a", "arrival", "T1", nullptr, 0, 10), op("i", "inspection", "T1", nullptr, 10, 70),
                        op("s", "shunting", "T7", "T1", 70, 85), op("r", "sorting", "T7", nullptr, 85, 130)});
    CHECK_EQ(st.size(), size_t(2));
    CHECK_EQ(*st[0].track_id, std::string("T1"));
    CHECK_EQ(st[0].end, 85 * MINUTE);  // путь занят до окончания вытягивания
    CHECK_EQ(st[0].ops.size(), size_t(3));
    CHECK_EQ(*st[1].track_id, std::string("T7"));
    CHECK_EQ(st[1].end, 130 * MINUTE);
}

int main() {
    test_time();
    test_format();
    test_sha1();
    test_interval_book();
    test_stays();
    if (failures) {
        std::cerr << "Модульные тесты: ошибок — " << failures << "\n";
        return 1;
    }
    std::cout << "Модульные тесты: все проверки пройдены\n";
    return 0;
}
