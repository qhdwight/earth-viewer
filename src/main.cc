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

namespace {
    [[noreturn]] void Die(char const *what, I64 code) {
        printf("%s failed (%lld)\n", what, code);
        ExitProcess(1);
    }

#define VK_CHECK(expr)                                                    \
  do {                                                                    \
    if (VkResult vk_check_result = (expr); vk_check_result != VK_SUCCESS) \
      Die(#expr, vk_check_result);                                        \
  } while (0)

    constexpr U32 kMaxImages = 16;

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

    void CreateSwapchain() {
        vkDeviceWaitIdle(r.device);
        for (U32 i = 0; i < r.image_count; ++i) {
            vkDestroyImageView(r.device, r.views[i], nullptr);
            vkDestroySemaphore(r.device, r.render_done[i], nullptr);
        }

        VkSurfaceCapabilitiesKHR caps;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r.physical_device,
            r.surface, &caps));
        r.extent = caps.currentExtent;

        VkSurfaceFormatKHR formats[32];
        U32 format_count = 32;
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
        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = r.surface;
        info.minImageCount = min_images;
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = r.extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = old_swapchain;
        VK_CHECK(vkCreateSwapchainKHR(r.device, &info, nullptr, &r.swapchain));
        vkDestroySwapchainKHR(r.device, old_swapchain, nullptr);

