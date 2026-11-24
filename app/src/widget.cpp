#include "widget.h"

#include <commctrl.h>
#include <uiautomation.h>

#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// One strip per taskbar: the main one (Shell_TrayWnd) and each secondary monitor's (Shell_SecondaryTrayWnd).
// All strips show the same model; each is placed on its own taskbar.

namespace widget {
namespace {

struct Strip {
    HWND bar = nullptr, wnd = nullptr;
    bool primary = false, hover = false;
    RECT lastPos{};
};

std::vector<std::unique_ptr<Strip>> g_strips;
HWND g_notify = nullptr, g_tip = nullptr;
HINSTANCE g_inst = nullptr;
UINT g_clickMsg = 0, g_menuMsg = 0;
StripModel g_model;
bool g_light = false, g_left = false, g_allMonitors = true;
int g_width = 0;
RECT g_clicked{};  // screen rect of the strip clicked last (the panel opens next to it)

constexpr UINT_PTR kAnimTimer = 7;

// ---- Secondary taskbars draw their clock in XAML (no window), so measure it with UI Automation on a
// background MTA thread. Never call UIA from the UI thread: Explorer waits on our child windows' thread.
std::mutex g_uiaMu;
std::map<HWND, int> g_trayLeft;  // secondary bar -> left edge of its clock / tray buttons (screen x)
// Each measuring thread gets its own stop flag, so shutting down never waits for a slow UIA call.
std::shared_ptr<std::atomic<bool>> g_uiaStop;

void UiaLoop(std::shared_ptr<std::atomic<bool>> stop) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                     reinterpret_cast<void**>(&uia));
    IUIAutomationCondition* cond = nullptr;
    if (uia) {
        VARIANT v;
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"SystemTrayIcon");
        uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &cond);
        VariantClear(&v);
    }
    while (uia && cond && !*stop) {
        std::map<HWND, int> found;
        for (HWND bar = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); bar;
             bar = FindWindowExW(nullptr, bar, L"Shell_SecondaryTrayWnd", nullptr)) {
            IUIAutomationElement* root = nullptr;
            if (FAILED(uia->ElementFromHandle(bar, &root)) || !root) continue;
            IUIAutomationElementArray* arr = nullptr;
            if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
                int n = 0, left = INT_MAX;
                arr->get_Length(&n);
                for (int i = 0; i < n; ++i) {
                    IUIAutomationElement* e = nullptr;
                    RECT r{};
                    if (SUCCEEDED(arr->GetElement(i, &e)) && e) {
                        e->get_CurrentBoundingRectangle(&r);
                        if (r.right > r.left && r.left < left) left = r.left;
                        e->Release();
                    }
                }
                if (left != INT_MAX) found[bar] = left;
                arr->Release();
            }
            root->Release();
        }
        if (*stop) break;
        {
            std::lock_guard<std::mutex> l(g_uiaMu);
            g_trayLeft = found;
        }
        for (int i = 0; i < 30 && !*stop; ++i) Sleep(100);  // every 3 s
    }
    if (cond) cond->Release();
    if (uia) uia->Release();
    CoUninitialize();
}

