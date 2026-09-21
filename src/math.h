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

// Taylor series after reducing to [-pi/2, pi/2]; error is below 1e-5.
inline F32 Sin(F32 x) {
    const F32 turns = (x + kPi) / kTau;
    I32 whole = static_cast<I32>(turns);
    if (static_cast<F32>(whole) > turns) {
        whole -= 1;
    }
    x -= static_cast<F32>(whole) * kTau;
    if (x > 0.5f * kPi) {
        x = kPi - x;
    } else if (x < -0.5f * kPi) {
        x = -kPi - x;
    }
    const F32 x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}

inline F32 Cos(const F32 x) { return Sin(x + 0.5f * kPi); }

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
