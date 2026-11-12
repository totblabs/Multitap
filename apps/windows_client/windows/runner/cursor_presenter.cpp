#include "cursor_presenter.h"
#include "video_bounds.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace multitap {
namespace {
constexpr UINT_PTR kSubclassId = 0x54435631;  // 'TCV1'
uint16_t U16(const uint8_t* bytes, size_t at) { return uint16_t(bytes[at] | (bytes[at + 1] << 8)); }
uint32_t U32(const uint8_t* bytes, size_t at) {
  return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) | (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24);
}
int32_t I32(const uint8_t* bytes, size_t at) {
  const uint32_t value = U32(bytes, at);
  int32_t result = 0;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}
bool Bit(const std::vector<uint8_t>& mask, size_t stride, unsigned row, unsigned x) {
  return (mask[size_t(row) * stride + x / 8] & uint8_t(0x80u >> (x % 8))) != 0;
}
void SetBit(std::vector<uint8_t>& mask, size_t stride, unsigned row, unsigned x) {
  mask[size_t(row) * stride + x / 8] |= uint8_t(0x80u >> (x % 8));
}
}  // namespace

std::optional<CursorRecord> ParseCursorRecord(const uint8_t* bytes, size_t size) {
  if (!bytes || size < kCursorRecordHeader || bytes[2] || bytes[3]) return std::nullopt;
  CursorRecord record;
  if (bytes[0] == kCursorStateRecord) {
    if (size != kCursorRecordHeader || (bytes[1] & ~3u)) return std::nullopt;
    auto& state = record.state;
    state.visible = (bytes[1] & 1) != 0;
    state.position = (bytes[1] & 2) != 0;
    state.id = U32(bytes, 4);
    state.x = I32(bytes, 8);
    state.y = I32(bytes, 12);
    if (state.visible ? (state.id == 0 || state.id > kCursorMaxShapeId) : state.id != 0) return std::nullopt;
    if (state.position ? (state.x < 0 || state.y < 0) : (state.x || state.y)) return std::nullopt;
    return record;
  }
  if (bytes[0] != kCursorShapeRecord || (bytes[1] != 1 && bytes[1] != 2)) return std::nullopt;
  record.isShape = true;
  auto& shape = record.shape;
  shape.id = U32(bytes, 4);
  const unsigned width = U16(bytes, 8), height = U16(bytes, 10);
  if (shape.id == 0 || shape.id > kCursorMaxShapeId || width < 1 || height < 1 || width > 256 || height > 256) return std::nullopt;
  const bool mono = bytes[1] == 2;
  const size_t stride = RemoteCursorMaskStride(width);
  const size_t mask = stride * height * (mono ? 2 : 1), color = mono ? 0 : size_t(width) * height * 4;
  if (size != kCursorRecordHeader + mask + color) return std::nullopt;
  shape.shape.format = mono ? RemoteCursorFormat::MonochromeAndXor : RemoteCursorFormat::ColorBgraAnd;
  shape.shape.width = uint16_t(width);
  shape.shape.height = uint16_t(height);
  shape.shape.hotspotX = U16(bytes, 12);
  shape.shape.hotspotY = U16(bytes, 14);
  shape.shape.mask.assign(bytes + kCursorRecordHeader, bytes + kCursorRecordHeader + mask);
  if (!mono) shape.shape.color.assign(bytes + kCursorRecordHeader + mask, bytes + size);
  // Hotspot bounds, exact sizes and zero padding bits.
  if (!validRemoteCursorShape(shape.shape)) return std::nullopt;
  return record;
}

bool ValidCursorPicture(const CursorPicture& p) {
  constexpr double limit = double(std::numeric_limits<LONG>::max());
  video::FrameLayout source{};
  return std::isfinite(p.left) && std::isfinite(p.top) && std::isfinite(p.width) && std::isfinite(p.height) &&
         std::fabs(p.left) <= limit && std::fabs(p.top) <= limit && p.width > 0 && p.height > 0 &&
         p.width <= limit && p.height <= limit && p.sourceWidth >= 1 && p.sourceHeight >= 1 &&
          video::frameLayout(p.sourceWidth,p.sourceHeight,source,false);
}

