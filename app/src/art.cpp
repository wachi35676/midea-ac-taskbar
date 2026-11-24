// Art for the AC app: see art.h for the contract. Pure Direct2D drawing, no state, no caches:
// every call creates the few brushes/geometries it needs and releases them before returning.
#include "art.h"

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

namespace art {
namespace {

using Col = D2D1_COLOR_F;
using Pt = D2D1_POINT_2F;

constexpr float kPi = 3.14159265f;
constexpr float kTau = 6.28318531f;

// ---------------------------------------------------------------- small math / colour helpers

Col Rgb(unsigned hex, float a = 1.0f) {
    return Col{ static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
                static_cast<float>(hex & 0xFF) / 255.0f, a };
}
Col Mix(const Col& a, const Col& b, float t) {
    return Col{ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}
Col Fade(Col c, float a) { c.a *= a; return c; }
float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float Sat(float v) { return Clamp(v, 0.0f, 1.0f); }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }
float Smooth(float e0, float e1, float x) {
    float t = Sat((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}
float Finite(float v, float fallback) { return (v == v && v > -1e9f && v < 1e9f) ? v : fallback; }
Pt P(float x, float y) { return Pt{ x, y }; }

// Deterministic 0..1 hash of two small integers (no rand, no statics).
float Hash(int i, int salt) {
    unsigned x = static_cast<unsigned>(i) * 0x9E3779B1u ^ (static_cast<unsigned>(salt) * 0x85EBCA77u + 0xC2B2AE3Du);
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return static_cast<float>(x & 0xFFFFFFu) / 16777216.0f;
}
// Fractional part, computed in double so hours of uptime do not cost precision.
float Fract(double v) { return static_cast<float>(v - std::floor(v)); }

// ---------------------------------------------------------------- path builder

class PathB {
public:
    explicit PathB(ID2D1Factory* f, bool alternate = false) {
        if (f && SUCCEEDED(f->CreatePathGeometry(&g_)) && SUCCEEDED(g_->Open(&s_)))
            s_->SetFillMode(alternate ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
    }
    void Begin(Pt p, bool filled = true) {
        End(false);
        if (s_) { s_->BeginFigure(p, filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW); in_ = true; }
    }
    void Line(Pt p) { if (in_) s_->AddLine(p); }
    void Bez(Pt c1, Pt c2, Pt p) { if (in_) s_->AddBezier(D2D1::BezierSegment(c1, c2, p)); }
    void Quad(Pt c, Pt p) { if (in_) s_->AddQuadraticBezier(D2D1::QuadraticBezierSegment(c, p)); }
    void Arc(Pt p, float rad, bool clockwise, bool large = false) {
        if (in_) s_->AddArc(D2D1::ArcSegment(p, D2D1::SizeF(rad, rad), 0.0f,
            clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
            large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    }
    void End(bool closed) {
        if (in_) { s_->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN); in_ = false; }
    }
    void RoundRect(float l, float t, float r, float b, float rad) {
        rad = std::max(0.0f, std::min(rad, std::min((r - l), (b - t)) * 0.5f));
        Begin(P(l + rad, t));
        Line(P(r - rad, t)); Arc(P(r, t + rad), rad, true);
        Line(P(r, b - rad)); Arc(P(r - rad, b), rad, true);
        Line(P(l + rad, b)); Arc(P(l, b - rad), rad, true);
        Line(P(l, t + rad)); Arc(P(l + rad, t), rad, true);
        End(true);
    }
    ID2D1PathGeometry* Done() {
        End(false);
        if (s_) { s_->Close(); s_.Reset(); }
        return g_.Get();
    }
private:
    ComPtr<ID2D1PathGeometry> g_;
    ComPtr<ID2D1GeometrySink> s_;
    bool in_ = false;
};

// ---------------------------------------------------------------- painter: one brush, one stroke style

struct Painter {
    ID2D1RenderTarget* rt;
    ComPtr<ID2D1Factory> f;
    ComPtr<ID2D1SolidColorBrush> b;
    ComPtr<ID2D1StrokeStyle> round;
    bool ok = false;

    explicit Painter(ID2D1RenderTarget* target) : rt(target) {
        if (!rt) return;
        rt->GetFactory(&f);
        if (!f) return;
        if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &b))) return;
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        if (FAILED(f->CreateStrokeStyle(sp, nullptr, 0, &round))) return;
        ok = true;
    }
    ID2D1Brush* B(const Col& c) { b->SetColor(c); return b.Get(); }

    void Line(Pt a, Pt c, const Col& col, float w) { rt->DrawLine(a, c, B(col), w, round.Get()); }
    void Dot(Pt c, float r, const Col& col) { rt->FillEllipse(D2D1::Ellipse(c, r, r), B(col)); }
    void Ring(Pt c, float r, const Col& col, float w) { rt->DrawEllipse(D2D1::Ellipse(c, r, r), B(col), w); }
    void FillRR(float l, float t, float r, float bt, float rad, const Col& col) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l, t, r, bt), rad, rad), B(col));
    }
    void Fill(ID2D1Geometry* g, const Col& col) { if (g) rt->FillGeometry(g, B(col)); }
    void Stroke(ID2D1Geometry* g, const Col& col, float w) { if (g) rt->DrawGeometry(g, B(col), w, round.Get()); }

    ComPtr<ID2D1LinearGradientBrush> Gradient(Pt p0, Pt p1, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1LinearGradientBrush> br;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops.begin(), static_cast<UINT32>(stops.size()), &coll)))
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(p0, p1), coll.Get(), &br);
        return br;
    }
};

Pt Polar(Pt c, float r, float deg) {
    float a = deg * kPi / 180.0f;
    return P(c.x + r * std::cos(a), c.y + r * std::sin(a));
}

// Mode helpers ------------------------------------------------------------------------------

int SafeMode(int m) { return (m >= Auto && m <= Fan) ? m : Fan; }

// ---------------------------------------------------------------- glyph toolkit (20-unit box)

struct G {
    Painter& p;
    Col c;
    float sw;  // stroke width in box units

    void L(float x1, float y1, float x2, float y2) { p.Line(P(x1, y1), P(x2, y2), c, sw); }
    void LA(float x1, float y1, float x2, float y2, float a) { p.Line(P(x1, y1), P(x2, y2), Fade(c, a), sw); }
    void Dot(float x, float y, float r) { p.Dot(P(x, y), r, c); }
    void Circ(float x, float y, float r) { p.Ring(P(x, y), r, c, sw); }
    void Poly(std::initializer_list<Pt> pts, bool closed) {
        PathB b(p.f.Get());
        bool first = true;
        for (const Pt& q : pts) { if (first) { b.Begin(q, false); first = false; } else b.Line(q); }
        b.End(closed);
        p.Stroke(b.Done(), c, sw);
    }
    // Arc from deg0 to deg1 (clockwise on screen when deg1 > deg0).
    void Arc(float cx, float cy, float r, float deg0, float deg1) {
        PathB b(p.f.Get());
        b.Begin(Polar(P(cx, cy), r, deg0), false);
        float mid = (deg0 + deg1) * 0.5f;
        b.Arc(Polar(P(cx, cy), r, mid), r, deg1 > deg0);
        b.Arc(Polar(P(cx, cy), r, deg1), r, deg1 > deg0);
        p.Stroke(b.Done(), c, sw);
    }
    // Open arrow head at `tip`, pointing along `deg`.
    void Head(float x, float y, float deg, float size) {
        Pt t = P(x, y);
        Pt a = Polar(t, size, deg + 180.0f - 42.0f);
        Pt b2 = Polar(t, size, deg + 180.0f + 42.0f);
        Poly({ a, t, b2 }, false);
    }
    void Stroke(PathB& b) { p.Stroke(b.Done(), c, sw); }
    void Fill(PathB& b) { p.Fill(b.Done(), c); }
};

// Extra transform for a hover motion, undone on scope exit.
struct Xf {
    ID2D1RenderTarget* rt;
    D2D1_MATRIX_3X2_F old;
    Xf(G& g, const D2D1_MATRIX_3X2_F& m) : rt(g.p.rt) {
        rt->GetTransform(&old);
        rt->SetTransform(m * old);
    }
    ~Xf() { rt->SetTransform(old); }
};
D2D1_MATRIX_3X2_F Rot(float deg, float cx, float cy) { return D2D1::Matrix3x2F::Rotation(deg, P(cx, cy)); }
D2D1_MATRIX_3X2_F Shift(float x, float y) { return D2D1::Matrix3x2F::Translation(x, y); }
D2D1_MATRIX_3X2_F Zoom(float k, float cx, float cy) { return D2D1::Matrix3x2F::Scale(k, k, P(cx, cy)); }

// Hover loops: all are 0 at t = 0, so the resting pose is the pose at t = 0.
float Loop(double t, double period) { return Fract(t / period); }                              // 0..1 sawtooth
float Swing(double t, double period) { return std::sin(Loop(t, period) * kTau); }              // -1..1
float Pulse01(double t, double period) { float v = std::sin(Loop(t, period) * kPi); return v * v; }  // 0..1..0

void Droplet(G& g, float cx, float top, float r, bool fill) {
    // Pointed top, round bottom.
    float cy = top + r * 1.72f;
    PathB b(g.p.f.Get());
    b.Begin(P(cx, top), fill);
    b.Bez(P(cx - r * 0.25f, top + r * 0.55f), P(cx - r, cy - r * 0.95f), P(cx - r, cy));
    b.Arc(P(cx + r, cy), r, false);
    b.Bez(P(cx + r, cy - r * 0.95f), P(cx + r * 0.25f, top + r * 0.55f), P(cx, top));
    b.End(true);
    if (fill) g.Fill(b); else g.Stroke(b);
}

void Star4(G& g, float cx, float cy, float r, bool fill) {
    float k = r * 0.16f;
    PathB b(g.p.f.Get());
    b.Begin(P(cx, cy - r), fill);
    b.Quad(P(cx + k, cy - k), P(cx + r, cy));
    b.Quad(P(cx + k, cy + k), P(cx, cy + r));
    b.Quad(P(cx - k, cy + k), P(cx - r, cy));
    b.Quad(P(cx - k, cy - k), P(cx, cy - r));
    b.End(true);
    if (fill) g.Fill(b); else g.Stroke(b);
}

void Snowflake(G& g, float cx, float cy, float r, bool branches) {
    for (int k = 0; k < 3; ++k) {
        float a = (90.0f + 60.0f * static_cast<float>(k)) * kPi / 180.0f;
        float dx = std::cos(a), dy = std::sin(a);
        g.L(cx - dx * r, cy - dy * r, cx + dx * r, cy + dy * r);
        if (!branches) continue;
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            float s = static_cast<float>(sgn);
            float bx = cx + s * dx * r * 0.56f, by = cy + s * dy * r * 0.56f;
            float ex = cx + s * dx * r * 0.90f, ey = cy + s * dy * r * 0.90f;
            float nx = -dy * r * 0.30f, ny = dx * r * 0.30f;
            g.L(bx, by, ex + nx, ey + ny);
            g.L(bx, by, ex - nx, ey - ny);
        }
    }
}

