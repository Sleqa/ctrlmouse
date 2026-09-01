// ControllerMouse (ctrlmouse) - map a game controller to mouse input (Windows).
//
// Left stick           -> mouse movement
// Right stick Y-axis   -> scroll wheel
// Cross/A button       -> left click
// Circle/B button      -> right click
//
// Uses DirectInput, so it works with DualSense/DualShock and other HID pads as
// well as XInput controllers - no extra software or drivers needed.
//
// A normal desktop window. Closing it hides it to the system tray (the mapping
// keeps running); double-click the tray icon to restore, or right-click it for
// Show / Quit. Settings persist in config.json next to the exe.
//
// Build: run compile.bat (MSVC) or use CMake.

#define _CRT_SECURE_NO_WARNINGS
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <wincodec.h>
#include <commoncontrols.h>
#include <shlobj.h>
extern "C" {
#include <hidsdi.h>
}
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// Enable Windows visual styles (themed common controls v6) so the UI uses the
// modern look instead of the classic grey Win95 controls.
#pragma comment(linker, "\"/manifestdependency:type='win32' "                  \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "              \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// --- Config ----------------------------------------------------------------
// Every controller action is rebindable. Fullscreen and the launcher are hold
// actions; the rest act on press or while held.
enum { F_LCLICK, F_RCLICK, F_KEYBOARD, F_PLAYPAUSE, F_FULLSCREEN,
       F_LAUNCHER, F_TOGGLE, F_COUNT };
static const char* kBindKeyA[F_COUNT] = {
    "bind_lclick", "bind_rclick", "bind_keyboard", "bind_playpause",
    "bind_fullscreen", "bind_launcher", "bind_toggle"};

struct Config {
    double mouse_sensitivity;   // pixels per poll at full stick deflection
    double scroll_sensitivity;  // scroll steps per poll at full deflection
    double deadzone;            // fraction of stick travel ignored near centre
    bool   enabled;
    bool   game_pause;          // auto-pause the mapping while a game is fullscreen
    int    fullscreen_key;      // 0 = F11, 1 = Alt+Enter, 2 = F
    double mouse_curve;         // 1 = linear; higher = finer near centre
    int    bind[F_COUNT];       // controller button per action
};

// Default toggle: 13 = touchpad click on a DualSense (unused by the mapping).
// Cross, Circle, Triangle, Square, Square (hold), Options (hold), Touchpad.
static const Config DEFAULTS = {18.0, 1.0, 0.15, true, true, 0, 2.0,
                                {1, 2, 3, 0, 0, 9, 13}};
static const wchar_t* MUTEX_NAME = L"ControllerMouse_SingleInstance";
static const wchar_t* CLASS_NAME = L"ControllerMouseWindow";

// --- Shared state (UI thread writes config, worker thread reads it) --------
static CRITICAL_SECTION g_cs;
static Config           g_cfg = DEFAULTS;
static volatile bool    g_running = true;
static volatile bool    g_connected = false;
static HANDLE           g_worker = NULL;
static HWND             g_hwnd = NULL;
static NOTIFYICONDATAW  g_nid = {};
static int              g_status_state = -1;
static wchar_t          g_status_txt[64] = L"Controller";
// Friendly name of the pad in use, shown in place of "Controller" once one is
// found. Written by the worker, read by the UI thread; worst case a repaint
// shows the previous name for one frame.
static wchar_t          g_pad_name[48] = L"Controller";
static wchar_t          g_mouse_val_txt[32] = L"";
static wchar_t          g_scroll_val_txt[32] = L"";
static wchar_t          g_dz_val_txt[32] = L"";
static wchar_t          g_curve_val_txt[32] = L"";

static Config get_cfg() {
    EnterCriticalSection(&g_cs);
    Config c = g_cfg;
    LeaveCriticalSection(&g_cs);
    return c;
}

// --- config.json (next to the exe) -----------------------------------------
// Settings live in %APPDATA%\ctrlmouse, not beside the exe: people run this
// straight out of Downloads, and clearing that folder was taking the settings
// with it.
static std::wstring exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring p(buf);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

static std::wstring data_dir() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    PWSTR base = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &base)) &&
        base) {
        std::wstring d(base);
        CoTaskMemFree(base);
        d += L"\\ctrlmouse";
        if (CreateDirectoryW(d.c_str(), NULL) ||
            GetLastError() == ERROR_ALREADY_EXISTS) {
            cached = d + L"\\";
            return cached;
        }
    }
    cached = exe_dir();   // fall back to the old location rather than fail
    return cached;
}

static std::wstring config_path() { return data_dir() + L"config.json"; }

// Bring settings written by an older build across, once.
static void migrate_old_data() {
    if (data_dir() == exe_dir()) return;
    const wchar_t* names[] = {L"config.json", L"apps.txt", L"hidhide_declined"};
    for (int i = 0; i < 3; i++) {
        std::wstring dst = data_dir() + names[i];
        std::wstring src = exe_dir() + names[i];
        if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(src.c_str()) != INVALID_FILE_ATTRIBUTES)
            MoveFileW(src.c_str(), dst.c_str());
    }
}

static void save_config(const Config& c) {
    std::wstring path = config_path();
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    fprintf(f,
            "{\n"
            "  \"mouse_sensitivity\": %.3f,\n"
            "  \"scroll_sensitivity\": %.3f,\n"
            "  \"deadzone\": %.3f,\n"
            "  \"enabled\": %s,\n"
            "  \"game_pause\": %s,\n"
            "  \"fullscreen_key\": %d,\n"
            "  \"mouse_curve\": %.2f",
            c.mouse_sensitivity, c.scroll_sensitivity, c.deadzone,
            c.enabled ? "true" : "false",
            c.game_pause ? "true" : "false", c.fullscreen_key,
            c.mouse_curve);
    // Flat keys rather than a nested object: the reader looks each name up
    // directly, so nesting would buy nothing and cost a real parser.
    for (int i = 0; i < F_COUNT; i++)
        fprintf(f, ",\n  \"%s\": %d", kBindKeyA[i], c.bind[i]);
    fprintf(f, "\n}\n");
    fclose(f);
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static bool parse_double(const std::string& s, const char* key, double& out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return false;
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return false;
    out = strtod(s.c_str() + p + 1, NULL);
    return true;
}

static void parse_bool(const std::string& s, const char* key, bool& out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return;
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return;
    size_t end = s.find_first_of(",}", p);
    out = s.substr(p, end - p).find("true") != std::string::npos;
}

static Config load_config() {
    Config c = DEFAULTS;
    FILE* f = _wfopen(config_path().c_str(), L"rb");
    if (!f) {
        save_config(c);
        return c;
    }
    std::string s;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    parse_double(s, "mouse_sensitivity", c.mouse_sensitivity);
    parse_double(s, "scroll_sensitivity", c.scroll_sensitivity);
    parse_double(s, "deadzone", c.deadzone);
    parse_bool(s, "enabled", c.enabled);
    parse_bool(s, "game_pause", c.game_pause);
    double tb;
    // Older configs stored only the toggle under its own name.
    if (parse_double(s, "toggle_button", tb)) c.bind[F_TOGGLE] = (int)tb;
    for (int i = 0; i < F_COUNT; i++) {
        double v;
        if (parse_double(s, kBindKeyA[i], v) && v >= 0 && v < 32)
            c.bind[i] = (int)v;
    }
    double fk;
    if (parse_double(s, "fullscreen_key", fk)) {
        c.fullscreen_key = (int)fk;
        if (c.fullscreen_key < 0 || c.fullscreen_key > 2) c.fullscreen_key = 0;
    }
    parse_double(s, "mouse_curve", c.mouse_curve);
    if (c.mouse_curve < 1.0) c.mouse_curve = 1.0;
    if (c.mouse_curve > 3.0) c.mouse_curve = 3.0;
    return c;
}

// --- Mouse output ----------------------------------------------------------
static void mouse_move(LONG dx, LONG dy) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

static void mouse_scroll(int steps) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.mouseData = (DWORD)(steps * WHEEL_DELTA);
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &in, sizeof(in));
}

static void mouse_button(DWORD flag) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flag;
    SendInput(1, &in, sizeof(in));
}

// Tap a virtual key. Used for the media keys, which Windows routes to
// whichever app owns media playback, so this works without knowing about it.
static void tap_key(WORD vk) {
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// Fullscreen has no system-wide key, so this sends whichever shortcut the
// user's player actually uses.
static void send_fullscreen(int which) {
    if (which == 1) {   // Alt+Enter
        INPUT in[4] = {};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_MENU;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = VK_RETURN;
        in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = VK_RETURN;
        in[2].ki.dwFlags = KEYEVENTF_KEYUP;
        in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_MENU;
        in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, in, sizeof(INPUT));
    } else {
        tap_key(which == 2 ? 'F' : VK_F11);
    }
}

static void edge_click(bool pressed, bool& prev, DWORD down, DWORD up) {
    if (pressed && !prev) {
        mouse_button(down);
        prev = true;
    } else if (!pressed && prev) {
        mouse_button(up);
        prev = false;
    }
}

// Release any held buttons (e.g. when the controller is unplugged).
static void edge_click_release_all(bool& a_down, bool& b_down) {
    if (a_down) { mouse_button(MOUSEEVENTF_LEFTUP); a_down = false; }
    if (b_down) { mouse_button(MOUSEEVENTF_RIGHTUP); b_down = false; }
}

// --- On-screen keyboard: state shared with the worker ------------------------
// The worker thread only detects controller events and posts them here; all
// window/state manipulation happens on the UI thread.
#define WM_GAMEPAD (WM_APP + 2)
enum { GP_KB_TOGGLE = 1, GP_KB_SELECT, GP_KB_BACKSPACE, GP_KB_NAV,
       GP_TOGGLE, GP_CAPTURED,
       GP_LX_TOGGLE, GP_LX_NAV, GP_LX_SELECT, GP_LX_CLOSE };

static volatile bool g_kb_visible = false;
static volatile bool g_lx_visible = false;   // app launcher popup

// --- Game detection / toggle-bind state (shared with the worker) -----------
static volatile bool g_game_active = false;  // fullscreen game detected
static volatile bool g_override    = false;  // user forced mapping on in-game
static volatile bool g_capture     = false;  // waiting for a new bind press
static volatile int  g_capture_feature = -1; // which action is being rebound

// True when a fullscreen game (or other fullscreen app) is in front. Two cheap
// checks, no process enumeration: the shell's own notification state (which
// reports exclusive D3D fullscreen), plus a "foreground window covers its whole
// monitor" heuristic to catch borderless-fullscreen games.
static bool is_game_running() {
    QUERY_USER_NOTIFICATION_STATE q;
    if (SUCCEEDED(SHQueryUserNotificationState(&q)) &&
        (q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE))
        return true;

    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd || fg == GetShellWindow()) return false;
    wchar_t cls[64] = L"";
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd"))
        return false;
    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

// Map a DirectInput POV hat value to 0=up,1=right,2=down,3=left (-1 centred).
static int pov_dir(DWORD pov) {
    if (LOWORD(pov) == 0xFFFF) return -1;
    return (int)(((pov + 4500) / 9000) % 4);
}

// Device instance IDs of every DualSense HID collection present, in the form
// SetupDiGetDeviceInstanceId returns - this is what HidHide blacklists by.
// Filled by hid_scan() below; declared here because the HidHide code uses it.
#define MAX_INST 16
static std::wstring g_pad_inst[MAX_INST];
static int          g_pad_inst_count = 0;

// --- HidHide integration ----------------------------------------------------
// Opening the pad exclusively is not enough to stop other software reacting to
// it: that only blocks other user-mode CreateFile opens, while RawInput
// consumers, the Game Bar and Steam Input keep getting fed by the OS itself.
// Genuinely hiding a device needs a kernel filter driver sitting under the HID
// stack, which is exactly what HidHide is. It is optional - without it the app
// still works, it just cannot stop the pad reaching other software.
//
// Contract from HidHide's Shared/HidHideIoctlContract.h.
#define HH_DEVICE_PATH L"\\\\.\\HidHide"
#define HH_CTL(n)      CTL_CODE(32769, (n), METHOD_BUFFERED, FILE_READ_DATA)
#define HH_GET_WHITELIST          HH_CTL(2048)
#define HH_SET_WHITELIST          HH_CTL(2049)
#define HH_GET_BLACKLIST          HH_CTL(2050)
#define HH_SET_BLACKLIST          HH_CTL(2051)
#define HH_GET_ACTIVE             HH_CTL(2052)
#define HH_SET_ACTIVE             HH_CTL(2053)
// NOTE: the session blacklist (2056/2057) exists only on HidHide's master
// branch - no released build implements it - so the persistent blacklist is
// the only option that works on an installable version. That means the change
// has to be rolled back explicitly, including after a crash; see
// hh_restore_file().
#define HH_RELEASES_URL L"https://github.com/nefarius/HidHide/releases/latest"

static HANDLE g_hh = INVALID_HANDLE_VALUE;
static bool   g_hh_whitelisted = false;   // are we allowed to see hidden pads?
static bool   g_hh_hiding = false;        // is the pad currently hidden?

static bool is_elevated() {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el = {};
    DWORD sz = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &sz);
    CloseHandle(tok);
    return ok && el.TokenIsElevated;
}

static bool hh_present() {
    HANDLE h = CreateFileW(HH_DEVICE_PATH, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static bool hh_open() {
    if (g_hh != INVALID_HANDLE_VALUE) return true;
    g_hh = CreateFileW(HH_DEVICE_PATH, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, 0, NULL);
    return g_hh != INVALID_HANDLE_VALUE;
}

static void hh_close() {
    if (g_hh != INVALID_HANDLE_VALUE) { CloseHandle(g_hh); g_hh = INVALID_HANDLE_VALUE; }
    g_hh_hiding = false;
}

// HidHide identifies applications by NT full image name, e.g.
// \Device\HarddiskVolume3\Apps\ctrlmouse.exe - volume-based so it survives a
// drive-letter change.
static bool hh_self_image_name(std::wstring& out) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH || path[1] != L':') return false;
    wchar_t drive[3] = {path[0], L':', 0};
    wchar_t devname[512];
    if (!QueryDosDeviceW(drive, devname, 512)) return false;
    out = std::wstring(devname) + (path + 2);
    return true;
}

// MULTI_SZ: each string null-terminated, list closed by one more null.
static std::wstring hh_multi_sz(const std::wstring* items, int n) {
    std::wstring b;
    for (int i = 0; i < n; i++) {
        if (items[i].empty()) continue;
        b += items[i];
        b.push_back(L'\0');
    }
    b.push_back(L'\0');
    return b;
}

static bool hh_ioctl(DWORD code, std::wstring& payload) {
    DWORD ret = 0;
    return DeviceIoControl(g_hh, code, (LPVOID)payload.data(),
                           (DWORD)(payload.size() * sizeof(wchar_t)),
                           NULL, 0, &ret, NULL) != 0;
}

// Add ourselves to HidHide's whitelist, preserving whatever is already there.
// This must succeed before anything is hidden, otherwise we would hide the pad
// from ourselves too.
static bool hh_whitelist_self() {
    std::wstring me;
    if (!hh_self_image_name(me)) return false;

    DWORD need = 0;
    DeviceIoControl(g_hh, HH_GET_WHITELIST, NULL, 0, NULL, 0, &need, NULL);
    std::wstring cur;
    if (need >= sizeof(wchar_t)) {
        cur.resize(need / sizeof(wchar_t));
        DWORD got = 0;
        if (!DeviceIoControl(g_hh, HH_GET_WHITELIST, NULL, 0, &cur[0],
                             (DWORD)(cur.size() * sizeof(wchar_t)), &got, NULL))
            return false;   // never overwrite a list we failed to read
        cur.resize(got / sizeof(wchar_t));
    }

    // Walk the MULTI_SZ looking for ourselves (paths are case-insensitive).
    std::wstring items[64];
    int n = 0;
    for (size_t i = 0; i < cur.size() && n < 63;) {
        size_t e = cur.find(L'\0', i);
        if (e == std::wstring::npos || e == i) break;
        items[n++] = cur.substr(i, e - i);
        i = e + 1;
    }
    for (int i = 0; i < n; i++)
        if (_wcsicmp(items[i].c_str(), me.c_str()) == 0) return true;  // already there

    items[n++] = me;
    std::wstring payload = hh_multi_sz(items, n);
    return hh_ioctl(HH_SET_WHITELIST, payload);
}

// Hide every DualSense collection from other software, for this session only.
// Session entries are owned by this process and the driver drops them if we
// exit or crash, so the pad can never be left hidden.
static BOOLEAN g_hh_prev_active = FALSE;
static bool    g_hh_changed_active = false;
static std::wstring g_hh_saved_blacklist;   // caller's list, before we touched it
static bool         g_hh_have_saved = false;

static bool hh_read_multi_sz(DWORD code, std::wstring& out) {
    DWORD need = 0;
    DeviceIoControl(g_hh, code, NULL, 0, NULL, 0, &need, NULL);
    out.clear();
    if (need < sizeof(wchar_t)) { out.push_back(L'\0'); return true; }
    out.resize(need / sizeof(wchar_t));
    DWORD got = 0;
    if (!DeviceIoControl(g_hh, code, NULL, 0, &out[0],
                         (DWORD)(out.size() * sizeof(wchar_t)), &got, NULL))
        return false;
    out.resize(got / sizeof(wchar_t));
    if (out.empty()) out.push_back(L'\0');
    return true;
}

