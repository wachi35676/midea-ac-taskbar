#pragma once
// The panel with every reading and every control. It is shown two ways, each with its own window:
//  - the click panel, which opens above the taskbar strip and closes when it loses focus;
//  - the desktop widget, the same panel as a window that stays on the desktop, never takes focus, can be
//    dragged by any empty area, and remembers where it is.
#include <windows.h>

#include <functional>
#include <vector>

#include "backdrop.h"
#include "schedule.h"
#include "worker.h"

namespace flyout {

struct Callbacks {
    std::function<void(const midea::ac::Changes&)> change;
    std::function<void()> refresh;
    std::function<void()> exit;
    std::function<void(const Schedule&)> schedule;  // the user changed a timer
};

// ---- the click panel ----

// Opens above `anchor` (the strip's screen rectangle). `pending`: a change is on its way to the AC.
void Show(HINSTANCE inst, const RECT& anchor, const Snapshot& snap, bool pending, const Schedule& sched, Callbacks cb);
void Update(const Snapshot& snap, bool pending, const Schedule& sched);
void Hide();
bool Visible();
// The panel's backdrop (Mica, Acrylic or clear glass); applies at once if it is open.
void SetBackdrop(Backdrop b);
// True for a moment after the panel closed because it lost focus, so the click on the strip that
// caused that doesn't immediately reopen it.
bool JustClosed();
// Fills the power chart (demo mode): `watts`, oldest first, `spacingSeconds` apart, ending now.
void SeedPowerHistory(const std::vector<float>& watts, double spacingSeconds);

// ---- the desktop widget ----

constexpr int kWidgetSizes = 5;  // 75 %, 100 %, 125 %, 150 %, 200 %

struct WidgetOptions {
    int size = 1;                  // 0..kWidgetSizes-1
    int background = 0;            // 0 clear glass, 1 frosted glass, 2 solid
    bool onTop = false;            // above other windows
    bool locked = false;           // can't be dragged
    bool hasPosition = false;      // x, y are where the user left it; false: the default place (bottom right of the second screen)
    int x = 0, y = 0;
    std::function<void(int, int)> moved;  // it was placed or dragged: save x, y
    std::function<void()> menu;           // right-click
};

void ShowWidget(HINSTANCE inst, const Snapshot& snap, bool pending, const Schedule& sched, Callbacks cb, const WidgetOptions& opts);
void UpdateWidget(const Snapshot& snap, bool pending, const Schedule& sched, const WidgetOptions& opts);
void HideWidget();
bool WidgetVisible();

}  // namespace flyout
