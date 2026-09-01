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

// Enable Windows visual styles (themed common controls v6) so the UI uses the
// modern look instead of the classic grey Win95 controls.
#pragma comment(linker, "\"/manifestdependency:type='win32' "                  \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "              \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// --- Config ----------------------------------------------------------------
struct Config {
    double mouse_sensitivity;   // pixels per poll at full stick deflection
    double scroll_sensitivity;  // scroll steps per poll at full deflection
    double deadzone;            // fraction of stick travel ignored near centre
    bool   enabled;
    int    toggle_button;       // controller button that toggles enable/disable
    bool   game_pause;          // auto-pause the mapping while a game is fullscreen
};

// Default toggle: 13 = touchpad click on a DualSense (unused by the mapping).
static const Config DEFAULTS = {18.0, 1.0, 0.15, true, 13, true};
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
static wchar_t          g_status_txt[64] = L"Controller: ...";
static wchar_t          g_mouse_val_txt[32] = L"";
static wchar_t          g_scroll_val_txt[32] = L"";
static wchar_t          g_dz_val_txt[32] = L"";
static wchar_t          g_bind_val_txt[32] = L"";

static Config get_cfg() {
    EnterCriticalSection(&g_cs);
    Config c = g_cfg;
    LeaveCriticalSection(&g_cs);
    return c;
}

// --- config.json (next to the exe) -----------------------------------------
static std::wstring config_path() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring p(buf);
    size_t slash = p.find_last_of(L"\\/");
    return p.substr(0, slash + 1) + L"config.json";
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
            "  \"toggle_button\": %d,\n"
            "  \"game_pause\": %s\n"
            "}\n",
            c.mouse_sensitivity, c.scroll_sensitivity, c.deadzone,
            c.enabled ? "true" : "false", c.toggle_button,
            c.game_pause ? "true" : "false");
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
    double tb;
    if (parse_double(s, "toggle_button", tb)) c.toggle_button = (int)tb;
    parse_bool(s, "game_pause", c.game_pause);
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
       GP_TOGGLE, GP_CAPTURED };

static volatile bool g_kb_visible = false;

// --- Game detection / toggle-bind state (shared with the worker) -----------
static volatile bool g_game_active = false;  // fullscreen game detected
static volatile bool g_override    = false;  // user forced mapping on in-game
static volatile bool g_capture     = false;  // waiting for a new bind press

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
#define HH_GET_ACTIVE             HH_CTL(2052)
#define HH_SET_ACTIVE             HH_CTL(2053)
#define HH_ADD_SESSION_BLACKLIST  HH_CTL(2056)
#define HH_CLR_SESSION_BLACKLIST  HH_CTL(2057)
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

static bool hh_hide(bool on) {
    if (g_hh == INVALID_HANDLE_VALUE || on == g_hh_hiding) return true;
    DWORD ret = 0;
    if (!on) {
        std::wstring empty;
        empty.push_back(L'\0');
        hh_ioctl(HH_CLR_SESSION_BLACKLIST, empty);
        // Only put the global switch back if we were the ones who turned it
        // on. The user may be hiding other devices with it, and forcing it off
        // would silently break their own HidHide setup.
        if (g_hh_changed_active) {
            DeviceIoControl(g_hh, HH_SET_ACTIVE, &g_hh_prev_active,
                            sizeof(g_hh_prev_active), NULL, 0, &ret, NULL);
            g_hh_changed_active = false;
        }
        g_hh_hiding = false;
        return true;
    }
    if (!g_hh_whitelisted || !g_pad_inst_count) return false;
    std::wstring payload = hh_multi_sz(g_pad_inst, g_pad_inst_count);
    if (!hh_ioctl(HH_ADD_SESSION_BLACKLIST, payload)) return false;

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
    return true;
}

// Ask the device what it is rather than pattern-matching its path. Path
// formats differ by transport - USB gives HID\VID_054C&PID_0CE6\..., while
// Bluetooth gives HID\{00001124-...}_VID&0002054C_PID&0CE6\... - so any
// string match silently misses one of them. Opening with zero desired access
// is a query-only open: it always succeeds and never conflicts with an
// exclusive handle.
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
        hid_parse(g_hid_buf, got, g_hid_state);
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

static BOOL CALLBACK enum_cb(const DIDEVICEINSTANCEW* inst, void*) {
    if (g_pad_count < MAX_PADS) g_pad_guids[g_pad_count++] = inst->guidInstance;
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
            if (IsEqualGUID(g_pad_guids[i], g_last_good) && try_open(g_pad_guids[i]))
                return true;

    for (int i = 0; i < g_pad_count; i++)
        if (try_open(g_pad_guids[i])) return true;
    return false;
}

