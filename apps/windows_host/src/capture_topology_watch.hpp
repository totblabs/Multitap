#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace host {
// Re-runs a capture's topology check (the owned monitor selection and the
// display device's current position/size) on its own thread instead of on
// every capture tick. The check takes ~0.06 ms when Windows answers at once,
// but EnumDisplayMonitors/EnumDisplayDevices/EnumDisplaySettingsEx wait on the
// display configuration and the field data showed 11-94 ms "topology" stalls
// inside capture ticks. A change is now noticed within `interval` (plus the
// check's own duration) instead of within one tick; the capture thread then
// fails with the check's own message exactly as before. The capture item
// itself stays bound to its monitor, so no other display is captured
// meanwhile. The caller runs one synchronous check before starting.
class CaptureTopologyWatch {
 public:
  using Check = std::function<void()>;  // throws std::exception on a changed topology
  CaptureTopologyWatch(Check check, std::chrono::milliseconds interval)
      : check_(std::move(check)), interval_(interval), thread_([this] { run(); }) {}
  ~CaptureTopologyWatch() { { std::lock_guard lock(mutex_); stop_ = true; } wake_.notify_all(); thread_.join(); }
  CaptureTopologyWatch(const CaptureTopologyWatch&) = delete;
  CaptureTopologyWatch& operator=(const CaptureTopologyWatch&) = delete;
  // Capture thread: throws the watch's failure once it saw one.
  void verify() const {
    if (!failed_.load(std::memory_order_acquire)) return;
    std::lock_guard lock(mutex_); throw std::runtime_error(failure_);
  }
  uint64_t checks() const { return checks_.load(); }
 private:
  void run() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
      lock.unlock();
      std::string failure;
      try { check_(); } catch (const std::exception& error) { failure = error.what(); } catch (...) { failure = "Cannot validate the selected virtual monitor topology"; }
      lock.lock();
      ++checks_;
      if (!failure.empty()) { failure_ = std::move(failure); failed_.store(true, std::memory_order_release); return; }
      wake_.wait_for(lock, interval_, [this] { return stop_; });
    }
  }
  Check check_; std::chrono::milliseconds interval_;
  mutable std::mutex mutex_; std::condition_variable wake_; bool stop_ = false;
  std::string failure_; std::atomic<bool> failed_{false}; std::atomic<uint64_t> checks_{0};
  std::thread thread_;  // last: starts after every member above exists
};
}
