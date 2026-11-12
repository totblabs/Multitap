#pragma once
// The Flutter channel `multitap/usb` (lib/data/native_usb.dart): this PC's USB
// devices for usb-v1. Device enumeration, capture and installation run on a
// worker; requests run on their own threads (usb_capture_core.h); every reply
// is completed on the UI thread through one posted message, like the clipboard.
// Each window has its own channel and engine; the channels of one process know
// each other, so a device held by another remote window is shown as such.
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <windows.h>
#include <functional>
#include <memory>
#include <string>

namespace multitap {
// The UsbDk installer that ships with this client (usbdk\UsbDk_1.0.22_x64.msi
// beside the executable), pinned: Red Hat, Inc., Apache-2.0.
struct UsbDkInstaller {
  static constexpr const char* sha256 = "91f6f695e1e13c656024e6d3b55620bf08d8835ef05ee0496935ba6bb62466a5";
  static constexpr unsigned long long size = 6348800;
  static constexpr const wchar_t* file = L"UsbDk_1.0.22_x64.msi";
};
// Verifies the pinned installer (size, SHA-256, Red Hat, Inc. signature) and
// runs it silently through msiexec; a fixed code on failure, "" on success.
// [directory] is where `usbdk\UsbDk_1.0.22_x64.msi` lives.
std::string InstallUsbDk(const std::wstring& directory);

class UsbCaptureChannel {
 public:
  static constexpr UINT completed_message = WM_APP + 192;
  explicit UsbCaptureChannel(HWND reply_window);
  ~UsbCaptureChannel();
  UsbCaptureChannel(const UsbCaptureChannel&) = delete;
  UsbCaptureChannel& operator=(const UsbCaptureChannel&) = delete;
  void Enqueue(const flutter::MethodCall<flutter::EncodableValue>& call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  // On the UI thread when `completed_message` arrives.
  void Complete();
  // Called on the UI thread when a captured device left the bus.
  void SetGone(std::function<void(int64_t)> gone);
  // Called on the UI thread (coalesced) when a USB device, disk or volume arrived or left, while a session watches.
  void SetChanged(std::function<void()> changed);
  // Called on the UI thread when another window of this client asks for a
  // device this one holds (its 이 창으로 옮기기): the device id and that window's VM name.
  void SetHandOver(std::function<void(const std::string& id, const std::string& window)> handOver);
  // Gives every captured device back (window closing).
  void Shutdown();
 private:
  struct State; std::shared_ptr<State> state_;
};
}  // namespace multitap
