#pragma once
// usb-v1 on the Host (docs/usb-redirection.md): the session logic between the
// relay channel (client side) and the VM's kernel (usbip-win2 via the loopback
// USB/IP server). No transport and no driver here: tests drive it directly with
// a fake kernel and a fake client.
//
// Rules this file keeps:
// - Only a session.start carrying the broker's grant (`usbProtocol`, the Host's
//   own `usbUrl` and `usbGrant`) does anything.
// - Every device is re-validated here, not trusted from the client or the
//   relay: strict descriptor parse, the default deny list, and the class
//   permissions (usbGrant) the broker passed on. The VM's GET_DESCRIPTOR for
//   the device and configuration is answered from the validated copy.
// - Only bulk and interrupt endpoints of the validated device, plus endpoint 0,
//   are ever used; isochronous requests end the connection. A few control
//   requests that would change the device's identity on the PC's bus
//   (SET_ADDRESS, SYNCH_FRAME, test-mode) are refused locally.
// - Backpressure: at most `maxInFlight` requests / `windowBytes` bytes out at a
//   time for the whole session; the kernel's connection thread waits for room
//   (so the VM's own queue fills, not ours). Transfers above the relay's
//   per-frame limit are split into sequential chunks (a short IN chunk ends the
//   transfer, as USB defines it).
// - A device is detached when the client says so, the broker revokes it, the
//   kernel's connection ends or the session ends. Detaching never waits for
//   the client.
// - Diagnostics: hostAction 250 with fixed stage codes and counts only, never a
//   vendor/product ID, name, serial number or data.
#include "machine.hpp"
#include "operational_log.hpp"
#include "usb_policy.hpp"
#include "usb_virtual_mouse.hpp"
#include "usbip_wire.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>

namespace host {
inline constexpr const char* usbProtocolName = "usb-v1";

enum class UsbStage : uint32_t {
  channelReady = 1, statusReady = 2, driverMissing = 3, driverInstalled = 4, driverInstallFailed = 5, protectionFailed = 6, restartRequired = 7, vhciUnavailable = 8,
  attached = 10, rejected = 11, attachFailed = 12, detached = 13, policyDetach = 14, importAccepted = 15, importRefused = 16,
  urbRefused = 17, windowFull = 18, channelClosed = 20, leftovers = 21, kernelClosed = 22, messageInvalid = 23,
  // Field test (a stick whose USBSTOR never started): what the VM asked and did not get.
  // urbFailed: error = errno of a failed request, expected = its class. unlinked: the VM cancelled a request
  // (error 0 the client confirmed, 1 it did not in time), expected = class. urbSlow: a control request still
  // unanswered, error = seconds, expected = class. deviceSummary: items = requests, kib, cause = failed,
  // expected = cancelled. vmDeviceState: the VM's own device node never started (error = Windows problem
  // code, expected = seconds since attach).
  urbFailed = 24, unlinked = 25, urbSlow = 26, deviceSummary = 27, vmDeviceState = 28,
  // controllerAdopted: our own pinned package without its owner marker was taken back.
  // controllerReplaced: Host settings replaced another product's controller (error = result code).
  // vhciUnavailable carries the ControllerOwner code in expected.
  controllerAdopted = 29, controllerReplaced = 30,
  // virtualMouseSummary (for a PC where the game mouse moves nothing): when the virtual mouse goes,
  // items = reports from the client, error = 1 the VM read its interrupt endpoint at least once (else 0),
  // expected = 1 the VM imported the device (else 0).
  virtualMouseSummary = 31,
  // The native PlugOut attempt failed: cause=1 returned false, cause=2 threw.
  // This is not a Windows error or evidence that the port remains present.
  detachFailed = 32 };
inline void recordUsbEvent(UsbStage stage, bool ok, uint32_t error = 0, uint32_t count = 0) noexcept {
  recordHostEvent("hostAction", {{"action", 250}, {"stage", int64_t(stage)}, {"ok", ok ? 1 : 0}, {"error", int64_t(error)}, {"expected", int64_t(count)}});
}
inline void recordUsbSummary(uint32_t requests, uint32_t kib, uint32_t failed, uint32_t cancelled) noexcept {
  recordHostEvent("hostAction", {{"action", 250}, {"stage", int64_t(UsbStage::deviceSummary)}, {"ok", 1}, {"items", int64_t(requests)}, {"kib", int64_t(kib)}, {"cause", int64_t(failed)}, {"expected", int64_t(cancelled)}});
}
// What the diagnostics may say about a request: bmRequestType<<8 | bRequest for a control transfer, else
// 0x10000 | transfer type<<8 | endpoint address. Never data, never an ID.
inline uint32_t usbRequestClass(bool control, const uint8_t* setup, uint8_t transfer, uint8_t address) {
  return control ? uint32_t(setup[0]) << 8 | setup[1] : 0x10000u | uint32_t(transfer) << 8 | address;
}
// Fixed reasons as small numbers for the diagnostics log (index + 1).
inline uint32_t usbReasonNumber(const std::string& reason) {
  static const char* const reasons[] = {"usb_class_denied", "usb_hid_boot_denied", "usb_isochronous_denied", "usb_descriptor_invalid", "usb_class_not_granted", "usb_vhci_unavailable", "usb_limit_reached",
    "usb_attach_failed", "usb_busy", "usb_user_stopped", "usb_device_removed", "usb_vm_detached", "usb_policy_changed", "usb_device_error", "usb_session_closed", "usb_driver_missing",
    "usb_driver_install_failed", "usb_vhci_unprotected", "usb_setup_failed", "usb_restart_required"};
  for (uint32_t i = 0; i < std::size(reasons); ++i) if (reason == reasons[i]) return i + 1;
  return 0;
}

// The Host's own channel URL from a granted stream session.start, or "".
inline std::string usbSessionUrl(const Json& start, const std::string& sessionId) {
  if (start.get("usbProtocol").string() != usbProtocolName || start.get("mode").string() != "stream" || sessionId.empty()) return {};
  const auto url = start.get("usbUrl").string(), path = "/api/v1/hosts/sessions/" + sessionId + "/usb";
  if (url.size() > 512 || !transport::relaySyntax(url,path)) return {};
  return url;
}
// The class permissions of the grant: absent or not exactly true is denied.
inline usb::Grant usbSessionGrant(const Json& start) {
  const auto& grant = start.get("usbGrant");
  const auto flag = [&](const char* key) { const auto* value = std::get_if<bool>(&grant.get(key).value); return value && *value; };
  return {flag("drives"), flag("printers"), flag("smartCards"), flag("serialPorts")};
}

inline bool usbBase64Decode(const std::string& text, usb::Bytes& out) {
  out.clear();
  if (text.empty() || text.size() > (usb::maxDescriptorBytes / 3 + 1) * 4) return false;
  DWORD length = 0;
  if (!CryptStringToBinaryA(text.data(), DWORD(text.size()), CRYPT_STRING_BASE64, nullptr, &length, nullptr, nullptr) || !length || length > usb::maxDescriptorBytes) return false;
  out.resize(length);
  if (!CryptStringToBinaryA(text.data(), DWORD(text.size()), CRYPT_STRING_BASE64, out.data(), &length, nullptr, nullptr)) { out.clear(); return false; }
  out.resize(length);
  // Canonical only (no whitespace or stray characters the decoder skipped).
  DWORD encoded = 0;
  if (!CryptBinaryToStringA(out.data(), length, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &encoded)) { out.clear(); return false; }
  std::string again(encoded, '\0');
  if (!CryptBinaryToStringA(out.data(), length, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, again.data(), &encoded)) { out.clear(); return false; }
  again.resize(encoded);
  if (again != text) { out.clear(); return false; }
  return true;
}

struct UsbPrepared { bool ready = false; std::string reason; };
// The VM's own device node for an attached device: state 0 not in the tree, 1 present but not started,
// 2 started; problem = Windows' problem code (0 none).
struct UsbVmState { int state = 0; uint32_t problem = 0; };
struct UsbHooks {
  std::function<bool(const usb::Bytes&)> sendFrame;                       // relay frame; false: queue full / closed
  std::function<bool(const std::string&)> sendText;                       // relay JSON; false: queue full / closed
  std::function<UsbPrepared()> prepare;                                   // blocking: driver installed, vhci protected
  std::function<int(const std::string& busId)> attach;                    // blocking: asks the vhci to import; port > 0
  std::function<void(int port)> detach;                                   // blocking: vhci plug-out
  std::function<void()> cancelAttach;                                     // stops attach attempts in progress
  std::function<UsbVmState(uint16_t vendor, uint16_t product)> deviceState;   // the VM's device node (optional)
};

namespace usbdefaults {
inline constexpr size_t windowBytes = 1048576, maxInFlight = 32, maxDevices = 4, maxAttaches = 64, totalLimit = 4 * 1024 * 1024;
// A request the VM cancelled is answered when the client confirms; without a confirmation after this long, anyway.
inline constexpr int unlinkAnswerMs = 3000;
// A control request still unanswered after this long is noted once (the USB limit for a control transfer is 5 s).
inline constexpr int slowControlMs = 5000;
// More cancelled requests than this still waiting for the client's return: the device is wedged on the PC.
inline constexpr size_t maxOrphans = 8;
// The VM's device node not started this long after the attach: the VM could not start the device.
inline constexpr int vmStartMs = 45000;
// The VM's device node present with another Windows problem (28: no driver for the device, 52: a driver Windows
// does not accept, ...) on this many polls in a row and this long after the attach: recorded once, never ended.
// Installing a driver is the desktop administrator's to do; the record says which problem Windows names.
inline constexpr int vmProblemMs = 10000;
inline constexpr unsigned vmProblemPolls = 3;
// Noted per session and kind (diagnostics stay bounded).
inline constexpr unsigned noteBudget = 8;
// The virtual mouse (usb_virtual_mouse.hpp): the Host's own reads waiting at the client, the VM reads it
// holds at most, and how long the VM may have no read waiting before the Host takes its reads back.
// Each read comes back one round trip after the report that answered it, so the client can send a report
// every millisecond (the endpoint's interval) only with a round trip's worth of reads waiting: 24 covers a
// 20 ms round trip, and video bursts on the way back, and leaves 8 of the relay's 32 requests to the other
// devices (the model in the client's game_mouse_usb_test; docs/usb-redirection.md).
inline constexpr size_t mouseAhead = 24, mouseVmReads = 32;
inline constexpr int mouseIdleMs = 1000, mouseStaleMs = 300;
}

class UsbSessionCore {
 public:
  UsbSessionCore(std::string sessionId, usb::Grant grant, UsbHooks hooks) : sessionId_(std::move(sessionId)), grant_(grant), hooks_(std::move(hooks)) {}
  ~UsbSessionCore() { closed(); }