// Three-blade fan centred at (cx, cy) with outer radius r, rotated by `rot` degrees.
void FanBlades(G& g, float cx, float cy, float r, float rot) {
    for (int k = 0; k < 3; ++k) {
        float a = (rot + 120.0f * static_cast<float>(k)) * kPi / 180.0f;
        float ca = std::cos(a), sa = std::sin(a);
        auto M = [&](float x, float y) { return P(cx + (x * ca - y * sa) * r, cy + (x * sa + y * ca) * r); };
        PathB b(g.p.f.Get());
        b.Begin(M(0.16f, 0.0f), false);
        b.Bez(M(0.30f, -0.56f), M(0.92f, -0.62f), M(1.0f, -0.16f));
        b.Bez(M(1.04f, 0.16f), M(0.56f, 0.22f), M(0.16f, 0.0f));
        b.End(true);
        g.Stroke(b);
    }
    g.Dot(cx, cy, std::max(0.9f, r * 0.13f));
}

void FanLevel(G& g, int bars, bool autoBadge, double t) {
    static const float kSpeed[5] = { 100.0f, 140.0f, 190.0f, 260.0f, 340.0f };  // degrees per second
    float speed = autoBadge ? 150.0f : kSpeed[std::clamp(bars, 0, 4)];
    float rot = -90.0f + Fract(t * static_cast<double>(speed) / 120.0) * 120.0f;  // 3-fold symmetric
    FanBlades(g, 7.0f, 10.0f, 5.4f, rot);
    if (autoBadge) {
        g.Poly({ P(13.6f, 13.6f), P(15.9f, 6.4f), P(18.2f, 13.6f) }, false);
        g.L(14.5f, 11.2f, 17.3f, 11.2f);
        return;
    }
    for (int i = 0; i < 4; ++i) {
        float y = 15.4f - 3.6f * static_cast<float>(i);
        g.LA(14.6f, y, 17.9f, y, i < bars ? 1.0f : 0.28f);
    }
}

void LouverDiagram(G& g, int index, bool horizontal) {
    index = index < 0 ? -1 : std::min(index, 4);
    float f = index < 0 ? 0.5f : static_cast<float>(index) / 4.0f;
    Pt pivot;
    float a0, a1;
    if (!horizontal) {
        // Side view: the unit is the block at the top left, the louver hangs off its lower right corner.
        PathB b(g.p.f.Get());
        b.RoundRect(2.4f, 2.6f, 8.4f, 7.6f, 1.6f);
        g.Stroke(b);
        pivot = P(7.2f, 9.6f);
        a0 = 6.0f; a1 = 78.0f;
    } else {
        // Top view: the unit is the bar at the top, the air leaves downwards, left to right.
        PathB b(g.p.f.Get());
        b.RoundRect(3.0f, 2.4f, 17.0f, 5.6f, 1.5f);
        g.Stroke(b);
        pivot = P(10.0f, 7.4f);
        a0 = 140.0f; a1 = 40.0f;
    }
    float ang = Lerp(a0, a1, f);
    if (index < 0) {
        // Free / swinging: the range as an arc with an arrow head on both ends.
        float r = 8.6f;
        float lo = std::min(a0, a1) + 4.0f, hi = std::max(a0, a1) - 4.0f;
        g.Arc(pivot.x, pivot.y, r, lo, hi);
        Pt e0 = Polar(pivot, r, lo), e1 = Polar(pivot, r, hi);
        g.Head(e0.x, e0.y, lo - 90.0f + 14.0f, 2.3f);
        g.Head(e1.x, e1.y, hi + 90.0f - 14.0f, 2.3f);
        Pt l1 = Polar(pivot, 4.6f, ang);
        g.L(pivot.x, pivot.y, l1.x, l1.y);
        return;
    }
    Pt l1 = Polar(pivot, 3.6f, ang);
    g.L(pivot.x, pivot.y, l1.x, l1.y);
    Pt s0 = Polar(pivot, 6.2f, ang), s1 = Polar(pivot, 10.4f, ang);
    g.L(s0.x, s0.y, s1.x, s1.y);
    g.Head(s1.x, s1.y, ang, 2.7f);
}

void GlyphBody(G& g, Glyph id, double t) {
    switch (id) {
    case Glyph::Power:
        g.Arc(10.0f, 10.8f, 6.3f, -52.0f, 232.0f);
        g.L(10.0f, 3.0f, 10.0f, 9.6f);
        break;
    case Glyph::Plus:
        g.L(10.0f, 4.5f, 10.0f, 15.5f);
        g.L(4.5f, 10.0f, 15.5f, 10.0f);
        break;
    case Glyph::Minus:
        g.L(4.5f, 10.0f, 15.5f, 10.0f);
        break;
    case Glyph::Refresh: {
        g.Arc(10.0f, 10.6f, 6.3f, 48.0f, 328.0f);
        Pt e = Polar(P(10.0f, 10.6f), 6.3f, 328.0f);
        g.Poly({ P(e.x + 0.3f, e.y - 4.2f), P(e.x + 0.3f, e.y + 0.2f), P(e.x - 4.1f, e.y + 0.2f) }, false);
        break;
    }
    case Glyph::Settings: {
        PathB b(g.p.f.Get());
        Pt c = P(10.0f, 10.0f);
        for (int k = 0; k < 8; ++k) {
            float a = 45.0f * static_cast<float>(k);
            Pt p0 = Polar(c, 5.5f, a - 13.0f);
            if (k == 0) b.Begin(p0, false); else b.Line(p0);
            b.Line(Polar(c, 7.5f, a - 7.5f));
            b.Line(Polar(c, 7.5f, a + 7.5f));
            b.Line(Polar(c, 5.5f, a + 13.0f));
        }
        b.End(true);
        g.Stroke(b);
        g.Circ(10.0f, 10.0f, 2.3f);
        break;
    }
    case Glyph::Close:
        g.L(5.2f, 5.2f, 14.8f, 14.8f);
        g.L(14.8f, 5.2f, 5.2f, 14.8f);
        break;
    case Glyph::Chevron:
        g.Poly({ P(7.8f, 5.0f), P(12.8f, 10.0f), P(7.8f, 15.0f) }, false);
        break;
    case Glyph::ModeAuto: {
        // The ring turns half a turn per loop (it has two gaps, so that is seamless).
        Xf x(g, Rot(Loop(t, 1.2) * 180.0f, 10.0f, 10.0f));
        g.Arc(10.0f, 10.0f, 7.2f, -72.0f, 72.0f);
        g.Arc(10.0f, 10.0f, 7.2f, 108.0f, 252.0f);
    }
        g.Poly({ P(7.0f, 13.6f), P(10.0f, 6.2f), P(13.0f, 13.6f) }, false);
        g.L(8.1f, 11.0f, 11.9f, 11.0f);
        break;
    case Glyph::ModeCool: {
        Xf x(g, Rot(Loop(t, 1.2) * 60.0f, 10.0f, 10.0f));
        Snowflake(g, 10.0f, 10.0f, 7.2f, true);
        break;
    }
    case Glyph::ModeDry: {
        Xf x(g, Shift(0.0f, Swing(t, 1.1) * 0.8f));
        Droplet(g, 10.0f, 2.8f, 5.3f, false);
        PathB b(g.p.f.Get());
        b.Begin(P(7.5f, 12.4f), false);
        b.Bez(P(7.6f, 13.9f), P(8.6f, 14.9f), P(10.0f, 15.0f));
        g.Stroke(b);
        break;
    }
    case Glyph::ModeHeat: {
        g.Circ(10.0f, 10.0f, 3.1f);
        float turn = Loop(t, 1.2) * 45.0f;
        for (int k = 0; k < 8; ++k) {
            Pt a = Polar(P(10.0f, 10.0f), 5.6f, turn + 45.0f * static_cast<float>(k));
            Pt b2 = Polar(P(10.0f, 10.0f), 7.5f, turn + 45.0f * static_cast<float>(k));
            g.L(a.x, a.y, b2.x, b2.y);
        }
        break;
    }
    case Glyph::ModeFan:
        FanBlades(g, 10.0f, 10.0f, 7.4f, -90.0f + Loop(t, 1.0) * 120.0f);
        break;
    case Glyph::FanSilent: FanLevel(g, 0, false, t); break;
    case Glyph::FanLow:    FanLevel(g, 1, false, t); break;
    case Glyph::FanMedium: FanLevel(g, 2, false, t); break;
    case Glyph::FanHigh:   FanLevel(g, 3, false, t); break;
    case Glyph::FanMax:    FanLevel(g, 4, false, t); break;
    case Glyph::FanAuto:   FanLevel(g, 0, true, t); break;
    case Glyph::SwingOff:
        g.L(10.0f, 3.6f, 10.0f, 16.4f);
        g.Head(10.0f, 3.4f, -90.0f, 3.4f);
        g.Head(10.0f, 16.6f, 90.0f, 3.4f);
        g.L(4.6f, 14.6f, 15.4f, 5.4f);
        break;
    case Glyph::SwingVertical:
        g.L(10.0f, 3.6f, 10.0f, 16.4f);
        g.Head(10.0f, 3.4f, -90.0f, 3.4f);
        g.Head(10.0f, 16.6f, 90.0f, 3.4f);
        break;
    case Glyph::SwingHorizontal:
        g.L(3.6f, 10.0f, 16.4f, 10.0f);
        g.Head(3.4f, 10.0f, 180.0f, 3.4f);
        g.Head(16.6f, 10.0f, 0.0f, 3.4f);
        break;
    case Glyph::SwingBoth:
        g.L(10.0f, 3.0f, 10.0f, 17.0f);
        g.L(3.0f, 10.0f, 17.0f, 10.0f);
        g.Head(10.0f, 2.8f, -90.0f, 2.7f);
        g.Head(10.0f, 17.2f, 90.0f, 2.7f);
        g.Head(2.8f, 10.0f, 180.0f, 2.7f);
        g.Head(17.2f, 10.0f, 0.0f, 2.7f);
        break;
    case Glyph::LouverPosition:
        LouverDiagram(g, 2, false);
        break;
    case Glyph::Eco: {
        Xf x(g, Rot(Swing(t, 1.2) * 7.0f, 3.6f, 16.6f));
        PathB b(g.p.f.Get());
        b.Begin(P(4.8f, 15.2f), false);
        b.Bez(P(3.6f, 8.4f), P(8.6f, 3.9f), P(16.2f, 4.0f));
        b.Bez(P(16.4f, 11.2f), P(11.8f, 16.2f), P(4.8f, 15.2f));
        b.End(true);
        g.Stroke(b);
        g.L(3.4f, 16.8f, 4.8f, 15.2f);
        g.L(4.8f, 15.2f, 11.4f, 8.8f);
        break;
    }
    case Glyph::Turbo: {
        float k = Pulse01(t, 0.8);
        float d1 = 0.6f * k, d2 = 1.5f * k;
        g.Poly({ P(4.8f + d1, 5.4f), P(9.4f + d1, 10.0f), P(4.8f + d1, 14.6f) }, false);
        g.Poly({ P(10.6f + d2, 5.4f), P(15.2f + d2, 10.0f), P(10.6f + d2, 14.6f) }, false);
        break;
    }
    case Glyph::Sleep: {
        Xf x(g, Rot(Swing(t, 1.2) * 9.0f, 10.5f, 10.0f));
        PathB b(g.p.f.Get());
        b.Begin(P(16.9f, 11.0f), false);
        b.Arc(P(9.0f, 3.1f), 7.2f, true, true);
        b.Arc(P(16.9f, 11.0f), 5.7f, false, false);
        b.End(true);
        g.Stroke(b);
        break;
    }
    case Glyph::Purifier:
    {
        float k = Pulse01(t, 1.0);
        Star4(g, 8.6f, 11.0f, 6.0f * (1.0f - 0.14f * k), false);
        Star4(g, 15.6f, 5.0f, 2.6f * (1.0f + 0.30f * k), false);
        break;
    }
    case Glyph::BreezeAway: {
        float k = Pulse01(t, 1.0);
        Xf x(g, Shift(0.7f * k, -0.9f * k));
        PathB b(g.p.f.Get());
        b.Begin(P(3.0f, 9.4f), false);
        b.Line(P(9.0f, 9.4f));
        b.Bez(P(12.6f, 9.4f), P(14.6f, 7.6f), P(14.9f, 4.0f));
        g.Stroke(b);
        g.Head(14.95f, 3.4f, -86.0f, 3.0f);
        PathB b2(g.p.f.Get());
        b2.Begin(P(3.0f, 13.4f), false);
        b2.Line(P(10.5f, 13.4f));
        b2.Bez(P(14.0f, 13.4f), P(16.6f, 12.4f), P(17.6f, 9.6f));
        g.Stroke(b2);
        break;
    }
    case Glyph::FreezeGuard: {
        PathB b(g.p.f.Get());
        b.Begin(P(10.0f, 2.6f), false);
        b.Line(P(16.0f, 4.9f));
        b.Line(P(16.0f, 9.8f));
        b.Bez(P(16.0f, 13.8f), P(13.2f, 16.4f), P(10.0f, 17.6f));
        b.Bez(P(6.8f, 16.4f), P(4.0f, 13.8f), P(4.0f, 9.8f));
        b.Line(P(4.0f, 4.9f));
        b.End(true);
        g.Stroke(b);
        Xf x(g, Rot(Loop(t, 1.2) * 60.0f, 10.0f, 9.9f));
        Snowflake(g, 10.0f, 9.9f, 3.1f, false);
        break;
    }
    case Glyph::Beep:
        g.Poly({ P(3.4f, 8.0f), P(6.4f, 8.0f), P(10.2f, 4.6f), P(10.2f, 15.4f), P(6.4f, 12.0f), P(3.4f, 12.0f) }, true);
        g.Arc(10.4f, 10.0f, 3.3f, -42.0f, 42.0f);
        {
            G wave = g;
            wave.c = Fade(g.c, 1.0f - 0.65f * Pulse01(t, 0.9));
            wave.Arc(10.4f, 10.0f, 6.4f, -42.0f, 42.0f);
        }
        break;
    case Glyph::Fahrenheit:
        g.Circ(5.0f, 6.0f, 1.7f);
        g.Poly({ P(15.4f, 5.0f), P(10.0f, 5.0f), P(10.0f, 15.4f) }, false);
        g.L(10.0f, 10.0f, 14.2f, 10.0f);
        break;
    case Glyph::SelfClean: {
        Star4(g, 10.0f, 10.0f, 3.5f * (1.0f - 0.2f * Pulse01(t, 1.2)), true);
        Xf x(g, Rot(Loop(t, 1.2) * 180.0f, 10.0f, 10.0f));
        g.Arc(10.0f, 10.0f, 6.9f, 20.0f, 160.0f);
        g.Arc(10.0f, 10.0f, 6.9f, 200.0f, 340.0f);
        Pt e1 = Polar(P(10.0f, 10.0f), 6.9f, 160.0f);
        Pt e2 = Polar(P(10.0f, 10.0f), 6.9f, 340.0f);
        g.Head(e1.x, e1.y, 160.0f + 78.0f, 2.6f);
        g.Head(e2.x, e2.y, 340.0f + 78.0f, 2.6f);
        break;
    }
    case Glyph::Thermometer: {
        PathB b(g.p.f.Get());
        b.Begin(P(11.9f, 11.3f), false);
        b.Line(P(11.9f, 4.2f));
        b.Arc(P(8.1f, 4.2f), 1.9f, false);
        b.Line(P(8.1f, 11.3f));
        b.Arc(P(11.9f, 11.3f), 3.5f, false, true);
        b.End(true);
        g.Stroke(b);
        g.Dot(10.0f, 14.2f, 1.25f);
        g.L(10.0f, 8.6f, 10.0f, 14.0f);
        break;
    }
    case Glyph::Outdoor:
        g.Poly({ P(2.6f, 16.0f), P(7.6f, 8.0f), P(10.8f, 13.0f), P(13.0f, 10.2f), P(17.4f, 16.0f) }, true);
        g.Circ(14.6f, 5.0f, 1.9f);
        break;
    case Glyph::Bolt:
        g.Poly({ P(10.9f, 2.6f), P(4.2f, 11.2f), P(9.6f, 11.2f), P(9.0f, 17.4f), P(15.8f, 8.8f), P(10.4f, 8.8f) }, true);
        break;
    case Glyph::Droplet:
        Droplet(g, 10.0f, 2.8f, 5.3f, false);
        break;
    case Glyph::Info:
        g.Circ(10.0f, 10.0f, 7.2f);
        g.L(10.0f, 9.4f, 10.0f, 13.8f);
        g.Dot(10.0f, 6.4f, 1.0f);
        break;
    }
}

