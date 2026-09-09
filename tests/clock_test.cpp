// Headless checks for the calendar: civil date arithmetic, and the Clock
// that carries it on the fixed step.

#include <rewind/sim/clock.h>
#include <rewind/sim/serialize.h>

#include <cmath>
#include <cstdio>

using namespace rw;

namespace {

int failures = 0;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

void test_civil() {
    // The epoch, and the weekday it fell on.
    CHECK(days_from_civil(1970, 1, 1) == 0);
    CHECK((civil_from_days(0) == Date{1970, 1, 1}));
    CHECK(weekday_from_days(0) == 3); // a Thursday, with Monday as 0

    // Round trip both ways over three centuries, including before the epoch.
    for (i64 z = -40000; z <= 40000; z += 7) {
        const Date d = civil_from_days(z);
        CHECK(d.month >= 1 && d.month <= 12);
        CHECK(d.day >= 1 && d.day <= 31);
        CHECK(days_from_civil(d.year, d.month, d.day) == z);
    }

    // The centurial leap rule: 1900 is not a leap year, 2000 is.
    CHECK(days_from_civil(1900, 3, 1) - days_from_civil(1900, 2, 28) == 1);
    CHECK(days_from_civil(2000, 3, 1) - days_from_civil(2000, 2, 29) == 1);
    CHECK((civil_from_days(days_from_civil(2024, 2, 29)) == Date{2024, 2, 29}));

    // A year is 365 days, or 366 with a 29th of February in it.
    CHECK(days_from_civil(1979, 1, 1) - days_from_civil(1978, 1, 1) == 365);
    CHECK(days_from_civil(2025, 1, 1) - days_from_civil(2024, 1, 1) == 366);

    // Known weekdays either side of a month end.
    CHECK(weekday_from_days(days_from_civil(2026, 9, 7)) == 0); // Monday
    CHECK(weekday_from_days(days_from_civil(2026, 9, 13)) == 6); // Sunday
    CHECK(weekday_from_days(days_from_civil(1978, 4, 3)) == 0);  // Monday

    // It is constant-evaluable, so a deadline can be a constant.
    static_assert(days_from_civil(1970, 1, 1) == 0);
    static_assert(civil_from_days(0).year == 1970);
}

void test_reading() {
    Clock c;
    c.set_start(1978, 4, 3, 6.5f);
    CHECK((c.date() == Date{1978, 4, 3}));
    CHECK(c.hour() == 6 && c.minute() == 30);
    CHECK(near(c.hour_of_day(), 6.5f));
    CHECK(c.weekday() == 0 && !c.weekend());
    CHECK(near(static_cast<float>(c.rate()), 180.0f)); // 20 s an hour

    // Twenty simulated seconds is one game hour at the default rate.
    for (int i = 0; i < 20 * 60; ++i) c.tick(1.0f / 60.0f);
    CHECK(c.hour() == 7 && c.minute() == 30);
    CHECK((c.date() == Date{1978, 4, 3}));

    // Saturday and Sunday are the weekend; Monday to Friday are not.
    Clock w;
    for (u32 i = 0; i < 7; ++i) {
        w.set_start(1978, 4, 3 + i);
        CHECK(w.weekday() == i);
        CHECK(w.weekend() == (i >= 5));
    }
}

void test_edges() {
    // Midnight: exactly one day edge, and an hour edge with it.
    Clock d;
    d.set_start(1978, 4, 3, 23.0f);
    d.hour_seconds = 1; // an hour a second, to keep the test short
    int days = 0, hours = 0;
    for (int i = 0; i < 60 * 2; ++i) {
        const ClockEdge e = d.tick(1.0f / 60.0f);
        if (e.day) {
            ++days;
            CHECK(e.hour); // a day edge always carries an hour edge
        }
        if (e.hour) ++hours;
        CHECK(!e.month && !e.year);
    }
    CHECK(days == 1 && hours == 2);
    CHECK((d.date() == Date{1978, 4, 4} && d.hour() == 1 && d.weekday() == 1));

    // New year's eve: a year edge implies a month edge implies a day edge.
    Clock y;
    y.set_start(1978, 12, 31, 23.0f);
    y.hour_seconds = 1;
    bool saw_month = false, saw_year = false;
    for (int i = 0; i < 60 * 2; ++i) {
        const ClockEdge e = y.tick(1.0f / 60.0f);
        if (e.month) {
            saw_month = true;
            CHECK(e.day);
        }
        if (e.year) {
            saw_year = true;
            CHECK(e.month);
        }
    }
    CHECK(saw_month && saw_year);
    CHECK((y.date() == Date{1979, 1, 1}));

    // A month edge without a year edge, on a 30-day month.
    Clock m;
    m.set_start(1978, 4, 30, 23.0f);
    m.hour_seconds = 1;
    int months = 0;
    for (int i = 0; i < 60 * 2; ++i) {
        const ClockEdge e = m.tick(1.0f / 60.0f);
        if (e.month) ++months;
        CHECK(!e.year);
    }
    CHECK((months == 1 && m.date() == Date{1978, 5, 1}));

    // A stopped clock does not advance and does not divide by zero.
    Clock stopped;
    stopped.set_start(1978, 4, 3, 8.0f);
    stopped.hour_seconds = 0;
    const ClockEdge e = stopped.tick(1.0f / 60.0f);
    CHECK(!e.hour && !e.day && !e.month && !e.year);
    CHECK(near(stopped.hour_of_day(), 8.0f) && near(static_cast<float>(stopped.rate()), 0.0f));
}

void test_deadlines() {
    Clock c;
    c.set_start(1978, 4, 3);
    const i64 due = Clock::day_of(1978, 4, 17);
    CHECK(c.days_until(due) == 14);

    // Run a fortnight through and the deadline is today, then past.
    c.hour_seconds = 1;
    for (int i = 0; i < 14 * 24 * 60; ++i) c.tick(1.0f / 60.0f);
    CHECK((c.date() == Date{1978, 4, 17}));
    CHECK(c.days_until(due) == 0);
    for (int i = 0; i < 24 * 60; ++i) c.tick(1.0f / 60.0f);
    CHECK(c.days_until(due) == -1);
    CHECK(near(static_cast<float>(c.days()), 15.0f, 0.01f));
}

void test_round_trip() {
    Clock saved;
    saved.set_start(1978, 4, 3, 6.5f);
    saved.hour_seconds = 12;
    for (int i = 0; i < 500; ++i) saved.tick(1.0f / 60.0f);

    Writer w;
    {
        SaveVisitor save(w);
        saved.reflect(save);
    }
    Clock loaded;
    {
        Reader r(w.bytes());
        LoadVisitor load(r);
        loaded.reflect(load);
    }
    CHECK(loaded.day_number() == saved.day_number());
    CHECK(loaded.date() == saved.date());
    CHECK(loaded.hour() == saved.hour() && loaded.minute() == saved.minute());
    CHECK(near(loaded.hour_seconds, 12.0f));

    // And it carries on identically from there, which is what a rewind needs.
    for (int i = 0; i < 500; ++i) {
        saved.tick(1.0f / 60.0f);
        loaded.tick(1.0f / 60.0f);
    }
    CHECK(loaded.time == saved.time);
}

} // namespace

int main() {
    test_civil();
    test_reading();
    test_edges();
    test_deadlines();
    test_round_trip();
    if (failures == 0) std::printf("clock_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
