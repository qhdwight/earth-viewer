// The platform layer for macOS (see platform.h), in Objective-C++ with ARC.

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <mach/mach_time.h>
#include <sys/time.h>

#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#include "src/clock.h"
#include "src/icon.h"
#include "src/platform.h"

constexpr unichar kCharEscape = 27;
constexpr NSInteger kMiddleButton = 2;
// Trackpads scroll in points rather than wheel notches.
constexpr CGFloat kScrollPointsPerNotch = 10.0;

char const *const kSurfaceExtensionName = VK_EXT_METAL_SURFACE_EXTENSION_NAME;

// The AppKit objects in a Platform. They are retained for the program's
// lifetime by the bridged casts in CreateAppWindow.
static NSWindow *PlatformWindow(const Platform &platform) {
    return (__bridge NSWindow *)platform.window;
}

static NSView *PlatformView(const Platform &platform) {
    return (__bridge NSView *)platform.view;
}

static CAMetalLayer *PlatformLayer(const Platform &platform) {
    return (__bridge CAMetalLayer *)platform.layer;
}

// Closing only sets a flag; main.cc sees it and exits its loop.
@interface EarthViewerWindowDelegate : NSObject <NSWindowDelegate> {
@public
    Platform *platform;
}
@end

@implementation EarthViewerWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    platform->closed = true;
    return NO;
}
@end

// Follows the window's size and backing scale. A changed drawable size asks
// main.cc for a new swapchain, since the surface reports the drawable size.
static void UpdateDrawableSize(Platform &platform, Input &input) {
    CAMetalLayer *const layer = PlatformLayer(platform);
    const CGFloat scale = PlatformWindow(platform).backingScaleFactor;
    if (static_cast<F32>(scale) != input.uiScale) {
        input.uiScale = static_cast<F32>(scale);
        layer.contentsScale = scale;
    }
    const NSSize size = PlatformView(platform).bounds.size;
    const U32 width = static_cast<U32>(size.width * scale + 0.5);
    const U32 height = static_cast<U32>(size.height * scale + 0.5);
    if (width != platform.drawableWidth || height != platform.drawableHeight) {
        platform.drawableWidth = width;
        platform.drawableHeight = height;
        layer.drawableSize = CGSizeMake(width, height);
        input.resizePending = true;
    }
}

void CreateAppWindow(Platform &platform, Input &input) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        // The Dock draws icons at up to 512 pixels.
        NSData *const iconData = [NSData dataWithBytes:kIconPng512 length:sizeof(kIconPng512)];
        NSApp.applicationIconImage = [[NSImage alloc] initWithData:iconData];

        // The content size is in points, which match UI pixels.
        NSWindow *const window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kWindowWidth, kWindowHeight)
                                                             styleMask:NSWindowStyleMaskTitled |
                                                                       NSWindowStyleMaskClosable |
                                                                       NSWindowStyleMaskMiniaturizable |
                                                                       NSWindowStyleMaskResizable
                                                               backing:NSBackingStoreBuffered
                                                                 defer:NO];
        if (!window) {
            Die("creating the window", 0);
        }
        // The window holds its delegate weakly; the Platform keeps it alive.
        EarthViewerWindowDelegate *const windowDelegate = [[EarthViewerWindowDelegate alloc] init];
        windowDelegate->platform = &platform;
        window.releasedWhenClosed = NO;
        window.delegate = windowDelegate;
        window.title = @"Earth Viewer";
        [window center];

        // A layer-hosting view: the CAMetalLayer is set before wantsLayer.
        NSView *const view = window.contentView;
        CAMetalLayer *const layer = [CAMetalLayer layer];
        layer.opaque = YES;
        view.layer = layer;
        view.wantsLayer = YES;

        platform.window = (__bridge_retained void *)window;
        platform.view = (__bridge_retained void *)view;
        platform.layer = (__bridge_retained void *)layer;
        platform.windowDelegate = (__bridge_retained void *)windowDelegate;
        UpdateDrawableSize(platform, input);
        input.resizePending = false;

        [window makeKeyAndOrderFront:nil];
        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

VkSurfaceKHR CreateSurface(const Platform &platform, const VkInstance instance) {
    const VkMetalSurfaceCreateInfoEXT surfaceInfo = {
        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pLayer = PlatformLayer(platform),
    };
    VkSurfaceKHR surface;
    const VkResult result = vkCreateMetalSurfaceEXT(instance, &surfaceInfo, nullptr, &surface);
    if (result != VK_SUCCESS) {
        Die("vkCreateMetalSurfaceEXT", result);
    }
    return surface;
}

// The first UTF-16 unit of a key's characters, upper-cased for ASCII letters
// so key state matches main.cc's 'W', 'A', ... indices.
static unichar KeyCharacter(NSString *const characters) {
    if (characters.length == 0) {
        return 0;
    }
    const unichar character = [characters characterAtIndex:0];
    return character >= 'a' && character <= 'z' ? static_cast<unichar>(character - 'a' + 'A') : character;
}

static bool InView(const Platform &platform, const NSPoint location) {
    return NSPointInRect(location, PlatformView(platform).bounds);
}

