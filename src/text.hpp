#pragma once

// Included after the shared Vulkan helpers in main.cpp, and after slug.hpp.
//
// Text is rendered with the Slug algorithm: glyph outlines are uploaded once
// as quadratic curves (see slug.hpp), and text.frag computes each pixel's
// coverage from them directly. Each character is one quad over its glyph's
// bounds. All storage is reserved at startup; the frame fence protects writes
// to the persistent vertex buffer.

constexpr U32 kTextMaxQuads = 4096;
// The reference README's tip for crisp text without hinting: size the font so
// capital letters are a whole number of pixels tall, and put the baseline on
// a pixel boundary, so their tops and bottoms land on the pixel grid.
constexpr F32 kTextCapHeightPixels = 12.0f; // UI pixels; rounded to whole physical pixels
// Panel layout, in UI pixels.
constexpr F32 kPanelMargin = 8.0f;
constexpr F32 kPanelPadding = 6.0f;
constexpr F32 kLineHeight = 24.0f;
constexpr F32 kFpsPanelWidth = 132.0f;
constexpr F32 kFpsPanelHeight = 32.0f;
constexpr F32 kHoverOffset = 16.0f; // from the cursor to the hover label
constexpr U32 kHoverLabelBytes = 256;
// Marks quads that text.frag fills solid, for panels. Bit 15 of the band
// maxima word, which Slug glyph data leaves zero.
constexpr U32 kTextSolidFlag = 0x8000;

// Matches TextVertex in text.vert, which mirrors the five attributes of the
// Slug reference vertex shader.
struct TextVertex {
    F32 pos[4]; // xy: position in pixels, zw: dilation normal
    F32 em[2]; // em-space sample coordinates
    U32 glyph[2]; // band location; band max x | band max y << 16 | flags
    F32 jac[4]; // inverse Jacobian, em per pixel
    F32 bnd[4]; // band scale x, y and band offset x, y
    F32 col[4]; // colour, straight alpha
};

// Matches the push_constant blocks in text.vert and text.frag.
struct TextPush {
    F32 viewport[2];
    VkDeviceAddress vertices;
    VkDeviceAddress curves;
    VkDeviceAddress bands;
};

static_assert(sizeof(TextVertex) == 80);
static_assert(offsetof(TextVertex, glyph) == 24);
static_assert(offsetof(TextVertex, col) == 64);
static_assert(sizeof(TextPush) == 32);
static_assert(offsetof(TextPush, curves) == 16);
static_assert(kTextMaxQuads * 4 <= 65536);

constexpr U64 kFontCurveBytes = sizeof(SlugFont::curves);
constexpr U64 kFontBandBytes = sizeof(SlugFont::bands);
constexpr U64 kFontBufferSize = kFontCurveBytes + kFontBandBytes;

// The host-visible text buffer: quad vertices and indices, then the staging
// area the font is uploaded from, laid out like the device-local font buffer.
struct TextHostBuffer {
    TextVertex quads[kTextMaxQuads][4];
    U16 quadIndices[kTextMaxQuads][6];
    F32 curves[kSlugMaxCurveTexels][4];
    U32 bands[kSlugMaxBandEntries][2];
};

constexpr U64 kTextIndexOffset = offsetof(TextHostBuffer, quadIndices);
constexpr U64 kTextUploadOffset = offsetof(TextHostBuffer, curves);
constexpr U64 kTextBufferSize = sizeof(TextHostBuffer);
static_assert(kTextUploadOffset % 16 == 0);
static_assert(offsetof(TextHostBuffer, bands) == kTextUploadOffset + kFontCurveBytes);
static_assert(kTextBufferSize == kTextUploadOffset + kFontBufferSize);

struct TextRenderer {
    // Host-visible, mapped for the program's lifetime.
    VkBuffer buffer;
    VkDeviceMemory bufferMemory;
    VkDeviceAddress address;
    TextHostBuffer *host;
    // Device-local: curve texels, then band entries.
    VkBuffer fontBuffer;
    VkDeviceMemory fontMemory;
    VkDeviceAddress curveAddress;
    VkDeviceAddress bandAddress;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    F32 pixelsPerEm;
    U32 quadCount;
    SlugFont font;
};

