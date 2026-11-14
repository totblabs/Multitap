// gamepad-v1 on the Host (gamepad_device.hpp): the controller's descriptors and answers, the input report, the
// rumble report, the kernel's side of four pads (reads that wait for a state, cancels, malformed requests), the
// loopback server with a fake kernel, the service's owner with a substituted controller (plugged in on the first
// state, never twice, unplugged on removal and at the session's end, a failed plug-in not tried again in the
// session), and the messages the broker relays. Nothing is installed or attached on this machine.
#include <winsock2.h>
#include "gamepad_device.hpp"
#include <array>
#include <cstring>
#include <iostream>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
bool waitFor(const std::function<bool()>& condition, int milliseconds = 3000) {
  for (int waited = 0; waited < milliseconds; waited += 5) { if (condition()) return true; Sleep(5); }
  return condition();
}
uint32_t devidOf(int pad) { return 0x00010000u | uint32_t(pad + 1); }
usb::Urb submitUrb(int pad, uint32_t seq, uint32_t direction, uint32_t endpoint, int32_t length, std::array<uint8_t, 8> setup = {}) {
  usb::Urb urb; urb.command = usb::cmdSubmit; urb.seq = seq; urb.devid = devidOf(pad); urb.direction = direction; urb.endpoint = endpoint; urb.length = length; urb.packets = -1;
  std::memcpy(urb.setup, setup.data(), 8); return urb;
}
usb::Urb unlinkUrb(int pad, uint32_t seq, uint32_t target) { usb::Urb urb; urb.command = usb::cmdUnlink; urb.seq = seq; urb.devid = devidOf(pad); urb.target = target; return urb; }
int32_t status(const usb::Bytes& reply) { return int32_t(usb::get32(&reply[20])); }
usb::Bytes payload(const usb::Bytes& reply) { return usb::Bytes(reply.begin() + 48, reply.end()); }

void descriptors() {
  const auto& device = usb::xpad::deviceDescriptor(); const auto& config = usb::xpad::configurationDescriptor();
  require(device.size() == 18 && device[4] == 0xFF && device[8] == 0x5E && device[9] == 0x04 && device[10] == 0x8E && device[11] == 0x02, "The device is not a wired Xbox 360 controller");
  require(config.size() == 153 && config[2] == 153 && config[3] == 0 && config[4] == 4, "The configuration is not four interfaces in 153 bytes");
  // Every descriptor's length adds up to the whole.
  size_t at = 0, interfaces = 0, endpoints = 0;
  while (at < config.size()) { require(config[at] >= 2 && at + config[at] <= config.size(), "A descriptor overruns the configuration"); if (config[at + 1] == 4) ++interfaces; if (config[at + 1] == 5) ++endpoints; at += config[at]; }
  require(at == config.size() && interfaces == 4 && endpoints == 7, "The configuration's descriptors do not add up");
  usb::xpad::State s; s.buttons = 0x1000 | 0x0001; s.lt = 255; s.rt = 1; s.lx = -32768; s.ly = 32767; s.rx = 258; s.ry = -2;
  const auto report = usb::xpad::report(s);
  require(report == usb::Bytes{0x00, 0x14, 0x01, 0x10, 0xFF, 0x01, 0x00, 0x80, 0xFF, 0x7F, 0x02, 0x01, 0xFE, 0xFF, 0, 0, 0, 0, 0, 0}, "The input report is not XInput's layout");
  s.buttons = 0xFFFF; require(usb::xpad::report(s)[3] == 0xF7, "An undefined button bit reached the report");
  const uint8_t big[8] = {0x00, 0x08, 0x00, 0xC8, 0x1E, 0, 0, 0}, led[3] = {0x01, 0x03, 0x06};
  require(usb::xpad::rumble(big, 8) == usb::xpad::Rumble{0xC8, 0x1E} && !usb::xpad::rumble(led, 3) && !usb::xpad::rumble(big, 4), "The rumble report was not read exactly");
  const uint8_t caps[8] = {0xC1, 0x01, 0x00, 0x01, 0x00, 0x00, 0x14, 0x00}, unknown[8] = {0xC0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00};
  require(usb::xpad::control(caps, 0).status == 0 && usb::xpad::control(caps, 0).data.size() == 20, "The capability request was not answered");
  require(usb::xpad::control(unknown, 0).status == -32, "An unknown vendor request was not stalled");
  const uint8_t serial[8] = {0x80, 0x06, 0x03, 0x03, 0x09, 0x04, 0xFF, 0x00};
  const auto answer = usb::xpad::control(serial, 2);
  require(answer.status == 0 && answer.data.size() == 2 + 14 * 2 && answer.data[2 + 13 * 2] == '3', "The serial does not name its pad");
}

