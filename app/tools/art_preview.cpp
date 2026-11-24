// Renders contact sheets of the art module (app\src\art.cpp) to PNG, for looking at.
// Usage: art_preview.exe <output directory>
#include <windows.h>
#include <objbase.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "art.h"

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID2D1Factory> g_d2d;
ComPtr<IWICImagingFactory> g_wic;
ComPtr<IDWriteFactory> g_dw;
ComPtr<IDWriteTextFormat> g_font;
std::wstring g_out;

struct Canvas {
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> ink;
    UINT w = 0, h = 0;
};

bool Make(Canvas& c, UINT w, UINT h) {
    c.w = w; c.h = h;
    if (FAILED(g_wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &c.bmp))) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(g_d2d->CreateWicBitmapRenderTarget(c.bmp.Get(), props, &c.rt))) return false;
    return SUCCEEDED(c.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &c.ink));
}

bool Save(IWICBitmap* bmp, UINT w, UINT h, const std::wstring& name) {
    std::wstring path = g_out + L"\\" + name + L".png";
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(g_wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
              SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
              SUCCEEDED(frame->WriteSource(bmp, nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    std::printf("%s %ls (%ux%u)\n", ok ? "wrote" : "FAILED", path.c_str(), w, h);
    return ok;
}

// Draws a sheet at 1x and saves it; with zoom > 1 also saves a nearest-neighbour blow-up as <name>@Nx.
void Sheet(const std::wstring& name, UINT w, UINT h, UINT zoom, bool save1x, const std::function<void(Canvas&)>& draw) {
    Canvas c;
    if (!Make(c, w, h)) { std::printf("canvas failed\n"); return; }
    c.rt->BeginDraw();
    draw(c);
    c.rt->EndDraw();
    if (save1x) Save(c.bmp.Get(), w, h, name);
    if (zoom > 1) {
        Canvas z;
        if (!Make(z, w * zoom, h * zoom)) return;
        ComPtr<ID2D1Bitmap> src;
        z.rt->BeginDraw();
        if (SUCCEEDED(z.rt->CreateBitmapFromWicBitmap(c.bmp.Get(), &src)))
            z.rt->DrawBitmap(src.Get(), D2D1::RectF(0, 0, static_cast<float>(w * zoom), static_cast<float>(h * zoom)), 1.0f,
                             D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        z.rt->EndDraw();
        Save(z.bmp.Get(), w * zoom, h * zoom, name + L"@" + std::to_wstring(zoom) + L"x");
    }
}

void Text(Canvas& c, const std::wstring& s, float x, float y, D2D1_COLOR_F col) {
    c.ink->SetColor(col);
    c.rt->DrawText(s.c_str(), static_cast<UINT32>(s.size()), g_font.Get(), D2D1::RectF(x, y, x + 600.0f, y + 20.0f), c.ink.Get());
}
void Fill(Canvas& c, D2D1_RECT_F r, D2D1_COLOR_F col) {
    c.ink->SetColor(col);
    c.rt->FillRectangle(r, c.ink.Get());
}

D2D1_COLOR_F PanelBg(bool light) { return light ? D2D1::ColorF(0xFFFFFF) : D2D1::ColorF(0x2B2B2B); }
D2D1_COLOR_F TaskbarBg(bool light) { return light ? D2D1::ColorF(0xEEEEEE) : D2D1::ColorF(0x1C1C1C); }
D2D1_COLOR_F LabelInk(bool light) { return light ? D2D1::ColorF(0x000000, 0.55f) : D2D1::ColorF(0xFFFFFF, 0.55f); }
D2D1_COLOR_F IconInk(bool light) { return light ? D2D1::ColorF(0x000000, 0.90f) : D2D1::ColorF(0xFFFFFF, 0.95f); }

struct Cell {
    std::wstring label;
    art::Scene s;
};

art::Scene Base(int mode, double t = 3.4) {
    art::Scene s;
    s.mode = mode; s.fromMode = mode;
    s.time = t;
    return s;
}

const wchar_t* ModeName(int m) {
    switch (m) {
    case art::Auto: return L"auto";
    case art::Cool: return L"cool";
    case art::Dry: return L"dry";
    case art::Heat: return L"heat";
    default: return L"fan";
    }
}

std::vector<Cell> SceneCells() {
    std::vector<Cell> v;
    const int modes[5] = { art::Cool, art::Heat, art::Dry, art::Fan, art::Auto };
    for (int m : modes) {
        v.push_back({ std::wstring(ModeName(m)) + L" on", Base(m) });
        art::Scene off = Base(m);
        off.power = 0;
        v.push_back({ std::wstring(ModeName(m)) + L" off", off });
    }
    for (float f : { 0.2f, 0.6f, 1.0f }) {
        art::Scene s = Base(art::Cool); s.fan = f;
        wchar_t b[32]; swprintf(b, 32, L"cool fan %.1f", f);
        v.push_back({ b, s });
    }
    for (float f : { 0.0f, 0.5f, 1.0f }) {
        art::Scene s = Base(art::Cool); s.flap = f;
        wchar_t b[32]; swprintf(b, 32, L"cool flap %.1f", f);
        v.push_back({ b, s });
    }
    for (float f : { -1.0f, 0.0f, 1.0f }) {
        art::Scene s = Base(art::Cool); s.sway = f;
        wchar_t b[32]; swprintf(b, 32, L"cool sway %+.0f", f);
        v.push_back({ b, s });
    }
    { art::Scene s = Base(art::Cool); s.turbo = true; s.fan = 1.0f; v.push_back({ L"cool turbo", s }); }
    { art::Scene s = Base(art::Cool); s.sleep = true; s.fan = 0.2f; v.push_back({ L"cool sleep", s }); }
    { art::Scene s = Base(art::Cool); s.eco = true; v.push_back({ L"cool eco", s }); }
    { art::Scene s = Base(art::Cool); s.purifier = true; v.push_back({ L"cool purifier", s }); }
    { art::Scene s = Base(art::Cool); s.breezeAway = true; v.push_back({ L"cool breezeAway", s }); }
    { art::Scene s = Base(art::Cool); s.online = false; v.push_back({ L"offline", s }); }
    { art::Scene s = Base(art::Cool); s.reducedMotion = true; v.push_back({ L"cool reducedMotion", s }); }
    { art::Scene s = Base(art::Heat); s.reducedMotion = true; v.push_back({ L"heat reducedMotion", s }); }
    { art::Scene s = Base(art::Heat); s.defrost = true; v.push_back({ L"heat defrost", s }); }
    { art::Scene s = Base(art::Heat); s.fromMode = art::Cool; s.modeMix = 0.5f; v.push_back({ L"cool>heat mix 0.5", s }); }
    { art::Scene s = Base(art::Cool); s.power = 0.5f; v.push_back({ L"cool power 0.5", s }); }
    { art::Scene s = Base(art::Heat); s.eco = true; s.turbo = true; s.sleep = true; s.purifier = true; s.targetTemperature = 22.5f;
      v.push_back({ L"heat all flags 22.5", s }); }
    { art::Scene s = Base(art::Dry); s.flap = 1.0f; s.sway = 0.6f; s.fan = 1.0f; v.push_back({ L"dry flap 1 sway .6 fan 1", s }); }
    { art::Scene s = Base(art::Heat); s.breezeAway = true; v.push_back({ L"heat breezeAway", s }); }
    return v;
}

constexpr float kSceneW = 360, kSceneH = 168;

// Scene cells in a grid, each drawn at `scale` through a transform (so the art is also tested under one).
void SceneSheet(const std::wstring& name, const std::vector<Cell>& cells, size_t first, size_t count, int cols, float scale, bool light,
                float sceneH = kSceneH, bool anchors = false) {
    const float pad = 8, labelH = 16;
    float cw = kSceneW * scale + pad * 2, ch = sceneH * scale + labelH + pad * 2;
    count = std::min(count, cells.size() - first);
    int rows = static_cast<int>((count + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols));
    Sheet(name, static_cast<UINT>(cw * static_cast<float>(cols)), static_cast<UINT>(ch * static_cast<float>(rows)), 1, true, [&](Canvas& c) {
        c.rt->Clear(light ? D2D1::ColorF(0xDADADA) : D2D1::ColorF(0x101010));
        std::vector<ComPtr<ID2D1PathGeometry>> cornerMasks;
        for (size_t i = 0; i < count; ++i) {
            float x = static_cast<float>(i % static_cast<size_t>(cols)) * cw + pad;
            float y = static_cast<float>(i / static_cast<size_t>(cols)) * ch + pad;
            Fill(c, D2D1::RectF(x, y, x + kSceneW * scale, y + sceneH * scale + labelH), PanelBg(light));
            // The panel clips the scene to a rounded card: show the corners the same way.
            const float cr = 8.0f * scale;
            c.ink->SetColor(light ? D2D1::ColorF(0xDADADA) : D2D1::ColorF(0x101010));
            for (int k = 0; k < 2; ++k) {
                float ex = k == 0 ? x : x + kSceneW * scale - cr;
                ComPtr<ID2D1PathGeometry> g; ComPtr<ID2D1GeometrySink> sk;
                if (SUCCEEDED(g_d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sk))) {
                    D2D1_POINT_2F corner = D2D1::Point2F(k == 0 ? x : x + kSceneW * scale, y);
                    sk->BeginFigure(corner, D2D1_FIGURE_BEGIN_FILLED);
                    sk->AddLine(D2D1::Point2F(k == 0 ? ex + cr : ex, y));
                    sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(corner.x, y + cr), D2D1::SizeF(cr, cr), 0,
                               k == 0 ? D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE : D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sk->Close();
                    cornerMasks.push_back(g);
                }
            }
            Text(c, cells[first + i].label, x + 4, y + sceneH * scale, LabelInk(light));
            art::Scene s = cells[first + i].s;
            s.light = light;
            c.rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(x, y));
            art::SceneAnchors a{};
            art::DrawScene(c.rt.Get(), D2D1::RectF(0, 0, kSceneW, sceneH), s, &a);
            if (anchors) {
                // The panel's overlay: the round power button (top right).
                c.ink->SetColor(D2D1::ColorF(0xFF2D55, 0.8f));
                c.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(kSceneW - 27, 27), 17, 17), c.ink.Get(), 0.75f);
                c.ink->SetColor(D2D1::ColorF(0xFF2D55));
                for (D2D1_POINT_2F q : { a.led, a.louver, a.room, a.wall }) c.rt->FillEllipse(D2D1::Ellipse(q, 2.2f, 2.2f), c.ink.Get());
            }
            c.rt->SetTransform(D2D1::Matrix3x2F::Identity());
            c.ink->SetColor(light ? D2D1::ColorF(0xDADADA) : D2D1::ColorF(0x101010));
            for (auto& g : cornerMasks) c.rt->FillGeometry(g.Get(), c.ink.Get());
            cornerMasks.clear();
        }
    });
}

