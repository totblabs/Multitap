#pragma once
// usb-v1 on the client PC: the logic between the Dart session and the UsbDk
// driver (docs/usb-redirection.md "PC side"), over an abstract Api so it is
// tested without a driver, a device or an installation. The real Api is
// usbdk_api.h.
//
// - A device is captured (taken away from its own Windows driver) only by
//   Capture(), which Dart calls after the VM accepted it; it is given back by
//   Release(), by closing the handle (UsbDk returns it when the handle closes,
//   also when this process dies) or when it is unplugged.
// - Requests run on worker threads; the answer is delivered through a callback.
//   Cancel() aborts the request's pipe (endpoint 0 for a control transfer: the
//   direction bit of its setup is not an endpoint address) and cancels the one
//   pending I/O. A request being cancelled answers with -ECONNRESET, never data.
// - A control transfer the device has not answered within the USB limit (5 s)
//   is aborted and answers -ETIMEDOUT, so the VM never waits for it for ever.
// - SET_INTERFACE, SET_CONFIGURATION and CLEAR_FEATURE(ENDPOINT_HALT) are not
//   control transfers for UsbDk but its own calls, so they are translated.
// - No capture-registry lock is held across a call into UsbDk: listing a device that is busy with
//   the VM's traffic never blocks Transfer/Cancel/Release (Transfer runs on the
//   UI thread).
// - Nothing leaves this layer but fixed status numbers, bytes the device sent,
//   descriptors and counters. No error text, no names.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace multitap::usb {

struct DeviceInfo {
  std::wstring deviceId, instanceId;
  uint8_t descriptor[18]{};
  uint64_t speed = 0;  // UsbDk: 1 low, 2 full, 3 high, 4 super
};

// Wire statuses (negated Linux errno), as usb-v1 allows.
inline constexpr int kOk = 0, kIo = -5, kNoDevice = -19, kStall = -32, kTimeout = -110, kProtocol = -71, kOverflow = -75, kReset = -104, kShutdown = -108;

class Api {
 public:
  virtual ~Api() = default;
  virtual bool Devices(std::vector<DeviceInfo>& out) = 0;
  // The full configuration descriptor [index] of [device].
  virtual bool ConfigDescriptor(const DeviceInfo& device, unsigned index, std::vector<uint8_t>& out) = 0;
  // Takes the device from its Windows driver; nullptr + [error] on failure.
  virtual void* Start(const DeviceInfo& device, DWORD& error) = 0;
  virtual void Stop(void* handle) = 0;
  // One blocking request. [type]: 0 control, 2 bulk, 3 interrupt. For a control
  // transfer [buffer] holds the 8-byte setup packet followed by the data (OUT) or
  // room for it (IN), [address] is 0; otherwise [address] has bit 7 for IN.
  // Returns a Win32 error (0 = done); [usbd] is the USBD status (also when the
  // call failed, if the driver reported one), [actual] the data bytes moved
  // (control: without the setup packet). [token] names the request for CancelRequest.
  virtual DWORD Transfer(void* handle, uint8_t address, int type, std::vector<uint8_t>& buffer, uint32_t& usbd, size_t& actual, int64_t token) = 0;
  virtual bool Abort(void* handle, uint8_t address) = 0;
  // Cancels the one pending I/O of [token] (it then ends with ERROR_OPERATION_ABORTED).
  virtual bool CancelRequest(void*, int64_t) { return false; }
  virtual bool Reset(void* handle, uint8_t address) = 0;
  virtual bool SetAltSetting(void* handle, unsigned interfaceIndex, unsigned alternate) = 0;
  // Has Windows taken [device] back (started, no problem; with [wantChild] also
  // with a child device, e.g. the disk)? 1 yes, 0 not yet, -1 cannot be told.
  // For "not yet", [why] is what was missing (kBack*, below).
  virtual int Returned(const DeviceInfo&, bool /*wantChild*/, uint32_t& /*why*/) { return -1; }
};

// Why a released device is not back yet (diagnostics: stage 10 `error`): its node is
// not in the device tree, present but not started, started without its child (the
// disk of a storage device), or kBackProblemBase + the Windows problem code (CM_PROB_*).
inline constexpr uint32_t kBackAbsent = 1, kBackNotStarted = 2, kBackNoChild = 3, kBackProblemBase = 100;

