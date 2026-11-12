#ifndef MULTITAP_REMOTE_INPUT_H_
#define MULTITAP_REMOTE_INPUT_H_
#include <windows.h>
#include <flutter/encodable_value.h>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include "relative_mouse.h"
namespace multitap {
class RemoteInput {
 public:
  using Callback = std::function<void(flutter::EncodableMap)>;
  explicit RemoteInput(Callback callback) : callback_(std::move(callback)) {}
  ~RemoteInput() { Capture(nullptr, false); }
  bool Capture(HWND window, bool enabled);
  // Keyboard capture is active (the remote view owns input focus).
  bool captured() const { return hook_ != nullptr; }
  // The current captured session negotiated committed Unicode text. A new
  // policy cannot complete a partial packet left by the previous policy.
  void SetTextInputEnabled(bool enabled);
  // While the focused session grants mouse lock, Ctrl+Alt+Shift+Z toggles its
  // option locally, including from off. Retire messages queued by an earlier
  // capture or a previous arm/disarm cycle before the root dispatches them.
  void SetRelativeToggle(bool enabled) {
    if (relative_toggle_ != enabled) ++relative_toggle_generation_;
    relative_toggle_ = enabled;
  }
  bool RelativeTogglePending(WPARAM generation) const {
    return captured() && RelativeToggleCurrent(generation);
  }
 private:
  friend struct RemoteInputTestPeer;
  bool HandleKey(WPARAM message, const KBDLLHOOKSTRUCT& key, bool foreground);
  bool HandleTextPacket(const KBDLLHOOKSTRUCT& key, bool down);
  void EmitText(uint32_t scalar);
  bool RelativeToggleCurrent(WPARAM generation) const {
    return relative_toggle_ && generation == relative_toggle_generation_;
  }
  static LRESULT CALLBACK Hook(int code, WPARAM message, LPARAM data);
  inline static RemoteInput* active_ = nullptr;
  HWND window_ = nullptr;
  HHOOK hook_ = nullptr;
  std::array<bool, 256> pressed_{};
  uint64_t capture_generation_ = 0;
  uintptr_t relative_toggle_generation_ = 0;
  bool relative_toggle_ = false, toggle_held_ = false;
  bool text_input_enabled_ = false;
  uint16_t text_high_surrogate_ = 0;
  Callback callback_;
};
}
#endif
