#version 460
#extension GL_EXT_buffer_reference : require

// Each satellite is a camera-facing square of six vertices, sized to fit a
// round dot plus its halo. Positions are in world space, Earth radii.
layout(location = 0) out vec2 local; // position in the sprite, in dot radii
layout(location = 1) flat out vec3 color;
layout(location = 2) flat out float pixelsPerRadius;

layout(buffer_reference, std430) readonly buffer Positions {
    vec4 positions[];
};

layout(push_constant) uniform SatellitePush {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 sunDirection;
    // x: dot radius in world units, y: world units per pixel per unit of view
    // depth, zw: clip-space offset per world unit across the view.
    vec4 sprite;
    Positions positions;
} pc;

const int kVerticesPerSprite = 6;
const vec2 kCorners[kVerticesPerSprite] = vec2[kVerticesPerSprite](
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),
    vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
// Dots never shrink below this on screen, like the border lines.
const float kMinRadiusPixels = 2.0;
const float kHaloRadii = 3.0; // must match satellite.frag
const vec3 kSunlitColor = vec3(1.0, 0.72, 0.28);
const vec3 kShadowColor = vec3(0.22, 0.28, 0.45);

void main() {
    const vec3 p = pc.positions.positions[gl_VertexIndex / kVerticesPerSprite].xyz;
    const vec2 corner = kCorners[gl_VertexIndex % kVerticesPerSprite];

    // Hidden when the line of sight from the camera meets the globe first:
    // solve |o + t d| = 1 for t in (0, 1).
    const vec3 o = pc.cameraPos.xyz;
    const vec3 d = p - o;
    const float a = dot(d, d);
    const float b = dot(o, d);
    const float discriminant = b * b - a * (dot(o, o) - 1.0);
    const float t = (-b - sqrt(max(discriminant, 0.0))) / a;
    if (discriminant > 0.0 && t > 0.0 && t < 1.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // outside the clip volume
        return;
    }

    // In Earth's shadow, approximated as a cylinder behind the globe.
    const float sunward = dot(p, pc.sunDirection.xyz);
    const bool shadowed = sunward < 0.0 && dot(p, p) - sunward * sunward < 1.0;
    color = shadowed ? kShadowColor : kSunlitColor;

    // An offset perpendicular to the view changes clip xy by a fixed amount per
    // world unit at any depth, so the sprite is built directly in clip space.
    const vec4 clip = pc.viewProj * vec4(p, 1.0);
    const float worldPerPixel = pc.sprite.y * max(clip.w, 0.0);
    const float radius = max(pc.sprite.x, kMinRadiusPixels * worldPerPixel);
    pixelsPerRadius = radius / max(worldPerPixel, 1.0e-12);
    local = corner * kHaloRadii;
    gl_Position = clip + vec4(local * radius * pc.sprite.zw, 0.0, 0.0);
}
