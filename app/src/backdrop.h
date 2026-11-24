#pragma once
// Window backdrops, as the battery app does them: the DWM frame is extended over the whole window, and the
// system backdrop is Mica or Acrylic (Windows 11 22H2+), or a "glass" blur-behind with a thin tint. The
// window then paints on a transparent background (premultiplied alpha) so the backdrop shows.
//
// Mica and Acrylic only render while the window is active; on a window that is never active (the desktop
// widget) Windows draws them as flat grey. For those, Clear glass (a light blur), Frosted (the same blur
// with a stronger tint) and Solid (an opaque fill the window paints itself) are used.
#include <windows.h>

enum class Backdrop : int { Mica = 0, Acrylic = 1, Clear = 2, Frosted = 3, Solid = 4 };

namespace backdrop {
void Apply(HWND wnd, bool light, Backdrop b);
bool Supported(HWND wnd);  // false before Windows 11 22H2: paint opaque instead
}  // namespace backdrop
