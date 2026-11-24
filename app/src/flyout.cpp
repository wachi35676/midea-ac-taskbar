#include "flyout.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "art.h"
#include "backdrop.h"
#include "gfx.h"
#include "model.h"

// The same panel code twice (flyout_view.inc): the click panel and the desktop widget. Constant-condition and
// unused-function warnings are expected here, because each copy only uses part of the shared code.
#pragma warning(disable : 4127 4505)

#define VIEW_NS popup_view
#define VIEW_WIDGET 0
#define VIEW_CLASS L"ACTaskbarPanel"
#include "flyout_view.inc"
#undef VIEW_NS
#undef VIEW_WIDGET
#undef VIEW_CLASS

#define VIEW_NS widget_view
#define VIEW_WIDGET 1
#define VIEW_CLASS L"ACTaskbarWidget"
#include "flyout_view.inc"
#undef VIEW_NS
#undef VIEW_WIDGET
#undef VIEW_CLASS

namespace flyout {

void Show(HINSTANCE inst, const RECT& anchor, const Snapshot& snap, bool pending, const Schedule& sched, Callbacks cb) {
    popup_view::Show(inst, anchor, snap, pending, sched, std::move(cb), WidgetOptions{});
}
void Update(const Snapshot& snap, bool pending, const Schedule& sched) { popup_view::Update(snap, pending, sched, WidgetOptions{}); }
void Hide() { popup_view::Hide(); }
bool Visible() { return popup_view::Visible(); }
void SetBackdrop(Backdrop b) { popup_view::SetBackdrop(b); }
bool JustClosed() { return popup_view::JustClosed(); }
void SeedPowerHistory(const std::vector<float>& watts, double spacingSeconds) {
    popup_view::SeedWatts(watts, spacingSeconds);
    widget_view::SeedWatts(watts, spacingSeconds);
}

void ShowWidget(HINSTANCE inst, const Snapshot& snap, bool pending, const Schedule& sched, Callbacks cb, const WidgetOptions& opts) {
    widget_view::Show(inst, RECT{}, snap, pending, sched, std::move(cb), opts);
}
void UpdateWidget(const Snapshot& snap, bool pending, const Schedule& sched, const WidgetOptions& opts) {
    widget_view::Update(snap, pending, sched, opts);
}
void HideWidget() { widget_view::Hide(); }
bool WidgetVisible() { return widget_view::Visible(); }

}  // namespace flyout
