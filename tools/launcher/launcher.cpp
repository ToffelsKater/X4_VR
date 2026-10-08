// X4 Native VR launcher: pick a profile (VR mode, comfort), check X4's own display settings,
// start the game with the VR layer, and stay open as a live panel. The layer re-reads
// reports/captures/stereo.txt every 0.5 s, so profile changes apply while playing.
#include "launcher_settings.hpp"
#include "hud_mod.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
using namespace x4vr::launcher;

namespace {
enum Id { ProfileBox = 100, SaveButton, DeleteButton, ModeAlternate, ModePair, ModeMono, ScaleBar, ScaleText,
          PredictBar, PredictText, AsyncBox, ExternalBox, SharedBox, RecenterButton, RuntimeBox, WidthEdit, HeightEdit, ChecksText, FixButton, StatusText, PlayButton, TrackerButton,
          HudEdit, HudApply, HudRemove, HudText, SeatBox, ReportButton };
constexpr const wchar_t* project_url = L"https://github.com/ToffelsKater/X4_VR";

struct App {
    fs::path root, bin, captures, profiles, x4_exe;
    Settings profile; // current profile values (saved only on Save)
    HWND window{};
    std::map<int, HWND> controls;
    HFONT font{};
    int dpi = 96;
    HANDLE game{}; // crash_watch (which runs X4) when started from here
    bool filling = false; // controls are being set from a profile: ignore change notifications
} app;

std::wstring widen(const std::string& text) {
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()));
    return out;
}
std::string narrow(const std::wstring& text) {
    std::string out(WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()), nullptr, nullptr);
    return out;
}
std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text; text << in.rdbuf();
    return text.str();
}
// Replace in one step: the layer must never read a half-written stereo.txt.
bool write_file(const fs::path& path, const std::string& text) {
    const auto temp = fs::path(path).concat(L".tmp");
    { std::ofstream out(temp, std::ios::binary); out << text; if (!out) return false; }
    for (int attempt = 0; attempt < 20; ++attempt) { // the layer may have the file open for a moment
        if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        Sleep(10);
    }
    return false;
}
std::wstring text_of(HWND window) {
    std::wstring text(size_t(GetWindowTextLengthW(window)), L'\0');
    GetWindowTextW(window, text.data(), int(text.size())+1);
    return text;
}
bool running(const wchar_t* exe) {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    for (auto ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry))
        found = !_wcsicmp(entry.szExeFile, exe);
    CloseHandle(snapshot);
    return found;
}
// The Vulkan loader ignores VK_ADD_LAYER_PATH in an elevated process: X4 then runs flat with head tracking only (#21).
bool elevated() { static const bool yes = IsUserAnAdmin(); return yes; }
std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    while (!text.empty() && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    return text;
}
bool freetrack_path_ok() {
    wchar_t value[1024]{}; DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FreeTrack\\FreeTrackClient", L"Path", RRF_RT_REG_SZ, nullptr, value, &size)) return false;
    return lower(value) == lower(app.bin.wstring());
}
// Point X4's FreeTrack lookup at our DLL, as install.ps1 does: a different existing path (a
// real FreeTrack/opentrack install) is kept in config\freetrack-path.backup for uninstall.ps1.
void fix_freetrack_path() {
    wchar_t previous[1024]{}; DWORD size = sizeof(previous);
    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\FreeTrack\\FreeTrackClient", L"Path", RRF_RT_REG_SZ, nullptr, previous, &size) &&
        lower(previous) != lower(app.bin.wstring())) {
        std::ofstream backup(app.root/L"config"/L"freetrack-path.backup", std::ios::binary);
        backup << narrow(previous);
    }
    const auto path = app.bin.wstring();
    const auto error = RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FreeTrack\\FreeTrackClient", L"Path", REG_SZ,
                                       path.c_str(), DWORD((path.size()+1)*sizeof(wchar_t)));
    MessageBoxW(app.window, error ? L"Could not set the head-tracking path in the registry." :
                (L"Head tracking now uses:\n"+path+L"\n\nStart X4 again if it is running.").c_str(), L"X4 VR", error ? MB_ICONERROR : MB_ICONINFORMATION);
}
// First start with no head tracker configured at all: point X4's FreeTrack support here. Another
// tracker's path (opentrack, TrackIR) is left alone; the Status box offers the button for that.
void default_freetrack_path() {
    DWORD size{};
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FreeTrack\\FreeTrackClient", L"Path", RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_FILE_NOT_FOUND) return;
    const auto path = app.bin.wstring();
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FreeTrack\\FreeTrackClient", L"Path", REG_SZ, path.c_str(), DWORD((path.size()+1)*sizeof(wchar_t)));
}
// X4 keeps one config per Steam account under Documents\Egosoft\X4\<id>\; use the newest.
fs::path x4_config() {
    PWSTR documents{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) return {};
    const fs::path base = fs::path(documents)/L"Egosoft"/L"X4";
    CoTaskMemFree(documents);
    fs::path newest; fs::file_time_type time{};
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(base, error)) {
        const auto config = entry.path()/L"config.xml";
        const auto modified = fs::last_write_time(config, error);
        if (!error && (newest.empty() || modified > time)) { newest = config; time = modified; }
    }
    return newest;
}
std::vector<std::string> missing_build_files() {
    std::vector<std::string> missing;
    for (const auto* name : {L"VkLayer_x4vr_observe.json", L"x4vr_observe.dll", L"x4_openvr.dll", L"FreeTrackClient64.dll", L"crash_watch.exe", L"openvr_api.dll"})
        if (!fs::exists(app.bin/name)) missing.push_back(narrow(name));
    if (!fs::exists(app.x4_exe)) missing.push_back("..\\X4.exe");
    return missing;
}

