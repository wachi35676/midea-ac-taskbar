#include "settings.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <cmath>
#include <vector>

#include "art.h"
#include "backdrop.h"
#include "gfx.h"

namespace settings {
namespace {

constexpr float kW = 600, kH = 800;  // client size in DIPs
constexpr float kPad = 28, kCardH = 68, kCardGap = 4;

HWND g_wnd = nullptr;
ID2D1HwndRenderTarget* g_rt = nullptr;
Provider g_provider;
Handler g_handler;
State g_state;
bool g_translucent = false;
POINT g_mouse{-1, -1};
bool g_enabled = true;               // false: the cards being drawn are dimmed and do not react
float g_scroll = 0, g_contentH = 0;  // the content scrolls when it is taller than the window (DIPs)

struct Hit {
    D2D1_RECT_F r;
    Action action;
    int value;
};
std::vector<Hit> g_hits;

float Scale() { return g_wnd ? GetDpiForWindow(g_wnd) / 96.0f : 1.0f; }

struct Palette {
    bool light;
    D2D1_COLOR_F bg, card, control, controlHover, text, sub, accent, onAccent, danger, stroke;
};
Palette Colors() {
    Palette c;
    c.light = gfx::AppsUseLightTheme();
    c.bg = c.light ? gfx::Rgb(0xF3F3F3) : gfx::Rgb(0x202020);
    if (g_translucent) {
        c.card = c.light ? gfx::Rgb(0xFFFFFF, 0.7f) : gfx::Rgb(0xFFFFFF, 0.05f);
        c.control = c.light ? gfx::Rgb(0xFFFFFF, 0.7f) : gfx::Rgb(0xFFFFFF, 0.06f);
        c.controlHover = c.light ? gfx::Rgb(0xF9F9F9, 0.5f) : gfx::Rgb(0xFFFFFF, 0.09f);
    } else {
        c.card = c.light ? gfx::Rgb(0xFFFFFF) : gfx::Rgb(0x2B2B2B);
        c.control = c.light ? gfx::Rgb(0xF5F5F5) : gfx::Rgb(0x373737);
        c.controlHover = c.light ? gfx::Rgb(0xEBEBEB) : gfx::Rgb(0x404040);
    }
    c.stroke = c.light ? gfx::Rgb(0x000000, 0.06f) : gfx::Rgb(0x000000, 0.25f);
    c.text = c.light ? gfx::Rgb(0x1A1A1A) : gfx::Rgb(0xFFFFFF);
    c.sub = c.light ? gfx::Rgb(0x5F5F5F) : gfx::Rgb(0xC5C5C5);
    c.accent = gfx::Accent(c.light);
    const float lum = 0.2126f * c.accent.r + 0.7152f * c.accent.g + 0.0722f * c.accent.b;
    c.onAccent = lum > 0.55f ? gfx::Rgb(0x000000) : gfx::Rgb(0xFFFFFF);
    c.danger = gfx::Rgb(0xC42B1C);
    return c;
}

struct Painter {
    ID2D1RenderTarget* rt;
    ID2D1SolidColorBrush* br = nullptr;
    Palette c;
    float s;
    float dim = 1.0f;  // multiplies every alpha: dimmed (disabled) cards
    explicit Painter(ID2D1RenderTarget* r) : rt(r), c(Colors()), s(Scale()) { rt->CreateSolidColorBrush(c.text, &br); }
    ~Painter() { gfx::SafeRelease(br); }
    D2D1_RECT_F R(float l, float t, float r, float b) const { return D2D1::RectF(l * s, t * s, r * s, b * s); }
    void Fill(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F col) {
        col.a *= dim;
        br->SetColor(col);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius * s, radius * s), br);
    }
    void Stroke(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F col) {
        col.a *= dim;
        br->SetColor(col);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f),
                                                   radius * s, radius * s), br, 1.0f);
    }
    void Text(const std::wstring& t, const D2D1_RECT_F& r, float size, D2D1_COLOR_F col,
              DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING) {
        col.a *= dim;
        br->SetColor(col);
        rt->DrawText(t.c_str(), static_cast<UINT32>(t.size()), gfx::Font(size * s, w, a), r, br);
    }
    bool Hover(const D2D1_RECT_F& r) const {
        return g_mouse.x >= r.left && g_mouse.x <= r.right && g_mouse.y >= r.top && g_mouse.y <= r.bottom;
    }
};