// Sets up the 20-unit box inside r (largest centred square), composing with the caller's transform.
struct BoxScope {
    ID2D1RenderTarget* rt;
    D2D1_MATRIX_3X2_F old;
    float scale;
    BoxScope(ID2D1RenderTarget* target, const D2D1_RECT_F& r) : rt(target) {
        rt->GetTransform(&old);
        rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        float w = r.right - r.left, h = r.bottom - r.top;
        float side = std::min(w, h);
        scale = side / 20.0f;
        float ox = r.left + (w - side) * 0.5f, oy = r.top + (h - side) * 0.5f;
        rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(ox, oy) * old);
    }
    ~BoxScope() {
        rt->SetTransform(old);
        rt->PopAxisAlignedClip();
    }
    // 1.5 units at 20 px, never thinner than ~1.2 px on screen.
    float StrokeWidth() const { return std::max(1.5f, 1.2f / std::max(scale, 0.05f)); }
};

// ---------------------------------------------------------------- scene
// The unit is an opaque solid seen slightly from below and from the right, so three faces show: the
// front (lightest, the light comes from the upper left), the right side (a step darker) and the underside
// with the outlet (darkest). Only shadows, air and highlights are translucent.

Col Shade(const Col& c, float k) { return Col{ c.r * k, c.g * k, c.b * k, c.a }; }

struct Theme {
    Col front, frontLow, side, under, underLow;  // the solid's faces
    Col glass, slot, rim, shadow, edge, vane, ledOff, grey;
};

Theme MakeTheme(bool light) {
    Theme t;
    if (light) {
        Col body = Rgb(0xFBFBFC);
        t.front = body; t.frontLow = Shade(body, 0.955f);
        t.side = Shade(body, 0.86f); t.under = Shade(body, 0.80f); t.underLow = Shade(body, 0.73f);
        t.glass = Rgb(0x1B1C20); t.slot = Rgb(0x2A2C31);
        t.rim = Rgb(0xFFFFFF, 0.85f); t.shadow = Rgb(0x000000, 0.15f); t.edge = Rgb(0x14181E, 0.13f);
        t.vane = Rgb(0x8E949C, 0.55f); t.ledOff = Rgb(0xB4B8BE); t.grey = Rgb(0xD3D5D9);
    } else {
        Col body = Rgb(0x66666C);
        t.front = body; t.frontLow = Shade(body, 0.90f);
        t.side = Shade(body, 0.76f); t.under = Shade(body, 0.63f); t.underLow = Shade(body, 0.54f);
        t.glass = Rgb(0x0E0F11); t.slot = Rgb(0x111214);
        t.rim = Rgb(0xFFFFFF, 0.16f); t.shadow = Rgb(0x000000, 0.50f); t.edge = Rgb(0xFFFFFF, 0.0f);
        t.vane = Rgb(0x6A6C72, 0.60f); t.ledOff = Rgb(0x3C3D41); t.grey = Rgb(0x4A4A4E);
    }
    return t;
}

struct Lay {
    float s;               // scale: 1 at 360 wide (and at least 140 tall)
    float fL, fT, fR, fB;  // front face
    float dx, dy;          // screen offset from the front edge of the solid to its back edge on the wall
    float u0, u1, v0, v1;  // the outlet slot on the underside, in underside coordinates
    float airB;            // lowest y the air may reach
    D2D1_RECT_F r;

    // A point on the underside: u along the unit (0 left .. 1 right), v from the front edge (0) to the wall (1).
    Pt Under(float u, float v) const { return P(fL + (fR - fL) * u + dx * v, fB + dy * v); }
};

// The unit hangs at the top of the wall, centred, at every power value; the air has the room below it.
// The top right corner stays free for the panel's round button (about 34x34 DIPs, 10 in).
Lay MakeLay(const D2D1_RECT_F& r) {
    Lay l{};
    l.r = r;
    float w = r.right - r.left, h = r.bottom - r.top;
    l.s = std::max(0.05f, std::min(w / 360.0f, h / 140.0f));
    float s = l.s;
    l.dx = 11.0f * s; l.dy = 17.0f * s;
    float fw = 198.0f * s, fh = 35.0f * s;
    l.fL = (r.left + r.right - (fw + l.dx)) * 0.5f; l.fR = l.fL + fw;
    l.fT = r.top + 12.0f * s; l.fB = l.fT + fh;
    l.u0 = 0.065f; l.u1 = 0.935f; l.v0 = 0.24f; l.v1 = 0.80f;
    l.airB = r.bottom - 4.0f * s;
    return l;
}

// The air: where a stream that leaves the slot at lateral position a (-1..1) is after fraction u (0..1).
struct Flow {
    float cx, y0, sh, len, spread, drop, sway, heat, s, unitHalf, unitH;
    float drift;  // the unit sits right of the middle: the air eases back towards the middle of the rect
    bool away;
    float wob;  // wobble phase (radians)

    Pt At(float a, float u, int i) const {
        if (away) {
            // Deflected along the wall: out of the slot, around the end of the unit and up beside it.
            float sg = a < 0.0f ? -1.0f : 1.0f;
            float ab = std::fabs(a);
            float x0 = cx + a * sh;
            float xe = cx + sg * (unitHalf + (8.0f + (1.0f - ab) * 17.0f) * s);
            float rx = std::fabs(xe - x0) * 0.5f;
            float ry = Lerp(15.0f, 33.0f, 1.0f - ab) * s;
            if (u < 0.62f) {
                float th = kPi * (u / 0.62f);
                return P(x0 + sg * rx * (1.0f - std::cos(th)), y0 + ry * std::sin(th));
            }
            float v = (u - 0.62f) / 0.38f;
            return P(xe + sg * 3.0f * s * v * v, y0 - v * unitH * 0.60f);
        }
        float e = std::pow(u, 0.85f);
        float x = cx + a * sh * (1.0f + (spread - 1.0f) * e) + sway * 44.0f * s * u * u + drift * u * std::sqrt(u);
        float yn = u * (1.0f - heat * 0.42f * u * u) * Lerp(1.0f, 1.36f, heat);
        float sg = (a < -0.01f) ? -1.0f : 1.0f;
        x += heat * sg * (5.0f + 9.0f * std::fabs(a)) * s * u * u * u;
        x += std::sin(u * 5.2f - wob + static_cast<float>(i) * 1.7f) * 2.4f * s * u;
        return P(x, y0 + len * drop * yn);
    }
};

