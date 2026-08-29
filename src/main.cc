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

[[noreturn]] static void Die(char const *what, I64 code) {
    printf("%s failed (%lld)\n", what, code);
    ExitProcess(1);
}

#define VK_CHECK(expr)                                                    \
  do {                                                                    \
    if (VkResult vk_check_result = (expr); vk_check_result != VK_SUCCESS) \
      Die(#expr, vk_check_result);                                        \
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

struct Renderer {
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    U32 queue_family = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    U32 image_count = 0;
    VkImage images[kMaxImages]{};
    VkImageView views[kMaxImages]{};
    VkSemaphore render_done[kMaxImages]{};

    VkSemaphore acquire = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

Renderer r;

static void CreateSwapchain() {
    vkDeviceWaitIdle(r.device);
    for (U32 i = 0; i < r.image_count; ++i) {
        vkDestroyImageView(r.device, r.views[i], nullptr);
        vkDestroySemaphore(r.device, r.render_done[i], nullptr);
    }

    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r.physical_device,
        r.surface, &caps));
    r.extent = caps.currentExtent;

    VkSurfaceFormatKHR formats[kMaxFormats];
    U32 format_count = kMaxFormats;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r.physical_device, r.surface,
                                         &format_count, formats);
    VkSurfaceFormatKHR chosen = formats[0];
    for (U32 i = 0; i < format_count; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            chosen = formats[i];
    r.format = chosen.format;

    U32 min_images = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) min_images = Min(min_images, caps.maxImageCount);

    VkSwapchainKHR old_swapchain = r.swapchain;
    VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = r.surface,
        .minImageCount = min_images,
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
        .oldSwapchain = old_swapchain,
    };
    VK_CHECK(vkCreateSwapchainKHR(r.device, &info, nullptr, &r.swapchain));
    vkDestroySwapchainKHR(r.device, old_swapchain, nullptr);

    r.image_count = kMaxImages;
    VK_CHECK(
        vkGetSwapchainImagesKHR(r.device, r.swapchain, &r.image_count, r.images));
    for (U32 i = 0; i < r.image_count; ++i) {
        VkImageViewCreateInfo view = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = r.images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = r.format,
            .subresourceRange = kColorSubresource,
        };
        VK_CHECK(vkCreateImageView(r.device, &view, nullptr, &r.views[i]));

        VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(r.device, &sem, nullptr, &r.render_done[i]));
    }
}

static void InitVulkan(HWND hwnd) {
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = kAppName,
        .apiVersion = VK_API_VERSION_1_4,
    };

    char const *instance_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = instance_extensions,
    };
    VK_CHECK(vkCreateInstance(&instance_info, nullptr, &r.instance));

    VkWin32SurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = GetModuleHandleW(nullptr),
        .hwnd = hwnd,
    };
    VK_CHECK(
        vkCreateWin32SurfaceKHR(r.instance, &surface_info, nullptr, &r.surface));

    // Pick the first physical device that speaks Vulkan 1.4 and can both draw
    // and present on one queue family.
    VkPhysicalDevice devices[kMaxDevices];
    U32 device_count = kMaxDevices;
    vkEnumeratePhysicalDevices(r.instance, &device_count, devices);
    for (U32 i = 0; i < device_count && !r.physical_device; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.apiVersion < VK_API_VERSION_1_4) continue;

        VkQueueFamilyProperties families[kMaxQueueFamilies];
        U32 family_count = kMaxQueueFamilies;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count,
                                                 families);
        for (U32 f = 0; f < family_count; ++f) {
            if ((families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                vkGetPhysicalDeviceWin32PresentationSupportKHR(devices[i], f)) {
                r.physical_device = devices[i];
                r.queue_family = f;
                printf("Using %s\n", props.deviceName);
                break;
            }
        }
    }
    if (!r.physical_device) Die("finding a Vulkan 1.4 device", 0);

    VkPhysicalDeviceVulkan13Features features13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan14Features features14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .pNext = &features13,
        .maintenance5 = VK_TRUE,
    };

    F32 const priority = kQueuePriority;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = r.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };

    char const *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features14,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions,
    };
    VK_CHECK(vkCreateDevice(r.physical_device, &device_info, nullptr, &r.device));
    vkGetDeviceQueue(r.device, r.queue_family, 0, &r.queue);

    VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(r.device, &sem, nullptr, &r.acquire));
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    VK_CHECK(vkCreateFence(r.device, &fence_info, nullptr, &r.fence));

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = r.queue_family,
    };
    VK_CHECK(vkCreateCommandPool(r.device, &pool_info, nullptr, &r.pool));
    VkCommandBufferAllocateInfo cmd_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_CHECK(vkAllocateCommandBuffers(r.device, &cmd_info, &r.cmd));

    CreateSwapchain();
}

