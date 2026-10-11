#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX  // Bazel predefines this on Windows
#define NOMINMAX
#endif
// Bazel's MSVC toolchain targets Windows 7; the per-monitor DPI functions need
// the Windows 10 declarations.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__APPLE__)
#include <CoreText/CoreText.h>
#include <unistd.h>
#endif

#include <vulkan/vulkan.h>

#include "src/core.h"
#include "src/platform.h"
#include "src/bit/bitarray.h"
#include "src/borders.h"
#include "src/earth_frag.h"
#include "src/earth_vert.h"
#include "src/geodetic.h"
#include "src/icon.h"
#include "src/link_frag.h"
#include "src/link_vert.h"
#include "src/math.h"
#include "src/satellite_frag.h"
#include "src/satellite_vert.h"
#include "src/satellites.h"
#include "src/stations.h"
#include "src/sphere_frag.h"
#include "src/sphere_vert.h"
#include "src/sun.h"
#include "src/text_frag.h"
#include "src/text_vert.h"

#include <cstdio>

[[noreturn]] void Die(char const *what, const I64 code) {
    printf("%s failed (%lld)\n", what, code);
    fflush(stdout);
#if defined(_WIN32)
    ExitProcess(1);
#else
    _exit(1);
#endif
}

#define VK_CHECK(expr)                                                    \
  do {                                                                    \
    if (VkResult vkCheckResult = (expr); vkCheckResult != VK_SUCCESS) {   \
      Die(#expr, vkCheckResult);                                          \
      }                                                                   \
  } while (0)

constexpr char const *kAppName = "earth-viewer";

constexpr U32 kMaxImages = 16;
constexpr U32 kMaxFormats = 32;
constexpr U32 kMaxDevices = 16;
constexpr U32 kMaxQueueFamilies = 16;
// Every Vulkan device supports 4x colour multisampling, so no query needed.
constexpr VkSampleCountFlagBits kSampleCount = VK_SAMPLE_COUNT_4_BIT;

constexpr U32 kInstanceCount = 1;
constexpr U32 kFullScreenVertexCount = 3; // sphere.vert covers the screen with one triangle

constexpr F32 kQueuePriority = 1.0f;
// Border lines have a fixed width on the ground, in Earth radii, so they
// thicken as the camera approaches. Below the pixel floor they would break
// up into dots, so they never get thinner than that on screen.
constexpr F32 kLineWidthWorld = 0.001f;
constexpr F32 kMinLineWidthPixels = 1.5f;
// Ribbons are drawn this much wider than the line so the fragment shader
// can fade the true edge across pixels whose centres lie just outside it.
constexpr F32 kLineAaPaddingPixels = 1.0f;
// Only meaningful for line topologies; must stay 1 without the wideLines feature.
constexpr F32 kRasterLineWidth = 1.0f;
constexpr U64 kWaitForever = ~0ull;

// Camera. The globe is a unit sphere at the origin, so distances are in Earth
// radii. Movement speed scales with altitude so the surface never rushes by.
constexpr F32 kFovScale = kSqrt3; // 1 / tan(60 degrees / 2)
constexpr F32 kNearPlane = 1.0e-4f;
constexpr F32 kMouseRadiansPerPixel = 0.003f;
constexpr F32 kMaxPitch = 0.5f * kPi - 0.01f;
constexpr F32 kSpeedPerAltitude = 1.0f; // altitudes per second
constexpr F32 kMinAltitude = 1.0e-3f;
// Scroll wheel zoom: each notch scales the altitude by 1 + kZoomPerNotch, up to
// kMaxAltitude Earth radii.
constexpr F32 kZoomPerNotch = 0.2f;
constexpr F32 kMaxAltitude = 50.0f;
constexpr F32 kBoost = 4.0f;
constexpr F32 kMaxFrameSeconds = 0.1f;
// Simulation speeds, as multiples of real time; [ steps up and ] steps down.
constexpr F64 kTimeRates[] = {
    0.0, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0, 3600.0
};
constexpr I32 kRealTimeRate = 4; // index of 1x

constexpr VkImageSubresourceRange kColorSubresource = {
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};
constexpr VkClearColorValue kClearColor = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};

// ---------------------------------------------------------------------------
// Border geometry. The header stores each ring once; the GPU buffer holds a
// line list (two packed points per segment) so the whole globe is one draw.

static constexpr U32 CountBorderSegments() {
    U32 segments = 0;
    for (const BorderRing &ring: kBorderRings) {
        segments += ring.count - 1;
    }
    return segments;
}

constexpr U32 kBorderSegmentCount = CountBorderSegments();
constexpr U32 kPointsPerSegment = 2;
constexpr U32 kSegmentPointCount = kPointsPerSegment * kBorderSegmentCount;
constexpr U64 kVertexBufferSize = kSegmentPointCount * sizeof(U32);
// earth.vert expands every segment into a quad of two triangles.
constexpr U32 kVerticesPerSegment = 6;
constexpr U32 kDrawVertexCount = kVerticesPerSegment * kBorderSegmentCount;