// USBD_STATUS values (usb.h) the mapping knows. Checked against the SDK's
// usb.h by the native test (until 2026-11-02 INTERNAL_HC_ERROR held ERROR_BUSY's
// value and TIMEOUT held INVALID_PARAMETER's, so every stall UsbDk reports on a
// control pipe reached the VM as -EPROTO, which usbip-win2 turns into BTSTUFF).
inline constexpr uint32_t kUsbdStall = 0xC0000004u, kUsbdHalted = 0xC0000030u, kUsbdOverrun = 0xC0000008u, kUsbdBufferOverrun = 0xC000000Cu, kUsbdBabble = 0xC0000012u,
    kUsbdCanceled = 0xC0010000u, kUsbdInternalHc = 0x80000800u, kUsbdTimeout = 0xC0006000u, kUsbdNoResponse = 0xC0000005u, kUsbdDeviceGone = 0xC0007000u,
    kUsbdInvalidParameter = 0x80000300u;

inline int MapStatus(DWORD win32, uint32_t usbd, bool cancelled, bool control, bool* gone = nullptr) {
  if (gone) *gone = false;
  if (cancelled) return kReset;
  const auto fromUsbd = [&](uint32_t status) {
    switch (status) {
      case kUsbdStall: case kUsbdHalted: return kStall;
      // UsbDk completes a STALL on the control pipe as success with INTERNAL_HC_ERROR
      // (RedirectorStrategy.cpp, "Do not report STALL conditions on control pipe").
      case kUsbdInternalHc: return control ? kStall : kProtocol;
      case kUsbdOverrun: case kUsbdBufferOverrun: case kUsbdBabble: return kOverflow;
      case kUsbdCanceled: return kReset;
      case kUsbdTimeout: return kTimeout;
      case kUsbdNoResponse: case kUsbdDeviceGone: return kNoDevice;
      case kUsbdInvalidParameter: return kIo;   // usbip-win2 hands -EIO to the VM as INVALID_PARAMETER again
      default: return kProtocol;
    }
  };
  if (win32 != 0) {
    switch (win32) {
      case ERROR_OPERATION_ABORTED: return kShutdown;
      case ERROR_DEVICE_NOT_CONNECTED: case ERROR_FILE_NOT_FOUND: case ERROR_BAD_COMMAND: case ERROR_INVALID_HANDLE: case ERROR_NO_SUCH_DEVICE:
        if (gone) *gone = true;
        return kNoDevice;
      case ERROR_SEM_TIMEOUT: case WAIT_TIMEOUT: return kTimeout;
      // A USBD error the driver reported with the failure wins; without one, a
      // failed transfer with ERROR_GEN_FAILURE is how Windows reports a stalled
      // endpoint to a UsbDk client (libusb reads it the same way). A stall must
      // stay a stall: the VM answers it with a clear-halt, an I/O error it does not.
      case ERROR_GEN_FAILURE: return (usbd >> 28) != 0 && usbd != 0 ? fromUsbd(usbd) : kStall;
      default: return (usbd >> 28) != 0 && usbd != 0 && fromUsbd(usbd) != kProtocol ? fromUsbd(usbd) : kIo;
    }
  }
  if (usbd == 0 || (usbd >> 28) == 0) return kOk;    // severity 0: success
  return fromUsbd(usbd);
}

struct Listed { std::string id; std::wstring deviceId, instanceId; std::vector<uint8_t> descriptors; std::string speed; bool busy = false; bool unreadable = false; };
struct Request { int64_t token = 0; uint8_t endpoint = 0; int type = 2; size_t length = 0; bool hasSetup = false; uint8_t setup[8]{}; std::vector<uint8_t> data; };
struct Result { int status = kIo; std::vector<uint8_t> data; uint32_t usbd = 0; };   // usbd: the driver's raw USBD status (diagnostics only)

// What the diagnostics may say about a request: a control transfer is
// bmRequestType<<8 | bRequest; any other is 0x10000 | type<<8 | endpoint
// address. Never data.
inline uint32_t RequestClass(const Request& request) {
  if (request.type == 0 && request.hasSetup) return uint32_t(request.setup[0]) << 8 | request.setup[1];
  return 0x10000u | uint32_t(request.type) << 8 | request.endpoint;
}

// Counters of one capture (fixed numbers only). [firstStatus] is the magnitude
// of the first status that was neither success nor a cancellation, with
// [firstClass] the request it belonged to and [firstUsbd] the USBD status the
// driver reported for it; [slowClass] the first request that took longer than
// [Captures::slowMs].
struct Stats {
  uint32_t urbs = 0, kib = 0, stalls = 0, failed = 0, timeouts = 0, slow = 0, firstClass = 0, firstStatus = 0, slowClass = 0, firstUsbd = 0;
};

