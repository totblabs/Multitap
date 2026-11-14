// Explicit probe only (administrator, in a signed-in session; uses this PC's pinned usbip-win2 controller): plugs
// a virtual Xbox 360 controller (gamepad_device.hpp) in through the real controller and loopback server, checks
// that Windows' own Xbox driver takes it and XInput reads the state sent (buttons, triggers, sticks, a change),
// that XInput's rumble comes back, then unplugs it and checks it went. Prints one JSON line; nothing stays.
#include <winsock2.h>
#include "gamepad_device.hpp"
#include <iostream>
using namespace host;
namespace {
struct XGamepad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XState { DWORD packet; XGamepad pad; };
struct XVibration { WORD left, right; };
using GetState = DWORD(WINAPI*)(DWORD, XState*);
using SetState = DWORD(WINAPI*)(DWORD, XVibration*);
bool waitFor(const std::function<bool()>& condition, int milliseconds) {
  for (int waited = 0; waited < milliseconds; waited += 20) { if (condition()) return true; Sleep(20); }
  return condition();
}
}  // namespace

int main() {
  WSADATA wsa{}; if (WSAStartup(MAKEWORD(2, 2), &wsa)) { std::cout << "{\"ok\":false,\"step\":\"winsock\"}\n"; return 1; }
  const HMODULE xinput = LoadLibraryW(L"xinput1_4.dll");
  const auto getState = xinput ? reinterpret_cast<GetState>(reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState"))) : nullptr;
  const auto setState = xinput ? reinterpret_cast<SetState>(reinterpret_cast<void*>(GetProcAddress(xinput, "XInputSetState"))) : nullptr;
  if (!getState || !setState) { std::cout << "{\"ok\":false,\"step\":\"xinput\"}\n"; return 1; }
  bool before[4]{}; for (DWORD i = 0; i < 4; ++i) { XState s{}; before[i] = getState(i, &s) == ERROR_SUCCESS; }
  std::mutex lock; std::vector<std::pair<int, int>> felt;
  bool plugged = false, seen = false, matched = false, changed = false, rumbled = false, gone = false; int slot = -1; uint32_t controls = 0, stalled = 0, lastStalled = 0, reads = 0;
  {
    usb::GamepadDevices pads;
    pads.onRumble([&](int pad, uint8_t low, uint8_t high) { std::lock_guard g(lock); if (pad == 0) felt.push_back({low, high}); });
    usb::xpad::State first; first.buttons = 0x1000; first.lt = 200; first.rt = 10; first.lx = 12000; first.ly = -12000; first.rx = -300; first.ry = 32767;
    pads.state(0, first);
    plugged = waitFor([&] { return pads.plugged(0); }, 30000);
    if (plugged) {
      seen = waitFor([&] { for (DWORD i = 0; i < 4; ++i) { XState s{}; if (!before[i] && getState(i, &s) == ERROR_SUCCESS) { slot = int(i); return true; } } return false; }, 15000);
      if (seen) {
        matched = waitFor([&] { XState s{}; return getState(DWORD(slot), &s) == ERROR_SUCCESS && s.pad.buttons == 0x1000 && s.pad.lt == 200 && s.pad.rt == 10 && s.pad.lx == 12000 && s.pad.ly == -12000 && s.pad.rx == -300 && s.pad.ry == 32767; }, 3000);
        usb::xpad::State next; next.buttons = 0x0001 | 0x0010; next.ry = -32768;
        pads.state(0, next);
        changed = waitFor([&] { XState s{}; return getState(DWORD(slot), &s) == ERROR_SUCCESS && s.pad.buttons == 0x0011 && s.pad.ry == -32768 && s.pad.lt == 0; }, 3000);
        XVibration v{0x8000, 0x4000}; setState(DWORD(slot), &v);
        rumbled = waitFor([&] { std::lock_guard g(lock); return !felt.empty() && felt.back() == std::pair<int, int>{0x80, 0x40}; }, 3000);
        XVibration off{0, 0}; setState(DWORD(slot), &off);
      }
      controls = pads.core().controls(0); stalled = pads.core().stalled(0); lastStalled = pads.core().lastStalled(0); reads = pads.core().readsSeen(0);
    }
    pads.remove(0);
    gone = waitFor([&] { XState s{}; return slot < 0 || getState(DWORD(slot), &s) == ERROR_DEVICE_NOT_CONNECTED; }, 15000) && waitFor([&] { return !pads.plugged(0); }, 5000);
    pads.stop();
  }
  const bool ok = plugged && seen && matched && changed && rumbled && gone && usb::keptPorts().empty();
  std::cout << "{\"ok\":" << (ok ? "true" : "false") << ",\"plugged\":" << plugged << ",\"xinputSlot\":" << slot << ",\"stateMatched\":" << matched << ",\"changeSeen\":" << changed
            << ",\"rumbleBack\":" << rumbled << ",\"rumbleReports\":" << felt.size() << ",\"controlRequests\":" << controls << ",\"stalled\":" << stalled << ",\"lastStalled\":" << lastStalled
            << ",\"reads\":" << reads << ",\"goneAfterRemove\":" << gone << "}\n";
  WSACleanup();
  return ok ? 0 : 1;
}
