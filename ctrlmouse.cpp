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
static HFONT            g_font = NULL;
static HFONT            g_font_hdr = NULL;
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

// --- DirectInput controller worker -----------------------------------------
// DirectInput axes are mapped (after SetProperty below) to [-1000, 1000]:
//   lX  = left stick X     lY  = left stick Y     lRz = right stick Y
// Buttons (DualSense / DualShock layout): [1] = Cross, [2] = Circle.
static LPDIRECTINPUT8       g_di = NULL;
static LPDIRECTINPUTDEVICE8 g_dev = NULL;
static GUID                 g_dev_guid;
static bool                 g_found = false;

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

static BOOL CALLBACK enum_cb(const DIDEVICEINSTANCEW* inst, void*) {
    g_dev_guid = inst->guidInstance;
    g_found = true;
    return DIENUM_STOP;  // take the first attached game controller
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

static bool ensure_device() {
    if (g_dev) return true;
    if (!g_di) {
        if (FAILED(DirectInput8Create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION,
                                      IID_IDirectInput8, (void**)&g_di, NULL)))
            return false;
    }
    g_found = false;
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_cb, NULL, DIEDFL_ATTACHEDONLY);
    if (!g_found) return false;
    if (FAILED(g_di->CreateDevice(g_dev_guid, &g_dev, NULL))) {
        g_dev = NULL;
        return false;
    }
    g_dev->SetDataFormat(&c_dfDIJoystick2);
    g_dev->SetCooperativeLevel(g_hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    set_axis_range(DIJOFS_X);
    set_axis_range(DIJOFS_Y);
    set_axis_range(DIJOFS_RZ);
    g_dev->Acquire();
    return true;
}