static DWORD WINAPI worker_thread(LPVOID) {
    double scroll_accum = 0.0;
    bool a_down = false, b_down = false;
    bool tri_prev = false, cross_prev = false, circ_prev = false;
    int dpad_prev = -1;
    ULONGLONG dpad_t0 = 0, dpad_last = 0;  // hold-to-repeat timing
    unsigned btn_mask_prev = 0;            // all-button mask (bind capture)
    bool tbtn_prev = false;                // toggle-button edge state
    ULONGLONG gamechk_last = 0;            // last fullscreen-game check
    bool game_prev = false;                // previous fullscreen-game state
    // Whether the pad should be held exclusively right now. Computed at the
    // end of each iteration from the mapping state, so the pad is grabbed only
    // while we are actually driving the mouse and is handed straight back the
    // moment the mapping is switched off or pauses for a game.
    bool want_exclusive = false;
    int  open_fail_streak = 0;             // consecutive failures to see any pad

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
                tri_prev = cross_prev = circ_prev = false;
                tbtn_prev = false;
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
            edge_click_release_all(a_down, b_down);
            tri_prev = cross_prev = circ_prev = false;
            tbtn_prev = false;
            dpad_prev = -1;
            btn_mask_prev = 0;
            Sleep(300);
            continue;
        }

        g_connected = true;
        unsigned mask = st.mask;

        if (g_capture) {
            // Bind capture: the first newly pressed button becomes the toggle.
            unsigned fresh = mask & ~btn_mask_prev;
            if (fresh) {
                int idx = 0;
                while (!(fresh & (1u << idx))) idx++;
                PostMessageW(g_hwnd, WM_GAMEPAD, GP_CAPTURED, idx);
            }
        } else {
            // The enable/disable toggle works even while the mapping is off.
            int tb = cfg.toggle_button;
            bool tbtn = (tb >= 0 && tb < 32) && ((mask >> tb) & 1);
            if (tbtn && !tbtn_prev)
                PostMessageW(g_hwnd, WM_GAMEPAD, GP_TOGGLE, 0);
            tbtn_prev = tbtn;
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
            tri_prev = cross_prev = circ_prev = false;
            tbtn_prev = false;
            dpad_prev = -1;
            btn_mask_prev = 0;
            game_prev = false;
            Sleep(150);
            continue;
        }
        game_prev = g_game_active;

        bool mapping_on = cfg.enabled &&
                          !(cfg.game_pause && g_game_active && !g_override);

        if (mapping_on && !g_capture) {
            double nlx = norm(st.lx, cfg.deadzone);
            double nly = norm(st.ly, cfg.deadzone);  // Y is screen-oriented
            LONG dx = (LONG)std::lround(nlx * cfg.mouse_sensitivity);
            LONG dy = (LONG)std::lround(nly * cfg.mouse_sensitivity);
            if (dx || dy) mouse_move(dx, dy);

            double nrz = norm(st.ry, cfg.deadzone);  // right stick Y
            if (nrz != 0.0) {
                scroll_accum += -nrz * cfg.scroll_sensitivity;  // stick up -> scroll up
                int steps = (int)scroll_accum;
                if (steps) {
                    mouse_scroll(steps);
                    scroll_accum -= steps;
                }
            } else {
                scroll_accum = 0.0;
            }

            bool cross  = (mask >> 1) & 1;
            bool circle = (mask >> 2) & 1;
            bool tri    = (mask >> 3) & 1;

            if (tri && !tri_prev)
                PostMessageW(g_hwnd, WM_GAMEPAD, GP_KB_TOGGLE, 0);
            tri_prev = tri;

            if (g_kb_visible) {
                // Keyboard open: buttons drive the keyboard, not the mouse.
                if (cross && !cross_prev)
                    PostMessageW(g_hwnd, WM_GAMEPAD, GP_KB_SELECT, 0);
                if (circle && !circ_prev)
                    PostMessageW(g_hwnd, WM_GAMEPAD, GP_KB_BACKSPACE, 0);
                // D-pad with hold-to-repeat: first move immediately, then
                // after 400ms repeat every 110ms while held.
                int dir = st.hat;
                ULONGLONG tnow = GetTickCount64();
                if (dir != dpad_prev) {
                    if (dir != -1) {
                        PostMessageW(g_hwnd, WM_GAMEPAD, GP_KB_NAV, dir);
                        dpad_t0 = dpad_last = tnow;
                    }
                    dpad_prev = dir;
                } else if (dir != -1 && tnow - dpad_t0 >= 400 &&
                           tnow - dpad_last >= 110) {
                    PostMessageW(g_hwnd, WM_GAMEPAD, GP_KB_NAV, dir);
                    dpad_last = tnow;
                }
            } else {
                a = cross;
                b = circle;
                dpad_prev = -1;
            }
            cross_prev = cross;
            circ_prev = circle;
        } else {
            scroll_accum = 0.0;
            tri_prev = cross_prev = circ_prev = false;
            dpad_prev = -1;
        }

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

