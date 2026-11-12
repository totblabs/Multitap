// usb-v1 capture on the client PC: listing, capture/busy, request translation
// (bulk, interrupt, control with the setup packet, SET_INTERFACE/CONFIGURATION/
// CLEAR_FEATURE as UsbDk calls), status mapping, cancellation, release and
// unplugging, over a substituted UsbDk. No driver, device or installation.
// usbdk_api.h is compiled too, so its data layout checks run; the real library
// is not loaded.
#include "usb_capture_core.h"
#include "usbdk_api.h"
#include <usb.h>
#include <array>
#include <iostream>
#include <optional>

using namespace multitap::usb;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
template <class F> bool waitFor(F condition, int milliseconds = 3000) {
  for (int waited = 0; waited < milliseconds; waited += 5) { if (condition()) return true; Sleep(5); }
  return condition();
}

struct Shared {
  std::mutex mutex;
  std::vector<std::string> log;
  std::vector<DeviceInfo> devices;
  std::map<std::wstring, std::vector<uint8_t>> configurations;
  DWORD startError = 0; bool refuseDevices = false;
  // The next Transfer: what it returns (or blocks until aborted).
  DWORD error = 0; uint32_t usbd = 0; size_t actual = 0; std::vector<uint8_t> response; bool block = false;
  std::atomic<bool> aborted{false}; std::atomic<int> blocking{0};
  std::vector<uint8_t> lastBuffer; uint8_t lastAddress = 0xee; int lastType = -1; unsigned altInterface = 99, altSetting = 99; uint8_t resetAddress = 0xee, abortAddress = 0xee;
  bool altResult = true, resetResult = true;
  std::atomic<int> stops{0}, transfers{0}, configCalls{0};
  // A pipe abort that does not reach a request (a control transfer UsbDk does not find by address), and the cancel of one I/O.
  std::atomic<bool> ignoreAbort{false}, ignoreCancel{false}, forceReturn{false}, holdConfig{false}; std::atomic<int64_t> cancelledToken{-1}; int64_t lastToken = -1;
  std::atomic<int> apiDestroyed{0};
  std::vector<int> returnedAnswers; size_t returnedAt = 0; uint32_t returnWhy = 0;
  void note(const std::string& text) { std::lock_guard lock(mutex); log.push_back(text); }
  bool has(const std::string& text) { std::lock_guard lock(mutex); return std::find(log.begin(), log.end(), text) != log.end(); }
};
class FakeApi final : public Api {
 public:
  explicit FakeApi(std::shared_ptr<Shared> shared) : s(std::move(shared)) {}
  ~FakeApi() override { ++s->apiDestroyed; }
  std::shared_ptr<Shared> s;
  bool Devices(std::vector<DeviceInfo>& out) override { if (s->refuseDevices) return false; out = s->devices; return true; }
  bool ConfigDescriptor(const DeviceInfo& device, unsigned index, std::vector<uint8_t>& out) override {
    ++s->configCalls;
    while (s->holdConfig) Sleep(2);
    const auto found = s->configurations.find(device.deviceId + L"#" + std::to_wstring(index));
    if (found == s->configurations.end()) return false;
    out = found->second; return true;
  }
  void* Start(const DeviceInfo& device, DWORD& error) override { s->note("start"); error = s->startError; (void)device; return s->startError ? nullptr : reinterpret_cast<void*>(0x1234); }
  void Stop(void*) override { ++s->stops; s->note("stop"); }
  DWORD Transfer(void*, uint8_t address, int type, std::vector<uint8_t>& buffer, uint32_t& usbd, size_t& actual, int64_t token) override {
    ++s->transfers;
    { std::lock_guard lock(s->mutex); s->lastBuffer = buffer; s->lastAddress = address; s->lastType = type; s->lastToken = token; }
    if (s->block) {
      ++s->blocking;
      while (!s->forceReturn && !(s->aborted && !s->ignoreAbort) && (s->ignoreCancel || s->cancelledToken != token)) Sleep(2);
      --s->blocking;
      if (s->forceReturn) return ERROR_DEVICE_NOT_CONNECTED;
      return ERROR_OPERATION_ABORTED;
    }
    if (!s->response.empty()) { const size_t offset = type == 0 ? 8 : 0; for (size_t i = 0; i < s->response.size() && offset + i < buffer.size(); ++i) buffer[offset + i] = s->response[i]; }
    usbd = s->usbd; actual = s->actual;
    return s->error;
  }
  bool Abort(void*, uint8_t address) override { s->abortAddress = address; s->aborted = true; s->note("abort"); return true; }
  bool CancelRequest(void*, int64_t token) override { s->cancelledToken = token; s->note("cancel-io"); return true; }
  int Returned(const DeviceInfo&, bool, uint32_t& why) override { std::lock_guard lock(s->mutex); why = s->returnWhy; if (s->returnedAnswers.empty()) return -1; const int answer = s->returnedAnswers[std::min(s->returnedAt, s->returnedAnswers.size() - 1)]; ++s->returnedAt; return answer; }
  bool Reset(void*, uint8_t address) override { s->resetAddress = address; s->note("reset"); return s->resetResult; }
  bool SetAltSetting(void*, unsigned interfaceIndex, unsigned alternate) override { s->altInterface = interfaceIndex; s->altSetting = alternate; s->note("alt"); return s->altResult; }
};