  // A relay JSON message. false: the channel must close.
  bool message(const Json& value) {
    const auto type = value.get("type").string();
    if (type == "usb.error" || type == "usb.channel.closed") return false;
    if (type == "usb.ready") return ready(value);
    if (!ready_) return false;
    if (type == "usb.attach") return attach(value);
    if (type == "usb.detach") return detach(value);
    return true;  // nothing else is relayed to the Host
  }

  // A relay binary frame from the client. false: the channel must close.
  bool frame(const uint8_t* data, size_t size) {
    const auto decoded = usb::decodeFrame(data, size);
    if (!ready_ || !decoded) { recordUsbEvent(UsbStage::messageInvalid, false); return false; }
    std::vector<std::function<void()>> after;
    bool ok = true;
    {
      std::lock_guard lock(mutex_);
      auto found = devices_.find(decoded->device);
      if (found == devices_.end()) return true;           // a late answer for a device that is gone
      Device& device = *found->second;
      if (decoded->type == usb::frameReturn) ok = device.mouse.ahead.count(decoded->seq) ? mouseReturn(device, *decoded, after) : onReturn(device, *decoded, after);
      else if (decoded->type == usb::frameUnlinkReturn) {
        const auto target = device.unlinks.find(decoded->seq);
        if (target != device.unlinks.end()) {
          const uint32_t relaySeq = target->second;
          device.unlinks.erase(target);
          // The client aborted the request (-ECONNRESET): the VM's cancel is answered now, not when (if ever)
          // the device lets go of the request. A stuck request must not hold the VM's USB stack for ever.
          if (decoded->status == -104) answerUnlink(device, relaySeq, 0, after);
        }
      }
      else ok = false;
      mouseTopUp();                                       // room that came back goes to the mouse first
    }
    for (auto& action : after) action();
    windowChanged_.notify_all();
    return ok;
  }

