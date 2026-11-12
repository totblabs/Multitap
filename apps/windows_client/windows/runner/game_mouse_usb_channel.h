#pragma once
// The runner's side of the game mouse over USB (game_mouse_usb.h): the Dart
// channel "multitap/game-mouse-usb" and the relative mouse's sink. Dart owns
// the usb-v1 socket: it tells this side when the VM took the device
// ("attached" {device}), passes each Host frame for it ("frame", answered
// "ok", "older" or "invalid") and gets every frame to send ("send") and the
// first read of a Host that answers the device itself ("confirmed").
// ASCII only; everything runs on the window's thread except the wake thread,
// which only posts a message.
#include <windows.h>

#include <flutter/binary_messenger.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

#include "game_mouse_usb.h"
#include "relative_mouse.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace multitap {

// Posted to the root window when a paced report may go.
constexpr UINT kGameMouseWakeMessage = WM_APP + 188;

// Wakes the window's thread at a paced report's time to well under a
// millisecond (a high-resolution waitable timer on a small thread) instead of
// the ~15 ms window timer, which would hold the last motion of a stroke back.
class GameMouseWake {
 public:
  ~GameMouseWake() { Stop(); }
  void Start(HWND window) {
    if (thread_.joinable()) return;
    window_ = window;
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);  // before Windows 10 1803
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!timer_ || !stop_) { Close(); return; }
    thread_ = std::thread([this] {
      const HANDLE handles[2] = {stop_, timer_};
      while (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) PostMessageW(window_, kGameMouseWakeMessage, 0, 0);
    });
  }
  void Arm(int64_t delay_us) {
    if (!timer_) return;
    LARGE_INTEGER due;
    due.QuadPart = -std::max<int64_t>(1, delay_us * 10);
    SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0);
  }
  void Stop() {
    if (stop_) SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
    Close();
  }

 private:
  void Close() {
    if (timer_) CloseHandle(timer_);
    if (stop_) CloseHandle(stop_);
    timer_ = stop_ = nullptr;
  }
  HWND window_ = nullptr;
  HANDLE timer_ = nullptr, stop_ = nullptr;
  std::thread thread_;
};

class GameMouseUsbChannel {
 public:
  using V = flutter::EncodableValue;
  GameMouseUsbChannel(flutter::BinaryMessenger* messenger, HWND window) : window_(window) {
    channel_ = std::make_unique<flutter::MethodChannel<V>>(messenger, "multitap/game-mouse-usb", &flutter::StandardMethodCodec::GetInstance());
    channel_->SetMethodCallHandler([this](const auto& call, auto result) { Handle(call, std::move(result)); });
  }
  ~GameMouseUsbChannel() {
    if (channel_) channel_->SetMethodCallHandler(nullptr);
    wake_.Stop();
  }

  // The sink for one engagement when the VM reads the device now; otherwise
  // nothing, and the engagement uses the injected path.
  std::optional<RelativeMouseSink> Engage() {
    if (!device_ || !device_->TakesInput(NowUs())) return std::nullopt;
    state_ = game_mouse::MouseState(GetSystemMetrics(SM_SWAPBUTTON) != 0);
    RelativeMouseSink sink;
    sink.input = [this](const RAWMOUSE& raw, HANDLE source) {
      if (!device_) return;
      const auto now = NowUs();
      for (const auto& report : state_.Mouse(uintptr_t(source), int(raw.lLastX), int(raw.lLastY), raw.usButtonFlags, int16_t(raw.usButtonData), now / 1000))
        device_->Report(report, now);
      Pace(now);
    };
    sink.tick = [this] {
      if (!device_) return false;
      const auto now = NowUs();
      if (state_.AnyHeld()) {
        // VK_LBUTTON ... VK_XBUTTON2 are the physical buttons, as the bits are.
        static constexpr int keys[5] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        if (const auto up = state_.Check([](int bit) { return GetAsyncKeyState(keys[bit]) < 0; }, now / 1000)) device_->Report(*up, now);
      }
      device_->Tick(now);
      Pace(now);
      return device_->TakesInput(now);
    };
    sink.released = [this] {
      const auto up = state_.ReleaseAll();
      if (!device_) return;
      const auto now = NowUs();
      if (up) device_->Report(*up, now);
      Pace(now);
    };
    return sink;
  }
  // The wake message; true when it was this one.
  bool HandleMessage(UINT message) {
    if (message != kGameMouseWakeMessage) return false;
    if (device_) {
      const auto now = NowUs();
      device_->Tick(now);
      Pace(now);
    }
    return true;
  }

 private:
  static int64_t NowUs() {
    return int64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  void Pace(int64_t now) {
    if (const auto at = device_ ? device_->Wake() : std::nullopt) wake_.Arm(std::max<int64_t>(0, *at - now));
  }
  void Send(const game_mouse::Bytes& bytes) {
    if (channel_) channel_->InvokeMethod("send", std::make_unique<V>(std::vector<uint8_t>(bytes)));
  }
  void Handle(const flutter::MethodCall<V>& call, std::unique_ptr<flutter::MethodResult<V>> result) {
    const auto& method = call.method_name();
    if (method == "attached") {
      const auto* id = std::get_if<int32_t>(call.arguments());
      if (!id || *id < 1 || *id > 255) { result->Error("invalid_arguments", "Expected a device number."); return; }
      device_ = std::make_unique<game_mouse::Device>(uint8_t(*id), [this](const game_mouse::Bytes& bytes) { Send(bytes); });
      wake_.Start(window_);
      result->Success();
      return;
    }
    if (method == "detached") {
      device_.reset();
      result->Success();
      return;
    }
    if (method != "frame") { result->NotImplemented(); return; }
    const auto* bytes = std::get_if<std::vector<uint8_t>>(call.arguments());
    if (!bytes || !device_) { result->Success(V("invalid")); return; }
    const bool was = device_->confirmed();
    const auto now = NowUs();
    const auto seen = device_->Frame(bytes->data(), bytes->size(), now);
    Pace(now);
    if (!was && device_->confirmed() && channel_) channel_->InvokeMethod("confirmed", nullptr);
    result->Success(V(seen == game_mouse::Device::Seen::kOk ? "ok" : seen == game_mouse::Device::Seen::kOlderHost ? "older" : "invalid"));
  }

  HWND window_;
  std::unique_ptr<flutter::MethodChannel<V>> channel_;
  std::unique_ptr<game_mouse::Device> device_;
  game_mouse::MouseState state_;
  GameMouseWake wake_;
};

}  // namespace multitap