// Longitude in the low 16 bits, latitude in the high 16 bits. Unpacked by
// earth.vert.
static constexpr U32 PackBorderPoint(const I16 (&point)[2]) {
    return static_cast<U16>(point[0]) | (static_cast<U32>(static_cast<U16>(point[1])) << 16);
}

// Matches the push_constant blocks in earth.vert and earth.frag.
struct PushConstants {
    Mat4 viewProj;
    F32 cameraPos[4];
    // x: half line width in world units.
    // y: minimum half line width in world units per unit of view depth.
    // z: anti-aliasing padding in world units per unit of view depth.
    // w: UI scale, for screen-space sizes in sphere.frag.
    F32 line[4];
    // xyz: unit vector toward the Sun, for the glow in sphere.frag.
    F32 sunDirection[4];
    VkDeviceAddress vertexBuffer;
};

static_assert(sizeof(PushConstants) == 120);
static_assert(offsetof(PushConstants, cameraPos) == 64);
static_assert(offsetof(PushConstants, line) == 80);
static_assert(offsetof(PushConstants, sunDirection) == 96);
static_assert(offsetof(PushConstants, vertexBuffer) == 112);

// Satellites are dots with a fixed radius on the ground scale, like the border
// lines, with a minimum size on screen set in satellite.vert.
constexpr F32 kSatelliteRadiusWorld = 0.0005f; // Earth radii, about 3 km
constexpr U32 kVerticesPerSprite = 6;
// Markers are the satellites followed by the ground stations, as vec4.
constexpr U32 kMarkerCount = kSatelliteCount + kStationCount;
constexpr U64 kSatelliteBufferSize = kMarkerCount * 4 * sizeof(F32);
// Station to satellite links: ribbons like the borders, but thinner.
constexpr F32 kLinkWidthWorld = 0.0004f; // Earth radii, about 2.5 km
constexpr F32 kMinLinkWidthPixels = 1.0f;
constexpr U64 kLinkBufferSize = kMaxLinks * 2 * 4 * sizeof(F32);

// Matches the push_constant block in satellite.vert.
struct SatellitePush {
    Mat4 viewProj;
    F32 cameraPos[4];
    F32 sunDirection[4];
    // x: dot radius in world units, y: world units per UI pixel per unit of
    // view depth, zw: clip-space offset per world unit across the view.
    F32 sprite[4];
    VkDeviceAddress positions;
};

static_assert(sizeof(SatellitePush) == 120);
static_assert(offsetof(SatellitePush, sprite) == 96);
static_assert(offsetof(SatellitePush, positions) == 112);

// ---------------------------------------------------------------------------

struct Camera {
    Vec3 position;
    F32 yaw; // radians about +Y; 0 looks down -Z at the prime meridian
    F32 pitch; // radians above the horizontal plane
};

struct CameraBasis {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
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
    // Multisampled colour target, resolved into the swapchain image.
    VkImage msaaImage = VK_NULL_HANDLE;
    VkDeviceMemory msaaMemory = VK_NULL_HANDLE;
    VkImageView msaaView = VK_NULL_HANDLE;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkDeviceAddress vertexAddress = 0;

    VkSemaphore acquire = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline spherePipeline = VK_NULL_HANDLE;
    VkPipeline linePipeline = VK_NULL_HANDLE;

    // Satellite positions as vec4 in world space, rewritten every frame.
    VkBuffer satelliteBuffer = VK_NULL_HANDLE;
    VkDeviceMemory satelliteMemory = VK_NULL_HANDLE;
    VkDeviceAddress satelliteAddress = 0;
    F32 *satellitePositions = nullptr;
    VkPipelineLayout satelliteLayout = VK_NULL_HANDLE;
    VkPipeline satellitePipeline = VK_NULL_HANDLE;

    // Link endpoints as vec4 pairs in world space, rewritten every frame.
    VkBuffer linkBuffer = VK_NULL_HANDLE;
    VkDeviceMemory linkMemory = VK_NULL_HANDLE;
    VkDeviceAddress linkAddress = 0;
    F32 *linkPoints = nullptr;
    VkPipeline linkPipeline = VK_NULL_HANDLE;
};

static Renderer r;
static Camera camera = {.position = {0.0f, 0.0f, 3.0f}, .yaw = 0.0f, .pitch = 0.0f};
Input input;

static F64 TimeRate() {
    return kTimeRates[kRealTimeRate + input.timeRateStep];
}

void StepTimeRate(const U32 character) {
    const I32 slowest = -kRealTimeRate;
    const I32 fastest = static_cast<I32>(Count(kTimeRates)) - 1 - kRealTimeRate;
    if (character == ']') {
        input.timeRateStep = Min(input.timeRateStep + 1, fastest);
    }
    if (character == '[') {
        input.timeRateStep = Max(input.timeRateStep - 1, slowest);
    }
}