  // --- the kernel's side (called from the loopback server's threads) ----------
  // OP_REQ_IMPORT for [busId]: the exported device, or nullopt (refused).
  // [toKernel] writes bytes to that kernel connection.
  struct Imported { uint8_t id = 0; usb::ExportedDevice exported; };
  std::optional<Imported> importDevice(const std::string& busId, std::function<bool(const usb::Bytes&)> toKernel, std::function<void()> closeKernel = {}) {
    std::lock_guard lock(mutex_);
    if (closed_) return std::nullopt;
    for (auto& [id, device] : devices_) {
      if (device->busId != busId) continue;
      if (device->state != State::attaching || device->imported) { recordUsbEvent(UsbStage::importRefused, false); return std::nullopt; }
      device->imported = true; device->state = State::attached; device->toKernel = std::move(toKernel); device->closeKernel = std::move(closeKernel);
      // The client learns of the device before the kernel's first request can
      // exist, so the relay never sees a request for a device it thinks is pending.
      if (hooks_.sendText) hooks_.sendText("{\"type\":\"usb.attached\",\"device\":" + std::to_string(id) + '}');
      recordUsbEvent(UsbStage::importAccepted, true, 0, uint32_t(devices_.size()));
      return Imported{id, device->exported};
    }
    recordUsbEvent(UsbStage::importRefused, false);
    return std::nullopt;
  }

  // CMD_SUBMIT from the kernel. Blocks while the session's window is full.
  // false: this kernel connection must end.
  bool submit(uint8_t id, const usb::Urb& urb, usb::Bytes data) {
    std::vector<std::function<void()>> after;
    bool result;
    {
      std::unique_lock lock(mutex_);
      result = submitLocked(lock, id, urb, std::move(data), after);
    }
    for (auto& action : after) action();
    return result;
  }
  // CMD_UNLINK from the kernel.
  bool unlink(uint8_t id, const usb::Urb& urb) {
    std::vector<std::function<void()>> after;
    {
      std::lock_guard lock(mutex_);
      auto found = devices_.find(id);
      if (found == devices_.end() || found->second->state != State::attached) return false;
      Device& device = *found->second;
      const auto target = device.kernelToRelay.find(urb.target);
      if (device.virtualMouse) {                           // its reads wait here, never at the client
        auto& reads = device.mouse.vmReads;
        const auto held = std::find(reads.begin(), reads.end(), urb.target);
        const bool waiting = held != reads.end();
        if (waiting) reads.erase(held);
        const auto bytes = usb::encodeRetUnlink(urb.seq, usb::deviceId(device.exported), waiting ? -104 : 0);
        after.push_back([send = device.toKernel, bytes] { if (send) send(bytes); });
      } else if (target == device.kernelToRelay.end()) {   // already completed: nothing to cancel
        const auto bytes = usb::encodeRetUnlink(urb.seq, usb::deviceId(device.exported), 0);
        after.push_back([send = device.toKernel, bytes] { if (send) send(bytes); });
      } else if (const auto held = device.pending.find(target->second); held == device.pending.end() || !held->second) {
        // A kernel number whose request is no longer pending (pending.at()
        // would throw out of the SYSTEM service here). Answered as already completed; the stale entry goes.
        device.kernelToRelay.erase(target);
        const auto bytes = usb::encodeRetUnlink(urb.seq, usb::deviceId(device.exported), 0);
        after.push_back([send = device.toKernel, bytes] { if (send) send(bytes); });
      } else {
        Pending& pending = *held->second;
        pending.unlinking = true; pending.unlinkKernelSeq = urb.seq; pending.unlinkSince = clock();
        ++device.cancelled;
        usb::Frame frame; frame.type = usb::frameUnlink; frame.device = id; frame.seq = ++sequence_; frame.length = pending.relaySeq;
        device.unlinks[frame.seq] = pending.relaySeq;
        if (hooks_.sendFrame) hooks_.sendFrame(usb::encodeFrame(frame));
      }
    }
    for (auto& action : after) action();
    return true;
  }

  // The kernel's connection for [id] ended (the VM detached it or the vhci went away).
  void kernelClosed(uint8_t id) {
    std::vector<std::function<void()>> after;
    {
      std::lock_guard lock(mutex_);
      const auto found = devices_.find(id);
      if (found == devices_.end()) return;
      recordUsbEvent(UsbStage::kernelClosed, true, 0, uint32_t(devices_.size() - 1));
      removeDevice(id, "usb_vm_detached", true, after);
    }
    for (auto& action : after) action();
    windowChanged_.notify_all();
  }

  // The channel ended (revocation, session end, failure): nothing stays attached.
  void closed() {
    std::vector<std::function<void()>> after;
    std::vector<std::thread> workers;
    {
      std::lock_guard lock(mutex_);
      if (closed_) return;
      closed_ = true;
      const auto count = uint32_t(devices_.size());
      for (auto it = devices_.begin(); it != devices_.end();) { const auto id = it->first; ++it; removeDevice(id, "usb_session_closed", false, after); }
      if (ready_) recordUsbEvent(UsbStage::channelClosed, true, 0, count);
      workers.swap(workers_);
    }
    windowChanged_.notify_all();
    if (hooks_.cancelAttach) hooks_.cancelAttach();
    for (auto& action : after) action();
    for (auto& worker : workers) if (worker.joinable()) worker.join();
  }