struct SceneCtx {
    Painter& p;
    const Scene& sc;
    Lay l;
    Theme th;
    Flow fl;
    double t;        // effective time (frozen when motion is reduced)
    float power;     // 0..1, 0 when offline
    float fan;       // 0..1
    float fanEff;    // fan with turbo / sleep folded in
    float air;       // overall airflow opacity
    float mix;       // mode crossfade
    int from, to;
};

float ModeWeight(const SceneCtx& c, int m) {
    return (c.from == m ? 1.0f - c.mix : 0.0f) + (c.to == m ? c.mix : 0.0f);
}

Col StreamColor(const SceneCtx& c, int mode, int i, int n) {
    bool light = c.sc.light;
    if (mode == Auto) {
        float k = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.5f;
        return Mix(ModeColor(Auto, light), ModeColor(Cool, light), k * 0.85f);
    }
    return ModeColor(mode, light);
}
// The mode change wipes across the streams, so each ribbon is mostly one colour or the other.
Col StreamColorMixed(const SceneCtx& c, int i, int n) {
    float off = n > 1 ? 0.6f * static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
    return Mix(StreamColor(c, c.from, i, n), StreamColor(c, c.to, i, n), Smooth(off, off + 0.4f, c.mix));
}

void Quad(Painter& p, Pt a, Pt b, Pt c2, Pt d, const Col& col) {
    PathB pb(p.f.Get());
    pb.Begin(a); pb.Line(b); pb.Line(c2); pb.Line(d); pb.End(true);
    p.Fill(pb.Done(), col);
}

// One 7-segment glyph. mask bits: a=1 b=2 c=4 d=8 e=16 f=32 g=64.
void Seg7(Painter& p, float x, float y, float w, float h, unsigned mask, const Col& col, float sw) {
    float g = sw * 0.85f, m = y + h * 0.5f;
    if (mask & 1)  p.Line(P(x + g, y), P(x + w - g, y), col, sw);
    if (mask & 2)  p.Line(P(x + w, y + g), P(x + w, m - g), col, sw);
    if (mask & 4)  p.Line(P(x + w, m + g), P(x + w, y + h - g), col, sw);
    if (mask & 8)  p.Line(P(x + g, y + h), P(x + w - g, y + h), col, sw);
    if (mask & 16) p.Line(P(x, m + g), P(x, y + h - g), col, sw);
    if (mask & 32) p.Line(P(x, y + g), P(x, m - g), col, sw);
    if (mask & 64) p.Line(P(x + g, m), P(x + w - g, m), col, sw);
}

// The display: dark glass set into the front face, lit by the mode colour when the unit is on.
void DrawDisplay(SceneCtx& c) {
    static const unsigned kDigit[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    const Lay& l = c.l;
    const Scene& sc = c.sc;
    float s = l.s;
    float dh = 10.0f * s, dw = 5.2f * s, gap = 3.8f * s, sw = 1.45f * s;

    unsigned masks[4] = {};
    int n = 0;
    bool half = false;
    if (!sc.online) { masks[0] = 0x40; masks[1] = 0x40; n = 2; }
    else if (sc.defrost) { masks[0] = 0x5E; masks[1] = 0x71; n = 2; }
    else {
        float tv = Finite(sc.targetTemperature, -1000.0f);
        if (tv < 0.0f || tv > 199.0f) { masks[0] = 0x40; masks[1] = 0x40; n = 2; }
        else {
            int twice = static_cast<int>(std::floor(tv * 2.0f + 0.5f));
            int whole = twice / 2;
            half = (twice % 2) != 0;
            if (whole >= 100) masks[n++] = kDigit[(whole / 100) % 10];
            if (whole >= 10) masks[n++] = kDigit[(whole / 10) % 10];
            masks[n++] = kDigit[whole % 10];
        }
    }
    float halfW = half ? (dw * 0.72f + 5.4f * s) : 0.0f;
    float content = static_cast<float>(n) * dw + static_cast<float>(n - 1) * gap + halfW;
    float padX = 7.0f * s, gh = 19.0f * s;
    float gR = l.fR - 9.0f * s, gL = gR - content - padX * 2.0f;
    float gT = (l.fT + l.fB) * 0.5f - gh * 0.5f, gB = gT + gh;
    Pt glassC = P((gL + gR) * 0.5f, (gT + gB) * 0.5f);

    Col tint = Mix(ModeColor(c.from, false), ModeColor(c.to, false), c.mix);
    float on = sc.online ? c.power : 0.0f;
    Col g0 = Mix(c.th.glass, tint, 0.20f * on), g1 = Mix(c.th.glass, tint, 0.05f * on);
    if (auto gr = c.p.Gradient(P(gL, gT), P(gR, gB), { { 0.0f, g0 }, { 1.0f, g1 } }))
        c.p.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(gL, gT, gR, gB), 4.5f * s, 4.5f * s), gr.Get());
    // The lip the glass sits in: dark above (in shadow), a faint light line below.
    c.p.Line(P(gL + 4.0f * s, gB + 0.5f * s), P(gR - 4.0f * s, gB + 0.5f * s), Fade(c.th.rim, 0.6f), 0.8f * s);

    Col col;
    if (!sc.online) col = Rgb(0x8A8D93, 0.75f);
    else col = Fade(Mix(tint, Rgb(0xFFFFFF), 0.25f), on);
    if (col.a <= 0.01f) return;
    float x = gR - padX;
    float y = glassC.y - dh * 0.5f;
    if (half) {
        x -= dw * 0.72f;
        Seg7(c.p, x, y + dh * 0.36f, dw * 0.72f, dh * 0.64f, kDigit[5], col, sw * 0.85f);
        x -= 2.7f * s;
        c.p.Dot(P(x, y + dh), sw * 0.6f, col);
        x -= 2.7f * s;
    }
    for (int i = n - 1; i >= 0; --i) {
        x -= dw;
        Seg7(c.p, x, y, dw, dh, masks[i], col, sw);
        x -= gap;
    }
}

// The louver flap: a thin slab hinged on the wall-side edge of the outlet. Closed it lies flush in the
// underside and covers the slot; as it opens its free edge swings down, and the air runs along it.
void DrawFlap(SceneCtx& c, float k) {
    Painter& p = c.p;
    const Lay& l = c.l;
    float s = l.s;
    float flap = Sat(Finite(c.sc.flap, 0.5f));
    float th = c.power * Lerp(26.0f, 82.0f, flap) * kPi / 180.0f;
    float dv = (l.v1 - l.v0) + 0.035f;
    float ox = -dv * l.dx * std::cos(th);
    float oy = -dv * l.dy * std::cos(th) + 14.5f * s * std::sin(th);
    float ua = l.u0 - 0.004f, ub = l.u1 + 0.004f;
    Pt h0 = l.Under(ua, l.v1 + 0.02f), h1 = l.Under(ub, l.v1 + 0.02f);
    Pt f0 = P(h0.x + ox, h0.y + oy), f1 = P(h1.x + ox, h1.y + oy);

    // Closed: part of the underside, a touch lighter. Open: its inner face turns towards the light.
    float turn = Sat(std::sin(th));
    Col face = Mix(Mix(c.th.under, c.th.front, 0.10f), Mix(c.th.side, c.th.front, 0.45f), turn);
    face = Mix(face, c.th.grey, k);
    PathB b(p.f.Get());
    b.Begin(h0); b.Line(h1); b.Line(f1); b.Line(f0); b.End(true);
    ID2D1PathGeometry* g = b.Done();
    p.Fill(g, face);
    p.Stroke(g, face, 1.3f * s);
    // The slab's thickness along the free edge, then the rim highlight.
    float tk = 1.5f * s;
    Quad(p, f0, f1, P(f1.x, f1.y + tk), P(f0.x, f0.y + tk), Mix(Shade(face, c.sc.light ? 0.80f : 0.66f), c.th.grey, k * 0.5f));
    p.Line(f0, f1, Fade(c.th.rim, 0.9f), 0.9f * s);
}

// The wall the unit hangs on: a calm plaster tone over the whole rect, lit from the upper left, with a
// faint ceiling line along the top. It fades out towards the bottom, where the room is.
void DrawWall(SceneCtx& c) {
    Painter& p = c.p;
    const D2D1_RECT_F& r = c.l.r;
    float s = c.l.s;
    bool light = c.sc.light;
    Col base = light ? Rgb(0xEFEDE9) : Rgb(0x38383B);
    if (auto gr = p.Gradient(P(0, r.top), P(0, r.bottom), { { 0.0f, base }, { 0.45f, Fade(base, 0.9f) }, { 0.82f, Fade(base, 0.35f) }, { 1.0f, Fade(base, 0.0f) } }))
        p.rt->FillRectangle(r, gr.Get());
    // Light falling across the plaster from the upper left.
    Col glow = Rgb(0xFFFFFF, light ? 0.60f : 0.055f);
    if (auto gr = p.Gradient(P(r.left, r.top), P(r.left + (r.right - r.left) * 0.62f, r.bottom), { { 0.0f, glow }, { 1.0f, Fade(glow, 0.0f) } }))
        p.rt->FillRectangle(r, gr.Get());
    // Ceiling: a slightly shaded band and the line where it meets the wall.
    float cy = r.top + 5.0f * s;
    p.rt->FillRectangle(D2D1::RectF(r.left, r.top, r.right, cy), p.B(Rgb(0x000000, light ? 0.035f : 0.16f)));
    p.rt->FillRectangle(D2D1::RectF(r.left, cy, r.right, cy + 1.0f * s), p.B(light ? Rgb(0x000000, 0.055f) : Rgb(0xFFFFFF, 0.055f)));
    if (auto gr = p.Gradient(P(0, cy + 1.0f * s), P(0, cy + 7.0f * s), { { 0.0f, Rgb(0x000000, light ? 0.030f : 0.10f) }, { 1.0f, Rgb(0x000000, 0.0f) } }))
        p.rt->FillRectangle(D2D1::RectF(r.left, cy + 1.0f * s, r.right, cy + 7.0f * s), gr.Get());
}

