#pragma once
// Service-side launch of the Role::secure SYSTEM helper. Called only from the
// SYSTEM service's worker relay (session 0), for a worker.secure.request of the
// user worker it launched itself. It duplicates the service's OWN token,
// retargets it to the stream's session, and starts the service's own image on
// winsta0\Winlogon in a kill-on-close job. It never takes another process's
// token, never changes privileges, and touches no policy value. The helper then
// enforces secure::helperAllowed by itself, so a launch on a session that does
// not show the secure desktop captures and injects nothing.
//
// Everything the helper and the worker share is created here WITHOUT a name:
//   * the frame section and its event: the helper inherits them, the worker
//     gets a read-only / wait-only duplicate (secure_desktop_ipc.hpp);
//   * the input pipe: the service keeps the write end, the helper inherits the
//     read end. Closing the write end ends the helper;
//   * a wait handle of the worker process, so the helper ends with its worker.
// Exactly these four handles are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <userenv.h>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "machine.hpp"  // host::WinHandle, randomName
#include "secure_desktop_service.hpp"
#include "service_worker_drain.hpp"

namespace host::secure {

namespace detail {
inline bool tokenUserIsSystem(HANDLE token) {
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (!size || size > 4096) return false;
  std::vector<BYTE> bytes(size);
  if (!GetTokenInformation(token, TokenUser, bytes.data(), size, &size)) return false;
  const auto* user = reinterpret_cast<const TOKEN_USER*>(bytes.data());
  return IsValidSid(user->User.Sid) && IsWellKnownSid(user->User.Sid, WinLocalSystemSid);
}
inline bool tokenSession(HANDLE token, DWORD& session) {
  DWORD size = 0;
  return GetTokenInformation(token, TokenSessionId, &session, sizeof(session), &size) && size == sizeof(session);
}
// Close a handle that lives in another process (a duplicate handed out too early).
inline void closeRemote(HANDLE process, HANDLE remote) {
  if (remote) DuplicateHandle(process, remote, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
}
}  // namespace detail

// A process's user SID as a string (the worker's signed-in user). Never SYSTEM.
inline std::optional<std::string> processUserSid(HANDLE process) {
  HANDLE raw = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return {};
  WinHandle token(raw);
  DWORD size = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
  if (!size || size > 4096) return {};
  std::vector<BYTE> bytes(size);
  if (!GetTokenInformation(token.value, TokenUser, bytes.data(), size, &size)) return {};
  const auto* user = reinterpret_cast<const TOKEN_USER*>(bytes.data());
  LPWSTR text = nullptr;
  if (!IsValidSid(user->User.Sid) || IsWellKnownSid(user->User.Sid, WinLocalSystemSid) || !ConvertSidToStringSidW(user->User.Sid, &text)) return {};
  std::string out;
  for (const wchar_t* p = text; *p; ++p) { if (*p > 127) { LocalFree(text); return {}; } out.push_back(char(*p)); }
  LocalFree(text);
  if (out.rfind("S-1-5-21-", 0) != 0 && out.rfind("S-1-12-1-", 0) != 0) return {};  // a real account (local/domain or Entra), never a service SID
  return out;
}

// The service's hold on one launched helper.
class HelperLink final : public ServiceHelper {
 public:
  HelperLink(WinHandle job, WinHandle process, WinHandle input) : job_(std::move(job)), process_(std::move(process)), input_(std::move(input)) {}
  ~HelperLink() override { close(); }

  bool alive() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return process_.value && WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT;
  }

  // One input message, framed like the worker pipe (u32 length, kind 0, text).
  // The helper reads all the time, so the write completes at once; a helper
  // that does not take it within sendTimeoutMs gets no further input.
  bool send(const std::string& json) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!input_.value || broken_ || json.empty() || json.size() > 65536) return false;
    std::vector<BYTE> packet(5 + json.size());
    const uint32_t length = uint32_t(json.size() + 1);
    packet[0] = BYTE(length); packet[1] = BYTE(length >> 8); packet[2] = BYTE(length >> 16); packet[3] = BYTE(length >> 24); packet[4] = 0;
    std::memcpy(packet.data() + 5, json.data(), json.size());
    if (!writeEvent_.value) return false;
    ResetEvent(writeEvent_.value);
    OVERLAPPED operation{}; operation.hEvent = writeEvent_.value;
    DWORD written = 0;
    BOOL ok = WriteFile(input_.value, packet.data(), DWORD(packet.size()), &written, &operation);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      if (WaitForSingleObject(writeEvent_.value, sendTimeoutMs) != WAIT_OBJECT_0) CancelIoEx(input_.value, &operation);
      ok = GetOverlappedResult(input_.value, &operation, &written, TRUE);
    }
    if (!ok || written != packet.size()) { broken_ = true; return false; }
    return true;
  }

  // The write end closes first: the helper sees the end of its pipe, releases
  // what it holds on the Winlogon desktop and exits by itself. A helper that
  // does not is ended with its job.
  void close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    input_ = WinHandle();
    if (process_.value && WaitForSingleObject(process_.value, orderlyExitMs) != WAIT_OBJECT_0) {
      if (job_.value) TerminateJobObject(job_.value, 1);
      (void)WaitForSingleObject(process_.value, 2000);
    }
    job_ = WinHandle();
    process_ = WinHandle();
  }

 private:
  static constexpr DWORD sendTimeoutMs = 150, orderlyExitMs = 500;
  std::mutex mutex_;
  WinHandle job_, process_, input_;
  WinHandle writeEvent_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};  // one event for every write of this link
  bool broken_ = false;
};

