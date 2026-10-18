#pragma once

#include "src/core.hpp"
#include "src/j2.hpp"
#include "src/math.hpp"

// Geodetic coordinates on the WGS 84 ellipsoid. kEarthRadius in j2.hpp is its
// semi-major axis.
inline constexpr F64 kWgs84Flattening = 1.0 / 298.257223563;
inline constexpr F64 kDegreesPerRadian = 180.0 / kPi64;

struct Geodetic {
    F64 latitude; // radians, positive north
    F64 longitude; // radians, positive east
    F64 altitude; // meters above the ellipsoid
};

// Bowring's method from ECEF meters, with two steps: each derives the geodetic
// latitude from an estimate of the parametric latitude beta, then refines
// beta from it. Sines and cosines come from ratios, so the only angle
// functions are the two final arctangents. Against a converged long double
// solution it is within about 1e-8 m from the ground to geostationary altitude.
inline Geodetic GeodeticFromEcef(const Vec3d p) {
    const F64 a = kEarthRadius;
    const F64 b = a * (1.0 - kWgs84Flattening);
    const F64 e2 = kWgs84Flattening * (2.0 - kWgs84Flattening); // first eccentricity squared
    const F64 ep2 = e2 / (1.0 - e2); // second eccentricity squared
    const F64 axial = Sqrt(p.x * p.x + p.y * p.y); // distance from the polar axis

    // First estimate of beta: tan beta = a z / (b axial).
    F64 betaY = p.z * a;
    F64 betaX = axial * b;
    F64 north = 0.0;
    F64 outward = 0.0;
    for (U32 step = 0; step < 2; ++step) {
        const F64 length = Sqrt(betaY * betaY + betaX * betaX);
        const F64 sinBeta = length > 0.0 ? betaY / length : 0.0;
        const F64 cosBeta = length > 0.0 ? betaX / length : 1.0;
        north = p.z + ep2 * b * sinBeta * sinBeta * sinBeta;
        outward = axial - e2 * a * cosBeta * cosBeta * cosBeta;
        // Refined beta: tan beta = (b / a) tan latitude.
        betaY = b * north;
        betaX = a * outward;
    }
    const F64 hypotenuse = Sqrt(north * north + outward * outward);
    const F64 sinLatitude = north / hypotenuse;
    const F64 cosLatitude = outward / hypotenuse;

    return {
        .latitude = ArcTan2(north, outward),
        .longitude = ArcTan2(p.y, p.x),
        // Height along the normal; stable at every latitude, including the poles.
        .altitude = axial * cosLatitude + p.z * sinLatitude - a * Sqrt(1.0 - e2 * sinLatitude * sinLatitude),
    };
}
