#pragma once
// The two wire formats of usb-v1 on the Host, as pure byte codecs:
//  - USB/IP (kernel.org usbip_protocol; usbip-win2 v0.9.7.x speaks protocol
//    0x0111, big-endian) between the VM's kernel driver and the Host's loopback
//    server: OP_REQ_IMPORT/OP_REP_IMPORT, then CMD_SUBMIT / RET_SUBMIT /
//    CMD_UNLINK / RET_UNLINK with 48-byte headers.
//  - the relay frames of usb-v1 (server/src/usb.mjs): a 24-byte little-endian
//    header, then the payload.
// Standard C++ only.
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace host::usb {
using Bytes = std::vector<uint8_t>;

// --- USB/IP ------------------------------------------------------------------
inline constexpr uint16_t usbipVersion = 0x0111;
inline constexpr uint16_t opReqImport = 0x8003, opRepImport = 0x0003, opReqDevlist = 0x8005, opRepDevlist = 0x0005;
inline constexpr uint32_t stOk = 0, stNa = 1, stDevBusy = 2, stDevErr = 3, stNoDev = 4, stError = 5;
inline constexpr size_t usbipBusIdSize = 32, usbipDeviceSize = 312, usbipHeaderSize = 48, usbipCommonSize = 8;
inline constexpr uint32_t cmdSubmit = 1, cmdUnlink = 2, retSubmit = 3, retUnlink = 4;

inline void put16(Bytes& out, uint16_t value) { out.push_back(uint8_t(value >> 8)); out.push_back(uint8_t(value)); }
inline void put32(Bytes& out, uint32_t value) { for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(value >> shift)); }
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

struct OpCommon { uint16_t version = 0, code = 0; uint32_t status = 0; };
inline Bytes encodeOpCommon(uint16_t code, uint32_t status) { Bytes out; put16(out, usbipVersion); put16(out, code); put32(out, status); return out; }
inline std::optional<OpCommon> decodeOpCommon(const uint8_t* data, size_t size) {
  if (size < usbipCommonSize) return std::nullopt;
  return OpCommon{get16(data), get16(data + 2), get32(data + 4)};
}

// The exported device as the kernel's import reply describes it.
struct ExportedDevice {
  std::string path, busId; uint32_t busNumber = 1, deviceNumber = 1, speed = 3;   // speed: 1 low, 2 full, 3 high
  uint16_t vendor = 0, product = 0, bcdDevice = 0; uint8_t cls = 0, subclass = 0, protocol = 0, configurationValue = 1, configurations = 1, interfaces = 1;
};
inline Bytes encodeDevice(const ExportedDevice& device) {
  Bytes out;
  auto fixed = [&](const std::string& text, size_t size) { Bytes field(size, 0); std::memcpy(field.data(), text.data(), std::min(text.size(), size - 1)); out.insert(out.end(), field.begin(), field.end()); };
  fixed(device.path, 256); fixed(device.busId, usbipBusIdSize);
  put32(out, device.busNumber); put32(out, device.deviceNumber); put32(out, device.speed);
  put16(out, device.vendor); put16(out, device.product); put16(out, device.bcdDevice);
  out.insert(out.end(), {device.cls, device.subclass, device.protocol, device.configurationValue, device.configurations, device.interfaces});
  return out;
}
inline uint32_t deviceId(const ExportedDevice& device) { return device.busNumber << 16 | device.deviceNumber; }
// "1-N": a busid is only ever one of these.
inline std::string busIdFor(unsigned deviceNumber) { return "1-" + std::to_string(deviceNumber); }
// The busid text of a 32-byte field, or nullopt when it is not NUL-terminated.
inline std::optional<std::string> decodeBusId(const uint8_t* data) {
  const auto* end = static_cast<const uint8_t*>(std::memchr(data, 0, usbipBusIdSize));
  if (!end) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(data), size_t(end - data));
}
inline Bytes encodeImportReply(const ExportedDevice& device) { Bytes out = encodeOpCommon(opRepImport, stOk), body = encodeDevice(device); out.insert(out.end(), body.begin(), body.end()); return out; }