namespace detail {

// The service's own token as a primary token of `consoleSession`: still SYSTEM,
// nothing else changed. Empty on any doubt.
inline WinHandle systemTokenForSession(DWORD consoleSession) {
  HANDLE raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw)) return {};
  WinHandle serviceToken(raw);
  DWORD ownSession = 0xffffffffu;
  if (!tokenUserIsSystem(serviceToken.value) || !tokenSession(serviceToken.value, ownSession) || ownSession != 0) return {};
  HANDLE primaryRaw = nullptr;
  if (!DuplicateTokenEx(serviceToken.value,
                        TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                        nullptr, SecurityImpersonation, TokenPrimary, &primaryRaw))
    return {};
  WinHandle token(primaryRaw);
  DWORD session = consoleSession, tokenSessionNow = 0;
  if (!SetTokenInformation(token.value, TokenSessionId, &session, sizeof(session)) ||
      !tokenSession(token.value, tokenSessionNow) || tokenSessionNow != consoleSession || !tokenUserIsSystem(token.value))
    return {};
  return token;
}

// The input pipe of one child: one instance, its DACL admits SYSTEM only, and
// both ends are opened here, so there is nothing left for anyone to connect to.
// The service keeps `write`; the child inherits `read`. The name is random and
// is used only for this open.
struct InputPipe { WinHandle write, read; };
inline std::optional<InputPipe> createInputPipe() {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr)) return {};
  struct FreeDescriptor { PSECURITY_DESCRIPTOR p; ~FreeDescriptor() { if (p) LocalFree(p); } } freeDescriptor{descriptor};
  SECURITY_ATTRIBUTES systemOnly{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
  SECURITY_ATTRIBUTES systemOnlyInherit{sizeof(SECURITY_ATTRIBUTES), descriptor, TRUE};
  const std::wstring pipeName = L"\\\\.\\pipe\\MultitapHost.SecureInput." + std::to_wstring(GetCurrentProcessId()) + L"." + randomName();
  InputPipe pipe;
  pipe.write = WinHandle(CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                           PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 0, 0, &systemOnly));
  if (!pipe.write.value || pipe.write.value == INVALID_HANDLE_VALUE) { pipe.write.value = nullptr; return {}; }
  pipe.read = WinHandle(CreateFileW(pipeName.c_str(), GENERIC_READ, 0, &systemOnlyInherit, OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  if (!pipe.read.value || pipe.read.value == INVALID_HANDLE_VALUE) { pipe.read.value = nullptr; return {}; }
  ULONG client = 0;
  if (!GetNamedPipeClientProcessId(pipe.write.value, &client) || client != GetCurrentProcessId()) return {};
  return pipe;
}

struct Spawned {
  WinHandle job;      // kill-on-close
  WinHandle process;
  WinHandle thread;   // the suspended main thread: the caller resumes it
};
// Start the service's own image as SYSTEM in `consoleSession` on `desktop`
// (L"winsta0\\Winlogon" or L"winsta0\\Default"), suspended, in the service's
// tracking job and a kill-on-close job from its first instruction, inheriting
// exactly `inherited`. nullopt: nothing was started.
inline std::optional<Spawned> spawnSystemChild(const std::wstring& image, DWORD consoleSession, const wchar_t* desktopName,
                                               const std::wstring& arguments, std::vector<HANDLE> inherited,
                                               service_workers::Role role) {
  if (image.empty() || !consoleSession || consoleSession == 0xffffffffu || inherited.empty() || !desktopName) return {};
  // The service may not create children while it stops (update, shutdown).
  auto physicalCreation = service_workers::reserveCurrent(role, true);
  WinHandle trackingJob(physicalCreation.createTrackingJob());
  if (!trackingJob.value) return {};
  WinHandle token = systemTokenForSession(consoleSession);
  if (!token.value) return {};

  WinHandle job(CreateJobObjectW(nullptr, nullptr));
  if (!job.value) return {};
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return {};

  void* environment = nullptr;
  if (!CreateEnvironmentBlock(&environment, token.value, FALSE)) return {};
  struct Env { void* p; ~Env() { if (p) DestroyEnvironmentBlock(p); } } env{environment};

  HANDLE jobs[2] = {trackingJob.value, job.value};
  SIZE_T attributeBytes = 0;
  InitializeProcThreadAttributeList(nullptr, 2, 0, &attributeBytes);
  std::vector<BYTE> attributeStorage(attributeBytes);
  auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
  if (!attributeBytes || !InitializeProcThreadAttributeList(attributes, 2, 0, &attributeBytes)) return {};
  struct DeleteAttributes { PPROC_THREAD_ATTRIBUTE_LIST p; ~DeleteAttributes() { DeleteProcThreadAttributeList(p); } } deleteAttributes{attributes};
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof(jobs), nullptr, nullptr) ||
      !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr))
    return {};

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  std::wstring desktop = desktopName;
  startup.StartupInfo.lpDesktop = desktop.data();
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION created{};
  const std::wstring command = L"\"" + image + L"\" " + arguments;
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  const auto slash = image.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  const std::wstring directory = image.substr(0, slash);
  physicalCreation.requireLaunchAllowed();
  if (!CreateProcessAsUserW(token.value, image.c_str(), buffer.data(), nullptr, nullptr, TRUE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, environment,
                            directory.c_str(), &startup.StartupInfo, &created))
    return {};
  Spawned spawned;
  spawned.job = std::move(job);
  spawned.thread = WinHandle(created.hThread);
  HANDLE processRaw = created.hProcess;
  // The child exists: a failure below ends it (and the job's close would too).
  const auto abort = [&] { if (processRaw) { TerminateProcess(processRaw, 1); WaitForSingleObject(processRaw, 2000); CloseHandle(processRaw); processRaw = nullptr; } };
  try { physicalCreation.retain(processRaw); } catch (...) { abort(); return {}; }
  BOOL inJob = FALSE;
  if (!processRaw || !IsProcessInJob(processRaw, spawned.job.value, &inJob) || !inJob) { abort(); return {}; }
  spawned.process = WinHandle(processRaw);
  return spawned;
}
inline std::wstring handleText(HANDLE handle) { return std::to_wstring(uint64_t(reinterpret_cast<uintptr_t>(handle))); }
inline void endSpawned(Spawned& spawned) {
  if (spawned.process.value) { TerminateProcess(spawned.process.value, 1); WaitForSingleObject(spawned.process.value, 2000); }
}

}  // namespace detail