// Because we have to mutate HidHide's persistent blacklist, a crash between
// hiding and restoring would leave the user's pad hidden with no obvious
// cause. So the original list is written to disk before the first change and
// replayed on the next start if it is still there.
static std::wstring hh_restore_file() {
    std::wstring p = config_path();
    p.resize(p.find_last_of(L"\\/") + 1);
    return p + L"hidhide_restore.bin";
}

static void hh_write_restore(const std::wstring& list) {
    HANDLE f = CreateFileW(hh_restore_file().c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, list.data(), (DWORD)(list.size() * sizeof(wchar_t)), &w, NULL);
    CloseHandle(f);
}

static void hh_clear_restore() { DeleteFileW(hh_restore_file().c_str()); }

// Replay a blacklist left behind by a previous run that did not shut down
// cleanly. Runs before we touch anything else.
static void hh_recover_blacklist() {
    HANDLE f = CreateFileW(hh_restore_file().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD sz = GetFileSize(f, NULL);
    if (sz && sz != INVALID_FILE_SIZE && (sz % sizeof(wchar_t)) == 0) {
        std::wstring list;
        list.resize(sz / sizeof(wchar_t));
        DWORD r = 0;
        if (ReadFile(f, &list[0], sz, &r, NULL) && r == sz)
            hh_ioctl(HH_SET_BLACKLIST, list);
    }
    CloseHandle(f);
    hh_clear_restore();
}

// Rebuild the device stack so HidHide's filter re-evaluates it. Without this
// the blacklist only takes effect the next time the pad is reconnected, which
// is what HidHide's own documentation tells users to do by hand.
static void hh_restart_devices() {
    for (int i = 0; i < g_pad_inst_count; i++) {
        DEVINST inst;
        if (CM_Locate_DevNodeW(&inst, (DEVINSTID_W)g_pad_inst[i].c_str(),
                               CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            continue;
        PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
        CM_Query_And_Remove_SubTreeW(inst, &veto, NULL, 0, CM_REMOVE_NO_RESTART);
        CM_Setup_DevNode(inst, CM_SETUP_DEVNODE_READY);
    }
}

static bool hh_hide(bool on) {
    if (g_hh == INVALID_HANDLE_VALUE || on == g_hh_hiding) return true;
    DWORD ret = 0;
    if (!on) {
        if (g_hh_have_saved) {
            hh_ioctl(HH_SET_BLACKLIST, g_hh_saved_blacklist);
            g_hh_have_saved = false;
            hh_clear_restore();
        }
        // Only put the global switch back if we were the ones who turned it
        // on. The user may be hiding other devices with it, and forcing it off
        // would silently break their own HidHide setup.
        if (g_hh_changed_active) {
            DeviceIoControl(g_hh, HH_SET_ACTIVE, &g_hh_prev_active,
                            sizeof(g_hh_prev_active), NULL, 0, &ret, NULL);
            g_hh_changed_active = false;
        }
        g_hh_hiding = false;
        hh_restart_devices();
        return true;
    }
    if (!g_hh_whitelisted || !g_pad_inst_count) return false;

    // Append our pad to whatever the user already hides, never replace it.
    if (!g_hh_have_saved) {
        if (!hh_read_multi_sz(HH_GET_BLACKLIST, g_hh_saved_blacklist))
            return false;   // never overwrite a list we could not read
        hh_write_restore(g_hh_saved_blacklist);
        g_hh_have_saved = true;
    }
    std::wstring items[MAX_INST + 64];
    int n = 0;
    for (size_t i = 0; i < g_hh_saved_blacklist.size() && n < 64;) {
        size_t e = g_hh_saved_blacklist.find(L'\0', i);
        if (e == std::wstring::npos || e == i) break;
        items[n++] = g_hh_saved_blacklist.substr(i, e - i);
        i = e + 1;
    }
    for (int i = 0; i < g_pad_inst_count && n < MAX_INST + 64; i++) {
        bool dup = false;
        for (int j = 0; j < n; j++)
            if (_wcsicmp(items[j].c_str(), g_pad_inst[i].c_str()) == 0) dup = true;
        if (!dup) items[n++] = g_pad_inst[i];
    }
    std::wstring payload = hh_multi_sz(items, n);
    if (!hh_ioctl(HH_SET_BLACKLIST, payload)) return false;

    // HidHide has a global on/off switch, and a blacklist means nothing while
    // it is off. Read the old value if we can - purely so we can put it back -
    // but always write TRUE, and treat failure to do so as failure to hide.
    // Making the write conditional on a successful read is what previously let
    // this report success while HidHide was globally disabled.
    BOOLEAN prev = FALSE;
    bool have_prev = DeviceIoControl(g_hh, HH_GET_ACTIVE, NULL, 0, &prev,
                                     sizeof(prev), &ret, NULL) != 0;
    BOOLEAN on_val = TRUE;
    if (!DeviceIoControl(g_hh, HH_SET_ACTIVE, &on_val, sizeof(on_val),
                         NULL, 0, &ret, NULL))
        return false;
    g_hh_changed_active = have_prev && !prev;   // only restore what we changed
    g_hh_prev_active = prev;

    // Confirm it really is on rather than trusting the write.
    BOOLEAN now = FALSE;
    if (DeviceIoControl(g_hh, HH_GET_ACTIVE, NULL, 0, &now, sizeof(now),
                        &ret, NULL) && !now)
        return false;

    g_hh_hiding = true;
    hh_restart_devices();   // make the filter re-evaluate the pad right away
    return true;
}

// --- Normalised pad state ---------------------------------------------------
// Both input backends below fill this, so the worker loop does not care which
// one is in use. Axes are [-1000, 1000]; button bit indices deliberately match
// the DirectInput button order for a DualSense, so an existing
// config.json "toggle_button" keeps meaning the same physical button.
//   0 Square  1 Cross  2 Circle  3 Triangle  4 L1  5 R1  6 L2  7 R2
//   8 Create  9 Options  10 L3  11 R3  12 PS  13 Touchpad
struct PadState {
    int      lx, ly, rx, ry;
    unsigned mask;
    int      hat;   // 0=up,1=right,2=down,3=left, -1 centred
};

// --- Raw HID backend (DualSense) --------------------------------------------
// Why this exists: any app that takes direct HID control of a DualSense (game
// streaming clients such as Artemis/Moonlight, DS4Windows, Steam) switches the
// pad from its basic Bluetooth report (0x01) into the extended report (0x31).
// The pad stays in that mode after the app exits, and Windows' own HID game
// controller mapping cannot decode it - joy.cpl goes dead, DirectInput reports
// nothing, and only re-pairing Bluetooth resets it. Reading the reports
// ourselves sidesteps the whole problem: we understand both formats, so the
// mode the pad happens to be in stops mattering.
#define SONY_VID          0x054C
#define PID_DUALSENSE     0x0CE6
#define PID_DUALSENSE_EDGE 0x0DF2

static HANDLE   g_hid = INVALID_HANDLE_VALUE;
static OVERLAPPED g_hid_ov = {};
static BYTE     g_hid_buf[256];
static bool     g_hid_pending = false;
static USHORT   g_hid_inlen = 0;
static PadState g_hid_state = {};
static bool     g_hid_exclusive = false;   // did we actually get exclusive access?
static bool     g_hid_bt = false;          // Bluetooth transport (vs USB)
// Bumped on every (re)open. The worker uses it to tell that its edge-detection
// state refers to a handle that no longer exists.
static unsigned g_hid_gen = 0;
// False until a genuine report has been parsed on the current handle. Until
// then hid_poll can only hand back the zeroed placeholder from the open, which
// must not be mistaken for "every button released".
static bool     g_hid_have_report = false;

static void hid_close() {
    if (g_hid != INVALID_HANDLE_VALUE) {
        if (g_hid_pending) { CancelIo(g_hid); g_hid_pending = false; }
        CloseHandle(g_hid);
        g_hid = INVALID_HANDLE_VALUE;
    }
    if (g_hid_ov.hEvent) { CloseHandle(g_hid_ov.hEvent); g_hid_ov.hEvent = NULL; }
    memset(&g_hid_state, 0, sizeof(g_hid_state));
    g_hid_state.hat = -1;
}

static bool hid_try_path(const wchar_t* path, bool exclusive) {
    // Exclusive (share mode 0) stops other user-mode apps opening the pad, so
    // D-pad/L3/face buttons stop leaking into whatever else is running while
    // we own the controller. Shared lets a streaming client or Steam hold it
    // at the same time; we only ever read either way.
    HANDLE h = CreateFileW(path, GENERIC_READ,
                           exclusive ? 0 : (FILE_SHARE_READ | FILE_SHARE_WRITE),
                           NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    HIDD_ATTRIBUTES attr = {sizeof(attr)};
    if (!HidD_GetAttributes(h, &attr) || attr.VendorID != SONY_VID ||
        (attr.ProductID != PID_DUALSENSE && attr.ProductID != PID_DUALSENSE_EDGE)) {
        CloseHandle(h);
        return false;
    }
    PHIDP_PREPARSED_DATA pp = NULL;
    HIDP_CAPS caps = {};
    if (!HidD_GetPreparsedData(h, &pp)) { CloseHandle(h); return false; }
    bool ok = HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS;
    HidD_FreePreparsedData(pp);
    // Skip the vendor-defined collections the DualSense also exposes; only the
    // gamepad collection has input reports big enough to be the real thing.
    if (!ok || caps.InputReportByteLength < 10) { CloseHandle(h); return false; }

    g_hid = h;
    g_hid_inlen = caps.InputReportByteLength;
    if (g_hid_inlen > sizeof(g_hid_buf)) g_hid_inlen = sizeof(g_hid_buf);
    // Transport decides how a report ID of 0x01 is laid out, and it cannot be
    // inferred from the size of a received report: HID ReadFile always returns
    // the full advertised report length, zero-padded, so a 10-byte Bluetooth
    // report arrives as 78 bytes. A DualSense advertises 64-byte input reports
    // over USB and 78 over Bluetooth.
    g_hid_bt = caps.InputReportByteLength > 64;
    {
        std::wstring p(path);
        for (size_t i = 0; i < p.size(); i++) p[i] = (wchar_t)towlower(p[i]);
        if (p.find(L"bth") != std::wstring::npos) g_hid_bt = true;
    }
    g_hid_ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hid_pending = false;
    memset(&g_hid_state, 0, sizeof(g_hid_state));
    g_hid_state.hat = -1;
    g_hid_gen++;
    g_hid_have_report = false;
    wcscpy(g_pad_name, attr.ProductID == PID_DUALSENSE_EDGE ? L"DualSense Edge"
                                                            : L"DualSense");
    return true;
}

// Ask the device what it is rather than pattern-matching its path. Path
// formats differ by transport - USB gives HID\VID_054C&PID_0CE6\..., while
// Bluetooth gives HID\{00001124-...}_VID&0002054C_PID&0CE6\... - so any
// string match silently misses one of them. Opening with zero desired access
// is a query-only open: it always succeeds and never conflicts with an
// exclusive handle.
// Reduce a raw HID/DirectInput product string to something worth showing.
// Pads rarely report a tidy name - a DualShock 4 calls itself "Wireless
// Controller" - so match the families we know and fall back to the raw name.
static void set_pad_name(const wchar_t* raw) {
    std::wstring s(raw ? raw : L"");
    for (size_t i = 0; i < s.size(); i++) s[i] = (wchar_t)towlower(s[i]);
    const wchar_t* name = NULL;
    if (s.find(L"dualsense") != std::wstring::npos) name = L"DualSense";
    else if (s.find(L"dualshock") != std::wstring::npos) name = L"DualShock";
    else if (s.find(L"xbox") != std::wstring::npos) name = L"Xbox Controller";
    else if (s.find(L"pro controller") != std::wstring::npos ||
             s.find(L"switch") != std::wstring::npos) name = L"Switch Controller";
    else if (s.find(L"wireless controller") != std::wstring::npos) name = L"DualShock";
    if (name) wcscpy(g_pad_name, name);
    else if (raw && *raw) { wcsncpy(g_pad_name, raw, 47); g_pad_name[47] = 0; }
    else wcscpy(g_pad_name, L"Third Party");
}

static bool path_is_dualsense(const wchar_t* path) {
    HANDLE q = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (q == INVALID_HANDLE_VALUE) return false;
    HIDD_ATTRIBUTES a = {sizeof(a)};
    bool ok = HidD_GetAttributes(q, &a) != FALSE && a.VendorID == SONY_VID &&
              (a.ProductID == PID_DUALSENSE || a.ProductID == PID_DUALSENSE_EDGE);
    CloseHandle(q);
    return ok;
}

static bool hid_scan(bool exclusive) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return false;

    bool opened = false;
    g_pad_inst_count = 0;
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &hidGuid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* det =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)malloc(need);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dev = {sizeof(dev)};
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, NULL, &dev)) {
            // Record every DualSense collection, not just the one we read
            // from: hiding only the gamepad collection would leave the others
            // visible to whatever else is listening.
            if (path_is_dualsense(det->DevicePath) && g_pad_inst_count < MAX_INST) {
                wchar_t inst[512];
                if (SetupDiGetDeviceInstanceIdW(set, &dev, inst, 512, NULL))
                    g_pad_inst[g_pad_inst_count++] = inst;
            }
            if (!opened && hid_try_path(det->DevicePath, exclusive)) opened = true;
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(set);
    return opened;
}

// Exclusive is best-effort: if something already holds the pad we still want
// to work, just without blocking it.
static bool hid_open(bool exclusive) {
    if (g_hid != INVALID_HANDLE_VALUE) return true;
    if (exclusive && hid_scan(true)) { g_hid_exclusive = true; return true; }
    if (hid_scan(false)) { g_hid_exclusive = false; return true; }
    return false;
}

static inline int hid_axis(BYTE v) {   // 0..255 (128 centre) -> -1000..1000
    int n = ((int)v - 128) * 1000 / 127;
    if (n > 1000) n = 1000;
    if (n < -1000) n = -1000;
    return n;
}

// DualSense hat: 0=N,1=NE,2=E,3=SE,4=S,5=SW,6=W,7=NW,8+=centred.
static inline int hid_hat(BYTE b) {
    int h = b & 0x0F;
    if (h > 7) return -1;
    return ((h + 1) / 2) % 4;
}

static void hid_buttons(const BYTE* b, PadState& st) {
    st.hat = hid_hat(b[0]);
    unsigned m = 0;
    if (b[0] & 0x10) m |= 1u << 0;    // Square
    if (b[0] & 0x20) m |= 1u << 1;    // Cross
    if (b[0] & 0x40) m |= 1u << 2;    // Circle
    if (b[0] & 0x80) m |= 1u << 3;    // Triangle
    if (b[1] & 0x01) m |= 1u << 4;    // L1
    if (b[1] & 0x02) m |= 1u << 5;    // R1
    if (b[1] & 0x04) m |= 1u << 6;    // L2
    if (b[1] & 0x08) m |= 1u << 7;    // R2
    if (b[1] & 0x10) m |= 1u << 8;    // Create
    if (b[1] & 0x20) m |= 1u << 9;    // Options
    if (b[1] & 0x40) m |= 1u << 10;   // L3
    if (b[1] & 0x80) m |= 1u << 11;   // R3
    if (b[2] & 0x01) m |= 1u << 12;   // PS
    if (b[2] & 0x02) m |= 1u << 13;   // Touchpad click
    st.mask = m;
}

// The three report layouts the pad can be in. USB 0x01 and Bluetooth extended
// 0x31 share a payload that differs only by a one-byte header shift; the short
// Bluetooth 0x01 report orders its fields differently.
static bool hid_parse(const BYTE* buf, DWORD len, PadState& st) {
    if (len < 10) return false;
    if (buf[0] == 0x01 && g_hid_bt) {          // Bluetooth basic
        // Same axis offsets as the USB layout, but the button bytes sit three
        // earlier - which is why getting this branch wrong leaves the sticks
        // working and every button dead.
        st.lx = hid_axis(buf[1]);
        st.ly = hid_axis(buf[2]);
        st.rx = hid_axis(buf[3]);
        st.ry = hid_axis(buf[4]);
        hid_buttons(buf + 5, st);
        return true;
    }
    int off;
    if (buf[0] == 0x01) off = 1;               // USB full report
    else if (buf[0] == 0x31) off = 2;          // Bluetooth extended
    else return false;
    if (len < (DWORD)off + 10) return false;
    st.lx = hid_axis(buf[off + 0]);
    st.ly = hid_axis(buf[off + 1]);
    st.rx = hid_axis(buf[off + 2]);
    st.ry = hid_axis(buf[off + 3]);
    hid_buttons(buf + off + 7, st);
    return true;
}

// Drain every report queued since the last call and keep the newest, so input
// never lags behind a pad that reports faster than this loop runs.
static bool hid_poll(PadState& out) {
    if (g_hid == INVALID_HANDLE_VALUE) return false;
    bool alive = true;
    for (int guard = 0; guard < 64; guard++) {
        if (!g_hid_pending) {
            ResetEvent(g_hid_ov.hEvent);
            if (!ReadFile(g_hid, g_hid_buf, g_hid_inlen, NULL, &g_hid_ov)) {
                if (GetLastError() != ERROR_IO_PENDING) { alive = false; break; }
            }
            g_hid_pending = true;
        }
        if (WaitForSingleObject(g_hid_ov.hEvent, 0) != WAIT_OBJECT_0) break;
        DWORD got = 0;
        if (!GetOverlappedResult(g_hid, &g_hid_ov, &got, FALSE)) { alive = false; break; }
        g_hid_pending = false;
        if (hid_parse(g_hid_buf, got, g_hid_state)) g_hid_have_report = true;
    }
    if (!alive) { hid_close(); return false; }
    out = g_hid_state;
    return true;
}

// --- DirectInput controller worker -----------------------------------------
// DirectInput axes are mapped (after SetProperty below) to [-1000, 1000]:
//   lX  = left stick X     lY  = left stick Y     lRz = right stick Y
// Buttons (DualSense / DualShock layout): [1] = Cross, [2] = Circle.
static LPDIRECTINPUT8       g_di = NULL;
static LPDIRECTINPUTDEVICE8 g_dev = NULL;

static double apply_deadzone(double value, double dz) {
    double a = fabs(value);
    if (a < dz) return 0.0;
    double scaled = (a - dz) / (1.0 - dz);
    if (scaled > 1.0) scaled = 1.0;
    return (value > 0 ? 1.0 : -1.0) * scaled;
}

static double norm(LONG raw, double dz) {
    double n = raw / 1000.0;
    if (n > 1.0) n = 1.0;
    if (n < -1.0) n = -1.0;
    return apply_deadzone(n, dz);
}

#define MAX_PADS 8
static GUID g_pad_guids[MAX_PADS];
static int  g_pad_count = 0;
static GUID g_open_guid = {};        // pad we currently have open
static GUID g_last_good = {};        // last pad that actually produced input
static bool g_have_last_good = false;

static std::wstring g_pad_names[MAX_PADS];

static BOOL CALLBACK enum_cb(const DIDEVICEINSTANCEW* inst, void*) {
    if (g_pad_count < MAX_PADS) {
        g_pad_names[g_pad_count] = inst->tszProductName;
        g_pad_guids[g_pad_count++] = inst->guidInstance;
    }
    return DIENUM_CONTINUE;   // collect them all; ensure_device() picks
}

static void set_axis_range(DWORD offset) {
    DIPROPRANGE pr = {};
    pr.diph.dwSize = sizeof(DIPROPRANGE);
    pr.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    pr.diph.dwHow = DIPH_BYOFFSET;
    pr.diph.dwObj = offset;
    pr.lMin = -1000;
    pr.lMax = 1000;
    g_dev->SetProperty(DIPROP_RANGE, &pr.diph);
}

static void drop_device() {
    if (g_dev) {
        g_dev->Unacquire();
        g_dev->Release();
        g_dev = NULL;
    }
}

// Open one specific pad. Returns false (leaving no device open) unless it was
// configured completely - a half-configured device polls fine but reports
// nothing, which is indistinguishable from a dead controller.
static bool try_open(const GUID& guid) {
    if (FAILED(g_di->CreateDevice(guid, &g_dev, NULL))) {
        g_dev = NULL;
        return false;
    }
    if (FAILED(g_dev->SetDataFormat(&c_dfDIJoystick2)) ||
        FAILED(g_dev->SetCooperativeLevel(g_hwnd,
                                          DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
        drop_device();
        return false;
    }
    set_axis_range(DIJOFS_X);
    set_axis_range(DIJOFS_Y);
    set_axis_range(DIJOFS_RZ);
    g_dev->Acquire();   // may fail transiently; the poll loop retries
    g_open_guid = guid;
    g_hid_gen++;
    return true;
}

static bool ensure_device() {
    if (g_dev) return true;
    if (!g_di) {
        if (FAILED(DirectInput8Create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION,
                                      IID_IDirectInput8, (void**)&g_di, NULL)))
            return false;
    }
    g_pad_count = 0;
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_cb, NULL, DIEDFL_ATTACHEDONLY);
    if (!g_pad_count) return false;

    // Prefer the pad we last actually received input from. Games (and Steam
    // Input) can register virtual controllers that enumerate ahead of the real
    // one; binding to a virtual pad looks exactly like a dead controller, and
    // it persists across app restarts because the enumeration order does.
    if (g_have_last_good)
        for (int i = 0; i < g_pad_count; i++)
            if (IsEqualGUID(g_pad_guids[i], g_last_good) && try_open(g_pad_guids[i])) {
                set_pad_name(g_pad_names[i].c_str());
                return true;
            }

    for (int i = 0; i < g_pad_count; i++)
        if (try_open(g_pad_guids[i])) {
            set_pad_name(g_pad_names[i].c_str());
            return true;
        }
    return false;
}

static DWORD WINAPI worker_thread(LPVOID) {
    double scroll_accum = 0.0;
    double move_ax = 0.0, move_ay = 0.0;   // sub-pixel cursor remainder
    bool a_down = false, b_down = false;

    int dpad_prev = -1;
    ULONGLONG dpad_t0 = 0, dpad_last = 0;  // hold-to-repeat timing
    unsigned btn_mask_prev = 0;            // all-button mask, for edge detection
    ULONGLONG hold_t0[F_COUNT] = {};       // when each bound button went down
    bool hold_fired[F_COUNT] = {};         // its hold action already ran
    ULONGLONG tbtn_last_fire = 0;          // debounce reference for the toggle
    ULONGLONG gamechk_last = 0;            // last fullscreen-game check
    bool game_prev = false;                // previous fullscreen-game state
    // Whether the pad should be held exclusively right now. Computed at the
    // end of each iteration from the mapping state, so the pad is grabbed only
    // while we are actually driving the mouse and is handed straight back the
    // moment the mapping is switched off or pauses for a game.
    bool want_exclusive = false;
    int  open_fail_streak = 0;             // consecutive failures to see any pad
    unsigned hid_gen_seen = 0;             // handle generation our edges refer to
    // Media controls (D-pad + Square) while the on-screen keyboard is closed.
    int       media_dir = -1;              // D-pad direction being held
    ULONGLONG media_t0 = 0, media_last = 0;
    int       media_reps = 0;              // repeats so far, drives acceleration


    while (g_running) {
        Config cfg = get_cfg();
        bool a = false, b = false;

        // Prefer raw HID (a DualSense we can read in any report mode); fall
        // back to DirectInput for every other pad.
        PadState st = {};
        st.hat = -1;
        bool got = false;

        if (g_hid != INVALID_HANDLE_VALUE || (!g_dev && hid_open(want_exclusive)))
            got = hid_poll(st);

        if (!got && g_hid == INVALID_HANDLE_VALUE) {
            if (!ensure_device()) {
                g_connected = false;
                scroll_accum = 0.0;
                edge_click_release_all(a_down, b_down);
                // If we are hiding the pad and can no longer see it either,
                // the whitelist is not doing its job - unhide rather than sit
                // there having made the controller invisible to everyone.
                if (g_hh_hiding && ++open_fail_streak >= 8) {
                    hh_hide(false);
                    g_hh_whitelisted = false;   // stop re-hiding until restart
                    open_fail_streak = 0;
                }
                Sleep(400);
                continue;
            }
            open_fail_streak = 0;
            DIJOYSTATE2 js;
            HRESULT hr = g_dev->Poll();
            if (FAILED(hr)) {
                hr = g_dev->Acquire();
                if (SUCCEEDED(hr)) hr = g_dev->Poll();  // fresh data after re-acquiring
            }
            if (SUCCEEDED(hr)) hr = g_dev->GetDeviceState(sizeof(js), &js);

            if (FAILED(hr)) {  // unplugged, or another app took the device
                drop_device();
                g_connected = false;
                scroll_accum = 0.0;
                // Let go of anything we are holding down. Without this an
                // injected LEFTDOWN outlives the app: the desktop is stuck
                // mid-drag, and even killing the process cannot clear it,
                // because the button state lives in the OS input stack
                // rather than in here.
                edge_click_release_all(a_down, b_down);
                for (int f = 0; f < F_COUNT; f++) hold_fired[f] = true;
                dpad_prev = -1;
                btn_mask_prev = 0;
                Sleep(300);
                continue;
            }
            st.lx = js.lX;
            st.ly = js.lY;
            st.ry = js.lRz;
            st.hat = pov_dir(js.rgdwPOV[0]);
            for (int bi = 0; bi < 32; bi++)
                if (js.rgbButtons[bi] & 0x80) st.mask |= (1u << bi);
            if (st.mask || st.lx || st.ly || st.ry || st.hat != -1) {
                g_last_good = g_open_guid;   // remember the real pad, not a virtual one
                g_have_last_good = true;
            }
            got = true;
        }

        if (!got) {   // HID pad went away mid-read
            g_connected = false;
            scroll_accum = 0.0;
            move_ax = move_ay = 0.0;
            edge_click_release_all(a_down, b_down);
            for (int f = 0; f < F_COUNT; f++) hold_fired[f] = true;
            dpad_prev = -1;
            btn_mask_prev = 0;
            Sleep(300);
            continue;
        }

        g_connected = true;
        unsigned mask = st.mask;

        // A reopened handle starts with no history, so a button still held
        // across the reopen would look like a brand new press. That is what
        // made the toggle button fire twice: toggling changes the hide state,
        // which rebuilds the device stack, which reopens the handle - all
        // while the touchpad is still physically down. Treat everything as
        // already-held; these clear themselves on the first poll that shows a
        // button released.
        // Wait for a genuine report before reconciling, then adopt exactly
        // what the pad currently reads. Adopting the real state (rather than
        // assuming everything is held) means a button spanning the reopen
        // produces no edge, while one released during it still works on its
        // next press.
        if (g_hid_gen != hid_gen_seen &&
            (g_hid == INVALID_HANDLE_VALUE || g_hid_have_report)) {
            hid_gen_seen = g_hid_gen;
            btn_mask_prev = mask;              // adopt every button at once
            for (int f = 0; f < F_COUNT; f++) hold_fired[f] = true;
            dpad_prev = st.hat;
            media_dir = st.hat;
        }

        // Edge helpers over the raw mask, so every action reads its own bound
        // button rather than a hard-coded index.
        unsigned prev_mask = btn_mask_prev;
        auto bit = [&](int f) {
            int b = cfg.bind[f];
            return (b >= 0 && b < 32) ? b : -1;
        };
        auto is_down = [&](int f) {
            int b = bit(f);
            return b >= 0 && ((mask >> b) & 1) != 0;
        };
        auto went_down = [&](int f) {
            int b = bit(f);
            return b >= 0 && ((mask >> b) & 1) && !((prev_mask >> b) & 1);
        };
        auto went_up = [&](int f) {
            int b = bit(f);
            return b >= 0 && !((mask >> b) & 1) && ((prev_mask >> b) & 1);
        };

        ULONGLONG bnow = GetTickCount64();
        for (int f = 0; f < F_COUNT; f++) {
            if (went_down(f)) { hold_t0[f] = bnow; hold_fired[f] = false; }
            if (went_up(f))   { hold_fired[f] = false; }
        }

        if (g_capture) {
            // Rebinding: the first newly pressed button is the new binding.
            unsigned fresh = mask & ~prev_mask;
            if (fresh) {
                int idx = 0;
                while (!(fresh & (1u << idx))) idx++;
                PostMessageW(g_hwnd, WM_GAMEPAD, GP_CAPTURED, idx);
            }
        } else {
            // Works even while the mapping is off, so it can turn it back on.
            // Debounced: toggling rebuilds the device stack and a touchpad
            // click can bounce, either of which can present a second edge
            // within a few tens of milliseconds.
            if (went_down(F_TOGGLE) && bnow - tbtn_last_fire >= 300) {
                tbtn_last_fire = bnow;
                PostMessageW(g_hwnd, WM_GAMEPAD, GP_TOGGLE, 0);
            }
        }
        btn_mask_prev = mask;

        // Game check: at most two cheap API calls every 2 seconds.
        if (cfg.game_pause) {
            ULONGLONG gnow = GetTickCount64();
            if (gnow - gamechk_last >= 2000) {
                g_game_active = is_game_running();
                gamechk_last = gnow;
            }
        } else {
            g_game_active = false;
        }
        if (!g_game_active) g_override = false;  // override lasts one game session

        // A fullscreen game just exited. Re-open the pad from scratch: an
        // acquisition held across a game that grabbed the device can survive
        // in a state where it polls and reads fine but never reports input
        // again - which looked like a soft lock that only replugging fixed.
        if (game_prev && !g_game_active) {
            drop_device();
            hid_close();   // reopened next iteration, in whatever mode it is now in
            edge_click_release_all(a_down, b_down);
            for (int f = 0; f < F_COUNT; f++) hold_fired[f] = true;
            dpad_prev = -1;
            btn_mask_prev = mask;
            game_prev = false;
            Sleep(150);
            continue;
        }
        game_prev = g_game_active;

        bool mapping_on = cfg.enabled &&
                          !(cfg.game_pause && g_game_active && !g_override);

        if (mapping_on && !g_capture) {
            // Stick to cursor movement. Three things matter here for fine
            // control, and they have to work together:
            //
            //  * Radial deadzone and magnitude. Treating the axes separately
            //    lets a diagonal reach a magnitude of 1.41, so diagonals ran
            //    faster than cardinals; this normalises the vector instead.
            //  * A response curve. Raising the normalised magnitude to a power
            //    keeps full deflection at full speed while stretching the
            //    slow end of the range over much more stick travel, which is
            //    what makes small adjustments possible at a high sensitivity.
            //  * Sub-pixel accumulation. Rounding each poll independently
            //    discards anything under half a pixel, so below a certain
            //    deflection the cursor simply would not move no matter how
            //    gentle the curve. The remainder is carried to the next poll.
            double rx = st.lx / 1000.0, ry = st.ly / 1000.0;
            double m = sqrt(rx * rx + ry * ry);
            if (m > 1.0) { rx /= m; ry /= m; m = 1.0; }
            if (m > cfg.deadzone) {
                double t = (m - cfg.deadzone) / (1.0 - cfg.deadzone);
                double speed = pow(t, cfg.mouse_curve) * cfg.mouse_sensitivity;
                move_ax += (rx / m) * speed;
                move_ay += (ry / m) * speed;   // Y is screen-oriented
                LONG dx = (LONG)move_ax, dy = (LONG)move_ay;
                if (dx || dy) {
                    mouse_move(dx, dy);
                    move_ax -= dx;
                    move_ay -= dy;
                }
            } else {
                move_ax = move_ay = 0.0;
            }

            double nrz = norm(st.ry, cfg.deadzone);  // right stick Y
            if (nrz != 0.0) {
                scroll_accum += nrz * cfg.scroll_sensitivity;  // stick up -> scroll down
                int steps = (int)scroll_accum;
                if (steps) {
                    mouse_scroll(steps);
                    scroll_accum -= steps;
                }
            } else {
                scroll_accum = 0.0;
            }

            if (went_down(F_KEYBOARD))
                PostMessageW(g_hwnd, WM_GAMEPAD, GP_KB_TOGGLE, 0);

            // Launcher is a hold, checked before the popups so it works
            // whichever of them happens to be up.
            if (is_down(F_LAUNCHER) && !hold_fired[F_LAUNCHER] &&
                bnow - hold_t0[F_LAUNCHER] >= 500) {
                PostMessageW(g_hwnd, WM_GAMEPAD, GP_LX_TOGGLE, 0);
                hold_fired[F_LAUNCHER] = true;
            }

            if (g_lx_visible) {
                // Launcher owns navigation and the click buttons while up.
                media_dir = st.hat;
                if (went_down(F_LCLICK))
                    PostMessageW(g_hwnd, WM_GAMEPAD, GP_LX_SELECT, 0);
                if (went_down(F_RCLICK))
                    PostMessageW(g_hwnd, WM_GAMEPAD, GP_LX_CLOSE, 0);
                int dir = st.hat;
                if (dir != dpad_prev) {
                    if (dir != -1) {
                        PostMessageW(g_hwnd, WM_GAMEPAD, GP_LX_NAV, dir);
                        dpad_t0 = dpad_last = bnow;
                    }
                    dpad_prev = dir;
                } else if (dir != -1 && bnow - dpad_t0 >= 400 &&
                           bnow - dpad_last >= 110) {
                    PostMessageW(g_hwnd, WM_GAMEPAD, GP_LX_NAV, dir);
                    dpad_last = bnow;
                }
            } else if (g_kb_visible) {
                // Keep media state in step while the keyboard owns the D-pad,
                // so closing it with a direction held doesn't fire.
                media_dir = st.hat;
                if (went_down(F_LCLICK))
                    PostMessageW(g_hwnd, WM_GAMEPAD, GP_KB_SELECT, 0);
                if (went_down(F_RCLICK))
                    PostMessageW(g_hwnd, WM_GAMEPAD, GP_KB_BACKSPACE, 0);
                // D-pad with hold-to-repeat: first move immediately, then
                // after 400ms repeat every 110ms while held.
                int dir = st.hat;
                if (dir != dpad_prev) {
                    if (dir != -1) {
                        PostMessageW(g_hwnd, WM_GAMEPAD, GP_KB_NAV, dir);
                        dpad_t0 = dpad_last = bnow;
                    }
                    dpad_prev = dir;
                } else if (dir != -1 && bnow - dpad_t0 >= 400 &&
                           bnow - dpad_last >= 110) {
                    PostMessageW(g_hwnd, WM_GAMEPAD, GP_KB_NAV, dir);
                    dpad_last = bnow;
                }
            } else {
                a = is_down(F_LCLICK);
                b = is_down(F_RCLICK);
                dpad_prev = -1;

                // Fullscreen is a hold.
                if (is_down(F_FULLSCREEN) && !hold_fired[F_FULLSCREEN] &&
                    bnow - hold_t0[F_FULLSCREEN] >= 600) {
                    send_fullscreen(cfg.fullscreen_key);
                    hold_fired[F_FULLSCREEN] = true;
                }
                // Play/pause is a tap. If it shares a button with fullscreen -
                // as it does by default - it can only fire on release, once a
                // hold has been ruled out. On its own button it fires at once.
                if (cfg.bind[F_PLAYPAUSE] == cfg.bind[F_FULLSCREEN]) {
                    if (went_up(F_PLAYPAUSE) && !hold_fired[F_FULLSCREEN])
                        tap_key(VK_MEDIA_PLAY_PAUSE);
                } else if (went_down(F_PLAYPAUSE)) {
                    tap_key(VK_MEDIA_PLAY_PAUSE);
                }

                // Media on the D-pad: up/down are system-wide volume keys,
                // left/right send arrow keys, which is how players seek.
                int dir = st.hat;
                WORD mk = 0;
                if (dir == 0)      mk = VK_VOLUME_UP;
                else if (dir == 2) mk = VK_VOLUME_DOWN;
                else if (dir == 1) mk = VK_RIGHT;
                else if (dir == 3) mk = VK_LEFT;
                if (dir != media_dir) {
                    media_dir = dir;
                    media_t0 = media_last = bnow;
                    media_reps = 0;
                    if (mk) tap_key(mk);
                } else if (mk && bnow - media_t0 >= 350) {
                    // Speeds up the longer it is held: 140ms down to 40ms.
                    // Signed on purpose - in unsigned this wrapped once the
                    // subtraction went negative and stalled the repeat.
                    int gap = 140 - media_reps * 8;
                    if (gap < 40) gap = 40;
                    if (bnow - media_last >= (ULONGLONG)gap) {
                        tap_key(mk);
                        media_last = bnow;
                        media_reps++;
                    }
                }
            }
        } else {
            scroll_accum = 0.0;
            move_ax = move_ay = 0.0;
            dpad_prev = -1;
            // Track the pad while paused too, so re-enabling with something
            // already held does not fire an action.
            media_dir = st.hat;
            for (int f = 0; f < F_COUNT; f++) hold_fired[f] = true;
        }

        btn_mask_prev = mask;

        edge_click(a, a_down, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
        edge_click(b, b_down, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);

        // Re-open the pad if the required access level changed. Dropping the
        // handle is what hands the controller back to other apps, so this is
        // also what makes the toggle button work as a "give me my pad back"
        // gesture mid-stream.
        bool now_exclusive = mapping_on && !g_capture;
        if (now_exclusive != want_exclusive) {
            want_exclusive = now_exclusive;
            if (g_hid != INVALID_HANDLE_VALUE && g_hid_exclusive != now_exclusive)
                hid_close();   // reopened at the top of the next iteration
        }

        // Hide the pad from every other application while we own it, so the
        // D-pad and L3 cannot drive menus or media at the same time as us.
        // Whitelisting ourselves first is what stops us hiding it from
        // ourselves; if that failed we never hide anything.
        if (g_hh != INVALID_HANDLE_VALUE)
            hh_hide(now_exclusive && g_pad_inst_count > 0);

        Sleep(8);  // ~120 Hz
    }

    // Never exit holding a button: an unmatched LEFTDOWN would leave the whole
    // desktop stuck in a drag after we are gone.
    edge_click_release_all(a_down, b_down);
    hh_hide(false);   // give the pad back before we go
    hid_close();
    drop_device();
    if (g_di) {
        g_di->Release();
        g_di = NULL;
    }
    return 0;
}

// --- On-screen keyboard window ----------------------------------------------
// A non-activating topmost popup: it never takes focus, so the keys it sends
// go to whichever application the user is actually working in.
struct KbKey { const wchar_t* label; WORD vk; int units; };

static const KbKey KB_ROW0[] = {{L"1",'1',1},{L"2",'2',1},{L"3",'3',1},{L"4",'4',1},{L"5",'5',1},
                                {L"6",'6',1},{L"7",'7',1},{L"8",'8',1},{L"9",'9',1},{L"0",'0',1}};
static const KbKey KB_ROW1[] = {{L"Q",'Q',1},{L"W",'W',1},{L"E",'E',1},{L"R",'R',1},{L"T",'T',1},
                                {L"Y",'Y',1},{L"U",'U',1},{L"I",'I',1},{L"O",'O',1},{L"P",'P',1}};
static const KbKey KB_ROW2[] = {{L"A",'A',1},{L"S",'S',1},{L"D",'D',1},{L"F",'F',1},{L"G",'G',1},
                                {L"H",'H',1},{L"J",'J',1},{L"K",'K',1},{L"L",'L',1}};
static const KbKey KB_ROW3[] = {{L"Z",'Z',1},{L"X",'X',1},{L"C",'C',1},{L"V",'V',1},{L"B",'B',1},
                                {L"N",'N',1},{L"M",'M',1},{L",",VK_OEM_COMMA,1},{L".",VK_OEM_PERIOD,1}};
static const KbKey KB_ROW4[] = {{L"Shift",VK_SHIFT,2},{L"Space",VK_SPACE,6},{L"Enter",VK_RETURN,2}};

static const KbKey* KB_ROWS[] = {KB_ROW0, KB_ROW1, KB_ROW2, KB_ROW3, KB_ROW4};
static const int    KB_COUNT[] = {10, 10, 9, 9, 3};
#define KB_NROWS 5

// Geometry, in DIPs: 48 unit keys, 6 gaps, 12 margin.
//
// No drop shadow around the card: this window is WS_EX_LAYERED with a single
// constant alpha (that is what the open/close fade animates), which gives no
// per-pixel alpha, and ID2D1HwndRenderTarget is opaque - so anything drawn
// outside the card would composite as solid black rather than as a soft
// shadow. Depth comes from DWM's rounded corners plus the in-card key glow
// instead. Real per-pixel shadows would need UpdateLayeredWindow with a WIC
// bitmap target, which is incompatible with the constant-alpha fade.
#define KB_KU 48
#define KB_GAP 6
#define KB_M 12
#define KB_W (10 * KB_KU + 9 * KB_GAP + 2 * KB_M)
#define KB_H (KB_NROWS * KB_KU + (KB_NROWS - 1) * KB_GAP + 2 * KB_M)

// Dark theme palette. Keys are not flat fills: each is a soft vertical
// gradient with a lit top edge and a faint sheen falling away from it, which
// is what gives a physical, raised look without any transparency.
// Windows 11 Fluent dark theme. Values follow the documented WinUI resources
// so the popups read as part of the OS rather than as a lookalike: flat fills,
// a single hairline border, small corner radii, and accent-filled selection
// with black text - which is what dark-theme Windows does, because the dark
// accent is a light blue.
#define KB_CLR_BG     RGB(32, 32, 32)    // SolidBackgroundFillColorBase
#define KB_CLR_KEY    RGB(59, 59, 59)    // ControlFillColorDefault over the base
#define KB_CLR_SEL    RGB(76, 194, 255)  // AccentFillColorDefault (dark theme)
#define KB_CLR_ARMED  RGB(90, 90, 90)    // toggled control, kept neutral so it
                                         // does not compete with selection
#define KB_CLR_TEXT   RGB(255, 255, 255) // TextFillColorPrimary
#define KB_CLR_TEXT2  RGB(200, 200, 200) // TextFillColorSecondary (~78.6%)
#define KB_CLR_ONACC  RGB(0, 0, 0)       // TextOnAccentFillColorPrimary
#define KB_BORDER_A   0.08f              // ControlStrokeColorDefault (~7%)
#define KB_RADIUS     6.0f               // control corner radius
#define KB_CARD_RADIUS 8.0f              // flyout / container corner radius

static HWND   g_kb = NULL;
static int    g_kb_row = 1, g_kb_col = 0;
static bool   g_kb_shift = false;
// The only GDI object left: the window-class background brush, which just
// prevents a white flash between window creation and the first D2D paint.
static HBRUSH g_kb_bg = NULL;

// Open/close + key-press animation state.
static int       g_kb_anim = 0;           // 0 idle, 1 opening, 2 closing
static ULONGLONG g_kb_anim_t0 = 0;
static ULONGLONG g_kb_pulse_t0 = 0;       // key-press flash start (0 = none)
static int       g_kb_x = 0, g_kb_y = 0;  // resting position
#define KB_TIMER    1
#define KB_ANIM_MS  160
#define KB_PULSE_MS 140
#define KB_SLIDE    26

static void init_theme() {
    g_kb_bg = CreateSolidBrush(KB_CLR_BG);
}

// --- Direct2D / DirectWrite --------------------------------------------------
// Both windows are fully D2D-drawn. All layout in this file is expressed in
// DIPs (device-independent pixels, 1 DIP = 1px at 96 DPI); each render target
// is told the real monitor DPI, so Direct2D scales every shape and glyph to
// physical pixels itself. That is what keeps the UI sharp on a 4K display
// instead of being bitmap-stretched by the compositor.
static UINT g_dpi = 96;

static inline float  dpi_scale()          { return g_dpi / 96.0f; }
static inline int    dip_to_px(int dip)   { return MulDiv(dip, (int)g_dpi, 96); }
static inline int    px_to_dip(int px)    { return MulDiv(px, 96, (int)g_dpi); }

static ID2D1Factory1*     g_d2d_factory = NULL;
static IDWriteFactory*    g_dwrite_factory = NULL;
// Sizes are DIPs, and a touch larger than the old GDI fonts: this is often
// driven from a couch, so the text needs to hold up at a distance.
static IDWriteTextFormat* g_tf_body = NULL;    // 13, values
static IDWriteTextFormat* g_tf_label = NULL;   // 13, dim labels
static IDWriteTextFormat* g_tf_header = NULL;  // 15 semibold, status line
static IDWriteTextFormat* g_tf_key = NULL;     // 18 semibold, keyboard keys

static ID2D1HwndRenderTarget* g_rt_main = NULL;
static ID2D1SolidColorBrush*  g_br_main_key = NULL;
static ID2D1SolidColorBrush*  g_br_main_sel = NULL;
static ID2D1SolidColorBrush*  g_br_main_armed = NULL;
static ID2D1SolidColorBrush*  g_br_main_toggle_off = NULL;
static ID2D1SolidColorBrush*  g_br_main_text = NULL;
static ID2D1SolidColorBrush*  g_br_main_dim = NULL;
static ID2D1SolidColorBrush*  g_br_main_white = NULL;
static ID2D1SolidColorBrush*  g_br_main_status = NULL;  // color set per-draw
static ID2D1SolidColorBrush*  g_br_main_glow = NULL;    // alpha set per-draw
static ID2D1SolidColorBrush*  g_br_main_onacc = NULL;   // knob/label on accent

static ID2D1HwndRenderTarget* g_rt_kb = NULL;
static ID2D1SolidColorBrush*  g_br_kb_key = NULL;
static ID2D1SolidColorBrush*  g_br_kb_sel = NULL;
static ID2D1SolidColorBrush*  g_br_kb_armed = NULL;
static ID2D1SolidColorBrush*  g_br_kb_text = NULL;
static ID2D1SolidColorBrush*  g_br_kb_flash = NULL;   // color set per-draw
static ID2D1SolidColorBrush*  g_br_kb_onacc = NULL;   // label on an accent fill
static ID2D1SolidColorBrush*  g_br_kb_border = NULL;  // hairline control stroke

static inline D2D1_COLOR_F d2d_clr(COLORREF c, float a = 1.0f) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                         GetBValue(c) / 255.0f, a);
}