inline std::string Utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? size_t(size) : 0, '\0');
  if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}

class Captures {
 public:
  using Gone = std::function<void(int64_t handle)>;
  static constexpr unsigned slowMs = 5000, defaultControlTimeoutMs = 5000;
  explicit Captures(std::unique_ptr<Api> api, Gone gone = {}) : api_(std::move(api)), completion_(std::make_shared<Completion>()) {
    completion_->gone = std::move(gone);
    watchdog_ = std::thread([this] { Watch(); });
  }
  ~Captures() {
    // Do not start another callback after shutdown. A callback already entered
    // may finish; its owner must keep its callback target alive while running.
    // No callback runs under an engine lock, so it may reenter or destroy us.
    completion_->open = false;
    {
      std::lock_guard lock(watchMutex_);
      watchStop_ = true;
    }
    watchWake_.notify_all();
    if (watchdog_.joinable()) watchdog_.join();
    ReleaseAll();
  }
  Captures(const Captures&) = delete;
  Captures& operator=(const Captures&) = delete;

  void SetControlTimeout(unsigned milliseconds) { controlTimeoutMs_ = milliseconds; }
  void SetReleaseWait(unsigned milliseconds) { releaseWaitMs_ = milliseconds; }

  // This PC's USB devices with every descriptor, id = "deviceId|instanceId".
  // UsbDk is asked without any lock held. A device this client has captured is
  // not asked again (it is busy with the VM's traffic): its descriptors are the
  // ones read before it was captured.
  std::vector<Listed> List() {
    std::vector<DeviceInfo> found;
    std::vector<Listed> out;
    if (!api_->Devices(found)) return out;
    std::set<std::string> busy;
    std::map<std::string, std::vector<uint8_t>> cache;
    {
      std::lock_guard lock(mutex_);
      for (const auto& [handle, capture] : captures_) busy.insert(capture->id);
      cache = descriptorCache_;
    }
    std::map<std::string, std::vector<uint8_t>> learned;
    std::map<std::string, DeviceInfo> seen;
    for (const auto& device : found) {
      Listed item;
      item.deviceId = device.deviceId; item.instanceId = device.instanceId;
      item.id = Utf8(device.deviceId + L"|" + device.instanceId);
      seen[item.id] = device;
      if (busy.count(item.id)) {
        const auto known = cache.find(item.id);
        if (known == cache.end()) continue;
        item.descriptors = known->second;
        item.busy = true;
      } else {
        item.descriptors.assign(device.descriptor, device.descriptor + 18);
        const unsigned count = device.descriptor[17];
        bool complete = count >= 1 && count <= 4;
        for (unsigned index = 0; complete && index < count; ++index) {
          std::vector<uint8_t> configuration;
          if (!api_->ConfigDescriptor(device, index, configuration) || configuration.size() < 9 || configuration.size() > 4096) complete = false;
          else item.descriptors.insert(item.descriptors.end(), configuration.begin(), configuration.end());
        }
        // A device whose configuration UsbDk would not give (a USB stick in Explorer,
        // absent from the panel) is reported, not dropped, so the panel and the
        // diagnostics can say so.
        if (!complete) { item.descriptors.assign(device.descriptor, device.descriptor + 18); item.unreadable = true; }
        else learned[item.id] = item.descriptors;
      }
      item.speed = device.speed == 1 ? "low" : device.speed == 2 ? "full" : "high";   // SuperSpeed devices are offered as high speed
      out.push_back(std::move(item));
    }
    {
      std::lock_guard lock(mutex_);
      if (descriptorCache_.size() > 64) descriptorCache_.clear();
      for (auto& [id, bytes] : learned) descriptorCache_[id] = std::move(bytes);
      for (auto& [id, device] : seen) known_[id] = device;
    }
    return out;
  }

