#pragma once
// Shared Direct2D / DirectWrite helpers, theme colours and the small vector icons used by the taskbar strip
// and the click panel. Placeholder look: the visual design comes later.
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>

namespace gfx {

ID2D1Factory* D2D();
IDWriteFactory* DWrite();

// Cached text format (family, size in DIPs, weight). Never release the result.
IDWriteTextFormat* Font(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
// Width of `text` in DIPs.
float TextWidth(const std::wstring& text, IDWriteTextFormat* f);

// Forces the light (1) or dark (0) theme in this app only, ignoring Windows (-1: follow Windows). For screenshots.
void SetThemeOverride(int light);
bool SystemUsesLightTheme();  // taskbar
bool AppsUseLightTheme();     // windows like the panel
// The Windows accent as controls use it: a lighter shade on dark backgrounds, a darker one on light
// ones (SystemAccentColorLight2 / Dark1). Read fresh each call, so a new accent shows at the next paint.
D2D1_COLOR_F Accent(bool lightTheme);

D2D1_COLOR_F Rgb(unsigned rgb, float a = 1.0f);

enum class Icon { Power, Cool, Heat, Dry, Fan, Auto, Warning, Minus, Plus, Refresh };
// Draws an icon centred in `box` with the given brush.
void DrawIcon(ID2D1RenderTarget* rt, Icon icon, const D2D1_RECT_F& box, ID2D1Brush* brush);

template <class T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

}  // namespace gfx
