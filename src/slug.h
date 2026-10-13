#pragma once

// Included after the platform headers: GDI on Windows and Core Text on macOS
// supply the glyph outlines, and on Linux they are read from a TrueType file.
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

// Working memory for BuildSlugFont, owned by the caller.
struct SlugScratch {
    U8 outline[kSlugOutlineBytes];
    SlugGlyphBuilder builder;
};

inline F32 SlugFixedToEm(const FIXED value) {
    return (static_cast<F32>(value.value) + static_cast<F32>(value.fract) * (1.0f / 65536.0f)) /
           static_cast<F32>(kSlugOutlineEm);
}

// Decodes a GGO_NATIVE outline. TrueType contours are lines and quadratic
// B-splines; in a spline, the on-curve point between two consecutive
// off-curve points is implied at their midpoint.
inline void SlugDecodeOutline(SlugFont &font, SlugGlyphBuilder &builder, const U8 (&outline)[kSlugOutlineBytes],
                              const U32 size) {
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
inline void BuildSlugFont(SlugFont &font, SlugScratch &scratch) {
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
    U8 (&outline)[kSlugOutlineBytes] = scratch.outline;
    SlugGlyphBuilder &builder = scratch.builder;
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

// Working memory for BuildSlugFont, owned by the caller.
struct SlugScratch {
    SlugGlyphBuilder builder;
};

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
inline void BuildSlugFont(SlugFont &font, SlugScratch &scratch) {
    const CTFontRef handle = CTFontCreateWithName(CFSTR("Menlo"), static_cast<CGFloat>(kSlugOutlineEm), nullptr);
    if (!handle) {
        Die("creating the outline font", 0);
    }
    font.ascent = static_cast<F32>(CTFontGetAscent(handle)) / static_cast<F32>(kSlugOutlineEm);

    SlugGlyphBuilder &builder = scratch.builder;
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

#elif defined(__linux__)

#include <cstdio>

// Linux has no system font API without third-party libraries, so the glyphs
// come straight from a TrueType file. Monospace fonts that distributions
// commonly install, tried in order.
inline constexpr char const *kSlugFontPaths[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/TTF/LiberationMono-Regular.ttf",
    "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
};
inline constexpr U32 kSlugMaxFontBytes = 1u << 22;
inline constexpr U32 kSlugMaxGlyphPoints = 1024;
inline constexpr U32 kSlugMaxComponentDepth = 4;
inline constexpr U32 kSlugTrueTypeVersion = 0x00010000;
inline constexpr U32 kSlugAppleTrueTypeVersion = 0x74727565; // 'true'

// Simple glyph point flags.
inline constexpr U8 kSlugOnCurve = 0x01;
inline constexpr U8 kSlugXShort = 0x02;
inline constexpr U8 kSlugYShort = 0x04;
inline constexpr U8 kSlugRepeat = 0x08;
inline constexpr U8 kSlugXSameOrPositive = 0x10;
inline constexpr U8 kSlugYSameOrPositive = 0x20;
// Composite glyph component flags.
inline constexpr U32 kSlugArgsAreWords = 0x0001;
inline constexpr U32 kSlugArgsAreOffsets = 0x0002;
inline constexpr U32 kSlugHaveScale = 0x0008;
inline constexpr U32 kSlugMoreComponents = 0x0020;
inline constexpr U32 kSlugHaveXYScale = 0x0040;
inline constexpr U32 kSlugHaveTwoByTwo = 0x0080;

// Byte offsets of the tables a glyph needs, into the font file.
struct SlugTrueType {
    const U8 (&file)[kSlugMaxFontBytes];
    U32 glyf, loca, hmtx, cmap; // cmap is the format 4 subtable
    U32 hMetricCount;
    bool longLoca;
};

// Font units to em, as x' = xx x + yx y + dx and y' = xy x + yy y + dy.
struct SlugTransform {
    F32 xx, xy, yx, yy, dx, dy;
};

// One simple glyph's points, read in font units and then moved to em.
struct SlugGlyphPoints {
    U8 flags[kSlugMaxGlyphPoints];
    F32 xs[kSlugMaxGlyphPoints];
    F32 ys[kSlugMaxGlyphPoints];
};

// Working memory for BuildSlugFont, owned by the caller.
struct SlugScratch {
    U8 file[kSlugMaxFontBytes];
    SlugGlyphPoints points;
    SlugGlyphBuilder builder;
};

inline U32 SlugReadU16(const U8 *const p) { return static_cast<U32>(p[0]) << 8 | p[1]; }
inline I32 SlugReadI16(const U8 *const p) { return static_cast<I16>(SlugReadU16(p)); }
inline U32 SlugReadU32(const U8 *const p) { return SlugReadU16(p) << 16 | SlugReadU16(p + 2); }

inline U32 SlugFindTable(const U8 (&file)[kSlugMaxFontBytes], const U32 size, const char (&tag)[5]) {
    const U32 tableCount = SlugReadU16(file + 4);
    if (12 + 16 * tableCount > size) {
        Die("reading the font's table directory", tableCount);
    }
    for (U32 i = 0; i < tableCount; ++i) {
        const U8 *const record = file + 12 + 16 * i;
        if (record[0] == tag[0] && record[1] == tag[1] && record[2] == tag[2] && record[3] == tag[3]) {
            const U32 offset = SlugReadU32(record + 8);
            if (offset > size || SlugReadU32(record + 12) > size - offset) {
                Die("reading a font table", i);
            }
            return offset;
        }
    }
    Die("finding a font table", SlugReadU32(reinterpret_cast<const U8 *>(tag)));
}

// The Unicode BMP mapping, which covers ASCII.
inline U32 SlugFindCharacterMap(const U8 (&file)[kSlugMaxFontBytes], const U32 cmap) {
    const U32 recordCount = SlugReadU16(file + cmap + 2);
    for (U32 i = 0; i < recordCount; ++i) {
        const U8 *const record = file + cmap + 4 + 8 * i;
        const U32 platform = SlugReadU16(record);
        const U32 encoding = SlugReadU16(record + 2);
        const U32 subtable = cmap + SlugReadU32(record + 4);
        const bool unicode = platform == 0 || (platform == 3 && encoding == 1);
        if (unicode && SlugReadU16(file + subtable) == 4) {
            return subtable;
        }
    }
    Die("finding a Unicode character map", recordCount);
}

// Format 4 maps sorted ranges of characters, each by a delta or through an
// array of glyph indices.
inline U32 SlugGlyphIndex(const SlugTrueType &font, const U32 character) {
    const U8 *const subtable = font.file + font.cmap;
    const U32 segmentBytes = SlugReadU16(subtable + 6);
    const U8 *const endCodes = subtable + 14;
    const U8 *const startCodes = endCodes + segmentBytes + 2;
    const U8 *const deltas = startCodes + segmentBytes;
    const U8 *const rangeOffsets = deltas + segmentBytes;
    for (U32 i = 0; i < segmentBytes; i += 2) {
        if (SlugReadU16(endCodes + i) < character) {
            continue;
        }
        const U32 start = SlugReadU16(startCodes + i);
        if (start > character) {
            return 0;
        }
        const U32 delta = SlugReadU16(deltas + i);
        const U32 rangeOffset = SlugReadU16(rangeOffsets + i);
        if (rangeOffset == 0) {
            return (character + delta) & 0xFFFF;
        }
        const U32 glyph = SlugReadU16(rangeOffsets + i + rangeOffset + 2 * (character - start));
        return glyph == 0 ? 0 : (glyph + delta) & 0xFFFF;
    }
    return 0;
}

inline U32 SlugGlyphOffset(const SlugTrueType &font, const U32 glyph) {
    const U8 *const loca = font.file + font.loca;
    return font.longLoca ? SlugReadU32(loca + 4 * glyph) : 2 * SlugReadU16(loca + 2 * glyph);
}

// One closed contour of on-curve and off-curve points. Between two
// consecutive off-curve points the on-curve point is implied at their
// midpoint, so the contour starts on a real on-curve point at either end, or
// else on the implied one between the ends.
inline void SlugDecodeContour(SlugFont &font, SlugGlyphBuilder &builder, const SlugGlyphPoints &points,
                              const U32 first, const U32 last) {
    const U8 (&flags)[kSlugMaxGlyphPoints] = points.flags;
    const F32 (&xs)[kSlugMaxGlyphPoints] = points.xs;
    const F32 (&ys)[kSlugMaxGlyphPoints] = points.ys;
    const bool firstOn = (flags[first] & kSlugOnCurve) != 0;
    const bool lastOn = (flags[last] & kSlugOnCurve) != 0;
    const U32 start = firstOn ? first : last;
    const F32 startX = firstOn || lastOn ? xs[start] : 0.5f * (xs[first] + xs[last]);
    const F32 startY = firstOn || lastOn ? ys[start] : 0.5f * (ys[first] + ys[last]);
    const U32 begin = firstOn ? first + 1 : first;
    const U32 end = !firstOn && lastOn ? last : last + 1;
    SlugBeginContour(font, builder, startX, startY);
    bool pending = false;
    F32 controlX = 0.0f;
    F32 controlY = 0.0f;
    for (U32 i = begin; i < end; ++i) {
        if ((flags[i] & kSlugOnCurve) != 0) {
            if (pending) {
                SlugAddCurve(font, builder, controlX, controlY, xs[i], ys[i]);
            } else {
                SlugAddLine(font, builder, xs[i], ys[i]);
            }
            pending = false;
        } else {
            if (pending) {
                SlugAddCurve(font, builder, controlX, controlY, 0.5f * (controlX + xs[i]),
                             0.5f * (controlY + ys[i]));
            }
            controlX = xs[i];
            controlY = ys[i];
            pending = true;
        }
    }
    if (pending) {
        SlugAddCurve(font, builder, controlX, controlY, startX, startY);
    } else if (builder.penX != startX || builder.penY != startY) {
        SlugAddLine(font, builder, startX, startY);
    }
    SlugEndContour(font);
}

// Adds a glyph's contours. Composite glyphs place other glyphs, each under
// its own transform, one after another through the same points.
inline void SlugDecodeGlyph(SlugFont &font, SlugGlyphBuilder &builder, SlugGlyphPoints &points,
                            const SlugTrueType &trueType, const U32 glyph, const SlugTransform &transform,
                            const U32 depth) {
    const U32 offset = SlugGlyphOffset(trueType, glyph);
    if (offset == SlugGlyphOffset(trueType, glyph + 1)) {
        return; // blank, such as space
    }
    const U8 *const data = trueType.file + trueType.glyf + offset;
    const I32 contourCount = SlugReadI16(data);
    if (contourCount < 0) {
        if (depth == kSlugMaxComponentDepth) {
            Die("reading a composite glyph (nested too deep)", glyph);
        }
        const U8 *cursor = data + 10;
        U32 flags = kSlugMoreComponents;
        while ((flags & kSlugMoreComponents) != 0) {
            flags = SlugReadU16(cursor);
            const U32 component = SlugReadU16(cursor + 2);
            cursor += 4;
            if ((flags & kSlugArgsAreOffsets) == 0) {
                Die("reading a composite glyph (point-matched components are unsupported)", glyph);
            }
            const bool words = (flags & kSlugArgsAreWords) != 0;
            const F32 dx = static_cast<F32>(words ? SlugReadI16(cursor) : static_cast<I8>(cursor[0]));
            const F32 dy = static_cast<F32>(words ? SlugReadI16(cursor + 2) : static_cast<I8>(cursor[1]));
            cursor += words ? 4 : 2;
            // Scales are 2.14 fixed point.
            constexpr F32 kUnitsPerScale = 1.0f / 16384.0f;
            F32 xx = 1.0f, xy = 0.0f, yx = 0.0f, yy = 1.0f;
            if ((flags & kSlugHaveScale) != 0) {
                xx = yy = static_cast<F32>(SlugReadI16(cursor)) * kUnitsPerScale;
                cursor += 2;
            } else if ((flags & kSlugHaveXYScale) != 0) {
                xx = static_cast<F32>(SlugReadI16(cursor)) * kUnitsPerScale;
                yy = static_cast<F32>(SlugReadI16(cursor + 2)) * kUnitsPerScale;
                cursor += 4;
            } else if ((flags & kSlugHaveTwoByTwo) != 0) {
                xx = static_cast<F32>(SlugReadI16(cursor)) * kUnitsPerScale;
                xy = static_cast<F32>(SlugReadI16(cursor + 2)) * kUnitsPerScale;
                yx = static_cast<F32>(SlugReadI16(cursor + 4)) * kUnitsPerScale;
                yy = static_cast<F32>(SlugReadI16(cursor + 6)) * kUnitsPerScale;
                cursor += 8;
            }
            const SlugTransform &t = transform;
            const SlugTransform combined = {
                .xx = t.xx * xx + t.yx * xy,
                .xy = t.xy * xx + t.yy * xy,
                .yx = t.xx * yx + t.yx * yy,
                .yy = t.xy * yx + t.yy * yy,
                .dx = t.xx * dx + t.yx * dy + t.dx,
                .dy = t.xy * dx + t.yy * dy + t.dy,
            };
            SlugDecodeGlyph(font, builder, points, trueType, component, combined, depth + 1);
        }
        return;
    }
    if (contourCount == 0) {
        return;
    }

    const U8 *const endPoints = data + 10;
    const U32 pointCount = SlugReadU16(endPoints + 2 * (contourCount - 1)) + 1;
    if (pointCount > kSlugMaxGlyphPoints) {
        Die("reading a glyph (too many points)", pointCount);
    }
    const U8 *cursor = endPoints + 2 * contourCount;
    cursor += 2 + SlugReadU16(cursor); // hinting instructions
    U8 (&flags)[kSlugMaxGlyphPoints] = points.flags;
    for (U32 i = 0; i < pointCount;) {
        const U8 flag = *cursor++;
        const U32 repeats = (flag & kSlugRepeat) != 0 ? *cursor++ : 0;
        for (U32 r = 0; r <= repeats && i < pointCount; ++r) {
            flags[i++] = flag;
        }
    }
    // Coordinates are deltas from the previous point: a byte with the sign in
    // the flags, the same as before, or a signed word.
    I32 x = 0;
    for (U32 i = 0; i < pointCount; ++i) {
        if ((flags[i] & kSlugXShort) != 0) {
            x += (flags[i] & kSlugXSameOrPositive) != 0 ? *cursor : -static_cast<I32>(*cursor);
            cursor += 1;
        } else if ((flags[i] & kSlugXSameOrPositive) == 0) {
            x += SlugReadI16(cursor);
            cursor += 2;
        }
        points.xs[i] = static_cast<F32>(x);
    }
    I32 y = 0;
    for (U32 i = 0; i < pointCount; ++i) {
        if ((flags[i] & kSlugYShort) != 0) {
            y += (flags[i] & kSlugYSameOrPositive) != 0 ? *cursor : -static_cast<I32>(*cursor);
            cursor += 1;
        } else if ((flags[i] & kSlugYSameOrPositive) == 0) {
            y += SlugReadI16(cursor);
            cursor += 2;
        }
        points.ys[i] = static_cast<F32>(y);
    }
    for (U32 i = 0; i < pointCount; ++i) {
        const F32 fx = points.xs[i];
        const F32 fy = points.ys[i];
        points.xs[i] = transform.xx * fx + transform.yx * fy + transform.dx;
        points.ys[i] = transform.xy * fx + transform.yy * fy + transform.dy;
    }

    U32 first = 0;
    for (I32 c = 0; c < contourCount; ++c) {
        const U32 last = SlugReadU16(endPoints + 2 * c);
        if (last < first || last >= pointCount) {
            Die("reading a glyph contour", c);
        }
        SlugDecodeContour(font, builder, points, first, last);
        first = last + 1;
    }
}

// Extracts printable ASCII from the first font found as unhinted TrueType
// outlines in em units, with the baseline at y = 0 and y pointing up.
inline void BuildSlugFont(SlugFont &font, SlugScratch &scratch) {
    U8 (&file)[kSlugMaxFontBytes] = scratch.file;
    FILE *stream = nullptr;
    for (U32 i = 0; i < Count(kSlugFontPaths) && !stream; ++i) {
        stream = fopen(kSlugFontPaths[i], "rb");
    }
    if (!stream) {
        Die("opening a monospace TrueType font (install DejaVu Sans Mono)", 0);
    }
    const U64 size = fread(file, 1, sizeof(file), stream);
    fclose(stream);
    if (size < 12 || size == sizeof(file)) {
        Die("reading the font file", static_cast<I64>(size));
    }
    const U32 version = SlugReadU32(file);
    if (version != kSlugTrueTypeVersion && version != kSlugAppleTrueTypeVersion) {
        Die("reading the font file (only TrueType outlines are supported)", version);
    }

    const U32 fileSize = static_cast<U32>(size);
    const U32 head = SlugFindTable(file, fileSize, "head");
    const U32 hhea = SlugFindTable(file, fileSize, "hhea");
    const SlugTrueType trueType = {
        .file = file,
        .glyf = SlugFindTable(file, fileSize, "glyf"),
        .loca = SlugFindTable(file, fileSize, "loca"),
        .hmtx = SlugFindTable(file, fileSize, "hmtx"),
        .cmap = SlugFindCharacterMap(file, SlugFindTable(file, fileSize, "cmap")),
        .hMetricCount = SlugReadU16(file + hhea + 34),
        .longLoca = SlugReadI16(file + head + 50) != 0,
    };
    const F32 emPerUnit = 1.0f / static_cast<F32>(SlugReadU16(file + head + 18));
    font.ascent = static_cast<F32>(SlugReadI16(file + hhea + 4)) * emPerUnit;

    const SlugTransform toEm = {.xx = emPerUnit, .yy = emPerUnit};
    SlugGlyphBuilder &builder = scratch.builder;
    for (U32 i = 0; i < kSlugGlyphCount; ++i) {
        const U32 character = kSlugFirstChar + i;
        const U32 glyph = SlugGlyphIndex(trueType, character);
        if (glyph == 0 && character != ' ') {
            Die("finding a glyph", character);
        }
        builder.curveCount = 0;
        SlugDecodeGlyph(font, builder, scratch.points, trueType, glyph, toEm, 0);
        // Glyphs past the last metric share its advance.
        const U32 metric = Min(glyph, trueType.hMetricCount - 1);
        SlugFinishGlyph(font, font.glyphs[i], builder,
                        static_cast<F32>(SlugReadU16(file + trueType.hmtx + 4 * metric)) * emPerUnit);
    }
    font.capHeight = font.glyphs['H' - kSlugFirstChar].maxY;
}

#endif
