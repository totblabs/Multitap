#pragma once
// The Host's own pointer device: a USB mouse that never moves, for a PC with no mouse attached.
//
// Windows hides its pointer on a PC with no mouse (GetSystemMetrics(SM_MOUSEPRESENT) == 0): GetCursorInfo has
// no CURSOR_SHOWING and no cursor handle, and the absolute positions this Host injects for a free mouse do not
// bring it back (the relative moves of a held mouse do). Seen on a Host on a physical PC on 2026-11-14
// (hostAction 200 stage 248: noMouseTicks and notShowing on every tick with the mouse free). A VM always has a
// mouse (QEMU's PS/2 mouse and USB tablet), as an RDP session has its own. Here the Host plugs in a mouse of its
// own through the pinned usbip-win2 controller and the loopback USB/IP server of usb-v1 (usb_redirection.hpp,
// usb_driver.hpp): Windows then has a mouse and shows the pointer. No new driver, nothing else on the machine
// changes.
//
// - Asked for only by a stream's worker that sees no mouse (worker.pointer.absent, once per capture epoch): a
//   PC with a mouse never gets it, and the controller is never installed for it there.
// - It stays while the service runs (later streams find it) and goes when the service stops. A sweep of
//   ports left attached (a USB session's start, the service's start) never takes it (keptPort).
// - It sends no report: Windows' interrupt reads wait here until Windows cancels them. Control requests are
//   answered with the descriptors of usb_virtual_mouse.hpp under its own product number and strings.
// - Diagnostics: hostAction 250 stages pointerAttached / pointerFailed / pointerGone, fixed codes only.
#include "usb_redirection.hpp"
#include <algorithm>
#include <deque>

namespace host::usb {
// The kernel's side of the one device (called from the loopback server's connection thread).
class PointerDeviceCore {
 public:
  struct Imported { uint8_t id = 0; ExportedDevice exported; };
  static constexpr uint8_t deviceId = 1;
  static constexpr size_t maxReads = 32;   // Windows' HID class keeps two; anything above this is stalled
  static std::string busId() { return busIdFor(1); }
  static ExportedDevice exportedDevice() {
    ExportedDevice device;
    device.busId = busId(); device.path = "/Multitap/pointer"; device.busNumber = 1; device.deviceNumber = 1;
    device.speed = 2;                                                    // full speed, as a USB mouse is
    device.vendor = vmouse::vendor; device.product = vmouse::pointer.product; device.bcdDevice = 0x0100;
    device.cls = 0; device.subclass = 0; device.protocol = 0; device.configurationValue = 1; device.configurations = 1; device.interfaces = 1;
    return device;
  }

