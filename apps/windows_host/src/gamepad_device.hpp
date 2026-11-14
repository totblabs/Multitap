#pragma once
// gamepad-v1 (docs/remote-session-protocol.md "Gamepads"): the client reads the gamepads of its PC or phone and
// sends their state; the Host plugs in one virtual wired Xbox 360 controller per pad through the pinned
// usbip-win2 controller and its own loopback USB/IP server (as the pointer device, pointer_device.hpp), and
// Windows' own Xbox controller driver (xusb22) takes it, so every XInput game sees an ordinary controller. No new
// driver. The controller's rumble comes back to the client.
//
// The device is the wired Xbox 360 controller as it describes itself on the bus (045E:028E, four vendor
// interfaces; the facts are the device's own, cross-checked with VIIPER's device/xbox360, GPL-3.0, of which no
// code is used here). Its strings are Multitap's. Interrupt IN 0x81 carries the 20-byte input report: a read is
// answered with the newest state when one is waiting, otherwise it waits for the next. OUT 0x01 carries the
// 8-byte rumble report (and LED reports, which are ignored). The other endpoints (headset, plug-in module) are
// held or accepted and carry nothing.
//
// Pads belong to the session that sent them: they go when it ends. Diagnostics: hostAction 250 stages
// gamepadAttached / gamepadFailed / gamepadGone, fixed codes only.
#include "usb_redirection.hpp"
#include <algorithm>
#include <array>
#include <deque>
#include <map>

