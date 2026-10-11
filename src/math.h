#pragma once

#if !defined(__aarch64__)
#include <immintrin.h>
#endif

#include "src/core.h"

struct Vec3 {
    F32 x, y, z;
};

struct Vec4 {
    F32 x, y, z, w;
};

// Double precision, for orbits: float resolves a low orbit only to about half
// a meter.
struct Vec3d {
    F64 x, y, z;
};

constexpr Vec3 Add(const Vec3 a, const Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 Sub(const Vec3 a, const Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 Scale(const Vec3 a, const F32 s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr F32 Dot(const Vec3 a, const Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

constexpr Vec3 Cross(const Vec3 a, const Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

constexpr Vec3d Add(const Vec3d a, const Vec3d b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3d Sub(const Vec3d a, const Vec3d b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3d Scale(const Vec3d a, const F64 s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr F64 Dot(const Vec3d a, const Vec3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

#if defined(__aarch64__)
inline F32 Sqrt(const F32 x) { return __builtin_sqrtf(x); }
inline F64 Sqrt(const F64 x) { return __builtin_sqrt(x); }
#else
inline F32 Sqrt(const F32 x) {
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
}

inline F64 Sqrt(const F64 x) {
    return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x)));
}
#endif

inline F64 Length(const Vec3d a) { return Sqrt(Dot(a, a)); }

inline constexpr F64 kPi64 = 3.14159265358979323846;

// Arctangent of t in [0, 1], to double precision. Two half-angle steps,
// atan t = 2 atan(t / (1 + sqrt(1 + t^2))), bring t below tan(pi/16), about
// 0.199, where twelve terms of the Taylor series leave an error near 1e-19.
inline F64 ArcTanUnit(const F64 t) {
    const F64 half = t / (1.0 + Sqrt(1.0 + t * t));
    const F64 quarter = half / (1.0 + Sqrt(1.0 + half * half));
    const F64 square = quarter * quarter;
    F64 sum = 0.0;
    F64 power = quarter;
    for (U32 k = 0; k < 12; ++k) {
        const F64 term = power / static_cast<F64>(2 * k + 1);
        sum += (k & 1) != 0 ? -term : term;
        power *= square;
    }
    return 4.0 * sum;
}

// Angle of (x, y) from +x in radians, in (-pi, pi], to double precision.
inline F64 ArcTan2(const F64 y, const F64 x) {
    const F64 absX = Abs(x);
    const F64 absY = Abs(y);
    if (absX == 0.0 && absY == 0.0) {
        return 0.0;
    }
    // Fold into the first octant, where the ratio is at most 1.
    const bool steep = absY > absX;
    F64 angle = ArcTanUnit(steep ? absX / absY : absY / absX);
    if (steep) {
        angle = 0.5 * kPi64 - angle;
    }
    if (x < 0.0) {
        angle = kPi64 - angle;
    }
    return y < 0.0 ? -angle : angle;
}

inline F32 Length(const Vec3 a) { return Sqrt(Dot(a, a)); }

inline Vec3 Normalize(const Vec3 a) { return Scale(a, 1.0f / Length(a)); }

// Both values come from one range reduction: x = r + q * pi/2 with r in
// [-pi/4, pi/4], where short Taylor series for sin r and cos r are accurate to
// about 3e-7. The quadrant q then swaps and negates them.
inline void SinCos(const F32 x, F32 &sine, F32 &cosine) {
    const F32 quarterTurns = x * (2.0f / kPi);
    I32 quadrant = static_cast<I32>(quarterTurns + 0.5f);
    if (static_cast<F32>(quadrant) > quarterTurns + 0.5f) {
        quadrant -= 1; // truncation rounded a negative value up
    }
    const F32 r = x - static_cast<F32>(quadrant) * (0.5f * kPi);
    const F32 r2 = r * r;
    const F32 s = r * (1.0f + r2 * (-1.0f / 6.0f + r2 * (1.0f / 120.0f + r2 * (-1.0f / 5040.0f))));
    const F32 c = 1.0f + r2 * (-1.0f / 2.0f + r2 * (1.0f / 24.0f + r2 * (-1.0f / 720.0f + r2 * (1.0f / 40320.0f))));
    // sin(r + pi/2) = cos r and cos(r + pi/2) = -sin r; a half turn negates both.
    const bool odd = (quadrant & 1) != 0;
    const F32 sign = (quadrant & 2) != 0 ? -1.0f : 1.0f;
    sine = sign * (odd ? c : s);
    cosine = sign * (odd ? -s : c);
}

// ---------------------------------------------------------------------------
// Rigid geometric algebra, following Eric Lengyel's conventions at
// rigidgeometricalgebra.org. The algebra is G(3,0,1): basis vectors e1, e2, e3
// and the projective e4, which squares to zero. An element's bulk is its part
// without e4 and its weight the part with e4; it is unitized when its weight
// has unit length. Only the products this program needs are written out, each
// specialized to its operand types. pga.glsl mirrors these for the shaders.

// x e1 + y e2 + z e3 + e4: a unitized point. A Vec3 is a direction, with no e4.
struct Point3 {
    Vec3 xyz;
};

struct Point3d {
    Vec3d xyz;
};

// v.x e41 + v.y e42 + v.z e43 + m.x e23 + m.y e31 + m.z e12. The direction v is
// the weight and the moment m the bulk; m is v crossed into any point on it.
struct Line3 {
    Vec3 v;
    Vec3 m;
};

// n.x e423 + n.y e431 + n.z e412 + w e321: the points p where n . p + w = 0.
// The normal n is the weight and w the bulk.
struct Plane3d {
    Vec3d n;
    F64 w;
};

// A rigid motion: v.x e41 + v.y e42 + v.z e43 + vw 1234 + m.x e23 + m.y e31 +
// m.z e12 + mw. The weight (v, vw) holds the rotation and the bulk (m, mw) the
// translation. Laid out as two vec4 for the shaders.
struct Motor3 {
    Vec3 v;
    F32 vw;
    Vec3 m;
    F32 mw;
};

// The difference of two points is a direction, and a point moved along a
// direction is a point. The sum of two points is not a point.
constexpr Vec3 Sub(const Point3 a, const Point3 b) { return Sub(a.xyz, b.xyz); }
constexpr Point3 Add(const Point3 p, const Vec3 d) { return {Add(p.xyz, d)}; }

// The join p ^ q of two unitized points: the line through p toward q.
constexpr Line3 Wedge(const Point3 p, const Point3 q) { return {Sub(q, p), Cross(p.xyz, q.xyz)}; }

// The meet p v g of a unitized point and a plane: the point's signed distance
// above the plane, in units of the normal's length.
constexpr F64 Antiwedge(const Point3d p, const Plane3d g) { return Dot(g.n, p.xyz) + g.w; }

// The point on a plane closest to the origin.
constexpr Point3d Support(const Plane3d g) { return {Scale(g.n, -g.w / Dot(g.n, g.n))}; }

// Moves points by t. The bulk holds half the displacement.
constexpr Motor3 Translation(const Vec3 t) { return {{}, 1.0f, Scale(t, 0.5f), 0.0f}; }

// Turns points by angle radians about a unitized line, by the right-hand rule
// about its direction. The weight and bulk hold half the angle.
inline Motor3 Rotation(const Line3 axis, const F32 angle) {
    F32 sine, cosine;
    SinCos(0.5f * angle, sine, cosine);
    return {Scale(axis.v, sine), cosine, Scale(axis.m, sine), 0.0f};
}

// The geometric antiproduct a v b: motion b followed by motion a.
constexpr Motor3 Compose(const Motor3 a, const Motor3 b) {
    return {
        .v = Add(Add(Scale(b.v, a.vw), Scale(a.v, b.vw)), Cross(a.v, b.v)),
        .vw = a.vw * b.vw - Dot(a.v, b.v),
        .m = Add(Add(Add(Scale(b.m, a.vw), Scale(a.m, b.vw)), Add(Scale(b.v, a.mw), Scale(a.v, b.mw))),
                 Add(Cross(a.v, b.m), Cross(a.m, b.v))),
        .mw = a.vw * b.mw + a.mw * b.vw - Dot(a.v, b.m) - Dot(a.m, b.v),
    };
}

// The antireverse, which negates the bivector parts. For a unitized motor it
// is the inverse motion.
constexpr Motor3 Antireverse(const Motor3 q) { return {Scale(q.v, -1.0f), q.vw, Scale(q.m, -1.0f), q.mw}; }

// The sandwich q v p v ~q, expanded for a unitized point and a direction.
// Directions lack e4, so translation leaves them alone.
constexpr Point3 Transform(const Motor3 q, const Point3 p) {
    const Vec3 a = Add(Cross(q.v, p.xyz), q.m);
    return {Add(p.xyz, Scale(Sub(Add(Scale(a, q.vw), Cross(q.v, a)), Scale(q.v, q.mw)), 2.0f))};
}

constexpr Vec3 Transform(const Motor3 q, const Vec3 d) {
    const Vec3 a = Cross(q.v, d);
    return Add(d, Scale(Add(Scale(a, q.vw), Cross(q.v, a)), 2.0f));
}

// ---------------------------------------------------------------------------
// Perspective projection into Vulkan clip space, from camera space looking
// down -Z: y points down, depth spans [0, 1] and the far plane is at infinity.
// Projection is not a rigid motion, so it stays outside the algebra. Matches
// Project in pga.glsl.
struct Perspective {
    F32 scaleX; // fovScale / aspect, where fovScale is 1 / tan(verticalFov / 2)
    F32 scaleY; // -fovScale
    F32 nearPlane;
    F32 unused;
};

constexpr Vec4 Project(const Perspective &projection, const Point3 p) {
    return {
        projection.scaleX * p.xyz.x,
        projection.scaleY * p.xyz.y,
        -p.xyz.z - projection.nearPlane,
        -p.xyz.z,
    };
}
