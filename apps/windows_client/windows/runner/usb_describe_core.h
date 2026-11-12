#pragma once
// What the USB picker shows for a device of this PC (docs/usb-redirection.md,
// "Picker"): real names, drives with their letters, tokens, the devices that
// must never be offered, duplicates told apart, and the check before a storage
// device is taken away. Pure logic over plain structs, so it is tested with
// fake device trees (test/native/usb_describe_core_test.cpp); the real tree is
// read by usb_topology.cpp.
//
// Everything produced here stays on this PC: names, volume labels, drive
// letters and VID:PID are shown to the user and never logged or sent
// (descriptor bytes that the VM needs are the only thing that crosses).
#include <windows.h>
#include <compressapi.h>
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "usb_vendors_data.h"
#include "ui_language.h"

namespace multitap::usb {

inline std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? size_t(size) : 0, '\0');
  if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}
inline std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring out(size > 0 ? size_t(size) : 0, L'\0');
  if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
  return out;
}

// --- text helpers ----------------------------------------------------------------

inline std::wstring Trim(std::wstring text) {
  const auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\0' || c == 0xA0; };
  while (!text.empty() && space(text.back())) text.pop_back();
  size_t start = 0;
  while (start < text.size() && space(text[start])) ++start;
  text.erase(0, start);
  return text;
}
inline std::wstring Lower(std::wstring text) {
  for (auto& c : text) c = wchar_t(std::towlower(c));
  return text;
}
// Lower case with runs of blanks collapsed, for comparing names.
inline std::wstring Normal(const std::wstring& text) {
  std::wstring out;
  for (const wchar_t c : Lower(Trim(text))) {
    if (c == L' ' || c == L'\t') { if (!out.empty() && out.back() != L' ') out += L' '; }
    else out += c;
  }
  return out;
}
inline bool EndsWith(const std::wstring& text, const std::wstring& suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}
inline bool StartsWith(const std::wstring& text, const std::wstring& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// The names Windows gives a device it knows nothing about. They say what the
// driver is, not what the device is, so they never become a device's name.
inline bool IsGenericName(const std::wstring& name) {
  static const std::set<std::wstring> generic = {
      L"usb composite device", L"usb mass storage device", L"usb input device", L"usb device", L"unknown usb device",
      L"usb 복합 장치", L"usb 대용량 저장 장치", L"usb 입력 장치", L"usb 장치", L"알 수 없는 usb 장치", L"usb 복합 장치(비디오)",
      L"mass storage device", L"generic usb device", L"usb storage device", L"usb attached scsi (uas) mass storage device",
      L"usb 연결 scsi(uas) 대용량 저장 장치", L"usb serial device", L"usb 직렬 장치",
      L"hid-compliant device", L"hid 규격 장치", L"usb 장치(복합)"};
  const auto normal = Normal(name);
  if (normal.empty() || generic.count(normal)) return true;
  return StartsWith(normal, L"unknown usb device") || StartsWith(normal, L"알 수 없는 usb 장치");
}

// Windows adds these words to a disk's name; they are not part of the product.
inline std::wstring CleanDiskName(std::wstring name) {
  name = Trim(std::move(name));
  for (bool again = true; again;) {
    again = false;
    for (const wchar_t* suffix : {L" USB Device", L" SCSI Disk Device", L" Disk Device", L" USB Disk Device", L" UASP Device"}) {
      if (EndsWith(Normal(name), Lower(suffix)) && name.size() >= wcslen(suffix)) {
        name = Trim(name.substr(0, name.size() - wcslen(suffix)));
        again = true;
      }
    }
  }
  return name;
}

// A manufacturer's name as people say it: legal suffixes and "(formerly ...)" go.
inline std::wstring CleanVendor(std::wstring name) {
  name = Trim(std::move(name));
  if (name.size() > 6 && EndsWith(Lower(name), L".com")) name.resize(name.size() - 4);
  for (const wchar_t* cut : {L" (", L" - ", L" / "}) {
    const auto at = name.find(cut);
    if (at != std::wstring::npos && at > 1) name = Trim(name.substr(0, at));
  }
  static const wchar_t* const suffixes[] = {L"Co., Ltd.", L"Co.,Ltd.", L"Co. Ltd.", L"Co., Ltd", L"Co.,Ltd", L"Company, Inc.", L"Company", L"Corporation", L"Corp.", L"Corp",
      L"Incorporated", L"Inc.", L"Inc", L"Limited", L"Ltd.", L"Ltd", L"GmbH", L"LLC", L"S.A.", L"AG", L"B.V.", L"Technology", L"Technologies", L"Electronics", L"Semiconductor"};
  for (bool again = true; again && name.size() > 2;) {
    again = false;
    while (!name.empty() && (name.back() == L',' || name.back() == L' ')) name.pop_back();
    for (const wchar_t* suffix : suffixes) {
      const size_t length = wcslen(suffix);
      if (name.size() > length + 1 && EndsWith(Lower(name), Lower(suffix)) && (name[name.size() - length - 1] == L' ' || name[name.size() - length - 1] == L',')) {
        name = Trim(name.substr(0, name.size() - length));
        again = true;
        break;
      }
    }
  }
  while (!name.empty() && (name.back() == L',' || name.back() == L' ')) name.pop_back();
  return name;
}

// --- vendor names (usb.ids, vendors only) ------------------------------------------

class VendorNames {
 public:
  static const VendorNames& Get() { static const VendorNames names; return names; }
  // The manufacturer registered for [vid], cleaned; "" when unknown.
  std::wstring Name(uint16_t vid) const {
    const auto found = std::lower_bound(names_.begin(), names_.end(), vid, [](const auto& item, uint16_t id) { return item.first < id; });
    return found != names_.end() && found->first == vid ? CleanVendor(found->second) : std::wstring();
  }
  size_t Count() const { return names_.size(); }

 private:
  VendorNames() {
    DECOMPRESSOR_HANDLE handle = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &handle)) return;
    std::string raw(kVendorRawSize, '\0');
    SIZE_T used = 0;
    const bool ok = Decompress(handle, kVendorBlob, sizeof(kVendorBlob), raw.data(), raw.size(), &used) && used == raw.size();
    CloseDecompressor(handle);
    if (!ok) return;   // no table: names fall back to what the device reports itself
    size_t at = 0;
    while (at < raw.size()) {
      const auto end = raw.find('\n', at);
      if (end == std::string::npos) break;
      if (end - at > 5 && raw[at + 4] == '\t') {
        unsigned vid = 0; bool good = true;
        for (size_t i = 0; i < 4; ++i) {
          const char c = raw[at + i];
          vid = vid << 4 | unsigned(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : (good = false, 0));
        }
        if (good) names_.emplace_back(uint16_t(vid), Widen(raw.substr(at + 5, end - at - 5)));
      }
      at = end + 1;
    }
    std::stable_sort(names_.begin(), names_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  std::vector<std::pair<uint16_t, std::wstring>> names_;
};

// --- the device tree as the real layer reads it ----------------------------------

struct RawVolume {
  std::wstring path;          // \\?\Volume{...}\ (the safe-hand-over check opens it)
  std::wstring label, fileSystem;
  std::wstring letters;       // "EF" for E: and F:
  bool hasNpki = false;       // an NPKI or GPKI folder in the root (existence only)
  uint64_t bytes = 0;
};
struct RawDisk {
  std::wstring name;          // the disk's own friendly name
  uint64_t bytes = 0;
  std::vector<RawVolume> volumes;
};
struct RawDevice {
  std::wstring deviceId, instanceId;
  std::vector<uint8_t> descriptors;   // device descriptor + every configuration
  std::wstring busReportedDesc, friendlyName, deviceDesc;   // 3 names, best first
  uint32_t port = 0;                  // the hub port, 0 unknown
  uint64_t arrivalMs = 0;             // LastArrivalDate as Unix milliseconds, 0 unknown
  std::set<std::wstring> childClasses;   // Windows setup classes of the device's subtree
  std::vector<RawDisk> disks;
  bool capturedHere = false;          // this client holds it for the VM
};

enum class Kind { Storage, Token, Printer, Phone, Camera, Audio, Bluetooth, Hub, Input, Network, Other };
inline const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::Storage: return "storage"; case Kind::Token: return "token"; case Kind::Printer: return "printer"; case Kind::Phone: return "phone";
    case Kind::Camera: return "camera"; case Kind::Audio: return "audio"; case Kind::Bluetooth: return "bluetooth"; case Kind::Hub: return "hub";
    case Kind::Input: return "input"; case Kind::Network: return "network"; default: return "other";
  }
}