double Seconds() {
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

float ScaleOf(HWND bar) { return GetDpiForWindow(bar ? bar : GetDesktopWindow()) / 96.0f; }

Strip* Find(HWND wnd) {
    for (auto& s : g_strips) if (s->wnd == wnd) return s.get();
    return nullptr;
}

// Desired width in pixels (per DPI) for the current text.
int MeasureWidth(float s) {
    const float w1 = gfx::TextWidth(g_model.line1, gfx::Font(15 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD));
    const float w2 = gfx::TextWidth(g_model.line2, gfx::Font(11.5f * s));
    // pill padding + glyph (42x28, a 3D unit with its air below) + gap + text + pill padding
    return static_cast<int>(std::ceil(10 * s + 42 * s + 9 * s + std::fmax(w1, w2) + 12 * s));
}

void Render(Strip& st) {
    if (!st.wnd) return;
    RECT rc;
    GetClientRect(st.wnd, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    const float s = ScaleOf(st.bar);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);

    ID2D1DCRenderTarget* rt = nullptr;
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (SUCCEEDED(gfx::D2D()->CreateDCRenderTarget(&props, &rt)) && SUCCEEDED(rt->BindDC(mem, &rc))) {
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 1.0f / 255));  // nearly transparent so the whole strip takes clicks
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        ID2D1SolidColorBrush* br = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);

        const D2D1_COLOR_F fg = g_light ? gfx::Rgb(0x1A1A1A) : gfx::Rgb(0xFFFFFF);
        {  // a soft pill so the strip reads as one object, apart from the taskbar icons; brighter on hover
            const float a = g_light ? (st.hover ? 0.09f : 0.045f) : (st.hover ? 0.11f : 0.055f);
            br->SetColor(g_light ? gfx::Rgb(0x000000, a) : gfx::Rgb(0xFFFFFF, a));
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(2 * s, 5 * s, w - 2 * s, h - 5 * s), 7 * s, 7 * s), br);
        }

        const float gh = std::round(28 * s), gw = std::round(42 * s), ix = std::round(10 * s), iy = std::round((h - gh) / 2);
        art::Scene sc = g_model.scene;
        sc.light = g_light;
        sc.time = Seconds();
        sc.reducedMotion = ReducedMotion();
        art::DrawStripGlyph(rt, D2D1::RectF(ix, iy, ix + gw, iy + gh), sc);

        const float tx = ix + gw + 9 * s;
        D2D1_COLOR_F c1 = fg, c2 = fg;
        c2.a = g_light ? 0.68f : 0.72f;
        if (g_model.dim) c2.a = 0.55f;  // the room temperature stays readable when the AC is off
        if (g_model.alert) c1 = gfx::Rgb(0xFF5A5A);
        br->SetColor(c1);
        const float mid = h / 2.0f;
        rt->DrawText(g_model.line1.c_str(), static_cast<UINT32>(g_model.line1.size()),
                     gfx::Font(15 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(tx, mid - 19 * s, w * 1.0f, mid + 1 * s), br);
        br->SetColor(c2);
        rt->DrawText(g_model.line2.c_str(), static_cast<UINT32>(g_model.line2.size()), gfx::Font(11.5f * s),
                     D2D1::RectF(tx, mid + 1 * s, w * 1.0f, mid + 17 * s), br);
        gfx::SafeRelease(br);
        rt->EndDraw();
    }
    gfx::SafeRelease(rt);

    POINT src{0, 0};
    SIZE size{w, h};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(st.wnd, nullptr, nullptr, &size, mem, &src, 0, &blend, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void RenderAll() {
    for (auto& s : g_strips) Render(*s);
}

void SetTip(HWND wnd, const wchar_t* text, bool add) {
    if (!g_tip) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_notify;
    ti.uId = reinterpret_cast<UINT_PTR>(wnd);
    ti.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(g_tip, add ? TTM_ADDTOOLW : TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Strip* st = Find(h);
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_TIMER:
            if (wp == kAnimTimer && st) Render(*st);
            return 0;
        case WM_MOUSEMOVE:
            if (st && !st->hover) {
                st->hover = true;
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                Render(*st);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (st) { st->hover = false; Render(*st); }
            return 0;
        case WM_LBUTTONUP: GetWindowRect(h, &g_clicked); PostMessageW(g_notify, g_clickMsg, 0, 0); return 0;
        case WM_RBUTTONUP: GetWindowRect(h, &g_clicked); PostMessageW(g_notify, g_menuMsg, 0, 0); return 0;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: return 0;
        default: return DefWindowProcW(h, m, wp, lp);
    }
}

bool AddStrip(HWND bar, bool primary) {
    auto st = std::make_unique<Strip>();
    st->bar = bar;
    st->primary = primary;
    RECT b;
    GetWindowRect(bar, &b);
    st->wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, L"ACTaskbarStrip", L"AC", WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, g_width, b.bottom - b.top, bar, nullptr, g_inst, nullptr);
    if (!st->wnd) return false;
    SetTip(st->wnd, g_model.tooltip.empty() ? L"Air conditioner" : g_model.tooltip.c_str(), true);
    if (g_model.animate && !ReducedMotion()) SetTimer(st->wnd, kAnimTimer, 33, nullptr);
    g_strips.push_back(std::move(st));
    return true;
}

void RemoveStrip(size_t i) {
    if (g_strips[i]->wnd && IsWindow(g_strips[i]->wnd)) DestroyWindow(g_strips[i]->wnd);
    g_strips.erase(g_strips.begin() + i);
}

// Places one strip on its taskbar. Returns false if that taskbar is gone.
bool Place(Strip& st) {
    if (!IsWindow(st.bar) || !IsWindow(st.wnd)) return false;
    RECT bar;
    GetWindowRect(st.bar, &bar);
    const int barW = bar.right - bar.left, barH = bar.bottom - bar.top;
    if (barW < barH) {  // vertical taskbar: not handled yet, keep out of the way
        ShowWindow(st.wnd, SW_HIDE);
        return true;
    }
    const float s = ScaleOf(st.bar);
    const int width = MeasureWidth(s);
    const int margin = static_cast<int>(8 * s);
    auto layered = [&](HWND c) {
        return c != st.wnd && IsWindowVisible(c) && (GetWindowLongW(c, GWL_EXSTYLE) & WS_EX_LAYERED);
    };
    int x;
    if (g_left) {
        // Left end: from the taskbar's left edge, stepping right past other apps' strips (open-battery).
        int left = bar.left;
        for (bool moved = true; moved;) {
            moved = false;
            for (HWND c = GetWindow(st.bar, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
                if (!layered(c)) continue;
                RECT r;
                GetWindowRect(c, &r);
                if (r.right > left && r.left < left + width + 2 * margin) { left = r.right; moved = true; }
            }
        }
        x = left + margin;
    } else {
        // Right end: just left of the notification area (main taskbar) or of the clock (other monitors).
        int right = bar.right - static_cast<int>(84 * s);  // fallback: room for a secondary clock
        if (st.primary) {
            HWND tray = FindWindowExW(st.bar, nullptr, L"TrayNotifyWnd", nullptr);
            RECT tr{};
            if (tray && GetWindowRect(tray, &tr) && tr.right > tr.left) right = tr.left;
            else right = bar.right - barW / 4;
        } else {
            std::lock_guard<std::mutex> l(g_uiaMu);
            auto it = g_trayLeft.find(st.bar);
            if (it != g_trayLeft.end() && it->second > bar.left && it->second <= bar.right) right = it->second;
        }
        for (bool moved = true; moved;) {
            moved = false;
            for (HWND c = GetWindow(st.bar, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
                if (!layered(c)) continue;
                RECT r;
                GetWindowRect(c, &r);
                if (r.left < right && r.right > right - width - 2 * margin) { right = r.left; moved = true; }
            }
        }
        x = right - width - margin;
    }
    RECT pos{x - bar.left, 0, x - bar.left + width, barH};
    const bool covered = GetWindow(st.wnd, GW_HWNDPREV) != nullptr;  // a sibling is drawn above us
    if (!EqualRect(&pos, &st.lastPos) || covered) {
        SetWindowPos(st.wnd, HWND_TOP, pos.left, pos.top, width, barH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!EqualRect(&pos, &st.lastPos)) {
            st.lastPos = pos;
            Render(st);
        }
    }
    return true;
}

}  // namespace

bool Create(HINSTANCE inst, HWND notify, UINT clickMsg, UINT menuMsg) {
    g_inst = inst;
    g_notify = notify;
    g_clickMsg = clickMsg;
    g_menuMsg = menuMsg;
    HWND main = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!main) return false;
    g_light = gfx::SystemUsesLightTheme();

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ACTaskbarStrip";
        RegisterClassW(&wc);
        registered = true;
    }
    if (g_model.line1.empty()) { g_model.scene.power = 0; g_model.line1 = L"AC"; g_model.line2 = L"Connecting…"; g_model.dim = true; }
    g_width = MeasureWidth(ScaleOf(main));

    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, notify, nullptr, inst, nullptr);
    if (g_tip) SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 400);

    if (!AddStrip(main, true)) return false;
    g_uiaStop = std::make_shared<std::atomic<bool>>(false);
    std::thread(UiaLoop, g_uiaStop).detach();
    Tick();
    for (auto& s : g_strips) ShowWindow(s->wnd, SW_SHOWNOACTIVATE);
    RenderAll();
    return true;
}

