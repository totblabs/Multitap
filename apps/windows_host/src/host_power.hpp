#pragma once
#include "operational_log.hpp"
#include "host_text.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <devguid.h>
#include <powrprof.h>
#include <reason.h>
#include <cstdio>
#include <cwctype>
#include <functional>
#include <optional>
#ifdef _MSC_VER
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

// Physical PC power (pc-power-v1, server/src/pc-power.mjs): power management
// for a physical PC too. The service announces 종료 and 다시 시작 and the
// PC's wired adapters for Wake-on-LAN, then answers each broker request with
// accepted or a fixed refusal code. Windows is asked for an ordinary shutdown:
// never SHUTDOWN_FORCE_OTHERS or any other flag that closes applications
// without asking. Adapter and BIOS settings are only read, never changed.
namespace host {
inline constexpr const char* hostPowerProtocol = "pc-power-v1";

struct WakeAdapter { std::string mac; std::optional<bool> wakeEnabled, magicPacket; };

// Append only: the index is hostAction 281's error number (server/admin/js/labels.js).
inline const std::array<const char*, 7> hostPowerCodes{"", "invalid_request", "shutdown_in_progress", "privilege_unavailable", "other_users_signed_in", "machine_locked", "shutdown_failed"};

// Windows' answer to InitiateShutdownW (or to enabling the privilege) as a fixed code; "" is accepted.
inline const char* hostPowerRefusal(DWORD error) {
  switch (error) {
    case ERROR_SUCCESS: return "";
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: case ERROR_NOT_ALL_ASSIGNED: return "privilege_unavailable";
    case ERROR_SHUTDOWN_IN_PROGRESS: case ERROR_SHUTDOWN_IS_SCHEDULED: return "shutdown_in_progress";
    case ERROR_SHUTDOWN_USERS_LOGGED_ON: return "other_users_signed_in";
    case ERROR_MACHINE_LOCKED: return "machine_locked";
    default: return "shutdown_failed";
  }
}

struct HostPowerEnvironment {
  // Asks Windows to restart (true) or shut down (false); returns its Win32 answer.
  std::function<DWORD(bool restart)> shutdown;
  std::function<std::vector<WakeAdapter>()> adapters;
};

inline std::string hostPowerHello(const std::optional<std::vector<WakeAdapter>>& adapters) {
  const auto flag = [](const std::optional<bool>& value) { return value ? (*value ? "true" : "false") : "null"; };
  std::string message = std::string("{\"type\":\"host.power.hello\",\"protocol\":\"") + hostPowerProtocol + "\",\"actions\":[\"shutdown\",\"reboot\"]";
  // No list when the adapters could not be read: the broker keeps the last one.
  if (adapters) {
    message += ",\"wake\":[";
    for (size_t i = 0; i < adapters->size() && i < 8; ++i) {
      const auto& adapter = (*adapters)[i];
      message += std::string(i ? "," : "") + "{\"mac\":" + jsonQuote(adapter.mac) + ",\"wakeEnabled\":" + flag(adapter.wakeEnabled) + ",\"magicPacket\":" + flag(adapter.magicPacket) + '}';
    }
    message += ']';
  }
  return message + '}';
}

class HostPower {
 public:
  using Send = std::function<void(const std::string&)>;
  HostPower(HostPowerEnvironment environment, Send send) : environment_(std::move(environment)), send_(std::move(send)) {}
  // First power message of every channel (after host.update.hello).
  std::string hello() {
    std::optional<std::vector<WakeAdapter>> adapters;
    try { if (environment_.adapters) adapters = environment_.adapters(); } catch (...) { adapters.reset(); }
    return hostPowerHello(adapters);
  }
  void request(const Json& message) {
    const auto id = message.get("requestId").string(), action = message.get("action").string();
    if (id.size() != 36 || id.find_first_not_of("0123456789abcdef-") != std::string::npos) return;  // Nothing correlatable to answer.
    const bool restart = action == "reboot";
    std::string code;
    if (!restart && action != "shutdown") code = "invalid_request";
    else {
      std::lock_guard lock(mutex_);
      // Windows already accepted one: a second request changes nothing.
      if (accepted_) code = "shutdown_in_progress";
      else {
        DWORD error = ERROR_GEN_FAILURE;
        try { if (environment_.shutdown) error = environment_.shutdown(restart); } catch (...) { error = ERROR_GEN_FAILURE; }
        code = hostPowerRefusal(error); accepted_ = code.empty();
      }
    }
    uint32_t index = 0;
    for (size_t i = 0; i < hostPowerCodes.size(); ++i) if (code == hostPowerCodes[i]) index = uint32_t(i);
    recordHostEvent("hostAction", {{"action", 281}, {"stage", restart ? 1 : 2}, {"error", index}, {"ok", code.empty()}});
    try { send_("{\"type\":\"host.power.result\",\"requestId\":" + jsonQuote(id) + ",\"state\":" + (code.empty() ? "\"accepted\"" : "\"refused\"") + ",\"code\":" + jsonQuote(code) + '}'); } catch (...) { }
  }