// Offered nowhere: sharing them would cut the PC off from the connection (a USB
// network adapter), from the VM's own input (keyboard, mouse) or from its other
// USB devices (a hub). Listed only as a count.
inline bool NeededByThisPc(Kind kind) { return kind == Kind::Hub || kind == Kind::Input || kind == Kind::Network; }

struct Described {
  Kind kind = Kind::Other;
  std::string title, subtitle, vidPid, memoryKey;
  std::vector<std::string> chips;
  uint64_t arrivalMs = 0;
  bool busyElsewhere = false;           // held by another session or program (heuristic)
  // A storage device Windows shows no usable drive for (no volume with a file system Windows knows, no letter): not
  // formatted, or a format Windows cannot read. The VM would see an uninitialised disk, not a drive.
  bool noReadableDrive = false;
  std::vector<std::wstring> volumePaths;   // for the check before a storage device is taken
};

struct Interface { uint8_t cls = 0, sub = 0, proto = 0; };
struct Parsed {
  bool ok = false; uint16_t vid = 0, pid = 0; uint8_t deviceClass = 0;
  std::vector<Interface> interfaces;
  bool Has(uint8_t cls) const { for (const auto& i : interfaces) if (i.cls == cls) return true; return false; }
  bool Has(uint8_t cls, uint8_t sub) const { for (const auto& i : interfaces) if (i.cls == cls && i.sub == sub) return true; return false; }
};
inline Parsed ParseDescriptors(const std::vector<uint8_t>& bytes) {
  Parsed out;
  if (bytes.size() < 18 || bytes[0] != 18 || bytes[1] != 1) return out;
  out.vid = uint16_t(bytes[8] | bytes[9] << 8); out.pid = uint16_t(bytes[10] | bytes[11] << 8); out.deviceClass = bytes[4];
  size_t at = 18;
  while (at + 9 <= bytes.size() && bytes[at + 1] == 2) {
    const size_t total = size_t(bytes[at + 2] | bytes[at + 3] << 8);
    if (total < 9 || at + total > bytes.size()) break;
    for (size_t i = at; i + 2 <= at + total;) {
      const size_t length = bytes[i];
      if (length < 2 || i + length > at + total) break;
      if (bytes[i + 1] == 4 && length >= 9) out.interfaces.push_back({bytes[i + 5], bytes[i + 6], bytes[i + 7]});
      i += length;
    }
    at += total;
  }
  out.ok = true;
  return out;
}

