#pragma once

// The platform layer for Windows (see platform.h), compiled as part of main.cc.

char const *const kSurfaceExtensionName = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
constexpr wchar_t const *kClassName = L"earth-viewer";
constexpr wchar_t const *kWindowTitle = L"Earth Viewer";
constexpr int kIdcArrow = 32512; // IDC_ARROW
constexpr U32 kDefaultDpi = 96;

struct Platform {
    HWND hwnd;
    // Window icons, rebuilt from the embedded PNGs at each monitor's DPI.
    HICON bigIcon;
    HICON smallIcon;
    // Where WndProc reports input. Window messages arrive through a callback
    // that takes no arguments of ours, so it finds the Platform through the
    // window's user data and the Input through here.
    Input *input;
};

// Decodes the smallest embedded PNG at least as large as the icon, so Windows
// only ever scales down.
static HICON CreateWindowIcon(const int size) {
    const bool fromSmall = size <= 32;
    const U8 *const png = fromSmall ? kIconPng32 : kIconPng64;
    const DWORD bytes = fromSmall ? sizeof(kIconPng32) : sizeof(kIconPng64);
    constexpr DWORD kIconFormatVersion = 0x00030000;
    return CreateIconFromResourceEx(const_cast<PBYTE>(png), bytes, TRUE, kIconFormatVersion, size, size,
                                    LR_DEFAULTCOLOR);
}

// The big icon shows in the taskbar and Alt+Tab, the small one in the title bar.
static void SetWindowIcons(Platform &platform, const UINT dpi) {
    const HICON taskbarIcon = CreateWindowIcon(GetSystemMetricsForDpi(SM_CXICON, dpi));
    const HICON titleBarIcon = CreateWindowIcon(GetSystemMetricsForDpi(SM_CXSMICON, dpi));
    SendMessageW(platform.hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(taskbarIcon));
    SendMessageW(platform.hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(titleBarIcon));
    if (platform.bigIcon) {
        DestroyIcon(platform.bigIcon);
    }
    if (platform.smallIcon) {
        DestroyIcon(platform.smallIcon);
    }
    platform.bigIcon = taskbarIcon;
    platform.smallIcon = titleBarIcon;
}

static POINT MousePoint(const LPARAM lparam) {
    return {.x = static_cast<I16>(LOWORD(lparam)), .y = static_cast<I16>(HIWORD(lparam))};
}

