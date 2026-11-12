#pragma once
// cursor-v1 (docs/remote-session-protocol.md, "Candidate cursor separation"):
// the host leaves the cursor out of the video and sends shape/state records;
// this presenter shows the remote shape as the real local Windows cursor over
// the streaming picture. Pure Win32 + std (no Flutter types) so a native test
// can drive it with faked cursor calls. ASCII only.
//
// Rule: the picture's Flutter MouseRegion asks for the `none` cursor exactly
// while the remote view owns input (captured). The presenter subclasses the
// Flutter view window; after Flutter has handled WM_SETCURSOR (and on a 50 ms
// timer while presenting) it replaces only that `none` cursor, or a cursor it
// set itself, with the remote shape (or keeps it hidden when the remote cursor
// is hidden), and only while keyboard capture is active. Any other cursor
// Flutter chose (toolbar, letterbox, dialogs) is left untouched, so there is
// never a second or invented cursor. No ShowCursor counter, no system cursor
// replacement, no hooks. While Flutter's last answer (under 250 ms old) was
// `none` and our cursor is still current, WM_SETCURSOR keeps the remote shape
// without letting Flutter hide it first, so a pointer move never flickers.
#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "remote_cursor.hpp"

namespace multitap {

inline constexpr uint8_t kCursorShapeRecord = 1, kCursorStateRecord = 2;
inline constexpr size_t kCursorRecordHeader = 16;
inline constexpr size_t kCursorCacheLimit = 64;
inline constexpr uint32_t kCursorMaxShapeId = 2147483647u;
inline constexpr UINT_PTR kCursorTimer = 19042;
inline constexpr UINT kCursorTimerMs = 50;
// A position record moves the pointer only after this much local stillness.
inline constexpr ULONGLONG kCursorWarpQuietMs = 50;
// Flutter's own WM_SETCURSOR choice is re-read at least this often.
inline constexpr ULONGLONG kCursorFlutterRecheckMs = 250;

struct CursorShapeRecord {
  uint32_t id = 0;
  RemoteCursorShape shape;
};
struct CursorStateRecord {
  bool visible = false, position = false;
  uint32_t id = 0;
  int32_t x = 0, y = 0;
};
struct CursorRecord {
  bool isShape = false;
  CursorShapeRecord shape;
  CursorStateRecord state;
};
// Strict parse of one record (the payload after the 12-byte media header).
std::optional<CursorRecord> ParseCursorRecord(const uint8_t* bytes, size_t size);

// The picture in logical (Flutter) pixels of the view's client area and the
// remote source size the host's positions refer to.
struct CursorPicture {
  double left = 0, top = 0, width = 0, height = 0;
  uint32_t sourceWidth = 0, sourceHeight = 0;
};
bool ValidCursorPicture(const CursorPicture& picture);

// Nearest-neighbour copy for a displayed-picture/source ratio; exact (the
// same shape) when the ratio is within 5 %. At most 256 x 256.
SIZE ScaledCursorSize(unsigned width, unsigned height, double scale);
RemoteCursorShape ScaleCursorShape(const RemoteCursorShape& shape, double scale);

// Operating-system calls, replaceable by the native test.
struct CursorOs {
  HCURSOR(WINAPI* get)() = ::GetCursor;
  HCURSOR(WINAPI* set)(HCURSOR) = ::SetCursor;
  BOOL(WINAPI* position)(LPPOINT) = ::GetCursorPos;
  BOOL(WINAPI* move)(int, int) = ::SetCursorPos;
  HWND(WINAPI* window)(POINT) = ::WindowFromPoint;
  ULONGLONG(WINAPI* ticks)() = ::GetTickCount64;
  UINT(WINAPI* dpi)(HWND) = ::GetDpiForWindow;
  BOOL(WINAPI* toScreen)(HWND, LPPOINT) = ::ClientToScreen;
  HCURSOR (*create)(const RemoteCursorShape&) noexcept = CreateRemoteCursorHandle;
};

// One UI thread owns and destroys this object. The caller checks that a
// record's media lease is the current one; a new lease resets all state.
class CursorPresenter {
 public:
  using CaptureProbe = std::function<bool()>;
  // cursor-echo-v1: holds this PC's pointer on a screen pixel while 게임 마우스
  // is engaged pinned (RelativeMouse::Pin); false when not pinned.
  using Pin = std::function<bool(POINT screen)>;
  explicit CursorPresenter(CaptureProbe captured, CursorOs os = {});
  ~CursorPresenter();
  CursorPresenter(const CursorPresenter&) = delete;
  CursorPresenter& operator=(const CursorPresenter&) = delete;