inline std::string HexId(uint16_t value) {
  char text[5]; const char hex[] = "0123456789ABCDEF";
  for (int i = 0; i < 4; ++i) text[i] = hex[(value >> (12 - 4 * i)) & 15];
  text[4] = 0; return text;
}

// "32GB" as the box says it: decimal units, snapped to the capacity the stick is
// sold as (a 32GB stick holds about 30 to 31.5e9 bytes). "" when unknown.
inline std::string FormatSize(uint64_t bytes) {
  if (bytes == 0) return {};
  const double gb = double(bytes) / 1e9;
  static const double sold[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1000, 2000, 4000, 8000};
  for (const double size : sold) if (gb <= size * 1.005 && gb >= size * 0.90 && size >= 1) {
    return size >= 1000 ? std::to_string(int(size / 1000)) + "TB" : std::to_string(int(size)) + "GB";
  }
  char text[32];
  if (gb >= 1000) snprintf(text, sizeof(text), "%.1fTB", gb / 1000);
  else if (gb >= 10) snprintf(text, sizeof(text), "%.0fGB", gb);
  else if (gb >= 1) snprintf(text, sizeof(text), "%.1fGB", gb);
  else snprintf(text, sizeof(text), "%.0fMB", double(bytes) / 1e6);
  std::string out = text;
  const auto dot = out.find(".0");
  if (dot != std::string::npos) out.erase(dot, 2);
  return out;
}

