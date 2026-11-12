#pragma once
#include <chrono>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

namespace host {
// Windows Graphics Capture throttles every session to one frame per
// GraphicsCaptureSession.MinUpdateInterval: 16 ms by default on Windows 11
// build 26100 (measured). A frame composed sooner after the previous delivered
// one is not dropped but held until the first composition after the interval
// (measured with a 100 ms interval: delivered 99-121 ms after the previous
// frame; with 40 ms on a ~31 ms compositor every other composition), so on a
// 60 Hz output a delivery gap that jitters below 16 ms costs a whole refresh.
// The capture loop paces itself (FramePacer, captureMinimumGap), so the session
// should hand over every composed frame. Windows without the property
// (IGraphicsCaptureSession5, before 24H2) keep their own behaviour; false then.
inline bool deliverEveryComposedFrame(const winrt::Windows::Graphics::Capture::GraphicsCaptureSession& session) {
  const auto interval = session.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession5>();
  if (!interval) return false;
  try { interval.MinUpdateInterval(std::chrono::milliseconds(1)); return true; } catch (...) { return false; }
}
}
