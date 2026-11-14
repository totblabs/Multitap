// The Host's own pointer device (pointer_device.hpp): the kernel's side of the device (import, control
// answers under its own identity, waiting reads, cancels, malformed requests), the loopback USB/IP server
// over real sockets with a fake kernel, and the service's owner with a substituted controller: plugged in once
// per need, never swept, plugged in again after Windows let go of it, unplugged at stop, a stop during the
// plug-in. Nothing is installed, attached or changed on this machine.
#include <winsock2.h>
#include "pointer_device.hpp"
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
constexpr uint32_t devid = 0x00010001;

usb::Urb submitUrb(uint32_t seq, uint32_t direction, uint32_t endpoint, int32_t length, std::array<uint8_t, 8> setup = {}) {
  usb::Urb urb; urb.command = usb::cmdSubmit; urb.seq = seq; urb.devid = devid; urb.direction = direction; urb.endpoint = endpoint; urb.length = length; urb.packets = -1;
  std::memcpy(urb.setup, setup.data(), 8); return urb;
}
usb::Urb unlinkUrb(uint32_t seq, uint32_t target) { usb::Urb urb; urb.command = usb::cmdUnlink; urb.seq = seq; urb.devid = devid; urb.target = target; return urb; }
int32_t status(const usb::Bytes& reply) { return int32_t(usb::get32(&reply[20])); }

