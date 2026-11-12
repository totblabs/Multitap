#pragma once
#include <windows.h>

namespace multitap {
// An active viewer requests wakefulness only for its own lifetime. This does
// not change the power plan, lock policy, display configuration or Windows UAC.
class SessionPowerActivity {
 public:
  ~SessionPowerActivity() { Set(false); }
  bool Set(bool enabled) {
    if (!enabled) {
      if (request_ && request_ != INVALID_HANDLE_VALUE) {
        if (display_) PowerClearRequest(request_, PowerRequestDisplayRequired);
        if (system_) PowerClearRequest(request_, PowerRequestSystemRequired);
        CloseHandle(request_);
      }
      request_=nullptr; display_=system_=false; return true;
    }
    if (active()) return true;
    Set(false);
    REASON_CONTEXT reason{}; reason.Version=POWER_REQUEST_CONTEXT_VERSION;
    reason.Flags=POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    reason.Reason.SimpleReasonString=const_cast<PWSTR>(L"Multitap active remote desktop session");
    request_=PowerCreateRequest(&reason);
    if (!request_ || request_==INVALID_HANDLE_VALUE) { request_=nullptr; return false; }
    system_=PowerSetRequest(request_,PowerRequestSystemRequired)!=FALSE;
    display_=PowerSetRequest(request_,PowerRequestDisplayRequired)!=FALSE;
    if (!active()) { Set(false); return false; }
    return true;
  }
  bool active() const { return system_ && display_; }
  SessionPowerActivity() = default;
  SessionPowerActivity(const SessionPowerActivity&) = delete;
 private:
  HANDLE request_=nullptr;
  bool system_=false, display_=false;
};
}
