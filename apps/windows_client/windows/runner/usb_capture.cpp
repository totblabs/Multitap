#include "usb_capture.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <setupapi.h>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include "usbdk_api.h"
#include "usb_topology.h"
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "bcrypt.lib")

namespace multitap {
namespace {
using V = flutter::EncodableValue;
using M = flutter::EncodableMap;
using L = flutter::EncodableList;

std::wstring ExecutableDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  if (!length || length >= path.size()) return {};
  path.resize(length);
  const auto slash = path.find_last_of(L'\\');
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::string FileSha256(HANDLE file) {
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; std::string out;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
  DWORD size = 0, got = 0; std::vector<uint8_t> object;
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &got, 0) >= 0) {
    object.resize(size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) >= 0) {
      std::vector<uint8_t> buffer(1 << 16); DWORD read = 0; bool ok = true;
      LARGE_INTEGER start{}; SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
      while (ReadFile(file, buffer.data(), DWORD(buffer.size()), &read, nullptr) && read) if (BCryptHashData(hash, buffer.data(), read, 0) < 0) { ok = false; break; }
      uint8_t digest[32]{};
      if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
        const char hex[] = "0123456789abcdef";
        for (const auto value : digest) { out += hex[value >> 4]; out += hex[value & 15]; }
      }
      BCryptDestroyHash(hash);
    }
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return out;
}
}  // namespace

std::string InstallUsbDk(const std::wstring& directory) {
  const std::wstring path = directory + L"\\usbdk\\" + UsbDkInstaller::file;
  // Held without write sharing from the check to the end of the installation.
  HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (raw == INVALID_HANDLE_VALUE) return "usbdk_missing";
  struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{raw};
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(raw, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
      info.nFileSizeHigh != 0 || info.nFileSizeLow != UsbDkInstaller::size) return "usbdk_signature_invalid";
  if (FileSha256(raw) != UsbDkInstaller::sha256 || !usb::usbdk::SignedByRedHat(path)) return "usbdk_signature_invalid";
  wchar_t system[MAX_PATH]{};
  const UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
  if (!systemLength || systemLength >= MAX_PATH) return "usbdk_install_failed";
  const std::wstring msiexec = std::wstring(system) + L"\\msiexec.exe";
  // The client runs unelevated. Installing the
  // driver is the one step here that needs administrator rights, so only this
  // msiexec asks for them ("runas": one Windows approval prompt). The MSI sits
  // in the administrator-only install folder and stays held above; its size,
  // SHA-256 and Red Hat signature were just verified.
  const std::wstring arguments = L"/i \"" + path + L"\" /qn /norestart";
  SHELLEXECUTEINFOW execute{}; execute.cbSize = sizeof(execute);
  execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC; execute.lpVerb = L"runas";
  execute.lpFile = msiexec.c_str(); execute.lpParameters = arguments.c_str(); execute.lpDirectory = system; execute.nShow = SW_HIDE;
  if (!ShellExecuteExW(&execute) || !execute.hProcess) return GetLastError() == ERROR_CANCELLED ? "usbdk_elevation_cancelled" : "usbdk_install_failed";
  PROCESS_INFORMATION process{}; process.hProcess = execute.hProcess;
  const DWORD wait = WaitForSingleObject(process.hProcess, 5 * 60 * 1000);
  DWORD code = 1;
  if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  if (wait != WAIT_OBJECT_0) return "usbdk_install_failed";
  if (code == 3010 || code == 1641) return "usbdk_restart_required";
  return code == 0 ? "" : "usbdk_install_failed";
}

