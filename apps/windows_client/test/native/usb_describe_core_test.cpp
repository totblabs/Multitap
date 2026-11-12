// The USB picker's naming and classification (usb_describe_core.h) over fake
// device trees: drives with and without label or letter, several partitions, the
// NPKI chip, tokens, composite devices, the devices that must never be offered,
// identical devices, arrival data, the vendor table and the check before a
// storage device is taken (a fake volume layer). No device, driver or disk is
// touched.
#include "usb_describe_core.h"
#include <iostream>
#include <stdexcept>

using namespace multitap::usb;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
void same(const std::string& actual, const std::string& expected, const char* reason) {
  ++checks;
  if (actual != expected) throw std::runtime_error(std::string(reason) + ": got \"" + actual + "\", expected \"" + expected + "\"");
}

struct Iface { uint8_t cls, sub, proto; };
// A device descriptor plus one configuration holding [interfaces].
std::vector<uint8_t> descriptors(uint16_t vid, uint16_t pid, uint8_t deviceClass, std::vector<Iface> interfaces) {
  std::vector<uint8_t> out = {18, 1, 0, 2, deviceClass, 0, 0, 64, uint8_t(vid & 255), uint8_t(vid >> 8), uint8_t(pid & 255), uint8_t(pid >> 8), 0, 1, 1, 2, 3, 1};
  std::vector<uint8_t> body;
  uint8_t number = 0;
  for (const auto& i : interfaces) body.insert(body.end(), {9, 4, number++, 0, 0, i.cls, i.sub, i.proto, 0});
  const size_t total = 9 + body.size();
  out.insert(out.end(), {9, 2, uint8_t(total & 255), uint8_t(total >> 8), uint8_t(interfaces.size()), 1, 0, 0x80, 50});
  out.insert(out.end(), body.begin(), body.end());
  return out;
}
RawVolume volume(const std::wstring& label, const std::wstring& letters, uint64_t bytes = 31000000000ull, bool npki = false, const std::wstring& fs = L"exFAT") {
  RawVolume v; v.path = L"\\\\?\\Volume{" + label + letters + L"}\\"; v.label = label; v.letters = letters; v.bytes = bytes; v.hasNpki = npki; v.fileSystem = fs; return v;
}
RawDevice stickDevice(const std::wstring& serial, std::vector<RawVolume> volumes, const std::wstring& diskName = L"SanDisk Cruzer Blade USB Device", uint64_t size = 31000000000ull) {
  RawDevice d; d.deviceId = L"USB\\VID_0781&PID_5567"; d.instanceId = serial;
  d.descriptors = descriptors(0x0781, 0x5567, 0, {{8, 6, 0x50}});
  d.busReportedDesc = L"Cruzer Blade"; d.deviceDesc = L"USB 대용량 저장 장치";
  RawDisk disk; disk.name = diskName; disk.bytes = size; disk.volumes = std::move(volumes);
  d.disks.push_back(std::move(disk));
  d.childClasses = {L"DiskDrive"};
  return d;
}

void vendors() {
  const auto& names = VendorNames::Get();
  require(names.Count() > 3000, "The vendor table did not load");
  require(names.Name(0x0781) == L"SanDisk", "SanDisk");
  require(names.Name(0x090c) == L"Silicon Motion", "Silicon Motion keeps its name without legal words");
  require(names.Name(0x046d) == L"Logitech", "Logitech");
  require(names.Name(0xfffe).empty(), "An unregistered vendor has no name");
  require(CleanVendor(L"Kingston Technology Company, Inc.") == L"Kingston", "Kingston");
  require(CleanVendor(L"Alcor Micro, Corp.") == L"Alcor Micro", "Alcor");
  require(CleanVendor(L"Realtek Semiconductor Corp.") == L"Realtek", "Realtek");
  require(CleanVendor(L"Genesys Logic, Inc. (formerly Foo)") == L"Genesys Logic", "Parentheses are cut");
}