static inline D2D1_RECT_F to_f(const RECT& r) {
    return D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom);
}

static void d2d_init_process() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                      __uuidof(ID2D1Factory1), (void**)&g_d2d_factory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        (IUnknown**)&g_dwrite_factory);
    if (!g_dwrite_factory) return;
    g_dwrite_factory->CreateTextFormat(L"Segoe UI", NULL,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &g_tf_body);
    g_dwrite_factory->CreateTextFormat(L"Segoe UI", NULL,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &g_tf_label);
    g_dwrite_factory->CreateTextFormat(L"Segoe UI", NULL,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"en-us", &g_tf_header);
    g_dwrite_factory->CreateTextFormat(L"Segoe UI", NULL,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-us", &g_tf_key);
    IDWriteTextFormat* left[] = {g_tf_body, g_tf_label, g_tf_header};
    for (int i = 0; i < 3; i++) {
        if (!left[i]) continue;
        left[i]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        left[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        left[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (g_tf_key) {
        g_tf_key->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_tf_key->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_tf_key->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
}

// grayscale = the correct AA mode for a layered (per-window alpha) popup;
// ClearType's subpixel weights are wrong once the window is composited
// translucently, which is what makes GDI text look fringed there today.
static ID2D1HwndRenderTarget* d2d_create_rt(HWND hwnd, bool grayscale_text) {
    if (!g_d2d_factory) return NULL;
    RECT rc;
    GetClientRect(hwnd, &rc);
    ID2D1HwndRenderTarget* rt = NULL;
    if (FAILED(g_d2d_factory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd,
                D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
            &rt)))
        return NULL;
    rt->SetDpi((float)g_dpi, (float)g_dpi);   // draw in DIPs from here on
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->SetTextAntialiasMode(grayscale_text ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE
                                            : D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    return rt;
}

// Soft glow without ID2D1Effect (which would need a full ID2D1Device): stack
// a few progressively larger, progressively fainter rounded rects behind the
// shape. Cheap, and reads as a real blur at these sizes. Only valid over an
// opaque background - see the note on the keyboard geometry above.
// One rounded control, Fluent style: a flat fill plus a hairline stroke drawn
// inside the bounds, so adjacent controls keep an even 1px line between them.
static void draw_control(ID2D1RenderTarget* rt, D2D1_RECT_F r, float radius,
                         ID2D1Brush* fill, ID2D1Brush* border) {
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), fill);
    if (border)
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f,
                                          r.right - 0.5f, r.bottom - 0.5f),
                              radius, radius),
            border, 1.0f);
}

