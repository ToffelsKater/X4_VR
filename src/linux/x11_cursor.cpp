// X4's mouse cursor through XCB (see x11_cursor.hpp). The reply structs are the X11 protocol's
// fixed wire layouts (xproto.h, xfixes.h), declared here so the build needs no X headers.
#include "x11_cursor.hpp"
#include "linux_runtime.hpp"
#include <dlfcn.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace x4vr::linux_port {
namespace {
struct Connection;
struct Cookie { unsigned int sequence; };
struct GenericError { uint8_t response_type, error_code; uint16_t sequence; uint32_t resource_id; uint16_t minor_code; uint8_t major_code, pad0; uint32_t pad[5]; uint32_t full_sequence; };
struct Screen { uint32_t root; }; // first field of xcb_screen_t
struct ScreenIterator { Screen* data; int rem; int index; };
struct QueryPointerReply { uint8_t response_type, same_screen; uint16_t sequence; uint32_t length, root, child;
                           int16_t root_x, root_y, win_x, win_y; uint16_t mask; uint8_t pad0[2]; };
struct GeometryReply { uint8_t response_type, depth; uint16_t sequence; uint32_t length, root; int16_t x, y;
                       uint16_t width, height, border_width; uint8_t pad0[2]; };
struct QueryTreeReply { uint8_t response_type, pad0; uint16_t sequence; uint32_t length, root, parent; uint16_t children_len; uint8_t pad1[14]; };
struct InternAtomReply { uint8_t response_type, pad0; uint16_t sequence; uint32_t length, atom; };
struct PropertyReply { uint8_t response_type, format; uint16_t sequence; uint32_t length, type, bytes_after, value_len; uint8_t pad0[12]; };
struct ClientMessage { uint8_t response_type, format; uint16_t sequence; uint32_t window, type; uint32_t data[5]; };
struct KeymapReply { uint8_t response_type, pad0; uint16_t sequence; uint32_t length; uint8_t keys[32]; };
struct InputFocusReply { uint8_t response_type, revert_to; uint16_t sequence; uint32_t length, focus; };
// X keycodes (evdev + 8), as Xwayland uses them.
constexpr uint8_t key_control_left = 37, key_control_right = 105, key_f11 = 95, key_f12 = 96;
struct CursorImageReply { uint8_t response_type, pad0; uint16_t sequence; uint32_t length; int16_t x, y;
                          uint16_t width, height, xhot, yhot; uint32_t cursor_serial; uint8_t pad1[8]; };
constexpr uint32_t atom_string = 31, atom_wm_class = 67, atom_cardinal = 6;

struct Xcb {
    void* lib{}; void* xfixes{};
    Connection* (*connect)(const char*, int*){};
    int (*has_error)(Connection*){};
    const void* (*get_setup)(Connection*){};
    ScreenIterator (*roots)(const void*){};
    Cookie (*query_pointer)(Connection*, uint32_t){};
    QueryPointerReply* (*query_pointer_reply)(Connection*, Cookie, GenericError**){};
    Cookie (*get_geometry)(Connection*, uint32_t){};
    GeometryReply* (*get_geometry_reply)(Connection*, Cookie, GenericError**){};
    Cookie (*query_tree)(Connection*, uint32_t){};
    QueryTreeReply* (*query_tree_reply)(Connection*, Cookie, GenericError**){};
    uint32_t* (*query_tree_children)(const QueryTreeReply*){};
    Cookie (*intern_atom)(Connection*, uint8_t, uint16_t, const char*){};
    InternAtomReply* (*intern_atom_reply)(Connection*, Cookie, GenericError**){};
    Cookie (*get_property)(Connection*, uint8_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t){};
    PropertyReply* (*get_property_reply)(Connection*, Cookie, GenericError**){};
    void* (*get_property_value)(const PropertyReply*){};
    int (*get_property_value_length)(const PropertyReply*){};
    Cookie (*xfixes_query_version)(Connection*, uint32_t, uint32_t){};
    void* (*xfixes_query_version_reply)(Connection*, Cookie, GenericError**){};
    Cookie (*xfixes_cursor_image)(Connection*){};
    CursorImageReply* (*xfixes_cursor_image_reply)(Connection*, Cookie, GenericError**){};
    uint32_t* (*xfixes_cursor_image_pixels)(const CursorImageReply*){};
    Cookie (*query_keymap)(Connection*){};
    KeymapReply* (*query_keymap_reply)(Connection*, Cookie, GenericError**){};
    Cookie (*get_input_focus)(Connection*){};
    InputFocusReply* (*get_input_focus_reply)(Connection*, Cookie, GenericError**){};
    Cookie (*send_event)(Connection*, uint8_t, uint32_t, uint32_t, const char*){};
    int (*flush)(Connection*){};
    bool load() {
        lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        xfixes = lib ? dlopen("libxcb-xfixes.so.0", RTLD_NOW | RTLD_LOCAL) : nullptr;
        if (!xfixes) return false;
        const auto get = [](void* from, auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(from, name)); return fn != nullptr; };
        return get(lib, connect, "xcb_connect") && get(lib, has_error, "xcb_connection_has_error") && get(lib, get_setup, "xcb_get_setup") &&
            get(lib, roots, "xcb_setup_roots_iterator") && get(lib, query_pointer, "xcb_query_pointer") &&
            get(lib, query_pointer_reply, "xcb_query_pointer_reply") && get(lib, get_geometry, "xcb_get_geometry") &&
            get(lib, get_geometry_reply, "xcb_get_geometry_reply") && get(lib, query_tree, "xcb_query_tree") &&
            get(lib, query_tree_reply, "xcb_query_tree_reply") && get(lib, query_tree_children, "xcb_query_tree_children") &&
            get(lib, intern_atom, "xcb_intern_atom") && get(lib, intern_atom_reply, "xcb_intern_atom_reply") &&
            get(lib, get_property, "xcb_get_property") && get(lib, get_property_reply, "xcb_get_property_reply") &&
            get(lib, get_property_value, "xcb_get_property_value") && get(lib, get_property_value_length, "xcb_get_property_value_length") &&
            get(lib, query_keymap, "xcb_query_keymap") && get(lib, query_keymap_reply, "xcb_query_keymap_reply") &&
            get(lib, get_input_focus, "xcb_get_input_focus") && get(lib, get_input_focus_reply, "xcb_get_input_focus_reply") &&
            get(lib, send_event, "xcb_send_event") && get(lib, flush, "xcb_flush") &&
            get(xfixes, xfixes_query_version, "xcb_xfixes_query_version") && get(xfixes, xfixes_query_version_reply, "xcb_xfixes_query_version_reply") &&
            get(xfixes, xfixes_cursor_image, "xcb_xfixes_get_cursor_image") && get(xfixes, xfixes_cursor_image_reply, "xcb_xfixes_get_cursor_image_reply") &&
            get(xfixes, xfixes_cursor_image_pixels, "xcb_xfixes_get_cursor_image_cursor_image");
    }
};