void names() {
  require(IsGenericName(L"USB Composite Device") && IsGenericName(L"USB 대용량 저장 장치") && IsGenericName(L"USB 복합 장치") && IsGenericName(L"") &&
              IsGenericName(L"알 수 없는 USB 장치(장치 설명자 요청 실패)"), "Generic names");
  require(!IsGenericName(L"YubiKey OTP+FIDO+CCID") && !IsGenericName(L"Cruzer Blade"), "Real names are kept");
  require(CleanDiskName(L"SanDisk Cruzer Blade USB Device") == L"SanDisk Cruzer Blade", "USB Device is trimmed");
  require(CleanDiskName(L"Samsung PSSD T7 SCSI Disk Device") == L"Samsung PSSD T7", "SCSI Disk Device is trimmed");
  require(Combine(L"SanDisk", L"Cruzer Blade") == L"SanDisk Cruzer Blade" && Combine(L"SanDisk", L"SanDisk Ultra") == L"SanDisk Ultra" && Combine(L"", L"X") == L"X", "Combine");
  same(FormatSize(31000000000ull), "32GB", "A 32GB stick");
  same(FormatSize(7700000000ull), "8GB", "An 8GB stick");
  same(FormatSize(1000204886016ull), "1TB", "A 1TB disk");
  same(FormatSize(500107862016ull), "512GB", "A 500GB disk is sold as 512GB only when close");
  same(FormatSize(120034123776ull), "128GB", "Snaps up to 128");
  same(FormatSize(5400000000ull), "5.4GB", "An odd size stays decimal");
  same(FormatSize(300000000ull), "300MB", "Megabytes");
  same(FormatSize(0), "", "Unknown size");
}

