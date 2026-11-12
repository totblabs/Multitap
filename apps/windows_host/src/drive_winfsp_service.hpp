#pragma once
#include "service_worker_drain.hpp"
// The service side of the WinFsp backend (docs/drive-redirection.md, "WinFsp
// backend"): it starts the user-session helper (`--drive-fs`) with the session
// user's own token and bridges the helper's file operations to the drive-v1
// request multiplexer. No WinFsp header is needed here: the service only
// checks that WinFsp is installed and talks to the helper in frames.
#include "drive_winfsp_wire.hpp"
#include "drive_mount.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>

namespace host::drive::fs {
struct FsShare { std::string token, id, label; bool readOnly = false; };
struct FsMountResult { std::string token, letter, error; uint32_t status = 0; };  // status: the NTSTATUS behind a failed mount (diagnostics only)

// What Redirection needs from a WinFsp backend; tests substitute a fake.
class FsMounter {
 public:
  virtual ~FsMounter() = default;
  virtual bool available() = 0;
  virtual std::vector<FsMountResult> map(DWORD session, Backend& backend, const std::vector<FsShare>& shares) = 0;
  virtual void setReadOnly(const std::string& token, bool value) = 0;
  virtual void unmap(const std::vector<std::string>& tokens) = 0;
  virtual std::string openRoot(DWORD, const std::string&) { return "drive_open_unavailable"; }
  virtual void cancelOpen() {}  // wakes observation at channel end; does not claim shell cancellation
  virtual void stop() = 0;  // unmounts everything and ends the helper
  virtual void onLost(std::function<void()>) {}  // the helper ended without being asked
};

// ---- install check (service) ---------------------------------------------------
// WinFsp is usable when its installer registered an install folder that holds
// the DLL for this architecture. The DLL itself is loaded only in the helper
// (user session); `FspLoad` and the volume creation there are the real test and a
// failure falls back to WebDAV. (The driver service name depends on the SxS instance,
// so it is not checked here.)
inline std::wstring winfspRegisteredLibrary() {
  wchar_t path[MAX_PATH]{}; DWORD size = sizeof(path);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\WinFsp", L"InstallDir", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) return {};
  std::wstring directory = path; if (directory.empty()) return {};
  if (directory.back() != L'\\') directory += L'\\';
  return directory + L"bin\\winfsp-x64.dll";
}
inline bool winfspInstalled() {
  const auto library = winfspRegisteredLibrary();
  if (library.empty()) return false;
  const DWORD attributes = GetFileAttributesW(library.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
  return true;
}

// ---- a small worker pool ---------------------------------------------------------
class Workers {
 public:
  explicit Workers(size_t count) { for (size_t i = 0; i < count; ++i) threads_.emplace_back([this] { run(); }); }
  ~Workers() { stop(); }
  bool post(std::function<void()> job) { { std::lock_guard lock(mutex_); if (stopping_ || jobs_.size() >= wire::maxQueuedCalls) return false; jobs_.push_back(std::move(job)); } wake_.notify_one(); return true; }
  void stop() {
    { std::lock_guard lock(mutex_); if (stopping_) return; stopping_ = true; jobs_.clear(); }
    wake_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
  }
 private:
  void run() { for (;;) { std::function<void()> job; { std::unique_lock lock(mutex_); wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); }); if (stopping_) return; job = std::move(jobs_.front()); jobs_.pop_front(); } job(); } }
  std::mutex mutex_; std::condition_variable wake_; std::deque<std::function<void()>> jobs_; std::vector<std::thread> threads_; bool stopping_ = false;
};

