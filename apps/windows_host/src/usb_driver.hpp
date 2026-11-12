#pragma once
// usb-v1 in the VM: the pinned usbip-win2 virtual host controller
// (docs/usb-redirection.md "Driver"). The Host never builds, edits, re-signs or
// test-signs it. It ships the vendor's INF/SYS/CAT files unchanged (Microsoft
// attestation-signed, BSD-2-Clause), checks every byte against the pins below
// and the Microsoft signature of each .sys and .cat, and installs it natively
// (SetupAPI/newdev, never PowerShell) only when the service needs it: an
// administrator granted `usbDevices` on this VM and a streaming session asked
// for it. Nothing here runs the upstream installer or any of its tools.
//
// The signed package cannot be edited, and its device object is reachable by
// every local user (SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R): any VM user
// could attach devices from their own USB/IP server, make the kernel open
// outbound TCP connections as SYSTEM and detach other users' devices. So after
// the install the Host (1) sets the devnode's security descriptor (applied on
// every start of the device), (2) sets the same descriptor on the running
// device object, and (3) proves it by opening the device interface with a
// restricted copy of its own token (Administrators and SYSTEM switched off): the
// open must fail with ACCESS_DENIED. Until that proof succeeds the controller
// is reported unusable (`usb_vhci_unprotected`) and nothing is attached.
#include "machine.hpp"
#include "usb_core.hpp"
#include <cfgmgr32.h>
#include <setupapi.h>
#include <newdev.h>
#include <softpub.h>
#include <mscat.h>
#include <wintrust.h>
#include <winioctl.h>
#include <initguid.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace host::usb {
// --- The pinned package ------------------------------------------------------
// usbip-win2 0.9.7.5-preview, as published by the Open Source Codesigning
// Initiative (OSSign/vadimgrn--usbip-win2-old, release 0.9.7.5-preview,
// 2025-12-03): INF DriverVer 12/01/2025,17.10.14.812 (usbip2_ude) and
// 17.10.14.779 (usbip2_filter). Verified: every .sys and .cat carries
// a valid Microsoft Windows Hardware Compatibility Publisher signature (issuer
// Microsoft Windows Third Party Component CA 2014, EKU 1.3.6.1.4.1.311.10.3.5.1,
// timestamped by the Microsoft Time-Stamp Service).
struct PackageFile { const wchar_t* name; const char* sha256; bool driver; bool catalog; };
inline const PackageFile* packageFiles() {
  static const PackageFile files[] = {
    {L"usbip2_ude.inf", "ce84dbdb634e620bdd9b1c2117155b4b04efa1a357b2247b98049989f98ee9b2", false, false},
    {L"usbip2_ude.sys", "434ab4426dd877f632b0afd4d5a365f2c41bdeb8fea1308fd0fe5042cf1de9ea", true, false},
    {L"usbip2_ude.cat", "3587f744766f3a55845e3dc9d990730ce6e779c0d67594b748f351d51eb501b3", false, true},
    {L"usbip2_filter.inf", "a3bd59c2d564dbf388ffd3c7a73fbb2fe2235a477fb1061d83a3fc69c25b5911", false, false},
    {L"usbip2_filter.sys", "b4e794626a067af8ba92ef608c049905ac21e312b18ed1da4338d4f155a3fb49", true, false},
    {L"usbip2_filter.cat", "efdb52949e258b79fc60cf522898b67f390856210421fa9b293d89c3ab93eacf", false, true},
    {nullptr, nullptr, false, false}};
  return files;
}
inline constexpr const wchar_t* signerName = L"Microsoft Windows Hardware Compatibility Publisher";
inline constexpr const wchar_t* vhciHardwareId = L"ROOT\\USBIP_WIN2\\UDE";
inline constexpr const wchar_t* ownerKey = L"SOFTWARE\\Multitap\\Host\\Usb";
// What SYSTEM and Administrators may do with the device; nobody else.
inline constexpr const wchar_t* vhciSecurity = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";

// --- vhci ioctl interface (usbip-win2 include/usbip/vhci.h, ABI of 0.9.7.x) ----
DEFINE_GUID(GUID_DEVINTERFACE_USBIP_HOST_CONTROLLER, 0xB4030C06, 0xDC5F, 0x4FCC, 0x87, 0xEB, 0xE5, 0x51, 0x5A, 0x09, 0x35, 0xC0);
namespace vhci {
struct Base { ULONG size; };
struct Location { int port; char busid[32]; char service[32]; char host[1025]; };
struct Properties { UINT32 devid; int speed; UINT16 vendor; UINT16 product; };
struct ImportedDevice : Location, Properties {};
constexpr DWORD code(int function) { return CTL_CODE(FILE_DEVICE_UNKNOWN, function, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA); }
inline constexpr DWORD pluginHardware = code(0x800), plugoutHardware = code(0x801), getImportedDevices = code(0x802), stopAttachAttempts = code(0x805);
struct PluginHardware : Base, Location {};
struct StopAttachAttempts : Base, Location { int count; };
struct PlugoutHardware : Base { int port; };
struct GetImportedDevices : Base { ImportedDevice devices[1]; };
}  // namespace vhci