// image: the service's own executable. workerProcess: the handle CreateProcess
// returned for this stream's user worker. nullopt on any failure (the caller
// answers worker.secure.failed and the notice stands in).
inline std::optional<HelperGrant> launchSecureDesktopHelper(const std::wstring& image, DWORD consoleSession, HANDLE workerProcess,
                                                             const std::string& userSid, const HelperArgs& args) {
  if (image.empty() || !consoleSession || consoleSession == 0xffffffffu || !workerProcess ||
      userSid.rfind("S-1-", 0) != 0 || !ipc::sizeValid(args.width, args.height))
    return {};
  try {
    if (WaitForSingleObject(workerProcess, 0) != WAIT_TIMEOUT) return {};
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    // The frame section and its event (unnamed; the helper inherits them).
    const uint64_t bytes = ipc::sectionBytes(args.width, args.height);
    WinHandle section(CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, DWORD(bytes >> 32), DWORD(bytes & 0xffffffffu), nullptr));
    if (!section.value) return {};
    WinHandle frameEvent(CreateEventW(&inherit, FALSE, FALSE, nullptr));
    if (!frameEvent.value) return {};
    auto pipe = detail::createInputPipe();
    if (!pipe) return {};
    // The helper ends with its worker.
    HANDLE workerWaitRaw = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), workerProcess, GetCurrentProcess(), &workerWaitRaw, SYNCHRONIZE, TRUE, 0)) return {};
    WinHandle workerWait(workerWaitRaw);

    const std::wstring userSidW(userSid.begin(), userSid.end());
    const std::wstring arguments = L"--secure-desktop-helper-v1 --session " + std::to_wstring(consoleSession) +
                                   L" --sid " + userSidW + L" --adapter " + std::to_wstring(args.adapterLow) + L":" + std::to_wstring(args.adapterHigh) +
                                   L" --rect " + std::to_wstring(args.left) + L"," + std::to_wstring(args.top) + L"," +
                                   std::to_wstring(args.width) + L"," + std::to_wstring(args.height) +
                                   L" --section " + detail::handleText(section.value) + L" --event " + detail::handleText(frameEvent.value) +
                                   L" --input " + detail::handleText(pipe->read.value) + L" --worker " + detail::handleText(workerWait.value);
    auto spawned = detail::spawnSystemChild(image, consoleSession, L"winsta0\\Winlogon", arguments,
                                            {section.value, frameEvent.value, pipe->read.value, workerWait.value},
                                            service_workers::Role::secureDesktopHelper);
    if (!spawned) return {};

    // The worker's read-only view of the section and its wait-only event.
    HANDLE remoteSection = nullptr, remoteEvent = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), section.value, workerProcess, &remoteSection, FILE_MAP_READ, FALSE, 0)) { detail::endSpawned(*spawned); return {}; }
    if (!DuplicateHandle(GetCurrentProcess(), frameEvent.value, workerProcess, &remoteEvent, SYNCHRONIZE, FALSE, 0)) { detail::closeRemote(workerProcess, remoteSection); detail::endSpawned(*spawned); return {}; }
    if (ResumeThread(spawned->thread.value) == DWORD(-1)) { detail::closeRemote(workerProcess, remoteSection); detail::closeRemote(workerProcess, remoteEvent); detail::endSpawned(*spawned); return {}; }

    HelperGrant grant;
    grant.helper = std::make_shared<HelperLink>(std::move(spawned->job), std::move(spawned->process), std::move(pipe->write));
    grant.section = uint64_t(reinterpret_cast<uintptr_t>(remoteSection));
    grant.event = uint64_t(reinterpret_cast<uintptr_t>(remoteEvent));
    // If the worker cannot be told about its two handles, they are closed in it again.
    HANDLE workerDupRaw = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), workerProcess, GetCurrentProcess(), &workerDupRaw, PROCESS_DUP_HANDLE, FALSE, 0)) {
      auto workerDup = std::make_shared<WinHandle>(workerDupRaw);
      grant.revoke = [workerDup, remoteSection, remoteEvent] { detail::closeRemote(workerDup->value, remoteSection); detail::closeRemote(workerDup->value, remoteEvent); };
    }
    // The service's own copies of the section, the event, the pipe's read end and
    // the worker wait handle close here; the helper holds its inherited ones.
    return grant;
  } catch (...) {
    return {};
  }
}

