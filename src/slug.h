#pragma once

// The glyph outlines come from font.h, embedded in the binary and the same on
// every platform.
//
// Font data for the Slug algorithm by Eric Lengyel, "GPU-Centered Font
// Rendering Directly from Glyph Outlines", JCGT 2017, which text.frag renders.
// Glyphs stay as their quadratic Bezier outlines; nothing is rasterized ahead
// of time, so text is exact at any size. The layout follows the reference
// code's README at https://github.com/EricLengyel/Slug, except that the curve
// and band data live in linear buffers instead of 4096-wide textures.
//
// Curve data: each texel is (x1, y1, x2, y2), the first two control points of
// one curve. The third control point is the xy of the next texel, which is
// also the start of the next curve along the contour.
//
// Band data: every glyph is cut into equal horizontal bands in y and vertical
// bands in x. At a glyph's band location sit its horizontal band headers, then
// its vertical band headers, each (curve count, offset from the location), and
// then the curve lists, each entry (curve texel, 0). A pixel only tests the
// curves of the one horizontal and one vertical band it falls in.

#include "src/core.h"
#include "src/font.h"

inline constexpr U32 kSlugFirstChar = 32;
inline constexpr U32 kSlugGlyphCount = 95; // printable ASCII
inline constexpr U32 kSlugMaxGlyphCurves = 512;
inline constexpr U32 kSlugMaxBands = 16;
inline constexpr U32 kSlugMaxCurveTexels = 16384;
inline constexpr U32 kSlugMaxBandEntries = 32768;
// Bands overlap by this much in em space, as the reference recommends.
inline constexpr F32 kSlugBandEpsilon = 1.0f / 1024.0f;

struct SlugGlyph {
    F32 minX, minY, maxX, maxY; // em-space bounds of the control points
    F32 advance; // em
    // Band index = em coordinate * scale + offset, for x (vertical bands)
    // and y (horizontal bands).
    F32 bandScale[2];
    F32 bandOffset[2];
    U32 bandLocation; // first header in SlugFont::bands
    U32 bandMaxX; // vertical band count minus one
    U32 bandMaxY; // horizontal band count minus one
    U32 curveCount; // zero for blank glyphs such as space
};

struct SlugFont {
    F32 curves[kSlugMaxCurveTexels][4];
    U32 bands[kSlugMaxBandEntries][2];
    SlugGlyph glyphs[kSlugGlyphCount];
    F32 ascent; // em, from the top of the line to the baseline
    F32 capHeight; // em, the height of 'H'
    U32 curveTexelCount;
    U32 bandEntryCount;
};

// Bounds of one curve of the glyph being built, for band assignment.
struct SlugCurveBounds {
    U32 texel;
    F32 minX, minY, maxX, maxY;
};

struct SlugGlyphBuilder {
    SlugCurveBounds curves[kSlugMaxGlyphCurves];
    U32 curveCount;
    F32 penX, penY; // end of the last curve, the start of the next
};

inline void SlugBeginContour(SlugFont &font, SlugGlyphBuilder &builder, const F32 x, const F32 y) {
    if (font.curveTexelCount + 1 > kSlugMaxCurveTexels) {
        Die("Slug curve storage", font.curveTexelCount);
    }
    F32 (&texel)[4] = font.curves[font.curveTexelCount];
    texel[0] = x;
    texel[1] = y;
    texel[2] = 0.0f;
    texel[3] = 0.0f;
    builder.penX = x;
    builder.penY = y;
}

// Appends the quadratic curve from the pen through (x2, y2) to (x3, y3). The
// current texel already holds the pen as p1; p3 starts a new texel.
inline void SlugAddCurve(SlugFont &font, SlugGlyphBuilder &builder, const F32 x2, const F32 y2, const F32 x3,
                         const F32 y3) {
    const F32 x1 = builder.penX;
    const F32 y1 = builder.penY;
    if (x1 == x3 && y1 == y3 && x1 == x2 && y1 == y2) {
        return; // a point contributes nothing
    }
    if (builder.curveCount == kSlugMaxGlyphCurves || font.curveTexelCount + 2 > kSlugMaxCurveTexels) {
        Die("Slug glyph curves", builder.curveCount);
    }
    const U32 texel = font.curveTexelCount;
    font.curves[texel][2] = x2;
    font.curves[texel][3] = y2;
    builder.curves[builder.curveCount++] = {
        .texel = texel,
        .minX = Min(Min(x1, x2), x3),
        .minY = Min(Min(y1, y2), y3),
        .maxX = Max(Max(x1, x2), x3),
        .maxY = Max(Max(y1, y2), y3),
    };
    font.curveTexelCount += 1;
    F32 (&next)[4] = font.curves[font.curveTexelCount];
    next[0] = x3;
    next[1] = y3;
    next[2] = 0.0f;
    next[3] = 0.0f;
    builder.penX = x3;
    builder.penY = y3;
}

