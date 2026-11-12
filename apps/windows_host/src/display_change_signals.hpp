#pragma once
// Display changes the boot service sees without starting a session worker
// (backlog #70). The display probe (display_cache.hpp) starts a worker process;
// it keeps a ready observation until one of these signals or a minute.
// Each counts hostDisplayChangeEpoch (signal_cache.hpp):
//   - a monitor or display adapter device interface arrived or was removed
//     (CM_Register_Notification): a virtual or physical monitor plugged or
//     unplugged, an adapter started, stopped, disabled, installed or removed;
//   - Windows saved a display configuration: it keeps each applied topology
//     and mode under GraphicsDrivers\Configuration and the monitor sets under
//     \Connectivity (readable by SYSTEM only). The other GraphicsDrivers keys
//     are not watched: FeatureSetUsage, for one, counts graphics use.
//   - the virtual display's own state changed (native_display_service.hpp
//     counts each changed lease snapshot).
// Nothing runs while nothing changes: the registry waits are thread pool
// waits, the device callbacks come from the PnP manager.
#include "signal_cache.hpp"
#include <windows.h>
#include <cfgmgr32.h>
#include <memory>
#include <utility>
#include <vector>

namespace host {
struct RegistryWatchKey { HKEY root; std::wstring path; };
inline std::vector<RegistryWatchKey> displayConfigurationKeys() {
  return {{HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers\\Configuration"},
          {HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers\\Connectivity"}};
}
class DisplayChangeSignals {
 public:
  explicit DisplayChangeSignals(const std::vector<RegistryWatchKey>& keys = displayConfigurationKeys(), bool devices = true) {
    for (const auto& key : keys) { auto watch = std::make_unique<RegistryWatch>(); if (watch->open(key)) registry_.push_back(std::move(watch)); }
    if (devices) for (const GUID& type : {displayAdapterInterface, monitorInterface}) {
      CM_NOTIFY_FILTER filter{}; filter.cbSize = sizeof(filter); filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE; filter.u.DeviceInterface.ClassGuid = type;
      HCMNOTIFICATION handle = nullptr;
      if (CM_Register_Notification(&filter, nullptr, deviceChanged, &handle) == CR_SUCCESS) devices_.push_back(handle);
    }
  }
  ~DisplayChangeSignals() { for (const auto handle : devices_) CM_Unregister_Notification(handle); }
  DisplayChangeSignals(const DisplayChangeSignals&) = delete; DisplayChangeSignals& operator=(const DisplayChangeSignals&) = delete;
  // What is watched (a key that cannot be opened is skipped; the probe's
  // maximum age still bounds what it would have reported).
  size_t registryWatches() const { return registry_.size(); }
  size_t deviceWatches() const { return devices_.size(); }
  // GUID_DEVINTERFACE_DISPLAY_ADAPTER and GUID_DEVINTERFACE_MONITOR (Ntddvdeo.h).
  static constexpr GUID displayAdapterInterface{0x5b45201d, 0xf2f2, 0x4f3b, {0x85, 0xbb, 0x30, 0xff, 0x1f, 0x95, 0x35, 0x99}};
  static constexpr GUID monitorInterface{0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
 private:
  static DWORD CALLBACK deviceChanged(HCMNOTIFICATION, PVOID, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD) {
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) noteHostDisplayChange();
    return ERROR_SUCCESS;
  }
  // One key and its subtree. The notification is re-armed before the change is
  // counted, so a change right after the count is seen too.
  class RegistryWatch {
   public:
    bool open(const RegistryWatchKey& key) {
      if (RegOpenKeyExW(key.root, key.path.c_str(), 0, KEY_NOTIFY, &key_) != ERROR_SUCCESS) { key_ = nullptr; return false; }
      event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      return event_ && arm() && RegisterWaitForSingleObject(&wait_, event_, changed, this, INFINITE, WT_EXECUTEDEFAULT);
    }
    ~RegistryWatch() { if (wait_) UnregisterWaitEx(wait_, INVALID_HANDLE_VALUE); if (key_) RegCloseKey(key_); if (event_) CloseHandle(event_); }
   private:
    bool arm() { return RegNotifyChangeKeyValue(key_, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, event_, TRUE) == ERROR_SUCCESS; }
    static void CALLBACK changed(void* self, BOOLEAN) { (void)static_cast<RegistryWatch*>(self)->arm(); noteHostDisplayChange(); }
    HKEY key_ = nullptr; HANDLE event_ = nullptr, wait_ = nullptr;
  };
  std::vector<std::unique_ptr<RegistryWatch>> registry_;
  std::vector<HCMNOTIFICATION> devices_;
};
}