static void d2d_release_main() {
    ID2D1SolidColorBrush** bs[] = {&g_br_main_key, &g_br_main_sel, &g_br_main_armed,
                                   &g_br_main_toggle_off, &g_br_main_text,
                                   &g_br_main_dim, &g_br_main_white,
                                   &g_br_main_status, &g_br_main_glow,
                                   &g_br_main_onacc};
    for (int i = 0; i < 10; i++)
        if (*bs[i]) { (*bs[i])->Release(); *bs[i] = NULL; }
    if (g_rt_main) { g_rt_main->Release(); g_rt_main = NULL; }
}

static bool d2d_create_main(HWND hwnd) {
    g_rt_main = d2d_create_rt(hwnd, false);
    if (!g_rt_main) return false;
    g_rt_main->CreateSolidColorBrush(d2d_clr(KB_CLR_KEY), &g_br_main_key);
    g_rt_main->CreateSolidColorBrush(d2d_clr(KB_CLR_SEL), &g_br_main_sel);
    g_rt_main->CreateSolidColorBrush(d2d_clr(KB_CLR_ARMED), &g_br_main_armed);
    g_rt_main->CreateSolidColorBrush(d2d_clr(RGB(62, 62, 72)), &g_br_main_toggle_off);
    g_rt_main->CreateSolidColorBrush(d2d_clr(RGB(235, 235, 240)), &g_br_main_text);
    g_rt_main->CreateSolidColorBrush(d2d_clr(RGB(158, 158, 170)), &g_br_main_dim);
    g_rt_main->CreateSolidColorBrush(d2d_clr(RGB(255, 255, 255)), &g_br_main_white);
    g_rt_main->CreateSolidColorBrush(d2d_clr(RGB(240, 110, 110)), &g_br_main_status);
    g_rt_main->CreateSolidColorBrush(d2d_clr(KB_CLR_SEL), &g_br_main_glow);
    g_rt_main->CreateSolidColorBrush(d2d_clr(KB_CLR_ONACC), &g_br_main_onacc);
    return true;
}

static void d2d_release_kb() {
    ID2D1SolidColorBrush** bs[] = {&g_br_kb_key, &g_br_kb_sel, &g_br_kb_armed,
                                   &g_br_kb_text, &g_br_kb_flash,
                                   &g_br_kb_onacc, &g_br_kb_border};
    for (int i = 0; i < 7; i++)
        if (*bs[i]) { (*bs[i])->Release(); *bs[i] = NULL; }
    if (g_rt_kb) { g_rt_kb->Release(); g_rt_kb = NULL; }
}

