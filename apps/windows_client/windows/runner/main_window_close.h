#ifndef MULTITAP_MAIN_WINDOW_CLOSE_H_
#define MULTITAP_MAIN_WINDOW_CLOSE_H_

#include <windows.h>

#include <functional>

namespace multitap::close {

// What the client's main window does when it is asked to close (closing
// goes to the tray). Two different requests reach it:
//  - kUserClose: the user's X, Alt+F4, the system menu or the taskbar menu.
//    Windows sends WM_SYSCOMMAND/SC_CLOSE for these. The window rests in the
//    notification area instead of ending the program.
//  - kQuitRequest: a plain WM_CLOSE with no SC_CLOSE before it: the client's
//    own 완전 종료 (tray menu, account menu), the installer's CloseMainWindow,
//    Task Manager's "End task". This is a request to exit.
enum class Origin { kUserClose, kQuitRequest };

enum class Action {
  kPass,        // not ours to decide: the window's ordinary handling follows
  kHideToTray,  // hide the window; the program, the sign-in and sessions stay
  kExit,        // end now, without a question (nothing would be lost)
  kAskApp,      // the app asks in its own dialog first
};

struct State {
  // The main client window with a notification-area icon capability. False for
  // remote session windows, Host settings and the isolated QA fixtures, which
  // keep their own close handling untouched.
  bool tray_enabled = false;
  // The icon is actually in the notification area right now. Without it a
  // hidden window could not be brought back, so X asks as before.
  bool tray_ready = false;
  // The app already allowed this close (its own confirmation ran).
  bool allowed = false;
  // Remote session windows are open: exiting would end those connections.
  bool sessions = false;
  // Windows is logging off or shutting down.
  bool session_ending = false;
};

inline Action Decide(Origin origin, const State& state) {
  if (!state.tray_enabled) return Action::kPass;
  // Windows never waits for a question: logoff and shutdown end quietly.
  if (state.session_ending) return Action::kPass;
  if (origin == Origin::kUserClose)
    return state.tray_ready ? Action::kHideToTray : Action::kAskApp;
  if (state.allowed) return Action::kPass;
  return state.sessions ? Action::kAskApp : Action::kExit;
}

// Windows asks every top-level window before logoff/shutdown. Answering TRUE
// and doing nothing else is "never block it". When the session really ends
// only the quick cleanup runs (the tray icon), never a dialog.
// Returns whether [message] was one of these two; [result] is then the answer.
inline bool HandleSessionEnd(UINT message, WPARAM wparam, bool& ending,
                             LRESULT& result,
                             const std::function<void()>& on_ending) {
  if (message == WM_QUERYENDSESSION) {
    result = TRUE;
    return true;
  }
  if (message == WM_ENDSESSION) {
    ending = wparam != FALSE;  // FALSE: another program vetoed it
    if (ending && on_ending) on_ending();
    result = 0;
    return true;
  }
  return false;
}

}  // namespace multitap::close

#endif  // MULTITAP_MAIN_WINDOW_CLOSE_H_
