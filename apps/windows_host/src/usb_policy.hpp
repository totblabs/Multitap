#pragma once
// usb-v1 (docs/usb-redirection.md): the Host's own check of a client's USB
// descriptors. The client sends a device descriptor followed by every
// configuration descriptor in full; the Host parses them strictly, decides with
// the default policy, and from then on answers the VM's GET_DESCRIPTOR from
// this validated copy, so the VM binds drivers only to descriptors the Host
// accepted. The broker runs the same rules (server/src/usb.mjs); both read the
// shared vectors server/test/fixtures/usb-descriptor-vectors.json.
//
// Standard C++ only (no Windows headers), so the tests exercise it directly.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace host::usb {
inline constexpr size_t maxDescriptorBytes = 4096;

struct Endpoint { uint8_t address = 0, type = 0; uint16_t maxPacket = 0; };      // type: 0 control, 1 isochronous, 2 bulk, 3 interrupt
struct Interface { uint8_t number = 0, alternate = 0, declared = 0, cls = 0, subclass = 0, protocol = 0; std::vector<Endpoint> endpoints; };
// An interface association (IAD, type 11): Windows names the function after
// these codes (USB\Class_xx), so a driver binds to them as to an interface's.
struct Function { uint8_t first = 0, count = 0, cls = 0, subclass = 0, protocol = 0; };
struct Configuration { uint8_t value = 0; std::vector<Interface> interfaces; std::vector<Function> functions; size_t offset = 0, length = 0; };
struct Device {
  uint16_t vendor = 0, product = 0, bcdDevice = 0;
  uint8_t cls = 0, subclass = 0, protocol = 0, maxPacket0 = 0;
  std::vector<Configuration> configurations;
  std::vector<uint8_t> bytes;  // the validated copy: device descriptor, then each configuration
};

// Strict parse; nullopt for anything malformed (nothing is partly read).
inline std::optional<Device> parseDescriptors(const uint8_t* data, size_t size) {
  if (!data || size < 18 + 9 || size > maxDescriptorBytes || data[0] != 18 || data[1] != 1) return std::nullopt;
  Device device;
  device.vendor = uint16_t(data[8] | data[9] << 8); device.product = uint16_t(data[10] | data[11] << 8); device.bcdDevice = uint16_t(data[12] | data[13] << 8);
  device.cls = data[4]; device.subclass = data[5]; device.protocol = data[6]; device.maxPacket0 = data[7];
  const unsigned count = data[17];
  if (count < 1 || count > 4 || (device.maxPacket0 != 8 && device.maxPacket0 != 16 && device.maxPacket0 != 32 && device.maxPacket0 != 64)) return std::nullopt;
  size_t at = 18;
  for (unsigned index = 0; index < count; ++index) {
    if (at + 9 > size || data[at] != 9 || data[at + 1] != 2) return std::nullopt;
    const size_t total = size_t(data[at + 2] | data[at + 3] << 8);
    if (total < 9 || at + total > size) return std::nullopt;
    Configuration configuration; configuration.value = data[at + 5]; configuration.offset = at; configuration.length = total;
    if (data[at + 4] < 1 || configuration.value < 1) return std::nullopt;
    Interface* current = nullptr;
    for (size_t cursor = at + 9; cursor < at + total;) {
      // Both header bytes inside this configuration before either is read.
      if (cursor + 2 > at + total) return std::nullopt;
      const size_t length = data[cursor], type = data[cursor + 1];
      if (length < 2 || cursor + length > at + total) return std::nullopt;
      if (type == 11) {
        if (length != 8) return std::nullopt;
        configuration.functions.push_back({data[cursor + 2], data[cursor + 3], data[cursor + 4], data[cursor + 5], data[cursor + 6]});
      } else if (type == 4) {
        if (length != 9) return std::nullopt;
        configuration.interfaces.push_back({data[cursor + 2], data[cursor + 3], data[cursor + 4], data[cursor + 5], data[cursor + 6], data[cursor + 7], {}});
        current = &configuration.interfaces.back();
      } else if (type == 5) {
        if (!current || (length != 7 && length != 9)) return std::nullopt;
        current->endpoints.push_back({data[cursor + 2], uint8_t(data[cursor + 3] & 3), uint16_t(data[cursor + 4] | data[cursor + 5] << 8)});
      }
      cursor += length;
    }
    if (configuration.interfaces.empty()) return std::nullopt;
    for (const auto& item : configuration.interfaces) if (item.endpoints.size() != item.declared) return std::nullopt;
    device.configurations.push_back(std::move(configuration));
    at += total;
  }
  if (at != size) return std::nullopt;
  device.bytes.assign(data, data + size);
  return device;
}

struct Decision {
  bool allowed = false;
  std::string reason;                 // when denied: a usb_* code the wire allows
  std::set<std::string> requires_;    // other administrator permissions the interfaces need
  std::set<int> classes;
};

inline const char* classPermission(int cls) {
  switch (cls) { case 0x02: case 0x0a: return "serialPorts"; case 0x07: return "printers"; case 0x08: return "drives"; case 0x0b: return "smartCards"; default: return nullptr; }
}
inline const char* deniedClassReason(int cls) {
  switch (cls) { case 0x01: case 0x0e: return "usb_isochronous_denied"; case 0x09: case 0xe0: return "usb_class_denied"; default: return nullptr; }
}