static bool d2d_create_kb(HWND hwnd) {
    g_rt_kb = d2d_create_rt(hwnd, true);
    if (!g_rt_kb) return false;
    g_rt_kb->CreateSolidColorBrush(d2d_clr(KB_CLR_KEY), &g_br_kb_key);
    g_rt_kb->CreateSolidColorBrush(d2d_clr(KB_CLR_SEL), &g_br_kb_sel);
    g_rt_kb->CreateSolidColorBrush(d2d_clr(KB_CLR_ARMED), &g_br_kb_armed);
    g_rt_kb->CreateSolidColorBrush(d2d_clr(KB_CLR_TEXT), &g_br_kb_text);
    g_rt_kb->CreateSolidColorBrush(d2d_clr(KB_CLR_ONACC), &g_br_kb_onacc);
    g_rt_kb->CreateSolidColorBrush(d2d_clr(KB_CLR_KEY), &g_br_kb_flash);
    g_rt_kb->CreateSolidColorBrush(
        D2D1::ColorF(1, 1, 1, KB_BORDER_A), &g_br_kb_border);
    return true;
}

static COLORREF lerp_clr(COLORREF a, COLORREF b, double t) {
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

static int kb_key_width(const KbKey& k) {
    return k.units * KB_KU + (k.units - 1) * KB_GAP;
}

static RECT kb_key_rect(int row, int idx) {
    int roww = (KB_COUNT[row] - 1) * KB_GAP;
    for (int i = 0; i < KB_COUNT[row]; i++) roww += kb_key_width(KB_ROWS[row][i]);
    int x = (KB_W - roww) / 2;
    for (int i = 0; i < idx; i++) x += kb_key_width(KB_ROWS[row][i]) + KB_GAP;
    int y = KB_M + row * (KB_KU + KB_GAP);
    RECT r = {x, y, x + kb_key_width(KB_ROWS[row][idx]), y + KB_KU};
    return r;
}

static void kb_send_vk(WORD vk, bool shift) {
    INPUT in[4] = {};
    int n = 0;
    if (shift) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_SHIFT; n++; }
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; n++;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    if (shift) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_SHIFT;
                 in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    SendInput(n, in, sizeof(INPUT));
}

static void kb_select() {
    const KbKey& k = KB_ROWS[g_kb_row][g_kb_col];
    if (k.vk == VK_SHIFT) {
        g_kb_shift = !g_kb_shift;   // one-shot: applies to the next key
    } else {
        kb_send_vk(k.vk, g_kb_shift);
        g_kb_shift = false;
        g_kb_pulse_t0 = GetTickCount64();          // flash the pressed key
        if (g_kb) SetTimer(g_kb, KB_TIMER, 15, NULL);
    }
    if (g_kb) InvalidateRect(g_kb, NULL, FALSE);
}

static void kb_nav(int dir) {
    if (dir == 0) g_kb_row = (g_kb_row + KB_NROWS - 1) % KB_NROWS;        // up
    else if (dir == 2) g_kb_row = (g_kb_row + 1) % KB_NROWS;              // down
    else if (dir == 1) g_kb_col = (g_kb_col + 1) % KB_COUNT[g_kb_row];    // right
    else if (dir == 3) g_kb_col = (g_kb_col + KB_COUNT[g_kb_row] - 1) % KB_COUNT[g_kb_row];  // left
    if (g_kb_col >= KB_COUNT[g_kb_row]) g_kb_col = KB_COUNT[g_kb_row] - 1;
    if (g_kb) InvalidateRect(g_kb, NULL, FALSE);
}

static LRESULT CALLBACK kb_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: {
        // Drives the open/close slide+fade and the key-press flash decay.
        ULONGLONG now = GetTickCount64();
        bool active = false;
        if (g_kb_anim) {
            double t = (double)(now - g_kb_anim_t0) / KB_ANIM_MS;
            if (t > 1.0) t = 1.0;
            double e = 1.0 - pow(1.0 - t, 3);                  // ease-out cubic
            double a = (g_kb_anim == 1) ? e : 1.0 - e;         // opening / closing
            SetLayeredWindowAttributes(hwnd, 0, (BYTE)(255 * a), LWA_ALPHA);
            SetWindowPos(hwnd, NULL, g_kb_x,
                         g_kb_y + (int)(dip_to_px(KB_SLIDE) * (1.0 - a)), 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
            if (t >= 1.0) {
                if (g_kb_anim == 2) ShowWindow(hwnd, SW_HIDE);
                g_kb_anim = 0;
            } else {
                active = true;
            }
        }
        if (g_kb_pulse_t0) {
            if (now - g_kb_pulse_t0 < KB_PULSE_MS) active = true;
            else g_kb_pulse_t0 = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        // Nothing animates while the keyboard merely sits open, so the timer
        // stops once the open/close slide and the key flash are done.
        if (!active) KillTimer(hwnd, KB_TIMER);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_rt_kb) g_rt_kb->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (!g_rt_kb) d2d_create_kb(hwnd);
        if (g_rt_kb) {
            // No manual double-buffering needed: ID2D1HwndRenderTarget is
            // already back-buffered.
            g_rt_kb->BeginDraw();
            g_rt_kb->Clear(d2d_clr(KB_CLR_BG));

            // Flyout surface: flat base colour, a hairline border and the
            // 8px radius Windows uses for menus and flyouts.
            {
                D2D1_SIZE_F sz = g_rt_kb->GetSize();
                D2D1_RECT_F cr = D2D1::RectF(0.5f, 0.5f, sz.width - 0.5f,
                                             sz.height - 0.5f);
                g_rt_kb->DrawRoundedRectangle(
                    D2D1::RoundedRect(cr, KB_CARD_RADIUS, KB_CARD_RADIUS),
                    g_br_kb_border, 1.0f);
            }

            for (int r = 0; r < KB_NROWS; r++) {
                for (int i = 0; i < KB_COUNT[r]; i++) {
                    RECT kr = kb_key_rect(r, i);
                    D2D1_RECT_F kf = D2D1::RectF((float)kr.left, (float)kr.top,
                                                 (float)kr.right, (float)kr.bottom);
                    bool sel = (r == g_kb_row && i == g_kb_col);
                    bool armed = (KB_ROWS[r][i].vk == VK_SHIFT && g_kb_shift);

                    // Selection is an accent-filled control with black text,
                    // which is how dark-theme Windows shows a default or
                    // selected button - the dark accent is a light blue, so
                    // white text on it would fail contrast.
                    ID2D1Brush* fill = g_br_kb_key;
                    ID2D1Brush* border = g_br_kb_border;
                    ID2D1Brush* tb = g_br_kb_text;
                    if (sel) {
                        fill = g_br_kb_sel;
                        border = NULL;
                        tb = g_br_kb_onacc;
                        // Press feedback: briefly wash the fill toward white,
                        // matching the momentary lightening Windows uses.
                        if (g_kb_pulse_t0) {
                            double f = 1.0 - (double)(GetTickCount64() - g_kb_pulse_t0)
                                               / KB_PULSE_MS;
                            if (f > 0.0) {
                                g_br_kb_flash->SetColor(d2d_clr(lerp_clr(
                                    KB_CLR_SEL, RGB(255, 255, 255), f * 0.65)));
                                fill = g_br_kb_flash;
                            }
                        }
                    } else if (armed) {
                        fill = g_br_kb_armed;
                    }
                    draw_control(g_rt_kb, kf, KB_RADIUS, fill, border);

                    // Letters follow the Shift state, so the keyboard shows
                    // what will actually be typed.
                    const wchar_t* lab = KB_ROWS[r][i].label;
                    wchar_t lower[2];
                    if (!g_kb_shift && lab[0] >= L'A' && lab[0] <= L'Z' && !lab[1]) {
                        lower[0] = (wchar_t)towlower(lab[0]);
                        lower[1] = 0;
                        lab = lower;
                    }
                    if (g_tf_key)
                        g_rt_kb->DrawText(lab, (UINT32)wcslen(lab), g_tf_key, kf, tb);
                }
            }

            HRESULT hr = g_rt_kb->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET) d2d_release_kb();
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        d2d_release_kb();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void kb_ensure() {
    if (g_kb) return;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = kb_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"ControllerMouseKB";
    RegisterClassW(&wc);

    DWORD style = WS_POPUP;   // borderless; the dark surface is the chrome
    DWORD ex = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    // Window size is physical pixels; the layout above is DIPs.
    RECT r = {0, 0, dip_to_px(KB_W), dip_to_px(KB_H)};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int ww = r.right - r.left, wh = r.bottom - r.top;

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_kb_x = wa.left + (wa.right - wa.left - ww) / 2;   // bottom-centre of screen
    g_kb_y = wa.bottom - wh - dip_to_px(12);

    g_kb = CreateWindowExW(ex, L"ControllerMouseKB", L"", style,
                           g_kb_x, g_kb_y, ww, wh, g_hwnd, NULL,
                           GetModuleHandleW(NULL), NULL);

    // Rounded window corners on Windows 11 (best-effort; harmless elsewhere).
    if (g_kb) {
        SetLayeredWindowAttributes(g_kb, 0, 255, LWA_ALPHA);
        DWORD pref = 2;  // DWMWCP_ROUND
        DwmSetWindowAttribute(g_kb, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                              &pref, sizeof(pref));
    }
}

static void kb_toggle() {
    kb_ensure();
    if (!g_kb) return;
    ULONGLONG now = GetTickCount64();
    if (g_kb_visible) {
        g_kb_visible = false;   // buttons revert to the mouse immediately
        g_kb_anim = 2;          // fade + slide down, hidden when done
        g_kb_anim_t0 = now;
        SetTimer(g_kb, KB_TIMER, 15, NULL);
    } else {
        g_kb_shift = false;
        SetLayeredWindowAttributes(g_kb, 0, 0, LWA_ALPHA);
        SetWindowPos(g_kb, HWND_TOPMOST, g_kb_x, g_kb_y + dip_to_px(KB_SLIDE), 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_kb_visible = true;
        g_kb_anim = 1;          // fade in + slide up to rest
        g_kb_anim_t0 = now;
        SetTimer(g_kb, KB_TIMER, 15, NULL);
        InvalidateRect(g_kb, NULL, FALSE);
    }
}

// --- App launcher -----------------------------------------------------------
// A second popup built on the same pattern as the keyboard: non-activating,
// topmost, D2D-drawn, driven entirely from the pad. Held Options opens it,
// the D-pad moves between tiles and Cross launches. The last tile is always a
// "+" placeholder that adds another app, so the grid grows with the list.
#define LX_TW   132      // tile size and spacing, in DIPs
#define LX_TH   100
#define LX_GAP  14
#define LX_M    22
#define LX_HDR  38       // room for the title above the grid
#define LX_COLS 4
#define LX_MAX_APPS 24
#define LX_TIMER 2

static HWND        g_lx = NULL;
static int         g_lx_sel = 0;
static int         g_lx_anim = 0;          // 0 idle, 1 opening, 2 closing
static ULONGLONG   g_lx_anim_t0 = 0;
static int         g_lx_x = 0, g_lx_y = 0;
static std::wstring g_lx_apps[LX_MAX_APPS];
static int         g_lx_count = 0;
// Close prompt: D-pad up on a running app slides its icon out of the tile and
// a confirmation in from below. Only the selected tile can be in this state.
static bool        g_lx_close_mode = false;
static int         g_lx_close_anim = 0;    // 1 sliding in, 2 sliding out
static ULONGLONG   g_lx_close_t0 = 0;

static ID2D1HwndRenderTarget* g_rt_lx = NULL;
static ID2D1SolidColorBrush*  g_br_lx_text = NULL;
static ID2D1SolidColorBrush*  g_br_lx_dim = NULL;
static ID2D1SolidColorBrush*  g_br_lx_sel = NULL;
static ID2D1SolidColorBrush*  g_br_lx_onacc = NULL;
static ID2D1SolidColorBrush*  g_br_lx_face = NULL;
static ID2D1SolidColorBrush*  g_br_lx_border = NULL;
static ID2D1SolidColorBrush*  g_br_lx_warn = NULL;   // colour set per-draw
// Icons are device-dependent, so they live and die with the render target.
static ID2D1Bitmap*           g_lx_icon[LX_MAX_APPS] = {};
static IWICImagingFactory*    g_wic = NULL;

// Shell icon -> D2D bitmap. The jumbo list gives a 256px icon where one
// exists, which matters on a high-DPI display; SHGetFileInfo's 32px icon is
// the fallback.
static HICON shell_icon(const wchar_t* path) {
    SHFILEINFOW fi = {};
    if (SHGetFileInfoW(path, 0, &fi, sizeof(fi), SHGFI_SYSICONINDEX)) {
        IImageList* il = NULL;
        if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_IImageList, (void**)&il)) && il) {
            HICON h = NULL;
            il->GetIcon(fi.iIcon, ILD_TRANSPARENT, &h);
            il->Release();
            if (h) return h;
        }
    }
    SHFILEINFOW fi2 = {};
    if (SHGetFileInfoW(path, 0, &fi2, sizeof(fi2), SHGFI_ICON | SHGFI_LARGEICON))
        return fi2.hIcon;
    return NULL;
}

static ID2D1Bitmap* load_icon_bitmap(ID2D1RenderTarget* rt, const wchar_t* path) {
    if (!rt || !g_wic) return NULL;
    HICON ico = shell_icon(path);
    if (!ico) return NULL;
    IWICBitmap* wb = NULL;
    ID2D1Bitmap* out = NULL;
    if (SUCCEEDED(g_wic->CreateBitmapFromHICON(ico, &wb)) && wb) {
        IWICFormatConverter* fc = NULL;
        if (SUCCEEDED(g_wic->CreateFormatConverter(&fc)) && fc) {
            if (SUCCEEDED(fc->Initialize(wb, GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, NULL, 0.0,
                                         WICBitmapPaletteTypeMedianCut)))
                rt->CreateBitmapFromWicBitmap(fc, NULL, &out);
            fc->Release();
        }
        wb->Release();
    }
    DestroyIcon(ico);
    return out;
}

static void lx_release_icons() {
    for (int i = 0; i < LX_MAX_APPS; i++)
        if (g_lx_icon[i]) { g_lx_icon[i]->Release(); g_lx_icon[i] = NULL; }
}

// One path per line, next to config.json - trivial to hand-edit, and avoids
// teaching the minimal JSON writer about arrays.
static std::wstring apps_path() {
    std::wstring p = config_path();
    p.resize(p.find_last_of(L"\\/") + 1);
    return p + L"apps.txt";
}

static void lx_load() {
    lx_release_icons();
    g_lx_count = 0;
    FILE* f = _wfopen(apps_path().c_str(), L"rb, ccs=UTF-8");
    if (!f) return;
    wchar_t line[MAX_PATH];
    while (g_lx_count < LX_MAX_APPS && fgetws(line, MAX_PATH, f)) {
        size_t n = wcslen(line);
        while (n && (line[n - 1] == L'\n' || line[n - 1] == L'\r')) line[--n] = 0;
        if (n) g_lx_apps[g_lx_count++] = line;
    }
    fclose(f);
}

static void lx_save() {
    FILE* f = _wfopen(apps_path().c_str(), L"wb, ccs=UTF-8");
    if (!f) return;
    for (int i = 0; i < g_lx_count; i++) fwprintf(f, L"%s\n", g_lx_apps[i].c_str());
    fclose(f);
}

static int lx_tiles() { return g_lx_count + 1; }   // apps plus the "+" tile
static int lx_rows()  { return (lx_tiles() + LX_COLS - 1) / LX_COLS; }
static int lx_width() { return LX_COLS * LX_TW + (LX_COLS - 1) * LX_GAP + 2 * LX_M; }
static int lx_height() {
    int r = lx_rows();
    return LX_HDR + r * LX_TH + (r - 1) * LX_GAP + 2 * LX_M;
}

static RECT lx_tile_rect(int i) {
    int col = i % LX_COLS, row = i / LX_COLS;
    int x = LX_M + col * (LX_TW + LX_GAP);
    int y = LX_M + LX_HDR + row * (LX_TH + LX_GAP);
    RECT r = {x, y, x + LX_TW, y + LX_TH};
    return r;
}

// Display name for a tile: the file name without extension is what people
// recognise, and the full path rarely fits.
static std::wstring lx_label(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    std::wstring n = (s == std::wstring::npos) ? path : path.substr(s + 1);
    size_t d = n.find_last_of(L'.');
    if (d != std::wstring::npos && d > 0) n = n.substr(0, d);
    return n;
}

// A .lnk points at the real executable, and that is what a running process
// reports, so shortcuts have to be resolved before matching.
static std::wstring resolve_target(const std::wstring& path) {
    size_t d = path.find_last_of(L'.');
    if (d == std::wstring::npos || _wcsicmp(path.c_str() + d, L".lnk") != 0)
        return path;
    std::wstring out = path;
    IShellLinkW* sl = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, (void**)&sl)) && sl) {
        IPersistFile* pf = NULL;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
            if (SUCCEEDED(pf->Load(path.c_str(), STGM_READ))) {
                wchar_t buf[MAX_PATH] = L"";
                if (SUCCEEDED(sl->GetPath(buf, MAX_PATH, NULL, 0)) && buf[0])
                    out = buf;
            }
            pf->Release();
        }
        sl->Release();
    }
    return out;
}

