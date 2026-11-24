// AC Taskbar: shows the air conditioner on the taskbar and controls it from a click panel.
//   ACTaskbar.exe                 run (one instance)
//   ACTaskbar.exe --panel         run and open the panel once the first reading arrives (for testing)
//   ACTaskbar.exe --settings      run and open the settings window
//   ACTaskbar.exe --demo [mode]   run on made-up data (cool, heat, dry, fan, auto, off), no AC needed
//   ACTaskbar.exe --theme light|dark   force a theme in this app only (for screenshots)
//   ACTaskbar.exe --quit          close the running instance
//   ACTaskbar.exe --creds FILE    use another ac_creds.json
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "demo.h"
#include "flyout.h"
#include "gfx.h"
#include "model.h"
#include "schedule.h"
#include "settings.h"
#include "widget.h"
#include "worker.h"

namespace {

constexpr UINT WM_APP_SNAPSHOT = WM_APP + 1;
constexpr UINT WM_APP_CLICK = WM_APP + 2;
constexpr UINT WM_APP_MENU = WM_APP + 3;
constexpr UINT kTimerTick = 1;
constexpr wchar_t kWndClass[] = L"ACTaskbarWnd";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ACTaskbar";

enum MenuId : UINT { IdOpen = 1, IdSettings, IdRefresh, IdExit };

HINSTANCE g_inst;
HWND g_wnd;
UINT g_taskbarCreated;
Worker g_worker;
Snapshot g_last;
std::vector<std::pair<unsigned, midea::ac::Changes>> g_pending;  // queued, not yet confirmed
bool g_openPanelOnData = false;
bool g_demo = false;  // made-up data: nothing is sent to the AC or saved
Schedule g_sched;

// Settings under HKCU\Software\ACTaskbar.
DWORD GetSetting(const wchar_t* name, DWORD def) {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\ACTaskbar", name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : def;
}
void PutSetting(const wchar_t* name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ACTaskbar", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(k);
}
constexpr int kIntervals[] = {5, 10, 30, 60};
int IntervalIndex() { return static_cast<int>(GetSetting(L"Interval", 1) % 4); }

std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/"));
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ac_creds.json next to the exe, in a parent folder (the project root during development), or in
// %APPDATA%\ACTaskbar.
std::wstring FindCreds() {
    std::wstring dir = ExeDir();
    for (int i = 0; i < 4; ++i) {
        if (FileExists(dir + L"\\ac_creds.json")) return dir + L"\\ac_creds.json";
        const size_t cut = dir.find_last_of(L"\\/");
        if (cut == std::wstring::npos) break;
        dir = dir.substr(0, cut);
    }
    wchar_t appdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return std::wstring(appdata) + L"\\ACTaskbar\\ac_creds.json";
    return ExeDir() + L"\\ac_creds.json";
}

// Unit codes: the AC sometimes reports a code for a moment (seen as 25 while it was switched on and off)
// that clears by itself. Only a code that stays 20 s or more is shown as an error.
int g_codeSeen = 0;
ULONGLONG g_codeSince = 0;
int g_lastCode = 0;
std::string g_lastCodeAt;

void TrackUnitCode(int code) {
    if (code == 0) { g_codeSeen = 0; return; }
    if (code != g_codeSeen) { g_codeSeen = code; g_codeSince = GetTickCount64(); }
    if (code != g_lastCode || g_lastCodeAt.empty()) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char b[16];
        snprintf(b, sizeof(b), "%02d:%02d", t.wHour, t.wMinute);
        g_lastCode = code;
        g_lastCodeAt = b;
    }
}

// The latest snapshot with every unconfirmed change replayed on top, so clicks show at once.
Snapshot Display(bool* pending) {
    Snapshot s = g_last;
    const bool persistent = g_codeSeen && GetTickCount64() - g_codeSince >= 20000;
    if (!persistent) s.state.errorCode = 0;
    s.lastUnitCode = g_lastCode;
    s.lastUnitCodeAt = g_lastCodeAt;
    bool any = false;
    for (const auto& [seq, changes] : g_pending) {
        if (seq <= g_last.appliedSeq) continue;
        model::Apply(s.state, changes);
        any = true;
    }
    if (pending) *pending = any;
    return s;
}