void Filmstrip(const std::wstring& name, int mode, bool light) {
    std::vector<Cell> cells;
    for (int i = 0; i < 6; ++i) {
        double t = 10.0 + 0.2 * i;
        wchar_t b[32]; swprintf(b, 32, L"%s t=+%.1fs", ModeName(mode), 0.2 * i);
        cells.push_back({ b, Base(mode, t) });
    }
    SceneSheet(name, cells, 0, 6, 2, 2.0f, light);
}

// kind: 0 dark taskbar, 1 light taskbar, 2 the user's translucent green-black taskbar.
D2D1_COLOR_F StripBg(int kind) { return kind == 2 ? D2D1::ColorF(0x1E2418) : TaskbarBg(kind == 1); }

// Rows: the 5 modes. Columns: on at 40x18, 44x20, 48x22 and 52x20 (2.6:1); off at 40x18 and 44x20;
// then offline / reduced motion / half power.
void StripSheet(const std::wstring& name, int kind) {
    const int modes[5] = { art::Cool, art::Heat, art::Dry, art::Fan, art::Auto };
    const int ws[7] = { 40, 44, 48, 52, 40, 44, 44 }, hs[7] = { 18, 20, 22, 20, 18, 20, 20 };
    const float cellW = 60, cellH = 30;
    bool light = kind == 1;
    Sheet(name, static_cast<UINT>(cellW * 7), static_cast<UINT>(cellH * 5), 4, true, [&](Canvas& c) {
        c.rt->Clear(StripBg(kind));
        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < 7; ++col) {
                art::Scene s = Base(modes[row], 3.4 + 0.21 * col + 0.4 * row);
                s.light = light;
                if (col == 4 || col == 5) s.power = 0;
                if (col == 6) { if (row == 0 || row == 3) s.online = false; else if (row == 1) s.reducedMotion = true; else s.power = 0.5f; }
                float h = static_cast<float>(hs[col]), w = static_cast<float>(ws[col]);
                float x = std::round(static_cast<float>(col) * cellW + (cellW - w) * 0.5f);
                float y = std::round(static_cast<float>(row) * cellH + (cellH - h) * 0.5f);
                art::DrawStripGlyph(c.rt.Get(), D2D1::RectF(x, y, x + w, y + h), s);
            }
    });
}

