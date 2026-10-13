#pragma once

#include "src/core.h"
#include "src/j2.h"
#include "src/math.h"

// A synthetic constellation of circular 300 km orbits, laid out as Walker
// delta shells. Each shell has evenly spaced planes, each plane carries
// evenly spaced satellites, and every plane is phase shifted from its
// neighbour so satellites do not line up at the plane crossings.

inline constexpr F64 kSatelliteAltitude = 300e3; // m

struct WalkerShell {
    F32 inclination; // radians
    U32 planes;
    U32 perPlane;
    U32 phasing; // Walker F: plane-to-plane shift in units of 360 / total degrees
};

inline constexpr WalkerShell kShells[] = {
    {.inclination = 43.0f * kRadiansPerDegree, .planes = 16, .perPlane = 32, .phasing = 1},
    {.inclination = 53.0f * kRadiansPerDegree, .planes = 16, .perPlane = 32, .phasing = 1},
    {.inclination = 70.0f * kRadiansPerDegree, .planes = 4, .perPlane = 8, .phasing = 1}, // near polar
    {.inclination = 97.0f * kRadiansPerDegree, .planes = 4, .perPlane = 8, .phasing = 1}, // near polar
};

constexpr U32 CountSatellites() {
    U32 count = 0;
    for (const WalkerShell &shell: kShells) {
        count += shell.planes * shell.perPlane;
    }
    return count;
}

inline constexpr U32 kSatelliteCount = CountSatellites();

// The constellation is synthetic, so its orientation is arbitrary: the
// inertial frame is taken to coincide with ECEF at the start. Positions then
// agree, and only the velocity changes, by the Earth's rotation w x p.
inline void InitConstellation(OrbitState (&states)[kSatelliteCount]) {
    const F64 radius = kEarthRadius + kSatelliteAltitude;
    const F64 speed = Sqrt(kEarthMu / radius);
    const F64 w = kEarthRotationRate;
    U32 index = 0;
    for (const WalkerShell &shell: kShells) {
        const U32 total = shell.planes * shell.perPlane;
        F32 sinInclination, cosInclination;
        SinCos(shell.inclination, sinInclination, cosInclination);
        for (U32 plane = 0; plane < shell.planes; ++plane) {
            F32 sinNode, cosNode;
            SinCos(kTau * static_cast<F32>(plane) / static_cast<F32>(shell.planes), sinNode, cosNode);
            for (U32 slot = 0; slot < shell.perPlane; ++slot) {
                // Argument of latitude: the angle from the ascending node.
                const F32 turns = static_cast<F32>(slot) / static_cast<F32>(shell.perPlane) +
                                  static_cast<F32>(shell.phasing * plane) / static_cast<F32>(total);
                F32 sinU, cosU;
                SinCos(kTau * turns, sinU, cosU);
                const Vec3d radial = {
                    cosNode * cosU - sinNode * sinU * cosInclination,
                    sinNode * cosU + cosNode * sinU * cosInclination,
                    sinU * sinInclination,
                };
                const Vec3d alongTrack = {
                    -cosNode * sinU - sinNode * cosU * cosInclination,
                    -sinNode * sinU + cosNode * cosU * cosInclination,
                    cosU * sinInclination,
                };
                const Vec3d position = Scale(radial, radius);
                const Vec3d inertialVelocity = Scale(alongTrack, speed);
                states[index++] = {
                    .position = position,
                    .velocity = {
                        inertialVelocity.x + w * position.y, inertialVelocity.y - w * position.x, inertialVelocity.z
                    },
                };
            }
        }
    }
}

inline void PropagateConstellation(OrbitState (&states)[kSatelliteCount], const F64 seconds) {
    for (OrbitState &state: states) {
        state = PropagateJ2(state, seconds);
    }
}
