#ifndef PGA_GLSL
#define PGA_GLSL

// Rigid geometric algebra for the shaders, mirroring math.h and Eric Lengyel's
// conventions at rigidgeometricalgebra.org. A vec3 is a unitized point or a
// direction, as the name says. A plane is a vec4: normal n, the weight, in xyz
// and the bulk in w, holding the points p where dot(n, p) + w = 0.

// A line: direction v, the weight, and moment m, the bulk.
struct Line {
    vec3 v;
    vec3 m;
};

// A motor, laid out as Motor3 in math.h: v holds e41, e42, e43 and the
// antiscalar, the rotation; m holds e23, e31, e12 and the scalar, the
// translation.
struct Motor {
    vec4 v;
    vec4 m;
};

// The join of two points: the line through p toward q.
Line Wedge(const vec3 p, const vec3 q) {
    return Line(q - p, cross(p, q));
}

// The join of a line and a point: the plane containing both.
vec4 Wedge(const Line l, const vec3 p) {
    return vec4(cross(l.v, p) + l.m, -dot(p, l.m));
}

// The meet of a point and a plane: the point's signed distance above it, in
// units of the normal's length.
float Antiwedge(const vec3 p, const vec4 g) {
    return dot(g.xyz, p) + g.w;
}

// For a unitized motor, the inverse motion.
Motor Antireverse(const Motor q) {
    return Motor(vec4(-q.v.xyz, q.v.w), vec4(-q.m.xyz, q.m.w));
}

// The sandwich q v p v ~q for a point, and for a direction, which lacks e4 and
// so ignores the translation.
vec3 Transform(const Motor q, const vec3 p) {
    const vec3 a = cross(q.v.xyz, p) + q.m.xyz;
    return p + 2.0 * (q.v.w * a + cross(q.v.xyz, a) - q.m.w * q.v.xyz);
}

vec3 TransformDirection(const Motor q, const vec3 d) {
    const vec3 a = cross(q.v.xyz, d);
    return d + 2.0 * (q.v.w * a + cross(q.v.xyz, a));
}

// Clip coordinates of a camera-space point, as Project in math.h. projection
// holds Perspective: x and y scales and the near plane distance.
vec4 Project(const vec4 projection, const vec3 p) {
    return vec4(projection.x * p.x, projection.y * p.y, -p.z - projection.z, -p.z);
}

#endif
