#pragma once
// devices-v1: what Host settings shows about the streaming device features
// (공유 폴더, USB, 프린터, 사운드카드). A read-only observation of this VM,
// taken fresh for every status request: nothing is installed, started or
// changed. Fixed codes only, never a device name, path or Windows error text.
// Whether a user may use a feature is the administrator's per-user x VM
// permission on the broker, which this VM cannot see: these fields say only
// whether this VM can provide it.
#include <cstring>
#include <initializer_list>
#include <string>

namespace host {
struct DeviceObservation {
  // Drive sharing: the WinFsp DLL its installer registered (the user-session
  // helper still makes the real load test), and the in-box WebClient service of
  // the WebDAV fallback: "usable" (present, not disabled), "disabled", "missing".
  // `winfspPackage`: this Host carries the pinned WinFsp MSI (bin\winfsp), so
  // Host settings can install it (settings_rpc.hpp "installWinfsp").
  bool winfsp = false, winfspPackage = false;
  const char* webClient = "unknown";
  // usb-v1: workstation Windows 10 1809+, the pinned package beside this Host, and
  // usb::controllerOwnerName(): "none" (installed at first use), "started"/"stopped"
  // (ours), "unmarked"/"duplicates" (our package, marker lost or registered twice:
  // taken back automatically),
  // "foreign" (another usbip-win2 or product), "multiple", "unknown".
  bool usbPlatform = false, usbPackage = false;
  const char* usbController = "unknown";
  // print-v1: printUnavailableReason() ("" when printing can be offered); null unread.
  // `spooler`: the Print Spooler service, "running", "stopped" (startable),
  // "starting", "disabled" (start type Disabled: left alone), "missing", "unknown".
  const char* print = nullptr;
  const char* spooler = "unknown";
  // The default render endpoint streaming captures: "present", "absent",
  // "remote" (an RDP session sees only its own endpoints), "unknown".
  const char* audio = "unknown";
};

inline const char* deviceCode(const char* value, std::initializer_list<const char*> allowed, const char* fallback) {
  if (value) for (const char* item : allowed) if (std::strcmp(value, item) == 0) return item;
  return fallback;
}
inline std::string deviceStatusJson(const DeviceObservation& o) {
  const char* controller = deviceCode(o.usbController, {"none", "started", "stopped", "unmarked", "duplicates", "foreign", "multiple"}, "unknown");
  const char* print = o.print ? deviceCode(o.print, {"", "print_spooler_unavailable", "print_driver_unavailable"}, "unknown") : "unknown";
  return std::string("{\"schema\":1,\"drives\":{\"winfsp\":") + (o.winfsp ? "true" : "false") + ",\"package\":" + (o.winfspPackage ? "true" : "false") +
         ",\"webClient\":\"" + deviceCode(o.webClient, {"usable", "disabled", "missing"}, "unknown") +
         "\"},\"usb\":{\"supported\":" + (o.usbPlatform ? "true" : "false") + ",\"package\":" + (o.usbPackage ? "true" : "false") +
         ",\"controller\":\"" + controller + "\"},\"printers\":{\"reason\":\"" + print +
         "\",\"spooler\":\"" + deviceCode(o.spooler, {"running", "stopped", "starting", "disabled", "missing"}, "unknown") +
         "\"},\"audio\":{\"output\":\"" + deviceCode(o.audio, {"present", "absent", "remote"}, "unknown") + "\"}}";
}
}  // namespace host
