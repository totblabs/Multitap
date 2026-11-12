// usb-v1 on the Host: the session.start grant, the shared descriptor policy
// vectors, the USB/IP and relay frame codecs, the session logic with a fake
// kernel and a fake client (attach, descriptors, bulk/interrupt/control,
// splitting, the window, unlink, revocation, detaching, crash cleanup), the
// controller preparation sequence with a substituted Windows (install, ACL
// proof, leftovers), the loopback USB/IP server over real sockets, and the
// vhci ioctl ABI. Nothing is installed, attached or changed on this machine;
// with MULTITAP_USB_PACKAGE set to a folder holding the pinned package, the
// package is also verified statically (hashes, Microsoft signatures, catalogs).
#include <winsock2.h>
#include "usb_redirection.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
const std::string session = "00000000-0000-4000-8000-000000000250";

usb::Bytes fromHex(const std::string& hex) {
  usb::Bytes out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
  return out;
}
std::string toBase64(const usb::Bytes& bytes) {
  DWORD length = 0;
  CryptBinaryToStringA(bytes.data(), DWORD(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length);
  std::string out(length, '\0');
  CryptBinaryToStringA(bytes.data(), DWORD(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &length);
  out.resize(length); return out;
}
std::string readFile(const char* path) { std::ifstream file(path, std::ios::binary); return std::string((std::istreambuf_iterator<char>(file)), {}); }
const Json& vectors() { static const Json value = JsonParser::parse(readFile(MULTITAP_USB_VECTORS)); return value; }
usb::Bytes vector(const std::string& name) { return fromHex(vectors().get(name).get("descriptors").string()); }
bool waitFor(const std::function<bool()>& condition, int milliseconds = 3000) {
  for (int waited = 0; waited < milliseconds; waited += 5) { if (condition()) return true; Sleep(5); }
  return condition();
}

void grant() {
  const auto start = [](const std::string& extra) { return JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"" + session + "\"" + extra + '}'); };
  const auto url = "wss://vdi.example/api/v1/hosts/sessions/" + session + "/usb";
  require(usbSessionUrl(start(",\"usbProtocol\":\"usb-v1\",\"usbUrl\":\"" + url + "\""), session) == url, "A granted start was not recognised");
  require(usbSessionUrl(start(",\"usbUrl\":\"" + url + "\""), session).empty(), "A URL without the grant started USB");
  require(usbSessionUrl(start(",\"usbProtocol\":\"usb-v2\",\"usbUrl\":\"" + url + "\""), session).empty(), "Another protocol started USB");
  require(usbSessionUrl(start(",\"usbProtocol\":\"usb-v1\""), session).empty(), "A grant without a URL started USB");
  require(usbSessionUrl(start(",\"usbProtocol\":\"usb-v1\",\"usbUrl\":\"https://vdi.example/api/v1/hosts/sessions/" + session + "/usb\""), session).empty(), "A non-WebSocket URL was accepted");
  require(usbSessionUrl(start(",\"usbProtocol\":\"usb-v1\",\"usbUrl\":\"wss://vdi.example/api/v1/hosts/sessions/other/usb\""), session).empty(), "Another session's URL was accepted");
  require(usbSessionUrl(JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"rdp\",\"sessionId\":\"" + session + "\",\"usbProtocol\":\"usb-v1\",\"usbUrl\":\"" + url + "\"}"), session).empty(), "An RDP start started streaming USB");
  const auto grant = usbSessionGrant(start(",\"usbGrant\":{\"drives\":true,\"printers\":\"yes\",\"serialPorts\":true}"));
  require(grant.drives && !grant.printers && !grant.smartCards && grant.serialPorts, "The class grant was not read exactly");
  const auto none = usbSessionGrant(start(""));
  require(!none.drives && !none.printers && !none.smartCards && !none.serialPorts, "A missing grant allowed a class");
}

// Descriptor sets built here: the Host's own, stricter rules on top of the
// shared vectors: keyboard/mouse protocols with any subclass, network adapters, IAD function codes, and a
// configuration that ends inside a descriptor header.
usb::Bytes interfaceOf(uint8_t number, uint8_t cls, uint8_t subclass, uint8_t protocol) {
  return {9, 4, number, 0, 1, cls, subclass, protocol, 0, 7, 5, uint8_t(0x81 + number), 2, 64, 0, 0};
}
usb::Bytes associationOf(uint8_t first, uint8_t count, uint8_t cls, uint8_t subclass, uint8_t protocol, uint8_t length = 8) {
  usb::Bytes out{length, 11, first, count, cls, subclass, protocol, 0}; out.resize(length, 0); return out;
}
usb::Bytes deviceOf(const std::vector<usb::Bytes>& parts, uint8_t interfaces, uint8_t cls = 0, uint8_t subclass = 0, uint8_t protocol = 0, size_t extra = 0) {
  usb::Bytes body; for (const auto& part : parts) body.insert(body.end(), part.begin(), part.end());
  body.resize(body.size() + extra, 9);   // [extra] stray bytes at the end of the configuration
  const size_t total = 9 + body.size();
  usb::Bytes out{18, 1, 0x00, 0x02, cls, subclass, protocol, 64, 0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0, 0, 0, 1,
                 9, 2, uint8_t(total & 0xff), uint8_t(total >> 8), interfaces, 1, 0, 0x80, 50};
  out.insert(out.end(), body.begin(), body.end()); return out;
}
void hostOnlyPolicy() {
  const auto decided = [](const usb::Bytes& bytes) { const auto device = usb::parseDescriptors(bytes.data(), bytes.size()); require(device.has_value(), "A built descriptor set did not parse"); return usb::decide(*device); };
  const auto denied = [&](const usb::Bytes& bytes, const char* reason, const char* what) { const auto d = decided(bytes); require(!d.allowed && d.reason == reason, what); };
  // Keyboards and mice by protocol, whatever the subclass; a report-protocol HID stays allowed.
  denied(deviceOf({interfaceOf(0, 3, 0, 1)}, 1), "usb_hid_boot_denied", "A keyboard without the boot subclass was allowed");
  denied(deviceOf({interfaceOf(0, 3, 0, 2)}, 1), "usb_hid_boot_denied", "A mouse without the boot subclass was allowed");
  denied(deviceOf({interfaceOf(0, 8, 6, 80), interfaceOf(1, 3, 0, 1)}, 2), "usb_hid_boot_denied", "A keyboard behind a storage interface was allowed");
  { const auto d = decided(deviceOf({interfaceOf(0, 3, 0, 0)}, 1)); require(d.allowed && d.requires_.empty(), "A report-protocol HID was refused"); }
  // Network adapters: no permission covers them, so a full grant still does not.
  for (const auto& [codes, what] : std::vector<std::pair<std::array<uint8_t, 3>, const char*>>{
           {{2, 6, 0}, "CDC ECM"}, {{2, 13, 0}, "CDC NCM"}, {{2, 12, 7}, "CDC EEM"}, {{2, 14, 0}, "MBIM"}, {{2, 7, 0}, "CDC ATM"},
           {{2, 2, 0xff}, "RNDIS (CDC)"}, {{0xe0, 1, 3}, "RNDIS (wireless class)"}, {{0xef, 4, 1}, "RNDIS (misc class)"}, {{0xef, 4, 6}, "RNDIS raw IPv6"}}) {
    const auto d = decided(deviceOf({interfaceOf(0, codes[0], codes[1], codes[2]), interfaceOf(1, 10, 0, 0)}, 2));
    require(!d.allowed && d.reason == "usb_class_denied", what);   // denied before any grant is looked at
  }
  { const auto d = decided(deviceOf({interfaceOf(0, 2, 2, 1), interfaceOf(1, 10, 0, 0)}, 2)); require(d.allowed && d.requires_ == std::set<std::string>{"serialPorts"}, "A CDC serial adapter was refused"); }
  // Interface associations: Windows names the function after them, so their codes are judged too.
  denied(deviceOf({associationOf(0, 2, 0x0e, 3, 0), interfaceOf(0, 0xff, 0, 0), interfaceOf(1, 0xff, 0, 0)}, 2, 0xef, 2, 1), "usb_isochronous_denied", "A video function behind vendor interfaces was allowed");
  denied(deviceOf({associationOf(0, 1, 3, 0, 1), interfaceOf(0, 0xff, 0, 0)}, 1, 0xef, 2, 1), "usb_hid_boot_denied", "A keyboard function behind a vendor interface was allowed");
  denied(deviceOf({associationOf(0, 2, 2, 6, 0), interfaceOf(0, 0xff, 0, 0), interfaceOf(1, 0xff, 0, 0)}, 2, 0xef, 2, 1), "usb_class_denied", "A network function behind vendor interfaces was allowed");
  { const auto d = decided(deviceOf({associationOf(0, 1, 8, 6, 80), interfaceOf(0, 0xff, 0, 0)}, 1, 0xef, 2, 1));
    require(d.allowed && d.requires_ == std::set<std::string>{"drives"} && !usb::covers(d, {}) && usb::covers(d, {true, false, false, false}), "A storage function did not need the drives permission"); }
  { const auto bad = deviceOf({associationOf(0, 1, 8, 6, 80, 9), interfaceOf(0, 8, 6, 80)}, 1, 0xef, 2, 1);
    require(!usb::parseDescriptors(bad.data(), bad.size()), "An association descriptor of the wrong length parsed"); }
  // A configuration that ends one byte into a descriptor header is malformed (that byte's type was read past the end).
  { const auto stray = deviceOf({interfaceOf(0, 8, 6, 80)}, 1, 0, 0, 0, 1);
    require(!usb::parseDescriptors(stray.data(), stray.size()), "A configuration ending inside a descriptor header parsed"); }
  { const auto good = deviceOf({interfaceOf(0, 8, 6, 80)}, 1); require(usb::parseDescriptors(good.data(), good.size()).has_value(), "A built storage device did not parse"); }
}

void policy() {
  const auto* object = std::get_if<Json::Object>(&vectors().value);
  require(object && object->size() >= 15, "The shared vectors are missing");
  for (const auto& [name, entry] : *object) {
    const auto bytes = fromHex(entry.get("descriptors").string());
    const auto device = usb::parseDescriptors(bytes.data(), bytes.size());
    require(device.has_value(), "A vector did not parse");
    const auto decision = usb::decide(*device);
    const auto& expect = entry.get("expect");
    require(decision.allowed == expect.get("allowed").boolean(), name.c_str());
    if (decision.allowed) {
      std::set<std::string> wanted; for (const auto& item : std::get<Json::Array>(expect.get("requires").value)) wanted.insert(item.string());
      std::set<int> classes; for (const auto& item : std::get<Json::Array>(expect.get("classes").value)) classes.insert(int(item.number()));
      require(decision.requires_ == wanted, (name + ": requires").c_str());
      require(decision.classes == classes, (name + ": classes").c_str());
    } else require(decision.reason == expect.get("reason").string(), (name + ": reason").c_str());
  }
  // Malformed sets are refused, never partly read.
  const auto good = vector("mass storage stick");
  require(usb::parseDescriptors(good.data(), good.size()).has_value(), "A good set did not parse");
  require(!usb::parseDescriptors(good.data(), 20) && !usb::parseDescriptors(nullptr, 0), "A short set parsed");
  auto mutated = [&](size_t at, uint8_t value) { auto copy = good; copy[at] = value; return !usb::parseDescriptors(copy.data(), copy.size()).has_value(); };
  require(mutated(0, 17) && mutated(17, 0) && mutated(17, 5) && mutated(7, 7) && mutated(20, 255) && mutated(18 + 9, 1) && mutated(18 + 9 + 4, 3), "A malformed set parsed");
  auto trailing = good; trailing.push_back(0);
  require(!usb::parseDescriptors(trailing.data(), trailing.size()), "Trailing bytes were accepted");
  const usb::Bytes huge(usb::maxDescriptorBytes + 1, 0);
  require(!usb::parseDescriptors(huge.data(), huge.size()), "An oversized set was accepted");
  // Cross-permissions: the class permissions of the grant decide.
  const auto stickDevice = *usb::parseDescriptors(good.data(), good.size());
  const auto decision = usb::decide(stickDevice);
  require(!usb::covers(decision, {}) && !usb::covers(decision, {false, true, true, true}) && usb::covers(decision, {true, false, false, false}), "Mass storage was not tied to the drives permission");
  const auto composite = vector("storage plus serial composite"); const auto compositeDecision = usb::decide(*usb::parseDescriptors(composite.data(), composite.size()));
  require(!usb::covers(compositeDecision, {true, false, false, false}) && !usb::covers(compositeDecision, {false, false, false, true}) && usb::covers(compositeDecision, {true, false, false, true}), "A composite device needed one permission, not both");
  // Endpoints: only the validated device's bulk/interrupt endpoints and endpoint 0.
  require(usb::endpointType(stickDevice, 0x81) == 2 && usb::endpointType(stickDevice, 0x02) == 2 && usb::endpointType(stickDevice, 0x00) == 0 && usb::endpointType(stickDevice, 0x80) == 0, "A real endpoint was refused");
  require(!usb::endpointType(stickDevice, 0x82) && !usb::endpointType(stickDevice, 0x01) && !usb::endpointType(stickDevice, 0x03), "An endpoint the device does not have was allowed");
  // The VM's GET_DESCRIPTOR comes from the validated copy.
  const uint8_t getDevice[8] = {0x80, 6, 0, 1, 0, 0, 18, 0}, getConfig[8] = {0x80, 6, 0, 2, 0, 0, 255, 0}, getShort[8] = {0x80, 6, 0, 2, 0, 0, 9, 0}, getString[8] = {0x80, 6, 1, 3, 9, 4, 255, 0}, getSecond[8] = {0x80, 6, 1, 2, 0, 0, 255, 0};
  const auto deviceBytes = usb::localDescriptor(stickDevice, getDevice);
  require(deviceBytes && deviceBytes->size() == 18 && std::equal(deviceBytes->begin(), deviceBytes->end(), good.begin()), "The device descriptor did not come from the copy");
  const auto configBytes = usb::localDescriptor(stickDevice, getConfig);
  require(configBytes && configBytes->size() == good.size() - 18 && std::equal(configBytes->begin(), configBytes->end(), good.begin() + 18), "The configuration descriptor did not come from the copy");
  require(usb::localDescriptor(stickDevice, getShort)->size() == 9, "wLength was not honoured");
  require(!usb::localDescriptor(stickDevice, getString) && !usb::localDescriptor(stickDevice, getSecond), "A string or missing configuration was answered locally");
  hostOnlyPolicy();
}

