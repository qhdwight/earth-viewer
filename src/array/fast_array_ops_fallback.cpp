// Portable C++ versions of every routine in fast_array_ops.hpp, for targets
// where the AVX2 assembly cannot run, such as Apple silicon. BUILD.bazel
// compiles exactly one of the assembly or this file, so the symbols are the
// same. The behavior matches the assembly, including empty arrays:
//
// - Contains and find compare with ==, so for floats NaN never matches and
//   -0.0 matches 0.0. Find returns the first match, or ~0ULL for none.
// - Min and max of an empty integer array return the identity for the
//   operation: the largest value for min and the smallest for max. For floats
//   they return a NaN with every bit set, the seed the assembly starts from.
// - Min and max index return 0 for an empty array, as the assembly does; its
//   README says ~0U, but the code returns the length. Like the assembly, which
//   index wins among equal extremes is not guaranteed; these return the first.
//
// NaN inputs to min and max are not handled the way the AVX2 min and max
// instructions handle them; neither version promises anything for NaN.

#include "fast_array_ops.hpp"

#include <cstdint>

namespace {

template <typename T>
uint64_t Find(const void *const p, const uint64_t n, const T q) {
    const T *const values = static_cast<const T *>(p);
    for (uint64_t i = 0; i < n; ++i) {
        if (values[i] == q) {
            return i;
        }
    }
    return ~0ull;
}

// Index 0 for an empty array, like the assembly.
template <typename T>
uint32_t MinIndex(const T *const p, const uint32_t n) {
    uint32_t best = 0;
    for (uint32_t i = 1; i < n; ++i) {
        if (p[i] < p[best]) {
            best = i;
        }
    }
    return best;
}

template <typename T>
uint32_t MaxIndex(const T *const p, const uint32_t n) {
    uint32_t best = 0;
    for (uint32_t i = 1; i < n; ++i) {
        if (p[i] > p[best]) {
            best = i;
        }
    }
    return best;
}

template <typename T>
T Min(const T *const p, const uint32_t n, const T empty) {
    if (n == 0) {
        return empty;
    }
    T best = p[0];
    for (uint32_t i = 1; i < n; ++i) {
        best = p[i] < best ? p[i] : best;
    }
    return best;
}

template <typename T>
T Max(const T *const p, const uint32_t n, const T empty) {
    if (n == 0) {
        return empty;
    }
    T best = p[0];
    for (uint32_t i = 1; i < n; ++i) {
        best = p[i] > best ? p[i] : best;
    }
    return best;
}

// Every bit set: what the assembly's float min and max return when empty.
constexpr float kFloatEmpty = __builtin_bit_cast(float, 0xFFFFFFFFu);
constexpr double kDoubleEmpty = __builtin_bit_cast(double, 0xFFFFFFFFFFFFFFFFull);

} // namespace

extern "C" bool FastContains8(const void *p, uint64_t n, uint8_t q) { return Find(p, n, q) != ~0ull; }
extern "C" bool FastContains16(const void *p, uint64_t n, uint16_t q) { return Find(p, n, q) != ~0ull; }
extern "C" bool FastContains32(const void *p, uint64_t n, uint32_t q) { return Find(p, n, q) != ~0ull; }
extern "C" bool FastContains64(const void *p, uint64_t n, uint64_t q) { return Find(p, n, q) != ~0ull; }
extern "C" bool FastContainsFloat(const float *p, uint64_t n, float q) { return Find(p, n, q) != ~0ull; }
extern "C" bool FastContainsDouble(const double *p, uint64_t n, double q) { return Find(p, n, q) != ~0ull; }

extern "C" uint64_t FastFind8(const void *p, uint64_t n, uint8_t q) { return Find(p, n, q); }
extern "C" uint64_t FastFind16(const void *p, uint64_t n, uint16_t q) { return Find(p, n, q); }
extern "C" uint64_t FastFind32(const void *p, uint64_t n, uint32_t q) { return Find(p, n, q); }
extern "C" uint64_t FastFind64(const void *p, uint64_t n, uint64_t q) { return Find(p, n, q); }
extern "C" uint64_t FastFindFloat(const float *p, uint64_t n, float q) { return Find(p, n, q); }
extern "C" uint64_t FastFindDouble(const double *p, uint64_t n, double q) { return Find(p, n, q); }