// The animation: 8 frames over one wisp cycle, per mode, at 44x20; the last row is fan speed 0.2 and 1.0.
void StripFilm(const std::wstring& name, int kind) {
    const int modes[6] = { art::Cool, art::Heat, art::Dry, art::Fan, art::Auto, art::Cool };
    const float cellW = 52, cellH = 28;
    bool light = kind == 1;
    Sheet(name, static_cast<UINT>(cellW * 8), static_cast<UINT>(cellH * 6), 4, true, [&](Canvas& c) {
        c.rt->Clear(StripBg(kind));
        for (int row = 0; row < 6; ++row)
            for (int col = 0; col < 8; ++col) {
                art::Scene s = Base(modes[row], 20.0 + 0.2 * col);
                s.light = light;
                if (row == 5) s.fan = col < 4 ? 0.2f : 1.0f;
                float x = static_cast<float>(col) * cellW + 4, y = static_cast<float>(row) * cellH + 4;
                art::DrawStripGlyph(c.rt.Get(), D2D1::RectF(x, y, x + 44, y + 20), s);
            }
    });
}

// Rendered natively at larger scales (125 % with a half-pixel offset, 150 %, 200 %, 400 %): no blow-up.
void StripHiDpi(const std::wstring& name, int kind) {
    bool light = kind == 1;
    const float ws[4] = { 55, 66, 88, 176 }, hs[4] = { 25, 30, 40, 80 }, xs[4] = { 8, 76, 156, 258 };
    Sheet(name, 446, 4 * 92, 1, true, [&](Canvas& c) {
        c.rt->Clear(StripBg(kind));
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col) {
                art::Scene s = Base(row == 1 ? art::Heat : art::Cool, 3.4 + 0.3 * col);
                s.light = light;
                if (row == 2) s.power = 0;
                if (row == 3) s.online = false;
                float x = xs[col], y = static_cast<float>(row) * 92 + 6 + (col == 0 ? 0.5f : 0.0f);
                art::DrawStripGlyph(c.rt.Get(), D2D1::RectF(x, y, x + ws[col], y + hs[col]), s);
            }
    });
}

