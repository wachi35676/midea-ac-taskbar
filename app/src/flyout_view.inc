// The panel code, shared by the click panel and the desktop widget. flyout.cpp includes this file twice,
// with VIEW_NS (the namespace), VIEW_WIDGET (0 or 1) and VIEW_CLASS (the window class) set differently, so each
// copy has its own window and state and the two can be open at the same time. See DESIGN.md.
// Colour rule: controls follow Windows (theme + accent); only the AC itself (the scene) uses the mode colour.

namespace VIEW_NS {

using flyout::Callbacks;
void Hide();
bool Visible();

namespace {

using midea::ac::Changes;

// This file is included twice (flyout.cpp): once as the click panel, once as the desktop widget.
constexpr bool kWidget = VIEW_WIDGET == 1;

constexpr float kWidth = 392, kHeight = 776;  // DIPs; never changes
constexpr float kPad = 16;
constexpr UINT_PTR kAnimTimer = 1;

// Vertical layout (DIPs)
constexpr float kSceneTop = 12, kSceneBottom = 196;
constexpr float kTempTop = 208;
constexpr float kModeTop = 282, kModeH = 54;
constexpr float kTabsTop = 348, kTabsH = 32;
constexpr float kContentTop = 392, kContentBottom = 760;

enum class Act {
    None, Power, TempDown, TempUp, Mode, Tab, Fan, FanSlider, SwingV, SwingH, VPos, HPos, Feature, Refresh, SelfClean,
    SleepTimer, SchedToggle, SchedEdit, SchedDelete, SchedAdd,
    DraftAction, DraftTime, DraftRepeat, DraftDate, DraftDay, DraftSave, DraftCancel, DraftDelete
};
struct Hit {
    D2D1_RECT_F r;
    Act act;
    int arg;
    bool enabled;
    int key;  // stable id for hover animation
};

enum Tab { TabControls, TabSchedule, TabDetails, TabCount };

struct FeatureDef {
    const wchar_t* label;
    const char* key;
    bool midea::ac::State::*field;
    art::Glyph glyph;
    bool needsPower;
};
const FeatureDef kFeatures[] = {
    {L"Eco", "eco", &midea::ac::State::eco, art::Glyph::Eco, true},
    {L"Turbo", "turbo", &midea::ac::State::turbo, art::Glyph::Turbo, true},
    {L"Sleep", "sleep", &midea::ac::State::sleep, art::Glyph::Sleep, true},
    {L"Purifier", "purifier", &midea::ac::State::purifier, art::Glyph::Purifier, false},
    {L"Breeze away", "breeze_away", &midea::ac::State::breezeAway, art::Glyph::BreezeAway, false},
    {L"Freeze guard", "freeze_protection", &midea::ac::State::freezeProtection, art::Glyph::FreezeGuard, false},
    {L"Beep", "beep", &midea::ac::State::sound, art::Glyph::Beep, false},
    {L"Unit in °F", "fahrenheit", &midea::ac::State::fahrenheit, art::Glyph::Fahrenheit, false},
};

// ---------------- state ----------------
HWND g_wnd = nullptr;
Backdrop g_backdrop = Backdrop::Clear;
bool g_translucent = false;  // a system backdrop shows through (Windows 11 22H2+)
void ApplyBackdrop();
ID2D1HwndRenderTarget* g_rt = nullptr;
flyout::WidgetOptions g_opts;  // the desktop widget's settings (the widget copy only)
float g_widgetScale = 0;       // DPI scale times the user's size choice (the widget copy only)
bool g_placing = false, g_placed = false;
Snapshot g_snap;
Schedule g_sched;
bool g_pending = false;
Callbacks g_cb;
std::vector<Hit> g_hits;
int g_hoverKey = -1;
double g_hoverStart = 0;
std::map<int, float> g_hoverAnim;  // key -> 0..1
int g_tab = TabControls, g_prevTab = TabControls;
float g_tabMix = 1;  // 0..1 crossfade from g_prevTab to g_tab
bool g_confirmClean = false;
int g_edit = -2;             // schedule editor: -2 closed, -1 new entry, >= 0 the entry being edited
ScheduleEntry g_draft;
int g_listTop = 0;           // first visible schedule row
bool g_dragging = false;
int g_dragValue = 0;
int g_downX = 0;       // where the pointer went down on the fan ramp (a click picks a preset, a drag an exact speed)
float g_fit = 1.0f;    // the click panel scales down to fit a work area shorter than itself
D2D1_RECT_F g_sliderTrack{};
ULONGLONG g_closedAt = 0;
RECT g_anchor{};
std::deque<std::pair<double, float>> g_watts;  // (time s, watts), last 10 minutes

// Eased scene values
double g_time = 0, g_lastFrame = 0;
float g_power = 0, g_fan = 0, g_flap = 0.5f, g_sway = 0, g_modeMix = 1;
int g_mode = art::Cool, g_fromMode = art::Cool;
bool g_animInit = false;

double Now() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / f.QuadPart;
}

bool ReducedMotion() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return !on;
}

float DpiScale() { return g_wnd ? GetDpiForWindow(g_wnd) / 96.0f : 1.0f; }
float Scale() { return kWidget && g_widgetScale > 0 ? g_widgetScale : DpiScale() * g_fit; }
bool Online() { return g_snap.hasData && g_snap.status == Snapshot::Status::Online; }

// ---------------- theme (Windows) ----------------
struct Theme {
    bool light;
    D2D1_COLOR_F bg, card, raised, chip, control, controlHover, text, sub, divider, accent, accentSoft, onAccent, danger;
};
Theme MakeTheme() {
    Theme t;
    t.light = gfx::AppsUseLightTheme();
    if (g_translucent) {
        // Over Mica / Acrylic / glass: the see-through fills Windows 11 uses (Card / Control fill colours).
        t.bg = t.light ? gfx::Rgb(0xF3F3F3, 0.0f) : gfx::Rgb(0x202020, 0.0f);
        t.card = t.light ? gfx::Rgb(0xFFFFFF, 0.62f) : gfx::Rgb(0xFFFFFF, 0.05f);
        t.raised = t.light ? gfx::Rgb(0xFFFFFF, 0.95f) : gfx::Rgb(0xFFFFFF, 0.13f);
        t.chip = t.light ? gfx::Rgb(0xFFFFFF, 0.78f) : gfx::Rgb(0x1A1A1A, 0.66f);
        t.control = t.light ? gfx::Rgb(0xFFFFFF, 0.55f) : gfx::Rgb(0xFFFFFF, 0.065f);
        t.controlHover = t.light ? gfx::Rgb(0xFFFFFF, 0.85f) : gfx::Rgb(0xFFFFFF, 0.11f);
    } else {
        t.bg = t.light ? gfx::Rgb(0xF3F3F3) : gfx::Rgb(0x202020);
        t.card = t.light ? gfx::Rgb(0xFFFFFF) : gfx::Rgb(0x2B2B2B);
        t.raised = t.card;
        t.chip = t.light ? gfx::Rgb(0xF3F3F3, 0.82f) : gfx::Rgb(0x202020, 0.78f);
        t.control = t.light ? gfx::Rgb(0xE9E9E9) : gfx::Rgb(0x363636);
        t.controlHover = t.light ? gfx::Rgb(0xDDDDDD) : gfx::Rgb(0x424242);
    }
    t.text = t.light ? gfx::Rgb(0x1A1A1A) : gfx::Rgb(0xFFFFFF);
    t.sub = t.light ? gfx::Rgb(0x616161) : gfx::Rgb(0xB8B8B8);
    t.divider = t.light ? gfx::Rgb(0xE0E0E0) : gfx::Rgb(0x3A3A3A);
    t.accent = gfx::Accent(t.light);
    t.accentSoft = t.accent;
    t.accentSoft.a = t.light ? 0.18f : 0.24f;
    const float lum = 0.2126f * t.accent.r + 0.7152f * t.accent.g + 0.0722f * t.accent.b;
    t.onAccent = lum > 0.55f ? gfx::Rgb(0x000000) : gfx::Rgb(0xFFFFFF);
    t.danger = gfx::Rgb(0xE5484D);
    return t;
}

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }

// ---------------- scene inputs ----------------
float FanTarget(const midea::ac::State& st) {
    if (st.breezeAway) return 0.15f;
    switch (st.fanSpeed) {
        case 102: return 0.55f;
        case 20: return 0.2f;
        case 40: return 0.4f;
        case 60: return 0.6f;
        case 80: return 0.8f;
        default: return std::fmax(0.08f, std::fmin(1.0f, st.fanSpeed / 100.0f));
    }
}
float FlapFor(int angle) {  // louver position -> opening (art: index / 4)
    switch (angle) {
        case 1: return 0.0f;
        case 25: return 0.25f;
        case 50: return 0.5f;
        case 75: return 0.75f;
        case 100: return 1.0f;
        default: return 0.55f;
    }
}
float SwayFor(int angle) {
    switch (angle) {
        case 1: return -1.0f;
        case 25: return -0.5f;
        case 75: return 0.5f;
        case 100: return 1.0f;
        default: return 0.0f;
    }
}

// Advances eased values. Returns 2 while anything moves (ambient scene, tweens, a hovered glyph),
// 1 when only the sleep countdown needs a once-a-second repaint, 0 when idle.
int Step() {
    const double now = Now();
    double dt = g_animInit ? now - g_lastFrame : 0;
    if (dt > 0.1) dt = 0.1;
    g_lastFrame = now;
    const bool reduced = ReducedMotion();
    const auto& st = g_snap.state;
    const bool on = Online() && st.power;
    constexpr double kTwoPi = 6.283185307;

    if (!reduced) g_time += dt;
    auto ease = [&](float& v, float target, float tau) {
        if (reduced || !g_animInit) { v = target; return; }
        v += (target - v) * static_cast<float>(1 - std::exp(-dt / tau));
        if (std::fabs(target - v) < 0.002f) v = target;
    };

    if (st.mode != g_mode && st.mode >= 1 && st.mode <= 5) {
        g_fromMode = g_mode;
        g_mode = st.mode;
        g_modeMix = (reduced || !g_animInit) ? 1.0f : 0.0f;
    }
    if (g_modeMix < 1) g_modeMix = std::fmin(1.0f, g_modeMix + static_cast<float>(dt / 0.35));

    ease(g_power, on ? 1.0f : 0.0f, 0.4f / 3);
    ease(g_fan, on ? FanTarget(st) : 0.0f, 0.25f);
    const bool swingV = (st.swing & 0xC) == 0xC, swingH = (st.swing & 0x3) == 0x3;
    const float flapTarget = swingV ? 0.5f + 0.45f * static_cast<float>(std::sin(g_time * kTwoPi / 5.0)) : FlapFor(st.verticalAngle);
    const float swayTarget = swingH ? 0.8f * static_cast<float>(std::sin(g_time * kTwoPi / 6.0)) : SwayFor(st.horizontalAngle);
    ease(g_flap, flapTarget, 0.3f);
    ease(g_sway, swayTarget, 0.3f);

    for (auto it = g_hoverAnim.begin(); it != g_hoverAnim.end();) {
        const float target = it->first == g_hoverKey ? 1.0f : 0.0f;
        float& v = it->second;
        if (reduced) v = target;
        else v += (target - v) * static_cast<float>(1 - std::exp(-dt / 0.04));
        if (target == 0 && v < 0.01f) it = g_hoverAnim.erase(it);
        else ++it;
    }
    if (g_tabMix < 1) g_tabMix = reduced ? 1.0f : std::fmin(1.0f, g_tabMix + static_cast<float>(dt / 0.15));

    g_animInit = true;
    const bool ambient = !reduced && (g_power > 0.001f || g_hoverKey >= 0);
    const bool tweening = g_modeMix < 1 || g_tabMix < 1 || !g_hoverAnim.empty() || g_dragging ||
                          (g_power > 0.001f && g_power < 0.999f);
    if (ambient || tweening) return 2;
    return g_sched.offAt != 0 ? 1 : 0;  // the sleep countdown ticks
}