// --- Everything the Host does to Windows, substitutable by tests ----------------
struct DriverAccess {
  std::function<bool()> packagePresent;                                    // the pinned files are installed with this Host
  std::function<bool()> packageVerified;                                   // hashes + Microsoft signatures + catalogs
  std::function<bool()> platformSupported;                                 // workstation Windows 10 1809+/11, not Server
  std::function<int()> controllerState;                                    // 0 none, 1 ours and started, 2 ours, not started, 3 another installation owns it, 4 unreadable
  std::function<DWORD(bool& restartRequired)> install;                     // installs/repairs our controller; Windows error
  std::function<bool()> protect;                                           // applies the restricted security descriptor
  std::function<bool()> protectionProven;                                  // a restricted token cannot open the interface; SYSTEM can
  std::function<int()> detachAll;                                          // plug-out of every port; returns how many were attached
  // State 3 only: adopts the controller when it is ours without a marker (see
  // ControllerOwner) and returns the owner afterwards. Unset: never adopts.
  std::function<int()> adopt;
};

// Whose the controller is (Host settings, diagnostics, adoption). An unmarked
// controller is ours only when the driver bound to it is byte-identical to the
// pinned package: an earlier Multitap install whose owner marker was lost. A
// different usbip-win2 version or another product is "foreign" and is never
// taken silently; Host settings can replace it after a confirmation.
enum class ControllerOwner : int { none = 0, ours = 1, oursStopped = 2, unmarked = 3, foreign = 4, multiple = 5, unknown = 6, duplicates = 7 };
inline const char* controllerOwnerName(ControllerOwner owner) {
  switch (owner) {
    case ControllerOwner::none: return "none";
    case ControllerOwner::ours: return "started";
    case ControllerOwner::oursStopped: return "stopped";
    case ControllerOwner::unmarked: return "unmarked";
    case ControllerOwner::foreign: return "foreign";
    case ControllerOwner::multiple: return "multiple";
    case ControllerOwner::duplicates: return "duplicates";
    default: return "unknown";
  }
}
// One registered controller node as the owner decision sees it. [pinned]: its
// bound package compared with the pins; empty when that could not be read.
struct ControllerView { bool marked = false, started = false; std::optional<bool> pinned; };
// unmarked: one node, our package, marker lost. duplicates: several nodes, every
// unmarked one our package (an earlier double registration): the Host keeps one.
// multiple: several nodes and at least one is (or may be) another product's.
inline ControllerOwner classifyControllers(const std::vector<ControllerView>& nodes) {
  if (nodes.empty()) return ControllerOwner::none;
  if (nodes.size() == 1) {
    const auto& node = nodes[0];
    if (node.marked) return node.started ? ControllerOwner::ours : ControllerOwner::oursStopped;
    if (!node.pinned) return ControllerOwner::unknown;
    return *node.pinned ? ControllerOwner::unmarked : ControllerOwner::foreign;
  }
  for (const auto& node : nodes) if (!node.marked && node.pinned != std::optional<bool>(true)) return ControllerOwner::multiple;
  return ControllerOwner::duplicates;
}

enum class Prepare { ready, packageMissing, platformUnsupported, signatureInvalid, otherInstallation, installFailed, restartRequired, protectionFailed };
inline const char* prepareReason(Prepare value) {
  switch (value) {
    case Prepare::ready: return "";
    case Prepare::packageMissing: case Prepare::platformUnsupported: return "usb_driver_missing";
    case Prepare::signatureInvalid: case Prepare::installFailed: return "usb_driver_install_failed";
    case Prepare::otherInstallation: return "usb_vhci_unavailable";
    case Prepare::restartRequired: return "usb_restart_required";
    default: return "usb_vhci_unprotected";
  }
}

// The decision sequence of the service (tested with a substituted Windows).
inline Prepare prepareController(const DriverAccess& access, const std::atomic<bool>& stopping) {
  if (!access.platformSupported()) { recordUsbEvent(UsbStage::driverMissing, false, 1); return Prepare::platformUnsupported; }
  if (!access.packagePresent()) { recordUsbEvent(UsbStage::driverMissing, false, 2); return Prepare::packageMissing; }
  if (!access.packageVerified()) { recordUsbEvent(UsbStage::driverInstallFailed, false, 1); return Prepare::signatureInvalid; }
  int state = access.controllerState();
  // An incomplete device list is not "no controller": registering another node on
  // that answer is how a VM ended up with two and a marker on only one.
  if (state == 4) { recordUsbEvent(UsbStage::driverInstallFailed, false, 4); return Prepare::installFailed; }
  if (state == 3) {
    // Our own package that only lost its marker is taken back without a user step.
    const int owner = access.adopt ? access.adopt() : int(ControllerOwner::unknown);
    if (owner == int(ControllerOwner::ours) || owner == int(ControllerOwner::oursStopped)) { recordUsbEvent(UsbStage::controllerAdopted, true); state = access.controllerState(); }
    if (state == 3) { recordUsbEvent(UsbStage::vhciUnavailable, false, 3, uint32_t(owner)); return Prepare::otherInstallation; }
  }
  if (state != 1) {
    if (stopping) return Prepare::installFailed;
    bool restart = false;
    const DWORD error = access.install(restart);
    if (error) { recordUsbEvent(UsbStage::driverInstallFailed, false, error); return Prepare::installFailed; }
    state = access.controllerState();
    if (state != 1) { recordUsbEvent(restart ? UsbStage::restartRequired : UsbStage::driverInstallFailed, false, 2); return restart ? Prepare::restartRequired : Prepare::installFailed; }
    recordUsbEvent(UsbStage::driverInstalled, true);
  }
  if (stopping) return Prepare::installFailed;
  // Protection is applied (again) on every start of a session: the running device
  // object's descriptor is not kept across a restart of the device.
  if (!access.protect() || !access.protectionProven()) { recordUsbEvent(UsbStage::protectionFailed, false); return Prepare::protectionFailed; }
  // A crashed run may have left ports attached: no session starts with strangers.
  if (const int left = access.detachAll(); left > 0) recordUsbEvent(UsbStage::leftovers, true, 0, uint32_t(left));
  return Prepare::ready;
}

