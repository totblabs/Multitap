#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace multitap::clipboard {
// Metadata only. These hooks never open/read the clipboard or fetch a format.
struct ChangeWatchAccess {
  std::function<bool(HWND)> add = [](HWND window) { return AddClipboardFormatListener(window) != FALSE; };
  std::function<void(HWND)> remove = [](HWND window) { RemoveClipboardFormatListener(window); };
  std::function<DWORD()> sequence = [] { return GetClipboardSequenceNumber(); };
  std::function<HWND()> owner = [] { return GetClipboardOwner(); };
};
class ChangeWatch {
 public:
  using Check = std::function<bool()>;
  using Notify = std::function<void(const std::string&, DWORD)>;
  ChangeWatch(Check check, Notify notify, ChangeWatchAccess access = {})
      : check_(std::move(check)), notify_(std::move(notify)), access_(std::move(access)) {}
  ~ChangeWatch() { close(); }
  ChangeWatch(const ChangeWatch&) = delete;
  std::optional<DWORD> enable(HWND window, const std::string& owner) {
    try { if (!window || owner.empty() || !check_()) { close(); return {}; } }
    catch (...) { close(); return {}; }
    try {
      if (window_ == window && owner_ == owner) return access_.sequence();
      close();
      if (!access_.add(window)) return {};
      window_ = window; owner_ = owner;
      last_ = access_.sequence();
      return last_;
    } catch (...) { close(); return {}; }
  }
  void disable(const std::string& owner) { if (owner == owner_) close(); }
  void close() {
    if (window_) { const HWND previous = window_; window_ = nullptr; try { access_.remove(previous); } catch (...) {} }
    owner_.clear(); last_.reset(); applied_.reset();
  }
  bool validate() { try { if (window_ && !check_()) close(); } catch (...) { close(); } return window_ != nullptr; }
  bool active() const { return window_ != nullptr; }
  DWORD lastSequence() const { return last_.value_or(0); }
  void applied(DWORD sequence) { applied_ = sequence; }
  // The window this process passes to OpenClipboard when it publishes content
  // received from the peer. While that window still owns the clipboard, the
  // content is the peer's own data and must never be announced back to it.
  void writer(HWND window) { writer_.store(window); }
  // Content published as an OLE data object (virtual files for a paste-pull offer): no window of ours owns the
  // clipboard then, so the publisher says which sequence is its own. Set once, before the watch is used.
  void ownContent(std::function<bool(DWORD)> own) { own_ = std::move(own); }
  // A change that was seen and deliberately not announced: kSkipAppliedOwn, kSkipWriterOwner or kSkipOwnOffer.
  // Diagnostics only; set once, before the watch is used.
  static constexpr uint32_t kSkipAppliedOwn = 1, kSkipWriterOwner = 2, kSkipOwnOffer = 3;
  void onSkip(std::function<void(uint32_t)> skipped) { skipped_ = std::move(skipped); }
  // Whether the clipboard holds our own write now, by the test changed() applies to a notification. The sequence enable() hands back
  // is otherwise taken for a copy made on this PC while the window was away (the VM's copy published here, a placeholder for it).
  bool ownCurrent() {
    if (const HWND own = writer_.load(); own && ownerIs(own)) return true;
    try { return own_ && own_(access_.sequence()); } catch (...) { return false; }
  }
  void changed() {
    if (!validate()) return;
    const DWORD sequence = access_.sequence();
    if (last_ && *last_ == sequence) return;
    last_ = sequence;
    if (applied_) { const bool own = *applied_ == sequence; applied_.reset(); if (own) { skip(kSkipAppliedOwn); return; } }
    // The applied sequence alone is not enough: the number read here can be
    // newer than the one sampled inside our own write (another listener adding
    // a format, the shell, a late notification), and an unrecognised own write
    // echoes back to the peer, which echoes it back again.
    if (const HWND own = writer_.load(); own && ownerIs(own)) { skip(kSkipWriterOwner); return; }
    if (own_ && own_(sequence)) { skip(kSkipOwnOffer); return; }
    notify_(owner_, sequence);
  }
 private:
  void skip(uint32_t reason) { if (skipped_) try { skipped_(reason); } catch (...) {} }
  std::function<void(uint32_t)> skipped_;
  bool ownerIs(HWND window) { try { return access_.owner() == window; } catch (...) { return false; } }
  Check check_; Notify notify_; ChangeWatchAccess access_;
  HWND window_ = nullptr; std::string owner_;
  std::optional<DWORD> last_, applied_;
  std::atomic<HWND> writer_{nullptr};
  std::function<bool(DWORD)> own_;
};
}