// Applies one event to the input state. Returns whether AppKit should also
// see it: key events are kept from it, since unhandled ones beep.
static bool HandleEvent(Platform &platform, Input &input, NSEvent *const event) {
    const NSEventType type = event.type;
    const NSEventModifierFlags modifiers = event.modifierFlags;
    const bool inView = event.window == PlatformWindow(platform) && InView(platform, event.locationInWindow);
    if (type == NSEventTypeKeyDown) {
        const unichar key = KeyCharacter(event.charactersIgnoringModifiers);
        if (key == kCharEscape || ((modifiers & NSEventModifierFlagCommand) != 0 && key == 'Q')) {
            platform.closed = true;
        }
        // Keys released while Command is held send no key up, so Command
        // shortcuts never count as held keys.
        if ((modifiers & NSEventModifierFlagCommand) != 0) {
            return false;
        }
        if (key == 'H' && !event.isARepeat) {
            input.hideSatellites = !input.hideSatellites;
        }
        if (!event.isARepeat) {
            // Characters rather than keys, so [ and ] work on any layout.
            StepTimeRate(input, KeyCharacter(event.characters));
        }
        if (key < kMaxKeys) {
            input.keyDown[key] = true;
        }
        return false;
    }
    if (type == NSEventTypeKeyUp) {
        const unichar key = KeyCharacter(event.charactersIgnoringModifiers);
        if (key < kMaxKeys) {
            input.keyDown[key] = false;
        }
        return false;
    }
    if (type == NSEventTypeFlagsChanged) {
        input.keyDown[kKeyShift] = (modifiers & NSEventModifierFlagShift) != 0;
    }
    // Drags start only inside the view, not on the title bar. AppKit keeps
    // sending a drag's events to the window after the cursor leaves it.
    if ((type == NSEventTypeLeftMouseDown || type == NSEventTypeRightMouseDown) && inView) {
        input.dragging = true;
        input.lastMouseX = input.mouseX;
        input.lastMouseY = input.mouseY;
    }
    if (type == NSEventTypeOtherMouseDown && inView && event.buttonNumber == kMiddleButton) {
        input.orbiting = true;
        input.lastMouseX = input.mouseX;
        input.lastMouseY = input.mouseY;
    }
    if (type == NSEventTypeLeftMouseUp || type == NSEventTypeRightMouseUp) {
        input.dragging = false;
    }
    if (type == NSEventTypeOtherMouseUp && event.buttonNumber == kMiddleButton) {
        input.orbiting = false;
    }
    if (type == NSEventTypeScrollWheel && inView) {
        // Positive is scrolling up, toward zooming in, as on Windows.
        const CGFloat delta = event.scrollingDeltaY;
        input.wheelNotches += static_cast<F32>(event.hasPreciseScrollingDeltas ? delta / kScrollPointsPerNotch : delta);
    }
    return true;
}

// Reads the cursor in window pixels, top left origin like Windows.
static void PollMouse(const Platform &platform, Input &input) {
    const NSPoint location = PlatformWindow(platform).mouseLocationOutsideOfEventStream;
    input.mouseInside = InView(platform, location);
    const CGFloat x = location.x * input.uiScale;
    const CGFloat y = (PlatformView(platform).bounds.size.height - location.y) * input.uiScale;
    MoveMouse(input, static_cast<I32>(x < 0.0 ? x - 1.0 : x), static_cast<I32>(y < 0.0 ? y - 1.0 : y));
}

bool PumpEvents(Platform &platform, Input &input) {
    @autoreleasepool {
        PollMouse(platform, input);
        for (;;) {
            NSEvent *const event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                      untilDate:NSDate.distantPast
                                                         inMode:NSDefaultRunLoopMode
                                                        dequeue:YES];
            if (!event) {
                break;
            }
            if (HandleEvent(platform, input, event)) {
                [NSApp sendEvent:event];
            }
        }
        [NSApp updateWindows];
        if (!PlatformWindow(platform).isKeyWindow) {
            for (bool &down: input.keyDown) {
                down = false;
            }
        }
        UpdateDrawableSize(platform, input);
    }
    return !platform.closed;
}

bool WindowVisible(const Platform &platform) {
    return !PlatformWindow(platform).isMiniaturized && platform.drawableWidth > 0 && platform.drawableHeight > 0;
}

// Leaves the event queued for PumpEvents.
void WaitForEvents(const Platform &) {
    @autoreleasepool {
        [NSApp nextEventMatchingMask:NSEventMaskAny
                           untilDate:NSDate.distantFuture
                              inMode:NSDefaultRunLoopMode
                             dequeue:NO];
    }
}

U64 Counter() {
    return mach_absolute_time();
}

U64 CounterFrequency() {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    return 1000000000ull * timebase.denom / timebase.numer;
}

U64 UtcFileTimeTicks() {
    timeval now;
    gettimeofday(&now, nullptr);
    return (static_cast<U64>(now.tv_sec) + kUnixEpochFileTimeSeconds) * kTicksPerSecond +
           static_cast<U64>(now.tv_usec) * (kTicksPerSecond / 1000000);
}