namespace host::usb::xpad {
inline constexpr uint16_t vendor = 0x045E, product = 0x028E;
inline constexpr size_t reportLength = 20;
inline constexpr int maxPads = 4;
inline constexpr uint8_t inputEndpoint = 0x81, outputEndpoint = 0x01;

inline const Bytes& deviceDescriptor() {
  static const Bytes bytes{18, 0x01, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 8, 0x5E, 0x04, 0x8E, 0x02, 0x14, 0x01, 1, 2, 3, 1};
  return bytes;
}
// Configuration 1, four interfaces, 153 bytes: 0 the controller (input 0x81, output 0x01), 1 the headset,
// 2 the plug-in module, 3 the security method (no endpoints; its string is not named here).
inline const Bytes& configurationDescriptor() {
  static const Bytes bytes{
      0x09, 0x02, 0x99, 0x00, 0x04, 0x01, 0x00, 0xA0, 0xFA,
      0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x5D, 0x01, 0x00,
      0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, 0x01, 0x08, 0x00, 0x00,
      0x07, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,
      0x07, 0x05, 0x01, 0x03, 0x20, 0x00, 0x08,
      0x09, 0x04, 0x01, 0x00, 0x04, 0xFF, 0x5D, 0x03, 0x00,
      0x1B, 0x21, 0x00, 0x01, 0x01, 0x01, 0x82, 0x40, 0x01, 0x02, 0x20, 0x16, 0x83, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x07, 0x05, 0x82, 0x03, 0x20, 0x00, 0x02,
      0x07, 0x05, 0x02, 0x03, 0x20, 0x00, 0x04,
      0x07, 0x05, 0x83, 0x03, 0x20, 0x00, 0x40,
      0x07, 0x05, 0x03, 0x03, 0x20, 0x00, 0x10,
      0x09, 0x04, 0x02, 0x00, 0x01, 0xFF, 0x5D, 0x02, 0x00,
      0x09, 0x21, 0x00, 0x01, 0x01, 0x22, 0x84, 0x07, 0x00,
      0x07, 0x05, 0x84, 0x03, 0x20, 0x00, 0x10,
      0x09, 0x04, 0x03, 0x00, 0x00, 0xFF, 0xFD, 0x13, 0x00,
      0x06, 0x41, 0x00, 0x01, 0x01, 0x03};
  return bytes;
}
// 0 languages (US English), 1 maker, 2 product, 3 serial (one per pad, so Windows keeps one instance per slot).
inline std::optional<Bytes> stringDescriptor(unsigned index, int pad) {
  if (index == 0) return Bytes{4, 0x03, 0x09, 0x04};
  std::string text;
  if (index == 1) text = "ToTB";
  else if (index == 2) text = "Multitap Gamepad";
  else if (index == 3) text = "MULTITAP-PAD-" + std::to_string(pad + 1);
  else return std::nullopt;
  Bytes out{0, 0x03};
  for (const char c : text) { out.push_back(uint8_t(c)); out.push_back(0); }
  out[0] = uint8_t(out.size());
  return out;
}

// XInput's buttons (XINPUT_GAMEPAD_*): the same bits travel in the report and in gamepad-v1.
inline constexpr uint16_t buttonMask = 0xF7FF;   // every defined bit (0x0800 is not one)
struct State {
  uint16_t buttons = 0; uint8_t lt = 0, rt = 0; int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool operator==(const State&) const = default;
};
inline Bytes report(const State& s) {
  Bytes out(reportLength, 0);
  out[0] = 0x00; out[1] = 0x14;
  const auto put16 = [&](size_t at, uint16_t value) { out[at] = uint8_t(value); out[at + 1] = uint8_t(value >> 8); };
  put16(2, uint16_t(s.buttons & buttonMask)); out[4] = s.lt; out[5] = s.rt;
  put16(6, uint16_t(s.lx)); put16(8, uint16_t(s.ly)); put16(10, uint16_t(s.rx)); put16(12, uint16_t(s.ry));
  return out;
}

struct ControlAnswer { int32_t status = -32; Bytes data; };
// The standard requests of a bus-powered full-speed device, and the driver's vendor request for the controller's
// capabilities (bmRequestType C1, bRequest 01, wValue 0100: every button, trigger and stick present). Anything
// else stalls.
inline ControlAnswer control(const uint8_t* setup, int pad) {
  const unsigned type = setup[0], request = setup[1], value = unsigned(setup[2] | setup[3] << 8), length = unsigned(setup[6] | setup[7] << 8);
  std::optional<Bytes> data;
  bool ok = true;
  if (type == 0x80 && request == 6) {
    const unsigned kind = value >> 8, index = value & 0xFF;
    if (kind == 1 && index == 0) data = deviceDescriptor();
    else if (kind == 2 && index == 0) data = configurationDescriptor();
    else if (kind == 3) data = stringDescriptor(index, pad);
    ok = data.has_value();
  }
  else if ((type == 0x80 || type == 0x81 || type == 0x82) && request == 0) data = Bytes{0, 0};   // GET_STATUS
  else if (type == 0x80 && request == 8) data = Bytes{1};                                        // GET_CONFIGURATION
  else if (type == 0x81 && request == 0x0A) data = Bytes{0};                                     // GET_INTERFACE
  else if (type == 0xC1 && request == 0x01 && value == 0x0100)                                   // capabilities (as VIIPER answers, a gamepad)
    data = Bytes{0x00, 0x14, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
  // SET_CONFIGURATION, CLEAR/SET_FEATURE (device, interface, endpoint), SET_INTERFACE.
  else ok = (type == 0x00 && (request == 9 || request == 1 || request == 3)) || (type == 0x01 && (request == 0x0B || request == 1 || request == 3)) ||
            (type == 0x02 && (request == 1 || request == 3));
  if (!ok) return {};
  ControlAnswer answer; answer.status = 0;
  if (data) { answer.data = std::move(*data); if (answer.data.size() > length) answer.data.resize(length); }
  return answer;
}

// The rumble report on OUT 0x01: 00 08 00 <large motor> <small motor> 00 00 00. LED reports (01 03 ..) and
// anything else: nullopt.
struct Rumble { uint8_t low = 0, high = 0; bool operator==(const Rumble&) const = default; };
inline std::optional<Rumble> rumble(const uint8_t* data, size_t size) {
  if (!data || size < 5 || data[0] != 0x00 || data[1] != 0x08) return std::nullopt;
  return Rumble{data[3], data[4]};
}
}  // namespace host::usb::xpad

namespace host::usb {
// The kernel's side of up to four pads (device ids 1..4, busids 1-1..1-4), on one loopback server.
class GamepadDeviceCore {
 public:
  struct Imported { uint8_t id = 0; ExportedDevice exported; };
  static constexpr size_t maxReads = 8;
  static std::string busId(int pad) { return busIdFor(unsigned(pad) + 1); }
  static ExportedDevice exportedDevice(int pad) {
    ExportedDevice device;
    device.busId = busId(pad); device.path = "/Multitap/gamepad/" + std::to_string(pad + 1); device.busNumber = 1; device.deviceNumber = uint32_t(pad + 1);
    device.speed = 2;
    device.vendor = xpad::vendor; device.product = xpad::product; device.bcdDevice = 0x0114;
    device.cls = 0xFF; device.subclass = 0xFF; device.protocol = 0xFF; device.configurationValue = 1; device.configurations = 1; device.interfaces = 4;
    return device;
  }

