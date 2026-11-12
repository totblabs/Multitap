#pragma once
// 게임 마우스 over usb-v1 (docs/usb-redirection.md): Multitap's own
// virtual USB HID mouse, the device the client makes up from its PC's Raw
// Input while the remote window holds the mouse (ToTB Link's smooth mode).
// The Host knows it only by these exact descriptors; the client's attach goes
// through the ordinary usb-v1 checks first (it is report-protocol HID, so the
// boot-HID deny rule never applies and needs no exception). For this device
// the Host answers every control request itself and keeps interrupt reads
// waiting at the client ahead of the VM's own (usb_core.hpp), so a report is
// one network crossing from the PC to the VM's HID driver.
//
// Byte for byte ToTB Link's mouse (src/UsbBridge.App/VirtualHid.cs, proven
// against usbip-win2 and Windows' HID stack): 5 buttons, X/Y signed 16-bit
// relative, wheel and horizontal pan signed 8-bit; a 7-byte report without a
// report ID on interrupt IN 0x81. 1209:0001 is a pid.codes test id, as in
// ToTB Link. Standard C++ only.
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace host::usb::vmouse {
using Bytes = std::vector<uint8_t>;
inline constexpr uint16_t vendor = 0x1209, product = 0x0001;
inline constexpr uint8_t endpoint = 0x81;
inline constexpr size_t reportLength = 7;

inline const Bytes& reportDescriptor() {
  static const Bytes bytes{
      0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
      0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
      0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
      0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
      0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
      0x05, 0x0C, 0x0A, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
      0xC0, 0xC0};
  return bytes;
}
inline Bytes deviceDescriptor() {
  return {18, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 64, vendor & 0xFF, vendor >> 8, product & 0xFF, product >> 8, 0x00, 0x01, 1, 2, 3, 1};
}
// Configuration, interface, HID and endpoint descriptors together (34 bytes).
inline Bytes configurationDescriptor() {
  const auto report = reportDescriptor().size();
  return {9, 0x02, 34, 0, 1, 1, 0, 0xA0, 50,
          9, 0x04, 0, 0, 1, 0x03, 0x00, 0x00, 0,
          9, 0x21, 0x11, 0x01, 0, 1, 0x22, uint8_t(report), uint8_t(report >> 8),
          7, 0x05, endpoint, 0x03, uint8_t(reportLength), 0, 1};
}
// What usb.attach carries: the device descriptor, then the configuration.
inline const Bytes& descriptors() {
  static const Bytes bytes = [] { auto out = deviceDescriptor(); const auto config = configurationDescriptor(); out.insert(out.end(), config.begin(), config.end()); return out; }();
  return bytes;
}
// The narrow identification: these exact bytes and nothing else.
inline bool matches(const uint8_t* data, size_t size) { return data && size == descriptors().size() && std::equal(descriptors().begin(), descriptors().end(), data); }

// String 0 is the language list (US English); 1 maker, 2 product, 3 serial
// (one fixed serial, so the VM keeps one device instance across sessions).
inline std::optional<Bytes> stringDescriptor(unsigned index) {
  if (index == 0) return Bytes{4, 0x03, 0x09, 0x04};
  const char* text = index == 1 ? "ToTB" : index == 2 ? "Multitap Game Mouse" : index == 3 ? "MULTITAP-GAME-MOUSE" : nullptr;
  if (!text) return std::nullopt;
  Bytes out{0, 0x03};
  for (const char* c = text; *c; ++c) { out.push_back(uint8_t(*c)); out.push_back(0); }
  out[0] = uint8_t(out.size());
  return out;
}

// A report as a state: its buttons without motion (motion was already sent).
inline Bytes still(const Bytes& report) { Bytes out(reportLength, 0); if (!report.empty()) out[0] = report[0]; return out; }

// [into] takes the motion of [next] when both have the same buttons and the
// sums stay within the report's ranges (ToTB Link's SyntheticHidDevice.Join).
inline bool join(Bytes& into, const Bytes& next) {
  if (into.size() != reportLength || next.size() != reportLength || into[0] != next[0]) return false;
  const auto wide = [](const Bytes& r, size_t at) { return int(int16_t(uint16_t(r[at] | r[at + 1] << 8))); };
  const int x = wide(into, 1) + wide(next, 1), y = wide(into, 3) + wide(next, 3), wheel = int(int8_t(into[5])) + int(int8_t(next[5])), pan = int(int8_t(into[6])) + int(int8_t(next[6]));
  if (x < -32767 || x > 32767 || y < -32767 || y > 32767 || wheel < -127 || wheel > 127 || pan < -127 || pan > 127) return false;
  into[1] = uint8_t(x); into[2] = uint8_t(x >> 8); into[3] = uint8_t(y); into[4] = uint8_t(y >> 8); into[5] = uint8_t(wheel); into[6] = uint8_t(pan);
  return true;
}

// The answer to one control request: status 0 or -EPIPE (stall), and the
// bytes of an IN transfer (at most wLength). [last]: the newest report.
struct ControlAnswer { int32_t status = -32; Bytes data; };
inline ControlAnswer control(const uint8_t* setup, const Bytes& last) {
  const unsigned type = setup[0], request = setup[1], value = unsigned(setup[2] | setup[3] << 8), length = unsigned(setup[6] | setup[7] << 8);
  std::optional<Bytes> data;
  bool ok = true;
  if (type == 0x80 && request == 6) {
    const unsigned kind = value >> 8, index = value & 0xFF;
    if (kind == 1 && index == 0) data = deviceDescriptor();
    else if (kind == 2 && index == 0) data = configurationDescriptor();
    else if (kind == 3) data = stringDescriptor(index);
    ok = data.has_value();
  } else if (type == 0x81 && request == 6) {
    if (value >> 8 == 0x22) data = reportDescriptor();
    else if (value >> 8 == 0x21) { const auto config = configurationDescriptor(); data = Bytes(config.begin() + 18, config.begin() + 27); }
    ok = data.has_value();
  }
  else if ((type == 0x80 || type == 0x81 || type == 0x82) && request == 0) data = Bytes{0, 0};      // GET_STATUS
  else if (type == 0x80 && request == 8) data = Bytes{1};                                           // GET_CONFIGURATION
  else if (type == 0x81 && request == 0x0A) data = Bytes{0};                                        // GET_INTERFACE
  else if (type == 0xA1 && request == 1) { ok = value >> 8 == 1; if (ok) data = still(last); }     // GET_REPORT (input only)
  else if (type == 0xA1 && request == 2) data = Bytes{0};                                           // GET_IDLE
  else if (type == 0xA1 && request == 3) data = Bytes{1};                                           // GET_PROTOCOL: report
  // SET_CONFIGURATION, CLEAR/SET_FEATURE, SET_INTERFACE, SET_IDLE, SET_PROTOCOL, SET_REPORT.
  else ok = (type == 0x00 && (request == 9 || request == 1 || request == 3)) || (type == 0x01 && (request == 0x0B || request == 1 || request == 3)) ||
            (type == 0x02 && (request == 1 || request == 3)) || (type == 0x21 && (request == 0x0A || request == 0x0B || request == 0x09));
  if (!ok) return {};
  ControlAnswer answer; answer.status = 0;
  if (data) { answer.data = std::move(*data); if (answer.data.size() > length) answer.data.resize(length); }
  return answer;
}
}  // namespace host::usb::vmouse
