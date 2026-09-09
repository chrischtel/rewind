#pragma once

// A calendar on the fixed step. Games that run on dates -- a deadline, a
// wage paid monthly, a shop that shuts at six, a fuel price that moves on a
// Monday -- need to know what day it is without ever asking the operating
// system, because tick() may not. Clock derives all of it from simulated
// seconds it accumulates itself, so it rewinds and replays with the rest of
// the state.
//
//     Clock clock;
//     clock.set_start(1978, 4, 3, 6.5f);   // in init(): Monday, half six
//     const ClockEdge edge = clock.tick(ctx.dt);
//     if (edge.day) pay_daily_costs();
//     if (edge.month) close_the_books();

#include "rewind/core/types.h"
#include "rewind/sim/reflect.h"

#include <cmath>

namespace rw {

/// A civil date. Proleptic Gregorian, the calendar every European country
/// has used since 1582 and the one every date library agrees on.
struct Date {
    i32 year = 1970;
    u32 month = 1; ///< 1..12
    u32 day = 1;   ///< 1..31

    bool operator==(const Date&) const = default;

    void reflect(Visitor& v) {
        v.field("year", year);
        v.field("month", month, {.min = 1, .max = 12});
        v.field("day", day, {.min = 1, .max = 31});
    }
};

// ---- civil date arithmetic ----------------------------------------------------------
// Howard Hinnant's algorithms: exact for every year, no tables, no loops.
// "Days" throughout means days since 1970-01-01, which may be negative.

/// Day number of a civil date. `month` is 1..12, `day` is 1..31.
constexpr i64 days_from_civil(i32 y, u32 m, u32 d) {
    y -= m <= 2;
    const i64 era = (y >= 0 ? y : y - 399) / 400;
    const u32 yoe = static_cast<u32>(y - era * 400);           // [0, 399]
    const u32 mp = m > 2 ? m - 3 : m + 9;                      // March = 0
    const u32 doy = (153 * mp + 2) / 5 + d - 1;                // [0, 365]
    const u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;     // [0, 146096]
    return era * 146097 + static_cast<i64>(doe) - 719468;
}

/// The civil date of a day number.
constexpr Date civil_from_days(i64 z) {
    z += 719468;
    const i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const u32 doe = static_cast<u32>(z - era * 146097);                  // [0, 146096]
    const u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    const i64 y = static_cast<i64>(yoe) + era * 400;
    const u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);             // [0, 365]
    const u32 mp = (5 * doy + 2) / 153;                                  // [0, 11]
    const u32 d = doy - (153 * mp + 2) / 5 + 1;                          // [1, 31]
    const u32 m = mp < 10 ? mp + 3 : mp - 9;                             // [1, 12]
    return Date{static_cast<i32>(y + (m <= 2)), m, d};
}

/// Day of the week of a day number. 0 = Monday, 6 = Sunday, because this
/// engine is written where the week starts on Monday.
constexpr u32 weekday_from_days(i64 z) {
    const u32 sunday_based = static_cast<u32>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
    return (sunday_based + 6) % 7;
}

/// What rolled over on the tick that just ran. A day boundary implies an
/// hour boundary, and a year implies a month.
struct ClockEdge {
    bool hour = false;
    bool day = false;
    bool month = false;
    bool year = false;
};

/// Calendar time carried on the fixed step.
///
/// `hour_seconds` is how long one game hour takes in *simulated* seconds, so
/// the engine's 1/4x to 4x speed control multiplies it without the clock
/// knowing: at the default 20, a day is eight minutes at normal speed.
struct Clock {
    i64 epoch_day = 0;       ///< Day number the game started on.
    double time = 0;         ///< Game seconds since 00:00 on epoch_day. Never negative.
    float hour_seconds = 20; ///< Simulated seconds per game hour.

    /// Set the date and time the game begins. Call it in init(), not tick().
    void set_start(i32 year, u32 month, u32 day, float start_hour = 0) {
        epoch_day = days_from_civil(year, month, day);
        time = static_cast<double>(start_hour) * 3600.0;
    }

    /// Game seconds per simulated second.
    double rate() const { return hour_seconds > 0 ? 3600.0 / static_cast<double>(hour_seconds) : 0.0; }

    /// Advance, and say what rolled over. Call once per tick().
    ClockEdge tick(float dt) {
        const i64 day0 = day_number();
        const u32 hour0 = hour();

        time += static_cast<double>(dt) * rate();
        if (time < 0) time = 0;

        ClockEdge edge;
        const i64 day1 = day_number();
        if (day1 != day0) {
            edge.day = true;
            const Date before = civil_from_days(day0);
            const Date after = civil_from_days(day1);
            edge.year = after.year != before.year;
            edge.month = edge.year || after.month != before.month;
        }
        edge.hour = edge.day || hour() != hour0;
        return edge;
    }

    // ---- reading it -----------------------------------------------------------------

    /// Whole days since the game started, plus the fraction of today.
    double days() const { return time / 86400.0; }
    i64 day_number() const { return epoch_day + static_cast<i64>(std::floor(time / 86400.0)); }
    Date date() const { return civil_from_days(day_number()); }
    u32 weekday() const { return weekday_from_days(day_number()); }
    bool weekend() const { return weekday() >= 5; }

    /// Seconds since midnight, 0 .. 86400.
    double day_seconds() const {
        const double s = std::fmod(time, 86400.0);
        return s < 0 ? s + 86400.0 : s;
    }
    /// The time of day as hours, 0 .. 24, for anything that wants it smooth:
    /// a sun angle, a clock hand, the shape of the traffic on the forecourt.
    float hour_of_day() const { return static_cast<float>(day_seconds() / 3600.0); }
    u32 hour() const { return static_cast<u32>(day_seconds() / 3600.0); }
    u32 minute() const { return static_cast<u32>(std::fmod(day_seconds() / 60.0, 60.0)); }

    /// Day number of a date, for a deadline: `due = clock.day_of(1978, 4, 17)`.
    static i64 day_of(i32 year, u32 month, u32 day) { return days_from_civil(year, month, day); }
    /// Days from today until `day`, negative once it is past.
    i64 days_until(i64 day) const { return day - day_number(); }

    void reflect(Visitor& v) {
        v.field("epoch_day", epoch_day);
        v.field("time", time, {.min = 0});
        v.field("hour_seconds", hour_seconds, {.min = 1, .max = 3600, .tooltip = "Simulated seconds per game hour"});
    }
};

} // namespace rw
