#version 460
#extension GL_EXT_buffer_reference : require

// Text pixel shader: a GLSL port of SlugPixelShader.hlsl from the Slug
// reference code, https://github.com/EricLengyel/Slug.
// Slug shader code Copyright 2017 by Eric Lengyel, MIT License; see LICENSE-SLUG.
//
// Each pixel computes its coverage directly from the glyph's quadratic Bezier
// outline: a horizontal and a vertical ray from the pixel center count signed
// crossings with the curves of the band the pixel lies in, each crossing
// weighted by how far it is from the center in pixels, and the two estimates
// are blended.
//
// Changes from the reference: curve and band data are read from linear
// buffers through device addresses instead of 4096-wide textures, so the
// band location wrap is gone; only the nonzero fill rule is kept, as TrueType
// requires; and quads flagged solid are filled for UI panels.

layout(buffer_reference, std430) readonly buffer Curves {
    vec4 texels[]; // (x1, y1, x2, y2); p3 is the xy of the next texel
};

layout(buffer_reference, std430) readonly buffer Bands {
    uvec2 entries[]; // band headers (count, offset), then curve lists (texel, 0)
};

layout(push_constant) uniform TextPush {
    layout(offset = 16) Curves curves;
    Bands bands;
} pc;

layout(location = 0) in vec4 color;
layout(location = 1) in vec2 texcoord;
layout(location = 2) flat in vec4 banding;
layout(location = 3) flat in ivec4 glyph;
layout(location = 0) out vec4 outColor;

// Matches kSolidFlag in text.vert, here within the band max x half.
const int kSolidFlag = 0x8000;

uint CalcRootCode(float y1, float y2, float y3) {
    // Calculate the root eligibility code for a sample-relative quadratic Bezier curve.
    // Extract the signs of the y coordinates of the three control points.

    uint i1 = floatBitsToUint(y1) >> 31u;
    uint i2 = floatBitsToUint(y2) >> 30u;
    uint i3 = floatBitsToUint(y3) >> 29u;

    uint shift = (i2 & 2u) | (i1 & ~2u);
    shift = (i3 & 4u) | (shift & ~4u);

    // Eligibility is returned in bits 0 and 8.

    return (0x2E74u >> shift) & 0x0101u;
}