void storage() {
  // Label and letter, with a certificate folder.
  auto d = DescribeOne(stickDevice(L"A1B2", {volume(L"내 USB", L"E", 31000000000ull, true)}));
  require(d.kind == Kind::Storage, "storage kind");
  same(d.title, "내 USB (E:)", "label and letter");
  same(d.subtitle, "SanDisk Cruzer Blade · 32GB · exFAT", "vendor, product, size and file system");
  require(d.chips.empty(), "Certificate-folder hints must not become USB badges");
  same(d.vidPid, "0781:5567", "VID:PID is secondary text");
  // Without a label.
  same(DescribeOne(stickDevice(L"A1B2", {volume(L"", L"F")})).title, "USB 드라이브 (F:)", "no label");
  require(DescribeOne(stickDevice(L"A1B2", {volume(L"", L"F")})).chips.empty(), "no NPKI, no chip");
  // Without a letter.
  same(DescribeOne(stickDevice(L"A1B2", {volume(L"백업", L"")})).title, "백업 · 드라이브 문자 없음", "labelled, no letter");
  same(DescribeOne(stickDevice(L"A1B2", {volume(L"", L"")})).title, "USB 드라이브 · 드라이브 문자 없음", "no label, no letter");
  same(DescribeOne(stickDevice(L"A1B2", {})).title, "USB 드라이브 · 드라이브 문자 없음", "a disk without any volume (RAW or still mounting)");
  // A stick with no volume (or only one Windows has no file system for) has no drive the VM could show either.
  require(DescribeOne(stickDevice(L"A1B2", {})).noReadableDrive, "a disk without any volume has no readable drive");
  require(DescribeOne(stickDevice(L"A1B2", {volume(L"", L"", 31000000000ull, false, L"")})).noReadableDrive, "a volume without a known file system has no readable drive");
  require(!DescribeOne(stickDevice(L"A1B2", {volume(L"백업", L"")})).noReadableDrive && !DescribeOne(stickDevice(L"A1B2", {volume(L"X", L"H")})).noReadableDrive, "a formatted volume (with or without a letter) is readable");
  // Several partitions: all letters, the labelled letter's label.
  d = DescribeOne(stickDevice(L"A1B2", {volume(L"", L"G"), volume(L"자료", L"F")}));
  same(d.title, "자료 (F:, G:)", "two letters, sorted");
  require(d.volumePaths.size() == 2, "both volumes are known to the hand-over check");
  // A volume without a letter next to one with a letter does not hide the letter.
  same(DescribeOne(stickDevice(L"A1B2", {volume(L"", L""), volume(L"X", L"H")})).title, "X (H:)", "mixed letters");
  // A file system and size that are not known leave the text short.
  RawDevice bare = stickDevice(L"A1B2", {volume(L"V", L"E", 0, false, L"")}, L"", 0);
  d = DescribeOne(bare);
  same(d.subtitle, "SanDisk Cruzer Blade", "product only when there is no disk name, size or file system");
  // The bus-reported name beats a generic one, a missing disk name falls back to it.
  bare.busReportedDesc = L""; bare.deviceDesc = L"USB 대용량 저장 장치";
  same(DescribeOne(bare).subtitle, "SanDisk", "vendor only when the device names itself generically");
  // Two disks (card reader with two cards): one entry, both letters, sizes added.
  RawDevice reader = stickDevice(L"R1", {volume(L"SD", L"E", 15000000000ull)}, L"Generic- SD/MMC USB Device", 15000000000ull);
  RawDisk second; second.name = L"Generic- MS/MS-PRO USB Device"; second.bytes = 1000000000ull; second.volumes = {volume(L"MS", L"F", 1000000000ull)};
  reader.disks.push_back(second);
  d = DescribeOne(reader);
  same(d.title, "SD (E:, F:)", "card reader with two cards");
  same(d.subtitle, "Generic- SD/MMC · 16GB · exFAT", "sizes of both cards");
  // An empty card reader is still a device the user can read about.
  RawDevice empty = stickDevice(L"R2", {}, L"Generic- SD/MMC USB Device", 0);
  empty.busReportedDesc = L"USB3.0 Card Reader";
  d = DescribeOne(empty);
  same(d.title, "USB3.0 Card Reader", "an empty reader is called by its name");
  same(d.subtitle, "메모리 카드가 없어요", "and says why it has no drive");
  // Storage whose disk is not up yet: the class says it is a drive.
  RawDevice early; early.deviceId = L"USB\\VID_0781&PID_5567"; early.instanceId = L"A1B2"; early.descriptors = descriptors(0x0781, 0x5567, 0, {{8, 6, 0x50}});
  early.busReportedDesc = L"Cruzer Blade";
  d = DescribeOne(early);
  require(d.kind == Kind::Storage, "Mass storage class without a disk yet is storage");
  same(d.title, "USB 드라이브 · 드라이브 문자 없음", "still mounting");
  same(d.subtitle, "SanDisk Cruzer Blade", "its product name");
}