  // Called about twice a second by the session loop: answers a cancel the client never confirmed, notes a
  // control request nobody answered, and ends a device the VM could not start. Uses the injected clock for tests.
  void tick() {
    std::vector<std::function<void()>> after;
    {
      std::lock_guard lock(mutex_);
      if (closed_) return;
      const auto now = clock();
      const auto since = [&](std::chrono::steady_clock::time_point then) { return int(std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count()); };
      std::vector<uint8_t> failing;
      for (auto& [id, holder] : devices_) {
        Device& device = *holder;
        if (device.state != State::attached) continue;
        std::vector<uint32_t> late;
        size_t orphans = 0;
        for (auto& [relaySeq, pending] : device.pending) {
          if (pending->unlinkAnswered) { ++orphans; continue; }
          if (pending->unlinking && since(pending->unlinkSince) >= usbdefaults::unlinkAnswerMs) late.push_back(relaySeq);
          else if (!pending->unlinking && !pending->slowNoted && pending->transfer == 0 && since(pending->submittedAt) >= usbdefaults::slowControlMs) {
            pending->slowNoted = true;
            note(2, UsbStage::urbSlow, false, uint32_t(since(pending->submittedAt) / 1000), pending->cls);
          }
        }
        for (const auto relaySeq : late) answerUnlink(device, relaySeq, 1, after);
        if (orphans + late.size() > usbdefaults::maxOrphans) { failing.push_back(id); continue; }
        if (device.virtualMouse && device.mouse.reading && device.mouse.vmReads.empty() && since(device.mouse.vmReadAt) >= usbdefaults::mouseIdleMs) mouseIdle(device);
        else if (device.virtualMouse) mouseReadAhead(device);   // reads the relay queue had no room for
        if (hooks_.deviceState && device.port > 0 && !device.vmStarted && since(device.attachedAt) >= 1000 && since(device.lastPoll) >= 1000) {
          device.lastPoll = now;
          UsbVmState state;
          try { state = hooks_.deviceState(device.desc.vendor, device.desc.product); } catch (...) { state = {}; }
          if (state.state == 2) device.vmStarted = true;
          else {
            device.problemPolls = state.problem == 10 ? device.problemPolls + 1 : 0;
            // The VM's USBSTOR could not start (problem 10), or it has not started within the limit and
            // Windows names no other problem (a hung start). Another problem (no driver for a vendor
            // device, say) is the VM user's to solve and never ends the attachment.
            if (device.problemPolls >= 2 || (state.problem == 0 && since(device.attachedAt) >= usbdefaults::vmStartMs)) {
              note(3, UsbStage::vmDeviceState, false, state.problem, uint32_t(since(device.attachedAt) / 1000));
              failing.push_back(id);
            }
            else if (!device.problemNoted) {
              // Still attached and still the VM user's (or administrator's) to solve: said once, with Windows' own code.
              const bool lasting = state.state == 1 && state.problem != 0 && state.problem != 10;
              device.otherProblemPolls = !lasting ? 0 : state.problem == device.otherProblem ? device.otherProblemPolls + 1 : 1;
              device.otherProblem = lasting ? state.problem : 0;
              if (lasting && device.otherProblemPolls >= usbdefaults::vmProblemPolls && since(device.attachedAt) >= usbdefaults::vmProblemMs) {
                device.problemNoted = true; ++vmProblemsNoted_;
                note(3, UsbStage::vmDeviceState, false, state.problem, uint32_t(since(device.attachedAt) / 1000));
              }
            }
          }
        }
      }
      for (const auto id : failing) removeDevice(id, "usb_device_error", true, after);
    }
    for (auto& action : after) action();
    windowChanged_.notify_all();
  }
  // Tests drive the clock.
  std::function<std::chrono::steady_clock::time_point()> clock = [] { return std::chrono::steady_clock::now(); };

  size_t deviceCount() const { std::lock_guard lock(mutex_); return devices_.size(); }
  size_t inflightRequests() const { std::lock_guard lock(mutex_); return inflight_; }
  size_t inflightBytes() const { std::lock_guard lock(mutex_); return inflightBytes_; }
  bool usable() const { std::lock_guard lock(mutex_); return !closed_; }
  bool deviceExists(uint8_t id) const { std::lock_guard lock(mutex_); return devices_.count(id) != 0; }
  // Devices whose node stayed unstarted with a Windows problem that does not end them (recorded once each).
  unsigned vmProblemsNoted() const { std::lock_guard lock(mutex_); return vmProblemsNoted_; }

 private:
  enum class State { attaching, attached };
  struct Pending {
    uint32_t kernelSeq = 0, direction = 0, relaySeq = 0, unlinkKernelSeq = 0, endpoint = 0; uint8_t address = 0, transfer = 0, setup[8]{};
    size_t total = 0, moved = 0, chunk = 0; usb::Bytes out, in; bool unlinking = false;
    uint32_t cls = 0;                                           // usbRequestClass
    std::chrono::steady_clock::time_point submittedAt, unlinkSince;
    bool unlinkAnswered = false;                                // the VM's cancel was answered; the late return is dropped
    bool slowNoted = false;
  };
  struct Device {
    uint8_t id = 0; usb::Device desc; usb::Decision decision; std::string busId; usb::ExportedDevice exported;
    State state = State::attaching; bool imported = false; int port = 0;
    uint32_t requests = 0, kib = 0, failed = 0, cancelled = 0;  // for the summary when the device goes
    std::chrono::steady_clock::time_point attachedAt, lastPoll; bool vmStarted = false; unsigned problemPolls = 0;
    uint32_t otherProblem = 0; unsigned otherProblemPolls = 0; bool problemNoted = false;  // a problem that does not end the attachment
    // Multitap's virtual mouse (usb_virtual_mouse.hpp), known by its exact descriptors.
    bool virtualMouse = false;
    struct Mouse {
      std::deque<uint32_t> vmReads;                       // the VM's interrupt reads waiting here (kernel seq)
      std::set<uint32_t> ahead;                           // the Host's own reads waiting at the client (relay seq)
      std::deque<std::pair<usb::Bytes, std::chrono::steady_clock::time_point>> reports;   // waiting for a VM read
      usb::Bytes last = usb::Bytes(usb::vmouse::reportLength, 0);
      std::chrono::steady_clock::time_point vmReadAt; bool reading = false;
    } mouse;
    std::function<bool(const usb::Bytes&)> toKernel;
    std::function<void()> closeKernel;                      // ends the kernel's connection (the device thread then stops)
    std::map<uint32_t, std::unique_ptr<Pending>> pending;   // by relay sequence number
    std::map<uint32_t, uint32_t> kernelToRelay;             // kernel sequence number -> relay sequence number
    std::map<uint32_t, uint32_t> unlinks;                   // relay unlink sequence number -> target relay sequence number
  };
  mutable std::mutex mutex_;
  std::condition_variable windowChanged_;
  std::string sessionId_; usb::Grant grant_; UsbHooks hooks_;
  std::map<uint8_t, std::unique_ptr<Device>> devices_;
  std::set<uint8_t> seen_;
  std::vector<std::thread> workers_;
  bool ready_ = false, closed_ = false, vhciReady_ = false; std::string vhciReason_;
  size_t limitUrb_ = usb::maxUrbBytes, limitWindow_ = usbdefaults::windowBytes, limitFlight_ = usbdefaults::maxInFlight, limitDevices_ = usbdefaults::maxDevices, limitAttaches_ = usbdefaults::maxAttaches;
  size_t inflight_ = 0, inflightBytes_ = 0, attaches_ = 0; uint32_t sequence_ = 0, busNumber_ = 0;
  unsigned vmProblemsNoted_ = 0;
  unsigned noted_[4]{};                                          // per kind: failed, unlinked, slow, vm state