 private:
  HostPowerEnvironment environment_; Send send_; std::mutex mutex_; bool accepted_ = false;
};

// Read only: wired adapters on a hardware bus (PCI or USB), their MAC, whether
// Windows has armed the device to wake the computer (what `powercfg
// /devicequery wake_armed` lists) and the driver's standard
// *WakeOnMagicPacket setting (null when the driver has none).
inline std::vector<WakeAdapter> windowsWakeAdapters() {
  const auto lower = [](std::wstring text) { for (auto& c : text) c = wchar_t(towlower(c)); return text; };
  struct Device { std::wstring name; std::optional<bool> magicPacket; };
  std::map<std::wstring, Device> devices;
  {
    const HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) throw std::runtime_error("wake_adapters_unavailable");
    struct Set { HDEVINFO value; ~Set() { SetupDiDestroyDeviceInfoList(value); } } owner{set};
    SP_DEVINFO_DATA info{}; info.cbSize = sizeof(info);
    for (DWORD index = 0; index < 1024 && SetupDiEnumDeviceInfo(set, index, &info); ++index) {
      wchar_t instance[512]{};
      if (!SetupDiGetDeviceInstanceIdW(set, &info, instance, DWORD(std::size(instance)), nullptr)) continue;
      if (_wcsnicmp(instance, L"PCI\\", 4) != 0 && _wcsnicmp(instance, L"USB\\", 4) != 0) continue;
      const HKEY key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
      if (key == INVALID_HANDLE_VALUE) continue;
      wchar_t guid[64]{}, magic[8]{}; DWORD type = 0, size = DWORD(sizeof(guid) - sizeof(wchar_t));
      const bool named = RegQueryValueExW(key, L"NetCfgInstanceId", nullptr, &type, reinterpret_cast<BYTE*>(guid), &size) == ERROR_SUCCESS && type == REG_SZ;
      std::optional<bool> magicPacket; type = 0; size = DWORD(sizeof(magic) - sizeof(wchar_t));
      if (RegQueryValueExW(key, L"*WakeOnMagicPacket", nullptr, &type, reinterpret_cast<BYTE*>(magic), &size) == ERROR_SUCCESS && type == REG_SZ) magicPacket = std::wstring(magic) != L"0";
      RegCloseKey(key);
      if (!named) continue;
      wchar_t name[256]{};
      if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<BYTE*>(name), DWORD(sizeof(name) - sizeof(wchar_t)), nullptr))
        SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<BYTE*>(name), DWORD(sizeof(name) - sizeof(wchar_t)), nullptr);
      devices[lower(guid)] = {name, magicPacket};
    }
  }
  std::optional<std::set<std::wstring>> armed;
  if (DevicePowerOpen(0)) {
    std::set<std::wstring> names;
    for (ULONG index = 0; index < 4096; ++index) {
      wchar_t name[512]{}; ULONG size = ULONG(sizeof(name) - sizeof(wchar_t));
      if (!DevicePowerEnumDevices(index, DEVICEPOWER_FILTER_DEVICES_PRESENT | DEVICEPOWER_FILTER_WAKEENABLED,
            PDCAP_WAKE_FROM_D0_SUPPORTED | PDCAP_WAKE_FROM_D1_SUPPORTED | PDCAP_WAKE_FROM_D2_SUPPORTED | PDCAP_WAKE_FROM_D3_SUPPORTED, reinterpret_cast<PBYTE>(name), &size)) break;
      names.insert(name);
    }
    DevicePowerClose();
    armed = std::move(names);
  }
  ULONG bytes = 16384; std::vector<BYTE> buffer; ULONG result = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 4 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
    buffer.assign(bytes, 0);
    result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bytes);
  }
  if (result == ERROR_NO_DATA) return {};
  if (result != NO_ERROR) throw std::runtime_error("wake_adapters_unavailable");
  std::vector<WakeAdapter> adapters;
  for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter && adapters.size() < 8; adapter = adapter->Next) {
    if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD || adapter->PhysicalAddressLength != 6 || !adapter->AdapterName) continue;
    const auto device = devices.find(lower(wide(adapter->AdapterName)));
    if (device == devices.end()) continue;
    const auto* a = adapter->PhysicalAddress;
    char mac[18]{}; std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
    adapters.push_back({mac, armed ? std::optional<bool>(armed->count(device->second.name) > 0) : std::nullopt, device->second.magicPacket});
  }
  return adapters;
}