// Dark theme palette.
#define KB_CLR_BG    RGB(24, 24, 28)     // window background
#define KB_CLR_KEY   RGB(45, 45, 52)     // key face
#define KB_CLR_SEL   RGB(0, 120, 212)    // selected key (Windows accent blue)
#define KB_CLR_ARMED RGB(70, 70, 110)    // Shift key while armed
#define KB_CLR_TEXT  RGB(232, 232, 238)  // key labels

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

static ID2D1HwndRenderTarget* g_rt_kb = NULL;
static ID2D1SolidColorBrush*  g_br_kb_key = NULL;
static ID2D1SolidColorBrush*  g_br_kb_sel = NULL;
static ID2D1SolidColorBrush*  g_br_kb_armed = NULL;
static ID2D1SolidColorBrush*  g_br_kb_text = NULL;
static ID2D1SolidColorBrush*  g_br_kb_flash = NULL;   // color set per-draw

static inline D2D1_COLOR_F d2d_clr(COLORREF c, float a = 1.0f) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                         GetBValue(c) / 255.0f, a);
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
static void d2d_soft_glow(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br,
                          D2D1_RECT_F r, float radius, float spread,
                          float alpha, int layers) {
    for (int i = layers; i >= 1; i--) {
        float o = spread * i / layers;
        br->SetOpacity(alpha / (i * 1.35f));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(r.left - o, r.top - o,
                                          r.right + o, r.bottom + o),
                              radius + o, radius + o),
            br);
    }
    br->SetOpacity(1.0f);
}

static void d2d_release_main() {
    ID2D1SolidColorBrush** bs[] = {&g_br_main_key, &g_br_main_sel, &g_br_main_armed,
                                   &g_br_main_toggle_off, &g_br_main_text,
                                   &g_br_main_dim, &g_br_main_white,
                                   &g_br_main_status, &g_br_main_glow};
    for (int i = 0; i < 9; i++)
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
    return true;
}