  // A bounded number of each kind of diagnostic per session.
  void note(unsigned kind, UsbStage stage, bool ok, uint32_t error, uint32_t cls) {
    if (noted_[kind]++ < usbdefaults::noteBudget) recordUsbEvent(stage, ok, error, cls);
  }

  // The VM's cancel of [relaySeq] is answered with -ECONNRESET now. The request itself stays accounted in the
  // window (the relay counts it until the client returns it); when it does return, nothing more is sent.
  void answerUnlink(Device& device, uint32_t relaySeq, uint32_t how, std::vector<std::function<void()>>& after) {
    const auto found = device.pending.find(relaySeq);
    if (found == device.pending.end()) return;
    Pending& pending = *found->second;
    if (!pending.unlinking || pending.unlinkAnswered) return;
    pending.unlinkAnswered = true;
    device.kernelToRelay.erase(pending.kernelSeq);
    note(1, UsbStage::unlinked, how == 0, how, pending.cls);
    const auto bytes = usb::encodeRetUnlink(pending.unlinkKernelSeq, usb::deviceId(device.exported), -104);
    after.push_back([send = device.toKernel, bytes] { if (send) send(bytes); });
  }

  bool submitLocked(std::unique_lock<std::mutex>& lock, uint8_t id, const usb::Urb& urb, usb::Bytes data, std::vector<std::function<void()>>& after) {
    auto found = devices_.find(id);
    if (found == devices_.end() || found->second->state != State::attached) return false;
    Device* device = found->second.get();
    if (urb.packets != -1 && urb.packets != 0) { recordUsbEvent(UsbStage::urbRefused, false, 1); return false; }   // isochronous
    if (urb.length < 0 || size_t(urb.length) > usbdefaults::totalLimit) { recordUsbEvent(UsbStage::urbRefused, false, 2); return false; }
    const bool control = (urb.endpoint & 15) == 0;
    uint8_t address = uint8_t((urb.endpoint & 15) | (urb.direction == 1 ? 0x80 : 0));
    if (control) address = uint8_t(urb.setup[0] & 0x80);
    const auto type = usb::endpointType(device->desc, address);
    const uint32_t cls = usbRequestClass(control, urb.setup, type ? *type : uint8_t(2), address);
    if (!type || (control && *type != 0) || (!control && *type == 0)) { completeLocally(*device, urb, -32, nullptr, 0, after); recordUsbEvent(UsbStage::urbRefused, false, 3, cls); return true; }
    if (data.size() != (urb.direction == 1 ? size_t(0) : size_t(urb.length))) { recordUsbEvent(UsbStage::urbRefused, false, 4, cls); return false; }
    if (control) {
      if (size_t(urb.setup[6] | urb.setup[7] << 8) != size_t(urb.length) || (urb.length != 0 && ((urb.setup[0] & 0x80) != 0) != (urb.direction == 1))) { recordUsbEvent(UsbStage::urbRefused, false, 4, cls); return false; }
      if (size_t(urb.length) > limitUrb_) { completeLocally(*device, urb, -75, nullptr, 0, after); return true; }
      if (const auto local = usb::localDescriptor(device->desc, urb.setup)) { completeLocally(*device, urb, 0, local->data(), local->size(), after); return true; }
      if (refusedControl(urb.setup) || usb::refusedDescriptorRequest(urb.setup)) { completeLocally(*device, urb, -32, nullptr, 0, after); recordUsbEvent(UsbStage::urbRefused, false, 5, cls); return true; }
    }
    if (device->virtualMouse) return mouseSubmit(*device, urb, after);
    if (device->kernelToRelay.count(urb.seq)) return false;   // a repeated sequence number
    auto pending = std::make_unique<Pending>();
    pending->kernelSeq = urb.seq; pending->direction = urb.direction; pending->address = address; pending->transfer = *type; pending->total = size_t(urb.length);
    pending->endpoint = urb.endpoint; std::memcpy(pending->setup, urb.setup, 8); pending->out = std::move(data);
    pending->cls = cls; pending->submittedAt = clock();
    const size_t first = std::min(limitUrb_, pending->total);
    const auto room = [&] { return closed_ || !devices_.count(id) || (inflight_ < limitFlight_ && inflightBytes_ + first <= limitWindow_); };
    if (!windowChanged_.wait_for(lock, std::chrono::seconds(60), room)) { recordUsbEvent(UsbStage::windowFull, false); return false; }
    found = devices_.find(id);                                  // the lock was released while waiting
    if (closed_ || found == devices_.end() || found->second->state != State::attached || found->second->kernelToRelay.count(urb.seq)) return false;
    device = found->second.get();
    const uint32_t relaySeq = ++sequence_;
    pending->relaySeq = relaySeq;
    device->kernelToRelay[urb.seq] = relaySeq;
    Pending& stored = *pending;
    device->pending[relaySeq] = std::move(pending);
    return sendChunk(*device, stored);
  }
  static bool integer(double number, double low, double high) { return std::isfinite(number) && std::floor(number) == number && number >= low && number <= high; }