struct UsbCaptureChannel::State : std::enable_shared_from_this<UsbCaptureChannel::State> {
  struct Job { std::string method; M arguments; std::unique_ptr<flutter::MethodResult<V>> result; std::shared_ptr<std::atomic<bool>> cancelled; std::pair<std::string, int64_t> prepareKey; };
  struct Completion { std::unique_ptr<flutter::MethodResult<V>> result; V value; std::string error; DWORD nativeError = 0; };
  explicit State(HWND window) : window(window) {}
  HWND window;
  // Every window's channel in this process. Each remote window has its own
  // engine, so "held by another window" is asked of the other engines
  // themselves: no copy that a closed window could leave behind. Never freed
  // (a channel may unregister during process exit).
  struct Windows { std::mutex mutex; std::vector<std::weak_ptr<State>> live; };
  static Windows& All() { static auto* windows = new Windows(); return *windows; }
  // This window's VM name, which the other windows show for a device it holds
  // (on this PC only), and the devices other windows asked it to give back.
  std::string label;
  std::deque<std::pair<std::string, std::string>> handOvers;
  std::function<void(const std::string&, const std::string&)> handOverCallback;
  // The other window of this process whose engine holds [id], if any.
  std::shared_ptr<State> HolderOf(const std::string& id) {
    std::vector<std::shared_ptr<State>> others;
    {
      std::lock_guard lock(All().mutex);
      for (const auto& weak : All().live) if (auto state = weak.lock(); state && state.get() != this) others.push_back(state);
    }
    for (auto& state : others) if (auto* engine = state->Current(); engine && engine->Holds(id)) return state;
    return nullptr;
  }
  std::string Label() { std::lock_guard lock(mutex); return label; }
  // Two lanes: what can take long (listing walks the device tree and the volumes,
  // the safe-hand-over check, the driver installation, the "did it come back"
  // wait) must never hold up what the VM is waiting for (capturing and giving
  // back a device). Cancelling a request is not a job at all (see Enqueue).
  struct Lane { std::deque<Job> jobs; std::condition_variable wake; std::thread worker; };
  std::mutex mutex, engineMutex;
  Lane query, deviceLane;
  std::map<std::pair<std::string, int64_t>, std::shared_ptr<std::atomic<bool>>> preparingJobs;
  std::deque<Completion> done; std::deque<int64_t> gone;
  std::function<void(int64_t)> goneCallback;
  std::function<void()> changedCallback;
  std::atomic<bool> changed{false};
  std::unique_ptr<usb::ChangeWatcher> watcher;
  std::unique_ptr<usb::Captures> captures;
  bool stopped = false;
  usb::Captures* Current() { std::lock_guard lock(mutex); return captures.get(); }

  void Post() { PostMessageW(window, UsbCaptureChannel::completed_message, 0, 0); }
  void Finish(std::unique_ptr<flutter::MethodResult<V>> result, V value) {
    { std::lock_guard lock(mutex); if (stopped) return; done.push_back({std::move(result), std::move(value), {}}); }
    Post();
  }
  void Fail(std::unique_ptr<flutter::MethodResult<V>> result, std::string code, DWORD nativeError = 0) {
    { std::lock_guard lock(mutex); done.push_back({std::move(result), V(), std::move(code), nativeError}); }
    Post();
  }
  // The capture engine, available once UsbDk is installed and verified.
  usb::Captures* Engine() {
    std::lock_guard engineGuard(engineMutex);   // both lanes may ask at once
    if (auto* existing = Current()) return existing;
    auto api = usb::UsbDkApi::Load();
    if (!api) return nullptr;
    auto made = std::make_unique<usb::Captures>(std::move(api), [weak = weak_from_this()](int64_t handle) {
      if (auto state = weak.lock()) {
        { std::lock_guard lock(state->mutex); if (state->stopped) return; state->gone.push_back(handle); }
        state->Post();
      }
    });
    std::lock_guard lock(mutex);   // Enqueue reads the pointer under the same lock
    captures = std::move(made);
    return captures.get();
  }
  static const V* Find(const M& arguments, const char* key) { const auto found = arguments.find(V(key)); return found == arguments.end() ? nullptr : &found->second; }
  static std::optional<int64_t> Integer(const M& arguments, const char* key) {
    const auto* value = Find(arguments, key);
    if (!value) return std::nullopt;
    if (const auto* narrow = std::get_if<int32_t>(value)) return *narrow;
    if (const auto* wide = std::get_if<int64_t>(value)) return *wide;
    return std::nullopt;
  }
  static const std::vector<uint8_t>* Bytes(const M& arguments, const char* key) {
    const auto* value = Find(arguments, key);
    return value ? std::get_if<std::vector<uint8_t>>(value) : nullptr;
  }

