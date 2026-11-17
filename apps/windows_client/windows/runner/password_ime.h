#ifndef MULTITAP_PASSWORD_IME_H_
#define MULTITAP_PASSWORD_IME_H_

#include <windows.h>
#include <imm.h>

namespace multitap {

// IMM input contexts belong to the window/thread. Temporarily disassociate the
// context, then restore its original open/conversion state when focus leaves.
class PasswordIme {
 public:
  ~PasswordIme() { Restore(); }
  void Set(HWND window, bool enabled) {
    requested_ = enabled;
    if (enabled) Apply(window); else Restore();
  }
  void Activate(HWND window, bool active) {
    if (!active) Restore();
    else if (requested_) Apply(window);
  }
  // The IME state of the view as a bit mask. A wire format (multitap/input
  // inputMethodState, then the `error` of a signIn stage 3 diagnostics row):
  //     1  the view had an input context when asked
  //     2  IME open                    (0 without a context)
  //     4  Hangul / native mode        (0 without a context)
  //     8  the view has the Win32 keyboard focus
  //    16  the view's top-level window is the foreground window
  //    32  password mode requested
  //    64  the context is held away by password mode
  //   128  the thread's keyboard layout is Korean
  //   256  repaired: the context was missing and is back
  // Outside password mode the view must have its context, so one found missing
  // there is associated again (the thread's default one). A password field
  // has none on purpose: never repaired while requested or held.
  int Observe(HWND view) {
    int bits = (requested_ ? 32 : 0) | (held_ ? 64 : 0);
    if (!IsWindow(view)) return bits;
    if (HIMC context = ImmGetContext(view)) {
      DWORD conversion = 0, sentence = 0;
      bits |= 1;
      if (ImmGetOpenStatus(context)) bits |= 2;
      if (ImmGetConversionStatus(context, &conversion, &sentence) &&
          (conversion & IME_CMODE_NATIVE)) bits |= 4;
      ImmReleaseContext(view, context);
    } else if (!requested_ && !held_) {
      ImmAssociateContextEx(view, nullptr, IACE_DEFAULT);
      if (HIMC repaired = ImmGetContext(view)) {
        bits |= 256;
        ImmReleaseContext(view, repaired);
      }
    }
    if (GetFocus() == view) bits |= 8;
    HWND foreground = GetForegroundWindow();
    if (foreground &&
        GetAncestor(foreground, GA_ROOT) == GetAncestor(view, GA_ROOT)) bits |= 16;
    if (PRIMARYLANGID(LOWORD(reinterpret_cast<UINT_PTR>(GetKeyboardLayout(0)))) ==
        LANG_KOREAN) bits |= 128;
    return bits;
  }
 private:
  void Apply(HWND window) {
    if (held_ || !IsWindow(window)) return;
    HIMC context = ImmGetContext(window);
    if (!context) return;
    window_ = window;
    context_ = context;
    open_ = ImmGetOpenStatus(context);
    ImmGetConversionStatus(context, &conversion_, &sentence_);
    ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    ImmSetConversionStatus(context,
        conversion_ & ~(IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE), sentence_);
    ImmSetOpenStatus(context, FALSE);
    ImmReleaseContext(window, context);
    ImmAssociateContext(window, nullptr);
    held_ = true;
  }
  void Restore() {
    if (!held_) return;
    if (IsWindow(window_)) {
      ImmAssociateContext(window_, context_);
      ImmSetConversionStatus(context_, conversion_, sentence_);
      ImmSetOpenStatus(context_, open_);
    }
    held_ = false;
    context_ = nullptr;
  }
  bool requested_ = false, held_ = false;
  HWND window_ = nullptr;
  HIMC context_ = nullptr;
  BOOL open_ = FALSE;
  DWORD conversion_ = 0, sentence_ = 0;
};
}  // namespace multitap
#endif