// "SanDisk" + "Cruzer Blade", without saying the maker twice.
inline std::wstring Combine(const std::wstring& vendor, const std::wstring& product) {
  if (product.empty()) return vendor;
  if (vendor.empty()) return product;
  const auto v = Normal(vendor), p = Normal(product);
  if (StartsWith(p, v) || p.find(v) != std::wstring::npos) return product;
  return vendor + L" " + product;
}

inline std::wstring SerialOf(const RawDevice& device) {
  // Windows invents an instance ID with '&' for a device without a serial number.
  return device.instanceId.find(L'&') != std::wstring::npos ? std::wstring() : device.instanceId;
}

// The classification and the texts for one device. Duplicates are told apart
// afterwards (DescribeAll), because that needs the other devices.
inline Described DescribeOne(const RawDevice& device, bool* hidden = nullptr) {
  Described out;
  const Parsed parsed = ParseDescriptors(device.descriptors);
  const auto child = [&](const wchar_t* name) { return device.childClasses.count(name) != 0; };
  const bool hub = parsed.deviceClass == 9 || parsed.Has(9);
  const bool network = child(L"Net") || parsed.Has(2, 6) || parsed.Has(2, 0x0d) || (parsed.Has(0xe0) && [&] { for (const auto& i : parsed.interfaces) if (i.cls == 0xe0 && i.sub == 1 && i.proto == 3) return true; return false; }());
  const bool input = child(L"Keyboard") || child(L"Mouse") || [&] { for (const auto& i : parsed.interfaces) if (i.cls == 3 && i.sub == 1) return true; return false; }();
  const bool token = parsed.Has(0x0b) || child(L"SmartCardReader");
  bool volumes = false;
  for (const auto& disk : device.disks) if (!disk.volumes.empty()) volumes = true;
  const bool storage = parsed.Has(8) || !device.disks.empty();

  if (hub) out.kind = Kind::Hub;
  else if (network) out.kind = Kind::Network;
  else if (input) out.kind = Kind::Input;
  else if (token) out.kind = Kind::Token;
  else if (storage) out.kind = Kind::Storage;
  else if (parsed.Has(7) || child(L"PrintQueue") || child(L"Printer")) out.kind = Kind::Printer;
  else if (child(L"WPD")) out.kind = Kind::Phone;
  else if (parsed.Has(6) || child(L"Image")) out.kind = Kind::Camera;
  else if (parsed.Has(1) || child(L"MEDIA")) out.kind = Kind::Audio;
  else if (parsed.Has(0xe0) || child(L"Bluetooth")) out.kind = Kind::Bluetooth;
  else out.kind = Kind::Other;
  if (hidden) *hidden = NeededByThisPc(out.kind);

  const std::wstring vendor = VendorNames::Get().Name(parsed.vid);
  std::wstring product;
  for (const auto* name : {&device.busReportedDesc, &device.friendlyName, &device.deviceDesc}) {
    const auto cleaned = Trim(*name);
    if (!cleaned.empty() && !IsGenericName(cleaned)) { product = cleaned; break; }
  }
  const bool nameless = Trim(device.busReportedDesc).empty() && Trim(device.friendlyName).empty() && Trim(device.deviceDesc).empty();
  out.busyElsewhere = nameless && !device.capturedHere && out.kind != Kind::Hub;
  out.arrivalMs = device.arrivalMs;
  out.vidPid = HexId(parsed.vid) + ":" + HexId(parsed.pid);
  const auto serial = SerialOf(device);
  out.memoryKey = Narrow(Lower(Widen(out.vidPid))) + (serial.empty() ? std::string() : ":" + Narrow(Lower(serial)));

  if (out.kind == Kind::Storage) {
    // Every volume of every disk of the device, the way Explorer lists a card reader.
    std::wstring letters, label, anyLabel, diskName;
    uint64_t bytes = 0;
    std::vector<std::wstring> systems;
    for (const auto& disk : device.disks) {
      if (diskName.empty() && !Trim(disk.name).empty()) diskName = CleanDiskName(disk.name);
      bytes += disk.bytes;
      for (const auto& volume : disk.volumes) {
        for (const wchar_t c : volume.letters) if (letters.find(c) == std::wstring::npos) letters += c;
        if (label.empty() && !volume.label.empty() && !volume.letters.empty()) label = volume.label;
        if (anyLabel.empty()) anyLabel = volume.label;
        if (!volume.fileSystem.empty() && std::find(systems.begin(), systems.end(), volume.fileSystem) == systems.end()) systems.push_back(volume.fileSystem);
        out.volumePaths.push_back(volume.path);
        if (disk.bytes == 0) bytes += volume.bytes;
      }
    }
    std::sort(letters.begin(), letters.end());
    const bool emptyReader = !volumes && !device.disks.empty() && bytes == 0;
    if (emptyReader) {
      out.title = Narrow(product.empty() ? Combine(vendor, multitap::Tr(L"카드 리더기", L"card reader")) : product);
      out.subtitle = multitap::TrA("메모리 카드가 없어요", "No memory card");
    } else {
      if (label.empty()) label = anyLabel;
      std::wstring title = label.empty() ? std::wstring(multitap::Tr(L"USB 드라이브", L"USB drive")) : label;
      if (letters.empty()) title += multitap::Tr(L" · 드라이브 문자 없음", L" · no drive letter");
      else {
        title += L" (";
        for (size_t i = 0; i < letters.size(); ++i) { if (i) title += L", "; title += letters[i]; title += L':'; }
        title += L")";
      }
      out.title = Narrow(title);
      out.noReadableDrive = letters.empty() && systems.empty();
      const std::wstring base = !diskName.empty() ? diskName : Combine(vendor, product);
      std::string subtitle = Narrow(base);
      const auto size = FormatSize(bytes);
      if (!size.empty()) subtitle += (subtitle.empty() ? "" : " · ") + size;
      if (!systems.empty()) {
        std::wstring joined;
        for (const auto& item : systems) joined += (joined.empty() ? L"" : L"/") + item;
        subtitle += (subtitle.empty() ? "" : " · ") + Narrow(joined);
      }
      out.subtitle = subtitle;
    }
    return out;
  }

  std::wstring title = product;
  if (title.empty()) {
    switch (out.kind) {
      case Kind::Token: title = vendor.empty() ? std::wstring(multitap::Tr(L"보안 토큰", L"Security token")) : vendor + multitap::Tr(L" 보안 토큰", L" security token"); break;
      case Kind::Printer: title = vendor.empty() ? std::wstring(multitap::Tr(L"프린터", L"Printer")) : vendor + multitap::Tr(L" 프린터", L" printer"); break;
      case Kind::Phone: title = vendor.empty() ? std::wstring(multitap::Tr(L"휴대폰", L"Phone")) : vendor + multitap::Tr(L" 휴대폰", L" phone"); break;
      case Kind::Camera: title = vendor.empty() ? std::wstring(multitap::Tr(L"카메라", L"Camera")) : vendor + multitap::Tr(L" 카메라", L" camera"); break;
      default: title = vendor.empty() ? std::wstring(multitap::Tr(L"USB 장치", L"USB device")) : vendor + multitap::Tr(L" USB 장치", L" USB device"); break;
    }
  }
  out.title = Narrow(title);
  if (!vendor.empty() && Normal(title).find(Normal(vendor)) == std::wstring::npos) out.subtitle = Narrow(vendor);
  return out;
}

