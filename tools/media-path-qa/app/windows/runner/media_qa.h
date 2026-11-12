// Media path measurement harness (not product code). Exposes the actual
// multitap::RemoteEngine to Dart through a "push" method call shaped like the
// product's multitap/remote channel, so the harness's Dart side can replay the
// product RemoteSession receive loop (WebSocket -> platform channel -> native
// decode queue) against a local source and time every step.
#pragma once
#include <flutter/flutter_view_controller.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <memory>
#include <vector>
#include "remote_engine.h"

class MediaQa {
 public:
  explicit MediaQa(flutter::FlutterViewController* controller);
  ~MediaQa();
 private:
  void Stop();
  flutter::FlutterViewController* controller_;
  std::unique_ptr<flutter::PluginRegistrarWindows> registrar_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  std::unique_ptr<multitap::RemoteEngine> engine_;
  std::vector<std::vector<uint8_t>> packets_;
  uint32_t width_ = 0, height_ = 0;
  HANDLE burner_job_ = nullptr;
  DWORD previous_priority_ = 0;
};
