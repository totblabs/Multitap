#pragma once
#include "gpu_h264.hpp"
#include <future>
#include <mutex>

namespace host {
// Test the configured encoder at the current output sizes, holding both
// encoder instances together for a dual desktop. One verified IDR is enough
// to establish format support. The capture loop measures sustained load.
class StreamRefreshProbe {
 public:
  using Shapes = std::vector<std::pair<int, int>>;

  int maximum(const Shapes& shapes, bool idle) {
    const auto selection = gpu::configuredSelection();
    if (!selection.valid || selection.mode == gpu::Mode::disabled || shapes.empty() || shapes.size() > 2) return 60;
    std::lock_guard lock(mutex_);
    if (job_.valid()) {
      if (job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return 60;
      try {
        rate_ = job_.get();
        recheck_ = Clock::time_point::max();
      } catch (...) {
        rate_ = 60;
        recheck_ = cancel_ && cancel_->load() ? Clock::time_point{} : Clock::now() + std::chrono::minutes(2);
      }
    }
    if (Clock::now() < recheck_ && shapes == shapes_ &&
        selection.mode == selection_.mode && selection.adapterLuid == selection_.adapterLuid) return rate_;
    if (idle) {
      auto stop = std::make_shared<std::atomic_bool>(false);
      job_ = std::async(std::launch::async, [shapes, selection, stop] { return supportedRate(shapes, selection, stop.get()); });
      cancel_ = std::move(stop);
      shapes_ = shapes;
      selection_ = selection;
      rate_ = 60;
    }
    return 60;
  }

  void yieldToStream() {
    std::lock_guard lock(mutex_);
    if (job_.valid() && cancel_) *cancel_ = true;
  }

  static int supportedRate(const Shapes& shapes, const gpu::Selection& selection, const std::atomic_bool* stop = nullptr) {
    Runtime runtime;
    if (shapes.empty() || shapes.size() > 2 || gpu::orderedCandidates(gpu::enumerateAdapters(), selection).empty()) return 60;
    std::vector<std::vector<uint8_t>> samples;
    for (const auto& [width, height] : shapes) {
      validateFrameMemory(width, height, 0, 4);
      samples.emplace_back(size_t(width) * height * 3 / 2, uint8_t{128});
    }
    const auto accepts = [&](int fps) {
      std::vector<std::unique_ptr<EncoderImplementation>> encoders;
      try {
        for (size_t i = 0; i < shapes.size(); ++i) {
          if (stop && stop->load()) throw std::runtime_error("Refresh probe interrupted by a stream");
          const auto [width, height] = shapes[i];
          auto encoder = gpu::createEncoder(width, height, fps, 8000000, selection);
          std::vector<Frame> frames;
          encoder->encode(samples[i].data(), 0, frames);
          if (frames.size() != 1 || !frames.front().key || !encoder->hardwareInfo().hardwareVerified)
            throw gpu::Failure(encoderGpuOutputContract, "Unverified GPU refresh probe");
          encoders.push_back(std::move(encoder));
        }
        return true;
      } catch (const gpu::Failure& error) {
        // A busy device, failure to encode, or lost adapter does not prove
        // that this rate is unsupported. Recheck transient failures later.
        if (error.code() != encoderGpuCapabilityUnsupported) throw;
        return false;
      }
    };
    if (accepts(240)) return 240;
    int low = 60, high = 239;
    while (low < high) {
      const int middle = (low + high + 1) / 2;
      if (accepts(middle)) low = middle;
      else high = middle - 1;
    }
    return low;
  }

 private:
  std::mutex mutex_;
  Shapes shapes_;
  gpu::Selection selection_;
  int rate_ = 60;
  Clock::time_point recheck_{};
  std::shared_ptr<std::atomic_bool> cancel_;
  std::future<int> job_;
};
}
