#pragma once
// Role::secure helper entry: the SYSTEM child the service launches on
// winsta0\Winlogon (secure_desktop_launch.hpp). Everything it shares arrives as
// four inherited handles named on its command line: the frame section and its
// event, the read end of the service's input pipe, and a wait handle of the
// stream's user worker. It opens nothing by name. It captures the stream's one
// owned output and injects gated input through secure::runSecureDesktopHelper,
// owns no encoder and no socket, and exits when the input desktop is Default
// again, the scope is lost, the service closes the pipe, or the worker exits.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "agent.hpp"  // host::RemoteInput only
#include "secure_desktop_capture.hpp"
#include "secure_desktop_helper.hpp"
#include "secure_desktop_ipc.hpp"
#include "secure_desktop_transport.hpp"
#include "signin_worker_native.hpp"  // native::observeConsole, observeWorker

namespace host::secure::entry {

inline ConsoleView consoleView() noexcept {
  ConsoleView v;
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

// The window in front (on the input desktop this thread is on) belongs to a
// process of this session whose image is System32\consent.exe or
// CredentialUIBroker.exe (secure::securityPromptImage).
inline bool securityPromptInFront(DWORD session) noexcept {
  const HWND window = GetForegroundWindow(); if (!window) return false;
  DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
  DWORD owner = 0; if (!pid || !ProcessIdToSessionId(pid, &owner) || owner != session) return false;
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!process) return false;
  wchar_t image[1024]{}; DWORD length = 1024;
  const bool named = QueryFullProcessImageNameW(process, 0, image, &length) != FALSE;
  CloseHandle(process);
  wchar_t system[MAX_PATH + 1]{}; const UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
  return named && systemLength && systemLength < MAX_PATH && securityPromptImage(std::wstring_view(image, length), std::wstring_view(system, systemLength));
}
inline HelperView selfView() noexcept {
  HelperView v;
  try {
    const auto facts = host::signin::native::observeWorker();
    v.isSystem = facts.localSystem;
    v.session = facts.session;
    v.winSta0 = facts.stationWinSta0;
    v.threadWinlogon = facts.threadWinlogon;
    v.inputWinlogon = facts.inputWinlogon;
    v.promptForeground = v.threadWinlogon && v.inputWinlogon && securityPromptInFront(facts.session);
  } catch (...) {
    v = {};
  }
  return v;
}

// Returns 0 on a clean end (desktop returned, service or worker gone), non-zero
// on a configuration, scope or capture failure. Never touches any policy value.
inline int command(int argc, const wchar_t* const* argv) noexcept {
  try {
    const auto parsed = parseCommand(argc, argv);
    if (!parsed) return ERROR_INVALID_PARAMETER;
    // The four inherited handles are owned from here on.
    host::signin::native::Handle section(parsed->section), frameEvent(parsed->event), input(parsed->input), worker(parsed->worker);
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (GetFileType(input.get()) != FILE_TYPE_PIPE) return ERROR_INVALID_HANDLE;
    if (WaitForSingleObject(worker.get(), 0) != WAIT_TIMEOUT) return ERROR_INVALID_HANDLE;
    const size_t bytes = ipc::sectionBytes(parsed->output.width, parsed->output.height);
    auto* view = static_cast<uint8_t*>(MapViewOfFile(section.get(), FILE_MAP_WRITE, 0, 0, bytes));
    if (!view) return ERROR_INVALID_HANDLE;
    struct Unmap { uint8_t* p; ~Unmap() { UnmapViewOfFile(p); } } unmap{view};
    ipc::writerInit(view, parsed->output.width, parsed->output.height);

    HelperConfig config;
    config.session = parsed->session;
    config.userSid = parsed->sid;
    config.output = parsed->output;

    // The injector maps a client position onto the owned output's rectangle
    // only; Ctrl+Alt+Del stays refused (host::RemoteInput).
    const RECT rect{parsed->output.left, parsed->output.top,
                    parsed->output.left + LONG(parsed->output.width), parsed->output.top + LONG(parsed->output.height)};
    // Counts only (hostAction 200 stage 246): never what an input was.
    ipc::InputCounts counts;
    // The desktop SendInput's absolute positions are fractions of, in physical
    // pixels (DXGI), read again when the duplication is opened again.
    RECT desktop = physicalDesktopBounds();
    RemoteInput injector([&counts](INPUT& event) -> UINT {
      UINT sent = SendInput(1, &event, sizeof(event));
      if (sent != 1) {
        // The thread may have lost the input desktop: join it again (Winlogon only) and try once more.
        counts.lastError = GetLastError();
        if (attachInputDesktop(L"Winlogon")) sent = SendInput(1, &event, sizeof(event));
        if (sent != 1) { counts.lastError = GetLastError(); ++counts.sendFailed; }
      }
      return sent;
    }, false, [&desktop] { return desktop.right > desktop.left ? desktop : remotePointerDesktopBounds(); });
    injector.area(rect);
    OutputCapture capture(parsed->output);
    HelperExit exit = HelperExit::badConfig;
    {
      InputReader reader(input.get());
      HelperIo io;
      io.console = [] { return consoleView(); };
      io.self = [] { return selfView(); };
      io.nowMs = [] { return uint64_t(GetTickCount64()); };
      io.idle = [] { Sleep(16); };
      io.open = [&] { const auto step = capture.open(); if (step == CaptureStep::frame) desktop = physicalDesktopBounds(); return step; };
      io.capture = [&](std::vector<uint8_t>& bgra) { return capture.acquire(bgra, 16); };
      io.frame = [&](const uint8_t* bgra, uint32_t w, uint32_t h) -> bool {
        ipc::writeFrame(view, bgra, w, h);
        SetEvent(frameEvent.get());
        return true;
      };
      io.alive = [&] { ipc::writeAlive(view); };
      io.nextInput = [&]() -> std::optional<Json> { return reader.pop(); };
      io.inject = [&](const Json& event) -> bool {
        if (event.get("event").string() == "text") return injector.text(event);
        // Independent outputs (secure_desktop_tracks.hpp): the position is a desktop pixel the worker's track
        // plan placed. Only a pixel on this helper's own output is taken; it becomes the same fraction of that
        // output a stream of one output sends. Anything else is refused, never moved onto the output.
        const auto* fields = std::get_if<Json::Object>(&event.value);
        if (fields && (fields->contains(sysinput::placedX) || fields->contains(sysinput::placedY))) {
          const auto placed = sysinput::placeOnDesktop(event, sysinput::Area{rect.left, rect.top, rect.right, rect.bottom});
          return placed && injector.apply(*placed);
        }
        return injector.apply(event);
      };
      // Whatever this helper still holds is released on the desktop that takes
      // input NOW (after a prompt closes that is the user's desktop again):
      // key-ups and button-ups of its own presses only, never a new press.
      io.releaseKeys = [&] {
        for (int attempt = 0; attempt < 3 && injector.pending(); ++attempt) {
          (void)attachInputDesktop();
          injector.reset();
          if (injector.pending()) Sleep(20);
        }
      };
      io.stopped = [&] { return reader.closed() || WaitForSingleObject(worker.get(), 0) != WAIT_TIMEOUT; };
      io.note = [&](InputNote what) {
        if (what == InputNote::received) ++counts.received;
        else if (what == InputNote::dropped) ++counts.dropped;
        else if (what == InputNote::injected) ++counts.injected;
        else ++counts.refused;
        ipc::writeInputCounts(view, counts);
      };
      exit = runSecureDesktopHelper(config, io);
    }
    capture.close();
    ipc::writeState(view, exit == HelperExit::desktopReturned ? ipc::stateEnded : ipc::stateFailed);
    SetEvent(frameEvent.get());
    return exit == HelperExit::desktopReturned || exit == HelperExit::transportClosed ? 0 : 64 + int(exit);
  } catch (...) {
    return ERROR_INVALID_DATA;
  }
}

}  // namespace host::secure::entry
