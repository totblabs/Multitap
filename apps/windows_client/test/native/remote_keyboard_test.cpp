// Production keyboard dispatch with substituted KBDLLHOOKSTRUCTs only.
// Never calls Capture(true), installs hooks, creates windows, changes focus or
// calls SendInput. The OS hook/own-window smoke target is deliberately separate.
#include "remote_input.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace multitap {
struct RemoteInputTestPeer {
  static bool Dispatch(RemoteInput& input, UINT vk, UINT scan, bool down,
                       DWORD flags = 0, bool foreground = true,
                       bool system = false, ULONG_PTR extra = 0) {
    KBDLLHOOKSTRUCT key{};
    key.vkCode = vk;
    key.scanCode = scan;
    key.flags = flags | (down ? 0 : LLKHF_UP);
    key.dwExtraInfo = extra;
    return input.HandleKey(system ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP)
                                  : (down ? WM_KEYDOWN : WM_KEYUP),
                           key, foreground);
  }
  static bool InvalidMessage(RemoteInput& input) {
    KBDLLHOOKSTRUCT key{};
    key.vkCode = VK_HANGUL;
    key.scanCode = 0xf2;
    return input.HandleKey(WM_CHAR, key, true);
  }
  static bool Held(const RemoteInput& input, UINT vk) { return input.pressed_[vk]; }
  static bool Installed(const RemoteInput& input) { return input.hook_ != nullptr; }
  static WPARAM ToggleGeneration(const RemoteInput& input) {
    return input.relative_toggle_generation_;
  }
  static bool ToggleCurrent(const RemoteInput& input, WPARAM generation) {
    return input.RelativeToggleCurrent(generation);
  }
};
}