void tokens_and_others() {
  RawDevice token; token.deviceId = L"USB\\VID_1050&PID_0407"; token.instanceId = L"0001234";
  token.descriptors = descriptors(0x1050, 0x0407, 0, {{3, 0, 0}, {0x0b, 0, 0}});
  token.busReportedDesc = L"YubiKey FIDO+CCID"; token.deviceDesc = L"USB 복합 장치";
  auto d = DescribeOne(token);
  require(d.kind == Kind::Token, "CCID is a token");
  same(d.title, "YubiKey FIDO+CCID", "composite devices show their product name");
  same(d.subtitle, "Yubico", "vendor from the table");
  // A reader recognised only by its setup class.
  RawDevice reader; reader.deviceId = L"USB\\VID_072F&PID_90CC"; reader.instanceId = L"X"; reader.descriptors = descriptors(0x072f, 0x90cc, 0xff, {{0xff, 0, 0}});
  reader.deviceDesc = L"ACR38 USB Smart Card Reader"; reader.childClasses = {L"SmartCardReader"};
  require(DescribeOne(reader).kind == Kind::Token, "SmartCardReader child makes a token");
  // Token with only generic names: vendor + role, never VID:PID as the name.
  token.busReportedDesc = L""; token.deviceDesc = L"USB 복합 장치";
  d = DescribeOne(token);
  same(d.title, "Yubico 보안 토큰", "generic names give vendor and role");
  require(d.title.find("1050") == std::string::npos, "never raw VID/PID as the name");
  // A composite with nothing to say and an unknown vendor.
  RawDevice mystery; mystery.deviceId = L"USB\\VID_FFFE&PID_0001"; mystery.instanceId = L"5&1&0&2"; mystery.descriptors = descriptors(0xfffe, 1, 0xff, {{0xff, 0, 0}});
  mystery.deviceDesc = L"USB 복합 장치";
  d = DescribeOne(mystery);
  same(d.title, "USB 장치", "unknown vendor, generic name");
  same(d.vidPid, "FFFE:0001", "ids stay small and secondary");
  require(d.subtitle.empty() && !d.busyElsewhere, "no subtitle");
  // Printer, phone, camera, others.
  RawDevice printer; printer.deviceId = L"USB\\VID_03F0&PID_0001"; printer.instanceId = L"1"; printer.descriptors = descriptors(0x03f0, 1, 0, {{7, 1, 2}}); printer.busReportedDesc = L"LaserJet 1020";
  d = DescribeOne(printer);
  require(d.kind == Kind::Printer, "printer"); same(d.title, "LaserJet 1020", "printer name"); same(d.subtitle, "HP", "vendor");
  RawDevice phone; phone.deviceId = L"USB\\VID_04E8&PID_6860"; phone.instanceId = L"2"; phone.descriptors = descriptors(0x04e8, 0x6860, 0, {{0xff, 0xff, 0}, {0xff, 0x42, 1}}); phone.busReportedDesc = L"SM-S918N"; phone.childClasses = {L"WPD"};
  require(DescribeOne(phone).kind == Kind::Phone, "MTP phone");
  RawDevice camera; camera.deviceId = L"USB\\VID_04A9&PID_3000"; camera.instanceId = L"3"; camera.descriptors = descriptors(0x04a9, 0x3000, 0, {{6, 1, 1}}); camera.busReportedDesc = L"Canon EOS";
  require(DescribeOne(camera).kind == Kind::Camera, "PTP camera");
  RawDevice audio; audio.deviceId = L"USB\\VID_0D8C&PID_0014"; audio.instanceId = L"4"; audio.descriptors = descriptors(0x0d8c, 0x14, 0, {{1, 1, 0}, {1, 2, 0}, {3, 0, 0}}); audio.busReportedDesc = L"USB Audio Device";
  require(DescribeOne(audio).kind == Kind::Audio, "audio");
  RawDevice bluetooth; bluetooth.deviceId = L"USB\\VID_8087&PID_0033"; bluetooth.instanceId = L"5"; bluetooth.descriptors = descriptors(0x8087, 0x33, 0xe0, {{0xe0, 1, 1}}); bluetooth.busReportedDesc = L"Intel Wireless Bluetooth";
  require(DescribeOne(bluetooth).kind == Kind::Bluetooth, "bluetooth");
  // A device with no name at all and not held by us is held by someone else.
  RawDevice held; held.deviceId = L"USB\\VID_0781&PID_5567"; held.instanceId = L"A1B2"; held.descriptors = descriptors(0x0781, 0x5567, 0, {{8, 6, 0x50}});
  require(DescribeOne(held).busyElsewhere, "A device Windows has no name for is in use elsewhere");
  held.capturedHere = true;
  require(!DescribeOne(held).busyElsewhere, "...unless this client holds it");
}

