#pragma once

#include <cstdint>

using U8 = std::uint8_t;
using U16 = std::uint16_t;
using U32 = std::uint32_t;
using U64 = std::uint64_t;

using I8 = std::int8_t;
using I16 = std::int16_t;
using I32 = std::int32_t;
using I64 = std::int64_t;

using F32 = float;

inline constexpr F32 kPi = 3.14159265f;
inline constexpr F32 kTau = 2.0f * kPi;
inline constexpr F32 kE = 2.71828183f;
inline constexpr F32 kSqrt2 = 1.41421356f;
inline constexpr F32 kSqrt3 = 1.73205081f;

inline constexpr F32 kPosInf = __builtin_bit_cast(F32, 0x7F800000u);
inline constexpr F32 kNan = __builtin_bit_cast(F32, 0x7FC00000u);

constexpr U8 Min(const U8 a, const U8 b) { return a < b ? a : b; }
constexpr U16 Min(const U16 a, const U16 b) { return a < b ? a : b; }
constexpr U32 Min(const U32 a, const U32 b) { return a < b ? a : b; }
constexpr U64 Min(const U64 a, const U64 b) { return a < b ? a : b; }
constexpr I8 Min(const I8 a, const I8 b) { return a < b ? a : b; }
constexpr I16 Min(const I16 a, const I16 b) { return a < b ? a : b; }
constexpr I32 Min(const I32 a, const I32 b) { return a < b ? a : b; }
constexpr I64 Min(const I64 a, const I64 b) { return a < b ? a : b; }
constexpr F32 Min(const F32 a, const F32 b) { return a < b ? a : b; }

template<typename T, U64 N>
constexpr U64 Count(const T (&)[N]) {
    return N;
}

template<typename T, U64 N>
constexpr void Copy(T (&dst)[N], const T (&src)[N]) {
    for (U64 i = 0; i < N; ++i) {
        dst[i] = src[i];
    }
}
