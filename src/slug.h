#pragma once

// Included after the platform headers: GDI on Windows and Core Text on macOS
// supply the glyph outlines.
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

inline constexpr U32 kSlugFirstChar = 32;
inline constexpr U32 kSlugGlyphCount = 95; // printable ASCII
// Outlines are requested at one unit per design unit of Consolas or Menlo.
inline constexpr I32 kSlugOutlineEm = 2048;
inline constexpr U32 kSlugOutlineBytes = 16384;
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
    F32 *const texel = font.curves[font.curveTexelCount];
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
    F32 *const next = font.curves[font.curveTexelCount];
    next[0] = x3;
    next[1] = y3;
    next[2] = 0.0f;
    next[3] = 0.0f;
    builder.penX = x3;
    builder.penY = y3;
}

// The reference README's encoding of a straight line: duplicate the end point.
inline void SlugAddLine(SlugFont &font, SlugGlyphBuilder &builder, const F32 x, const F32 y) {
    SlugAddCurve(font, builder, x, y, x, y);
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

#if defined(_WIN32)

inline F32 SlugFixedToEm(const FIXED value) {
    return (static_cast<F32>(value.value) + static_cast<F32>(value.fract) * (1.0f / 65536.0f)) /
           static_cast<F32>(kSlugOutlineEm);
}

// Decodes a GGO_NATIVE outline. TrueType contours are lines and quadratic
// B-splines; in a spline, the on-curve point between two consecutive
// off-curve points is implied at their midpoint.
inline void SlugDecodeOutline(SlugFont &font, SlugGlyphBuilder &builder, const U8 *const outline, const U32 size) {
    const U8 *cursor = outline;
    while (cursor < outline + size) {
        const TTPOLYGONHEADER *const header = reinterpret_cast<const TTPOLYGONHEADER *>(cursor);
        if (header->dwType != TT_POLYGON_TYPE) {
            Die("reading a glyph contour", header->dwType);
        }
        const F32 startX = SlugFixedToEm(header->pfxStart.x);
        const F32 startY = SlugFixedToEm(header->pfxStart.y);
        SlugBeginContour(font, builder, startX, startY);
        const U8 *record = cursor + sizeof(TTPOLYGONHEADER);
        const U8 *const contourEnd = cursor + header->cb;
        while (record < contourEnd) {
            const TTPOLYCURVE *const curve = reinterpret_cast<const TTPOLYCURVE *>(record);
            const POINTFX *const points = curve->apfx;
            if (curve->wType == TT_PRIM_LINE) {
                for (U32 i = 0; i < curve->cpfx; ++i) {
                    SlugAddLine(font, builder, SlugFixedToEm(points[i].x), SlugFixedToEm(points[i].y));
                }
            } else if (curve->wType == TT_PRIM_QSPLINE) {
                for (U32 i = 0; i + 1 < curve->cpfx; ++i) {
                    const F32 controlX = SlugFixedToEm(points[i].x);
                    const F32 controlY = SlugFixedToEm(points[i].y);
                    const F32 nextX = SlugFixedToEm(points[i + 1].x);
                    const F32 nextY = SlugFixedToEm(points[i + 1].y);
                    const bool last = i + 2 == curve->cpfx;
                    SlugAddCurve(font, builder, controlX, controlY, last ? nextX : 0.5f * (controlX + nextX),
                                 last ? nextY : 0.5f * (controlY + nextY));
                }
            } else {
                Die("reading a glyph curve (cubic outlines are unsupported)", curve->wType);
            }
            record += 2 * sizeof(WORD) + curve->cpfx * sizeof(POINTFX);
        }
        if (builder.penX != startX || builder.penY != startY) {
            SlugAddLine(font, builder, startX, startY);
        }
        SlugEndContour(font);
        cursor = contourEnd;
    }
}

// Extracts printable ASCII from Consolas as unhinted TrueType outlines in em
// units, with the baseline at y = 0 and y pointing up.
inline void BuildSlugFont(SlugFont &font) {
    const HDC dc = CreateCompatibleDC(nullptr);
    const HFONT handle = CreateFontW(-kSlugOutlineEm, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
                                     OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                     FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!dc || !handle) {
        Die("creating the outline font", GetLastError());
    }
    const HGDIOBJ previous = SelectObject(dc, handle);
    TEXTMETRICW metrics;
    if (!GetTextMetricsW(dc, &metrics)) {
        Die("reading font metrics", GetLastError());
    }
    font.ascent = static_cast<F32>(metrics.tmAscent) / static_cast<F32>(kSlugOutlineEm);

    constexpr MAT2 kIdentity = {.eM11 = {.fract = 0, .value = 1}, .eM22 = {.fract = 0, .value = 1}};
    static U8 outline[kSlugOutlineBytes];
    static SlugGlyphBuilder builder;
    for (U32 i = 0; i < kSlugGlyphCount; ++i) {
        const UINT character = kSlugFirstChar + i;
        GLYPHMETRICS glyphMetrics;
        const DWORD size = GetGlyphOutlineW(dc, character, GGO_NATIVE | GGO_UNHINTED, &glyphMetrics, 0, nullptr,
                                            &kIdentity);
        if (size == GDI_ERROR || size > kSlugOutlineBytes ||
            (size > 0 && GetGlyphOutlineW(dc, character, GGO_NATIVE | GGO_UNHINTED, &glyphMetrics, size, outline,
                                          &kIdentity) == GDI_ERROR)) {
            Die("reading a glyph outline", character);
        }
        builder.curveCount = 0;
        SlugDecodeOutline(font, builder, outline, size);

        SlugFinishGlyph(font, font.glyphs[i], builder,
                        static_cast<F32>(glyphMetrics.gmCellIncX) / static_cast<F32>(kSlugOutlineEm));
    }
    // Measured from the outline: GDI's reported cap height for Consolas is a
    // placeholder of exactly half an em.
    font.capHeight = font.glyphs['H' - kSlugFirstChar].maxY;
    SelectObject(dc, previous);
    DeleteObject(handle);
    DeleteDC(dc);
}

#elif defined(__APPLE__)

// Core Text reports a glyph's path one element at a time; this carries the
// contour being built between calls.
struct SlugPathContext {
    SlugFont *font;
    SlugGlyphBuilder *builder;
    F32 startX, startY;
    bool open;
};

inline void SlugCloseContour(SlugPathContext &context) {
    if (!context.open) {
        return;
    }
    if (context.builder->penX != context.startX || context.builder->penY != context.startY) {
        SlugAddLine(*context.font, *context.builder, context.startX, context.startY);
    }
    SlugEndContour(*context.font);
    context.open = false;
}

// CGPathApply callback. TrueType outlines arrive as lines and quadratics with
// the implied on-curve points already made explicit.
inline void SlugAddPathElement(void *const info, const CGPathElement *const element) {
    SlugPathContext &context = *static_cast<SlugPathContext *>(info);
    const CGPoint *const p = element->points;
    constexpr F32 kEmPerUnit = 1.0f / static_cast<F32>(kSlugOutlineEm);
    if (element->type == kCGPathElementMoveToPoint) {
        SlugCloseContour(context);
        context.startX = static_cast<F32>(p[0].x) * kEmPerUnit;
        context.startY = static_cast<F32>(p[0].y) * kEmPerUnit;
        context.open = true;
        SlugBeginContour(*context.font, *context.builder, context.startX, context.startY);
    } else if (element->type == kCGPathElementAddLineToPoint) {
        SlugAddLine(*context.font, *context.builder, static_cast<F32>(p[0].x) * kEmPerUnit,
                    static_cast<F32>(p[0].y) * kEmPerUnit);
    } else if (element->type == kCGPathElementAddQuadCurveToPoint) {
        SlugAddCurve(*context.font, *context.builder, static_cast<F32>(p[0].x) * kEmPerUnit,
                     static_cast<F32>(p[0].y) * kEmPerUnit, static_cast<F32>(p[1].x) * kEmPerUnit,
                     static_cast<F32>(p[1].y) * kEmPerUnit);
    } else if (element->type == kCGPathElementCloseSubpath) {
        SlugCloseContour(context);
    } else {
        Die("reading a glyph curve (cubic outlines are unsupported)", element->type);
    }
}

// Extracts printable ASCII from Menlo as unhinted TrueType outlines in em
// units, with the baseline at y = 0 and y pointing up.
inline void BuildSlugFont(SlugFont &font) {
    const CTFontRef handle = CTFontCreateWithName(CFSTR("Menlo"), static_cast<CGFloat>(kSlugOutlineEm), nullptr);
    if (!handle) {
        Die("creating the outline font", 0);
    }
    font.ascent = static_cast<F32>(CTFontGetAscent(handle)) / static_cast<F32>(kSlugOutlineEm);

    static SlugGlyphBuilder builder;
    for (U32 i = 0; i < kSlugGlyphCount; ++i) {
        const UniChar character = static_cast<UniChar>(kSlugFirstChar + i);
        CGGlyph glyphIndex;
        if (!CTFontGetGlyphsForCharacters(handle, &character, &glyphIndex, 1)) {
            Die("finding a glyph", character);
        }
        builder.curveCount = 0;
        // Blank glyphs such as space have no path.
        const CGPathRef path = CTFontCreatePathForGlyph(handle, glyphIndex, nullptr);
        if (path) {
            SlugPathContext context = {.font = &font, .builder = &builder};
            CGPathApply(path, &context, SlugAddPathElement);
            SlugCloseContour(context);
            CGPathRelease(path);
        }
        CGSize advance;
        CTFontGetAdvancesForGlyphs(handle, kCTFontOrientationHorizontal, &glyphIndex, &advance, 1);
        SlugFinishGlyph(font, font.glyphs[i], builder,
                        static_cast<F32>(advance.width) / static_cast<F32>(kSlugOutlineEm));
    }
    font.capHeight = font.glyphs['H' - kSlugFirstChar].maxY;
    CFRelease(handle);
}

#endif