  bool ready(const Json& value) {
    if (ready_ || value.get("sessionId").string() != sessionId_ || value.get("protocol").string() != usbProtocolName) return false;
    const auto& limits = value.get("limits");
    const double urb = limits.get("maxUrbBytes").number(-1), window = limits.get("windowBytes").number(-1), flight = limits.get("maxInFlight").number(-1),
        devices = limits.get("maxDevices").number(-1), attaches = limits.get("maxAttaches").number(-1);
    if (!integer(urb, 4096, usb::maxUrbBytes) || !integer(window, urb, 16 * 1024 * 1024) || !integer(flight, 1, 256) || !integer(devices, 1, 16) || !integer(attaches, 1, 1024)) return false;
    {
      std::lock_guard lock(mutex_);
      limitUrb_ = size_t(urb); limitWindow_ = size_t(window); limitFlight_ = size_t(flight); limitDevices_ = size_t(devices); limitAttaches_ = size_t(attaches);
      ready_ = true;
    }
    recordUsbEvent(UsbStage::channelReady, true);
    // Installing and protecting the VM's virtual host controller can take a
    // while; the channel stays responsive meanwhile.
    std::lock_guard lock(mutex_);
    workers_.emplace_back([this] {
      UsbPrepared prepared;
      try { prepared = hooks_.prepare ? hooks_.prepare() : UsbPrepared{false, "usb_setup_failed"}; } catch (...) { prepared = {false, "usb_setup_failed"}; }
      {
        std::lock_guard inner(mutex_);
        if (closed_) return;
        vhciReady_ = prepared.ready; vhciReason_ = prepared.ready ? std::string() : (prepared.reason.empty() ? std::string("usb_setup_failed") : prepared.reason);
      }
      if (prepared.ready) { recordUsbEvent(UsbStage::statusReady, true); if (hooks_.sendText) hooks_.sendText("{\"type\":\"usb.status\",\"state\":\"ready\"}"); }
      else {
        const auto reason = prepared.reason.empty() ? std::string("usb_setup_failed") : prepared.reason;
        recordUsbEvent(UsbStage::vhciUnavailable, false, usbReasonNumber(reason));
        if (hooks_.sendText) hooks_.sendText("{\"type\":\"usb.status\",\"state\":\"unavailable\",\"reason\":" + jsonQuote(reason) + '}');
      }
    });
    return true;
  }

  void reject(uint8_t id, const std::string& reason) {
    recordUsbEvent(UsbStage::rejected, false, usbReasonNumber(reason));
    if (hooks_.sendText) hooks_.sendText("{\"type\":\"usb.rejected\",\"device\":" + std::to_string(id) + ",\"reason\":" + jsonQuote(reason) + '}');
  }

  bool attach(const Json& value) {
    const double number = value.get("device").number(-1);
    if (!integer(number, 1, 255)) return false;
    const uint8_t id = uint8_t(number);
    const auto speed = value.get("speed").string();
    usb::Bytes bytes;
    std::lock_guard lock(mutex_);
    if (seen_.count(id) || attaches_ >= limitAttaches_ || (speed != "low" && speed != "full" && speed != "high")) return false;
    seen_.insert(id); ++attaches_;
    if (!usbBase64Decode(value.get("descriptors").string(), bytes)) { reject(id, "usb_descriptor_invalid"); return true; }
    auto parsed = usb::parseDescriptors(bytes.data(), bytes.size());
    if (!parsed) { reject(id, "usb_descriptor_invalid"); return true; }
    const auto decision = usb::decide(*parsed);
    if (!decision.allowed) { reject(id, decision.reason); return true; }
    if (!usb::covers(decision, grant_)) { reject(id, "usb_class_not_granted"); return true; }
    // After a failed preparation the device is refused with that failure, not the
    // generic "unavailable" (which said "another installation" to everyone reading it).
    if (!vhciReady_) { reject(id, vhciReason_.empty() ? std::string("usb_vhci_unavailable") : vhciReason_); return true; }
    if (devices_.size() >= limitDevices_) { reject(id, "usb_limit_reached"); return true; }
    auto device = std::make_unique<Device>();
    device->id = id; device->decision = decision; device->busId = usb::busIdFor(++busNumber_);
    auto& exported = device->exported;
    exported.busId = device->busId; exported.path = "/Multitap/usb/" + device->busId; exported.busNumber = 1; exported.deviceNumber = busNumber_;
    exported.speed = speed == "low" ? 1 : speed == "full" ? 2 : 3;
    exported.vendor = parsed->vendor; exported.product = parsed->product; exported.bcdDevice = parsed->bcdDevice;
    exported.cls = parsed->cls; exported.subclass = parsed->subclass; exported.protocol = parsed->protocol;
    exported.configurations = uint8_t(parsed->configurations.size()); exported.configurationValue = parsed->configurations[0].value;
    exported.interfaces = uint8_t(std::set<uint8_t>([&] { std::set<uint8_t> numbers; for (const auto& item : parsed->configurations[0].interfaces) numbers.insert(item.number); return numbers; }()).size());
    device->desc = std::move(*parsed);
    device->virtualMouse = usb::vmouse::matches(bytes.data(), bytes.size());
    const std::string busId = device->busId;
    devices_[id] = std::move(device);
    workers_.emplace_back([this, id, busId] {
      int port = 0;
      try { port = hooks_.attach ? hooks_.attach(busId) : 0; } catch (...) { port = 0; }
      std::vector<std::function<void()>> after;
      {
        std::lock_guard inner(mutex_);
        const auto found = devices_.find(id);
        if (port > 0) {
          if (found == devices_.end() || closed_) { after.push_back([this, port] { if (hooks_.detach) hooks_.detach(port); }); }
          else { found->second->port = port; found->second->attachedAt = found->second->lastPoll = clock(); recordUsbEvent(UsbStage::attached, true, 0, uint32_t(devices_.size())); }
        } else if (found != devices_.end() && !closed_) {
          if (found->second->imported) removeDevice(id, "usb_device_error", true, after);          // imported, but the plug-in failed afterwards
          else { recordUsbEvent(UsbStage::attachFailed, false, 0, uint32_t(devices_.size())); devices_.erase(found); reject(id, "usb_attach_failed"); }
        }
      }
      for (auto& action : after) action();
      windowChanged_.notify_all();
    });
    return true;
  }

  bool detach(const Json& value) {
    const double number = value.get("device").number(-1);
    if (!integer(number, 1, 255)) return false;
    const auto reason = value.get("reason").string();
    std::vector<std::function<void()>> after;
    {
      std::lock_guard lock(mutex_);
      if (!devices_.count(uint8_t(number))) return true;
      recordUsbEvent(reason == "usb_policy_changed" ? UsbStage::policyDetach : UsbStage::detached, true, usbReasonNumber(reason), uint32_t(devices_.size() - 1));
      removeDevice(uint8_t(number), reason, false, after);
    }
    for (auto& action : after) action();
    windowChanged_.notify_all();
    return true;
  }