SIZE ScaledCursorSize(unsigned w, unsigned h, double scale) {
  if (!w || !h || !std::isfinite(scale) || scale <= 0 || std::fabs(scale - 1.0) <= 0.05) return SIZE{LONG(w), LONG(h)};
  scale = (std::min)(scale, 256.0 / double((std::max)(w, h)));
  return SIZE{std::clamp<LONG>(std::lround(w * scale), 1, 256), std::clamp<LONG>(std::lround(h * scale), 1, 256)};
}

RemoteCursorShape ScaleCursorShape(const RemoteCursorShape& shape, double scale) {
  const unsigned w = shape.width, h = shape.height;
  const SIZE target = ScaledCursorSize(w, h, scale);
  const unsigned tw = unsigned(target.cx), th = unsigned(target.cy);
  if (tw == w && th == h) return shape;
  RemoteCursorShape out;
  out.format = shape.format;
  out.width = uint16_t(tw);
  out.height = uint16_t(th);
  out.hotspotX = uint16_t((std::min)(unsigned(double(shape.hotspotX) * tw / w), tw - 1));
  out.hotspotY = uint16_t((std::min)(unsigned(double(shape.hotspotY) * th / h), th - 1));
  const bool mono = shape.format == RemoteCursorFormat::MonochromeAndXor;
  const size_t sourceStride = RemoteCursorMaskStride(w), stride = RemoteCursorMaskStride(tw);
  out.mask.assign(stride * th * (mono ? 2 : 1), 0);
  if (!mono) out.color.assign(size_t(tw) * th * 4, 0);
  for (unsigned y = 0; y < th; ++y) {
    const unsigned sy = (std::min)(h - 1, unsigned((y + 0.5) * h / th));
    for (unsigned x = 0; x < tw; ++x) {
      const unsigned sx = (std::min)(w - 1, unsigned((x + 0.5) * w / tw));
      if (Bit(shape.mask, sourceStride, sy, sx)) SetBit(out.mask, stride, y, x);
      if (mono) {
        if (Bit(shape.mask, sourceStride, sy + h, sx)) SetBit(out.mask, stride, y + th, x);
      } else {
        std::memcpy(out.color.data() + (size_t(y) * tw + x) * 4, shape.color.data() + (size_t(sy) * w + sx) * 4, 4);
      }
    }
  }
  return out;
}

CursorPresenter::CursorPresenter(CaptureProbe captured, CursorOs os)
    : captured_(std::move(captured)), os_(os) {}

CursorPresenter::~CursorPresenter() { Detach(); }

bool CursorPresenter::Attach(HWND view) {
  if (GetCurrentThreadId() != owner_ || !view) return false;
  if (view_ == view) return true;
  Detach();
  if (!SetWindowSubclass(view, SubclassProc, kSubclassId, reinterpret_cast<DWORD_PTR>(this))) return false;
  view_ = view;
  return true;
}

void CursorPresenter::Detach() {
  Reset();
  StopTimer();
  if (view_) {
    if (IsWindow(view_)) RemoveWindowSubclass(view_, SubclassProc, kSubclassId);
    view_ = nullptr;
  }
  DestroyFallback();
}

void CursorPresenter::Destroy(Entry& entry) {
  if (!entry.handle) return;
  // Never destroy the cursor Windows is showing for this thread.
  if (os_.get() == entry.handle) os_.set(LocalFallback());
  DestroyIcon(reinterpret_cast<HICON>(entry.handle));
  entry.handle = nullptr;
}

void CursorPresenter::Reset() {
  StopTimer();
  RestoreLocal();
  for (auto& entry : entries_) Destroy(entry);
  entries_.clear();
  lease_ = 0;
  epoch_ = 0;
  epochSeen_ = false;
  lastShapeId_ = id_ = 0;
  visible_ = false;
  stateSeen_ = ownsHidden_ = false;
  warpClient_.reset();
  remote_.reset();
  localMoveSeen_ = flutterNone_ = false;
}

bool CursorPresenter::Owns(HCURSOR cursor) const {
  return cursor && (cursor == fallback_ ||
      std::any_of(entries_.begin(), entries_.end(), [cursor](const Entry& e) { return e.handle == cursor; }));
}

bool CursorPresenter::Cached(uint32_t id) const {
  return std::any_of(entries_.begin(), entries_.end(), [id](const Entry& e) { return e.id == id; });
}

CursorPresenter::Entry* CursorPresenter::Find(uint32_t id) {
  for (auto& entry : entries_) if (entry.id == id) return &entry;
  return nullptr;
}

