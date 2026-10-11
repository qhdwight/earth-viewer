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

static NSWindow *window;
static NSView *view;
static CAMetalLayer *layer;
static U32 drawableWidth; // pixels
static U32 drawableHeight;
static bool closed;

// Closing only sets a flag; main.cc sees it and exits its loop.
@interface EarthViewerWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation EarthViewerWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    closed = true;
    return NO;
}
@end

static EarthViewerWindowDelegate *windowDelegate;

// Follows the window's size and backing scale. A changed drawable size asks
// main.cc for a new swapchain, since the surface reports the drawable size.
static void UpdateDrawableSize() {
    const CGFloat scale = window.backingScaleFactor;
    if (static_cast<F32>(scale) != uiScale) {
        uiScale = static_cast<F32>(scale);
        layer.contentsScale = scale;
    }
    const NSSize size = view.bounds.size;
    const U32 width = static_cast<U32>(size.width * scale + 0.5);
    const U32 height = static_cast<U32>(size.height * scale + 0.5);
    if (width != drawableWidth || height != drawableHeight) {
        drawableWidth = width;
        drawableHeight = height;
        layer.drawableSize = CGSizeMake(width, height);
        resizePending = true;
    }
}

void CreateAppWindow() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        // The Dock draws icons at up to 512 pixels.
        NSData *const iconData = [NSData dataWithBytes:kIconPng512 length:sizeof(kIconPng512)];
        NSApp.applicationIconImage = [[NSImage alloc] initWithData:iconData];

        // The content size is in points, which match UI pixels.
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kWindowWidth, kWindowHeight)
                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                       NSWindowStyleMaskMiniaturizable |
                                                       NSWindowStyleMaskResizable
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        if (!window) {
            Die("creating the window", 0);
        }
        windowDelegate = [[EarthViewerWindowDelegate alloc] init];
        window.releasedWhenClosed = NO;
        window.delegate = windowDelegate;
        window.title = @"Earth Viewer";
        [window center];

        // A layer-hosting view: the CAMetalLayer is set before wantsLayer.
        view = window.contentView;
        layer = [CAMetalLayer layer];
        layer.opaque = YES;
        view.layer = layer;
        view.wantsLayer = YES;
        UpdateDrawableSize();
        resizePending = false;

        [window makeKeyAndOrderFront:nil];
        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

VkSurfaceKHR CreateSurface(const VkInstance instance) {
    const VkMetalSurfaceCreateInfoEXT surfaceInfo = {
        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pLayer = layer,
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

static bool InView(const NSPoint location) {
    return NSPointInRect(location, view.bounds);
}

// Applies one event to the input state. Returns whether AppKit should also
// see it: key events are kept from it, since unhandled ones beep.
static bool HandleEvent(NSEvent *const event) {
    const NSEventType type = event.type;
    const NSEventModifierFlags modifiers = event.modifierFlags;
    const bool inView = event.window == window && InView(event.locationInWindow);
    if (type == NSEventTypeKeyDown) {
        const unichar key = KeyCharacter(event.charactersIgnoringModifiers);
        if (key == kCharEscape || ((modifiers & NSEventModifierFlagCommand) != 0 && key == 'Q')) {
            closed = true;
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
            StepTimeRate(KeyCharacter(event.characters));
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
static void PollMouse() {
    const NSPoint location = window.mouseLocationOutsideOfEventStream;
    input.mouseInside = InView(location);
    const CGFloat x = location.x * uiScale;
    const CGFloat y = (view.bounds.size.height - location.y) * uiScale;
    MoveMouse(static_cast<I32>(x < 0.0 ? x - 1.0 : x), static_cast<I32>(y < 0.0 ? y - 1.0 : y));
}

bool PumpEvents() {
    @autoreleasepool {
        PollMouse();
        for (;;) {
            NSEvent *const event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                      untilDate:NSDate.distantPast
                                                         inMode:NSDefaultRunLoopMode
                                                        dequeue:YES];
            if (!event) {
                break;
            }
            if (HandleEvent(event)) {
                [NSApp sendEvent:event];
            }
        }
        [NSApp updateWindows];
        if (!window.isKeyWindow) {
            for (bool &down: input.keyDown) {
                down = false;
            }
        }
        UpdateDrawableSize();
    }
    return !closed;
}

bool WindowVisible() {
    return !window.isMiniaturized && drawableWidth > 0 && drawableHeight > 0;
}

// Leaves the event queued for PumpEvents.
void WaitForEvents() {
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