namespace {
using V = flutter::EncodableValue;
using Event = flutter::EncodableMap;
using multitap::RemoteInput;
using Peer = multitap::RemoteInputTestPeer;
int checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
bool Down(const Event& event) { return std::get<bool>(event.at(V("down"))); }
int Number(const Event& event, const char* name) {
  return std::get<int32_t>(event.at(V(name)));
}
bool Extended(const Event& event) {
  return std::get<bool>(event.at(V("extended")));
}
void WireKey(const Event& event, UINT vk, UINT scan, bool down, bool extended) {
  Check(std::get<std::string>(event.at(V("type"))) == "input" &&
        std::get<std::string>(event.at(V("event"))) == "key",
        "Existing input/key wire shape changed");
  Check(event.size() == 6 && Number(event, "vk") == static_cast<int>(vk) &&
        Number(event, "scan") == static_cast<int>(scan) &&
        Down(event) == down && Extended(event) == extended,
        "Semantic VK, observed scan, direction or extended flag changed");
}

void DedicatedLanguageKeys() {
  std::vector<Event> events;
  RemoteInput input([&](auto event) { events.push_back(std::move(event)); });
  for (const auto [vk, scan] : {std::pair<UINT, UINT>{VK_HANGUL, 0xf2},
                               std::pair<UINT, UINT>{VK_HANJA, 0xf1}}) {
    for (const bool extended : {false, true}) {
      events.clear();
      Check(Peer::Dispatch(input, vk, scan, false, extended ? LLKHF_EXTENDED : 0),
            "Dedicated release-only language action was not captured");
      Check(events.size() == 2, "Release-only action lacks exactly one make/break pair");
      WireKey(events[0], vk, scan, true, extended);
      WireKey(events[1], vk, scan, false, extended);
      Check(!Peer::Held(input, vk), "Completed IME action left a held key");
    }
    events.clear();
    Peer::Dispatch(input, vk, scan, false);
    Peer::Dispatch(input, vk, scan, false);
    Check(events.size() == 4 && Down(events[0]) && !Down(events[1]) &&
          Down(events[2]) && !Down(events[3]),
          "Two dedicated taps were lost or combined into a held key");
    events.clear();
    Peer::Dispatch(input, vk, scan, true);
    Peer::Dispatch(input, vk, scan, false);
    Check(events.size() == 2 && Down(events[0]) && !Down(events[1]),
          "Existing paired language key gained an extra toggle");
  }
  Check(!Peer::Installed(input), "Memory test acquired an OS hook");
}

void ExistingKeysAndGuards() {
  std::vector<Event> events;
  RemoteInput input([&](auto event) { events.push_back(std::move(event)); });
  // Korean 101-key semantic Right Alt/Ctrl and an ordinary Right Alt remain
  // paired physical events. Do not reinterpret an arbitrary Alt as Hangul.
  for (const auto [vk, scan] : {std::pair<UINT, UINT>{VK_HANGUL, 0x38},
                               std::pair<UINT, UINT>{VK_HANJA, 0x1d},
                               std::pair<UINT, UINT>{VK_RMENU, 0x38}}) {
    events.clear();
    Peer::Dispatch(input, vk, scan, true, LLKHF_EXTENDED, true, true);
    Peer::Dispatch(input, vk, scan, false, LLKHF_EXTENDED, true, true);
    Check(events.size() == 2, "Paired system key duplicated or disappeared");
    WireKey(events[0], vk, scan, true, true);
    WireKey(events[1], vk, scan, false, true);
  }
  for (const auto [vk, scan] : {std::pair<UINT, UINT>{VK_RMENU, 0x38},
                               std::pair<UINT, UINT>{VK_HANGUL, 0x38},
                               std::pair<UINT, UINT>{VK_HANJA, 0xf2},
                               std::pair<UINT, UINT>{'A', 0x1e}}) {
    events.clear();
    Peer::Dispatch(input, vk, scan, false);
    Check(events.size() == 1 && !Down(events[0]),
          "Unknown or ordinary orphan release invented a press");
  }
  events.clear();
  for (int i = 0; i < 3; ++i) Peer::Dispatch(input, 'A', 0x1e, true);
  Peer::Dispatch(input, 'A', 0x1e, false);
  Check(events.size() == 4 && !Peer::Held(input, 'A'), "Ordinary repeat/pairing changed");
  events.clear();
  for (const auto [flags, extra] : {
         std::pair<DWORD, ULONG_PTR>{LLKHF_INJECTED, 0x54425141},
         std::pair<DWORD, ULONG_PTR>{LLKHF_LOWER_IL_INJECTED, 0}}) {
    Check(!Peer::Dispatch(input, VK_HANGUL, 0xf2, false, flags, true, false, extra),
          "QA or lower-integrity input bypassed the exclusion");
  }
  Check(!Peer::InvalidMessage(input) && events.empty(), "Non-key hook message emitted input");
  Check(!Peer::Dispatch(input, 0, 0xf2, false) &&
        !Peer::Dispatch(input, 256, 0xf2, false) && events.empty(),
        "Out-of-range VK entered the key state");
  Peer::Dispatch(input, VK_LCONTROL, 0x1d, true);
  Peer::Dispatch(input, VK_RCONTROL, 0x1d, true, LLKHF_EXTENDED);
  Peer::Dispatch(input, VK_LCONTROL, 0x1d, false);
  Check(!Peer::Held(input, VK_LCONTROL) && Peer::Held(input, VK_RCONTROL),
        "Sided modifier release cleared the other Ctrl");
  events.clear();
  Check(!Peer::Dispatch(input, VK_HANGUL, 0xf2, false, 0, false) &&
        events.empty() && !Peer::Held(input, VK_RCONTROL),
        "Background language release bypassed focus reset");
  input.Capture(nullptr, false); // No installed hook: state-only reset.
  events.clear();
  Peer::Dispatch(input, VK_LCONTROL, 0x1d, true);
  Peer::Dispatch(input, VK_LMENU, 0x38, true);
  Check(!Peer::Dispatch(input, VK_DELETE, 0x53, true, LLKHF_EXTENDED) &&
        events.size() == 2, "Secure attention was forwarded as ordinary input");
  Check(!Peer::Installed(input), "Memory guards installed an OS hook");
}

int ToggleMessages() {
  MSG message{};
  int count = 0;
  while (PeekMessageW(&message, reinterpret_cast<HWND>(-1), multitap::kRelativeMouseToggleMessage,
                      multitap::kRelativeMouseToggleMessage, PM_REMOVE)) ++count;
  return count;
}

// relative-mouse-v1: Ctrl+Alt+Shift+Z stays here only while armed.
void RelativeToggle() {
  std::vector<Event> events;
  RemoteInput input([&](auto event) { events.push_back(std::move(event)); });
  const auto modifiers = [&](bool down) {
    Peer::Dispatch(input, VK_LCONTROL, 0x1d, down);
    Peer::Dispatch(input, VK_LMENU, 0x38, down);
    Peer::Dispatch(input, VK_LSHIFT, 0x2a, down);
  };
  ToggleMessages();
  // Not armed: every key reaches the remote exactly as before.
  modifiers(true);
  Check(Peer::Dispatch(input, 'Z', 0x2c, true) && Peer::Dispatch(input, 'Z', 0x2c, false), "Unarmed Z was not consumed for the remote");
  modifiers(false);
  Check(events.size() == 8 && ToggleMessages() == 0, "Unarmed Ctrl+Alt+Shift+Z changed");
  WireKey(events[3], 'Z', 0x2c, true, false);
  WireKey(events[4], 'Z', 0x2c, false, false);
  events.clear();
  // Armed: the modifiers go on, Z never reaches the remote, one toggle.
  input.SetRelativeToggle(true);
  modifiers(true);
  Check(Peer::Dispatch(input, 'Z', 0x2c, true), "The toggle was not consumed");
  Check(ToggleMessages() == 1, "Toggle waited for a key release instead of the first Z keydown");
  for (int i = 0; i < 3; ++i) Check(Peer::Dispatch(input, 'Z', 0x2c, true), "A toggle repeat was not consumed");
  Check(Peer::Dispatch(input, 'Z', 0x2c, false), "The toggle release was not consumed");
  modifiers(false);
  Check(events.size() == 6 && ToggleMessages() == 0, "Z reached the remote or toggled twice");
  for (int i = 0; i < 3; ++i) {
    Check(Down(events[i]) && !Down(events[i + 3]) &&
          Number(events[i], "vk") == Number(events[i + 3], "vk"),
          "The shortcut left a remote modifier pressed");
  }
  for (const auto& event : events) Check(Number(event, "vk") != 'Z', "Z reached the remote");
  events.clear();
  // Releasing a modifier before Z and then auto-repeating Z still means the
  // same physical press. Only a fresh Z make/break pair can toggle again.
  modifiers(true);
  Peer::Dispatch(input, 'Z', 0x2c, true);
  modifiers(false);
  for (int i = 0; i < 3; ++i) Peer::Dispatch(input, 'Z', 0x2c, true);
  Check(ToggleMessages() == 1 && events.size() == 6,
        "Modifier release or Z repeat toggled more than once");
  Peer::Dispatch(input, 'Z', 0x2c, false);
  modifiers(true);
  Peer::Dispatch(input, 'Z', 0x2c, true);
  Check(ToggleMessages() == 1, "A new physical Z press did not toggle immediately");
  Peer::Dispatch(input, 'Z', 0x2c, false);
  modifiers(false);
  Check(events.size() == 12 && !Peer::Held(input, VK_LCONTROL) &&
        !Peer::Held(input, VK_LMENU) && !Peer::Held(input, VK_LSHIFT),
        "Repeated toggles lost a modifier release");
  events.clear();
  // Armed, but not the whole chord.
  Peer::Dispatch(input, 'Z', 0x2c, true);
  Peer::Dispatch(input, 'Z', 0x2c, false);
  Peer::Dispatch(input, VK_LCONTROL, 0x1d, true);
  Peer::Dispatch(input, 'Z', 0x2c, true);
  Peer::Dispatch(input, 'Z', 0x2c, false);
  Peer::Dispatch(input, VK_LCONTROL, 0x1d, false);
  Check(events.size() == 6 && ToggleMessages() == 0, "Z or Ctrl+Z stopped reaching the remote");
  events.clear();
  // A Z press already forwarded to the VM must keep its release there. Adding
  // modifiers while Z is held cannot turn an auto-repeat into a fresh chord.
  Peer::Dispatch(input, 'Z', 0x2c, true);
  modifiers(true);
  for (int i = 0; i < 3; ++i) Peer::Dispatch(input, 'Z', 0x2c, true);
  Peer::Dispatch(input, 'Z', 0x2c, false);
  modifiers(false);
  Check(ToggleMessages() == 0 && events.size() == 11,
        "An already-held Z repeat became a shortcut and swallowed its remote release");
  WireKey(events[0], 'Z', 0x2c, true, false);
  WireKey(events[7], 'Z', 0x2c, false, false);
  Check(!Peer::Held(input, 'Z'), "The forwarded Z stayed pressed");
  events.clear();
  // Disarmed while held: its release still stays here.
  modifiers(true);
  Peer::Dispatch(input, 'Z', 0x2c, true);
  input.SetRelativeToggle(false);
  Peer::Dispatch(input, 'Z', 0x2c, false);
  modifiers(false);
  Check(events.size() == 6 && ToggleMessages() == 1, "An orphan Z release reached the remote");
  events.clear();
  // A capture reset forgets the held key.
  input.SetRelativeToggle(true);
  modifiers(true);
  Peer::Dispatch(input, 'Z', 0x2c, true);
  input.Capture(nullptr, false);
  input.SetRelativeToggle(false);
  events.clear();
  Peer::Dispatch(input, 'Z', 0x2c, false);
  Check(events.size() == 1 && ToggleMessages() == 1, "A reset left the key swallowed");
}

void RelativeToggleQueueOwnership() {
  RemoteInput input([](auto) {});
  input.SetRelativeToggle(true);
  const auto first = Peer::ToggleGeneration(input);
  Peer::Dispatch(input, VK_LCONTROL, 0x1d, true);
  Peer::Dispatch(input, VK_LMENU, 0x38, true);
  Peer::Dispatch(input, VK_LSHIFT, 0x2a, true);
  Peer::Dispatch(input, 'Z', 0x2c, true);
  MSG queued{};
  Check(PeekMessageW(&queued, reinterpret_cast<HWND>(-1),
                    multitap::kRelativeMouseToggleMessage,
                    multitap::kRelativeMouseToggleMessage, PM_REMOVE) &&
        queued.wParam == first,
        "Posted shortcut did not carry its capture/arming epoch");
  Check(Peer::ToggleCurrent(input, first), "An armed toggle did not own its epoch");
  Check(!input.RelativeTogglePending(first), "No keyboard capture admitted a queued toggle");
  input.SetRelativeToggle(true);
  Check(Peer::ToggleCurrent(input, first), "An unchanged arm invalidated a current chord");
  input.SetRelativeToggle(false);
  Check(!Peer::ToggleCurrent(input, first), "Disarming admitted a queued chord");
  input.SetRelativeToggle(true);
  Check(!Peer::ToggleCurrent(input, first), "Rearming revived an old queued chord");
  const auto rearmed = Peer::ToggleGeneration(input);
  Check(Peer::ToggleCurrent(input, rearmed), "A fresh arming epoch was rejected");
  input.Capture(nullptr, false);
  Check(!Peer::ToggleCurrent(input, rearmed), "Capture release admitted its old queued chord");
  Check(!Peer::Installed(input), "Queue ownership test installed an OS hook");
}

void ReentrantCaptureReturn() {
  std::vector<Event> events;
  RemoteInput* current = nullptr;
  bool returnOnMake = true;
  RemoteInput input([&](auto event) {
    events.push_back(std::move(event));
    if (returnOnMake && Number(events.back(), "vk") == VK_HANGUL && Down(events.back())) {
      returnOnMake = false;
      current->Capture(nullptr, false); // No OS hook was installed.
      Peer::Dispatch(*current, 'B', 0x30, true);
    }
  });
  current = &input;
  Check(Peer::Dispatch(input, VK_HANGUL, 0xf2, false), "Returning action was not consumed");
  Check(events.size() == 2 && Number(events[0], "vk") == VK_HANGUL && Down(events[0]) &&
        Number(events[1], "vk") == 'B' && Down(events[1]),
        "Old language release escaped after capture returned to a successor");
  Check(!Peer::Held(input, VK_HANGUL) && Peer::Held(input, 'B'),
        "Old action changed the successor key state");
  Peer::Dispatch(input, 'B', 0x30, false);
  Check(events.size() == 3 && !Down(events.back()) && !Peer::Held(input, 'B'),
        "Successor key lost its matching release");
}
}

int main() {
  try {
    DedicatedLanguageKeys();
    ExistingKeysAndGuards();
    RelativeToggle();
    RelativeToggleQueueOwnership();
    ReentrantCaptureReturn();
    std::cout << "{\"kind\":\"substituted-keyboard-dispatch\",\"checks\":" << checks
              << ",\"osHookInstalled\":false,\"osInputInjected\":false,"
                 "\"physicalKeyboardVerified\":false,\"guestImeVerified\":false}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