void wire() {
  require(usb::usbipDeviceSize == usb::encodeDevice({}).size() && usb::encodeDevice({}).size() == 312, "The exported device is not 312 bytes");
  usb::ExportedDevice device; device.busId = "1-3"; device.path = "/x"; device.busNumber = 1; device.deviceNumber = 3; device.speed = 3; device.vendor = 0x1234; device.product = 0x5678; device.bcdDevice = 0x0100; device.cls = 8; device.interfaces = 1;
  const auto bytes = usb::encodeDevice(device);
  require(bytes[256] == '1' && bytes[257] == '-' && bytes[258] == '3' && bytes[259] == 0, "The busid is not at offset 256");
  require(usb::get32(&bytes[288]) == 1 && usb::get32(&bytes[292]) == 3 && usb::get32(&bytes[296]) == 3 && usb::get16(&bytes[300]) == 0x1234 && usb::get16(&bytes[302]) == 0x5678 && bytes[306] == 8, "A device field is misplaced");
  const auto reply = usb::encodeImportReply(device);
  require(reply.size() == 8 + 312 && reply[0] == 0x01 && reply[1] == 0x11 && reply[2] == 0x00 && reply[3] == 0x03 && usb::get32(&reply[4]) == 0, "The import reply header is wrong");
  require(usb::deviceId(device) == 0x00010003, "The devid is not busnum<<16|devnum");
  uint8_t id[usb::usbipBusIdSize] = {'1', '-', '9'};
  require(usb::decodeBusId(id) == "1-9", "A busid was not read");
  uint8_t unterminated[usb::usbipBusIdSize]; std::memset(unterminated, 'a', sizeof(unterminated));
  require(!usb::decodeBusId(unterminated), "An unterminated busid was accepted");

  usb::Bytes cmd;
  usb::put32(cmd, usb::cmdSubmit); usb::put32(cmd, 77); usb::put32(cmd, 0x00010003); usb::put32(cmd, 1); usb::put32(cmd, 2);
  usb::put32(cmd, 0); usb::put32(cmd, 512); usb::put32(cmd, 0); usb::put32(cmd, 0xffffffffu); usb::put32(cmd, 0);
  cmd.insert(cmd.end(), {0x80, 6, 0, 1, 0, 0, 18, 0});
  const auto urb = usb::decodeUrbHeader(cmd.data(), cmd.size());
  require(cmd.size() == 48 && urb && urb->command == usb::cmdSubmit && urb->seq == 77 && urb->devid == 0x00010003 && urb->direction == 1 && urb->endpoint == 2 && urb->length == 512 && urb->packets == -1 && urb->setup[1] == 6, "CMD_SUBMIT was decoded wrongly");
  usb::Bytes unlink;
  usb::put32(unlink, usb::cmdUnlink); usb::put32(unlink, 78); usb::put32(unlink, 0x00010003); usb::put32(unlink, 0); usb::put32(unlink, 0); usb::put32(unlink, 77); unlink.resize(48, 0);
  const auto parsedUnlink = usb::decodeUrbHeader(unlink.data(), unlink.size());
  require(parsedUnlink && parsedUnlink->command == usb::cmdUnlink && parsedUnlink->target == 77, "CMD_UNLINK was decoded wrongly");
  auto bad = cmd; bad[3] = 9; require(!usb::decodeUrbHeader(bad.data(), bad.size()), "An unknown command decoded");
  bad = cmd; bad[15] = 2; require(!usb::decodeUrbHeader(bad.data(), bad.size()), "A direction of 2 decoded");
  require(!usb::decodeUrbHeader(cmd.data(), 47), "A short header decoded");

  const uint8_t data[3] = {1, 2, 3};
  const auto ret = usb::encodeRetSubmit(77, 0x00010003, 1, 2, 0, data, 3);
  require(ret.size() == 48 + 3 && usb::get32(&ret[0]) == usb::retSubmit && usb::get32(&ret[4]) == 77 && usb::get32(&ret[20]) == 0 && usb::get32(&ret[24]) == 3 && usb::get32(&ret[32]) == 0xffffffffu && ret[48] == 1, "RET_SUBMIT is wrong");
  const auto retStall = usb::encodeRetSubmit(5, 1, 0, 2, -32, nullptr, 0);
  require(retStall.size() == 48 && int32_t(usb::get32(&retStall[20])) == -32, "A negative status was not kept");
  const auto retUnlink = usb::encodeRetUnlink(78, 0x00010003, -104);
  require(retUnlink.size() == 48 && usb::get32(&retUnlink[0]) == usb::retUnlink && int32_t(usb::get32(&retUnlink[20])) == -104, "RET_UNLINK is wrong");

  usb::Frame frame; frame.type = usb::frameSubmit; frame.device = 3; frame.endpoint = 0x81; frame.transfer = 2; frame.seq = 0x01020304; frame.length = 512; frame.status = -32; frame.payload = {9, 8, 7};
  const auto encoded = usb::encodeFrame(frame);
  require(encoded.size() == 24 + 3 && encoded[4] == 0x04 && encoded[7] == 0x01 && encoded[8] == 0x00 && encoded[9] == 0x02, "The relay frame is not little-endian");
  const auto decoded = usb::decodeFrame(encoded.data(), encoded.size());
  require(decoded && decoded->device == 3 && decoded->endpoint == 0x81 && decoded->seq == 0x01020304 && decoded->length == 512 && decoded->status == -32 && decoded->payload.size() == 3, "A relay frame did not round-trip");
  require(!usb::decodeFrame(encoded.data(), 23), "A short relay frame decoded");
  const usb::Bytes huge(usb::frameHeaderSize + usb::maxUrbBytes + 1, 0);
  require(!usb::decodeFrame(huge.data(), huge.size()), "An oversized relay frame decoded");
}

// --- the session core with a fake kernel and a fake client ----------------------------------
struct Harness {
  std::mutex mutex; std::vector<std::string> texts; std::vector<usb::Bytes> frames; std::vector<usb::Bytes> kernelWrites; std::vector<int> detached;
  std::atomic<int> nextPort{7}; std::atomic<bool> attachFails{false}, relayFull{false}, kernelClosed{false}; UsbPrepared prepared{true, ""};
  std::atomic<int> cancels{0}; std::function<bool(const std::string&)> attachProbe;
  std::atomic<int> detachBehavior{0}; // 0 succeeds, 1 false, 2 throws.
  std::atomic<long long> nowMs{0}; UsbVmState vmState{2, 0}; unsigned vmPolls = 0;   // a fake clock and the VM's device node
  std::unique_ptr<UsbSessionCore> core;
  explicit Harness(usb::Grant grant = {true, false, false, true}) {
    UsbHooks hooks;
    hooks.sendText = [this](const std::string& text) { if (relayFull) return false; std::lock_guard lock(mutex); texts.push_back(text); return true; };
    hooks.sendFrame = [this](const usb::Bytes& bytes) { if (relayFull) return false; std::lock_guard lock(mutex); frames.push_back(bytes); return true; };
    hooks.prepare = [this] { return prepared; };
    hooks.attach = [this](const std::string& busId) -> int {
      if (attachFails) return 0;
      // The kernel imports the busid over its connection, then the plug-in returns.
      const auto imported = core->importDevice(busId, [this](const usb::Bytes& bytes) { std::lock_guard lock(mutex); kernelWrites.push_back(bytes); return true; }, [this] { kernelClosed = true; });
      if (!imported) return 0;
      return nextPort++;
    };
    UsbRedirection::Options options;
    options.detach = [this](int port) { std::lock_guard lock(mutex); detached.push_back(port); if (detachBehavior == 2) throw std::runtime_error("synthetic PlugOut failure"); return detachBehavior != 1; };
    hooks.detach = observedUsbDetach(options.detach);
    hooks.cancelAttach = [this] { ++cancels; };
    hooks.deviceState = [this](uint16_t, uint16_t) { ++vmPolls; return vmState; };
    core = std::make_unique<UsbSessionCore>(session, grant, std::move(hooks));
    core->clock = [this] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(nowMs.load() + 1000000)); };
  }
  bool ready() {
    return core->message(JsonParser::parse("{\"type\":\"usb.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"usb-v1\",\"limits\":{\"maxUrbBytes\":262144,\"windowBytes\":1048576,\"maxInFlight\":32,\"maxDevices\":4,\"maxAttaches\":64,\"descriptorBytes\":4096}}"));
  }
  bool attach(int device, const usb::Bytes& descriptors, const char* speed = "high") {
    return core->message(JsonParser::parse("{\"type\":\"usb.attach\",\"device\":" + std::to_string(device) + ",\"descriptors\":\"" + toBase64(descriptors) + "\",\"speed\":\"" + speed + "\"}"));
  }
  bool hasText(const std::string& part) { std::lock_guard lock(mutex); return std::any_of(texts.begin(), texts.end(), [&](const auto& item) { return item.find(part) != std::string::npos; }); }
  size_t textCount(const std::string& part) { std::lock_guard lock(mutex); return size_t(std::count_if(texts.begin(), texts.end(), [&](const auto& item) { return item.find(part) != std::string::npos; })); }
  size_t frameCount() { std::lock_guard lock(mutex); return frames.size(); }
  usb::Frame frame(size_t index) { std::lock_guard lock(mutex); return *usb::decodeFrame(frames.at(index).data(), frames.at(index).size()); }
  size_t kernelCount() { std::lock_guard lock(mutex); return kernelWrites.size(); }
  usb::Bytes kernel(size_t index) { std::lock_guard lock(mutex); return kernelWrites.at(index); }
  // A device attached through the fake kernel (id 1 unless stated).
  void attached(int device, const usb::Bytes& descriptors) {
    require(ready() && waitFor([&] { return hasText("\"usb.status\"") && hasText("\"ready\""); }), "The controller was not reported ready");
    require(attach(device, descriptors), "The attach was refused as a protocol error");
    require(waitFor([&] { return hasText("\"usb.attached\",\"device\":" + std::to_string(device)); }), "The device was never reported attached");
    require(waitFor([&] { return core->deviceExists(uint8_t(device)); }), "The device does not exist");
  }
  bool returnFrame(const usb::Frame& submit, int32_t status, usb::Bytes payload = {}, uint32_t length = UINT32_MAX) {
    usb::Frame answer; answer.type = usb::frameReturn; answer.device = submit.device; answer.endpoint = submit.endpoint; answer.transfer = submit.transfer; answer.seq = submit.seq;
    answer.status = status; answer.length = length == UINT32_MAX ? uint32_t(payload.size()) : length; answer.payload = std::move(payload);
    const auto bytes = usb::encodeFrame(answer);
    return core->frame(bytes.data(), bytes.size());
  }
};
usb::Urb urbOut(uint32_t seq, uint32_t endpoint, int32_t length) { usb::Urb urb; urb.command = usb::cmdSubmit; urb.seq = seq; urb.direction = 0; urb.endpoint = endpoint; urb.length = length; urb.packets = -1; return urb; }
usb::Urb urbIn(uint32_t seq, uint32_t endpoint, int32_t length) { auto urb = urbOut(seq, endpoint, length); urb.direction = 1; return urb; }
usb::Urb unlinkOf(uint32_t seq, uint32_t target) { usb::Urb urb; urb.command = usb::cmdUnlink; urb.seq = seq; urb.target = target; return urb; }