void Destroy() {
    if (g_uiaStop) *g_uiaStop = true;  // the thread notices within 100 ms (or after its current UIA call)
    g_uiaStop.reset();
    while (!g_strips.empty()) RemoveStrip(g_strips.size() - 1);
    if (g_tip) DestroyWindow(g_tip);
    g_tip = nullptr;
    std::lock_guard<std::mutex> l(g_uiaMu);
    g_trayLeft.clear();
}

bool Alive() {
    return !g_strips.empty() && g_strips[0]->primary && IsWindow(g_strips[0]->wnd) && IsWindow(g_strips[0]->bar);
}

void Update(const StripModel& m) {
    g_model = m;
    for (auto& s : g_strips) {
        SetTip(s->wnd, g_model.tooltip.c_str(), false);
        if (m.animate && !ReducedMotion()) SetTimer(s->wnd, kAnimTimer, 33, nullptr);  // 30 fps only while running
        else KillTimer(s->wnd, kAnimTimer);
    }
    const int w = g_strips.empty() ? 0 : MeasureWidth(ScaleOf(g_strips[0]->bar));
    if (w != g_width) {
        g_width = w;
        for (auto& s : g_strips) s->lastPos = {};  // force a re-layout at the new width
        Tick();
    }
    RenderAll();
}