void GlyphSheet(const std::wstring& name, bool light) {
    const int count = static_cast<int>(art::Glyph::Info) + 1;
    const int cols = 4;
    const float cellW = 80, cellH = 32;
    int rows = (count + cols - 1) / cols;
    Sheet(name, static_cast<UINT>(cellW * cols), static_cast<UINT>(cellH * static_cast<float>(rows)), 4, true, [&](Canvas& c) {
        c.rt->Clear(PanelBg(light));
        for (int i = 0; i < count; ++i) {
            float x = static_cast<float>(i % cols) * cellW, y = static_cast<float>(i / cols) * cellH;
            float px = x + 6;
            for (int size : { 16, 20, 24 }) {
                float sz = static_cast<float>(size);
                float py = std::round(y + (cellH - sz) * 0.5f);
                art::DrawGlyph(c.rt.Get(), static_cast<art::Glyph>(i), D2D1::RectF(px, py, px + sz, py + sz), IconInk(light), 0.0);
                px += sz + 4;
            }
        }
    });
}

// The glyphs that move on hover, at 24 px: t = 0 (rest), then four steps through the first 1.2 s.
void HoverSheet(const std::wstring& name, bool light) {
    const art::Glyph gs[] = { art::Glyph::ModeAuto, art::Glyph::ModeCool, art::Glyph::ModeDry, art::Glyph::ModeHeat, art::Glyph::ModeFan,
                              art::Glyph::FanLow, art::Glyph::FanAuto, art::Glyph::Eco, art::Glyph::Turbo, art::Glyph::Sleep,
                              art::Glyph::Purifier, art::Glyph::BreezeAway, art::Glyph::FreezeGuard, art::Glyph::Beep, art::Glyph::SelfClean };
    const double ts[] = { 0.0, 0.15, 0.3, 0.6, 0.9, 1.2 };
    const float cell = 32;
    Sheet(name, static_cast<UINT>(cell * 6), static_cast<UINT>(cell * 15), 4, false, [&](Canvas& c) {
        c.rt->Clear(PanelBg(light));
        for (int row = 0; row < 15; ++row)
            for (int col = 0; col < 6; ++col) {
                float x = static_cast<float>(col) * cell + 4, y = static_cast<float>(row) * cell + 4;
                art::DrawGlyph(c.rt.Get(), gs[row], D2D1::RectF(x, y, x + 24, y + 24), IconInk(light), ts[col]);
            }
    });
}

