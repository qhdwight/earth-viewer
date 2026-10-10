#version 460

layout(location = 0) in vec3 rayDir;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 cameraPos;
    layout(offset = 96) vec4 sunDirection;
} pc;

const vec3 kSphereColor = vec3(0.005); // linear; the swapchain is sRGB
const float kStarGridScale = 180.0;
const float kStarDensity = 0.025;
const float kPi = 3.14159265;

// The glow is a thin haze whose density falls off exponentially with height
// above the surface. Its size is physical, in Earth radii, so it scales with
// zoom like everything else.
const float kGlowScaleHeight = 0.02; // about 250 km
const float kGlowGain = 1.8;
const vec3 kGlowColor = vec3(1.0);
// The glow is full strength where the haze is sunlit and falls to
// kNightGlow on the night side. The haze sits above the ground, so light
// wraps a little past the terminator: kTwilightWrap is the cosine of the
// sun angle over which it fades.
const float kNightGlow = 0.15;
const float kTwilightWrap = 0.3;

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

// Probability that a standard normal variable is below x. Logistic fit with
// error under 0.01.
float NormalCdf(const float x) {
    return 1.0 / (1.0 + exp(-1.702 * x));
}

// Amount of haze along the ray o + t d for t >= 0, stopping at the surface
// when hit is true. Grazing rays cross far more haze than steep ones, which
// is what makes the outline glow. b is dot(o, d).
float GlowDepth(const vec3 o, const vec3 d, const float b, const bool hit) {
    const float h = kGlowScaleHeight;
    const float r = length(o);
    const float altitude = r - 1.0;
    if (hit) {
        // Slant path from the camera down to the surface through locally
        // flat haze. The 2h/pi term caps it at the curved value when grazing.
        const float t = -b - sqrt(max(b * b - dot(o, o) + 1.0, 0.0));
        const float mu = dot(normalize(o + d * t), -d);
        return h * (1.0 - exp(-altitude / h)) / sqrt(mu * mu + 2.0 * h / kPi);
    }
    if (b >= 0.0) {
        // Looking away from the globe: the densest point is the camera.
        const float mu = b / r;
        return h * exp(-altitude / h) / sqrt(mu * mu + 2.0 * h / (kPi * r));
    }
    // Passing the globe: altitude along the ray is nearly a parabola around
    // the closest approach, so the density along t is a Gaussian. Clamping
    // keeps limb pixels that partly cover the globe finite.
    const float closest = max(sqrt(max(dot(o, o) - b * b, 0.0)), 1.0);
    const float sigma = sqrt(closest * h);
    return exp(-(closest - 1.0) / h) * sqrt(2.0 * kPi) * sigma * NormalCdf(-b / sigma);
}

// Squaring the depth is a contrast curve: thin paths, such as looking
// straight down at the globe or straight up from near the ground, stay
// almost black while grazing paths glow.
vec3 Glow(const float depth) {
    const float x = kGlowGain * depth;
    return kGlowColor * (1.0 - exp(-x * x));
}

// How strongly the haze at point p glows, from its angle to the Sun.
// Dividing by a clamped length keeps the centre of the globe finite.
float Sunlight(const vec3 p) {
    const float cosine = dot(p, pc.sunDirection.xyz) / max(length(p), 0.000001);
    return mix(kNightGlow, 1.0, smoothstep(-kTwilightWrap, kTwilightWrap, cosine));
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
    // Both glow paths are evaluated so limb pixels blend them smoothly.
    // Each path is lit by the Sun at its densest point: the closest approach
    // for rays passing the globe, the surface hit for rays striking it.
    const vec3 skyPoint = o + d * max(-b, 0.0);
    const vec3 globePoint = o + d * (-b - sqrt(max(discriminant, 0.0)));
    const vec3 sky = background + Glow(GlowDepth(o, d, b, false)) * Sunlight(skyPoint);
    const vec3 globe = kSphereColor + Glow(GlowDepth(o, d, b, true)) * Sunlight(globePoint);
    // Composite here so the globe occludes stars even with MAX blending.
    outColor = vec4(mix(sky, globe, alpha), 1.0);
}