// ---- the bridge ------------------------------------------------------------------
class FsBridge {
 public:
  using Read = std::function<size_t(uint8_t*, size_t)>;
  using Write = std::function<bool(const std::vector<uint8_t>&)>;
  FsBridge(Backend& backend, Read read, Write write, std::function<void()> closeTransport = {}, std::function<void()> lost = {}, size_t workers = 12)
      : backend_(backend), read_(std::move(read)), write_(std::move(write)), closeTransport_(std::move(closeTransport)), lost_(std::move(lost)), pool_(workers) {
    reader_ = std::thread([this] { readLoop(); });
  }
  ~FsBridge() { stop(); }
  // "" when the helper loaded WinFsp, else its error code.
  std::string waitReady(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, timeout, [this] { return ready_ || !readyError_.empty() || !alive_; })) return "drive_helper_failed";
    if (ready_) return {};
    return readyError_.empty() ? "drive_helper_failed" : readyError_;
  }
  FsMountResult mount(const FsShare& share, std::chrono::milliseconds timeout) {
    FsMountResult out; out.token = share.token;
    { std::lock_guard lock(mutex_); registered_[share.id] = share.token; outcomes_.erase(share.token); }
    if (!send(wire::controlFrame("{\"type\":\"mount\",\"token\":" + jsonQuote(share.token) + ",\"id\":" + jsonQuote(share.id) + ",\"label\":" + jsonQuote(share.label) + ",\"readOnly\":" + (share.readOnly ? "true" : "false") + "}"))) { forget(share); out.error = "drive_map_failed"; return out; }
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, timeout, [&] { return outcomes_.count(share.token) != 0 || !alive_; }) || !outcomes_.count(share.token)) {
      lock.unlock(); forget(share);
      // The helper may still be inside WinFsp. Queue cancellation before
      // returning for WebDAV fallback; a later mount success must be removed.
      send(wire::controlFrame("{\"type\":\"unmount\",\"token\":" + jsonQuote(share.token) + "}"));
      out.error = "drive_map_failed"; return out;
    }
    out = outcomes_[share.token]; outcomes_.erase(share.token);
    if (out.letter.empty()) { lock.unlock(); forget(share); }
    return out;
  }
  // The helper's "unmounted" answers are kept only while an unmount waits for
  // them: every answer used to stay for the helper's life (a session), one
  // token per share removal, and a late or unasked one stayed too.
  bool unmount(const std::string& token, std::chrono::milliseconds timeout) {
    { std::lock_guard lock(mutex_); for (auto it = registered_.begin(); it != registered_.end();) if (it->second == token) it = registered_.erase(it); else ++it; unmounted_.erase(token); ++unmountWaits_[token]; }
    struct Done { FsBridge& self; const std::string& token; ~Done() { std::lock_guard lock(self.mutex_); if (const auto at = self.unmountWaits_.find(token); at != self.unmountWaits_.end() && !--at->second) { self.unmountWaits_.erase(at); self.unmounted_.erase(token); } } } done{*this, token};
    if (!send(wire::controlFrame("{\"type\":\"unmount\",\"token\":" + jsonQuote(token) + "}"))) return false;
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return unmounted_.count(token) != 0 || !alive_; }) && unmounted_.count(token) != 0;
  }
  // Test observation: answers and waits held for unmounts (bounded by those waiting).
  size_t unmountRecords() { std::lock_guard lock(mutex_); return unmounted_.size() + unmountWaits_.size(); }
  // One actual operation remains owned until its reply or transport retirement.
  // The UI's deadline never frees this slot or resubmits the shell action.
  std::string openRoot(const std::string& token) {
    uint32_t id = 0;
    {
      std::lock_guard lock(mutex_);
      const bool registered = std::any_of(registered_.begin(), registered_.end(), [&](const auto& s) { return s.second == token; });
      if (!alive_ || stopped_ || openCancelled_ || !registered) return "drive_share_unknown";
      if (openId_ || nextOpenId_ == UINT32_MAX) return "drive_busy";
      id = ++nextOpenId_; openId_ = id; openToken_ = token; openDone_ = false;
    }
    if (!send(wire::controlFrame("{\"type\":\"open\",\"token\":" + jsonQuote(token) + ",\"id\":" + std::to_string(id) + "}"))) {
      std::lock_guard lock(mutex_); openCancelled_ = true; return "drive_open_unknown";
    }
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return openDone_ || openCancelled_ || !alive_ || stopped_; });
    if (!openDone_) return "drive_open_unknown";
    const auto error = openError_; openId_ = 0; openToken_.clear(); return error;
  }
  void cancelOpen() { { std::lock_guard lock(mutex_); openCancelled_ = true; } changed_.notify_all(); }
  void setReadOnly(const std::string& token, bool value) { send(wire::controlFrame("{\"type\":\"readOnly\",\"token\":" + jsonQuote(token) + ",\"value\":" + (value ? "true" : "false") + "}")); }
  bool alive() { std::lock_guard lock(mutex_); return alive_ && !stopped_; }
  size_t served() { std::lock_guard lock(mutex_); return served_; }
  // Asks the helper to end, closes the transport (which also ends a helper that does not listen) and joins.
  // First half of stop(): tell the helper to unmount and end, and close its input.
  void signalStop() {
    { std::lock_guard lock(mutex_); if (stopped_) return; stopped_ = true; }
    std::lock_guard writeLock(writeMutex_);
    write_(wire::encode(wire::controlFrame("{\"type\":\"stop\"}")));
    if (closeTransport_) closeTransport_();
  }
  // Second half: wait for the helper's output to end (the caller kills a helper that does not end).
  void stop() {
    signalStop();
    pool_.stop();
    if (reader_.joinable()) reader_.join();
  }
 private:
  Backend& backend_; Read read_; Write write_; std::function<void()> closeTransport_, lost_; Workers pool_;
  std::mutex mutex_, writeMutex_; std::condition_variable changed_; std::thread reader_;
  bool alive_ = true, ready_ = false, stopped_ = false; std::string readyError_; size_t served_ = 0;
  std::map<std::string, std::string> registered_;  // share id -> token (the only shares the helper may name)
  std::map<std::string, FsMountResult> outcomes_; std::set<std::string> unmounted_; std::map<std::string, unsigned> unmountWaits_;
  uint32_t nextOpenId_ = 0, openId_ = 0; std::string openToken_, openError_;
  bool openDone_ = false, openCancelled_ = false;

  bool send(const wire::Frame& frame) { const auto bytes = wire::encode(frame); std::lock_guard lock(writeMutex_); { std::lock_guard state(mutex_); if (stopped_) return false; } return write_(bytes); }
  void forget(const FsShare& share) {
    std::lock_guard lock(mutex_);
    const auto found = registered_.find(share.id);
    if (found != registered_.end() && found->second == share.token) registered_.erase(found);
    outcomes_.erase(share.token);
  }
  void readLoop() {
    wire::Frame frame;
    while (wire::readFrame(read_, frame)) {
      if (frame.kind == wire::Kind::call) call(frame);
      else if (frame.kind == wire::Kind::control) control(frame.header);
      else break;
    }
    { std::lock_guard lock(mutex_); alive_ = false; }
    changed_.notify_all();
    if (lost_) { bool wasStopped; { std::lock_guard lock(mutex_); wasStopped = stopped_; } if (!wasStopped) lost_(); }
  }
  void call(const wire::Frame& frame) {
    Call parsed;
    const auto answer = [this, id = frame.id](Result result) { send(wire::resultFrame(id, result)); };
    if (!wire::callParse(frame, parsed)) { Result r; r.error = "drive_invalid_path"; answer(std::move(r)); return; }
    { std::unique_lock lock(mutex_); const auto known = registered_.find(parsed.share); if (known == registered_.end()) { Result r; r.error = "drive_share_unknown"; lock.unlock(); answer(std::move(r)); return; } ++served_; }
    if (!pool_.post([this, parsed = std::move(parsed), answer]() mutable {
      Result result;
      try { auto future = backend_.submit(std::move(parsed)); result = future.get(); }
      catch (...) { result.error = "drive_io_failed"; }
      answer(std::move(result));
    })) { Result r; r.error = "drive_busy"; answer(std::move(r)); }
  }
  void control(const std::string& text) {
    Json json; try { json = JsonParser::parse(text); } catch (...) { return; }
    const auto type = json.get("type").string(), token = json.get("token").string();
    bool orphanedMount = false;
    { std::lock_guard lock(mutex_);
      const bool registered = std::any_of(registered_.begin(), registered_.end(), [&](const auto& share) { return share.second == token; });
      if (type == "ready") ready_ = true;
      else if (type == "unavailable") readyError_ = json.get("error").string().empty() ? "drive_helper_failed" : "winfsp_missing";
      else if (type == "mounted") {
        if (!registered) orphanedMount = shareToken(token);
        else { FsMountResult r; r.token = token; r.letter = json.get("letter").string(); if (r.letter.size() != 1 || r.letter[0] < 'D' || r.letter[0] > 'Z') { r.letter.clear(); r.error = "drive_map_failed"; } outcomes_[token] = r; }
      }
      else if (type == "failed") { if (registered) { FsMountResult r; r.token = token; const auto error = json.get("error").string(); r.error = error == "drive_letter_unavailable" ? error : "drive_map_failed"; r.status = uint32_t(json.get("status").number(0)); outcomes_[token] = r; } }
      else if (type == "opened") {
        const auto* fields = std::get_if<Json::Object>(&json.value);
        if (!fields || fields->size() != 4 || !std::holds_alternative<std::string>(json.get("error").value) || !openId_ || json.get("id").number() != openId_ ||
            token != openToken_ || (!json.get("error").string().empty() && !openErrorCode(json.get("error").string()))) return;
        openError_ = json.get("error").string(); openDone_ = true;
      }
      else if (type == "unmounted") { if (unmountWaits_.count(token)) unmounted_.insert(token); }
      else return; }
    changed_.notify_all();
    // Never hold mutex_ while sending: send() takes the write lock and then
    // checks the bridge state. Duplicate cancellation is harmless in Helper.
    if (orphanedMount) send(wire::controlFrame("{\"type\":\"unmount\",\"token\":" + jsonQuote(token) + "}"));
  }
};