double CursorPresenter::Scale() const {
  if (!picture_ || !view_) return 1.0;
  UINT dpi = os_.dpi(view_);
  if (dpi < 24 || dpi > 2400) dpi = 96;
  return picture_->width * double(dpi) / 96.0 / double(picture_->sourceWidth);
}

HCURSOR CursorPresenter::Handle(uint32_t id) {
  auto* entry = Find(id);
  if (!entry) return nullptr;
  entry->used = ++clock_;
  const double scale = Scale();
  const SIZE size = ScaledCursorSize(entry->shape.width, entry->shape.height, scale);
  if (entry->handle && entry->width == size.cx && entry->height == size.cy) return entry->handle;
  const auto scaled = ScaleCursorShape(entry->shape, scale);
  const HCURSOR created = os_.create ? os_.create(scaled) : nullptr;
  if (!created) return entry->handle;  // keep the previous size rather than nothing
  const HCURSOR previous = entry->handle;
  entry->handle = created;
  entry->width = scaled.width;
  entry->height = scaled.height;
  if (previous) {
    if (os_.get() == previous) os_.set(created);
    DestroyIcon(reinterpret_cast<HICON>(previous));
  }
  return created;
}

bool CursorPresenter::Presenting() const {
  return view_ && lease_ > 0 && captured_ && captured_();
}

HCURSOR CursorPresenter::LocalFallback() {
  if (fallback_) return fallback_;
  const HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
  if (arrow && !fallbackAttempted_) {
    fallbackAttempted_ = true;
    const HCURSOR copy = reinterpret_cast<HCURSOR>(CopyIcon(reinterpret_cast<HICON>(arrow)));
    // Never claim or destroy a shared cursor, including an unexpected copy
    // failure. A stock arrow stays visible but belongs to Flutter/Windows.
    if (copy && copy != arrow) fallback_ = copy;
  }
  return fallback_ ? fallback_ : arrow;
}

void CursorPresenter::DestroyFallback() {
  if (!fallback_) { fallbackAttempted_ = false; return; }
  // WM_NCDESTROY can already have cleared view_. Unselect independently of
  // that HWND, and never destroy a handle that Windows is still showing.
  if (os_.get() == fallback_) os_.set(LoadCursorW(nullptr, IDC_ARROW));
  if (os_.get() == fallback_) return;
  DestroyIcon(reinterpret_cast<HICON>(fallback_));
  fallback_ = nullptr;
  fallbackAttempted_ = false;
}

void CursorPresenter::RestoreLocal() {
  POINT point{};
  if (view_ && os_.position(&point) && os_.window(point) == view_) {
    const HCURSOR now = os_.get();
    if (Owns(now) || (!now && ownsHidden_)) {
      os_.set(LocalFallback());
    }
  }
  ownsHidden_ = false;
}

bool CursorPresenter::Present() {
  if (GetCurrentThreadId() != owner_ || !Presenting()) return false;
  POINT point{};
  if (!os_.position(&point) || os_.window(point) != view_) return false;
  const HCURSOR now = os_.get();
  // Flutter chose a real cursor here (toolbar, letterbox, not captured):
  // only its `none` or a cursor of ours may be replaced.
  if (now && !Owns(now)) {
    ownsHidden_ = false;
    return false;
  }
  // cursor-compose-v1: the VM's cursor is in the video; nothing here.
  if (Concealed()) {
    if (os_.get() != nullptr) os_.set(nullptr);
    ownsHidden_ = true;
    return true;
  }
  HCURSOR desired = visible_ ? Handle(id_) : nullptr;
  const bool unavailable = !stateSeen_ || (visible_ && !desired);
  if (unavailable) desired = LocalFallback();
  if (os_.get() != desired) os_.set(desired);
  ownsHidden_ = !unavailable && !visible_;
  return true;
}

void CursorPresenter::StartTimer() {
  if (timer_ || !view_) return;
  timer_ = SetTimer(view_, kCursorTimer, kCursorTimerMs, nullptr) != 0;
}

void CursorPresenter::StopTimer() {
  if (timer_ && view_ && IsWindow(view_)) KillTimer(view_, kCursorTimer);
  timer_ = false;
}

