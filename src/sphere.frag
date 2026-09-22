#version 460

layout(location = 0) in vec3 rayDir;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 cameraPos;
} pc;

const vec3 kSphereColor = vec3(0.005); // linear; the swapchain is sRGB
const float kStarGridScale = 180.0;
const float kStarDensity = 0.025;

uvec3 HashCell(const ivec3 cell) {
    // Every output must depend on all three coordinates. Pairwise mixing
    // repeated star selection along Z and brightness along X, creating bands.
    const uvec3 coordinates = uvec3(cell) * uvec3(1597334677u, 3812015801u, 2798796415u);
    const uint seed = coordinates.x ^ coordinates.y ^ coordinates.z;
    uvec3 h = uvec3(seed) ^ uvec3(0x68bc21ebu, 0x02e5be93u, 0x967a889bu);
    h = (h ^ (h >> 16u)) * 0x7feb352du;
    h = (h ^ (h >> 15u)) * 0x846ca68bu;
    return h ^ (h >> 16u);
}

vec3 StarField(const vec3 direction, const float pixelAngle) {
    const ivec3 cell = ivec3(floor(direction * kStarGridScale));
    vec3 color = vec3(0.0);
    // Search neighbouring cells so a star's soft edge crosses cell boundaries.
    // A thin spherical shell avoids longitude seams and pole clustering.
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const ivec3 neighbour = cell + ivec3(x, y, z);
                const uvec3 hash = HashCell(neighbour);
                const vec3 random = vec3(hash & uvec3(65535u)) / 65536.0;
                const float chance = float(hash.x >> 16u) / 65536.0;
                const vec3 point = vec3(neighbour) + random;
                if (chance >= kStarDensity || abs(length(point) - kStarGridScale) > 0.5) {
                    continue;
                }
                const float brightness = float(hash.y >> 16u) / 65536.0;
                const float radius = mix(0.55, 1.15, brightness * brightness);
                const float distancePixels = length(direction - normalize(point)) / pixelAngle;
                const float coverage = 1.0 - smoothstep(max(radius - 0.5, 0.0), radius + 0.5, distancePixels);
                const vec3 tint = mix(vec3(0.75, 0.85, 1.0), vec3(1.0, 0.9, 0.75), random.z);
                color += tint * mix(0.12, 0.8, brightness * brightness) * coverage;
            }
        }
    }
    return color;
}

void main() {
    // Nearest intersection of the ray o + t d with the unit sphere:
    // t^2 + 2 t (o.d) + (o.o - 1) = 0.
    const vec3 o = pc.cameraPos.xyz;
    const vec3 d = normalize(rayDir);
    // Derivatives must be evaluated before branching around the opaque globe.
    const float pixelAngle = max(max(length(dFdx(d)), length(dFdy(d))), 0.000001);
    const float b = dot(o, d);
    const float discriminant = b * b - (dot(o, o) - 1.0);
    // Fade the limb over about one pixel instead of cutting it hard.
    const float limbWidth = max(fwidth(discriminant), 0.000001);
    const float alpha = b < 0.0 ? smoothstep(0.0, limbWidth, discriminant) : 0.0;
    vec3 background = vec3(0.0);
    if (alpha < 1.0) {
        background = StarField(d, pixelAngle);
    }
    // Composite here so the globe occludes stars even with MAX blending.
    outColor = vec4(mix(background, kSphereColor, alpha), 1.0);
}