void DrawUnit(SceneCtx& c) {
    Painter& p = c.p;
    const Lay& l = c.l;
    const Scene& sc = c.sc;
    const Theme& th = c.th;
    float s = l.s;
    bool off = !sc.online;

    // Off: a touch flatter. Offline: mostly flat grey.
    float k = off ? 0.62f : (1.0f - c.power) * 0.12f;
    auto Tone = [&](const Col& col) { return Mix(col, th.grey, k); };

    Pt bT = P(l.fR + l.dx, l.fT + l.dy), bB = P(l.fR + l.dx, l.fB + l.dy), bL = P(l.fL + l.dx, l.fB + l.dy);

    // Contact shadow on the wall behind the unit, pushed away from the light.
    if (!off) {
        Col sh = Fade(th.shadow, 0.25f);
        for (int i = 0; i < 4; ++i) {
            float g = (1.0f + 2.4f * static_cast<float>(i)) * s;
            p.FillRR(l.fL + l.dx - g * 0.3f + 2.0f * s, l.fT + l.dy - g * 0.2f, l.fR + l.dx + g * 0.55f + 1.0f * s, l.fB + l.dy + g * 0.9f + 1.5f * s,
                     3.0f * s + g * 0.6f, sh);
        }
    }

    // Underside and right side: flat faces, stroked in their own colour so the joins are soft.
    PathB under(p.f.Get());
    under.Begin(P(l.fL, l.fB)); under.Line(P(l.fR, l.fB)); under.Line(bB); under.Line(bL); under.End(true);
    ID2D1PathGeometry* gUnder = under.Done();
    if (auto gr = p.Gradient(P(0, l.fB), P(0, l.fB + l.dy), { { 0.0f, Tone(th.under) }, { 1.0f, Tone(th.underLow) } })) {
        if (gUnder) { p.rt->FillGeometry(gUnder, gr.Get()); p.rt->DrawGeometry(gUnder, gr.Get(), 1.2f * s, p.round.Get()); }
    }
    PathB side(p.f.Get());
    side.Begin(P(l.fR, l.fT)); side.Line(bT); side.Line(bB); side.Line(P(l.fR, l.fB)); side.End(true);
    ID2D1PathGeometry* gSide = side.Done();
    p.Fill(gSide, Tone(th.side));
    p.Stroke(gSide, Tone(th.side), 1.2f * s);

    // Outlet slot, with the sideways vanes inside it.
    PathB slot(p.f.Get());
    slot.Begin(l.Under(l.u0, l.v0)); slot.Line(l.Under(l.u1, l.v0)); slot.Line(l.Under(l.u1, l.v1)); slot.Line(l.Under(l.u0, l.v1));
    slot.End(true);
    ID2D1PathGeometry* gSlot = slot.Done();
    Col slotCol = Mix(th.slot, th.grey, k * 0.6f);
    p.Fill(gSlot, slotCol);
    p.Stroke(gSlot, slotCol, 1.6f * s);
    if (c.power > 0.02f) {
        float sway = Clamp(Finite(sc.sway, 0.0f), -1.0f, 1.0f);
        const int nv = 12;
        for (int i = 0; i < nv; ++i) {
            float u = Lerp(l.u0 + 0.02f, l.u1 - 0.02f, (static_cast<float>(i) + 0.5f) / static_cast<float>(nv));
            p.Line(l.Under(u + sway * 0.006f, l.v0 + 0.05f), l.Under(u - sway * 0.006f, l.v1 - 0.10f), Fade(th.vane, c.power), 0.9f * s);
        }
    }
    DrawFlap(c, k);

    // Front face, drawn last: rounded, a soft top-to-bottom gradient, a rim highlight along the top edge.
    float rad = 4.5f * s;
    D2D1_ROUNDED_RECT fr = D2D1::RoundedRect(D2D1::RectF(l.fL, l.fT, l.fR, l.fB), rad, rad);
    // The corners the rounding uncovers belong to the side and the underside.
    Quad(p, P(l.fR - rad, l.fT), P(l.fR, l.fT), P(l.fR, l.fT + rad), P(l.fR - rad, l.fT + rad), Tone(th.side));
    Quad(p, P(l.fL, l.fB - rad), P(l.fL + rad, l.fB - rad), P(l.fL + rad, l.fB), P(l.fL, l.fB), Tone(th.under));
    Quad(p, P(l.fR - rad, l.fB - rad), P(l.fR, l.fB - rad), P(l.fR, l.fB), P(l.fR - rad, l.fB), Tone(th.under));
    if (auto gr = p.Gradient(P(l.fL, l.fT), P(l.fL + (l.fR - l.fL) * 0.12f, l.fB), { { 0.0f, Tone(th.front) }, { 1.0f, Tone(th.frontLow) } }))
        p.rt->FillRoundedRectangle(fr, gr.Get());
    if (th.edge.a > 0.0f && !off) p.rt->DrawRoundedRectangle(fr, p.B(th.edge), 0.8f * s);
    p.Line(P(l.fL + rad, l.fT + 0.6f * s), P(l.fR - rad, l.fT + 0.6f * s), Fade(th.rim, off ? 0.4f : 1.0f), 1.0f * s);

    if (off) {
        // Disconnected: a dashed outline around the whole solid.
        ComPtr<ID2D1StrokeStyle> dash;
        float d[2] = { 3.0f, 3.0f };
        p.f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f), d, 2, &dash);
        PathB o(p.f.Get());
        o.Begin(P(l.fL + rad, l.fT), false);
        o.Line(P(l.fR, l.fT)); o.Line(bT); o.Line(bB); o.Line(bL); o.Line(P(l.fL, l.fB)); o.Line(P(l.fL, l.fT + rad));
        o.Arc(P(l.fL + rad, l.fT), rad, true);
        o.End(true);
        ID2D1PathGeometry* go = o.Done();
        if (go && dash) p.rt->DrawGeometry(go, p.B(sc.light ? Rgb(0x50555C, 0.55f) : Rgb(0xD0D2D6, 0.50f)), 1.1f * s, dash.Get());
    }

    DrawDisplay(c);
}

// Gradient brushes for one stream colour, made once per frame and re-aimed for every ribbon.
struct StreamBrushes {
    Col col{};
    ComPtr<ID2D1LinearGradientBrush> pulse, guide, guideUp;
};

void MakeStreamBrushes(SceneCtx& c, const Col& col, StreamBrushes& sb) {
    sb.col = col;
    // Pulse: fades in from its tail, slightly softened head (0 = tail, 1 = head).
    sb.pulse = c.p.Gradient(P(0, 0), P(0, 1), { { 0.0f, Fade(col, 0.0f) }, { 0.2f, Fade(col, 0.16f) }, { 0.42f, Fade(col, 0.62f) },
                                                { 0.62f, col }, { 0.9f, col }, { 1.0f, Fade(col, 0.35f) } });
    if (c.fl.away)
        sb.guideUp = c.p.Gradient(P(0, 0), P(0, 1), { { 0.0f, Fade(col, 0.7f) }, { 0.35f, Fade(col, 0.45f) }, { 1.0f, Fade(col, 0.0f) } });
    else
        sb.guide = c.p.Gradient(P(0, 0), P(0, 1), { { 0.0f, Fade(col, 0.0f) }, { 0.25f, col }, { 0.6f, Fade(col, 0.55f) }, { 1.0f, Fade(col, 0.0f) } });
}

// A stream ribbon over [u0, u1] of stream `a`: a soft stroke along the flow.
void Pulse(SceneCtx& c, float a, int i, float u0, float u1, float width, StreamBrushes& sb, float alpha) {
    constexpr int n = 12;
    float lo = std::max(u0, 0.0f), hi = std::min(u1, 1.0f);
    if (hi - lo < 0.03f || !sb.pulse) return;
    PathB b(c.p.f.Get());
    Pt first{}, last{};
    for (int j = 0; j < n; ++j) {
        float u = Lerp(lo, hi, static_cast<float>(j) / static_cast<float>(n - 1));
        Pt q = c.fl.At(a, u, i);
        if (j == 0) { b.Begin(q, false); first = q; } else b.Line(q);
        last = q;
    }
    ID2D1PathGeometry* g = b.Done();
    // Where the ribbon is cut by the start or the end of the stream the gradient axis is extended past
    // the cut, so the fade keeps its place on the ribbon.
    float span = u1 - u0;
    float t0 = (lo - u0) / span, t1 = (hi - u0) / span;
    float k = 1.0f / std::max(0.05f, t1 - t0);
    float dx = (last.x - first.x) * k, dy = (last.y - first.y) * k;
    sb.pulse->SetStartPoint(P(first.x - dx * t0, first.y - dy * t0));
    sb.pulse->SetEndPoint(P(first.x + dx * (1.0f - t0), first.y + dy * (1.0f - t0)));
    sb.pulse->SetOpacity(alpha);
    if (g) c.p.rt->DrawGeometry(g, sb.pulse.Get(), width, c.p.round.Get());
}

// Faint full-length lines under the moving ribbons: they keep the shape of the airflow readable.
void Guide(SceneCtx& c, float a, int i, float width, StreamBrushes& sb, float alpha) {
    constexpr int n = 14;
    PathB b(c.p.f.Get());
    Pt first{}, last{};
    for (int j = 0; j < n; ++j) {
        float u = Lerp(0.06f, 1.0f, static_cast<float>(j) / static_cast<float>(n - 1));
        Pt q = c.fl.At(a, u, i);
        if (j == 0) { b.Begin(q, false); first = q; } else b.Line(q);
        last = q;
    }
    ID2D1PathGeometry* g = b.Done();
    ID2D1LinearGradientBrush* br = c.fl.away ? sb.guideUp.Get() : sb.guide.Get();
    if (!g || !br) return;
    if (c.fl.away) {
        // Fades out as it climbs the wall beside the unit.
        br->SetStartPoint(P(0, c.fl.y0));
        br->SetEndPoint(P(0, last.y));
    } else {
        br->SetStartPoint(first);
        br->SetEndPoint(P(first.x, c.fl.y0 + c.fl.len * c.fl.drop * Lerp(1.0f, 0.8f, c.fl.heat)));
    }
    br->SetOpacity(alpha);
    c.p.rt->DrawGeometry(g, br, width, c.p.round.Get());
}

void DrawAirGlow(SceneCtx& c) {
    // A soft body of air under the outlet: gives the streams something to sit in.
    const Flow& f = c.fl;
    if (f.away || c.air <= 0.01f) return;
    float len = f.len * f.drop * Lerp(1.0f, 0.78f, f.heat);
    float yb = f.y0 + len;
    float xs = f.sway * 44.0f * f.s + f.drift;
    float topHalf = f.sh * 1.02f, botHalf = f.sh * f.spread * 1.12f;
    PathB b(c.p.f.Get());
    b.Begin(P(f.cx - topHalf, f.y0));
    b.Line(P(f.cx + topHalf, f.y0));
    b.Quad(P(f.cx + Lerp(topHalf, botHalf, 0.62f) + xs * 0.25f, f.y0 + len * 0.5f), P(f.cx + botHalf + xs, yb));
    b.Line(P(f.cx - botHalf + xs, yb));
    b.Quad(P(f.cx - Lerp(topHalf, botHalf, 0.62f) + xs * 0.25f, f.y0 + len * 0.5f), P(f.cx - topHalf, f.y0));
    b.End(true);
    Col base = Mix(ModeColor(c.from, c.sc.light), ModeColor(c.to, c.sc.light), c.mix);
    float a = (c.sc.light ? 0.20f : 0.17f) * c.air * Lerp(0.6f, 1.0f, c.fan);
    // Starts clear at the unit's lower edge, so nothing pale shows beside the underside.
    auto gr = c.p.Gradient(P(0, c.l.fB + c.l.dy - 1.0f * f.s), P(0, yb),
                           { { 0.0f, Fade(base, 0.0f) }, { 0.16f, Fade(base, a) }, { 0.6f, Fade(base, a * 0.42f) }, { 1.0f, Fade(base, 0.0f) } });
    ID2D1PathGeometry* g = b.Done();
    if (gr && g) c.p.rt->FillGeometry(g, gr.Get());
}

