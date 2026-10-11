// Pins the sign conventions in math.h. Expected values come from a brute-force
// G(3,0,1) that builds the antiproduct and sandwich from Lengyel's
// definitions of the complements and antireverse.

#include "src/math.h"

#include <cstdio>

static U32 failures = 0;

static void Check(const char *const what, const F64 actual, const F64 expected) {
    if (Abs(actual - expected) > 1.0e-5) {
        printf("FAIL %s: %.7f, expected %.7f\n", what, actual, expected);
        ++failures;
    }
}

static void Check(const char *const what, const Vec3 actual, const Vec3 expected) {
    Check(what, actual.x, expected.x);
    Check(what, actual.y, expected.y);
    Check(what, actual.z, expected.z);
}

int main() {
    constexpr Line3 kZAxis = {{0.0f, 0.0f, 1.0f}, {}};
    Check("rotation turns +X to +Y about +Z",
          Transform(Rotation(kZAxis, 0.5f * kPi), Point3{{1.0f, 0.0f, 0.0f}}).xyz, {0.0f, 1.0f, 0.0f});
    Check("translation moves the origin",
          Transform(Translation({0.5f, -1.0f, 2.0f}), Point3{}).xyz, {0.5f, -1.0f, 2.0f});
    Check("translation leaves directions",
          Transform(Translation({0.5f, -1.0f, 2.0f}), Vec3{1.0f, 2.0f, 3.0f}), {1.0f, 2.0f, 3.0f});

    const Line3 join = Wedge(Point3{{1.0f, 2.0f, 3.0f}}, Point3{{4.0f, -1.0f, 2.0f}});
    Check("join direction", join.v, {3.0f, -3.0f, -1.0f});
    Check("join moment", join.m, {7.0f, 10.0f, -9.0f});

    // A screw: turn 1.2 radians about the line through (1, 2, 0) along (1, 1, 1),
    // then move by (0.5, -1, 2).
    const F32 inverseRoot3 = 1.0f / kSqrt3;
    const Point3 onAxis = {{1.0f, 2.0f, 0.0f}};
    const Line3 axis = Wedge(onAxis, Add(onAxis, Vec3{inverseRoot3, inverseRoot3, inverseRoot3}));
    const Motor3 turn = Rotation(axis, 1.2f);
    const Motor3 screw = Compose(Translation({0.5f, -1.0f, 2.0f}), turn);
    Check("composed weight", screw.v, {0.3259965f, 0.3259965f, 0.3259965f});
    Check("composed weight antiscalar", screw.vw, 0.8253356);
    Check("composed bulk", screw.m, {0.3693321f, -0.4941669f, 0.7438365f});
    Check("composed bulk scalar", screw.mw, -0.2444974);
    Check("points on the axis stay", Transform(turn, onAxis).xyz, onAxis.xyz);

    const Point3 p = {{0.3f, -1.2f, 2.5f}};
    Check("screw moves a point", Transform(screw, p).xyz, {4.0160274f, -2.1790729f, 1.2630455f});
    Check("screw turns a direction", Transform(screw, p.xyz), {2.4398014f, -1.2786021f, 0.4388007f});
    Check("composition applies right to left", Transform(screw, p).xyz,
          Transform(Translation({0.5f, -1.0f, 2.0f}), Transform(turn, p)).xyz);
    Check("antireverse undoes", Transform(Antireverse(screw), Transform(screw, p)).xyz, p.xyz);

    // The plane z = 2.
    const Plane3d plane = {{0.0, 0.0, 1.0}, -2.0};
    Check("height above a plane", Antiwedge(Point3d{{0.0, 0.0, 5.0}}, plane), 3.0);
    Check("plane support", Support(plane).xyz.z, 2.0);

    if (failures != 0) {
        printf("%u checks failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