std::vector<uint8_t> config(uint8_t value) { return {9, 2, 18, 0, 1, value, 0, 0x80, 50, 9, 4, 0, 0, 0, 8, 6, 0x50, 0}; }
DeviceInfo stick(const std::wstring& id = L"USB\\VID_0781&PID_5567") {
  DeviceInfo device; device.deviceId = id; device.instanceId = L"A1B2"; device.speed = 3;
  const uint8_t descriptor[18] = {18, 1, 0, 2, 0, 0, 0, 64, 0x81, 0x07, 0x67, 0x55, 0, 1, 0, 0, 0, 1};
  std::memcpy(device.descriptor, descriptor, 18);
  return device;
}
struct Rig {
  std::shared_ptr<Shared> shared = std::make_shared<Shared>();
  std::unique_ptr<Captures> captures;
  std::mutex mutex; std::vector<int64_t> gone;
  Rig() {
    shared->devices = {stick()};
    shared->configurations[L"USB\\VID_0781&PID_5567#0"] = config(1);
    captures = std::make_unique<Captures>(std::make_unique<FakeApi>(shared), [this](int64_t handle) { std::lock_guard lock(mutex); gone.push_back(handle); });
  }
  int64_t capture() { std::string code; const auto handle = captures->Capture(captures->List().at(0).id, code); require(handle > 0, "capture failed"); return handle; }
  // One request, waiting for its answer.
  Result run(int64_t handle, Request request) {
    std::mutex lock; std::condition_variable done; std::optional<Result> result;
    captures->Transfer(handle, std::move(request), [&](Result value) { std::lock_guard guard(lock); result = std::move(value); done.notify_all(); });
    std::unique_lock guard(lock);
    require(done.wait_for(guard, std::chrono::seconds(5), [&] { return result.has_value(); }), "no answer");
    return *result;
  }
};
Request bulk(int64_t token, uint8_t endpoint, size_t length, std::vector<uint8_t> data = {}) { Request request; request.token = token; request.endpoint = endpoint; request.type = 2; request.length = length; request.data = std::move(data); return request; }
Request control(int64_t token, std::array<uint8_t, 8> setup, size_t length, std::vector<uint8_t> data = {}) {
  Request request; request.token = token; request.endpoint = setup[0] & 0x80; request.type = 0; request.length = length; request.hasSetup = true; request.data = std::move(data);
  std::memcpy(request.setup, setup.data(), 8); return request;
}

void listing() {
  Rig rig;
  auto list = rig.captures->List();
  require(list.size() == 1 && list[0].id == "USB\\VID_0781&PID_5567|A1B2" && list[0].speed == "high" && !list[0].busy, "The device was not listed");
  require(list[0].descriptors.size() == 18 + 18 && list[0].descriptors[0] == 18 && list[0].descriptors[18] == 9 && list[0].descriptors[19] == 2, "The descriptors are not the device descriptor and the configuration");
  rig.shared->devices[0].speed = 1; require(rig.captures->List()[0].speed == "low", "Low speed");
  rig.shared->devices[0].speed = 2; require(rig.captures->List()[0].speed == "full", "Full speed");
  rig.shared->devices[0].speed = 4; require(rig.captures->List()[0].speed == "high", "SuperSpeed devices are offered as high speed");
  rig.shared->devices[0].speed = 3;
  // A device whose configuration cannot be read is reported as unreadable with its
  // device descriptor only (2026-11-12: it used to be dropped without a trace).
  rig.shared->devices.push_back(stick(L"USB\\VID_1234&PID_0001"));
  { const auto both = rig.captures->List();
    require(both.size() == 2 && !both[0].unreadable && both[1].unreadable && both[1].descriptors.size() == 18, "An unreadable configuration was dropped or offered"); }
  rig.shared->refuseDevices = true; require(rig.captures->List().empty(), "A failing enumeration listed devices");
  rig.shared->refuseDevices = false;
  const auto handle = rig.capture();
  require(rig.captures->List()[0].busy && rig.captures->Count() == 1, "A captured device is not marked");
  rig.captures->Release(handle);
  require(!rig.captures->List()[0].busy && rig.shared->stops == 1 && rig.captures->Count() == 0, "Release did not give the device back");
  rig.captures->Release(handle);
  require(rig.shared->stops == 1, "A second release stopped the capture again");
}

void capturing() {
  Rig rig; std::string code;
  const std::string id = rig.captures->List()[0].id;
  require(rig.captures->Capture("USB\\VID_9999&PID_9999|X", code) == 0 && code == "usb_capture_failed" && !rig.shared->has("start"), "An unknown device was captured");
  rig.shared->startError = ERROR_ACCESS_DENIED;
  require(rig.captures->Capture(id, code) == 0 && code == "usb_device_busy", "An access-denied start was not reported busy");
  rig.shared->startError = ERROR_SHARING_VIOLATION;
  require(rig.captures->Capture(id, code) == 0 && code == "usb_device_busy", "A sharing violation was not reported busy");
  rig.shared->startError = ERROR_GEN_FAILURE;
  require(rig.captures->Capture(id, code) == 0 && code == "usb_capture_failed", "Another failure was not a capture failure");
  rig.shared->startError = 0;
  const auto handle = rig.captures->Capture(id, code);
  require(handle > 0, "capture");
  require(rig.captures->Capture(id, code) == 0 && code == "usb_device_busy", "A device was captured twice");
  rig.captures->Release(handle);
  require(rig.captures->Capture(id, code) > 0, "A released device could not be captured again");
  rig.shared->refuseDevices = true;
  require(rig.captures->Capture(id, code) == 0 && code == "usb_capture_failed", "A failing enumeration captured");
}

