#include "worker.h"

#include <chrono>

using clock_type = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
constexpr auto kFastInterval = 2s;    // right after a change, so the panel shows the unit settling
constexpr auto kFastWindow = 12s;
constexpr auto kDebounce = 350ms;     // rapid clicks (temperature + + +) become one write
constexpr auto kMaxBackoff = 60s;
}  // namespace

void Worker::Start(std::wstring credsPath, HWND notify, UINT msg) {
    credsPath_ = std::move(credsPath);
    notify_ = notify;
    msg_ = msg;
    thread_ = std::thread([this] { Run(); });
}

void Worker::Stop() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (!thread_.joinable()) return;
    // A network call can take a few seconds to time out; don't hold up exit for it.
    HANDLE h = thread_.native_handle();
    if (WaitForSingleObject(h, 3000) == WAIT_OBJECT_0) thread_.join();
    else thread_.detach();
}

unsigned Worker::Queue(const midea::ac::Changes& changes) {
    unsigned seq;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (const auto& c : changes) {  // later values for the same key replace earlier ones
            bool replaced = false;
            for (auto& p : pending_)
                if (p.first == c.first) { p.second = c.second; replaced = true; }
            if (!replaced) pending_.push_back(c);
        }
        seq = ++queuedSeq_;
    }
    cv_.notify_all();
    return seq;
}

void Worker::SetInterval(int seconds) {
    intervalSec_ = seconds < 2 ? 2 : seconds;
    cv_.notify_all();
}

void Worker::SetBeep(bool on) {
    beep_ = on ? 1 : 0;
}

void Worker::RefreshNow() {
    {
        std::lock_guard<std::mutex> l(mu_);
        refresh_ = true;
    }
    cv_.notify_all();
}

void Worker::Post(Snapshot s) {
    auto* p = new Snapshot(std::move(s));
    if (!PostMessageW(notify_, msg_, 0, reinterpret_cast<LPARAM>(p))) delete p;
}

void Worker::Run() {
    midea::ac::Controller ac(credsPath_);
    Snapshot snap;
    unsigned appliedSeq = 0;
    int failures = 0;
    auto fastUntil = clock_type::now();
    auto nextPoll = clock_type::now();  // poll at once
    Post(snap);                         // "Connecting"

    auto fill = [&](bool ok, const std::string& err) {
        snap.hasData = snap.hasData || ok;
        if (ok || ac.Online()) {
            snap.state = ac.state();
            snap.caps = ac.capabilities();
        }
        if (ok) {
            snap.status = Snapshot::Status::Online;
            snap.error.clear();
            char b[16];
            SYSTEMTIME t;
            GetLocalTime(&t);
            snprintf(b, sizeof(b), "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
            snap.updatedAt = b;
        } else {
            snap.error = err;
            if (err.find("cannot open credentials") != std::string::npos ||
                err.find("missing ip") != std::string::npos)
                snap.status = Snapshot::Status::NoCredentials;
            else if (!ac.Online())
                snap.status = Snapshot::Status::Offline;
        }
        snap.appliedSeq = appliedSeq;
        snap.busy = false;
    };

    for (;;) {
        midea::ac::Changes changes;
        bool refresh = false;
        unsigned seq = 0;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait_until(l, nextPoll, [&] { return stop_ || refresh_ || !pending_.empty(); });
            if (stop_) return;
            if (!pending_.empty()) {
                // Let a burst of clicks finish, then send them together.
                cv_.wait_for(l, kDebounce, [&] { return stop_; });
                if (stop_) return;
                changes.swap(pending_);
                seq = queuedSeq_;
            }
            refresh = refresh_;
            refresh_ = false;
        }

        if (const int b = beep_.exchange(-1); b >= 0) ac.SetBeep(b != 0);

        std::string err;
        if (!changes.empty()) {
            snap.busy = true;
            Post(snap);
            const bool ok = ac.Set(changes, &err);
            appliedSeq = seq;
            fill(ok, err);
            if (ok) snap.status = Snapshot::Status::Online;
            Post(snap);
            fastUntil = clock_type::now() + kFastWindow;
            nextPoll = clock_type::now() + kFastInterval;
            continue;
        }

        if (refresh || clock_type::now() >= nextPoll) {
            const bool ok = ac.Poll(&err);
            fill(ok, err);
            Post(snap);
            if (ok) {
                failures = 0;
                const auto interval = std::chrono::seconds(intervalSec_.load());
                nextPoll = clock_type::now() + (clock_type::now() < fastUntil ? kFastInterval : interval);
            } else {
                ++failures;
                auto backoff = 5s * (1 << (failures > 4 ? 4 : failures - 1));  // 5, 10, 20, 40 s
                nextPoll = clock_type::now() + (backoff > kMaxBackoff ? kMaxBackoff : backoff);
            }
        }
    }
}
