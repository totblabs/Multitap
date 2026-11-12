#pragma once
// The clipboard transfer notice outside the session window. When files move over the clipboard and the user is
// in another window (local Explorer, to paste), a small always-on-top card shows the same progress the session
// window shows at its center, and the session window's taskbar button carries the progress too.
//
// The card never takes focus (WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW: no taskbar button of its own), sits at the
// center of the work area of the monitor under the cursor, can be dragged, and hides itself while the
// session window is the foreground window (that window draws the same card inside itself).
#include <windows.h>
#include "transfer_notice_policy.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace multitap {

enum class TaskbarMode { none, normal, indeterminate, error };

// The progress on a window's taskbar button. The application uses ITaskbarList3; tests substitute a recorder.
class TaskbarProgress {
 public:
  virtual ~TaskbarProgress() = default;
  virtual void Set(HWND window, TaskbarMode mode, int percent) = 0;
};
std::unique_ptr<TaskbarProgress> CreateTaskbarProgress();

// Where the card goes: centered in `work_area`, clamped as far inside as it fits.
RECT NoticePlacement(const RECT& work_area, int width, int height, int margin);
// `rect` moved the least that puts it inside `work_area` (as far as it fits).
RECT KeepInside(RECT rect, const RECT& work_area);
// The card's size in pixels at `dpi` for this content (with 자세히 open or not), or for an otherwise empty one of `kind`.
SIZE NoticeSize(const TransferNoticeContent& content, bool expanded, UINT dpi);
SIZE NoticeSize(TransferNoticeContent::Kind kind, UINT dpi);
// The card drawn without any window (previews and tests): premultiplied BGRA rows, top down. `hot` 0 none, 1 취소,
// 2 닫기, 3 폴더 열기, 4 자세히 under the pointer.
bool RenderTransferNotice(const TransferNoticeContent& content, bool expanded, UINT dpi, std::vector<uint8_t>& pixels, SIZE& size,
                          int hot = 0);

// The system calls the card makes, replaceable in tests.
struct TransferNoticeAccess {
  // Whether `owner` (or a window it owns) is the foreground window.
  std::function<bool(HWND owner)> owner_foreground;
  // Native-only minimized/hidden proof. False is the safe default for a visible owner.
  std::function<bool(HWND owner)> owner_nonpainted;
  // Where the user is looking: the card opens on this point's monitor.
  std::function<POINT()> cursor;
  // Tests only: the dpi of the monitor instead of the real one.
  std::function<UINT()> dpi_override;
  // Tests only: every frame drawn, as premultiplied BGRA rows (top down), before it is handed to the window.
  std::function<void(const uint8_t* bits, int width, int height)> frame_sink;
};
struct TransferNoticeCallbacks {
  std::function<void(const NoticeStamp&,std::function<void(bool)>)> prepare;
  std::function<void(const NoticeStamp&,bool accepted)> presentation;
  // open: 폴더 열기 on a finished paste whose folder is known.
  std::function<void(const NoticeStamp&)> cancel,close,open;
};

class TransferNotice {
 public:
  explicit TransferNotice(std::unique_ptr<TaskbarProgress> taskbar, TransferNoticeAccess access = {});
  ~TransferNotice();
  TransferNotice(const TransferNotice&) = delete;
  TransferNotice& operator=(const TransferNotice&) = delete;
  // The one card of this process. Only explicit Acquire changes its owner;
  // content/taskbar/hide calls require that claim's native-issued lease.
  static TransferNotice& Shared();

  uint64_t Observe(HWND window,NoticeSurfaceObservers::Callback callback);
  bool Observes(HWND window,uint64_t token) const;
  NoticeSurfaceStamp Surface() const;
  NoticeSurfaceStamp SurfaceStatus();

  NoticeReply Acquire(HWND owner,std::string token,int generation,TransferNoticeCallbacks callbacks);
  NoticeReply Update(const NoticeIdentity& identity,const TransferNoticeContent& content);
  NoticeReply Hide(const NoticeIdentity& identity);
  NoticeReply SetTaskbar(const NoticeIdentity& identity,TaskbarMode mode,int percent);
  void Prepared(const NoticeStamp& state,bool prepared);
  // Legacy direct native test/consumer adapter. The Dart bridge never calls it;
  // it cannot preempt an active protocol lease.

  // Shows or updates the card for `owner`'s transfer. It appears only while `owner` is not the foreground window.
  // `on_cancel` runs when 취소 is pressed. 닫기 is available only after the transfer ends.
  void Show(HWND owner, const TransferNoticeContent& content, std::function<void()> on_cancel, std::function<void()> on_close);
  // The transfer ended (or the owner stopped caring): the card goes if it is this owner's.
  void Hide(HWND owner);
  void SetTaskbar(HWND owner, TaskbarMode mode, int percent);
  // The owner window is being destroyed: its card and taskbar progress go, and no callback runs afterwards.
  void Detach(HWND owner,uint64_t observerToken=0);

  // For tests: the card's window (null until first shown), whether it is on screen, and a re-evaluation of the
  // foreground rule that the timer otherwise does.
  HWND window() const;
  bool visible() const;
  void Refresh();
  // The card as drawn: what a test reads back (the pixel under a point is not needed; the state is).
  TransferNoticeContent content() const;
  bool dismissed() const;
  // Presses the buttons without a mouse (tests).
  void PressCancel();
  void PressClose();
  void PressOpen();
  void PressDetails();
  bool expanded() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace multitap