void MoveMouse(const I32 x, const I32 y) {
    input.mouseX = x;
    input.mouseY = y;
    if (input.dragging || input.orbiting) {
        const I32 dx = x - input.lastMouseX;
        const I32 dy = y - input.lastMouseY;
        if (input.dragging) {
            input.mouseDx += dx;
            input.mouseDy += dy;
        }
        if (input.orbiting) {
            input.orbitDx += dx;
            input.orbitDy += dy;
        }
        input.lastMouseX = x;
        input.lastMouseY = y;
    }
}

constexpr F32 kFpsUpdateSeconds = 0.5f;
static U32 displayedFps = 0;
// The app renders at full resolution, so sizes meant for the screen, like
// text, minimum line widths and dots, are given in UI pixels and multiplied by
// uiScale. Updated when the window changes monitors.
F32 uiScale = 1.0f;
bool resizePending = false;
// Simulated UTC as Windows FILETIME ticks, advanced once per frame at the
// selected time rate. The Sun, the satellites and the clock all follow it.
static U64 frameUtcTicks = 0;
// ECEF orbit states, advanced by simulated time every frame.
static OrbitState satelliteStates[kSatelliteCount];
static GroundStation groundStations[kStationCount];
static Link links[kMaxLinks];
static U32 linkCount = 0;

// What the cursor is over: found each frame by PickHover, labelled by DrawOverlay.
enum HoverKind : U32 {
    kHoverNone,
    kHoverSatellite,
    kHoverStation,
    kHoverLink,
};

struct Hover {
    HoverKind kind;
    U32 index; // into satelliteStates, groundStations or links
};

static Hover hover;
// Picking distances from the cursor, in UI pixels. Dots win over links.
constexpr F32 kPickDotPixels = 8.0f;
constexpr F32 kPickLinkPixels = 5.0f;
// Points on the surface sit exactly on the globe; as in the shaders, this
// margin keeps them from counting as hidden behind it.
constexpr F32 kOcclusionMargin = 0.001f;

static U32 FindMemoryType(const U32 typeBits, const VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties memoryProps;
    vkGetPhysicalDeviceMemoryProperties(r.physicalDevice, &memoryProps);
    for (const U32 iType: SmallBitArray{typeBits}) {
        if ((memoryProps.memoryTypes[iType].propertyFlags & wanted) == wanted) {
            return iType;
        }
    }
    Die("finding a memory type", wanted);
}

static U64 FastFind64(void *p, const U64 n, const U64 v) {
    for (U64 i = 0; i < n; ++i) {
        if (static_cast<U64 *>(p)[i] == v) {
            return i;
        }
    }
    return static_cast<U64>(-1);
}

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
    constexpr VkSurfaceFormatKHR kPreferredFormat = {
        .format = VK_FORMAT_B8G8R8A8_SRGB,
        .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
    };
    static_assert(sizeof(VkSurfaceFormatKHR) == sizeof(U64));
    const U64 preferredIndex = FastFind64(formats, formatCount, __builtin_bit_cast(U64, kPreferredFormat));
    const VkSurfaceFormatKHR chosen = preferredIndex == ~0ull ? formats[0] : formats[preferredIndex];
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
    VK_CHECK(vkGetSwapchainImagesKHR(r.device, r.swapchain, &r.imageCount, r.images));
    for (U32 i = 0; i < r.imageCount; ++i) {
        const VkImageViewCreateInfo view = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = r.images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = r.format,
            .subresourceRange = kColorSubresource,
        };
        VK_CHECK(vkCreateImageView(r.device, &view, nullptr, &r.views[i]));

        constexpr VkSemaphoreCreateInfo kSem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(r.device, &kSem, nullptr, &r.renderDone[i]));
    }

    // The multisampled target follows the swapchain extent, so it is rebuilt
    // here with it. Destroying null handles is allowed on the first pass.
    vkDestroyImageView(r.device, r.msaaView, nullptr);
    vkDestroyImage(r.device, r.msaaImage, nullptr);
    vkFreeMemory(r.device, r.msaaMemory, nullptr);
    const VkImageCreateInfo msaaInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = r.format,
        .extent = {.width = r.extent.width, .height = r.extent.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = kSampleCount,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(r.device, &msaaInfo, nullptr, &r.msaaImage));
    VkMemoryRequirements msaaRequirements;
    vkGetImageMemoryRequirements(r.device, r.msaaImage, &msaaRequirements);
    const VkMemoryAllocateInfo msaaAlloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = msaaRequirements.size,
        .memoryTypeIndex = FindMemoryType(msaaRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VK_CHECK(vkAllocateMemory(r.device, &msaaAlloc, nullptr, &r.msaaMemory));
    VK_CHECK(vkBindImageMemory(r.device, r.msaaImage, r.msaaMemory, 0));
    const VkImageViewCreateInfo msaaView = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = r.msaaImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = r.format,
        .subresourceRange = kColorSubresource,
    };
    VK_CHECK(vkCreateImageView(r.device, &msaaView, nullptr, &r.msaaView));
}

