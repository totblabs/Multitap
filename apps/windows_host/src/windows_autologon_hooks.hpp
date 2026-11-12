#pragma once
#include "windows_autologon_job.hpp"
#include "windows_autologon_update_gate.hpp"
#include "windows_autologon_native.hpp"
#include "service_worker_drain.hpp"
#include "installed_host_layout.hpp"
#include "host_text.hpp"
#include <iostream>
#include <atomic>
#include <memory>

namespace host::autologon {
inline bool equalPath(const std::filesystem::path& a, const std::filesystem::path& b) {
  return _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
}
inline uint64_t processCreated(HANDLE process) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
  return (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
inline bool childImageMatches(HANDLE process, DWORD pid, uint64_t created, const std::filesystem::path& expected) {
  if (!pid || !created || GetProcessId(process) != pid || processCreated(process) != created) return false;
  std::wstring image(32768, 0); DWORD count = DWORD(image.size());
  if (!QueryFullProcessImageNameW(process, 0, image.data(), &count) || !count || count >= image.size()) return false;
  image.resize(count); return equalPath(image, expected);
}
inline RecoveryStopState recoveryStopState(HANDLE event) {
  if (!event || event == INVALID_HANDLE_VALUE) return RecoveryStopState::unknown;
  const DWORD wait = WaitForSingleObject(event, 0);
  return wait == WAIT_TIMEOUT ? RecoveryStopState::running : wait == WAIT_OBJECT_0 ? RecoveryStopState::requested : RecoveryStopState::unknown;
}
inline std::filesystem::path installedRecoveryTool() {
  // The layout and its protection are decided in installed_host_layout.hpp
  // (folders made by Windows Installer carry TrustedInstaller's entry).
  const auto tool = installedHostTool(programFilesFolder(), std::filesystem::path(executablePath()), MULTITAP_HOST_VERSION_UTF8, installedHostFiles());
  if (tool.empty()) throw Failure("installed_host_required");
  return tool;
}
class WindowsAutologonRecoveryJob {
 public:
  WindowsAutologonRecoveryJob() = default;
  WindowsAutologonRecoveryJob(const WindowsAutologonRecoveryJob&) = delete;
  WindowsAutologonRecoveryJob& operator=(const WindowsAutologonRecoveryJob&) = delete;
  // Launch runs only on the owned work item; after launchDone publication only
  // the service loop observes it. Neither captures service/bridge lifetime.
  void start(const std::atomic<bool>* closing, HANDLE stopEvent) noexcept {
    if (!observation_.begin(GetTickCount64())) return;
    try {
      if ((closing && closing->load()) || recoveryStopState(stopEvent) != RecoveryStopState::running) throw Failure("service_closing");
      expected_ = installedRecoveryTool();
      // Keep this exact executable image stable across CreateProcess. A normal
      // update must acquire the same autologon mutex before stopping processes.
      image_ = WinHandle(CreateFileW(expected_.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      BY_HANDLE_FILE_INFORMATION file{};
      if (image_.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(image_.value, &file) ||
          (file.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) throw Failure("installed_host_required");
      if ((closing && closing->load()) || recoveryStopState(stopEvent) != RecoveryStopState::running) throw Failure("service_closing");
      std::wstring command = L"\"" + expected_.wstring() + L"\" --windows-autologon-startup-recovery";
      auto physicalCreation=service_workers::reserveCurrent(service_workers::Role::autologonRecovery,true);
      WinHandle physicalJob(physicalCreation.createTrackingJob());
      service_workers::JobListStartup startup(physicalJob.value);PROCESS_INFORMATION process{};
      physicalCreation.requireLaunchAllowed();
      if (!CreateProcessW(expected_.c_str(), command.data(), nullptr, nullptr, FALSE,
          CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT, nullptr, expected_.parent_path().c_str(), &startup.startup.StartupInfo, &process)) throw Failure("launch_failed");
      // Adopt the exact returned handle before any fallible observation. Even
      // identity failure cannot release/relaunch a possibly admitted owner.
      process_ = WinHandle(process.hProcess); CloseHandle(process.hThread);
      physicalCreation.retain(process_.value);
      pid_ = process.dwProcessId; created_ = processCreated(process_.value);
      identity_ = childImageMatches(process_.value, pid_, created_, expected_);
      tick();
    } catch (...) {
      if (!process_.value) { observation_.launchFailed(); image_ = WinHandle{}; }
      // If CreateProcess succeeded, retain the returned process handle even on
      // later read/identity failure and observe until actual exit.
    }
  }
  void tick() noexcept {
    if (!observation_.active() || !process_.value) return;
    try {
      const DWORD wait = WaitForSingleObject(process_.value, 0);
      DWORD code = 0; const bool ended = wait == WAIT_OBJECT_0;
      const bool known = ended && GetExitCodeProcess(process_.value, &code);
      const bool same = identity_ && childImageMatches(process_.value, pid_, created_, expected_);
      observation_.observe(GetTickCount64(), ended, known ? std::optional<uint32_t>(code) : std::nullopt, same);
      if (!observation_.active()) { process_ = WinHandle{}; image_ = WinHandle{}; }
    } catch (...) {
      // An observation/allocation failure cannot stop the service or authorize
      // termination, slot reuse or a second launch. Keep owned handles intact.
    }
  }
  const RecoveryObservation& observation() const { return observation_; }
 private:
  // Destruction closes observer handles only. No TerminateProcess, job-object
  // kill-on-close, cancellation, slot reuse, detached C++ worker or unbounded join.
  WinHandle process_, image_;
  DWORD pid_ = 0;
  uint64_t created_ = 0;
  bool identity_ = false;
  std::filesystem::path expected_;
  RecoveryObservation observation_;
};
// Even path/ACL/metadata/CreateProcess work stays off the service/stream loop.
// The work item captures only shared owned state. Once launchDone is published,
// it never touches job again and the service becomes its sole observer.
class WindowsAutologonStartupOwner {
 public:
  WindowsAutologonStartupOwner() = default;
  WindowsAutologonStartupOwner(const WindowsAutologonStartupOwner&) = delete;
  WindowsAutologonStartupOwner& operator=(const WindowsAutologonStartupOwner&) = delete;
  ~WindowsAutologonStartupOwner() { closeScheduling(); }
  void closeScheduling() noexcept {
    schedulingClosed_ = true;
    if (state_) state_->closing.store(true);
  }
  void closeSchedulingAndJoin() noexcept {
    closeScheduling();
    if(state_&&state_->launchSubmitted.load())while(!state_->launchDone.load())Sleep(100);
  }
  void start(HANDLE serviceStop) noexcept {
    if (scheduled_ || schedulingClosed_) return;
    scheduled_ = true;
    try {
      state_ = std::make_shared<State>(); state_->queued = GetTickCount64();
      HANDLE duplicate = nullptr;
      if (!serviceStop || !DuplicateHandle(GetCurrentProcess(), serviceStop, GetCurrentProcess(), &duplicate, SYNCHRONIZE, FALSE, 0)) return;
      // The work item owns a duplicate; serviceMain may close its original event
      // during teardown without a stale handle/reuse race in this callback.
      state_->stopEvent = WinHandle(duplicate);
      auto* work = new std::shared_ptr<State>(state_);
      state_->launchSubmitted.store(true);
      if (!QueueUserWorkItem(run, work, WT_EXECUTEONLYONCE)) {
        delete work; state_->launchDone.store(true);
      }
    } catch (...) { /* scheduling failure never gates Host readiness */ }
  }
  void tick() noexcept {
    if (state_ && state_->launchDone.load() && !state_->closing.load()) state_->job.tick();
  }
 private:
  struct State {
    std::atomic<bool> closing{false}, launchDone{false}, launchSubmitted{false};
    uint64_t queued = 0;
    WinHandle stopEvent;
    WindowsAutologonRecoveryJob job;
  };
  static DWORD WINAPI run(void* context) noexcept {
    std::unique_ptr<std::shared_ptr<State>> work(static_cast<std::shared_ptr<State>*>(context));
    const auto state = *work;
    try {
      const auto now = GetTickCount64();
      if (queuedRecoveryMayStart(state->queued, now, state->closing.load(), recoveryStopState(state->stopEvent.value))) state->job.start(&state->closing, state->stopEvent.value);
    } catch (...) {}
    state->launchDone.store(true);
    return 0;
  }
  std::shared_ptr<State> state_;
  bool scheduled_ = false, schedulingClosed_ = false;
};
inline int startupRecoveryCommand() {
  requireAdmin();
  const std::string result = windowsAutologonStartupRecovery();
  const bool ok = result == "unchanged" || result == "recovered";
  std::cout << "{\"kind\":\"windows-autologon-startup\",\"result\":" << jsonQuote(result) << ",\"ok\":" << (ok ? "true" : "false") << "}\n";
  return ok ? 0 : 20;
}
inline int updateJournalCheckCommand() {
  requireAdmin();
  // READ ONLY. The installer already owns the verified global change mutex.
  // Never reenter Controller/Guard here, and never read LSA/Winlogon/account data.
  bool safe = false; const char* phase = "unknown";
  try {
    NativeAccess access; const auto record = access.load();
    if (!record) { safe = true; phase = "absent"; }
    else {
      if (completedJournalForUpdate(*record)) { safe = true; phase = record->phase == "owned" ? "owned" : "disabled"; }
      else if (record->phase == "changing") phase = "changing";
    }
  } catch (...) { safe = false; }
  std::cout << "{\"kind\":\"windows-autologon-update-journal\",\"phase\":" << jsonQuote(phase) << ",\"safeToUpdate\":" << (safe ? "true" : "false") << "}\n";
  return safe ? 0 : 20;
}
inline int uninstallCleanupCommand(bool confirmed) {
  requireAdmin();
  const std::string result = windowsAutologonUninstallCleanup(confirmed);
  const bool ok = cleanupAllowsStateDeletion(confirmed, true, true, uint32_t(0), result);
  std::cout << "{\"kind\":\"windows-autologon-uninstall-cleanup\",\"result\":" << jsonQuote(result) << ",\"ok\":" << (ok ? "true" : "false")
    << ",\"productRemovalIntegrated\":false,\"message\":" << jsonQuote(ui_text::utf8(ok ? ui_text::Id::AutologonCleanupDone : ui_text::Id::AutologonCleanupFailed)) << "}\n";
  return ok ? 0 : 20;
}
} // namespace host::autologon