// ---- profiles and the live stereo.txt ----
std::vector<std::wstring> profile_names() {
    std::vector<std::wstring> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(app.profiles, error))
        if (entry.path().extension() == L".txt") names.push_back(entry.path().stem().wstring());
    std::sort(names.begin(), names.end());
    return names;
}
void apply_live(int recenter_step = 0) {
    const auto defaults = parse_settings(read_file(app.root/L"config"/L"stereo.txt"));
    const auto live_path = app.captures/L"stereo.txt";
    const auto recenter = std::atoi(get(parse_settings(read_file(live_path)), "recenter", "0").c_str()) + recenter_step;
    std::error_code error; fs::create_directories(app.captures, error);
    write_file(live_path, format_settings(compose_live(defaults, app.profile, std::to_string(recenter))));
}
void fill_controls() {
    app.filling = true;
    const bool stereo = get(app.profile, "stereo", "1") != "0", pair = get(app.profile, "pair", "0") == "1";
    CheckRadioButton(app.window, ModeAlternate, ModeMono, !stereo ? ModeMono : pair ? ModePair : ModeAlternate);
    const auto scale = std::atof(get(app.profile, "ipd_scale", "1").c_str());
    SendMessageW(app.controls[ScaleBar], TBM_SETPOS, TRUE, LPARAM(std::lround(scale*100)));
    const auto predict = std::atof(get(app.profile, "predict", "0.035").c_str());
    SendMessageW(app.controls[PredictBar], TBM_SETPOS, TRUE, LPARAM(std::lround(predict*1000)));
    CheckDlgButton(app.window, AsyncBox, get(app.profile, "async_submit", "1") != "0" ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(app.window, ExternalBox, get(app.profile, "external_vr", "0") == "1" ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(app.window, SharedBox, get(app.profile, "shared_pose", "0") == "1" ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(app.controls[RuntimeBox], CB_SETCURSEL, uses_openxr(app.profile) ? 1 : 0, 0);
    SetWindowTextW(app.controls[WidthEdit], widen(get(app.profile, "x4_width", "0")).c_str());
    SetWindowTextW(app.controls[HeightEdit], widen(get(app.profile, "x4_height", "0")).c_str());
    app.filling = false;
}
void update_labels() {
    wchar_t text[64];
    swprintf_s(text, L"%.2f", double(SendMessageW(app.controls[ScaleBar], TBM_GETPOS, 0, 0))/100);
    SetWindowTextW(app.controls[ScaleText], text);
    swprintf_s(text, L"%d ms", int(SendMessageW(app.controls[PredictBar], TBM_GETPOS, 0, 0)));
    SetWindowTextW(app.controls[PredictText], text);
}
// Controls -> profile -> live stereo.txt.
void controls_changed() {
    if (app.filling) return;
    const bool mono = IsDlgButtonChecked(app.window, ModeMono), pair = IsDlgButtonChecked(app.window, ModePair);
    set(app.profile, "stereo", mono ? "0" : "1");
    set(app.profile, "pair", pair ? "1" : "0");
    char value[32];
    sprintf_s(value, "%.2f", double(SendMessageW(app.controls[ScaleBar], TBM_GETPOS, 0, 0))/100);
    set(app.profile, "ipd_scale", value);
    sprintf_s(value, "%.3f", double(SendMessageW(app.controls[PredictBar], TBM_GETPOS, 0, 0))/1000);
    set(app.profile, "predict", value);
    set(app.profile, "async_submit", IsDlgButtonChecked(app.window, AsyncBox) ? "1" : "0");
    set(app.profile, "external_vr", IsDlgButtonChecked(app.window, ExternalBox) ? "1" : "0");
    set(app.profile, "shared_pose", IsDlgButtonChecked(app.window, SharedBox) ? "1" : "0");
    set(app.profile, "runtime", SendMessageW(app.controls[RuntimeBox], CB_GETCURSEL, 0, 0) == 1 ? "openxr" : "openvr");
    set(app.profile, "x4_width", std::to_string(_wtoi(text_of(app.controls[WidthEdit]).c_str())));
    set(app.profile, "x4_height", std::to_string(_wtoi(text_of(app.controls[HeightEdit]).c_str())));
    update_labels();
    apply_live();
}
void remember_profile(const std::wstring& name) {
    std::error_code error; fs::create_directories(app.captures, error);
    write_file(app.captures/L"launcher-profile.txt", narrow(name));
}
void load_profile(const std::wstring& name) {
    app.profile = profile_of(parse_settings(read_file(app.profiles/(name+L".txt"))));
    remember_profile(name);
    fill_controls();
    update_labels();
    apply_live();
}
void refresh_profiles(const std::wstring& select) {
    const auto box = app.controls[ProfileBox];
    SendMessageW(box, CB_RESETCONTENT, 0, 0);
    const auto names = profile_names();
    for (const auto& name : names) SendMessageW(box, CB_ADDSTRING, 0, LPARAM(name.c_str()));
    const auto found = std::find(names.begin(), names.end(), select);
    SendMessageW(box, CB_SETCURSEL, found == names.end() ? WPARAM(-1) : WPARAM(found-names.begin()), 0);
    if (found == names.end()) SetWindowTextW(box, select.c_str());
}
std::wstring chosen_profile() {
    auto name = text_of(app.controls[ProfileBox]);
    name.erase(std::remove_if(name.begin(), name.end(), [](wchar_t c) { return wcschr(L"\\/:*?\"<>|", c) != nullptr; }), name.end());
    return name;
}

// ---- X4 config.xml ----
std::vector<Check> x4_checks(std::string& xml, fs::path& path) {
    path = x4_config();
    xml = path.empty() ? std::string() : read_file(path);
    return xml.empty() ? std::vector<Check>{} : check_x4(xml, std::atoi(get(app.profile, "x4_width", "0").c_str()),
                                                          std::atoi(get(app.profile, "x4_height", "0").c_str()));
}
void fix_x4_settings() {
    if (running(L"X4.exe")) { MessageBoxW(app.window, L"Close X4 first: it rewrites config.xml when it exits.", L"X4 VR", MB_ICONWARNING); return; }
    std::string xml; fs::path path;
    const auto checks = x4_checks(xml, path);
    if (xml.empty()) { MessageBoxW(app.window, L"X4's config.xml was not found under Documents\\Egosoft\\X4.", L"X4 VR", MB_ICONWARNING); return; }
    SYSTEMTIME now; GetLocalTime(&now);
    wchar_t stamp[32]; swprintf_s(stamp, L".bak-%04d%02d%02d-%02d%02d%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::error_code error;
    fs::copy_file(path, fs::path(path).concat(stamp), error);
    if (error || !write_file(path, fix_x4(xml, checks))) { MessageBoxW(app.window, L"Could not update config.xml.", L"X4 VR", MB_ICONERROR); return; }
    MessageBoxW(app.window, (L"X4 settings updated.\nBackup: "+fs::path(path).concat(stamp).wstring()).c_str(), L"X4 VR", MB_ICONINFORMATION);
}

// ---- HUD distance mod: extensions\x4vr_hud, generated from the game's own files (hud_mod.hpp) ----
fs::path hud_extension() { return app.x4_exe.parent_path()/L"extensions"/L"x4vr_hud"; }
std::string md5_hex(const std::string& data) { // X4's catalog index lists an MD5 per file
    unsigned char digest[16]{};
    BCryptHash(BCRYPT_MD5_ALG_HANDLE, nullptr, 0, PUCHAR(data.data()), ULONG(data.size()), digest, sizeof(digest));
    std::string hex;
    for (const auto byte : digest) { char pair[3]; sprintf_s(pair, "%02x", byte); hex += pair; }
    return hex;
}
std::map<std::string, std::string> hud_originals() {
    std::set<std::string> paths(hud_anchor_files().begin(), hud_anchor_files().end());
    paths.insert(hud_scripts().begin(), hud_scripts().end());
    return read_game_files(app.x4_exe.parent_path(), paths);
}
std::string source_hash(const std::map<std::string, std::string>& originals) {
    std::string all;
    for (const auto& [path, blob] : originals) all += path+md5_hex(blob);
    return md5_hex(all);
}
Settings installed_hud() { return parse_settings(read_file(hud_extension()/L"x4vr_hud.txt")); } // scale=, source=
fs::path x4_content() { const auto config = x4_config(); return config.empty() ? config : config.parent_path()/L"content.xml"; }
bool hud_disabled_in_x4() { bool disabled = false; enable_extension(read_file(x4_content()), "x4vr_hud", disabled); return disabled; }
bool install_hud(double scale, std::string& error) {
    const auto originals = hud_originals();
    const auto files = hud_files(originals, scale, error);
    if (files.empty()) return false;
    std::error_code removed;
    fs::remove_all(hud_extension(), removed);
    fs::create_directories(hud_extension(), removed);
    std::ofstream data(hud_extension()/L"subst_01.dat", std::ios::binary);
    std::string index;
    const auto stamp = std::to_string(std::time(nullptr));
    for (const auto& [path, blob] : files) {
        data.write(blob.data(), std::streamsize(blob.size()));
        index += path+' '+std::to_string(blob.size())+' '+stamp+' '+md5_hex(blob)+'\n';
    }
    data.close();
    const auto scale_text = format_number(scale);
    const bool ok = data && write_file(hud_extension()/L"subst_01.cat", index) &&
        write_file(hud_extension()/L"content.xml", "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<content id=\"x4vr_hud\" name=\"X4 VR HUD distance\" version=\"100\" save=\"0\" enabled=\"1\"\n"
            "  description=\"Generated by X4VRLauncher: HUD "+scale_text+"x farther away at the same apparent size, for VR. "
            "Built from your own game files.\">\n</content>\n") &&
        write_file(hud_extension()/L"x4vr_hud.txt", format_settings({{"scale", scale_text}, {"source", source_hash(originals)}}));
    if (!ok) error = "could not write "+narrow(hud_extension().wstring());
    return ok;
}
// After an X4 update the mod would replace new game scripts with old copies: rebuild it first.
void refresh_hud_mod() {
    const auto installed = installed_hud();
    const auto scale = std::atof(get(installed, "scale", "0").c_str());
    std::string error;
    if (scale > 0 && get(installed, "source") != source_hash(hud_originals()) && !install_hud(scale, error)) {
        std::error_code removed;
        fs::remove_all(hud_extension(), removed);
        MessageBoxW(app.window, widen("The HUD distance mod no longer matches this X4 version ("+error+") and was removed.").c_str(),
                    L"X4 VR", MB_ICONWARNING);
    }
}
void apply_hud(bool remove) {
    if (running(L"X4.exe")) { MessageBoxW(app.window, L"Close X4 first: it loads extensions at startup.", L"X4 VR", MB_ICONWARNING); return; }
    std::error_code removed;
    if (remove) { fs::remove_all(hud_extension(), removed); return; }
    auto text = text_of(app.controls[HudEdit]);
    std::replace(text.begin(), text.end(), L',', L'.'); // "2,5" on comma-decimal keyboards; _wtof would read 2
    const auto scale = _wtof(text.c_str());
    if (scale < 1 || scale > 6) { MessageBoxW(app.window, L"HUD distance: use a factor between 1 and 6 (2.5 is a good start).", L"X4 VR", MB_ICONINFORMATION); return; }
    std::string error;
    if (!install_hud(scale, error)) { MessageBoxW(app.window, widen("Could not build the HUD mod: "+error).c_str(), L"X4 VR", MB_ICONERROR); return; }
    bool disabled = false;
    const auto content = enable_extension(read_file(x4_content()), "x4vr_hud", disabled);
    if (disabled && !write_file(x4_content(), content))
        MessageBoxW(app.window, L"X4 has the HUD distance mod turned off. Turn on \"X4 VR HUD distance\" in X4's Extensions menu.", L"X4 VR", MB_ICONWARNING);
}

// ---- Seat position mod: extensions\x4vr_seat, one XML diff per ship (hud_mod.hpp) ----
fs::path seat_extension() { return app.x4_exe.parent_path()/L"extensions"/L"x4vr_seat"; }
bool seat_installed() { return fs::exists(seat_extension()/L"x4vr_seat.txt"); }
void apply_seat(bool install) {
    if (running(L"X4.exe")) { MessageBoxW(app.window, L"Close X4 first: it loads extensions at startup.", L"X4 VR", MB_ICONWARNING); return; }
    std::error_code error;
    fs::remove_all(seat_extension(), error);
    if (!install) return;
    bool ok = true;
    for (const auto& [path, text] : seat_files()) {
        const auto target = seat_extension()/fs::path(path);
        fs::create_directories(target.parent_path(), error);
        ok = write_file(target, text) && ok;
    }
    if (!ok) { MessageBoxW(app.window, L"Could not write the seat position mod into X4's extensions folder.", L"X4 VR", MB_ICONERROR); return; }
    bool disabled = false;
    const auto content = enable_extension(read_file(x4_content()), "x4vr_seat", disabled);
    if (disabled && !write_file(x4_content(), content))
        MessageBoxW(app.window, L"X4 has the seat position mod turned off. Turn on \"X4 VR seat position\" in X4's Extensions menu.", L"X4 VR", MB_ICONWARNING);
}
// A newer launcher knows more ships: rewrite an installed mod that lists other ones.
void refresh_seat_mod() {
    if (seat_installed() && read_file(seat_extension()/L"x4vr_seat.txt") != seat_files().at("x4vr_seat.txt")) apply_seat(true);
}

// ---- bug report: logs zipped for the GitHub issue (a web link cannot attach files itself) ----
fs::path newest(const fs::path& dir, const std::wstring& prefix, const std::wstring& suffix = L"") {
    fs::path found; fs::file_time_type time{};
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        const auto name = entry.path().filename().wstring();
        if (name.rfind(prefix, 0) || (!suffix.empty() && (name.size() < suffix.size() || name.compare(name.size()-suffix.size(), suffix.size(), suffix))))
            continue;
        const auto modified = entry.last_write_time(error);
        if (!error && (found.empty() || modified > time)) { found = entry.path(); time = modified; }
    }
    return found;
}
void copy_tail(const fs::path& from, const fs::path& to, size_t limit) { // big append-only logs: the end matters
    auto text = read_file(from);
    if (text.empty()) return;
    if (text.size() > limit) { text.erase(0, text.size()-limit); text.erase(0, text.find('\n')+1); }
    std::ofstream(to, std::ios::binary) << text;
}
std::string git_commit() {
    auto release = read_file(app.root/L"version.txt"); // download package
    while (!release.empty() && (release.back() == '\n' || release.back() == '\r')) release.pop_back();
    if (!release.empty()) return release;
    auto head = read_file(app.root/L".git"/L"HEAD");
    while (!head.empty() && (head.back() == '\n' || head.back() == '\r')) head.pop_back();
    if (head.rfind("ref: ", 0)) return head.substr(0, 12);
    auto sha = read_file(app.root/L".git"/widen(head.substr(5)));
    if (sha.empty()) { // packed refs: "<sha> refs/heads/main"
        const auto packed = read_file(app.root/L".git"/L"packed-refs"), ref = head.substr(5);
        const auto at = packed.find(" "+ref);
        if (at != std::string::npos && at >= 40) sha = packed.substr(at-40, 40);
    }
    return sha.substr(0, 12);
}
std::string windows_version() {
    RTL_OSVERSIONINFOW info{sizeof(info)};
    const auto get = reinterpret_cast<LONG (WINAPI*)(RTL_OSVERSIONINFOW*)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!get || get(&info)) return "unknown";
    return std::to_string(info.dwMajorVersion)+"."+std::to_string(info.dwMinorVersion)+"."+std::to_string(info.dwBuildNumber);
}
std::string url_encode(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || strchr("-_.~", c)) out += char(c);
        else { char hex[4]; sprintf_s(hex, "%%%02X", c); out += hex; }
    }
    return out;
}
void report_bug() {
    if (MessageBoxW(app.window, L"This packs the VR logs, crash dumps and X4's config.xml into a zip and opens a new GitHub issue.\n\n"
                    L"The files contain paths from this PC, including your Windows user name. Look through the zip before you attach it.\n\n"
                    L"Creating the issue needs a free GitHub account. Without one, post in the Reddit thread and share the zip through a file host.\n\nContinue?",
                    L"Report a bug", MB_ICONINFORMATION | MB_OKCANCEL) != IDOK) return;
    SYSTEMTIME now; GetLocalTime(&now);
    wchar_t stamp[32]; swprintf_s(stamp, L"%04d%02d%02d-%02d%02d%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    const auto name = std::wstring(L"bug-report-")+stamp;
    const auto staging = app.captures/name, zip = app.captures/(name+L".zip");
    std::error_code error;
    fs::create_directories(staging, error);
    const auto copy = [&](const fs::path& from, const std::wstring& as = L"") {
        if (!from.empty() && fs::exists(from, error)) fs::copy_file(from, staging/(as.empty() ? from.filename().wstring() : as), fs::copy_options::overwrite_existing, error);
    };
    for (const auto* file : {L"stereo.txt", L"launcher-profile.txt", L"head.txt", L"turn.txt"}) copy(app.captures/file);
    for (const auto* file : {L"state.txt", L"pair_stats.txt"}) copy_tail(app.captures/file, staging/file, 64*1024);
    copy(newest(app.captures, L"launcher-", L".stdout.log")); copy(newest(app.captures, L"launcher-", L".stderr.log"));
    if (const auto debug = newest(app.captures, L"debug-"); !debug.empty()) {
        copy_tail(debug/L"debug-events.log", staging/L"debug-events.log", 4*1024*1024);
        copy(newest(debug, L"crash-", L".dmp"));
    }
    if (const auto process = newest(app.captures, L"process-"); !process.empty()) copy_tail(process/L"events.jsonl", staging/L"events.jsonl", 2*1024*1024);
    copy(x4_config(), L"x4-config.xml");
    // Summary for the issue text and system.txt.
    std::string gpu = "unknown";
    const auto events = read_file(staging/L"events.jsonl");
    if (const auto at = events.find("\"gpu\":\""); at != std::string::npos) gpu = events.substr(at+7, events.find('"', at+7)-at-7);
    WIN32_FILE_ATTRIBUTE_DATA x4_file{};
    SYSTEMTIME x4_time{};
    if (GetFileAttributesExW(app.x4_exe.c_str(), GetFileExInfoStandard, &x4_file)) FileTimeToSystemTime(&x4_file.ftLastWriteTime, &x4_time);
    char x4_date[16]; sprintf_s(x4_date, "%04d-%02d-%02d", x4_time.wYear, x4_time.wMonth, x4_time.wDay);
    const auto hud = installed_hud();
    std::string extensions;
    for (const auto& entry : fs::directory_iterator(app.x4_exe.parent_path()/L"extensions", error)) extensions += narrow(entry.path().filename().wstring())+", ";
    const std::string summary = "- X4 VR build: "+git_commit()+"\n- Windows: "+windows_version()+"\n- GPU: "+gpu+
        "\n- X4.exe: "+std::to_string(fs::file_size(app.x4_exe, error))+" bytes, "+x4_date+
        "\n- SteamVR running: "+(running(L"vrserver.exe") ? "yes" : "no")+"\n- HUD distance mod: "+get(hud, "scale", "off")+"\n";
    std::ofstream(staging/L"system.txt", std::ios::binary) << summary << "\nStatus panel:\n" << narrow(text_of(app.controls[StatusText]))
        << "\n\nX4 settings check:\n" << narrow(text_of(app.controls[ChecksText])) << "\n\nExtensions: " << extensions << '\n';
    // tar ships with Windows 10 and later; -a picks the zip format from the name.
    auto command = L"tar.exe -a -c -f \""+zip.wstring()+L"\" -C \""+staging.wstring()+L"\" .";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    DWORD code = 1;
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        WaitForSingleObject(process.hProcess, 60000);
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess); CloseHandle(process.hThread);
    }
    fs::remove_all(staging, error);
    if (code) { MessageBoxW(app.window, (L"Could not create "+zip.wstring()).c_str(), L"Report a bug", MB_ICONERROR); return; }
    const auto select = L"/select,\""+zip.wstring()+L'"';
    ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
    const std::string body = "**What happened?**\n\n\n**Steps to reproduce**\n1. \n\n**What did you expect?**\n\n\n**System** (from X4VRLauncher)\n"+
        summary+"\n**Logs:** drag `"+narrow(zip.filename().wstring())+"` from the Explorer window that just opened into this box.\n";
    const auto url = std::wstring(project_url)+L"/issues/new?title="+widen(url_encode("[Bug] "))+L"&body="+widen(url_encode(body));
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- launch: same child environment as scripts/observe.ps1 -Target Game -OpenVRBootstrap -CrashWatch ----
struct CaseInsensitive { bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
std::wstring environment_block() {
    std::map<std::wstring, std::wstring, CaseInsensitive> variables;
    const auto strings = GetEnvironmentStringsW();
    for (auto p = strings; *p; p += wcslen(p)+1) {
        const std::wstring entry(p);
        const auto split = entry.find(L'=', 1); // entries like "=C:=C:\" start with '='
        if (split != std::wstring::npos) variables[entry.substr(0, split)] = entry.substr(split+1);
    }
    FreeEnvironmentStringsW(strings);
    const auto prepend = [&](const wchar_t* name, const std::wstring& value) {
        auto& current = variables[name];
        current = current.empty() ? value : value+L';'+current;
    };
    const auto bin = app.bin.wstring();
    prepend(L"PATH", bin); // the layer's dependencies live beside it, not in the game folder
    prepend(L"VK_ADD_LAYER_PATH", bin);
    prepend(L"VK_INSTANCE_LAYERS", L"VK_LAYER_X4VR_observe");
    variables[L"SteamAppId"] = variables[L"SteamGameId"] = L"392160"; // launch directly under Steam's app context
    variables[L"DISABLE_VULKAN_OBS_CAPTURE"] = variables[L"DISABLE_RTSS_LAYER"] = L"1"; // their Vulkan layers load even with the programs closed and break ours (#21)
    variables[L"X4VR_CAPTURE_DIR"] = app.captures.wstring();
    variables[L"X4VR_OPENVR_BOOTSTRAP"] = L"1"; // the runtime bootstrap, whichever backend
    variables[L"X4VR_RUNTIME"] = uses_openxr(app.profile) ? L"openxr" : L"openvr";
    for (const auto* off : {L"X4VR_CAPTURE_MEMORY", L"X4VR_CAPTURE_NATIVE_CAMERA", L"X4VR_CAPTURE_STACK", L"X4VR_CAPTURE_SHADERS", L"X4VR_HEAD_LOOK", L"X4VR_SCENE_COPY"})
        variables[off] = L"0";
    variables[L"X4VR_GAME_ARGS"] = L"-skipintro -nocputhrottle";
    std::wstring block;
    for (const auto& [name, value] : variables) block += name+L'='+value+L'\0';
    return block+L'\0';
}
void play() {
    if (const auto missing = missing_build_files(); !missing.empty()) {
        std::string list; for (const auto& m : missing) list += "\n  "+m;
        MessageBoxW(app.window, widen("Build files missing (run scripts\\install.ps1):"+list).c_str(), L"X4 VR", MB_ICONERROR);
        return;
    }
    if (running(L"X4.exe")) return;
    if (elevated() && MessageBoxW(app.window, L"The launcher runs as administrator. X4 then starts without the VR layer: head tracking works, but no image reaches the headset.\n\n"
                                              L"Close the launcher and start it without administrator rights.\n\nLaunch anyway?",
                                  L"X4 VR", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
    if (!uses_openxr(app.profile) && !running(L"vrserver.exe") && MessageBoxW(app.window, L"SteamVR does not seem to be running. Start it (and your headset software) first.\n\nLaunch anyway?",
                                                 L"X4 VR", MB_ICONWARNING | MB_YESNO) != IDYES) return;
    refresh_hud_mod();
    refresh_seat_mod();
    apply_live();
    const auto stamp = std::to_wstring(GetTickCount64());
    const auto debug = app.captures/(L"debug-launcher-"+stamp);
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    const auto log = [&](const wchar_t* suffix) {
        return CreateFileW((app.captures/(L"launcher-"+stamp+suffix)).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, 0, nullptr);
    };
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log(L".stdout.log"); startup.hStdError = log(L".stderr.log");
    // A source build also has crash_watch_dev.exe (watchpoints, traces); the download only the plain recorder.
    const auto dev_watcher = app.bin/L"crash_watch_dev.exe";
    const auto watcher = (fs::exists(dev_watcher) ? dev_watcher : app.bin/L"crash_watch.exe").wstring();
    auto command = L'"'+watcher+L"\" \""+app.x4_exe.wstring()+L"\" \""+debug.wstring()+L'"';
    auto environment = environment_block();
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(watcher.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                        environment.data(), app.x4_exe.parent_path().c_str(), &startup, &process);
    CloseHandle(startup.hStdOutput); CloseHandle(startup.hStdError);
    if (!started) { MessageBoxW(app.window, L"Could not start the game (crash_watch.exe).", L"X4 VR", MB_ICONERROR); return; }
    CloseHandle(process.hThread);
    app.game = process.hProcess;
}

// ---- status panel (every second) ----
void refresh_status() {
    if (app.game && WaitForSingleObject(app.game, 0) == WAIT_OBJECT_0) { CloseHandle(app.game); app.game = nullptr; }
    const bool x4 = running(L"X4.exe"), steamvr = running(L"vrserver.exe");
    const auto missing = missing_build_files();
    std::string status = uses_openxr(app.profile) ? "VR runtime: OpenXR (Windows' active OpenXR runtime)\r\n"
                         : std::string("SteamVR: ")+(steamvr ? "running" : "not running (start it before Play)")+"\r\n";
    const bool tracker = freetrack_path_ok();
    status += std::string("Head tracking DLL path: ")+(tracker ? "ok" : "not set (press Fix head-tracking path)")+"\r\n";
    status += "Build: "+(missing.empty() ? std::string("ok") : "missing "+missing.front())+"\r\n";
    if (elevated()) status += "Run as administrator: on (no image in the headset)\r\n"; // also lands in the bug report's system.txt
    status += std::string("X4: ")+(x4 ? "running (changes above apply live)" : "not running")+"\r\n";
    std::error_code error;
    const auto stats_path = app.captures/L"pair_stats.txt";
    const auto age = fs::file_time_type::clock::now()-fs::last_write_time(stats_path, error);
    if (x4 && !error && age < std::chrono::seconds(5)) {
        const auto text = read_file(stats_path);
        const auto end = text.find_last_not_of("\r\n");
        const auto start = end == std::string::npos ? std::string::npos : text.rfind('\n', end);
        Stats stats{};
        if (end != std::string::npos && parse_stats_line(text.substr(start == std::string::npos ? 0 : start+1), stats))
            status += "Headset: "+std::to_string(stats.fps)+" frames/s, late "+std::to_string(stats.late)+", repeated eye images "+std::to_string(stats.repeated);
    }
    SetWindowTextW(app.controls[StatusText], widen(status).c_str());

    std::string xml; fs::path path;
    const auto checks = x4_checks(xml, path);
    std::string lines; int failing = 0;
    for (const auto& check : checks) {
        if (check.ok) continue;
        if (++failing <= 5) lines += std::string(check.required ? "\xE2\x9C\x97 " : "\xE2\x80\xA2 ")+check.label+" (now: "+check.current+")"
                                     +(check.required ? "" : ", recommended")+"\r\n";
    }
    if (failing > 5) lines += "+ "+std::to_string(failing-5)+" more\r\n";
    if (xml.empty()) lines = "X4 config.xml not found (start X4 once).";
    else if (!failing) lines = "\xE2\x9C\x93 All X4 settings match VR requirements.";
    SetWindowTextW(app.controls[ChecksText], widen(lines).c_str());
    EnableWindow(app.controls[FixButton], failing && !x4);
    EnableWindow(app.controls[PlayButton], !x4 && !app.game && missing.empty());
    EnableWindow(app.controls[TrackerButton], !tracker);

    const auto hud = get(installed_hud(), "scale");
    SetWindowTextW(app.controls[HudText], hud.empty() ? L"Off: X4's HUD sits about 15 cm from your eyes in VR."
                                        : hud_disabled_in_x4() ? L"Off in X4's Extensions menu: press Apply to turn it back on."
                                                               : widen("On: "+hud+"x farther away, same apparent size.").c_str());
    EnableWindow(app.controls[HudApply], !x4);
    EnableWindow(app.controls[HudRemove], !x4 && !hud.empty());
    CheckDlgButton(app.window, SeatBox, seat_installed() ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(app.controls[SeatBox], !x4);
}

// ---- window ----
HWND add(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    const auto scale = [](int v) { return MulDiv(v, app.dpi, 96); };
    const auto control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, scale(x), scale(y), scale(w), scale(h),
                                         app.window, HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, WPARAM(app.font), TRUE);
    if (id) app.controls[id] = control;
    return control;
}
void create_controls() {
    add(L"STATIC", L"Profile", 0, 16, 18, 60, 20, 0);
    add(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_SORT | WS_VSCROLL | WS_TABSTOP, 80, 14, 244, 200, ProfileBox);
    add(L"BUTTON", L"Save", BS_PUSHBUTTON | WS_TABSTOP, 332, 13, 80, 26, SaveButton);
    add(L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, 418, 13, 86, 26, DeleteButton);
    add(L"BUTTON", L"VR mode", BS_GROUPBOX, 16, 48, 488, 96, 0);
    add(L"BUTTON", L"Alternate eyes: 45 Hz per eye, needs a steady 90 fps (recommended)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 28, 68, 470, 22, ModeAlternate);
    add(L"BUTTON", L"Pair: 90 Hz per eye, needs 180 fps (experimental)", BS_AUTORADIOBUTTON, 28, 92, 470, 22, ModePair);
    add(L"BUTTON", L"Mono: same image in both eyes, no depth", BS_AUTORADIOBUTTON, 28, 116, 470, 22, ModeMono);
    add(L"STATIC", L"World scale", 0, 16, 160, 110, 20, 0);
    add(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, 124, 154, 300, 30, ScaleBar);
    add(L"STATIC", L"", 0, 432, 160, 72, 20, ScaleText);
    add(L"STATIC", L"Prediction", 0, 16, 196, 110, 20, 0);
    add(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, 124, 190, 300, 30, PredictBar);
    add(L"STATIC", L"", 0, 432, 196, 72, 20, PredictText);
    add(L"STATIC", L"VR runtime", 0, 16, 232, 110, 20, 0);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 124, 228, 300, 100, RuntimeBox);
    add(L"STATIC", L"next start", 0, 432, 232, 72, 20, 0);
    add(L"BUTTON", L"Stutter protection (async submission)", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 264, 300, 22, AsyncBox);
    add(L"BUTTON", L"Recenter view (Ctrl+F12)", BS_PUSHBUTTON | WS_TABSTOP, 330, 262, 174, 26, RecenterButton);
    add(L"BUTTON", L"External views (F2/F3): screen fills the view", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 290, 488, 22, ExternalBox);
    add(L"BUTTON", L"Steam Link / Steam Frame: fix the jittering right eye", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 314, 488, 22, SharedBox);
    add(L"BUTTON", L"X4 settings", BS_GROUPBOX, 16, 348, 488, 170, 0);
    add(L"STATIC", L"Resolution", 0, 28, 372, 80, 20, 0);
    add(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, 112, 369, 64, 24, WidthEdit);
    add(L"STATIC", L"x", 0, 182, 372, 12, 20, 0);
    add(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, 196, 369, 64, 24, HeightEdit);
    add(L"STATIC", L"(0 = don't check)", 0, 268, 372, 230, 20, 0);
    add(L"STATIC", L"", 0, 28, 400, 470, 80, ChecksText);
    add(L"BUTTON", L"Fix X4 settings", BS_PUSHBUTTON | WS_TABSTOP, 28, 484, 180, 26, FixButton);
    add(L"STATIC", L"Backs up config.xml; X4 must be closed", 0, 216, 488, 284, 20, 0);
    add(L"BUTTON", L"HUD distance (X4 extension, applies at the next start)", BS_GROUPBOX, 16, 526, 488, 76, 0);
    add(L"STATIC", L"Factor", 0, 28, 550, 50, 20, 0);
    add(L"EDIT", L"2.5", WS_BORDER | WS_TABSTOP, 80, 547, 48, 24, HudEdit);
    add(L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, 136, 546, 80, 26, HudApply);
    add(L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, 222, 546, 80, 26, HudRemove);
    add(L"STATIC", L"", 0, 28, 578, 470, 20, HudText);
    add(L"BUTTON", widen("Camera further back in ships where it sits too far forward ("+seat_ships()+")").c_str(), BS_AUTOCHECKBOX | WS_TABSTOP, 16, 608, 488, 22, SeatBox);
    add(L"BUTTON", L"Status", BS_GROUPBOX, 16, 640, 488, 142, 0);
    add(L"STATIC", L"", 0, 28, 660, 470, 86, StatusText);
    add(L"BUTTON", L"Fix head-tracking path", BS_PUSHBUTTON | WS_TABSTOP, 28, 748, 180, 26, TrackerButton);
    add(L"BUTTON", L"Play X4 in VR", BS_DEFPUSHBUTTON | WS_TABSTOP, 16, 792, 376, 40, PlayButton);
    add(L"BUTTON", L"Report a bug", BS_PUSHBUTTON | WS_TABSTOP, 400, 792, 104, 40, ReportButton);
    add(L"STATIC", L"In game: Ctrl+F12 recenters, Ctrl+F11 switches to the flat theater screen and back.", 0, 16, 840, 488, 20, 0);
    SendMessageW(app.controls[ScaleBar], TBM_SETRANGE, TRUE, MAKELPARAM(50, 200));
    SendMessageW(app.controls[PredictBar], TBM_SETRANGE, TRUE, MAKELPARAM(0, 60));
    SendMessageW(app.controls[RuntimeBox], CB_ADDSTRING, 0, LPARAM(L"OpenVR (SteamVR)"));
    SendMessageW(app.controls[RuntimeBox], CB_ADDSTRING, 0, LPARAM(L"OpenXR (experimental)"));
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_HSCROLL: controls_changed(); return 0;
    case WM_TIMER: refresh_status(); return 0;
    case WM_COMMAND: {
        const int id = LOWORD(w), code = HIWORD(w);
        if (id == ProfileBox && code == CBN_SELCHANGE) {
            const auto box = app.controls[ProfileBox];
            const auto index = SendMessageW(box, CB_GETCURSEL, 0, 0);
            if (index >= 0) {
                std::wstring name(size_t(SendMessageW(box, CB_GETLBTEXTLEN, index, 0)), L'\0');
                SendMessageW(box, CB_GETLBTEXT, index, LPARAM(name.data()));
                load_profile(name.c_str());
            }
        } else if (id == SaveButton) {
            const auto name = chosen_profile();
            if (name.empty()) { MessageBoxW(window, L"Type a profile name into the Profile box first.", L"X4 VR", MB_ICONINFORMATION); return 0; }
            std::error_code error; fs::create_directories(app.profiles, error);
            write_file(app.profiles/(name+L".txt"), format_settings(app.profile));
            remember_profile(name);
            refresh_profiles(name);
        } else if (id == DeleteButton) {
            const auto name = chosen_profile();
            if (!name.empty() && MessageBoxW(window, (L"Delete profile \""+name+L"\"?").c_str(), L"X4 VR", MB_ICONQUESTION | MB_YESNO) == IDYES) {
                std::error_code error; fs::remove(app.profiles/(name+L".txt"), error);
                refresh_profiles(L"");
            }
        } else if ((id >= ModeAlternate && id <= ModeMono) || (id >= AsyncBox && id <= SharedBox)) {
            if (code == BN_CLICKED) controls_changed();
        } else if (id == RuntimeBox && code == CBN_SELCHANGE) {
            controls_changed();
        } else if ((id == WidthEdit || id == HeightEdit) && code == EN_CHANGE) {
            controls_changed();
        } else if (id == RecenterButton) {
            apply_live(1);
        } else if (id == FixButton) {
            fix_x4_settings(); refresh_status();
        } else if (id == TrackerButton) {
            fix_freetrack_path(); refresh_status();
        } else if (id == PlayButton) {
            play(); refresh_status();
        } else if (id == HudApply || id == HudRemove) {
            apply_hud(id == HudRemove); refresh_status();
        } else if (id == SeatBox && code == BN_CLICKED) {
            apply_seat(IsDlgButtonChecked(window, SeatBox) == BST_CHECKED); refresh_status();
        } else if (id == ReportButton) {
            report_bug();
        }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
fs::path find_root() { // a source checkout (CMakeLists.txt) or a download package (version.txt)
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    for (auto dir = fs::path(module).parent_path(); !dir.empty() && dir != dir.parent_path(); dir = dir.parent_path())
        if (fs::exists(dir/L"config"/L"stereo.txt") && (fs::exists(dir/L"CMakeLists.txt") || fs::exists(dir/L"version.txt"))) return dir;
    return {};
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    app.root = find_root();
    if (app.root.empty()) { MessageBoxW(nullptr, L"Could not find the X4 VR project folder (config\\stereo.txt) next to this program.", L"X4 VR", MB_ICONERROR); return 1; }
    app.bin = app.root/L"build"/L"Release";
    app.captures = app.root/L"reports"/L"captures";
    app.profiles = app.root/L"config"/L"profiles";
    app.x4_exe = app.root.parent_path()/L"X4.exe";
    default_freetrack_path();
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    const auto screen = GetDC(nullptr); app.dpi = GetDeviceCaps(screen, LOGPIXELSY); ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    app.font = CreateFontIndirectW(&metrics.lfMessageFont);

    WNDCLASSW type{};
    type.lpfnWndProc = window_proc; type.hInstance = instance; type.lpszClassName = L"X4VRLauncher";
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hbrBackground = HBRUSH(COLOR_BTNFACE+1);
    type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&type);
    RECT size{0, 0, MulDiv(520, app.dpi, 96), MulDiv(868, app.dpi, 96)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&size, style, FALSE);
    app.window = CreateWindowExW(0, type.lpszClassName, L"X4 Native VR", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                 size.right-size.left, size.bottom-size.top, nullptr, nullptr, instance, nullptr);
    create_controls();
    if (const auto hud = get(installed_hud(), "scale"); !hud.empty()) SetWindowTextW(app.controls[HudEdit], widen(hud).c_str());

    auto last = widen(read_file(app.captures/L"launcher-profile.txt"));
    const auto names = profile_names();
    if (std::find(names.begin(), names.end(), last) == names.end()) last = names.empty() ? L"Default" : names.front();
    refresh_profiles(last);
    if (fs::exists(app.profiles/(last+L".txt"))) load_profile(last);
    else { app.profile = profile_of(parse_settings(read_file(app.captures/L"stereo.txt"))); fill_controls(); update_labels(); }
    refresh_status();
    SetTimer(app.window, 1, 1000, nullptr);
    ShowWindow(app.window, show);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(app.window, &message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return 0;
}