  std::optional<Imported> importDevice(const std::string& bus, std::function<bool(const Bytes&)> toKernel, std::function<void()> closeKernel = {}) {
    std::lock_guard lock(mutex_);
    for (int pad = 0; pad < xpad::maxPads; ++pad) {
      if (bus != busId(pad)) continue;
      Pad& p = pads_[pad];
      if (p.imported) return std::nullopt;
      p.imported = true; p.toKernel = std::move(toKernel); p.closeKernel = std::move(closeKernel); p.reads.clear(); p.held.clear();
      p.fresh = true;                                  // the first read gets the state as it is now
      return Imported{uint8_t(pad + 1), exportedDevice(pad)};
    }
    return std::nullopt;
  }
  bool submit(uint8_t id, const Urb& urb, Bytes data) {
    std::function<bool(const Bytes&)> send; Bytes reply; std::optional<xpad::Rumble> felt; int pad = -1;
    {
      std::lock_guard lock(mutex_);
      if (id < 1 || id > xpad::maxPads) return false;
      pad = id - 1; Pad& p = pads_[pad];
      if (!p.imported) return false;
      if (urb.packets != -1 && urb.packets != 0) return false;                                  // isochronous
      if (urb.length < 0 || urb.length > 4096) return false;
      if (data.size() != (urb.direction == 1 ? size_t(0) : size_t(urb.length))) return false;
      send = p.toKernel;
      const uint32_t devid = deviceId(exportedDevice(pad));
      const unsigned endpoint = urb.endpoint & 15;
      if (endpoint == 0) {
        const bool in = (urb.setup[0] & 0x80) != 0;
        if (size_t(urb.setup[6] | urb.setup[7] << 8) != size_t(urb.length) || (urb.length != 0 && in != (urb.direction == 1))) return false;
        ++p.controls;
        const auto answer = xpad::control(urb.setup, pad);
        if (answer.status != 0) { ++p.stalled; p.lastStalled = uint32_t(urb.setup[0]) << 8 | urb.setup[1]; }
        reply = encodeRetSubmit(urb.seq, devid, urb.direction, urb.endpoint, answer.status, in ? answer.data.data() : nullptr,
                                in ? answer.data.size() : answer.status == 0 ? size_t(urb.length) : 0);
      } else if (urb.direction == 1 && endpoint == (xpad::inputEndpoint & 15) && size_t(urb.length) >= xpad::reportLength) {
        if (std::find(p.reads.begin(), p.reads.end(), urb.seq) != p.reads.end()) return false;
        ++p.readsSeen;
        if (p.fresh && p.reads.empty()) { p.fresh = false; reply = encodeRetSubmit(urb.seq, devid, 1, endpoint, 0, p.latest.data(), p.latest.size()); }
        else if (p.reads.size() < maxReads) { p.reads.push_back(urb.seq); return true; }
        else reply = encodeRetSubmit(urb.seq, devid, 1, endpoint, -32, nullptr, 0);
      } else if (urb.direction == 1 && endpoint >= 2 && endpoint <= 4) {                         // headset, module: nothing to say
        auto& held = p.held[uint8_t(endpoint)];
        if (std::find(held.begin(), held.end(), urb.seq) != held.end()) return false;
        if (held.size() < 4) { held.push_back(urb.seq); return true; }
        reply = encodeRetSubmit(urb.seq, devid, 1, endpoint, -32, nullptr, 0);
      } else if (urb.direction == 0 && endpoint >= 1 && endpoint <= 3) {                         // rumble and LED, headset out
        if (endpoint == (xpad::outputEndpoint & 15)) felt = xpad::rumble(data.data(), data.size());
        reply = encodeRetSubmit(urb.seq, devid, 0, endpoint, 0, nullptr, size_t(urb.length));
      } else reply = encodeRetSubmit(urb.seq, devid, urb.direction, endpoint, -32, nullptr, 0);
    }
    if (felt) { std::function<void(int, uint8_t, uint8_t)> hook; { std::lock_guard lock(mutex_); hook = rumble_; } if (hook) hook(pad, felt->low, felt->high); }
    return !send || send(reply);
  }
  bool unlink(uint8_t id, const Urb& urb) {
    std::function<bool(const Bytes&)> send; Bytes reply;
    {
      std::lock_guard lock(mutex_);
      if (id < 1 || id > xpad::maxPads || !pads_[id - 1].imported) return false;
      Pad& p = pads_[id - 1];
      bool waiting = false;
      if (const auto it = std::find(p.reads.begin(), p.reads.end(), urb.target); it != p.reads.end()) { p.reads.erase(it); waiting = true; }
      for (auto& [endpoint, held] : p.held) { (void)endpoint; if (const auto it = std::find(held.begin(), held.end(), urb.target); it != held.end()) { held.erase(it); waiting = true; } }
      send = p.toKernel;
      reply = encodeRetUnlink(urb.seq, deviceId(exportedDevice(id - 1)), waiting ? -104 : 0);
    }
    return !send || send(reply);
  }
  // On the kernel connection's own thread, after its last request: no other thread is inside a send or a close of
  // that connection when this returns (kernel_), so the server may free it.
  void kernelClosed(uint8_t id) {
    std::function<void(int)> gone;
    {
      std::lock_guard kernel(kernel_);
      std::lock_guard lock(mutex_);
      if (id < 1 || id > xpad::maxPads || !pads_[id - 1].imported) return;
      Pad& p = pads_[id - 1];
      p.imported = false; p.toKernel = {}; p.closeKernel = {}; p.reads.clear(); p.held.clear();
      gone = gone_;
    }
    if (gone) gone(id - 1);
  }
  // The newest state of [pad]: answers a waiting read, or the next one. Kept while the pad is not imported yet.
  // From the service's threads: the reply goes out under kernel_, so the connection cannot be freed meanwhile.
  void state(int pad, const xpad::State& s) {
    if (pad < 0 || pad >= xpad::maxPads) return;
    std::lock_guard kernel(kernel_);
    std::function<bool(const Bytes&)> send; Bytes reply;
    {
      std::lock_guard lock(mutex_);
      Pad& p = pads_[pad];
      p.latest = xpad::report(s);
      if (p.imported && !p.reads.empty()) {
        const uint32_t seq = p.reads.front(); p.reads.pop_front(); p.fresh = false;
        send = p.toKernel; reply = encodeRetSubmit(seq, deviceId(exportedDevice(pad)), 1, xpad::inputEndpoint & 15, 0, p.latest.data(), p.latest.size());
      } else p.fresh = true;
    }
    if (send) send(reply);
  }
  // Ends [pad]'s kernel connection (its port could not be unplugged). Under kernel_ for the same reason as state().
  void close(int pad) {
    std::lock_guard kernel(kernel_);
    std::function<void()> close;
    { std::lock_guard lock(mutex_); if (pad >= 0 && pad < xpad::maxPads) close = pads_[pad].closeKernel; }
    if (close) close();
  }
  void onRumble(std::function<void(int pad, uint8_t low, uint8_t high)> hook) { std::lock_guard lock(mutex_); rumble_ = std::move(hook); }
  void onGone(std::function<void(int pad)> hook) { std::lock_guard lock(mutex_); gone_ = std::move(hook); }
  bool imported(int pad) const { std::lock_guard lock(mutex_); return pad >= 0 && pad < xpad::maxPads && pads_[pad].imported; }
  size_t waitingReads(int pad) const { std::lock_guard lock(mutex_); return pads_[pad].reads.size(); }
  uint32_t controls(int pad) const { std::lock_guard lock(mutex_); return pads_[pad].controls; }
  uint32_t stalled(int pad) const { std::lock_guard lock(mutex_); return pads_[pad].stalled; }
  uint32_t lastStalled(int pad) const { std::lock_guard lock(mutex_); return pads_[pad].lastStalled; }
  uint32_t readsSeen(int pad) const { std::lock_guard lock(mutex_); return pads_[pad].readsSeen; }
 private:
  struct Pad {
    bool imported = false, fresh = true; Bytes latest = xpad::report({});
    std::function<bool(const Bytes&)> toKernel; std::function<void()> closeKernel;
    std::deque<uint32_t> reads; std::map<uint8_t, std::deque<uint32_t>> held;
    uint32_t controls = 0, stalled = 0, lastStalled = 0, readsSeen = 0;
  };
  mutable std::mutex mutex_;
  // Held by another thread's send to (or close of) a kernel connection, and by that connection's end (kernelClosed):
  // the server frees a connection only after its end, so it never frees one in use. Taken before mutex_.
  std::mutex kernel_;
  std::array<Pad, xpad::maxPads> pads_;
  std::function<void(int, uint8_t, uint8_t)> rumble_;
  std::function<void(int)> gone_;
};

// Why a pad could not be plugged in (hostAction 250 stage gamepadFailed, error): as the pointer device.
inline constexpr uint32_t gamepadServerFailed = 20, gamepadAttachFailed = 21;

// The service's owner of the pads. Plugging in and unplugging run in order on one worker thread; a state never
// waits for them.
class GamepadDevices {
 public:
  struct Options {
    DriverAccess driver = nativeDriverAccess();
    std::function<int(const std::string&, uint16_t, const std::string&)> attach = [](const std::string& host, uint16_t port, const std::string& bus) { return vhciAttach(host, port, bus); };
    std::function<bool(int)> detach = [](int port) { return vhciDetach(port); };
    std::function<void()> cancelAttach = [] { vhciCancelAttach(); };
    UsbIpAccess server;
  };
  explicit GamepadDevices(Options options = {}) : options_(std::move(options)) {
    core_.onGone([this](int pad) {
      int port = 0;
      // The next state plugs it in again.
      { std::lock_guard lock(mutex_); if (stopped_ || pad < 0 || pad >= xpad::maxPads) return; port = ports_[pad]; ports_[pad] = 0; if (port) wanted_[pad] = false; }
      if (port) { keptPorts().remove(port); recordUsbEvent(UsbStage::gamepadGone, true, 0, uint32_t(pad + 1)); }
    });
    worker_ = std::thread([this] { run(); });
  }
  ~GamepadDevices() { stop(); }
  GamepadDevices(const GamepadDevices&) = delete;
  GamepadDevices& operator=(const GamepadDevices&) = delete;

