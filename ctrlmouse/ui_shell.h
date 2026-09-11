// The settings window's shell: the navigation rail down the left and the
// overview page that sits beside it. Layout only - every colour, glyph and
// string still comes from the same places the rest of the window uses.
static POINT     g_ui_pointer = {-1000, -1000};   // in content space
static float     g_ui_nav_y = 146.0f;             // where the nav marker is
static float     g_ui_switch[3] = {-1, -1, -1};   // toggle knob positions
static int       g_ui_frames = 0;

// Motion is a system preference, so honour it rather than always animating.
static bool ui_motion() {
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    return enabled != FALSE;
}
static void ui_animate(HWND hwnd) {
    g_ui_frames = 18;
    SetTimer(hwnd, 90, 16, NULL);
}

static RECT ui_nav_rect(int i) {
    RECT r = {14, 146 + i * 54, rail_w() - 14, 192 + i * 54};
    return r;
}

static void ui_text(const wchar_t* text, D2D1_RECT_F r,
                    IDWriteTextFormat* font, ID2D1Brush* brush) {
    if (font && brush)
        g_rt_main->DrawText(text, (UINT32)wcslen(text), font, r, brush);
}
static void ui_text(const wchar_t* text, RECT r,
                    IDWriteTextFormat* font, ID2D1Brush* brush) {
    ui_text(text, to_f(r), font, brush);
}

// Every setting sits on one of these. Interactive ones lift very slightly
// under the pointer, which is the only cue that a whole card is clickable.
static void ui_card(RECT r, bool interactive = false) {
    draw_control(g_rt_main, to_f(r), CARD_R, g_br_main_card, g_br_main_border);
    // g_ui_pointer is kept in the rail's coordinates; cards are scrolled.
    POINT p = g_ui_pointer;
    p.y += g_scroll;
    if (interactive && PtInRect(&r, p)) {
        g_br_main_glow->SetOpacity(0.055f);
        draw_control(g_rt_main, to_f(r), CARD_R, g_br_main_glow, NULL);
        g_br_main_glow->SetOpacity(1);
    }
}

// A pad outline for the status panel. Drawn rather than shipped as art so it
// stays sharp at any DPI, and deliberately generic - it stands for whatever
// is plugged in, not for one particular controller.
static void ui_controller(float x, float y) {
    ID2D1PathGeometry* shape = NULL;
    if (!g_d2d_factory || FAILED(g_d2d_factory->CreatePathGeometry(&shape)))
        return;
    ID2D1GeometrySink* sink = NULL;
    if (SUCCEEDED(shape->Open(&sink))) {
        sink->BeginFigure(D2D1::Point2F(x + 26, y + 8),
                          D2D1_FIGURE_BEGIN_FILLED);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x + 4, y + 8),
                                            D2D1::Point2F(x - 2, y + 58),
                                            D2D1::Point2F(x + 8, y + 64)));
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x + 17, y + 73),
                                            D2D1::Point2F(x + 32, y + 46),
                                            D2D1::Point2F(x + 38, y + 46)));
        sink->AddLine(D2D1::Point2F(x + 82, y + 46));
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x + 88, y + 46),
                                            D2D1::Point2F(x + 103, y + 73),
                                            D2D1::Point2F(x + 112, y + 64)));
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x + 122, y + 58),
                                            D2D1::Point2F(x + 116, y + 8),
                                            D2D1::Point2F(x + 94, y + 8)));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();
        g_br_main_glow->SetOpacity(.09f);
        g_rt_main->FillGeometry(shape, g_br_main_glow);
        g_br_main_glow->SetOpacity(.45f);
        g_rt_main->DrawGeometry(shape, g_br_main_glow, 1.5f);
        g_rt_main->DrawLine(D2D1::Point2F(x + 24, y + 28),
                            D2D1::Point2F(x + 40, y + 28), g_br_main_glow, 3);
        g_rt_main->DrawLine(D2D1::Point2F(x + 32, y + 20),
                            D2D1::Point2F(x + 32, y + 36), g_br_main_glow, 3);
        for (int i = 0; i < 4; i++) {
            float a = 3.14159265f * .5f * i;
            g_rt_main->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(x + 90 + 8 * cosf(a),
                                            y + 28 + 8 * sinf(a)), 2.4f, 2.4f),
                g_br_main_glow);
        }
        g_br_main_glow->SetOpacity(1);
    }
    shape->Release();
}

