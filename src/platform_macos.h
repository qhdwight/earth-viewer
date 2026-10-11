#pragma once

// Included by main.cc after its globals. The window, input, clocks and
// Vulkan surface for macOS; platform_win32.h provides the same functions.
//
// AppKit is driven through the Objective-C runtime's C interface, so this
// stays ordinary C++. Every message send goes through Msg, which casts
// objc_msgSend to the method's real signature. That is only correct on arm64,
// where structs such as CGRect come back in registers.

#if !defined(__aarch64__)
#error "macOS builds target Apple silicon"
#endif

constexpr char const *kSurfaceExtensionName = VK_EXT_METAL_SURFACE_EXTENSION_NAME;

// AppKit enumerations, which live in Objective-C headers.
constexpr long kActivationPolicyRegular = 0;
constexpr unsigned long kWindowStyle = 1 | 2 | 4 | 8; // titled, closable, miniaturizable, resizable
constexpr unsigned long kBackingStoreBuffered = 2;
constexpr unsigned long long kEventMaskAny = ~0ull;
constexpr unsigned long kEventLeftMouseDown = 1;
constexpr unsigned long kEventLeftMouseUp = 2;
constexpr unsigned long kEventRightMouseDown = 3;
constexpr unsigned long kEventRightMouseUp = 4;
constexpr unsigned long kEventKeyDown = 10;
constexpr unsigned long kEventKeyUp = 11;
constexpr unsigned long kEventFlagsChanged = 12;
constexpr unsigned long kEventScrollWheel = 22;
constexpr unsigned long kEventOtherMouseDown = 25;
constexpr unsigned long kEventOtherMouseUp = 26;
constexpr long kMiddleButton = 2;
constexpr unsigned long kModifierShift = 1ul << 17;
constexpr unsigned long kModifierCommand = 1ul << 20;
constexpr U16 kCharEscape = 27;
// Trackpads scroll in points rather than wheel notches.
constexpr F64 kScrollPointsPerNotch = 10.0;

template<typename R = id, typename... A>
static R Msg(const void *const object, char const *const selector, const A... args) {
    using Send = R (*)(const void *, SEL, A...);
    return reinterpret_cast<Send>(objc_msgSend)(object, sel_registerName(selector), args...);
}

static id ObjcClass(char const *const name) {
    return reinterpret_cast<id>(objc_getClass(name));
}

struct MacWindow {
    id app;
    id window;
    id view;
    id layer; // CAMetalLayer backing the content view
    id distantPast;
    id distantFuture;
    U32 width; // drawable size in pixels
    U32 height;
    bool closed;
};

static MacWindow mac;

// NSWindowDelegate windowShouldClose:. The main loop sees the flag and exits.
static BOOL WindowShouldClose(id, SEL, id) {
    mac.closed = true;
    return NO;
}

// Follows the window's size and backing scale. A changed drawable size asks
// main.cc for a new swapchain, since the surface reports the drawable size.
static void UpdateDrawableSize() {
    const F64 scale = Msg<F64>(mac.window, "backingScaleFactor");
    if (static_cast<F32>(scale) != uiScale) {
        uiScale = static_cast<F32>(scale);
        Msg<void>(mac.layer, "setContentsScale:", scale);
    }
    const CGRect bounds = Msg<CGRect>(mac.view, "bounds");
    const U32 width = static_cast<U32>(bounds.size.width * scale + 0.5);
    const U32 height = static_cast<U32>(bounds.size.height * scale + 0.5);
    if (width != mac.width || height != mac.height) {
        mac.width = width;
        mac.height = height;
        Msg<void>(mac.layer, "setDrawableSize:", CGSize{static_cast<F64>(width), static_cast<F64>(height)});
        resizePending = true;
    }
}