void attachment() {
  // The controller is reported before anything attaches; before it is, devices are refused.
  { Harness h; require(h.ready(), "ready was refused");
    h.core->message(JsonParser::parse("{\"type\":\"usb.attach\",\"device\":1,\"descriptors\":\"" + toBase64(vector("mass storage stick")) + "\",\"speed\":\"high\"}"));
    require(waitFor([&] { return h.hasText("usb_vhci_unavailable") || h.hasText("\"usb.status\""); }), "No answer");
    require(h.textCount("\"usb.rejected\"") == 1 || h.textCount("\"usb.attached\"") == 0, "A device attached before the controller was ready"); }
  { Harness h; h.prepared = {false, "usb_driver_install_failed"};
    require(h.ready(), "ready");
    require(waitFor([&] { return h.hasText("\"unavailable\"") && h.hasText("usb_driver_install_failed"); }), "The failure reason was not reported");
    require(h.attach(1, vector("mass storage stick")), "attach");
    require(h.hasText("\"usb.rejected\",\"device\":1,\"reason\":\"usb_driver_install_failed\""), "A device was refused with a reason other than the preparation failure");
    require(h.core->deviceCount() == 0, "A device exists without a controller"); }
  // Limits: the ready message must be exact.
  { Harness h; require(!h.core->message(JsonParser::parse("{\"type\":\"usb.ready\",\"sessionId\":\"other\",\"protocol\":\"usb-v1\",\"limits\":{}}")), "Another session's ready was accepted");
    require(!h.core->message(JsonParser::parse("{\"type\":\"usb.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"usb-v1\",\"limits\":{\"maxUrbBytes\":999999999,\"windowBytes\":1,\"maxInFlight\":0,\"maxDevices\":1,\"maxAttaches\":1}}")), "Absurd limits were accepted");
    require(!h.core->message(JsonParser::parse("{\"type\":\"usb.attach\",\"device\":1}")), "A message before ready was accepted");
    require(!h.core->frame(nullptr, 0), "A frame before ready was accepted"); }
  // A normal attach: the client is told before any request, the kernel got the exported device.
  { Harness h; h.attached(1, vector("mass storage stick"));
    require(h.core->deviceCount() == 1 && h.detached.empty(), "State after attach");
    require(h.attach(2, vector("cdc serial adapter")), "A second attach was a protocol error"); }
  // Policy refusals are answered by the Host, per device, with fixed reasons.
  { Harness h; require(h.ready() && waitFor([&] { return h.hasText("\"ready\""); }), "ready");
    const struct { const char* vector; const char* reason; } cases[] = {{"boot keyboard", "usb_hid_boot_denied"}, {"boot mouse", "usb_hid_boot_denied"}, {"hub", "usb_class_denied"}, {"bluetooth adapter", "usb_class_denied"},
      {"usb headset (audio, isochronous)", "usb_isochronous_denied"}, {"webcam (video)", "usb_isochronous_denied"}, {"keyboard with a storage interface", "usb_hid_boot_denied"}, {"vendor device with an isochronous endpoint", "usb_isochronous_denied"}};
    int id = 1;
    for (const auto& item : cases) { require(h.attach(id, vector(item.vector)), item.vector); require(h.hasText("{\"type\":\"usb.rejected\",\"device\":" + std::to_string(id) + ",\"reason\":\"" + item.reason + "\"}"), item.vector); ++id; }
    require(h.core->deviceCount() == 0 && h.kernelCount() == 0, "A denied device reached the kernel"); }
  // Class permissions: storage needs drives, a printer needs printers, serial needs serialPorts.
  { Harness h({false, false, false, false}); require(h.ready() && waitFor([&] { return h.hasText("\"ready\""); }), "ready");
    require(h.attach(1, vector("mass storage stick")) && h.hasText("\"device\":1,\"reason\":\"usb_class_not_granted\""), "Storage attached without drives");
    require(h.attach(2, vector("printer")) && h.hasText("\"device\":2,\"reason\":\"usb_class_not_granted\""), "A printer attached without printers");
    require(h.attach(3, vector("cdc serial adapter")) && h.hasText("\"device\":3,\"reason\":\"usb_class_not_granted\""), "A serial adapter attached without serialPorts");
    require(h.attach(4, vector("vendor specific serial chip")) && waitFor([&] { return h.hasText("\"usb.attached\",\"device\":4"); }), "A vendor serial chip needs only usbDevices"); }
  // Descriptor problems and protocol errors.
  { Harness h; require(h.ready() && waitFor([&] { return h.hasText("\"ready\""); }), "ready");
    auto broken = vector("mass storage stick"); broken[0] = 17;
    require(h.attach(1, broken) && h.hasText("\"device\":1,\"reason\":\"usb_descriptor_invalid\""), "Broken descriptors attached");
    require(h.core->message(JsonParser::parse("{\"type\":\"usb.attach\",\"device\":2,\"descriptors\":\"!!!!\",\"speed\":\"high\"}")) && h.hasText("\"device\":2,\"reason\":\"usb_descriptor_invalid\""), "Bad base64 attached");
    require(!h.attach(1, vector("mass storage stick")), "A reused device id was accepted");
    require(!h.attach(0, vector("mass storage stick")) && !h.attach(256, vector("mass storage stick")), "An out-of-range device id was accepted");
    require(!h.attach(3, vector("mass storage stick"), "super"), "An unknown speed was accepted"); }
  // The plug-in failing before the import is a refusal, with no leftover.
  { Harness h; h.attachFails = true; require(h.ready() && waitFor([&] { return h.hasText("\"ready\""); }), "ready");
    require(h.attach(1, vector("mass storage stick")), "attach");
    require(waitFor([&] { return h.hasText("\"device\":1,\"reason\":\"usb_attach_failed\""); }) && h.core->deviceCount() == 0, "A failed plug-in left a device"); }
  // Four devices at a time.
  { Harness h; h.attached(1, vector("vendor specific serial chip"));
    for (int id = 2; id <= 4; ++id) require(h.attach(id, vector("vendor specific serial chip")), "attach");
    require(waitFor([&] { return h.core->deviceCount() == 4; }), "Four devices were not allowed");
    require(h.attach(5, vector("vendor specific serial chip")) && h.hasText("\"device\":5,\"reason\":\"usb_limit_reached\""), "A fifth device was allowed"); }
}

void transfers() {
  const auto stick = vector("mass storage stick");
  { Harness h; h.attached(1, stick);
    // The import reply of the real server carries the exported device; here the core's record.
    const auto imported = h.core->deviceCount();
    require(imported == 1, "One device");

    // Descriptors come from the validated copy: nothing is sent to the client.
    usb::Urb getDevice = urbIn(1, 0, 18); const uint8_t setup1[8] = {0x80, 6, 0, 1, 0, 0, 18, 0}; std::memcpy(getDevice.setup, setup1, 8);
    require(h.core->submit(1, getDevice, {}), "A local descriptor request failed");
    require(h.frameCount() == 0 && h.kernelCount() == 1, "The descriptor request went to the client");
    auto reply = h.kernel(0);
    require(usb::get32(&reply[0]) == usb::retSubmit && usb::get32(&reply[4]) == 1 && usb::get32(&reply[24]) == 18 && reply.size() == 48 + 18 && reply[48] == 18, "The local descriptor reply is wrong");

    // Bulk OUT, then IN with a short answer.
    usb::Bytes block(4096, 0x5a);
    require(h.core->submit(1, urbOut(2, 2, 4096), block), "bulk OUT");
    require(h.frameCount() == 1, "The OUT request did not reach the client");
    auto out = h.frame(0);
    require(out.type == usb::frameSubmit && out.device == 1 && out.endpoint == 0x02 && out.transfer == 2 && out.length == 4096 && out.payload == block && out.seq != 0, "The OUT frame is wrong");
    require(h.core->inflightRequests() == 1 && h.core->inflightBytes() == 4096, "The window did not count the request");
    require(h.returnFrame(out, 0, {}, 4096) && h.core->inflightRequests() == 0 && h.core->inflightBytes() == 0, "The return did not free the window");
    require(h.kernelCount() == 2, "The kernel was not answered");
    reply = h.kernel(1);
    require(usb::get32(&reply[4]) == 2 && usb::get32(&reply[12]) == 0 && usb::get32(&reply[24]) == 4096 && reply.size() == 48, "The OUT answer is wrong");

    require(h.core->submit(1, urbIn(3, 1, 512), {}), "bulk IN");
    auto in = h.frame(1);
    require(in.endpoint == 0x81 && in.length == 512 && in.payload.empty(), "The IN request is wrong");
    require(h.returnFrame(in, 0, usb::Bytes(100, 7)), "IN return");
    reply = h.kernel(2);
    require(usb::get32(&reply[4]) == 3 && usb::get32(&reply[24]) == 100 && reply.size() == 148 && reply[48] == 7, "A short IN answer was not delivered as it came");

    // A stall is a normal answer.
    require(h.core->submit(1, urbIn(4, 1, 64), {}), "bulk IN 2");
    require(h.returnFrame(h.frame(2), -32), "stall return");
    reply = h.kernel(3);
    require(int32_t(usb::get32(&reply[20])) == -32 && reply.size() == 48, "A stall was not passed on");

    // A control IN transfer to the device (not a descriptor): forwarded with its setup.
    usb::Urb status = urbIn(5, 0, 2); const uint8_t setup2[8] = {0x80, 0, 0, 0, 0, 0, 2, 0}; std::memcpy(status.setup, setup2, 8);
    require(h.core->submit(1, status, {}), "control");
    const auto control = h.frame(3);
    require(control.transfer == 0 && control.endpoint == 0x80 && control.length == 2 && std::memcmp(control.setup, setup2, 8) == 0, "The control frame is wrong");
    require(h.returnFrame(control, 0, {0, 0}), "control return");
    require(usb::get32(&h.kernel(4)[24]) == 2, "The control answer is wrong");

    // Requests the VM may not make of the PC's device are answered locally.
    usb::Urb setAddress = urbOut(6, 0, 0); const uint8_t setup3[8] = {0x00, 5, 9, 0, 0, 0, 0, 0}; std::memcpy(setAddress.setup, setup3, 8);
    const size_t before = h.frameCount();
    require(h.core->submit(1, setAddress, {}) && h.frameCount() == before && int32_t(usb::get32(&h.kernel(5)[20])) == -32, "SET_ADDRESS reached the device");
    usb::Urb testMode = urbOut(7, 0, 0); const uint8_t setup4[8] = {0x00, 3, 2, 0, 0, 0, 0, 0}; std::memcpy(testMode.setup, setup4, 8);
    require(h.core->submit(1, testMode, {}) && h.frameCount() == before, "TEST_MODE reached the device");
    // An endpoint the validated device does not have is a stall, not a request.
    require(h.core->submit(1, urbOut(8, 3, 16), usb::Bytes(16, 0)) && h.frameCount() == before && int32_t(usb::get32(&h.kernel(7)[20])) == -32, "A foreign endpoint reached the client");
    // Isochronous, a bad length or a payload that disagrees end the connection.
    auto iso = urbOut(9, 2, 8); iso.packets = 4;
    require(!h.core->submit(1, iso, usb::Bytes(8, 0)), "An isochronous request was served");
    require(!h.core->submit(1, urbOut(10, 2, 8), usb::Bytes(7, 0)), "A payload shorter than its length was served");
    require(!h.core->submit(1, urbIn(11, 1, 8), usb::Bytes(8, 0)), "An IN request with data was served");
    require(!h.core->submit(1, urbOut(12, 2, -1), {}), "A negative length was served");
    require(!h.core->submit(2, urbOut(13, 2, 0), {}), "A request for an unknown device was served");
    usb::Urb badControl = urbIn(14, 0, 18); const uint8_t setup5[8] = {0x80, 6, 0, 1, 0, 0, 17, 0}; std::memcpy(badControl.setup, setup5, 8);
    require(!h.core->submit(1, badControl, {}), "A control request whose wLength disagrees was served");
  }
  // A transfer larger than one relay frame is split, and a short chunk ends an IN transfer.
  { Harness h; h.attached(1, stick);
    const size_t total = 700 * 1024; usb::Bytes data(total); for (size_t i = 0; i < total; ++i) data[i] = uint8_t(i * 7);
    require(h.core->submit(1, urbOut(1, 2, int32_t(total)), data), "large OUT");
    require(h.frameCount() == 1 && h.frame(0).length == usb::maxUrbBytes, "The first chunk is not one relay frame");
    require(h.returnFrame(h.frame(0), 0, {}, usb::maxUrbBytes) && h.frameCount() == 2 && h.kernelCount() == 0, "The second chunk was not sent after the first returned");
    require(h.frame(1).seq != h.frame(0).seq && h.frame(1).payload == usb::Bytes(data.begin() + long(usb::maxUrbBytes), data.begin() + long(2 * usb::maxUrbBytes)), "The second chunk is wrong");
    require(h.returnFrame(h.frame(1), 0, {}, usb::maxUrbBytes) && h.frameCount() == 3 && h.frame(2).length == total - 2 * usb::maxUrbBytes, "The last chunk is wrong");
    require(h.returnFrame(h.frame(2), 0, {}, uint32_t(total - 2 * usb::maxUrbBytes)) && h.kernelCount() == 1, "The kernel was not answered at the end");
    require(usb::get32(&h.kernel(0)[24]) == total && h.core->inflightBytes() == 0, "The total moved is wrong");
    // IN: a short chunk ends the transfer.
    require(h.core->submit(1, urbIn(2, 1, int32_t(total)), {}) && h.frame(3).length == usb::maxUrbBytes, "large IN");
    require(h.returnFrame(h.frame(3), 0, usb::Bytes(usb::maxUrbBytes, 1)) && h.frameCount() == 5, "The second IN chunk was not requested");
    require(h.returnFrame(h.frame(4), 0, usb::Bytes(10, 2)) && h.kernelCount() == 2, "A short chunk did not end the transfer");
    const auto reply = h.kernel(1);
    require(usb::get32(&reply[24]) == usb::maxUrbBytes + 10 && reply.size() == 48 + usb::maxUrbBytes + 10, "The IN transfer was not assembled");
    // An error in the middle ends it with that status.
    require(h.core->submit(1, urbIn(3, 1, int32_t(total)), {}) && h.returnFrame(h.frame(5), -71), "error return");
    require(int32_t(usb::get32(&h.kernel(2)[20])) == -71, "The error status was lost"); }
}