// Field 2026-11-02: a stick shared in one remote window failed in a second one with a generic error. Another window may
// take a device only once this engine let it go: Holds() lasts until UsbDk's Stop returned, not only while it is listed
// as captured. UsbDk's "already redirected" start has its own code.
void heldUntilUsbDkLetsGo() {
  Rig rig; std::string code;
  const std::string id = rig.captures->List().at(0).id;
  require(!rig.captures->Holds(id), "A device nobody captured was held");
  rig.shared->startError = ERROR_ALREADY_EXISTS; DWORD native = 0;
  require(rig.captures->Capture(id, code, &native) == 0 && code == "usb_device_redirected" && native == ERROR_ALREADY_EXISTS && !rig.captures->Holds(id),
      "A device UsbDk already redirects was not told apart");
  rig.shared->startError = 0;
  const auto handle = rig.captures->Capture(id, code);
  require(handle > 0 && rig.captures->Holds(id), "A captured device was not held");
  // A request the device does not let go of keeps the release waiting.
  auto& s = *rig.shared;
  s.block = true; s.ignoreAbort = true; s.ignoreCancel = true;
  rig.captures->SetReleaseWait(400);
  std::atomic<bool> ended{false}, released{false};
  rig.captures->Transfer(handle, bulk(81, 0x81, 512), [&](Result) { ended = true; });
  require(waitFor([&] { return s.blocking == 1; }), "The request never started");
  std::thread releaser([&] { rig.captures->Release(handle); released = true; });
  const bool releasing = waitFor([&] { return rig.captures->Count() == 0; });
  const bool heldMeanwhile = releasing && !released && s.stops == 0 && rig.captures->Holds(id);
  releaser.join();
  require(releasing, "The release never started");
  require(heldMeanwhile, "A device being given back looked free before UsbDk let it go");
  require(s.stops == 1 && !rig.captures->Holds(id), "A device UsbDk let go of still looked held");
  s.forceReturn = true;
  require(waitFor([&] { return ended.load(); }), "The blocked request never ended");
}

void requests() {
  Rig rig; const auto handle = rig.capture();
  auto& s = *rig.shared;
  // Bulk OUT: the data goes out whole.
  s.actual = 4; auto result = rig.run(handle, bulk(1, 0x02, 4, {1, 2, 3, 4}));
  require(result.status == kOk && result.data.empty() && s.lastAddress == 0x02 && s.lastType == 2 && s.lastBuffer == std::vector<uint8_t>({1, 2, 3, 4}), "A bulk OUT was wrong");
  s.actual = 3; result = rig.run(handle, bulk(2, 0x02, 4, {1, 2, 3, 4}));
  require(result.status == kIo, "A partial OUT was reported as done");
  // Bulk IN: the bytes the device sent.
  s.actual = 3; s.response = {7, 8, 9}; result = rig.run(handle, bulk(3, 0x81, 64));
  require(result.status == kOk && result.data == std::vector<uint8_t>({7, 8, 9}) && s.lastBuffer.size() == 64 && s.lastAddress == 0x81, "A short IN was wrong");
  s.actual = 65; result = rig.run(handle, bulk(4, 0x81, 64));
  require(result.status == kOverflow && result.data.empty(), "More than asked was relayed");
  // Interrupt.
  s.actual = 1; s.response = {5}; auto interrupt = bulk(5, 0x83, 8); interrupt.type = 3;
  result = rig.run(handle, interrupt);
  require(result.status == kOk && s.lastType == 3 && result.data == std::vector<uint8_t>({5}), "An interrupt IN was wrong");
  // Control IN: setup first, room for the data, endpoint 0; the answer skips the setup.
  s.actual = 2; s.response = {0xaa, 0xbb};
  result = rig.run(handle, control(6, {0x80, 0, 0, 0, 0, 0, 2, 0}, 2));
  require(result.status == kOk && result.data == std::vector<uint8_t>({0xaa, 0xbb}) && s.lastType == 0 && s.lastAddress == 0 && s.lastBuffer.size() == 10 && s.lastBuffer[0] == 0x80, "A control IN was wrong");
  // Control OUT (a class request) carries its data after the setup.
  s.actual = 3;
  result = rig.run(handle, control(7, {0x21, 9, 0, 2, 0, 0, 3, 0}, 3, {1, 2, 3}));
  require(result.status == kOk && s.lastBuffer == std::vector<uint8_t>({0x21, 9, 0, 2, 0, 0, 3, 0, 1, 2, 3}), "A control OUT was wrong");
  // A control request without its setup packet is refused.
  Request bad = control(8, {0x80, 0, 0, 0, 0, 0, 0, 0}, 0); bad.hasSetup = false;
  require(rig.run(handle, bad).status == kProtocol, "A control request without a setup packet ran");
  // Requests on a handle that is not captured.
  require(rig.run(handle + 1, bulk(9, 0x81, 8)).status == kNoDevice, "A request on an unknown handle ran");
}