// The final texel of a contour holds only its last end point.
inline void SlugEndContour(SlugFont &font) {
    font.curveTexelCount += 1;
}

inline bool SlugOverlaps(const F32 curveMin, const F32 curveMax, const F32 bandMin, const F32 bandMax) {
    return curveMax >= bandMin - kSlugBandEpsilon && curveMin <= bandMax + kSlugBandEpsilon;
}

// Straight lines along a band's direction never cross a ray parallel to them,
// so the reference leaves them out of those bands. A curve is such a line
// exactly when its extent across the band is zero.
inline bool SlugInHorizontalBand(const SlugCurveBounds &curve, const F32 bandMin, const F32 bandMax) {
    return curve.minY != curve.maxY && SlugOverlaps(curve.minY, curve.maxY, bandMin, bandMax);
}

inline bool SlugInVerticalBand(const SlugCurveBounds &curve, const F32 bandMin, const F32 bandMax) {
    return curve.minX != curve.maxX && SlugOverlaps(curve.minX, curve.maxX, bandMin, bandMax);
}

// The band count that minimizes the most curves in any one band, preferring
// fewer bands on ties.
inline U32 SlugChooseBandCount(const SlugGlyphBuilder &builder, const F32 low, const F32 high, const bool horizontal) {
    U32 bestCount = 1;
    U32 bestLoad = ~0u;
    for (U32 count = 1; count <= kSlugMaxBands; ++count) {
        const F32 thickness = (high - low) / static_cast<F32>(count);
        U32 load = 0;
        for (U32 band = 0; band < count; ++band) {
            const F32 bandMin = low + thickness * static_cast<F32>(band);
            const F32 bandMax = bandMin + thickness;
            U32 inBand = 0;
            for (U32 i = 0; i < builder.curveCount; ++i) {
                const SlugCurveBounds &curve = builder.curves[i];
                inBand += (horizontal
                               ? SlugInHorizontalBand(curve, bandMin, bandMax)
                               : SlugInVerticalBand(curve, bandMin, bandMax))
                              ? 1
                              : 0;
            }
            load = Max(load, inBand);
        }
        if (load < bestLoad) {
            bestLoad = load;
            bestCount = count;
        }
    }
    return bestCount;
}

// Writes one band's curve list, sorted in descending order of the coordinate
// the ray runs along so the shader can stop at the first curve behind it.
inline void SlugWriteBand(SlugFont &font, const SlugGlyphBuilder &builder, const U32 location, const U32 header,
                          const F32 bandMin, const F32 bandMax, const bool horizontal) {
    U16 members[kSlugMaxGlyphCurves];
    U32 count = 0;
    for (U32 i = 0; i < builder.curveCount; ++i) {
        const SlugCurveBounds &curve = builder.curves[i];
        if (horizontal ? SlugInHorizontalBand(curve, bandMin, bandMax) : SlugInVerticalBand(curve, bandMin, bandMax)) {
            members[count++] = static_cast<U16>(i);
        }
    }
    // Insertion sort; bands hold at most a few dozen curves.
    for (U32 i = 1; i < count; ++i) {
        const U16 member = members[i];
        const F32 key = horizontal ? builder.curves[member].maxX : builder.curves[member].maxY;
        U32 j = i;
        for (; j > 0; --j) {
            const SlugCurveBounds &previous = builder.curves[members[j - 1]];
            if ((horizontal ? previous.maxX : previous.maxY) >= key) {
                break;
            }
            members[j] = members[j - 1];
        }
        members[j] = member;
    }
    if (font.bandEntryCount + count > kSlugMaxBandEntries) {
        Die("Slug band storage", font.bandEntryCount);
    }
    font.bands[header][0] = count;
    font.bands[header][1] = font.bandEntryCount - location;
    for (U32 i = 0; i < count; ++i) {
        font.bands[font.bandEntryCount][0] = builder.curves[members[i]].texel;
        font.bands[font.bandEntryCount][1] = 0;
        font.bandEntryCount += 1;
    }
}