// Glyph motion time: only the hovered control's icon moves.
double GlyphTime(int key) {
    if (key != g_hoverKey || ReducedMotion()) return 0.0;
    return Now() - g_hoverStart;
}

art::Scene SceneNow(bool light) {
    art::Scene s;
    const auto& st = g_snap.state;
    s.mode = g_mode;
    s.fromMode = g_fromMode;
    s.modeMix = 1 - std::pow(1 - g_modeMix, 3.0f);  // ease-out
    s.power = g_power;
    s.fan = g_fan;
    s.flap = g_flap;
    s.sway = g_sway;
    s.eco = st.eco;
    s.turbo = st.turbo;
    s.sleep = st.sleep;
    s.purifier = st.purifier;
    s.breezeAway = st.breezeAway;
    s.defrost = st.defrost;
    s.targetTemperature = static_cast<float>(st.targetTemperature);
    s.online = Online();
    s.time = g_time;
    s.reducedMotion = ReducedMotion();
    s.light = light;
    return s;
}

// ---------------- painter ----------------
struct Painter {
    ID2D1RenderTarget* rt;
    ID2D1SolidColorBrush* br = nullptr;
    Theme th;
    float s;
    explicit Painter(ID2D1RenderTarget* r) : rt(r), th(MakeTheme()), s(Scale()) {
        rt->CreateSolidColorBrush(th.text, &br);
    }
    ~Painter() { gfx::SafeRelease(br); }

    D2D1_RECT_F R(float l, float t, float r, float b) const { return D2D1::RectF(l * s, t * s, r * s, b * s); }
    void Fill(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
        br->SetColor(c);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius * s, radius * s), br);
    }
    void Outline(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float w = 1.2f) {
        br->SetColor(c);
        const float h = w * s / 2;
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h), radius * s, radius * s), br, w * s);
    }
    void Circle(const D2D1_RECT_F& r, D2D1_COLOR_F c) {
        br->SetColor(c);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2), (r.right - r.left) / 2,
                                      (r.bottom - r.top) / 2), br);
    }
    void Text(const std::wstring& t, const D2D1_RECT_F& r, float size, D2D1_COLOR_F c,
              DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING) {
        br->SetColor(c);
        rt->DrawText(t.c_str(), static_cast<UINT32>(t.size()), gfx::Font(size * s, w, a), r, br);
    }
    void Add(const D2D1_RECT_F& r, Act a, int arg, bool enabled, int key) { g_hits.push_back({r, a, arg, enabled, key}); }
    float Hover(int key, bool enabled) const {
        if (!enabled) return 0;
        auto it = g_hoverAnim.find(key);
        return it == g_hoverAnim.end() ? 0.0f : it->second;
    }
    // Background of a selectable control: accent tint when selected, else neutral that brightens on hover.
    D2D1_COLOR_F ControlBg(bool selected, int key, bool enabled) const {
        if (selected) return th.accentSoft;
        return Mix(th.control, th.controlHover, Hover(key, enabled));
    }
    // A small on/off switch inside `r` (Windows style).
    void Switch(const D2D1_RECT_F& r, bool on, bool enabled, float a) {
        const float h = r.bottom - r.top, cy = (r.top + r.bottom) / 2;
        D2D1_COLOR_F tc = on ? th.accent : th.controlHover;
        br->SetColor(WithAlpha(tc, a * (enabled ? 1 : 0.4f)));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, h / 2, h / 2), br);
        const float kr = h / 2 - 3 * s, kx = on ? r.right - h / 2 : r.left + h / 2;
        br->SetColor(WithAlpha(on ? th.onAccent : th.text, a * (enabled ? 1 : 0.4f)));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), kr, kr), br);
    }
};

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
template <class T>
std::wstring Opt(const std::optional<T>& v, const wchar_t* unit = L"") {
    if (!v) return L"—";
    wchar_t b[32];
    if constexpr (std::is_floating_point_v<T>) swprintf(b, 32, L"%.1f%s", static_cast<double>(*v), unit);
    else swprintf(b, 32, L"%d%s", static_cast<int>(*v), unit);
    return b;
}
std::wstring Watts(float w) {
    wchar_t b[32];
    if (w >= 1000) swprintf(b, 32, L"%.2f kW", w / 1000.0);
    else swprintf(b, 32, L"%d W", static_cast<int>(w));
    return b;
}
std::wstring Clock(int minutes) {
    wchar_t b[16];
    swprintf(b, 16, L"%02d:%02d", (minutes / 60) % 24, minutes % 60);
    return b;
}

// Hover keys (stable across frames).
enum KeyBase { KPower = 1, KTempDown, KTempUp, KMode = 10, KTab = 20, KFan = 30, KSlider = 40, KSwingV = 41, KSwingH = 42,
               KVPos = 50, KHPos = 60, KFeature = 70, KRefresh = 90, KClean = 91, KSleep = 100, KSched = 130, KAdd = 150, KDraft = 160 };