struct FindAppCtx { const wchar_t* exe; const wchar_t* base; HWND found; };

static const wchar_t* path_base(const wchar_t* p) {
    const wchar_t* s = wcsrchr(p, L'\\');
    return s ? s + 1 : p;
}

static BOOL CALLBACK find_app_cb(HWND h, LPARAM lp) {
    FindAppCtx* c = (FindAppCtx*)lp;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    if (!GetWindowTextLengthW(h)) return TRUE;   // skip invisible helper windows
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return TRUE;
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) return TRUE;
    wchar_t img[MAX_PATH] = L"";
    DWORD n = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(ph, 0, img, &n) != 0;
    CloseHandle(ph);
    if (!ok) return TRUE;
    // Full path first; fall back to the file name, since a launcher stub may
    // live somewhere other than the shortcut points to.
    if (_wcsicmp(img, c->exe) == 0 || _wcsicmp(path_base(img), c->base) == 0) {
        c->found = h;
        return FALSE;
    }
    return TRUE;
}

static HWND find_app_window(const std::wstring& path) {
    std::wstring exe = resolve_target(path);
    FindAppCtx c = {exe.c_str(), path_base(exe.c_str()), NULL};
    EnumWindows(find_app_cb, (LPARAM)&c);
    return c.found;
}

// Ask politely, then insist. Done on its own thread so the wait does not
// freeze the UI - an app showing a "save changes?" prompt would otherwise
// block us for the full timeout.
static DWORD WINAPI close_proc(LPVOID param) {
    DWORD pid = (DWORD)(ULONG_PTR)param;
    HANDLE ph = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    if (!ph) return 0;
    if (WaitForSingleObject(ph, 3000) == WAIT_TIMEOUT) TerminateProcess(ph, 0);
    CloseHandle(ph);
    return 0;
}

static void force_close_app(const std::wstring& path) {
    HWND h = find_app_window(path);
    if (!h) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    PostMessageW(h, WM_CLOSE, 0, 0);
    if (pid)
        CloseHandle(CreateThread(NULL, 0, close_proc,
                                 (LPVOID)(ULONG_PTR)pid, 0, NULL));
}

// True if an existing window was brought forward.
static bool activate_running(const std::wstring& path) {
    FindAppCtx c = {NULL, NULL, find_app_window(path)};
    if (!c.found) return false;
    if (IsIconic(c.found)) ShowWindow(c.found, SW_RESTORE);
    if (!SetForegroundWindow(c.found)) {
        // Windows refuses focus changes from a process that is not already in
        // the foreground. SwitchToThisWindow is what the shell itself uses for
        // alt-tab style switching; resolved dynamically as it is not in every
        // SDK header.
        typedef void(WINAPI * SwitchFn)(HWND, BOOL);
        HMODULE u = GetModuleHandleW(L"user32.dll");
        SwitchFn f = u ? (SwitchFn)GetProcAddress(u, "SwitchToThisWindow") : NULL;
        if (f) f(c.found, TRUE);
    }
    return true;
}

static void d2d_release_lx() {
    ID2D1SolidColorBrush** bs[] = {&g_br_lx_text, &g_br_lx_dim, &g_br_lx_sel,
                                   &g_br_lx_face, &g_br_lx_border,
                                   &g_br_lx_onacc, &g_br_lx_warn};
    for (int i = 0; i < 7; i++)
        if (*bs[i]) { (*bs[i])->Release(); *bs[i] = NULL; }
    lx_release_icons();
    if (g_rt_lx) { g_rt_lx->Release(); g_rt_lx = NULL; }
}

static bool d2d_create_lx(HWND hwnd) {
    g_rt_lx = d2d_create_rt(hwnd, true);
    if (!g_rt_lx) return false;
    g_rt_lx->CreateSolidColorBrush(d2d_clr(KB_CLR_TEXT), &g_br_lx_text);
    g_rt_lx->CreateSolidColorBrush(d2d_clr(KB_CLR_TEXT2), &g_br_lx_dim);
    g_rt_lx->CreateSolidColorBrush(d2d_clr(KB_CLR_SEL), &g_br_lx_sel);
    g_rt_lx->CreateSolidColorBrush(d2d_clr(KB_CLR_ONACC), &g_br_lx_onacc);
    // CardBackgroundFillColorDefault sits a little above the flyout base.
    g_rt_lx->CreateSolidColorBrush(d2d_clr(RGB(45, 45, 45)), &g_br_lx_face);
    g_rt_lx->CreateSolidColorBrush(
        D2D1::ColorF(1, 1, 1, KB_BORDER_A), &g_br_lx_border);
    g_rt_lx->CreateSolidColorBrush(d2d_clr(KB_CLR_SEL), &g_br_lx_warn);
    return true;
}

static LRESULT CALLBACK lx_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: {
        ULONGLONG now = GetTickCount64();
        bool active = false;
        if (g_lx_anim) {
            double t = (double)(now - g_lx_anim_t0) / KB_ANIM_MS;
            if (t > 1.0) t = 1.0;
            double e = 1.0 - pow(1.0 - t, 3);
            double a = (g_lx_anim == 1) ? e : 1.0 - e;
            SetLayeredWindowAttributes(hwnd, 0, (BYTE)(255 * a), LWA_ALPHA);
            SetWindowPos(hwnd, NULL, g_lx_x,
                         g_lx_y + (int)(dip_to_px(KB_SLIDE) * (1.0 - a)), 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
            if (t >= 1.0) {
                if (g_lx_anim == 2) ShowWindow(hwnd, SW_HIDE);
                g_lx_anim = 0;
            } else {
                active = true;
            }
        }
        if (g_lx_close_anim) {
            if (now - g_lx_close_t0 >= KB_ANIM_MS) g_lx_close_anim = 0;
            else active = true;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (!active) KillTimer(hwnd, LX_TIMER);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_rt_lx) g_rt_lx->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (!g_rt_lx) d2d_create_lx(hwnd);
        if (g_rt_lx) {
            g_rt_lx->BeginDraw();
            g_rt_lx->Clear(d2d_clr(KB_CLR_BG));
            D2D1_SIZE_F sz = g_rt_lx->GetSize();
            g_rt_lx->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, sz.width - 0.5f,
                                              sz.height - 0.5f),
                                  KB_CARD_RADIUS, KB_CARD_RADIUS),
                g_br_lx_border, 1.0f);
            if (g_tf_header) {
                D2D1_RECT_F hr = D2D1::RectF((float)LX_M, 14.0f,
                                             sz.width - LX_M, 14.0f + 24.0f);
                g_rt_lx->DrawText(L"Apps", 4, g_tf_header, hr, g_br_lx_dim);
            }

            for (int i = 0; i < lx_tiles(); i++) {
                RECT tr = lx_tile_rect(i);
                D2D1_RECT_F tf = to_f(tr);
                bool sel = (i == g_lx_sel);

                // How far this tile is through the close prompt: 0 shows the
                // app, 1 shows the confirmation.
                float cp = 0.0f;
                if (sel) {
                    if (g_lx_close_anim) {
                        double t = (double)(GetTickCount64() - g_lx_close_t0)
                                   / KB_ANIM_MS;
                        if (t > 1.0) t = 1.0;
                        double e = 1.0 - pow(1.0 - t, 3);
                        cp = (g_lx_close_anim == 1) ? (float)e : (float)(1.0 - e);
                    } else if (g_lx_close_mode) {
                        cp = 1.0f;
                    }
                }

                // The tile washes from accent to the close red as the prompt
                // arrives, so the destructive state is obvious before reading
                // any text. Black label on accent, white on red.
                ID2D1Brush* fill = g_br_lx_face;
                ID2D1Brush* tb = g_br_lx_text;
                if (sel) {
                    if (cp > 0.0f) {
                        g_br_lx_warn->SetColor(d2d_clr(
                            lerp_clr(KB_CLR_SEL, RGB(196, 43, 28), cp)));
                        fill = g_br_lx_warn;
                        tb = (cp >= 0.5f) ? (ID2D1Brush*)g_br_lx_text
                                          : (ID2D1Brush*)g_br_lx_onacc;
                    } else {
                        fill = g_br_lx_sel;
                        tb = g_br_lx_onacc;
                    }
                }
                draw_control(g_rt_lx, tf, KB_CARD_RADIUS, fill,
                             sel ? NULL : (ID2D1Brush*)g_br_lx_border);

                float th = tf.bottom - tf.top;
                if (cp > 0.0f)
                    g_rt_lx->PushAxisAlignedClip(tf, D2D1_ANTIALIAS_MODE_ALIASED);

                if (i == g_lx_count) {
                    if (g_tf_key)
                        g_rt_lx->DrawText(L"+", 1, g_tf_key, tf, tb);
                } else if (g_tf_body) {
                    g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    g_tf_body->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

                    float dy = -cp * th;   // app content slides up and out
                    if (!g_lx_icon[i])
                        g_lx_icon[i] = load_icon_bitmap(g_rt_lx, g_lx_apps[i].c_str());
                    if (g_lx_icon[i]) {
                        float cx = (tf.left + tf.right) / 2;
                        D2D1_RECT_F ir = D2D1::RectF(cx - 22, tf.top + 12 + dy,
                                                     cx + 22, tf.top + 56 + dy);
                        g_rt_lx->DrawBitmap(g_lx_icon[i], ir, 1.0f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    }
                    std::wstring nm = lx_label(g_lx_apps[i]);
                    D2D1_RECT_F lr = g_lx_icon[i]
                        ? D2D1::RectF(tf.left + 8, tf.top + 60 + dy, tf.right - 8,
                                      tf.bottom - 6 + dy)
                        : D2D1::RectF(tf.left + 10, tf.top + 10 + dy, tf.right - 10,
                                      tf.bottom - 10 + dy);
                    g_rt_lx->DrawText(nm.c_str(), (UINT32)nm.size(),
                                      g_tf_body, lr, tb);

                    // Confirmation rises from the bottom edge as the app leaves.
                    if (cp > 0.0f) {
                        float uy = (1.0f - cp) * th;
                        if (g_tf_key) {
                            D2D1_RECT_F xr = D2D1::RectF(tf.left, tf.top + 10 + uy,
                                                         tf.right, tf.top + 58 + uy);
                            // U+2715 as an escape: a literal here would depend
                            // on the compiler's source codepage.
                            g_rt_lx->DrawText(L"\x2715", 1, g_tf_key, xr, tb);
                        }
                        D2D1_RECT_F qr = D2D1::RectF(tf.left + 8, tf.top + 60 + uy,
                                                     tf.right - 8, tf.bottom - 6 + uy);
                        g_rt_lx->DrawText(L"Close?", 6, g_tf_body, qr, tb);
                    }
                    g_tf_body->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                }
                if (cp > 0.0f) g_rt_lx->PopAxisAlignedClip();
            }
            HRESULT hr = g_rt_lx->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET) d2d_release_lx();
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        d2d_release_lx();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void lx_ensure() {
    if (g_lx) return;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = lx_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"ControllerMouseLauncher";
    RegisterClassW(&wc);
    DWORD style = WS_POPUP;
    DWORD ex = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    g_lx = CreateWindowExW(ex, L"ControllerMouseLauncher", L"", style,
                           0, 0, dip_to_px(lx_width()), dip_to_px(lx_height()),
                           g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (g_lx) {
        SetLayeredWindowAttributes(g_lx, 0, 255, LWA_ALPHA);
        DWORD pref = 2;  // DWMWCP_ROUND
        DwmSetWindowAttribute(g_lx, 33, &pref, sizeof(pref));
    }
}

// Re-centre and resize: the grid grows a row at a time as apps are added.
static void lx_relayout() {
    if (!g_lx) return;
    int ww = dip_to_px(lx_width()), wh = dip_to_px(lx_height());
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_lx_x = wa.left + (wa.right - wa.left - ww) / 2;
    g_lx_y = wa.top + (wa.bottom - wa.top - wh) / 2;
    SetWindowPos(g_lx, NULL, g_lx_x, g_lx_y, ww, wh,
                 SWP_NOACTIVATE | SWP_NOZORDER);
}

static void lx_toggle() {
    lx_ensure();
    if (!g_lx) return;
    ULONGLONG now = GetTickCount64();
    if (g_lx_visible) {
        g_lx_visible = false;
        g_lx_anim = 2;
        g_lx_anim_t0 = now;
        SetTimer(g_lx, LX_TIMER, 15, NULL);
    } else {
        lx_load();
        g_lx_close_mode = false;
        g_lx_close_anim = 0;
        if (g_lx_sel >= lx_tiles()) g_lx_sel = lx_tiles() - 1;
        lx_relayout();
        SetLayeredWindowAttributes(g_lx, 0, 0, LWA_ALPHA);
        SetWindowPos(g_lx, HWND_TOPMOST, g_lx_x, g_lx_y + dip_to_px(KB_SLIDE),
                     0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_lx_visible = true;
        g_lx_anim = 1;
        g_lx_anim_t0 = now;
        SetTimer(g_lx, LX_TIMER, 15, NULL);
        InvalidateRect(g_lx, NULL, FALSE);
    }
}

static void lx_set_close_mode(bool on) {
    if (g_lx_close_mode == on) return;
    g_lx_close_mode = on;
    g_lx_close_anim = on ? 1 : 2;
    g_lx_close_t0 = GetTickCount64();
    if (g_lx) {
        SetTimer(g_lx, LX_TIMER, 15, NULL);
        InvalidateRect(g_lx, NULL, FALSE);
    }
}

static void lx_nav(int dir) {
    int n = lx_tiles();
    if (n <= 0) return;
    if (g_lx_close_mode) {
        // Anything except another "up" backs out of the prompt.
        if (dir != 0) lx_set_close_mode(false);
        return;
    }
    // Up on a running app asks whether to close it, instead of moving a row.
    if (dir == 0 && g_lx_sel < g_lx_count &&
        find_app_window(g_lx_apps[g_lx_sel])) {
        lx_set_close_mode(true);
        return;
    }
    if (dir == 1)      g_lx_sel = (g_lx_sel + 1) % n;
    else if (dir == 3) g_lx_sel = (g_lx_sel + n - 1) % n;
    else if (dir == 2) g_lx_sel = (g_lx_sel + LX_COLS) % n;
    else if (dir == 0) g_lx_sel = (g_lx_sel - LX_COLS + n * 2) % n;
    if (g_lx) InvalidateRect(g_lx, NULL, FALSE);
}

// --- System tray -----------------------------------------------------------
#define WM_TRAYICON   (WM_APP + 1)
#define ID_TRAY_SHOW  2001
#define ID_TRAY_QUIT  2002

static void add_tray_icon(HWND hwnd) {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1),
                                    IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"ControllerMouse");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void remove_tray_icon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void hide_to_tray(HWND hwnd) {
    ShowWindow(hwnd, SW_HIDE);
    add_tray_icon(hwnd);
}

static void restore_from_tray(HWND hwnd) {
    remove_tray_icon();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

// --- Window ----------------------------------------------------------------
#define ID_TIMER     1

// Trackbar indices (mouse sensitivity / scroll sensitivity / deadzone).
enum { TRK_MOUSE = 0, TRK_SCROLL = 1, TRK_DEADZONE = 2, TRK_CURVE = 3 };
#define NTRACKS 4
static const int kTrackLo[NTRACKS] = {1, 1, 0, 10};
static const int kTrackHi[NTRACKS] = {60, 50, 50, 30};

// Client size, and the whole layout below, in DIPs. Nothing here is a native
// child control any more - every label, slider, toggle and button is drawn by
// Direct2D in WM_PAINT and hit-tested by hand, so all of it scales cleanly to
// whatever DPI the monitor reports.
#define WIN_W 384
#define WIN_H 756

static const RECT kStatusRect = {20, 14, 20 + 344, 14 + 24};
static const RECT kHideRect   = {20, 44, 20 + 240, 44 + 20};
static const RECT kHidBtnRect = {284, 42, 284 + 80, 42 + 24};

static const RECT kLabelRect[NTRACKS] = {
    {20, 82, 20 + 200, 82 + 18},
    {20, 144, 20 + 200, 144 + 18},
    {20, 206, 20 + 200, 206 + 18},
    {20, 268, 20 + 200, 268 + 18},
};
static const RECT kValRect[NTRACKS] = {
    {284, 82, 284 + 80, 82 + 18},
    {284, 144, 284 + 80, 144 + 18},
    {284, 206, 284 + 80, 206 + 18},
    {284, 268, 284 + 80, 268 + 18},
};
static const RECT kTrackRect[NTRACKS] = {
    {20, 104, 20 + 344, 104 + 28},
    {20, 166, 20 + 344, 166 + 28},
    {20, 228, 20 + 344, 228 + 28},
    {20, 290, 20 + 344, 290 + 28},
};
#define NTOGGLES 2
static const RECT kToggleRect[NTOGGLES] = {
    {20, 332, 20 + 46, 332 + 22},
    {196, 332, 196 + 46, 332 + 22},
};
static const RECT kToggleLabel[NTOGGLES] = {
    {74, 334, 74 + 110, 334 + 18},
    {250, 334, 250 + 114, 334 + 18},
};

// Hold-Square-for-fullscreen: which shortcut to send. Segmented picker, since
// the right answer depends entirely on the app being used.
static const RECT kFsLabelRect = {20, 406, 20 + 100, 406 + 18};
#define NFSKEYS 3
static const RECT kFsSeg[NFSKEYS] = {
    {124, 402, 124 + 76, 402 + 26},
    {206, 402, 206 + 76, 402 + 26},
    {288, 402, 288 + 76, 402 + 26},
};
static const wchar_t* kFsName[NFSKEYS] = {L"F11", L"Alt+Enter", L"F"};

// Feature list. Each row is an icon for what the action does, its name, and a
// button showing the control bound to it - click to rebind. The two D-pad
// rows are shown for reference and are not rebindable.
static const RECT kLegendHdr = {20, 416, 20 + 344, 416 + 18};
#define NROWS 9
#define ROW_Y0   442
#define ROW_STEP 30
// icon kind
enum { IC_LCLICK, IC_RCLICK, IC_KEYBOARD, IC_PLAY, IC_FULLSCREEN,
       IC_LAUNCHER, IC_POWER, IC_VOLUME, IC_SCRUB };
// feature index, or -1 for a fixed row
static const int kRowFeature[NROWS] = {
    F_LCLICK, F_RCLICK, F_KEYBOARD, F_PLAYPAUSE, F_FULLSCREEN,
    F_LAUNCHER, F_TOGGLE, -1, -1};
static const int kRowIcon[NROWS] = {
    IC_LCLICK, IC_RCLICK, IC_KEYBOARD, IC_PLAY, IC_FULLSCREEN,
    IC_LAUNCHER, IC_POWER, IC_VOLUME, IC_SCRUB};
static const wchar_t* kRowName[NROWS] = {
    L"Left click", L"Right click", L"On-screen keyboard", L"Play / pause",
    L"Fullscreen (hold)", L"App launcher (hold)", L"Toggle mapping",
    L"Volume up / down", L"Seek / scrub"};
static const wchar_t* kRowFixed[NROWS] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    L"D-pad !91 !93", L"D-pad !90 !92"};

static RECT row_btn_rect(int i) {
    int y = ROW_Y0 + i * ROW_STEP;
    RECT r = {248, y, 248 + 116, y + 26};
    return r;
}

static const RECT kFooterRect = {20, 726, 20 + 344, 726 + 18};
static const wchar_t* kFooterText =
    L"Close sends to tray; right-click the tray icon to quit.";

static const wchar_t* kTrackLabel[NTRACKS] = {
    L"Mouse sensitivity", L"Scroll sensitivity", L"Deadzone",
    L"Response curve"};
static const wchar_t* kToggleText[NTOGGLES] = {L"Enabled", L"Pause in games"};

// HidHide state, kept to one short line. Detail only appears when something
// needs doing about it.
static const wchar_t* hide_status_text() {
    if (g_hh == INVALID_HANDLE_VALUE) return L"HidHide: Not installed";
    if (!g_hh_whitelisted)            return L"HidHide: Installed (needs admin)";
    if (!g_pad_inst_count)            return L"HidHide: Installed (no pad found)";
    return g_hh_hiding ? L"HidHide: Installed - pad hidden"
                       : L"HidHide: Installed";
}

static int g_drag_track = -1;  // trackbar index being dragged by the mouse, -1 = none

// --- Feature icons ----------------------------------------------------------
// Drawn rather than shipped as bitmaps or taken from an icon font: they stay
// sharp at any DPI, and nothing depends on a particular font being present.
static void fill_tri(ID2D1RenderTarget* rt, D2D1_POINT_2F a, D2D1_POINT_2F b,
                     D2D1_POINT_2F c, ID2D1Brush* br) {
    if (!g_d2d_factory) return;
    ID2D1PathGeometry* g = NULL;
    if (FAILED(g_d2d_factory->CreatePathGeometry(&g)) || !g) return;
    ID2D1GeometrySink* sink = NULL;
    if (SUCCEEDED(g->Open(&sink)) && sink) {
        sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
        D2D1_POINT_2F pts[2] = {b, c};
        sink->AddLines(pts, 2);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();
        rt->FillGeometry(g, br);
    }
    g->Release();
}

static void draw_feature_icon(ID2D1RenderTarget* rt, float cx, float cy,
                              int kind, ID2D1Brush* on, ID2D1Brush* off) {
    switch (kind) {
    case IC_LCLICK:
    case IC_RCLICK: {
        // Mouse body with the pressed button filled.
        D2D1_RECT_F body = D2D1::RectF(cx - 7, cy - 10, cx + 7, cy + 10);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 7, 7), off, 1.3f);
        float split = cy - 2;
        D2D1_RECT_F half = (kind == IC_LCLICK)
            ? D2D1::RectF(cx - 6, cy - 9, cx - 0.5f, split)
            : D2D1::RectF(cx + 0.5f, cy - 9, cx + 6, split);
        rt->FillRectangle(half, on);
        rt->DrawLine(D2D1::Point2F(cx - 7, split), D2D1::Point2F(cx + 7, split),
                     off, 1.2f);
        break;
    }
    case IC_KEYBOARD: {
        D2D1_RECT_F b = D2D1::RectF(cx - 11, cy - 7, cx + 11, cy + 7);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(b, 3, 3), off, 1.3f);
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 4; c++)
                rt->FillRectangle(
                    D2D1::RectF(cx - 8 + c * 5, cy - 4 + r * 5,
                                cx - 5.5f + c * 5, cy - 1.5f + r * 5), on);
        break;
    }
    case IC_PLAY: {
        fill_tri(rt, D2D1::Point2F(cx - 9, cy - 7), D2D1::Point2F(cx - 9, cy + 7),
                 D2D1::Point2F(cx - 1, cy), on);
        rt->FillRectangle(D2D1::RectF(cx + 3, cy - 7, cx + 5, cy + 7), on);
        rt->FillRectangle(D2D1::RectF(cx + 7, cy - 7, cx + 9, cy + 7), on);
        break;
    }
    case IC_FULLSCREEN: {
        // Four corner brackets.
        const float o = 9, t = 1.6f, l = 5;
        D2D1_RECT_F r[8] = {
            {cx - o, cy - o, cx - o + l, cy - o + t},
            {cx - o, cy - o, cx - o + t, cy - o + l},
            {cx + o - l, cy - o, cx + o, cy - o + t},
            {cx + o - t, cy - o, cx + o, cy - o + l},
            {cx - o, cy + o - t, cx - o + l, cy + o},
            {cx - o, cy + o - l, cx - o + t, cy + o},
            {cx + o - l, cy + o - t, cx + o, cy + o},
            {cx + o - t, cy + o - l, cx + o, cy + o}};
        for (int i = 0; i < 8; i++) rt->FillRectangle(r[i], on);
        break;
    }
    case IC_LAUNCHER: {
        for (int i = 0; i < 4; i++) {
            float x = cx - 9 + (i % 2) * 10, y = cy - 9 + (i / 2) * 10;
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(x, y, x + 8, y + 8), 2, 2), on);
        }
        break;
    }
    case IC_POWER: {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 1), 8, 8), off, 1.4f);
        rt->FillRectangle(D2D1::RectF(cx - 1, cy - 10, cx + 1, cy - 1), on);
        break;
    }
    case IC_VOLUME: {
        rt->FillRectangle(D2D1::RectF(cx - 10, cy - 3, cx - 5, cy + 3), on);
        fill_tri(rt, D2D1::Point2F(cx - 5, cy - 8), D2D1::Point2F(cx - 5, cy + 8),
                 D2D1::Point2F(cx + 1, cy), on);
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 1, cy), 6, 6), off, 1.3f);
        break;
    }
    case IC_SCRUB: {
        fill_tri(rt, D2D1::Point2F(cx - 2, cy - 7), D2D1::Point2F(cx - 2, cy + 7),
                 D2D1::Point2F(cx - 10, cy), on);
        fill_tri(rt, D2D1::Point2F(cx + 2, cy - 7), D2D1::Point2F(cx + 2, cy + 7),
                 D2D1::Point2F(cx + 10, cy), on);
        break;
    }
    }
}

