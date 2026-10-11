#version 460

layout(location = 0) in vec3 rayDir;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    layout(offset = 48) vec4 cameraPos;
    vec4 line; // w: UI scale, physical pixels per UI pixel
    vec4 sunDirection;
} pc;

// Colours are linear; the swapchain is sRGB. The palette follows 1980s
// computer graphics: neon green coastlines and cyan grids on black.
const vec3 kSphereColor = vec3(0.002, 0.008, 0.008);
const float kStarGridScale = 180.0;
const float kStarDensity = 0.025;
const float kPi = 3.14159265;

// Latitude and longitude lines, drawn exactly from the ray hit rather than as
// geometry. Widths are in UI pixels, so the lines stay faint at any zoom.
const float kGraticuleDegrees = 15.0;
const vec3 kGraticuleColor = vec3(0.06, 0.5, 0.7);
const float kGraticuleIntensity = 0.16;
const float kGraticuleMajorBoost = 2.0; // the equator and prime meridian
const float kGraticuleWidthPixels = 1.0;
// Lines closer together than this on screen fade out, so the meridians do
// not merge into a solid cap near the poles or at the limb.
const float kGridMinSpacingPixels = 5.0;

// The Sun, drawn like a bright star on film: a small core that burns out to
// white, a warm halo, and six diffraction spikes fixed to the screen. Sizes
// are in UI pixels, so it looks the same at any zoom or display scaling.
const float kSunCoreRadius = 6.0;
const vec3 kSunCoreColor = vec3(1.0, 0.95, 0.8) * 6.0; // past 1, it clips to white
const float kSunHaloScale = 7.0;
const vec3 kSunHaloColor = vec3(1.0, 0.55, 0.12) * 2.5;
const float kSunGlareScale = 44.0;
const vec3 kSunGlareColor = vec3(1.0, 0.55, 0.2) * 0.05;
const int kSunSpikePairs = 3; // six rays, 60 degrees apart
const float kSunSpikeLength = 14.0; // e-folding length
const float kSunSpikeWidth = 2.4; // at the core, widening outward
const float kSunSpikeTaper = 0.08;
const vec3 kSunSpikeColor = vec3(1.0, 0.65, 0.25) * 1.6;
// Beyond this the Sun adds nothing visible, so the work is skipped.
const float kSunReach = 300.0;

// The globe's day side is faintly lit, so it reads as a crescent against the
// Sun. Kept dim so the lines drawn on it stay legible.
const vec3 kDayColor = vec3(0.004, 0.022, 0.014);

// The glow is a thin haze whose density falls off exponentially with height
// above the surface. Its size is physical, in Earth radii, so it scales with
// zoom like everything else.
const float kGlowScaleHeight = 0.02; // about 250 km
const float kGlowGain = 1.8;
const vec3 kGlowColor = vec3(0.25, 1.0, 0.8);
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
                const vec3 tint = mix(vec3(0.6, 0.85, 1.0), vec3(0.8, 1.0, 0.95), random.z);
                color += tint * mix(0.12, 0.8, brightness * brightness) * coverage;
            }
        }
    }
    return color;
}

// Coverage of the grid lines at integer values of coordinate, a family that
// moves footprint cells per pixel. Lines get one pixel of anti-aliasing and
// fade out once they crowd together on screen.
float GridLines(const float coordinate, const float footprint, const float widthPixels) {
    const float distancePixels = abs(fract(coordinate + 0.5) - 0.5) / max(footprint, 1.0e-9);
    const float halfWidth = 0.5 * widthPixels;
    const float line = 1.0 - smoothstep(halfWidth - 0.5, halfWidth + 0.5, distancePixels);
    const float spacingPixels = 1.0 / max(footprint, 1.0e-9);
    // Thinner than a pixel, a line dims instead of breaking up.
    return line * min(widthPixels, 1.0) *
           smoothstep(kGridMinSpacingPixels, 2.0 * kGridMinSpacingPixels, spacingPixels);
}

// Latitude and longitude lines at point p on the globe, in UI pixels scaled
// by uiScale. Must run in uniform control flow for the derivatives.
vec3 Graticule(const vec3 p, const float uiScale) {
    const float cell = radians(kGraticuleDegrees);
    const float lat = asin(clamp(p.y, -1.0, 1.0));
    const float lon = atan(p.x, p.z);
    // atan jumps at the antimeridian, where its derivative would spike. The
    // same angle measured from the far side jumps at 0 degrees instead, so
    // the smaller of the two derivatives is right everywhere.
    const float lonFootprint = min(fwidth(lon), fwidth(atan(-p.x, -p.z))) / cell;
    const float latFootprint = fwidth(lat) / cell;
    const float width = kGraticuleWidthPixels * uiScale;
    const float parallels = GridLines(lat / cell, latFootprint, width) *
                            (abs(lat) < 0.5 * cell ? kGraticuleMajorBoost : 1.0);
    const float meridians = GridLines(lon / cell, lonFootprint, width) *
                            (abs(lon) < 0.5 * cell ? kGraticuleMajorBoost : 1.0);
    return kGraticuleColor * kGraticuleIntensity * max(parallels, meridians);
}