flyout::WidgetOptions DeskOptions();

void Refresh() {
    bool pending = false;
    const Snapshot s = Display(&pending);
    widget::Update(model::Strip(s));
    flyout::Update(s, pending, g_sched);  // also records the power history while hidden
    flyout::UpdateWidget(s, pending, g_sched, DeskOptions());
}

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

void Exit() {
    flyout::Hide();
    flyout::HideWidget();
    settings::Close();
    DestroyWindow(g_wnd);
}

void OpenSettings();

// What the click panel and the desktop widget do when you use them.
flyout::Callbacks MakeCallbacks() {
    flyout::Callbacks cb;
    cb.change = [](const midea::ac::Changes& c) {
        for (const auto& [k, v] : c)
            if (k == "beep" && !g_demo) PutSetting(L"Beep", v == "true");  // the AC doesn't remember it; the app does
        const unsigned seq = g_worker.Queue(c);
        g_pending.emplace_back(seq, c);
        Refresh();
    };
    cb.refresh = [] { g_worker.RefreshNow(); };
    cb.exit = [] { Exit(); };
    cb.schedule = [](const Schedule& s) {
        g_sched = s;
        if (!g_demo) schedule::Save(g_sched);
        Refresh();
    };
    return cb;
}

// ---- the desktop widget (settings under HKCU\Software\ACTaskbar: DesktopWidget*) ----

bool DesktopWidgetWanted() { return GetSetting(L"DesktopWidget", 1) != 0; }

void ShowWidgetMenu() {
    const bool locked = GetSetting(L"DesktopWidgetLocked", 0) != 0;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Settings");
    AppendMenuW(m, MF_STRING | (locked ? MF_CHECKED : 0), 2, L"Lock position");
    AppendMenuW(m, MF_STRING, 3, L"Reset position");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 4, L"Hide widget");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    const UINT id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (id) {
        case 1: OpenSettings(); break;
        case 2: PutSetting(L"DesktopWidgetLocked", locked ? 0 : 1); Refresh(); break;
        case 3: PutSetting(L"DesktopWidgetPlaced", 0); Refresh(); break;
        case 4: PutSetting(L"DesktopWidget", 0); flyout::HideWidget(); break;
        default: break;
    }
}

flyout::WidgetOptions DeskOptions() {
    flyout::WidgetOptions o;
    o.size = static_cast<int>(std::min<DWORD>(GetSetting(L"DesktopWidgetSize", 1), flyout::kWidgetSizes - 1));
    o.background = static_cast<int>(std::min<DWORD>(GetSetting(L"DesktopWidgetBackdrop", 0), 2));
    o.onTop = GetSetting(L"DesktopWidgetOnTop", 0) != 0;
    o.locked = GetSetting(L"DesktopWidgetLocked", 0) != 0;
    o.hasPosition = GetSetting(L"DesktopWidgetPlaced", 0) != 0;
    o.x = static_cast<int>(GetSetting(L"DesktopWidgetX", 0));
    o.y = static_cast<int>(GetSetting(L"DesktopWidgetY", 0));
    o.moved = [](int x, int y) {
        PutSetting(L"DesktopWidgetX", static_cast<DWORD>(x));
        PutSetting(L"DesktopWidgetY", static_cast<DWORD>(y));
        PutSetting(L"DesktopWidgetPlaced", 1);
    };
    o.menu = [] { ShowWidgetMenu(); };
    return o;
}

// Brings the widget up when it is wanted (also at startup) and takes it away when it is not. Cheap: runs every second.
void EnsureDesktopWidget() {
    if (!DesktopWidgetWanted()) {
        if (flyout::WidgetVisible()) flyout::HideWidget();
        return;
    }
    if (flyout::WidgetVisible()) return;
    bool pending = false;
    const Snapshot s = Display(&pending);
    flyout::ShowWidget(g_inst, s, pending, g_sched, MakeCallbacks(), DeskOptions());
}

void OpenPanel() {
    bool pending = false;
    const Snapshot s = Display(&pending);
    flyout::Show(g_inst, widget::ScreenRect(), s, pending, g_sched, MakeCallbacks());
}

