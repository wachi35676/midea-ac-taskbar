#pragma once
// The AC worker thread: owns the midea::ac::Controller, polls it, applies queued changes, and posts
// snapshots to a window. The UI thread never waits on the network.
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "../../native/src/midea/ac_controller.h"

struct Snapshot {
    enum class Status { Connecting, Online, Offline, NoCredentials } status = Status::Connecting;
    bool hasData = false;            // state/caps below were read at least once
    midea::ac::State state;
    midea::ac::Capabilities caps;
    std::string error;               // last failure, empty when fine
    std::string updatedAt;           // ISO time of the last good read
    std::string ip;
    unsigned appliedSeq = 0;         // every change up to this sequence number has been sent
    bool busy = false;               // a change is being sent right now
    // Filled by the UI thread: the last non-zero code the unit reported, and when (codes that clear
    // within 20 s are treated as transient and not shown as errors).
    int lastUnitCode = 0;
    std::string lastUnitCodeAt;
};

class Worker {
public:
    // Posts `new Snapshot` (owned by the receiver) to `notify` with message `msg`.
    void Start(std::wstring credsPath, HWND notify, UINT msg);
    void Stop();                                       // waits up to 3 s, then detaches
    unsigned Queue(const midea::ac::Changes& changes);  // returns the change's sequence number
    void RefreshNow();
    void SetInterval(int seconds);
    void SetBeep(bool on);  // the saved beep choice, applied to the controller before its next action  // normal poll interval (fast polling after a change is separate)

private:
    void Run();
    void Post(Snapshot s);

    std::wstring credsPath_;
    HWND notify_ = nullptr;
    UINT msg_ = 0;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    midea::ac::Changes pending_;
    unsigned queuedSeq_ = 0;
    bool refresh_ = false, stop_ = false;
    std::atomic<int> intervalSec_{10};
    std::atomic<int> beep_{-1};  // -1: not set yet
};