// The Sun at screen offset (x, y) in UI pixels from its centre.
vec3 Sun(const vec2 offset) {
    const float r = length(offset);
    const float core = 1.0 - smoothstep(kSunCoreRadius - 1.0, kSunCoreRadius + 1.0, r);
    vec3 color = kSunCoreColor * core + kSunHaloColor * exp(-r / kSunHaloScale) +
                 kSunGlareColor * exp(-r / kSunGlareScale);
    // Each spike pair is a line through the centre; a ray fades along its
    // length and blurs across it as it widens.
    const float width = kSunSpikeWidth + kSunSpikeTaper * r;
    float spikes = 0.0;
    for (int i = 0; i < kSunSpikePairs; ++i) {
        const float angle = float(i) * kPi / float(kSunSpikePairs);
        const float across = dot(offset, vec2(-sin(angle), cos(angle)));
        spikes += exp(-across * across / (width * width));
    }
    return color + kSunSpikeColor * spikes * exp(-r / kSunSpikeLength);
}

// Probability that a standard normal variable is below x. Logistic fit with
// error under 0.01.
float NormalCdf(const float x) {
    return 1.0 / (1.0 + exp(-1.702 * x));
}

// Amount of haze along the ray o + t d for t >= 0, stopping at the surface
// when hit is true. Grazing rays cross far more haze than steep ones, which
// is what makes the outline glow. b is dot(o, d) and moment is the ray's
// moment o x d, as long as its closest approach to the globe's centre.
float GlowDepth(const vec3 o, const vec3 d, const float b, const vec3 moment, const bool hit) {
    const float h = kGlowScaleHeight;
    const float r = length(o);
    const float altitude = r - 1.0;
    if (hit) {
        // Slant path from the camera down to the surface through locally
        // flat haze. The 2h/pi term caps it at the curved value when grazing.
        const float t = -b - sqrt(max(1.0 - dot(moment, moment), 0.0));
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
    const float closest = max(length(moment), 1.0);
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
    const vec3 dx = dFdx(d);
    const vec3 dy = dFdy(d);
    const float pixelAngle = max(max(length(dx), length(dy)), 0.000001);
    // The ray as a line: unit direction d, the weight, and moment o x d, the
    // bulk. By Lagrange's identity the discriminant is the squared weight
    // less the squared bulk, which avoids subtracting nearly equal b^2 and
    // o.o far from the globe.
    const vec3 moment = cross(o, d);
    const float b = dot(o, d);
    const float discriminant = 1.0 - dot(moment, moment);
    // Fade the limb over about one pixel instead of cutting it hard.
    const float limbWidth = max(fwidth(discriminant), 0.000001);
    const float alpha = b < 0.0 ? smoothstep(0.0, limbWidth, discriminant) : 0.0;
    // Grid lines first: their derivatives need every pixel of the quad.
    const vec3 globePoint = o + d * (-b - sqrt(max(discriminant, 0.0)));
    const vec3 graticule = Graticule(normalize(globePoint), pc.line.w);
    vec3 background = vec3(0.0);
    if (alpha < 1.0) {
        // Star sizes are in UI pixels, so they look the same at any display scaling.
        background += StarField(d, pixelAngle * pc.line.w);
        // The Sun's screen offset from this pixel: project the direction
        // difference onto the world-space steps of one pixel right and down.
        const vec3 sun = pc.sunDirection.xyz;
        const vec3 toPixel = d - sun;
        const vec2 offset = vec2(dot(toPixel, dx) / max(dot(dx, dx), 1.0e-12),
                                 dot(toPixel, dy) / max(dot(dy, dy), 1.0e-12)) / pc.line.w;
        if (dot(d, sun) > 0.0 && length(offset) < kSunReach) {
            background += Sun(offset);
        }
    }
    // Both glow paths are evaluated so limb pixels blend them smoothly.
    // Each path is lit by the Sun at its densest point: the closest approach
    // for rays passing the globe, the surface hit for rays striking it. The
    // closest approach is the line's support point d x m, ahead of the camera
    // only when b < 0.
    const vec3 skyPoint = b < 0.0 ? cross(d, moment) : o;
    const vec3 sky = background + Glow(GlowDepth(o, d, b, moment, false)) * Sunlight(skyPoint);
    const vec3 normal = normalize(globePoint);
    const float daylight = smoothstep(-0.05, 0.3, dot(normal, pc.sunDirection.xyz));
    const vec3 globe = kSphereColor + kDayColor * daylight + graticule + Glow(GlowDepth(o, d, b, moment, true)) * Sunlight(globePoint);
    // Composite here so the globe occludes stars even with MAX blending.
    outColor = vec4(mix(sky, globe, alpha), 1.0);
}