  // Drops [id]: its requests stop holding the window, the kernel connection is
  // closed and the vhci port unplugged. [tellClient]: the VM-side reason goes
  // to the client; otherwise the client already knows.
  void removeDevice(uint8_t id, const std::string& reason, bool tellClient, std::vector<std::function<void()>>& after) {
    const auto found = devices_.find(id);
    if (found == devices_.end()) return;
    Device& device = *found->second;
    if (device.virtualMouse) recordHostEvent("hostAction", {{"action", 250}, {"stage", int64_t(UsbStage::virtualMouseSummary)}, {"ok", 1}, {"items", int64_t(device.requests)},
                                                           {"error", device.mouse.vmReadAt != std::chrono::steady_clock::time_point{} ? 1 : 0}, {"expected", device.imported ? 1 : 0}});
    else if (device.requests) recordUsbSummary(device.requests, device.kib, device.failed, device.cancelled);
    for (const auto& [seq, pending] : device.pending) { if (inflight_) --inflight_; inflightBytes_ -= std::min(inflightBytes_, pending->chunk); (void)seq; }
    for (size_t i = 0; i < device.mouse.ahead.size(); ++i) { if (inflight_) --inflight_; inflightBytes_ -= std::min(inflightBytes_, usb::vmouse::reportLength); }
    if (tellClient && hooks_.sendText && !closed_) hooks_.sendText("{\"type\":\"usb.detach\",\"device\":" + std::to_string(id) + ",\"reason\":" + jsonQuote(reason) + '}');
    const int port = device.port;
    if (device.closeKernel) after.push_back(device.closeKernel);
    if (port > 0 && hooks_.detach) after.push_back([detach = hooks_.detach, port] { detach(port); });
    // A device still importing: the attach worker finds it gone and unplugs.
    devices_.erase(found);
  }

  // The USB-level requests the VM may not make of a PC's device.
  static bool refusedControl(const uint8_t* setup) {
    const unsigned type = (setup[0] >> 5) & 3, recipient = setup[0] & 31, request = setup[1];
    if (type != 0) return false;                                 // standard requests only
    if (recipient == 0 && (request == 5 || request == 12)) return true;                    // SET_ADDRESS, SYNCH_FRAME (device level)
    if (recipient == 2 && request == 12) return true;                                      // SYNCH_FRAME
    if (recipient == 0 && request == 3 && (setup[2] | setup[3] << 8) == 2) return true;    // SET_FEATURE(TEST_MODE)
    return false;
  }

  void completeLocally(Device& device, const usb::Urb& urb, int32_t status, const uint8_t* data, size_t size, std::vector<std::function<void()>>& after) {
    const auto bytes = usb::encodeRetSubmit(urb.seq, usb::deviceId(device.exported), urb.direction, urb.endpoint, status, data, size);
    after.push_back([send = device.toKernel, bytes] { if (send) send(bytes); });
  }

  // --- the virtual mouse (usb_virtual_mouse.hpp) --------------------------------------------------
  // Control is answered here; a VM interrupt read takes a report that already arrived or waits here, while
  // up to mouseAhead reads of the Host's own wait at the client for its next reports (read-ahead: one
  // network crossing per report, not a round trip per VM read). Nothing the VM asks reaches the client.
  bool mouseSubmit(Device& device, const usb::Urb& urb, std::vector<std::function<void()>>& after) {
    if ((urb.endpoint & 15) == 0) {
      const auto answer = usb::vmouse::control(urb.setup, device.mouse.last);
      const bool in = (urb.setup[0] & 0x80) != 0;
      completeLocally(device, urb, answer.status, in ? answer.data.data() : nullptr, in ? answer.data.size() : answer.status == 0 ? size_t(urb.length) : 0, after);
      return true;
    }
    auto& mouse = device.mouse;
    // Only reads with room for a whole report (the endpoint check already passed), a bounded number of them.
    if (urb.direction != 1 || size_t(urb.length) < usb::vmouse::reportLength || mouse.vmReads.size() >= usbdefaults::mouseVmReads) { completeLocally(device, urb, -32, nullptr, 0, after); return true; }
    if (std::find(mouse.vmReads.begin(), mouse.vmReads.end(), urb.seq) != mouse.vmReads.end()) return false;   // a repeated sequence number
    mouse.vmReadAt = clock();
    if (auto report = mouseFresh(mouse)) answerRead(device, urb.seq, *report, after);
    else mouse.vmReads.push_back(urb.seq);
    mouse.reading = true;
    mouseReadAhead(device);
    return true;
  }
  // A mouse short of reads (another device held the window when it began, or the relay queue was full) gets
  // no return of its own to start more: every return of any device tops it up, before a request waiting for
  // room takes that room.
  void mouseTopUp() {
    for (auto& entry : devices_) if (entry.second->virtualMouse && entry.second->state == State::attached) mouseReadAhead(*entry.second);
  }
  void mouseReadAhead(Device& device) {
    auto& mouse = device.mouse;
    while (mouse.reading && mouse.ahead.size() < usbdefaults::mouseAhead && inflight_ < limitFlight_ && inflightBytes_ + usb::vmouse::reportLength <= limitWindow_) {
      usb::Frame frame; frame.type = usb::frameSubmit; frame.device = device.id; frame.endpoint = usb::vmouse::endpoint; frame.transfer = 3;
      frame.seq = ++sequence_; frame.length = uint32_t(usb::vmouse::reportLength);
      if (!hooks_.sendFrame || !hooks_.sendFrame(usb::encodeFrame(frame))) return;   // the relay queue is full: a later return, VM read or tick tries again
      mouse.ahead.insert(frame.seq); ++inflight_; inflightBytes_ += usb::vmouse::reportLength;
    }
  }
  // The VM has had no read waiting for a while (its driver stopped): the Host's reads come back from the client.
  void mouseIdle(Device& device) {
    device.mouse.reading = false;
    for (const auto target : device.mouse.ahead) {
      usb::Frame frame; frame.type = usb::frameUnlink; frame.device = device.id; frame.seq = ++sequence_; frame.length = target;
      device.unlinks[frame.seq] = target;
      if (hooks_.sendFrame) hooks_.sendFrame(usb::encodeFrame(frame));
    }
  }
  bool mouseReturn(Device& device, const usb::Frame& frame, std::vector<std::function<void()>>& after) {
    auto& mouse = device.mouse;
    if (frame.endpoint != usb::vmouse::endpoint || frame.transfer != 3) return false;
    if (frame.status == 0 ? frame.length != usb::vmouse::reportLength || frame.payload.size() != usb::vmouse::reportLength : !frame.payload.empty()) return false;
    mouse.ahead.erase(frame.seq);
    if (inflight_) --inflight_;
    inflightBytes_ -= std::min(inflightBytes_, usb::vmouse::reportLength);
    if (frame.status == 0) mouseDeliver(device, frame.payload, after);
    else if (frame.status != -104) { ++device.failed; note(0, UsbStage::urbFailed, false, uint32_t(-frame.status), usbRequestClass(false, frame.setup, 3, usb::vmouse::endpoint)); }
    mouseReadAhead(device);
    return true;
  }
  void mouseDeliver(Device& device, const usb::Bytes& report, std::vector<std::function<void()>>& after) {
    auto& mouse = device.mouse;
    mouse.last = report;
    ++device.requests;
    if (!mouse.vmReads.empty()) { const auto seq = mouse.vmReads.front(); mouse.vmReads.pop_front(); answerRead(device, seq, report, after); return; }
    // No VM read waiting: kept in order, motion joined into the newest with the same buttons.
    const auto now = clock();
    if (!mouse.reports.empty() && usb::vmouse::join(mouse.reports.back().first, report)) { mouse.reports.back().second = now; return; }
    mouse.reports.emplace_back(report, now);
    while (mouse.reports.size() > 256) mouse.reports.pop_front();
  }
  // The oldest report waiting for a VM read. Older than mouseStaleMs: dropped, the newest of them standing
  // for the state now without its motion (ToTB Link's SyntheticHidDevice.Fresh).
  std::optional<usb::Bytes> mouseFresh(Device::Mouse& mouse) {
    if (mouse.reports.empty()) return std::nullopt;
    const auto now = clock(); const auto stale = std::chrono::milliseconds(usbdefaults::mouseStaleMs);
    while (mouse.reports.size() > 1 && now - mouse.reports.front().second > stale) mouse.reports.pop_front();
    auto [report, at] = std::move(mouse.reports.front());
    mouse.reports.pop_front();
    return now - at > stale ? usb::vmouse::still(report) : report;
  }
  void answerRead(Device& device, uint32_t kernelSeq, const usb::Bytes& report, std::vector<std::function<void()>>& after) {
    const auto bytes = usb::encodeRetSubmit(kernelSeq, usb::deviceId(device.exported), 1, usb::vmouse::endpoint & 15, 0, report.data(), report.size());
    after.push_back([send = device.toKernel, bytes] { if (send) send(bytes); });
  }