// ---- the helper process ------------------------------------------------------------
struct UserProcess {
  virtual ~UserProcess() = default;
  WinHandle process, job, toChild, fromChild;
  virtual size_t read(uint8_t* buffer, size_t size) { DWORD count = 0; return ReadFile(fromChild.value, buffer, DWORD(std::min<size_t>(size, 1 << 20)), &count, nullptr) ? size_t(count) : size_t(0); }
  virtual bool write(const std::vector<uint8_t>& bytes) { size_t done = 0; while (done < bytes.size()) { DWORD count = 0; if (!WriteFile(toChild.value, bytes.data() + done, DWORD(std::min<size_t>(bytes.size() - done, 1 << 20)), &count, nullptr) || !count) return false; done += count; } return true; }
  virtual void closeInput() { toChild = WinHandle(); }
  virtual bool wait(unsigned milliseconds) { return WaitForSingleObject(process.value, milliseconds) == WAIT_OBJECT_0; }
  virtual void terminate() { TerminateProcess(process.value, 1); }
};
// Starts `MultitapHost.exe <argument>` in the session with the user's own token
// (the one Explorer runs with), like runDriveMountHelper but long-lived: the
// pipes stay open and the job object ends the process with the service.
inline std::unique_ptr<UserProcess> startUserProcess(DWORD session, const wchar_t* argument, service_workers::Role role = service_workers::Role::driveFileSystem) {
  auto physicalCreation=service_workers::reserveCurrent(role);
  WinHandle physicalJob(physicalCreation.createTrackingJob());
  if (!session || session == 0xffffffff) throw std::runtime_error("no_interactive_session");
  HANDLE raw = nullptr; if (!WTSQueryUserToken(session, &raw)) throw std::runtime_error("no_interactive_session");
  WinHandle user(raw); HANDLE primaryRaw = nullptr;
  winCheck(DuplicateTokenEx(user.value, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID, nullptr, SecurityImpersonation, TokenPrimary, &primaryRaw), "Drive file system token");
  WinHandle primary(primaryRaw);
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE}; HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
  winCheck(CreatePipe(&inRead, &inWrite, &inherit, 1 << 20), "Drive file system input"); WinHandle childIn(inRead), parentIn(inWrite);
  winCheck(CreatePipe(&outRead, &outWrite, &inherit, 1 << 20), "Drive file system output"); WinHandle parentOut(outRead), childOut(outWrite);
  winCheck(SetHandleInformation(parentIn.value, HANDLE_FLAG_INHERIT, 0) && SetHandleInformation(parentOut.value, HANDLE_FLAG_INHERIT, 0), "Drive file system pipe inheritance");
  const DWORD attributeCount=physicalJob.value?2:1;SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, attributeCount, 0, &size); std::vector<BYTE> attributes(size);
  auto* list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  winCheck(InitializeProcThreadAttributeList(list, attributeCount, 0, &size), "Drive file system attributes");
  struct Cleanup { PPROC_THREAD_ATTRIBUTE_LIST value; ~Cleanup() { DeleteProcThreadAttributeList(value); } } cleanup{list};
  HANDLE handles[]{childIn.value, childOut.value};
  winCheck(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr), "Drive file system handles");
  struct Environment { void* value = nullptr; ~Environment() { if (value) DestroyEnvironmentBlock(value); } } environment;
  winCheck(CreateEnvironmentBlock(&environment.value, primary.value, FALSE), "Drive file system environment");
  auto out = std::make_unique<UserProcess>();
  out->job = WinHandle(CreateJobObjectW(nullptr, nullptr)); winCheck(out->job.value != nullptr, "Drive file system job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  winCheck(SetInformationJobObject(out->job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Drive file system job limits");
  const auto executable = executablePath(); std::wstring line = L"\"" + executable + L"\" " + argument;
  HANDLE ownedJobs[]{physicalJob.value,out->job.value};if(physicalJob.value)winCheck(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,ownedJobs,sizeof(ownedJobs),nullptr,nullptr),"Drive file system tracking job");
  STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = list; wchar_t desktop[] = L"winsta0\\default"; startup.StartupInfo.lpDesktop = desktop;
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES; startup.StartupInfo.hStdInput = childIn.value; startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = childOut.value;
  physicalCreation.requireLaunchAllowed();
  PROCESS_INFORMATION process{};
  winCheck(CreateProcessAsUserW(primary.value, executable.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, environment.value, std::filesystem::path(executable).parent_path().c_str(), &startup.StartupInfo, &process), "Start drive file system");
  out->process = WinHandle(process.hProcess); WinHandle thread(process.hThread);
  physicalCreation.retain(out->process.value);
  if (!physicalJob.value&&!AssignProcessToJobObject(out->job.value, out->process.value)) { TerminateProcess(out->process.value, 1); winCheck(FALSE, "Isolate drive file system"); }
  out->toChild = std::move(parentIn); out->fromChild = std::move(parentOut);
  winCheck(ResumeThread(thread.value) != DWORD(-1), "Resume drive file system");
  return out;
}