// Describes every device, tells identical titles apart and drops nothing: the
// caller decides what to do with the devices this PC needs ([hidden]).
struct DescribedDevice { Described info; bool hidden = false; std::string id; };
inline std::vector<DescribedDevice> DescribeAll(const std::vector<RawDevice>& devices) {
  std::vector<DescribedDevice> out;
  for (const auto& device : devices) {
    DescribedDevice item;
    item.info = DescribeOne(device, &item.hidden);
    item.id = Narrow(device.deviceId + L"|" + device.instanceId);
    out.push_back(std::move(item));
  }
  // Two identical sticks: the drive letter is already in the title; otherwise the
  // serial number's end, otherwise the hub port, otherwise a count.
  std::map<std::string, std::vector<size_t>> groups;
  for (size_t i = 0; i < out.size(); ++i) if (!out[i].hidden) groups[std::string(KindName(out[i].info.kind)) + "\n" + out[i].info.title].push_back(i);
  for (const auto& [key, members] : groups) {
    if (members.size() < 2) continue;
    std::vector<std::wstring> serials; std::vector<uint32_t> ports;
    bool serialsOk = true, portsOk = true;
    for (const auto index : members) {
      const auto serial = SerialOf(devices[index]);
      serials.push_back(serial); ports.push_back(devices[index].port);
      if (serial.empty()) serialsOk = false;
      if (devices[index].port == 0) portsOk = false;
    }
    const auto distinct = [&](size_t keep) {
      std::set<std::wstring> seen;
      for (const auto& serial : serials) seen.insert(serial.size() > keep ? serial.substr(serial.size() - keep) : serial);
      return seen.size() == serials.size();
    };
    size_t keep = 4;
    if (serialsOk) { while (keep < 32 && !distinct(keep)) keep += 4; serialsOk = distinct(keep); }
    if (portsOk) portsOk = std::set<uint32_t>(ports.begin(), ports.end()).size() == ports.size();
    for (size_t n = 0; n < members.size(); ++n) {
      auto& title = out[members[n]].info.title;
      if (serialsOk) {
        auto tail = Lower(serials[n].size() > keep ? serials[n].substr(serials[n].size() - keep) : serials[n]);
        for (auto& c : tail) c = wchar_t(std::towupper(c));
        title += multitap::TrA(" (일련번호 …", " (serial …") + Narrow(tail) + ")";
      } else if (portsOk) {
        title += multitap::TrA(" (포트 ", " (port ") + std::to_string(ports[n]) + ")";
      } else {
        title += " (" + std::to_string(n + 1) + ")";
      }
    }
  }
  return out;
}

