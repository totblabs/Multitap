#pragma once
// Warm media start (field data: a stream's media start, hostMediaMs,
// p50 323 / p90 772 ms; a resize restart's capture start 181 ms of 194).
// A stream's session worker is a fresh process, so its first capture loaded
// the D3D11 driver and Windows Graphics Capture, and its audio thread the
// loopback endpoint, only after the display was ready; and every resize
// restart released the capture's D3D11 device and created a new one.
//
// CaptureDevices holds one device for the next capture of this worker:
//  - warm(): while the display is prepared, its own thread creates the device
//    a capture would create (default adapter, hardware, BGRA) and loads
//    Windows Graphics Capture, then opens and closes the loopback endpoint.
//    None of this depends on the prepared display; nothing is captured.
//  - keep(): a capture leaves its device for the next capture (a resize
//    restart, an encode-scale rebuild) instead of releasing it.
//  - take(): the next capture's device, handed out only when it was not
//    removed and is still on the adapter a new device would be created on now
//    (the first DXGI adapter). Otherwise empty: the capture creates its own
//    exactly as before, WARP fallback included.
// A failed or late warm-up only means the start is as cold as before.
#include "media.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

struct ID3D11Device;

namespace host {
struct CaptureDeviceAccess {
  std::function<std::shared_ptr<ID3D11Device>()> create;
  std::function<std::optional<uint64_t>(ID3D11Device*)> adapter;  // the device's adapter
  std::function<std::optional<uint64_t>()> current;               // where a new device would be created now
  std::function<bool(ID3D11Device*)> removed;
  std::function<void(ID3D11Device*)> warmCapture;                 // optional
  std::function<void()> warmAudio;                                // optional
};
// A capture waits at most this long for a warm-up still creating its device;
// then it creates its own, as before.
inline constexpr std::chrono::milliseconds captureDeviceWarmWait{2000};

class CaptureDevices {
 public:
  using Device = std::shared_ptr<ID3D11Device>;
  explicit CaptureDevices(CaptureDeviceAccess access, std::chrono::milliseconds wait = captureDeviceWarmWait) : access_(std::move(access)), wait_(wait) {}
  ~CaptureDevices() { join(); }
  CaptureDevices(const CaptureDevices&) = delete;
  CaptureDevices& operator=(const CaptureDevices&) = delete;
  // Starts the warm-up unless one runs or a device is already held. `audio`:
  // the stream asked for audio, so its loopback endpoint is opened too.
  void warm(bool audio) {
    {
      std::lock_guard lock(mutex_);
      if (creating_ || device_) return;
    }
    join();
    std::lock_guard lock(mutex_);
    if (creating_ || device_) return;
    creating_ = true; const uint64_t started = generation_;
    try {
      thread_ = std::thread([this, audio, started] {
        Device created;
        try { if (access_.create) created = access_.create(); } catch (...) { created.reset(); }
        try { if (access_.warmCapture && created) access_.warmCapture(created.get()); } catch (...) {}
        {
          std::lock_guard guard(mutex_);
          // A keep() or take() since the start wins; this device is dropped.
          if (!device_ && generation_ == started) device_ = std::move(created);
          creating_ = false;
        }
        ready_.notify_all();
        created.reset();
        // Only loads the audio engine; the capture may already run beside it.
        try { if (audio && access_.warmAudio) access_.warmAudio(); } catch (...) {}
      });
    } catch (...) { creating_ = false; }
  }
  void keep(Device device) {
    { std::lock_guard lock(mutex_); device_ = std::move(device); ++generation_; }
    ready_.notify_all();
  }
  Device take() {
    Device device;
    {
      std::unique_lock lock(mutex_);
      ready_.wait_for(lock, wait_, [this] { return !creating_; });
      device = std::move(device_); device_.reset(); ++generation_;
    }
    if (!device) return {};
    try {
      if (!access_.removed || access_.removed(device.get())) return {};
      const auto mine = access_.adapter ? access_.adapter(device.get()) : std::nullopt;
      const auto now = access_.current ? access_.current() : std::nullopt;
      if (!mine || !now || *mine != *now) return {};
    } catch (...) { return {}; }
    return device;
  }
  void clear() { keep({}); }
  bool holding() { std::lock_guard lock(mutex_); return bool(device_); }
  // Waits for a warm-up thread (it may still be opening the audio endpoint).
  void join() { if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join(); }
 private:
  CaptureDeviceAccess access_; std::chrono::milliseconds wait_;
  std::mutex mutex_; std::condition_variable ready_;
  Device device_; bool creating_ = false;
  // Bumped by keep() and take(): a warm-up started before them does not store.
  uint64_t generation_ = 0;
  std::thread thread_;
};
// The worker's devices: media.cpp's own capture device creation and checks.
inline CaptureDeviceAccess windowsCaptureDeviceAccess() {
  return {createCaptureDevice, captureDeviceAdapter, defaultCaptureAdapter, captureDeviceRemoved, warmCaptureRuntime, warmLoopbackAudio};
}
}  // namespace host
