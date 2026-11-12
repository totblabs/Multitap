// What the client's main window does for each way it can be asked to close
// (main_window_close.h): the user's X goes to the tray, a quit request exits
// (asking first only when remote connections would end), and Windows logoff
// and shutdown are never blocked and never ask anything.
#include "main_window_close.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace multitap::close;

void requireAt(bool ok, int line) {
  if (!ok) throw std::runtime_error("Main window close QA failed at line " + std::to_string(line));
}
#define require(ok) requireAt((ok), __LINE__)

State Main(bool tray_ready, bool allowed = false, bool sessions = false, bool ending = false) {
  State state;
  state.tray_enabled = true;
  state.tray_ready = tray_ready;
  state.allowed = allowed;
  state.sessions = sessions;
  state.session_ending = ending;
  return state;
}
}  // namespace

int main() {
  try {
    // X / Alt+F4 / system menu: to the tray, whatever else is going on.
    require(Decide(Origin::kUserClose, Main(true)) == Action::kHideToTray);
    require(Decide(Origin::kUserClose, Main(true, false, true)) == Action::kHideToTray);   // sessions stay open
    require(Decide(Origin::kUserClose, Main(true, true)) == Action::kHideToTray);          // even right after a quit allowance
    // No icon in the notification area (no Explorer): a hidden window could not
    // come back, so the app asks as it always did.
    require(Decide(Origin::kUserClose, Main(false)) == Action::kAskApp);
    require(Decide(Origin::kUserClose, Main(false, false, true)) == Action::kAskApp);

    // A quit request (tray / account menu 완전 종료, the installer, Task Manager).
    require(Decide(Origin::kQuitRequest, Main(true)) == Action::kExit);                    // nothing open: at once, no dialog
    require(Decide(Origin::kQuitRequest, Main(false)) == Action::kExit);                   // the icon does not matter
    require(Decide(Origin::kQuitRequest, Main(true, false, true)) == Action::kAskApp);     // connections would end: confirm
    require(Decide(Origin::kQuitRequest, Main(true, true, true)) == Action::kPass);        // the app already confirmed
    require(Decide(Origin::kQuitRequest, Main(true, true, false)) == Action::kPass);

    // Logoff and shutdown: never a question, never a tray detour.
    for (const auto origin : {Origin::kUserClose, Origin::kQuitRequest})
      for (const bool sessions : {false, true})
        for (const bool tray : {false, true})
          require(Decide(origin, Main(tray, false, sessions, true)) == Action::kPass);

    // Windows that are not the tray-enabled main window keep their own handling.
    State session_window;   // remote session windows, Host settings, QA fixtures
    for (const auto origin : {Origin::kUserClose, Origin::kQuitRequest}) {
      require(Decide(origin, session_window) == Action::kPass);
      session_window.tray_ready = true;
      session_window.sessions = true;
      require(Decide(origin, session_window) == Action::kPass);
    }

    // WM_QUERYENDSESSION / WM_ENDSESSION.
    bool ending = false;
    LRESULT result = 99;
    int cleanups = 0;
    const auto cleanup = [&] { ++cleanups; };
    require(HandleSessionEnd(WM_QUERYENDSESSION, 0, ending, result, cleanup) && result == TRUE && !ending && cleanups == 0);
    require(HandleSessionEnd(WM_QUERYENDSESSION, ENDSESSION_CLOSEAPP, ending, result, cleanup) && result == TRUE);
    require(HandleSessionEnd(WM_ENDSESSION, FALSE, ending, result, cleanup) && result == 0 && !ending && cleanups == 0);
    require(HandleSessionEnd(WM_ENDSESSION, TRUE, ending, result, cleanup) && result == 0 && ending && cleanups == 1);
    require(HandleSessionEnd(WM_ENDSESSION, FALSE, ending, result, cleanup) && !ending);   // cancelled later
    require(!HandleSessionEnd(WM_CLOSE, 0, ending, result, cleanup));
    require(!HandleSessionEnd(WM_SYSCOMMAND, SC_CLOSE, ending, result, cleanup));
    require(HandleSessionEnd(WM_ENDSESSION, TRUE, ending, result, nullptr) && ending);     // no cleanup callback is fine

    std::cout << "{\"passed\":true,\"closeToTray\":true,\"quitRequest\":true,\"shutdownNotBlocked\":true}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
