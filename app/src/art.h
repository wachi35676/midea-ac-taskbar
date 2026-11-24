#pragma once
// Art for the AC app: the animated AC scene in the panel, the tiny animated strip glyph on the taskbar,
// and the icon set. Pure drawing: no state, no timers, no windows. The caller eases every input
// (flap angle, fan intensity, mode crossfade, power fade) and passes the time for ambient motion.
//
// Contract: draw only inside the given rect; never assume a size (it scales with DPI); work on any
// ID2D1RenderTarget (HWND, DC with premultiplied alpha for the layered taskbar strip, WIC for previews).
#include <d2d1.h>

namespace art {

// AC modes, as the device numbers them.
enum Mode : int { Auto = 1, Cool = 2, Dry = 3, Heat = 4, Fan = 5 };

struct Scene {
    // Device state, already eased by the caller where noted.
    int mode = Cool;          // current mode
    int fromMode = Cool;      // previous mode, for the crossfade
    float modeMix = 1.0f;     // 0 = fromMode, 1 = mode (eased)
    float power = 1.0f;       // 0 = off, 1 = on (eased): unit dims, airflow fades out, LED turns off
    float fan = 0.6f;         // airflow intensity 0..1 (eased): 0.2 silent ... 1.0 max
    float flap = 0.5f;        // vertical louver opening 0..1 (eased): 0 = closed/up, 1 = pointing fully down
    float sway = 0.0f;        // horizontal air direction -1 (left) .. 1 (right) (eased/animated by caller)
    bool eco = false, turbo = false, sleep = false, purifier = false, breezeAway = false, defrost = false;
    float targetTemperature = 24.0f;  // shown on the unit's small LED display when on
    bool online = true;       // false: grey, static, no airflow, a subtle "disconnected" look
    // Rendering context
    double time = 0.0;        // seconds, monotonic, for ambient motion (particles, waves, fan blades)
    bool reducedMotion = false;  // true: no ambient motion at all; draw a pleasing static frame instead
    bool light = false;       // light theme (else dark)
};

// The hero illustration at the top of the panel (roughly 2:1 landscape, e.g. 352x170 DIPs at 100 %):
// a clean wall-mounted split unit with its louver, and the air coming out of it, styled per mode:
// Cool = cool blue streams with drifting frost specks; Heat = warm orange rising heat shimmer;
// Dry = teal with falling droplets; Fan = neutral soft streams; Auto = gentle green/blue blend.
// Fan intensity sets stream speed/length/density; flap sets the outflow angle; sway bends it sideways;
// turbo = stronger, longer bursts; sleep = slower, dimmer, a small moon; eco = a small leaf on the unit;
// purifier = tiny sparkle ions in the air; breezeAway = air deflected up along the wall, away from the room.
void DrawScene(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, const Scene& s);

// Layout: the whole rect is the wall (it fades out towards the bottom, where the room is); the unit hangs
// centred at the top at every power value, and the top right corner stays free for the panel's round
// button (about 34x34 DIPs, 10 in).
//
// Screen points (in the same coordinate space as `r`) the panel can start dotted leader lines from:
// led = top edge of the unit's display, above the digits; louver = the mouth of the outlet;
// room = a point in the room air at the lower left; wall = a point on the bare wall at the right, under
// the top right button. They depend only on `r`: never on time or on any other field of the scene.
struct SceneAnchors {
    D2D1_POINT_2F led, louver, room, wall;
};
// Same drawing; also fills `out` (may be null). Passing rt = nullptr only computes the anchors.
void DrawScene(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, const Scene& s, SceneAnchors* out);

// Taskbar-size version (18-22 px tall at 100 %, 2:1 to 2.6:1; 44x20 DIPs is the intended rect): a small
// solid split unit in the manner of the Windows 11 Fluent 3D icons, seen from slightly below and from the
// left, near-white on both light and dark taskbars. On: the display glows in the mode colour and air wisps
// flow from the outlet (fan sets their length). Off (power = 0): static, centred, display dark. Offline: grey.
// Give it a rect on whole pixels: it snaps its edges to whole units of the rect's coordinate space.
void DrawStripGlyph(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, const Scene& s);

// Accent colour for a mode (dark and light theme variants).
D2D1_COLOR_F ModeColor(int mode, bool light);

// Icon set for controls. Line icons, consistent stroke (~1.5 px at 20 px size), rounded caps, drawn in `color`.
// `t` (seconds) animates the few icons that can move (FanSpeed spins); pass 0 for static.
enum class Glyph {
    Power, Plus, Minus, Refresh, Settings, Close, Chevron,
    ModeAuto, ModeCool, ModeDry, ModeHeat, ModeFan,
    FanSilent, FanLow, FanMedium, FanHigh, FanMax, FanAuto,   // fan with 0..4 "speed bars" / an A badge
    SwingOff, SwingVertical, SwingHorizontal, SwingBoth,
    LouverPosition,  // a louver at an angle: use DrawLouverPosition for the 5 positions
    Eco, Turbo, Sleep, Purifier, BreezeAway, FreezeGuard, Beep, Fahrenheit, SelfClean,
    Thermometer, Outdoor, Bolt, Droplet, Info,
};
void DrawGlyph(ID2D1RenderTarget* rt, Glyph g, const D2D1_RECT_F& r, D2D1_COLOR_F color, double t = 0.0);

// One of the 5 fixed louver positions (index 0..4, top to bottom), as a small diagram of the louver
// and the air direction; index -1 draws "free/swinging" (an arc with arrows).
void DrawLouverPosition(ID2D1RenderTarget* rt, int index, bool horizontal, const D2D1_RECT_F& r, D2D1_COLOR_F color);

}  // namespace art