void returnsAreChecked() {
  const auto stick = vector("mass storage stick");
  const auto make = [&](Harness& h) { h.attached(1, stick); require(h.core->submit(1, urbIn(1, 1, 64), {}), "submit"); return h.frame(0); };
  { Harness h; auto request = make(h); request.seq += 1; require(!h.returnFrame(request, 0), "A return for an unknown request was accepted"); }
  { Harness h; auto request = make(h); request.endpoint = 0x82; require(!h.returnFrame(request, 0), "A return on another endpoint was accepted"); }
  { Harness h; auto request = make(h); request.transfer = 3; require(!h.returnFrame(request, 0), "A return of another transfer type was accepted"); }
  { Harness h; auto request = make(h); require(!h.returnFrame(request, 0, usb::Bytes(65, 0)), "A return longer than asked was accepted");
    // The refused return leaves the request pending, so the VM's unlink of it
    // is answered normally (it must not find the kernel number with no request behind it and throw).
    const size_t frames = h.frameCount(); bool threw = false, unlinked = false;
    try { unlinked = h.core->unlink(1, unlinkOf(2, 1)); } catch (...) { threw = true; }
    require(!threw && unlinked && h.frameCount() == frames + 1, "An unlink after a refused return threw or was not sent to the client"); }
  { Harness h; auto request = make(h); require(!h.returnFrame(request, 0, usb::Bytes(4, 0), 5), "A payload that disagrees with its length was accepted"); }
  { Harness h; auto request = make(h); require(h.returnFrame(request, 0, usb::Bytes(4, 0)) && !h.returnFrame(request, 0, usb::Bytes(4, 0)), "A request was answered twice"); }
  { Harness h; auto request = make(h); request.type = usb::frameSubmit; const auto bytes = usb::encodeFrame(request); require(!h.core->frame(bytes.data(), bytes.size()), "A client submit was accepted"); }
  { Harness h; h.attached(1, stick); const uint8_t shortFrame[10] = {}; require(!h.core->frame(shortFrame, sizeof(shortFrame)), "A truncated frame was accepted"); }
  { Harness h; h.attached(1, stick); usb::Frame late; late.type = usb::frameReturn; late.device = 9; late.seq = 5; const auto bytes = usb::encodeFrame(late);
    require(h.core->frame(bytes.data(), bytes.size()), "A late answer for a device that is gone ended the channel"); }
}

void window() {
  const auto stick = vector("mass storage stick");
  // The window is bytes and requests, shared by the session; the kernel's thread waits for room.
  { Harness h; h.attached(1, stick);
    for (uint32_t n = 1; n <= 4; ++n) require(h.core->submit(1, urbIn(n, 1, int32_t(usb::maxUrbBytes)), {}), "fill the window");
    require(h.core->inflightBytes() == 4 * usb::maxUrbBytes, "The window did not fill");
    std::atomic<bool> done{false}; std::atomic<bool> result{false};
    std::thread waiter([&] { result = h.core->submit(1, urbIn(5, 1, 4096), {}); done = true; });
    Sleep(150);
    require(!done && h.frameCount() == 4, "A request went out over a full window");
    require(h.returnFrame(h.frame(0), 0, usb::Bytes(10, 0)), "free some room");
    require(waitFor([&] { return done.load(); }) && result && h.frameCount() == 5, "The waiting request did not go out when room appeared");
    waiter.join(); }
  { Harness h; h.attached(1, stick);
    for (uint32_t n = 1; n <= 32; ++n) require(h.core->submit(1, urbIn(n, 1, 8), {}), "fill the count");
    std::atomic<bool> done{false};
    std::thread waiter([&] { h.core->submit(1, urbIn(33, 1, 8), {}); done = true; });
    Sleep(100); require(!done, "The 33rd request went out");
    h.core->closed();                               // a closing session releases the waiter
    require(waitFor([&] { return done.load(); }), "A waiting request was not released by closing"); waiter.join(); }
  // A full relay queue ends the connection instead of losing the request.
  { Harness h; h.attached(1, stick); h.relayFull = true;
    require(!h.core->submit(1, urbIn(1, 1, 8), {}), "A request the relay could not take was reported as sent"); }
}

void unlinking() {
  const auto stick = vector("mass storage stick");
  { Harness h; h.attached(1, stick);
    require(h.core->submit(1, urbIn(1, 1, 512), {}), "submit");
    require(h.core->unlink(1, unlinkOf(2, 1)), "unlink");
    const auto frame = h.frame(1);
    require(frame.type == usb::frameUnlink && frame.length == h.frame(0).seq && frame.seq != h.frame(0).seq, "The unlink frame is wrong");
    require(h.kernelCount() == 0, "The kernel was answered before the request ended");
    // The client answers the unlink and returns the cancelled request: the request is not answered, its unlink is.
    usb::Frame ack; ack.type = usb::frameUnlinkReturn; ack.device = 1; ack.seq = frame.seq; ack.status = -104; const auto ackBytes = usb::encodeFrame(ack);
    require(h.core->frame(ackBytes.data(), ackBytes.size()), "unlink-return");
    require(h.returnFrame(h.frame(0), -104), "cancelled return");
    require(h.kernelCount() == 1, "A cancelled request produced a wrong number of answers");
    const auto reply = h.kernel(0);
    require(usb::get32(&reply[0]) == usb::retUnlink && usb::get32(&reply[4]) == 2 && int32_t(usb::get32(&reply[20])) == -104, "The unlink answer is wrong");
    require(h.core->inflightRequests() == 0, "A cancelled request kept the window"); }
  { Harness h; h.attached(1, stick);
    // An unlink of a request that already completed is answered at once with status 0.
    require(h.core->submit(1, urbIn(1, 1, 8), {}) && h.returnFrame(h.frame(0), 0, usb::Bytes(8, 0)), "complete");
    require(h.core->unlink(1, unlinkOf(2, 1)) && h.frameCount() == 1, "A completed request was unlinked at the client");
    const auto reply = h.kernel(1);
    require(usb::get32(&reply[0]) == usb::retUnlink && int32_t(usb::get32(&reply[20])) == 0, "A completed request's unlink was not answered with 0");
    // An unknown unlink acknowledgement is ignored; a Host-unknown device cannot be unlinked.
    require(!h.core->unlink(3, unlinkOf(3, 1)), "An unlink for an unknown device was accepted"); }
}

void detaching() {
  const auto stick = vector("mass storage stick");
  // The client removes the device (unplugged on the PC or the user stopped it).
  { Harness h; h.attached(1, stick);
    require(h.core->submit(1, urbIn(1, 1, 512), {}), "submit");
    require(h.core->message(JsonParser::parse("{\"type\":\"usb.detach\",\"device\":1,\"reason\":\"usb_device_removed\"}")), "detach");
    require(h.detached == std::vector<int>{7} && h.kernelClosed && h.core->deviceCount() == 0 && h.core->inflightRequests() == 0 && h.core->inflightBytes() == 0, "A removed device stayed attached or kept the window");
    require(h.textCount("usb.detach") == 0, "The client was told about its own detach");
    require(h.core->message(JsonParser::parse("{\"type\":\"usb.detach\",\"device\":1,\"reason\":\"usb_device_removed\"}")), "A second detach ended the channel");
    require(h.core->message(JsonParser::parse("{\"type\":\"usb.detach\",\"device\":99,\"reason\":\"usb_user_stopped\"}")), "A detach of an unknown device ended the channel");
    require(h.returnFrame(h.frame(0), 0, usb::Bytes(8, 0)), "A late return for a removed device ended the channel"); }
  // A failed native PlugOut must remain observable, but must not throw through
  // client detach or stop the remaining session cleanup. No real IOCTL runs.
  for (const int behavior : {1, 2}) {
    { Harness h; h.attached(1, stick); h.detachBehavior = behavior;
      require(h.core->message(JsonParser::parse("{\"type\":\"usb.detach\",\"device\":1,\"reason\":\"usb_user_stopped\"}")), "Native detach failure ended the channel");
      require(h.detached == std::vector<int>{7} && h.kernelClosed && h.core->deviceCount() == 0, "Native detach failure blocked logical/kernel cleanup");
      require(h.textCount("usb.detach") == 0, "Native failure invented an unnegotiated peer acknowledgement"); }
    { Harness h; h.attached(1, stick); require(h.attach(2, vector("vendor specific serial chip")) && waitFor([&] { return h.hasText("\"usb.attached\",\"device\":2"); }), "second cleanup device");
      h.detachBehavior = behavior; h.core->closed();
      require(h.detached == std::vector<int>({7, 8}) && h.core->deviceCount() == 0, "One native failure skipped another port's cleanup");
      h.core->closed(); require(h.detached.size() == 2, "Repeated close repeated a failed native detach"); }
  }
  // The broker revokes (class permission): the same, and the other device stays.
  { Harness h; h.attached(1, stick); require(h.attach(2, vector("vendor specific serial chip")) && waitFor([&] { return h.hasText("\"usb.attached\",\"device\":2"); }), "second device");
    require(h.core->message(JsonParser::parse("{\"type\":\"usb.detach\",\"device\":1,\"reason\":\"usb_policy_changed\"}")), "revoke");
    require(h.core->deviceCount() == 1 && h.core->deviceExists(2) && !h.core->deviceExists(1), "Revoking one device removed the other"); }
  // The VM ends the kernel connection: the client is told, the port unplugged.
  { Harness h; h.attached(1, stick);
    h.core->kernelClosed(1);
    require(h.hasText("{\"type\":\"usb.detach\",\"device\":1,\"reason\":\"usb_vm_detached\"}") && h.detached == std::vector<int>{7} && h.core->deviceCount() == 0, "A VM-side detach was not passed on");
    h.core->kernelClosed(1); require(h.textCount("usb_vm_detached") == 1, "A second close was reported again"); }
  // The session ends (revocation of usbDevices, stream end, crash): nothing stays attached.
  { Harness h; h.attached(1, stick); require(h.attach(2, vector("vendor specific serial chip")) && waitFor([&] { return h.hasText("\"usb.attached\",\"device\":2"); }), "second device");
    require(h.core->submit(1, urbIn(1, 1, 512), {}), "submit");
    h.core->closed();
    require(h.detached.size() == 2 && h.core->deviceCount() == 0 && h.core->inflightBytes() == 0 && h.cancels >= 1, "Closing left devices attached");
    require(!h.core->usable() && !h.core->submit(1, urbIn(2, 1, 8), {}), "A closed session served a request");
    h.core->closed(); require(h.detached.size() == 2, "Closing twice detached twice"); }
  // A plug-in that returns after the device was removed is unplugged again.
  for (const int behavior : {0, 1, 2}) { Harness h; std::atomic<bool> release{false}; std::atomic<int> started{0};
    UsbHooks hooks;
    hooks.sendText = [&](const std::string&) { return true; }; hooks.sendFrame = [](const usb::Bytes&) { return true; };
    hooks.prepare = [] { return UsbPrepared{true, ""}; };
    hooks.attach = [&](const std::string&) { ++started; while (!release) Sleep(5); return 9; };
    std::vector<int> unplugged; std::mutex lock;
    UsbRedirection::Options options;
    options.detach = [&](int port) { std::lock_guard guard(lock); unplugged.push_back(port); if (behavior == 2) throw std::runtime_error("synthetic late PlugOut failure"); return behavior != 1; };
    hooks.detach = observedUsbDetach(options.detach);
    UsbSessionCore core(session, {}, std::move(hooks));
    core.message(JsonParser::parse("{\"type\":\"usb.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"usb-v1\",\"limits\":{\"maxUrbBytes\":262144,\"windowBytes\":1048576,\"maxInFlight\":32,\"maxDevices\":4,\"maxAttaches\":64}}"));
    Sleep(100);
    core.message(JsonParser::parse("{\"type\":\"usb.attach\",\"device\":1,\"descriptors\":\"" + toBase64(vector("vendor specific serial chip")) + "\",\"speed\":\"high\"}"));
    require(waitFor([&] { return started == 1; }), "The plug-in did not start");
    core.message(JsonParser::parse("{\"type\":\"usb.detach\",\"device\":1,\"reason\":\"usb_user_stopped\"}"));
    release = true;
    core.closed();
    std::lock_guard guard(lock);
    require(unplugged == std::vector<int>{9}, "A plug-in finished after the removal stayed plugged"); }
}

// --- a field failure: a stick whose USBSTOR never started -------------------------------------------------------
usb::Urb urbControl(uint32_t seq, std::array<uint8_t, 8> setup) {
  usb::Urb urb; urb.command = usb::cmdSubmit; urb.seq = seq; urb.direction = (setup[0] & 0x80) ? 1 : 0; urb.endpoint = 0; urb.length = int32_t(setup[6] | setup[7] << 8); urb.packets = -1;
  std::memcpy(urb.setup, setup.data(), 8); return urb;
}
// The status the kernel's answer number [index] carries, and its length.
int32_t kernelStatus(Harness& h, size_t index) { return int32_t(usb::get32(&h.kernel(index)[20])); }
uint32_t kernelLength(Harness& h, size_t index) { return usb::get32(&h.kernel(index)[24]); }

// The Microsoft OS string (0xEE) and the BOS descriptor are refused for the VM
// (a STALL, never forwarded), so an allowed device cannot ask the VM's Windows for another driver by compatible ID.
void descriptorRequestsRefused() {
  require(usb::refusedDescriptorRequest(std::array<uint8_t, 8>{0x80, 6, 0xee, 3, 0, 0, 255, 0}.data()) &&
          usb::refusedDescriptorRequest(std::array<uint8_t, 8>{0x80, 6, 0, 0x0f, 0, 0, 5, 0}.data()), "An MS OS string or BOS request was let through");
  require(!usb::refusedDescriptorRequest(std::array<uint8_t, 8>{0x80, 6, 1, 3, 9, 4, 255, 0}.data()) &&
          !usb::refusedDescriptorRequest(std::array<uint8_t, 8>{0x80, 6, 0, 1, 0, 0, 18, 0}.data()) &&
          !usb::refusedDescriptorRequest(std::array<uint8_t, 8>{0xc0, 6, 0xee, 3, 0, 0, 255, 0}.data()) &&
          !usb::refusedDescriptorRequest(std::array<uint8_t, 8>{0x00, 7, 0, 0x0f, 0, 0, 0, 0}.data()), "An ordinary request was refused");
  const auto stick = vector("mass storage stick");
  Harness h; h.attached(1, stick);
  const size_t frames = h.frameCount(), answers = h.kernelCount();
  require(h.core->submit(1, urbControl(1, {0x80, 6, 0xee, 3, 0, 0, 255, 0}), {}) && h.core->submit(1, urbControl(2, {0x80, 6, 0, 0x0f, 0, 0, 5, 0}), {}),
          "A refused descriptor request ended the connection");
  require(h.frameCount() == frames && h.kernelCount() == answers + 2 && kernelStatus(h, answers) == -32 && kernelStatus(h, answers + 1) == -32,
          "An MS OS string or BOS request reached the device or was not stalled");
  require(h.core->submit(1, urbControl(3, {0x80, 6, 1, 3, 9, 4, 255, 0}), {}) && h.frameCount() == frames + 1, "An ordinary string request did not reach the device");
}

