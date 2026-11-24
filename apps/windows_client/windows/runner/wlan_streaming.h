#ifndef MULTITAP_WLAN_STREAMING_H_
#define MULTITAP_WLAN_STREAMING_H_
#include <windows.h>
#include <wlanapi.h>
#include <atomic>
#include <cstdint>
namespace multitap {
// While a stream runs on a PC that is on Wi-Fi: each connected Wi-Fi adapter
// is told that media is streaming (wlan_intf_opcode_media_streaming_mode) and
// that it should not scan for networks in the background
// (wlan_intf_opcode_background_scan_enabled), through Windows' own WLAN API
// (WlanSetInterface). An adapter that scans leaves its channel for a moment,
// and what was sent meanwhile comes late in a burst; the Android app holds
// the Wi-Fi low-latency lock for the same reason (AndroidStreamWifi.kt).
//  - Windows keeps both only while the adapter is connected, and per client of
//    the API: Release sets them back and closes this client. An adapter that
//    connected again is set again at the next Touch.
//  - Nothing here is needed for a stream: without the WLAN service (a wired
//    PC, a server edition without wlanapi.dll), without a connected adapter or
//    when Windows refuses, the stream runs as before and mode() says which.
//  - Scans that still complete are counted, so the records show whether the
//    adapter kept scanning.
// The window's thread calls Hold, Touch and Release; Windows calls Notified on
// a thread of its own, which only sets flags.
class WlanStreaming {
 public:
  // What mode() returns: no connected Wi-Fi adapter (or no WLAN service); both
  // settings taken by every connected adapter; one of them taken; none taken.
  enum Mode : int { kNoWifi = 0, kSet = 1, kPartly = 2, kRefused = 3 };
  WlanStreaming() = default;
  WlanStreaming(const WlanStreaming&) = delete;
  WlanStreaming& operator=(const WlanStreaming&) = delete;
  ~WlanStreaming() { Release(); if (library_) FreeLibrary(library_); }

  // A stream started. Again while held: nothing.
  void Hold() {
    if (handle_ || !Load()) return;
    DWORD version = 0; HANDLE handle = nullptr;
    error_ = open_(2, nullptr, &version, &handle);
    if (error_ != ERROR_SUCCESS) return;
    handle_ = handle; scans_ = 0; again_ = false;
    // Scan results and connections of every adapter, from Windows' auto configuration.
    notify_(handle_, WLAN_NOTIFICATION_SOURCE_ACM, TRUE, &WlanStreaming::Notified, this, nullptr, nullptr);
    Apply(true);
  }
  // Every second while the stream runs: an adapter that connected since is set too.
  void Touch() { if (handle_ && again_.exchange(false)) Apply(true); }
  // The stream ended.
  void Release() {
    if (!handle_) return;
    Apply(false);
    close_(handle_, nullptr); handle_ = nullptr; mode_ = kNoWifi;
  }
  bool held() const { return handle_ != nullptr; }
  bool tried() const { return loaded_; }  // Hold was called: mode() and error() say what came of it
  int mode() const { return mode_; }
  uint32_t error() const { return error_; }      // the Win32 code of what was refused last (0: nothing)
  // Scans completed or failed since the last call (or Hold).
  uint64_t TakeScans() { return scans_.exchange(0); }

 private:
  using Open = DWORD(WINAPI*)(DWORD, PVOID, PDWORD, PHANDLE);
  using Close = DWORD(WINAPI*)(HANDLE, PVOID);
  using Enum = DWORD(WINAPI*)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
  using Set = DWORD(WINAPI*)(HANDLE, const GUID*, WLAN_INTF_OPCODE, DWORD, const PVOID, PVOID);
  using Notify = DWORD(WINAPI*)(HANDLE, DWORD, BOOL, WLAN_NOTIFICATION_CALLBACK, PVOID, PVOID, PDWORD);
  using Free = VOID(WINAPI*)(PVOID);
  // wlanapi.dll is not on every Windows (server editions without the Wireless
  // LAN Service), so it is not imported: loaded from System32 when first needed.
  bool Load() {
    if (loaded_) return open_ != nullptr;
    loaded_ = true;
    library_ = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library_) { error_ = GetLastError(); return false; }
    const auto open = reinterpret_cast<Open>(GetProcAddress(library_, "WlanOpenHandle"));
    close_ = reinterpret_cast<Close>(GetProcAddress(library_, "WlanCloseHandle"));
    enum_ = reinterpret_cast<Enum>(GetProcAddress(library_, "WlanEnumInterfaces"));
    set_ = reinterpret_cast<Set>(GetProcAddress(library_, "WlanSetInterface"));
    notify_ = reinterpret_cast<Notify>(GetProcAddress(library_, "WlanRegisterNotification"));
    free_ = reinterpret_cast<Free>(GetProcAddress(library_, "WlanFreeMemory"));
    if (!open || !close_ || !enum_ || !set_ || !notify_ || !free_) { error_ = ERROR_PROC_NOT_FOUND; return false; }
    open_ = open; return true;
  }
  // `on`: streaming mode on and background scans off; otherwise both back.
  void Apply(bool on) {
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    const DWORD listed = enum_(handle_, nullptr, &list);
    if (listed != ERROR_SUCCESS || !list) { if (on) { error_ = listed; mode_ = kNoWifi; } return; }
    unsigned connected = 0, both = 0, one = 0; DWORD refused = ERROR_SUCCESS;
    for (DWORD n = 0; n < list->dwNumberOfItems; ++n) {
      const WLAN_INTERFACE_INFO& adapter = list->InterfaceInfo[n];
      if (adapter.isState != wlan_interface_state_connected) continue;
      ++connected;
      BOOL streaming = on ? TRUE : FALSE, scanning = on ? FALSE : TRUE;
      const DWORD first = set_(handle_, &adapter.InterfaceGuid, wlan_intf_opcode_media_streaming_mode, sizeof(streaming), &streaming, nullptr);
      const DWORD second = set_(handle_, &adapter.InterfaceGuid, wlan_intf_opcode_background_scan_enabled, sizeof(scanning), &scanning, nullptr);
      if (first == ERROR_SUCCESS && second == ERROR_SUCCESS) ++both; else if (first == ERROR_SUCCESS || second == ERROR_SUCCESS) ++one;
      if (first != ERROR_SUCCESS) refused = first; else if (second != ERROR_SUCCESS) refused = second;
    }
    free_(list);
    if (!on) return;
    error_ = refused;
    mode_ = !connected ? kNoWifi : both == connected ? kSet : both + one ? kPartly : kRefused;
  }
  static VOID WINAPI Notified(PWLAN_NOTIFICATION_DATA data, PVOID context) {
    auto* self = static_cast<WlanStreaming*>(context);
    if (!data || !self || data->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM) return;
    if (data->NotificationCode == wlan_notification_acm_scan_complete || data->NotificationCode == wlan_notification_acm_scan_fail) ++self->scans_;
    else if (data->NotificationCode == wlan_notification_acm_connection_complete) self->again_ = true;
  }
  HMODULE library_ = nullptr;
  bool loaded_ = false;
  Open open_ = nullptr; Close close_ = nullptr; Enum enum_ = nullptr; Set set_ = nullptr; Notify notify_ = nullptr; Free free_ = nullptr;
  HANDLE handle_ = nullptr;
  std::atomic<int> mode_{kNoWifi};
  std::atomic<uint32_t> error_{0};
  std::atomic<uint64_t> scans_{0};
  std::atomic<bool> again_{false};
};
}
#endif
