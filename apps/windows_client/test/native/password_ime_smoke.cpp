#include <windows.h>
#include <imm.h>
#include <iostream>
#include "password_ime.h"

int main() {
  HWND window = CreateWindowExW(0, L"EDIT", L"Multitap isolated IME test",
      WS_OVERLAPPED, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!window) return 1;
  HIMC context = ImmCreateContext();
  if (!context) { DestroyWindow(window); return 2; }
  HIMC original = ImmAssociateContext(window, context);
  ImmSetOpenStatus(context, TRUE);
  ImmSetConversionStatus(context, IME_CMODE_NATIVE, 0);
  const BOOL initial_open = ImmGetOpenStatus(context);
  DWORD initial_conversion = 0, initial_sentence = 0;
  ImmGetConversionStatus(context, &initial_conversion, &initial_sentence);
  bool ok = true;
  {
    multitap::PasswordIme scope;
    scope.Set(window, true);
    HIMC disabled = ImmGetContext(window);
    if (disabled) { ok = false; ImmReleaseContext(window, disabled); }
    scope.Set(window, false);
    HIMC restored = ImmGetContext(window);
    DWORD conversion = 0, sentence = 0;
    if (!restored || restored != context) ok = false;
    if (restored) {
      ImmGetConversionStatus(restored, &conversion, &sentence);
      ok = ok && ImmGetOpenStatus(restored) == initial_open &&
          conversion == initial_conversion && sentence == initial_sentence;
      ImmReleaseContext(window, restored);
    }
    scope.Set(window, true);
    scope.Activate(window, false);
    HIMC background = ImmGetContext(window);
    ok = ok && background == context;
    if (background) ImmReleaseContext(window, background);
    scope.Activate(window, true);
    HIMC active = ImmGetContext(window);
    ok = ok && active == nullptr;
    if (active) ImmReleaseContext(window, active);
    // Contract only (it held before the Dart focus fix too): once Dart releases
    // password mode while in the background, coming back does not disable again.
    scope.Set(window, true);
    scope.Activate(window, false);
    scope.Set(window, false);
    scope.Activate(window, true);
    HIMC released = ImmGetContext(window);
    ok = ok && released == context;
    if (released) ImmReleaseContext(window, released);
  }
  {
    // Observe reports what the window has and repairs only outside password mode.
    multitap::PasswordIme scope;
    HIMC present = ImmGetContext(window);
    DWORD conversion = 0, sentence = 0;
    const bool was_open = present && ImmGetOpenStatus(present);
    if (present) {
      ImmGetConversionStatus(present, &conversion, &sentence);
      ImmReleaseContext(window, present);
    }
    const int healthy = scope.Observe(window);
    ok = ok && present && (healthy & 1) && !(healthy & (32 | 64 | 256)) &&
        ((healthy & 2) != 0) == was_open &&
        ((healthy & 4) != 0) == ((conversion & IME_CMODE_NATIVE) != 0);
    // Taken away from outside (not by the scope): reported missing, put back.
    // The repair associates the thread's default context, not `context`.
    ImmAssociateContext(window, nullptr);
    const int repaired = scope.Observe(window);
    ok = ok && !(repaired & 1) && (repaired & 256) && !(repaired & (2 | 4 | 32 | 64));
    HIMC back = ImmGetContext(window);
    ok = ok && back != nullptr;
    if (back) ImmReleaseContext(window, back);
    const int settled = scope.Observe(window);
    ok = ok && (settled & 1) && !(settled & 256);
    // A password field has no context on purpose: missing, but not repaired.
    scope.Set(window, true);
    const int password = scope.Observe(window);
    ok = ok && !(password & 1) && (password & 32) && (password & 64) && !(password & 256);
    HIMC held = ImmGetContext(window);
    ok = ok && held == nullptr;
    if (held) ImmReleaseContext(window, held);
    scope.Set(window, false);
    const int after = scope.Observe(window);
    ok = ok && (after & 1) && !(after & (32 | 64 | 256));
  }
  ImmAssociateContext(window, original);
  ImmDestroyContext(context);
  DestroyWindow(window);
  std::cout << (ok ? "Native IMM scope: disabled during password focus; restored on blur/background and after release; a context missing outside password mode is reported and put back.\n" : "Native IMM scope failed.\n");
  return ok ? 0 : 3;
}
