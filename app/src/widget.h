#pragma once
// The strip on the taskbar: a layered child window of Shell_TrayWnd (the technique open-battery uses).
// Sits at either end: right (default, just left of the notification area) or left (at the taskbar's left
// edge), and steps past any other app strip already there (open-battery uses the same spots).
#include <windows.h>

#include <string>

#include "art.h"
#include "gfx.h"

struct StripModel {
    art::Scene scene;           // drives the tiny animated AC glyph (mode, power, fan, online)
    bool animate = false;       // AC running: animate the glyph at 30 fps; otherwise fully still
    std::wstring line1, line2;  // "Cooling to 24°" / "Room 26.3° · 1.2 kW"
    bool dim = false;           // off or offline
    bool alert = false;         // error code
    std::wstring tooltip;
};

namespace widget {
// Clicks are posted to `notify` as `clickMsg` (left) and `menuMsg` (right).
bool Create(HINSTANCE inst, HWND notify, UINT clickMsg, UINT menuMsg);
void Destroy();
bool Alive();
void Update(const StripModel& m);
void Retheme();
// Which end of the taskbar the strip lives at (kept across Create/Destroy).
void SetSide(bool left);
// A strip on every monitor's taskbar (default), or on the main one only.
void SetAllMonitors(bool all);
// Re-measures the taskbar and moves/raises the strip; call about once a second.
// Returns false when the taskbar it lives in has gone (Explorer restarted).
bool Tick();
RECT ScreenRect();
}  // namespace widget
