#pragma once

#include "src/core.hpp"
#include "src/math.hpp"

// Orbit propagation in the Earth-fixed frame, ECEF: x through the prime
// meridian, y through 90 degrees east, z through the north pole. Units are
// meters and seconds, in double precision throughout.
//
// Gravity is a point mass plus J2, the equatorial bulge, which is the largest
// perturbation for low orbits. J2 is symmetric about the spin axis, so it does
// not change with time in this frame. The rotating frame adds Coriolis and
// centrifugal accelerations instead.
//
// Not modeled: higher harmonics, drag, the Sun and Moon, polar motion and
// precession. Over a day these move a low orbit by kilometers.

inline constexpr F64 kEarthMu = 3.986004418e14; // m^3/s^2, WGS 84
inline constexpr F64 kEarthRadius = 6378137.0; // m, WGS 84 equatorial
inline constexpr F64 kEarthJ2 = 1.08262668e-3; // EGM2008
inline constexpr F64 kEarthRotationRate = 7.292115e-5; // rad/s, WGS 84

// Longest RK4 step. Error per step grows with the fifth power of the step.
// With 10 s, a 400 km orbit stays within half a meter of a 1 s reference over
// a day, and its Jacobi integral, the energy conserved in a rotating frame,
// drifts by under 1e-9.
inline constexpr F64 kJ2MaxStepSeconds = 10.0;

struct OrbitState {
    Vec3d position; // m, ECEF
    Vec3d velocity; // m/s, relative to the rotating Earth
};

// Acceleration as seen in the rotating frame, for position p and velocity v.
inline Vec3d J2Acceleration(const Vec3d p, const Vec3d v) {
    const F64 r2 = Dot(p, p);
    const F64 muOverR3 = kEarthMu / (r2 * Sqrt(r2));
    const F64 bulge = 1.5 * kEarthJ2 * kEarthRadius * kEarthRadius / r2;
    const F64 z2OverR2 = p.z * p.z / r2;
    const F64 equatorial = -muOverR3 * (1.0 + bulge * (1.0 - 5.0 * z2OverR2));
    const F64 polar = -muOverR3 * (1.0 + bulge * (3.0 - 5.0 * z2OverR2));
    // With spin w about z: centrifugal -w x (w x p) = w^2 (x, y, 0) and
    // Coriolis -2 w x v = 2w (v.y, -v.x, 0).
    constexpr F64 kW = kEarthRotationRate;
    return {
        equatorial * p.x + kW * kW * p.x + 2.0 * kW * v.y,
        equatorial * p.y + kW * kW * p.y - 2.0 * kW * v.x,
        polar * p.z,
    };
}

// One classic fourth-order Runge-Kutta step of h seconds, which may be negative.
inline OrbitState J2Step(const OrbitState state, const F64 h) {
    const Vec3d p1 = state.position;
    const Vec3d v1 = state.velocity;
    const Vec3d a1 = J2Acceleration(p1, v1);
    const Vec3d p2 = Add(p1, Scale(v1, 0.5 * h));
    const Vec3d v2 = Add(v1, Scale(a1, 0.5 * h));
    const Vec3d a2 = J2Acceleration(p2, v2);
    const Vec3d p3 = Add(p1, Scale(v2, 0.5 * h));
    const Vec3d v3 = Add(v1, Scale(a2, 0.5 * h));
    const Vec3d a3 = J2Acceleration(p3, v3);
    const Vec3d p4 = Add(p1, Scale(v3, h));
    const Vec3d v4 = Add(v1, Scale(a3, h));
    const Vec3d a4 = J2Acceleration(p4, v4);
    const F64 sixth = h / 6.0;
    return {
        .position = Add(p1, Scale(Add(Add(v1, v4), Scale(Add(v2, v3), 2.0)), sixth)),
        .velocity = Add(v1, Scale(Add(Add(a1, a4), Scale(Add(a2, a3), 2.0)), sixth)),
    };
}

// Advances a state by any number of seconds, forward or backward, in equal
// steps no longer than kJ2MaxStepSeconds.
inline OrbitState PropagateJ2(const OrbitState state, const F64 seconds) {
    const F64 span = seconds < 0.0 ? -seconds : seconds;
    const U64 steps = static_cast<U64>(span / kJ2MaxStepSeconds) + 1;
    const F64 h = seconds / static_cast<F64>(steps);
    OrbitState result = state;
    for (U64 i = 0; i < steps; ++i) {
        result = J2Step(result, h);
    }
    return result;
}

// Renderer world space: the unit sphere is the equatorial radius, with +X
// through 90 degrees east, +Y through the north pole and +Z through the prime
// meridian.
inline Point3 WorldFromEcef(const Vec3d p) {
    constexpr F64 kScale = 1.0 / kEarthRadius;
    return {{static_cast<F32>(p.y * kScale), static_cast<F32>(p.z * kScale), static_cast<F32>(p.x * kScale)}};
}
