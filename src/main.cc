#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX  // Bazel predefines this on Windows
#define NOMINMAX
#endif
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "core.h"
#include "src/triangle_frag.h"
#include "src/triangle_vert.h"

#include <cstdio>

[[noreturn]] static void Die(char const *what, const I64 code) {
    printf("%s failed (%lld)\n", what, code);
    ExitProcess(1);
}

#define VK_CHECK(expr)                                                    \
  do {                                                                    \
    if (VkResult vkCheckResult = (expr); vkCheckResult != VK_SUCCESS) \
      Die(#expr, vkCheckResult);                                        \
  } while (0)

constexpr char const *kAppName = "rct";
constexpr wchar_t const *kClassName = L"rct";
constexpr wchar_t const *kWindowTitle = L"rct triangle";
constexpr int kIdcArrow = 32512; // IDC_ARROW

constexpr U32 kMaxImages = 16;
constexpr U32 kMaxFormats = 32;
constexpr U32 kMaxDevices = 16;
constexpr U32 kMaxQueueFamilies = 16;

constexpr U32 kVerticesPerTriangle = 3;
constexpr U32 kInstanceCount = 1;

constexpr U32 kWindowWidth = 1280;
constexpr U32 kWindowHeight = 720;

constexpr F32 kQueuePriority = 1.0f;
constexpr F32 kLineWidth = 1.0f;
constexpr U64 kWaitForever = ~0ull;

constexpr VkImageSubresourceRange kColorSubresource = {
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};
constexpr VkClearColorValue kClearColor = {.float32 = {0.05f, 0.05f, 0.08f, 1.0f}};

// Matches the std430 Vertex struct in triangle.vert.
struct Vertex {
    F32 position[4];
    F32 color[4];
};

constexpr Vertex kTriangleVertices[kVerticesPerTriangle] = {
    {.position = {0.0f, -0.6f, 0.0f, 1.0f}, .color = {1.0f, 0.0f, 0.0f, 1.0f}},
    {.position = {0.6f, 0.6f, 0.0f, 1.0f}, .color = {0.0f, 1.0f, 0.0f, 1.0f}},
    {.position = {-0.6f, 0.6f, 0.0f, 1.0f}, .color = {0.0f, 0.0f, 1.0f, 1.0f}},
};

struct Renderer {
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    U32 queueFamily = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    U32 imageCount = 0;
    VkImage images[kMaxImages]{};
    VkImageView views[kMaxImages]{};
    VkSemaphore renderDone[kMaxImages]{};

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkDeviceAddress vertexAddress = 0;

    VkSemaphore acquire = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

static Renderer r;

static void CreateSwapchain() {
    vkDeviceWaitIdle(r.device);
    for (U32 i = 0; i < r.imageCount; ++i) {
        vkDestroyImageView(r.device, r.views[i], nullptr);
        vkDestroySemaphore(r.device, r.renderDone[i], nullptr);
    }

    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r.physicalDevice, r.surface, &caps));
    r.extent = caps.currentExtent;

    VkSurfaceFormatKHR formats[kMaxFormats];
    U32 formatCount = kMaxFormats;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r.physicalDevice, r.surface, &formatCount, formats);
    VkSurfaceFormatKHR chosen = formats[0];
    for (U32 iFormat = 0; iFormat < formatCount; ++iFormat) {
        if (formats[iFormat].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[iFormat].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = formats[iFormat];
        }
    }
    r.format = chosen.format;

    U32 minImages = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) {
        minImages = Min(minImages, caps.maxImageCount);
    }

    VkSwapchainKHR oldSwapchain = r.swapchain;
    const VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = r.surface,
        .minImageCount = minImages,
        .imageFormat = chosen.format,
        .imageColorSpace = chosen.colorSpace,
        .imageExtent = r.extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = oldSwapchain,
    };
    VK_CHECK(vkCreateSwapchainKHR(r.device, &info, nullptr, &r.swapchain));
    vkDestroySwapchainKHR(r.device, oldSwapchain, nullptr);

    r.imageCount = kMaxImages;
    VK_CHECK(
        vkGetSwapchainImagesKHR(r.device, r.swapchain, &r.imageCount, r.images));
    for (U32 i = 0; i < r.imageCount; ++i) {
        VkImageViewCreateInfo view = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = r.images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = r.format,
            .subresourceRange = kColorSubresource,
        };
        VK_CHECK(vkCreateImageView(r.device, &view, nullptr, &r.views[i]));

        VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(r.device, &sem, nullptr, &r.renderDone[i]));
    }
}

