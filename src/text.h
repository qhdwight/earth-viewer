#pragma once

// Included after the shared Vulkan helpers in main.cc, and after slug.h.
//
// Text is rendered with the Slug algorithm: glyph outlines are uploaded once
// as quadratic curves (see slug.h), and text.frag computes each pixel's
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
constexpr U64 kTextIndexOffset = kTextMaxQuads * 4 * sizeof(TextVertex);
constexpr U64 kTextUploadOffset = kTextIndexOffset + kTextMaxQuads * 6 * sizeof(U16);
constexpr U64 kTextBufferSize = kTextUploadOffset + kFontBufferSize;
static_assert(kTextUploadOffset % 16 == 0);

struct TextRenderer {
    // Host-visible: vertices, quad indices, then the font upload staging area.
    VkBuffer buffer;
    VkDeviceMemory bufferMemory;
    VkDeviceAddress address;
    TextVertex *vertices;
    // Device-local: curve texels, then band entries.
    VkBuffer fontBuffer;
    VkDeviceMemory fontMemory;
    VkDeviceAddress curveAddress;
    VkDeviceAddress bandAddress;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    F32 pixelsPerEm;
    U32 quadCount;
};

static TextRenderer textRenderer;
static SlugFont slugFont;

// Copies the font from the staging area into device-local memory, where every
// text pixel reads it many times per frame.
static void UploadFont(U8 *const staging) {
    const U64 curveBytes = slugFont.curveTexelCount * sizeof(slugFont.curves[0]);
    const U64 bandBytes = slugFont.bandEntryCount * sizeof(slugFont.bands[0]);
    F32 (*const curves)[4] = reinterpret_cast<F32 (*)[4]>(staging);
    for (U32 i = 0; i < slugFont.curveTexelCount; ++i) {
        for (U32 j = 0; j < 4; ++j) {
            curves[i][j] = slugFont.curves[i][j];
        }
    }
    U32 (*const bands)[2] = reinterpret_cast<U32 (*)[2]>(staging + kFontCurveBytes);
    for (U32 i = 0; i < slugFont.bandEntryCount; ++i) {
        bands[i][0] = slugFont.bands[i][0];
        bands[i][1] = slugFont.bands[i][1];
    }

    const VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = kFontBufferSize,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(r.device, &bufferInfo, nullptr, &textRenderer.fontBuffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(r.device, textRenderer.fontBuffer, &requirements);
    const VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const VkMemoryAllocateInfo memoryInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = requirements.size,
        .memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VK_CHECK(vkAllocateMemory(r.device, &memoryInfo, nullptr, &textRenderer.fontMemory));
    VK_CHECK(vkBindBufferMemory(r.device, textRenderer.fontBuffer, textRenderer.fontMemory, 0));
    const VkBufferDeviceAddressInfo addressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = textRenderer.fontBuffer,
    };
    textRenderer.curveAddress = vkGetBufferDeviceAddress(r.device, &addressInfo);
    textRenderer.bandAddress = textRenderer.curveAddress + kFontCurveBytes;

    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r.cmd, &begin));
    const VkBufferCopy copies[] = {
        {.srcOffset = kTextUploadOffset, .dstOffset = 0, .size = curveBytes},
        {.srcOffset = kTextUploadOffset + kFontCurveBytes, .dstOffset = kFontCurveBytes, .size = bandBytes},
    };
    vkCmdCopyBuffer(r.cmd, textRenderer.buffer, textRenderer.fontBuffer, static_cast<U32>(Count(copies)), copies);
    // Make the copy visible to the shader reads of every later submission.
    const VkMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
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
static F32 UiPixels(const F32 value) {
    return RoundPixels(value * uiScale);
}

