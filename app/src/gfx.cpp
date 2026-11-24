#include "gfx.h"

#include <dwmapi.h>

#include <cmath>
#include <map>
#include <tuple>

namespace gfx {
namespace {

bool ReadDword(const wchar_t* key, const wchar_t* value, DWORD& out) {
    DWORD size = sizeof(out);
    return RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
}
const wchar_t* kPersonalize = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

}  // namespace

ID2D1Factory* D2D() {
    static ID2D1Factory* f = [] {
        ID2D1Factory* p = nullptr;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &p);
        return p;
    }();
    return f;
}

IDWriteFactory* DWrite() {
    static IDWriteFactory* f = [] {
        IDWriteFactory* p = nullptr;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&p));
        return p;
    }();
    return f;
}

IDWriteTextFormat* Font(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    static std::map<std::tuple<int, int, int>, IDWriteTextFormat*> cache;
    const auto key = std::make_tuple(static_cast<int>(size * 100), static_cast<int>(weight), static_cast<int>(align));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    DWrite()->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (f) {
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    cache[key] = f;
    return f;
}

float TextWidth(const std::wstring& text, IDWriteTextFormat* f) {
    IDWriteTextLayout* l = nullptr;
    if (FAILED(DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), f, 10000, 100, &l))) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}

namespace {
int g_themeOverride = -1;
}

void SetThemeOverride(int light) { g_themeOverride = light; }

bool SystemUsesLightTheme() {
    if (g_themeOverride >= 0) return g_themeOverride == 1;
    DWORD v = 0;
    return ReadDword(kPersonalize, L"SystemUsesLightTheme", v) && v != 0;
}

bool AppsUseLightTheme() {
    if (g_themeOverride >= 0) return g_themeOverride == 1;
    DWORD v = 1;
    return !ReadDword(kPersonalize, L"AppsUseLightTheme", v) || v != 0;
}

D2D1_COLOR_F Rgb(unsigned rgb, float a) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}

D2D1_COLOR_F Accent(bool lightTheme) {
    // AccentPalette: 8 RGBA entries, from Light3, Light2, Light1, Accent, Dark1, Dark2, Dark3.
    BYTE pal[32];
    DWORD size = sizeof(pal);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                     RRF_RT_REG_BINARY, nullptr, pal, &size) == ERROR_SUCCESS && size >= 28) {
        const BYTE* c = pal + (lightTheme ? 4 : 1) * 4;
        return D2D1::ColorF(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, 1.0f);
    }
    DWORD abgr = 0;  // fallback: the plain accent (ABGR)
    size = sizeof(abgr);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &abgr,
                     &size) == ERROR_SUCCESS)
        return D2D1::ColorF((abgr & 0xFF) / 255.f, ((abgr >> 8) & 0xFF) / 255.f, ((abgr >> 16) & 0xFF) / 255.f, 1.0f);
    return Rgb(0x4CC2FF);
}