void core() {
  usb::GamepadDeviceCore core; std::vector<usb::Bytes> writes; std::vector<int> gone; std::vector<std::array<int, 3>> felt;
  const auto toKernel = [&](const usb::Bytes& bytes) { writes.push_back(bytes); return true; };
  core.onGone([&](int pad) { gone.push_back(pad); });
  core.onRumble([&](int pad, uint8_t low, uint8_t high) { felt.push_back({pad, low, high}); });
  require(!core.importDevice("1-5", toKernel) && !core.importDevice("2-1", toKernel), "A busid outside the four pads was imported");
  usb::xpad::State early; early.buttons = 0x2000;
  core.state(1, early);                                   // before the import: kept for the first read
  const auto second = core.importDevice("1-2", toKernel);
  require(second && second->id == 2 && second->exported.vendor == 0x045E && second->exported.product == 0x028E && second->exported.interfaces == 4 && usb::deviceId(second->exported) == devidOf(1), "Pad 2 was not exported");
  require(!core.importDevice("1-2", toKernel), "A pad was imported twice");
  require(core.submit(2, submitUrb(1, 1, 1, 0, 18, {0x80, 6, 0, 1, 0, 0, 18, 0}), {}) && payload(writes.back()) == usb::xpad::deviceDescriptor(), "The device descriptor did not come back");
  require(core.submit(2, submitUrb(1, 2, 1, 0, 153, {0x80, 6, 0, 2, 0, 0, 153, 0}), {}) && payload(writes.back()) == usb::xpad::configurationDescriptor(), "The configuration did not come back");
  require(core.submit(2, submitUrb(1, 3, 0, 0, 0, {0x00, 9, 1, 0, 0, 0, 0, 0}), {}) && status(writes.back()) == 0, "SET_CONFIGURATION was not accepted");
  // The first read takes the state that waited; the next waits for a new one.
  require(core.submit(2, submitUrb(1, 10, 1, 1, 32), {}) && usb::get32(&writes.back()[4]) == 10 && payload(writes.back()) == usb::xpad::report(early), "The first read did not get the waiting state");
  const auto before = writes.size();
  require(core.submit(2, submitUrb(1, 11, 1, 1, 32), {}) && writes.size() == before && core.waitingReads(1) == 1, "A read was answered without a new state");
  usb::xpad::State next; next.buttons = 0x0010; next.ry = -100;
  core.state(1, next);
  require(writes.size() == before + 1 && usb::get32(&writes.back()[4]) == 11 && payload(writes.back()) == usb::xpad::report(next), "A new state did not answer the waiting read");
  core.state(1, next);                                    // no read waiting: kept for the next
  require(core.submit(2, submitUrb(1, 12, 1, 1, 32), {}) && usb::get32(&writes.back()[4]) == 12, "A state with no read waiting was lost");
  require(core.submit(2, submitUrb(1, 13, 1, 1, 32), {}) && core.waitingReads(1) == 1, "A read did not wait");
  require(!core.submit(2, submitUrb(1, 13, 1, 1, 32), {}), "A repeated sequence number was accepted");
  require(core.submit(2, submitUrb(1, 14, 1, 1, 8), {}) && status(writes.back()) == -32, "A read too short for a report was not stalled");
  // Headset and module reads wait; their cancel is -ECONNRESET.
  require(core.submit(2, submitUrb(1, 20, 1, 2, 32), {}) && core.unlink(2, unlinkUrb(1, 21, 20)) && usb::get32(&writes.back()[0]) == usb::retUnlink && status(writes.back()) == -104, "A held read was not cancelled");
  require(core.unlink(2, unlinkUrb(1, 22, 99)) && status(writes.back()) == 0, "A cancel of nothing was not answered 0");
  // Rumble on OUT 0x01 is told once per report; the LED report is accepted and ignored.
  require(core.submit(2, submitUrb(1, 30, 0, 1, 8), {0x00, 0x08, 0x00, 0x80, 0x40, 0, 0, 0}) && status(writes.back()) == 0 && usb::get32(&writes.back()[24]) == 8, "The rumble report was not accepted");
  require(felt.size() == 1 && felt[0] == std::array<int, 3>{1, 0x80, 0x40}, "The rumble was not told");
  require(core.submit(2, submitUrb(1, 31, 0, 1, 3), {0x01, 0x03, 0x06}) && status(writes.back()) == 0 && felt.size() == 1, "The LED report was taken for rumble");
  require(core.submit(2, submitUrb(1, 32, 0, 5, 3), {1, 2, 3}) && status(writes.back()) == -32, "An unknown endpoint was not stalled");
  // Malformed requests end the connection.
  { auto iso = submitUrb(1, 40, 1, 1, 32); iso.packets = 2; require(!core.submit(2, iso, {}), "An isochronous request was accepted"); }
  require(!core.submit(2, submitUrb(1, 41, 0, 1, 8), {0, 8, 0}), "An OUT whose data disagrees was accepted");
  require(!core.submit(1, submitUrb(0, 42, 1, 0, 18, {0x80, 6, 0, 1, 0, 0, 18, 0}), {}), "A pad not imported was served");
  require(!core.submit(9, submitUrb(1, 43, 1, 0, 18, {0x80, 6, 0, 1, 0, 0, 18, 0}), {}), "Another device id was accepted");
  core.kernelClosed(2); core.kernelClosed(2);
  require(gone == std::vector<int>{1} && !core.imported(1) && core.waitingReads(1) == 0, "The end of the connection was not seen once");
  require(core.importDevice("1-2", toKernel).has_value(), "The pad could not be imported again");
}

