// Texture-path measurement harness (not product code). Drives the actual
// multitap::RemoteEngine (CPU H.264 decoder + Flutter texture) inside a real
// Flutter Windows view with pre-encoded synthetic H.264 pushed at a fixed rate.
#pragma once
#include <flutter/flutter_view_controller.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "remote_engine.h"

// Wraps the engine's texture to time Flutter's raster-thread work for it:
// pixel buffer: callback -> release (the glTexImage2D upload); GPU surface:
// descriptor callback -> release. Records the raster thread id.
struct QaRegistrar final : flutter::TextureRegistrar {
  flutter::TextureRegistrar* real = nullptr;
  std::unique_ptr<flutter::TextureVariant> wrapped;
  flutter::TextureVariant* original = nullptr;
  std::atomic<DWORD> raster_thread{0};
  std::atomic<uint64_t> takes{0};
  std::atomic<int64_t> take_total_ns{0}, take_max_ns{0};
  struct Held { FlutterDesktopPixelBuffer buffer{}; const FlutterDesktopPixelBuffer* inner = nullptr; QaRegistrar* owner = nullptr; int64_t began = 0; };
  struct HeldSurface { FlutterDesktopGpuSurfaceDescriptor descriptor{}; const FlutterDesktopGpuSurfaceDescriptor* inner = nullptr; QaRegistrar* owner = nullptr; int64_t began = 0; };
  void record(int64_t began);
  int64_t RegisterTexture(flutter::TextureVariant* texture) override;
  bool MarkTextureFrameAvailable(int64_t id) override { return real->MarkTextureFrameAvailable(id); }
  void UnregisterTexture(int64_t id, std::function<void()> callback) override;
  bool UnregisterTexture(int64_t id) override { UnregisterTexture(id, nullptr); return true; }
};

class QaHarness {
 public:
  explicit QaHarness(flutter::FlutterViewController* controller);
  ~QaHarness();
 private:
  void Stop();
  void Push();
  flutter::FlutterViewController* controller_;
  std::unique_ptr<flutter::PluginRegistrarWindows> registrar_;
  QaRegistrar proxy_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  std::unique_ptr<multitap::RemoteEngine> engine_;
  std::vector<std::vector<uint8_t>> packets_;
  uint32_t width_ = 0, height_ = 0, fixture_width_ = 0, fixture_height_ = 0;
  int fps_ = 60;
  int pattern_ = 0, jitter_ms_ = 4, drift_ppm_ = 0;
  bool timer_ = false;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> pushed_{0}, rejected_{0};
  std::thread pusher_;
};