extern "C" float FastMinFloat(const float *p, uint32_t n) { return Min(p, n, kFloatEmpty); }
extern "C" double FastMinDouble(const double *p, uint32_t n) { return Min(p, n, kDoubleEmpty); }
extern "C" int8_t FastMinI8(const int8_t *p, uint32_t n) { return Min<int8_t>(p, n, INT8_MAX); }
extern "C" int16_t FastMinI16(const int16_t *p, uint32_t n) { return Min<int16_t>(p, n, INT16_MAX); }
extern "C" int32_t FastMinI32(const int32_t *p, uint32_t n) { return Min<int32_t>(p, n, INT32_MAX); }
extern "C" int64_t FastMinI64(const int64_t *p, uint32_t n) { return Min<int64_t>(p, n, INT64_MAX); }
extern "C" uint8_t FastMinU8(const uint8_t *p, uint32_t n) { return Min<uint8_t>(p, n, UINT8_MAX); }
extern "C" uint16_t FastMinU16(const uint16_t *p, uint32_t n) { return Min<uint16_t>(p, n, UINT16_MAX); }
extern "C" uint32_t FastMinU32(const uint32_t *p, uint32_t n) { return Min<uint32_t>(p, n, UINT32_MAX); }
extern "C" uint64_t FastMinU64(const uint64_t *p, uint32_t n) { return Min<uint64_t>(p, n, UINT64_MAX); }

extern "C" float FastMaxFloat(const float *p, uint32_t n) { return Max(p, n, kFloatEmpty); }
extern "C" double FastMaxDouble(const double *p, uint32_t n) { return Max(p, n, kDoubleEmpty); }
extern "C" int8_t FastMaxI8(const int8_t *p, uint32_t n) { return Max<int8_t>(p, n, INT8_MIN); }
extern "C" int16_t FastMaxI16(const int16_t *p, uint32_t n) { return Max<int16_t>(p, n, INT16_MIN); }
extern "C" int32_t FastMaxI32(const int32_t *p, uint32_t n) { return Max<int32_t>(p, n, INT32_MIN); }
extern "C" int64_t FastMaxI64(const int64_t *p, uint32_t n) { return Max<int64_t>(p, n, INT64_MIN); }
extern "C" uint8_t FastMaxU8(const uint8_t *p, uint32_t n) { return Max<uint8_t>(p, n, 0); }
extern "C" uint16_t FastMaxU16(const uint16_t *p, uint32_t n) { return Max<uint16_t>(p, n, 0); }
extern "C" uint32_t FastMaxU32(const uint32_t *p, uint32_t n) { return Max<uint32_t>(p, n, 0); }
extern "C" uint64_t FastMaxU64(const uint64_t *p, uint32_t n) { return Max<uint64_t>(p, n, 0); }

extern "C" uint32_t FastMinIdxFloat(const float *p, uint32_t n) { return MinIndex(p, n); }
extern "C" uint32_t FastMinIdxDouble(const double *p, uint32_t n) { return MinIndex(p, n); }
extern "C" uint32_t FastMinIdxI8(const int8_t *p, uint32_t n) { return MinIndex(p, n); }
extern "C" uint32_t FastMinIdxI16(const int16_t *p, uint32_t n) { return MinIndex(p, n); }
extern "C" uint32_t FastMinIdxI32(const int32_t *p, uint32_t n) { return MinIndex(p, n); }
extern "C" uint32_t FastMinIdxI64(const int64_t *p, uint32_t n) { return MinIndex(p, n); }
extern "C" uint32_t FastMinIdxU8(const uint8_t *p, uint32_t n) { return MinIndex(p, n); }
extern "C" uint32_t FastMinIdxU16(const uint16_t *p, uint32_t n) { return MinIndex(p, n); }
extern "C" uint32_t FastMinIdxU32(const uint32_t *p, uint32_t n) { return MinIndex(p, n); }
extern "C" uint32_t FastMinIdxU64(const uint64_t *p, uint32_t n) { return MinIndex(p, n); }

extern "C" uint32_t FastMaxIdxFloat(const float *p, uint32_t n) { return MaxIndex(p, n); }
extern "C" uint32_t FastMaxIdxDouble(const double *p, uint32_t n) { return MaxIndex(p, n); }
extern "C" uint32_t FastMaxIdxI8(const int8_t *p, uint32_t n) { return MaxIndex(p, n); }
extern "C" uint32_t FastMaxIdxI16(const int16_t *p, uint32_t n) { return MaxIndex(p, n); }
extern "C" uint32_t FastMaxIdxI32(const int32_t *p, uint32_t n) { return MaxIndex(p, n); }
extern "C" uint32_t FastMaxIdxI64(const int64_t *p, uint32_t n) { return MaxIndex(p, n); }
extern "C" uint32_t FastMaxIdxU8(const uint8_t *p, uint32_t n) { return MaxIndex(p, n); }
extern "C" uint32_t FastMaxIdxU16(const uint16_t *p, uint32_t n) { return MaxIndex(p, n); }
extern "C" uint32_t FastMaxIdxU32(const uint32_t *p, uint32_t n) { return MaxIndex(p, n); }
extern "C" uint32_t FastMaxIdxU64(const uint64_t *p, uint32_t n) { return MaxIndex(p, n); }
