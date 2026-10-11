#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_GOOGLE_include_directive : require

#include "globe.glsl"

// Each satellite or ground station is a camera-facing square of six
// vertices, sized to fit a round dot plus its halo. Positions are in world
// space, Earth radii; w is 0 for a satellite and 1 for a ground station.
layout(location = 0) out vec2 local; // position in the sprite, in dot radii
layout(location = 1) flat out vec3 color;
layout(location = 2) flat out float pixelsPerRadius;

layout(buffer_reference, std430) readonly buffer Positions {
    vec4 positions[];
};

layout(push_constant) uniform SatellitePush {
    Motor view; // world to camera space
    vec4 projection; // Perspective in math.h
    vec4 cameraPos;
    vec4 sunDirection;
    // x: dot radius in world units, y: world units per UI pixel per unit of
    // view depth, zw: clip-space offset per world unit across the view. A UI
    // pixel is a pixel at 100% display scaling.
    vec4 sprite;
    Positions positions;
} pc;

const int kVerticesPerSprite = 6;
const vec2 kCorners[kVerticesPerSprite] = vec2[kVerticesPerSprite](
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),
    vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
// Dots never shrink below this on screen, in UI pixels, like the border lines.
const float kMinRadiusPixels = 2.0;
const float kHaloRadii = 3.0; // must match satellite.frag
// Sunlit satellites glow mint, those in Earth's shadow dim blue, and ground
// stations, where the laser links start, white.
const vec3 kSunlitColor = vec3(0.5, 1.0, 0.85);
const vec3 kShadowColor = vec3(0.08, 0.2, 0.45);
const vec3 kStationColor = vec3(1.0);

void main() {
    const vec4 marker = pc.positions.positions[gl_VertexIndex / kVerticesPerSprite];
    const vec3 p = marker.xyz;
    const vec2 corner = kCorners[gl_VertexIndex % kVerticesPerSprite];

    // Ground stations sit on the surface; the margin in HiddenByGlobe keeps
    // them from flickering in and out.
    if (HiddenByGlobe(pc.cameraPos.xyz, p)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // outside the clip volume
        return;
    }

    // In Earth's shadow, approximated as a cylinder behind the globe: on the
    // night side and within one radius of the line from the Sun through the
    // globe's centre. That line has unit direction and no moment, so its
    // distance from p is the length of the cross product.
    const vec3 sun = pc.sunDirection.xyz;
    const vec3 offAxis = cross(sun, p);
    const bool shadowed = dot(p, sun) < 0.0 && dot(offAxis, offAxis) < 1.0;
    color = marker.w > 0.5 ? kStationColor : (shadowed ? kShadowColor : kSunlitColor);

    // An offset perpendicular to the view changes clip xy by a fixed amount per
    // world unit at any depth, so the sprite is built directly in clip space.
    const vec4 clip = Project(pc.projection, Transform(pc.view, p));
    const float worldPerPixel = pc.sprite.y * max(clip.w, 0.0);
    const float radius = max(pc.sprite.x, kMinRadiusPixels * worldPerPixel);
    pixelsPerRadius = radius / max(worldPerPixel, 1.0e-12);
    local = corner * kHaloRadii;
    gl_Position = clip + vec4(local * radius * pc.sprite.zw, 0.0, 0.0);
}