  // Subclasses the Flutter view window (one at a time).
  bool Attach(HWND view);
  // Reset() plus removal of the subclass and timer (window destruction).
  void Detach();
  // One cursor record of media |epoch|. S_OK applied, S_FALSE stale epoch,
  // E_INVALIDARG malformed / out of sequence. A valid visible state whose
  // native handle cannot be created is retained for retry, reports failure
  // and uses a visible local fallback; it is never acknowledged as ready.
  HRESULT Apply(int64_t lease, uint32_t epoch, const uint8_t* bytes, size_t size);
  HRESULT SetPicture(int64_t lease, const CursorPicture& picture);
  // Re-evaluates presentation after capture changes.
  void Refresh();
  // relative-mouse-v1: capture ended, the pointer continues from the last
  // position the remote reported, if one is known.
  void MoveToRemote();
  // cursor-echo-v1: while |pin| holds the pointer, every position the remote
  // reports moves it there at once (no stillness wait); otherwise positions
  // warp as before.
  void SetPin(Pin pin) { pin_ = std::move(pin); }
  // cursor-compose-v1: true while 게임 마우스 holds the mouse with the VM's
  // cursor drawn into the video (RelativeMouse::concealed). Meanwhile this
  // presenter shows no cursor at all over the picture, whatever the records
  // say, and positions move nothing (they are only remembered for
  // MoveToRemote). Call Refresh() after it changes.
  using Conceal = std::function<bool()>;
  void SetConceal(Conceal conceal) { conceal_ = std::move(conceal); }
  // Session stop/start/disconnect: destroys every shape; a presenter cursor
  // that is still current becomes our own arrow copy until Flutter sets its own.
  // An owned hidden cursor over this view is also restored without mouse input.
  void Reset();
  // The cursor rule (WM_SETCURSOR and timer). True when the remote state was
  // applied or kept, false when Flutter's cursor stays untouched.
  bool Present();

  // Inspection for tests and diagnostics.
  int64_t lease() const { return lease_; }
  bool visible() const { return visible_; }
  uint32_t shapeId() const { return id_; }
  size_t cachedShapes() const { return entries_.size(); }
  bool timerRunning() const { return timer_; }
  bool Owns(HCURSOR cursor) const;
  bool Cached(uint32_t id) const;
  uint64_t warps() const { return warps_; }
  uint64_t pins() const { return pins_; }
  HWND view() const { return view_; }
  // For the subclass procedure (exposed for tests that send real messages).
  static LRESULT CALLBACK SubclassProc(HWND window, UINT message, WPARAM wparam,
                                       LPARAM lparam, UINT_PTR id, DWORD_PTR data);

 private:
  struct Entry {
    uint32_t id = 0;
    RemoteCursorShape shape;
    HCURSOR handle = nullptr;
    uint16_t width = 0, height = 0;  // size the handle was built at
    uint64_t used = 0;
  };
  Entry* Find(uint32_t id);
  HCURSOR Handle(uint32_t id);
  double Scale() const;
  bool Presenting() const;
  bool Concealed() const { return conceal_ && conceal_(); }
  HCURSOR LocalFallback();
  void DestroyFallback();
  void RestoreLocal();
  void Warp(int32_t x, int32_t y);
  bool PinTo(int32_t x, int32_t y);
  // The client pixel of source pixel (x, y) in the picture (the inverse of the
  // input mapping) and the picture's client pixel rectangle.
  bool Map(int32_t x, int32_t y, POINT& client, RECT& rect) const;
  void MoveTo(int32_t x, int32_t y, ULONGLONG now);
  void LocalMove(LPARAM lparam);
  void StartTimer();
  void StopTimer();
  void Destroy(Entry& entry);

  CaptureProbe captured_;
  CursorOs os_;
  Pin pin_;
  Conceal conceal_;
  uint64_t pins_ = 0;
  const DWORD owner_ = GetCurrentThreadId();
  HWND view_ = nullptr;
  int64_t lease_ = 0;
  uint32_t epoch_ = 0;
  bool epochSeen_ = false;
  uint32_t lastShapeId_ = 0, id_ = 0;
  bool visible_ = false, timer_ = false;
  // No state is different from an explicit remote-hidden state. Only a
  // null cursor we presented over our own view belongs to this presenter.
  bool stateSeen_ = false, ownsHidden_ = false;
  // A shared stock arrow cannot identify our restoration: Flutter may choose
  // that same stock cursor for a toolbar or modal. Keep a distinct owned copy
  // across capture/lease resets so re-entry can replace only our own fallback.
  HCURSOR fallback_ = nullptr;
  bool fallbackAttempted_ = false;
  std::optional<CursorPicture> picture_;
  std::vector<Entry> entries_;
  uint64_t clock_ = 0, warps_ = 0;
  ULONGLONG lastLocalMove_ = 0, warpAt_ = 0, flutterAt_ = 0;
  bool localMoveSeen_ = false, flutterNone_ = false;
  std::optional<POINT> warpClient_, remote_;
};

}  // namespace multitap