static LRESULT CALLBACK WndProc(HWND window, const UINT msg, const WPARAM wparam, const LPARAM lparam) {
    // CreateAppWindow passes the Platform with the first message, WM_NCCREATE.
    // The few messages before it go to the default handler.
    if (msg == WM_NCCREATE) {
        Platform *const created = static_cast<Platform *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        created->hwnd = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
    }
    Platform *const found = reinterpret_cast<Platform *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!found) {
        return DefWindowProcW(window, msg, wparam, lparam);
    }
    Platform &platform = *found;
    Input &input = *platform.input;
    switch (msg) {
        case WM_DPICHANGED: {
            // Take the new monitor's scale and the size Windows suggests for
            // it; the swapchain follows the resize on the next frame.
            input.uiScale = static_cast<F32>(HIWORD(wparam)) / static_cast<F32>(kDefaultDpi);
            SetWindowIcons(platform, HIWORD(wparam));
            const RECT *const suggested = reinterpret_cast<const RECT *>(lparam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
        case WM_KEYDOWN: {
            if (wparam == VK_ESCAPE) {
                DestroyWindow(window);
            }
            // Bit 30 of lparam is set for auto-repeat, so holding H toggles once.
            const bool repeat = (lparam & (1 << 30)) != 0;
            if (wparam == 'H' && !repeat) {
                input.hideSatellites = !input.hideSatellites;
            }
            input.keyDown[wparam & (kMaxKeys - 1)] = true;
            return 0;
        }
        case WM_CHAR: {
            // Characters rather than key codes, so [ and ] work on any layout.
            const bool repeat = (lparam & (1 << 30)) != 0;
            if (!repeat) {
                StepTimeRate(input, static_cast<U32>(wparam));
            }
            return 0;
        }
        case WM_KEYUP: {
            input.keyDown[wparam & (kMaxKeys - 1)] = false;
            return 0;
        }
        case WM_KILLFOCUS: {
            for (bool &down: input.keyDown) {
                down = false;
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            SetCapture(window);
            input.dragging = true;
            const POINT mouse = MousePoint(lparam);
            input.lastMouseX = mouse.x;
            input.lastMouseY = mouse.y;
            return 0;
        }
        case WM_MBUTTONDOWN: {
            SetCapture(window);
            input.orbiting = true;
            const POINT mouse = MousePoint(lparam);
            input.lastMouseX = mouse.x;
            input.lastMouseY = mouse.y;
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP: {
            if (msg == WM_MBUTTONUP) {
                input.orbiting = false;
            } else {
                input.dragging = false;
            }
            if (!input.dragging && !input.orbiting) {
                ReleaseCapture();
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            const POINT mouse = MousePoint(lparam);
            if (!input.mouseInside) {
                // Ask for WM_MOUSELEAVE, so the hover label leaves with the cursor.
                TRACKMOUSEEVENT track = {.cbSize = sizeof(track), .dwFlags = TME_LEAVE, .hwndTrack = window};
                TrackMouseEvent(&track);
                input.mouseInside = true;
            }
            MoveMouse(input, mouse.x, mouse.y);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            // Positive is the wheel rolled away from the user: zoom in.
            input.wheelNotches += static_cast<F32>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<F32>(WHEEL_DELTA);
            return 0;
        }
        case WM_MOUSELEAVE: {
            input.mouseInside = false;
            return 0;
        }
        default: {
            return DefWindowProcW(window, msg, wparam, lparam);
        }
    }
}

void CreateAppWindow(Platform &platform, Input &input) {
    HINSTANCE hinstance = GetModuleHandleW(nullptr);
    const WNDCLASSW wc = {
        .lpfnWndProc = WndProc,
        .hInstance = hinstance,
        .hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(kIdcArrow)),
        .lpszClassName = kClassName,
    };
    // Without this Windows renders the app at 96 DPI and stretches the result
    // on scaled displays, which blurs text and thin lines.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RegisterClassW(&wc);
    platform.input = &input;
    const HWND hwnd = CreateWindowExW(0, kClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT, CW_USEDEFAULT, kWindowWidth,
                                      kWindowHeight, nullptr, nullptr, hinstance, &platform);
    if (!hwnd) Die("CreateWindowExW", static_cast<I64>(GetLastError()));
    // The window size is in UI pixels; scale it for the monitor it opened on.
    const UINT dpi = GetDpiForWindow(hwnd);
    input.uiScale = static_cast<F32>(dpi) / static_cast<F32>(kDefaultDpi);
    SetWindowPos(hwnd, nullptr, 0, 0,
                 MulDiv(static_cast<int>(kWindowWidth), static_cast<int>(dpi), static_cast<int>(kDefaultDpi)),
                 MulDiv(static_cast<int>(kWindowHeight), static_cast<int>(dpi), static_cast<int>(kDefaultDpi)),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowIcons(platform, dpi);
    ShowWindow(hwnd, SW_SHOW);
}

VkSurfaceKHR CreateSurface(const Platform &platform, const VkInstance instance) {
    const VkWin32SurfaceCreateInfoKHR surfaceInfo = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = GetModuleHandleW(nullptr),
        .hwnd = platform.hwnd,
    };
    VkSurfaceKHR surface;
    VK_CHECK(vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface));
    return surface;
}

// Messages for the window go to WndProc, which writes the Platform and Input
// it was given at creation.
bool PumpEvents(Platform &, Input &) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

bool WindowVisible(const Platform &platform) {
    RECT rect;
    GetClientRect(platform.hwnd, &rect);
    return rect.right > rect.left && rect.bottom > rect.top;
}

void WaitForEvents(const Platform &) {
    WaitMessage();
}

U64 Counter() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<U64>(now.QuadPart);
}

U64 CounterFrequency() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return static_cast<U64>(frequency.QuadPart);
}

U64 UtcFileTimeTicks() {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return (static_cast<U64>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}
