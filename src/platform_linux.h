#pragma once

// The platform layer for Linux (see platform.h), compiled as part of main.cc.
// It speaks X11 through Xlib, so Wayland sessions run it under XWayland.

char const *const kSurfaceExtensionName = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
constexpr char const *kWindowTitle = "Earth Viewer";
constexpr F32 kDefaultDpi = 96.0f;
constexpr long kMaxResourceLongs = 16384; // of the root window's resource database
constexpr int kMaxKeyText = 8;
constexpr long kWindowEvents = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                               PointerMotionMask | EnterWindowMask | LeaveWindowMask | FocusChangeMask |
                               StructureNotifyMask;

struct Platform {
    Display *display;
    Window window;
    Atom wmDeleteWindow;
    U32 width; // pixels
    U32 height;
    bool mapped;
    bool closed;
    // _NET_WM_ICON: width, height, then ARGB pixels. Xlib passes 32-bit
    // property items in longs.
    long iconProperty[2 + kIconArgbSize * kIconArgbSize];
};

void *AllocatePages(const U64 bytes) {
    void *const pages = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) {
        Die("mmap", static_cast<I64>(bytes));
    }
    return pages;
}

// Desktops publish their scale as Xft.dpi in the root window's resource
// database, and GNOME and KDE update it when the scale changes. Unscaled
// without one.
static F32 ReadUiScale(Display *const display) {
    Atom type;
    int format;
    unsigned long count;
    unsigned long remaining;
    unsigned char *resources = nullptr;
    F32 dpi = kDefaultDpi;
    const int status = XGetWindowProperty(display, DefaultRootWindow(display), XA_RESOURCE_MANAGER, 0,
                                          kMaxResourceLongs, False, XA_STRING, &type, &format, &count,
                                          &remaining, &resources);
    if (status != Success || !resources) {
        return 1.0f;
    }
    // One "name:\tvalue" line per resource.
    constexpr char kDpiName[] = "Xft.dpi:";
    const char *line = reinterpret_cast<const char *>(resources);
    while (*line) {
        U32 matched = 0;
        while (matched + 1 < sizeof(kDpiName) && line[matched] == kDpiName[matched]) {
            ++matched;
        }
        if (matched + 1 == sizeof(kDpiName)) {
            sscanf(line + matched, "%f", &dpi);
        }
        while (*line && *line != '\n') {
            ++line;
        }
        if (*line) {
            ++line;
        }
    }
    XFree(resources);
    return dpi > 0.0f ? dpi / kDefaultDpi : 1.0f;
}

// Key state is indexed like Windows virtual keys: upper-case ASCII letters and
// kKeyShift. Other Latin-1 key symbols keep their value, everything else is 0.
static U32 KeyIndex(const KeySym keySym) {
    if (keySym >= XK_a && keySym <= XK_z) {
        return static_cast<U32>(keySym - XK_a + 'A');
    }
    if (keySym == XK_Shift_L || keySym == XK_Shift_R) {
        return kKeyShift;
    }
    return keySym < kMaxKeys ? static_cast<U32>(keySym) : 0;
}

static void HandleEvent(Platform &platform, Input &input, XEvent &event) {
    switch (event.type) {
        case KeyPress: {
            // The unshifted symbol, so Shift+W still holds W.
            const KeySym keySym = XLookupKeysym(&event.xkey, 0);
            if (keySym == XK_Escape) {
                platform.closed = true;
            }
            const U32 key = KeyIndex(keySym);
            // With detectable auto-repeat a held key repeats as presses alone.
            const bool repeat = key != 0 && input.keyDown[key];
            if (key == 'H' && !repeat) {
                input.hideSatellites = !input.hideSatellites;
            }
            if (!repeat) {
                // Characters rather than key codes, so [ and ] work on any layout.
                char text[kMaxKeyText];
                const int length = XLookupString(&event.xkey, text, sizeof(text), nullptr, nullptr);
                if (length == 1) {
                    StepTimeRate(input, static_cast<U8>(text[0]));
                }
            }
            input.keyDown[key] = true;
            break;
        }
        case KeyRelease: {
            input.keyDown[KeyIndex(XLookupKeysym(&event.xkey, 0))] = false;
            break;
        }
        case FocusOut: {
            for (bool &down: input.keyDown) {
                down = false;
            }
            break;
        }
        // X grabs the pointer from a press to its release, so drags keep
        // reporting motion outside the window, as SetCapture does on Windows.
        case ButtonPress: {
            const unsigned int button = event.xbutton.button;
            if (button == Button1 || button == Button3) {
                input.dragging = true;
                input.lastMouseX = event.xbutton.x;
                input.lastMouseY = event.xbutton.y;
            }
            if (button == Button2) {
                input.orbiting = true;
                input.lastMouseX = event.xbutton.x;
                input.lastMouseY = event.xbutton.y;
            }
            // The wheel arrives as button 4, rolled away from the user to zoom
            // in, and button 5.
            if (button == Button4) {
                input.wheelNotches += 1.0f;
            }
            if (button == Button5) {
                input.wheelNotches -= 1.0f;
            }
            break;
        }
        case ButtonRelease: {
            const unsigned int button = event.xbutton.button;
            if (button == Button1 || button == Button3) {
                input.dragging = false;
            }
            if (button == Button2) {
                input.orbiting = false;
            }
            break;
        }
        case MotionNotify: {
            const I32 x = event.xmotion.x;
            const I32 y = event.xmotion.y;
            input.mouseInside = x >= 0 && y >= 0 && x < static_cast<I32>(platform.width) &&
                                y < static_cast<I32>(platform.height);
            MoveMouse(input, x, y);
            break;
        }
        case EnterNotify: {
            input.mouseInside = true;
            MoveMouse(input, event.xcrossing.x, event.xcrossing.y);
            break;
        }
        case LeaveNotify: {
            input.mouseInside = false;
            break;
        }
        case ConfigureNotify: {
            const U32 width = static_cast<U32>(event.xconfigure.width);
            const U32 height = static_cast<U32>(event.xconfigure.height);
            if (width != platform.width || height != platform.height) {
                platform.width = width;
                platform.height = height;
                input.resizePending = true;
            }
            break;
        }
        // Window managers unmap minimized windows.
        case MapNotify: {
            platform.mapped = true;
            break;
        }
        case UnmapNotify: {
            platform.mapped = false;
            break;
        }
        case ClientMessage: {
            if (static_cast<Atom>(event.xclient.data.l[0]) == platform.wmDeleteWindow) {
                platform.closed = true;
            }
            break;
        }
        case PropertyNotify: {
            if (event.xproperty.atom == XA_RESOURCE_MANAGER) {
                input.uiScale = ReadUiScale(platform.display);
            }
            break;
        }
        default: {
            break;
        }
    }
}