  void Run(Job& job) {
    const auto& method = job.method;
    if (method == "environment") {
      const bool x64 = sizeof(void*) == 8;
      Finish(std::move(job.result), V(M{{V("driver"), V(std::string(!x64 ? "unsupported" : Engine() ? "ready" : "missing"))}}));
    } else if (method == "list") {
      auto* engine = Engine();
      L items;
      if (engine) {
        const auto listed = engine->List();
        const auto described = usb::DescribeAll(usb::ReadTopology(listed));
        for (size_t i = 0; i < listed.size() && i < described.size(); ++i) {
          const auto& device = listed[i]; const auto& info = described[i].info;
          L chips;
          for (const auto& chip : info.chips) chips.push_back(V(chip));
          M item{{V("id"), V(device.id)}, {V("name"), V(info.title)}, {V("subtitle"), V(info.subtitle)}, {V("vidPid"), V(info.vidPid)},
              {V("kind"), V(std::string(usb::KindName(info.kind)))}, {V("neededByPc"), V(described[i].hidden)}, {V("chips"), V(std::move(chips))},
              {V("arrivalMs"), V(int64_t(info.arrivalMs))}, {V("memoryKey"), V(info.memoryKey)}, {V("busyElsewhere"), V(info.busyElsewhere)}, {V("noReadableDrive"), V(info.noReadableDrive)},
              {V("descriptors"), V(device.descriptors)}, {V("speed"), V(device.speed)}, {V("busy"), V(device.busy)}, {V("unreadable"), V(device.unreadable)}};
          // Held by another remote window of this client: its VM name ("" unknown).
          if (!device.busy) if (const auto holder = HolderOf(device.id)) item[V("otherWindow")] = V(holder->Label());
          items.push_back(V(std::move(item)));
        }
      }
      Finish(std::move(job.result), V(std::move(items)));
    } else if (method == "prepare") {
      // Before a storage device leaves this PC: are files open on it?
      const auto* id = Find(job.arguments, "id");
      const auto* text = id ? std::get_if<std::string>(id) : nullptr;
      const auto bar = text ? text->find('|') : std::string::npos;
      if (!text || bar == std::string::npos) { Fail(std::move(job.result), "usb_capture_failed"); return; }
      if (job.cancelled && *job.cancelled) { Finish(std::move(job.result), V(M{{V("ready"), V(false)}, {V("checked"), V(false)}})); return; }
      const auto volumes = usb::VolumePathsOf(usb::Widen(text->substr(0, bar)), usb::Widen(text->substr(bar + 1)));
      auto api = usb::MakeVolumeApi();
      const auto outcome = usb::PrepareStorage(*api, volumes, false, [flag = job.cancelled] { return flag && flag->load(); });
      Finish(std::move(job.result), V(M{{V("ready"), V(outcome.ready)}, {V("checked"), V(outcome.checkedAll)}, {V("blocker"), V(outcome.blocker)}}));
    } else if (method == "watch") {
      if (!watcher) watcher = std::make_unique<usb::ChangeWatcher>([this] {
        if (!changed.exchange(true)) Post();
      });
      Finish(std::move(job.result), V());
    } else if (method == "unwatch") {
      watcher.reset();
      Finish(std::move(job.result), V());    } else if (method == "capture") {
      const auto* id = Find(job.arguments, "id");
      const auto* text = id ? std::get_if<std::string>(id) : nullptr;
      const auto* windowName = Find(job.arguments, "window");
      if (const auto* name = windowName ? std::get_if<std::string>(windowName) : nullptr; name && name->size() <= 256) { std::lock_guard lock(mutex); label = *name; }
      auto* engine = Engine();
      if (!text || !engine) { Fail(std::move(job.result), "usb_capture_failed"); return; }
      // Another window of this client holds it: UsbDk would refuse it anyway,
      // and its volumes are left alone.
      if (HolderOf(*text)) { Fail(std::move(job.result), "usb_device_redirected"); return; }
      // The pre-offer query never dismounts. Recheck and dismount immediately
      // before this accepted capture; its pending ownership is retained by
      // Dart until the late handle has actually been released on cancellation.
      const auto bar = text->find('|');
      if (bar == std::string::npos) { Fail(std::move(job.result), "usb_capture_failed"); return; }
      auto volumes = usb::MakeVolumeApi();
      if (!usb::PrepareStorage(*volumes, usb::VolumePathsOf(usb::Widen(text->substr(0, bar)), usb::Widen(text->substr(bar + 1)))).ready) {
        Fail(std::move(job.result), "usb_device_busy"); return;
      }
      std::string code; DWORD error = 0;
      const auto handle = engine->Capture(*text, code, &error);
      if (handle <= 0) Fail(std::move(job.result), code.empty() ? "usb_capture_failed" : code, error);
      else Finish(std::move(job.result), V(handle));
    } else if (method == "release") {
      const auto handle = Integer(job.arguments, "handle");
      usb::Stats stats;
      if (auto* engine = Current(); handle && engine) stats = engine->Release(*handle);
      Finish(std::move(job.result), V(M{{V("urbs"), V(int64_t(stats.urbs))}, {V("kib"), V(int64_t(stats.kib))}, {V("stalls"), V(int64_t(stats.stalls))}, {V("failed"), V(int64_t(stats.failed))},
          {V("timeouts"), V(int64_t(stats.timeouts))}, {V("slow"), V(int64_t(stats.slow))}, {V("firstClass"), V(int64_t(stats.firstClass))}, {V("firstStatus"), V(int64_t(stats.firstStatus))},
          {V("slowClass"), V(int64_t(stats.slowClass))}, {V("firstUsbd"), V(int64_t(stats.firstUsbd))}}));
    } else if (method == "returned") {
      // After a release: did Windows take the device back? (0 cannot be told, 1 yes, 2 not in time)
      const auto* id = Find(job.arguments, "id"); const auto* storage = Find(job.arguments, "storage");
      const auto* text = id ? std::get_if<std::string>(id) : nullptr;
      const auto* wantChild = storage ? std::get_if<bool>(storage) : nullptr;
      auto* engine = Current();
      uint32_t why = 0;
      const int state = text && engine ? engine->WaitReturned(*text, wantChild && *wantChild, 10000, &why) : 0;
      Finish(std::move(job.result), V(M{{V("state"), V(int32_t(state))}, {V("why"), V(int64_t(why))}}));
    } else if (method == "install") {
      const auto code = InstallUsbDk(ExecutableDirectory());
      if (code.empty() && !Engine()) Fail(std::move(job.result), "usbdk_missing");
      else if (code.empty()) Finish(std::move(job.result), V());
      else Fail(std::move(job.result), code);
    } else {
      Fail(std::move(job.result), "usb_capture_failed");
    }
  }