// ---------------- scene card: illustration, status chip, power ----------------
void SceneCard(Painter& p) {
    const auto& st = g_snap.state;
    const D2D1_RECT_F card = p.R(kPad, kSceneTop, kWidth - kPad, kSceneBottom);
    p.Fill(card, 10, p.th.card);
    art::DrawScene(p.rt, card, SceneNow(p.th.light));

    if (g_snap.status == Snapshot::Status::Offline || g_snap.status == Snapshot::Status::NoCredentials) {
        const std::wstring msg = g_snap.status == Snapshot::Status::Offline ? L"Can't reach the AC on your network. Retrying."
                                                                             : L"Run tools\\get_key.py once to pair the AC.";
        p.Text(msg, p.R(kPad, kSceneBottom - 30, kWidth - kPad, kSceneBottom - 10), 12, p.th.sub, DWRITE_FONT_WEIGHT_NORMAL,
               DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    // Readings as small chips along the bottom of the wall: room, outside, power (and a running sleep timer).
    // They always fit inside the card: full labels if there is room, else icon + value only, and if even
    // that is too wide, the lowest-priority chips are left out (outside first, then power).
    struct Chip { art::Glyph g; std::wstring full, compact; int priority; };  // higher priority stays longer
    const std::wstring room = st.indoorTemperature ? model::Deg(*st.indoorTemperature) : L"--";
    const std::wstring outside = st.outdoorTemperature ? model::Deg(*st.outdoorTemperature) : L"--";
    const std::wstring watts = st.outdoorUnitPower ? Watts(static_cast<float>(*st.outdoorUnitPower)) : L"--";
    std::vector<Chip> chips = {
        {art::Glyph::Thermometer, L"Room " + room, room, 3},
        {art::Glyph::Outdoor, L"Outside " + outside, outside, 0},
        {art::Glyph::Bolt, watts, watts, 1},
    };
    if (g_sched.offAt) {
        const long long left = static_cast<long long>(g_sched.offAt - time(nullptr));
        if (left > 0) {
            wchar_t b[32], c[32];
            swprintf(b, 32, L"Off in %lld:%02lld", left / 3600, (left / 60) % 60);
            swprintf(c, 32, L"%lld:%02lld", left / 3600, (left / 60) % 60);
            chips.push_back({art::Glyph::Sleep, b, c, 2});
        }
    }
    const float gap = 6, avail = (kWidth - kPad) - 10 - (kPad + 10);  // inside the card, 10 DIPs in each side
    auto chipW = [&](const std::wstring& text) { return 8 + 14 + 5 + gfx::TextWidth(text, gfx::Font(12 * p.s)) / p.s + 10; };
    auto total = [&](bool compact) {
        float w = 0;
        for (const auto& c : chips) w += chipW(compact ? c.compact : c.full) + gap;
        return w - gap;
    };
    bool compact = total(false) > avail;
    while (compact && total(true) > avail && chips.size() > 1) {  // drop the lowest-priority chip
        size_t lo = 0;
        for (size_t i = 1; i < chips.size(); ++i) if (chips[i].priority < chips[lo].priority) lo = i;
        chips.erase(chips.begin() + static_cast<long>(lo));
        compact = total(false) > avail;
    }
    float x = kPad + 10;
    const float y1 = kSceneBottom - 10, y0 = y1 - 24;
    for (const auto& c : chips) {
        const std::wstring& text = compact ? c.compact : c.full;
        const float w = chipW(text);
        const D2D1_RECT_F r = p.R(x, y0, x + w, y1);
        p.Fill(r, 12, p.th.chip);
        art::DrawGlyph(p.rt, c.g, p.R(x + 8, y0 + 5, x + 22, y0 + 19), p.th.sub);
        p.Text(text, p.R(x + 27, y0, x + w, y1), 12, p.th.text);
        x += w + gap;
    }
}

// The control bar: a round power button, then one wide pill for the target temperature.
void TemperatureRow(Painter& p) {
    const auto& st = g_snap.state;
    const auto& caps = g_snap.caps;
    const bool online = Online(), on = online && st.power;
    const float top = kTempTop, h = 56;

    const D2D1_RECT_F pw = p.R(kPad, top + 4, kPad + h, top + 4 + h);
    p.Add(pw, Act::Power, 0, online, KPower);
    D2D1_COLOR_F pbg = on ? p.th.accent : Mix(p.th.control, p.th.controlHover, p.Hover(KPower, online));
    if (!online) pbg = WithAlpha(pbg, 0.5f);
    p.Circle(pw, pbg);
    const float m = 17 * p.s;
    art::DrawGlyph(p.rt, art::Glyph::Power, D2D1::RectF(pw.left + m, pw.top + m, pw.right - m, pw.bottom - m),
                   WithAlpha(on ? p.th.onAccent : p.th.text, online ? 1.0f : 0.4f), GlyphTime(KPower));

    const float x0 = kPad + h + 10, x1 = kWidth - kPad;
    const D2D1_RECT_F pill = p.R(x0, top + 4, x1, top + 4 + h);
    p.Fill(pill, h / 2, p.th.card);
    const bool canTemp = online && st.mode != 5;
    const float b = 40;
    const D2D1_RECT_F minus = p.R(x0 + 8, top + 12, x0 + 8 + b, top + 12 + b);
    const D2D1_RECT_F plus = p.R(x1 - 8 - b, top + 12, x1 - 8, top + 12 + b);
    const bool minusOk = canTemp && st.targetTemperature > caps.minTemperature;
    const bool plusOk = canTemp && st.targetTemperature < caps.maxTemperature;
    p.Add(minus, Act::TempDown, 0, minusOk, KTempDown);
    p.Add(plus, Act::TempUp, 0, plusOk, KTempUp);
    for (auto [r, key, glyph, ok] : {std::tuple{minus, (int)KTempDown, art::Glyph::Minus, minusOk},
                                     std::tuple{plus, (int)KTempUp, art::Glyph::Plus, plusOk}}) {
        p.Circle(r, Mix(p.th.control, p.th.controlHover, p.Hover(key, ok)));
        const float mm = 12 * p.s;
        art::DrawGlyph(p.rt, glyph, D2D1::RectF(r.left + mm, r.top + mm, r.right - mm, r.bottom - mm),
                       WithAlpha(p.th.text, ok ? 1.0f : 0.35f));
    }
    const bool fan = st.mode == 5;
    const std::wstring big = !g_snap.hasData ? L"--" : (fan ? L"—" : model::Deg(st.targetTemperature));
    p.Text(big, p.R(x0 + 52, top + 6, x1 - 52, top + 44), 30, WithAlpha(p.th.text, on ? 1.0f : 0.6f),
           DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    p.Text(fan ? L"no target in Fan mode" : L"target temperature", p.R(x0 + 52, top + 41, x1 - 52, top + 56), 11, p.th.sub,
           DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void ModeRow(Painter& p) {
    const auto& st = g_snap.state;
    const bool online = Online();
    const int modes[] = {1, 2, 3, 4, 5};
    const wchar_t* names[] = {L"Auto", L"Cool", L"Dry", L"Heat", L"Fan"};
    const art::Glyph glyphs[] = {art::Glyph::ModeAuto, art::Glyph::ModeCool, art::Glyph::ModeDry, art::Glyph::ModeHeat, art::Glyph::ModeFan};
    const float gap = 6, w = (kWidth - 2 * kPad - 4 * gap) / 5;
    for (int i = 0; i < 5; ++i) {
        bool supported = !g_snap.caps.received;
        for (auto m : g_snap.caps.modes) supported |= static_cast<int>(m) == modes[i];
        const bool enabled = online && supported;
        const float x = kPad + i * (w + gap);
        const D2D1_RECT_F r = p.R(x, kModeTop, x + w, kModeTop + kModeH);
        const int key = KMode + i;
        p.Add(r, Act::Mode, modes[i], enabled, key);
        const bool sel = g_snap.hasData && st.mode == modes[i];
        p.Fill(r, 8, p.ControlBg(sel, key, enabled));
        if (sel) p.Outline(r, 8, WithAlpha(p.th.accent, 0.9f));
        const float a = enabled ? 1.0f : 0.35f;
        art::DrawGlyph(p.rt, glyphs[i], p.R(x + w / 2 - 11, kModeTop + 7, x + w / 2 + 11, kModeTop + 29),
                       WithAlpha(sel ? p.th.accent : p.th.text, a), GlyphTime(key));
        p.Text(names[i], p.R(x, kModeTop + 31, x + w, kModeTop + 49), 12, WithAlpha(sel ? p.th.text : p.th.sub, a),
               sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void Tabs(Painter& p) {
    const wchar_t* names[] = {L"Controls", L"Schedule", L"Details"};
    const D2D1_RECT_F bar = p.R(kPad, kTabsTop, kWidth - kPad, kTabsTop + kTabsH);
    p.Fill(bar, 8, p.th.control);
    const float w = (kWidth - 2 * kPad) / static_cast<float>(TabCount);
    for (int i = 0; i < TabCount; ++i) {
        const float x = kPad + i * w;
        const D2D1_RECT_F r = p.R(x + 3, kTabsTop + 3, x + w - 3, kTabsTop + kTabsH - 3);
        p.Add(r, Act::Tab, i, true, KTab + i);
        if (i == g_tab) p.Fill(r, 6, p.th.raised);
        else if (p.Hover(KTab + i, true) > 0) p.Fill(r, 6, WithAlpha(p.th.controlHover, p.Hover(KTab + i, true)));
        std::wstring name = names[i];
        bool anySchedule = g_sched.offAt != 0;
        for (const auto& e : g_sched.entries) anySchedule |= e.enabled;
        if (i == TabSchedule && anySchedule) name += L" \u2022";
        p.Text(name, r, 12.5f, i == g_tab ? p.th.text : p.th.sub,
               i == g_tab ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

// ---------------- shared small controls ----------------

// A segmented control: one hit per option.
void Segmented(Painter& p, float x, float y, float w, float h, const std::vector<std::wstring>& names, int selected, Act act,
               int keyBase, float a) {
    p.Fill(p.R(x, y, x + w, y + h), 8, WithAlpha(p.th.control, a));
    const float sw = (w - 4) / static_cast<float>(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        const D2D1_RECT_F r = p.R(x + 2 + static_cast<float>(i) * sw, y + 2, x + 2 + static_cast<float>(i + 1) * sw, y + h - 2);
        const int key = keyBase + static_cast<int>(i);
        p.Add(r, act, static_cast<int>(i), true, key);
        const bool sel = static_cast<int>(i) == selected;
        if (sel) p.Fill(r, 6, WithAlpha(p.th.accent, a));
        else if (p.Hover(key, true) > 0) p.Fill(r, 6, WithAlpha(p.th.controlHover, a * p.Hover(key, true)));
        p.Text(names[i], r, 12.5f, WithAlpha(sel ? p.th.onAccent : p.th.text, a),
               sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void RoundButton(Painter& p, const D2D1_RECT_F& r, art::Glyph g, Act act, int arg, int key, bool enabled, float a) {
    p.Add(r, act, arg, enabled, key);
    p.Circle(r, WithAlpha(Mix(p.th.control, p.th.controlHover, p.Hover(key, enabled)), a));
    const float m = (r.right - r.left) * 0.3f;
    art::DrawGlyph(p.rt, g, D2D1::RectF(r.left + m, r.top + m, r.right - m, r.bottom - m), WithAlpha(p.th.text, a * (enabled ? 1 : 0.35f)));
}

// style 0 neutral, 1 accent, 2 danger
void TextButton(Painter& p, const D2D1_RECT_F& r, const std::wstring& label, Act act, int arg, int key, bool enabled, float a, int style) {
    p.Add(r, act, arg, enabled, key);
    D2D1_COLOR_F bg = style == 1 ? p.th.accent : style == 2 ? p.th.danger : Mix(p.th.control, p.th.controlHover, p.Hover(key, enabled));
    if (style && p.Hover(key, enabled) > 0) bg = Mix(bg, gfx::Rgb(0xFFFFFF, bg.a), 0.12f * p.Hover(key, enabled));
    p.Fill(r, 6, WithAlpha(bg, a * (enabled ? 1 : 0.45f)));
    const D2D1_COLOR_F fg = style == 1 ? p.th.onAccent : style == 2 ? gfx::Rgb(0xFFFFFF) : p.th.text;
    p.Text(label, r, 12.5f, WithAlpha(fg, a * (enabled ? 1 : 0.6f)), style ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
           DWRITE_TEXT_ALIGNMENT_CENTER);
}

// A ring gauge: a track and a value arc that starts at the top and runs clockwise. Arguments are in DIPs.
void Ring(Painter& p, float cx, float cy, float rad, float frac, D2D1_COLOR_F track, D2D1_COLOR_F fill, float width) {
    const float s = p.s;
    cx *= s; cy *= s; rad *= s; width *= s;
    p.br->SetColor(track);
    p.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), p.br, width);
    frac = std::fmax(0.0f, std::fmin(1.0f, frac));
    if (frac < 0.004f) return;
    p.br->SetColor(fill);
    if (frac > 0.996f) {
        p.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), p.br, width);
        return;
    }
    constexpr float kPi = 3.14159265f;
    const float a0 = -kPi / 2, a1 = a0 + frac * 2 * kPi;
    ID2D1Factory* f = nullptr;
    p.rt->GetFactory(&f);
    ID2D1PathGeometry* g = nullptr;
    f->CreatePathGeometry(&g);
    ID2D1GeometrySink* k = nullptr;
    g->Open(&k);
    k->BeginFigure(D2D1::Point2F(cx + rad * std::cos(a0), cy + rad * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
    k->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + rad * std::cos(a1), cy + rad * std::sin(a1)), D2D1::SizeF(rad, rad), 0,
                               D2D1_SWEEP_DIRECTION_CLOCKWISE, frac > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    k->EndFigure(D2D1_FIGURE_END_OPEN);
    k->Close();
    ID2D1StrokeStyle* st = nullptr;
    f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &st);
    p.rt->DrawGeometry(g, p.br, width, st);
    gfx::SafeRelease(st);
    gfx::SafeRelease(k);
    gfx::SafeRelease(g);
    gfx::SafeRelease(f);
}

// A group heading on the Controls page.
void Heading(Painter& p, float y, const wchar_t* text, float a) {
    p.Text(text, p.R(kPad, y, kWidth - kPad, y + 18), 12, WithAlpha(p.th.sub, a), DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

// ---------------- Controls: fan, air direction and features on one page ----------------
void ControlsTab(Painter& p, float a) {
    const auto& st = g_snap.state;
    const auto& caps = g_snap.caps;
    const bool online = Online(), on = st.power;
    const float ga = a * (online ? 1.0f : 0.4f);

    // ---- Fan: one ramp (click a bar for a preset, drag for an exact speed), its value, and Auto
    Heading(p, 396, L"Fan speed", a);
    const float rampTop = 418, rampBase = 452, autoW = 76, valW = 46, gap = 8;
    const float rx0 = kPad, rx1 = kWidth - kPad - autoW - gap - valW - gap;
    const bool isAuto = st.fanSpeed == 102 && !st.breezeAway;
    const int value = g_dragging ? g_dragValue : (st.breezeAway || st.fanSpeed > 100 ? 0 : st.fanSpeed);
    g_sliderTrack = p.R(rx0, rampTop, rx1, rampBase);  // x range the drag maps onto 1..100 %
    p.Add(p.R(rx0 - 2, rampTop - 4, rx1 + 2, rampBase + 4), Act::FanSlider, 0, online, KSlider);
    {
        const float bg = 6, bw = (rx1 - rx0 - 4 * bg) / 5, hover = p.Hover(KSlider, online);
        for (int i = 0; i < 5; ++i) {
            const float x = rx0 + static_cast<float>(i) * (bw + bg), hgt = 14.0f + static_cast<float>(i) * 5.0f;
            const D2D1_RECT_F bar = p.R(x, rampBase - hgt, x + bw, rampBase);
            p.Fill(bar, 4, WithAlpha(Mix(p.th.control, p.th.controlHover, hover), ga));
            // Bar i covers 20*i .. 20*(i+1) per cent: it fills from the left as the speed rises through it.
            const float fill = std::fmax(0.0f, std::fmin(1.0f, (static_cast<float>(value) - 20.0f * static_cast<float>(i)) / 20.0f));
            if (fill > 0) p.Fill(p.R(x, rampBase - hgt, x + std::fmax(6.0f, bw * fill), rampBase), 4, WithAlpha(p.th.accent, ga));
        }
    }
    {
        std::wstring v = isAuto ? L"auto" : st.breezeAway ? L"breeze" : std::to_wstring(value) + L"%";
        p.Text(v, p.R(rx1 + gap, rampBase - 24, rx1 + gap + valW, rampBase - 4), 12.5f, WithAlpha(p.th.text, ga),
               DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    {
        const float ax = kWidth - kPad - autoW;
        const D2D1_RECT_F autoR = p.R(ax, rampBase - 34, kWidth - kPad, rampBase);
        p.Add(autoR, Act::Fan, 102, online, KFan + 5);
        p.Fill(autoR, 8, WithAlpha(p.ControlBg(isAuto, KFan + 5, online), a));
        if (isAuto) p.Outline(autoR, 8, WithAlpha(p.th.accent, 0.9f * a));
        art::DrawGlyph(p.rt, art::Glyph::FanAuto, p.R(ax + 10, rampBase - 25, ax + 26, rampBase - 9),
                       WithAlpha(isAuto ? p.th.accent : p.th.text, ga), GlyphTime(KFan + 5));
        p.Text(L"Auto", p.R(ax + 32, rampBase - 34, kWidth - kPad - 4, rampBase), 12.5f, WithAlpha(p.th.text, ga));
    }

    // ---- Air direction: one track per axis (Swing, then five fixed positions) with the chosen one lit
    Heading(p, 470, L"Air direction", a);
    struct Axis { const wchar_t* name; bool supportsPos; bool swinging; int angle; Act swingAct, posAct; int kSwing, kPos; bool horizontal; art::Glyph g; };
    const Axis axes[] = {
        {L"Up/down", caps.verticalAngle, (st.swing & 0xC) == 0xC, st.verticalAngle, Act::SwingV, Act::VPos, KSwingV, KVPos, false, art::Glyph::SwingVertical},
        {L"Left/right", caps.horizontalAngle, (st.swing & 0x3) == 0x3, st.horizontalAngle, Act::SwingH, Act::HPos, KSwingH, KHPos, true, art::Glyph::SwingHorizontal},
    };
    const int positions[] = {1, 25, 50, 75, 100};
    for (int ai = 0; ai < 2; ++ai) {
        const Axis& ax = axes[ai];
        const float y = 492.0f + static_cast<float>(ai) * 40.0f, h = 34, tx0 = kPad + 64, sw = 78;
        p.Text(ax.name, p.R(kPad, y, tx0, y + h), 12, WithAlpha(p.th.sub, a));
        const float pw = ax.supportsPos ? (kWidth - kPad - tx0 - sw) / 5.0f : 0.0f;
        const float txe = ax.supportsPos ? kWidth - kPad : tx0 + sw;
        p.Fill(p.R(tx0, y, txe, y + h), 8, WithAlpha(p.th.control, a));
        int sel = ax.swinging ? 0 : -1;
        if (!ax.swinging && ax.supportsPos)
            for (int i = 0; i < 5; ++i) if (ax.angle == positions[i]) sel = i + 1;
        const int segments = ax.supportsPos ? 6 : 1;
        for (int i = 0; i < segments; ++i) {
            const float sx0 = i == 0 ? tx0 : tx0 + sw + static_cast<float>(i - 1) * pw;
            const float sx1 = i == 0 ? tx0 + sw : tx0 + sw + static_cast<float>(i) * pw;
            const int key = i == 0 ? ax.kSwing : ax.kPos + i - 1;
            p.Add(p.R(sx0, y, sx1, y + h), i == 0 ? ax.swingAct : ax.posAct, i == 0 ? 0 : positions[i - 1], online, key);
            const D2D1_RECT_F seg = p.R(sx0 + 2, y + 2, sx1 - 2, y + h - 2);
            const bool lit = i == sel;
            if (lit) {
                p.Fill(seg, 6, WithAlpha(p.th.accentSoft, a));
                p.Outline(seg, 6, WithAlpha(p.th.accent, 0.9f * a));
            } else if (p.Hover(key, online) > 0) {
                p.Fill(seg, 6, WithAlpha(p.th.controlHover, a * p.Hover(key, online)));
            }
            if (i == 0) {
                art::DrawGlyph(p.rt, ax.g, p.R(sx0 + 10, y + 9, sx0 + 26, y + 25), WithAlpha(lit ? p.th.accent : p.th.text, ga), GlyphTime(key));
                p.Text(L"Swing", p.R(sx0 + 30, y, sx1, y + h), 12, WithAlpha(p.th.text, ga));
            } else {
                const float cx = (sx0 + sx1) / 2;
                art::DrawLouverPosition(p.rt, i - 1, ax.horizontal, p.R(cx - 11, y + 6, cx + 11, y + 28), WithAlpha(lit ? p.th.accent : p.th.text, ga));
            }
        }
    }

    // ---- Features: up to eight tiles
    Heading(p, 584, L"Features", a);
    const float tgap = 6, tw = (kWidth - 2 * kPad - 3 * tgap) / 4, th = 56;
    int slot = 0;
    bool needsPowerShown = false;
    for (int i = 0; i < static_cast<int>(std::size(kFeatures)); ++i) {
        const auto& f = kFeatures[i];
        const std::string k = f.key;
        bool supported = true;
        if (k == "eco") supported = caps.eco;
        else if (k == "turbo") supported = caps.Turbo();
        else if (k == "purifier") supported = caps.purifier;
        else if (k == "breeze_away") supported = caps.breezeAway || caps.breezeControl;
        else if (k == "freeze_protection") supported = caps.freezeProtection;
        if (caps.received && !supported) continue;
        const bool enabled = online && (!f.needsPower || on);
        needsPowerShown |= f.needsPower && !on;
        const float x = kPad + static_cast<float>(slot % 4) * (tw + tgap), y = 606.0f + static_cast<float>(slot / 4) * (th + tgap);
        ++slot;
        const D2D1_RECT_F r = p.R(x, y, x + tw, y + th);
        const int key = KFeature + i;
        p.Add(r, Act::Feature, i, enabled, key);
        const bool active = st.*(f.field);
        p.Fill(r, 8, WithAlpha(p.ControlBg(active, key, enabled), a));
        if (active) p.Outline(r, 8, WithAlpha(p.th.accent, 0.9f * a));
        const float fa = a * (enabled ? 1.0f : 0.35f);
        art::DrawGlyph(p.rt, f.glyph, p.R(x + tw / 2 - 10, y + 8, x + tw / 2 + 10, y + 28), WithAlpha(active ? p.th.accent : p.th.text, fa), GlyphTime(key));
        p.Text(f.label, p.R(x + 2, y + 32, x + tw - 2, y + 51), 11.5f, WithAlpha(active ? p.th.text : p.th.sub, fa),
               active ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (needsPowerShown && online)
        p.Text(L"Eco, Turbo and Sleep work while the AC is running.", p.R(kPad, 740, kWidth - kPad, 758), 11.5f, WithAlpha(p.th.sub, a));
}

// ---------------- Schedule ----------------
constexpr int kVisibleRows = 4;

std::wstring EntryWhen(const ScheduleEntry& e) {
    if (e.once) return schedule::DateLabel(e.date);
    return schedule::DaysLabel(e.days);
}

// Today on one 24-hour bar: a marker where each timer fires (filled = turns on, hollow = turns off), a span
// from each "on" to the next "off", and a hairline for now.
void DayBar(Painter& p, float y, float a) {
    const float x0 = kPad, x1 = kWidth - kPad, w = x1 - x0;
    const time_t now = time(nullptr);
    tm lt{};
    localtime_s(&lt, &now);
    const int today = schedule::Today(now), dayBit = 1 << ((lt.tm_wday + 6) % 7), nowMin = lt.tm_hour * 60 + lt.tm_min;
    struct Mark { int minutes; bool on; };
    std::vector<Mark> marks;
    for (const auto& e : g_sched.entries) {
        if (!e.enabled) continue;
        if (e.once ? e.date == today : (e.days & dayBit) != 0) marks.push_back({e.minutes, e.turnOn});
    }
    if (g_sched.offAt && schedule::Today(g_sched.offAt) == today) {
        tm o{};
        localtime_s(&o, &g_sched.offAt);
        marks.push_back({o.tm_hour * 60 + o.tm_min, false});
    }
    std::sort(marks.begin(), marks.end(), [](const Mark& l, const Mark& r) { return l.minutes < r.minutes; });
    auto X = [&](int minutes) { return x0 + w * static_cast<float>(minutes) / 1440.0f; };
    const float ty = y + 4, th = 12, cy = ty + th / 2;
    p.Fill(p.R(x0, ty, x1, ty + th), 6, WithAlpha(p.th.control, a));
    for (size_t i = 0; i < marks.size(); ++i) {
        if (!marks[i].on) continue;
        int end = 1440;
        for (size_t j = i + 1; j < marks.size(); ++j)
            if (!marks[j].on) { end = marks[j].minutes; break; }
        p.Fill(p.R(X(marks[i].minutes), ty + 2, X(end), ty + th - 2), 4, WithAlpha(p.th.accentSoft, a));
    }
    for (int hr : {0, 6, 12, 18, 24}) {
        const float cxh = X(hr * 60);
        const std::wstring label = hr == 24 ? L"24" : (hr < 10 ? L"0" : L"") + std::to_wstring(hr);
        const DWRITE_TEXT_ALIGNMENT al = hr == 0 ? DWRITE_TEXT_ALIGNMENT_LEADING : hr == 24 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_CENTER;
        const float lx0 = hr == 0 ? cxh : cxh - 20, lx1 = hr == 24 ? cxh : cxh + 20;
        p.Text(label, p.R(lx0, ty + th + 2, lx1, ty + th + 16), 10.5f, WithAlpha(p.th.sub, a), DWRITE_FONT_WEIGHT_NORMAL, al);
    }
    for (const auto& m : marks) {
        const float mx = X(m.minutes), r = 5.5f;
        const D2D1_RECT_F box = p.R(mx - r, cy - r, mx + r, cy + r);
        if (m.on) {
            p.Circle(box, WithAlpha(p.th.accent, a));
        } else {
            p.Circle(box, WithAlpha(p.th.bg.a > 0.5f ? p.th.bg : p.th.card, a));
            p.br->SetColor(WithAlpha(p.th.accent, a));
            p.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((mx)*p.s, cy * p.s), (r - 0.8f) * p.s, (r - 0.8f) * p.s), p.br, 1.6f * p.s);
        }
    }
    const float nx = X(nowMin);
    p.br->SetColor(WithAlpha(p.th.text, 0.7f * a));
    p.rt->FillRectangle(p.R(nx - 0.75f, ty - 3, nx + 0.75f, ty + th + 3), p.br);
}

void ScheduleEditor(Painter& p, float a) {
    const float x0 = kPad, x1 = kWidth - kPad;
    const ScheduleEntry& d = g_draft;
    p.Text(g_edit >= 0 ? L"Edit schedule" : L"New schedule", p.R(x0, 396, x1, 418), 14, WithAlpha(p.th.text, a), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Segmented(p, x0, 424, x1 - x0, 38, {L"Turn on", L"Turn off"}, d.turnOn ? 0 : 1, Act::DraftAction, KDraft, a);

    // The time: -1 h, -15 min, the time, +15 min, +1 h
    {
        const float y = 474, h = 52, bh = 36, by = y + (h - bh) / 2;
        TextButton(p, p.R(x0, by, x0 + 52, by + bh), L"\u22121 h", Act::DraftTime, -60, KDraft + 2, true, a, 0);
        TextButton(p, p.R(x0 + 58, by, x0 + 106, by + bh), L"\u221215", Act::DraftTime, -15, KDraft + 3, true, a, 0);
        TextButton(p, p.R(x1 - 106, by, x1 - 58, by + bh), L"+15", Act::DraftTime, 15, KDraft + 4, true, a, 0);
        TextButton(p, p.R(x1 - 52, by, x1, by + bh), L"+1 h", Act::DraftTime, 60, KDraft + 5, true, a, 0);
        p.Text(Clock(d.minutes), p.R(x0 + 110, y, x1 - 110, y + h), 32, WithAlpha(p.th.text, a), DWRITE_FONT_WEIGHT_SEMI_BOLD,
               DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Once on a date, or every week on chosen days
    Segmented(p, x0, 540, x1 - x0, 38, {L"Once on a date", L"Repeat on days"}, d.once ? 0 : 1, Act::DraftRepeat, KDraft + 8, a);
    if (d.once) {
        const bool canEarlier = d.date > schedule::Today(time(nullptr));
        RoundButton(p, p.R(x0, 594, x0 + 34, 628), art::Glyph::Minus, Act::DraftDate, -1, KDraft + 10, canEarlier, a);
        RoundButton(p, p.R(x1 - 34, 594, x1, 628), art::Glyph::Plus, Act::DraftDate, 1, KDraft + 11, true, a);
        p.Text(schedule::DateLabel(d.date), p.R(x0 + 40, 590, x1 - 40, 632), 15, WithAlpha(p.th.text, a), DWRITE_FONT_WEIGHT_SEMI_BOLD,
               DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        const wchar_t* dn[] = {L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun"};
        const float dg = 6, dw = (x1 - x0 - 6 * dg) / 7;
        for (int i = 0; i < 7; ++i) {
            const float x = x0 + static_cast<float>(i) * (dw + dg);
            const D2D1_RECT_F r = p.R(x, 590, x + dw, 632);
            const int key = KDraft + 12 + i;
            p.Add(r, Act::DraftDay, i, true, key);
            const bool on = ((d.days >> i) & 1) != 0;
            p.Fill(r, 8, WithAlpha(on ? p.th.accentSoft : Mix(p.th.control, p.th.controlHover, p.Hover(key, true)), a));
            if (on) p.Outline(r, 8, WithAlpha(p.th.accent, 0.9f * a));
            p.Text(dn[i], r, 12, WithAlpha(on ? p.th.text : p.th.sub, a), on ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    // A plain-language summary
    ScheduleEntry probe = d;
    probe.lastFiredDay = -1;
    const bool past = schedule::Missed(probe, time(nullptr));
    std::wstring summary = std::wstring(d.turnOn ? L"Turns the AC on" : L"Turns the AC off") + L" at " + Clock(d.minutes) + L", " +
                           (d.once ? schedule::DateLabel(d.date) : schedule::DaysLabel(d.days)) + L".";
    if (past) summary = L"That time has already passed. Pick a later time or date.";
    p.Text(summary, p.R(x0, 646, x1, 666), 12, WithAlpha(past ? p.th.danger : p.th.sub, a));

    // Actions, pinned to the bottom
    const float by = kContentBottom - 44;
    if (g_edit >= 0) TextButton(p, p.R(x0, by, x0 + 88, by + 34), L"Delete", Act::DraftDelete, 0, KDraft + 20, true, a, 2);
    TextButton(p, p.R(x1 - 196, by, x1 - 106, by + 34), L"Cancel", Act::DraftCancel, 0, KDraft + 21, true, a, 0);
    TextButton(p, p.R(x1 - 96, by, x1, by + 34), L"Save", Act::DraftSave, 0, KDraft + 22, !past && (d.once || d.days), a, 1);
}

void ScheduleTab(Painter& p, float a) {
    if (g_edit != -2) {
        ScheduleEditor(p, a);
        return;
    }
    const float x0 = kPad, x1 = kWidth - kPad;
    const time_t now = time(nullptr);

    // What happens next
    time_t when = 0;
    bool nextOn = false;
    std::wstring nextText = L"Nothing is scheduled";
    D2D1_COLOR_F nextColor = p.th.sub;
    if (schedule::Next(g_sched, now, when, nextOn)) {
        tm t{};
        localtime_s(&t, &when);
        nextText = std::wstring(nextOn ? L"Next: turns on at " : L"Next: turns off at ") + Clock(t.tm_hour * 60 + t.tm_min) + L", " +
                   schedule::DateLabel(schedule::Today(when));
        nextColor = p.th.accent;
    }
    p.Text(nextText, p.R(x0, 396, x1, 418), 13, WithAlpha(nextColor, a), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    DayBar(p, 420, a);

    // Sleep timer: the heading turns into the countdown while one runs.
    const long long left = g_sched.offAt ? static_cast<long long>(g_sched.offAt - now) : 0;
    if (left > 0) {
        wchar_t b[40];
        swprintf(b, 40, L"Sleep timer \u00B7 off in %lld:%02lld", left / 3600, (left / 60) % 60);
        p.Text(b, p.R(x0, 466, x1, 484), 12, WithAlpha(p.th.accent, a), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    } else {
        Heading(p, 466, L"Sleep timer", a);
    }
    const int mins[] = {0, 30, 60, 120, 240};
    const wchar_t* names[] = {left > 0 ? L"Cancel" : L"Off", L"30 min", L"1 hour", L"2 hours", L"4 hours"};
    const float cg = 6, cw = (x1 - x0 - 4 * cg) / 5;
    for (int i = 0; i < 5; ++i) {
        const float x = x0 + static_cast<float>(i) * (cw + cg);
        const D2D1_RECT_F r = p.R(x, 488, x + cw, 520);
        const int key = KSleep + i;
        p.Add(r, Act::SleepTimer, mins[i], true, key);
        const bool sel = i == 0 && left <= 0;
        p.Fill(r, 8, WithAlpha(p.ControlBg(sel, key, true), a));
        if (sel) p.Outline(r, 8, WithAlpha(p.th.accent, 0.9f * a));
        p.Text(names[i], r, 12, WithAlpha(p.th.text, a), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // The schedules
    Heading(p, 538, L"Schedules", a);
    TextButton(p, p.R(x1 - 84, 532, x1, 558), L"+  Add", Act::SchedAdd, 0, KAdd, true, a, 0);
    const int n = static_cast<int>(g_sched.entries.size());
    if (g_listTop > n - kVisibleRows) g_listTop = std::max(0, n - kVisibleRows);
    const float ly = 566, rowH = 42, pitch = 46;
    if (n == 0)
        p.Text(L"No schedules yet. Add one to turn the AC on or off at a set time.", p.R(x0, ly + 20, x1, ly + 60), 12,
               WithAlpha(p.th.sub, a), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (int row = 0; row < kVisibleRows && g_listTop + row < n; ++row) {
        const int i = g_listTop + row;
        const ScheduleEntry& e = g_sched.entries[static_cast<size_t>(i)];
        const float y = ly + static_cast<float>(row) * pitch;
        const int kb = KSched + row * 4;
        const bool missed = schedule::Missed(e, now);
        const D2D1_RECT_F rowR = p.R(x0, y, x1, y + rowH);
        p.Add(rowR, Act::SchedEdit, i, true, kb);
        p.Fill(rowR, 8, WithAlpha(Mix(p.th.control, p.th.controlHover, p.Hover(kb, true)), a));
        p.Add(p.R(x0 + 2, y, x0 + 50, y + rowH), Act::SchedToggle, i, true, kb + 1);
        p.Switch(p.R(x0 + 10, y + 12, x0 + 40, y + 30), e.enabled, true, a);
        const float ea = a * (e.enabled ? 1.0f : 0.55f);
        p.Text(Clock(e.minutes), p.R(x0 + 54, y, x0 + 128, y + rowH), 18, WithAlpha(p.th.text, ea), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        art::DrawGlyph(p.rt, art::Glyph::Power, p.R(x0 + 136, y + 6, x0 + 150, y + 20), WithAlpha(e.turnOn ? p.th.accent : p.th.sub, ea));
        p.Text(e.turnOn ? L"Turn on" : L"Turn off", p.R(x0 + 156, y + 3, x1 - 34, y + 23), 13, WithAlpha(p.th.text, ea));
        std::wstring whenText = EntryWhen(e);
        if (missed) whenText += L"  \u00B7  missed";
        else if (e.once && !e.enabled && e.lastFiredDay == e.date) whenText += L"  \u00B7  done";
        p.Text(whenText, p.R(x0 + 136, y + 22, x1 - 34, y + 39), 11.5f, WithAlpha(missed ? p.th.danger : p.th.sub, ea));
        const D2D1_RECT_F del = p.R(x1 - 30, y + 9, x1 - 8, y + 33);
        p.Add(del, Act::SchedDelete, i, true, kb + 2);
        if (p.Hover(kb + 2, true) > 0) p.Circle(del, WithAlpha(p.th.controlHover, a * p.Hover(kb + 2, true)));
        art::DrawGlyph(p.rt, art::Glyph::Close, p.R(x1 - 24, y + 15, x1 - 14, y + 27), WithAlpha(p.th.sub, a));
    }
    std::wstring foot = L"Timers run while AC Taskbar is running";
    if (n > kVisibleRows) {
        wchar_t b[64];
        swprintf(b, 64, L"%d\u2013%d of %d  \u00B7  scroll for more", g_listTop + 1, std::min(n, g_listTop + kVisibleRows), n);
        foot = b;
    }
    p.Text(foot, p.R(x0, 746, x1, 760), 11, WithAlpha(p.th.sub, a));
}

// Power over the last 10 minutes: filled area with a line and a baseline.
void PowerChart(Painter& p, const D2D1_RECT_F& r, float a) {
    p.br->SetColor(WithAlpha(p.th.divider, a));
    p.rt->FillRectangle(D2D1::RectF(r.left, r.bottom - 1, r.right, r.bottom), p.br);
    if (g_watts.size() < 2) {
        p.Text(L"Collecting readings…", r, 11.5f, WithAlpha(p.th.sub, a), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    const double t1 = Now(), t0 = t1 - 600;
    float maxW = 100;
    for (auto& [t, w] : g_watts) maxW = std::fmax(maxW, w);
    maxW *= 1.1f;
    ID2D1Factory* f = nullptr;
    p.rt->GetFactory(&f);
    auto build = [&](bool closed) {
        ID2D1PathGeometry* g = nullptr;
        f->CreatePathGeometry(&g);
        ID2D1GeometrySink* k = nullptr;
        g->Open(&k);
        bool first = true;
        float lastX = r.left;
        for (auto& [t, w] : g_watts) {
            const float x = r.left + static_cast<float>(std::fmax(0.0, (t - t0) / 600.0)) * (r.right - r.left);
            const float y = r.bottom - 1 - (w / maxW) * (r.bottom - r.top - 1);
            if (first) {
                k->BeginFigure(closed ? D2D1::Point2F(x, r.bottom) : D2D1::Point2F(x, y), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                if (closed) k->AddLine(D2D1::Point2F(x, y));
                first = false;
            } else {
                k->AddLine(D2D1::Point2F(x, y));
            }
            lastX = x;
        }
        if (closed) k->AddLine(D2D1::Point2F(lastX, r.bottom));
        k->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        k->Close();
        gfx::SafeRelease(k);
        return g;
    };
    ID2D1PathGeometry* area = build(true);
    ID2D1PathGeometry* line = build(false);
    p.br->SetColor(WithAlpha(p.th.accent, 0.18f * a));
    p.rt->FillGeometry(area, p.br);
    p.br->SetColor(WithAlpha(p.th.accent, a));
    ID2D1StrokeStyle* st = nullptr;
    f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &st);
    p.rt->DrawGeometry(line, p.br, 1.5f * p.s, st);
    gfx::SafeRelease(st);
    gfx::SafeRelease(area);
    gfx::SafeRelease(line);
    gfx::SafeRelease(f);
}

void DetailsTab(Painter& p, float a) {
    const auto& d = g_snap.state;
    const bool online = Online();
    const float x0 = kPad, x1 = kWidth - kPad;
    const float ca = a * (online ? 1.0f : 0.6f);

    // ---- header: when it was last read, and the two actions
    const std::wstring read = L"Last read " + Wide(g_snap.updatedAt.empty() ? std::string("--") : g_snap.updatedAt);
    const bool cleanOk = online && g_snap.caps.selfClean;
    if (g_confirmClean) {
        p.Text(L"Self-clean takes a while; no cooling or heating meanwhile.", p.R(x0, 396, x1 - 104, 420), 11.5f, WithAlpha(p.th.text, a));
        TextButton(p, p.R(x1 - 96, 396, x1, 422), L"Start clean", Act::SelfClean, 0, KClean, cleanOk, a, 2);
    } else {
        p.Text(read, p.R(x0, 396, x1 - 170, 422), 11.5f, WithAlpha(p.th.sub, a));
        TextButton(p, p.R(x1 - 164, 396, x1 - 92, 422), L"Refresh", Act::Refresh, 0, KRefresh, true, a, 0);
        TextButton(p, p.R(x1 - 84, 396, x1, 422), L"Self-clean", Act::SelfClean, 0, KClean, cleanOk, a, 0);
    }

    // ---- power: the live draw, and the last ten minutes of it
    {
        const D2D1_RECT_F card = p.R(x0, 428, x1, 524);
        p.Fill(card, 10, WithAlpha(p.th.card, a));
        art::DrawGlyph(p.rt, art::Glyph::Bolt, p.R(x0 + 14, 440, x0 + 30, 456), WithAlpha(p.th.accent, ca));
        p.Text(L"Power", p.R(x0 + 36, 438, x0 + 130, 458), 12, WithAlpha(p.th.sub, ca));
        p.Text(d.outdoorUnitPower ? Watts(static_cast<float>(*d.outdoorUnitPower)) : L"\u2014", p.R(x0 + 14, 458, x0 + 140, 492), 26,
               WithAlpha(p.th.text, ca), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float peak = 0;
        for (auto& [t, w] : g_watts) peak = std::fmax(peak, w);
        p.Text(peak > 0 ? L"peak " + Watts(peak) : L"", p.R(x0 + 14, 494, x0 + 140, 512), 11.5f, WithAlpha(p.th.sub, ca));
        PowerChart(p, p.R(x0 + 150, 440, x1 - 14, 496), ca);
        p.Text(L"10 min ago", p.R(x0 + 150, 502, x0 + 230, 518), 10.5f, WithAlpha(p.th.sub, ca));
        p.Text(L"now", p.R(x1 - 94, 502, x1 - 14, 518), 10.5f, WithAlpha(p.th.sub, ca), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // ---- three ring gauges
    {
        struct Gauge { const wchar_t* name; const wchar_t* unit; std::optional<float> value; float lo, hi; std::optional<float> mark; };
        const Gauge gauges[] = {
            {L"Compressor", L"Hz", d.compressorFrequency ? std::optional<float>(static_cast<float>(*d.compressorFrequency)) : std::nullopt, 0, 100,
             d.targetCompressorFrequency && *d.targetCompressorFrequency > 0 ? std::optional<float>(static_cast<float>(*d.targetCompressorFrequency)) : std::nullopt},
            {L"Current", L"A", d.compressorCurrent ? std::optional<float>(static_cast<float>(*d.compressorCurrent)) : std::nullopt, 0, 12, std::nullopt},
            {L"Mains", L"V", d.compressorVoltage ? std::optional<float>(static_cast<float>(*d.compressorVoltage)) : std::nullopt, 190, 250, std::nullopt},
        };
        const float gw = (x1 - x0 - 16) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const Gauge& g = gauges[i];
            const float x = x0 + static_cast<float>(i) * (gw + 8), cx = x + gw / 2, cy = 570;
            p.Fill(p.R(x, 532, x + gw, 616), 10, WithAlpha(p.th.card, a));
            const float frac = g.value ? (*g.value - g.lo) / (g.hi - g.lo) : 0.0f;
            Ring(p, cx, cy, 20, frac, WithAlpha(p.th.control, a), WithAlpha(p.th.accent, ca), 5);
            if (g.mark) {  // where the compressor is heading
                const float ang = -1.5707963f + 2 * 3.14159265f * std::fmax(0.0f, std::fmin(1.0f, (*g.mark - g.lo) / (g.hi - g.lo)));
                p.Circle(p.R(cx + 20 * std::cos(ang) - 2.5f, cy + 20 * std::sin(ang) - 2.5f, cx + 20 * std::cos(ang) + 2.5f, cy + 20 * std::sin(ang) + 2.5f),
                         WithAlpha(p.th.text, ca));
            }
            wchar_t vb[16];
            if (g.value) swprintf(vb, 16, L"%d", static_cast<int>(std::lround(*g.value)));
            else swprintf(vb, 16, L"\u2014");
            p.Text(vb, p.R(cx - 18, cy - 12, cx + 18, cy + 3), 13.5f, WithAlpha(p.th.text, ca), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            p.Text(g.unit, p.R(cx - 18, cy + 2, cx + 18, cy + 14), 9.5f, WithAlpha(p.th.sub, ca), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            p.Text(g.name, p.R(x, 596, x + gw, 614), 11.5f, WithAlpha(p.th.sub, ca), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    // ---- temperatures, all on one shared -10..80 degree axis
    Heading(p, 624, L"Temperatures", a);
    p.Text(L"-10\u00B0 to 80\u00B0", p.R(x1 - 120, 624, x1, 642), 10.5f, WithAlpha(p.th.sub, a), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    {
        struct Row { art::Glyph g; const wchar_t* name; std::optional<double> v; };
        const Row rows[] = {
            {art::Glyph::Thermometer, L"Room", d.indoorTemperature},
            {art::Glyph::Outdoor, L"Outside", d.outdoorTemperature},
            {art::Glyph::Thermometer, L"Indoor coil", d.indoorCoil},
            {art::Glyph::Outdoor, L"Outdoor coil", d.outdoorCoil},
            {art::Glyph::ModeHeat, L"Discharge pipe", d.dischargePipe ? std::optional<double>(static_cast<double>(*d.dischargePipe)) : std::nullopt},
        };
        const float bx0 = x0 + 112, bx1 = x1 - 52;
        for (int i = 0; i < 5; ++i) {
            const float y = 646.0f + static_cast<float>(i) * 18.5f;
            p.br->SetColor(WithAlpha(p.th.sub, ca));
            art::DrawGlyph(p.rt, rows[i].g, p.R(x0, y + 1, x0 + 14, y + 15), WithAlpha(p.th.sub, ca));
            p.Text(rows[i].name, p.R(x0 + 20, y, bx0 - 6, y + 17), 12, WithAlpha(p.th.sub, ca));
            p.Fill(p.R(bx0, y + 6, bx1, y + 11), 2.5f, WithAlpha(p.th.control, a));
            const float zero = bx0 + (bx1 - bx0) * (10.0f / 90.0f);  // 0 degrees: where water freezes
            p.br->SetColor(WithAlpha(p.th.sub, 0.5f * a));
            p.rt->FillRectangle(p.R(zero - 0.5f, y + 4, zero + 0.5f, y + 13), p.br);
            if (rows[i].v) {
                const float frac = std::fmax(0.0f, std::fmin(1.0f, (static_cast<float>(*rows[i].v) + 10.0f) / 90.0f));
                p.Fill(p.R(bx0, y + 6, bx0 + std::fmax(5.0f, (bx1 - bx0) * frac), y + 11), 2.5f, WithAlpha(p.th.accent, ca));
                wchar_t vb[24];
                swprintf(vb, 24, L"%.1f\u00B0", *rows[i].v);
                p.Text(vb, p.R(bx1 + 8, y, x1, y + 17), 12.5f, WithAlpha(p.th.text, ca), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
            } else {
                p.Text(L"\u2014", p.R(bx1 + 8, y, x1, y + 17), 12.5f, WithAlpha(p.th.sub, ca), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
        }
    }

    // ---- status chips: defrost, fans, and the last code the unit reported
    {
        float x = x0;
        const float y0 = 740, y1 = 760;
        auto chip = [&](art::Glyph g, const std::wstring& text, bool hot) {
            const float tw = gfx::TextWidth(text, gfx::Font(11.5f * p.s)) / p.s, w = 8 + 12 + 5 + tw + 9;
            p.Fill(p.R(x, y0, x + w, y1), 10, WithAlpha(hot ? p.th.accentSoft : p.th.control, a));
            art::DrawGlyph(p.rt, g, p.R(x + 8, y0 + 4, x + 20, y0 + 16), WithAlpha(hot ? p.th.accent : p.th.sub, ca));
            p.Text(text, p.R(x + 25, y0, x + w, y1), 11.5f, WithAlpha(p.th.text, ca));
            x += w + 6;
        };
        chip(art::Glyph::ModeCool, d.defrost ? L"Defrosting" : L"No defrost", d.defrost);
        std::wstring fans = L"Fans " + (d.indoorFan ? std::to_wstring(*d.indoorFan) : std::wstring(L"\u2014")) + L" \u00B7 " +
                            (d.outdoorFan ? std::to_wstring(*d.outdoorFan) : std::wstring(L"\u2014"));
        chip(art::Glyph::ModeFan, fans, false);
        chip(art::Glyph::Info, g_snap.lastUnitCode ? L"Code " + std::to_wstring(g_snap.lastUnitCode) + L" at " + Wide(g_snap.lastUnitCodeAt) : L"No unit code",
             g_snap.lastUnitCode != 0);
    }
}

void DrawTab(Painter& p, int tab, float a) {
    switch (tab) {
        case TabControls: ControlsTab(p, a); break;
        case TabSchedule: ScheduleTab(p, a); break;
        case TabDetails: DetailsTab(p, a); break;
        default: break;
    }
}

void Paint() {
    if (!g_wnd) return;
    RECT rc;
    GetClientRect(g_wnd, &rc);
    if (!g_rt) {
        const auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        gfx::D2D()->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_wnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt);
        if (!g_rt) return;
    }
    const int moving = Step();
    g_rt->BeginDraw();
    {
        Painter p(g_rt);
        g_rt->Clear(g_translucent ? D2D1::ColorF(0, 0, 0, 0) : p.th.bg);  // transparent: the backdrop shows
        g_hits.clear();
        SceneCard(p);
        TemperatureRow(p);
        ModeRow(p);
        Tabs(p);
        if (g_tabMix < 1 && g_prevTab != g_tab) {  // the previous tab fades out underneath, without hits
            const size_t mark = g_hits.size();
            DrawTab(p, g_prevTab, 1 - g_tabMix);
            g_hits.resize(mark);
        }
        DrawTab(p, g_tab, g_tabMix < 1 ? g_tabMix : 1.0f);
    }
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) gfx::SafeRelease(g_rt);
    if (moving) SetTimer(g_wnd, kAnimTimer, moving == 2 ? (kWidget ? 33 : 16) : 1000, nullptr);
    else KillTimer(g_wnd, kAnimTimer);
}

void Place() {
    HMONITOR mon = MonitorFromRect(&g_anchor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT wa = mi.rcWork;
    // A work area shorter than the panel (a small laptop screen): scale the panel down so it fits.
    const float dpi = DpiScale();
    g_fit = std::fmin(1.0f, (static_cast<float>(wa.bottom - wa.top) - 24.0f * dpi) / (kHeight * dpi));
    const float s = Scale();
    const int gap = static_cast<int>(12 * s);
    const int w = static_cast<int>(kWidth * s), h = static_cast<int>(kHeight * s);
    // Line the panel up with the strip: from its left edge when the strip is in the left half of the
    // screen, from its right edge otherwise; then keep it on screen.
    const bool leftHalf = (g_anchor.left + g_anchor.right) / 2 < (wa.left + wa.right) / 2;
    int x = leftHalf ? g_anchor.left : g_anchor.right - w;
    if (x + w > wa.right - gap) x = wa.right - gap - w;
    if (x < wa.left + gap) x = wa.left + gap;
    const int y = wa.bottom - gap - h;
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    if (g_rt) g_rt->Resize(D2D1::SizeU(w, h));
}

constexpr float kWidgetScales[flyout::kWidgetSizes] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f};

BOOL CALLBACK CollectMonitor(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(m);
    return TRUE;
}

// Where a widget nobody has placed goes: the second screen if there is one, else the main screen.
HMONITOR DefaultMonitor() {
    std::vector<HMONITOR> all;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&all));
    for (HMONITOR m : all) {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(m, &mi) && !(mi.dwFlags & MONITORINFOF_PRIMARY)) return m;
    }
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

float MonitorScale(HMONITOR mon) {
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    return static_cast<float>(dx) / 96.0f;
}

// Sizes the widget (the whole panel scales with it) and puts it where the user left it, inside the work
// area of its monitor. A widget nobody has placed goes to the bottom right of the second screen.
void PlaceWidget() {
    if (g_placing || !g_wnd) return;  // moving between monitors sends WM_DPICHANGED: not again from here
    g_placing = true;
    const float user = kWidgetScales[std::clamp(g_opts.size, 0, flyout::kWidgetSizes - 1)];
    bool fresh = !g_opts.hasPosition;
    int x = g_opts.x, y = g_opts.y;
    const bool inPlace = !fresh && g_placed && IsWindowVisible(g_wnd);
    if (inPlace) {  // already on screen: grow or shrink where it is
        RECT r;
        GetWindowRect(g_wnd, &r);
        x = r.left;
        y = r.top;
    }
    HMONITOR mon = nullptr;
    if (!fresh) {
        const RECT probe{x, y, x + 10, y + 10};
        mon = MonitorFromRect(&probe, MONITOR_DEFAULTTONULL);
        if (!mon) fresh = true;  // its monitor is gone
    }
    if (fresh) mon = DefaultMonitor();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const int left = static_cast<int>(mi.rcWork.left), top = static_cast<int>(mi.rcWork.top);
    const int right = static_cast<int>(mi.rcWork.right), bottom = static_cast<int>(mi.rcWork.bottom);
    // On screen, the window's own DPI is the truth (it changes as it is dragged between monitors). The widget
    // never outgrows its screen: a 200 % widget on a small display shrinks to fit.
    const float wanted = (inPlace && !fresh ? DpiScale() : MonitorScale(mon)) * user;
    const float cap = std::fmin(static_cast<float>(right - left) / kWidth, static_cast<float>(bottom - top) / kHeight) * 0.97f;
    g_widgetScale = std::fmin(wanted, cap);
    const int w = static_cast<int>(std::lround(kWidth * g_widgetScale)), h = static_cast<int>(std::lround(kHeight * g_widgetScale));
    if (fresh) {
        const int gap = static_cast<int>(std::lround(24 * g_widgetScale));
        x = right - w - gap;
        y = bottom - h - gap;
    }
    x = std::clamp(x, left, std::max(left, right - w));
    y = std::clamp(y, top, std::max(top, bottom - h));
    SetWindowPos(g_wnd, g_opts.onTop ? HWND_TOPMOST : HWND_NOTOPMOST, x, y, w, h, SWP_NOACTIVATE);
    if (g_rt) g_rt->Resize(D2D1::SizeU(static_cast<UINT>(w), static_cast<UINT>(h)));
    g_placed = true;
    g_placing = false;
    if (fresh && g_opts.moved) g_opts.moved(x, y);
}

int HitAt(int x, int y) {
    for (int i = static_cast<int>(g_hits.size()) - 1; i >= 0; --i) {
        const auto& r = g_hits[i].r;
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i;
    }
    return -1;
}

std::string Num(double v) {
    char b[16];
    snprintf(b, sizeof(b), "%g", v);
    return b;
}

int SliderValue(int x) {
    const float t = (x - g_sliderTrack.left) / (g_sliderTrack.right - g_sliderTrack.left);
    return static_cast<int>(std::lround(1 + 99 * std::fmin(1.f, std::fmax(0.f, t))));
}

void Send(const Changes& c) {
    if (g_cb.change) g_cb.change(c);
}

void SendSchedule() {
    if (g_cb.schedule) g_cb.schedule(g_sched);
}

void Activate(const Hit& h) {
    if (!h.enabled) return;
    const auto& st = g_snap.state;
    if (h.act != Act::SelfClean) g_confirmClean = false;
    switch (h.act) {
        case Act::Power: Send({{"power", st.power ? "false" : "true"}}); break;
        case Act::TempDown: Send({{"target_temperature", Num(std::fmax(g_snap.caps.minTemperature, st.targetTemperature - 0.5))}}); break;
        case Act::TempUp: Send({{"target_temperature", Num(std::fmin(g_snap.caps.maxTemperature, st.targetTemperature + 0.5))}}); break;
        case Act::Mode: {
            const char* names[] = {"", "auto", "cool", "dry", "heat", "fan"};
            Send({{"mode", names[h.arg]}});
            break;
        }
        case Act::Tab:
            if (h.arg != g_tab) { g_prevTab = g_tab; g_tab = h.arg; g_tabMix = 0; g_edit = -2; }
            break;
        case Act::Fan: {
            const char* name = h.arg == 20 ? "silent" : h.arg == 40 ? "low" : h.arg == 60 ? "medium" : h.arg == 80 ? "high"
                             : h.arg == 100 ? "max" : "auto";
            Changes c = {{"fan_speed", name}};
            if (st.breezeAway) c.insert(c.begin(), {"breeze_away", "false"});
            Send(c);
            break;
        }
        case Act::SwingV: case Act::SwingH: {
            const bool v = (st.swing & 0xC) == 0xC, hz = (st.swing & 0x3) == 0x3;
            const bool nv = h.act == Act::SwingV ? !v : v, nh = h.act == Act::SwingH ? !hz : hz;
            Send({{"swing", nv && nh ? "both" : nv ? "vertical" : nh ? "horizontal" : "off"}});
            break;
        }
        case Act::VPos: Send({{"vertical_angle", std::to_string(h.arg)}}); break;
        case Act::HPos: Send({{"horizontal_angle", std::to_string(h.arg)}}); break;
        case Act::Feature: {
            const auto& f = kFeatures[h.arg];
            Send({{f.key, st.*(f.field) ? "false" : "true"}});
            break;
        }
        case Act::Refresh: if (g_cb.refresh) g_cb.refresh(); break;
        case Act::SelfClean:
            if (g_confirmClean) { Send({{"self_clean", "start"}}); g_confirmClean = false; }
            else g_confirmClean = true;
            break;
        case Act::SleepTimer:
            g_sched.offAt = h.arg ? time(nullptr) + h.arg * 60 : 0;
            SendSchedule();
            break;
        case Act::SchedToggle:
            if (h.arg < static_cast<int>(g_sched.entries.size())) {
                auto& e = g_sched.entries[h.arg];
                e.enabled = !e.enabled;
                if (e.enabled && schedule::Missed(e, time(nullptr))) e.enabled = false;  // a past one-time entry stays off
                SendSchedule();
            }
            break;
        case Act::SchedDelete:
            if (h.arg < static_cast<int>(g_sched.entries.size())) {
                g_sched.entries.erase(g_sched.entries.begin() + h.arg);
                SendSchedule();
            }
            break;
        case Act::SchedEdit:
            if (h.arg < static_cast<int>(g_sched.entries.size())) {
                g_edit = h.arg;
                g_draft = g_sched.entries[h.arg];
                if (!g_draft.date) g_draft.date = schedule::Today(time(nullptr));
            }
            break;
        case Act::SchedAdd: {
            // Default: the next whole hour, once, today (tomorrow if it is already past 23:00).
            const time_t now = time(nullptr);
            tm t{};
            localtime_s(&t, &now);
            g_draft = ScheduleEntry{};
            g_draft.turnOn = !g_snap.state.power;
            g_draft.minutes = ((t.tm_hour + 1) % 24) * 60;
            g_draft.once = true;
            g_draft.date = schedule::Today(now);
            if (t.tm_hour == 23) g_draft.date = schedule::AddDays(g_draft.date, 1);
            g_draft.days = 0x1F;
            g_edit = -1;
            break;
        }
        case Act::DraftAction: g_draft.turnOn = h.arg == 0; break;
        case Act::DraftTime: g_draft.minutes = ((g_draft.minutes + h.arg) % 1440 + 1440) % 1440; break;
        case Act::DraftRepeat:
            g_draft.once = h.arg == 0;
            if (!g_draft.date) g_draft.date = schedule::Today(time(nullptr));
            break;
        case Act::DraftDate: {
            const int today = schedule::Today(time(nullptr));
            const int d = schedule::AddDays(g_draft.date ? g_draft.date : today, h.arg);
            if (d >= today) g_draft.date = d;
            break;
        }
        case Act::DraftDay: {
            const uint8_t bit = static_cast<uint8_t>(1 << h.arg);
            if ((g_draft.days & ~bit) != 0 || !(g_draft.days & bit)) g_draft.days ^= bit;  // keep at least one day
            break;
        }
        case Act::DraftSave: {
            ScheduleEntry e = g_draft;
            e.enabled = true;
            e.lastFiredDay = -1;  // edited: it may run again
            if (g_edit >= 0 && g_edit < static_cast<int>(g_sched.entries.size())) g_sched.entries[g_edit] = e;
            else g_sched.entries.push_back(e);
            // Keep the list in time order: one-time entries by date and time first, repeating ones by time.
            std::stable_sort(g_sched.entries.begin(), g_sched.entries.end(), [](const ScheduleEntry& x, const ScheduleEntry& y) {
                const long long kx = (x.once ? static_cast<long long>(x.date) : 0) * 1440 + x.minutes;
                const long long ky = (y.once ? static_cast<long long>(y.date) : 0) * 1440 + y.minutes;
                return kx < ky;
            });
            g_edit = -2;
            SendSchedule();
            break;
        }
        case Act::DraftCancel: g_edit = -2; break;
        case Act::DraftDelete:
            if (g_edit >= 0 && g_edit < static_cast<int>(g_sched.entries.size())) {
                g_sched.entries.erase(g_sched.entries.begin() + g_edit);
                SendSchedule();
            }
            g_edit = -2;
            break;
        default: break;
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void SetHover(int key) {
    if (key == g_hoverKey) return;
    g_hoverKey = key;
    g_hoverStart = Now();
    if (key >= 0) g_hoverAnim.try_emplace(key, 0.0f);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            Paint();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kAnimTimer) InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_ACTIVATE:
            if (!kWidget && LOWORD(wp) == WA_INACTIVE) { g_closedAt = GetTickCount64(); Hide(); }
            return 0;
        case WM_MOUSEACTIVATE: return kWidget ? MA_NOACTIVATE : MA_ACTIVATE;
        case WM_NCHITTEST:  // the widget is dragged by anything that is not a control
            if (kWidget && !g_opts.locked) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(h, &pt);
                RECT rc;
                GetClientRect(h, &rc);
                if (PtInRect(&rc, pt)) return HitAt(pt.x, pt.y) < 0 ? HTCAPTION : HTCLIENT;
            }
            break;
        case WM_EXITSIZEMOVE:  // dragged: remember where it ended up
            if (kWidget && g_opts.moved) {
                RECT r;
                GetWindowRect(h, &r);
                g_opts.moved(static_cast<int>(r.left), static_cast<int>(r.top));
            }
            return 0;
        case WM_NCRBUTTONUP: case WM_RBUTTONUP:
            if (kWidget) {
                if (g_opts.menu) g_opts.menu();
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (g_dragging) {
                g_dragValue = SliderValue(x);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            const int hit = HitAt(x, y);
            const int key = hit >= 0 && g_hits[hit].enabled ? g_hits[hit].key : -1;
            if (key != g_hoverKey) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                SetHover(key);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursor(nullptr, g_hoverKey >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSELEAVE: SetHover(-1); return 0;
        case WM_LBUTTONDOWN: {
            const int hit = HitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit >= 0 && g_hits[hit].act == Act::FanSlider && g_hits[hit].enabled) {
                g_dragging = true;
                g_downX = GET_X_LPARAM(lp);
                g_dragValue = SliderValue(GET_X_LPARAM(lp));
                SetCapture(h);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_dragging) {
                g_dragging = false;
                ReleaseCapture();
                const int x = GET_X_LPARAM(lp);
                if (std::abs(x - g_downX) < static_cast<int>(4 * Scale())) {  // a click: the preset bar under the pointer
                    const float bw = (g_sliderTrack.right - g_sliderTrack.left) / 5.0f;
                    const int idx = std::max(0, std::min(4, static_cast<int>((static_cast<float>(x) - g_sliderTrack.left) / bw)));
                    const int speeds[] = {20, 40, 60, 80, 100};
                    Hit preset{};
                    preset.act = Act::Fan;
                    preset.arg = speeds[idx];
                    preset.enabled = true;
                    Activate(preset);
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
                Changes c = {{"fan_speed", std::to_string(g_dragValue)}};
                if (g_snap.state.breezeAway) c.insert(c.begin(), {"breeze_away", "false"});
                Send(c);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            const int hit = HitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit >= 0) Activate(g_hits[hit]);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (g_tab == TabSchedule && g_edit == -2) {
                const int n = static_cast<int>(g_sched.entries.size());
                g_listTop += GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
                g_listTop = std::max(0, std::min(g_listTop, std::max(0, n - kVisibleRows)));
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN: {
            if (wp == VK_ESCAPE) {
                if (g_edit != -2) { g_edit = -2; InvalidateRect(h, nullptr, FALSE); }  // Esc closes the editor first
                else Hide();
                return 0;
            }
            const auto& st = g_snap.state;
            if ((wp == VK_UP || wp == VK_DOWN) && Online() && st.mode != 5) {
                const double t = st.targetTemperature + (wp == VK_UP ? 0.5 : -0.5);
                if (t >= g_snap.caps.minTemperature && t <= g_snap.caps.maxTemperature) Send({{"target_temperature", Num(t)}});
            } else if (wp == VK_TAB) {
                g_prevTab = g_tab;
                g_tab = (g_tab + ((GetKeyState(VK_SHIFT) & 0x8000) ? TabCount - 1 : 1)) % TabCount;
                g_tabMix = 0;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_DPICHANGED:
            if (kWidget) PlaceWidget();
            else Place();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SETTINGCHANGE: case WM_DWMCOLORIZATIONCOLORCHANGED:
            ApplyBackdrop();  // light/dark may have changed
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_DESTROY: gfx::SafeRelease(g_rt); return 0;
        default: break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void ApplyBackdrop() {
    if (!g_wnd) return;
    if (kWidget) {
        // A desktop widget is never the active window, where Windows paints Mica and Acrylic as flat grey:
        // use the glass blur (clear or frosted), or an opaque fill.
        const Backdrop b = g_opts.background == 1 ? Backdrop::Frosted : g_opts.background == 2 ? Backdrop::Solid : Backdrop::Clear;
        g_translucent = b != Backdrop::Solid;
        backdrop::Apply(g_wnd, gfx::AppsUseLightTheme(), b);
        return;
    }
    if (g_translucent) {
        backdrop::Apply(g_wnd, gfx::AppsUseLightTheme(), g_backdrop);
    } else {  // before Windows 11 22H2: an opaque panel with rounded corners
        const BOOL dark = !gfx::AppsUseLightTheme();
        DwmSetWindowAttribute(g_wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        const DWORD round = 2;  // DWMWCP_ROUND
        DwmSetWindowAttribute(g_wnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    }
}

void RecordWatts() {
    if (!g_snap.state.outdoorUnitPower || g_snap.status != Snapshot::Status::Online) return;
    const double t = Now();
    if (!g_watts.empty() && t - g_watts.back().first < 2) return;
    g_watts.emplace_back(t, static_cast<float>(*g_snap.state.outdoorUnitPower));
    while (!g_watts.empty() && t - g_watts.front().first > 600) g_watts.pop_front();
}

}  // namespace

void Show(HINSTANCE inst, const RECT& anchor, const Snapshot& snap, bool pending, const Schedule& sched, Callbacks cb,
          const flyout::WidgetOptions& opts) {
    g_anchor = anchor;
    g_snap = snap;
    g_sched = sched;
    g_pending = pending;
    g_cb = std::move(cb);
    g_opts = opts;
    g_confirmClean = false;
    RecordWatts();
    if (!g_wnd) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = VIEW_CLASS;
        RegisterClassW(&wc);
        const DWORD ex = kWidget ? (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (opts.onTop ? WS_EX_TOPMOST : 0))
                                 : (WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
        g_wnd = CreateWindowExW(ex, VIEW_CLASS, L"Air conditioner", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
        if (!g_wnd) return;
        if (!kWidget) g_translucent = backdrop::Supported(g_wnd);
    }
    g_animInit = false;  // start from the current state, no catch-up animation
    ApplyBackdrop();
    if (kWidget) {
        g_placed = false;
        PlaceWidget();
        ShowWindow(g_wnd, SW_SHOWNOACTIVATE);  // it never takes focus
        ApplyBackdrop();                       // again once visible: DWM can skip a backdrop set on a hidden window
    } else {
        Place();
        ShowWindow(g_wnd, SW_SHOW);
        ApplyBackdrop();
        SetForegroundWindow(g_wnd);
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void Update(const Snapshot& snap, bool pending, const Schedule& sched, const flyout::WidgetOptions& opts) {
    g_snap = snap;
    g_sched = sched;
    g_pending = pending;
    RecordWatts();
    if (kWidget && g_wnd) {
        const flyout::WidgetOptions old = g_opts;
        g_opts = opts;
        if (IsWindowVisible(g_wnd)) {
            if (opts.background != old.background) ApplyBackdrop();
            const bool reset = !opts.hasPosition && old.hasPosition;  // "Reset position": back to the default place
            if (opts.size != old.size || opts.onTop != old.onTop || reset) {
                if (reset) g_placed = false;
                PlaceWidget();
            }
        }
    }
    if (Visible()) InvalidateRect(g_wnd, nullptr, FALSE);
}

void Hide() {
    if (g_wnd && IsWindowVisible(g_wnd)) ShowWindow(g_wnd, SW_HIDE);
    if (g_wnd) KillTimer(g_wnd, kAnimTimer);
    g_dragging = false;
    g_hoverKey = -1;
    g_hoverAnim.clear();
}

bool Visible() { return g_wnd && IsWindowVisible(g_wnd); }

void SetBackdrop(Backdrop b) {
    g_backdrop = b;
    ApplyBackdrop();
    if (Visible()) InvalidateRect(g_wnd, nullptr, FALSE);
}

bool JustClosed() { return GetTickCount64() - g_closedAt < 300; }

void SeedWatts(const std::vector<float>& watts, double spacingSeconds) {
    g_watts.clear();
    const double now = Now();
    for (size_t i = 0; i < watts.size(); ++i)
        g_watts.emplace_back(now - static_cast<double>(watts.size() - 1 - i) * spacingSeconds, watts[i]);
}

}  // namespace VIEW_NS