void Section(Painter& p, float& y, const wchar_t* title) {
    y += 14;
    p.Text(title, p.R(kPad, y, kW - kPad, y + 20), 14, p.c.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    y += 26;
}

// Card with icon, title and description; returns the card's right edge for the control (in DIPs).
D2D1_RECT_F Card(Painter& p, float y, art::Glyph g, const std::wstring& title, const std::wstring& desc) {
    const D2D1_RECT_F r = p.R(kPad, y, kW - kPad, y + kCardH);
    p.Fill(r, 6, p.c.card);
    p.Stroke(r, 6, p.c.stroke);
    D2D1_COLOR_F ic = p.c.text;
    ic.a *= p.dim;
    art::DrawGlyph(p.rt, g, p.R(kPad + 18, y + 24, kPad + 38, y + 44), ic);
    p.Text(title, p.R(kPad + 58, y + 14, kW - kPad - 200, y + 34), 14, p.c.text);
    p.Text(desc, p.R(kPad + 58, y + 34, kW - kPad - 200, y + 52), 12, p.c.sub);
    return r;
}

// Segmented choice at the right of a card.
void Segments(Painter& p, float y, const std::vector<std::wstring>& options, int selected, Action a) {
    float total = 0;
    std::vector<float> widths;
    for (const auto& o : options) {
        const float w = gfx::TextWidth(o, gfx::Font(13 * p.s)) / p.s + 24;
        widths.push_back(w);
        total += w;
    }
    float x = kW - kPad - 18 - total;
    const float top = y + (kCardH - 32) / 2;
    p.Fill(p.R(x - 2, top - 2, x + total + 2, top + 34), 6, p.c.control);
    for (size_t i = 0; i < options.size(); ++i) {
        const D2D1_RECT_F r = p.R(x, top, x + widths[i], top + 32);
        const bool sel = static_cast<int>(i) == selected;
        if (sel) p.Fill(r, 5, p.c.accent);
        else if (g_enabled && p.Hover(r)) p.Fill(r, 5, p.c.controlHover);
        p.Text(options[i], r, 13, sel ? p.c.onAccent : p.c.text, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
               DWRITE_TEXT_ALIGNMENT_CENTER);
        if (g_enabled) g_hits.push_back({r, a, static_cast<int>(i)});
        x += widths[i];
    }
}

void Switch(Painter& p, float y, bool on, Action a) {
    const float w = 40, h = 20, x = kW - kPad - 18 - w, top = y + (kCardH - h) / 2;
    const D2D1_RECT_F r = p.R(x, top, x + w, top + h);
    if (on) p.Fill(r, h / 2, p.c.accent);
    else {
        p.Fill(r, h / 2, p.Hover(r) ? p.c.controlHover : p.c.control);
        p.Stroke(r, h / 2, p.c.sub);
    }
    const float kr = (on ? 7.0f : 6.0f) * p.s, cx = (on ? x + w - 10 : x + 10) * p.s, cy = (top + h / 2) * p.s;
    D2D1_COLOR_F kc = on ? p.c.onAccent : p.c.sub;
    kc.a *= p.dim;
    p.br->SetColor(kc);
    p.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), kr, kr), p.br);
    p.Text(on ? L"On" : L"Off", p.R(x - 40, top, x - 10, top + h), 13, p.c.text, DWRITE_FONT_WEIGHT_NORMAL,
           DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (g_enabled) g_hits.push_back({p.R(x - 44, top - 6, x + w, top + h + 6), a, on ? 0 : 1});
}