#if defined(_WIN32)
#include "src/platform_win32.h"
#endif

static void InitVulkan() {
    constexpr VkApplicationInfo kApp = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = kAppName,
        .apiVersion = VK_API_VERSION_1_4,
    };

    char const *const kInstanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        kSurfaceExtensionName,
    };
    const VkInstanceCreateInfo instanceInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &kApp,
        .enabledExtensionCount = Count(kInstanceExtensions),
        .ppEnabledExtensionNames = kInstanceExtensions,
    };
    VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &r.instance));

    r.surface = CreateSurface(r.instance);

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
            VkBool32 presents = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devices[iDevice], iFamily, r.surface, &presents);
            if (!(families[iFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !presents) {
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
        // glslang compiles discard to demote-to-helper for Vulkan 1.3 targets,
        // which keeps derivatives valid around it. Required of every 1.3 device.
        .shaderDemoteToHelperInvocation = VK_TRUE,
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
// Creates a host-visible buffer that shaders read through its device address
// and returns it persistently mapped.
static void *CreateMappedBuffer(const U64 size, VkBuffer &buffer, VkDeviceMemory &memory,
                                VkDeviceAddress &address) {
    const VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(r.device, &bufferInfo, nullptr, &buffer));

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(r.device, buffer, &requirements);

    constexpr VkMemoryPropertyFlags kWanted =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const U32 memoryType = FindMemoryType(requirements.memoryTypeBits, kWanted);

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
    VK_CHECK(vkAllocateMemory(r.device, &allocInfo, nullptr, &memory));
    VK_CHECK(vkBindBufferMemory(r.device, buffer, memory, 0));

    void *mapped;
    VK_CHECK(vkMapMemory(r.device, memory, 0, size, 0, &mapped));
    const VkBufferDeviceAddressInfo addressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer,
    };
    address = vkGetBufferDeviceAddress(r.device, &addressInfo);
    return mapped;
}

static void CreateVertexBuffer() {
    // Expand every ring into line segments straight into the mapped buffer.
    U32 *dst = static_cast<U32 *>(CreateMappedBuffer(kVertexBufferSize, r.vertexBuffer, r.vertexMemory,
                                                     r.vertexAddress));
    U32 written = 0;
    for (const BorderRing &ring: kBorderRings) {
        for (U32 i = ring.first; i + 1 < ring.first + ring.count; ++i) {
            dst[written++] = PackBorderPoint(kBorderPoints[i + 0]);
            dst[written++] = PackBorderPoint(kBorderPoints[i + 1]);
        }
    }
    if (written != kSegmentPointCount) Die("expanding border rings", written);
    vkUnmapMemory(r.device, r.vertexMemory);
}

static void CreateSatelliteBuffer() {
    r.satellitePositions = static_cast<F32 *>(CreateMappedBuffer(kSatelliteBufferSize, r.satelliteBuffer,
                                                                 r.satelliteMemory, r.satelliteAddress));
    r.linkPoints = static_cast<F32 *>(CreateMappedBuffer(kLinkBufferSize, r.linkBuffer, r.linkMemory,
                                                         r.linkAddress));
}

static void WriteMarker(F32 *const dst, const Vec3d ecef, const F32 kind) {
    const Vec3 p = WorldFromEcef(ecef);
    dst[0] = p.x;
    dst[1] = p.y;
    dst[2] = p.z;
    dst[3] = kind;
}

// Stations never move in ECEF, so their markers are written once.
static void WriteStationMarkers() {
    for (U32 i = 0; i < kStationCount; ++i) {
        WriteMarker(r.satellitePositions + 4 * (kSatelliteCount + i), groundStations[i].position, 1.0f);
    }
}

static VkPipelineLayout CreatePushLayout(const U32 pushSize) {
    const VkPushConstantRange pushRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = pushSize,
    };
    const VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushRange,
    };
    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r.device, &layoutInfo, nullptr, &layout));
    return layout;
}