        r.image_count = kMaxImages;
        VK_CHECK(
            vkGetSwapchainImagesKHR(r.device, r.swapchain, &r.image_count, r.images));
        for (U32 i = 0; i < r.image_count; ++i) {
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = r.images[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = r.format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(r.device, &view, nullptr, &r.views[i]));

            VkSemaphoreCreateInfo sem{};
            sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VK_CHECK(vkCreateSemaphore(r.device, &sem, nullptr, &r.render_done[i]));
        }
    }

    void InitVulkan(HWND hwnd) {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "rct";
        app.apiVersion = VK_API_VERSION_1_4;

        char const *instance_extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };
        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &app;
        instance_info.enabledExtensionCount = 2;
        instance_info.ppEnabledExtensionNames = instance_extensions;
        VK_CHECK(vkCreateInstance(&instance_info, nullptr, &r.instance));

        VkWin32SurfaceCreateInfoKHR surface_info{};
        surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        surface_info.hinstance = GetModuleHandleW(nullptr);
        surface_info.hwnd = hwnd;
        VK_CHECK(
            vkCreateWin32SurfaceKHR(r.instance, &surface_info, nullptr, &r.surface));

        // Pick the first physical device that speaks Vulkan 1.4 and can both draw
        // and present on one queue family.
        VkPhysicalDevice devices[16];
        U32 device_count = 16;
        vkEnumeratePhysicalDevices(r.instance, &device_count, devices);
        for (U32 i = 0; i < device_count && !r.physical_device; ++i) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devices[i], &props);
            if (props.apiVersion < VK_API_VERSION_1_4) continue;

            VkQueueFamilyProperties families[16];
            U32 family_count = 16;
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

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.synchronization2 = VK_TRUE;
        features13.dynamicRendering = VK_TRUE;
        VkPhysicalDeviceVulkan14Features features14{};
        features14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
        features14.pNext = &features13;
        features14.maintenance5 = VK_TRUE;

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = r.queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;

        char const *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = &features14;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = device_extensions;
        VK_CHECK(vkCreateDevice(r.physical_device, &device_info, nullptr, &r.device));
        vkGetDeviceQueue(r.device, r.queue_family, 0, &r.queue);

        VkSemaphoreCreateInfo sem{};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(r.device, &sem, nullptr, &r.acquire));
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(r.device, &fence_info, nullptr, &r.fence));

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = r.queue_family;
        VK_CHECK(vkCreateCommandPool(r.device, &pool_info, nullptr, &r.pool));
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.commandPool = r.pool;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(r.device, &cmd_info, &r.cmd));

        CreateSwapchain();
    }

    void CreatePipeline() {
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        VK_CHECK(vkCreatePipelineLayout(r.device, &layout_info, nullptr,
            &r.pipeline_layout));

        // maintenance5 (core 1.4): chain SPIR-V straight into the stage info,
        // no VkShaderModule objects needed.
        VkShaderModuleCreateInfo vert_code{};
        vert_code.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vert_code.codeSize = sizeof(kTriangleVert);
        vert_code.pCode = kTriangleVert;
        VkShaderModuleCreateInfo frag_code = vert_code;
        frag_code.codeSize = sizeof(kTriangleFrag);
        frag_code.pCode = kTriangleFrag;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].pNext = &vert_code;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].pName = "main";
        stages[1] = stages[0];
        stages[1].pNext = &frag_code;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType =
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{}; // counts are dynamic
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;

        // Viewport and scissor stay dynamic so a resize never rebuilds the
        // pipeline (extended dynamic state, core 1.3).
        VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
            VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;

        // Dynamic rendering (core 1.3): no VkRenderPass, no VkFramebuffer.
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &r.format;

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &rendering;
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = stages;
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState = &viewport;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = r.pipeline_layout;
        VK_CHECK(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1,
            &pipeline_info, nullptr, &r.pipeline));
    }

    void TransitionImage(VkImage image, VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                         VkPipelineStageFlags2 dst_stage,
                         VkAccessFlags2 dst_access) {
        // synchronization2 (core 1.3).
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(r.cmd, &dependency);
    }

    void DrawFrame() {
        VK_CHECK(vkWaitForFences(r.device, 1, &r.fence, VK_TRUE, ~0ull));

        U32 image;
        VkResult acquired = vkAcquireNextImageKHR(r.device, r.swapchain, ~0ull,
                                                  r.acquire, VK_NULL_HANDLE, &image);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            CreateSwapchain();
            return;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            Die("vkAcquireNextImageKHR", acquired);

        VK_CHECK(vkResetFences(r.device, 1, &r.fence));
        VK_CHECK(vkResetCommandPool(r.device, r.pool, 0));

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(r.cmd, &begin));

        TransitionImage(r.images[image], VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = r.views[image];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.05f, 0.05f, 0.08f, 1.0f}};
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, r.extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(r.cmd, &rendering);

        vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
        VkViewport viewport{
            0.0f, 0.0f, static_cast<F32>(r.extent.width),
            static_cast<F32>(r.extent.height), 0.0f, 1.0f
        };
        vkCmdSetViewportWithCount(r.cmd, 1, &viewport);
        VkRect2D scissor{{0, 0}, r.extent};
        vkCmdSetScissorWithCount(r.cmd, 1, &scissor);
        vkCmdDraw(r.cmd, 3, 1, 0, 0);

        vkCmdEndRendering(r.cmd);

        TransitionImage(r.images[image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);

        VK_CHECK(vkEndCommandBuffer(r.cmd));

        VkCommandBufferSubmitInfo cmd_submit{};
        cmd_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit.commandBuffer = r.cmd;
        VkSemaphoreSubmitInfo wait_info{};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_info.semaphore = r.acquire;
        wait_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphoreSubmitInfo signal_info = wait_info;
        signal_info.semaphore = r.render_done[image];
        signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait_info;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_submit;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal_info;
        VK_CHECK(vkQueueSubmit2(r.queue, 1, &submit, r.fence));

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &r.render_done[image];
        present.swapchainCount = 1;
        present.pSwapchains = &r.swapchain;
        present.pImageIndices = &image;
        VkResult presented = vkQueuePresentKHR(r.queue, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
            CreateSwapchain();
        else if (presented != VK_SUCCESS)
            Die("vkQueuePresentKHR", presented);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
} // namespace

int main() {
    HINSTANCE hinstance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinstance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */);
    wc.lpszClassName = L"rct";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"rct", L"rct triangle", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr,
                                nullptr, hinstance, nullptr);
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