static void d2d_release_kb() {
    ID2D1SolidColorBrush** bs[] = {&g_br_kb_key, &g_br_kb_sel, &g_br_kb_armed,
                                   &g_br_kb_text, &g_br_kb_flash};
    for (int i = 0; i < 5; i++)
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
    g_rt_kb->CreateSolidColorBrush(d2d_clr(KB_CLR_SEL), &g_br_kb_flash);
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

            for (int r = 0; r < KB_NROWS; r++) {
                for (int i = 0; i < KB_COUNT[r]; i++) {
                    RECT kr = kb_key_rect(r, i);
                    D2D1_RECT_F kf = D2D1::RectF((float)kr.left, (float)kr.top,
                                                 (float)kr.right, (float)kr.bottom);
                    bool sel = (r == g_kb_row && i == g_kb_col);
                    bool armed = (KB_ROWS[r][i].vk == VK_SHIFT && g_kb_shift);

                    ID2D1SolidColorBrush* fill =
                        sel ? g_br_kb_sel : (armed ? g_br_kb_armed : g_br_kb_key);
                    if (sel && g_kb_pulse_t0) {
                        // key-press flash: bright at press, decaying back to accent
                        double f = 1.0 - (double)(GetTickCount64() - g_kb_pulse_t0)
                                           / KB_PULSE_MS;
                        if (f > 0.0) {
                            g_br_kb_flash->SetColor(
                                d2d_clr(lerp_clr(KB_CLR_SEL, RGB(150, 205, 255), f)));
                            fill = g_br_kb_flash;
                        }
                    }
                    // Gentle glow around the selected key so it reads at a
                    // distance (this is a couch/TV UI).
                    if (sel)
                        d2d_soft_glow(g_rt_kb, g_br_kb_sel, kf, 12.0f, 7.0f, 0.30f, 3);

                    g_rt_kb->FillRoundedRectangle(
                        D2D1::RoundedRect(kf, 12.0f, 12.0f), fill);
                    if (g_tf_key)
                        g_rt_kb->DrawText(KB_ROWS[r][i].label,
                                          (UINT32)wcslen(KB_ROWS[r][i].label),
                                          g_tf_key, kf, g_br_kb_text);
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
enum { TRK_MOUSE = 0, TRK_SCROLL = 1, TRK_DEADZONE = 2 };
static const int kTrackLo[3] = {1, 1, 0};
static const int kTrackHi[3] = {60, 50, 50};

// Client size, and the whole layout below, in DIPs. Nothing here is a native
// child control any more - every label, slider, toggle and button is drawn by
// Direct2D in WM_PAINT and hit-tested by hand, so all of it scales cleanly to
// whatever DPI the monitor reports.
#define WIN_W 384
#define WIN_H 360

static const RECT kStatusRect  = {20, 16, 20 + 344, 16 + 22};
static const RECT kTrackRect[3] = {
    {20, 78, 20 + 344, 78 + 28},
    {20, 140, 20 + 344, 140 + 28},
    {20, 202, 20 + 344, 202 + 28},
};
static const RECT kLabelRect[3] = {
    {20, 56, 20 + 200, 56 + 18},
    {20, 118, 20 + 200, 118 + 18},
    {20, 180, 20 + 200, 180 + 18},
};
static const RECT kValRect[3] = {
    {284, 56, 284 + 80, 56 + 18},
    {284, 118, 284 + 80, 118 + 18},
    {284, 180, 284 + 80, 180 + 18},
};
#define NTOGGLES 2
static const RECT kToggleRect[NTOGGLES] = {
    {20, 244, 20 + 46, 244 + 22},
    {196, 244, 196 + 46, 244 + 22},
};
static const RECT kToggleLabel[NTOGGLES] = {
    {74, 246, 74 + 110, 246 + 18},
    {250, 246, 250 + 114, 246 + 18},
};
static const RECT kBindLabelRect = {20, 282, 20 + 100, 282 + 18};
static const RECT kBindValRect = {124, 282, 124 + 150, 282 + 18};
static const RECT kBindBtnRect = {284, 278, 284 + 80, 278 + 24};
static const RECT kHelpRect[2] = {
    {20, 314, 20 + 352, 314 + 18},
    {20, 332, 20 + 352, 332 + 18},
};

static const wchar_t* kTrackLabel[3] = {
    L"Mouse sensitivity", L"Scroll sensitivity", L"Deadzone"};
static const wchar_t* kToggleText[NTOGGLES] = {L"Enabled", L"Pause in games"};
static const wchar_t* kHelpText[2] = {
    L"Triangle: on-screen keyboard  (D-pad move, Cross type,",
    L"Circle backspace).  Close sends to tray; tray icon to quit."};

// Second line of the status area: whether the pad is actually hidden from
// other apps, since that silently depends on HidHide being present and on us
// having been able to whitelist ourselves.
static const RECT kHideRect = {20, 38, 20 + 344, 38 + 16};
static const wchar_t* hide_status_text() {
    static wchar_t buf[160];
    if (g_hh == INVALID_HANDLE_VALUE)
        return L"HidHide not installed - pad stays visible to other apps";
    if (!g_hh_whitelisted)
        return L"HidHide: allow-list write failed - run ctrlmouse as admin once";
    if (!g_pad_inst_count)
        return L"HidHide ready, but no DualSense collections found to hide";
    // Report HidHide's global switch as well as our own state: a blacklist
    // with the master switch off enforces nothing, and that combination used
    // to be indistinguishable from working.
    BOOLEAN act = FALSE;
    DWORD ret = 0;
    bool known = g_hh != INVALID_HANDLE_VALUE &&
                 DeviceIoControl(g_hh, HH_GET_ACTIVE, NULL, 0, &act,
                                 sizeof(act), &ret, NULL) != 0;
    swprintf(buf, 160, L"HidHide: %s, %d collection%s, master switch %s",
             g_hh_hiding ? L"hiding" : L"idle",
             g_pad_inst_count, g_pad_inst_count == 1 ? L"" : L"s",
             !known ? L"unreadable" : (act ? L"on" : L"OFF"));
    return buf;
}

static int g_drag_track = -1;  // trackbar index being dragged by the mouse, -1 = none

static inline D2D1_RECT_F to_f(const RECT& r) {
    return D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom);
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
    }
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

static void update_bind_text() {
    // DualSense / DualShock DirectInput button names for the common indices.
    static const wchar_t* names[] = {
        L"Square", L"Cross", L"Circle", L"Triangle", L"L1", L"R1", L"L2",
        L"R2", L"Create", L"Options", L"L3", L"R3", L"PS", L"Touchpad"};
    int b = get_cfg().toggle_button;
    if (b >= 0 && b < 14) swprintf(g_bind_val_txt, 32, L"%s", names[b]);
    else swprintf(g_bind_val_txt, 32, L"Button %d", b);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

// Current trackbar position (in the same integer units the old TBM_* range
// used) derived straight from config, so painting and hit-testing agree.
static int track_current_pos(int idx) {
    Config c = get_cfg();
    if (idx == TRK_MOUSE) return (int)std::lround(c.mouse_sensitivity);
    if (idx == TRK_SCROLL) return (int)std::lround(c.scroll_sensitivity * 10);
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
    Config c = g_cfg;
    LeaveCriticalSection(&g_cs);
    save_config(c);
    update_value(idx);
}

static int hit_test_track(POINT pt) {
    for (int i = 0; i < 3; i++)
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
        update_bind_text();
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
        if (PtInRect(&kBindBtnRect, pt)) {
            g_capture = true;   // worker reports the next pressed button
            InvalidateRect(hwnd, NULL, FALSE);
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
                for (int i = 0; i < 3; i++)
                    g_rt_main->DrawText(kTrackLabel[i], (UINT32)wcslen(kTrackLabel[i]),
                                        g_tf_label, to_f(kLabelRect[i]), g_br_main_dim);
                for (int i = 0; i < NTOGGLES; i++)
                    g_rt_main->DrawText(kToggleText[i], (UINT32)wcslen(kToggleText[i]),
                                        g_tf_label, to_f(kToggleLabel[i]), g_br_main_dim);
                g_rt_main->DrawText(L"Toggle button", 13, g_tf_label,
                                    to_f(kBindLabelRect), g_br_main_dim);
                for (int i = 0; i < 2; i++)
                    g_rt_main->DrawText(kHelpText[i], (UINT32)wcslen(kHelpText[i]),
                                        g_tf_label, to_f(kHelpRect[i]), g_br_main_dim);
            }

            // Trackbars: rounded channel + accent fill + round thumb.
            for (int i = 0; i < 3; i++) {
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
                const wchar_t* vals[3] = {g_mouse_val_txt, g_scroll_val_txt, g_dz_val_txt};
                g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                for (int i = 0; i < 3; i++)
                    g_rt_main->DrawText(vals[i], (UINT32)wcslen(vals[i]),
                                        g_tf_body, to_f(kValRect[i]), g_br_main_text);
                g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }

            // Toggle switches: pill track + sliding white knob.
            Config c = get_cfg();
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
                g_rt_main->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(kx + d / 2, (float)r.top + 3 + d / 2), d / 2, d / 2),
                    g_br_main_white);
            }

            // Bind button + its value text.
            {
                const RECT& r = kBindBtnRect;
                g_rt_main->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF((float)r.left, (float)r.top,
                                                  (float)r.right, (float)r.bottom), 10.0f, 10.0f),
                    g_capture ? g_br_main_armed : g_br_main_key);
                if (g_tf_body) {
                    D2D1_RECT_F rf = D2D1::RectF((float)r.left, (float)r.top,
                                                 (float)r.right, (float)r.bottom);
                    const wchar_t* t = g_capture ? L"Press..." : L"Change";
                    g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    g_rt_main->DrawText(t, (UINT32)wcslen(t), g_tf_body, rf, g_br_main_text);
                    g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                }
            }
            if (g_tf_body) {
                D2D1_RECT_F r = D2D1::RectF((float)kBindValRect.left, (float)kBindValRect.top,
                                            (float)kBindValRect.right, (float)kBindValRect.bottom);
                g_rt_main->DrawText(g_bind_val_txt, (UINT32)wcslen(g_bind_val_txt),
                                    g_tf_body, r, g_br_main_text);
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
        const wchar_t* txt;
        if (!g_connected)      { st = 0; txt = L"Controller: not detected"; }
        else if (c.enabled && c.game_pause && g_game_active && !g_override)
                               { st = 2; txt = L"Paused: game detected"; }
        else if (!c.enabled)   { st = 3; txt = L"Mapping disabled"; }
        else                   { st = 1; txt = L"Controller: connected"; }
        if (st != g_status_state) {
            g_status_state = st;
            wcscpy(g_status_txt, txt);
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
            EnterCriticalSection(&g_cs);
            g_cfg.toggle_button = (int)lp;
            Config c = g_cfg;
            LeaveCriticalSection(&g_cs);
            save_config(c);
            g_capture = false;
            update_bind_text();
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
