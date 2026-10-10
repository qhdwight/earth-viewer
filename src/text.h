#pragma once

// Included after the shared Vulkan helpers in main.cc. All storage is reserved
// at startup; the frame fence protects writes to the persistent vertex buffer.
constexpr U32 kTextMaxQuads = 4096;
constexpr U32 kFontFirst = 32;
constexpr U32 kFontCount = 95;
constexpr U32 kFontColumns = 16;
constexpr U32 kFontCellWidth = 24;
constexpr U32 kFontCellHeight = 32;
constexpr U32 kFontWidth = kFontColumns * kFontCellWidth;
constexpr U32 kFontHeight = 6 * kFontCellHeight;

struct TextVertex {
    F32 x, y, u, v;
    F32 color[4];
};
struct TextPush {
    F32 screenScale[2];
    VkDeviceAddress vertices;
};
static_assert(sizeof(TextVertex) == 32);
static_assert(offsetof(TextVertex, color) == 16);
static_assert(sizeof(TextPush) == 16);
static_assert(offsetof(TextPush, vertices) == 8);
static_assert(kTextMaxQuads * 4 <= 65536);
constexpr U64 kTextIndexOffset = kTextMaxQuads * 4 * sizeof(TextVertex);
constexpr U64 kTextUploadOffset = kTextIndexOffset + kTextMaxQuads * 6 * sizeof(U16);
constexpr U64 kTextBufferSize = kTextUploadOffset + kFontWidth * kFontHeight;

struct TextRenderer {
    VkBuffer buffer;
    VkDeviceMemory bufferMemory;
    VkDeviceAddress address;
    TextVertex *vertices;
    VkImage image;
    VkDeviceMemory imageMemory;
    VkImageView view;
    VkSampler sampler;
    VkDescriptorSetLayout descriptorLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptor;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    F32 advance[kFontCount];
    U32 quadCount;
};
static TextRenderer textRenderer;

static void RasterizeFont(U8 *const pixels) {
    // GDI is used only during initialization, with grayscale antialiasing
    // instead of display-specific ClearType. The atlas contains printable ASCII.
    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        Die("creating font DC", GetLastError());
    }
    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = kFontWidth;
    bitmapInfo.bmiHeader.biHeight = -static_cast<I32>(kFontHeight);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void *bits;
    const HBITMAP bitmap = CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    const HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!bitmap || !font) {
        Die("creating font atlas", GetLastError());
    }
    const HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    const HGDIOBJ oldFont = SelectObject(dc, font);
    PatBlt(dc, 0, 0, kFontWidth, kFontHeight, BLACKNESS);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    for (U32 i = 0; i < kFontCount; ++i) {
        const char character = static_cast<char>(kFontFirst + i);
        SIZE size;
        if (!GetTextExtentPoint32A(dc, &character, 1, &size) ||
            size.cx > static_cast<LONG>(kFontCellWidth - 2) ||
            size.cy > static_cast<LONG>(kFontCellHeight - 2)) {
            Die("measuring atlas glyph", i);
        }
        textRenderer.advance[i] = static_cast<F32>(size.cx);
        const I32 x = (i % kFontColumns) * kFontCellWidth + 1;
        const I32 y = (i / kFontColumns) * kFontCellHeight + 1;
        if (!TextOutA(dc, x, y, &character, 1)) {
            Die("rasterizing atlas glyph", i);
        }
    }
    GdiFlush();
    const U32 *const source = static_cast<const U32 *>(bits);
    for (U32 i = 0; i < kFontWidth * kFontHeight; ++i) {
        pixels[i] = static_cast<U8>(source[i] & 255u);
    }
    // Unused final cell supplies a white texel for solid UI rectangles.
    pixels[kFontWidth * kFontHeight - 1] = 255;
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

static void InitText() {
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
    RasterizeFont(static_cast<U8 *>(mapped) + kTextUploadOffset);

    const VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .extent = {.width = kFontWidth, .height = kFontHeight, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(r.device, &imageInfo, nullptr, &textRenderer.image));
    vkGetImageMemoryRequirements(r.device, textRenderer.image, &requirements);
    const VkMemoryAllocateInfo imageMemory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VK_CHECK(vkAllocateMemory(r.device, &imageMemory, nullptr, &textRenderer.imageMemory));
    VK_CHECK(vkBindImageMemory(r.device, textRenderer.image, textRenderer.imageMemory, 0));
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r.cmd, &begin));
    TransitionImage(textRenderer.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy copy = {
        .bufferOffset = kTextUploadOffset,
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
        .imageExtent = {.width = kFontWidth, .height = kFontHeight, .depth = 1},
    };
    vkCmdCopyBufferToImage(r.cmd, textRenderer.buffer, textRenderer.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    TransitionImage(textRenderer.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VK_CHECK(vkEndCommandBuffer(r.cmd));
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &r.cmd,
    };
    VK_CHECK(vkQueueSubmit(r.queue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(r.queue));
    VK_CHECK(vkResetCommandPool(r.device, r.pool, 0));

    const VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = textRenderer.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange = kColorSubresource,
    };
    VK_CHECK(vkCreateImageView(r.device, &viewInfo, nullptr, &textRenderer.view));
    const VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    VK_CHECK(vkCreateSampler(r.device, &samplerInfo, nullptr, &textRenderer.sampler));
    const VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    const VkDescriptorSetLayoutCreateInfo descriptorLayout = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r.device, &descriptorLayout, nullptr, &textRenderer.descriptorLayout));
    const VkDescriptorPoolSize poolSize = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1};
    const VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };
    VK_CHECK(vkCreateDescriptorPool(r.device, &poolInfo, nullptr, &textRenderer.descriptorPool));
    const VkDescriptorSetAllocateInfo descriptorInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = textRenderer.descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &textRenderer.descriptorLayout,
    };
    VK_CHECK(vkAllocateDescriptorSets(r.device, &descriptorInfo, &textRenderer.descriptor));
    const VkDescriptorImageInfo imageDescriptor = {
        .sampler = textRenderer.sampler,
        .imageView = textRenderer.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = textRenderer.descriptor,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &imageDescriptor,
    };
    vkUpdateDescriptorSets(r.device, 1, &write, 0, nullptr);
    const VkPushConstantRange range = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(TextPush)};
    const VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &textRenderer.descriptorLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };
    VK_CHECK(vkCreatePipelineLayout(r.device, &layoutInfo, nullptr, &textRenderer.layout));
    textRenderer.pipeline = CreatePipeline(kTextVert, sizeof(kTextVert), kTextFrag, sizeof(kTextFrag), textRenderer.layout, true);
}