  void Loop(Lane& lane) {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex);
        lane.wake.wait(lock, [this, &lane] { return stopped || !lane.jobs.empty(); });
        if (stopped && lane.jobs.empty()) return;
        job = std::move(lane.jobs.front()); lane.jobs.pop_front();
      }
      try { Run(job); }
      catch (...) { if (job.result) Fail(std::move(job.result), "usb_capture_failed"); }
      if (job.cancelled) { std::lock_guard lock(mutex); const auto found = preparingJobs.find(job.prepareKey); if (found != preparingJobs.end() && found->second == job.cancelled) preparingJobs.erase(found); }
    }
  }
};

UsbCaptureChannel::UsbCaptureChannel(HWND reply_window) : state_(std::make_shared<State>(reply_window)) {
  state_->query.worker = std::thread([this] { state_->Loop(state_->query); });
  state_->deviceLane.worker = std::thread([this] { state_->Loop(state_->deviceLane); });
  std::lock_guard lock(State::All().mutex);
  auto& live = State::All().live;
  live.erase(std::remove_if(live.begin(), live.end(), [](const auto& weak) { return weak.expired(); }), live.end());
  live.push_back(state_);
}

UsbCaptureChannel::~UsbCaptureChannel() { Shutdown(); }

void UsbCaptureChannel::SetGone(std::function<void(int64_t)> gone) { std::lock_guard lock(state_->mutex); state_->goneCallback = std::move(gone); }