void LouverSheet(const std::wstring& name, bool light) {
    const float cellW = 80, cellH = 32;
    Sheet(name, static_cast<UINT>(cellW * 6), static_cast<UINT>(cellH * 2), 4, true, [&](Canvas& c) {
        c.rt->Clear(PanelBg(light));
        for (int o = 0; o < 2; ++o)
            for (int idx = -1; idx <= 4; ++idx) {
                float x = static_cast<float>(idx + 1) * cellW, y = static_cast<float>(o) * cellH;
                float px = x + 6;
                for (int size : { 16, 20, 24 }) {
                    float sz = static_cast<float>(size);
                    float py = std::round(y + (cellH - sz) * 0.5f);
                    art::DrawLouverPosition(c.rt.Get(), idx, o == 1, D2D1::RectF(px, py, px + sz, py + sz), IconInk(light));
                    px += sz + 4;
                }
            }
    });
}

std::vector<BYTE> Pixels(const std::function<void(ID2D1RenderTarget*)>& draw, UINT w, UINT h) {
    Canvas c;
    std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
    if (!Make(c, w, h)) return px;
    c.rt->BeginDraw();
    c.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    draw(c.rt.Get());
    c.rt->EndDraw();
    WICRect rc{ 0, 0, static_cast<INT>(w), static_cast<INT>(h) };
    c.bmp->CopyPixels(&rc, w * 4, static_cast<UINT>(px.size()), px.data());
    return px;
}