  // Captures [id]. Returns the handle (> 0) or 0 with a fixed [code]; [nativeError]
  // gets UsbDk's Win32 error of a refused start (a number for the diagnostics).
  int64_t Capture(const std::string& id, std::string& code, DWORD* nativeError = nullptr) {
    std::vector<DeviceInfo> found;
    if (!api_->Devices(found)) { code = "usb_capture_failed"; return 0; }
    for (const auto& device : found) {
      if (Utf8(device.deviceId + L"|" + device.instanceId) != id) continue;
      {
        std::lock_guard lock(mutex_);
        for (const auto& [handle, capture] : captures_) if (capture->id == id) { code = "usb_device_busy"; return 0; }
      }
      DWORD error = 0;
      void* native = api_->Start(device, error);
      if (!native) {
        if (nativeError) *nativeError = error;
        // UsbDk refuses a device it already redirects (STATUS_OBJECT_NAME_COLLISION):
        // another window of this client or another program holds it.
        code = error == ERROR_ALREADY_EXISTS ? "usb_device_redirected"
            : error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION || error == ERROR_BUSY ? "usb_device_busy" : "usb_capture_failed";
        return 0;
      }
      auto capture = std::make_shared<Active>();
      capture->id = id; capture->native = native; capture->device = device;
      capture->configuration = ConfigurationValue(device);
      std::lock_guard lock(mutex_);
      const int64_t handle = ++nextHandle_;
      captures_[handle] = capture;
      known_[id] = device;
      return handle;
    }
    code = "usb_capture_failed";
    return 0;
  }