std::mutex state_mutex;
CursorState state;
std::atomic<bool> close_requested{false};

// X4's top-level window: WM_CLASS "X4" (2D) or SDL_APP_ID's (x4vr-run: "X4VR"), or
// _NET_WM_PID = this process; the largest one.
uint32_t find_window(const Xcb& x, Connection* c, uint32_t root, uint32_t pid_atom) {
    const char* app_id = std::getenv("SDL_APP_ID");
    const std::string_view window_class = app_id && *app_id ? app_id : "X4";
    GenericError* error{};
    auto* tree = x.query_tree_reply(c, x.query_tree(c, root), &error);
    std::free(error);
    if (!tree) return 0;
    const auto* children = x.query_tree_children(tree);
    uint32_t best{}, best_area{};
    for (uint16_t i = 0; i < tree->children_len; ++i) {
        const auto window = children[i];
        bool ours = false;
        error = nullptr;
        if (auto* p = x.get_property_reply(c, x.get_property(c, 0, window, atom_wm_class, atom_string, 0, 64), &error)) {
            const auto* text = static_cast<const char*>(x.get_property_value(p));
            const int length = x.get_property_value_length(p);
            const std::string_view value(text, size_t(std::max(length, 0)));
            const auto split = value.find('\0'); // "instance\0class\0"
            if (split != std::string_view::npos) {
                const auto name = value.substr(split+1);
                const auto end = std::min(name.find('\0'), name.size());
                ours = name.substr(0, end) == "X4" || name.substr(0, end) == window_class;
            }
            std::free(p);
        }
        std::free(error); error = nullptr;
        if (!ours && pid_atom)
            if (auto* p = x.get_property_reply(c, x.get_property(c, 0, window, pid_atom, atom_cardinal, 0, 1), &error)) {
                if (x.get_property_value_length(p) == 4) ours = *static_cast<const uint32_t*>(x.get_property_value(p)) == uint32_t(getpid());
                std::free(p);
            }
        std::free(error); error = nullptr;
        if (!ours) continue;
        if (auto* g = x.get_geometry_reply(c, x.get_geometry(c, window), &error)) {
            const uint32_t area = uint32_t(g->width)*g->height;
            if (area > best_area) { best = window; best_area = area; }
            std::free(g);
        }
        std::free(error); error = nullptr;
    }
    std::free(tree);
    return best;
}