inline void SlugBuildBands(SlugFont &font, SlugGlyph &glyph, const SlugGlyphBuilder &builder) {
    const U32 horizontalCount = SlugChooseBandCount(builder, glyph.minY, glyph.maxY, true);
    const U32 verticalCount = SlugChooseBandCount(builder, glyph.minX, glyph.maxX, false);
    const U32 location = font.bandEntryCount;
    if (location + horizontalCount + verticalCount > kSlugMaxBandEntries) {
        Die("Slug band storage", location);
    }
    font.bandEntryCount += horizontalCount + verticalCount;

    const F32 height = glyph.maxY - glyph.minY;
    const F32 width = glyph.maxX - glyph.minX;
    for (U32 band = 0; band < horizontalCount; ++band) {
        const F32 bandMin = glyph.minY + height * static_cast<F32>(band) / static_cast<F32>(horizontalCount);
        SlugWriteBand(font, builder, location, location + band, bandMin,
                      bandMin + height / static_cast<F32>(horizontalCount), true);
    }
    for (U32 band = 0; band < verticalCount; ++band) {
        const F32 bandMin = glyph.minX + width * static_cast<F32>(band) / static_cast<F32>(verticalCount);
        SlugWriteBand(font, builder, location, location + horizontalCount + band, bandMin,
                      bandMin + width / static_cast<F32>(verticalCount), false);
    }

    glyph.bandLocation = location;
    glyph.bandMaxX = verticalCount - 1;
    glyph.bandMaxY = horizontalCount - 1;
    glyph.bandScale[0] = width > 0.0f ? static_cast<F32>(verticalCount) / width : 0.0f;
    glyph.bandScale[1] = height > 0.0f ? static_cast<F32>(horizontalCount) / height : 0.0f;
    glyph.bandOffset[0] = -glyph.minX * glyph.bandScale[0];
    glyph.bandOffset[1] = -glyph.minY * glyph.bandScale[1];
}

// Fills in a glyph from the outline in the builder.
inline void SlugFinishGlyph(SlugFont &font, SlugGlyph &glyph, const SlugGlyphBuilder &builder, const F32 advance) {
    glyph = {};
    glyph.advance = advance;
    glyph.curveCount = builder.curveCount;
    if (builder.curveCount == 0) {
        return;
    }
    // A quadratic curve lies inside the hull of its control points.
    glyph.minX = glyph.minY = kPosInf;
    glyph.maxX = glyph.maxY = -kPosInf;
    for (U32 c = 0; c < builder.curveCount; ++c) {
        glyph.minX = Min(glyph.minX, builder.curves[c].minX);
        glyph.minY = Min(glyph.minY, builder.curves[c].minY);
        glyph.maxX = Max(glyph.maxX, builder.curves[c].maxX);
        glyph.maxY = Max(glyph.maxY, builder.curves[c].maxY);
    }
    SlugBuildBands(font, glyph, builder);
}

// Working memory for BuildSlugFont, owned by the caller.
struct SlugScratch {
    SlugGlyphBuilder builder;
};

// Builds the curve and band data for printable ASCII from the outlines
// embedded in font.h, in em units with the baseline at y = 0 and y up.
inline void BuildSlugFont(SlugFont &font, SlugScratch &scratch) {
    static_assert(kFontFirstCharacter == kSlugFirstChar && kFontGlyphCount == kSlugGlyphCount);
    constexpr F32 kEmPerUnit = 1.0f / kFontHalfUnitsPerEm;
    font.ascent = static_cast<F32>(kFontAscent) * kEmPerUnit;
    SlugGlyphBuilder &builder = scratch.builder;
    for (U32 i = 0; i < kSlugGlyphCount; ++i) {
        builder.curveCount = 0;
        for (U32 contour = kFontGlyphContours[i]; contour < kFontGlyphContours[i + 1]; ++contour) {
            const I16 (&start)[2] = kFontContourStarts[contour];
            SlugBeginContour(font, builder, static_cast<F32>(start[0]) * kEmPerUnit,
                             static_cast<F32>(start[1]) * kEmPerUnit);
            for (U32 curve = kFontContourCurves[contour]; curve < kFontContourCurves[contour + 1]; ++curve) {
                const I16 (&points)[4] = kFontCurves[curve];
                SlugAddCurve(font, builder, static_cast<F32>(points[0]) * kEmPerUnit,
                             static_cast<F32>(points[1]) * kEmPerUnit, static_cast<F32>(points[2]) * kEmPerUnit,
                             static_cast<F32>(points[3]) * kEmPerUnit);
            }
            SlugEndContour(font);
        }
        SlugFinishGlyph(font, font.glyphs[i], builder, static_cast<F32>(kFontAdvances[i]) * kEmPerUnit);
    }
    font.capHeight = font.glyphs['H' - kSlugFirstChar].maxY;
}