void CreateAppWindow(Platform &platform, Input &input) {
    Display *const display = XOpenDisplay(nullptr);
    if (!display) {
        Die("XOpenDisplay", 0);
    }
    platform.display = display;
    const Window root = DefaultRootWindow(display);
    // The window size is in UI pixels; scale it like the rest of the desktop.
    input.uiScale = ReadUiScale(display);
    platform.width = static_cast<U32>(static_cast<F32>(kWindowWidth) * input.uiScale + 0.5f);
    platform.height = static_cast<U32>(static_cast<F32>(kWindowHeight) * input.uiScale + 0.5f);
    XSetWindowAttributes attributes = {
        .background_pixel = BlackPixel(display, DefaultScreen(display)),
        .event_mask = kWindowEvents,
    };
    const Window window = XCreateWindow(display, root, 0, 0, platform.width, platform.height, 0, CopyFromParent,
                                        InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &attributes);
    if (!window) {
        Die("XCreateWindow", 0);
    }
    platform.window = window;
    // Watch the resource database for scale changes.
    XSelectInput(display, root, PropertyChangeMask);

    XStoreName(display, window, kWindowTitle);
    char appName[] = "earth-viewer";
    XClassHint classHint = {.res_name = appName, .res_class = appName};
    XSetClassHint(display, window, &classHint);
    // Closing the window asks rather than kills the connection.
    platform.wmDeleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &platform.wmDeleteWindow, 1);

    platform.iconProperty[0] = kIconArgbSize;
    platform.iconProperty[1] = kIconArgbSize;
    for (U32 i = 0; i < Count(kIconArgb64); ++i) {
        platform.iconProperty[2 + i] = kIconArgb64[i];
    }
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_ICON", False), XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char *>(platform.iconProperty),
                    static_cast<int>(Count(platform.iconProperty)));

    // Otherwise a held key repeats as release and press pairs, which look
    // like real presses.
    XkbSetDetectableAutoRepeat(display, True, nullptr);
    XMapWindow(display, window);
    XFlush(display);
}

VkSurfaceKHR CreateSurface(const Platform &platform, const VkInstance instance) {
    const VkXlibSurfaceCreateInfoKHR surfaceInfo = {
        .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .dpy = platform.display,
        .window = platform.window,
    };
    VkSurfaceKHR surface;
    VK_CHECK(vkCreateXlibSurfaceKHR(instance, &surfaceInfo, nullptr, &surface));
    return surface;
}

bool PumpEvents(Platform &platform, Input &input) {
    while (XPending(platform.display) > 0) {
        XEvent event;
        XNextEvent(platform.display, &event);
        HandleEvent(platform, input, event);
    }
    return !platform.closed;
}

bool WindowVisible(const Platform &platform) {
    return platform.mapped && platform.width > 0 && platform.height > 0;
}

// Leaves the event queued for PumpEvents.
void WaitForEvents(const Platform &platform) {
    XEvent event;
    XPeekEvent(platform.display, &event);
}

U64 Counter() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<U64>(now.tv_sec) * 1000000000ull + static_cast<U64>(now.tv_nsec);
}

U64 CounterFrequency() {
    return 1000000000ull;
}

U64 UtcFileTimeTicks() {
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (static_cast<U64>(now.tv_sec) + kUnixEpochFileTimeSeconds) * kTicksPerSecond +
           static_cast<U64>(now.tv_nsec) / (1000000000ull / kTicksPerSecond);
}