// Copies the font from the staging area into device-local memory, where every
// text pixel reads it many times per frame.
static void UploadFont(const Renderer &r, TextRenderer &text) {
    TextHostBuffer &host = *text.host;
    const U64 curveBytes = text.font.curveTexelCount * sizeof(text.font.curves[0]);
    const U64 bandBytes = text.font.bandEntryCount * sizeof(text.font.bands[0]);
    for (U32 i = 0; i < text.font.curveTexelCount; ++i) {
        for (U32 j = 0; j < 4; ++j) {
            host.curves[i][j] = text.font.curves[i][j];
        }
    }
    for (U32 i = 0; i < text.font.bandEntryCount; ++i) {
        host.bands[i][0] = text.font.bands[i][0];
        host.bands[i][1] = text.font.bands[i][1];
    }

    constexpr VkBufferCreateInfo kBufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = kFontBufferSize,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(r.device, &kBufferInfo, nullptr, &text.fontBuffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(r.device, text.fontBuffer, &requirements);
    constexpr VkMemoryAllocateFlagsInfo kFlags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const VkMemoryAllocateInfo memoryInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &kFlags,
        .allocationSize = requirements.size,
        .memoryTypeIndex = FindMemoryType(r, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VK_CHECK(vkAllocateMemory(r.device, &memoryInfo, nullptr, &text.fontMemory));
    VK_CHECK(vkBindBufferMemory(r.device, text.fontBuffer, text.fontMemory, 0));
    const VkBufferDeviceAddressInfo addressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = text.fontBuffer,
    };
    text.curveAddress = vkGetBufferDeviceAddress(r.device, &addressInfo);
    text.bandAddress = text.curveAddress + kFontCurveBytes;

    constexpr VkCommandBufferBeginInfo kBegin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r.cmd, &kBegin));
    const VkBufferCopy copies[] = {
        {.srcOffset = kTextUploadOffset, .dstOffset = 0, .size = curveBytes},
        {.srcOffset = kTextUploadOffset + kFontCurveBytes, .dstOffset = kFontCurveBytes, .size = bandBytes},
    };
    vkCmdCopyBuffer(r.cmd, text.buffer, text.fontBuffer, static_cast<U32>(Count(copies)), copies);
    // Make the copy visible to the shader reads of every later submission.
    constexpr VkMemoryBarrier2 kBarrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &kBarrier,
    };
    vkCmdPipelineBarrier2(r.cmd, &dependency);
    VK_CHECK(vkEndCommandBuffer(r.cmd));
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &r.cmd,
    };
    VK_CHECK(vkQueueSubmit(r.queue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(r.queue));
    VK_CHECK(vkResetCommandPool(r.device, r.pool, 0));
}

static F32 RoundPixels(const F32 value);

// Text and panel sizes are in UI pixels; the result is rounded to whole
// physical pixels so baselines, cap heights and panel edges stay on the grid.
static F32 UiPixels(const F32 value, const F32 uiScale) {
    return RoundPixels(value * uiScale);
}

static void InitText(const Renderer &r, TextRenderer &text, SlugScratch &fontScratch) {
    BuildSlugFont(text.font, fontScratch);

    constexpr VkBufferCreateInfo kBufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = kTextBufferSize,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(r.device, &kBufferInfo, nullptr, &text.buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(r.device, text.buffer, &requirements);
    constexpr VkMemoryAllocateFlagsInfo kFlags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const VkMemoryAllocateInfo bufferMemory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &kFlags,
        .allocationSize = requirements.size,
        .memoryTypeIndex = FindMemoryType(r, requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VK_CHECK(vkAllocateMemory(r.device, &bufferMemory, nullptr, &text.bufferMemory));
    VK_CHECK(vkBindBufferMemory(r.device, text.buffer, text.bufferMemory, 0));
    void *mapped;
    VK_CHECK(vkMapMemory(r.device, text.bufferMemory, 0, kTextBufferSize, 0, &mapped));
    text.host = static_cast<TextHostBuffer *>(mapped);
    const VkBufferDeviceAddressInfo addressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = text.buffer,
    };
    text.address = vkGetBufferDeviceAddress(r.device, &addressInfo);
    constexpr U16 kCorners[6] = {0, 1, 2, 2, 1, 3};
    for (U32 i = 0; i < kTextMaxQuads; ++i) {
        for (U32 j = 0; j < 6; ++j) {
            text.host->quadIndices[i][j] = static_cast<U16>(i * 4 + kCorners[j]);
        }
    }
    UploadFont(r, text);

    // Both stages read push constants: the vertex shader the vertex buffer,
    // the fragment shader the curve and band buffers.
    constexpr VkPushConstantRange kRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(TextPush),
    };
    const VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &kRange,
    };
    VK_CHECK(vkCreatePipelineLayout(r.device, &layoutInfo, nullptr, &text.layout));
    text.pipeline = CreatePipeline(r, kTextVert, sizeof(kTextVert), kTextFrag, sizeof(kTextFrag), text.layout, true);
}

