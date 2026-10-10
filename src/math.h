#pragma once

#include <immintrin.h>

#include "src/core.h"

struct Vec3 {
    F32 x, y, z;
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

inline F32 Sqrt(const F32 x) {
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
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
