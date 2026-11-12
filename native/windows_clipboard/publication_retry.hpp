#pragma once
#include <windows.h>
#include <ole2.h>
#include <optional>
#include <stdexcept>

namespace multitap::clipboard::detail {
// OLE cannot publish while another process has the clipboard open. A newer user
// copy made during the wait must invalidate every subsequent attempt.
template<class Sequence, class Publish, class Pause>
HRESULT retryPublication(std::optional<DWORD> expected, unsigned budgetMs,
                         Sequence sequence, Publish publish, Pause pause) {
  for (unsigned waited = 0, attempt = 0;; ++attempt) {
    if (expected && sequence() != *expected) throw std::runtime_error("clipboard_changed");
    const HRESULT result = publish();
    if (result != CLIPBRD_E_CANT_OPEN || waited >= budgetMs) return result;
    const unsigned step = attempt < 10 ? 10 : 25;
    pause(step); waited += step;
  }
}
}