static void CreateAppWindow() {
    mac.app = Msg(ObjcClass("NSApplication"), "sharedApplication");
    Msg<void>(mac.app, "setActivationPolicy:", kActivationPolicyRegular);
    const id iconData = Msg(ObjcClass("NSData"), "dataWithBytes:length:", static_cast<const void *>(kIconPng64),
                            static_cast<unsigned long>(sizeof(kIconPng64)));
    const id icon = Msg(Msg(ObjcClass("NSImage"), "alloc"), "initWithData:", iconData);
    Msg<void>(mac.app, "setApplicationIconImage:", icon);

    Class delegateClass = objc_allocateClassPair(objc_getClass("NSObject"), "EarthViewerWindowDelegate", 0);
    class_addMethod(delegateClass, sel_registerName("windowShouldClose:"), reinterpret_cast<IMP>(WindowShouldClose),
                    "B@:@");
    objc_registerClassPair(delegateClass);
    const id delegate = Msg(Msg(reinterpret_cast<id>(delegateClass), "alloc"), "init");

    // The content size is in points, which match UI pixels.
    const CGRect frame = {{0.0, 0.0}, {static_cast<F64>(kWindowWidth), static_cast<F64>(kWindowHeight)}};
    mac.window = Msg(Msg(ObjcClass("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", frame,
                     kWindowStyle, kBackingStoreBuffered, static_cast<BOOL>(NO));
    if (!mac.window) Die("creating the window", 0);
    // Closing only sets a flag; the window must outlive the loop that sees it.
    Msg<void>(mac.window, "setReleasedWhenClosed:", static_cast<BOOL>(NO));
    Msg<void>(mac.window, "setDelegate:", delegate);
    Msg<void>(mac.window, "setTitle:", Msg(ObjcClass("NSString"), "stringWithUTF8String:", "Earth Viewer"));
    Msg<void>(mac.window, "center");

    // A layer-hosting view: the CAMetalLayer is set before wantsLayer.
    mac.view = Msg(mac.window, "contentView");
    mac.layer = Msg(Msg(ObjcClass("CAMetalLayer"), "layer"), "retain");
    Msg<void>(mac.view, "setLayer:", mac.layer);
    Msg<void>(mac.view, "setWantsLayer:", static_cast<BOOL>(YES));
    Msg<void>(mac.layer, "setOpaque:", static_cast<BOOL>(YES));
    UpdateDrawableSize();
    resizePending = false;

    mac.distantPast = Msg(Msg(ObjcClass("NSDate"), "distantPast"), "retain");
    mac.distantFuture = Msg(Msg(ObjcClass("NSDate"), "distantFuture"), "retain");
    Msg<void>(mac.window, "makeKeyAndOrderFront:", static_cast<id>(nullptr));
    Msg<void>(mac.app, "finishLaunching");
    Msg<void>(mac.app, "activateIgnoringOtherApps:", static_cast<BOOL>(YES));
}

static void CreateSurface() {
    const VkMetalSurfaceCreateInfoEXT surfaceInfo = {
        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pLayer = mac.layer,
    };
    VK_CHECK(vkCreateMetalSurfaceEXT(r.instance, &surfaceInfo, nullptr, &r.surface));
}

// The first UTF-16 unit of a key event's characters, upper-cased for ASCII
// letters so key state matches main.cc's 'W', 'A', ... indices.
static U16 EventCharacter(const id event, char const *const selector) {
    const id characters = Msg(event, selector);
    if (Msg<unsigned long>(characters, "length") == 0) {
        return 0;
    }
    const U16 character = Msg<U16>(characters, "characterAtIndex:", 0ul);
    return character >= 'a' && character <= 'z' ? static_cast<U16>(character - 'a' + 'A') : character;
}

// Applies one event to the input state. Returns whether AppKit should also
// see it: key events are kept from it, since unhandled ones beep.
static bool HandleEvent(const id event) {
    const unsigned long type = Msg<unsigned long>(event, "type");
    const unsigned long modifiers = Msg<unsigned long>(event, "modifierFlags");
    const bool inWindow = Msg(event, "window") == mac.window;
    const CGPoint location = Msg<CGPoint>(event, "locationInWindow");
    const CGRect bounds = Msg<CGRect>(mac.view, "bounds");
    const bool inView = inWindow && location.x >= 0.0 && location.y >= 0.0 && location.x < bounds.size.width &&
                        location.y < bounds.size.height;
    if (type == kEventKeyDown) {
        const U16 key = EventCharacter(event, "charactersIgnoringModifiers");
        const bool repeat = Msg<BOOL>(event, "isARepeat");
        if (key == kCharEscape || ((modifiers & kModifierCommand) != 0 && key == 'Q')) {
            mac.closed = true;
        }
        // Keys released while Command is held send no key up, so Command
        // shortcuts never count as held keys.
        if ((modifiers & kModifierCommand) != 0) {
            return false;
        }
        if (key == 'H' && !repeat) {
            input.hideSatellites = !input.hideSatellites;
        }
        if (!repeat) {
            // Characters rather than keys, so [ and ] work on any layout.
            StepTimeRate(EventCharacter(event, "characters"));
        }
        if (key < kMaxKeys) {
            input.keyDown[key] = true;
        }
        return false;
    }
    if (type == kEventKeyUp) {
        const U16 key = EventCharacter(event, "charactersIgnoringModifiers");
        if (key < kMaxKeys) {
            input.keyDown[key] = false;
        }
        return false;
    }
    if (type == kEventFlagsChanged) {
        input.keyDown[kKeyShift] = (modifiers & kModifierShift) != 0;
    }
    // Drags start only inside the view, not on the title bar. AppKit keeps
    // sending a drag's events to the window after the cursor leaves it.
    if ((type == kEventLeftMouseDown || type == kEventRightMouseDown) && inView) {
        input.dragging = true;
        input.lastMouseX = input.mouseX;
        input.lastMouseY = input.mouseY;
    }
    if (type == kEventOtherMouseDown && inView && Msg<long>(event, "buttonNumber") == kMiddleButton) {
        input.orbiting = true;
        input.lastMouseX = input.mouseX;
        input.lastMouseY = input.mouseY;
    }
    if (type == kEventLeftMouseUp || type == kEventRightMouseUp) {
        input.dragging = false;
    }
    if (type == kEventOtherMouseUp && Msg<long>(event, "buttonNumber") == kMiddleButton) {
        input.orbiting = false;
    }
    if (type == kEventScrollWheel && inView) {
        // Positive is scrolling up, toward zooming in, as on Windows.
        const F64 delta = Msg<F64>(event, "scrollingDeltaY");
        const bool precise = Msg<BOOL>(event, "hasPreciseScrollingDeltas");
        input.wheelNotches += static_cast<F32>(precise ? delta / kScrollPointsPerNotch : delta);
    }
    return true;
}

// Reads the cursor in window pixels, top left origin like Windows.
static void PollMouse() {
    const CGPoint location = Msg<CGPoint>(mac.window, "mouseLocationOutsideOfEventStream");
    const CGRect bounds = Msg<CGRect>(mac.view, "bounds");
    input.mouseInside = location.x >= 0.0 && location.y >= 0.0 && location.x < bounds.size.width &&
                        location.y < bounds.size.height;
    const F64 x = location.x * static_cast<F64>(uiScale);
    const F64 y = (bounds.size.height - location.y) * static_cast<F64>(uiScale);
    MoveMouse(static_cast<I32>(x < 0.0 ? x - 1.0 : x), static_cast<I32>(y < 0.0 ? y - 1.0 : y));
}

// Handles every queued event. Returns false once the window is closed.
static bool PumpEvents() {
    // Objects that AppKit autoreleases while handling events go here.
    const id pool = Msg(Msg(ObjcClass("NSAutoreleasePool"), "alloc"), "init");
    PollMouse();
    for (;;) {
        const id event = Msg(mac.app, "nextEventMatchingMask:untilDate:inMode:dequeue:", kEventMaskAny,
                             mac.distantPast, static_cast<const void *>(kCFRunLoopDefaultMode), static_cast<BOOL>(YES));
        if (!event) {
            break;
        }
        if (HandleEvent(event)) {
            Msg<void>(mac.app, "sendEvent:", event);
        }
    }
    Msg<void>(mac.app, "updateWindows");
    if (!Msg<BOOL>(mac.window, "isKeyWindow")) {
        for (bool &down: input.keyDown) {
            down = false;
        }
    }
    UpdateDrawableSize();
    Msg<void>(pool, "drain");
    return !mac.closed;
}

// False while minimized, when there is nothing to draw into.
static bool WindowVisible() {
    return !Msg<BOOL>(mac.window, "isMiniaturized") && mac.width > 0 && mac.height > 0;
}

// Blocks until an event arrives and leaves it queued for PumpEvents.
static void WaitForEvents() {
    const id pool = Msg(Msg(ObjcClass("NSAutoreleasePool"), "alloc"), "init");
    Msg(mac.app, "nextEventMatchingMask:untilDate:inMode:dequeue:", kEventMaskAny, mac.distantFuture,
        static_cast<const void *>(kCFRunLoopDefaultMode), static_cast<BOOL>(NO));
    Msg<void>(pool, "drain");
}

static U64 Counter() {
    return mach_absolute_time();
}

static U64 CounterFrequency() {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    return 1000000000ull * timebase.denom / timebase.numer;
}

static U64 UtcFileTimeTicks() {
    timeval now;
    gettimeofday(&now, nullptr);
    return (static_cast<U64>(now.tv_sec) + kUnixEpochFileTimeSeconds) * kTicksPerSecond +
           static_cast<U64>(now.tv_usec) * (kTicksPerSecond / 1000000);
}