void never_offered() {
  bool hidden = false;
  RawDevice hub; hub.deviceId = L"USB\\VID_05E3&PID_0610"; hub.instanceId = L"1"; hub.descriptors = descriptors(0x05e3, 0x610, 9, {{9, 0, 1}}); hub.busReportedDesc = L"USB2.0 Hub";
  require(DescribeOne(hub, &hidden).kind == Kind::Hub && hidden, "hub");
  // A dock: hub and ethernet and card reader in one composite device.
  RawDevice dock; dock.deviceId = L"USB\\VID_17EF&PID_A387"; dock.instanceId = L"2"; dock.descriptors = descriptors(0x17ef, 0xa387, 0, {{9, 0, 0}, {0xff, 0, 0}}); dock.busReportedDesc = L"ThinkPad dock";
  require(DescribeOne(dock, &hidden).kind == Kind::Hub && hidden, "hub function in a composite device");
  RawDevice keyboard; keyboard.deviceId = L"USB\\VID_046D&PID_C31C"; keyboard.instanceId = L"3"; keyboard.descriptors = descriptors(0x046d, 0xc31c, 0, {{3, 1, 1}}); keyboard.busReportedDesc = L"USB Keyboard";
  require(DescribeOne(keyboard, &hidden).kind == Kind::Input && hidden, "boot keyboard");
  RawDevice mouse = keyboard; mouse.descriptors = descriptors(0x046d, 0xc077, 0, {{3, 1, 2}});
  require(DescribeOne(mouse, &hidden).kind == Kind::Input && hidden, "boot mouse");
  // A gaming keyboard without a boot interface is still a keyboard to Windows.
  RawDevice gaming = keyboard; gaming.descriptors = descriptors(0x1b1c, 0x1b2e, 0, {{3, 0, 0}}); gaming.childClasses = {L"Keyboard", L"HIDClass"};
  require(DescribeOne(gaming, &hidden).kind == Kind::Input && hidden, "keyboard found by its child class");
  // FIDO-style HID is not an input device.
  RawDevice fido = keyboard; fido.descriptors = descriptors(0x1050, 0x0120, 0, {{3, 0, 0}}); fido.childClasses = {L"HIDClass"}; fido.busReportedDesc = L"Security Key";
  require(DescribeOne(fido, &hidden).kind == Kind::Other && !hidden, "FIDO key is offered");
  // USB network adapters: found by their Net child, and by their descriptors.
  RawDevice nic; nic.deviceId = L"USB\\VID_0B95&PID_1790"; nic.instanceId = L"4"; nic.descriptors = descriptors(0x0b95, 0x1790, 0xff, {{0xff, 0xff, 0}}); nic.busReportedDesc = L"AX88179"; nic.childClasses = {L"Net"};
  require(DescribeOne(nic, &hidden).kind == Kind::Network && hidden, "USB ethernet by Net child");
  RawDevice ecm = nic; ecm.childClasses.clear(); ecm.descriptors = descriptors(0x0b95, 1, 0, {{2, 6, 0}, {10, 0, 0}});
  require(DescribeOne(ecm, &hidden).kind == Kind::Network && hidden, "CDC ECM by class");
  RawDevice rndis = nic; rndis.childClasses.clear(); rndis.descriptors = descriptors(0x04e8, 1, 0xef, {{0xe0, 1, 3}, {10, 0, 0}});
  require(DescribeOne(rndis, &hidden).kind == Kind::Network && hidden, "RNDIS (a phone's USB tethering) by class");
  RawDevice wifi = nic; wifi.busReportedDesc = L"802.11ac WLAN Adapter";
  require(DescribeOne(wifi, &hidden).kind == Kind::Network && hidden, "USB Wi-Fi dongle");
  // A serial adapter or a vendor device is not needed by the PC.
  RawDevice serial; serial.deviceId = L"USB\\VID_0403&PID_6001"; serial.instanceId = L"A5"; serial.descriptors = descriptors(0x0403, 0x6001, 0, {{0xff, 0xff, 0xff}}); serial.busReportedDesc = L"FT232R USB UART"; serial.childClasses = {L"Ports"};
  require(DescribeOne(serial, &hidden).kind == Kind::Other && !hidden, "serial adapter is offered");
  // DescribeAll keeps hidden devices (the caller counts them).
  const auto all = DescribeAll({hub, keyboard, nic, serial});
  int needed = 0; for (const auto& item : all) needed += item.hidden ? 1 : 0;
  require(all.size() == 4 && needed == 3, "three of four are needed by the PC");
}