  bool sendChunk(Device& device, Pending& pending) {
    pending.chunk = std::min(limitUrb_, pending.total - pending.moved);
    usb::Frame frame; frame.type = usb::frameSubmit; frame.device = device.id; frame.endpoint = pending.address; frame.transfer = pending.transfer;
    frame.seq = pending.relaySeq; frame.length = uint32_t(pending.chunk); std::memcpy(frame.setup, pending.setup, 8);
    if (pending.transfer != 0) std::memset(frame.setup, 0, 8);
    if (pending.direction == 0 && pending.chunk) frame.payload.assign(pending.out.begin() + long(pending.moved), pending.out.begin() + long(pending.moved + pending.chunk));
    ++inflight_; inflightBytes_ += pending.chunk;
    return hooks_.sendFrame ? hooks_.sendFrame(usb::encodeFrame(frame)) : false;
  }

  bool onReturn(Device& device, const usb::Frame& frame, std::vector<std::function<void()>>& after) {
    const auto found = device.pending.find(frame.seq);
    if (found == device.pending.end() || found->second->address != frame.endpoint || found->second->transfer != frame.transfer) return false;
    // The length is checked before the request leaves `pending`: otherwise a refused
    // return would leave kernelToRelay naming a request that is gone, and the VM's next unlink of it would throw.
    {
      const bool input = found->second->direction == 1;
      if (frame.length > found->second->chunk || frame.payload.size() != (input ? size_t(frame.length) : 0u)) return false;
    }
    std::unique_ptr<Pending> pending = std::move(found->second);
    device.pending.erase(found);
    if (inflight_) --inflight_;
    inflightBytes_ -= std::min(inflightBytes_, pending->chunk);
    const bool input = pending->direction == 1;
    const size_t chunk = pending->chunk;
    const uint32_t devid = usb::deviceId(device.exported);
    ++device.requests; device.kib += uint32_t((pending->moved + frame.length + 1023) / 1024);
    const auto finish = [&](int32_t status) {
      device.kernelToRelay.erase(pending->kernelSeq);
      if (pending->unlinkAnswered) return;     // the VM was told its request is cancelled; this is the late return
      if (pending->unlinking) {
        // A cancelled request is not answered; its unlink is (status -ECONNRESET).
        const auto bytes = usb::encodeRetUnlink(pending->unlinkKernelSeq, devid, -104);
        after.push_back([send = device.toKernel, bytes] { if (send) send(bytes); });
      } else {
        const auto bytes = usb::encodeRetSubmit(pending->kernelSeq, devid, pending->direction, pending->endpoint, status, input ? pending->in.data() : nullptr, input ? pending->in.size() : pending->moved);
        after.push_back([send = device.toKernel, bytes] { if (send) send(bytes); });
      }
    };
    if (frame.status != 0) {
      if (frame.status != -104) { ++device.failed; note(0, UsbStage::urbFailed, false, uint32_t(-frame.status), pending->cls); }   // -ECONNRESET is a cancel, not a failure
      finish(frame.status); return true;
    }
    pending->moved += frame.length;
    if (input) pending->in.insert(pending->in.end(), frame.payload.begin(), frame.payload.end());
    const bool shortRead = input && frame.length < chunk;
    if (pending->unlinking || shortRead || pending->moved >= pending->total) { finish(0); return true; }
    // The next chunk of a transfer larger than one relay frame.
    const uint32_t relaySeq = ++sequence_;
    device.kernelToRelay[pending->kernelSeq] = relaySeq;
    pending->relaySeq = relaySeq;
    Pending& next = *pending;
    device.pending[relaySeq] = std::move(pending);
    return sendChunk(device, next);
  }
};
}  // namespace host
