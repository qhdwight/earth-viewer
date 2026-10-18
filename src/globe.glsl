#ifndef GLOBE_GLSL
#define GLOBE_GLSL

#include "pga.glsl"

// Points on the surface sit exactly on the globe, at t = 1 below; this margin
// keeps them from flickering in and out. Matches kOcclusionMargin in main.cpp.
const float kOcclusionMargin = 0.001;

// Whether the unit globe hides point p from the eye: the line of sight, the
// join of eye and p, meets the globe first at eye + t (p - eye) for t in
// (0, 1). The quadratic's discriminant is the line's squared weight less its
// squared bulk. Matches HiddenByGlobe in main.cpp.
bool HiddenByGlobe(const vec3 eye, const vec3 p) {
    const Line sight = Wedge(eye, p);
    const float a = dot(sight.v, sight.v);
    const float discriminant = a - dot(sight.m, sight.m);
    const float t = (-dot(eye, sight.v) - sqrt(max(discriminant, 0.0))) / a;
    return discriminant > 0.0 && t > 0.0 && t < 1.0 - kOcclusionMargin;
}

#endif