void translated() {
  Rig rig; const auto handle = rig.capture();
  auto& s = *rig.shared; const int before = s.transfers;
  auto result = rig.run(handle, control(1, {0x01, 0x0b, 2, 0, 1, 0, 0, 0}, 0));
  require(result.status == kOk && s.altInterface == 1 && s.altSetting == 2, "SET_INTERFACE was not a UsbDk call");
  s.altResult = false; require(rig.run(handle, control(2, {0x01, 0x0b, 0, 0, 0, 0, 0, 0}, 0)).status == kStall, "A refused alternate setting was not a stall");
  require(rig.run(handle, control(3, {0x00, 0x09, 1, 0, 0, 0, 0, 0}, 0)).status == kOk, "SET_CONFIGURATION to the current configuration failed");
  require(rig.run(handle, control(4, {0x00, 0x09, 2, 0, 0, 0, 0, 0}, 0)).status == kStall, "SET_CONFIGURATION to another configuration was allowed");
  result = rig.run(handle, control(5, {0x02, 0x01, 0, 0, 0x81, 0, 0, 0}, 0));
  require(result.status == kOk && s.resetAddress == 0x81, "CLEAR_FEATURE(ENDPOINT_HALT) did not reset the pipe");
  s.resetResult = false; require(rig.run(handle, control(6, {0x02, 0x01, 0, 0, 0x02, 0, 0, 0}, 0)).status == kStall, "A refused pipe reset was not a stall");
  require(rig.run(handle, control(7, {0x00, 0x05, 9, 0, 0, 0, 0, 0}, 0)).status == kStall, "SET_ADDRESS was passed to the device");
  require(s.transfers == before, "A translated request reached the device as a transfer");
  // CLEAR_FEATURE for something else is an ordinary control transfer.
  s.resetResult = true; s.actual = 0;
  require(rig.run(handle, control(8, {0x00, 0x01, 1, 0, 0, 0, 0, 0}, 0)).status == kOk && s.transfers == before + 1, "Another CLEAR_FEATURE was not forwarded");
}

void statuses() {
  // The constants are the SDK's own values (they were not: 0x80000400 is ERROR_BUSY, 0x80000300 INVALID_PARAMETER).
  require(kUsbdStall == uint32_t(USBD_STATUS_STALL_PID) && kUsbdHalted == uint32_t(USBD_STATUS_ENDPOINT_HALTED) && kUsbdOverrun == uint32_t(USBD_STATUS_DATA_OVERRUN) &&
      kUsbdBufferOverrun == uint32_t(USBD_STATUS_BUFFER_OVERRUN) && kUsbdBabble == uint32_t(USBD_STATUS_BABBLE_DETECTED) && kUsbdCanceled == uint32_t(USBD_STATUS_CANCELED) &&
      kUsbdInternalHc == uint32_t(USBD_STATUS_INTERNAL_HC_ERROR) && kUsbdTimeout == uint32_t(USBD_STATUS_TIMEOUT) && kUsbdNoResponse == uint32_t(USBD_STATUS_DEV_NOT_RESPONDING) &&
      kUsbdDeviceGone == uint32_t(USBD_STATUS_DEVICE_GONE) && kUsbdInvalidParameter == uint32_t(USBD_STATUS_INVALID_PARAMETER), "A USBD constant is not the SDK's value");
  // Field 2026-11-02 (error 71, usbReq 0x8006): UsbDk completes a STALL on the control pipe as success with
  // INTERNAL_HC_ERROR. A GET_DESCRIPTOR the stick does not support must reach the VM as a stall, not -EPROTO.
  require(MapStatus(0, uint32_t(USBD_STATUS_INTERNAL_HC_ERROR), false, true) == kStall, "UsbDk's control-pipe stall was not a stall");
  require(MapStatus(0, uint32_t(USBD_STATUS_ERROR_BUSY), false, true) == kProtocol, "ERROR_BUSY was taken for a stall");
  require(MapStatus(0, uint32_t(USBD_STATUS_TIMEOUT), false, false) == kTimeout, "A USBD timeout was not a timeout");
  require(MapStatus(0, uint32_t(USBD_STATUS_INVALID_PARAMETER), false, false) == kIo, "INVALID_PARAMETER was not an I/O error");
  require(MapStatus(0, uint32_t(USBD_STATUS_DEVICE_GONE), false, false) == kNoDevice, "A device that is gone was not reported");
  require(MapStatus(0, 0, false, false) == kOk && MapStatus(0, 0x00000000u, false, false) == kOk, "Success");
  require(MapStatus(0, kUsbdStall, false, false) == kStall && MapStatus(0, kUsbdHalted, false, false) == kStall, "A stall");
  require(MapStatus(0, kUsbdInternalHc, false, true) == kStall && MapStatus(0, kUsbdInternalHc, false, false) == kProtocol, "A control stall and a host controller error");
  require(MapStatus(0, kUsbdOverrun, false, false) == kOverflow && MapStatus(0, kUsbdBabble, false, false) == kOverflow, "An overrun");
  require(MapStatus(0, kUsbdTimeout, false, false) == kTimeout && MapStatus(ERROR_SEM_TIMEOUT, 0, false, false) == kTimeout, "A timeout");
  require(MapStatus(0, 0xC0000099u, false, false) == kProtocol, "An unknown USBD error");
  require(MapStatus(0, kUsbdStall, true, false) == kReset && MapStatus(ERROR_OPERATION_ABORTED, 0, true, false) == kReset, "A cancelled request");
  require(MapStatus(ERROR_OPERATION_ABORTED, 0, false, false) == kShutdown, "An aborted request nobody cancelled");
  bool gone = false;
  require(MapStatus(ERROR_DEVICE_NOT_CONNECTED, 0, false, false, &gone) == kNoDevice && gone, "An unplugged device");
  require(MapStatus(ERROR_GEN_FAILURE, 0, false, false, &gone) == kStall && !gone, "ERROR_GEN_FAILURE (how UsbDk clients see a stalled endpoint) was not a stall");
  require(MapStatus(ERROR_GEN_FAILURE, kUsbdOverrun, false, false) == kOverflow && MapStatus(ERROR_GEN_FAILURE, kUsbdStall, false, true) == kStall, "The driver\u0027s own USBD status did not win over the Windows error");
  require(MapStatus(ERROR_INVALID_PARAMETER, 0, false, false) == kIo && MapStatus(ERROR_INVALID_PARAMETER, kUsbdStall, false, false) == kStall, "Another Windows error");
  Rig rig; const auto handle = rig.capture();
  rig.shared->usbd = kUsbdStall;
  require(rig.run(handle, bulk(1, 0x81, 8)).status == kStall, "A stall did not reach the answer");
}

