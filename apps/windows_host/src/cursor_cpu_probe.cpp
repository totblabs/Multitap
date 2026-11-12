#include "owned_output_size.hpp"
// cursor-v1 CPU probe on THIS PC's primary monitor (explicitly invoked, never
// part of SelfTest): the production Capture (WGC direct path), FrameEncodePolicy
// with its CBR refinement and the Microsoft software H.264 MFT at 60 fps, once
// with the cursor composed into the video (released behaviour) and once
// separated (composeCursor(false) + the cursor-v1 channel), while the real
// cursor is moved with SetCursorPos along a small circle. The cursor position
// is restored afterwards. Frames stay in memory; only counters and timings are
// printed. No input is injected into applications, nothing is sent anywhere.
#include "agent.hpp"
#include <iomanip>
#include <iostream>

namespace {
double processCpuMs() { FILETIME created, exited, kernel, user; GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user); const auto value = [](FILETIME t) { return double((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 10000.0; }; return value(kernel) + value(user); }
struct RestoreCursor { POINT original{}; bool saved = GetCursorPos(&original) != FALSE; ~RestoreCursor() { if (saved) SetCursorPos(original.x, original.y); } };
struct Result { int ticks = 0, fresh = 0, submitted = 0, encoded = 0, records = 0; double encodeMs = 0, captureMs = 0, cpuMs = 0, bytes = 0, recordBytes = 0, seconds = 0; };
Result phase(const RECT& source, bool separated, bool moving, int seconds) {
  using namespace host;
  const int width = int(source.right - source.left), height = int(source.bottom - source.top), fps = 60, bitrate = 12000000;
  Capture capture(width, height, source); Encoder encoder(width, height, fps, bitrate);
  FrameEncodePolicy policy(size_t(width) * height * 3 / 2, FrameRefinement::forCbr(bitrate, fps));
  cursor::SessionShapeIds ids; cursor::InjectedPointer none; std::optional<cursor::Channel> channel;
  if (separated) { capture.composeCursor(false); channel.emplace(source, [&] { return ids.next("probe"); }); }
  const POINT centre{source.left + width / 2, source.top + height / 2};
  const auto beginning = Clock::now(); FramePacer pacer; auto next = beginning;
  Result result; const int warmup = fps;  // the first IDR and its refinement are excluded
  double cpuStart = 0; const auto total = warmup + seconds * fps;
  for (int tick = 0; tick < total; ++tick) {
    if (tick == warmup) cpuStart = processCpuMs();
    if (moving) { const double angle = tick * 0.12; SetCursorPos(int(centre.x + std::cos(angle) * 160), int(centre.y + std::sin(angle) * 120)); }
    const auto& frame = capture.frame();
    const auto pts = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - beginning).count()) + 1;
    const auto decision = policy.prepare(frame, pts);
    double encodeMs = 0; int encoded = 0; size_t bytes = 0;
    if (decision.submit) {
      const auto began = Clock::now();
      encoder.encode(frame.data(), pts, [&](Frame output) {
        const auto now = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - beginning).count()) + 1;
        if (!policy.output(output.ptsUs, output.key, now, output.bytes.size())) throw std::runtime_error("Encoder output did not match");
        if (output.key && channel) channel->keyFrame();
        ++encoded; bytes += output.bytes.size();
      });
      encodeMs = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
    }
    if (channel) {
      const auto& seen = capture.cursorSample();
      for (const auto& record : channel->observe({seen.known, seen.flags, seen.handle, seen.position}, none.read(), hostMonotonicUs())) { ++result.records; result.recordBytes += double(record.size() + 12); }
      capture.composeCursor(channel->composeFallback());
    }
    if (tick >= warmup) {
      ++result.ticks; result.fresh += capture.frameUpdated() ? 1 : 0; result.submitted += decision.submit ? 1 : 0;
      result.encoded += encoded; result.encodeMs += encodeMs; result.bytes += double(bytes); result.captureMs += capture.lastCaptureMs() + capture.lastConvertMs();
    }
    next += std::chrono::microseconds(1000000 / fps); if (next < Clock::now()) next = Clock::now(); pacer.wait(next);
  }
  result.cpuMs = processCpuMs() - cpuStart; result.seconds = double(seconds);
  return result;
}
void print(std::ostream& out, const char* name, const Result& r) {
  out << '"' << name << "\":{\"ticks\":" << r.ticks << ",\"freshCaptureFps\":" << r.fresh / r.seconds << ",\"submittedFps\":" << r.submitted / r.seconds << ",\"encodedFps\":" << r.encoded / r.seconds
      << ",\"encodeMsPerSecond\":" << r.encodeMs / r.seconds << ",\"encodeMsPerFrame\":" << (r.encoded ? r.encodeMs / r.encoded : 0.0) << ",\"captureConvertMsPerTick\":" << (r.ticks ? r.captureMs / r.ticks : 0.0)
      << ",\"processCpuMsPerSecond\":" << r.cpuMs / r.seconds << ",\"videoKbps\":" << r.bytes * 8 / r.seconds / 1000 << ",\"cursorRecords\":" << r.records << ",\"cursorBytes\":" << r.recordBytes << '}';
}
}
int wmain(int argc, wchar_t** argv) {
  try {
    const int seconds = argc > 1 ? std::clamp(std::stoi(argv[1]), 2, 20) : 4;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    host::Runtime runtime;
    if (!host::interactiveDesktopAvailable()) { std::cout << "{\"kind\":\"cursor-cpu-probe\",\"state\":\"no_interactive_desktop\"}\n"; return 3; }
    const auto monitors = host::currentCaptureMonitors();
    auto chosen = std::find_if(monitors.begin(), monitors.end(), [](const auto& m) { return m.primary; }); if (chosen == monitors.end()) chosen = monitors.begin();
    const RECT source = chosen->bounds; const int width = int(source.right - source.left), height = int(source.bottom - source.top);
    if (!ownedOutputSize(uint64_t(std::max(width,0)),uint64_t(std::max(height,0)))) { std::cout << "{\"kind\":\"cursor-cpu-probe\",\"state\":\"unsupported_monitor_size\"}\n"; return 3; }
    RestoreCursor restore;
    const auto composedIdle = phase(source, false, false, seconds);
    const auto composedMoving = phase(source, false, true, seconds);
    const auto separatedMoving = phase(source, true, true, seconds);
    const auto separatedIdle = phase(source, true, false, seconds);
    std::ostringstream out; out << std::fixed << std::setprecision(2) << "{\"kind\":\"cursor-cpu-probe\",\"state\":\"ok\",\"width\":" << width << ",\"height\":" << height << ",\"fps\":60,\"bitrate\":12000000,\"secondsPerPhase\":" << seconds << ',';
    print(out, "composedIdle", composedIdle); out << ','; print(out, "composedCursorMoving", composedMoving); out << ',';
    print(out, "separatedCursorMoving", separatedMoving); out << ','; print(out, "separatedIdle", separatedIdle);
    out << ",\"cursorRestored\":" << (restore.saved ? "true" : "false") << ",\"pixelsSaved\":false,\"network\":false,\"note\":\"dev PC desktop (real WGC capture, real cursor moved by SetCursorPos); not a GPUless VM measurement\"}\n";
    std::cout << out.str();
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