void OpenSettings() {
    settings::Show(
        g_inst,
        [] {
            settings::State s;
            s.backdrop = static_cast<int>(GetSetting(L"Backdrop", 2) % 3);
            s.left = GetSetting(L"StripLeft", 0) != 0;
            s.allMonitors = GetSetting(L"AllMonitors", 1) != 0;
            s.autostart = AutostartEnabled();
            s.desktop = DesktopWidgetWanted();
            s.desktopSize = static_cast<int>(std::min<DWORD>(GetSetting(L"DesktopWidgetSize", 1), flyout::kWidgetSizes - 1));
            s.desktopBackground = static_cast<int>(std::min<DWORD>(GetSetting(L"DesktopWidgetBackdrop", 0), 2));
            s.desktopOnTop = GetSetting(L"DesktopWidgetOnTop", 0) != 0;
            s.desktopLocked = GetSetting(L"DesktopWidgetLocked", 0) != 0;
            s.interval = IntervalIndex();
            s.address = std::wstring(g_last.ip.begin(), g_last.ip.end());
            switch (g_last.status) {
                case Snapshot::Status::Online:
                    s.status = L"Online" + (g_last.updatedAt.empty() ? std::wstring() : L", last read " + std::wstring(g_last.updatedAt.begin(), g_last.updatedAt.end()));
                    break;
                case Snapshot::Status::Connecting: s.status = L"Connecting…"; break;
                case Snapshot::Status::Offline: s.status = L"Can't reach it right now; retrying"; break;
                case Snapshot::Status::NoCredentials: s.status = L"Not paired: run tools\\get_key.py once"; break;
            }
            return s;
        },
        [](settings::Action a, int v) {
            switch (a) {
                case settings::Action::Backdrop:
                    PutSetting(L"Backdrop", static_cast<DWORD>(v));
                    flyout::SetBackdrop(static_cast<Backdrop>(v));
                    break;
                case settings::Action::Side:
                    PutSetting(L"StripLeft", v == 0);
                    widget::SetSide(v == 0);
                    break;
                case settings::Action::Monitors:
                    PutSetting(L"AllMonitors", v == 0);
                    widget::SetAllMonitors(v == 0);
                    break;
                case settings::Action::Autostart: SetAutostart(v != 0); break;
                case settings::Action::Interval:
                    PutSetting(L"Interval", static_cast<DWORD>(v));
                    g_worker.SetInterval(kIntervals[v % 4]);
                    break;
                case settings::Action::Refresh: g_worker.RefreshNow(); break;
                case settings::Action::Exit: Exit(); break;
                case settings::Action::Desktop:
                    PutSetting(L"DesktopWidget", v != 0);
                    EnsureDesktopWidget();
                    break;
                case settings::Action::DesktopSize: PutSetting(L"DesktopWidgetSize", static_cast<DWORD>(v)); Refresh(); break;
                case settings::Action::DesktopBackground: PutSetting(L"DesktopWidgetBackdrop", static_cast<DWORD>(v)); Refresh(); break;
                case settings::Action::DesktopOnTop: PutSetting(L"DesktopWidgetOnTop", v != 0); Refresh(); break;
                case settings::Action::DesktopLocked: PutSetting(L"DesktopWidgetLocked", v != 0); Refresh(); break;
                case settings::Action::DesktopReset: PutSetting(L"DesktopWidgetPlaced", 0); Refresh(); break;
            }
        });
}

void ShowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IdOpen, L"Open panel");
    AppendMenuW(m, MF_STRING, IdSettings, L"Settings");
    AppendMenuW(m, MF_STRING, IdRefresh, L"Refresh now");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IdExit, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    const UINT id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (id) {
        case IdOpen: OpenPanel(); break;
        case IdSettings: OpenSettings(); break;
        case IdRefresh: g_worker.RefreshNow(); break;
        case IdExit: Exit(); break;
        default: break;
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == g_taskbarCreated && g_taskbarCreated) {  // Explorer restarted: rebuild the strip
        widget::Destroy();
        widget::Create(g_inst, h, WM_APP_CLICK, WM_APP_MENU);
        Refresh();
        return 0;
    }
    switch (m) {
        case WM_APP_SNAPSHOT: {
            std::unique_ptr<Snapshot> s(reinterpret_cast<Snapshot*>(lp));
            g_last = *s;
            if (g_last.status == Snapshot::Status::Online) TrackUnitCode(g_last.state.errorCode);
            // Drop changes the worker has sent (it reports the AC's answer in this snapshot).
            std::erase_if(g_pending, [](const auto& p) { return p.first <= g_last.appliedSeq; });
            Refresh();
            if (g_openPanelOnData && g_last.status != Snapshot::Status::Connecting) {
                g_openPanelOnData = false;
                OpenPanel();
            }
            return 0;
        }
        case WM_APP_CLICK:
            if (flyout::Visible()) flyout::Hide();
            else if (!flyout::JustClosed()) OpenPanel();
            return 0;
        case WM_APP_MENU: ShowMenu(); return 0;
        case WM_TIMER:
            if (wp != kTimerTick) return 0;
            if (!widget::Tick()) {  // taskbar gone or replaced
                widget::Destroy();
                if (widget::Create(g_inst, h, WM_APP_CLICK, WM_APP_MENU)) Refresh();
            }
            EnsureDesktopWidget();
            if (const int due = g_demo ? 0 : schedule::Due(g_sched, time(nullptr))) {
                schedule::Save(g_sched);
                const midea::ac::Changes c = {{"power", due > 0 ? "true" : "false"}};
                g_pending.emplace_back(g_worker.Queue(c), c);
                Refresh();
            }
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:  // accent changed
            Refresh();
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
                widget::Retheme();
                Refresh();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(h, kTimerTick);
            g_worker.Stop();
            widget::Destroy();
            PostQuitMessage(0);
            return 0;
        default: return DefWindowProcW(h, m, wp, lp);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    g_inst = inst;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring creds, demoMode = L"cool";
    bool quit = false, openSettings = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--quit") quit = true;
        else if (a == L"--panel") g_openPanelOnData = true;
        else if (a == L"--settings") openSettings = true;
        else if (a == L"--theme" && i + 1 < argc) gfx::SetThemeOverride(wcscmp(argv[++i], L"light") == 0 ? 1 : 0);
        else if (a == L"--demo") {
            g_demo = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') demoMode = argv[++i];
        }
        else if (a == L"--creds" && i + 1 < argc) creds = argv[++i];
    }
    LocalFree(argv);

    if (quit) {  // close the running copy and wait for it to exit (so a new copy can start straight after)
        if (HWND other = FindWindowW(kWndClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
            PostMessageW(other, WM_CLOSE, 0, 0);
            if (proc) {
                WaitForSingleObject(proc, 6000);
                CloseHandle(proc);
            }
        }
        return 0;
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\ACTaskbarSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClass;
    RegisterClassW(&wc);
    // A hidden top-level (not message-only) window, so it receives the TaskbarCreated broadcast.
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWndClass, L"AC Taskbar", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return 1;
    ChangeWindowMessageFilterEx(g_wnd, g_taskbarCreated, MSGFLT_ALLOW, nullptr);

    g_sched = g_demo ? demo::MakeSchedule() : schedule::Load();
    if (g_demo) {
        g_last = demo::MakeSnapshot(demoMode);
        flyout::SeedPowerHistory(demo::PowerHistory(demoMode), 10.0);
    }
    widget::SetSide(GetSetting(L"StripLeft", 0) != 0);
    widget::SetAllMonitors(GetSetting(L"AllMonitors", 1) != 0);
    flyout::SetBackdrop(static_cast<Backdrop>(GetSetting(L"Backdrop", 2) % 3));  // Clear glass by default
    widget::Create(inst, g_wnd, WM_APP_CLICK, WM_APP_MENU);
    EnsureDesktopWidget();
    SetTimer(g_wnd, kTimerTick, 1000, nullptr);
    g_worker.SetInterval(kIntervals[IntervalIndex()]);
    g_worker.SetBeep(GetSetting(L"Beep", 0) != 0);
    if (openSettings) OpenSettings();
    if (g_demo) {
        Refresh();
        if (g_openPanelOnData) OpenPanel();
    } else g_worker.Start(creds.empty() ? FindCreds() : creds, g_wnd, WM_APP_SNAPSHOT);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