static void CreatePipeline() {
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    VK_CHECK(vkCreatePipelineLayout(r.device, &layout_info, nullptr,
        &r.pipeline_layout));

    // maintenance5 (core 1.4): chain SPIR-V straight into the stage info,
    // no VkShaderModule objects needed.
    VkShaderModuleCreateInfo vert_code = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(kTriangleVert),
        .pCode = kTriangleVert,
    };
    VkShaderModuleCreateInfo frag_code = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(kTriangleFrag),
        .pCode = kTriangleFrag,
    };

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &vert_code,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &frag_code,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pName = "main",
        },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewport = { // counts are dynamic
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = kLineWidth,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };

    // Viewport and scissor stay dynamic so a resize never rebuilds the
    // pipeline (extended dynamic state, core 1.3).
    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
    };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };

    // Dynamic rendering (core 1.3): no VkRenderPass, no VkFramebuffer.
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &r.format,
    };

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = r.pipeline_layout,
    };
    VK_CHECK(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1,
        &pipeline_info, nullptr, &r.pipeline));
}

static void TransitionImage(VkImage image, VkImageLayout from, VkImageLayout to,
                     VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                     VkPipelineStageFlags2 dst_stage,
                     VkAccessFlags2 dst_access) {
    // synchronization2 (core 1.3).
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = kColorSubresource,
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(r.cmd, &dependency);
}

static void DrawFrame() {
    VK_CHECK(vkWaitForFences(r.device, 1, &r.fence, VK_TRUE, kWaitForever));

    U32 image;
    VkResult acquired = vkAcquireNextImageKHR(r.device, r.swapchain, kWaitForever,
                                              r.acquire, VK_NULL_HANDLE, &image);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        CreateSwapchain();
        return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        Die("vkAcquireNextImageKHR", acquired);

    VK_CHECK(vkResetFences(r.device, 1, &r.fence));
    VK_CHECK(vkResetCommandPool(r.device, r.pool, 0));

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r.cmd, &begin));

    TransitionImage(r.images[image], VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = r.views[image],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = kClearColor,
    };
    VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, r.extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    vkCmdBeginRendering(r.cmd, &rendering);

    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
    VkViewport viewport{
        0.0f, 0.0f, static_cast<F32>(r.extent.width),
        static_cast<F32>(r.extent.height), 0.0f, 1.0f
    };
    vkCmdSetViewportWithCount(r.cmd, 1, &viewport);
    VkRect2D scissor{{0, 0}, r.extent};
    vkCmdSetScissorWithCount(r.cmd, 1, &scissor);
    vkCmdDraw(r.cmd, kVerticesPerTriangle, kInstanceCount, 0, 0);

    vkCmdEndRendering(r.cmd);

    TransitionImage(r.images[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);

    VK_CHECK(vkEndCommandBuffer(r.cmd));

    VkCommandBufferSubmitInfo cmd_submit = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = r.cmd,
    };
    VkSemaphoreSubmitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = r.acquire,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSemaphoreSubmitInfo signal_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = r.render_done[image],
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait_info,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cmd_submit,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal_info,
    };
    VK_CHECK(vkQueueSubmit2(r.queue, 1, &submit, r.fence));

    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &r.render_done[image],
        .swapchainCount = 1,
        .pSwapchains = &r.swapchain,
        .pImageIndices = &image,
    };
    VkResult presented = vkQueuePresentKHR(r.queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
        CreateSwapchain();
    else if (presented != VK_SUCCESS)
        Die("vkQueuePresentKHR", presented);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int main() {
    HINSTANCE hinstance = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {
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