SOCKET connectLoopback(uint16_t port) {
  SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
  if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address))) { closesocket(socket); return INVALID_SOCKET; }
  DWORD timeout = 3000; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  return socket;
}
bool sendAll(SOCKET socket, const usb::Bytes& bytes) { return send(socket, reinterpret_cast<const char*>(bytes.data()), int(bytes.size()), 0) == int(bytes.size()); }
bool readAll(SOCKET socket, usb::Bytes& out, size_t size) {
  out.resize(size); size_t at = 0;
  while (at < size) { const int count = recv(socket, reinterpret_cast<char*>(out.data() + at), int(size - at), 0); if (count <= 0) return false; at += size_t(count); }
  return true;
}
usb::Bytes importRequest(const std::string& busId) { auto out = usb::encodeOpCommon(usb::opReqImport, 0); usb::Bytes field(32, 0); std::memcpy(field.data(), busId.data(), busId.size()); out.insert(out.end(), field.begin(), field.end()); return out; }
bool importOver(SOCKET socket, const std::string& busId) {
  usb::Bytes reply;
  if (!sendAll(socket, importRequest(busId)) || !readAll(socket, reply, 8 + 312)) return false;
  return usb::get32(&reply[4]) == usb::stOk && usb::get16(&reply[8 + 256 + 32 + 12]) == 0x045E && usb::get16(&reply[8 + 256 + 32 + 14]) == 0x028E;
}

// A substituted controller; the plug-in is played by a fake kernel over the socket, one per pad.
struct FakeController {
  std::mutex lock; bool supported = true; int nextPort = 7; std::atomic<int> attaches{0}, prepares{0}; std::vector<int> detached; std::map<int, SOCKET> kernels; std::map<std::string, int> ports;
  ~FakeController() { for (auto& [port, s] : kernels) closesocket(s); }
  void drop(int port) { std::lock_guard g(lock); if (const auto it = kernels.find(port); it != kernels.end()) { closesocket(it->second); kernels.erase(it); } }
  usb::GamepadDevices::Options options() {
    usb::GamepadDevices::Options o;
    o.driver.platformSupported = [this] { ++prepares; return supported; };
    o.driver.packagePresent = [] { return true; }; o.driver.packageVerified = [] { return true; }; o.driver.controllerState = [] { return 1; };
    o.driver.install = [](bool&) { return DWORD(0); }; o.driver.protect = [] { return true; }; o.driver.protectionProven = [] { return true; };
    o.driver.detachAll = [] { return 0; };
    o.attach = [this](const std::string&, uint16_t port, const std::string& busId) -> int {
      ++attaches;
      SOCKET s = connectLoopback(port);
      if (s == INVALID_SOCKET || !importOver(s, busId)) { if (s != INVALID_SOCKET) closesocket(s); return 0; }
      std::lock_guard g(lock); const int assigned = nextPort++; kernels[assigned] = s; ports[busId] = assigned; return assigned;
    };
    o.detach = [this](int port) { { std::lock_guard g(lock); detached.push_back(port); } drop(port); return true; };
    o.cancelAttach = [] {};
    o.server.trusted = [](SOCKET) { return true; };
    return o;
  }
};

void owner() {
  usb::keptPorts().clearForTesting();
  FakeController fake; usb::GamepadDevices pads(fake.options());
  usb::xpad::State a; a.buttons = 0x1000;
  pads.state(0, a);
  require(waitFor([&] { return pads.plugged(0); }), "The first state did not plug the pad in");
  const int port0 = pads.port(0);
  require(usb::keptPorts().contains(port0) && fake.attaches == 1, "The pad's port is not kept");
  for (int i = 0; i < 20; ++i) pads.state(0, a);
  Sleep(50); require(fake.attaches == 1, "A plugged pad was plugged in again");
  pads.state(1, a);
  require(waitFor([&] { return pads.plugged(1); }) && fake.attaches == 2 && pads.port(1) != port0, "A second pad was not plugged in on its own port");
  pads.remove(0);
  require(waitFor([&] { return !pads.plugged(0) && pads.idle(); }) && !usb::keptPorts().contains(port0), "A removed pad stayed");
  { std::lock_guard g(fake.lock); require(fake.detached == std::vector<int>{port0}, "The removed pad was not unplugged"); }
  // Windows let go of pad 1: the next state plugs it in again.
  const int port1 = pads.port(1); fake.drop(port1);
  require(waitFor([&] { return !pads.plugged(1) && !usb::keptPorts().contains(port1); }), "A pad Windows let go of still counted");
  pads.state(1, a);
  require(waitFor([&] { return pads.plugged(1); }) && fake.attaches == 3, "The pad was not plugged in again");
  // The session's end: every pad goes.
  pads.removeAll();
  require(waitFor([&] { return !pads.plugged(1) && pads.idle(); }) && usb::keptPorts().empty(), "The session's end left a pad");
  pads.stop();
  pads.state(2, a); Sleep(50);
  require(fake.attaches == 3, "A stopped owner plugged a pad in");
}