void cancelling() {
  Rig rig; const auto handle = rig.capture();
  auto& s = *rig.shared;
  s.block = true;
  std::mutex lock; std::optional<Result> result;
  rig.captures->Transfer(handle, bulk(11, 0x81, 512), [&](Result value) { std::lock_guard guard(lock); result = std::move(value); });
  require(waitFor([&] { return s.blocking == 1; }), "The request never reached the device");
  rig.captures->Cancel(handle, 999);                     // not one of ours
  Sleep(30); require(!s.aborted, "A foreign token aborted a pipe");
  rig.captures->Cancel(handle, 11);
  require(waitFor([&] { std::lock_guard guard(lock); return result.has_value(); }), "A cancelled request never answered");
  { std::lock_guard guard(lock); require(result->status == kReset && result->data.empty() && s.abortAddress == 0x81, "A cancelled request did not answer -ECONNRESET on its pipe"); }
  // Release aborts what is in flight, waits, then returns the device.
  s.aborted = false; std::optional<Result> second;
  rig.captures->Transfer(handle, bulk(12, 0x02, 4, {1, 2, 3, 4}), [&](Result value) { std::lock_guard guard(lock); second = std::move(value); });
  require(waitFor([&] { return s.blocking == 1; }), "The second request never reached the device");
  rig.captures->Release(handle);
  require(s.stops == 1 && s.blocking == 0, "Release left a request running or the device captured");
  require(waitFor([&] { std::lock_guard guard(lock); return second.has_value(); }), "A request in flight at release never answered");
  { std::lock_guard guard(lock); require(second->status == kShutdown, "A request interrupted by release was not a shutdown"); }
  require(rig.run(handle, bulk(13, 0x81, 8)).status == kNoDevice, "A released handle served a request");
}

void unplugging() {
  Rig rig; const auto handle = rig.capture();
  rig.shared->error = ERROR_DEVICE_NOT_CONNECTED;
  const auto result = rig.run(handle, bulk(1, 0x81, 8));
  require(result.status == kNoDevice, "An unplugged device was not reported");
  require(waitFor([&] { std::lock_guard lock(rig.mutex); return rig.gone.size() == 1 && rig.gone[0] == handle; }), "The unplugging was not reported once");
  rig.run(handle, bulk(2, 0x81, 8));
  Sleep(50);
  { std::lock_guard lock(rig.mutex); require(rig.gone.size() == 1, "The unplugging was reported twice"); }
}

void everythingGivenBack() {
  auto shared = std::make_shared<Shared>();
  {
    Rig rig; shared = rig.shared;
    rig.shared->devices.push_back(stick(L"USB\\VID_0002&PID_0002"));
    rig.shared->configurations[L"USB\\VID_0002&PID_0002#0"] = config(1);
    std::string code;
    for (const auto& item : rig.captures->List()) require(rig.captures->Capture(item.id, code) > 0, "capture");
    require(rig.captures->Count() == 2, "Two devices");
  }
  require(shared->stops == 2, "Destroying the captures left a device captured");
}

// The Windows-side half of the field failure (2026-10-27): a VM whose USBSTOR waited for ever. Every case here is a way a request on the PC
// could be left pending, or answered in a way that makes the VM wait.
void controlCancelAndTimeout() {
  Rig rig; const auto handle = rig.capture();
  auto& s = *rig.shared;
  // A control IN (GET_MAX_LUN) the device never answers, and a pipe abort that does not find it: the VM's cancel
  // must still end it. The direction bit of the setup is not an endpoint address: endpoint 0 is aborted.
  s.block = true; s.ignoreAbort = true;
  std::mutex lock; std::optional<Result> result;
  rig.captures->Transfer(handle, control(21, {0xa1, 0xfe, 0, 0, 0, 0, 1, 0}, 1), [&](Result value) { std::lock_guard guard(lock); result = std::move(value); });
  require(waitFor([&] { return s.blocking == 1; }), "The control request never reached the device");
  rig.captures->Cancel(handle, 21);
  require(s.abortAddress == 0, "A control transfer was aborted by its setup direction bit instead of endpoint 0");
  require(waitFor([&] { std::lock_guard guard(lock); return result.has_value(); }), "A cancelled control request whose pipe abort did not reach it never ended");
  { std::lock_guard guard(lock); require(result->status == kReset && s.has("cancel-io"), "The pending I/O was not cancelled"); }
  // The USB limit: a control transfer the device does not answer within the limit is aborted and answers a timeout.
  s.aborted = false; s.ignoreAbort = false; s.cancelledToken = -1; result.reset();
  rig.captures->SetControlTimeout(150);
  rig.captures->Transfer(handle, control(22, {0xa1, 0xfe, 0, 0, 0, 0, 1, 0}, 1), [&](Result value) { std::lock_guard guard(lock); result = std::move(value); });
  require(waitFor([&] { std::lock_guard guard(lock); return result.has_value(); }, 3000), "An unanswered control request was never ended");
  { std::lock_guard guard(lock); require(result->status == kTimeout && result->data.empty(), "An unanswered control request did not answer a timeout"); }
  // A bulk request is not timed out here (a drive may take long for one command; the VM cancels it itself).
  s.aborted = false; s.cancelledToken = -1; result.reset();
  rig.captures->Transfer(handle, bulk(23, 0x81, 512), [&](Result value) { std::lock_guard guard(lock); result = std::move(value); });
  Sleep(500);
  { std::lock_guard guard(lock); require(!result.has_value(), "A bulk request was ended by the control timeout"); }
  rig.captures->Cancel(handle, 23);
  require(waitFor([&] { std::lock_guard guard(lock); return result.has_value(); }) && result->status == kReset, "A cancelled bulk request did not end");
  s.block = false;
  const auto stats = rig.captures->Release(handle);
  require(stats.timeouts == 1 && stats.urbs == 3, "The counters missed a timeout");
}