static F32 RoundPixels(const F32 value) {
    const F32 shifted = value + 0.5f;
    const I32 whole = static_cast<I32>(shifted);
    return static_cast<F32>(static_cast<F32>(whole) > shifted ? whole - 1 : whole);
}

static U32 GlyphIndex(const U8 character) {
    return character >= kSlugFirstChar && character < kSlugFirstChar + kSlugGlyphCount
               ? character - kSlugFirstChar
               : '?' - kSlugFirstChar;
}

// Glyph advances are rounded to whole pixels, so every glyph starts on the
// pixel grid and monospaced columns stay aligned.
static F32 AdvancePixels(const SlugGlyph &glyph, const F32 pixelsPerEm) {
    return RoundPixels(glyph.advance * pixelsPerEm);
}

// One quad over the glyph's em-space bounds, origin at (penX, baseline) in
// pixels. Corners go top left, top right, bottom left, bottom right; text.vert
// dilates them outward by half a pixel along their normals.
static bool AppendGlyph(TextRenderer &text, const SlugGlyph &glyph, const F32 penX, const F32 baseline,
                        const F32 pixelsPerEm, const F32 (&color)[4]) {
    if (text.quadCount == kTextMaxQuads) {
        return false;
    }
    TextVertex (&vertices)[4] = text.host->quads[text.quadCount++];
    const F32 emPerPixel = 1.0f / pixelsPerEm;
    for (U32 corner = 0; corner < 4; ++corner) {
        const bool right = (corner & 1) != 0;
        const bool bottom = (corner & 2) != 0;
        const F32 emX = right ? glyph.maxX : glyph.minX;
        const F32 emY = bottom ? glyph.minY : glyph.maxY;
        vertices[corner] = {
            .pos = {
                penX + emX * pixelsPerEm,
                baseline - emY * pixelsPerEm,
                right ? 1.0f : -1.0f, bottom ? 1.0f : -1.0f
            },
            .em = {emX, emY},
            .glyph = {glyph.bandLocation, glyph.bandMaxX | (glyph.bandMaxY << 16)},
            // Em y points up and pixel y down.
            .jac = {emPerPixel, 0.0f, 0.0f, -emPerPixel},
            .bnd = {glyph.bandScale[0], glyph.bandScale[1], glyph.bandOffset[0], glyph.bandOffset[1]},
            .col = {color[0], color[1], color[2], color[3]},
        };
    }
    return true;
}

// A solid rectangle in the text batch, for panels behind text.
static bool AppendSolidQuad(TextRenderer &text, const F32 x, const F32 y, const F32 width, const F32 height,
                            const F32 (&color)[4]) {
    if (text.quadCount == kTextMaxQuads) {
        return false;
    }
    TextVertex (&vertices)[4] = text.host->quads[text.quadCount++];
    for (U32 corner = 0; corner < 4; ++corner) {
        vertices[corner] = {
            .pos = {(corner & 1) != 0 ? x + width : x, (corner & 2) != 0 ? y + height : y, 0.0f, 0.0f},
            .glyph = {0, kTextSolidFlag},
            .col = {color[0], color[1], color[2], color[3]},
        };
    }
    return true;
}

// (x, y) is the top left of the first line. Returns false on batch
// exhaustion. Unsupported bytes use a '?' glyph.
static bool AppendText(TextRenderer &text, const F32 uiScale, const char *string, const F32 x, const F32 y,
                       const F32 scale = 1.0f) {
    // Phosphor green, like the terminals of the era.
    constexpr F32 kTextColor[4] = {0.45f, 1.0f, 0.55f, 1.0f};
    const F32 pixelsPerEm = text.pixelsPerEm * scale;
    F32 penX = x;
    F32 lineTop = y;
    for (; *string != '\0'; ++string) {
        const U8 character = static_cast<U8>(*string);
        if (character == '\n') {
            penX = x;
            lineTop += UiPixels(kLineHeight, uiScale) * scale;
            continue;
        }
        const SlugGlyph &glyph = text.font.glyphs[GlyphIndex(character)];
        const F32 baseline = RoundPixels(lineTop + text.font.ascent * pixelsPerEm);
        if (glyph.curveCount > 0 && !AppendGlyph(text, glyph, penX, baseline, pixelsPerEm, kTextColor)) {
            return false;
        }
        penX += AdvancePixels(glyph, pixelsPerEm);
    }
    return true;
}