static void InitVulkan(HWND hwnd) {
    constexpr VkApplicationInfo kApp = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = kAppName,
        .apiVersion = VK_API_VERSION_1_4,
    };

    constexpr char const *kInstanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    const VkInstanceCreateInfo instanceInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &kApp,
        .enabledExtensionCount = Count(kInstanceExtensions),
        .ppEnabledExtensionNames = kInstanceExtensions,
    };
    VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &r.instance));

    const VkWin32SurfaceCreateInfoKHR surfaceInfo = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = GetModuleHandleW(nullptr),
        .hwnd = hwnd,
    };
    VK_CHECK(vkCreateWin32SurfaceKHR(r.instance, &surfaceInfo, nullptr, &r.surface));

    // Pick the first physical device that speaks Vulkan 1.4 and can both draw
    // and present on one queue family.
    VkPhysicalDevice devices[kMaxDevices];
    U32 deviceCount = kMaxDevices;
    vkEnumeratePhysicalDevices(r.instance, &deviceCount, devices);
    for (U32 iDevice = 0; iDevice < deviceCount && !r.physicalDevice; ++iDevice) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[iDevice], &props);
        if (props.apiVersion < VK_API_VERSION_1_4) {
            continue;
        }

        VkQueueFamilyProperties families[kMaxQueueFamilies];
        U32 familyCount = kMaxQueueFamilies;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[iDevice], &familyCount, families);
        for (U32 iFamily = 0; iFamily < familyCount; ++iFamily) {
            if (!(families[iFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
                !vkGetPhysicalDeviceWin32PresentationSupportKHR(devices[iDevice], iFamily)) {
                continue;
            }

            r.physicalDevice = devices[iDevice];
            r.queueFamily = iFamily;
            printf("Using %s\n", props.deviceName);
            break;
        }
    }
    if (!r.physicalDevice) Die("finding a Vulkan 1.4 device", 0);

    VkPhysicalDeviceVulkan12Features features12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .bufferDeviceAddress = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features features13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &features12,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan14Features features14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .pNext = &features13,
        .maintenance5 = VK_TRUE,
    };

    const VkDeviceQueueCreateInfo queueInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = r.queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &kQueuePriority,
    };

    constexpr char const *kDeviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const VkDeviceCreateInfo deviceInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features14,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = kDeviceExtensions,
    };
    VK_CHECK(vkCreateDevice(r.physicalDevice, &deviceInfo, nullptr, &r.device));
    vkGetDeviceQueue(r.device, r.queueFamily, 0, &r.queue);

    constexpr VkSemaphoreCreateInfo kSemInfo = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(r.device, &kSemInfo, nullptr, &r.acquire));
    constexpr VkFenceCreateInfo kFenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    VK_CHECK(vkCreateFence(r.device, &kFenceInfo, nullptr, &r.fence));

    const VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = r.queueFamily,
    };
    VK_CHECK(vkCreateCommandPool(r.device, &poolInfo, nullptr, &r.pool));
    const VkCommandBufferAllocateInfo cmdInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_CHECK(vkAllocateCommandBuffers(r.device, &cmdInfo, &r.cmd));

    CreateSwapchain();
}