// ---- the real mounter ----------------------------------------------------------------
class WinFspMounter final : public FsMounter {
 public:
  using Start = std::function<std::shared_ptr<UserProcess>(DWORD)>;
  explicit WinFspMounter(Start start = [](DWORD session) { return std::shared_ptr<UserProcess>(startUserProcess(session, L"--drive-fs")); },
                        std::chrono::milliseconds readyTimeout = std::chrono::seconds(20))
      : start_(std::move(start)), readyTimeout_(readyTimeout) {}
  ~WinFspMounter() override { stop(); }
  bool available() override {
    const auto now = GetTickCount64();
    std::lock_guard lock(mutex_);
    if (!checkedAt_ || now - checkedAt_ > 30000) { installed_ = winfspInstalled(); helperBroken_ = false; checkedAt_ = now; if (!checkedAt_) checkedAt_ = 1; }
    return installed_ && !helperBroken_;
  }
  std::vector<FsMountResult> map(DWORD session, Backend& backend, const std::vector<FsShare>& shares) override {
    std::vector<FsMountResult> out;
    std::shared_ptr<FsBridge> bridge; std::string failure;
    try { bridge = ensure(session, backend, failure); } catch (const std::exception& e) { failure = e.what() == std::string("no_interactive_session") ? "no_interactive_session" : "drive_helper_failed"; }
    for (const auto& share : shares) {
      if (!bridge) { FsMountResult r; r.token = share.token; r.error = failure.empty() ? "drive_helper_failed" : failure; out.push_back(r); continue; }
      out.push_back(bridge->mount(share, std::chrono::seconds(25)));
    }
    return out;
  }
  void setReadOnly(const std::string& token, bool value) override { std::shared_ptr<FsBridge> b; { std::lock_guard lock(mutex_); b = bridge_; } if (b) b->setReadOnly(token, value); }
  void unmap(const std::vector<std::string>& tokens) override { std::shared_ptr<FsBridge> b; { std::lock_guard lock(mutex_); b = bridge_; } if (b) for (const auto& token : tokens) b->unmount(token, std::chrono::seconds(10)); }
  std::string openRoot(DWORD session, const std::string& token) override {
    std::shared_ptr<FsBridge> b;
    { std::lock_guard lock(mutex_); if (session != session_) return "no_interactive_session"; b = bridge_; }
    return b ? b->openRoot(token) : "drive_open_unavailable";
  }
  void cancelOpen() override { std::shared_ptr<FsBridge> b; { std::lock_guard lock(mutex_); b = bridge_; } if (b) b->cancelOpen(); }
  void stop() override {
    std::lock_guard operation(operationMutex_);
    std::shared_ptr<FsBridge> b; std::shared_ptr<UserProcess> p;
    { std::lock_guard lock(mutex_); if (armed_) *armed_ = false; armed_.reset(); b = std::move(bridge_); p = std::move(process_); session_ = 0xffffffff; }
    end(b, p);
  }
  void onLost(std::function<void()> lost) override { std::lock_guard lock(mutex_); lost_ = std::move(lost); }
 private:
  // Serialize startup/replacement/stop without holding the state mutex while
  // reading pipes, waiting for readiness, destroying bridges or joining readers.
  std::mutex mutex_, operationMutex_; std::shared_ptr<FsBridge> bridge_; std::shared_ptr<UserProcess> process_;
  std::shared_ptr<std::atomic<bool>> armed_; DWORD session_ = 0xffffffff;
  uint64_t checkedAt_ = 0; bool installed_ = false, helperBroken_ = false; std::function<void()> lost_;
  Start start_; std::chrono::milliseconds readyTimeout_;
  static void end(const std::shared_ptr<FsBridge>& bridge, const std::shared_ptr<UserProcess>& process) {
    // Start the deadline before a control write: a helper not reading its pipe
    // must not prevent the owned-process timeout from being reached.
    std::thread deadline;
    if (process) deadline = std::thread([process] { if (!process->wait(8000)) process->terminate(); });
    if (bridge) bridge->signalStop();
    if (deadline.joinable()) deadline.join();
    if (bridge) bridge->stop();
  }
  std::shared_ptr<FsBridge> ensure(DWORD session, Backend& backend, std::string& failure) {
    std::lock_guard operation(operationMutex_);
    std::shared_ptr<FsBridge> previous; std::shared_ptr<UserProcess> previousProcess;
    {
      std::lock_guard lock(mutex_);
      if (bridge_ && session_ == session && bridge_->alive()) return bridge_;
      if (armed_) *armed_ = false; armed_.reset();
      previous = std::move(bridge_); previousProcess = std::move(process_); session_ = 0xffffffff;
    }
    end(previous, previousProcess);
    auto started = start_(session);
    if (!started) { failure = "drive_helper_failed"; return nullptr; }
    auto armed = std::make_shared<std::atomic<bool>>(false);
    auto bridge = std::make_shared<FsBridge>(backend, [started](uint8_t* b, size_t n) { return started->read(b, n); }, [started](const std::vector<uint8_t>& bytes) { return started->write(bytes); },
      [started] { started->closeInput(); }, [this, armed] {
        if (!*armed) return;  // an unavailable/early-exit startup falls back; it is not a live-channel loss
        std::function<void()> cb;
        { std::lock_guard lock(mutex_); if (armed_ != armed || !*armed) return; cb = lost_; }
        if (cb) cb();
      });
    const auto ready = bridge->waitReady(readyTimeout_);
    if (!ready.empty() || !bridge->alive()) {
      failure = ready == "winfsp_missing" ? "drive_helper_failed" : ready.empty() ? "drive_helper_failed" : ready;
      { std::lock_guard lock(mutex_); helperBroken_ = ready == "winfsp_missing"; }
      started->terminate(); end(bridge, started); return nullptr;
    }
    { std::lock_guard lock(mutex_); bridge_ = bridge; process_ = started; session_ = session; armed_ = armed; *armed = true; }
    return bridge;
  }
};
}