// --- Control legend icons ---------------------------------------------------
// Drawn rather than shipped as bitmaps: they stay sharp at any DPI, and a
// filled position on an otherwise plain diamond/cross reads the same whether
// the pad calls that button Cross, A or B.

// Four buttons in a diamond; `which` is 0=top,1=right,2=bottom,3=left.
static void draw_face_icon(ID2D1RenderTarget* rt, float cx, float cy, int which,
                           ID2D1Brush* on, ID2D1Brush* off) {
    const float d = 7.0f, r = 3.4f;
    D2D1_POINT_2F p[4] = {{cx, cy - d}, {cx + d, cy}, {cx, cy + d}, {cx - d, cy}};
    for (int i = 0; i < 4; i++) {
        D2D1_ELLIPSE e = D2D1::Ellipse(p[i], r, r);
        if (i == which) rt->FillEllipse(e, on);
        else            rt->DrawEllipse(e, off, 1.2f);
    }
}

// A D-pad cross with one axis highlighted; `vertical` picks up/down vs left/right.
static void draw_dpad_icon(ID2D1RenderTarget* rt, float cx, float cy, bool vertical,
                           ID2D1Brush* on, ID2D1Brush* off) {
    const float a = 3.2f, b = 10.0f;   // arm half-width, arm reach
    D2D1_RECT_F up    = D2D1::RectF(cx - a, cy - b, cx + a, cy - a);
    D2D1_RECT_F down  = D2D1::RectF(cx - a, cy + a, cx + a, cy + b);
    D2D1_RECT_F left  = D2D1::RectF(cx - b, cy - a, cx - a, cy + a);
    D2D1_RECT_F right = D2D1::RectF(cx + a, cy - a, cx + b, cy + a);
    rt->FillRectangle(up,    vertical ? on : off);
    rt->FillRectangle(down,  vertical ? on : off);
    rt->FillRectangle(left,  vertical ? off : on);
    rt->FillRectangle(right, vertical ? off : on);
    rt->FillRectangle(D2D1::RectF(cx - a, cy - a, cx + a, cy + a), off);
}

// Mouse messages arrive in physical pixels; the layout is in DIPs.
static POINT lparam_to_dip(LPARAM lp) {
    POINT pt = {px_to_dip((int)(short)LOWORD(lp)), px_to_dip((int)(short)HIWORD(lp))};
    return pt;
}