void UsbCaptureChannel::SetChanged(std::function<void()> changed) { std::lock_guard lock(state_->mutex); state_->changedCallback = std::move(changed); }

void UsbCaptureChannel::SetHandOver(std::function<void(const std::string&, const std::string&)> handOver) {
  std::lock_guard lock(state_->mutex); state_->handOverCallback = std::move(handOver);
}

void UsbCaptureChannel::Enqueue(const flutter::MethodCall<V>& call, std::unique_ptr<flutter::MethodResult<V>> result) {
  const auto* arguments = std::get_if<M>(call.arguments());
  M empty;
  const M& values = arguments ? *arguments : empty;
  if (call.method_name() == "cancelPrepare") {
    const auto* id = State::Find(values, "id"); const auto* text = id ? std::get_if<std::string>(id) : nullptr;
    const auto generation = State::Integer(values, "generation");
    if (text && generation) { std::lock_guard lock(state_->mutex); const auto found = state_->preparingJobs.find({*text, *generation}); if (found != state_->preparingJobs.end()) *found->second = true; }
    result->Success(); return;
  }
  if (call.method_name() == "transfer") {
    // Requests run on their own threads so a slow device never delays the others.
    const auto handle = State::Integer(values, "handle"), token = State::Integer(values, "token"), endpoint = State::Integer(values, "endpoint"),
        type = State::Integer(values, "type"), length = State::Integer(values, "length");
    const auto* data = State::Bytes(values, "data");
    const auto* setup = State::Bytes(values, "setup");
    if (!handle || !token || !endpoint || !type || !length || !data || *length < 0 || *length > 262144 || *endpoint < 0 || *endpoint > 255 || (*type != 0 && *type != 2 && *type != 3) ||
        (setup && setup->size() != 8) || (*type == 0 && !setup) || data->size() > 262144) {
      result->Error("usb_transfer_failed", "Invalid USB request.");
      return;
    }
    usb::Captures* engine = state_->Current();
    if (!engine) { state_->Finish(std::move(result), V(M{{V("status"), V(usb::kNoDevice)}, {V("data"), V(std::vector<uint8_t>())}})); return; }
    usb::Request request;
    request.token = *token; request.endpoint = uint8_t(*endpoint); request.type = int(*type); request.length = size_t(*length); request.data = *data;
    if (setup) { request.hasSetup = true; std::memcpy(request.setup, setup->data(), 8); }
    auto shared = std::make_shared<std::unique_ptr<flutter::MethodResult<V>>>(std::move(result));
    engine->Transfer(*handle, std::move(request), [weak = std::weak_ptr<State>(state_), shared](usb::Result outcome) {
      if (auto state = weak.lock()) state->Finish(std::move(*shared), V(M{{V("status"), V(int32_t(outcome.status))}, {V("data"), V(std::move(outcome.data))}}));
    });
    return;
  }
  if (call.method_name() == "cancel") {
    // Not queued behind a slow listing: a request the VM gave up on must end now.
    const auto handle = State::Integer(values, "handle"), token = State::Integer(values, "token");
    if (auto* engine = state_->Current(); handle && token && engine) engine->Cancel(*handle, *token);
    result->Success();
    return;
  }
  if (call.method_name() == "holder" || call.method_name() == "handOver") {
    // Another window of this client and the device it holds (이 창으로 옮기기):
    // answered at once, nothing here waits on UsbDk.
    const auto* id = State::Find(values, "id"); const auto* text = id ? std::get_if<std::string>(id) : nullptr;
    const auto holder = text ? state_->HolderOf(*text) : nullptr;
    if (call.method_name() == "holder") { result->Success(V(M{{V("held"), V(holder != nullptr)}})); return; }
    // That window's own session gives it back, so its VM is told.
    const auto* window = State::Find(values, "window"); const auto* name = window ? std::get_if<std::string>(window) : nullptr;
    bool asked = false;
    if (holder) {
      std::lock_guard lock(holder->mutex);
      if (!holder->stopped && holder->handOvers.size() < 16) { holder->handOvers.emplace_back(*text, name && name->size() <= 256 ? *name : std::string()); asked = true; }
    }
    if (asked) holder->Post();
    result->Success(V(M{{V("asked"), V(asked)}}));
    return;
  }
  auto& lane = call.method_name() == "capture" || call.method_name() == "release" || call.method_name() == "install" ? state_->deviceLane : state_->query;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopped || lane.jobs.size() >= 64) { result->Error("usb_capture_failed", "USB worker is busy."); return; }
    State::Job job{call.method_name(), values, std::move(result), {}, {}};
    if (call.method_name() == "prepare") {
      const auto* id = State::Find(values, "id"); const auto* text = id ? std::get_if<std::string>(id) : nullptr;
      const auto generation = State::Integer(values, "generation");
      if (text && generation && *generation > 0) { job.prepareKey = {*text, *generation}; job.cancelled = std::make_shared<std::atomic<bool>>(false); state_->preparingJobs[job.prepareKey] = job.cancelled; }
    }
    lane.jobs.push_back(std::move(job));
  }
  lane.wake.notify_one();
}

