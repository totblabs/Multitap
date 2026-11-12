#include "remote_input.h"
namespace multitap {
using V = flutter::EncodableValue;
void RemoteInput::SetTextInputEnabled(bool enabled) {
  if (enabled != text_input_enabled_) text_high_surrogate_ = 0;
  text_input_enabled_ = enabled;
}
bool RemoteInput::Capture(HWND window, bool enabled) {
  if (!enabled) {
    ++capture_generation_;
    ++relative_toggle_generation_;
    pressed_.fill(false);
    toggle_held_ = false;
    text_input_enabled_ = false;
    text_high_surrogate_ = 0;
    if (hook_) {
      UnhookWindowsHookEx(hook_); hook_ = nullptr;
      if (active_ == this) active_ = nullptr;
      callback_({{V("type"), V("input.reset")}});
      callback_({{V("type"), V("capture.released")}});
    }
    return true;
  }
  if (hook_) return true;
  if (!window || GetAncestor(GetForegroundWindow(), GA_ROOT) != GetAncestor(window, GA_ROOT)) return false;
  if (active_ && active_ != this) active_->Capture(nullptr, false);
  ++capture_generation_;
  ++relative_toggle_generation_;
  pressed_.fill(false);
  toggle_held_ = false;
  window_ = window; active_ = this;
  hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, Hook, GetModuleHandleW(nullptr), 0);
  if (!hook_) { active_ = nullptr; return false; }
  return true;
}
LRESULT CALLBACK RemoteInput::Hook(int code, WPARAM message, LPARAM data) {
  RemoteInput* self = active_;
  if (code < 0 || !self) return CallNextHookEx(nullptr, code, message, data);
  const auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
  const bool foreground = GetAncestor(GetForegroundWindow(), GA_ROOT) == GetAncestor(self->window_, GA_ROOT);
  return self->HandleKey(message, *key, foreground) ? 1 : CallNextHookEx(nullptr, code, message, data);
}
bool RemoteInput::HandleKey(WPARAM message, const KBDLLHOOKSTRUCT& key, bool foreground) {
  // A foreground nested client receives legitimate outer Host SendInput.
  // Keep lower-integrity and explicitly marked integrity-QA input excluded.
  // The QA marker is not provenance for unmarked same-PC network feedback.
  if ((key.flags & LLKHF_LOWER_IL_INJECTED) ||
      ((key.flags & LLKHF_INJECTED) && key.dwExtraInfo == ULONG_PTR(0x54425141))) return false;
  if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN &&
      message != WM_KEYUP && message != WM_SYSKEYUP) return false;
  if (!foreground) { Capture(nullptr, false); return false; }
  const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
  if (key.vkCode == VK_PACKET) return HandleTextPacket(key, down);
  // Ordinary editing/modifier keys interrupt a partial Unicode scalar.
  text_high_surrogate_ = 0;
  if (key.vkCode == 0 || key.vkCode >= pressed_.size()) return false;
  // Dedicated Korean language keys can arrive as a break-only F2/F1 event.
  // Sending that release alone cannot perform the Host's semantic IME action.
  // Complete only this exact unpaired shape; paired 101-key Right Alt/Ctrl,
  // ordinary orphan releases and every other VK/scan retain their own meaning.
  if (!down && !pressed_[key.vkCode] &&
      ((key.vkCode == VK_HANGUL && key.scanCode == 0xf2) ||
       (key.vkCode == VK_HANJA && key.scanCode == 0xf1))) {
    const auto generation = capture_generation_;
    auto make = key;
    make.flags &= ~DWORD(LLKHF_UP);
    HandleKey(message == WM_SYSKEYUP ? WM_SYSKEYDOWN : WM_KEYDOWN, make, true);
    // A callback may release/reacquire capture. Its reset owns the old press;
    // never send the remaining release through a successor input owner.
    if (capture_generation_ != generation) return true;
  }
  // WH_KEYBOARD_LL runs before Windows updates GetAsyncKeyState. Track the
  // captured stream itself, including left/right modifier release and repeats.
  const bool was_down = pressed_[key.vkCode];
  pressed_[key.vkCode] = down;
  const bool ctrl = pressed_[VK_CONTROL] || pressed_[VK_LCONTROL] || pressed_[VK_RCONTROL];
  const bool alt = pressed_[VK_MENU] || pressed_[VK_LMENU] || pressed_[VK_RMENU];
  const bool shift = pressed_[VK_SHIFT] || pressed_[VK_LSHIFT] || pressed_[VK_RSHIFT];
  // Not the key, its repeats or its release reach the remote.
  if (key.vkCode == 'Z' && (toggle_held_ || (down && !was_down && relative_toggle_ && ctrl && alt && shift))) {
    if (!down) toggle_held_ = false;
    else if (!toggle_held_) {
      toggle_held_ = true;
      PostMessageW(GetAncestor(window_, GA_ROOT), kRelativeMouseToggleMessage,
                   relative_toggle_generation_, 0);
    }
    return true;
  }
  if (down && ctrl && alt && ((shift && key.vkCode == VK_F12) || key.vkCode == VK_DELETE)) {
    const bool release_key = key.vkCode == VK_F12;
    Capture(nullptr, false);
    if (release_key) PostMessageW(GetAncestor(window_, GA_ROOT), WM_APP + 180, 0, 0);
    return release_key;
  }
  callback_({
      {V("type"), V("input")}, {V("event"), V("key")}, {V("vk"), V(static_cast<int32_t>(key.vkCode))},
      {V("scan"), V(static_cast<int32_t>(key.scanCode))}, {V("down"), V(down)},
      {V("extended"), V((key.flags & LLKHF_EXTENDED) != 0)}});
  return true;
}
bool RemoteInput::HandleTextPacket(const KBDLLHOOKSTRUCT& key, bool down) {
  // VK_PACKET scanCode is a UTF-16 unit, never a hardware scan. Do not relay
  // packet keyups, or route an unsupported packet through ordinary key input.
  if (!text_input_enabled_) { text_high_surrogate_ = 0; return true; }
  if (!down) return true;
  const auto unit = key.scanCode;
  if (unit > 0xffff || unit < 0x20 || (unit >= 0x7f && unit <= 0x9f)) {
    text_high_surrogate_ = 0;
    return true;
  }
  if (unit >= 0xd800 && unit <= 0xdbff) {
    text_high_surrogate_ = static_cast<uint16_t>(unit);
    return true;
  }
  const auto high = text_high_surrogate_;
  text_high_surrogate_ = 0;
  if (unit >= 0xdc00 && unit <= 0xdfff) {
    if (high) EmitText(0x10000u + ((uint32_t(high) - 0xd800u) << 10) + unit - 0xdc00u);
  } else {
    EmitText(unit);
  }
  return true;
}
void RemoteInput::EmitText(uint32_t scalar) {
  std::string text;
  if (scalar < 0x80) text.push_back(static_cast<char>(scalar));
  else if (scalar < 0x800) {
    text.push_back(static_cast<char>(0xc0 | (scalar >> 6)));
    text.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
  } else if (scalar < 0x10000) {
    text.push_back(static_cast<char>(0xe0 | (scalar >> 12)));
    text.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
    text.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
  } else {
    text.push_back(static_cast<char>(0xf0 | (scalar >> 18)));
    text.push_back(static_cast<char>(0x80 | ((scalar >> 12) & 0x3f)));
    text.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
    text.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
  }
  // Partial state was retired before this callback; it may synchronously
  // release capture and start a successor owner.
  callback_({{V("type"), V("input")}, {V("event"), V("text")}, {V("text"), V(text)}});
}
}