void CursorPresenter::Refresh() {
  if (GetCurrentThreadId() != owner_) return;
  if (!Presenting()) {
    StopTimer();
    RestoreLocal();
    return;
  }
  if (Presenting() && (Concealed() || (visible_ && Cached(id_)))) StartTimer(); else StopTimer();
  Present();
}

HRESULT CursorPresenter::SetPicture(int64_t lease, const CursorPicture& picture) {
  if (GetCurrentThreadId() != owner_ || lease < 1 || !ValidCursorPicture(picture)) return E_INVALIDARG;
  if (lease != lease_) { Reset(); lease_ = lease; }
  picture_ = picture;
  Refresh();
  return S_OK;
}

HRESULT CursorPresenter::Apply(int64_t lease, uint32_t epoch, const uint8_t* bytes, size_t size) {
  if (GetCurrentThreadId() != owner_ || lease < 1 || epoch > 65535) return E_INVALIDARG;
  const auto record = ParseCursorRecord(bytes, size);
  if (!record) return E_INVALIDARG;
  if (lease != lease_) { Reset(); lease_ = lease; }
  if (epochSeen_ && epoch < epoch_) return S_FALSE;  // a retired capture epoch
  if (record->isShape) {
    // Ids strictly increase within a session (across epochs).
    if (record->shape.id <= lastShapeId_) return E_INVALIDARG;
    epoch_ = epoch;
    epochSeen_ = true;
    lastShapeId_ = record->shape.id;
    while (entries_.size() >= kCursorCacheLimit) {
      auto victim = entries_.end();
      for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (it->id != id_ && (victim == entries_.end() || it->used < victim->used)) victim = it;
      if (victim == entries_.end()) break;
      Destroy(*victim);
      entries_.erase(victim);
    }
    Entry entry;
    entry.id = record->shape.id;
    entry.shape = record->shape.shape;
    entry.used = ++clock_;
    entries_.push_back(std::move(entry));
    return S_OK;
  }
  const auto& state = record->state;
  if (state.visible && state.id > lastShapeId_) return E_INVALIDARG;  // never sent
  if (state.position && picture_ &&
      (uint32_t(state.x) >= picture_->sourceWidth || uint32_t(state.y) >= picture_->sourceHeight)) return E_INVALIDARG;
  epoch_ = epoch;
  epochSeen_ = true;
  visible_ = state.visible;
  id_ = state.id;
  stateSeen_ = true;
  if (auto* entry = Find(id_)) entry->used = ++clock_;
  // A valid state is not proof that Windows actually created its cursor.
  // Keep the record/cache for a later retry, but acknowledge a visible state
  // only when its native handle exists. Explicit hidden needs no handle.
  const bool available = !visible_ || Handle(id_) != nullptr;
  if (state.position) {
    remote_ = POINT{state.x, state.y};
    // cursor-echo-v1: a pinned 게임 마우스 pointer goes to every position at
    // once; otherwise a position is an application warp, applied at rest.
    // cursor-compose-v1: the parked pointer stays; the position waits for
    // MoveToRemote at the release.
    if (!Concealed() && !PinTo(state.x, state.y)) Warp(state.x, state.y);
  }
  Refresh();
  return available ? S_OK : (Cached(id_) ? E_OUTOFMEMORY : E_INVALIDARG);
}

void CursorPresenter::Warp(int32_t x, int32_t y) {
  if (!Presenting()) return;
  const ULONGLONG now = os_.ticks();
  if (localMoveSeen_ && now - lastLocalMove_ < kCursorWarpQuietMs) return;  // the user is moving
  MoveTo(x, y, now);
}

bool CursorPresenter::PinTo(int32_t x, int32_t y) {
  if (!pin_ || !Presenting()) return false;
  POINT client{}, origin{};
  RECT rect{};
  if (!Map(x, y, client, rect) || !os_.toScreen(view_, &origin)) return false;
  if (!pin_(POINT{origin.x + client.x, origin.y + client.y})) return false;
  // The move the clip makes is ours, not the user's.
  warpClient_ = client;
  warpAt_ = os_.ticks();
  ++pins_;
  return true;
}

void CursorPresenter::MoveToRemote() {
  if (GetCurrentThreadId() != owner_) return;
  if (remote_) MoveTo(remote_->x, remote_->y, os_.ticks());
  // A concealed capture just ended: show the separated cursor again at once.
  Refresh();
}