// Bindless vertex fetch: the vertices live in a plain buffer whose GPU virtual
// address (VK_KHR_buffer_device_address, core 1.2) is handed to the vertex
// shader through a push constant. No descriptor sets, no vertex input state.
static void CreateVertexBuffer() {
    const VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(kTriangleVertices),
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(r.device, &bufferInfo, nullptr, &r.vertexBuffer));

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(r.device, r.vertexBuffer, &requirements);

    VkPhysicalDeviceMemoryProperties memoryProps;
    vkGetPhysicalDeviceMemoryProperties(r.physicalDevice, &memoryProps);
    constexpr VkMemoryPropertyFlags kWanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    U32 memoryType = ~0u;
    for (U32 iType = 0; iType < memoryProps.memoryTypeCount; ++iType) {
        if ((requirements.memoryTypeBits & (1u << iType)) &&
            (memoryProps.memoryTypes[iType].propertyFlags & kWanted) == kWanted) {
            memoryType = iType;
            break;
        }
    }
    if (memoryType == ~0u) Die("finding host-visible memory for the vertex buffer", 0);

    // Memory backing a buffer whose address is queried must itself be
    // allocated with the device-address flag.
    constexpr VkMemoryAllocateFlagsInfo kAllocFlags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &kAllocFlags,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType,
    };
    VK_CHECK(vkAllocateMemory(r.device, &allocInfo, nullptr, &r.vertexMemory));
    VK_CHECK(vkBindBufferMemory(r.device, r.vertexBuffer, r.vertexMemory, 0));

    void *mapped;
    VK_CHECK(vkMapMemory(r.device, r.vertexMemory, 0, sizeof(kTriangleVertices), 0, &mapped));
    const U8 *src = reinterpret_cast<const U8 *>(kTriangleVertices);
    U8 *dst = static_cast<U8 *>(mapped);
    for (U64 i = 0; i < sizeof(kTriangleVertices); ++i) dst[i] = src[i];
    vkUnmapMemory(r.device, r.vertexMemory);

    const VkBufferDeviceAddressInfo addressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = r.vertexBuffer,
    };
    r.vertexAddress = vkGetBufferDeviceAddress(r.device, &addressInfo);
}

static void CreatePipeline() {
    constexpr VkPushConstantRange kPushRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(VkDeviceAddress),
    };
    const VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &kPushRange,
    };
    VK_CHECK(vkCreatePipelineLayout(r.device, &layoutInfo, nullptr, &r.pipelineLayout));

    // maintenance5 (core 1.4): chain SPIR-V straight into the stage info,
    // no VkShaderModule objects needed.
    const VkShaderModuleCreateInfo vertCode = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(kTriangleVert),
        .pCode = kTriangleVert,
    };
    const VkShaderModuleCreateInfo fragCode = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(kTriangleFrag),
        .pCode = kTriangleFrag,
    };

    const VkPipelineShaderStageCreateInfo stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &vertCode,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &fragCode,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pName = "main",
        },
    };

    constexpr VkPipelineVertexInputStateCreateInfo kVertexInput = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    constexpr VkPipelineInputAssemblyStateCreateInfo kAssembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    constexpr VkPipelineViewportStateCreateInfo kViewport = {
        // counts are dynamic
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    };
    constexpr VkPipelineRasterizationStateCreateInfo kRaster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = kLineWidth,
    };
    constexpr VkPipelineMultisampleStateCreateInfo kMultisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    constexpr VkPipelineColorBlendAttachmentState kBlendAttachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &kBlendAttachment,
    };

    // Viewport and scissor stay dynamic so a resize never rebuilds the
    // pipeline (extended dynamic state, core 1.3).
    constexpr VkDynamicState kDynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
    };
    const VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = kDynamicStates,
    };

    // Dynamic rendering (core 1.3): no VkRenderPass, no VkFramebuffer.
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &r.format,
    };

    const VkGraphicsPipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &kVertexInput,
        .pInputAssemblyState = &kAssembly,
        .pViewportState = &kViewport,
        .pRasterizationState = &kRaster,
        .pMultisampleState = &kMultisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = r.pipelineLayout,
    };
    VK_CHECK(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &r.pipeline));
}