void storageStart() {
  // The exchange Windows' USBSTOR makes to start a bulk-only stick, played from the VM's kernel through the
  // Host core to a fake client, request by request. Every step must reach the client unchanged (or be answered
  // locally from the validated descriptors) and nothing may be refused: a refusal or a request that is never
  // answered leaves USBSTOR waiting.
  const auto stick = vector("mass storage stick");
  Harness h; h.attached(1, stick);
  uint32_t seq = 0; size_t answers = 0, frames = 0;
  const auto local = [&](std::array<uint8_t, 8> setup, size_t expect) {
    require(h.core->submit(1, urbControl(++seq, setup), {}) && h.frameCount() == frames && h.kernelCount() == ++answers, "A descriptor request was not answered locally");
    require(kernelStatus(h, answers - 1) == 0 && kernelLength(h, answers - 1) == expect, "A local descriptor answer is wrong");
  };
  const auto forwarded = [&](usb::Urb urb, usb::Bytes data, int32_t status, usb::Bytes reply, uint32_t expectLength, const char* what) {
    require(h.core->submit(1, urb, std::move(data)), what);
    require(h.frameCount() == ++frames, (std::string("The request never reached the client: ") + what).c_str());
    const auto sent = h.frame(frames - 1);
    require(sent.type == usb::frameSubmit && sent.seq != 0, what);
    const bool input = (sent.endpoint & 0x80) != 0;   // an OUT request is answered with the bytes written
    require(h.returnFrame(sent, status, std::move(reply), status == 0 && !input ? sent.length : UINT32_MAX), (std::string("The answer was refused: ") + what).c_str());
    require(h.kernelCount() == ++answers && kernelStatus(h, answers - 1) == status && kernelLength(h, answers - 1) == expectLength, (std::string("The kernel got a wrong answer: ") + what).c_str());
    return sent;
  };
  local({0x80, 6, 0, 1, 0, 0, 8, 0}, 8);          // GET_DESCRIPTOR(device, 8): the first look
  local({0x80, 6, 0, 1, 0, 0, 18, 0}, 18);
  local({0x80, 6, 0, 2, 0, 0, 9, 0}, 9);           // configuration header, then everything
  local({0x80, 6, 0, 2, 0, 0, 255, 0}, uint32_t(stick.size() - 18));
  // The serial number string (not held by the Host: the device answers it).
  auto sent = forwarded(urbControl(++seq, {0x80, 6, 3, 3, 9, 4, 255, 0}), {}, 0, usb::Bytes(26, 0x33), 26, "GET_DESCRIPTOR(string)");
  require(sent.transfer == 0 && sent.endpoint == 0x80 && sent.length == 255 && sent.setup[1] == 6, "The string request changed on the way");
  // SET_CONFIGURATION: the client answers it itself (a UsbDk call), the Host forwards it.
  forwarded(urbControl(++seq, {0x00, 9, 1, 0, 0, 0, 0, 0}), {}, 0, {}, 0, "SET_CONFIGURATION");
  // GET_MAX_LUN is a class request to the interface: forwarded as it is. Many sticks stall it; a stall is the answer.
  sent = forwarded(urbControl(++seq, {0xa1, 0xfe, 0, 0, 0, 0, 1, 0}), {}, -32, {}, 0, "GET_MAX_LUN stalled");
  require(sent.transfer == 0 && sent.endpoint == 0x80 && sent.length == 1 && sent.setup[0] == 0xa1 && sent.setup[1] == 0xfe && sent.setup[6] == 1, "GET_MAX_LUN was changed or refused");
  forwarded(urbControl(++seq, {0xa1, 0xfe, 0, 0, 0, 0, 1, 0}), {}, 0, {0}, 1, "GET_MAX_LUN answered");
  // Bulk-only transport: CBW out (31 bytes), data in (36), CSW in (13): INQUIRY.
  sent = forwarded(urbOut(++seq, 2, 31), usb::Bytes(31, 0x55), 0, {}, 31, "CBW");
  require(sent.transfer == 2 && sent.endpoint == 0x02 && sent.payload.size() == 31, "The CBW is wrong");
  forwarded(urbIn(++seq, 1, 36), {}, 0, usb::Bytes(36, 0x01), 36, "INQUIRY data");
  forwarded(urbIn(++seq, 1, 13), {}, 0, usb::Bytes(13, 0x02), 13, "INQUIRY CSW");
  // TEST UNIT READY: a stalled CSW, then clear-halt of the bulk-in endpoint, then the CSW again.
  forwarded(urbOut(++seq, 2, 31), usb::Bytes(31, 0x55), 0, {}, 31, "CBW 2");
  forwarded(urbIn(++seq, 1, 13), {}, -32, {}, 0, "CSW stalled");
  sent = forwarded(urbControl(++seq, {0x02, 1, 0, 0, 0x81, 0, 0, 0}), {}, 0, {}, 0, "CLEAR_FEATURE(HALT)");
  require(sent.setup[0] == 0x02 && sent.setup[1] == 1 && sent.setup[4] == 0x81, "CLEAR_FEATURE(ENDPOINT_HALT) was changed or refused");
  forwarded(urbIn(++seq, 1, 13), {}, 0, usb::Bytes(13, 0x03), 13, "CSW again");
  // READ CAPACITY(10): 8 bytes in. Bulk-only mass storage reset (class request, no data) and the clear-halts after it.
  forwarded(urbOut(++seq, 2, 31), usb::Bytes(31, 0x55), 0, {}, 31, "CBW 3");
  forwarded(urbIn(++seq, 1, 8), {}, 0, usb::Bytes(8, 0x04), 8, "READ CAPACITY data");
  forwarded(urbIn(++seq, 1, 13), {}, 0, usb::Bytes(13, 0x05), 13, "READ CAPACITY CSW");
  sent = forwarded(urbControl(++seq, {0x21, 0xff, 0, 0, 0, 0, 0, 0}), {}, 0, {}, 0, "Bulk-Only Mass Storage Reset");
  require(sent.setup[0] == 0x21 && sent.setup[1] == 0xff, "The mass storage reset was changed or refused");
  forwarded(urbControl(++seq, {0x02, 1, 0, 0, 0x02, 0, 0, 0}), {}, 0, {}, 0, "CLEAR_FEATURE(HALT) out");
  // A zero-length bulk OUT and a bulk request larger than the CSW (the stack may ask for more than it needs).
  forwarded(urbOut(++seq, 2, 0), {}, 0, {}, 0, "zero-length OUT");
  forwarded(urbIn(++seq, 1, 512), {}, 0, usb::Bytes(13, 0x06), 13, "short IN");
  require(h.core->inflightRequests() == 0 && h.core->inflightBytes() == 0, "The window was not given back");
  require(h.core->deviceExists(1) && h.textCount("usb.detach") == 0, "The device was dropped during a normal start");
}

void cancelledRequestsDoNotHangTheVm() {
  // The failure behind the field report: a request the device never answers (GET_MAX_LUN to a stick that ignores it)
  // is cancelled by the VM after its own timeout. The cancel must be answered even though the PC's device never lets
  // the request go; otherwise the VM waits for ever and the device node never starts.
  const auto stick = vector("mass storage stick");
  const auto unlinkAck = [](Harness& h, const usb::Frame& unlink, int32_t status) {
    usb::Frame ack; ack.type = usb::frameUnlinkReturn; ack.device = 1; ack.seq = unlink.seq; ack.status = status; const auto bytes = usb::encodeFrame(ack);
    return h.core->frame(bytes.data(), bytes.size());
  };
  { Harness h; h.attached(1, stick);
    require(h.core->submit(1, urbControl(5, {0xa1, 0xfe, 0, 0, 0, 0, 1, 0}), {}) && h.frameCount() == 1, "GET_MAX_LUN");
    require(h.core->unlink(1, unlinkOf(6, 5)) && h.frameCount() == 2, "unlink");
    require(h.kernelCount() == 0, "The cancel was answered before the client confirmed it");
    require(unlinkAck(h, h.frame(1), -104), "ack");
    require(h.kernelCount() == 1 && usb::get32(&h.kernel(0)[0]) == usb::retUnlink && usb::get32(&h.kernel(0)[4]) == 6 && int32_t(usb::get32(&h.kernel(0)[20])) == -104, "The VM's cancel was not answered when the client confirmed it");
    require(h.core->inflightRequests() == 1, "The window slot was released before the request was returned (the relay still counts it)");
    // The device lets the request go much later: nothing more reaches the kernel, the slot is free, nothing ends.
    require(h.returnFrame(h.frame(0), -104), "late return");
    require(h.kernelCount() == 1 && h.core->inflightRequests() == 0 && h.core->inflightBytes() == 0 && h.core->deviceExists(1), "A late return of a cancelled request was passed on or kept the window");
    // The same sequence number is not reusable by mistake: a repeated unlink of the same request is answered as already completed.
    require(h.core->unlink(1, unlinkOf(7, 5)) && h.kernelCount() == 2 && int32_t(usb::get32(&h.kernel(1)[20])) == 0, "A second cancel of a finished request");
  }
  { Harness h; h.attached(1, stick);
    // The client never confirms: after the limit the cancel is answered anyway.
    require(h.core->submit(1, urbControl(5, {0xa1, 0xfe, 0, 0, 0, 0, 1, 0}), {}) && h.core->unlink(1, unlinkOf(6, 5)), "submit and unlink");
    h.nowMs = 2900; h.core->tick(); require(h.kernelCount() == 0, "A cancel was answered too early");
    h.nowMs = 3100; h.core->tick();
    require(h.kernelCount() == 1 && int32_t(usb::get32(&h.kernel(0)[20])) == -104, "An unconfirmed cancel was never answered");
    h.core->tick(); require(h.kernelCount() == 1, "A cancel was answered twice");
    require(unlinkAck(h, h.frame(1), -104) && h.returnFrame(h.frame(0), -104) && h.kernelCount() == 1, "The late confirmation or return reached the kernel");
  }
  { Harness h; h.attached(1, stick);
    // The client says the request was already done (status 0): the request's own return is the answer, as before.
    require(h.core->submit(1, urbIn(5, 1, 13), {}) && h.core->unlink(1, unlinkOf(6, 5)), "submit and unlink");
    require(unlinkAck(h, h.frame(1), 0) && h.kernelCount() == 0, "A completed request's cancel was answered before its return");
    require(h.returnFrame(h.frame(0), 0, usb::Bytes(13, 1)) && h.kernelCount() == 1 && usb::get32(&h.kernel(0)[0]) == usb::retUnlink, "The return of a request that raced its cancel was not turned into the cancel's answer");
  }
  { Harness h; h.attached(1, stick);
    // A device wedged on the PC: too many cancelled requests that never come back end the attachment, with a reason.
    for (uint32_t n = 0; n < 10; ++n) {
      require(h.core->submit(1, urbControl(100 + n, {0xa1, 0xfe, 0, 0, 0, 0, 1, 0}), {}), "submit");
      require(h.core->unlink(1, unlinkOf(200 + n, 100 + n)), "unlink");
      require(unlinkAck(h, h.frame(2 * n + 1), -104), "ack");
    }
    h.core->tick();
    require(!h.core->deviceExists(1) && h.hasText("usb_device_error") && h.detached == std::vector<int>{7}, "A wedged device stayed attached without a reason"); }
  { Harness h; h.attached(1, stick);
    // A control request nobody answers is noted once and changes nothing else.
    require(h.core->submit(1, urbControl(5, {0xa1, 0xfe, 0, 0, 0, 0, 1, 0}), {}), "submit");
    h.nowMs = 6000; h.core->tick(); h.core->tick();
    require(h.core->deviceExists(1) && h.kernelCount() == 0 && h.core->inflightRequests() == 1, "Noting a slow request changed the state"); }
}