// The rail. Every page is one click away instead of being reached through a
// card on the overview and a Back button, and it collapses to icons when the
// window is too narrow to keep both it and a readable content column.
static void ui_sidebar() {
    float w = (float)rail_w();
    g_br_main_panel->SetColor(g_mica_main.active ? D2D1::ColorF(0, 0, 0, .10f)
                                                 : d2d_clr(KB_CLR_BG));
    g_rt_main->FillRectangle(D2D1::RectF(0, 0, w, (float)g_ch), g_br_main_panel);
    g_br_main_panel->SetColor(d2d_clr(KB_CLR_BG));
    g_rt_main->DrawLine(D2D1::Point2F(w - .5f, 0),
                        D2D1::Point2F(w - .5f, (float)g_ch), g_br_main_border);

    draw_control(g_rt_main, D2D1::RectF(22, 32, 54, 64), 10, g_br_main_sel, NULL);
    draw_feature_icon(g_rt_main, 38, 48, IC_CURSOR, g_br_main_onacc);
    if (w > 100) {
        ui_text(L"ctrlmouse", D2D1::RectF(65, 31, w - 10, 65), g_tf_key,
                g_br_main_text);
        ui_text(L"PAGES", D2D1::RectF(26, 116, w - 10, 136), g_tf_label,
                g_br_main_dim);
    }

    // The marker slides to the page rather than jumping, so it is obvious
    // which way you moved through the list.
    RECT nr = ui_nav_rect(g_page);
    float target = (float)nr.top;
    if (!ui_motion() || fabsf(target - g_ui_nav_y) < .5f) g_ui_nav_y = target;
    else g_ui_nav_y += (target - g_ui_nav_y) * .28f;
    g_br_main_glow->SetOpacity(.10f);
    draw_control(g_rt_main, D2D1::RectF(14, g_ui_nav_y, w - 14, g_ui_nav_y + 46),
                 11, g_br_main_glow, NULL);
    g_br_main_glow->SetOpacity(1);
    g_rt_main->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(14, g_ui_nav_y + 14, 17, g_ui_nav_y + 32),
                          1.5f, 1.5f), g_br_main_sel);

    const wchar_t* names[] = {L"Overview", L"Button layout", L"Per-app rules",
                              L"Name the buttons"};
    const int icons[] = {IC_TUNE, IC_PAD, IC_LAUNCHER, IC_KEYS};
    for (int i = 0; i < 4; i++) {
        RECT r = ui_nav_rect(i);
        if (PtInRect(&r, g_ui_pointer) && i != g_page) {
            g_br_main_glow->SetOpacity(.045f);
            draw_control(g_rt_main, to_f(r), 11, g_br_main_glow, NULL);
            g_br_main_glow->SetOpacity(1);
        }
        ID2D1Brush* b = i == g_page ? (ID2D1Brush*)g_br_main_sel : g_br_main_dim;
        draw_feature_icon(g_rt_main, 38, (float)(r.top + 23), icons[i], b);
        if (w > 100)
            ui_text(names[i], D2D1::RectF(62, (float)r.top, w - 20,
                                          (float)r.bottom), g_tf_body, b);
    }

    if (w > 100 && g_ch > 540) {
        g_rt_main->DrawLine(D2D1::Point2F(24, (float)g_ch - 108),
                            D2D1::Point2F(w - 24, (float)g_ch - 108),
                            g_br_main_border);
        g_rt_main->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(30, (float)g_ch - 79), 3, 3),
            g_connected ? (ID2D1Brush*)g_br_main_sel : g_br_main_dim);
        ui_text(g_connected ? L"Controller connected" : L"No controller",
                D2D1::RectF(42, (float)g_ch - 92, w - 12, (float)g_ch - 66),
                g_tf_label, g_br_main_dim);
    }
}

