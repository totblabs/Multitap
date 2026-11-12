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