void vmDeviceNeverStarts() {
  // The field report's own symptom: the VM's device node for the attached device never starts. The attachment is
  // ended with a reason after the limit (or at once when Windows names a failed start), so the client can show it and
  // the device goes back to the PC; a problem that is the VM user's to solve (no driver) never ends it.
  const auto stick = vector("mass storage stick");
  { Harness h; h.vmState = {1, 0}; h.attached(1, stick);
    h.nowMs = 10000; h.core->tick(); h.nowMs = 30000; h.core->tick();
    require(h.core->deviceExists(1) && h.vmPolls >= 2, "A device still starting was ended early");
    h.nowMs = 47000; h.core->tick();
    require(!h.core->deviceExists(1) && h.hasText("{\"type\":\"usb.detach\",\"device\":1,\"reason\":\"usb_device_error\"}") && h.detached == std::vector<int>{7} && h.kernelClosed, "A device that never started stayed attached"); }
  { Harness h; h.vmState = {1, 10}; h.attached(1, stick);
    h.nowMs = 1500; h.core->tick(); require(h.core->deviceExists(1), "One failed-start observation ended the device");
    h.nowMs = 3000; h.core->tick();
    require(!h.core->deviceExists(1) && h.hasText("usb_device_error"), "A failed start (problem 10) did not end the attachment"); }
  { Harness h; h.vmState = {1, 28}; h.attached(1, stick);
    h.nowMs = 120000; h.core->tick(); h.core->tick();
    require(h.core->deviceExists(1), "A missing driver ended the attachment"); }
  // A device Windows has no driver for (problem 28) is the desktop administrator's to solve (a standard Windows user
  // cannot install one): the attachment stays, and the Host records Windows' problem code once (hostAction 250
  // stage 28) instead of nothing.
  { Harness h; h.vmState = {1, 28}; h.attached(1, stick);
    h.nowMs = 2000; h.core->tick(); h.nowMs = 4000; h.core->tick(); h.nowMs = 6000; h.core->tick();
    require(h.core->vmProblemsNoted() == 0, "A problem was recorded before Windows had time to install the device");
    h.nowMs = 11000; h.core->tick();
    require(h.core->deviceExists(1) && h.core->vmProblemsNoted() == 1, "A device without a driver was not recorded");
    h.nowMs = 60000; h.core->tick(); h.nowMs = 120000; h.core->tick();
    require(h.core->deviceExists(1) && h.core->vmProblemsNoted() == 1, "The same problem was recorded again or ended the device"); }
  // A passing problem (Windows still installing) is not recorded; a device that starts is never recorded.
  { Harness h; h.vmState = {1, 28}; h.attached(1, stick);
    h.nowMs = 9000; h.core->tick(); h.nowMs = 10500; h.core->tick(); h.vmState = {2, 0}; h.nowMs = 12000; h.core->tick(); h.nowMs = 60000; h.core->tick();
    require(h.core->deviceExists(1) && h.core->vmProblemsNoted() == 0, "A device that started was recorded as failed"); }
  // Another problem starts the count again.
  { Harness h; h.vmState = {1, 28}; h.attached(1, stick);
    h.nowMs = 10000; h.core->tick(); h.vmState = {1, 52}; h.nowMs = 11500; h.core->tick(); h.nowMs = 13000; h.core->tick();
    require(h.core->vmProblemsNoted() == 0, "Two different problems were counted as one");
    h.nowMs = 14500; h.core->tick();
    require(h.core->vmProblemsNoted() == 1 && h.core->deviceExists(1), "A lasting second problem was not recorded"); }
  // The polls must follow each other: one without the problem starts the count again.
  { Harness h; h.vmState = {1, 28}; h.attached(1, stick);
    h.nowMs = 10000; h.core->tick(); h.nowMs = 11500; h.core->tick(); h.vmState = {1, 0}; h.nowMs = 13000; h.core->tick();
    h.vmState = {1, 28}; h.nowMs = 14500; h.core->tick(); h.nowMs = 16000; h.core->tick();
    require(h.core->vmProblemsNoted() == 0, "Polls with a gap were counted as following each other");
    h.nowMs = 17500; h.core->tick();
    require(h.core->vmProblemsNoted() == 1 && h.core->deviceExists(1), "Three polls in a row were not recorded"); }
  // A failed start (problem 10) keeps its own rule and is not counted here.
  { Harness h; h.vmState = {1, 10}; h.attached(1, stick);
    h.nowMs = 1500; h.core->tick(); h.nowMs = 3000; h.core->tick();
    require(!h.core->deviceExists(1) && h.core->vmProblemsNoted() == 0, "A failed start was recorded as a missing driver"); }
  { Harness h; h.vmState = {1, 0}; h.attached(1, stick);
    h.nowMs = 20000; h.core->tick(); h.vmState = {2, 0}; h.nowMs = 25000; h.core->tick();
    const auto polls = h.vmPolls; h.nowMs = 200000; h.core->tick();
    require(h.core->deviceExists(1) && h.vmPolls == polls, "A started device was polled or ended later"); }
  // The real lookup: a vendor/product that no device has is simply absent (read-only).
  require(usb::vmDeviceState(0xffff, 0xfffe).state == 0, "A device that does not exist was found in the VM's tree");
}
// --- 게임 마우스: Multitap's own virtual mouse (usb_virtual_mouse.hpp) -------------------------------------------
usb::Bytes mouseReport(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel = 0, int8_t pan = 0) {
  return {buttons, uint8_t(dx), uint8_t(uint16_t(dx) >> 8), uint8_t(dy), uint8_t(uint16_t(dy) >> 8), uint8_t(wheel), uint8_t(pan)};
}
usb::Bytes kernelPayload(Harness& h, size_t index) { const auto bytes = h.kernel(index); return usb::Bytes(bytes.begin() + 48, bytes.end()); }
void gameMouse() {
  const auto mouse = vector("multitap game mouse (report-protocol hid, made up by the client)");
  // The Host's copy is the shared vector byte for byte, and the policy needs no exception: report-protocol HID
  // (subclass 0) is not the boot mouse the deny list names, and it needs no class permission.
  require(mouse == usb::vmouse::descriptors() && usb::vmouse::matches(mouse.data(), mouse.size()), "The Host's virtual mouse differs from the shared vector");
  const auto parsed = usb::parseDescriptors(mouse.data(), mouse.size());
  require(parsed && usb::decide(*parsed).allowed && usb::decide(*parsed).requires_.empty() && parsed->configurations[0].interfaces[0].subclass == 0, "The virtual mouse is not plain report-protocol HID");
  require(!usb::decide(*usb::parseDescriptors(vector("boot mouse").data(), vector("boot mouse").size())).allowed, "A boot mouse is allowed now");
  auto other = mouse; other[10] = 0x02;   // another product id: an ordinary HID device, nothing special
  require(!usb::vmouse::matches(other.data(), other.size()) && !usb::vmouse::matches(mouse.data(), mouse.size() - 1), "Something else was taken for the virtual mouse");
  require(usb::vmouse::reportDescriptor().size() == 79 && mouse[18 + 25] == 79 && mouse[18 + 31] == 7, "The report descriptor and its length disagree");

  // Control: answered by the Host, nothing reaches the client (a client sees control only from older Hosts).
  { Harness h({false, false, false, false}); h.attached(1, mouse);
    uint32_t seq = 0;
    const auto local = [&](std::array<uint8_t, 8> setup, int32_t status, size_t length, const char* what) {
      const auto before = h.kernelCount();
      require(h.core->submit(1, urbControl(++seq, setup), {}) && h.kernelCount() == before + 1, what);
      require(kernelStatus(h, before) == status && kernelLength(h, before) == length, what);
      return kernelPayload(h, before);
    };
    local({0x80, 6, 0, 1, 0, 0, 18, 0}, 0, 18, "GET_DESCRIPTOR(device)");
    // Independent UTF16LE goldens: the product is display metadata, while the
    // manufacturer and shared device/configuration identity stay unchanged;
    // the fixed serial follows the current project-owned Multitap namespace.
    const usb::Bytes manufacturer{10, 3, 'T', 0, 'o', 0, 'T', 0, 'B', 0};
    require(local({0x80, 6, 1, 3, 9, 4, 255, 0}, 0, manufacturer.size(), "GET_DESCRIPTOR(manufacturer)") == manufacturer, "The manufacturer string changed");
    const usb::Bytes product{
        40, 3, 'M', 0, 'u', 0, 'l', 0, 't', 0, 'i', 0, 't', 0, 'a', 0, 'p', 0,
        ' ', 0, 'G', 0, 'a', 0, 'm', 0, 'e', 0, ' ', 0, 'M', 0, 'o', 0, 'u', 0, 's', 0, 'e', 0};
    require(local({0x80, 6, 2, 3, 9, 4, 255, 0}, 0, product.size(), "GET_DESCRIPTOR(string 2)") == product, "The full product string is wrong");
    require(local({0x80, 6, 2, 3, 9, 4, 2, 0}, 0, 2, "GET_DESCRIPTOR(product header)") == usb::Bytes({40, 3}), "The short product header is wrong");
    require(local({0x80, 6, 2, 3, 9, 4, 10, 0}, 0, 10, "GET_DESCRIPTOR(product prefix)") == usb::Bytes(product.begin(), product.begin() + 10), "The product string did not honour wLength");
    require(local({0x80, 6, 0, 3, 0, 0, 255, 0}, 0, 4, "languages")[2] == 0x09, "The language list is wrong");
    const usb::Bytes serial{
        40, 3, 'M', 0, 'U', 0, 'L', 0, 'T', 0, 'I', 0, 'T', 0, 'A', 0, 'P', 0, '-', 0,
        'G', 0, 'A', 0, 'M', 0, 'E', 0, '-', 0, 'M', 0, 'O', 0, 'U', 0, 'S', 0, 'E', 0};
    require(local({0x80, 6, 3, 3, 9, 4, 255, 0}, 0, serial.size(), "GET_DESCRIPTOR(serial)") == serial, "The Multitap serial string is wrong");
    local({0x80, 6, 9, 3, 9, 4, 255, 0}, -32, 0, "an unknown string");
    require(local({0x81, 6, 0, 0x22, 0, 0, 0x4f, 0}, 0, 79, "GET_DESCRIPTOR(report)") == usb::vmouse::reportDescriptor(), "The report descriptor was not the Host's");
    require(local({0x81, 6, 0, 0x22, 0, 0, 10, 0}, 0, 10, "a short report descriptor").size() == 10, "wLength was not honoured");
    local({0x81, 6, 0, 0x21, 0, 0, 9, 0}, 0, 9, "GET_DESCRIPTOR(HID)");
    local({0x00, 9, 1, 0, 0, 0, 0, 0}, 0, 0, "SET_CONFIGURATION");
    local({0x21, 0x0a, 0, 0, 0, 0, 0, 0}, 0, 0, "SET_IDLE");
    local({0x21, 0x0b, 1, 0, 0, 0, 0, 0}, 0, 0, "SET_PROTOCOL");
    local({0x80, 0, 0, 0, 0, 0, 2, 0}, 0, 2, "GET_STATUS");
    require(local({0xa1, 1, 0, 1, 0, 0, 7, 0}, 0, 7, "GET_REPORT(input)") == usb::Bytes(7, 0), "GET_REPORT was not the idle state");
    local({0xa1, 1, 0, 3, 0, 0, 7, 0}, -32, 0, "GET_REPORT(feature)");
    local({0xc0, 0x33, 0, 0, 0, 0, 4, 0}, -32, 0, "a vendor request");
    local({0x00, 5, 9, 0, 0, 0, 0, 0}, -32, 0, "SET_ADDRESS");
    local({0x00, 3, 2, 0, 0, 0, 0, 0}, -32, 0, "SET_FEATURE(TEST_MODE)");
    require(h.frameCount() == 0, "A control request of the virtual mouse reached the client");
    // An ordinary report-protocol HID device keeps today's path: its string and report requests go to the client.
    Harness plain({false, false, false, false}); plain.attached(1, vector("fido key (report-protocol hid)"));
    require(plain.core->submit(1, urbControl(1, {0x80, 6, 2, 3, 9, 4, 255, 0}), {}) && plain.frameCount() == 1, "An ordinary HID device was answered by the Host"); }

  // Read-ahead: the VM's first read starts mouseAhead reads of the Host's own at the client; each report the client
  // sends answers the VM's waiting read at once and is replaced by a new read.
  { Harness h; h.attached(1, mouse);
    require(h.core->submit(1, urbIn(10, 1, 7), {}) && h.kernelCount() == 0, "The VM's read was answered with nothing");
    require(h.frameCount() == usbdefaults::mouseAhead && h.core->inflightRequests() == usbdefaults::mouseAhead, "The Host did not read ahead");
    // Enough for a report every millisecond (the endpoint's interval) over a 20 ms round trip: each read comes back
    // only a round trip after its report, so fewer would hold reports back (the client's model: 8 reads at 20 ms
    // hold p95 2.4 ms, 24 hold 0.4 ms).
    require(h.frameCount() >= 20 && h.frameCount() + 8 <= usbdefaults::maxInFlight, "The read-ahead does not cover a 20 ms round trip or leaves the other devices no room");
    for (size_t i = 0; i < h.frameCount(); ++i) { const auto f = h.frame(i); require(f.type == usb::frameSubmit && f.endpoint == 0x81 && f.transfer == 3 && f.length == 7 && f.payload.empty(), "A read-ahead request is wrong"); }
    require(h.returnFrame(h.frame(0), 0, mouseReport(1, 5, -3)), "The client's report was refused");
    require(h.kernelCount() == 1 && usb::get32(&h.kernel(0)[4]) == 10 && kernelStatus(h, 0) == 0 && kernelPayload(h, 0) == mouseReport(1, 5, -3), "The report did not answer the VM's read");
    require(h.frameCount() == usbdefaults::mouseAhead + 1 && h.core->inflightRequests() == usbdefaults::mouseAhead, "The answered read was not replaced");
    // No VM read waiting: reports wait in order, motion joined, a button change kept apart.
    require(h.returnFrame(h.frame(1), 0, mouseReport(1, 2, 2)) && h.returnFrame(h.frame(2), 0, mouseReport(1, 3, 4)) && h.returnFrame(h.frame(3), 0, mouseReport(0, 1, 0)), "reports");
    require(h.kernelCount() == 1, "A report without a VM read reached the kernel");
    require(h.core->submit(1, urbIn(11, 1, 7), {}) && h.kernelCount() == 2 && kernelPayload(h, 1) == mouseReport(1, 5, 6), "Motion with the same buttons was not joined");
    require(h.core->submit(1, urbIn(12, 1, 7), {}) && h.kernelCount() == 3 && kernelPayload(h, 2) == mouseReport(0, 1, 0), "A button change was joined or lost");
    // A report that waited too long gives its state, not its motion.
    require(h.returnFrame(h.frame(4), 0, mouseReport(2, 40, 40)), "late report");
    h.nowMs = 400;
    require(h.core->submit(1, urbIn(13, 1, 7), {}) && h.kernelCount() == 4 && kernelPayload(h, 3) == mouseReport(2, 0, 0), "A stale report kept its motion");
    require(h.core->submit(1, urbIn(14, 1, 7), {}) && h.kernelCount() == 4, "A read with nothing to say was answered");
    // GET_REPORT now says the newest state.
    require(h.core->submit(1, urbControl(15, {0xa1, 1, 0, 1, 0, 0, 7, 0}), {}) && kernelPayload(h, 4) == mouseReport(2, 0, 0), "GET_REPORT did not follow the reports");
    // The VM cancels its waiting read: answered here, nothing goes to the client.
    const auto frames = h.frameCount();
    require(h.core->unlink(1, unlinkOf(16, 14)) && h.kernelCount() == 6 && usb::get32(&h.kernel(5)[0]) == usb::retUnlink && int32_t(usb::get32(&h.kernel(5)[20])) == -104, "The VM's cancel of a waiting read was not answered");
    require(h.core->unlink(1, unlinkOf(17, 10)) && int32_t(usb::get32(&h.kernel(6)[20])) == 0 && h.frameCount() == frames, "A cancel of an answered read went wrong or reached the client");
    // Requests that cannot be a report read are stalls, not a protocol error.
    require(h.core->submit(1, urbIn(18, 1, 4), {}) && kernelStatus(h, 7) == -32, "A read too small for a report was served");
    require(h.core->submit(1, urbOut(19, 1, 7), usb::Bytes(7, 0)) && kernelStatus(h, 8) == -32 && h.frameCount() == frames, "An OUT request reached the client");
    require(!h.core->submit(1, urbIn(20, 1, 7), {}) || !h.core->submit(1, urbIn(20, 1, 7), {}), "A repeated sequence number was served twice");
    // Returns are checked: the wrong length or data with a failure end the channel.
    Harness bad; bad.attached(1, mouse); require(bad.core->submit(1, urbIn(1, 1, 7), {}), "read");
    require(!bad.returnFrame(bad.frame(0), 0, usb::Bytes(6, 0)), "A short report was accepted");
    Harness failed; failed.attached(1, mouse); require(failed.core->submit(1, urbIn(1, 1, 7), {}), "read");
    require(failed.returnFrame(failed.frame(0), -71) && failed.frameCount() == usbdefaults::mouseAhead + 1 && failed.core->inflightRequests() == usbdefaults::mouseAhead, "A failed read was not replaced");
    // Detaching gives every read-ahead slot back.
    require(h.core->message(JsonParser::parse("{\"type\":\"usb.detach\",\"device\":1,\"reason\":\"usb_user_stopped\"}")) && h.core->inflightRequests() == 0 && h.core->inflightBytes() == 0, "The read-ahead kept the window"); }

  // The VM stops reading (driver stopped, device disabled): after a second the Host takes its reads back from the
  // client, and the next VM read starts them again.
  { Harness h; h.attached(1, mouse);
    require(h.core->submit(1, urbIn(1, 1, 7), {}) && h.core->unlink(1, unlinkOf(2, 1)), "read and cancel");
    const auto ahead = h.frameCount();
    h.nowMs = 900; h.core->tick(); require(h.frameCount() == ahead, "The reads were taken back too early");
    h.nowMs = 1100; h.core->tick();
    require(h.frameCount() == 2 * ahead, "The reads were not taken back");
    for (size_t i = 0; i < ahead; ++i) {
      const auto unlink = h.frame(ahead + i);
      require(unlink.type == usb::frameUnlink && unlink.length == h.frame(i).seq, "An unlink names the wrong read");
      usb::Frame ack; ack.type = usb::frameUnlinkReturn; ack.device = 1; ack.seq = unlink.seq; ack.status = -104; const auto bytes = usb::encodeFrame(ack);
      require(h.core->frame(bytes.data(), bytes.size()) && h.returnFrame(h.frame(i), -104), "The client's cancel was refused");
    }
    require(h.core->inflightRequests() == 0 && h.frameCount() == 2 * ahead && h.kernelCount() == 1, "Cancelled reads were replaced or reached the VM");
    h.nowMs = 5000; h.core->tick(); require(h.frameCount() == 2 * ahead, "A stopped mouse was asked again");
    require(h.core->submit(1, urbIn(3, 1, 7), {}) && h.frameCount() == 3 * ahead, "The next VM read did not start reading ahead again"); }

  // The window: read-ahead takes free room only and never waits. Room any device gives back goes to the mouse first
  // until it has its reads: a mouse short of reads gets no return of its own that would start more.
  { Harness h; h.attached(1, vector("mass storage stick"));
    for (uint32_t n = 1; n <= 30; ++n) require(h.core->submit(1, urbIn(n, 1, 8), {}), "fill");
    require(h.attach(2, mouse) && waitFor([&] { return h.core->deviceExists(2) && h.hasText("\"usb.attached\",\"device\":2"); }), "mouse");
    const auto before = h.frameCount();
    require(h.core->submit(2, urbIn(1, 1, 7), {}) && h.frameCount() == before + 2 && h.core->inflightRequests() == 32, "Read-ahead did not stop at the window");
    require(h.returnFrame(h.frame(0), 0, usb::Bytes(8, 0)) && h.frameCount() == before + 3 && h.frame(before + 2).device == 2 && h.core->inflightRequests() == 32,
            "Room the other device gave back did not go to the mouse");
    require(h.returnFrame(h.frame(before), 0, mouseReport(0, 1, 1)) && h.frameCount() == before + 4 && h.core->inflightRequests() == 32, "Read-ahead did not take the room that came back"); }
  // Another device filled the window before the VM's first read of the mouse: the mouse starts with no reads at
  // all, and gets them as that device's requests come back; that device keeps the rest of the window.
  { Harness h; h.attached(1, vector("mass storage stick"));
    for (uint32_t n = 1; n <= 32; ++n) require(h.core->submit(1, urbIn(n, 1, 8), {}), "fill");
    require(h.attach(2, mouse) && waitFor([&] { return h.core->deviceExists(2) && h.hasText("\"usb.attached\",\"device\":2"); }), "mouse");
    require(h.core->submit(2, urbIn(1, 1, 7), {}) && h.frameCount() == 32, "Read-ahead did not wait for room");
    for (size_t i = 0; i < 30; ++i) require(h.returnFrame(h.frame(i), 0, usb::Bytes(8, 0)), "storage return");
    size_t reads = 0;
    for (size_t i = 32; i < h.frameCount(); ++i) reads += h.frame(i).device == 2 && h.frame(i).type == usb::frameSubmit ? 1 : 0;
    require(reads == usbdefaults::mouseAhead && h.core->inflightRequests() == 2 + usbdefaults::mouseAhead, "A mouse that began with a full window never got its reads");
    for (uint32_t n = 33; h.core->inflightRequests() < 32; ++n) require(h.core->submit(1, urbIn(n, 1, 8), {}), "The other device lost the rest of the window"); }
  // The relay queue was full at the VM's first read: the next VM read or the session's tick starts the reads.
  { Harness h; h.attached(1, mouse);
    h.relayFull = true; require(h.core->submit(1, urbIn(1, 1, 7), {}) && h.frameCount() == 0, "read with a full relay queue");
    h.relayFull = false; h.core->tick();
    require(h.frameCount() == usbdefaults::mouseAhead, "The tick did not start the reads the relay queue had no room for"); }
  { Harness h; h.attached(1, mouse);
    h.relayFull = true; require(h.core->submit(1, urbIn(1, 1, 7), {}) && h.frameCount() == 0, "read with a full relay queue");
    h.relayFull = false; require(h.core->submit(1, urbIn(2, 1, 7), {}), "second read");
    require(h.frameCount() == usbdefaults::mouseAhead, "The next VM read did not start the reads the relay queue had no room for"); }
}