// Scene pipelines use MAX blending. The text overlay, the station links and
// the satellite and station dots use source-over, so they cover what is
// beneath them.
static VkPipeline CreatePipeline(const U32 *vertSpirv, const U64 vertSize, const U32 *fragSpirv,
                                 const U64 fragSize, const VkPipelineLayout layout,
                                 const bool sourceOver = false) {
    // maintenance5 (core 1.4): chain SPIR-V straight into the stage info,
    // no VkShaderModule objects needed.
    const VkShaderModuleCreateInfo vertCode = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vertSize,
        .pCode = vertSpirv,
    };
    const VkShaderModuleCreateInfo fragCode = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fragSize,
        .pCode = fragSpirv,
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
        .lineWidth = kRasterLineWidth,
    };
    constexpr VkPipelineMultisampleStateCreateInfo kMultisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = kSampleCount,
    };
    // The shaders output premultiplied colour with anti-aliased coverage and
    // the pipeline keeps the brighter of source and destination. Overlapping
    // pieces of the same line then saturate instead of stacking up, which
    // matters far away where many segments share a pixel. This works because
    // everything drawn is brighter than what it covers.
    const VkPipelineColorBlendAttachmentState blendAttachment = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = sourceOver ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE,
        .colorBlendOp = sourceOver ? VK_BLEND_OP_ADD : VK_BLEND_OP_MAX,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = sourceOver ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = sourceOver ? VK_BLEND_OP_ADD : VK_BLEND_OP_MAX,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blendAttachment,
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
        .layout = layout,
    };
    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    return pipeline;
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

#include "src/slug.h"
#include "src/text.h"

static CameraBasis ComputeBasis() {
    F32 sinYaw, cosYaw, sinPitch, cosPitch;
    SinCos(camera.yaw, sinYaw, cosYaw);
    SinCos(camera.pitch, sinPitch, cosPitch);
    const Vec3 forward = {sinYaw * cosPitch, sinPitch, -cosYaw * cosPitch};
    const Vec3 right = {cosYaw, 0.0f, sinYaw};
    return {.right = right, .up = Cross(right, forward), .forward = forward};
}

// Free-fly camera: drag the mouse to look, WASD to move along the view, Q/E
// down/up, Shift to go faster.
static void UpdateCamera(const F32 dt) {
    camera.yaw += static_cast<F32>(input.mouseDx) * kMouseRadiansPerPixel / uiScale;
    camera.pitch -= static_cast<F32>(input.mouseDy) * kMouseRadiansPerPixel / uiScale;
    camera.pitch = Clamp(camera.pitch, -kMaxPitch, kMaxPitch);
    input.mouseDx = 0;
    input.mouseDy = 0;

    // Middle-button orbit: turn the view exactly as a look drag would, and swing
    // the camera's position around the Earth's center by the same rotation. The
    // camera keeps its view of the globe while the globe turns under the
    // cursor. Scaling by altitude over distance keeps the ground moving at a
    // similar speed on screen at any height.
    if (input.orbitDx != 0 || input.orbitDy != 0) {
        const F32 distance = Length(camera.position);
        const F32 radiansPerPixel = kMouseRadiansPerPixel / uiScale * (distance - 1.0f) / distance;
        const F32 yawStep = static_cast<F32>(input.orbitDx) * radiansPerPixel;
        const F32 pitchStep = Clamp(camera.pitch - static_cast<F32>(input.orbitDy) * radiansPerPixel, -kMaxPitch,
                                    kMaxPitch) - camera.pitch;
        // Yaw turns about +Y.
        F32 sinYaw, cosYaw;
        SinCos(yawStep, sinYaw, cosYaw);
        const Vec3 p = camera.position;
        camera.position = {p.x * cosYaw - p.z * sinYaw, p.y, p.x * sinYaw + p.z * cosYaw};
        camera.yaw += yawStep;
        // Pitch turns about the camera's horizontal right axis (Rodrigues).
        const Vec3 right = ComputeBasis().right;
        F32 sinPitch, cosPitch;
        SinCos(pitchStep, sinPitch, cosPitch);
        const Vec3 q = camera.position;
        camera.position = Add(Add(Scale(q, cosPitch), Scale(Cross(right, q), sinPitch)),
                              Scale(right, Dot(right, q) * (1.0f - cosPitch)));
        camera.pitch += pitchStep;
        input.orbitDx = 0;
        input.orbitDy = 0;
    }

    // Scroll wheel: move straight toward or away from the Earth's center,
    // keeping the view direction. Dividing the altitude on the way in and
    // multiplying on the way out makes a notch in and a notch out cancel
    // exactly, and fractional notches from smooth wheels zoom proportionally.
    if (input.wheelNotches != 0.0f) {
        const F32 notches = input.wheelNotches;
        const F32 factor = notches > 0.0f ? 1.0f / (1.0f + kZoomPerNotch * notches) : 1.0f - kZoomPerNotch * notches;
        const F32 distance = Length(camera.position);
        // The ceiling only stops zooming out; flying farther with the keys is
        // still allowed, and zooming in from there must not snap back.
        const F32 ceiling = Max(kMaxAltitude, distance - 1.0f);
        const F32 altitude = Clamp((distance - 1.0f) * factor, kMinAltitude, ceiling);
        camera.position = Scale(camera.position, (1.0f + altitude) / distance);
        input.wheelNotches = 0.0f;
    }

    if (camera.yaw > kPi) {
        camera.yaw -= kTau;
    } else if (camera.yaw < -kPi) {
        camera.yaw += kTau;
    }

    const CameraBasis basis = ComputeBasis();
    Vec3 move{};
    if (input.keyDown['W']) {
        move = Add(move, basis.forward);
    }
    if (input.keyDown['S']) {
        move = Sub(move, basis.forward);
    }
    if (input.keyDown['D']) {
        move = Add(move, basis.right);
    }
    if (input.keyDown['A']) {
        move = Sub(move, basis.right);
    }
    if (input.keyDown['E']) {
        move = Add(move, basis.up);
    }
    if (input.keyDown['Q']) {
        move = Sub(move, basis.up);
    }
    if (Dot(move, move) > 0.0f) {
        const F32 altitude = Max(Length(camera.position) - 1.0f, kMinAltitude);
        const F32 boost = input.keyDown[kKeyShift] ? kBoost : 1.0f;
        const F32 speed = kSpeedPerAltitude * altitude * boost;
        camera.position = Add(camera.position, Scale(Normalize(move), speed * dt));
    }
    // Never sink below the surface; the sphere is drawn from outside only.
    const F32 distance = Length(camera.position);
    if (distance < 1.0f + kMinAltitude) {
        camera.position = Scale(camera.position, (1.0f + kMinAltitude) / distance);
    }
}