#ifndef MULTITAP_HOST_POWER_TEST
// The power fixture does not compile this, and only the service wires it
// (service.hpp): no test can restart or shut down a machine.
inline DWORD enableShutdownPrivilege(HANDLE token) {
  TOKEN_PRIVILEGES requested{}; requested.PrivilegeCount = 1;
  if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &requested.Privileges[0].Luid)) return GetLastError();
  requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED; SetLastError(ERROR_SUCCESS);
  if (!AdjustTokenPrivileges(token, FALSE, &requested, 0, nullptr, nullptr)) { const DWORD error = GetLastError(); return error ? error : ERROR_PRIVILEGE_NOT_HELD; }
  return GetLastError();  // ERROR_NOT_ALL_ASSIGNED: the account may not shut down
}
// An ordinary Windows shutdown or restart with a planned reason, no grace
// period and no force flag. With someone signed in at the console it is that
// user's own request (Windows asks their applications, which may refuse);
// a grace period would require SHUTDOWN_FORCE_SELF, which closes that user's
// applications without asking. Nobody signed in: the service's own request.
inline DWORD initiateWindowsShutdown(bool restart) {
  // Windows shows it to whoever is signed in: their language, not SYSTEM's.
  std::wstring notice = ui_text::get(ui_text::Id::PowerMessage, multitap::ui::consoleLanguage());
  wchar_t* message = notice.data();
  const DWORD flags = restart ? SHUTDOWN_RESTART : SHUTDOWN_POWEROFF;
  const DWORD reason = SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_MAINTENANCE | SHTDN_REASON_FLAG_PLANNED;
  const DWORD session = WTSGetActiveConsoleSessionId();
  HANDLE user = nullptr;
  if (session != 0xFFFFFFFF && WTSQueryUserToken(session, &user)) {
    WinHandle primary(user); HANDLE raw = nullptr;
    if (!DuplicateTokenEx(primary.value, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_ADJUST_PRIVILEGES, nullptr, SecurityImpersonation, TokenImpersonation, &raw)) return GetLastError();
    WinHandle impersonation(raw);
    if (const DWORD error = enableShutdownPrivilege(impersonation.value); error != ERROR_SUCCESS) return error;
    if (!SetThreadToken(nullptr, impersonation.value)) return GetLastError();
    const DWORD result = InitiateShutdownW(nullptr, message, 0, flags, reason);
    RevertToSelf();
    return result;
  }
  HANDLE process = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &process)) return GetLastError();
  WinHandle token(process);
  if (const DWORD error = enableShutdownPrivilege(token.value); error != ERROR_SUCCESS) return error;
  return InitiateShutdownW(nullptr, message, 0, flags, reason);
}
inline HostPowerEnvironment serviceHostPowerEnvironment() { return {initiateWindowsShutdown, windowsWakeAdapters}; }
#endif
}  // namespace host
