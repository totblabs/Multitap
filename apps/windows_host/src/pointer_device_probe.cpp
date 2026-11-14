// Explicit probe only (administrator; uses this PC's pinned usbip-win2 controller): plugs the Host's own pointer
// device (pointer_device.hpp) in through the real controller and loopback server, checks that Windows starts it
// as a mouse (the USB device node, its HID mouse, one more mouse interface), that Windows' HID reads wait at
// the Host, then unplugs it and checks that everything went. Prints one JSON line; nothing stays attached.
#include <winsock2.h>
#include "pointer_device.hpp"
#include <iostream>
using namespace host;
namespace {
// GUID_DEVINTERFACE_MOUSE (ntddmou.h): one interface per mouse Windows' mouse class driver serves.
const GUID mouseInterface{0x378DE44C, 0x56EF, 0x11D1, {0xBC, 0x8C, 0x00, 0xA0, 0xC9, 0x14, 0x05, 0xDD}};
int mice() {
  GUID guid = mouseInterface; ULONG length = 0;
  if (CM_Get_Device_Interface_List_SizeW(&length, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length > 65536) return -1;
  std::wstring list(length + 1, L'\0');
  if (CM_Get_Device_Interface_ListW(&guid, nullptr, list.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return -1;
  int count = 0; for (const wchar_t* at = list.c_str(); *at; at += wcslen(at) + 1) ++count;
  return count;
}
// The HID child of the pointer: 2 started, 1 present but not started, 0 none.
int hidChild() {
  ULONG size = 0;
  if (CM_Get_Device_ID_List_SizeW(&size, L"HID", CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS || size < 2) return 0;
  std::vector<wchar_t> list(size);
  if (CM_Get_Device_ID_ListW(L"HID", list.data(), size, CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS) return 0;
  const wchar_t prefix[] = L"HID\\VID_1209&PID_0002";
  int state = 0;
  for (wchar_t* id = list.data(); *id; id += wcslen(id) + 1) {
    if (_wcsnicmp(id, prefix, wcslen(prefix)) != 0) continue;
    DEVINST node = 0; ULONG status = 0, problem = 0;
    if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS || CM_Get_DevNode_Status(&status, &problem, node, 0) != CR_SUCCESS) continue;
    state = std::max(state, (status & DN_STARTED) && !problem ? 2 : 1);
  }
  return state;
}
bool waitFor(const std::function<bool()>& condition, int milliseconds) {
  for (int waited = 0; waited < milliseconds; waited += 50) { if (condition()) return true; Sleep(50); }
  return condition();
}
}  // namespace

int main(int argc, char** argv) {
  WSADATA wsa{}; if (WSAStartup(MAKEWORD(2, 2), &wsa)) { std::cout << "{\"ok\":false,\"step\":\"winsock\"}\n"; return 1; }
  const std::string mode = argc > 1 ? argv[1] : "";
  // "killed": plugged in, then this process ends without unplugging (a service that crashed or was ended at
  // shutdown). "state": what is left afterwards (run it after "killed").
  if (mode == "state") {
    const auto ports = usb::vhciPorts();
    std::cout << "{\"usbDevice\":" << usb::vmDeviceState(usb::vmouse::vendor, usb::vmouse::pointer.product).state << ",\"hidMouse\":" << hidChild() << ",\"mice\":" << mice() << ",\"controllerPorts\":" << ports.size() << "}\n";
    return 0;
  }
  if (mode == "killed") {
    static usb::PointerDevice* leaked = new usb::PointerDevice();
    leaked->ask();
    const bool plugged = waitFor([] { return leaked->plugged(); }, 30000);
    const bool started = plugged && waitFor([] { return hidChild() == 2; }, 15000);
    std::cout << "{\"plugged\":" << plugged << ",\"hidMouse\":" << started << ",\"mice\":" << mice() << ",\"port\":" << leaked->port() << "}" << std::endl;
    TerminateProcess(GetCurrentProcess(), 3);
  }
  const int miceBefore = mice(), presentBefore = GetSystemMetrics(SM_MOUSEPRESENT);
  bool plugged = false, deviceStarted = false, hidStarted = false, miceUp = false, readsWaiting = false, gone = false;
  int port = 0, miceDuring = -1, miceAfter = -1; uint32_t controls = 0, reads = 0; size_t waiting = 0;
  {
    usb::PointerDevice pointer;
    pointer.ask();
    plugged = waitFor([&] { return pointer.plugged(); }, 30000);
    port = pointer.port();
    if (plugged) {
      deviceStarted = waitFor([] { return usb::vmDeviceState(usb::vmouse::vendor, usb::vmouse::pointer.product).state == 2; }, 15000);
      hidStarted = waitFor([] { return hidChild() == 2; }, 15000);
      miceUp = waitFor([&] { return mice() == miceBefore + 1; }, 15000);
      miceDuring = mice();
      readsWaiting = waitFor([&] { return pointer.core().waitingReads() > 0; }, 5000);
      controls = pointer.core().controls(); reads = pointer.core().readsSeen(); waiting = pointer.core().waitingReads();
    }
    pointer.stop();
    gone = waitFor([] { return usb::vmDeviceState(usb::vmouse::vendor, usb::vmouse::pointer.product).state == 0 && hidChild() == 0; }, 15000);
    miceAfter = mice();
  }
  const bool ok = plugged && deviceStarted && hidStarted && miceUp && readsWaiting && gone && miceAfter == miceBefore && usb::keptPorts().empty();
  std::cout << "{\"ok\":" << (ok ? "true" : "false") << ",\"plugged\":" << plugged << ",\"port\":" << port << ",\"usbDeviceStarted\":" << deviceStarted << ",\"hidMouseStarted\":" << hidStarted
            << ",\"miceBefore\":" << miceBefore << ",\"miceDuring\":" << miceDuring << ",\"miceAfter\":" << miceAfter << ",\"mousePresentBefore\":" << presentBefore
            << ",\"controlRequests\":" << controls << ",\"readsSeen\":" << reads << ",\"readsWaiting\":" << waiting << ",\"goneAfterStop\":" << gone << "}\n";
  WSACleanup();
  return ok ? 0 : 1;
}