// The overview. Status panel, then the sliders two to a row, then the
// switches and the search mode.
static void ui_home(const Config& c) {
    float x = (float)content_x(), right = x + content_w();
    ui_text(L"ctrlmouse", title_rect(), g_tf_title, g_br_main_text);

    RECT hero = {(LONG)x, 108, (LONG)right, 210};
    ui_card(hero);
    g_br_main_glow->SetOpacity(.20f);
    g_rt_main->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x, 128, x + 3, 190), 1.5f, 1.5f),
        g_br_main_glow);
    g_br_main_glow->SetOpacity(1);
    COLORREF sc = RGB(240, 110, 110);
    if (g_status_state == 1) sc = RGB(88, 210, 128);
    else if (g_status_state == 2) sc = RGB(235, 180, 80);
    else if (g_status_state == 3) sc = RGB(150, 150, 158);
    if (g_br_main_status) g_br_main_status->SetColor(d2d_clr(sc));
    ui_text(g_status_txt, status_rect(), g_tf_header, g_br_main_status);
    ui_text(g_connected ? hide_status_text()
                        : L"Connect a controller to get started.",
            hide_rect(), g_tf_label, g_br_main_dim);
    ui_controller(right - 150, 119);
    if (g_hh == INVALID_HANDLE_VALUE) {
        RECT hb = hid_btn_rect();
        draw_control(g_rt_main, to_f(hb), 8, g_br_main_key, g_br_main_border);
        g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        ui_text(L"Install", hb, g_tf_body, g_br_main_text);
        g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    ui_text(L"POINTER", D2D1::RectF(x, SEC1_Y, right, SEC1_Y + 20), g_tf_label,
            g_br_main_dim);
    const int icons[] = {IC_CURSOR, IC_UPDOWN, IC_TUNE, IC_GEAR};
    const wchar_t* vals[] = {g_mouse_val_txt, g_scroll_val_txt, g_dz_val_txt,
                             g_curve_val_txt};
    for (int i = 0; i < NTRACKS; i++) {
        RECT r = slide_card(i);
        ui_card(r, true);
        draw_feature_icon(g_rt_main, (float)r.left + 29, (float)r.top + 29,
                          icons[i], g_br_main_sel);
        ui_text(kTrackLabel[i], slide_label(i), g_tf_body, g_br_main_text);
        ui_text(kTrackDesc[i], slide_desc(i), g_tf_label, g_br_main_dim);
        g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        ui_text(vals[i], slide_value(i), g_tf_body, g_br_main_sel);
        g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        RECT tr = slide_track(i);
        float cy = (tr.top + tr.bottom) * .5f;
        float fraction = (float)(track_current_pos(i) - kTrackLo[i])
                       / (kTrackHi[i] - kTrackLo[i]);
        float tx = tr.left + fraction * (tr.right - tr.left);
        draw_control(g_rt_main, D2D1::RectF((float)tr.left, cy - 2,
                                            (float)tr.right, cy + 2), 2,
                     g_br_main_key, NULL);
        if (tx > tr.left)
            draw_control(g_rt_main, D2D1::RectF((float)tr.left, cy - 2, tx,
                                                cy + 2), 2, g_br_main_sel, NULL);
        if (g_drag_track == i) {
            g_br_main_glow->SetOpacity(.14f);
            g_rt_main->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx, cy), 17, 17),
                                   g_br_main_glow);
            g_br_main_glow->SetOpacity(1);
        }
        g_rt_main->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx, cy), 7, 7),
                               g_br_main_text);
        g_rt_main->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx, cy), 3, 3),
                               g_br_main_onacc);
    }

    ui_text(L"BEHAVIOUR", D2D1::RectF(x, SEC2_Y, right, SEC2_Y + 20),
            g_tf_label, g_br_main_dim);
    bool on[] = {c.enabled, c.game_pause, startup_enabled()};
    const int ti[] = {IC_POWER, IC_FULLSCREEN, IC_BOLT};
    for (int i = 0; i < NTOGGLES; i++) {
        RECT cr = toggle_card(i);
        ui_card(cr, true);
        draw_feature_icon(g_rt_main, (float)cr.left + 28, (float)cr.top + 33,
                          ti[i], g_br_main_dim);
        ui_text(kToggleText[i], toggle_label(i), g_tf_body, g_br_main_text);
        ui_text(kToggleDesc[i], toggle_desc(i), g_tf_label, g_br_main_dim);
        RECT r = toggle_rect(i);
        float value = on[i] ? 1.0f : 0.0f;
        if (g_ui_switch[i] < 0 || !ui_motion() ||
            fabsf(value - g_ui_switch[i]) < .01f)
            g_ui_switch[i] = value;
        else
            g_ui_switch[i] += (value - g_ui_switch[i]) * .25f;
        draw_control(g_rt_main, to_f(r), 11,
                     on[i] ? (ID2D1Brush*)g_br_main_sel : g_br_main_toggle_off,
                     NULL);
        float cx = r.left + 11 + g_ui_switch[i] * (r.right - r.left - 22);
        g_rt_main->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx, (r.top + r.bottom) * .5f), 7, 7),
            on[i] ? (ID2D1Brush*)g_br_main_onacc : g_br_main_white);
    }

    RECT sr = search_card();
    ui_card(sr);
    draw_feature_icon(g_rt_main, x + 28, SEARCH_Y + 30, IC_SEARCH,
                      g_br_main_sel);
    ui_text(L"Search on hold",
            D2D1::RectF(x + 54, SEARCH_Y + 12, right - 20, SEARCH_Y + 34),
            g_tf_body, g_br_main_text);
    ui_text(c.search_mode
                ? L"Presses your hotkey to open the launcher you already use."
                : L"Shows a simple list of your installed apps.",
            D2D1::RectF(x + 54, SEARCH_Y + 36, right - 20, SEARCH_Y + 58),
            g_tf_label, g_br_main_dim);
    for (int i = 0; i < NSEARCH; i++) {
        RECT r = search_seg(i);
        bool selected = c.search_mode == i;
        draw_control(g_rt_main, to_f(r), 8,
                     selected ? (ID2D1Brush*)g_br_main_sel : g_br_main_key, NULL);
        g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        ui_text(kSearchName[i], r, g_tf_body,
                selected ? (ID2D1Brush*)g_br_main_onacc : g_br_main_dim);
        g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    if (c.search_mode == 1) {
        RECT r = search_key_rect();
        draw_control(g_rt_main, to_f(r), 8, g_br_main_key, g_br_main_border);
        wchar_t name[48];
        if (g_hotkey_capture) wcscpy(name, L"Press keys...");
        else hotkey_name(c.search_mods, c.search_vk, name, 48);
        g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        ui_text(name, r, g_tf_body, g_br_main_text);
        g_tf_body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    ui_text(kFooterText, footer_rect(), g_tf_label, g_br_main_dim);
}