// --- Windows implementation -------------------------------------------------------
inline std::wstring packageDirectory() {
  std::wstring path = executablePath();
  path.resize(path.find_last_of(L'\\'));
  return path + L"\\usb-driver";
}
inline std::string hexOf(const uint8_t* data, size_t size) {
  const char digits[] = "0123456789abcdef"; std::string out;
  for (size_t i = 0; i < size; ++i) { out += digits[data[i] >> 4]; out += digits[data[i] & 15]; }
  return out;
}
inline std::string fileSha256(const std::wstring& path) {
  WinHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (file.value == INVALID_HANDLE_VALUE) return {};
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file.value, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nFileSizeHigh || info.nFileSizeLow > 8 * 1024 * 1024) return {};
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; std::string out;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
  DWORD size = 0, got = 0; std::vector<uint8_t> object;
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &got, 0) >= 0) {
    object.resize(size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) >= 0) {
      std::vector<uint8_t> buffer(65536); DWORD read = 0; bool ok = true;
      while (ReadFile(file.value, buffer.data(), DWORD(buffer.size()), &read, nullptr) && read) if (BCryptHashData(hash, buffer.data(), read, 0) < 0) { ok = false; break; }
      std::array<uint8_t, 32> digest{};
      if (ok && BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) >= 0) out = hexOf(digest.data(), digest.size());
      BCryptDestroyHash(hash);
    }
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return out;
}
// The signer's simple display name of a verified WinVerifyTrust state.
inline std::wstring trustSigner(HANDLE state) {
  std::wstring name;
  if (auto* data = WTHelperProvDataFromStateData(state)) if (auto* signer = WTHelperGetProvSignerFromChain(data, 0, FALSE, 0)) if (auto* cert = WTHelperGetProvCertFromChain(signer, 0)) {
    wchar_t text[256]{}; if (CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, text, 256)) name = text;
  }
  return name;
}
inline bool verifyEmbeddedSignature(const std::wstring& path) {
  WINTRUST_FILE_INFO file{sizeof(file), path.c_str(), nullptr, nullptr};
  WINTRUST_DATA data{}; data.cbStruct = sizeof(data); data.dwUIChoice = WTD_UI_NONE; data.fdwRevocationChecks = WTD_REVOKE_NONE; data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file; data.dwStateAction = WTD_STATEACTION_VERIFY; data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG result = WinVerifyTrust(nullptr, &action, &data);
  const bool ok = result == ERROR_SUCCESS && trustSigner(data.hWVTStateData) == signerName;
  data.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &action, &data);
  return ok;
}
// [member] must be listed in the catalog [catalog], and the catalog itself must
// carry a valid Microsoft signature.
inline bool verifyCatalogMember(const std::wstring& catalog, const std::wstring& member) {
  if (!verifyEmbeddedSignature(catalog)) return false;
  HCATADMIN admin = nullptr;
  if (!CryptCATAdminAcquireContext2(&admin, nullptr, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) return false;
  struct Release { HCATADMIN& value; ~Release() { CryptCATAdminReleaseContext(value, 0); } } release{admin};
  WinHandle file(CreateFileW(member.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (file.value == INVALID_HANDLE_VALUE) return false;
  DWORD size = 0;
  if (!CryptCATAdminCalcHashFromFileHandle2(admin, file.value, &size, nullptr, 0) || !size || size > 64) return false;
  std::vector<uint8_t> hash(size);
  if (!CryptCATAdminCalcHashFromFileHandle2(admin, file.value, &size, hash.data(), 0)) return false;
  std::wstring tag; for (const auto value : hash) { wchar_t part[3]; swprintf_s(part, L"%02X", value); tag += part; }
  WINTRUST_CATALOG_INFO info{}; info.cbStruct = sizeof(info); info.pcwszCatalogFilePath = catalog.c_str(); info.pcwszMemberTag = tag.c_str();
  info.pcwszMemberFilePath = member.c_str(); info.hMemberFile = file.value; info.pbCalculatedFileHash = hash.data(); info.cbCalculatedFileHash = size; info.hCatAdmin = admin;
  WINTRUST_DATA data{}; data.cbStruct = sizeof(data); data.dwUIChoice = WTD_UI_NONE; data.fdwRevocationChecks = WTD_REVOKE_NONE; data.dwUnionChoice = WTD_CHOICE_CATALOG;
  data.pCatalog = &info; data.dwStateAction = WTD_STATEACTION_VERIFY; data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG result = WinVerifyTrust(nullptr, &action, &data);
  data.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &action, &data);
  return result == ERROR_SUCCESS;
}
// Hashes of every pinned file, then the Microsoft signature of each .sys and the
// catalog membership of each .sys and .inf.
inline bool verifyPackage(const std::wstring& directory) {
  try {
    for (const PackageFile* file = packageFiles(); file->name; ++file) if (fileSha256(directory + L"\\" + file->name) != file->sha256) return false;
    for (const wchar_t* base : {L"usbip2_ude", L"usbip2_filter"}) {
      const std::wstring stem = directory + L"\\" + base;
      if (!verifyEmbeddedSignature(stem + L".sys") || !verifyCatalogMember(stem + L".cat", stem + L".sys") || !verifyCatalogMember(stem + L".cat", stem + L".inf")) return false;
    }
    return true;
  } catch (...) { return false; }
}
inline bool platformSupported() {
  OSVERSIONINFOEXW info{}; info.dwOSVersionInfoSize = sizeof(info);
  using Rtl = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  const auto rtl = reinterpret_cast<Rtl>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
  if (!rtl || rtl(&info) != 0) return false;
  // Workstation Windows 10 1809 (17763) or later: Windows Server loads only HLK-signed device drivers.
  return info.dwMajorVersion >= 10 && info.dwBuildNumber >= 17763 && info.wProductType == VER_NT_WORKSTATION;
}

inline std::wstring vhciInterfacePath() {
  ULONG length = 0;
  GUID guid = GUID_DEVINTERFACE_USBIP_HOST_CONTROLLER;
  if (CM_Get_Device_Interface_List_SizeW(&length, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length < 2 || length > 4096) return {};
  std::wstring list(length, L'\0');
  if (CM_Get_Device_Interface_ListW(&guid, nullptr, list.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return {};
  const std::wstring first(list.c_str());
  return list.c_str()[first.size() + 1] == L'\0' ? first : std::wstring();   // exactly one controller
}
inline WinHandle openVhci(DWORD access = GENERIC_READ | GENERIC_WRITE) {
  const auto path = vhciInterfacePath();
  if (path.empty()) return WinHandle(INVALID_HANDLE_VALUE);
  return WinHandle(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}
inline bool valid(const WinHandle& handle) { return handle.value && handle.value != INVALID_HANDLE_VALUE; }

// Plug-in: the kernel connects to host:service (our loopback server), imports
// [busId] and creates the port. Blocks until then. Returns the port, or 0.
inline int vhciAttach(const std::string& host, uint16_t port, const std::string& busId) {
  auto handle = openVhci(); if (!valid(handle)) return 0;
  vhci::PluginHardware request{}; request.size = sizeof(request);
  if (strcpy_s(request.busid, busId.c_str()) || strcpy_s(request.host, host.c_str()) || strcpy_s(request.service, std::to_string(port).c_str())) return 0;
  constexpr DWORD outLength = DWORD(offsetof(vhci::PluginHardware, port) + sizeof(int));
  DWORD returned = 0;
  if (!DeviceIoControl(handle.value, vhci::pluginHardware, &request, sizeof(request), &request, outLength, &returned, nullptr) || returned != outLength || request.port <= 0) return 0;
  return request.port;
}
inline bool vhciDetach(int port) {
  auto handle = openVhci(); if (!valid(handle)) return false;
  vhci::PlugoutHardware request{}; request.size = sizeof(request); request.port = port;
  DWORD returned = 0;
  return DeviceIoControl(handle.value, vhci::plugoutHardware, &request, sizeof(request), nullptr, 0, &returned, nullptr) != FALSE;
}
inline int vhciCancelAttach() {
  auto handle = openVhci(); if (!valid(handle)) return -1;
  vhci::StopAttachAttempts request{}; request.size = sizeof(request);
  DWORD returned = 0;
  return DeviceIoControl(handle.value, vhci::stopAttachAttempts, &request, sizeof(request), &request, sizeof(request), &returned, nullptr) ? request.count : -1;
}
// Ports currently plugged in (0 when the controller is absent or unreadable).
inline std::vector<int> vhciPorts() {
  std::vector<int> ports;
  auto handle = openVhci(); if (!valid(handle)) return ports;
  for (ULONG count = 4; count <= 128; count <<= 1) {
    std::vector<uint8_t> buffer(offsetof(vhci::GetImportedDevices, devices) + count * sizeof(vhci::ImportedDevice));
    auto* request = reinterpret_cast<vhci::GetImportedDevices*>(buffer.data()); request->size = sizeof(*request);
    DWORD returned = 0;
    if (DeviceIoControl(handle.value, vhci::getImportedDevices, request, sizeof(request->size), buffer.data(), DWORD(buffer.size()), &returned, nullptr)) {
      const size_t offset = offsetof(vhci::GetImportedDevices, devices);
      if (returned < offset || (returned - offset) % sizeof(vhci::ImportedDevice)) return {};
      for (size_t i = 0; i < (returned - offset) / sizeof(vhci::ImportedDevice); ++i) if (request->devices[i].port > 0) ports.push_back(request->devices[i].port);
      return ports;
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
  }
  return ports;
}
inline int vhciDetachAll() {
  const auto ports = vhciPorts();
  for (const int port : ports) vhciDetach(port);
  return int(ports.size());
}

// --- device node: find, install, protect ---------------------------------------------
class DeviceSet {
 public:
  explicit DeviceSet(HDEVINFO value = INVALID_HANDLE_VALUE) : value_(value) {}
  DeviceSet(const DeviceSet&) = delete; DeviceSet& operator=(const DeviceSet&) = delete;
  ~DeviceSet() { if (value_ != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value_); }
  HDEVINFO get() const { return value_; }
  bool valid() const { return value_ != INVALID_HANDLE_VALUE; }
 private:
  HDEVINFO value_;
};
inline bool sameHardwareId(HDEVINFO set, SP_DEVINFO_DATA& device) {
  DWORD type = 0, needed = 0; wchar_t buffer[512]{};
  if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, &type, reinterpret_cast<BYTE*>(buffer), sizeof(buffer) - 4, &needed) || type != REG_MULTI_SZ) return false;
  return _wcsicmp(buffer, vhciHardwareId) == 0;
}
// The controller device nodes with our hardware ID (instance IDs, registered
// phantoms included). Empty optional: Windows did not give a complete list.
// That is never "no controller": an unreadable or cut-short list read as
// none would make the service register a second node and move the owner
// marker to it, and from then on every start would see "another installation".
inline std::optional<std::vector<std::wstring>> readControllerInstances() {
  std::vector<std::wstring> out;
  DeviceSet set(SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES));
  if (!set.valid()) return std::nullopt;
  for (DWORD index = 0; index < 65536; ++index) {
    SP_DEVINFO_DATA device{}; device.cbSize = sizeof(device);
    if (!SetupDiEnumDeviceInfo(set.get(), index, &device)) return GetLastError() == ERROR_NO_MORE_ITEMS ? std::optional(out) : std::nullopt;
    if (!sameHardwareId(set.get(), device)) continue;
    wchar_t id[512]{}; DWORD needed = 0;
    if (!SetupDiGetDeviceInstanceIdW(set.get(), &device, id, 512, &needed)) return std::nullopt;
    out.emplace_back(id);
  }
  return std::nullopt;
}
// For callers that only act on nodes they found (never on "none").
inline std::vector<std::wstring> controllerInstances() { return readControllerInstances().value_or(std::vector<std::wstring>{}); }
inline std::wstring ownedInstance() {
  HKEY key = nullptr; wchar_t value[512]{}; DWORD size = sizeof(value) - 2, type = 0;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ownerKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return {};
  const bool ok = RegQueryValueExW(key, L"Instance", nullptr, &type, reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS && type == REG_SZ;
  RegCloseKey(key);
  return ok ? std::wstring(value) : std::wstring();
}
inline bool rememberOwnedInstance(const std::wstring& instance) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, ownerKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
  const bool ok = RegSetValueExW(key, L"Instance", 0, REG_SZ, reinterpret_cast<const BYTE*>(instance.c_str()), DWORD((instance.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
  RegCloseKey(key); return ok;
}
inline int controllerState() {
  const auto listed = readControllerInstances();
  if (!listed) return 4;
  const auto& instances = *listed;
  if (instances.empty()) return 0;
  const auto owned = ownedInstance();
  if (instances.size() != 1 || owned.empty() || _wcsicmp(instances[0].c_str(), owned.c_str()) != 0) return 3;
  DEVINST node = 0; ULONG flags = 0, problem = 0; std::wstring id = instances[0];
  if (CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS || CM_Get_DevNode_Status(&flags, &problem, node, 0) != CR_SUCCESS) return 2;
  return (flags & DN_STARTED) && !problem && !vhciInterfacePath().empty() ? 1 : 2;
}
// Whether the driver package bound to [instance] is byte for byte the pinned
// controller package (INF, SYS, CAT in its driver store folder); empty when the
// binding or the files could not be read.
inline std::optional<bool> boundPackagePinned(const std::wstring& instance) {
  DeviceSet set(SetupDiCreateDeviceInfoList(nullptr, nullptr)); if (!set.valid()) return std::nullopt;
  SP_DEVINFO_DATA device{}; device.cbSize = sizeof(device);
  if (!SetupDiOpenDeviceInfoW(set.get(), instance.c_str(), nullptr, 0, &device) || !sameHardwareId(set.get(), device)) return std::nullopt;
  const HKEY key = SetupDiOpenDevRegKey(set.get(), &device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
  if (key == INVALID_HANDLE_VALUE) return std::nullopt;
  wchar_t inf[MAX_PATH]{}; DWORD bytes = sizeof(inf) - sizeof(wchar_t), type = 0;
  const bool read = RegQueryValueExW(key, L"InfPath", nullptr, &type, reinterpret_cast<BYTE*>(inf), &bytes) == ERROR_SUCCESS && type == REG_SZ;
  RegCloseKey(key); if (!read || !inf[0]) return std::nullopt;
  wchar_t stored[MAX_PATH]{};
  if (!SetupGetInfDriverStoreLocationW(inf, nullptr, nullptr, stored, MAX_PATH, nullptr)) return std::nullopt;
  std::wstring folder = stored; const auto slash = folder.find_last_of(L'\\'); if (slash == std::wstring::npos) return std::nullopt;
  if (_wcsicmp(folder.c_str() + slash + 1, L"usbip2_ude.inf") != 0) return false;   // another product's INF
  folder.resize(slash);
  for (const PackageFile* file = packageFiles(); file->name; ++file) {
    if (_wcsnicmp(file->name, L"usbip2_ude.", 11) != 0) continue;
    const auto digest = fileSha256(folder + L"\\" + file->name);
    if (digest.empty()) return std::nullopt;
    if (digest != file->sha256) return false;
  }
  return true;
}
inline bool controllerPresent(const std::wstring& instance) {
  DEVINST node = 0; std::wstring id = instance;
  return CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
}
inline ControllerOwner controllerOwner() {
  try {
    const auto listed = readControllerInstances();
    if (!listed) return ControllerOwner::unknown;
    const auto owned = ownedInstance(); const int state = controllerState();
    std::vector<ControllerView> nodes;
    for (const auto& instance : *listed) {
      ControllerView view; view.marked = !owned.empty() && _wcsicmp(instance.c_str(), owned.c_str()) == 0;
      view.started = view.marked && state == 1;
      if (!view.marked) view.pinned = boundPackagePinned(instance);
      nodes.push_back(view);
    }
    return classifyControllers(nodes);
  } catch (...) { return ControllerOwner::unknown; }
}
// Our own pinned package that lost its marker (one node), or registered twice
// (several nodes, every unmarked one ours): keep the marked node, else the first
// present one, remove the others after proving each again, and write the marker.
// Protection is applied by the session start that follows, as for any start.
inline int adoptUnmarkedController() {
  const auto owner = controllerOwner();
  if (owner != ControllerOwner::unmarked && owner != ControllerOwner::duplicates) return int(owner);
  const auto listed = readControllerInstances(); if (!listed || listed->empty()) return int(owner);
  const auto owned = ownedInstance();
  std::wstring keep;
  for (const auto& instance : *listed) if (!owned.empty() && _wcsicmp(instance.c_str(), owned.c_str()) == 0) keep = instance;
  if (keep.empty()) for (const auto& instance : *listed) if (controllerPresent(instance)) { keep = instance; break; }
  if (keep.empty()) keep = listed->front();
  for (const auto& instance : *listed) {
    if (_wcsicmp(instance.c_str(), keep.c_str()) == 0) continue;
    if (boundPackagePinned(instance) != std::optional<bool>(true)) return int(controllerOwner());
    DeviceSet set(SetupDiCreateDeviceInfoList(nullptr, nullptr)); SP_DEVINFO_DATA device{}; device.cbSize = sizeof(device);
    if (!set.valid() || !SetupDiOpenDeviceInfoW(set.get(), instance.c_str(), nullptr, 0, &device) || !sameHardwareId(set.get(), device) || !SetupDiCallClassInstaller(DIF_REMOVE, set.get(), &device)) return int(controllerOwner());
  }
  if (!rememberOwnedInstance(keep)) return int(owner);
  return int(controllerOwner());
}
// Host settings "Multitap용으로 바꾸기", after the window's confirmation: another
// product's controller node(s) go (its attached devices detach and its USB sharing
// stops), ours is installed from this Host's verified package. Never automatic.
// Its driver package stays in the driver store; nothing else of it is touched.
struct ReplaceAccess {
  std::function<bool()> platformSupported, packagePresent, packageVerified;
  std::function<ControllerOwner()> owner;
  std::function<int(bool& restartRequired)> removeOthers;   // removed nodes, or -1
  std::function<DWORD(bool& restartRequired)> install;
  std::function<int()> controllerState;
};
struct ReplaceResult { const char* code = ""; bool restart = false; int removed = 0; };
inline ReplaceResult replaceController(const ReplaceAccess& access) {
  ReplaceResult out;
  if (!access.platformSupported()) { out.code = "usb_platform_unsupported"; return out; }
  if (!access.packagePresent()) { out.code = "usb_driver_missing"; return out; }
  if (!access.packageVerified()) { out.code = "usb_driver_install_failed"; return out; }
  // Ours, our unmarked package and our own duplicates are taken back automatically.
  const auto owner = access.owner();
  if (owner != ControllerOwner::foreign && owner != ControllerOwner::multiple && owner != ControllerOwner::unknown) { out.code = "usb_controller_not_foreign"; return out; }
  bool removeRestart = false;
  out.removed = access.removeOthers(removeRestart);
  if (out.removed < 0) { out.code = "usb_replace_remove_failed"; return out; }
  if (removeRestart) { out.code = "usb_restart_required"; out.restart = true; return out; }
  bool installRestart = false;
  if (access.install(installRestart)) { out.code = "usb_driver_install_failed"; return out; }
  if (access.controllerState() == 1) return out;
  out.restart = installRestart; out.code = installRestart ? "usb_restart_required" : "usb_driver_install_failed";
  return out;
}
// Every controller node except a marked one of ours; -1 when one could not go.
inline int removeOtherControllers(bool& restartRequired) {
  restartRequired = false; int removed = 0;
  const auto owned = ownedInstance(); const auto listed = readControllerInstances(); if (!listed) return -1;
  for (const auto& instance : *listed) {
    if (!owned.empty() && _wcsicmp(instance.c_str(), owned.c_str()) == 0) continue;
    DeviceSet set(SetupDiCreateDeviceInfoList(nullptr, nullptr)); if (!set.valid()) return -1;
    SP_DEVINFO_DATA device{}; device.cbSize = sizeof(device);
    if (!SetupDiOpenDeviceInfoW(set.get(), instance.c_str(), nullptr, 0, &device) || !sameHardwareId(set.get(), device)) return -1;
    if (!SetupDiCallClassInstaller(DIF_REMOVE, set.get(), &device)) return -1;
    DEVINST gone = 0; std::wstring id = instance;
    if (CM_Locate_DevNodeW(&gone, id.data(), CM_LOCATE_DEVNODE_PHANTOM) != CR_NO_SUCH_DEVNODE) restartRequired = true;
    ++removed;
  }
  return removed;
}
// The values the vendor's INF turns on for debugging (verbose tracing, WDF
// verifier) are registry values, not signed content: switch them off. They are
// read when the driver starts.
inline void quietDriverParameters() {
  const struct { const wchar_t* key; const wchar_t* name; } values[] = {
    {L"SYSTEM\\CurrentControlSet\\Services\\usbip2_ude\\Parameters", L"VerboseOn"},
    {L"SYSTEM\\CurrentControlSet\\Services\\usbip2_ude\\Parameters\\Wdf", L"VerboseOn"},
    {L"SYSTEM\\CurrentControlSet\\Services\\usbip2_ude\\Parameters\\Wdf", L"VerifierOn"},
    {L"SYSTEM\\CurrentControlSet\\Services\\usbip2_filter\\Parameters", L"VerboseOn"}};
  for (const auto& item : values) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, item.key, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) continue;
    const DWORD zero = 0; RegSetValueExW(key, item.name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero)); RegCloseKey(key);
  }
}
inline bool restartController(const std::wstring& instance) {
  DeviceSet set(SetupDiCreateDeviceInfoList(nullptr, nullptr)); if (!set.valid()) return false;
  SP_DEVINFO_DATA device{}; device.cbSize = sizeof(device);
  if (!SetupDiOpenDeviceInfoW(set.get(), instance.c_str(), nullptr, 0, &device) || !sameHardwareId(set.get(), device)) return false;
  SP_PROPCHANGE_PARAMS params{}; params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER); params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
  params.StateChange = DICS_PROPCHANGE; params.Scope = DICS_FLAG_GLOBAL; params.HwProfile = 0;
  return SetupDiSetClassInstallParamsW(set.get(), &device, &params.ClassInstallHeader, sizeof(params)) && SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set.get(), &device);
}
// Stages the filter (an extension INF for the controller's root hub), creates
// our root-enumerated controller and binds the signed driver to it.
inline DWORD installController(const std::wstring& directory, bool& restartRequired) {
  restartRequired = false;
  const std::wstring filter = directory + L"\\usbip2_filter.inf", controller = directory + L"\\usbip2_ude.inf";
  BOOL reboot = FALSE;
  if (!DiInstallDriverW(nullptr, filter.c_str(), 0, &reboot)) return GetLastError();
  restartRequired = restartRequired || reboot;
  GUID guid{}; wchar_t className[MAX_CLASS_NAME_LEN]{}; DWORD needed = 0;
  if (!SetupDiGetINFClassW(controller.c_str(), &guid, className, MAX_CLASS_NAME_LEN, &needed)) return GetLastError();
  // Only a complete list may decide that a new node is needed.
  const auto listed = readControllerInstances(); if (!listed) return ERROR_GEN_FAILURE;
  const auto& existing = *listed;
  DeviceSet set(SetupDiCreateDeviceInfoList(&guid, nullptr)); if (!set.valid()) return GetLastError();
  SP_DEVINFO_DATA device{}; device.cbSize = sizeof(device);
  bool created = false;
  if (existing.empty()) {
    if (!SetupDiCreateDeviceInfoW(set.get(), className, &guid, L"USBip 3.X Emulated Host Controller", nullptr, DICD_GENERATE_ID, &device)) return GetLastError();
    created = true;
    static const wchar_t hardware[] = L"ROOT\\USBIP_WIN2\\UDE\0";
    if (!SetupDiSetDeviceRegistryPropertyW(set.get(), &device, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(hardware), sizeof(hardware))) { const DWORD error = GetLastError(); SetupDiCallClassInstaller(DIF_REMOVE, set.get(), &device); return error; }
  } else if (!SetupDiOpenDeviceInfoW(set.get(), existing[0].c_str(), nullptr, 0, &device)) return GetLastError();
  SP_DEVINSTALL_PARAMS_W parameters{}; parameters.cbSize = sizeof(parameters);
  const auto undo = [&](DWORD error) { if (created) SetupDiCallClassInstaller(DIF_REMOVE, set.get(), &device); return error; };
  if (!SetupDiGetDeviceInstallParamsW(set.get(), &device, &parameters)) return undo(GetLastError());
  parameters.Flags |= DI_ENUMSINGLEINF;
  if (wcscpy_s(parameters.DriverPath, controller.c_str()) || !SetupDiSetDeviceInstallParamsW(set.get(), &device, &parameters)) return undo(ERROR_INVALID_PARAMETER);
  if (!SetupDiBuildDriverInfoList(set.get(), &device, SPDIT_COMPATDRIVER)) return undo(GetLastError());
  SP_DRVINFO_DATA_W driver{}; driver.cbSize = sizeof(driver);
  if (!SetupDiEnumDriverInfoW(set.get(), &device, SPDIT_COMPATDRIVER, 0, &driver)) return undo(GetLastError());
  if (created && !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set.get(), &device)) return undo(GetLastError());
  wchar_t id[512]{};
  if (!SetupDiGetDeviceInstanceIdW(set.get(), &device, id, 512, &needed)) return undo(GetLastError());
  if (!rememberOwnedInstance(id)) return undo(ERROR_ACCESS_DENIED);   // never a controller we could not mark as ours
  reboot = FALSE;
  if (!DiInstallDevice(nullptr, set.get(), &device, &driver, 0, &reboot)) return GetLastError();
  restartRequired = restartRequired || reboot;
  quietDriverParameters();
  return ERROR_SUCCESS;
}

// Security: SYSTEM and Administrators only.
inline bool securityBlob(const wchar_t* sddl, PSECURITY_DESCRIPTOR& descriptor) {
  return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr) != FALSE;
}
inline bool protectController() {
  const auto instances = controllerInstances();
  if (instances.size() != 1) return false;
  // (1) The devnode's descriptor, applied whenever the device starts.
  {
    DeviceSet set(SetupDiCreateDeviceInfoList(nullptr, nullptr)); if (!set.valid()) return false;
    SP_DEVINFO_DATA device{}; device.cbSize = sizeof(device);
    if (!SetupDiOpenDeviceInfoW(set.get(), instances[0].c_str(), nullptr, 0, &device) || !sameHardwareId(set.get(), device)) return false;
    const std::wstring sddl = vhciSecurity;
    if (!SetupDiSetDeviceRegistryPropertyW(set.get(), &device, SPDRP_SECURITY_SDS, reinterpret_cast<const BYTE*>(sddl.c_str()), DWORD((sddl.size() + 1) * sizeof(wchar_t)))) return false;
  }
  // (2) The running device object, now.
  auto handle = openVhci(READ_CONTROL | WRITE_DAC | GENERIC_READ);
  if (!valid(handle)) return false;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!securityBlob(vhciSecurity, descriptor)) return false;
  struct Free { PSECURITY_DESCRIPTOR value; ~Free() { LocalFree(value); } } free{descriptor};
  BOOL present = FALSE, defaulted = FALSE; PACL dacl = nullptr;
  if (!GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present || !dacl) return false;
  return SetSecurityInfo(handle.value, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
}
// SYSTEM can open the interface; a copy of its token without SYSTEM,
// Administrators and the other powerful groups cannot (ERROR_ACCESS_DENIED).
inline bool protectionProven() {
  { auto system = openVhci(); if (!valid(system)) return false; }
  HANDLE raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &raw)) return false;
  WinHandle token(raw);
  // Every group of the token except Everyone / Authenticated Users / Users: a stand-in for a standard user.
  DWORD size = 0; GetTokenInformation(token.value, TokenGroups, nullptr, 0, &size);
  if (!size || size > 65536) return false;
  std::vector<uint8_t> groupBuffer(size);
  if (!GetTokenInformation(token.value, TokenGroups, groupBuffer.data(), size, &size)) return false;
  const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(groupBuffer.data());
  std::vector<SID_AND_ATTRIBUTES> disable;
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    PSID sid = groups->Groups[i].Sid;
    if (IsWellKnownSid(sid, WinWorldSid) || IsWellKnownSid(sid, WinAuthenticatedUserSid) || IsWellKnownSid(sid, WinBuiltinUsersSid)) continue;
    if (groups->Groups[i].Attributes & SE_GROUP_INTEGRITY) continue;
    disable.push_back({sid, 0});
  }
  // SYSTEM is the token's user: deny-only through the restricted copy as well.
  std::vector<uint8_t> userBuffer(256); DWORD userSize = DWORD(userBuffer.size());
  if (!GetTokenInformation(token.value, TokenUser, userBuffer.data(), userSize, &userSize)) { userBuffer.resize(userSize); if (!GetTokenInformation(token.value, TokenUser, userBuffer.data(), userSize, &userSize)) return false; }
  disable.push_back({reinterpret_cast<TOKEN_USER*>(userBuffer.data())->User.Sid, 0});
  HANDLE restrictedRaw = nullptr;
  if (!CreateRestrictedToken(token.value, DISABLE_MAX_PRIVILEGE, DWORD(disable.size()), disable.data(), 0, nullptr, 0, nullptr, &restrictedRaw)) return false;
  WinHandle restricted(restrictedRaw);
  HANDLE impersonationRaw = nullptr;
  if (!DuplicateTokenEx(restricted.value, TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr, SecurityImpersonation, TokenImpersonation, &impersonationRaw)) return false;
  WinHandle impersonation(impersonationRaw);
  if (!SetThreadToken(nullptr, impersonation.value)) return false;
  const auto path = vhciInterfacePath();
  const HANDLE attempt = path.empty() ? INVALID_HANDLE_VALUE : CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  const DWORD error = GetLastError();
  SetThreadToken(nullptr, nullptr);
  if (attempt != INVALID_HANDLE_VALUE) { CloseHandle(attempt); return false; }
  return error == ERROR_ACCESS_DENIED;
}