static PushConstants BuildPushConstants() {
    const CameraBasis basis = ComputeBasis();
    const F32 height = static_cast<F32>(r.extent.height);
    const F32 aspect = static_cast<F32>(r.extent.width) / height;
    // A point at view depth w spans 2w / (height * kFovScale) world units per pixel.
    const F32 unitsPerPixelPerDepth = 2.0f / (height * kFovScale);
    const F32 minHalfWidthPerDepth = 0.5f * kMinLineWidthPixels * uiScale * unitsPerPixelPerDepth;
    const F32 aaPaddingPerDepth = kLineAaPaddingPixels * unitsPerPixelPerDepth;
    const Vec3 sun = SunDirection(frameUtcTicks);
    return {
        .viewProj = Multiply(Perspective(kFovScale, aspect, kNearPlane),
                             View(camera.position, basis.right, basis.up, basis.forward)),
        .cameraPos = {camera.position.x, camera.position.y, camera.position.z, 1.0f},
        .line = {0.5f * kLineWidthWorld, minHalfWidthPerDepth, aaPaddingPerDepth, uiScale},
        .sunDirection = {sun.x, sun.y, sun.z, 0.0f},
        .vertexBuffer = r.vertexAddress,
    };
}

static SatellitePush BuildSatellitePush(const PushConstants &scene) {
    const F32 height = static_cast<F32>(r.extent.height);
    const F32 aspect = static_cast<F32>(r.extent.width) / height;
    return {
        .viewProj = scene.viewProj,
        .cameraPos = {scene.cameraPos[0], scene.cameraPos[1], scene.cameraPos[2], scene.cameraPos[3]},
        .sunDirection = {scene.sunDirection[0], scene.sunDirection[1], scene.sunDirection[2], scene.sunDirection[3]},
        .sprite = {kSatelliteRadiusWorld, uiScale * 2.0f / (height * kFovScale), kFovScale / aspect, kFovScale},
        .positions = r.satelliteAddress,
    };
}

// The shaders' visibility test: p is hidden when the line of sight from the
// camera meets the globe first, solving |eye + t (p - eye)| = 1 for t in (0, 1).
static bool HiddenByGlobe(const Vec3 eye, const Vec3 p) {
    const Vec3 d = Sub(p, eye);
    const F32 a = Dot(d, d);
    const F32 b = Dot(eye, d);
    const F32 discriminant = b * b - a * (Dot(eye, eye) - 1.0f);
    if (discriminant <= 0.0f) {
        return false;
    }
    const F32 t = (-b - Sqrt(discriminant)) / a;
    return t > 0.0f && t < 1.0f - kOcclusionMargin;
}

// Window pixel coordinates of a world point; w <= 0 means behind the camera.
static Vec4 ProjectToWindow(const Mat4 &viewProj, const Vec3 p) {
    const Vec4 clip = TransformPoint(viewProj, p);
    const F32 inverseW = clip.w > 0.0f ? 1.0f / clip.w : 0.0f;
    return {
        (clip.x * inverseW + 1.0f) * 0.5f * static_cast<F32>(r.extent.width),
        (clip.y * inverseW + 1.0f) * 0.5f * static_cast<F32>(r.extent.height),
        0.0f,
        clip.w,
    };
}