void listingNeverBlocksTheVm() {
  Rig rig; const auto handle = rig.capture();
  auto& s = *rig.shared;
  // Another device is slow to describe itself: listing waits for it, but a request, a cancel and a release must not
  // wait for the listing (Transfer runs on the UI thread).
  s.devices.push_back(stick(L"USB\\VID_0002&PID_0002")); s.configurations[L"USB\\VID_0002&PID_0002#0"] = config(1);
  s.holdConfig = true;
  std::atomic<bool> listed{false};
  std::thread lister([&] { rig.captures->List(); listed = true; });
  require(waitFor([&] { return s.configCalls >= 2; }), "The listing never asked the second device");
  s.actual = 2; s.response = {1, 2};
  const auto started = std::chrono::steady_clock::now();
  const auto answer = rig.run(handle, bulk(1, 0x81, 8));
  rig.captures->Cancel(handle, 12345);
  rig.captures->Count();
  require(answer.status == kOk && !listed && std::chrono::steady_clock::now() - started < std::chrono::seconds(2), "A request waited for the device listing");
  s.holdConfig = false; lister.join();
  // A captured device is not asked again: its descriptors are the ones read before.
  const int before = s.configCalls;
  const auto list = rig.captures->List();
  require(list.size() == 2 && s.configCalls == before + 1, "A captured device was asked for its descriptors again");
  bool found = false; for (const auto& item : list) if (item.busy) { found = true; require(item.descriptors.size() == 36, "A captured device lost its descriptors in the list"); }
  require(found, "A captured device was not marked busy");
}

void countersAndReturn() {
  Rig rig; const auto handle = rig.capture();
  auto& s = *rig.shared;
  s.actual = 1; s.response = {0}; require(rig.run(handle, control(1, {0xa1, 0xfe, 0, 0, 0, 0, 1, 0}, 1)).status == kOk, "GET_MAX_LUN");
  s.error = ERROR_GEN_FAILURE; s.actual = 0; s.response.clear();
  require(rig.run(handle, control(2, {0xa1, 0xfe, 0, 0, 0, 0, 1, 0}, 1)).status == kStall, "A stalled GET_MAX_LUN was not a stall");
  s.error = 0; s.usbd = kUsbdStall;
  require(rig.run(handle, bulk(3, 0x81, 13)).status == kStall, "A stalled CSW was not a stall");
  const auto stats = rig.captures->Release(handle);
  require(stats.urbs == 3 && stats.stalls == 2 && stats.failed == 0 && stats.firstClass == 0xa1fe && stats.firstStatus == 32 && stats.timeouts == 0, "The counters or the first non-OK request are wrong");
  // Did Windows take the device back?
  const std::string id = rig.captures->List().at(0).id;
  s.returnedAnswers = {0, 0, 1};
  require(rig.captures->WaitReturned(id, true, 5000) == 1 && s.returnedAt == 3, "A device that came back after a moment was not seen");
  s.returnedAnswers = {0}; s.returnedAt = 0;
  require(rig.captures->WaitReturned(id, true, 300) == 2, "A device that never came back was reported as back");
  // What was missing the last time is reported with a 2 (Code 43 here), and nothing with a 1.
  s.returnWhy = kBackProblemBase + 43; uint32_t why = 0;
  require(rig.captures->WaitReturned(id, true, 300, &why) == 2 && why == 143, "Why a device is not back was lost");
  s.returnedAnswers = {1}; s.returnedAt = 0;
  require(rig.captures->WaitReturned(id, true, 300, &why) == 1 && why == 0, "A device that is back carried a reason");
  s.returnedAnswers = {0}; s.returnWhy = 0;
  s.returnedAnswers.clear();
  require(rig.captures->WaitReturned(id, true, 300) == 0 && rig.captures->WaitReturned("USB\\VID_9999&PID_9999|X", false, 300) == 0, "An unknowable state was claimed");
}

// The field summary of 2026-11-02 (stage 9: error 71, usbReq 0x8006, usbStalls 0): an unsupported GET_DESCRIPTOR
// that UsbDk reports its way is a stall, and the first real failure keeps its raw USBD status for the diagnostics.
void fieldStallAndRawStatus() {
  Rig rig; const auto handle = rig.capture();
  auto& s = *rig.shared;
  s.usbd = uint32_t(USBD_STATUS_INTERNAL_HC_ERROR); s.actual = 0;
  require(rig.run(handle, control(1, {0x80, 0x06, 0xee, 0x03, 0, 0, 0x12, 0}, 0x12)).status == kStall, "An unsupported GET_DESCRIPTOR was not a stall");
  s.usbd = uint32_t(USBD_STATUS_INVALID_PARAMETER);
  require(rig.run(handle, bulk(2, 0x02, 4, {1, 2, 3, 4})).status == kIo, "A refused bulk OUT was not an I/O error");
  const auto stats = rig.captures->Release(handle);
  require(stats.stalls == 1 && stats.failed == 1 && stats.timeouts == 0, "The field counters are wrong");
  require(stats.firstStatus == 32 && stats.firstClass == 0x8006 && stats.firstUsbd == uint32_t(USBD_STATUS_INTERNAL_HC_ERROR), "The first failure lost its raw USBD status");
}