void DrawStreams(SceneCtx& c) {
    if (c.air <= 0.01f) return;
    static const float kLayerAt[4] = { 0.2f, 0.6f, 1.0f, 1.4f };
    static const float kLayerHz[4] = { 0.20f, 0.33f, 0.50f, 0.72f };
    constexpr int n = 7;
    float fan = c.fan;
    float plen = Lerp(0.40f, 0.60f, fan) * (c.sc.turbo ? 1.12f : 1.0f);
    float width = Lerp(1.7f, 2.5f, fan) * (c.sc.turbo ? 1.2f : 1.0f) * c.l.s;
    float baseA = (c.sc.light ? 0.90f : 0.92f) * c.air;
    // Density: three streams at a whisper, five in the middle, seven at full blast.
    float dens[n];
    for (int i = 0; i < n; ++i) {
        dens[i] = 1.0f;
        if (i == 0 || i == 6) dens[i] = Smooth(0.15f, 0.40f, c.fanEff);
        else if (i == 2 || i == 4) dens[i] = Smooth(0.48f, 0.80f, c.fanEff);
        if (c.fl.away && i == 3) dens[i] = 0.0f;
    }
    // One set of brushes per distinct stream colour (one in most modes, up to seven in Auto or mid-change).
    StreamBrushes brushes[n];
    int brushOf[n];
    int nb = 0;
    for (int i = 0; i < n; ++i) {
        Col col = StreamColorMixed(c, i, n);
        int found = -1;
        for (int j = 0; j < nb && found < 0; ++j)
            if (brushes[j].col.r == col.r && brushes[j].col.g == col.g && brushes[j].col.b == col.b) found = j;
        if (found < 0) { found = nb++; MakeStreamBrushes(c, col, brushes[found]); }
        brushOf[i] = found;
    }
    for (int i = 0; i < n; ++i) {
        if (dens[i] <= 0.01f) continue;
        float a = (static_cast<float>(i) - 3.0f) / 3.0f;
        Guide(c, a, i, width * 0.8f, brushes[brushOf[i]], (c.sc.light ? 0.30f : 0.26f) * c.air * dens[i]);
    }
    for (int layer = 0; layer < 4; ++layer) {
        float lw = std::max(0.0f, 1.0f - std::fabs(c.fanEff - kLayerAt[layer]) / 0.4f);
        if (lw <= 0.01f) continue;
        for (int i = 0; i < n; ++i) {
            if (dens[i] <= 0.01f) continue;
            float a = (static_cast<float>(i) - 3.0f) / 3.0f;
            for (int kk = 0; kk < 2; ++kk) {
                float ph = Fract(c.t * static_cast<double>(kLayerHz[layer]) * static_cast<double>(1.0f + 0.14f * (Hash(i, 7) - 0.5f))
                                 + static_cast<double>(Hash(i, 11 + layer) + 0.5f * static_cast<float>(kk)));
                float head = ph * (1.0f + plen);
                float um = head - plen * 0.5f;
                float env = Smooth(0.0f, 0.22f, um) * (1.0f - Smooth(0.55f, 1.0f, um));
                float al = baseA * lw * dens[i] * env;
                if (al <= 0.012f) continue;
                Pulse(c, a, i, head - plen, head, width, brushes[brushOf[i]], al);
            }
        }
    }
}

// Per-mode particles. `w` is the mode's crossfade weight.
void DrawParticles(SceneCtx& c, int mode, float w) {
    float A = c.air * w;
    if (A <= 0.01f) return;
    Painter& p = c.p;
    const Flow& f = c.fl;
    float s = c.l.s;
    bool light = c.sc.light;
    float dens = Lerp(0.55f, 1.0f, c.fan);
    float airH = c.l.airB - f.y0;

    if (mode == Cool) {
        Col col = light ? Rgb(0x2C8FDB) : Rgb(0xD4ECFF);
        for (int j = 0; j < 9; ++j) {
            float ph = Fract(c.t * static_cast<double>(0.16f + 0.07f * Hash(j, 3)) + static_cast<double>(Hash(j, 5)));
            float a = Hash(j, 9) * 2.2f - 1.1f;
            Pt q = f.At(a, 0.10f + ph * 0.88f, j);
            float env = Smooth(0.0f, 0.2f, ph) * (1.0f - Smooth(0.62f, 1.0f, ph));
            float al = A * env * dens * (0.55f + 0.45f * Hash(j, 13));
            if (al <= 0.02f) continue;
            float r = Lerp(1.7f, 3.0f, Hash(j, 17)) * s;
            float rot = Hash(j, 21) * kPi + static_cast<float>(std::fmod(c.t * 0.5, 6.283185307179586)) * (Hash(j, 23) > 0.5f ? 1.0f : -1.0f);
            for (int kq = 0; kq < 3; ++kq) {
                float an = rot + static_cast<float>(kq) * kPi / 3.0f;
                float dx = std::cos(an) * r, dy = std::sin(an) * r;
                p.Line(P(q.x - dx, q.y - dy), P(q.x + dx, q.y + dy), Fade(col, al), 0.95f * s);
            }
        }
    } else if (mode == Heat) {
        if (f.away) return;  // the warm air is sent along the wall: nothing rises in the room
        Col col = light ? Rgb(0xE9781A) : Rgb(0xFFC784);
        for (int j = 0; j < 7; ++j) {
            float ph = Fract(c.t * static_cast<double>(0.20f + 0.08f * Hash(j, 31)) + static_cast<double>(Hash(j, 33)));
            float fx = (static_cast<float>(j) + 0.5f) / 7.0f * 2.0f - 1.0f + (Hash(j, 35) - 0.5f) * 0.16f;
            float x = f.cx + f.drift * 0.9f + fx * f.sh * (f.away ? 0.8f : 1.38f) + f.sway * 26.0f * s;
            float yb = f.y0 + airH * (0.98f - 0.10f * Hash(j, 37));
            float y = yb - ph * airH * 0.46f;
            float env = Smooth(0.0f, 0.25f, ph) * (1.0f - Smooth(0.55f, 1.0f, ph));
            float al = A * env * dens * 0.85f;
            if (al <= 0.02f) continue;
            float hh = 5.2f * s, amp = 2.3f * s * (Hash(j, 39) > 0.5f ? 1.0f : -1.0f);
            PathB b(p.f.Get());
            b.Begin(P(x, y + hh), false);
            b.Bez(P(x + amp, y + hh * 0.45f), P(x - amp, y - hh * 0.45f), P(x, y - hh));
            p.Stroke(b.Done(), Fade(col, al), 1.25f * s);
        }
    } else if (mode == Dry) {
        if (f.away) return;
        Col col = light ? Rgb(0x129C8A) : Rgb(0x9BE8DC);
        for (int j = 0; j < 8; ++j) {
            float ph = Fract(c.t * static_cast<double>(0.30f + 0.10f * Hash(j, 41)) + static_cast<double>(Hash(j, 43)));
            float fx = (static_cast<float>(j) + 0.5f) / 8.0f * 2.0f - 1.0f + (Hash(j, 45) - 0.5f) * 0.14f;
            float fall = std::pow(ph, 1.5f);
            float x = f.cx + f.drift * fall + fx * f.sh * (f.away ? 0.85f : 1.15f) + f.sway * 22.0f * s * fall;
            float y = f.y0 + 9.0f * s + (airH - 14.0f * s) * fall;
            float env = Smooth(0.0f, 0.18f, ph) * (1.0f - Smooth(0.72f, 1.0f, ph));
            float al = A * env * dens * 0.9f;
            if (al <= 0.02f) continue;
            float r = Lerp(1.25f, 1.9f, Hash(j, 47)) * s;
            PathB b(p.f.Get());
            b.Begin(P(x, y - r * 2.3f));
            b.Bez(P(x - r * 0.2f, y - r * 1.4f), P(x - r, y - r * 0.8f), P(x - r, y));
            b.Arc(P(x + r, y), r, false);
            b.Bez(P(x + r, y - r * 0.8f), P(x + r * 0.2f, y - r * 1.4f), P(x, y - r * 2.3f));
            b.End(true);
            p.Fill(b.Done(), Fade(col, al));
        }
    } else {
        // Fan and Auto: a few motes riding the streams.
        int n = mode == Auto ? 9 : 6;
        for (int j = 0; j < n; ++j) {
            float ph = Fract(c.t * static_cast<double>(0.18f + 0.08f * Hash(j, 51)) + static_cast<double>(Hash(j, 53)));
            float a = Hash(j, 55) * 2.2f - 1.1f;
            Pt q = f.At(a, 0.08f + ph * 0.9f, j);
            float env = Smooth(0.0f, 0.2f, ph) * (1.0f - Smooth(0.6f, 1.0f, ph));
            float al = A * env * dens * (mode == Auto ? 0.85f : 0.6f);
            if (al <= 0.02f) continue;
            Col col = mode == Auto ? ((j & 1) ? ModeColor(Cool, light) : ModeColor(Auto, light)) : ModeColor(Fan, light);
            p.Dot(q, Lerp(1.0f, 1.7f, Hash(j, 57)) * s, Fade(col, al));
        }
    }
}

void DrawSparkles(SceneCtx& c) {
    if (!c.sc.purifier || c.air <= 0.01f) return;
    Painter& p = c.p;
    float s = c.l.s;
    Col col = c.sc.light ? Rgb(0x1FA3C4) : Rgb(0xC9F4FF);
    float airH = c.l.airB - c.fl.y0;
    for (int j = 0; j < 7; ++j) {
        double cyc = c.t * static_cast<double>(0.42f + 0.12f * Hash(j, 61)) + static_cast<double>(Hash(j, 63));
        int gen = static_cast<int>(std::fmod(std::floor(cyc), 4096.0));
        float ph = Fract(cyc);
        float fx = (static_cast<float>(j) + Hash(j + gen * 7, 65)) / 7.0f * 2.0f - 1.0f;
        float fy = 0.16f + 0.72f * Hash(j + gen * 13, 67);
        Pt q;
        if (c.fl.away) q = c.fl.At(fx, 0.15f + 0.7f * fy, j);
        else q = P(c.fl.cx + c.fl.drift * fy + fx * c.fl.sh * Lerp(1.0f, c.fl.spread, fy) * 1.05f + c.fl.sway * 40.0f * s * fy * fy, c.fl.y0 + airH * fy * 0.9f);
        float tw = std::sin(ph * kPi);
        tw *= tw;
        float r = Lerp(2.6f, 4.4f, Hash(j, 69)) * s * (0.35f + 0.65f * tw);
        float al = c.air * tw * 0.95f;
        if (al <= 0.02f) continue;
        float k = r * 0.14f;
        PathB b(p.f.Get());
        b.Begin(P(q.x, q.y - r));
        b.Quad(P(q.x + k, q.y - k), P(q.x + r, q.y));
        b.Quad(P(q.x + k, q.y + k), P(q.x, q.y + r));
        b.Quad(P(q.x - k, q.y + k), P(q.x - r, q.y));
        b.Quad(P(q.x - k, q.y - k), P(q.x, q.y - r));
        b.End(true);
        p.Fill(b.Done(), Fade(col, al));
    }
}