void DrawIcon(ID2D1RenderTarget* rt, Icon icon, const D2D1_RECT_F& box, ID2D1Brush* b) {
    const float cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    const float s = std::fmin(box.right - box.left, box.bottom - box.top) / 2;  // radius
    const float w = std::fmax(1.2f, s * 0.16f);                                // stroke
    const float pi = 3.14159265f;
    auto line = [&](float x1, float y1, float x2, float y2) {
        rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, w);
    };
    switch (icon) {
        case Icon::Power: {
            ID2D1PathGeometry* g = nullptr;
            D2D()->CreatePathGeometry(&g);
            ID2D1GeometrySink* k = nullptr;
            g->Open(&k);
            const float r = s * 0.72f, a0 = -pi / 2 + 0.6f, a1 = -pi / 2 - 0.6f + 2 * pi;
            k->BeginFigure(D2D1::Point2F(cx + r * std::cos(a0), cy + r * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
            k->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r * std::cos(a1), cy + r * std::sin(a1)), D2D1::SizeF(r, r), 0,
                                       D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
            k->EndFigure(D2D1_FIGURE_END_OPEN);
            k->Close();
            ID2D1StrokeStyle* st = nullptr;
            D2D()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &st);
            rt->DrawGeometry(g, b, w, st);
            rt->DrawLine(D2D1::Point2F(cx, cy - s * 0.9f), D2D1::Point2F(cx, cy - s * 0.15f), b, w, st);
            SafeRelease(st);
            SafeRelease(k);
            SafeRelease(g);
            break;
        }
        case Icon::Cool:  // snowflake: three crossed lines with small branches
            for (int i = 0; i < 3; ++i) {
                const float a = pi / 2 + i * pi / 3, dx = std::cos(a) * s * 0.85f, dy = std::sin(a) * s * 0.85f;
                line(cx - dx, cy - dy, cx + dx, cy + dy);
                for (int e : {-1, 1}) {
                    const float bx = cx + e * dx * 0.62f, by = cy + e * dy * 0.62f;
                    for (int t : {-1, 1}) {
                        const float ba = a + (e > 0 ? 0 : pi) + t * pi / 4;
                        line(bx, by, bx + std::cos(ba) * s * 0.28f, by + std::sin(ba) * s * 0.28f);
                    }
                }
            }
            break;
        case Icon::Heat:  // sun
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), s * 0.38f, s * 0.38f), b, w);
            for (int i = 0; i < 8; ++i) {
                const float a = i * pi / 4;
                line(cx + std::cos(a) * s * 0.62f, cy + std::sin(a) * s * 0.62f, cx + std::cos(a) * s * 0.92f,
                     cy + std::sin(a) * s * 0.92f);
            }
            break;
        case Icon::Dry: {  // water drop
            ID2D1PathGeometry* g = nullptr;
            D2D()->CreatePathGeometry(&g);
            ID2D1GeometrySink* k = nullptr;
            g->Open(&k);
            const float r = s * 0.5f, by = cy + s * 0.32f;
            k->BeginFigure(D2D1::Point2F(cx, cy - s * 0.9f), D2D1_FIGURE_BEGIN_HOLLOW);
            k->AddLine(D2D1::Point2F(cx + r * 0.95f, by - r * 0.3f));
            k->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx - r * 0.95f, by - r * 0.3f), D2D1::SizeF(r, r), 0,
                                       D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
            k->EndFigure(D2D1_FIGURE_END_CLOSED);
            k->Close();
            rt->DrawGeometry(g, b, w);
            SafeRelease(k);
            SafeRelease(g);
            break;
        }
        case Icon::Fan:  // three blades
            for (int i = 0; i < 3; ++i) {
                const float a = -pi / 2 + i * 2 * pi / 3;
                const float ex = cx + std::cos(a) * s * 0.52f, ey = cy + std::sin(a) * s * 0.52f;
                rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ex, ey), s * 0.3f, s * 0.3f), b, w);
            }
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), s * 0.14f, s * 0.14f), b);
            break;
        case Icon::Auto: {  // "A" in a circle
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), s * 0.85f, s * 0.85f), b, w);
            line(cx - s * 0.38f, cy + s * 0.42f, cx, cy - s * 0.45f);
            line(cx, cy - s * 0.45f, cx + s * 0.38f, cy + s * 0.42f);
            line(cx - s * 0.2f, cy + s * 0.08f, cx + s * 0.2f, cy + s * 0.08f);
            break;
        }
        case Icon::Warning:
            line(cx, cy - s * 0.85f, cx - s * 0.85f, cy + s * 0.7f);
            line(cx - s * 0.85f, cy + s * 0.7f, cx + s * 0.85f, cy + s * 0.7f);
            line(cx + s * 0.85f, cy + s * 0.7f, cx, cy - s * 0.85f);
            line(cx, cy - s * 0.25f, cx, cy + s * 0.2f);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + s * 0.42f), w * 0.6f, w * 0.6f), b);
            break;
        case Icon::Minus: line(cx - s * 0.5f, cy, cx + s * 0.5f, cy); break;
        case Icon::Plus:
            line(cx - s * 0.5f, cy, cx + s * 0.5f, cy);
            line(cx, cy - s * 0.5f, cx, cy + s * 0.5f);
            break;
        case Icon::Refresh: {
            ID2D1PathGeometry* g = nullptr;
            D2D()->CreatePathGeometry(&g);
            ID2D1GeometrySink* k = nullptr;
            g->Open(&k);
            const float r = s * 0.7f;
            k->BeginFigure(D2D1::Point2F(cx + r, cy), D2D1_FIGURE_BEGIN_HOLLOW);
            k->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx, cy - r), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                       D2D1_ARC_SIZE_LARGE));
            k->EndFigure(D2D1_FIGURE_END_OPEN);
            k->Close();
            rt->DrawGeometry(g, b, w);
            line(cx, cy - r, cx + s * 0.35f, cy - r - s * 0.25f);
            line(cx, cy - r, cx + s * 0.35f, cy - r + s * 0.25f);
            SafeRelease(k);
            SafeRelease(g);
            break;
        }
    }
}

}  // namespace gfx