void Checks() {
    auto sceneAt = [](art::Scene s, double t) {
        s.time = t;
        return Pixels([&](ID2D1RenderTarget* rt) { art::DrawScene(rt, D2D1::RectF(0, 0, 360, 168), s); }, 360, 168);
    };
    auto stripAt = [](art::Scene s, double t) {
        s.time = t;
        return Pixels([&](ID2D1RenderTarget* rt) { art::DrawStripGlyph(rt, D2D1::RectF(0, 0, 53, 24), s); }, 53, 24);
    };
    art::Scene rm = Base(art::Cool); rm.reducedMotion = true; rm.purifier = true;
    art::Scene offl = Base(art::Heat); offl.online = false;
    art::Scene off = Base(art::Cool); off.power = 0;
    art::Scene on = Base(art::Cool);
    std::printf("check scene reducedMotion static: %s\n", sceneAt(rm, 1.0) == sceneAt(rm, 7.37) ? "ok" : "FAIL");
    std::printf("check scene offline static:       %s\n", sceneAt(offl, 1.0) == sceneAt(offl, 7.37) ? "ok" : "FAIL");
    std::printf("check scene power 0 static:       %s\n", sceneAt(off, 1.0) == sceneAt(off, 7.37) ? "ok" : "FAIL");
    std::printf("check scene on animates:          %s\n", sceneAt(on, 1.0) != sceneAt(on, 7.37) ? "ok" : "FAIL");
    std::printf("check scene deterministic:        %s\n", sceneAt(on, 5.5) == sceneAt(on, 5.5) ? "ok" : "FAIL");
    std::printf("check strip power 0 static:       %s\n", stripAt(off, 1.0) == stripAt(off, 7.37) ? "ok" : "FAIL");
    std::printf("check strip reducedMotion static: %s\n", stripAt(rm, 1.0) == stripAt(rm, 7.37) ? "ok" : "FAIL");
    std::printf("check strip on animates:          %s\n", stripAt(on, 1.0) != stripAt(on, 7.37) ? "ok" : "FAIL");

    // Anchors without a target equal the anchors with one.
    art::SceneAnchors a{}, b{};
    art::DrawScene(nullptr, D2D1::RectF(10, 20, 370, 188), on, &a);
    Pixels([&](ID2D1RenderTarget* rt) { art::DrawScene(rt, D2D1::RectF(10, 20, 370, 188), on, &b); }, 380, 200);
    std::printf("check anchors (null target):      %s  led %.1f,%.1f louver %.1f,%.1f room %.1f,%.1f wall %.1f,%.1f\n",
                std::memcmp(&a, &b, sizeof a) == 0 ? "ok" : "FAIL", a.led.x, a.led.y, a.louver.x, a.louver.y, a.room.x, a.room.y, a.wall.x, a.wall.y);

    // Bad inputs must not crash.
    art::Scene bad = Base(99);
    bad.fromMode = -3; bad.modeMix = 7; bad.power = 3; bad.fan = -2; bad.flap = 9; bad.sway = -40;
    bad.targetTemperature = std::numeric_limits<float>::quiet_NaN(); bad.time = 1e9;
    Pixels([&](ID2D1RenderTarget* rt) {
        art::DrawScene(rt, D2D1::RectF(0, 0, 360, 168), bad);
        art::DrawScene(rt, D2D1::RectF(0, 0, 3, 2), bad);
        art::DrawStripGlyph(rt, D2D1::RectF(0, 0, 34, 24), bad);
        art::DrawLouverPosition(rt, 77, true, D2D1::RectF(0, 0, 20, 20), D2D1::ColorF(1, 1, 1));
        art::DrawLouverPosition(rt, -9, false, D2D1::RectF(0, 0, 20, 20), D2D1::ColorF(1, 1, 1));
        art::DrawGlyph(rt, static_cast<art::Glyph>(999), D2D1::RectF(0, 0, 20, 20), D2D1::ColorF(1, 1, 1));
    }, 360, 168);
    std::printf("check bad inputs:                 ok\n");

    // Cost on the (software) WIC target: a pessimistic bound for the hardware HWND target.
    Canvas c;
    if (!Make(c, 720, 336)) return;
    auto time = [&](const wchar_t* what, int n, const std::function<void(int)>& f) {
        LARGE_INTEGER q0, q1, fr;
        QueryPerformanceFrequency(&fr);
        QueryPerformanceCounter(&q0);
        for (int i = 0; i < n; ++i) {
            c.rt->BeginDraw();
            c.rt->Clear(D2D1::ColorF(0x202020));
            f(i);
            c.rt->EndDraw();
        }
        QueryPerformanceCounter(&q1);
        std::printf("cost %-34ls %.3f ms/frame\n", what, 1000.0 * static_cast<double>(q1.QuadPart - q0.QuadPart) / static_cast<double>(fr.QuadPart) / n);
    };
    time(L"empty frame (clear only)", 300, [&](int) {});
    art::Scene heavy = Base(art::Cool); heavy.fan = 0.8f; heavy.purifier = true; heavy.eco = true;
    time(L"scene 360x168 @1x cool fan .8", 300, [&](int i) { heavy.time = i / 60.0; art::DrawScene(c.rt.Get(), D2D1::RectF(0, 0, 360, 168), heavy); });
    time(L"scene 360x168 @2x cool fan .8", 300, [&](int i) {
        heavy.time = i / 60.0;
        c.rt->SetTransform(D2D1::Matrix3x2F::Scale(2, 2));
        art::DrawScene(c.rt.Get(), D2D1::RectF(0, 0, 360, 168), heavy);
        c.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    });
    time(L"strip glyph 34x24", 300, [&](int i) { heavy.time = i / 60.0; art::DrawStripGlyph(c.rt.Get(), D2D1::RectF(0, 0, 34, 24), heavy); });
    time(L"all 37 glyphs @20", 300, [&](int i) {
        for (int g = 0; g <= static_cast<int>(art::Glyph::Info); ++g)
            art::DrawGlyph(c.rt.Get(), static_cast<art::Glyph>(g), D2D1::RectF(0, 0, 20, 20), D2D1::ColorF(1, 1, 1), i / 60.0);
    });
}

