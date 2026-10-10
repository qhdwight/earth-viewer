#pragma once

#include "src/borders.h"
#include "src/core.h"
#include "src/j2.h"
#include "src/math.h"
#include "src/satellites.h"

// Ground stations at random land locations, and the links between them and
// the satellites they can see.

inline constexpr U32 kStationCount = 32;
inline constexpr U64 kStationSeed = 0x9e3779b97f4a7c15ull; // any fixed value gives a fixed layout
// A satellite is linked when it is at least 10 degrees above the station's
// horizon, a typical minimum elevation for ground antennas.
inline constexpr F64 kMinElevationSine = 0.17364817766693; // sin(10 degrees)
inline constexpr F64 kMinElevationSineSquared = kMinElevationSine * kMinElevationSine;
inline constexpr U32 kMaxLinks = kStationCount * kSatelliteCount;

struct GroundStation {
    Vec3d position; // m, ECEF, on the equatorial-radius sphere the renderer draws
    Vec3d up; // unit vector, the local vertical
};

struct Link {
    U16 station;
    U16 satellite;
};

// SplitMix64: a tiny generator whose output is identical on every platform,
// unlike the distributions in <random>.
inline U64 NextRandom(U64 &state) {
    state += 0x9e3779b97f4a7c15ull;
    U64 z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Uniform in [0, 1) from the top 24 bits, which fit a float exactly.
inline F32 RandomUnit(U64 &state) {
    return static_cast<F32>(NextRandom(state) >> 40) * (1.0f / 16777216.0f);
}

// Even-odd ray cast against every country outline, in the border data's
// units of 1/180 degree. Natural Earth splits polygons at the antimeridian,
// so a flat test in longitude and latitude is exact. Holes, such as Lesotho
// inside South Africa, are separate rings and also toggle the parity.
inline bool IsOnLand(const F32 longitude, const F32 latitude) {
    bool inside = false;
    for (U32 iRing = 0; iRing < kBorderCountryRingCount; ++iRing) {
        const BorderRing &ring = kBorderRings[iRing];
        for (U32 i = ring.first; i + 1 < ring.first + ring.count; ++i) {
            const F32 x0 = kBorderPoints[i + 0][0];
            const F32 y0 = kBorderPoints[i + 0][1];
            const F32 x1 = kBorderPoints[i + 1][0];
            const F32 y1 = kBorderPoints[i + 1][1];
            if ((y0 > latitude) != (y1 > latitude) &&
                longitude < x0 + (latitude - y0) * (x1 - x0) / (y1 - y0)) {
                inside = !inside;
            }
        }
    }
    return inside;
}

// Rejection sampling: uniform longitude and latitude, kept with probability
// cos(latitude) so stations are uniform over the surface, then kept only on land.
inline void InitGroundStations(GroundStation *const stations) {
    U64 random = kStationSeed;
    U32 count = 0;
    while (count < kStationCount) {
        const F32 longitude = RandomUnit(random) * 360.0f - 180.0f;
        const F32 latitude = RandomUnit(random) * 180.0f - 90.0f;
        F32 sinLatitude, cosLatitude;
        SinCos(latitude * kRadiansPerDegree, sinLatitude, cosLatitude);
        if (RandomUnit(random) >= cosLatitude ||
            !IsOnLand(longitude * kBorderUnitsPerDegree, latitude * kBorderUnitsPerDegree)) {
            continue;
        }
        F32 sinLongitude, cosLongitude;
        SinCos(longitude * kRadiansPerDegree, sinLongitude, cosLongitude);
        const Vec3d up = {cosLatitude * cosLongitude, cosLatitude * sinLongitude, sinLatitude};
        stations[count++] = {.position = Scale(up, kEarthRadius), .up = up};
    }
}

// Every station and satellite pair where the satellite is above the station's
// minimum elevation. Returns the number of links written.
inline U32 FindLinks(const GroundStation *const stations, const OrbitState *const satellites, Link *const links) {
    U32 count = 0;
    for (U32 station = 0; station < kStationCount; ++station) {
        for (U32 satellite = 0; satellite < kSatelliteCount; ++satellite) {
            const Vec3d lineOfSight = Sub(satellites[satellite].position, stations[station].position);
            const F64 height = Dot(lineOfSight, stations[station].up);
            if (Abs(height) * height >= kMinElevationSineSquared * Dot(lineOfSight, lineOfSight)) {
                links[count++] = {.station = static_cast<U16>(station), .satellite = static_cast<U16>(satellite)};
            }
        }
    }
    return count;
}
