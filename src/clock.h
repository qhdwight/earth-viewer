#pragma once

#include "src/core.h"

// Calendar and time-scale conversions for the clock overlay. Everything is
// integer arithmetic on UTC seconds since 1970, valid for any time after
// 1970, which a system clock always reports.

// Windows FILETIME counts 100 ns ticks since 1601-01-01 UTC.
inline constexpr U64 kTicksPerSecond = 10000000;
inline constexpr I64 kSecondsPerHour = 3600;
inline constexpr I64 kSecondsPerDay = 86400;
inline constexpr I64 kSecondsPerWeek = 7 * kSecondsPerDay;
inline constexpr U64 kTicksPerDay = static_cast<U64>(kSecondsPerDay) * kTicksPerSecond;
inline constexpr I64 kUnixEpochFileTimeSeconds = 11644473600; // 1601-01-01 to 1970-01-01

// GPS time started at 1980-01-06 00:00 UTC and does not count leap seconds,
// so it runs ahead of UTC by every leap second since. Increase this if the
// IERS schedules another; none has been added since the end of 2016.
inline constexpr I64 kGpsEpochUnixSeconds = 315964800;
inline constexpr I64 kGpsLeapSeconds = 18;

// US Pacific time: UTC-8, or UTC-7 from 2:00 local on the second Sunday of
// March until 2:00 local on the first Sunday of November.
inline constexpr I64 kPacificStandardOffset = -8 * kSecondsPerHour;
inline constexpr I64 kPacificDaylightOffset = -7 * kSecondsPerHour;

struct CivilTime {
    I32 year;
    U32 month; // 1 to 12
    U32 day;   // 1 to 31
    U32 hour;
    U32 minute;
    U32 second;
};

constexpr I64 UnixSecondsFromFileTime(const U64 ticks) {
    return static_cast<I64>(ticks / kTicksPerSecond) - kUnixEpochFileTimeSeconds;
}

// Howard Hinnant's algorithms: counting years from March puts the leap day at
// the end of the year, so every month length follows one formula.
constexpr I64 DaysFromCivil(const I32 year, const U32 month, const U32 day) {
    const I64 marchYear = year - (month <= 2 ? 1 : 0);
    const I64 era = marchYear / 400;
    const U32 yearOfEra = static_cast<U32>(marchYear - era * 400);
    const U32 dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const U32 dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<I64>(dayOfEra) - 719468;
}

constexpr CivilTime CivilFromUnixSeconds(const I64 seconds) {
    const I64 days = seconds / kSecondsPerDay + 719468;
    const I64 secondOfDay = seconds % kSecondsPerDay;
    const I64 era = days / 146097;
    const U32 dayOfEra = static_cast<U32>(days - era * 146097);
    const U32 yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const U32 dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const U32 monthIndex = (5 * dayOfYear + 2) / 153; // 0 is March
    const U32 month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    return {
        .year = static_cast<I32>(yearOfEra + era * 400 + (month <= 2 ? 1 : 0)),
        .month = month,
        .day = dayOfYear - (153 * monthIndex + 2) / 5 + 1,
        .hour = static_cast<U32>(secondOfDay / kSecondsPerHour),
        .minute = static_cast<U32>(secondOfDay / 60 % 60),
        .second = static_cast<U32>(secondOfDay % 60),
    };
}

// First Sunday on or after a day; day 0, 1970-01-01, was a Thursday.
constexpr I64 SundayOnOrAfter(const I64 days) {
    return days + (7 - (days + 4) % 7) % 7;
}

constexpr bool IsPacificDaylightTime(const I64 utcSeconds) {
    const I32 year = CivilFromUnixSeconds(utcSeconds).year;
    // 2:00 PST is 10:00 UTC, and 2:00 PDT is 9:00 UTC.
    const I64 start = SundayOnOrAfter(DaysFromCivil(year, 3, 8)) * kSecondsPerDay + 10 * kSecondsPerHour;
    const I64 end = SundayOnOrAfter(DaysFromCivil(year, 11, 1)) * kSecondsPerDay + 9 * kSecondsPerHour;
    return utcSeconds >= start && utcSeconds < end;
}

constexpr I64 GpsSecondsFromUnix(const I64 utcSeconds) {
    return utcSeconds - kGpsEpochUnixSeconds + kGpsLeapSeconds;
}

static_assert(DaysFromCivil(1970, 1, 1) == 0);
static_assert(DaysFromCivil(1980, 1, 6) * kSecondsPerDay == kGpsEpochUnixSeconds);
static_assert(CivilFromUnixSeconds(951782400).month == 2 && CivilFromUnixSeconds(951782400).day == 29); // 2000-02-29