// The VM's own device node of an attached device (matched by vendor and product, the parent node only):
// started, present but not started (with Windows' problem code), or not in the tree. Read-only.
inline UsbVmState vmDeviceState(uint16_t vendor, uint16_t product) {
  UsbVmState out;
  wchar_t prefix[48]{};
  swprintf_s(prefix, L"USB\\VID_%04X&PID_%04X\\", vendor, product);
  ULONG size = 0;
  if (CM_Get_Device_ID_List_SizeW(&size, L"USB", CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS || size < 2 || size > 1048576) return out;
  std::vector<wchar_t> list(size);
  if (CM_Get_Device_ID_ListW(L"USB", list.data(), size, CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS) return out;
  const size_t prefixLength = wcslen(prefix);
  for (wchar_t* id = list.data(); *id; id += wcslen(id) + 1) {
    if (_wcsnicmp(id, prefix, prefixLength) != 0) continue;
    DEVINST node = 0; ULONG status = 0, problem = 0;
    if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS || CM_Get_DevNode_Status(&status, &problem, node, 0) != CR_SUCCESS) continue;
    if (problem == 0 && (status & DN_STARTED)) return {2, 0};
    out.state = 1; out.problem = problem;
  }
  return out;
}
inline DriverAccess nativeDriverAccess() {
  DriverAccess access;
  access.platformSupported = platformSupported;
  access.packagePresent = [] { for (const PackageFile* file = packageFiles(); file->name; ++file) if (GetFileAttributesW((packageDirectory() + L"\\" + file->name).c_str()) == INVALID_FILE_ATTRIBUTES) return false; return true; };
  access.packageVerified = [] { return verifyPackage(packageDirectory()); };
  access.controllerState = controllerState;
  access.install = [](bool& restart) { return installController(packageDirectory(), restart); };
  access.protect = protectController;
  access.protectionProven = protectionProven;
  access.detachAll = vhciDetachAll;
  access.adopt = adoptUnmarkedController;
  return access;
}
inline ReplaceAccess nativeReplaceAccess() {
  const auto driver = nativeDriverAccess();
  ReplaceAccess access;
  access.platformSupported = driver.platformSupported; access.packagePresent = driver.packagePresent; access.packageVerified = driver.packageVerified;
  access.owner = controllerOwner; access.removeOthers = removeOtherControllers; access.install = driver.install; access.controllerState = controllerState;
  return access;
}
// Advertised only when this Host carries the package on a supported Windows.
inline bool usbRedirectionAvailable() {
  static std::mutex mutex; static ULONGLONG checkedAt = 0; static bool available = false;
  std::lock_guard lock(mutex);
  const auto now = GetTickCount64();
  if (checkedAt && now - checkedAt < 60000) return available;
  checkedAt = now;
  const auto access = nativeDriverAccess();
  available = access.platformSupported() && access.packagePresent();
  return available;
}
}  // namespace host::usb