void pendingRequestsBelongToTheirCapture() {
  UsbPendingIo pending;
  auto* first = reinterpret_cast<void*>(uintptr_t(0x1234));
  auto* second = reinterpret_cast<void*>(uintptr_t(0x5678));
  OVERLAPPED a{}, b{}, replacement{};
  require(pending.Add(first, 7, &a) && pending.Add(second, 7, &b), "Two captures could not reuse a relay token");
  require(!pending.Add(first, 7, &replacement), "A duplicate request replaced live pending I/O");
  require(pending.Cancel(first, 7, [&](OVERLAPPED* value) { return value == &a; }), "Cancellation used another capture's I/O");
  require(pending.Cancel(second, 7, [&](OVERLAPPED* value) { return value == &b; }), "Second capture lost its I/O");
  pending.Remove(first, 7, &a);
  require(!pending.Cancel(first, 7, [](OVERLAPPED*) { return true; }), "Completed I/O stayed cancellable");
  require(pending.Cancel(second, 7, [&](OVERLAPPED* value) { return value == &b; }), "Removing one capture erased another capture's same token");
  pending.Remove(second, 7, &replacement);
  require(pending.Cancel(second, 7, [&](OVERLAPPED* value) { return value == &b; }), "A stale registration erased different I/O");
  pending.Remove(second, 7, &b);
  require(!pending.Cancel(second, 7, [](OVERLAPPED*) { return true; }), "A removed request was cancelled");
}

void lateCompletionDoesNotUseTheDestroyedEngine() {
  Rig rig;
  const auto shared = rig.shared;
  const auto handle = rig.capture();
  shared->block = true; shared->ignoreAbort = true; shared->ignoreCancel = true;
  std::atomic<unsigned> callbacks{0};
  rig.captures->SetReleaseWait(10);
  rig.captures->Transfer(handle, bulk(61, 0x81, 512), [&](Result) { ++callbacks; });
  require(waitFor([&] { return shared->blocking == 1; }), "The delayed request never started");
  rig.captures.reset();
  require(shared->stops == 1, "Shutdown did not ask UsbDk to return the device");
  require(shared->apiDestroyed == 0, "Shutdown unloaded the API while the driver was still inside it");
  require(callbacks == 0, "An unfinished request completed during bounded shutdown");
  shared->forceReturn = true;
  require(waitFor([&] { return shared->apiDestroyed == 1; }), "The late request did not release its API ownership");
  require(callbacks == 0 && rig.gone.empty(), "A late result called the destroyed engine's owner");
}

void restartedRedirectorRejectsOldIdentity() {
  UsbRedirectorIdentities identities;
  UsbPendingIo pending;
  const HANDLE reusedNative = reinterpret_cast<HANDLE>(uintptr_t(0x1234));
  void* old = identities.Add(reusedNative);
  require(old && identities.Find(old) == reusedNative, "The first redirector identity did not resolve");
  OVERLAPPED first{}, second{};
  require(pending.Add(old, 7, &first), "The original capture request was not registered");
  unsigned submissions = 0, cancellations = 0;
  const auto submit = [&](void* identity) {
    const HANDLE native = identities.Find(identity);
    if (!native) return false;
    require(native == reusedNative, "Submission resolved the wrong native redirector");
    ++submissions; return true;
  };
  // The worker has retained [old] but has not entered UsbDk's submission lock
  // yet. Stop its capture and restart a device at the identical native address.
  const auto lateSubmit = [&] { return submit(old); };
  require(identities.Remove(old) == reusedNative, "Stopping the original identity lost its native handle");
  void* replacement = identities.Add(reusedNative);
  require(replacement && replacement != old, "A restarted redirector reused its lifetime identity");
  require(!lateSubmit() && submissions == 0, "A late request targeted a restarted redirector");
  require(submit(replacement) && submissions == 1, "The replacement capture could not submit");
  require(pending.Add(replacement, 7, &second), "A replacement capture collided with the old pending token");
  const auto cancel = [&](void* identity) {
    if (!identities.Find(identity)) return false;
    return pending.Cancel(identity, 7, [&](OVERLAPPED* value) { ++cancellations; return value == &second; });
  };
  require(!cancel(old) && cancellations == 0, "An old cancellation reached a restarted capture");
  require(cancel(replacement) && cancellations == 1, "The replacement request could not be cancelled");
  pending.Remove(old, 7, &first);
  require(pending.Cancel(replacement, 7, [&](OVERLAPPED* value) { return value == &second; }), "Old completion erased replacement I/O");
  require(identities.Remove(old) == nullptr && identities.Find(replacement) == reusedNative, "Old Stop removed the replacement capture");

  UsbRedirectorIdentities exhausted(UsbRedirectorIdentities::maxIdentity - 1);
  void* last = exhausted.Add(reusedNative);
  require(last && exhausted.Remove(last) == reusedNative, "The final lifetime identity was not usable");
  require(!exhausted.Add(reusedNative) && !exhausted.Find(last), "Lifetime identity exhaustion wrapped and reused an old capture");
}