  // The newest state of [pad] (0..3). The first one plugs the pad in; until Windows imports it, the state waits.
  void state(int pad, const xpad::State& s) {
    if (pad < 0 || pad >= xpad::maxPads) return;
    core_.state(pad, s);
    std::lock_guard lock(mutex_);
    // The wish is recorded also for a pad still plugged in: a removal waiting for the worker is overtaken.
    if (stopped_ || wanted_[pad] || failed_[pad]) return;
    wanted_[pad] = true; queue(pad);
  }
  // The client's pad went away: unplugged. A pad that could not be plugged in stays so for the session.
  void remove(int pad) { remove(pad, false); }
  // The session ended: every pad goes, and a pad that could not be plugged in may be tried again next time.
  void removeAll() { for (int pad = 0; pad < xpad::maxPads; ++pad) remove(pad, true); }
  // Nobody is there to hold the pads (the client or the Host's channel is away): each pad plugged in, or being
  // plugged in, reads neutral; none is plugged in or out.
  void neutralAll() {
    for (int pad = 0; pad < xpad::maxPads; ++pad) {
      bool held = false;
      { std::lock_guard lock(mutex_); held = !stopped_ && (ports_[pad] > 0 || wanted_[pad]); }
      if (held) core_.state(pad, {});
    }
  }
  void stop() {
    {
      std::lock_guard lock(mutex_);
      if (stopped_) return;
      stopped_ = true; stopping_ = true; jobs_.clear(); queued_.fill(false);
    }
    wake_.notify_all();
    if (options_.cancelAttach) try { options_.cancelAttach(); } catch (...) { }
    if (worker_.joinable()) worker_.join();
    for (int pad = 0; pad < xpad::maxPads; ++pad) unplug(pad);
    server_.stop();
  }
  // Rumble the VM's game asked for (from the kernel's connection thread).
  void onRumble(std::function<void(int pad, uint8_t low, uint8_t high)> hook) { core_.onRumble(std::move(hook)); }
  bool plugged(int pad) const { std::lock_guard lock(mutex_); return pad >= 0 && pad < xpad::maxPads && ports_[pad] > 0 && core_.imported(pad); }
  int port(int pad) const { std::lock_guard lock(mutex_); return pad >= 0 && pad < xpad::maxPads ? ports_[pad] : 0; }
  bool idle() const { std::lock_guard lock(mutex_); return jobs_.empty() && !busy_; }
  const GamepadDeviceCore& core() const { return core_; }