static void TransitionImage(VkImage image, const VkImageLayout from, const VkImageLayout to,
                            const VkPipelineStageFlags2 srcStage, const VkAccessFlags2 srcAccess,
                            const VkPipelineStageFlags2 dstStage,
                            const VkAccessFlags2 dstAccess) {
    // synchronization2 (core 1.3).
    const VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = kColorSubresource,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(r.cmd, &dependency);
}

static void DrawFrame() {
    VK_CHECK(vkWaitForFences(r.device, 1, &r.fence, VK_TRUE, kWaitForever));

    U32 image;
    const VkResult acquired = vkAcquireNextImageKHR(r.device, r.swapchain, kWaitForever,
                                                    r.acquire, VK_NULL_HANDLE, &image);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        CreateSwapchain();
        return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        Die("vkAcquireNextImageKHR", acquired);
    }

    VK_CHECK(vkResetFences(r.device, 1, &r.fence));
    VK_CHECK(vkResetCommandPool(r.device, r.pool, 0));

    constexpr VkCommandBufferBeginInfo kBegin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r.cmd, &kBegin));

    TransitionImage(r.images[image], VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    const VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = r.views[image],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {kClearColor},
    };
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {.x = 0, .y = 0}, .extent = r.extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    vkCmdBeginRendering(r.cmd, &rendering);

    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
    vkCmdPushConstants(r.cmd, r.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(r.vertexAddress), &r.vertexAddress);
    const VkViewport viewport = {
        .x = 0.0f, .y = 0.0f,
        .width = static_cast<F32>(r.extent.width), .height = static_cast<F32>(r.extent.height),
        .minDepth = 0.0f, .maxDepth = 1.0f
    };
    vkCmdSetViewportWithCount(r.cmd, 1, &viewport);
    const VkRect2D scissor{.offset = {.x = 0, .y = 0}, .extent = r.extent};
    vkCmdSetScissorWithCount(r.cmd, 1, &scissor);
    vkCmdDraw(r.cmd, kVerticesPerTriangle, kInstanceCount, 0, 0);

    vkCmdEndRendering(r.cmd);

    TransitionImage(r.images[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);

    VK_CHECK(vkEndCommandBuffer(r.cmd));

    const VkCommandBufferSubmitInfo cmdSubmit = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = r.cmd,
    };
    const VkSemaphoreSubmitInfo waitInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = r.acquire,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    const VkSemaphoreSubmitInfo signalInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = r.renderDone[image],
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &waitInfo,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cmdSubmit,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signalInfo,
    };
    VK_CHECK(vkQueueSubmit2(r.queue, 1, &submit, r.fence));

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &r.renderDone[image],
        .swapchainCount = 1,
        .pSwapchains = &r.swapchain,
        .pImageIndices = &image,
    };
    const VkResult presented = vkQueuePresentKHR(r.queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
        CreateSwapchain();
    else if (presented != VK_SUCCESS)
        Die("vkQueuePresentKHR", presented);
}

static LRESULT CALLBACK WndProc(HWND hwnd, const UINT msg, const WPARAM wparam, const LPARAM lparam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int main() {
    HINSTANCE hinstance = GetModuleHandleW(nullptr);
    const WNDCLASSW wc = {
        .lpfnWndProc = WndProc,
        .hInstance = hinstance,
        .hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(kIdcArrow)),
        .lpszClassName = kClassName,
    };
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, kClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, kWindowWidth,
                                kWindowHeight, nullptr, nullptr, hinstance, nullptr);
    if (!hwnd) Die("CreateWindowExW", static_cast<I64>(GetLastError()));
    ShowWindow(hwnd, SW_SHOW);

    InitVulkan(hwnd);
    CreateVertexBuffer();
    CreatePipeline();

    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                vkDeviceWaitIdle(r.device);
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        RECT rect;
        GetClientRect(hwnd, &rect);
        if (rect.right <= rect.left || rect.bottom <= rect.top) {
            WaitMessage(); // minimized: block instead of spinning
            continue;
        }
        DrawFrame();
    }
}