void Button(Painter& p, float y, const std::wstring& label, Action a, bool danger = false) {
    const float w = gfx::TextWidth(label, gfx::Font(13 * p.s)) / p.s + 32, h = 32;
    const float x = kW - kPad - 18 - w, top = y + (kCardH - h) / 2;
    const D2D1_RECT_F r = p.R(x, top, x + w, top + h);
    p.Fill(r, 5, danger ? (p.Hover(r) ? gfx::Rgb(0xD13438) : p.c.danger) : (p.Hover(r) ? p.c.controlHover : p.c.control));
    if (!danger) p.Stroke(r, 5, p.c.stroke);
    p.Text(label, r, 13, danger ? gfx::Rgb(0xFFFFFF) : p.c.text, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (g_enabled) g_hits.push_back({r, a, 0});
}

void Paint() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    if (!g_rt) {
        const auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        gfx::D2D()->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_wnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt);
        if (!g_rt) return;
    }
    g_hits.clear();
    g_rt->BeginDraw();
    {
        Painter p(g_rt);
        g_rt->Clear(g_translucent ? D2D1::ColorF(0, 0, 0, 0) : p.c.bg);
        float y = 20 - g_scroll;
        p.Text(L"Settings", p.R(kPad, y, kW - kPad, y + 36), 26, p.c.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        y += 40;

        Section(p, y, L"Desktop widget");
        Card(p, y, art::Glyph::Thermometer, L"Show on the desktop", L"The AC panel as a widget that stays on the desktop");
        Switch(p, y, g_state.desktop, Action::Desktop);
        y += kCardH + kCardGap;
        p.dim = g_state.desktop ? 1.0f : 0.45f;  // the rest only matters while the widget is on
        g_enabled = g_state.desktop;
        Card(p, y, art::Glyph::SwingBoth, L"Size", L"How big the widget is");
        Segments(p, y, {L"75%", L"100%", L"125%", L"150%", L"200%"}, g_state.desktopSize, Action::DesktopSize);
        y += kCardH + kCardGap;
        Card(p, y, art::Glyph::Droplet, L"Background", L"What shows behind the widget");
        Segments(p, y, {L"Clear glass", L"Frosted glass", L"Solid"}, g_state.desktopBackground, Action::DesktopBackground);
        y += kCardH + kCardGap;
        Card(p, y, art::Glyph::BreezeAway, L"Keep on top", L"Stay above other windows");
        Switch(p, y, g_state.desktopOnTop, Action::DesktopOnTop);
        y += kCardH + kCardGap;
        Card(p, y, art::Glyph::FreezeGuard, L"Lock position", L"Stop it being dragged by accident");
        Switch(p, y, g_state.desktopLocked, Action::DesktopLocked);
        y += kCardH + kCardGap;
        Card(p, y, art::Glyph::Refresh, L"Reset position", L"Back to the bottom right of the second screen");
        Button(p, y, L"Reset", Action::DesktopReset);
        y += kCardH + kCardGap;
        p.dim = 1.0f;
        g_enabled = true;

        Section(p, y, L"Panel");
        Card(p, y, art::Glyph::Purifier, L"Background", L"What shows behind the click panel");
        Segments(p, y, {L"Mica", L"Acrylic", L"Clear glass"}, g_state.backdrop, Action::Backdrop);
        y += kCardH + kCardGap;

        Section(p, y, L"Taskbar");
        Card(p, y, art::Glyph::SwingHorizontal, L"Position", L"Which end of the taskbar the strip sits at");
        Segments(p, y, {L"Left", L"Right"}, g_state.left ? 0 : 1, Action::Side);
        y += kCardH + kCardGap;
        Card(p, y, art::Glyph::Outdoor, L"Monitors", L"Show the strip on every monitor's taskbar, or the main one");
        Segments(p, y, {L"All", L"Main only"}, g_state.allMonitors ? 0 : 1, Action::Monitors);
        y += kCardH + kCardGap;

        Section(p, y, L"General");
        Card(p, y, art::Glyph::Power, L"Start with Windows", L"Open AC Taskbar when you sign in");
        Switch(p, y, g_state.autostart, Action::Autostart);
        y += kCardH + kCardGap;
        Card(p, y, art::Glyph::Refresh, L"Refresh every", L"How often to read the AC (faster right after a change)");
        Segments(p, y, {L"5 s", L"10 s", L"30 s", L"60 s"}, g_state.interval, Action::Interval);
        y += kCardH + kCardGap;

        Section(p, y, L"Air conditioner");
        Card(p, y, art::Glyph::ModeCool, g_state.address.empty() ? L"Not connected" : L"Midea AC at " + g_state.address,
             g_state.status);
        Button(p, y, L"Refresh now", Action::Refresh);
        y += kCardH + kCardGap;
        Card(p, y, art::Glyph::Close, L"Exit", L"Close AC Taskbar; your settings are kept");
        Button(p, y, L"Exit app", Action::Exit, true);
        y += kCardH;
        g_contentH = y + g_scroll + 24;
        if (g_contentH > kH) {  // a thin scroll thumb on the right
            const float track = kH - 16, thumb = std::fmax(40.0f, track * kH / g_contentH);
            const float top = 8 + (track - thumb) * g_scroll / (g_contentH - kH);
            D2D1_COLOR_F thumbColor = p.c.sub;
            thumbColor.a = 0.55f;
            p.Fill(p.R(kW - 7, top, kW - 3, top + thumb), 2, thumbColor);
        }
    }
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) gfx::SafeRelease(g_rt);
}