vec2 SolveHorizPoly(vec4 p12, vec2 p3) {
    // Solve for the values of t where the curve crosses y = 0.
    // The quadratic polynomial in t is given by
    //
    //     a t^2 - 2b t + c,
    //
    // where a = p1.y - 2 p2.y + p3.y, b = p1.y - p2.y, and c = p1.y.
    // The discriminant b^2 - ac is clamped to zero, and imaginary
    // roots are treated as a double root at the global minimum
    // where t = b / a.

    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.y;
    float rb = 0.5 / b.y;

    float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
    float t1 = (b.y - d) * ra;
    float t2 = (b.y + d) * ra;

    // If the polynomial is nearly linear, then solve -2b t + c = 0.

    if (abs(a.y) < 1.0 / 65536.0) {
        t1 = t2 = p12.y * rb;
    }

    // Return the x coordinates where C(t) = 0.

    return vec2((a.x * t1 - b.x * 2.0) * t1 + p12.x, (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

vec2 SolveVertPoly(vec4 p12, vec2 p3) {
    // Solve for the values of t where the curve crosses x = 0.

    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.x;
    float rb = 0.5 / b.x;

    float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
    float t1 = (b.x - d) * ra;
    float t2 = (b.x + d) * ra;

    // If the polynomial is nearly linear, then solve -2b t + c = 0.

    if (abs(a.x) < 1.0 / 65536.0) {
        t1 = t2 = p12.x * rb;
    }

    // Return the y coordinates where C(t) = 0.

    return vec2((a.y * t1 - b.y * 2.0) * t1 + p12.y, (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

float CalcCoverage(float xcov, float ycov, float xwgt, float ywgt) {
    // Combine coverages from the horizontal and vertical rays using their weights.
    // Absolute values ensure that either winding direction convention works.

    float coverage = max(abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, 1.0 / 65536.0), min(abs(xcov), abs(ycov)));

    // Nonzero fill rule.

    return clamp(coverage, 0.0, 1.0);
}

float SlugRender(vec2 renderCoord, vec4 bandTransform, ivec4 glyphData) {
    // The effective pixel dimensions of the em square are computed
    // independently for x and y directions with texcoord derivatives.

    vec2 emsPerPixel = fwidth(renderCoord);
    vec2 pixelsPerEm = 1.0 / emsPerPixel;

    ivec2 bandMax = glyphData.zw & 0x00FF;

    // Determine what bands the current pixel lies in by applying a scale and offset
    // to the render coordinates. The scales are given by bandTransform.xy, and the
    // offsets are given by bandTransform.zw. Band indexes are clamped to [0, bandMax.xy].

    ivec2 bandIndex = clamp(ivec2(renderCoord * bandTransform.xy + bandTransform.zw), ivec2(0, 0), bandMax);
    int glyphLoc = glyphData.x;

    float xcov = 0.0;
    float xwgt = 0.0;

    // Fetch data for the horizontal band. The number of curves intersecting the
    // band is in x, and the offset to the list of those curves is in y.

    uvec2 hbandData = pc.bands.entries[glyphLoc + bandIndex.y];
    int hbandLoc = glyphLoc + int(hbandData.y);

    // Loop over all curves in the horizontal band.

    for (int curveIndex = 0; curveIndex < int(hbandData.x); curveIndex++) {
        // Fetch the three 2D control points for the current curve. The first texel
        // contains p1 and p2, and the second texel contains p3 in xy. Subtracting
        // the render coordinates makes the curve relative to the sample position.
        //
        //     C(t) = (1 - t)^2 p1 + 2t(1 - t) p2 + t^2 p3

        int curveLoc = int(pc.bands.entries[hbandLoc + curveIndex].x);
        vec4 p12 = pc.curves.texels[curveLoc] - vec4(renderCoord, renderCoord);
        vec2 p3 = pc.curves.texels[curveLoc + 1].xy - renderCoord;

        // If the largest x coordinate among all three control points falls
        // left of the current pixel, then there are no more curves in the
        // horizontal band that can influence the result, so exit the loop.
        // (The curves are sorted in descending order by max x coordinate.)

        if (max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5) {
            break;
        }

        uint code = CalcRootCode(p12.y, p12.w, p3.y);
        if (code != 0u) {
            // At least one root makes a contribution. Calculate them and scale so
            // that the current pixel corresponds to the range [0,1].

            vec2 r = SolveHorizPoly(p12, p3) * pixelsPerEm.x;

            // Bits in code tell which roots make a contribution.

            if ((code & 1u) != 0u) {
                xcov += clamp(r.x + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }

            if (code > 1u) {
                xcov -= clamp(r.y + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    float ycov = 0.0;
    float ywgt = 0.0;

    // Fetch data for the vertical band. This follows the data for all
    // horizontal bands, so we have to add bandMax.y + 1.

    uvec2 vbandData = pc.bands.entries[glyphLoc + bandMax.y + 1 + bandIndex.x];
    int vbandLoc = glyphLoc + int(vbandData.y);

    // Loop over all curves in the vertical band.

    for (int curveIndex = 0; curveIndex < int(vbandData.x); curveIndex++) {
        int curveLoc = int(pc.bands.entries[vbandLoc + curveIndex].x);
        vec4 p12 = pc.curves.texels[curveLoc] - vec4(renderCoord, renderCoord);
        vec2 p3 = pc.curves.texels[curveLoc + 1].xy - renderCoord;

        // If the largest y coordinate among all three control points falls
        // below the current pixel, then there are no more curves in the
        // vertical band that can influence the result, so exit the loop.
        // (The curves are sorted in descending order by max y coordinate.)

        if (max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5) {
            break;
        }

        uint code = CalcRootCode(p12.x, p12.z, p3.x);
        if (code != 0u) {
            vec2 r = SolveVertPoly(p12, p3) * pixelsPerEm.y;

            if ((code & 1u) != 0u) {
                ycov -= clamp(r.x + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }

            if (code > 1u) {
                ycov += clamp(r.y + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    return CalcCoverage(xcov, ycov, xwgt, ywgt);
}

void main() {
    // The solid flag is constant over a quad, so derivatives inside
    // SlugRender are still taken in uniform control flow.
    const float coverage = (glyph.z & kSolidFlag) != 0 ? 1.0 : SlugRender(texcoord, banding, glyph);
    // Premultiplied for the overlay's source-over blending.
    outColor = vec4(color.rgb * color.a, color.a) * coverage;
}