static void drop_device() {
    if (g_dev) {
        g_dev->Unacquire();
        g_dev->Release();
        g_dev = NULL;
    }
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

    while (g_running) {
        Config cfg = get_cfg();
        bool a = false, b = false;

        if (!ensure_device()) {
            g_connected = false;
            scroll_accum = 0.0;
            edge_click_release_all(a_down, b_down);
            Sleep(400);
            continue;
        }

        DIJOYSTATE2 js;
        HRESULT hr = g_dev->Poll();
        if (FAILED(hr)) hr = g_dev->Acquire();
        if (SUCCEEDED(hr)) hr = g_dev->GetDeviceState(sizeof(js), &js);

        if (FAILED(hr)) {  // unplugged or lost
            drop_device();
            g_connected = false;
            scroll_accum = 0.0;
            Sleep(300);
            continue;
        }

        g_connected = true;

        // Pressed-button mask (cheap; also drives bind capture).
        unsigned mask = 0;
        for (int bi = 0; bi < 32; bi++)
            if (js.rgbButtons[bi] & 0x80) mask |= (1u << bi);

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

        bool mapping_on = cfg.enabled &&
                          !(cfg.game_pause && g_game_active && !g_override);

        if (mapping_on && !g_capture) {
            double nlx = norm(js.lX, cfg.deadzone);
            double nly = norm(js.lY, cfg.deadzone);  // DInput Y is screen-oriented
            LONG dx = (LONG)std::lround(nlx * cfg.mouse_sensitivity);
            LONG dy = (LONG)std::lround(nly * cfg.mouse_sensitivity);
            if (dx || dy) mouse_move(dx, dy);

            double nrz = norm(js.lRz, cfg.deadzone);  // right stick Y
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

            bool cross = (js.rgbButtons[1] & 0x80) != 0;   // Cross
            bool circle = (js.rgbButtons[2] & 0x80) != 0;  // Circle
            bool tri = (js.rgbButtons[3] & 0x80) != 0;     // Triangle

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
                int dir = pov_dir(js.rgdwPOV[0]);
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

        Sleep(8);  // ~120 Hz
    }

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

// Geometry: 48px unit keys, 6px gaps, 12px margin.
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
static HFONT  g_kb_font = NULL;
static HBRUSH g_kb_bg = NULL, g_kb_key = NULL, g_kb_sel = NULL, g_kb_armed = NULL;
static HBRUSH g_toggle_off = NULL;

// Open/close + key-press animation state.
static int       g_kb_anim = 0;           // 0 idle, 1 opening, 2 closing
static ULONGLONG g_kb_anim_t0 = 0;
static ULONGLONG g_kb_pulse_t0 = 0;       // key-press flash start (0 = none)
static int       g_kb_x = 0, g_kb_y = 0;  // resting position
#define KB_TIMER    1
#define KB_ANIM_MS  160
#define KB_PULSE_MS 140
#define KB_SLIDE    26

// Shared dark-theme resources (used by the keyboard AND the settings window).
static void init_theme() {
    g_kb_font = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_font_hdr = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_kb_bg     = CreateSolidBrush(KB_CLR_BG);
    g_kb_key    = CreateSolidBrush(KB_CLR_KEY);
    g_kb_sel    = CreateSolidBrush(KB_CLR_SEL);
    g_kb_armed  = CreateSolidBrush(KB_CLR_ARMED);
    g_toggle_off = CreateSolidBrush(RGB(62, 62, 72));
}

// --- Direct2D / DirectWrite --------------------------------------------------
// The main settings window is fully D2D-drawn (see wnd_proc's WM_PAINT). The
// keyboard popup still uses its original GDI paint path for now; it moves to
// D2D in a later pass.
static ID2D1Factory1*     g_d2d_factory = NULL;
static IDWriteFactory*    g_dwrite_factory = NULL;
static IDWriteTextFormat* g_tf_body = NULL;    // Segoe UI 12
static IDWriteTextFormat* g_tf_header = NULL;  // Segoe UI 14 semibold
static IDWriteTextFormat* g_tf_key = NULL;     // Segoe UI 17 semibold (keyboard, future use)

static ID2D1HwndRenderTarget* g_rt_main = NULL;
static ID2D1SolidColorBrush*  g_br_main_bg = NULL;
static ID2D1SolidColorBrush*  g_br_main_key = NULL;
static ID2D1SolidColorBrush*  g_br_main_sel = NULL;
static ID2D1SolidColorBrush*  g_br_main_armed = NULL;
static ID2D1SolidColorBrush*  g_br_main_toggle_off = NULL;
static ID2D1SolidColorBrush*  g_br_main_text = NULL;
static ID2D1SolidColorBrush*  g_br_main_white = NULL;
static ID2D1SolidColorBrush*  g_br_main_status = NULL;  // color set per-draw

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
        DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &g_tf_body);
    g_dwrite_factory->CreateTextFormat(L"Segoe UI", NULL,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &g_tf_header);
    g_dwrite_factory->CreateTextFormat(L"Segoe UI", NULL,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 17.0f, L"en-us", &g_tf_key);
    if (g_tf_body) {
        g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_tf_body->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (g_tf_header) {
        g_tf_header->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_tf_header->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (g_tf_key) {
        g_tf_key->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_tf_key->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

static ID2D1HwndRenderTarget* d2d_create_rt(HWND hwnd) {
    if (!g_d2d_factory) return NULL;
    RECT rc;
    GetClientRect(hwnd, &rc);
    ID2D1HwndRenderTarget* rt = NULL;
    g_d2d_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd,
            D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
        &rt);
    return rt;
}

static void d2d_release_main() {
    if (g_br_main_bg) { g_br_main_bg->Release(); g_br_main_bg = NULL; }
    if (g_br_main_key) { g_br_main_key->Release(); g_br_main_key = NULL; }
    if (g_br_main_sel) { g_br_main_sel->Release(); g_br_main_sel = NULL; }
    if (g_br_main_armed) { g_br_main_armed->Release(); g_br_main_armed = NULL; }
    if (g_br_main_toggle_off) { g_br_main_toggle_off->Release(); g_br_main_toggle_off = NULL; }
    if (g_br_main_text) { g_br_main_text->Release(); g_br_main_text = NULL; }
    if (g_br_main_white) { g_br_main_white->Release(); g_br_main_white = NULL; }
    if (g_br_main_status) { g_br_main_status->Release(); g_br_main_status = NULL; }
    if (g_rt_main) { g_rt_main->Release(); g_rt_main = NULL; }
}

static bool d2d_create_main(HWND hwnd) {
    g_rt_main = d2d_create_rt(hwnd);
    if (!g_rt_main) return false;
    g_rt_main->CreateSolidColorBrush(d2d_clr(KB_CLR_BG), &g_br_main_bg);
    g_rt_main->CreateSolidColorBrush(d2d_clr(KB_CLR_KEY), &g_br_main_key);
    g_rt_main->CreateSolidColorBrush(d2d_clr(KB_CLR_SEL), &g_br_main_sel);
    g_rt_main->CreateSolidColorBrush(d2d_clr(KB_CLR_ARMED), &g_br_main_armed);
    g_rt_main->CreateSolidColorBrush(d2d_clr(RGB(62, 62, 72)), &g_br_main_toggle_off);
    g_rt_main->CreateSolidColorBrush(d2d_clr(RGB(235, 235, 240)), &g_br_main_text);
    g_rt_main->CreateSolidColorBrush(d2d_clr(RGB(255, 255, 255)), &g_br_main_white);
    g_rt_main->CreateSolidColorBrush(d2d_clr(RGB(240, 110, 110)), &g_br_main_status);
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
                         g_kb_y + (int)(KB_SLIDE * (1.0 - a)), 0, 0,
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
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC win = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);

        // Double-buffer so hold-to-repeat navigation stays flicker-free.
        HDC hdc = CreateCompatibleDC(win);
        HBITMAP bmp = CreateCompatibleBitmap(win, client.right, client.bottom);
        HGDIOBJ oldbmp = SelectObject(hdc, bmp);

        FillRect(hdc, &client, g_kb_bg);
        HGDIOBJ oldfont = SelectObject(hdc, g_kb_font);
        HGDIOBJ oldpen = SelectObject(hdc, GetStockObject(NULL_PEN));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, KB_CLR_TEXT);
        for (int r = 0; r < KB_NROWS; r++) {
            for (int i = 0; i < KB_COUNT[r]; i++) {
                RECT kr = kb_key_rect(r, i);
                bool sel = (r == g_kb_row && i == g_kb_col);
                bool armed = (KB_ROWS[r][i].vk == VK_SHIFT && g_kb_shift);
                HBRUSH flash = NULL;
                if (sel && g_kb_pulse_t0) {
                    // key-press flash: bright at press, decaying back to accent
                    double f = 1.0 - (double)(GetTickCount64() - g_kb_pulse_t0)
                                       / KB_PULSE_MS;
                    if (f > 0.0)
                        flash = CreateSolidBrush(
                            lerp_clr(KB_CLR_SEL, RGB(150, 205, 255), f));
                }
                SelectObject(hdc, flash ? flash
                                        : (sel ? g_kb_sel
                                               : (armed ? g_kb_armed : g_kb_key)));
                RoundRect(hdc, kr.left, kr.top, kr.right + 1, kr.bottom + 1, 12, 12);
                DrawTextW(hdc, KB_ROWS[r][i].label, -1, &kr,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (flash) {
                    SelectObject(hdc, GetStockObject(NULL_BRUSH));
                    DeleteObject(flash);
                }
            }
        }
        SelectObject(hdc, oldpen);
        SelectObject(hdc, oldfont);
        BitBlt(win, 0, 0, client.right, client.bottom, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, oldbmp);
        DeleteObject(bmp);
        DeleteDC(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
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
    RECT r = {0, 0, KB_W, KB_H};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int ww = r.right - r.left, wh = r.bottom - r.top;

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_kb_x = wa.left + (wa.right - wa.left - ww) / 2;   // bottom-centre of screen
    g_kb_y = wa.bottom - wh - 12;

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
        SetWindowPos(g_kb, HWND_TOPMOST, g_kb_x, g_kb_y + KB_SLIDE, 0, 0,
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

// Fixed layout for the D2D-drawn controls (was previously native child HWNDs
// via TRACKBAR_CLASSW / BS_OWNERDRAW buttons; geometry unchanged).
static const RECT kStatusRect  = {20, 16, 20 + 344, 16 + 22};
static const RECT kTrackRect[3] = {
    {20, 78, 20 + 344, 78 + 28},
    {20, 140, 20 + 344, 140 + 28},
    {20, 202, 20 + 344, 202 + 28},
};
static const RECT kValRect[3] = {
    {284, 56, 284 + 80, 56 + 18},
    {284, 118, 284 + 80, 118 + 18},
    {284, 180, 284 + 80, 180 + 18},
};
static const RECT kToggleRect[2] = {
    {20, 244, 20 + 46, 244 + 22},
    {196, 244, 196 + 46, 244 + 22},
};
static const RECT kBindValRect = {124, 282, 124 + 150, 282 + 18};
static const RECT kBindBtnRect = {284, 278, 284 + 80, 278 + 24};

static int g_drag_track = -1;  // trackbar index being dragged by the mouse, -1 = none

static void apply_font(HWND h) {
    if (h && g_font) SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
}

static HWND make_text(HWND parent, const wchar_t* text, DWORD style,
                      int x, int y, int w, int h) {
    HWND c = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent, NULL, GetModuleHandleW(NULL), NULL);
    apply_font(c);
    return c;
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
    if (g_hwnd) {
        InvalidateRect(g_hwnd, &kTrackRect[idx], FALSE);
        InvalidateRect(g_hwnd, &kValRect[idx], FALSE);
    }
}

static void update_bind_text() {
    // DualSense / DualShock DirectInput button names for the common indices.
    static const wchar_t* names[] = {
        L"Square", L"Cross", L"Circle", L"Triangle", L"L1", L"R1", L"L2",
        L"R2", L"Create", L"Options", L"L3", L"R3", L"PS", L"Touchpad"};
    int b = get_cfg().toggle_button;
    if (b >= 0 && b < 14) swprintf(g_bind_val_txt, 32, L"%s", names[b]);
    else swprintf(g_bind_val_txt, 32, L"Button %d", b);
    if (g_hwnd) InvalidateRect(g_hwnd, &kBindValRect, FALSE);
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
        g_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        make_text(hwnd, L"Mouse sensitivity", 0, 20, 56, 200, 18);
        make_text(hwnd, L"Scroll sensitivity", 0, 20, 118, 200, 18);
        make_text(hwnd, L"Deadzone", 0, 20, 180, 200, 18);
        make_text(hwnd, L"Enabled", 0, 74, 246, 100, 18);
        make_text(hwnd, L"Pause in games", 0, 250, 246, 114, 18);
        make_text(hwnd, L"Toggle button", 0, 20, 282, 100, 18);
        make_text(hwnd, L"Triangle: on-screen keyboard  (D-pad move, Cross type,",
                  0, 20, 316, 352, 16);
        make_text(hwnd, L"Circle backspace).  Close sends to tray; tray icon to quit.",
                  0, 20, 334, 352, 16);

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
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(165, 165, 175));      // remaining labels: dim grey
        return (LRESULT)g_kb_bg;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
        int idx = hit_test_track(pt);
        if (idx >= 0) {
            g_drag_track = idx;
            SetCapture(hwnd);
            apply_track_pos(idx, track_pos_from_x(idx, pt.x));
            return 0;
        }
        if (PtInRect(&kToggleRect[0], pt)) {
            EnterCriticalSection(&g_cs);
            g_cfg.enabled = !g_cfg.enabled;
            Config c = g_cfg;
            LeaveCriticalSection(&g_cs);
            save_config(c);
            InvalidateRect(hwnd, &kToggleRect[0], FALSE);
            return 0;
        }
        if (PtInRect(&kToggleRect[1], pt)) {
            EnterCriticalSection(&g_cs);
            g_cfg.game_pause = !g_cfg.game_pause;
            Config c = g_cfg;
            LeaveCriticalSection(&g_cs);
            save_config(c);
            InvalidateRect(hwnd, &kToggleRect[1], FALSE);
            return 0;
        }
        if (PtInRect(&kBindBtnRect, pt)) {
            g_capture = true;   // worker reports the next pressed button
            InvalidateRect(hwnd, &kBindBtnRect, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_drag_track >= 0) {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
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
            if (g_tf_header && g_br_main_status) {
                D2D1_RECT_F r = D2D1::RectF((float)kStatusRect.left, (float)kStatusRect.top,
                                            (float)kStatusRect.right, (float)kStatusRect.bottom);
                g_rt_main->DrawText(g_status_txt, (UINT32)wcslen(g_status_txt),
                                    g_tf_header, r, g_br_main_status);
            }

            // Trackbars: rounded channel + accent fill + round thumb.
            for (int i = 0; i < 3; i++) {
                const RECT& r = kTrackRect[i];
                float left = (float)r.left, right = (float)r.right;
                float cy = (float)((r.top + r.bottom) / 2);
                g_rt_main->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(left, cy - 2, right, cy + 3), 4.0f, 4.0f),
                    g_br_main_key);
                int pos = track_current_pos(i);
                double frac = (double)(pos - kTrackLo[i]) / (double)(kTrackHi[i] - kTrackLo[i]);
                float tx = left + (float)(frac * (right - left));
                if (tx > left + 4)
                    g_rt_main->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(left, cy - 2, tx, cy + 3), 4.0f, 4.0f),
                        g_br_main_sel);
                g_rt_main->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx, cy), 8.0f, 8.0f),
                                       g_br_main_sel);
            }

            // Value readouts, right-aligned like the old SS_RIGHT statics.
            if (g_tf_body) {
                const wchar_t* vals[3] = {g_mouse_val_txt, g_scroll_val_txt, g_dz_val_txt};
                g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                for (int i = 0; i < 3; i++) {
                    D2D1_RECT_F r = D2D1::RectF((float)kValRect[i].left, (float)kValRect[i].top,
                                                (float)kValRect[i].right, (float)kValRect[i].bottom);
                    g_rt_main->DrawText(vals[i], (UINT32)wcslen(vals[i]),
                                        g_tf_body, r, g_br_main_text);
                }
                g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }

            // Toggle switches: pill track + sliding white knob.
            Config c = get_cfg();
            bool toggle_on[2] = {c.enabled, c.game_pause};
            for (int i = 0; i < 2; i++) {
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
            InvalidateRect(hwnd, &kStatusRect, FALSE);
            InvalidateRect(hwnd, &kToggleRect[0], FALSE);
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
            InvalidateRect(hwnd, &kToggleRect[0], FALSE);
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
            InvalidateRect(hwnd, &kBindBtnRect, FALSE);
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
    case WM_CLOSE:
        hide_to_tray(hwnd);  // close button -> tray, keep running
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        remove_tray_icon();
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
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

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
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

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 0, 0, LR_DEFAULTSIZE);
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = g_kb_bg;   // shared dark theme
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    RECT r = {0, 0, 384, 360};
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

    g_worker = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteCriticalSection(&g_cs);
    return 0;
}