// The same scene on a hardware HWND target (hidden window): the number that matters for the panel.
void HwndCost() {
    HWND wnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 720, 336, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!wnd) { std::printf("hwnd cost: no window\n"); return; }
    ComPtr<ID2D1HwndRenderTarget> rt;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(wnd, D2D1::SizeU(720, 336), D2D1_PRESENT_OPTIONS_IMMEDIATELY), &rt))) {
        std::printf("hwnd cost: no target\n");
        DestroyWindow(wnd);
        return;
    }
    auto run = [&](const wchar_t* what, art::Scene s, float scale) {
        LARGE_INTEGER q0{}, q1{}, fr{};
        QueryPerformanceFrequency(&fr);
        const int n = 600;
        for (int pass = 0; pass < 2; ++pass) {  // first pass warms up
            QueryPerformanceCounter(&q0);
            for (int i = 0; i < n; ++i) {
                s.time = i / 60.0;
                rt->BeginDraw();
                rt->Clear(D2D1::ColorF(0x202020));
                rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
                art::DrawScene(rt.Get(), D2D1::RectF(0, 0, 360, 168), s);
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
                art::DrawStripGlyph(rt.Get(), D2D1::RectF(0, 0, 34, 24), s);
                rt->EndDraw();
            }
            QueryPerformanceCounter(&q1);
        }
        std::printf("hwnd cost %-30ls %.3f ms/frame (scene + strip + clear + present)\n", what,
                    1000.0 * static_cast<double>(q1.QuadPart - q0.QuadPart) / static_cast<double>(fr.QuadPart) / n);
    };
    art::Scene cool = Base(art::Cool); cool.fan = 0.8f; cool.purifier = true; cool.eco = true;
    art::Scene autoMode = Base(art::Auto); autoMode.fan = 0.8f; autoMode.purifier = true;
    art::Scene off = Base(art::Cool); off.power = 0;
    run(L"cool fan .8 @1x", cool, 1.0f);
    run(L"cool fan .8 @2x", cool, 2.0f);
    run(L"auto fan .8 @2x (7 colours)", autoMode, 2.0f);
    run(L"off @2x", off, 2.0f);
    rt.Reset();
    DestroyWindow(wnd);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    g_out = argc > 1 ? argv[1] : L".";
    CreateDirectoryW(g_out.c_str(), nullptr);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf()))) return 1;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) return 1;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(g_dw.GetAddressOf())))) return 1;
        g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &g_font);

        std::vector<Cell> cells = SceneCells();
        for (int theme = 0; theme < 2; ++theme) {
            bool light = theme == 1;
            std::wstring th = light ? L"light" : L"dark";
            // 1. Scene contact sheets: everything at 1x on one sheet, then 2x in pages of 8.
            SceneSheet(L"scene_" + th + L"_1x", cells, 0, cells.size(), 4, 1.0f, light);
            for (size_t first = 0, page = 1; first < cells.size(); first += 8, ++page)
                SceneSheet(L"scene_" + th + L"_2x_" + std::to_wstring(page), cells, first, 8, 2, 2.0f, light);
            // The scene a little shorter (140 DIPs), with the leader-line anchors marked.
            SceneSheet(L"scene_" + th + L"_short140_anchors", cells, 0, 8, 2, 2.0f, light, 140.0f, true);
            {
                // Power 0 -> 1: the unit glides from the middle to the top, clear of the overlays.
                std::vector<Cell> sweep;
                for (float pw : { 0.0f, 0.15f, 0.3f, 0.5f, 0.7f, 0.85f, 0.95f, 1.0f }) {
                    art::Scene s = Base(art::Cool); s.power = pw; s.sleep = true; s.sway = pw > 0.85f ? 1.0f : 0.0f;
                    wchar_t b[48]; swprintf(b, 48, L"power %.1f (sleep%s)", pw, pw > 0.85f ? L", sway +1" : L"");
                    sweep.push_back({ b, s });
                }
                SceneSheet(L"scene_" + th + L"_power_sweep", sweep, 0, 8, 2, 2.0f, light, kSceneH, true);
            }
            HoverSheet(L"glyphs_hover_" + th, light);
            // 2. Filmstrips
            Filmstrip(L"film_cool_" + th, art::Cool, light);
            Filmstrip(L"film_heat_" + th, art::Heat, light);
            // 3. Strip glyph, 4. icons
            StripSheet(L"strip_" + th, light ? 1 : 0);
            StripFilm(L"strip_film_" + th, light ? 1 : 0);
            StripHiDpi(L"strip_hidpi_" + th, light ? 1 : 0);
            if (!light) {
                StripSheet(L"strip_greenblack", 2);
                StripFilm(L"strip_film_greenblack", 2);
                StripHiDpi(L"strip_hidpi_greenblack", 2);
            }
            GlyphSheet(L"glyphs_" + th, light);
            LouverSheet(L"louver_" + th, light);
        }
        Checks();
        HwndCost();
        g_font.Reset(); g_dw.Reset(); g_wic.Reset(); g_d2d.Reset();
    }
    CoUninitialize();
    return 0;
}