static bool AppendTextQuad(const F32 x, const F32 y, const F32 width, const F32 height,
                           const F32 u0, const F32 v0, const F32 u1, const F32 v1,
                           const F32 red, const F32 green, const F32 blue, const F32 alpha) {
    if (textRenderer.quadCount == kTextMaxQuads) {
        return false;
    }
    TextVertex *const vertices = textRenderer.vertices + textRenderer.quadCount++ * 4;
    vertices[0] = {x, y, u0, v0, {red, green, blue, alpha}};
    vertices[1] = {x + width, y, u1, v0, {red, green, blue, alpha}};
    vertices[2] = {x, y + height, u0, v1, {red, green, blue, alpha}};
    vertices[3] = {x + width, y + height, u1, v1, {red, green, blue, alpha}};
    return true;
}

// Returns false on batch exhaustion. Unsupported bytes use a '?' glyph.
static bool AppendText(const char *string, const F32 x, const F32 y, const F32 scale = 1.0f) {
    F32 penX = x;
    F32 penY = y;
    for (; *string != '\0'; ++string) {
        const U8 character = static_cast<U8>(*string);
        if (character == '\n') {
            penX = x;
            penY += 24.0f * scale;
            continue;
        }
        const U32 glyph = character >= kFontFirst && character < kFontFirst + kFontCount ?
                          character - kFontFirst : '?' - kFontFirst;
        const F32 u = static_cast<F32>((glyph % kFontColumns) * kFontCellWidth) / kFontWidth;
        const F32 v = static_cast<F32>((glyph / kFontColumns) * kFontCellHeight) / kFontHeight;
        if (character != ' ' && !AppendTextQuad(penX - scale, penY - scale,
                kFontCellWidth * scale, kFontCellHeight * scale, u, v,
                u + static_cast<F32>(kFontCellWidth) / kFontWidth,
                v + static_cast<F32>(kFontCellHeight) / kFontHeight, 1.0f, 1.0f, 1.0f, 1.0f)) {
            return false;
        }
        penX += textRenderer.advance[glyph] * scale;
    }
    return true;
}

static void DestroyText() {
    vkDestroyPipeline(r.device, textRenderer.pipeline, nullptr);
    vkDestroyPipelineLayout(r.device, textRenderer.layout, nullptr);
    vkDestroyDescriptorPool(r.device, textRenderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(r.device, textRenderer.descriptorLayout, nullptr);
    vkDestroySampler(r.device, textRenderer.sampler, nullptr);
    vkDestroyImageView(r.device, textRenderer.view, nullptr);
    vkDestroyImage(r.device, textRenderer.image, nullptr);
    vkFreeMemory(r.device, textRenderer.imageMemory, nullptr);
    vkUnmapMemory(r.device, textRenderer.bufferMemory);
    vkDestroyBuffer(r.device, textRenderer.buffer, nullptr);
    vkFreeMemory(r.device, textRenderer.bufferMemory, nullptr);
}

static void DrawFps() {
    textRenderer.quadCount = 0;
    char label[11];
    snprintf(label, sizeof(label), "FPS: %u", displayedFps);
    // A solid panel uses the reserved white texel and shares the text batch.
    const F32 whiteU = (kFontWidth - 0.5f) / kFontWidth;
    const F32 whiteV = (kFontHeight - 0.5f) / kFontHeight;
    AppendTextQuad(8.0f, 8.0f, 132.0f, 32.0f, whiteU, whiteV, whiteU, whiteV, 0.0f, 0.0f, 0.0f, 0.9f);
    AppendText(label, 14.0f, 14.0f);
    const TextPush push = {
        .screenScale = {2.0f / r.extent.width, 2.0f / r.extent.height},
        .vertices = textRenderer.address,
    };
    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, textRenderer.pipeline);
    vkCmdBindDescriptorSets(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, textRenderer.layout,
                            0, 1, &textRenderer.descriptor, 0, nullptr);
    vkCmdPushConstants(r.cmd, textRenderer.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdBindIndexBuffer(r.cmd, textRenderer.buffer, kTextIndexOffset, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(r.cmd, textRenderer.quadCount * 6, 1, 0, 0, 0);
}