void Apply() {
    g_translucent = backdrop::Supported(g_wnd);
    if (g_translucent) backdrop::Apply(g_wnd, gfx::AppsUseLightTheme(), Backdrop::Mica);
    else {
        const BOOL dark = !gfx::AppsUseLightTheme();
        DwmSetWindowAttribute(g_wnd, 20, &dark, sizeof(dark));
    }
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            Paint();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            return 0;
        case WM_MOUSEMOVE: {
            if (g_mouse.x < 0) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
            }
            g_mouse = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE: g_mouse = {-1, -1}; InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                bool over = false;
                for (const auto& hit : g_hits)
                    over |= g_mouse.x >= hit.r.left && g_mouse.x <= hit.r.right && g_mouse.y >= hit.r.top && g_mouse.y <= hit.r.bottom;
                SetCursor(LoadCursor(nullptr, over ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP: {
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            for (const auto& hit : g_hits) {
                if (x >= hit.r.left && x <= hit.r.right && y >= hit.r.top && y <= hit.r.bottom) {
                    if (g_handler) g_handler(hit.action, hit.value);
                    if (hit.action == Action::Exit) return 0;  // the window is gone
                    Refresh();
                    break;
                }
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const float maxScroll = std::fmax(0.0f, g_contentH - kH);
            g_scroll = std::fmin(maxScroll, std::fmax(0.0f, g_scroll - GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 56.0f));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) Close();
            return 0;
        case WM_SETTINGCHANGE: case WM_DWMCOLORIZATIONCOLORCHANGED:
            Apply();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY:
            gfx::SafeRelease(g_rt);
            g_wnd = nullptr;
            return 0;
        default: break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void Show(HINSTANCE inst, Provider provider, Handler handler) {
    g_provider = std::move(provider);
    g_handler = std::move(handler);
    g_scroll = 0;
    g_state = g_provider ? g_provider() : State{};
    if (g_wnd) {
        ShowWindow(g_wnd, SW_RESTORE);
        SetForegroundWindow(g_wnd);
        InvalidateRect(g_wnd, nullptr, FALSE);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ACTaskbarSettings";
        RegisterClassW(&wc);
        registered = true;
    }
    // Centred on the monitor under the cursor, sized for its DPI.
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor")))) fn(mon, 0, &dpiX, &dpiY);
    }
    const float s = dpiX / 96.0f;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, static_cast<LONG>(kW * s), static_cast<LONG>(kH * s)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpiX);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    g_wnd = CreateWindowExW(0, L"ACTaskbarSettings", L"AC Taskbar settings", style, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return;
    ShowWindow(g_wnd, SW_SHOW);
    Apply();  // after the window is shown, or DWM may skip the Mica backdrop
    SetForegroundWindow(g_wnd);
}

void Refresh() {
    if (!g_wnd) return;
    if (g_provider) g_state = g_provider();
    InvalidateRect(g_wnd, nullptr, FALSE);
}

bool Visible() { return g_wnd && IsWindowVisible(g_wnd); }

void Close() {
    if (g_wnd) DestroyWindow(g_wnd);
}

}  // namespace settings