bool CursorPresenter::Map(int32_t x, int32_t y, POINT& client, RECT& rect) const {
  if (!picture_ || !view_) return false;
  const auto& picture = *picture_;
  if (uint32_t(x) >= picture.sourceWidth || uint32_t(y) >= picture.sourceHeight) return false;
  UINT dpi = os_.dpi(view_);
  if (dpi < 24 || dpi > 2400) dpi = 96;
  const double s = double(dpi) / 96.0;
  const double left = picture.left * s, top = picture.top * s, width = picture.width * s, height = picture.height * s;
  rect = RECT{LONG(std::ceil(left)), LONG(std::ceil(top)), LONG(std::floor(left + width)) - 1, LONG(std::floor(top + height)) - 1};
  if (rect.right < rect.left || rect.bottom < rect.top) return false;
  // Inverse of the input mapping: x / (sourceWidth - 1) of the picture.
  const double nx = picture.sourceWidth > 1 ? double(x) / double(picture.sourceWidth - 1) : 0.0;
  const double ny = picture.sourceHeight > 1 ? double(y) / double(picture.sourceHeight - 1) : 0.0;
  client = POINT{std::clamp(LONG(std::lround(left + nx * width)), rect.left, rect.right),
                 std::clamp(LONG(std::lround(top + ny * height)), rect.top, rect.bottom)};
  return true;
}

void CursorPresenter::MoveTo(int32_t x, int32_t y, ULONGLONG now) {
  POINT client{};
  RECT rect{};
  if (!Map(x, y, client, rect)) return;
  POINT pointer{};
  if (!os_.position(&pointer) || os_.window(pointer) != view_) return;
  POINT origin{};
  if (!os_.toScreen(view_, &origin)) return;
  const LONG pointerX = pointer.x - origin.x, pointerY = pointer.y - origin.y;
  if (pointerX < rect.left || pointerX > rect.right || pointerY < rect.top || pointerY > rect.bottom) return;
  if (client.x == pointerX && client.y == pointerY) return;
  if (!os_.move(int(origin.x + client.x), int(origin.y + client.y))) return;
  warpClient_ = client;
  warpAt_ = now;
  ++warps_;
}

void CursorPresenter::LocalMove(LPARAM lparam) {
  const LONG x = LONG(short(LOWORD(lparam))), y = LONG(short(HIWORD(lparam)));
  const ULONGLONG now = os_.ticks();
  // The move our own SetCursorPos produced is not user movement.
  if (warpClient_ && now - warpAt_ <= 250 && std::abs(x - warpClient_->x) <= 1 && std::abs(y - warpClient_->y) <= 1) {
    warpClient_.reset();
    return;
  }
  lastLocalMove_ = now;
  localMoveSeen_ = true;
}

LRESULT CALLBACK CursorPresenter::SubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                               UINT_PTR, DWORD_PTR data) {
  auto* self = reinterpret_cast<CursorPresenter*>(data);
  switch (message) {
    case WM_SETCURSOR: {
      const bool client = self && LOWORD(lparam) == HTCLIENT && reinterpret_cast<HWND>(wparam) == window;
      // Flutter chose `none` a moment ago and nothing replaced our cursor
      // since: keep the remote shape without letting Flutter hide it first
      // (a hide/show pair on every mouse move could flicker). Flutter is
      // consulted again after kCursorFlutterRecheckMs or any foreign cursor.
      if (client && self->Presenting() && self->flutterNone_ &&
          self->os_.ticks() - self->flutterAt_ < kCursorFlutterRecheckMs) {
        const HCURSOR now = self->os_.get();
        if ((!now || self->Owns(now)) && self->Present()) return TRUE;
      }
      // Flutter first sets its own cursor (`none` over a captured picture).
      const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
      if (client) {
        self->flutterAt_ = self->os_.ticks();
        self->flutterNone_ = self->os_.get() == nullptr;
        if (self->Present()) return TRUE;
      }
      return result;
    }
    case WM_MOUSEMOVE:
      if (self) self->LocalMove(lparam);
      break;
    case WM_TIMER:
      if (self && wparam == kCursorTimer) {
        self->Refresh();
        return 0;
      }
      break;
    case WM_NCDESTROY:
      if (self && self->view_ == window) {
        self->Reset();
        self->timer_ = false;
        self->view_ = nullptr;
      }
      RemoveWindowSubclass(window, SubclassProc, kSubclassId);
      break;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

}  // namespace multitap
