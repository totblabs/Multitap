#pragma once
// gamepad-v1 (docs/remote-session-protocol.md "Gamepads"): this PC's XInput controllers for the session's Host.
// The channel "multitap/gamepad": Dart sends "start" and "stop" (polling) and "rumble" {slot, low, high}; this side
// sends "state" {slot, buttons, lt, rt, lx, ly, rx, ry} on every change and "removed" {slot}. A small thread polls
// the connected slots every 4 ms on a high-resolution timer (an absent slot once a second: XInput is slow to answer
// for one) and posts to the window's thread, which tells Dart. XInput is loaded at run time (xinput1_4, else
// xinput9_1_0), so nothing is linked. ASCII only.
#include <windows.h>
#include <xinput.h>

#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace multitap {

// Posted to the root window when a slot changed.
constexpr UINT kGamepadMessage = WM_APP + 189;

class GamepadInput {
 public:
  using V = flutter::EncodableValue;
  GamepadInput(flutter::BinaryMessenger* messenger, HWND window) : window_(window) {
    module_ = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module_) module_ = LoadLibraryExW(L"xinput9_1_0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module_) {
      get_ = reinterpret_cast<GetState>(reinterpret_cast<void*>(GetProcAddress(module_, "XInputGetState")));
      set_ = reinterpret_cast<SetState>(reinterpret_cast<void*>(GetProcAddress(module_, "XInputSetState")));
    }
    channel_ = std::make_unique<flutter::MethodChannel<V>>(messenger, "multitap/gamepad", &flutter::StandardMethodCodec::GetInstance());
    channel_->SetMethodCallHandler([this](const auto& call, auto result) { Handle(call, std::move(result)); });
  }
  ~GamepadInput() {
    if (channel_) channel_->SetMethodCallHandler(nullptr);
    Stop();
    if (module_) FreeLibrary(module_);
  }
  // On the window's thread: the changes the poller posted. True when the message was this one. [human]: a button,
  // a trigger past XInput's threshold or a stick outside its dead zone moved (the idle sign-out counts it as input,
  // as a key or the mouse; a stick that drifts inside its dead zone does not).
  bool HandleMessage(UINT message, bool* human = nullptr) {
    if (message != kGamepadMessage) return false;
    std::array<Slot, XUSER_MAX_COUNT> changed{};
    {
      std::lock_guard lock(mutex_);
      posted_ = false;
      for (size_t i = 0; i < slots_.size(); ++i) { changed[i] = slots_[i]; slots_[i].dirty = slots_[i].removed = false; }
    }
    if (!channel_ || !running_) return true;
    for (size_t i = 0; i < changed.size(); ++i) {
      if (changed[i].removed) {
        // The slot's pad is gone, and with it what this window set on its motors; another pad may take the slot.
        channel_->InvokeMethod("removed", std::make_unique<V>(int32_t(i)));
        delivered_[i] = XINPUT_GAMEPAD{}; vibrating_[i] = false;
      }
      else if (changed[i].dirty) {
        const auto& pad = changed[i].pad;
        if (human && Moved(delivered_[i], pad)) *human = true;
        delivered_[i] = pad;
        flutter::EncodableMap state{{V("slot"), V(int32_t(i))}, {V("buttons"), V(int32_t(pad.wButtons))}, {V("lt"), V(int32_t(pad.bLeftTrigger))}, {V("rt"), V(int32_t(pad.bRightTrigger))},
                                    {V("lx"), V(int32_t(pad.sThumbLX))}, {V("ly"), V(int32_t(pad.sThumbLY))}, {V("rx"), V(int32_t(pad.sThumbRX))}, {V("ry"), V(int32_t(pad.sThumbRY))}};
        channel_->InvokeMethod("state", std::make_unique<V>(std::move(state)));
      }
    }
    return true;
  }

 private:
  using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
  using SetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
  struct Slot { bool connected = false, dirty = false, removed = false; DWORD packet = 0; XINPUT_GAMEPAD pad{}; };

  static bool Moved(const XINPUT_GAMEPAD& before, const XINPUT_GAMEPAD& after) {
    if (before.wButtons != after.wButtons) return true;
    const auto trigger = [](BYTE was, BYTE now) {
      return now > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? now != was : was > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    };
    const auto stick = [](SHORT wasX, SHORT wasY, SHORT x, SHORT y, long long zone) {
      return (wasX != x || wasY != y) && (long long)x * x + (long long)y * y > zone * zone;
    };
    return trigger(before.bLeftTrigger, after.bLeftTrigger) || trigger(before.bRightTrigger, after.bRightTrigger) ||
           stick(before.sThumbLX, before.sThumbLY, after.sThumbLX, after.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ||
           stick(before.sThumbRX, before.sThumbRY, after.sThumbRX, after.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
  }

  void Handle(const flutter::MethodCall<V>& call, std::unique_ptr<flutter::MethodResult<V>> result) {
    const auto& method = call.method_name();
    if (method == "start") { Start(); result->Success(V(get_ != nullptr)); return; }
    if (method == "stop") { Stop(); result->Success(); return; }
    if (method == "rumble") {
      const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
      const auto number = [&](const char* key) -> int { if (!args) return -1; const auto it = args->find(V(key)); const auto* v = it == args->end() ? nullptr : std::get_if<int32_t>(&it->second); return v ? *v : -1; };
      const int slot = number("slot"), low = number("low"), high = number("high");
      if (slot < 0 || slot >= int(XUSER_MAX_COUNT) || low < 0 || low > 255 || high < 0 || high > 255) { result->Error("invalid_arguments", "Expected slot, low, high."); return; }
      if (set_) { XINPUT_VIBRATION vibration{WORD(low * 257), WORD(high * 257)}; set_(DWORD(slot), &vibration); }
      vibrating_[size_t(slot)] = low != 0 || high != 0;
      result->Success();
      return;
    }
    result->NotImplemented();
  }
  void Start() {
    if (running_ || !get_) return;
    {
      std::lock_guard lock(mutex_);
      for (auto& slot : slots_) slot = Slot{};
      posted_ = false;
    }
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!timer_ || !stop_) { Close(); return; }
    LARGE_INTEGER due; due.QuadPart = -1;
    SetWaitableTimerEx(timer_, &due, 4, nullptr, nullptr, nullptr, 0);   // every 4 ms
    running_ = true;
    thread_ = std::thread([this] { Poll(); });
  }
  void Stop() {
    if (!running_) return;
    running_ = false;
    if (stop_) SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
    // Only the motors this window set: the pads are shared by every window of the PC, and another may be driving them.
    if (set_) for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) if (vibrating_[i]) { XINPUT_VIBRATION off{}; set_(i, &off); }
    vibrating_.fill(false);
    delivered_.fill(XINPUT_GAMEPAD{});
    Close();
  }
  void Close() {
    if (timer_) { CancelWaitableTimer(timer_); CloseHandle(timer_); }
    if (stop_) CloseHandle(stop_);
    timer_ = stop_ = nullptr;
  }
  void Poll() {
    std::array<std::chrono::steady_clock::time_point, XUSER_MAX_COUNT> absentCheck{};
    const HANDLE handles[2] = {stop_, timer_};
    while (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
      const auto now = std::chrono::steady_clock::now();
      bool changed = false;
      for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        bool connected;
        { std::lock_guard lock(mutex_); connected = slots_[i].connected; }
        if (!connected && now - absentCheck[i] < std::chrono::seconds(1)) continue;
        if (!connected) absentCheck[i] = now;
        XINPUT_STATE state{};
        const bool present = get_(i, &state) == ERROR_SUCCESS;
        std::lock_guard lock(mutex_);
        auto& slot = slots_[i];
        if (!present) { if (slot.connected) { slot = Slot{}; slot.removed = true; changed = true; } continue; }
        if (!slot.connected || state.dwPacketNumber != slot.packet || std::memcmp(&state.Gamepad, &slot.pad, sizeof(slot.pad)) != 0) {
          slot.connected = true; slot.packet = state.dwPacketNumber; slot.pad = state.Gamepad; slot.dirty = true; slot.removed = false; changed = true;
        }
      }
      if (changed) {
        std::lock_guard lock(mutex_);
        if (!posted_) { posted_ = true; PostMessageW(window_, kGamepadMessage, 0, 0); }
      }
    }
  }

  HWND window_;
  HMODULE module_ = nullptr;
  GetState get_ = nullptr;
  SetState set_ = nullptr;
  std::unique_ptr<flutter::MethodChannel<V>> channel_;
  std::mutex mutex_;
  std::array<Slot, XUSER_MAX_COUNT> slots_{};
  // The window's thread only: each slot's last state given to Dart, and the slots whose motors this window set.
  std::array<XINPUT_GAMEPAD, XUSER_MAX_COUNT> delivered_{};
  std::array<bool, XUSER_MAX_COUNT> vibrating_{};
  bool posted_ = false;
  std::atomic<bool> running_{false};
  HANDLE timer_ = nullptr, stop_ = nullptr;
  std::thread thread_;
};

}  // namespace multitap