// Finds the visible satellite or station nearest the cursor, or failing that
// the nearest visible link, using the frame's own projection.
static void PickHover(const Mat4 &viewProj) {
    hover = {.kind = kHoverNone, .index = 0};
    if (!input.mouseInside || input.hideSatellites) {
        return;
    }
    const Vec3 eye = camera.position;
    const F32 mouseX = static_cast<F32>(input.mouseX) + 0.5f;
    const F32 mouseY = static_cast<F32>(input.mouseY) + 0.5f;

    const F32 dotRadius = kPickDotPixels * uiScale;
    F32 best = dotRadius * dotRadius;
    for (U32 i = 0; i < kMarkerCount; ++i) {
        const bool satellite = i < kSatelliteCount;
        const Vec3 p = WorldFromEcef(satellite
                                         ? satelliteStates[i].position
                                         : groundStations[i - kSatelliteCount].position);
        const Vec4 screen = ProjectToWindow(viewProj, p);
        if (screen.w <= 0.0f || HiddenByGlobe(eye, p)) {
            continue;
        }
        const F32 dx = screen.x - mouseX;
        const F32 dy = screen.y - mouseY;
        if (dx * dx + dy * dy < best) {
            best = dx * dx + dy * dy;
            hover = {
                .kind = satellite ? kHoverSatellite : kHoverStation,
                .index = satellite ? i : i - kSatelliteCount,
            };
        }
    }
    if (hover.kind != kHoverNone) {
        return;
    }

    const F32 linkRadius = kPickLinkPixels * uiScale;
    best = linkRadius * linkRadius;
    for (U32 i = 0; i < linkCount; ++i) {
        const Vec3 p0 = WorldFromEcef(groundStations[links[i].station].position);
        const Vec3 p1 = WorldFromEcef(satelliteStates[links[i].satellite].position);
        const Vec4 s0 = ProjectToWindow(viewProj, p0);
        const Vec4 s1 = ProjectToWindow(viewProj, p1);
        if (s0.w <= 0.0f || s1.w <= 0.0f) {
            continue;
        }
        // Nearest point on the segment as drawn on screen.
        const F32 ex = s1.x - s0.x;
        const F32 ey = s1.y - s0.y;
        const F32 length2 = Max(ex * ex + ey * ey, 1.0e-6f);
        const F32 t = Clamp(((mouseX - s0.x) * ex + (mouseY - s0.y) * ey) / length2, 0.0f, 1.0f);
        const F32 dx = s0.x + t * ex - mouseX;
        const F32 dy = s0.y + t * ey - mouseY;
        if (dx * dx + dy * dy >= best) {
            continue;
        }
        // The matching point along the 3D segment: screen space and world space
        // are related by perspective, so the parameter needs correcting.
        const F32 u = t * s0.w / (t * s0.w + (1.0f - t) * s1.w);
        if (HiddenByGlobe(eye, Add(p0, Scale(Sub(p1, p0), u)))) {
            continue;
        }
        best = dx * dx + dy * dy;
        hover = {.kind = kHoverLink, .index = i};
    }
}

static bool DrawFrame() {
    VK_CHECK(vkWaitForFences(r.device, 1, &r.fence, VK_TRUE, kWaitForever));

    // The fence guarantees the GPU has finished reading last frame's positions.
    for (U32 i = 0; i < kSatelliteCount; ++i) {
        WriteMarker(r.satellitePositions + 4 * i, satelliteStates[i].position, 0.0f);
    }
    for (U32 i = 0; i < linkCount; ++i) {
        WriteMarker(r.linkPoints + 8 * i, groundStations[links[i].station].position, 0.0f);
        WriteMarker(r.linkPoints + 8 * i + 4, satelliteStates[links[i].satellite].position, 0.0f);
    }

    U32 image;
    const VkResult acquired = vkAcquireNextImageKHR(r.device, r.swapchain, kWaitForever,
                                                    r.acquire, VK_NULL_HANDLE, &image);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        CreateSwapchain();
        return false;
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

    // Both the multisampled target and the swapchain image (the resolve
    // destination) start each frame with undefined contents.
    TransitionImage(r.msaaImage, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    TransitionImage(r.images[image], VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    // Draw into the multisampled image and resolve into the swapchain image
    // when rendering ends; the samples themselves are never stored.
    const VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = r.msaaView,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT,
        .resolveImageView = r.views[image],
        .resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
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

    const PushConstants push = BuildPushConstants();
    PickHover(push.viewProj);
    vkCmdPushConstants(r.cmd, r.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    const VkViewport viewport = {
        .x = 0.0f, .y = 0.0f,
        .width = static_cast<F32>(r.extent.width), .height = static_cast<F32>(r.extent.height),
        .minDepth = 0.0f, .maxDepth = 1.0f
    };
    vkCmdSetViewportWithCount(r.cmd, 1, &viewport);
    const VkRect2D scissor{.offset = {.x = 0, .y = 0}, .extent = r.extent};
    vkCmdSetScissorWithCount(r.cmd, 1, &scissor);

    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.spherePipeline);
    vkCmdDraw(r.cmd, kFullScreenVertexCount, kInstanceCount, 0, 0);
    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.linePipeline);
    vkCmdDraw(r.cmd, kDrawVertexCount, kInstanceCount, 0, 0);

    // Links share the scene layout, with their own widths and endpoints. They
    // are drawn after the borders with source-over blending, so they sit on top.
    if (!input.hideSatellites && linkCount > 0) {
        const F32 unitsPerPixelPerDepth = 2.0f / (static_cast<F32>(r.extent.height) * kFovScale);
        PushConstants linkPush = push;
        linkPush.line[0] = 0.5f * kLinkWidthWorld;
        linkPush.line[1] = 0.5f * kMinLinkWidthPixels * uiScale * unitsPerPixelPerDepth;
        linkPush.vertexBuffer = r.linkAddress;
        vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.linkPipeline);
        vkCmdPushConstants(r.cmd, r.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(linkPush), &linkPush);
        vkCmdDraw(r.cmd, kVerticesPerSegment * linkCount, kInstanceCount, 0, 0);
    }

    if (!input.hideSatellites) {
        const SatellitePush satellitePush = BuildSatellitePush(push);
        vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.satellitePipeline);
        vkCmdPushConstants(r.cmd, r.satelliteLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(satellitePush), &satellitePush);
        vkCmdDraw(r.cmd, kVerticesPerSprite * kMarkerCount, kInstanceCount, 0, 0);
    }

    DrawOverlay();

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
    return presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR;
}