 private:
  Options options_;
  GamepadDeviceCore core_;
  BasicUsbIpServer<GamepadDeviceCore> server_;
  uint16_t serverPort_ = 0;                                              // worker thread only
  // The pads whose wish (wanted_) changed, each at most once: a client that plugs and unplugs a pad without end
  // queues no more than four jobs, and the worker does only what the latest wish needs.
  mutable std::mutex mutex_; std::condition_variable wake_; std::deque<int> jobs_; std::thread worker_;
  std::array<int, xpad::maxPads> ports_{}; std::array<bool, xpad::maxPads> wanted_{}, failed_{}, queued_{};
  bool stopped_ = false, busy_ = false; std::atomic<bool> stopping_{false};

  void remove(int pad, bool forgetFailure) {
    if (pad < 0 || pad >= xpad::maxPads) return;
    core_.state(pad, {});
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    wanted_[pad] = false; if (forgetFailure) failed_[pad] = false; queue(pad);
  }
  void queue(int pad) {                                                  // under mutex_
    if (queued_[pad]) return;
    queued_[pad] = true; jobs_.push_back(pad); wake_.notify_one();
  }
  void run() {
    for (;;) {
      int pad = 0; bool plugIn = false, unplugOut = false;
      {
        std::unique_lock lock(mutex_);
        busy_ = false;
        wake_.wait(lock, [this] { return stopped_ || !jobs_.empty(); });
        if (stopped_) return;
        pad = jobs_.front(); jobs_.pop_front(); queued_[pad] = false; busy_ = true;
        plugIn = wanted_[pad] && ports_[pad] == 0 && !failed_[pad];
        unplugOut = !wanted_[pad] && ports_[pad] > 0;
      }
      if (plugIn) plug(pad); else if (unplugOut) unplug(pad);
    }
  }
  void unplug(int pad) {
    int port = 0;
    { std::lock_guard lock(mutex_); port = ports_[pad]; ports_[pad] = 0; }
    if (port <= 0) return;
    keptPorts().remove(port);
    bool unplugged = false;
    try { unplugged = options_.detach && options_.detach(port); } catch (...) { }
    if (!unplugged) core_.close(pad);
  }
  void plug(int pad) {
    uint32_t failed = 0, cause = 0;
    try {
      std::lock_guard serial(controllerLock());
      const auto prepared = prepareController(options_.driver, stopping_, false);
      if (prepared != Prepare::ready) failed = uint32_t(prepared);
      else {
        if (!serverPort_) serverPort_ = server_.start(core_, options_.server);
        if (!serverPort_) { failed = gamepadServerFailed; cause = uint32_t(WSAGetLastError()); }
        else if (!stopping_) {
          const int port = options_.attach ? options_.attach("127.0.0.1", serverPort_, GamepadDeviceCore::busId(pad)) : 0;
          if (port <= 0) failed = gamepadAttachFailed;
          else {
            keptPorts().add(port);
            bool keep = false;
            { std::lock_guard lock(mutex_); keep = !stopped_ && wanted_[pad]; if (keep) ports_[pad] = port; }
            if (!keep) { keptPorts().remove(port); try { if (options_.detach) options_.detach(port); } catch (...) { } }
            else recordUsbEvent(UsbStage::gamepadAttached, true, 0, uint32_t(pad + 1));
          }
        }
      }
    } catch (...) { failed = gamepadAttachFailed; cause = 1; }
    if (failed) {
      recordUsbEvent(UsbStage::gamepadFailed, false, failed, cause);
      std::lock_guard lock(mutex_);
      failed_[pad] = true; wanted_[pad] = false;                         // not again in this session
    }
  }
};

// The service's pads (set while the service runs; null otherwise).
inline std::atomic<GamepadDevices*>& serviceGamepads() { static std::atomic<GamepadDevices*> devices{nullptr}; return devices; }
}  // namespace host::usb

namespace host {
inline constexpr const char* gamepadProtocolName = "gamepad-v1";
// Advertised where the USB controller can be used at all (a workstation Windows with the pinned package).
inline std::string gamepadCapabilityFields() { return usb::usbRedirectionAvailable() ? std::string(",\"gamepadProtocol\":\"") + gamepadProtocolName + '"' : std::string(); }
inline bool gamepadGranted(const Json& start) { return start.get("mode").string() == "stream" && start.get("gamepadProtocol").string() == gamepadProtocolName; }

namespace gamepad_detail {
inline std::optional<int64_t> integer(const Json& value, int64_t low, int64_t high) {
  const auto* number = std::get_if<double>(&value.value);
  if (!number || !std::isfinite(*number) || std::floor(*number) != *number || *number < double(low) || *number > double(high)) return std::nullopt;
  return int64_t(*number);
}
inline bool only(const Json& message, std::initializer_list<const char*> keys) {
  const auto* fields = std::get_if<Json::Object>(&message.value);
  if (!fields) return false;
  for (const auto& [key, item] : *fields) { (void)item; if (std::none_of(keys.begin(), keys.end(), [&](const char* allowed) { return key == allowed; })) return false; }
  return true;
}
}  // namespace gamepad_detail
// {"type":"gamepad.state","sessionId","pad","buttons","lt","rt","lx","ly","rx","ry"}: exactly, in XInput's ranges.
inline std::optional<std::pair<int, usb::xpad::State>> gamepadStateMessage(const Json& message) {
  using namespace gamepad_detail;
  if (message.get("type").string() != "gamepad.state" || !only(message, {"type", "sessionId", "pad", "buttons", "lt", "rt", "lx", "ly", "rx", "ry"})) return std::nullopt;
  const auto pad = integer(message.get("pad"), 0, usb::xpad::maxPads - 1), buttons = integer(message.get("buttons"), 0, 0xFFFF), lt = integer(message.get("lt"), 0, 255), rt = integer(message.get("rt"), 0, 255);
  const auto lx = integer(message.get("lx"), -32768, 32767), ly = integer(message.get("ly"), -32768, 32767), rx = integer(message.get("rx"), -32768, 32767), ry = integer(message.get("ry"), -32768, 32767);
  if (!pad || !buttons || (*buttons & 0x0800) || !lt || !rt || !lx || !ly || !rx || !ry) return std::nullopt;
  usb::xpad::State state; state.buttons = uint16_t(*buttons); state.lt = uint8_t(*lt); state.rt = uint8_t(*rt);
  state.lx = int16_t(*lx); state.ly = int16_t(*ly); state.rx = int16_t(*rx); state.ry = int16_t(*ry);
  return std::pair{int(*pad), state};
}
// {"type":"gamepad.remove","sessionId","pad"}.
inline std::optional<int> gamepadRemoveMessage(const Json& message) {
  using namespace gamepad_detail;
  if (message.get("type").string() != "gamepad.remove" || !only(message, {"type", "sessionId", "pad"})) return std::nullopt;
  const auto pad = integer(message.get("pad"), 0, usb::xpad::maxPads - 1);
  if (!pad) return std::nullopt;
  return int(*pad);
}
inline std::string gamepadRumbleMessage(const std::string& sessionId, int pad, uint8_t low, uint8_t high) {
  return "{\"type\":\"gamepad.rumble\",\"sessionId\":" + jsonQuote(sessionId) + ",\"pad\":" + std::to_string(pad) + ",\"low\":" + std::to_string(low) + ",\"high\":" + std::to_string(high) + '}';
}
}  // namespace host