void UsbCaptureChannel::Complete() {
  std::deque<State::Completion> done; std::deque<int64_t> gone; std::function<void(int64_t)> callback; std::function<void()> changedCallback;
  std::deque<std::pair<std::string, std::string>> handOvers; std::function<void(const std::string&, const std::string&)> handOverCallback;
  {
    std::lock_guard lock(state_->mutex);
    done.swap(state_->done); gone.swap(state_->gone); callback = state_->goneCallback; changedCallback = state_->changedCallback;
    handOvers.swap(state_->handOvers); handOverCallback = state_->handOverCallback;
  }
  if (state_->changed.exchange(false) && changedCallback) changedCallback();
  for (auto& item : done) {
    if (!item.result) continue;
    if (item.error.empty()) item.result->Success(item.value);
    // UsbDk's Win32 error of a refused capture: a number for the diagnostics, never text.
    else if (item.nativeError) item.result->Error(item.error, "USB operation failed.", V(M{{V("error"), V(int64_t(item.nativeError))}}));
    else item.result->Error(item.error, "USB operation failed.");
  }
  if (callback) for (const auto handle : gone) callback(handle);
  if (handOverCallback) for (const auto& [id, window] : handOvers) handOverCallback(id, window);
}

void UsbCaptureChannel::Shutdown() {
  if (!state_) return;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopped && !state_->query.worker.joinable() && !state_->deviceLane.worker.joinable()) return;
    state_->stopped = true;
  }
  state_->query.wake.notify_all();
  state_->deviceLane.wake.notify_all();
  if (state_->query.worker.joinable()) state_->query.worker.join();
  if (state_->deviceLane.worker.joinable()) state_->deviceLane.worker.join();
  state_->watcher.reset();
  // Every captured device goes back to Windows; the other windows see it held
  // until then.
  if (state_->captures) state_->captures->ReleaseAll();
  {
    std::lock_guard lock(State::All().mutex);
    auto& live = State::All().live;
    live.erase(std::remove_if(live.begin(), live.end(), [this](const auto& weak) { const auto state = weak.lock(); return !state || state == state_; }), live.end());
  }
  std::lock_guard lock(state_->mutex);
  state_->goneCallback = nullptr; state_->changedCallback = nullptr; state_->handOverCallback = nullptr; state_->handOvers.clear();
}
}  // namespace multitap
