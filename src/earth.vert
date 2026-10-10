#version 460
#extension GL_EXT_buffer_reference : require

layout(location = 0) out vec3 worldPos;
// x: distance past the segment start, y: distance past the segment end,
// z: distance from the centre line. All in half widths; earth.frag rounds
// the caps with these.
layout(location = 1) out vec3 ribbon;

// Each point packs a longitude (low 16 bits) and a latitude (high 16 bits)
// as two's complement integers in units of 1/180 degree. Matches
// PackBorderPoint in main.cc. Points come in pairs, one pair per segment.
layout(buffer_reference, std430) readonly buffer VertexBuffer {
    uint points[];
};

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 cameraPos;
    // x: half width in world units, y: minimum half width per unit of view
    // depth, z: anti-aliasing padding per unit of view depth.
    vec4 line;
    vec4 sunDirection; // used by sphere.frag
    VertexBuffer vertexBuffer;
} pc;

const float kUnitsPerDegree = 180.0;

// Every segment is drawn as six vertices forming two triangles. Corner bit 0
// picks the side of the line, bit 1 picks the end of the segment.
const int kVerticesPerSegment = 6;
const int kCorners[kVerticesPerSegment] = int[kVerticesPerSegment](0, 1, 2, 1, 3, 2);

// Parametric unit sphere: +Y through the north pole, +Z through the prime
// meridian, +X through 90 degrees east.
vec3 SpherePoint(const uint point) {
    const int packed = int(point);
    const float lon = radians(float((packed << 16) >> 16) / kUnitsPerDegree);
    const float lat = radians(float(packed >> 16) / kUnitsPerDegree);
    return vec3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));
}

vec3 SafeNormalize(const vec3 v) {
    const float len = length(v);
    return len > 0.0 ? v / len : vec3(0.0);
}

void main() {
    const int segment = gl_VertexIndex / kVerticesPerSegment;
    const int corner = kCorners[gl_VertexIndex % kVerticesPerSegment];
    const float end = float(corner >> 1);          // 0 or 1
    const float side = float(corner & 1) * 2.0 - 1.0; // -1 or +1

    const vec3 p0 = SpherePoint(pc.vertexBuffer.points[2 * segment + 0]);
    const vec3 p1 = SpherePoint(pc.vertexBuffer.points[2 * segment + 1]);
    const vec3 p = mix(p0, p1, end);

    // A ribbon facing the camera: widen perpendicular to both the segment and
    // the view direction, and extend the ends by the same amount to make room
    // for round caps, which join adjacent segments smoothly.
    const vec3 along = SafeNormalize(p1 - p0);
    const vec3 across = SafeNormalize(cross(along, pc.cameraPos.xyz - p));

    const vec4 clip = pc.viewProj * vec4(p, 1.0);
    const float depth = max(clip.w, 0.0);
    const float halfWidth = max(pc.line.x, pc.line.y * depth);
    // The geometry is padded by about a pixel so the fragment shader can fade
    // the true edge; ribbon coordinates stay in units of the true half width.
    const float paddedHalfWidth = halfWidth + pc.line.z * depth;
    const float padded = paddedHalfWidth / halfWidth;
    const vec3 offset = (across * side + along * (end * 2.0 - 1.0)) * paddedHalfWidth;

    // At the start corners this vertex sits one padded half width before p0,
    // at the end corners one padded half width after p1.
    const float lengthInHalfWidths = length(p1 - p0) / halfWidth;
    worldPos = p;
    ribbon = vec3(mix(padded, -lengthInHalfWidths - padded, end),
                  mix(-lengthInHalfWidths - padded, padded, end),
                  side * padded);
    gl_Position = clip + pc.viewProj * vec4(offset, 0.0);
}