// The system injector of one stream (system_input.hpp): the same launch on
// winsta0\Default with three inherited handles (the input pipe's read end, a
// wait handle of the worker, a 64-byte section for its counts). The service keeps
// a read-only view of that section. Nothing is handed to the worker.
struct SystemInputLaunch {
  std::shared_ptr<ServiceHelper> helper;
  std::function<ipc::InputCounts()> counts;  // what the injector counted so far
};
inline std::optional<SystemInputLaunch> launchSystemInput(const std::wstring& image, DWORD consoleSession, HANDLE workerProcess,
                                                          const std::string& userSid, uint32_t grants) {
  if (image.empty() || !consoleSession || consoleSession == 0xffffffffu || !workerProcess || userSid.rfind("S-1-", 0) != 0 || grants > 7) return {};
  try {
    if (WaitForSingleObject(workerProcess, 0) != WAIT_TIMEOUT) return {};
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    WinHandle status(CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0, DWORD(sizeof(ipc::FrameHeader)), nullptr));
    if (!status.value) return {};
    const auto* view = static_cast<const uint8_t*>(MapViewOfFile(status.value, FILE_MAP_READ, 0, 0, sizeof(ipc::FrameHeader)));
    if (!view) return {};
    const std::shared_ptr<const uint8_t> held(view, [](const uint8_t* p) { UnmapViewOfFile(p); });
    auto pipe = detail::createInputPipe();
    if (!pipe) return {};
    HANDLE workerWaitRaw = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), workerProcess, GetCurrentProcess(), &workerWaitRaw, SYNCHRONIZE, TRUE, 0)) return {};
    WinHandle workerWait(workerWaitRaw);
    const std::wstring userSidW(userSid.begin(), userSid.end());
    const std::wstring arguments = L"--system-input-v1 --session " + std::to_wstring(consoleSession) + L" --sid " + userSidW +
                                   L" --grants " + std::to_wstring(grants) + L" --input " + detail::handleText(pipe->read.value) +
                                   L" --worker " + detail::handleText(workerWait.value) + L" --status " + detail::handleText(status.value);
    auto spawned = detail::spawnSystemChild(image, consoleSession, L"winsta0\\Default", arguments,
                                            {pipe->read.value, workerWait.value, status.value}, service_workers::Role::systemInput);
    if (!spawned) return {};
    if (ResumeThread(spawned->thread.value) == DWORD(-1)) { detail::endSpawned(*spawned); return {}; }
    SystemInputLaunch launch;
    launch.helper = std::make_shared<HelperLink>(std::move(spawned->job), std::move(spawned->process), std::move(pipe->write));
    launch.counts = [held] { return ipc::Reader{held.get(), 0, 0, 0}.inputCounts(); };
    return launch;
  } catch (...) {
    return {};
  }
}

}  // namespace host::secure