static void update_value(int idx) {
    Config c = get_cfg();
    if (idx == TRK_MOUSE) {
        swprintf(g_mouse_val_txt, 32, L"%d", (int)std::lround(c.mouse_sensitivity));
    } else if (idx == TRK_SCROLL) {
        swprintf(g_scroll_val_txt, 32, L"%.1f", c.scroll_sensitivity);
    } else if (idx == TRK_DEADZONE) {
        swprintf(g_dz_val_txt, 32, L"%d%%", (int)std::lround(c.deadzone * 100));
    } else if (idx == TRK_CURVE) {
        if (c.mouse_curve <= 1.02) wcscpy(g_curve_val_txt, L"Linear");
        else swprintf(g_curve_val_txt, 32, L"%.1f", c.mouse_curve);
    }
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

// DualSense / DualShock DirectInput button names for the common indices.
static void button_name(int b, wchar_t* out, size_t n) {
    static const wchar_t* names[] = {
        L"Square", L"Cross", L"Circle", L"Triangle", L"L1", L"R1", L"L2",
        L"R2", L"Create", L"Options", L"L3", L"R3", L"PS", L"Touchpad"};
    if (b >= 0 && b < 14) swprintf(out, n, L"%s", names[b]);
    else if (b >= 0)      swprintf(out, n, L"Button %d", b);
    else                  swprintf(out, n, L"Unbound");
}

// Current trackbar position (in the same integer units the old TBM_* range
// used) derived straight from config, so painting and hit-testing agree.
static int track_current_pos(int idx) {
    Config c = get_cfg();
    if (idx == TRK_MOUSE) return (int)std::lround(c.mouse_sensitivity);
    if (idx == TRK_SCROLL) return (int)std::lround(c.scroll_sensitivity * 10);
    if (idx == TRK_CURVE) return (int)std::lround(c.mouse_curve * 10);
    return (int)std::lround(c.deadzone * 100);
}

static int track_pos_from_x(int idx, int x) {
    const RECT& r = kTrackRect[idx];
    double frac = (double)(x - r.left) / (double)(r.right - r.left);
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    return kTrackLo[idx] + (int)std::lround(frac * (kTrackHi[idx] - kTrackLo[idx]));
}

static void apply_track_pos(int idx, int pos) {
    EnterCriticalSection(&g_cs);
    if (idx == TRK_MOUSE) g_cfg.mouse_sensitivity = pos;
    else if (idx == TRK_SCROLL) g_cfg.scroll_sensitivity = pos / 10.0;
    else if (idx == TRK_DEADZONE) g_cfg.deadzone = pos / 100.0;
    else if (idx == TRK_CURVE) g_cfg.mouse_curve = pos / 10.0;
    Config c = g_cfg;
    LeaveCriticalSection(&g_cs);
    save_config(c);
    update_value(idx);
}

static int hit_test_track(POINT pt) {
    for (int i = 0; i < NTRACKS; i++)
        if (PtInRect(&kTrackRect[i], pt)) return i;
    return -1;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        d2d_create_main(hwnd);
        update_value(TRK_MOUSE);
        update_value(TRK_SCROLL);
        update_value(TRK_DEADZONE);
        update_value(TRK_CURVE);
        SetTimer(hwnd, ID_TIMER, 500, NULL);
        return 0;
    }
    case WM_SIZE:
        if (g_rt_main) g_rt_main->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_ERASEBKGND:
        return 1;   // WM_PAINT clears the whole client area itself
    case WM_LBUTTONDOWN: {
        POINT pt = lparam_to_dip(lp);
        int idx = hit_test_track(pt);
        if (idx >= 0) {
            g_drag_track = idx;
            SetCapture(hwnd);
            apply_track_pos(idx, track_pos_from_x(idx, pt.x));
            return 0;
        }
        for (int i = 0; i < NTOGGLES; i++) {
            if (!PtInRect(&kToggleRect[i], pt)) continue;
            EnterCriticalSection(&g_cs);
            if (i == 0) g_cfg.enabled = !g_cfg.enabled;
            else        g_cfg.game_pause = !g_cfg.game_pause;
            Config c = g_cfg;
            LeaveCriticalSection(&g_cs);
            save_config(c);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        for (int i = 0; i < NROWS; i++) {
            int f = kRowFeature[i];
            if (f < 0) continue;
            RECT br = row_btn_rect(i);
            if (!PtInRect(&br, pt)) continue;
            g_capture_feature = f;
            g_capture = true;   // worker reports the next pressed button
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        for (int i = 0; i < NFSKEYS; i++) {
            if (!PtInRect(&kFsSeg[i], pt)) continue;
            EnterCriticalSection(&g_cs);
            g_cfg.fullscreen_key = i;
            Config c = g_cfg;
            LeaveCriticalSection(&g_cs);
            save_config(c);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g_hh == INVALID_HANDLE_VALUE && PtInRect(&kHidBtnRect, pt)) {
            // Open the download page only; installing a driver is the user's
            // decision to make in their own browser.
            ShellExecuteW(NULL, L"open", HH_RELEASES_URL, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_drag_track >= 0) {
            POINT pt = lparam_to_dip(lp);
            apply_track_pos(g_drag_track, track_pos_from_x(g_drag_track, pt.x));
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_drag_track >= 0) {
            g_drag_track = -1;
            ReleaseCapture();
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (!g_rt_main) d2d_create_main(hwnd);
        if (g_rt_main) {
            g_rt_main->BeginDraw();
            g_rt_main->Clear(d2d_clr(KB_CLR_BG));
            Config c = get_cfg();

            // Status label (4-state color, same logic as before).
            COLORREF sc = RGB(240, 110, 110);
            if (g_status_state == 1) sc = RGB(88, 210, 128);
            else if (g_status_state == 2) sc = RGB(235, 180, 80);
            else if (g_status_state == 3) sc = RGB(150, 150, 158);
            if (g_br_main_status) g_br_main_status->SetColor(d2d_clr(sc));
            if (g_tf_header && g_br_main_status)
                g_rt_main->DrawText(g_status_txt, (UINT32)wcslen(g_status_txt),
                                    g_tf_header, to_f(kStatusRect), g_br_main_status);

            // Section labels (were native STATIC controls; now DirectWrite so
            // they stay sharp at any DPI).
            if (g_tf_label) {
                const wchar_t* hs = hide_status_text();
                g_rt_main->DrawText(hs, (UINT32)wcslen(hs), g_tf_label,
                                    to_f(kHideRect), g_br_main_dim);
                for (int i = 0; i < NTRACKS; i++)
                    g_rt_main->DrawText(kTrackLabel[i], (UINT32)wcslen(kTrackLabel[i]),
                                        g_tf_label, to_f(kLabelRect[i]), g_br_main_dim);
                for (int i = 0; i < NTOGGLES; i++)
                    g_rt_main->DrawText(kToggleText[i], (UINT32)wcslen(kToggleText[i]),
                                        g_tf_label, to_f(kToggleLabel[i]), g_br_main_dim);
                g_rt_main->DrawText(L"Fullscreen", 10, g_tf_label,
                                    to_f(kFsLabelRect), g_br_main_dim);
                g_rt_main->DrawText(L"Controls", 8, g_tf_label,
                                    to_f(kLegendHdr), g_br_main_text);
                g_rt_main->DrawText(kFooterText, (UINT32)wcslen(kFooterText),
                                    g_tf_label, to_f(kFooterRect), g_br_main_dim);
            }

            // Feature rows: icon, name, and the control bound to it.
            for (int i = 0; i < NROWS; i++) {
                float cy = (float)(ROW_Y0 + i * ROW_STEP) + 13.0f;
                int f = kRowFeature[i];
                draw_feature_icon(g_rt_main, 34.0f, cy, kRowIcon[i],
                                  g_br_main_sel, g_br_main_dim);
                if (g_tf_label) {
                    RECT nr = {58, ROW_Y0 + i * ROW_STEP + 4,
                               58 + 184, ROW_Y0 + i * ROW_STEP + 22};
                    g_rt_main->DrawText(kRowName[i], (UINT32)wcslen(kRowName[i]),
                                        g_tf_label, to_f(nr), g_br_main_text);
                }
                RECT br = row_btn_rect(i);
                bool capturing = (g_capture && g_capture_feature == f && f >= 0);
                if (f >= 0)
                    draw_control(g_rt_main, to_f(br), 6.0f,
                                 capturing ? g_br_main_armed : g_br_main_key,
                                 NULL);
                if (g_tf_body) {
                    const wchar_t* t = kRowFixed[i];
                    wchar_t buf[32];
                    if (f >= 0) {
                        if (capturing) t = L"Press a button";
                        else { button_name(c.bind[f], buf, 32); t = buf; }
                    }
                    g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    g_rt_main->DrawText(t, (UINT32)wcslen(t), g_tf_body,
                                        to_f(br),
                                        f >= 0 ? g_br_main_text : g_br_main_dim);
                    g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                }
            }

            // Trackbars: rounded channel            // Trackbars: rounded channel + accent fill + round thumb.
            for (int i = 0; i < NTRACKS; i++) {
                const RECT& r = kTrackRect[i];
                float left = (float)r.left, right = (float)r.right;
                float cy = (float)((r.top + r.bottom) / 2);
                g_rt_main->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(left, cy - 2, right, cy + 3), 2.5f, 2.5f),
                    g_br_main_key);
                int pos = track_current_pos(i);
                double frac = (double)(pos - kTrackLo[i]) / (double)(kTrackHi[i] - kTrackLo[i]);
                float tx = left + (float)(frac * (right - left));
                if (tx > left + 4)
                    g_rt_main->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(left, cy - 2, tx, cy + 3), 2.5f, 2.5f),
                        g_br_main_sel);
                // Glow under the thumb while dragging - feedback the old flat
                // GDI Ellipse couldn't give.
                D2D1_ELLIPSE thumb = D2D1::Ellipse(D2D1::Point2F(tx, cy), 8.0f, 8.0f);
                if (g_drag_track == i && g_br_main_glow) {
                    for (int k = 3; k >= 1; k--) {
                        g_br_main_glow->SetOpacity(0.22f / k);
                        g_rt_main->FillEllipse(
                            D2D1::Ellipse(thumb.point, 8.0f + 3.5f * k, 8.0f + 3.5f * k),
                            g_br_main_glow);
                    }
                    g_br_main_glow->SetOpacity(1.0f);
                }
                g_rt_main->FillEllipse(thumb, g_br_main_sel);
                // Small white centre so the thumb reads against the fill.
                g_rt_main->FillEllipse(D2D1::Ellipse(thumb.point, 3.0f, 3.0f),
                                       g_br_main_white);
            }

            // Value readouts, right-aligned like the old SS_RIGHT statics.
            if (g_tf_body) {
                const wchar_t* vals[NTRACKS] = {g_mouse_val_txt, g_scroll_val_txt,
                                                g_dz_val_txt, g_curve_val_txt};
                g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                for (int i = 0; i < NTRACKS; i++)
                    g_rt_main->DrawText(vals[i], (UINT32)wcslen(vals[i]),
                                        g_tf_body, to_f(kValRect[i]), g_br_main_text);
                g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }

            // Toggle switches: pill track + sliding white knob.
            bool toggle_on[NTOGGLES] = {c.enabled, c.game_pause};
            for (int i = 0; i < NTOGGLES; i++) {
                const RECT& r = kToggleRect[i];
                float h = (float)(r.bottom - r.top);
                g_rt_main->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF((float)r.left, (float)r.top,
                                                  (float)r.right, (float)r.bottom), h / 2, h / 2),
                    toggle_on[i] ? g_br_main_sel : g_br_main_toggle_off);
                float d = h - 6;
                float kx = toggle_on[i] ? (float)r.right - 3 - d : (float)r.left + 3;
                // Windows 11 dark theme puts a black knob on the accent fill
                // and a white one on the off state, since the dark accent is a
                // light blue.
                g_rt_main->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(kx + d / 2, (float)r.top + 3 + d / 2), d / 2, d / 2),
                    toggle_on[i] ? (ID2D1Brush*)g_br_main_onacc : g_br_main_white);
            }


            // Fullscreen shortcut picker: selected segment is filled, the
            // others are outlined.
            for (int i = 0; i < NFSKEYS; i++) {
                D2D1_ROUNDED_RECT rr =
                    D2D1::RoundedRect(to_f(kFsSeg[i]), 8.0f, 8.0f);
                bool on = (c.fullscreen_key == i);
                if (on) g_rt_main->FillRoundedRectangle(rr, g_br_main_sel);
                else    g_rt_main->DrawRoundedRectangle(rr, g_br_main_key, 1.2f);
                if (g_tf_body) {
                    g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    g_rt_main->DrawText(kFsName[i], (UINT32)wcslen(kFsName[i]),
                                        g_tf_body, to_f(kFsSeg[i]),
                                        on ? (ID2D1Brush*)g_br_main_onacc
                                           : g_br_main_dim);
                    g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                }
            }

            // Install button, only while HidHide is missing.
            if (g_hh == INVALID_HANDLE_VALUE) {
                const RECT& r = kHidBtnRect;
                g_rt_main->FillRoundedRectangle(
                    D2D1::RoundedRect(to_f(r), 10.0f, 10.0f), g_br_main_key);
                if (g_tf_body) {
                    g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    g_rt_main->DrawText(L"Install", 7, g_tf_body, to_f(r), g_br_main_text);
                    g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                }
            }

            HRESULT hr = g_rt_main->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET) d2d_release_main();
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER: {
        Config c = get_cfg();
        int st;
        const wchar_t* state;
        if (!g_connected)      { st = 0; state = L"Disconnected"; }
        else if (c.enabled && c.game_pause && g_game_active && !g_override)
                               { st = 2; state = L"Paused - game detected"; }
        else if (!c.enabled)   { st = 3; state = L"Disabled"; }
        else                   { st = 1; state = L"Connected"; }
        // Name the pad once we have one, so the line reads e.g.
        // "DualSense Edge : Connected" rather than a generic label.
        wchar_t line[128];
        swprintf(line, 128, L"%s : %s",
                 g_connected ? g_pad_name : L"Controller", state);
        if (st != g_status_state || wcscmp(line, g_status_txt) != 0) {
            g_status_state = st;
            wcsncpy(g_status_txt, line, 63);
            g_status_txt[63] = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_GAMEPAD:
        switch (wp) {
        case GP_KB_TOGGLE:    kb_toggle(); break;
        case GP_KB_SELECT:    kb_select(); break;
        case GP_KB_BACKSPACE: kb_send_vk(VK_BACK, false); break;
        case GP_KB_NAV:       kb_nav((int)lp); break;
        case GP_LX_TOGGLE:    lx_toggle(); break;
        case GP_LX_NAV:       lx_nav((int)lp); break;
        case GP_LX_CLOSE:
            if (g_lx_close_mode) lx_set_close_mode(false);
            else if (g_lx_visible) lx_toggle();
            break;
        case GP_LX_SELECT: {
            if (!g_lx_visible) break;
            if (g_lx_close_mode) {
                if (g_lx_sel < g_lx_count) force_close_app(g_lx_apps[g_lx_sel]);
                lx_set_close_mode(false);
                break;
            }
            if (g_lx_sel == g_lx_count) {
                // "+" tile: pick an executable. The launcher never takes
                // focus, so it is dismissed first and the dialog is put up
                // from the settings window, which can.
                lx_toggle();
                wchar_t file[MAX_PATH] = L"";
                OPENFILENAMEW ofn = {sizeof(ofn)};
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"Programs and shortcuts\0*.exe;*.lnk\0All files\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrTitle = L"Add an app to the launcher";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
                if (GetOpenFileNameW(&ofn) && g_lx_count < LX_MAX_APPS) {
                    g_lx_apps[g_lx_count++] = file;
                    lx_save();
                }
            } else if (g_lx_sel < g_lx_count) {
                std::wstring app = g_lx_apps[g_lx_sel];
                lx_toggle();   // get out of the way before the app appears
                // Switch to it if it is already running, rather than starting
                // a second copy.
                if (!activate_running(app))
                    ShellExecuteW(NULL, L"open", app.c_str(), NULL, NULL,
                                  SW_SHOWNORMAL);
            }
            break;
        }
        case GP_TOGGLE: {
            // Controller keybind: toggles whatever the user perceives. If the
            // mapping is effectively off (disabled OR game-paused), turn it on
            // - forcing past the game pause until that game closes.
            EnterCriticalSection(&g_cs);
            bool effective = g_cfg.enabled &&
                             !(g_cfg.game_pause && g_game_active && !g_override);
            if (effective) { g_cfg.enabled = false; g_override = false; }
            else           { g_cfg.enabled = true;  g_override = true;  }
            Config c = g_cfg;
            LeaveCriticalSection(&g_cs);
            save_config(c);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        case GP_CAPTURED: {
            int f = g_capture_feature;
            if (f >= 0 && f < F_COUNT) {
                EnterCriticalSection(&g_cs);
                g_cfg.bind[f] = (int)lp;
                Config c = g_cfg;
                LeaveCriticalSection(&g_cs);
                save_config(c);
            }
            g_capture = false;
            g_capture_feature = -1;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        }
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            restore_from_tray(hwnd);
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Show");
            AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");
            SetForegroundWindow(hwnd);  // required so the menu dismisses correctly
            int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                     pt.x, pt.y, 0, hwnd, NULL);
            PostMessage(hwnd, WM_NULL, 0, 0);  // KB135788: let the menu reopen next time
            DestroyMenu(menu);
            if (cmd == ID_TRAY_SHOW) restore_from_tray(hwnd);
            else if (cmd == ID_TRAY_QUIT) DestroyWindow(hwnd);
        }
        return 0;
    case WM_DPICHANGED: {
        // Per-monitor-v2: re-point both render targets at the new DPI (all
        // drawing is in DIPs, so nothing else changes), resize the keyboard
        // popup to match, and take the window rect Windows suggests.
        g_dpi = HIWORD(wp);
        if (g_rt_main) g_rt_main->SetDpi((float)g_dpi, (float)g_dpi);
        if (g_rt_kb)   g_rt_kb->SetDpi((float)g_dpi, (float)g_dpi);
        if (g_kb) {
            RECT wa;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            int ww = dip_to_px(KB_W), wh = dip_to_px(KB_H);
            g_kb_x = wa.left + (wa.right - wa.left - ww) / 2;
            g_kb_y = wa.bottom - wh - dip_to_px(12);
            SetWindowPos(g_kb, NULL, g_kb_x, g_kb_y, ww, wh,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            InvalidateRect(g_kb, NULL, FALSE);
        }
        const RECT* sug = (const RECT*)lp;
        SetWindowPos(hwnd, NULL, sug->left, sug->top,
                     sug->right - sug->left, sug->bottom - sug->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        hide_to_tray(hwnd);  // close button -> tray, keep running
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        remove_tray_icon();
        d2d_release_main();
        g_running = false;
        if (g_worker) {
            WaitForSingleObject(g_worker, 1000);
            CloseHandle(g_worker);
            g_worker = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Per-monitor-v2 DPI awareness, resolved dynamically so this still builds and
// runs on SDKs/OS versions without it (same defensive approach as the DWM
// attribute constants above). Without this Windows silently bitmap-stretches
// the whole window on a high-DPI display, which would throw away everything
// Direct2D just bought us.
static void enable_dpi_awareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    typedef BOOL(WINAPI * SetCtxFn)(HANDLE);
    SetCtxFn fn = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (fn && fn((HANDLE)-4))   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        return;
    // Older Windows 10 / 8.1 fallback.
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT(WINAPI * SetAwareFn)(int);
        SetAwareFn sa = (SetAwareFn)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (sa) sa(2);   // PROCESS_PER_MONITOR_DPI_AWARE
        FreeLibrary(shcore);
    }
}

// Offer HidHide on first launch without it. We only ever open the download
// page - downloading or running an installer on the user's behalf is not
// something this app should be doing. Declining is remembered so this is not
// a recurring nag; delete the marker (or the config) to be asked again.
static void offer_hidhide() {
    std::wstring marker = config_path();
    marker.resize(marker.find_last_of(L"\\/") + 1);
    marker += L"hidhide_declined";
    if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    int r = MessageBoxW(
        NULL,
        L"ctrlmouse can stop your controller reaching other applications "
        L"while the mapping is on, so the D-pad and stick clicks don't drive "
        L"menus or media at the same time as the mouse.\n\n"
        L"That needs HidHide - a small, free, open-source driver by Nefarius. "
        L"It is a one-time install and ctrlmouse only hides the pad while the "
        L"mapping is enabled.\n\n"
        L"Without it everything else still works; the controller just stays "
        L"visible to other apps.\n\n"
        L"Open the HidHide download page?",
        L"ctrlmouse - optional: block the pad from other apps",
        MB_YESNO | MB_ICONINFORMATION);

    if (r == IDYES) {
        ShellExecuteW(NULL, L"open", HH_RELEASES_URL, NULL, NULL, SW_SHOWNORMAL);
    } else {
        HANDLE f = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    enable_dpi_awareness();
    HANDLE mutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(CLASS_NAME, NULL);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    // COM for the shell APIs (icons, shortcut resolution) and WIC, which is
    // how an HICON becomes something Direct2D can draw.
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                     IID_IWICImagingFactory, (void**)&g_wic);

    migrate_old_data();
    InitializeCriticalSection(&g_cs);
    g_cfg = load_config();
    init_theme();
    d2d_init_process();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 0, 0, LR_DEFAULTSIZE);
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = g_kb_bg;   // avoids a white flash before the first paint
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    // Pick up the DPI of the monitor the window will open on, so the very
    // first frame is already correctly scaled.
    {
        POINT origin = {0, 0};
        HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            typedef HRESULT(WINAPI * GetDpiFn)(HMONITOR, int, UINT*, UINT*);
            GetDpiFn get = (GetDpiFn)GetProcAddress(shcore, "GetDpiForMonitor");
            UINT dx = 96, dy = 96;
            if (get && SUCCEEDED(get(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx)
                g_dpi = dx;
            FreeLibrary(shcore);
        }
    }

    RECT r = {0, 0, dip_to_px(WIN_W), dip_to_px(WIN_H)};
    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_hwnd = CreateWindowW(
        CLASS_NAME, L"ControllerMouse", style,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        NULL, NULL, hInst, NULL);
    BOOL dark = TRUE;   // dark title bar to match (Win10 1809+ / Win11)
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                          &dark, sizeof(dark));
    ShowWindow(g_hwnd, SW_SHOW);

    // Optional device-hiding support. Whitelist ourselves up front: hiding is
    // only ever enabled if that worked, so we can't hide the pad from
    // ourselves. Requires elevation, and failing is not fatal.
    if (hh_open()) {
        hh_recover_blacklist();   // undo a previous run that died while hiding
        g_hh_whitelisted = hh_whitelist_self();
        // Writing HidHide's whitelist needs elevation. Rather than force a UAC
        // prompt on every launch with a manifest, ask only when we actually
        // needed it and did not have it.
        if (!g_hh_whitelisted && !is_elevated()) {
            int r = MessageBoxW(
                NULL,
                L"HidHide is installed, but ctrlmouse needs administrator "
                L"rights once to add itself to HidHide's allowed-applications "
                L"list.\n\nWithout that it cannot hide your controller from "
                L"other apps, so the D-pad and stick clicks will keep "
                L"reaching games, Steam and menus.\n\n"
                L"Restart ctrlmouse as administrator now?",
                L"ctrlmouse - administrator rights needed once",
                MB_YESNO | MB_ICONWARNING);
            if (r == IDYES) {
                wchar_t exe[MAX_PATH];
                if (GetModuleFileNameW(NULL, exe, MAX_PATH)) {
                    if (mutex) CloseHandle(mutex);   // let the new instance win
                    ShellExecuteW(NULL, L"runas", exe, NULL, NULL, SW_SHOWNORMAL);
                    return 0;
                }
            }
        }
    } else {
        offer_hidhide();
    }

    g_worker = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteCriticalSection(&g_cs);
    return 0;
}
