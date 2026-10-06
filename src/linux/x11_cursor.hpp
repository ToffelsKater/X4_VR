#pragma once
// X4's mouse cursor on Linux: X4 (SDL, through Xwayland) shows the X server's cursor, which never
// reaches its swapchain. A thread reads its position over X4's window and its image (XFixes)
// through XCB, loaded at runtime: no build dependency, and XCB reports errors per request instead
// of Xlib's process-wide handler that could end X4. Without X11 or XCB the cursor stays off.
// Counterpart of the Windows layer's GetCursorInfo/DrawIconEx (src/observe_layer.cpp).
#include <cstdint>
#include <vector>

namespace x4vr::linux_port {
struct CursorState {
    bool visible{};           // over X4's window, with a non-empty image
    int x{}, y{};             // pointer in X4's window, pixels
    uint32_t window_w{}, window_h{};
    uint32_t width{}, height{}, xhot{}, yhot{};
    std::vector<uint32_t> bgra; // width*height, straight alpha, B G R A bytes in memory
    uint64_t serial{};        // changes with the image
};
void start_cursor_reader(); // once; logs why if it can't
CursorState cursor_state();
// SteamVR's "Exit game" (VREvent_Quit): asks X4 to close as its window's close button does
// (WM_DELETE_WINDOW), through the reader's X connection.
void request_game_close();
}
