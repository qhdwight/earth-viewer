#version 460
#extension GL_EXT_buffer_reference : require

// Links between ground stations and satellites, drawn as camera-facing
// ribbons like the borders in earth.vert, but with 3D endpoints.
layout(location = 0) out vec3 worldPos;
// x: distance past the segment start, y: distance past the segment end,
// z: distance from the centre line. All in half widths.
layout(location = 1) out vec3 ribbon;

// Endpoints in world space, two per link.
layout(buffer_reference, std430) readonly buffer Segments {
    vec4 points[];
};

// Same layout as earth.vert; main.cc pushes link widths and this buffer.
layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 cameraPos;
    // x: half width in world units, y: minimum half width per unit of view
    // depth, z: anti-aliasing padding per unit of view depth.
    vec4 line;
    vec4 sunDirection;
    Segments segments;
} pc;

const int kVerticesPerSegment = 6;
const int kCorners[kVerticesPerSegment] = int[kVerticesPerSegment](0, 1, 2, 1, 3, 2);

vec3 SafeNormalize(const vec3 v) {
    const float len = length(v);
    return len > 0.0 ? v / len : vec3(0.0);
}

void main() {
    const int segment = gl_VertexIndex / kVerticesPerSegment;
    const int corner = kCorners[gl_VertexIndex % kVerticesPerSegment];
    const float end = float(corner >> 1);             // 0 or 1
    const float side = float(corner & 1) * 2.0 - 1.0; // -1 or +1

    const vec3 p0 = pc.segments.points[2 * segment + 0].xyz;
    const vec3 p1 = pc.segments.points[2 * segment + 1].xyz;
    const vec3 p = mix(p0, p1, end);

    const vec3 along = SafeNormalize(p1 - p0);
    const vec3 across = SafeNormalize(cross(along, pc.cameraPos.xyz - p));

    const vec4 clip = pc.viewProj * vec4(p, 1.0);
    const float depth = max(clip.w, 0.0);
    const float halfWidth = max(pc.line.x, pc.line.y * depth);
    const float paddedHalfWidth = halfWidth + pc.line.z * depth;
    const float padded = paddedHalfWidth / halfWidth;
    const vec3 offset = (across * side + along * (end * 2.0 - 1.0)) * paddedHalfWidth;

    const float lengthInHalfWidths = length(p1 - p0) / halfWidth;
    worldPos = p;
    ribbon = vec3(mix(padded, -lengthInHalfWidths - padded, end),
                  mix(-lengthInHalfWidths - padded, padded, end),
                  side * padded);
    gl_Position = clip + pc.viewProj * vec4(offset, 0.0);
}