// Small cues on the unit and the wall: power LED, eco leaf, turbo chevrons, sleep moon.
void DrawBadges(SceneCtx& c) {
    Painter& p = c.p;
    const Lay& l = c.l;
    const Scene& sc = c.sc;
    float s = l.s;
    float on = c.power;
    float mid = (l.fT + l.fB) * 0.5f;
    Col tint = Mix(ModeColor(c.from, sc.light), ModeColor(c.to, sc.light), c.mix);

    float x = l.fL + 13.0f * s;
    if (sc.online) {
        if (on > 0.01f) p.Dot(P(x, mid), 3.8f * s, Fade(tint, 0.22f * on));
        p.Dot(P(x, mid), 1.8f * s, Mix(c.th.ledOff, tint, on));
    } else {
        p.Dot(P(x, mid), 1.8f * s, c.th.ledOff);
        return;
    }
    x += 9.0f * s;

    Col dim = sc.light ? Rgb(0xA9ADB3) : Rgb(0x85868B);
    if (sc.eco) {
        float k = 0.66f * s, y = mid - 10.0f * k;
        Col leaf = Mix(dim, sc.light ? Rgb(0x2FA862) : Rgb(0x7BD88F), on);
        PathB b(p.f.Get());
        b.Begin(P(x + 4.8f * k, y + 15.2f * k));
        b.Bez(P(x + 3.6f * k, y + 8.4f * k), P(x + 8.6f * k, y + 3.9f * k), P(x + 16.2f * k, y + 4.0f * k));
        b.Bez(P(x + 16.4f * k, y + 11.2f * k), P(x + 11.8f * k, y + 16.2f * k), P(x + 4.8f * k, y + 15.2f * k));
        b.End(true);
        p.Fill(b.Done(), leaf);
        p.Line(P(x + 2.8f * k, y + 17.4f * k), P(x + 4.8f * k, y + 15.2f * k), leaf, 1.1f * s);
        p.Line(P(x + 6.2f * k, y + 13.8f * k), P(x + 11.4f * k, y + 8.8f * k), Fade(c.th.front, 0.85f), 0.9f * s);
        x += 15.0f * s;
    }
    if (sc.turbo) {
        Col col = Mix(dim, tint, on);
        for (int i = 0; i < 2; ++i) {
            float cx = x + 3.0f * s + static_cast<float>(i) * 4.6f * s;
            PathB b(p.f.Get());
            b.Begin(P(cx - 1.6f * s, mid - 3.6f * s), false);
            b.Line(P(cx + 2.0f * s, mid));
            b.Line(P(cx - 1.6f * s, mid + 3.6f * s));
            p.Stroke(b.Done(), col, 1.5f * s);
        }
    }

    // Sleep: a small moon on the wall, left of the unit.
    if (sc.sleep) {
        float R = 7.6f * s;
        Pt m = P(std::max(l.r.left + R + 6.0f * s, l.fL - 40.0f * s), l.fT + 16.0f * s);
        Col moon = sc.light ? Rgb(0xD9A520, 0.92f) : Rgb(0xF4E2A4, 0.95f);
        moon.a *= Lerp(0.55f, 1.0f, on);
        Pt a0 = Polar(m, R, 35.0f), a1 = Polar(m, R, -100.0f);
        PathB b(p.f.Get());
        b.Begin(a0);
        b.Arc(a1, R, true, true);
        b.Arc(a0, R * 0.80f, false, false);
        b.End(true);
        p.Fill(b.Done(), moon);
        p.Dot(P(m.x + R * 1.3f, m.y - R * 0.9f), 0.95f * s, Fade(moon, 0.8f));
        p.Dot(P(m.x + R * 1.8f, m.y + R * 0.3f), 0.7f * s, Fade(moon, 0.6f));
    }
}

}  // namespace

// ---------------------------------------------------------------- public API

D2D1_COLOR_F ModeColor(int mode, bool light) {
    switch (mode) {
    case Auto: return light ? Rgb(0x2FA862) : Rgb(0x7BD88F);
    case Cool: return light ? Rgb(0x1E8FE1) : Rgb(0x5AB8FF);
    case Dry:  return light ? Rgb(0x129C8A) : Rgb(0x3FC8B5);
    case Heat: return light ? Rgb(0xE8751A) : Rgb(0xFF9B45);
    case Fan:
    default:   return light ? Rgb(0x6E7E8A) : Rgb(0xB8C4CC);
    }
}

void DrawScene(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, const Scene& s) { DrawScene(rt, r, s, nullptr); }

void DrawScene(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, const Scene& s, SceneAnchors* out) {
    if (out) *out = SceneAnchors{ P(r.left, r.top), P(r.left, r.top), P(r.left, r.top), P(r.left, r.top) };
    if (r.right - r.left < 8.0f || r.bottom - r.top < 8.0f) return;
    Painter p(rt);
    if (rt && !p.ok) rt = nullptr;  // still report the anchors

    SceneCtx c{ p, s };
    c.th = MakeTheme(s.light);
    // The one place time enters: frozen for reduced motion and when offline.
    c.t = (s.reducedMotion || !s.online) ? 2.6 : s.time;
    c.power = s.online ? Sat(Finite(s.power, 0.0f)) : 0.0f;
    c.l = MakeLay(r);
    c.fan = Sat(Finite(s.fan, 0.6f));
    c.fanEff = Clamp(c.fan + (s.turbo ? 0.4f : 0.0f) - (s.sleep ? 0.25f : 0.0f), 0.2f, 1.4f);
    c.mix = Sat(Finite(s.modeMix, 1.0f));
    c.from = SafeMode(s.fromMode);
    c.to = SafeMode(s.mode);
    c.air = c.power * Smooth(0.0f, 0.15f, c.fan) * (s.sleep ? 0.8f : 1.0f);

    const Lay& l = c.l;
    float flap = Sat(Finite(s.flap, 0.5f));
    Pt mouth = l.Under(0.5f, (l.v0 + l.v1) * 0.5f);
    Flow& f = c.fl;
    f.cx = mouth.x; f.s = l.s;
    f.y0 = mouth.y;
    f.sh = (l.fR - l.fL) * (l.u1 - l.u0) * 0.5f * 0.88f;
    f.len = (l.airB - f.y0) * std::min(1.0f, Lerp(0.66f, 0.97f, c.fan) * (s.turbo ? 1.1f : 1.0f));
    f.spread = Lerp(1.58f, 1.10f, flap);
    f.drop = Lerp(0.80f, 1.0f, flap);
    f.sway = Clamp(Finite(s.sway, 0.0f), -1.0f, 1.0f);
    f.heat = ModeWeight(c, Heat);
    f.unitHalf = (l.fR - l.fL + l.dx) * 0.5f;
    f.unitH = l.fB + l.dy - l.fT;
    f.away = s.breezeAway;
    f.drift = ((r.left + r.right) * 0.5f - mouth.x) * 0.55f;
    f.wob = Fract(c.t * 0.3) * kTau;

    if (rt) {
        rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DrawWall(c);
        DrawAirGlow(c);
        DrawUnit(c);
        DrawStreams(c);
        for (int m = Auto; m <= Fan; ++m) DrawParticles(c, m, ModeWeight(c, m));
        DrawSparkles(c);
        DrawBadges(c);
        rt->PopAxisAlignedClip();
    }
    if (out) {
        // Top edge of the display glass, above the middle of a two-digit reading: it does not move when
        // the reading gets wider ("24.5", "100").
        out->led = P(l.fR - 23.1f * l.s, (l.fT + l.fB) * 0.5f - 9.5f * l.s);
        out->louver = mouth;
        out->room = P(r.left + 40.0f * l.s, r.bottom - 34.0f * l.s);
        out->wall = P(r.right - 27.0f * l.s, r.top + 66.0f * l.s);
    }
}

// ---------------------------------------------------------------- strip glyph
// A small solid object in the manner of the Windows 11 Fluent 3D icons: a wall-mounted split unit seen
// from slightly below and from the left, so the front face, the rounded left end and the underside with
// the outlet all show. Near-white in both themes. Long edges are horizontal and snapped to whole units
// (the caller's units are pixels); only the short end edges slant.
namespace {

struct StripPal {
    Col faceT, faceB, hi, sideT, sideB, underT, underB, slot, vane, dispOff;
};

// Desaturates towards the colour's own grey, then scales the brightness.
Col StripTone(const Col& c, float desat, float k) {
    float y = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    return Col{ Lerp(c.r, y, desat) * k, Lerp(c.g, y, desat) * k, Lerp(c.b, y, desat) * k, c.a };
}

StripPal MakeStripPal(float desat, float k) {
    StripPal q;
    q.faceT = StripTone(Rgb(0xF1F4F8), desat, k); q.faceB = StripTone(Rgb(0xD2DAE4), desat, k);
    q.hi = StripTone(Rgb(0xFFFFFF), desat, k);
    q.sideT = StripTone(Rgb(0xD0D8E1), desat, k); q.sideB = StripTone(Rgb(0xA3AEBB), desat, k);
    q.underT = StripTone(Rgb(0x8893A0), desat, k); q.underB = StripTone(Rgb(0xA9B3BF), desat, k);
    q.slot = StripTone(Rgb(0x1C2228), desat, k); q.vane = StripTone(Rgb(0x66717D), desat, k);
    q.dispOff = StripTone(Rgb(0x4C5661), desat, k);
    return q;
}

float Snap(float v) { return std::floor(v + 0.5f); }

void FillV(Painter& p, ID2D1Geometry* g, float y0, float y1, const Col& c0, const Col& c1) {
    if (!g) return;
    ComPtr<ID2D1LinearGradientBrush> br = p.Gradient(P(0.0f, y0), P(0.0f, y1), { { 0.0f, c0 }, { 1.0f, c1 } });
    if (br) p.rt->FillGeometry(g, br.Get());
    else p.Fill(g, Mix(c0, c1, 0.5f));
}

}  // namespace

