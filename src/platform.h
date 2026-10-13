#pragma once

#include <vulkan/vulkan_core.h>

#include "src/core.h"

// The boundary between main.cc and the platform layer: the window, input,
// clocks and Vulkan surface. platform_win32.h and platform_linux.h
// implement it inside main.cc's translation unit; platform_macos.mm is its own
// Objective-C++ one. The platform layer keeps no state of its own: main.cc
// owns a Platform and an Input and passes them in.

constexpr U32 kMaxKeys = 256;
// Input::keyDown holds upper-case ASCII letters and this, the Windows VK_SHIFT.
constexpr U32 kKeyShift = 0x10;
// Window client size in UI pixels.
constexpr U32 kWindowWidth = 1280;
constexpr U32 kWindowHeight = 720;

// Everything the platform layer reports to main.cc.
struct Input {
    bool keyDown[kMaxKeys];
    bool dragging;
    I32 lastMouseX;
    I32 lastMouseY;
    I32 mouseDx;
    I32 mouseDy;
    // Toggled with H. Hidden satellites keep propagating, so they reappear in
    // their real-time positions.
    bool hideSatellites;
    // Cursor position in window pixels, meaningful while mouseInside.
    I32 mouseX;
    I32 mouseY;
    bool mouseInside;
    // Steps through kTimeRates relative to real time, so zero is 1x.
    I32 timeRateStep;
    // Middle-button drag, which orbits the camera around the Earth's center.
    bool orbiting;
    I32 orbitDx;
    I32 orbitDy;
    // Accumulated wheel rotation in notches; smooth wheels and trackpads give
    // fractions.
    F32 wheelNotches;
    // Physical pixels per UI pixel, a pixel at 100% Windows display scaling or a
    // macOS point. The app renders at full resolution, so sizes meant for the
    // screen, like text, minimum line widths and dots, are given in UI pixels
    // and multiplied by this.
    F32 uiScale;
    // Set when the swapchain no longer matches the window and presenting would
    // not report it.
    bool resizePending;
};

// The window and its connection to the system. Each platform layer defines its
// own; macOS shares it between two translation units, so it is defined here.
#if defined(__APPLE__)
struct Platform {
    // NSWindow, NSView, CAMetalLayer and the window delegate, retained by
    // bridged casts in platform_macos.mm.
    void *window;
    void *view;
    void *layer;
    void *windowDelegate;
    U32 drawableWidth; // pixels
    U32 drawableHeight;
    bool closed;
};
#else
struct Platform;
#endif

// Defined in main.cc.
[[noreturn]] void Die(char const *what, I64 code);

// ] steps the simulation rate up and [ steps it down.
void StepTimeRate(Input &input, U32 character);

// The cursor moved to (x, y) in window pixels. While a button is held the
// movement turns into look or orbit deltas.
void MoveMouse(Input &input, I32 x, I32 y);

// Defined by the platform layer.
extern char const *const kSurfaceExtensionName;


void CreateAppWindow(Platform &platform, Input &input);

VkSurfaceKHR CreateSurface(const Platform &platform, VkInstance instance);

// Handles every queued event. Returns false once the window is closed.
bool PumpEvents(Platform &platform, Input &input);

// False while minimized, when there is nothing to draw into.
bool WindowVisible(const Platform &platform);

// Blocks until an event arrives.
void WaitForEvents(const Platform &platform);

U64 Counter();

U64 CounterFrequency();

// The current UTC time as Windows FILETIME ticks.
U64 UtcFileTimeTicks();
