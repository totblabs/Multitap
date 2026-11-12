#pragma once
// The Role::secure helper's control loop. It runs in the SYSTEM process the
// service launches on winsta0\Winlogon (the service's own token, the stream's
// session id) and is the single place the secure-desktop rules are enforced:
//
//   * before every delivered frame and before every injected input event the
//     scope is observed afresh (secure::helperAllowed): this process is SYSTEM,
//     in the stream's session, on WinSta0, its thread desktop and the input
//     desktop are both Winlogon, the session is unlocked and signed in as the
//     stream's user;
//   * only key / mouse move / button / wheel / text events are injected
//     (secure::inputEventAllowed); everything else is dropped;
//   * the moment the input desktop is no longer Winlogon it releases every key
//     and button it holds and returns; so does a lost scope, a failed capture, a
//     closed transport.
//
// It owns no encoder and no socket and touches no policy value. Capture
// (secure_desktop_capture.hpp), injection (host::RemoteInput), transport and the
// Windows observations are injected through HelperIo, so this loop is pure and
// the tests drive it with fakes.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "json_object.hpp"
#include "secure_desktop_scope.hpp"

namespace host::secure {

// The single owned output of the stream, by stable identity: the adapter LUID
// and the desktop rectangle the worker captures. The helper binds to exactly
// this output and refuses any other.
struct OwnedOutput {
  uint32_t adapterLow = 0;
  int32_t adapterHigh = 0;
  uint32_t width = 0, height = 0;
  int32_t left = 0, top = 0;
};

struct HelperConfig {
  uint32_t session = 0;
  std::string userSid;  // the stream's signed-in user SID (never a service SID)
  OwnedOutput output;
};

enum class HelperExit {
  desktopReturned,   // input desktop is no longer Winlogon (normal, expected end)
  scopeLost,         // locked / account / session / token / station scope failed
  duplicationFailed, // could not open or keep the duplication
  outputMismatch,    // the owned output is not attached as described
  transportClosed,   // the service pipe closed, the worker exited, or the frame sink refused
  badConfig
};

// none: nothing new this tick. frame: opened / a frame is ready. lost: the
// duplication is gone (a desktop switch) and must be opened again, which the
// loop does only after the next scope check. failed / mismatch end the helper.
enum class CaptureStep { none, frame, lost, failed, mismatch };
enum class InputNote { received, dropped, injected, refused };

// Injected capture, transport and observation. Every callback is required.
struct HelperIo {
  std::function<ConsoleView()> console;             // fresh WTS observation of the session
  std::function<HelperView()> self;                 // fresh observation of this process
  std::function<uint64_t()> nowMs;                  // GetTickCount64
  std::function<CaptureStep()> open;                // bind the duplication to the owned output (frame = opened)
  std::function<CaptureStep(std::vector<uint8_t>&)> capture;  // wait briefly for one composed BGRA frame
  std::function<bool(const uint8_t*, uint32_t, uint32_t)> frame;  // deliver BGRA; false => stop
  std::function<void()> alive;                      // heartbeat, once per loop tick
  std::function<std::optional<Json>()> nextInput;   // one queued input message, or nullopt
  std::function<bool(const Json&)> inject;          // inject one gated event (host::RemoteInput in production)
  std::function<void()> releaseKeys;                // attach to the current input desktop, release everything held
  std::function<bool()> stopped;                    // the service pipe closed or the worker exited
  // Optional: one input message was taken (received), dropped by the allowlist, accepted by the injector
  // (injected) or refused by it. Counts only; never what the input was.
  std::function<void(InputNote)> note;
  // Optional: a short wait while the window in front is not a security prompt (production: Sleep(16)).
  std::function<void()> idle;
};
// How long the window in front may be something other than a security prompt (a
// prompt still opening, the moment between two prompts) before the helper ends.
inline constexpr uint64_t promptGraceMs = 2000;

// Observations are sequenced before the clock is read: C++ leaves the order of
// argument evaluation open, and a clock read first would make a fresh
// observation look like one from the future (denied as stale).
inline Denial observeScope(const HelperConfig& config, const HelperIo& io, HelperView* seen = nullptr) {
  const ConsoleView console = io.console();
  const HelperView self = io.self();
  const uint64_t now = io.nowMs();
  if (seen) *seen = self;
  return helperAllowed(config.session, config.userSid, console, self, now);
}

inline HelperExit runSecureDesktopHelper(const HelperConfig& config, const HelperIo& io) {
  if (!config.session || config.session == 0xffffffffu || config.userSid.empty() ||
      !config.output.width || !config.output.height || !io.console || !io.self || !io.nowMs || !io.open ||
      !io.capture || !io.frame || !io.alive || !io.nextInput || !io.inject || !io.releaseKeys || !io.stopped)
    return HelperExit::badConfig;

  const auto finish = [&](HelperExit exit) { io.releaseKeys(); return exit; };
  const auto note = [&](InputNote what) { if (io.note) io.note(what); };
  // The exit a denied scope means: a thread or input desktop that is not
  // Winlogon while everything else holds is the prompt closing.
  const auto denied = [&](Denial denial) {
    return denial == Denial::helperInputNotWinlogon || denial == Denial::helperThreadNotWinlogon ? HelperExit::desktopReturned : HelperExit::scopeLost;
  };

  const auto captureEnd = [](CaptureStep step) { return step == CaptureStep::mismatch ? HelperExit::outputMismatch : HelperExit::duplicationFailed; };

  // While the window in front of the Winlogon desktop is not a
  // security prompt's (consent.exe, CredentialUIBroker.exe), nothing is delivered or injected and
  // held keys are released; after promptGraceMs of that the helper ends (scope lost).
  enum class Gate { proceed, skip, end };
  uint64_t notPromptSince = 0;
  HelperExit ending = HelperExit::scopeLost;
  const auto gate = [&](Denial scope) {
    if (scope == Denial::none) { notPromptSince = 0; return Gate::proceed; }
    if (scope != Denial::helperForegroundNotPrompt) { ending = denied(scope); return Gate::end; }
    const uint64_t now = io.nowMs();
    if (!notPromptSince) { notPromptSince = now ? now : 1; io.releaseKeys(); }
    if (now >= notPromptSince && now - notPromptSince < promptGraceMs) return Gate::skip;
    ending = HelperExit::scopeLost; return Gate::end;
  };

  std::vector<uint8_t> bgra;
  bool opened = false;
  unsigned losses = 0;
  for (;;) {
    if (io.stopped()) return finish(HelperExit::transportClosed);
    io.alive();
    // Nothing is opened, captured or injected unless the scope holds right now.
    const auto scopeGate = gate(observeScope(config, io));
    if (scopeGate == Gate::end) return finish(ending);
    if (scopeGate == Gate::skip) {
      // Queued input was meant for a prompt that is not in front: dropped.
      for (int drained = 0; drained < 64; ++drained) { const auto event = io.nextInput(); if (!event) break; note(InputNote::received); note(InputNote::dropped); }
      if (io.idle) io.idle();
      continue;
    }
    if (!opened) {
      const auto step = io.open();
      if (step == CaptureStep::lost) { if (++losses > 50) return finish(HelperExit::duplicationFailed); continue; }
      if (step != CaptureStep::frame) return finish(captureEnd(step));
      opened = true;
    }

    // Queued input: the allowlist first, then a fresh scope for every event.
    for (int drained = 0; drained < 64; ++drained) {
      const auto event = io.nextInput();
      if (!event) break;
      note(InputNote::received);
      const auto type = event->get("type").string();
      if (!inputTypeAllowed(type)) { note(InputNote::dropped); continue; }
      if (type == "input.reset") { io.releaseKeys(); continue; }
      if (!inputEventAllowed(event->get("event").string())) { note(InputNote::dropped); continue; }
      const auto before = gate(observeScope(config, io));
      if (before == Gate::end) return finish(ending);
      if (before == Gate::skip) { note(InputNote::dropped); continue; }
      note(io.inject(*event) ? InputNote::injected : InputNote::refused);
    }

    // One frame (a short wait inside, so input stays responsive).
    const auto step = io.capture(bgra);
    if (step == CaptureStep::none) continue;
    if (step == CaptureStep::lost) { opened = false; if (++losses > 50) return finish(HelperExit::duplicationFailed); continue; }
    if (step != CaptureStep::frame) return finish(captureEnd(step));
    losses = 0;
    if (bgra.size() != size_t(config.output.width) * config.output.height * 4) continue;
    const auto before = gate(observeScope(config, io));
    if (before == Gate::end) return finish(ending);
    if (before == Gate::skip) continue;   // a frame of something that is not a prompt is never delivered
    if (!io.frame(bgra.data(), config.output.width, config.output.height)) return finish(HelperExit::transportClosed);
  }
}

}  // namespace host::secure
