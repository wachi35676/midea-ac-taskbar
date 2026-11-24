#include "backdrop.h"

#include <dwmapi.h>

namespace backdrop {
namespace {

constexpr DWORD kDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kCorners = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kBackdrop = 38;  // DWMWA_SYSTEMBACKDROP_TYPE: 1 none, 2 Mica, 3 Acrylic

struct AccentPolicy { int state; int flags; DWORD color; int animation; };
struct CompositionData { int attribute; void* data; SIZE_T size; };
using SetCompositionFn = BOOL(WINAPI*)(HWND, CompositionData*);

// The blur behind the window (undocumented SetWindowCompositionAttribute, used by many taskbar tools) with
// a tint so text stays readable. state 4 = acrylic blur behind, 0 = off. `frosted` uses a stronger tint.
void SetGlass(HWND wnd, bool on, bool light, bool frosted) {
    static auto fn = reinterpret_cast<SetCompositionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute")));
    if (!fn) return;
    const DWORD tint = frosted ? (light ? 0xB8F3F3F3 : 0xA6201E1C)   // AABBGGRR
                               : (light ? 0x55F3F3F3 : 0x40201E1C);
    AccentPolicy a{on ? 4 : 0, 2, tint, 0};
    CompositionData d{19, &a, sizeof(a)};  // 19 = WCA_ACCENT_POLICY
    fn(wnd, &d);
}

}  // namespace

void Apply(HWND wnd, bool light, Backdrop b) {
    const BOOL dark = light ? FALSE : TRUE;
    DwmSetWindowAttribute(wnd, kDarkMode, &dark, sizeof(dark));
    const int corners = 2;  // round
    DwmSetWindowAttribute(wnd, kCorners, &corners, sizeof(corners));
    MARGINS m{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(wnd, &m);
    const int type = b == Backdrop::Mica ? 2 : b == Backdrop::Acrylic ? 3 : 1;  // glass and solid: no system backdrop
    DwmSetWindowAttribute(wnd, kBackdrop, &type, sizeof(type));
    const bool glass = b == Backdrop::Clear || b == Backdrop::Frosted;
    SetGlass(wnd, glass, light, b == Backdrop::Frosted);
    SetWindowPos(wnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool Supported(HWND wnd) {
    const int type = 2;
    return SUCCEEDED(DwmSetWindowAttribute(wnd, kBackdrop, &type, sizeof(type)));
}

}  // namespace backdrop