struct Urb {
  uint32_t command = 0, seq = 0, devid = 0, direction = 0, endpoint = 0;
  // CMD_SUBMIT
  uint32_t flags = 0; int32_t length = 0, startFrame = 0, packets = 0, interval = 0; uint8_t setup[8]{};
  // CMD_UNLINK: the seq to cancel
  uint32_t target = 0;
};
inline std::optional<Urb> decodeUrbHeader(const uint8_t* data, size_t size) {
  if (size < usbipHeaderSize) return std::nullopt;
  Urb urb;
  urb.command = get32(data); urb.seq = get32(data + 4); urb.devid = get32(data + 8); urb.direction = get32(data + 12); urb.endpoint = get32(data + 16);
  if (urb.command == cmdSubmit) {
    urb.flags = get32(data + 20); urb.length = int32_t(get32(data + 24)); urb.startFrame = int32_t(get32(data + 28)); urb.packets = int32_t(get32(data + 32)); urb.interval = int32_t(get32(data + 36));
    std::memcpy(urb.setup, data + 40, 8);
  } else if (urb.command == cmdUnlink) urb.target = get32(data + 20);
  else return std::nullopt;
  if (urb.direction > 1) return std::nullopt;
  return urb;
}
inline Bytes encodeRetSubmit(uint32_t seq, uint32_t devid, uint32_t direction, uint32_t endpoint, int32_t status, const uint8_t* data, size_t actual) {
  Bytes out; put32(out, retSubmit); put32(out, seq); put32(out, devid); put32(out, direction); put32(out, endpoint);
  put32(out, uint32_t(status)); put32(out, uint32_t(actual)); put32(out, 0); put32(out, 0xffffffffu); put32(out, 0);   // start_frame, number_of_packets (-1: not isochronous), error_count
  out.resize(usbipHeaderSize, 0);
  if (data && actual) out.insert(out.end(), data, data + actual);
  return out;
}
inline Bytes encodeRetUnlink(uint32_t seq, uint32_t devid, int32_t status) {
  Bytes out; put32(out, retUnlink); put32(out, seq); put32(out, devid); put32(out, 0); put32(out, 0); put32(out, uint32_t(status));
  out.resize(usbipHeaderSize, 0); return out;
}

// --- usb-v1 relay frames -----------------------------------------------------
inline constexpr size_t frameHeaderSize = 24;
inline constexpr uint8_t frameSubmit = 1, frameReturn = 2, frameUnlink = 3, frameUnlinkReturn = 4;
inline constexpr size_t maxUrbBytes = 262144;
struct Frame {
  uint8_t type = 0, device = 0, endpoint = 0, transfer = 0; uint32_t seq = 0, length = 0; int32_t status = 0; uint8_t setup[8]{};
  Bytes payload;
};
inline void putLe32(Bytes& out, uint32_t value) { for (int shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift)); }
inline uint32_t getLe32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline Bytes encodeFrame(const Frame& frame) {
  Bytes out{frame.type, frame.device, frame.endpoint, frame.transfer};
  putLe32(out, frame.seq); putLe32(out, frame.length); putLe32(out, uint32_t(frame.status));
  out.insert(out.end(), frame.setup, frame.setup + 8);
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  return out;
}
inline std::optional<Frame> decodeFrame(const uint8_t* data, size_t size) {
  if (size < frameHeaderSize || size > frameHeaderSize + maxUrbBytes) return std::nullopt;
  Frame frame;
  frame.type = data[0]; frame.device = data[1]; frame.endpoint = data[2]; frame.transfer = data[3];
  frame.seq = getLe32(data + 4); frame.length = getLe32(data + 8); frame.status = int32_t(getLe32(data + 12));
  std::memcpy(frame.setup, data + 16, 8);
  frame.payload.assign(data + frameHeaderSize, data + size);
  return frame;
}
}  // namespace host::usb