  // OP_REQ_IMPORT: only our busid, only while nothing holds it.
  std::optional<Imported> importDevice(const std::string& bus, std::function<bool(const Bytes&)> toKernel, std::function<void()> closeKernel = {}) {
    std::lock_guard lock(mutex_);
    if (bus != busId() || imported_) return std::nullopt;
    imported_ = true; toKernel_ = std::move(toKernel); closeKernel_ = std::move(closeKernel); reads_.clear();
    return Imported{deviceId, exportedDevice()};
  }
  // CMD_SUBMIT. false: the connection must end (a malformed or impossible request).
  bool submit(uint8_t id, const Urb& urb, Bytes data) {
    std::function<bool(const Bytes&)> send; Bytes reply;
    {
      std::lock_guard lock(mutex_);
      if (id != deviceId || !imported_) return false;
      if (urb.packets != -1 && urb.packets != 0) return false;                                   // isochronous
      if (urb.length < 0 || urb.length > 4096) return false;
      if (data.size() != (urb.direction == 1 ? size_t(0) : size_t(urb.length))) return false;
      send = toKernel_;
      const uint32_t devid = usb::deviceId(exportedDevice());
      if ((urb.endpoint & 15) == 0) {
        const bool in = (urb.setup[0] & 0x80) != 0;
        if (size_t(urb.setup[6] | urb.setup[7] << 8) != size_t(urb.length) || (urb.length != 0 && in != (urb.direction == 1))) return false;
        ++controls_;
        const auto answer = vmouse::control(urb.setup, Bytes(vmouse::reportLength, 0), vmouse::pointer);
        reply = encodeRetSubmit(urb.seq, devid, urb.direction, urb.endpoint, answer.status, in ? answer.data.data() : nullptr,
                                in ? answer.data.size() : answer.status == 0 ? size_t(urb.length) : 0);
      } else if (urb.direction == 1 && (urb.endpoint & 15) == (vmouse::endpoint & 15) && size_t(urb.length) >= vmouse::reportLength && reads_.size() < maxReads) {
        if (std::find(reads_.begin(), reads_.end(), urb.seq) != reads_.end()) return false;  // a repeated sequence number
        reads_.push_back(urb.seq); ++readsSeen_;
        return true;                                                                            // waits: the mouse never moves
      } else reply = encodeRetSubmit(urb.seq, devid, urb.direction, urb.endpoint, -32, nullptr, 0);
    }
    return !send || send(reply);
  }
  // CMD_UNLINK: a waiting read is cancelled (-ECONNRESET); anything else was already answered (0).
  bool unlink(uint8_t id, const Urb& urb) {
    std::function<bool(const Bytes&)> send; Bytes reply;
    {
      std::lock_guard lock(mutex_);
      if (id != deviceId || !imported_) return false;
      const auto held = std::find(reads_.begin(), reads_.end(), urb.target);
      const bool waiting = held != reads_.end();
      if (waiting) reads_.erase(held);
      send = toKernel_;
      reply = encodeRetUnlink(urb.seq, usb::deviceId(exportedDevice()), waiting ? -104 : 0);
    }
    return !send || send(reply);
  }
  // The kernel's connection ended: the device is gone (unplugged, or the controller went away).
  void kernelClosed(uint8_t id) {
    std::function<void()> gone;
    {
      std::lock_guard lock(mutex_);
      if (id != deviceId || !imported_) return;
      imported_ = false; toKernel_ = {}; closeKernel_ = {}; reads_.clear();
      gone = gone_;
    }
    if (gone) gone();
  }
  // Ends the kernel's connection now (the service stops and the port could not be unplugged).
  void close() {
    std::function<void()> close;
    { std::lock_guard lock(mutex_); close = closeKernel_; }
    if (close) close();
  }
  void onGone(std::function<void()> hook) { std::lock_guard lock(mutex_); gone_ = std::move(hook); }
  bool imported() const { std::lock_guard lock(mutex_); return imported_; }
  size_t waitingReads() const { std::lock_guard lock(mutex_); return reads_.size(); }
  uint32_t controls() const { std::lock_guard lock(mutex_); return controls_; }
  uint32_t readsSeen() const { std::lock_guard lock(mutex_); return readsSeen_; }
 private:
  mutable std::mutex mutex_;
  bool imported_ = false; std::function<bool(const Bytes&)> toKernel_; std::function<void()> closeKernel_, gone_;
  std::deque<uint32_t> reads_; uint32_t controls_ = 0, readsSeen_ = 0;
};

// Why the device could not be plugged in (hostAction 250 stage pointerFailed, error): 1..7 the controller's
// preparation (the Prepare value: packageMissing 1 ... protectionFailed 7), 20 the loopback server did not
// start, 21 the controller did not plug it in.
inline constexpr uint32_t pointerServerFailed = 20, pointerAttachFailed = 21;

// The service's owner of the device. ask() returns at once; the device is plugged in on a worker thread.
class PointerDevice {
 public:
  struct Options {
    DriverAccess driver = nativeDriverAccess();
    std::function<int(const std::string&, uint16_t, const std::string&)> attach = [](const std::string& host, uint16_t port, const std::string& bus) { return vhciAttach(host, port, bus); };
    std::function<bool(int)> detach = [](int port) { return vhciDetach(port); };
    std::function<void()> cancelAttach = [] { vhciCancelAttach(); };
    UsbIpAccess server;
  };
  explicit PointerDevice(Options options = {}) : options_(std::move(options)) {
    core_.onGone([this] {
      int port = 0;
      { std::lock_guard lock(mutex_); if (stopped_) return; port = port_; port_ = 0; }
      if (port) { keptPorts().remove(port); recordUsbEvent(UsbStage::pointerGone, true); }
    });
  }
  ~PointerDevice() { stop(); }
  PointerDevice(const PointerDevice&) = delete;
  PointerDevice& operator=(const PointerDevice&) = delete;