void reader() {
    static Xcb x;
    if (!x.load()) { log("X4VR cursor: libxcb / libxcb-xfixes not found; no mouse cursor in VR"); return; }
    const char* display = std::getenv("DISPLAY");
    if (!display || !*display) { log("X4VR cursor: no X display; no mouse cursor in VR"); return; }
    int screen_number = 0;
    auto* c = x.connect(nullptr, &screen_number);
    if (!c || x.has_error(c)) { log("X4VR cursor: can't connect to the X server; no mouse cursor in VR"); return; }
    auto it = x.roots(x.get_setup(c));
    for (int i = 0; i < screen_number && it.rem > 1; ++i) { ++it.data; --it.rem; }
    const uint32_t root = it.data->root;
    GenericError* error{};
    std::free(x.xfixes_query_version_reply(c, x.xfixes_query_version(c, 4, 0), &error)); // required before XFixes requests
    std::free(error); error = nullptr;
    uint32_t pid_atom{};
    if (auto* a = x.intern_atom_reply(c, x.intern_atom(c, 1, 11, "_NET_WM_PID"), &error)) { pid_atom = a->atom; std::free(a); }
    std::free(error); error = nullptr;
    uint32_t protocols_atom{}, delete_atom{};
    if (auto* a = x.intern_atom_reply(c, x.intern_atom(c, 0, 12, "WM_PROTOCOLS"), &error)) { protocols_atom = a->atom; std::free(a); }
    std::free(error); error = nullptr;
    if (auto* a = x.intern_atom_reply(c, x.intern_atom(c, 0, 16, "WM_DELETE_WINDOW"), &error)) { delete_atom = a->atom; std::free(a); }
    std::free(error); error = nullptr;
    uint32_t window{};
    auto searched = std::chrono::steady_clock::time_point{};
    bool logged = false;
    // Windows' hotkeys, watched (not grabbed: X4 still gets the keys) while X4's window has focus.
    const char* hotkeys_env = std::getenv("X4VR_HOTKEYS");
    const bool hotkeys = !(hotkeys_env && *hotkeys_env == '0');
    bool f11_down = false, f12_down = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        if (x.has_error(c)) { log("X4VR cursor: X connection lost; no mouse cursor in VR"); return; }
        const auto now = std::chrono::steady_clock::now();
        if (!window && now-searched > std::chrono::seconds(2)) {
            searched = now;
            window = find_window(x, c, root, pid_atom);
            if (window && !logged) { logged = true; log("X4VR cursor: reading X4's mouse cursor (X window "+std::to_string(window)+")"); }
        }
        if (window && protocols_atom && delete_atom && close_requested.exchange(false)) {
            ClientMessage message{33, 32, 0, window, protocols_atom, {delete_atom, 0, 0, 0, 0}}; // 33: ClientMessage
            x.send_event(c, 0, window, 0, reinterpret_cast<const char*>(&message));
            x.flush(c);
            log("X4VR cursor: SteamVR asked the game to exit: X4's window asked to close");
        }
        if (window && hotkeys) {
            auto* focus = x.get_input_focus_reply(c, x.get_input_focus(c), &error);
            std::free(error); error = nullptr;
            auto* keymap = focus && focus->focus == window ? x.query_keymap_reply(c, x.query_keymap(c), &error) : nullptr;
            std::free(error); error = nullptr;
            const auto down = [&](uint8_t key) { return keymap && (keymap->keys[key >> 3] >> (key & 7)) & 1; };
            const bool control = down(key_control_left) || down(key_control_right);
            const bool f11 = control && down(key_f11), f12 = control && down(key_f12);
            if (f12 && !f12_down) linux_port::control("recenter", "Ctrl+F12");
            if (f11 && !f11_down) linux_port::control("flat", "Ctrl+F11");
            f11_down = f11; f12_down = f12;
            std::free(focus); std::free(keymap);
        }
        CursorState next;
        if (window) {
            auto* pointer = x.query_pointer_reply(c, x.query_pointer(c, window), &error);
            if (error) { std::free(error); error = nullptr; window = 0; } // window gone: search again
            auto* geometry = window ? x.get_geometry_reply(c, x.get_geometry(c, window), &error) : nullptr;
            std::free(error); error = nullptr;
            if (pointer && geometry && pointer->same_screen && pointer->win_x >= 0 && pointer->win_y >= 0 &&
                pointer->win_x < geometry->width && pointer->win_y < geometry->height) {
                next.x = pointer->win_x; next.y = pointer->win_y;
                next.window_w = geometry->width; next.window_h = geometry->height;
                next.visible = true;
            }
            std::free(pointer); std::free(geometry);
        }
        if (next.visible) {
            auto* image = x.xfixes_cursor_image_reply(c, x.xfixes_cursor_image(c), &error);
            std::free(error); error = nullptr;
            if (image && image->width && image->height && image->width <= 256 && image->height <= 256) {
                next.width = image->width; next.height = image->height; next.xhot = image->xhot; next.yhot = image->yhot;
                next.serial = image->cursor_serial;
                const auto* argb = x.xfixes_cursor_image_pixels(image); // premultiplied ARGB
                next.bgra.resize(size_t(next.width)*next.height);
                bool any = false;
                for (size_t i = 0; i < next.bgra.size(); ++i) {
                    const uint32_t p = argb[i], a = p >> 24;
                    if (!a) { next.bgra[i] = 0; continue; }
                    any = true;
                    const auto un = [&](int shift) { return std::min<uint32_t>(255, ((p >> shift) & 0xff)*255/a) << shift; };
                    next.bgra[i] = (a << 24) | un(16) | un(8) | un(0); // straight alpha; bytes B G R A
                }
                next.visible = any; // X4 hides the cursor with an empty image (flight)
            } else next.visible = false;
            std::free(image);
        }
        std::lock_guard lock(state_mutex);
        state = std::move(next);
    }
}
}

void start_cursor_reader() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(reader).detach(); });
}
void request_game_close() { close_requested = true; }
CursorState cursor_state() {
    std::lock_guard lock(state_mutex);
    return state;
}
}