  // Runs [request] on [handle] and calls [done] (from a worker thread) once.
  void Transfer(int64_t handle, Request request, std::function<void(Result)> done) {
    std::shared_ptr<Active> capture;
    {
      std::lock_guard lock(mutex_);
      const auto found = captures_.find(handle);
      if (found != captures_.end()) capture = found->second;
    }
    if (!capture || capture->closing) { done({kNoDevice, {}}); return; }
    {
      std::lock_guard lock(capture->mutex);
      ++capture->active;
      const bool control = request.type == 0;
      capture->tokens[request.token] = Flight{control ? uint8_t(0) : request.endpoint, control, RequestClass(request), std::chrono::steady_clock::now()};
    }
    // A driver may ignore abort/Stop past the release deadline. The worker
    // owns the API (and its loaded DLL) and capture until its real completion.
    // It never dereferences Captures after being detached.
    std::thread([api = api_, completion = completion_, handle, capture, request = std::move(request), done = std::move(done)]() mutable {
      Result result = Run(*api, *capture, request);
      bool gone = false;
      {
        std::lock_guard lock(capture->mutex);
        const auto flight = capture->tokens.find(request.token);
        const auto elapsed = flight == capture->tokens.end() ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - flight->second.start).count();
        Tally(capture->stats, request, result, elapsed);
        capture->tokens.erase(request.token);
        capture->cancelled.erase(request.token);
        capture->timedOut.erase(request.token);
        if (result.status == kNoDevice && !capture->goneReported) gone = capture->goneReported = true;
        --capture->active;
      }
      capture->idle.notify_all();
      if (completion->open) done(std::move(result));
      if (gone && completion->open && completion->gone) completion->gone(handle);
    }).detach();
  }

  void Cancel(int64_t handle, int64_t token) {
    std::shared_ptr<Active> capture;
    {
      std::lock_guard lock(mutex_);
      const auto found = captures_.find(handle);
      if (found != captures_.end()) capture = found->second;
    }
    if (!capture) return;
    uint8_t address = 0;
    {
      std::lock_guard lock(capture->mutex);
      const auto found = capture->tokens.find(token);
      if (found == capture->tokens.end()) return;
      capture->cancelled.insert(token);
      address = found->second.abortAddress;
    }
    // UsbDk aborts a whole pipe: other requests on it end the same way. The one
    // pending I/O is cancelled as well, for a pipe whose abort does not reach it.
    AbortRequest(*capture, address, token);
  }

  // Gives the device back to Windows and waits (bounded) for requests to end.
  Stats Release(int64_t handle) {
    std::shared_ptr<Active> capture;
    {
      std::lock_guard lock(mutex_);
      const auto found = captures_.find(handle);
      if (found == captures_.end()) return {};
      capture = found->second;
      captures_.erase(found);
      releasing_.insert(capture->id);
    }
    Releasing done{*this, capture->id};
    Close(*capture);
    std::lock_guard lock(capture->mutex);
    return capture->stats;
  }

  void ReleaseAll() {
    std::vector<std::shared_ptr<Active>> all;
    {
      std::lock_guard lock(mutex_);
      for (auto& [handle, capture] : captures_) { all.push_back(capture); releasing_.insert(capture->id); }
      captures_.clear();
    }
    for (auto& capture : all) { Releasing done{*this, capture->id}; Close(*capture); }
  }
  size_t Count() const { std::lock_guard lock(mutex_); return captures_.size(); }

  // Does this engine hold [id]: captured, or given back but not yet let go by
  // UsbDk? Another window may take the device only once this is false.
  bool Holds(const std::string& id) const {
    std::lock_guard lock(mutex_);
    if (releasing_.count(id)) return true;
    for (const auto& [handle, capture] : captures_) if (capture->id == id) return true;
    return false;
  }

  // After Release(): did Windows take the device back? Polls (bounded) until it
  // is started again. 1 yes, 2 not within [milliseconds], 0 cannot be told;
  // for 2, [why] is the last thing missing (kBack*).
  int WaitReturned(const std::string& id, bool wantChild, unsigned milliseconds, uint32_t* why = nullptr) {
    if (why) *why = 0;
    DeviceInfo device;
    {
      std::lock_guard lock(mutex_);
      const auto found = known_.find(id);
      if (found == known_.end()) return 0;
      device = found->second;
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    for (;;) {
      uint32_t missing = 0;
      const int state = api_->Returned(device, wantChild, missing);
      if (state < 0) return 0;
      if (state > 0) return 1;
      if (std::chrono::steady_clock::now() >= end) { if (why) *why = missing; return 2; }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }

 private:
  struct Flight { uint8_t abortAddress = 0; bool control = false; uint32_t cls = 0; std::chrono::steady_clock::time_point start; };
  struct Active {
    std::string id; void* native = nullptr; DeviceInfo device; unsigned configuration = 1;
    std::mutex mutex; std::condition_variable idle; unsigned active = 0; std::atomic<bool> closing{false};
    std::map<int64_t, Flight> tokens;   // requests in flight
    std::set<int64_t> cancelled, timedOut;
    bool goneReported = false;
    Stats stats;
    std::mutex nativeMutex; bool stopped = false;   // Abort/Cancel never touch a handle that was stopped
  };
  struct Completion { std::atomic<bool> open{true}; Gone gone; };
  std::shared_ptr<Api> api_;
  std::shared_ptr<Completion> completion_;
  mutable std::mutex mutex_; std::map<int64_t, std::shared_ptr<Active>> captures_; int64_t nextHandle_ = 0;
  std::multiset<std::string> releasing_;   // given back, Stop not yet returned
  struct Releasing {
    Captures& self; std::string id;
    ~Releasing() { std::lock_guard lock(self.mutex_); const auto found = self.releasing_.find(id); if (found != self.releasing_.end()) self.releasing_.erase(found); }
  };
  std::map<std::string, std::vector<uint8_t>> descriptorCache_;
  std::map<std::string, DeviceInfo> known_;
  std::atomic<unsigned> controlTimeoutMs_{defaultControlTimeoutMs};
  std::atomic<unsigned> releaseWaitMs_{5000};
  std::thread watchdog_; std::mutex watchMutex_; std::condition_variable watchWake_; bool watchStop_ = false;

  static unsigned ConfigurationValue(const DeviceInfo&) { return 1; }

  static void Tally(Stats& stats, const Request& request, const Result& result, long long elapsedMs) {
    ++stats.urbs;
    stats.kib += uint32_t((request.data.size() + result.data.size()) / 1024);
    if (elapsedMs > slowMs && stats.slow++ == 0) stats.slowClass = RequestClass(request);
    if (result.status == kOk || result.status == kReset || result.status == kShutdown) return;
    if (result.status == kStall) ++stats.stalls;
    else if (result.status == kTimeout) ++stats.timeouts;
    else ++stats.failed;
    if (stats.firstStatus == 0) { stats.firstStatus = uint32_t(-result.status); stats.firstClass = RequestClass(request); stats.firstUsbd = result.usbd; }
  }

  void AbortRequest(Active& capture, uint8_t address, int64_t token) {
    std::lock_guard lock(capture.nativeMutex);
    if (capture.stopped) return;
    api_->Abort(capture.native, address);
    api_->CancelRequest(capture.native, token);
  }

  void Close(Active& capture) {
    capture.closing = true;
    std::vector<std::pair<uint8_t, int64_t>> flights;
    {
      std::lock_guard lock(capture.mutex);
      for (const auto& [token, flight] : capture.tokens) flights.emplace_back(flight.abortAddress, token);
    }
    for (const auto& [address, token] : flights) AbortRequest(capture, address, token);
    {
      std::unique_lock lock(capture.mutex);
      capture.idle.wait_for(lock, std::chrono::milliseconds(releaseWaitMs_.load()), [&] { return capture.active == 0; });
    }
    {
      std::lock_guard lock(capture.nativeMutex);
      capture.stopped = true;
      api_->Stop(capture.native);   // UsbDk returns the device to Windows
    }
  }

  // Control transfers older than the USB limit are aborted: the answer is a timeout.
  void Watch() {
    std::unique_lock lock(watchMutex_);
    while (!watchStop_) {
      watchWake_.wait_for(lock, std::chrono::milliseconds(100));
      if (watchStop_) break;
      lock.unlock();
      std::vector<std::shared_ptr<Active>> all;
      {
        std::lock_guard guard(mutex_);
        for (auto& [handle, capture] : captures_) all.push_back(capture);
      }
      const auto limit = std::chrono::milliseconds(controlTimeoutMs_.load());
      const auto now = std::chrono::steady_clock::now();
      for (auto& capture : all) {
        std::vector<int64_t> late;
        {
          std::lock_guard guard(capture->mutex);
          for (const auto& [token, flight] : capture->tokens)
            if (flight.control && now - flight.start > limit && !capture->timedOut.count(token) && !capture->cancelled.count(token)) late.push_back(token);
          for (const auto token : late) capture->timedOut.insert(token);
        }
        for (const auto token : late) AbortRequest(*capture, 0, token);
      }
      lock.lock();
    }
  }

  static bool Marked(Active& capture, int64_t token, bool timeout) {
    std::lock_guard lock(capture.mutex);
    return (timeout ? capture.timedOut : capture.cancelled).count(token) != 0;
  }

  static Result Run(Api& api, Active& capture, Request& request) {
    Result result;
    if (capture.closing) { result.status = kShutdown; return result; }
    const bool control = request.type == 0;
    if (control) {
      if (!request.hasSetup) { result.status = kProtocol; return result; }
      const uint8_t type = request.setup[0], code = request.setup[1];
      const unsigned value = unsigned(request.setup[2] | request.setup[3] << 8), index = unsigned(request.setup[4] | request.setup[5] << 8);
      // UsbDk has its own calls for what changes the device's configuration state.
      if (type == 0x01 && code == 0x0b) { result.status = api.SetAltSetting(capture.native, index & 0xff, value & 0xff) ? kOk : kStall; return result; }
      if (type == 0x00 && code == 0x09) { result.status = value == capture.configuration ? kOk : kStall; return result; }
      if (type == 0x02 && code == 0x01 && value == 0) { result.status = api.Reset(capture.native, uint8_t(index & 0xff)) ? kOk : kStall; return result; }
      if (type == 0x00 && code == 0x05) { result.status = kStall; return result; }    // SET_ADDRESS is never the VM's to do
    }
    std::vector<uint8_t> buffer;
    const bool input = control ? (request.setup[0] & 0x80) != 0 : (request.endpoint & 0x80) != 0;
    if (control) {
      buffer.assign(request.setup, request.setup + 8);
      if (input) buffer.resize(8 + request.length, 0); else buffer.insert(buffer.end(), request.data.begin(), request.data.end());
    } else if (input) buffer.assign(request.length, 0);
    else buffer = request.data;
    uint32_t usbd = 0; size_t actual = 0;
    const DWORD error = api.Transfer(capture.native, control ? uint8_t(0) : request.endpoint, request.type, buffer, usbd, actual, request.token);
    const bool cancelled = Marked(capture, request.token, false), timedOut = Marked(capture, request.token, true);
    bool gone = false;
    result.usbd = usbd;
    result.status = MapStatus(error, usbd, cancelled, control, &gone);
    if (timedOut && !cancelled && result.status != kOk) { result.status = kTimeout; return result; }   // our own abort of an unanswered request
    // An OUT request is delivered whole or reported as failed: a partial write would corrupt the stream.
    if (result.status == kOk && !input && actual != request.length) { result.status = kIo; return result; }
    if (result.status == kOk && input) {
      const size_t offset = control ? 8 : 0;
      if (actual > request.length || offset + actual > buffer.size()) { result.status = kOverflow; return result; }
      result.data.assign(buffer.begin() + long(offset), buffer.begin() + long(offset + actual));
    }
    return result;
  }
};
}  // namespace multitap::usb