void duplicates() {
  // Same label and letters cannot happen, so two sticks differ by letter.
  auto both = DescribeAll({stickDevice(L"S1", {volume(L"DATA", L"E")}), stickDevice(L"S2", {volume(L"DATA", L"F")})});
  same(both[0].info.title, "DATA (E:)", "first by letter"); same(both[1].info.title, "DATA (F:)", "second by letter");
  // Without letters: the serial's end.
  both = DescribeAll({stickDevice(L"0000AB12", {}), stickDevice(L"0000CD34", {})});
  same(both[0].info.title, "USB 드라이브 · 드라이브 문자 없음 (일련번호 …AB12)", "serial suffix");
  same(both[1].info.title, "USB 드라이브 · 드라이브 문자 없음 (일련번호 …CD34)", "serial suffix 2");
  // Serial endings that collide get longer.
  both = DescribeAll({stickDevice(L"11110001", {}), stickDevice(L"22220001", {})});
  require(both[0].info.title.find("…11110001") != std::string::npos && both[1].info.title.find("…22220001") != std::string::npos, "colliding endings get longer");
  require(both[0].info.title != both[1].info.title, "still different");
  // Without serials (Windows-made instance IDs): the hub port.
  auto a = stickDevice(L"5&2f&0&3", {}); a.port = 3;
  auto b = stickDevice(L"5&2f&0&4", {}); b.port = 4;
  both = DescribeAll({a, b});
  same(both[0].info.title, "USB 드라이브 · 드라이브 문자 없음 (포트 3)", "port"); same(both[1].info.title, "USB 드라이브 · 드라이브 문자 없음 (포트 4)", "port 2");
  // No serial and no port: a count, so they still differ.
  a.port = 0; b.port = 0;
  both = DescribeAll({a, b});
  require(both[0].info.title != both[1].info.title, "counted");
  // Two identical tokens.
  RawDevice t1; t1.deviceId = L"USB\\VID_1050&PID_0407"; t1.instanceId = L"7654321"; t1.descriptors = descriptors(0x1050, 0x407, 0, {{0x0b, 0, 0}}); t1.busReportedDesc = L"YubiKey";
  RawDevice t2 = t1; t2.instanceId = L"1234567";
  both = DescribeAll({t1, t2});
  same(both[0].info.title, "YubiKey (일련번호 …4321)", "token serial"); same(both[1].info.title, "YubiKey (일련번호 …4567)", "token serial 2");
  // A unique device is never decorated, and different kinds with one name are not duplicates.
  both = DescribeAll({t1, stickDevice(L"S", {volume(L"YubiKey", L"E")})});
  same(both[0].info.title, "YubiKey", "unique stays plain");
  // Hidden devices never count as duplicates.
  RawDevice hub; hub.deviceId = L"USB\\VID_05E3&PID_0610"; hub.instanceId = L"1"; hub.descriptors = descriptors(0x05e3, 0x610, 9, {{9, 0, 1}}); hub.busReportedDesc = L"USB2.0 Hub";
  RawDevice hub2 = hub; hub2.instanceId = L"2";
  both = DescribeAll({hub, hub2});
  same(both[0].info.title, "USB2.0 Hub", "hubs are not decorated");
}

void keys_and_arrival() {
  auto d = DescribeOne(stickDevice(L"ABCD1234", {volume(L"X", L"E")}));
  same(d.memoryKey, "0781:5567:abcd1234", "memory key has the serial");
  d = DescribeOne(stickDevice(L"5&1&0&1", {volume(L"X", L"E")}));
  same(d.memoryKey, "0781:5567", "no serial: the model");
  auto stick = stickDevice(L"S", {volume(L"X", L"E")}); stick.arrivalMs = 1790000000000ull;
  require(DescribeOne(stick).arrivalMs == 1790000000000ull, "arrival is carried");
}