void SetSide(bool left) {
    g_left = left;
    for (auto& s : g_strips) s->lastPos = {};
    Tick();
}

void SetAllMonitors(bool all) {
    g_allMonitors = all;
    Tick();
}

void Retheme() {
    g_light = gfx::SystemUsesLightTheme();
    RenderAll();
}

bool Tick() {
    if (!Alive()) return false;
    // Follow monitors coming and going: one strip per secondary taskbar.
    std::vector<HWND> bars;
    for (HWND b = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); b && g_allMonitors;
         b = FindWindowExW(nullptr, b, L"Shell_SecondaryTrayWnd", nullptr))
        bars.push_back(b);
    for (size_t i = g_strips.size(); i-- > 1;) {
        bool still = false;
        for (HWND b : bars) still |= b == g_strips[i]->bar;
        if (!still || !IsWindow(g_strips[i]->wnd)) RemoveStrip(i);
    }
    for (HWND b : bars) {
        bool have = false;
        for (auto& s : g_strips) have |= s->bar == b;
        if (!have && AddStrip(b, false)) {
            ShowWindow(g_strips.back()->wnd, SW_SHOWNOACTIVATE);
            Render(*g_strips.back());
        }
    }
    for (auto& s : g_strips) Place(*s);
    return true;
}

RECT ScreenRect() {
    if (g_clicked.right > g_clicked.left) return g_clicked;
    RECT r{};
    if (!g_strips.empty()) GetWindowRect(g_strips[0]->wnd, &r);
    return r;
}

}  // namespace widget