// A keyboard or a mouse: a HID interface with the
// boot subclass, or one that names the keyboard (1) or mouse (2) protocol with any
// subclass. A report-protocol HID (protocol 0: a FIDO key, the client's game
// mouse) stays allowed; its report usage is not part of these descriptors.
inline bool keyboardOrMouse(uint8_t cls, uint8_t subclass, uint8_t protocol) {
  return cls == 0x03 && (subclass == 1 || protocol == 1 || protocol == 2);
}
// A network adapter: CDC Ethernet (ECM 0x06), ATM (0x07), EEM (0x0C), NCM (0x0D)
// and MBIM (0x0E), RNDIS as Windows binds it (02/02/FF, E0/01/03, EF/04/xx). The
// policy has no permission for networks, so it is denied: the VM would get a
// network path the administrator never granted.
inline bool networkAdapter(uint8_t cls, uint8_t subclass, uint8_t protocol) {
  if (cls == 0x02) return subclass == 0x06 || subclass == 0x07 || subclass == 0x0c || subclass == 0x0d || subclass == 0x0e || (subclass == 0x02 && protocol == 0xff);
  if (cls == 0xe0) return subclass == 0x01 && protocol == 0x03;
  if (cls == 0xef) return subclass == 0x04;
  return false;
}
// One set of class codes (device, IAD function or interface): the reason it is denied, or nullptr.
inline const char* deniedCodes(uint8_t cls, uint8_t subclass, uint8_t protocol) {
  if (const char* reason = deniedClassReason(cls)) return reason;
  if (keyboardOrMouse(cls, subclass, protocol)) return "usb_hid_boot_denied";
  if (networkAdapter(cls, subclass, protocol)) return "usb_class_denied";
  return nullptr;
}

// The default deny list: hubs, wireless controllers (Bluetooth), audio/video
// (isochronous), any isochronous endpoint, keyboards and mice (which the VM keeps
// using from its own console) and network adapters. The rules apply to the
// device's codes, to every interface association (IAD) and to every interface.
inline Decision decide(const Device& device) {
  Decision decision;
  if (const char* reason = deniedCodes(device.cls, device.subclass, device.protocol)) { decision.reason = reason; return decision; }
  for (const auto& configuration : device.configurations) for (const auto& function : configuration.functions) {
    if (const char* reason = deniedCodes(function.cls, function.subclass, function.protocol)) { decision.reason = reason; return decision; }
    if (const char* permission = classPermission(function.cls)) decision.requires_.insert(permission);
  }
  for (const auto& configuration : device.configurations) for (const auto& item : configuration.interfaces) {
    decision.classes.insert(item.cls);
    for (const auto& endpoint : item.endpoints) if (endpoint.type == 1) { decision.reason = "usb_isochronous_denied"; decision.classes.clear(); return decision; }
    if (const char* reason = deniedCodes(item.cls, item.subclass, item.protocol)) { decision.reason = reason; decision.classes.clear(); decision.requires_.clear(); return decision; }
    if (const char* permission = classPermission(item.cls)) decision.requires_.insert(permission);
  }
  decision.allowed = true;
  return decision;
}

// The class permissions the broker granted this session (session.start usbGrant).
struct Grant { bool drives = false, printers = false, smartCards = false, serialPorts = false; };
inline bool covers(const Decision& decision, const Grant& grant) {
  for (const auto& need : decision.requires_) {
    if (need == "drives" && !grant.drives) return false;
    if (need == "printers" && !grant.printers) return false;
    if (need == "smartCards" && !grant.smartCards) return false;
    if (need == "serialPorts" && !grant.serialPorts) return false;
  }
  return true;
}

// Is [address] (bit 7 = IN) a bulk or interrupt endpoint of the validated
// device? Control endpoint 0 is always allowed; nothing else is.
inline std::optional<uint8_t> endpointType(const Device& device, uint8_t address) {
  if ((address & 0x7f) == 0) return uint8_t(0);
  for (const auto& configuration : device.configurations) for (const auto& item : configuration.interfaces)
    for (const auto& endpoint : item.endpoints) if (endpoint.address == address && (endpoint.type == 2 || endpoint.type == 3)) return endpoint.type;
  return std::nullopt;
}

// GET_DESCRIPTOR(device / configuration) answered from the validated copy.
// [setup] is the 8-byte setup packet; returns the bytes to send (at most
// wLength), or nullopt when the request is something else.
inline std::optional<std::vector<uint8_t>> localDescriptor(const Device& device, const uint8_t* setup) {
  if (setup[0] != 0x80 || setup[1] != 6) return std::nullopt;
  const unsigned type = setup[3], index = setup[2], length = unsigned(setup[6] | setup[7] << 8);
  std::vector<uint8_t> out;
  if (type == 1 && index == 0) out.assign(device.bytes.begin(), device.bytes.begin() + 18);
  else if (type == 2 && index < device.configurations.size()) {
    const auto& configuration = device.configurations[index];
    out.assign(device.bytes.begin() + long(configuration.offset), device.bytes.begin() + long(configuration.offset + configuration.length));
  } else return std::nullopt;
  if (out.size() > length) out.resize(length);
  return out;
}

// Requests from the VM that are refused (STALL) instead of reaching the device:
// the Microsoft OS string descriptor (string 0xEE)
// and the BOS descriptor (type 15, which carries Microsoft OS 2.0 descriptors).
// Through them a device the policy allowed (vendor class 0xFF, say) could ask
// Windows in the VM for another driver by compatible ID (RNDIS, WinUSB), which
// the class checks above never see.
inline bool refusedDescriptorRequest(const uint8_t* setup) {
  if ((setup[0] & 0x80) == 0 || ((setup[0] >> 5) & 3) != 0 || setup[1] != 6) return false;   // standard GET_DESCRIPTOR only
  const unsigned type = setup[3], index = setup[2];
  return type == 0x0f || (type == 3 && index == 0xee);
}
}  // namespace host::usb