static void DestroyText(const Renderer &r, const TextRenderer &text) {
    vkDestroyPipeline(r.device, text.pipeline, nullptr);
    vkDestroyPipelineLayout(r.device, text.layout, nullptr);
    vkDestroyBuffer(r.device, text.fontBuffer, nullptr);
    vkFreeMemory(r.device, text.fontMemory, nullptr);
    vkUnmapMemory(r.device, text.bufferMemory);
    vkDestroyBuffer(r.device, text.buffer, nullptr);
    vkFreeMemory(r.device, text.bufferMemory, nullptr);
}

// Widest line of a string in pixels at scale 1, matching AppendText's layout.
static F32 TextWidth(const TextRenderer &text, const char *string) {
    F32 widest = 0.0f;
    F32 width = 0.0f;
    for (; *string != '\0'; ++string) {
        const U8 character = static_cast<U8>(*string);
        if (character == '\n') {
            widest = Max(widest, width);
            width = 0.0f;
            continue;
        }
        width += AdvancePixels(text.font.glyphs[GlyphIndex(character)], text.pixelsPerEm);
    }
    return Max(widest, width);
}

constexpr U32 kClockLines = 4;

static void AppendPanel(TextRenderer &text, const F32 uiScale, const F32 x, const F32 y, const F32 width,
                        const F32 height) {
    constexpr F32 kPanelColor[4] = {0.0f, 0.01f, 0.015f, 0.85f};
    constexpr F32 kOutlineColor[4] = {0.05f, 0.45f, 0.6f, 1.0f};
    AppendSolidQuad(text, x, y, width, height, kPanelColor);
    // A thin outline, top, bottom, left, right, inside the panel's bounds.
    const F32 t = UiPixels(1.0f, uiScale);
    AppendSolidQuad(text, x, y, width, t, kOutlineColor);
    AppendSolidQuad(text, x, y + height - t, width, t, kOutlineColor);
    AppendSolidQuad(text, x, y + t, t, height - 2.0f * t, kOutlineColor);
    AppendSolidQuad(text, x + width - t, y + t, t, height - 2.0f * t, kOutlineColor);
}

static void AppendClock(TextRenderer &text, const F32 uiScale, const VkExtent2D extent, const U64 utcTicks,
                        const F64 timeRate) {
    const I64 utc = UnixSecondsFromFileTime(utcTicks);
    const bool daylight = IsPacificDaylightTime(utc);
    const CivilTime pacific = CivilFromUnixSeconds(utc + (daylight ? kPacificDaylightOffset : kPacificStandardOffset));
    const CivilTime universal = CivilFromUnixSeconds(utc);
    const I64 gps = GpsSecondsFromUnix(utc);
    // The simulation rate, set with [ and ].
    char rate[16];
    if (timeRate == 0.0) {
        snprintf(rate, sizeof(rate), "paused");
    } else {
        snprintf(rate, sizeof(rate), "%gx", timeRate);
    }
    char label[160];
    snprintf(label, sizeof(label),
             "%s  %04d-%02u-%02u %02u:%02u:%02u\n"
             "UTC  %04d-%02u-%02u %02u:%02u:%02u\n"
             "GPS  %ld (week %ld, %ld s)\n"
             "RATE %s",
             daylight ? "PDT" : "PST",
             pacific.year, pacific.month, pacific.day, pacific.hour, pacific.minute, pacific.second,
             universal.year, universal.month, universal.day, universal.hour, universal.minute, universal.second,
             gps, gps / kSecondsPerWeek, gps % kSecondsPerWeek, rate);
    const F32 width = TextWidth(text, label) + 2.0f * UiPixels(kPanelPadding, uiScale);
    const F32 height = kClockLines * UiPixels(kLineHeight, uiScale) + 2.0f * UiPixels(kPanelPadding, uiScale);
    const F32 x = static_cast<F32>(extent.width) - UiPixels(kPanelMargin, uiScale) - width;
    AppendPanel(text, uiScale, x, UiPixels(kPanelMargin, uiScale), width, height);
    AppendText(text, uiScale, label, x + UiPixels(kPanelPadding, uiScale),
               UiPixels(kPanelMargin, uiScale) + UiPixels(kPanelPadding, uiScale));
}

