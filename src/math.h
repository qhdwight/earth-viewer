#pragma once

#include <immintrin.h>

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

// Column-major, matching the GLSL default for a push constant mat4.
struct Mat4 {
    F32 m[16];
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

inline F32 Sqrt(const F32 x) {
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
}

inline F64 Sqrt(const F64 x) {
    return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x)));
}

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

// Vulkan clip space: y points down, depth spans [0, 1]. Far plane at infinity.
// fovScale is 1 / tan(verticalFov / 2).
constexpr Mat4 Perspective(const F32 fovScale, const F32 aspect, const F32 nearPlane) {
    return {
        {
            fovScale / aspect, 0.0f, 0.0f, 0.0f,
            0.0f, -fovScale, 0.0f, 0.0f,
            0.0f, 0.0f, -1.0f, -1.0f,
            0.0f, 0.0f, -nearPlane, 0.0f,
        }
    };
}

// View matrix from an orthonormal camera basis; the camera looks down -Z.
constexpr Mat4 View(const Vec3 position, const Vec3 right, const Vec3 up, const Vec3 forward) {
    return {
        {
            right.x, up.x, -forward.x, 0.0f,
            right.y, up.y, -forward.y, 0.0f,
            right.z, up.z, -forward.z, 0.0f,
            -Dot(right, position), -Dot(up, position), Dot(forward, position), 1.0f,
        }
    };
}

// m * (p, 1), with m column-major.
constexpr Vec4 TransformPoint(const Mat4 &m, const Vec3 p) {
    return {
        m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
        m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
        m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14],
        m.m[3] * p.x + m.m[7] * p.y + m.m[11] * p.z + m.m[15],
    };
}

constexpr Mat4 Multiply(const Mat4 &a, const Mat4 &b) {
    Mat4 out = {};
    for (U32 col = 0; col < 4; ++col) {
        for (U32 row = 0; row < 4; ++row) {
            F32 sum = 0.0f;
            for (U32 k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            out.m[col * 4 + row] = sum;
        }
    }
    return out;
}
