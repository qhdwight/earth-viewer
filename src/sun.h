#pragma once

#include "src/core.h"
#include "src/math.h"

// Windows FILETIME counts 100 ns ticks since 1601-01-01 UTC. The J2000.0
// epoch, 2000-01-01 12:00, is 145731 days and a half after that.
inline constexpr U64 kTicksPerSecond = 10000000;
inline constexpr U64 kSecondsPerDay = 86400;
inline constexpr U64 kTicksPerDay = kSecondsPerDay * kTicksPerSecond;
inline constexpr U64 kJ2000Ticks = 145731 * kTicksPerDay + kTicksPerDay / 2;
inline constexpr F32 kRadiansPerDegree = kPi / 180.0f;

// Angles below grow by about a degree a day, and Earth's rotation by about
// 361, so they are wrapped to one turn in double precision before becoming
// float radians.
inline F32 WrapDegrees(const F64 degrees) {
    const F64 turns = degrees / 360.0;
    I64 whole = static_cast<I64>(turns);
    if (static_cast<F64>(whole) > turns) {
        whole -= 1;
    }
    return static_cast<F32>(turns - static_cast<F64>(whole)) * kTau;
}

// Unit vector toward the Sun in world space: +Y through the north pole, +Z
// through the prime meridian, +X through 90 degrees east. Uses the low
// precision almanac formulas, good to about 0.01 degrees.
inline Vec3 SunDirection(const U64 fileTimeTicks) {
    const F64 days = static_cast<F64>(static_cast<I64>(fileTimeTicks - kJ2000Ticks)) / static_cast<F64>(kTicksPerDay);

    const F32 meanLongitude = WrapDegrees(280.460 + 0.9856474 * days);
    const F32 meanAnomaly = WrapDegrees(357.528 + 0.9856003 * days);
    const F32 eclipticLongitude = meanLongitude + 1.915f * kRadiansPerDegree * Sin(meanAnomaly) +
                                  0.020f * kRadiansPerDegree * Sin(2.0f * meanAnomaly);
    const F32 obliquity = static_cast<F32>(23.439 - 0.0000004 * days) * kRadiansPerDegree;
    // Greenwich mean sidereal time: how far Earth has turned against the stars.
    const F32 sidereal = WrapDegrees(280.46061837 + 360.98564736629 * days);

    // Equatorial frame: x toward the March equinox, z toward the north pole.
    const F32 x = Cos(eclipticLongitude);
    const F32 y = Cos(obliquity) * Sin(eclipticLongitude);
    const F32 z = Sin(obliquity) * Sin(eclipticLongitude);

    // Rotate by the sidereal angle into the Earth-fixed frame.
    const F32 cosSidereal = Cos(sidereal);
    const F32 sinSidereal = Sin(sidereal);
    const F32 towardPrimeMeridian = cosSidereal * x + sinSidereal * y;
    const F32 towardEast = cosSidereal * y - sinSidereal * x;
    return {towardEast, z, towardPrimeMeridian};
}
