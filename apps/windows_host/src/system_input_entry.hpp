#pragma once
// The system injector's process entry (system_input.hpp): the SYSTEM child the
// service starts on winsta0\Default for a stream whose worker cannot reach
// elevated windows. Three inherited handles arrive on its command line (the read
// end of the service's input pipe, a wait handle of the stream's user worker, a
// 64-byte section for its counts). It opens nothing by name, captures nothing,
// has no socket, and exits when the service closes the pipe, the worker exits,
// or its session is no longer the unlocked console session of the stream's user.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <optional>
#include <string>

#include "agent.hpp"  // host::RemoteInput only
#include "secure_desktop_capture.hpp"   // attachInputDesktop, physicalDesktopBounds
#include "secure_desktop_ipc.hpp"
#include "secure_desktop_transport.hpp"
#include "signin_worker_native.hpp"  // native::observeConsole, observeWorker
#include "system_input.hpp"
#include "touch_input.hpp"

namespace host::sysinput::entry {

inline secure::ConsoleView consoleView() noexcept {
  secure::ConsoleView v;
  try {
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return v;
    const auto facts = host::signin::native::observeConsole(session);
    v.session = facts.session;
    v.observedAt = facts.observedAt;
    if (facts.valid) {
      v.unlocked = facts.windows == host::signin::WindowsState::unlocked;
      v.noUser = facts.windows == host::signin::WindowsState::noUser;
      v.signedInSid = facts.signedInSid;
    }
  } catch (...) {
    v = {};
  }
  return v;
}

inline InjectorView selfView() noexcept {
  InjectorView v;
  try {
    const auto facts = host::signin::native::observeWorker();
    v.isSystem = facts.localSystem;
    v.session = facts.session;
    v.winSta0 = facts.stationWinSta0;
    v.inputDefault = facts.inputDefault;
  } catch (...) {
    v = {};
  }
  return v;
}

// Returns 0 on a clean end (the service or the worker is gone), non-zero when the
// scope was lost or the command line was not the service's.
inline int command(int argc, const wchar_t* const* argv) noexcept {
  try {
    const auto parsed = secure::entry::parseInjectorCommand(argc, argv);
    if (!parsed) return ERROR_INVALID_PARAMETER;
    host::signin::native::Handle input(parsed->input), worker(parsed->worker), status(parsed->status);
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (GetFileType(input.get()) != FILE_TYPE_PIPE) return ERROR_INVALID_HANDLE;
    if (WaitForSingleObject(worker.get(), 0) != WAIT_TIMEOUT) return ERROR_INVALID_HANDLE;
    auto* view = static_cast<uint8_t*>(MapViewOfFile(status.get(), FILE_MAP_WRITE, 0, 0, sizeof(secure::ipc::FrameHeader)));
    if (!view) return ERROR_INVALID_HANDLE;
    struct Unmap { uint8_t* p; ~Unmap() { UnmapViewOfFile(p); } } unmap{view};

    InjectorConfig config;
    config.session = parsed->session;
    config.userSid = parsed->sid;
    config.grants = {(parsed->grants & 1u) != 0, (parsed->grants & 2u) != 0, (parsed->grants & 4u) != 0};

    secure::ipc::InputCounts counts;
    const auto publish = [&] { secure::ipc::writeInputCounts(view, counts); };
    // SendInput's absolute positions are fractions of the physical virtual
    // desktop (DXGI; read again when the area changes: a mode change moved it).
    RECT desktop = secure::physicalDesktopBounds();
    RemoteInput injector([&counts](INPUT& event) -> UINT {
      UINT sent = SendInput(1, &event, sizeof(event));
      if (sent != 1) {
        // The thread may not be on the input desktop any more: join it again
        // (the user's desktop only) and try once more.
        counts.lastError = GetLastError();
        if (secure::attachInputDesktop(L"Default")) sent = SendInput(1, &event, sizeof(event));
        if (sent != 1) { counts.lastError = GetLastError(); ++counts.sendFailed; }
      }
      return sent;
    }, false, [&desktop] { return desktop.right > desktop.left ? desktop : remotePointerDesktopBounds(); }, nativeTouchInjection());

    InjectorExit exit = InjectorExit::badConfig;
    {
      secure::entry::InputReader reader(input.get());
      InjectorIo io;
      io.console = [] { return consoleView(); };
      io.self = [] { return selfView(); };
      io.nowMs = [] { return uint64_t(GetTickCount64()); };
      io.next = [&](uint32_t milliseconds) { return reader.wait(milliseconds); };
      io.attach = [] { return secure::attachInputDesktop(L"Default"); };
      // The stream's rectangle for positions that are fractions of the picture (one stream), the whole
      // physical desktop for positions that are desktop pixels (independent outputs, placed by the worker's
      // track plan). A pixel that is not on the desktop is refused, never moved onto it.
      RECT streamArea{};
      io.inject = [&](const Json& message) {
        const auto* fields = std::get_if<Json::Object>(&message.value);
        if (fields && (fields->contains(placedX) || fields->contains(placedY))) {
          if (desktop.right <= desktop.left) desktop = secure::physicalDesktopBounds();
          auto placed = placeOnDesktop(message, Area{desktop.left, desktop.top, desktop.right, desktop.bottom});
          if (!placed) {
            // The outputs may just have changed: read the desktop once more before refusing.
            desktop = secure::physicalDesktopBounds();
            placed = placeOnDesktop(message, Area{desktop.left, desktop.top, desktop.right, desktop.bottom});
            if (!placed) return false;
          }
          injector.area(desktop);
          return injector.apply(*placed);
        }
        if (fields && (fields->contains("x") || fields->contains("y"))) injector.area(streamArea);
        // Relative movement needs some output to move on, not a position: without a stream rectangle
        // (independent outputs send none) that is the desktop.
        else if (streamArea.right <= streamArea.left && message.get("event").string() == "moveRelative") injector.area(desktop);
        return injector.apply(message);
      };
      io.text = [&](const Json& message) { return injector.text(message); };
      io.touch = [&](const Json& message) { return injector.touch(message); };
      io.area = [&](const Json& message) {
        const auto area = parseArea(message);
        if (!area) return false;
        streamArea = RECT{area->left, area->top, area->right, area->bottom};
        injector.area(streamArea);
        desktop = secure::physicalDesktopBounds();
        return true;
      };
      io.release = [&] {
        for (int attempt = 0; attempt < 3 && (injector.pending() || injector.touchHeld()); ++attempt) {
          injector.reset();
          if (injector.pending()) Sleep(20);
        }
      };
      io.stopped = [&] { return reader.closed() || WaitForSingleObject(worker.get(), 0) != WAIT_TIMEOUT; };
      io.note = [&](Note what) {
        if (what == Note::received) ++counts.received;
        else if (what == Note::injected) ++counts.injected;
        else if (what == Note::dropped) ++counts.dropped;
        else if (what == Note::refused) ++counts.refused;
        else ++counts.offDesktop;
        publish();
      };
      exit = runSystemInput(config, io);
    }
    publish();
    return exit == InjectorExit::transportClosed ? 0 : 64 + int(exit);
  } catch (...) {
    return ERROR_INVALID_DATA;
  }
}

}  // namespace host::sysinput::entry