void preparation() {
  std::atomic<bool> stopping{false};
  struct Fake { bool supported = true, present = true, verified = true; int state = 1; DWORD installError = 0; bool installRestart = false, protect = true, proven = true; int attached = 0; std::vector<std::string> log; int afterInstall = 1; int adoptOwner = -1; };
  const auto access = [](Fake& f) {
    usb::DriverAccess a;
    a.platformSupported = [&f] { f.log.push_back("platform"); return f.supported; }; a.packagePresent = [&f] { f.log.push_back("present"); return f.present; };
    a.packageVerified = [&f] { f.log.push_back("verify"); return f.verified; }; a.controllerState = [&f] { f.log.push_back("state"); return f.state; };
    a.install = [&f](bool& restart) { f.log.push_back("install"); restart = f.installRestart; if (!f.installError) f.state = f.afterInstall; return f.installError; };
    a.protect = [&f] { f.log.push_back("protect"); return f.protect; }; a.protectionProven = [&f] { f.log.push_back("proven"); return f.proven; };
    a.detachAll = [&f] { f.log.push_back("detachAll"); return f.attached; };
    if (f.adoptOwner >= 0) a.adopt = [&f] { f.log.push_back("adopt"); if (f.adoptOwner == int(usb::ControllerOwner::ours)) f.state = 1; return f.adoptOwner; };
    return a;
  };
  const auto has = [](const Fake& f, const char* step) { return std::find(f.log.begin(), f.log.end(), step) != f.log.end(); };
  { Fake f; require(usb::prepareController(access(f), stopping) == usb::Prepare::ready && !has(f, "install") && has(f, "protect") && has(f, "proven"), "An installed controller was not simply protected and proven"); }
  { Fake f; f.state = 0; require(usb::prepareController(access(f), stopping) == usb::Prepare::ready && has(f, "install"), "A missing controller was not installed"); }
  { Fake f; f.state = 2; require(usb::prepareController(access(f), stopping) == usb::Prepare::ready && has(f, "install"), "A stopped controller was not repaired"); }
  { Fake f; f.supported = false; require(usb::prepareController(access(f), stopping) == usb::Prepare::platformUnsupported && !has(f, "install") && !has(f, "present"), "An unsupported Windows went on"); }
  { Fake f; f.present = false; require(usb::prepareController(access(f), stopping) == usb::Prepare::packageMissing && !has(f, "install"), "A Host without the package installed something"); }
  { Fake f; f.verified = false; f.state = 0; require(usb::prepareController(access(f), stopping) == usb::Prepare::signatureInvalid && !has(f, "install") && !has(f, "protect"), "A package that failed verification was installed"); }
  { Fake f; f.state = 3; require(usb::prepareController(access(f), stopping) == usb::Prepare::otherInstallation && !has(f, "install") && !has(f, "protect"), "Another installation was taken over"); }
  // Field case: our own pinned package without its marker is taken back; anything else never.
  { Fake f; f.state = 3; f.adoptOwner = int(usb::ControllerOwner::ours); require(usb::prepareController(access(f), stopping) == usb::Prepare::ready && has(f, "adopt") && !has(f, "install") && has(f, "protect") && has(f, "proven"), "Our unmarked controller was not adopted and protected"); }
  for (const auto owner : {usb::ControllerOwner::foreign, usb::ControllerOwner::multiple, usb::ControllerOwner::unknown}) { Fake f; f.state = 3; f.adoptOwner = int(owner); require(usb::prepareController(access(f), stopping) == usb::Prepare::otherInstallation && has(f, "adopt") && !has(f, "install") && !has(f, "protect"), "Another product's controller was taken over"); }
  { using V = usb::ControllerView; using O = usb::ControllerOwner;
    const V marked{true, true, std::nullopt}, markedStopped{true, false, std::nullopt}, pinned{false, false, true}, other{false, false, false}, unread{false, false, std::nullopt};
    require(usb::classifyControllers({}) == O::none && usb::classifyControllers({marked}) == O::ours && usb::classifyControllers({markedStopped}) == O::oursStopped &&
      usb::classifyControllers({pinned}) == O::unmarked && usb::classifyControllers({other}) == O::foreign && usb::classifyControllers({unread}) == O::unknown, "A single controller's owner was decided wrongly");
    // Several nodes: ours to tidy only when every unmarked one is our pinned package.
    require(usb::classifyControllers({marked, pinned}) == O::duplicates && usb::classifyControllers({pinned, pinned}) == O::duplicates &&
      usb::classifyControllers({marked, other}) == O::multiple && usb::classifyControllers({marked, unread}) == O::multiple && usb::classifyControllers({pinned, other}) == O::multiple, "Several controllers were decided wrongly"); }
  // An incomplete device list never leads to registering another controller.
  { Fake f; f.state = 4; require(usb::prepareController(access(f), stopping) == usb::Prepare::installFailed && !has(f, "install") && !has(f, "adopt"), "An unreadable device list installed a controller"); }
  { Fake f; f.state = 3; f.adoptOwner = int(usb::ControllerOwner::ours); require(usb::prepareController(access(f), stopping) == usb::Prepare::ready && has(f, "adopt"), "Our duplicated controller was not tidied and adopted"); }
  // The real lookup is read-only: whatever this machine has, it answers in the vocabulary.
  { const std::string name = usb::controllerOwnerName(usb::controllerOwner()); require(name == "none" || name == "started" || name == "stopped" || name == "unmarked" || name == "foreign" || name == "multiple" || name == "duplicates" || name == "unknown", "The live controller owner left the vocabulary"); }
  { Fake f; f.state = 0; f.installError = ERROR_ACCESS_DENIED; require(usb::prepareController(access(f), stopping) == usb::Prepare::installFailed && !has(f, "protect"), "A failed install went on"); }
  { Fake f; f.state = 0; f.afterInstall = 2; f.installRestart = true; require(usb::prepareController(access(f), stopping) == usb::Prepare::restartRequired && !has(f, "protect"), "A restart requirement was not reported"); }
  { Fake f; f.state = 0; f.afterInstall = 2; require(usb::prepareController(access(f), stopping) == usb::Prepare::installFailed, "A controller that did not start was ready"); }
  { Fake f; f.protect = false; require(usb::prepareController(access(f), stopping) == usb::Prepare::protectionFailed && !has(f, "detachAll"), "A controller that could not be protected was ready"); }
  { Fake f; f.proven = false; require(usb::prepareController(access(f), stopping) == usb::Prepare::protectionFailed, "A controller whose protection was not proven was ready"); }
  { Fake f; f.attached = 3; require(usb::prepareController(access(f), stopping) == usb::Prepare::ready && has(f, "detachAll"), "Leftover ports were not cleared"); }
  { Fake f; std::atomic<bool> stop{true}; f.state = 0; require(usb::prepareController(access(f), stop) != usb::Prepare::ready && !has(f, "install"), "A stopping service installed a driver"); }
  require(std::string(usb::prepareReason(usb::Prepare::ready)).empty() && std::string(usb::prepareReason(usb::Prepare::protectionFailed)) == "usb_vhci_unprotected" && std::string(usb::prepareReason(usb::Prepare::packageMissing)) == "usb_driver_missing" &&
    std::string(usb::prepareReason(usb::Prepare::restartRequired)) == "usb_restart_required" && std::string(usb::prepareReason(usb::Prepare::installFailed)) == "usb_driver_install_failed" && std::string(usb::prepareReason(usb::Prepare::otherInstallation)) == "usb_vhci_unavailable", "A reason code changed");
  // Through the core: a failed preparation is reported with its fixed reason and nothing attaches.
  { Harness h; h.prepared = {false, usb::prepareReason(usb::Prepare::protectionFailed)}; h.ready();
    require(waitFor([&] { return h.hasText("{\"type\":\"usb.status\",\"state\":\"unavailable\",\"reason\":\"usb_vhci_unprotected\"}"); }), "The protection failure was not reported"); }
}

