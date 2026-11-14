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
  ImmAssociateContext(window, original);
  ImmDestroyContext(context);
  DestroyWindow(window);
  std::cout << (ok ? "Native IMM scope: disabled during password focus; restored on blur/background and after release.\n" : "Native IMM scope failed.\n");
  return ok ? 0 : 3;
}