void core() {
  usb::PointerDeviceCore core; std::vector<usb::Bytes> writes; int gone = 0;
  const auto toKernel = [&](const usb::Bytes& bytes) { writes.push_back(bytes); return true; };
  core.onGone([&] { ++gone; });
  require(!core.importDevice("1-2", toKernel), "Another busid was imported");
  const auto imported = core.importDevice("1-1", toKernel);
  require(imported && imported->id == 1 && imported->exported.vendor == 0x1209 && imported->exported.product == 0x0002 && imported->exported.speed == 2 &&
          imported->exported.interfaces == 1 && usb::deviceId(imported->exported) == devid, "The device was not exported as the pointer");
  require(!core.importDevice("1-1", toKernel), "The device was imported twice");
  // Control: the descriptors of the virtual mouse under the pointer's product number and strings.
  require(core.submit(1, submitUrb(1, 1, 0, 18, {0x80, 6, 0, 1, 0, 0, 18, 0}), {}), "device descriptor");
  require(writes.size() == 1 && usb::get32(&writes[0][0]) == usb::retSubmit && usb::get32(&writes[0][4]) == 1 && status(writes[0]) == 0 && usb::get32(&writes[0][24]) == 18 &&
          writes[0].size() == 48 + 18 && writes[0][48 + 8] == 0x09 && writes[0][48 + 9] == 0x12 && writes[0][48 + 10] == 0x02 && writes[0][48 + 11] == 0x00, "The device descriptor is not the pointer's");
  require(core.submit(1, submitUrb(2, 1, 0, 255, {0x80, 6, 2, 3, 0x09, 0x04, 255, 0}), {}), "product string");
  { const std::string name = "Multitap Pointer"; const auto& w = writes.back(); bool same = w.size() == 48 + 2 + name.size() * 2;
    for (size_t i = 0; same && i < name.size(); ++i) same = w[48 + 2 + i * 2] == uint8_t(name[i]) && w[48 + 3 + i * 2] == 0;
    require(same, "The product string is not the pointer's"); }
  require(core.submit(1, submitUrb(3, 1, 0, 255, {0x80, 6, 3, 3, 0x09, 0x04, 255, 0}), {}) && writes.back()[48 + 2] == 'M' && writes.back().size() == 48 + 2 + 16 * 2, "The serial is not the pointer's");
  require(core.submit(1, submitUrb(4, 1, 0, 79, {0x81, 6, 0, 0x22, 0, 0, 79, 0}), {}) && usb::Bytes(writes.back().begin() + 48, writes.back().end()) == usb::vmouse::reportDescriptor(), "The report descriptor is not the mouse's");
  require(core.submit(1, submitUrb(5, 0, 0, 0, {0x21, 0x0A, 0, 0, 0, 0, 0, 0}), {}) && status(writes.back()) == 0 && usb::get32(&writes.back()[24]) == 0, "SET_IDLE was not accepted");
  require(core.submit(1, submitUrb(6, 0, 0, 0, {0x00, 9, 1, 0, 0, 0, 0, 0}), {}) && status(writes.back()) == 0, "SET_CONFIGURATION was not accepted");
  require(core.submit(1, submitUrb(7, 0, 0, 0, {0x40, 1, 0, 0, 0, 0, 0, 0}), {}) && status(writes.back()) == -32, "A vendor request was not stalled");
  require(core.submit(1, submitUrb(8, 1, 0, 7, {0xA1, 1, 0, 1, 0, 0, 7, 0}), {}) && status(writes.back()) == 0 && writes.back().size() == 48 + 7 &&
          std::all_of(writes.back().begin() + 48, writes.back().end(), [](uint8_t b) { return b == 0; }), "GET_REPORT is not a still mouse");
  // Interrupt reads wait (the mouse never moves); a cancel of a waiting read is -ECONNRESET, of anything else 0.
  const auto before = writes.size();
  require(core.submit(1, submitUrb(10, 1, 1, 7), {}) && core.submit(1, submitUrb(11, 1, 1, 8), {}) && writes.size() == before && core.waitingReads() == 2, "A read was answered");
  require(!core.submit(1, submitUrb(10, 1, 1, 7), {}), "A repeated sequence number was accepted");
  require(core.unlink(1, unlinkUrb(12, 10)) && usb::get32(&writes.back()[0]) == usb::retUnlink && usb::get32(&writes.back()[4]) == 12 && status(writes.back()) == -104 && core.waitingReads() == 1, "A waiting read was not cancelled");
  require(core.unlink(1, unlinkUrb(13, 99)) && status(writes.back()) == 0, "A cancel of nothing was not answered 0");
  require(core.submit(1, submitUrb(14, 1, 1, 4), {}) && status(writes.back()) == -32, "A read too short for a report was not stalled");
  require(core.submit(1, submitUrb(15, 0, 1, 7), usb::Bytes(7, 0)) && status(writes.back()) == -32, "An OUT on the mouse's endpoint was not stalled");
  require(core.submit(1, submitUrb(16, 1, 2, 7), {}) && status(writes.back()) == -32, "Another endpoint was not stalled");
  for (uint32_t seq = 100; core.waitingReads() < usb::PointerDeviceCore::maxReads; ++seq) require(core.submit(1, submitUrb(seq, 1, 1, 7), {}), "read");
  require(core.submit(1, submitUrb(500, 1, 1, 7), {}) && status(writes.back()) == -32 && core.waitingReads() == usb::PointerDeviceCore::maxReads, "Reads above the bound were held");
  // Malformed requests end the connection.
  { auto iso = submitUrb(20, 1, 1, 7); iso.packets = 3; require(!core.submit(1, iso, {}), "An isochronous request was accepted"); }
  require(!core.submit(1, submitUrb(21, 1, 1, 5000), {}), "An oversized request was accepted");
  require(!core.submit(1, submitUrb(22, 1, 0, 18, {0x80, 6, 0, 1, 0, 0, 64, 0}), {}), "A control request whose length disagrees was accepted");
  require(!core.submit(1, submitUrb(23, 0, 0, 4, {0x21, 9, 0, 2, 0, 0, 4, 0}), usb::Bytes(3, 0)), "An OUT whose data disagrees was accepted");
  require(!core.submit(2, submitUrb(24, 1, 0, 18, {0x80, 6, 0, 1, 0, 0, 18, 0}), {}), "Another device id was accepted");
  // The kernel let go: told once, and the device can be imported again.
  core.kernelClosed(1); core.kernelClosed(1);
  require(gone == 1 && !core.imported() && core.waitingReads() == 0, "The end of the connection was not seen once");
  require(core.importDevice("1-1", toKernel).has_value(), "The device could not be imported again");
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
usb::Bytes submitBytes(uint32_t seq, uint32_t direction, uint32_t endpoint, int32_t length, const uint8_t* setup = nullptr) {
  usb::Bytes cmd; usb::put32(cmd, usb::cmdSubmit); usb::put32(cmd, seq); usb::put32(cmd, devid); usb::put32(cmd, direction); usb::put32(cmd, endpoint);
  usb::put32(cmd, 0); usb::put32(cmd, uint32_t(length)); usb::put32(cmd, 0); usb::put32(cmd, 0xffffffffu); usb::put32(cmd, 0);
  if (setup) cmd.insert(cmd.end(), setup, setup + 8); else cmd.resize(48, 0);
  return cmd;
}
usb::Bytes unlinkBytes(uint32_t seq, uint32_t target) { usb::Bytes cmd; usb::put32(cmd, usb::cmdUnlink); usb::put32(cmd, seq); usb::put32(cmd, devid); usb::put32(cmd, 0); usb::put32(cmd, 0); usb::put32(cmd, target); cmd.resize(48, 0); return cmd; }
// The fake kernel's import: true when the reply is the pointer.
bool importOver(SOCKET socket) {
  usb::Bytes reply;
  if (!sendAll(socket, importRequest("1-1")) || !readAll(socket, reply, 8 + 312)) return false;
  return usb::get16(&reply[2]) == usb::opRepImport && usb::get32(&reply[4]) == usb::stOk && reply[8 + 256] == '1' && reply[8 + 256 + 1] == '-' &&
         usb::get16(&reply[8 + 256 + 32 + 12]) == 0x1209 && usb::get16(&reply[8 + 256 + 32 + 14]) == 0x0002;
}

void server() {
  usb::PointerDeviceCore core; std::atomic<int> gone{0}; core.onGone([&] { ++gone; });
  usb::BasicUsbIpServer<usb::PointerDeviceCore> server; usb::UsbIpAccess access; access.trusted = [](SOCKET) { return true; };
  const auto port = server.start(core, access);
  require(port != 0, "The server did not start");
  { SOCKET s = connectLoopback(port); require(sendAll(s, importRequest("1-7")), "send"); usb::Bytes reply;
    require(readAll(s, reply, 8) && usb::get32(&reply[4]) == usb::stNoDev, "Another busid was not refused"); closesocket(s); }
  SOCKET kernel = connectLoopback(port);
  require(importOver(kernel), "The import over the socket is not the pointer");
  { SOCKET again = connectLoopback(port); require(sendAll(again, importRequest("1-1")), "send"); usb::Bytes reply;
    require(readAll(again, reply, 8) && usb::get32(&reply[4]) == usb::stNoDev, "The device was imported twice over sockets"); closesocket(again); }
  const uint8_t setup[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
  usb::Bytes reply;
  require(sendAll(kernel, submitBytes(1, 1, 0, 18, setup)) && readAll(kernel, reply, 48 + 18) && reply[48 + 10] == 0x02, "The descriptor did not come back over the socket");
  require(sendAll(kernel, submitBytes(2, 1, 1, 7)) && waitFor([&] { return core.waitingReads() == 1; }), "The read did not wait");
  require(sendAll(kernel, unlinkBytes(3, 2)) && readAll(kernel, reply, 48) && usb::get32(&reply[0]) == usb::retUnlink && int32_t(usb::get32(&reply[20])) == -104, "The read was not cancelled over the socket");
  closesocket(kernel);
  require(waitFor([&] { return gone == 1 && !core.imported(); }), "The end of the kernel's connection was not seen");
  server.stop();
}

// A substituted controller: every preparation step logged, the plug-in played by a fake kernel over the socket.
struct FakeController {
  std::mutex lock; std::vector<std::string> log; bool supported = true; int attachResult = 7; std::atomic<int> attaches{0}, prepares{0};
  std::vector<int> detached; SOCKET kernel = INVALID_SOCKET; HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr); bool blockAttach = false;
  ~FakeController() { if (kernel != INVALID_SOCKET) closesocket(kernel); CloseHandle(cancel); }
  void note(const char* step) { std::lock_guard g(lock); log.push_back(step); }
  bool has(const char* step) { std::lock_guard g(lock); return std::find(log.begin(), log.end(), step) != log.end(); }
  void dropKernel() { std::lock_guard g(lock); if (kernel != INVALID_SOCKET) { closesocket(kernel); kernel = INVALID_SOCKET; } }
  usb::PointerDevice::Options options() {
    usb::PointerDevice::Options o;
    o.driver.platformSupported = [this] { ++prepares; note("platform"); return supported; };
    o.driver.packagePresent = [this] { note("present"); return true; };
    o.driver.packageVerified = [this] { note("verify"); return true; };
    o.driver.controllerState = [this] { note("state"); return 1; };
    o.driver.install = [this](bool&) { note("install"); return DWORD(0); };
    o.driver.protect = [this] { note("protect"); return true; };
    o.driver.protectionProven = [this] { note("proven"); return true; };
    o.driver.detachAll = [this] { note("detachAll"); return 0; };
    o.attach = [this](const std::string& host, uint16_t port, const std::string& busId) -> int {
      ++attaches;
      if (blockAttach) { WaitForSingleObject(cancel, 5000); return attachResult; }
      if (host != "127.0.0.1" || busId != "1-1" || attachResult <= 0) return attachResult > 0 ? 0 : attachResult;
      SOCKET s = connectLoopback(port);
      if (s == INVALID_SOCKET || !importOver(s)) { if (s != INVALID_SOCKET) closesocket(s); return 0; }
      { std::lock_guard g(lock); kernel = s; }
      return attachResult;
    };
    o.detach = [this](int port) { { std::lock_guard g(lock); detached.push_back(port); } dropKernel(); return true; };
    o.cancelAttach = [this] { SetEvent(cancel); };
    o.server.trusted = [](SOCKET) { return true; };
    return o;
  }
};

void device() {
  usb::keptPorts().clearForTesting();
  FakeController fake; usb::PointerDevice pointer(fake.options());
  pointer.ask();
  require(waitFor([&] { return pointer.plugged(); }), "The pointer was not plugged in");
  require(pointer.port() == 7 && usb::keptPorts().contains(7) && fake.attaches == 1, "The plugged port is not kept");
  require(fake.has("protect") && fake.has("proven") && !fake.has("detachAll"), "The pointer's preparation swept the session's ports");
  pointer.ask(); pointer.ask(); Sleep(100);
  require(fake.attaches == 1 && fake.prepares == 1, "A plugged pointer was plugged in again");
  // Windows let go of it (the controller was replaced, say): the next ask plugs it in again.
  fake.dropKernel();
  require(waitFor([&] { return !pointer.plugged() && pointer.port() == 0 && usb::keptPorts().empty(); }), "A pointer Windows let go of still counted as plugged");
  pointer.ask();
  require(waitFor([&] { return pointer.plugged(); }) && fake.attaches == 2 && usb::keptPorts().contains(7), "The pointer was not plugged in again");
  // The service stops: unplugged, no longer kept, and asks do nothing.
  pointer.stop();
  { std::lock_guard g(fake.lock); require(fake.detached == std::vector<int>{7}, "The pointer was not unplugged at stop"); }
  require(usb::keptPorts().empty() && pointer.port() == 0, "The stopped pointer is still kept");
  pointer.ask(); Sleep(50);
  require(fake.attaches == 2, "A stopped pointer was plugged in");
}

void failures() {
  usb::keptPorts().clearForTesting();
  { FakeController fake; fake.supported = false; usb::PointerDevice pointer(fake.options());
    pointer.ask();
    require(waitFor([&] { return fake.prepares == 1; }), "The preparation did not run");
    Sleep(50); require(!pointer.plugged() && fake.attaches == 0 && usb::keptPorts().empty(), "An unsupported Windows plugged something in");
    pointer.ask();
    require(waitFor([&] { return fake.prepares == 2; }), "A failed attempt was not tried again on the next ask"); }
  { FakeController fake; fake.attachResult = 0; usb::PointerDevice pointer(fake.options());
    pointer.ask();
    require(waitFor([&] { return fake.attaches == 1; }), "The plug-in was not tried");
    Sleep(50); require(!pointer.plugged() && usb::keptPorts().empty(), "A failed plug-in counted as plugged"); }
  // A stop during the plug-in: the attempt is cancelled; a port that still comes back is unplugged at once.
  { FakeController fake; fake.blockAttach = true; fake.attachResult = 9; usb::PointerDevice pointer(fake.options());
    pointer.ask();
    require(waitFor([&] { return fake.attaches == 1; }), "The plug-in did not begin");
    pointer.stop();
    { std::lock_guard g(fake.lock); require(fake.detached == std::vector<int>{9}, "A port that came back during the stop was not unplugged"); }
    require(usb::keptPorts().empty() && !pointer.plugged(), "A port that came back during the stop is kept"); }
  // A session's preparation still sweeps; the pointer's does not.
  { FakeController fake; std::atomic<bool> stopping{false}; const auto o = fake.options();
    require(usb::prepareController(o.driver, stopping, false) == usb::Prepare::ready && !fake.has("detachAll"), "A preparation without a sweep swept");
    require(usb::prepareController(o.driver, stopping) == usb::Prepare::ready && fake.has("detachAll"), "A session's preparation did not sweep"); }
}
}  // namespace

int main() {
  try {
    WSADATA wsa{}; if (WSAStartup(MAKEWORD(2, 2), &wsa)) throw std::runtime_error("Winsock");
    const std::pair<const char*, void (*)()> steps[] = {{"core", core}, {"server", server}, {"device", device}, {"failures", failures}};
    for (const auto& [name, step] : steps) { std::cout << name << std::endl; step(); }
    WSACleanup();
    std::cout << "Multitap Host pointer device regression passed (" << checks << " checks)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pointer device failed: " << error.what() << " (after " << checks << " checks)\n";
    return 1;
  }
}
