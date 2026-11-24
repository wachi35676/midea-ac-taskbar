#pragma once
// The settings window, in the style of the Windows 11 Settings app: Mica, sections of setting cards with
// switches and segmented choices. Every setting applies at once.
#include <windows.h>

#include <functional>
#include <string>

namespace settings {

struct State {
    int backdrop = 2;        // panel background: 0 Mica, 1 Acrylic, 2 Clear glass
    bool left = false;       // strip at the left end of the taskbar
    bool allMonitors = true; // a strip on every monitor's taskbar
    bool autostart = false;
    int interval = 1;        // refresh: 0 = 5 s, 1 = 10 s, 2 = 30 s, 3 = 60 s
    std::wstring address;    // the AC's IP address
    std::wstring status;     // "Online, updated 20:41"
    // Desktop widget
    bool desktop = true;
    int desktopSize = 1;        // 0..4 = 75, 100, 125, 150, 200 %
    int desktopBackground = 0;  // 0 clear glass, 1 frosted glass, 2 solid
    bool desktopOnTop = false, desktopLocked = false;
};

enum class Action {
    Backdrop, Side, Monitors, Autostart, Interval, Refresh, Exit,
    Desktop, DesktopSize, DesktopBackground, DesktopOnTop, DesktopLocked, DesktopReset
};
// `value`: the chosen option index, or 0/1 for switches.
using Handler = std::function<void(Action action, int value)>;
using Provider = std::function<State()>;

void Show(HINSTANCE inst, Provider provider, Handler handler);
void Refresh();  // settings or readings changed elsewhere, or the theme changed
bool Visible();
void Close();

}  // namespace settings