void failures() {
  usb::keptPorts().clearForTesting();
  FakeController fake; fake.supported = false; usb::GamepadDevices pads(fake.options());
  usb::xpad::State a; a.buttons = 0x1000;
  pads.state(0, a);
  require(waitFor([&] { return fake.prepares == 1 && pads.idle(); }), "The preparation did not run");
  for (int i = 0; i < 10; ++i) pads.state(0, a);
  Sleep(50); require(fake.prepares == 1 && !pads.plugged(0), "A failed pad was tried again in the same session");
  pads.removeAll();
  pads.state(0, a);
  require(waitFor([&] { return fake.prepares == 2; }), "The next session did not try again");
}

void messages() {
  const auto parse = [](const std::string& text) { return JsonParser::parse(text); };
  const auto state = gamepadStateMessage(parse(R"({"type":"gamepad.state","sessionId":"s","pad":3,"buttons":63487,"lt":255,"rt":0,"lx":-32768,"ly":32767,"rx":0,"ry":-1})"));
  require(state && state->first == 3 && state->second.buttons == 63487 && state->second.lt == 255 && state->second.lx == -32768 && state->second.ly == 32767 && state->second.ry == -1, "A state was not read exactly");
  for (const char* bad : {R"({"type":"gamepad.state","pad":4,"buttons":0,"lt":0,"rt":0,"lx":0,"ly":0,"rx":0,"ry":0})", R"({"type":"gamepad.state","pad":0,"buttons":2048,"lt":0,"rt":0,"lx":0,"ly":0,"rx":0,"ry":0})",
                          R"({"type":"gamepad.state","pad":0,"buttons":0,"lt":256,"rt":0,"lx":0,"ly":0,"rx":0,"ry":0})", R"({"type":"gamepad.state","pad":0,"buttons":0,"lt":0,"rt":0,"lx":32768,"ly":0,"rx":0,"ry":0})",
                          R"({"type":"gamepad.state","pad":0,"buttons":0,"lt":0,"rt":0,"lx":0.5,"ly":0,"rx":0,"ry":0})", R"({"type":"gamepad.state","pad":0,"buttons":0,"lt":0,"rt":0,"lx":0,"ly":0,"rx":0})",
                          R"({"type":"gamepad.state","pad":0,"buttons":0,"lt":0,"rt":0,"lx":0,"ly":0,"rx":0,"ry":0,"x":1})"})
    require(!gamepadStateMessage(parse(bad)), bad);
  require(gamepadRemoveMessage(parse(R"({"type":"gamepad.remove","sessionId":"s","pad":2})")) == std::optional<int>(2) && !gamepadRemoveMessage(parse(R"({"type":"gamepad.remove","pad":4})")) &&
          !gamepadRemoveMessage(parse(R"({"type":"gamepad.remove","pad":1,"x":1})")), "A removal was not read exactly");
  require(gamepadRumbleMessage("s", 1, 128, 64) == R"({"type":"gamepad.rumble","sessionId":"s","pad":1,"low":128,"high":64})", "The rumble message is not the protocol's");
  require(gamepadGranted(parse(R"({"type":"session.start","mode":"stream","gamepadProtocol":"gamepad-v1"})")) && !gamepadGranted(parse(R"({"type":"session.start","mode":"stream"})")), "The grant was not read exactly");
}
}  // namespace

int main() {
  try {
    WSADATA wsa{}; if (WSAStartup(MAKEWORD(2, 2), &wsa)) throw std::runtime_error("Winsock");
    const std::pair<const char*, void (*)()> steps[] = {{"descriptors", descriptors}, {"core", core}, {"owner", owner}, {"failures", failures}, {"messages", messages}};
    for (const auto& [name, step] : steps) { std::cout << name << std::endl; step(); }
    WSACleanup();
    std::cout << "Multitap Host gamepad regression passed (" << checks << " checks)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "gamepad failed: " << error.what() << " (after " << checks << " checks)\n";
    return 1;
  }
}