// --- the check before a storage device leaves this PC ----------------------------------

// The files on a stick must be closed before Windows loses it. A "safe eject"
// (CM_Request_Device_EjectW) is not used: it removes the device from the PnP tree,
// and a removed device is no longer in UsbDk's list, so it could not be captured
// afterwards. Locking each volume asks the same question (is anything open?) and
// leaves the device where UsbDk expects it.
enum class LockResult { Locked, InUse, Skipped };
class VolumeApi {
 public:
  virtual ~VolumeApi() = default;
  // A handle for [path] or nullptr (access denied, not ready: the volume is skipped).
  virtual void* Open(const std::wstring& path) = 0;
  // Locks the volume (writing out its cache). InUse: files or folders are open.
  virtual LockResult Lock(void* volume) = 0;
  virtual void Dismount(void* volume) = 0;   // while locked
  virtual void Close(void* volume) = 0;      // also unlocks
  // The program that holds the volume, when Windows says (usually it does not).
  virtual std::wstring Blocker(const std::wstring&) { return {}; }
};
struct PrepareOutcome { bool ready = true; bool checkedAll = true; std::string blocker; };

inline PrepareOutcome PrepareStorage(VolumeApi& api, const std::vector<std::wstring>& volumes, bool dismount = true, const std::function<bool()>& cancelled = {}) {
  PrepareOutcome out;
  std::vector<void*> locked;
  struct Handles {
    VolumeApi& api; std::vector<void*>& values;
    ~Handles() { for (auto* handle : values) { try { api.Close(handle); } catch (...) {} } }
  } handles{api, locked};
  for (const auto& path : volumes) {
    if (cancelled && cancelled()) { out.ready = false; return out; }
    void* handle = api.Open(path);
    if (!handle) { out.checkedAll = false; continue; }
    locked.push_back(handle);  // close even if Lock/Blocker throws
    const auto result = api.Lock(handle);
    if (cancelled && cancelled()) { out.ready = false; return out; }
    if (result == LockResult::InUse) {
      out.ready = false; out.blocker = Narrow(api.Blocker(path));
      return out;
    }
    if (result == LockResult::Skipped) { out.checkedAll = false; api.Close(handle); locked.pop_back(); continue; }
  }
  // Before the VM accepts: lock/flush/check only. A cancelled slow query must
  // never dismount later. Dismount is repeated only at accepted capture commit.
  if (dismount) for (auto* handle : locked) {
    if (cancelled && cancelled()) { out.ready = false; return out; }
    api.Dismount(handle);
  }
  return out;
}

}  // namespace multitap::usb