  // A stream saw no mouse. Nothing happens while the device is plugged in or on its way, or after stop().
  void ask() {
    std::lock_guard lock(mutex_);
    if (stopped_ || busy_ || (port_ > 0 && core_.imported())) return;
    if (worker_.joinable()) worker_.join();                             // the last attempt has ended (busy_ is false)
    busy_ = true;
    worker_ = std::thread([this] { plug(); });
  }
  // Unplugs the device and ends its server. Later asks do nothing.
  void stop() {
    int port = 0; bool busy = false;
    { std::lock_guard lock(mutex_); if (stopped_) return; stopped_ = true; stopping_ = true; busy = busy_; }
    if (busy && options_.cancelAttach) options_.cancelAttach();
    if (worker_.joinable()) worker_.join();
    { std::lock_guard lock(mutex_); port = port_; port_ = 0; }
    if (port > 0) {
      keptPorts().remove(port);
      bool unplugged = false;
      try { unplugged = options_.detach && options_.detach(port); } catch (...) { }
      if (!unplugged) core_.close();
    }
    server_.stop();
  }
  int port() const { std::lock_guard lock(mutex_); return port_; }
  bool plugged() const { std::lock_guard lock(mutex_); return port_ > 0 && core_.imported(); }
  const PointerDeviceCore& core() const { return core_; }

 private:
  Options options_;
  PointerDeviceCore core_;
  BasicUsbIpServer<PointerDeviceCore> server_;
  uint16_t serverPort_ = 0;                                              // worker thread only
  mutable std::mutex mutex_; std::thread worker_; bool busy_ = false, stopped_ = false; int port_ = 0;
  std::atomic<bool> stopping_{false};

  void plug() {
    uint32_t failed = 0, cause = 0;
    try {
      // From the preparation until the port is kept: no sweep of a USB session runs in between.
      std::lock_guard serial(controllerLock());
      int stale = 0;
      { std::lock_guard lock(mutex_); stale = port_; port_ = 0; }
      if (stale > 0) { keptPorts().remove(stale); try { if (options_.detach) options_.detach(stale); } catch (...) { } }
      // Its own preparation never sweeps: a USB session's devices may be attached now.
      const auto prepared = prepareController(options_.driver, stopping_, false);
      if (prepared != Prepare::ready) failed = uint32_t(prepared);
      else {
        if (!serverPort_) serverPort_ = server_.start(core_, options_.server);
        if (!serverPort_) { failed = pointerServerFailed; cause = uint32_t(WSAGetLastError()); }
        else if (!stopping_) {
          const int port = options_.attach ? options_.attach("127.0.0.1", serverPort_, PointerDeviceCore::busId()) : 0;
          if (port <= 0) failed = pointerAttachFailed;
          else {
            keptPorts().add(port);
            bool late = false;
            { std::lock_guard lock(mutex_); late = stopped_; if (!late) port_ = port; }
            if (late) { keptPorts().remove(port); try { if (options_.detach) options_.detach(port); } catch (...) { } }
            else recordUsbEvent(UsbStage::pointerAttached, true, 0, uint32_t(port));
          }
        }
      }
    } catch (...) { failed = pointerAttachFailed; cause = 1; }
    if (failed) recordUsbEvent(UsbStage::pointerFailed, false, failed, cause);
    std::lock_guard lock(mutex_);
    busy_ = false;
  }
};

// The service's one device (set while the service runs; null otherwise).
inline std::atomic<PointerDevice*>& servicePointerDevice() { static std::atomic<PointerDevice*> device{nullptr}; return device; }
}  // namespace host::usb
