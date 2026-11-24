#pragma once
// Demo mode (`ACTaskbar.exe --demo [cool|heat|dry|fan|auto|off]`): the whole app on made-up data, with no AC
// and no network. Clicks are applied on screen only, and nothing is saved (schedules, beep) or sent. Used for
// the README screenshots, and to try the app without a Midea AC.
#include <string>
#include <vector>

#include "schedule.h"
#include "worker.h"

namespace demo {

Snapshot MakeSnapshot(const std::wstring& mode);
Schedule MakeSchedule();
std::vector<float> PowerHistory(const std::wstring& mode);  // the last ten minutes, one sample every 10 s

}  // namespace demo
