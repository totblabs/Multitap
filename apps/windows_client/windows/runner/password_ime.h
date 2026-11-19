#ifndef MULTITAP_PASSWORD_IME_H_
#define MULTITAP_PASSWORD_IME_H_

#include <windows.h>
#include <commctrl.h>
#include <imm.h>

namespace multitap {

// IMM input contexts belong to the window/thread. Temporarily disassociate the
// context, then restore its original open/conversion state when focus leaves.
class PasswordIme {
 public:
  ~PasswordIme() {
    Restore();
    if (watched_ && IsWindow(watched_))
      RemoveWindowSubclass(watched_, &PasswordIme::Watch, kWatchId);
  }
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
  // What the input method did with the view's keys since the previous
  // observation (seen through a subclass of the view, set at the first one):
  //  1024  a composition began or changed (WM_IME_STARTCOMPOSITION/COMPOSITION)
  //  2048  the IME took a key (WM_KEYDOWN with VK_PROCESSKEY)
  //  4096  a Latin letter arrived while the context said open and Hangul
  // Field records of 2026-11-19 (an account with a Korean ID): after a sign-out
  // the ID field reported open + Hangul yet typed Latin only; a plain Win32
  // window with this hold and the real Korean IME did not repeat it. These
  // say next time whether the keys went past the IME.
  // Outside password mode the view must have its context, so one found missing
  // there is associated again (the thread's default one). A password field
  // has none on purpose: never repaired while requested or held.
  int Observe(HWND view) {
    int bits = (requested_ ? 32 : 0) | (held_ ? 64 : 0);
    if (!IsWindow(view)) return bits;
    if (watched_ == view) bits |= seen_;
    else if (!watched_ &&
             SetWindowSubclass(view, &PasswordIme::Watch, kWatchId,
                               reinterpret_cast<DWORD_PTR>(this))) watched_ = view;
    seen_ = 0;
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
  static constexpr UINT_PTR kWatchId = 0x50494d31;  // 'PIM1'
  static LRESULT CALLBACK Watch(HWND view, UINT message, WPARAM wparam,
                                LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<PasswordIme*>(data);
    switch (message) {
      case WM_IME_STARTCOMPOSITION:
      case WM_IME_COMPOSITION:
        self->seen_ |= 1024;
        break;
      case WM_KEYDOWN:
        if (wparam == VK_PROCESSKEY) self->seen_ |= 2048;
        break;
      case WM_CHAR:
        if ((wparam >= 'a' && wparam <= 'z') || (wparam >= 'A' && wparam <= 'Z')) {
          if (HIMC context = ImmGetContext(view)) {
            DWORD conversion = 0, sentence = 0;
            if (ImmGetOpenStatus(context) &&
                ImmGetConversionStatus(context, &conversion, &sentence) &&
                (conversion & IME_CMODE_NATIVE)) self->seen_ |= 4096;
            ImmReleaseContext(view, context);
          }
        }
        break;
      case WM_NCDESTROY:
        RemoveWindowSubclass(view, &PasswordIme::Watch, kWatchId);
        if (self->watched_ == view) self->watched_ = nullptr;
        break;
    }
    return DefSubclassProc(view, message, wparam, lparam);
  }
  bool requested_ = false, held_ = false;
  HWND watched_ = nullptr;
  int seen_ = 0;
  HWND window_ = nullptr;
  HIMC context_ = nullptr;
  BOOL open_ = FALSE;
  DWORD conversion_ = 0, sentence_ = 0;
};
}  // namespace multitap
#endif