// Writes a satellite's or station's ECEF state and WGS 84 position. Stations
// are fixed to the Earth, so they pass no velocity and get no velocity line.
// Latitude and longitude are signed degrees: negative is south or west.
static void FormatStateLabel(char (&label)[kHoverLabelBytes], const char *const name, const U32 index,
                             const Vec3d position, const Vec3d *const velocity) {
    const Geodetic geodetic = GeodeticFromEcef(position);
    char velocityLine[64] = "";
    if (velocity) {
        snprintf(velocityLine, sizeof(velocityLine), "ECEF vel %11.5f %11.5f %11.5f km/s\n",
                 velocity->x / 1000.0, velocity->y / 1000.0, velocity->z / 1000.0);
    }
    snprintf(label, sizeof(label),
             "%s #%u\n"
             "ECEF pos %11.3f %11.3f %11.3f km\n"
             "%s"
             "WGS84 lat %.6f  lon %.6f  alt %.3f km",
             name, index,
             position.x / 1000.0, position.y / 1000.0, position.z / 1000.0,
             velocityLine,
             geodetic.latitude * kDegreesPerRadian, geodetic.longitude * kDegreesPerRadian,
             geodetic.altitude / 1000.0);
}

// Names what the cursor is over, beside the cursor and kept inside the window.
// Links are named by their ends: their indices change every frame.
static void AppendHoverLabel(TextRenderer &text, const F32 uiScale, const VkExtent2D extent, const Input &input,
                             const Simulation &sim, const Hover hover) {
    if (hover.kind == kHoverNone) {
        return;
    }
    char label[kHoverLabelBytes];
    if (hover.kind == kHoverSatellite) {
        const OrbitState &state = sim.satelliteStates[hover.index];
        FormatStateLabel(label, "Satellite", hover.index, state.position, &state.velocity);
    } else if (hover.kind == kHoverStation) {
        FormatStateLabel(label, "Ground station", hover.index,
                         Support(sim.groundStations[hover.index].horizon).xyz, nullptr);
    } else {
        snprintf(label, sizeof(label), "Link: ground station #%u to satellite #%u",
                 static_cast<U32>(sim.links[hover.index].station),
                 static_cast<U32>(sim.links[hover.index].satellite));
    }
    U32 lines = 1;
    for (const char *c = label; *c != '\0'; ++c) {
        lines += *c == '\n' ? 1 : 0;
    }
    const F32 padding = UiPixels(kPanelPadding, uiScale);
    const F32 offset = UiPixels(kHoverOffset, uiScale);
    const F32 width = TextWidth(text, label) + 2.0f * padding;
    const F32 height = static_cast<F32>(lines) * UiPixels(kLineHeight, uiScale) + 2.0f * padding;
    const F32 mouseX = static_cast<F32>(input.mouseX);
    const F32 mouseY = static_cast<F32>(input.mouseY);
    const F32 x = mouseX + offset + width <= static_cast<F32>(extent.width)
                      ? mouseX + offset
                      : mouseX - offset - width;
    const F32 y = mouseY + offset + height <= static_cast<F32>(extent.height)
                      ? mouseY + offset
                      : mouseY - offset - height;
    AppendPanel(text, uiScale, x, y, width, height);
    AppendText(text, uiScale, label, x + padding, y + padding);
}

static void DrawOverlay(TextRenderer &text, const VkCommandBuffer cmd, const VkExtent2D extent, const Input &input,
                        const Simulation &sim, const Hover hover, const U32 displayedFps) {
    const F32 uiScale = input.uiScale;
    // The UI scale changes when the window moves to another monitor.
    text.pixelsPerEm = UiPixels(kTextCapHeightPixels, uiScale) / text.font.capHeight;
    text.quadCount = 0;
    char label[11];
    snprintf(label, sizeof(label), "FPS: %u", displayedFps);
    AppendPanel(text, uiScale, UiPixels(kPanelMargin, uiScale), UiPixels(kPanelMargin, uiScale),
                UiPixels(kFpsPanelWidth, uiScale), UiPixels(kFpsPanelHeight, uiScale));
    AppendText(text, uiScale, label,
               UiPixels(kPanelMargin, uiScale) + UiPixels(kPanelPadding, uiScale),
               UiPixels(kPanelMargin, uiScale) + UiPixels(kPanelPadding, uiScale));
    AppendClock(text, uiScale, extent, sim.utcTicks, TimeRate(input));
    AppendHoverLabel(text, uiScale, extent, input, sim, hover);
    const TextPush push = {
        .viewport = {static_cast<F32>(extent.width), static_cast<F32>(extent.height)},
        .vertices = text.address,
        .curves = text.curveAddress,
        .bands = text.bandAddress,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, text.pipeline);
    vkCmdPushConstants(cmd, text.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdBindIndexBuffer(cmd, text.buffer, kTextIndexOffset, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, text.quadCount * 6, 1, 0, 0, 0);
}