static void InitText() {
    BuildSlugFont(slugFont);

    const VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = kTextBufferSize,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(r.device, &bufferInfo, nullptr, &textRenderer.buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(r.device, textRenderer.buffer, &requirements);
    const VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const VkMemoryAllocateInfo bufferMemory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = requirements.size,
        .memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VK_CHECK(vkAllocateMemory(r.device, &bufferMemory, nullptr, &textRenderer.bufferMemory));
    VK_CHECK(vkBindBufferMemory(r.device, textRenderer.buffer, textRenderer.bufferMemory, 0));
    void *mapped;
    VK_CHECK(vkMapMemory(r.device, textRenderer.bufferMemory, 0, kTextBufferSize, 0, &mapped));
    textRenderer.vertices = static_cast<TextVertex *>(mapped);
    const VkBufferDeviceAddressInfo addressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = textRenderer.buffer,
    };
    textRenderer.address = vkGetBufferDeviceAddress(r.device, &addressInfo);
    U16 *const indices = reinterpret_cast<U16 *>(static_cast<U8 *>(mapped) + kTextIndexOffset);
    constexpr U16 kCorners[6] = {0, 1, 2, 2, 1, 3};
    for (U32 i = 0; i < kTextMaxQuads; ++i) {
        for (U32 j = 0; j < 6; ++j) {
            indices[i * 6 + j] = static_cast<U16>(i * 4 + kCorners[j]);
        }
    }
    UploadFont(static_cast<U8 *>(mapped) + kTextUploadOffset);

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
    VK_CHECK(vkCreatePipelineLayout(r.device, &layoutInfo, nullptr, &textRenderer.layout));
    textRenderer.pipeline = CreatePipeline(kTextVert, sizeof(kTextVert),
                                           kTextFrag, sizeof(kTextFrag), textRenderer.layout, true);
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

static TextVertex *NextTextQuad() {
    return textRenderer.quadCount == kTextMaxQuads ? nullptr : textRenderer.vertices + 4 * textRenderer.quadCount++;
}

// One quad over the glyph's em-space bounds, origin at (penX, baseline) in
// pixels. Corners go top left, top right, bottom left, bottom right; text.vert
// dilates them outward by half a pixel along their normals.
static bool AppendGlyph(const SlugGlyph &glyph, const F32 penX, const F32 baseline, const F32 pixelsPerEm,
                        const F32 (&color)[4]) {
    TextVertex *const vertices = NextTextQuad();
    if (!vertices) {
        return false;
    }
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
static bool AppendSolidQuad(const F32 x, const F32 y, const F32 width, const F32 height, const F32 (&color)[4]) {
    TextVertex *const vertices = NextTextQuad();
    if (!vertices) {
        return false;
    }
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
static bool AppendText(const char *string, const F32 x, const F32 y, const F32 scale = 1.0f) {
    // Phosphor green, like the terminals of the era.
    constexpr F32 kTextColor[4] = {0.45f, 1.0f, 0.55f, 1.0f};
    const F32 pixelsPerEm = textRenderer.pixelsPerEm * scale;
    F32 penX = x;
    F32 lineTop = y;
    for (; *string != '\0'; ++string) {
        const U8 character = static_cast<U8>(*string);
        if (character == '\n') {
            penX = x;
            lineTop += UiPixels(kLineHeight) * scale;
            continue;
        }
        const SlugGlyph &glyph = slugFont.glyphs[GlyphIndex(character)];
        const F32 baseline = RoundPixels(lineTop + slugFont.ascent * pixelsPerEm);
        if (glyph.curveCount > 0 && !AppendGlyph(glyph, penX, baseline, pixelsPerEm, kTextColor)) {
            return false;
        }
        penX += AdvancePixels(glyph, pixelsPerEm);
    }
    return true;
}

static void DestroyText() {
    vkDestroyPipeline(r.device, textRenderer.pipeline, nullptr);
    vkDestroyPipelineLayout(r.device, textRenderer.layout, nullptr);
    vkDestroyBuffer(r.device, textRenderer.fontBuffer, nullptr);
    vkFreeMemory(r.device, textRenderer.fontMemory, nullptr);
    vkUnmapMemory(r.device, textRenderer.bufferMemory);
    vkDestroyBuffer(r.device, textRenderer.buffer, nullptr);
    vkFreeMemory(r.device, textRenderer.bufferMemory, nullptr);
}

// Widest line of a string in pixels at scale 1, matching AppendText's layout.
static F32 TextWidth(const char *string) {
    F32 widest = 0.0f;
    F32 width = 0.0f;
    for (; *string != '\0'; ++string) {
        const U8 character = static_cast<U8>(*string);
        if (character == '\n') {
            widest = Max(widest, width);
            width = 0.0f;
            continue;
        }
        width += AdvancePixels(slugFont.glyphs[GlyphIndex(character)], textRenderer.pixelsPerEm);
    }
    return Max(widest, width);
}

constexpr U32 kClockLines = 4;

static void AppendPanel(const F32 x, const F32 y, const F32 width, const F32 height) {
    constexpr F32 kPanelColor[4] = {0.0f, 0.01f, 0.015f, 0.85f};
    constexpr F32 kOutlineColor[4] = {0.05f, 0.45f, 0.6f, 1.0f};
    AppendSolidQuad(x, y, width, height, kPanelColor);
    // A thin outline, top, bottom, left, right, inside the panel's bounds.
    const F32 t = UiPixels(1.0f);
    AppendSolidQuad(x, y, width, t, kOutlineColor);
    AppendSolidQuad(x, y + height - t, width, t, kOutlineColor);
    AppendSolidQuad(x, y + t, t, height - 2.0f * t, kOutlineColor);
    AppendSolidQuad(x + width - t, y + t, t, height - 2.0f * t, kOutlineColor);
}

static void AppendClock() {
    const I64 utc = UnixSecondsFromFileTime(frameUtcTicks);
    const bool daylight = IsPacificDaylightTime(utc);
    const CivilTime pacific = CivilFromUnixSeconds(utc + (daylight ? kPacificDaylightOffset : kPacificStandardOffset));
    const CivilTime universal = CivilFromUnixSeconds(utc);
    const I64 gps = GpsSecondsFromUnix(utc);
    // The simulation rate, set with [ and ].
    char rate[16];
    if (TimeRate() == 0.0) {
        snprintf(rate, sizeof(rate), "paused");
    } else {
        snprintf(rate, sizeof(rate), "%gx", TimeRate());
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
    const F32 width = TextWidth(label) + 2.0f * UiPixels(kPanelPadding);
    const F32 height = kClockLines * UiPixels(kLineHeight) + 2.0f * UiPixels(kPanelPadding);
    const F32 x = static_cast<F32>(r.extent.width) - UiPixels(kPanelMargin) - width;
    AppendPanel(x, UiPixels(kPanelMargin), width, height);
    AppendText(label, x + UiPixels(kPanelPadding), UiPixels(kPanelMargin) + UiPixels(kPanelPadding));
}

// Writes a satellite's or station's ECEF state and WGS 84 position. Stations
// are fixed to the Earth, so they pass no velocity and get no velocity line.
// Latitude and longitude are signed degrees: negative is south or west.
static void FormatStateLabel(char *const label, const U32 size, const char *const name, const U32 index,
                             const Vec3d position, const Vec3d *const velocity) {
    const Geodetic geodetic = GeodeticFromEcef(position);
    char velocityLine[64] = "";
    if (velocity) {
        snprintf(velocityLine, sizeof(velocityLine), "ECEF vel %11.5f %11.5f %11.5f km/s\n",
                 velocity->x / 1000.0, velocity->y / 1000.0, velocity->z / 1000.0);
    }
    snprintf(label, size,
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
static void AppendHoverLabel() {
    if (hover.kind == kHoverNone) {
        return;
    }
    char label[256];
    if (hover.kind == kHoverSatellite) {
        const OrbitState &state = satelliteStates[hover.index];
        FormatStateLabel(label, sizeof(label), "Satellite", hover.index, state.position, &state.velocity);
    } else if (hover.kind == kHoverStation) {
        FormatStateLabel(label, sizeof(label), "Ground station", hover.index,
                         Support(groundStations[hover.index].horizon).xyz, nullptr);
    } else {
        snprintf(label, sizeof(label), "Link: ground station #%u to satellite #%u",
                 static_cast<U32>(links[hover.index].station), static_cast<U32>(links[hover.index].satellite));
    }
    U32 lines = 1;
    for (const char *c = label; *c != '\0'; ++c) {
        lines += *c == '\n' ? 1 : 0;
    }
    const F32 padding = UiPixels(kPanelPadding);
    const F32 offset = UiPixels(kHoverOffset);
    const F32 width = TextWidth(label) + 2.0f * padding;
    const F32 height = static_cast<F32>(lines) * UiPixels(kLineHeight) + 2.0f * padding;
    const F32 mouseX = static_cast<F32>(input.mouseX);
    const F32 mouseY = static_cast<F32>(input.mouseY);
    const F32 x = mouseX + offset + width <= static_cast<F32>(r.extent.width)
                      ? mouseX + offset
                      : mouseX - offset - width;
    const F32 y = mouseY + offset + height <= static_cast<F32>(r.extent.height)
                      ? mouseY + offset
                      : mouseY - offset - height;
    AppendPanel(x, y, width, height);
    AppendText(label, x + padding, y + padding);
}

static void DrawOverlay() {
    // The UI scale changes when the window moves to another monitor.
    textRenderer.pixelsPerEm = UiPixels(kTextCapHeightPixels) / slugFont.capHeight;
    textRenderer.quadCount = 0;
    char label[11];
    snprintf(label, sizeof(label), "FPS: %u", displayedFps);
    AppendPanel(UiPixels(kPanelMargin), UiPixels(kPanelMargin), UiPixels(kFpsPanelWidth), UiPixels(kFpsPanelHeight));
    AppendText(label,
               UiPixels(kPanelMargin) + UiPixels(kPanelPadding),
               UiPixels(kPanelMargin) + UiPixels(kPanelPadding));
    AppendClock();
    AppendHoverLabel();
    const TextPush push = {
        .viewport = {static_cast<F32>(r.extent.width), static_cast<F32>(r.extent.height)},
        .vertices = textRenderer.address,
        .curves = textRenderer.curveAddress,
        .bands = textRenderer.bandAddress,
    };
    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, textRenderer.pipeline);
    vkCmdPushConstants(r.cmd, textRenderer.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdBindIndexBuffer(r.cmd, textRenderer.buffer, kTextIndexOffset, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(r.cmd, textRenderer.quadCount * 6, 1, 0, 0, 0);
}