void independentApiNamespacesKeepLateCleanupLocal() {
  // These are members of each UsbDkApi, not process/global registries. Two
  // window engines may legitimately issue the same numeric lifetime ticket.
  // A retained request is also bound to its original API object; its late
  // cleanup must never resolve that number through another API's namespace.
  UsbRedirectorIdentities firstApi, secondApi;
  UsbPendingIo firstPending, secondPending;
  const HANDLE reusedRaw = reinterpret_cast<HANDLE>(uintptr_t(0x4567));
  const auto old = firstApi.Add(reusedRaw);
  OVERLAPPED oldIo{}, newIo{};
  require(firstPending.Add(old, 9, &oldIo), "Original API request registration failed");
  const auto lateStop = [&] { return firstApi.Remove(old); };
  const auto lateCancel = [&] { return firstApi.Find(old) && firstPending.Cancel(old, 9, [](OVERLAPPED*) { return true; }); };
  require(firstApi.Remove(old) == reusedRaw, "Original capture could not stop");
  const auto replacement = secondApi.Add(reusedRaw);
  require(old == replacement && replacement != nullptr, "Separate API ticket namespaces were not exercised");
  require(secondPending.Add(replacement, 9, &newIo), "Independent API same-token registration failed");
  require(!lateStop() && !lateCancel(), "Old API cleanup still resolved its stopped redirector");
  firstPending.Remove(old, 9, &oldIo);
  require(secondApi.Find(replacement) == reusedRaw, "Old API cleanup erased the replacement API's redirector");
  require(secondPending.Cancel(replacement, 9, [&](OVERLAPPED* value) { return value == &newIo; }), "Old API completion erased another API's same-number I/O");
  require(secondApi.Remove(replacement) == reusedRaw, "The current API lost its own Stop ownership");
  secondPending.Remove(replacement, 9, &newIo);
}

void lateReleasedCaptureCannotStopItsReplacement() {
  Rig rig;
  const auto old = rig.capture();
  const auto shared = rig.shared;
  shared->block = true; shared->ignoreAbort = true; shared->ignoreCancel = true;
  // Never leave a failed assertion's substituted request permanently blocked.
  struct Unblock { std::shared_ptr<Shared> state; ~Unblock() { state->forceReturn = true; } } unblock{shared};
  std::atomic<unsigned> oldCallbacks{0};
  rig.captures->SetReleaseWait(10);
  rig.captures->Transfer(old, bulk(61, 0x81, 512), [&](Result) { ++oldCallbacks; });
  require(waitFor([&] { return shared->blocking == 1; }), "The old capture request did not enter the gate");
  rig.captures->Release(old);
  require(shared->stops == 1 && rig.captures->Count() == 0 && oldCallbacks == 0, "Bounded release did not retain its unfinished request");
  // The same FakeApi always returns raw 0x1234. The engine's lifetime handle,
  // not that raw address, must scope late Cancel/Release/completion callbacks.
  const auto replacement = rig.capture();
  require(replacement != old && rig.captures->Count() == 1, "A replacement did not receive a distinct engine handle");
  const auto cancelledBefore = shared->cancelledToken.load();
  rig.captures->Cancel(old, 61);
  rig.captures->Release(old);
  require(shared->stops == 1 && shared->cancelledToken == cancelledBefore, "Late old cleanup acted on the replacement capture");
  shared->forceReturn = true;
  require(waitFor([&] { return oldCallbacks == 1; }), "The released request never completed");
  require(waitFor([&] { std::lock_guard lock(rig.mutex); return rig.gone.size() == 1; }), "The old completion did not identify its original handle");
  { std::lock_guard lock(rig.mutex); require(rig.gone.front() == old, "The old completion reported the replacement as unplugged"); }
  require(shared->stops == 1 && rig.captures->Count() == 1 && shared->apiDestroyed == 0, "Late completion changed the replacement's ownership");
  shared->block = false; shared->actual = 1; shared->response = {0x5a};
  const auto result = rig.run(replacement, bulk(61, 0x81, 1));
  require(result.status == kOk && result.data == std::vector<uint8_t>({0x5a}), "The replacement could not use its same-token request");
  rig.captures->Release(replacement);
  require(shared->stops == 2 && rig.captures->Count() == 0, "The replacement lost its own release ownership");
}

void completionCanDestroyItsEngine() {
  Rig rig;
  const auto handle = rig.capture();
  const auto shared = rig.shared;
  shared->actual = 1;
  std::atomic<bool> completed{false};
  rig.captures->Transfer(handle, bulk(71, 0x81, 1), [&](Result) {
    rig.captures.reset();
    completed = true;
  });
  require(waitFor([&] { return completed.load(); }), "A completion deadlocked when destroying its engine");
  require(waitFor([&] { return shared->apiDestroyed == 1; }), "Reentrant shutdown did not release the API");
}
}  // namespace

int main() {
  try {
    listing(); capturing(); heldUntilUsbDkLetsGo(); requests(); translated(); statuses(); cancelling(); unplugging(); everythingGivenBack(); controlCancelAndTimeout(); listingNeverBlocksTheVm(); countersAndReturn(); fieldStallAndRawStatus(); pendingRequestsBelongToTheirCapture(); lateCompletionDoesNotUseTheDestroyedEngine(); restartedRedirectorRejectsOldIdentity(); independentApiNamespacesKeepLateCleanupLocal(); lateReleasedCaptureCannotStopItsReplacement(); completionCanDestroyItsEngine();
    std::cout << "Multitap client USB capture regression passed (" << checks << " checks)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "usb capture failed: " << error.what() << " (after " << checks << " checks)\n";
    return 1;
  }
}