int main() {
    CreateAppWindow();
    InitVulkan();
    CreateVertexBuffer();
    CreateSatelliteBuffer();
    InitConstellation(satelliteStates);
    InitGroundStations(groundStations);
    WriteStationMarkers();
    r.pipelineLayout = CreatePushLayout(sizeof(PushConstants));
    r.satelliteLayout = CreatePushLayout(sizeof(SatellitePush));
    r.spherePipeline = CreatePipeline(kSphereVert, sizeof(kSphereVert),
                                      kSphereFrag, sizeof(kSphereFrag), r.pipelineLayout);
    r.linePipeline = CreatePipeline(kEarthVert, sizeof(kEarthVert),
                                    kEarthFrag, sizeof(kEarthFrag), r.pipelineLayout);
    r.satellitePipeline = CreatePipeline(kSatelliteVert, sizeof(kSatelliteVert),
                                         kSatelliteFrag, sizeof(kSatelliteFrag), r.satelliteLayout, true);
    r.linkPipeline = CreatePipeline(kLinkVert, sizeof(kLinkVert),
                                    kLinkFrag, sizeof(kLinkFrag), r.pipelineLayout, true);
    InitText();

    const U64 frequency = CounterFrequency();
    U64 lastTime = Counter();
    U64 fpsStart = lastTime;
    U32 fpsFrames = 0;
    // Simulated time starts at the real UTC time. Unlike lastTime, its counter
    // is not reset after a minimize, so the simulation keeps running then too.
    const U64 simulationStartTicks = UtcFileTimeTicks();
    F64 simulationSeconds = 0.0;
    U64 simulationClock = lastTime;

    for (;;) {
        if (!PumpEvents()) {
            vkDeviceWaitIdle(r.device);
            DestroyText();
            return 0;
        }

        if (!WindowVisible()) {
            WaitForEvents(); // minimized: block instead of spinning
            lastTime = Counter();
            fpsStart = lastTime;
            fpsFrames = 0;
            continue;
        }
        if (resizePending) {
            resizePending = false;
            CreateSwapchain();
        }

        const U64 now = Counter();
        const F32 dt = static_cast<F32>(now - lastTime) / static_cast<F32>(frequency);
        lastTime = now;
        UpdateCamera(Min(dt, kMaxFrameSeconds));
        const F64 realSeconds = static_cast<F64>(now - simulationClock) / static_cast<F64>(frequency);
        simulationClock = now;
        const F64 simulatedSeconds = realSeconds * TimeRate();
        simulationSeconds += simulatedSeconds;
        PropagateConstellation(satelliteStates, kSatelliteCount, simulatedSeconds);
        linkCount = FindLinks(groundStations, satelliteStates, links);
        frameUtcTicks = simulationStartTicks + static_cast<U64>(simulationSeconds * static_cast<F64>(kTicksPerSecond));
        if (DrawFrame()) {
            ++fpsFrames;
        }
        const U64 frameEnd = Counter();
        const F32 fpsSeconds = static_cast<F32>(frameEnd - fpsStart) / static_cast<F32>(frequency);
        if (fpsSeconds >= kFpsUpdateSeconds) {
            const F32 fps = static_cast<F32>(fpsFrames) / fpsSeconds;
            displayedFps = static_cast<U32>(Min(fps + 0.5f, 99999.0f));
            fpsFrames = 0;
            fpsStart = frameEnd;
        }
    }
}