void DrawStripGlyph(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, const Scene& s) {
    if (!rt || r.right - r.left < 4.0f || r.bottom - r.top < 4.0f) return;
    Painter p(rt);
    if (!p.ok) return;
    rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    {
        // Designed on an 18-unit-tall grid; the width follows the rect between 1.5:1 and 2.6:1.
        float W = r.right - r.left, H = r.bottom - r.top;
        float gh = std::min(H, W / 1.5f), gw = std::min(W, gh * 2.6f);
        float u = gh / 18.0f;
        float ox = r.left + (W - gw) * 0.5f, oy = r.top + (H - gh) * 0.5f;

        float power = s.online ? Sat(Finite(s.power, 0.0f)) : 0.0f;
        float fan = Sat(Finite(s.fan, 0.6f));
        float mix = Sat(Finite(s.modeMix, 1.0f));
        int from = SafeMode(s.fromMode), to = SafeMode(s.mode);
        Col air = Mix(ModeColor(from, s.light), ModeColor(to, s.light), mix);
        // The display sits on the near-white body in both themes: the saturated (light theme) accent.
        Col lit = Mix(ModeColor(from, true), ModeColor(to, true), mix);

        // Off: a little desaturated and dimmer. Offline: plain grey.
        StripPal q = s.online ? MakeStripPal(Lerp(0.7f, 0.0f, power), Lerp(0.955f, 1.0f, power))
                              : MakeStripPal(1.0f, s.light ? 0.74f : 0.55f);

        // Rows: front face, then the underside (seam lip, slot, back lip); the air has the rest.
        // A deeper body than a pictogram would use, so the unit reads as a solid box, not a flat bar.
        float faceH = std::max(3.0f, Snap(8.5f * u)), dy = std::max(2.0f, Snap(3.6f * u)), dx = std::max(1.0f, Snap(2.4f * u));
        float bodyH = faceH + dy;
        // On: at the top, the air under it. Off: centred (a touch high, the shadow is below).
        float topOn = Snap(oy + 1.0f * u), topOff = Snap(oy + (gh - bodyH) * 0.5f - 0.5f * u);
        float T = Snap(Lerp(topOff, topOn, power));  // whole pixels on the way too: the edges never blur
        float X0 = Snap(ox + 1.0f * u), X1 = Snap(ox + gw - 1.0f * u);  // back left edge, front right edge
        float L = X0 + dx;                                             // front left edge
        float FB = T + faceH, UB = FB + dy;
        float rt2 = std::min(2.6f * u, faceH * 0.45f), rb = std::min(1.5f * u, dy * 0.45f);
        float kb = rb / dy;

        // Contact shadow on the wall under the unit: stacked translucent rounded shapes.
        {
            float a = s.light ? 0.075f : 0.16f;
            for (int k = 0; k < 3; ++k) {
                float e = static_cast<float>(k) * 0.55f * u;
                p.FillRR(X0 + 1.2f * u - e, UB - 3.0f * u, X1 - dx - 0.4f * u + e, UB + 0.9f * u + e * 1.3f, 1.8f * u + e, Rgb(0x000000, a));
            }
        }

        // The whole solid's outline: rounded left end (the unit's D-shaped profile), flat right end.
        PathB sil(p.f.Get());
        sil.Begin(P(L + rt2, T));
        sil.Line(P(X1 - rt2, T)); sil.Arc(P(X1, T + rt2), rt2, true);
        sil.Line(P(X1, FB));
        sil.Line(P(X1 - dx * (1.0f - kb), UB - rb)); sil.Quad(P(X1 - dx, UB), P(X1 - dx - rb, UB));
        sil.Line(P(X0 + rb, UB)); sil.Quad(P(X0, UB), P(X0, UB - rb));
        sil.Line(P(X0, T + dy + 0.5f * u)); sil.Bez(P(X0, T + dy * 0.35f), P(L - dx * 0.35f, T), P(L + rt2, T));
        sil.End(true);
        ID2D1PathGeometry* silG = sil.Done();
        // On a light taskbar a near-white body needs a contour: a hairline just outside the outline.
        if (s.light) p.Stroke(silG, Rgb(0x1C2632, s.online ? 0.34f : 0.26f), 2.0f);
        FillV(p, silG, T, UB, q.sideT, q.sideB);

        PathB under(p.f.Get());
        under.Begin(P(L, FB));
        under.Line(P(X1, FB));
        under.Line(P(X1 - dx * (1.0f - kb), UB - rb)); under.Quad(P(X1 - dx, UB), P(X1 - dx - rb, UB));
        under.Line(P(X0 + rb, UB)); under.Quad(P(X0, UB), P(X0 + dx * kb, UB - rb));
        under.End(true);
        FillV(p, under.Done(), FB, UB, q.underT, q.underB);

        // The outlet: a dark slot along the underside, its ends slanting with the depth.
        float lip = std::max(1.0f, Snap(dy * 0.25f));
        float sy0 = FB + lip, sy1 = UB - lip;
        float sxl = L + Snap(3.0f * u), sxr = X1 - Snap(3.0f * u);
        auto Sx = [&](float x, float y) { return x - dx * (y - FB) / dy; };
        if (sy1 - sy0 >= 1.0f && sxr - sxl > 4.0f * u) {
            PathB slot(p.f.Get());
            slot.Begin(P(Sx(sxl, sy0), sy0));
            slot.Line(P(Sx(sxr, sy0), sy0)); slot.Line(P(Sx(sxr, sy1), sy1)); slot.Line(P(Sx(sxl, sy1), sy1));
            slot.End(true);
            p.Fill(slot.Done(), q.slot);
            // Louver blades inside it, once there is room for them.
            float sh = sy1 - sy0;
            if (sh >= 3.0f) {
                float th = std::max(1.0f, Snap(sh * 0.2f));
                int n = sh >= 7.0f ? 2 : 1;
                for (int i = 1; i <= n; ++i) {
                    float y = Snap(sy0 + sh * static_cast<float>(i) / static_cast<float>(n + 1) - th * 0.5f);
                    rt->FillRectangle(D2D1::RectF(Sx(sxl, y) + 1.0f * u, y, Sx(sxr, y) - 1.0f * u, y + th), p.B(q.vane));
                }
            }
        }

        // Front face: bright at the top, cooler towards the seam.
        PathB face(p.f.Get());
        face.Begin(P(L + rt2, T));
        face.Line(P(X1 - rt2, T)); face.Arc(P(X1, T + rt2), rt2, true);
        face.Line(P(X1, FB)); face.Line(P(L, FB));
        face.Line(P(L, T + rt2)); face.Arc(P(L + rt2, T), rt2, true);
        face.End(true);
        FillV(p, face.Done(), T, FB, q.faceT, q.faceB);
        // Specular along the top front edge.
        float hiH = std::max(1.0f, Snap(0.7f * u));
        p.FillRR(L + rt2 * 0.7f, T, X1 - rt2 * 0.7f, T + hiH, hiH * 0.5f, q.hi);
        // Ambient occlusion where the face meets the underside.
        float aoH = std::max(1.0f, Snap(0.5f * u));
        rt->FillRectangle(D2D1::RectF(L, FB, X1 - dx * aoH / dy, FB + aoH), p.B(Rgb(0x0A1018, 0.16f)));

        // The display, at the right of the face.
        float dh = std::max(2.0f, Snap(2.0f * u)), dw = std::max(3.0f, Snap(5.0f * u));
        float dR = X1 - std::max(2.0f, Snap(3.0f * u)), dT = T + Snap((faceH - dh) * 0.5f);
        if (power > 0.01f) p.FillRR(dR - dw - 1.2f * u, dT - 1.2f * u, dR + 1.2f * u, dT + dh + 1.2f * u, dh * 0.5f + 1.2f * u, Fade(lit, 0.22f * power));
        p.FillRR(dR - dw, dT, dR, dT + dh, dh * 0.5f, Mix(q.dispOff, lit, power));

        // Air: soft wisps curling out of the outlet.
        float airA = power * Smooth(0.0f, 0.15f, fan);
        if (airA > 0.01f && sxr - sxl > 4.0f * u) {
            float y0 = (sy0 + sy1) * 0.5f, yMax = oy + gh - 0.6f * u;
            float len = (yMax - y0) * Lerp(0.74f, 1.0f, fan);
            float sway = Clamp(Finite(s.sway, 0.0f), -1.0f, 1.0f);
            float ww = std::max(1.25f, 1.5f * u);
            ComPtr<ID2D1LinearGradientBrush> grad =
                p.Gradient(P(0.0f, y0), P(0.0f, y0 + len), { { 0.0f, Fade(air, 0.0f) }, { 0.3f, Fade(air, (s.light ? 0.9f : 0.8f) * airA) }, { 1.0f, Fade(air, (s.light ? 0.42f : 0.32f) * airA) } });
            const double hz = 0.62;  // constant: a rate that followed the eased fan would make the pulse jump
            for (int i = 0; i < 3; ++i) {
                float fi = static_cast<float>(i);
                float wob = s.reducedMotion ? 0.0f : std::sin(kTau * Fract(s.time * 0.41 + static_cast<double>(i) * 0.31));
                float x = Sx(Lerp(sxl, sxr, 0.2f + 0.3f * fi), y0);
                // Straight down; swing (sway) and a slight breathing wobble only bend the tips.
                float sweep = (sway * 2.0f + wob * 0.6f) * u;
                Pt a = P(x, y0), b1 = P(x, y0 + len * 0.5f), b2 = P(x + sweep * 0.4f, y0 + len * 0.8f), e = P(x + sweep, y0 + len);
                auto At = [&](float t) {
                    float m = 1.0f - t;
                    float w0 = m * m * m, w1 = 3.0f * m * m * t, w2 = 3.0f * m * t * t, w3 = t * t * t;
                    return P(w0 * a.x + w1 * b1.x + w2 * b2.x + w3 * e.x, w0 * a.y + w1 * b1.y + w2 * b2.y + w3 * e.y);
                };
                auto Seg = [&](float t0, float t1, ID2D1Brush* br) {
                    PathB b(p.f.Get());
                    b.Begin(At(t0), false);
                    for (int k = 1; k <= 8; ++k) b.Line(At(Lerp(t0, t1, static_cast<float>(k) / 8.0f)));
                    if (ID2D1PathGeometry* g = b.Done()) rt->DrawGeometry(g, br, ww, p.round.Get());
                };
                if (grad) Seg(0.0f, 1.0f, grad.Get());
                else Seg(0.25f, 1.0f, p.B(Fade(air, 0.5f * airA)));
                if (s.reducedMotion) continue;
                // A brighter stretch runs down each wisp, staggered.
                float ph = Fract(s.time * hz + static_cast<double>(i) * 0.37);
                float dl = 0.55f;
                float h1 = ph * (1.0f + dl), h0 = h1 - dl;
                float c0 = std::max(0.3f, Sat(h0)), c1 = Sat(h1);
                if (c1 - c0 > 0.03f) {
                    float env = Smooth(0.0f, 0.3f, ph) * (1.0f - Smooth(0.7f, 1.0f, ph));
                    Seg(c0, c1, p.B(Fade(air, 0.85f * airA * env)));
                }
            }
        }
    }
    rt->PopAxisAlignedClip();
}

void DrawGlyph(ID2D1RenderTarget* rt, Glyph g, const D2D1_RECT_F& r, D2D1_COLOR_F color, double t) {
    if (!rt || r.right - r.left < 2.0f || r.bottom - r.top < 2.0f) return;
    Painter p(rt);
    if (!p.ok) return;
    BoxScope box(rt, r);
    G gc{ p, color, box.StrokeWidth() };
    GlyphBody(gc, g, t);
}

void DrawLouverPosition(ID2D1RenderTarget* rt, int index, bool horizontal, const D2D1_RECT_F& r, D2D1_COLOR_F color) {
    if (!rt || r.right - r.left < 2.0f || r.bottom - r.top < 2.0f) return;
    Painter p(rt);
    if (!p.ok) return;
    BoxScope box(rt, r);
    G gc{ p, color, box.StrokeWidth() };
    LouverDiagram(gc, index, horizontal);
}

}  // namespace art