// A fake volume layer: which volumes are open (in use), which refuse to open.
struct FakeVolumes final : VolumeApi {
  bool throwLock = false;
  std::set<std::wstring> inUse, denied, skipped; std::map<void*, std::wstring> open; std::vector<std::string> log; std::wstring blocker; intptr_t next = 1;
  void* Open(const std::wstring& path) override {
    if (denied.count(path)) { log.push_back("denied"); return nullptr; }
    void* handle = reinterpret_cast<void*>(next++); open[handle] = path; log.push_back("open"); return handle;
  }
  LockResult Lock(void* volume) override {
    if (throwLock) throw std::runtime_error("fake lock failure");
    const auto& path = open.at(volume);
    if (inUse.count(path)) { log.push_back("busy"); return LockResult::InUse; }
    if (skipped.count(path)) return LockResult::Skipped;
    log.push_back("lock"); return LockResult::Locked;
  }
  void Dismount(void*) override { log.push_back("dismount"); }
  void Close(void* volume) override { open.erase(volume); log.push_back("close"); }
  std::wstring Blocker(const std::wstring&) override { return blocker; }
  int count(const char* what) const { return int(std::count(log.begin(), log.end(), std::string(what))); }
};

void hand_over() {
  const std::vector<std::wstring> two = {L"\\\\?\\Volume{a}\\", L"\\\\?\\Volume{b}\\"};
  { FakeVolumes api; auto out = PrepareStorage(api, two, false);
    require(out.ready && out.checkedAll && api.count("lock") == 2 && api.count("dismount") == 0 && api.open.empty(), "A pre-offer check releases its locks and never dismounts, including after cancellation"); }
  { FakeVolumes api; api.throwLock = true; bool threw = false;
    try { (void)PrepareStorage(api, two, false); } catch (...) { threw = true; }
    require(threw && api.open.empty() && api.count("dismount") == 0, "A failed native prepare must not leave a volume handle locked or dismount it"); }
  { FakeVolumes api; auto out = PrepareStorage(api, two, false, [] { return true; });
    require(!out.ready && api.log.empty(), "Cancellation before native prepare must open no volumes"); }
  { FakeVolumes api; auto out = PrepareStorage(api, two, true, [&] { return api.count("lock") > 0; });
    require(!out.ready && api.count("lock") == 1 && api.count("dismount") == 0 && api.open.empty(), "Cancellation after a blocked lock returns must close it before another volume or dismount"); }
  { FakeVolumes api; auto out = PrepareStorage(api, two);
    require(out.ready && out.checkedAll && api.count("lock") == 2 && api.count("dismount") == 2 && api.open.empty(), "Idle volumes are locked, flushed, dismounted and released"); }
  { FakeVolumes api; auto out = PrepareStorage(api, {});
    require(out.ready && api.log.empty(), "Nothing to check, nothing done"); }
  { FakeVolumes api; api.inUse.insert(two[1]); api.blocker = L"Notepad.exe";
    auto out = PrepareStorage(api, two);
    require(!out.ready && out.blocker == "Notepad.exe", "A volume with open files stops the hand-over and names the program");
    require(api.count("dismount") == 0 && api.open.empty(), "Nothing is dismounted and every handle is released when one volume is busy"); }
  { FakeVolumes api; api.inUse.insert(two[0]);
    auto out = PrepareStorage(api, two);
    require(!out.ready && out.blocker.empty() && api.count("lock") == 0 && api.open.empty(), "Windows gave no program name: none is made up"); }
  { FakeVolumes api; api.denied.insert(two[0]);
    auto out = PrepareStorage(api, two);
    require(out.ready && !out.checkedAll && api.count("lock") == 1, "A volume that cannot be opened is skipped, the rest is checked"); }
  { FakeVolumes api; api.skipped.insert(two[1]);
    auto out = PrepareStorage(api, two);
    require(out.ready && !out.checkedAll && api.open.empty(), "A volume that cannot be locked for another reason does not block"); }
  { FakeVolumes api; api.denied.insert(two[0]); api.denied.insert(two[1]);
    auto out = PrepareStorage(api, two);
    require(out.ready && !out.checkedAll && api.count("dismount") == 0, "Nothing could be checked: proceeds, honestly flagged"); }
}
}  // namespace

int main() {
  try {
    vendors(); names(); storage(); tokens_and_others(); never_offered(); duplicates(); keys_and_arrival(); hand_over();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << "\n";
    return 1;
  }
  std::cout << "usb describe core: " << checks << " checks passed\n";
  return 0;
}