void abi() {
  require(usb::vhci::pluginHardware == 0x0022E000 && usb::vhci::plugoutHardware == 0x0022E004 && usb::vhci::getImportedDevices == 0x0022E008 && usb::vhci::stopAttachAttempts == 0x0022E014, "An ioctl code differs from usbip-win2 0.9.7");
  require(offsetof(usb::vhci::PluginHardware, port) == 4 && sizeof(usb::vhci::Location) == 1096 && sizeof(usb::vhci::PluginHardware) == 1100, "The plug-in request layout differs from usbip-win2");
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(usb::vhciSecurity, SDDL_REVISION_1, &descriptor, nullptr) != FALSE, "The controller descriptor is not valid SDDL");
  LocalFree(descriptor);
  const std::wstring sddl = usb::vhciSecurity;
  require(sddl.find(L";;;SY)") != std::wstring::npos && sddl.find(L";;;BA)") != std::wstring::npos && sddl.find(L";;;WD)") == std::wstring::npos && sddl.find(L";;;BU)") == std::wstring::npos && sddl.find(L";;;AU)") == std::wstring::npos && sddl.find(L";;;IU)") == std::wstring::npos, "The controller descriptor lets other users in");
  require(usb::packageFiles()[6].name == nullptr && std::string(usb::packageFiles()[0].sha256).size() == 64, "The package pins are malformed");
  for (const usb::PackageFile* file = usb::packageFiles(); file->name; ++file) require(std::string(file->sha256).find_first_not_of("0123456789abcdef") == std::string::npos && std::string(file->sha256).size() == 64, "A pin is not a SHA-256");
}

// --- the loopback server over real sockets ------------------------------------------------------
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
usb::Bytes submitBytes(uint32_t seq, uint32_t devid, uint32_t direction, uint32_t endpoint, int32_t length, const uint8_t* setup = nullptr) {
  usb::Bytes cmd; usb::put32(cmd, usb::cmdSubmit); usb::put32(cmd, seq); usb::put32(cmd, devid); usb::put32(cmd, direction); usb::put32(cmd, endpoint);
  usb::put32(cmd, 0); usb::put32(cmd, uint32_t(length)); usb::put32(cmd, 0); usb::put32(cmd, 0xffffffffu); usb::put32(cmd, 0);
  if (setup) cmd.insert(cmd.end(), setup, setup + 8); else cmd.resize(48, 0);
  return cmd;
}

void server() {
  WSADATA wsa{}; require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "Winsock");
  const auto stick = vector("mass storage stick");
  {
    // The real peer check: this process is not the System process, so it is refused by default.
    Harness h; h.attached(1, stick);
    usb::UsbIpServer closedServer;
    const auto port = closedServer.start(*h.core);
    require(port != 0, "The server did not start");
    const SOCKET refused = connectLoopback(port);
    require(refused != INVALID_SOCKET, "connect");
    usb::Bytes reply; const bool got = readAll(refused, reply, 1);
    require(!got && closedServer.refused() >= 1, "A user process was served");
    closesocket(refused);
    require(h.core->deviceCount() == 1 && h.kernelCount() == 0, "A refused connection imported a device");
    // The connection's owner can be found, and it is not System.
    const SOCKET self = connectLoopback(port); Sleep(50);
    // (the accepted side is what the server inspects; here the helper is exercised on the connecting side)
    require(usb::loopbackPeerProcess(self) == GetCurrentProcessId() || usb::loopbackPeerProcess(self) == 0, "The owner lookup returned another process");
    closesocket(self);
    closedServer.stop();
  }
  {
    Harness h; h.attached(1, stick);
    usb::UsbIpServer server; usb::UsbIpServer::Access access; access.trusted = [](SOCKET) { return true; };
    const auto port = server.start(*h.core, access);
    require(port != 0, "The server did not start");
    // Devlist: empty, devices are imported by busid only.
    { SOCKET s = connectLoopback(port); auto request = usb::encodeOpCommon(usb::opReqDevlist, 0); usb::put32(request, 0); require(sendAll(s, request), "devlist send");
      usb::Bytes reply; require(readAll(s, reply, 12) && usb::get16(&reply[2]) == usb::opRepDevlist && usb::get32(&reply[8]) == 0, "The device list was not empty"); closesocket(s); }
    // Garbage and wrong versions are closed.
    { SOCKET s = connectLoopback(port); usb::Bytes junk(8, 0xff); require(sendAll(s, junk), "junk send"); usb::Bytes reply; require(!readAll(s, reply, 1), "Garbage was answered"); closesocket(s); }
    // An unknown busid: NODEV, and the real device stays importable.
    { SOCKET s = connectLoopback(port); require(sendAll(s, importRequest("1-99")), "send"); usb::Bytes reply; require(readAll(s, reply, 8) && usb::get16(&reply[2]) == usb::opRepImport && usb::get32(&reply[4]) == usb::stNoDev, "An unknown busid was not refused"); closesocket(s); }
    // The import of the attached device: its descriptor goes back, then requests flow.
    const std::string busId = usb::busIdFor(1);
    SOCKET kernel = connectLoopback(port);
    // The harness's own plug-in already imported the device through the core; import again over the socket must be refused.
    require(sendAll(kernel, importRequest(busId)), "send");
    usb::Bytes reply; require(readAll(kernel, reply, 8) && usb::get32(&reply[4]) == usb::stNoDev, "A device was imported twice");
    closesocket(kernel);
  }
  {
    // A device that is waiting for its import: the full conversation over the socket.
    std::mutex lock; std::vector<std::string> texts; std::vector<usb::Bytes> frames;
    UsbHooks hooks; hooks.sendText = [&](const std::string& t) { std::lock_guard g(lock); texts.push_back(t); return true; };
    hooks.sendFrame = [&](const usb::Bytes& b) { std::lock_guard g(lock); frames.push_back(b); return true; };
    hooks.prepare = [] { return UsbPrepared{true, ""}; };
    std::thread kernelThread; std::atomic<bool> go{false}; std::atomic<uint16_t> serverPort{0}; std::atomic<int> imported{0}; std::atomic<int> plugged{0}; std::vector<int> detached;
    hooks.attach = [&](const std::string& busId) -> int {
      SOCKET s = connectLoopback(serverPort);
      if (s == INVALID_SOCKET) return 0;
      sendAll(s, importRequest(busId)); usb::Bytes reply;
      if (!readAll(s, reply, 8 + 312) || usb::get32(&reply[4]) != 0) { closesocket(s); return 0; }
      imported = 1;
      // Keep the connection: the thread below plays the kernel.
      kernelThread = std::thread([&, s, reply] {
        const uint32_t devid = usb::get32(&reply[8 + 256 + 32]) << 16 | usb::get32(&reply[8 + 256 + 36]);
        usb::Bytes d; // descriptor request, served locally by the Host
        const uint8_t setup[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
        sendAll(s, submitBytes(1, devid, 1, 0, 18, setup)); readAll(s, d, 48 + 18);
        // A bulk OUT with data, answered by the (fake) client through the core.
        auto out = submitBytes(2, devid, 0, 2, 100); out.insert(out.end(), 100, 0x42); sendAll(s, out);
        usb::Bytes r; readAll(s, r, 48);
        plugged = (usb::get32(&d[0]) == usb::retSubmit && d[48] == 18 && usb::get32(&r[4]) == 2 && usb::get32(&r[24]) == 100) ? 2 : -1;
        while (!go) Sleep(5);
        closesocket(s);   // the VM unplugged it
      });
      return 5;
    };
    hooks.detach = [&](int port) { std::lock_guard g(lock); detached.push_back(port); };
    UsbSessionCore core(session, {true, false, false, true}, std::move(hooks));
    usb::UsbIpServer server; usb::UsbIpServer::Access access; access.trusted = [](SOCKET) { return true; };
    serverPort = server.start(core, access);
    require(serverPort != 0, "The server did not start");
    core.message(JsonParser::parse("{\"type\":\"usb.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"usb-v1\",\"limits\":{\"maxUrbBytes\":262144,\"windowBytes\":1048576,\"maxInFlight\":32,\"maxDevices\":4,\"maxAttaches\":64}}"));
    require(waitFor([&] { std::lock_guard g(lock); return !texts.empty(); }), "no ready status");
    core.message(JsonParser::parse("{\"type\":\"usb.attach\",\"device\":1,\"descriptors\":\"" + toBase64(stick) + "\",\"speed\":\"high\"}"));
    require(waitFor([&] { std::lock_guard g(lock); return !frames.empty(); }), "The bulk request did not reach the client");
    { std::lock_guard g(lock); require(std::any_of(texts.begin(), texts.end(), [](const auto& t) { return t.find("\"usb.attached\"") != std::string::npos; }), "The client was not told of the device before its first request"); }
    const auto submit = *usb::decodeFrame(frames[0].data(), frames[0].size());
    require(submit.type == usb::frameSubmit && submit.endpoint == 0x02 && submit.length == 100 && submit.payload == usb::Bytes(100, 0x42), "The bulk frame is wrong");
    usb::Frame answer; answer.type = usb::frameReturn; answer.device = 1; answer.endpoint = 0x02; answer.transfer = 2; answer.seq = submit.seq; answer.length = 100;
    const auto bytes = usb::encodeFrame(answer); require(core.frame(bytes.data(), bytes.size()), "The return was refused");
    require(waitFor([&] { return plugged == 2; }), "The kernel did not get both answers");
    go = true;                                                   // the VM drops the connection
    require(waitFor([&] { std::lock_guard g(lock); return std::any_of(texts.begin(), texts.end(), [](const auto& t) { return t.find("usb_vm_detached") != std::string::npos; }); }), "The client was not told the VM detached the device");
    require(waitFor([&] { std::lock_guard g(lock); return detached == std::vector<int>{5}; }), "The port was not unplugged");
    core.closed(); server.stop();
    if (kernelThread.joinable()) kernelThread.join();
  }
  WSACleanup();
}

void realPackage() {
  wchar_t directory[MAX_PATH]{};
  const DWORD length = GetEnvironmentVariableW(L"MULTITAP_USB_PACKAGE", directory, MAX_PATH);
  if (!length || length >= MAX_PATH) { std::cout << "(package verification skipped: MULTITAP_USB_PACKAGE is not set)\n"; return; }
  const auto folder = std::filesystem::path(directory).wstring();
  require(usb::verifyPackage(folder), "The pinned package failed verification (hashes, Microsoft signatures or catalogs)");
  // A single changed byte must fail.
  const auto copy = std::filesystem::temp_directory_path() / (L"multitap-usb-test-" + randomName());
  std::filesystem::create_directories(copy);
  for (const usb::PackageFile* file = usb::packageFiles(); file->name; ++file) std::filesystem::copy_file(std::filesystem::path(folder) / file->name, copy / file->name);
  require(usb::verifyPackage(copy.wstring()), "A copy of the package failed verification");
  { std::fstream stream(copy / L"usbip2_ude.inf", std::ios::in | std::ios::out | std::ios::binary); stream.seekp(10); stream.put('X'); }
  require(!usb::verifyPackage(copy.wstring()), "A changed INF passed verification");
  std::filesystem::remove_all(copy);
}
}  // namespace

int main() {
  try {
    const std::pair<const char*, void (*)()> steps[] = {{"grant", grant}, {"policy", policy}, {"wire", wire}, {"attachment", attachment}, {"transfers", transfers}, {"returns", returnsAreChecked}, {"window", window},
      {"unlinking", unlinking}, {"storageStart", storageStart}, {"descriptorRequests", descriptorRequestsRefused}, {"cancelled", cancelledRequestsDoNotHangTheVm}, {"vmDeviceNeverStarts", vmDeviceNeverStarts}, {"gameMouse", gameMouse}, {"detaching", detaching}, {"preparation", preparation}, {"abi", abi}, {"server", server}, {"package", realPackage}};
    for (const auto& [name, step] : steps) { std::cout << name << std::endl; step(); }
    std::cout << "Multitap Host USB redirection regression passed (" << checks << " checks)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "usb-v1 failed: " << error.what() << " (after " << checks << " checks)\n";
    return 1;
  }
}
