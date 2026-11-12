#pragma once
// Which way a host.qemu-console.prepare goes.
//
// One way is a worker in the unlocked console
// session of an administrator, which changes Windows' display configuration to
// "QEMU output alone" (qemu_console.hpp). With that way alone everything else
// would have none:
//   - signed out or locked: the service could only answer
//     qemu_console_unavailable, and the console would show whatever Windows had
//     left on the QEMU output, which is nothing when a stream ended with the
//     session (the worker dies at sign-out before it can restore);
//   - a standard user signed in: the worker's display authority needs an
//     administrator token, so it would answer qemu_console_cancelled and the
//     console would not open at all ("화면 복구가 취소됐어요").
// Those are exactly the states a person opens the Proxmox console for.
// (A standard user's worker may change the display for a stream,
// with the state file kept by the service: display_v2_state_rpc.hpp. The
// console's worker is not part of that; a standard user's console is parked.)
//
// The second way needs no Windows session: the service's own SDK controller
// stops (NativeDisplayService::parkForConsole), the owned virtual outputs leave
// and Windows falls back to the displays that remain. It is the orderly stop of
// every service restart and of the render adapter re-arm, held for as long as
// the console's hold lasts; afterwards the ordinary cold start brings the
// outputs back. No Windows setting, no process in another session, no input.
//
// What the park proves is that the owned outputs are gone (the SDK said so).
// That Windows then draws on the QEMU output is Windows' own fallback and is
// not read back: session 0 cannot query the console session's displays.
//
// "Only the basic display" is the worker's route
// only. The park cannot switch a foreign display off: SetDisplayConfig needs
// the console session, and session 0 has none to act in. A VM with a foreign
// display (a vGPU's own head such as NVIDIA GRID) therefore keeps it on
// while parked, and Windows chooses the primary among the QEMU output and that
// display from its own database. When the foreign display stays primary, the
// Proxmox console shows the QEMU output as a secondary extended desktop: the
// wallpaper and nothing else. The sign-in screen, the lock screen, a UAC
// prompt and the taskbar are drawn on the primary display nobody sees, so the
// console cannot be used to sign in there. What helps then is Console's
// restart (automatic sign-in): once an administrator is signed in and
// unlocked, the next console open takes the worker's route and turns the
// foreign display off. Not changed here; to check on a real vGPU VM.
#include <cstdint>

namespace host {
enum class QemuConsoleRoute { refuse, worker, park };
struct QemuConsoleSessionFacts {
  bool provider = false;  // display manager v2 on the selected native provider
  bool console = false;   // an active interactive session is the console session
  bool unlocked = false;  // and it is unlocked
  bool elevated = false;  // and its worker runs as an administrator (worker_token.hpp)
  // No secure desktop (a UAC prompt) shows there, as far as the last display probe knows. Under one Windows
  // refuses the worker's display change until the prompt is answered, and answering it is what the console is
  // opened for (a stream that ended under a prompt): the park needs no desktop.
  bool desktop = true;
};
// The worker keeps every guarantee it had (lease, identity, deadline, exact
// restore point); it is chosen whenever it can act. A Host without the native
// provider has nothing to park and answers as before.
inline QemuConsoleRoute qemuConsoleRoute(const QemuConsoleSessionFacts& facts) noexcept {
  if (!facts.provider) return QemuConsoleRoute::refuse;
  return facts.console && facts.unlocked && facts.elevated && facts.desktop ? QemuConsoleRoute::worker : QemuConsoleRoute::park;
}
// A parked hold is not bound to a Windows session (signing in through the
// console is what it is for) or to a lease (none exists while parked): it stays
// while this is the same machine and the SDK owner is still parked.
inline bool qemuConsoleParkIdentityCurrent(bool sameMachine, bool parked) noexcept { return sameMachine && parked; }
// The SDK owner keeps itself parked a little longer than the hold it serves, so
// a renewal that is on its way never finds the outputs already coming back; if
// no renewal comes the owner starts again by itself.
inline constexpr uint64_t qemuConsoleParkMarginMillis = 5000;

// hostAction 215: a console park. stage 1 parked (ok) or refused (error = why),
// stage 2 ended. `cause`: 1 nobody signed in at the console, 2 locked,
// 3 a standard user's session, 4 an administrator's session under a secure desktop.
inline constexpr uint32_t qemuConsoleParkAction = 215;
enum class QemuConsoleParkRefusal : uint32_t { none = 0, displayBusy = 1, cancelled = 2 };
inline uint32_t qemuConsoleParkCause(const QemuConsoleSessionFacts& facts) noexcept {
  return !facts.console ? 1u : !facts.unlocked ? 2u : !facts.elevated ? 3u : 4u;
}
}  // namespace host
