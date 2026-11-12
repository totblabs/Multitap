#include "transfer_notice.h"

#include <ShellScalingApi.h>
#include <windowsx.h>
#include <objbase.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include <thread>

// GDI+ names min/max unqualified.
using std::max;
using std::min;
#include <gdiplus.h>
#include "ui_language.h"

namespace multitap {
namespace {

constexpr wchar_t kClassName[] = L"MultitapTransferNotice";
constexpr UINT_PTR kTimer = 1;
constexpr UINT kTimerMs = 200;
constexpr int kWidth = 360;       // device independent pixels (96 dpi)
constexpr int kMargin = 16;       // from the work area edge
constexpr int kPad = 16, kBadge = 30, kTextX = kPad + kBadge + 12, kRight = kWidth - kPad;

int Scale(int value, UINT dpi) { return MulDiv(value, int(dpi), 96); }

// ---- The taskbar ------------------------------------------------------------------------------------------

class SystemTaskbarProgress final : public TaskbarProgress {
 public:
  void Set(HWND window, TaskbarMode mode, int percent) override {
    if (!window || !IsWindow(window)) return;
    if (!list_ && !failed_) {
      const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      (void)init;  // S_FALSE / RPC_E_CHANGED_MODE: COM is already usable on this thread
      if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list_))) ||
          FAILED(list_->HrInit())) {
        list_.Reset();
        failed_ = true;
      }
    }
    if (!list_) return;
    TBPFLAG flag = TBPF_NOPROGRESS;
    if (mode == TaskbarMode::normal) flag = TBPF_NORMAL;
    else if (mode == TaskbarMode::indeterminate) flag = TBPF_INDETERMINATE;
    else if (mode == TaskbarMode::error) flag = TBPF_ERROR;
    if (mode == TaskbarMode::normal || mode == TaskbarMode::error)
      list_->SetProgressValue(window, ULONGLONG(std::clamp(percent, 0, 100)), 100);
    list_->SetProgressState(window, flag);
  }

 private:
  Microsoft::WRL::ComPtr<ITaskbarList3> list_;
  bool failed_ = false;
};

// ---- Drawing ----------------------------------------------------------------------------------------------

// The session chrome's colours (lib/ui/session_menu.dart SessionChrome), dark and light.
struct Palette {
  Gdiplus::Color surface, edge, ink, muted, faint, accent, good, danger, track, hover, buttonEdge, panel;
};
Palette PaletteFor(bool dark) {
  using Gdiplus::Color;
  if (dark) {
    return {Color(0xFF, 0x16, 0x1A, 0x1E), Color(0x40, 0xFF, 0xFF, 0xFF), Color(0xFF, 0xE4, 0xE9, 0xEC), Color(0xFF, 0x8E, 0x9A, 0xA3),
            Color(0xFF, 0x5E, 0x6A, 0x73), Color(0xFF, 0xF5, 0xA6, 0x23), Color(0xFF, 0x62, 0xE3, 0xA9), Color(0xFF, 0xFF, 0x8A, 0x7A),
            Color(0xFF, 0x2A, 0x32, 0x38), Color(0x22, 0xFF, 0xFF, 0xFF), Color(0xFF, 0x3A, 0x44, 0x4C), Color(0xFF, 0x1F, 0x24, 0x29)};
  }
  return {Color(0xFF, 0xFF, 0xFF, 0xFF), Color(0x30, 0x0F, 0x14, 0x19), Color(0xFF, 0x1A, 0x20, 0x25), Color(0xFF, 0x66, 0x71, 0x7A),
          Color(0xFF, 0xA3, 0xAC, 0xB3), Color(0xFF, 0xA8, 0x62, 0x00), Color(0xFF, 0x13, 0x86, 0x5A), Color(0xFF, 0xC2, 0x3E, 0x36),
          Color(0xFF, 0xE4, 0xE8, 0xEB), Color(0x18, 0x0F, 0x14, 0x19), Color(0xFF, 0xDC, 0xE1, 0xE5), Color(0xFF, 0xF0, 0xF2, 0xF4)};
}
Gdiplus::Color Tint(const Gdiplus::Color& color, BYTE alpha) { return Gdiplus::Color(alpha, color.GetR(), color.GetG(), color.GetB()); }

void RoundedPath(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius) {
  const float d = min(radius * 2, min(r.Width, r.Height));
  path.Reset();
  path.AddArc(r.X, r.Y, d, d, 180, 90);
  path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
  path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
  path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
  path.CloseFigure();
}

// ---- GDI+ -------------------------------------------------------------------------------------------------

struct GdiplusRuntime {
  GdiplusRuntime() {
    Gdiplus::GdiplusStartupInput input;
    ok = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
  }
  ~GdiplusRuntime() { if (ok) Gdiplus::GdiplusShutdown(token); }
  ULONG_PTR token = 0;
  bool ok = false;
};
GdiplusRuntime& Gdi() { static GdiplusRuntime runtime; return runtime; }

RECT Box(int x, int y, int w, int h, UINT dpi) { return {Scale(x, dpi), Scale(y, dpi), Scale(x + w, dpi), Scale(y + h, dpi)}; }
Gdiplus::RectF F(const RECT& r) { return Gdiplus::RectF(float(r.left), float(r.top), float(r.right - r.left), float(r.bottom - r.top)); }
bool In(const RECT& r, POINT p) { return r.right > r.left && PtInRect(&r, p); }

enum class Align { near_, center, far_ };
struct Type {
  float pixels; bool bold;
};
std::unique_ptr<Gdiplus::Font> FontOf(Type type) {
  static const wchar_t* const families[] = {L"Malgun Gothic", L"Segoe UI"};
  for (const auto* name : families) {
    Gdiplus::FontFamily family(name);
    if (family.IsAvailable())
      return std::make_unique<Gdiplus::Font>(&family, type.pixels, type.bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
  }
  return std::make_unique<Gdiplus::Font>(L"Arial", type.pixels, type.bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
}
// `lines` 1: one line, cut with an ellipsis (in the middle for `middle`); more: wrapped, the last line cut.
// Typographic (no padding around the text): measured widths are the drawn widths.
std::unique_ptr<Gdiplus::StringFormat> FormatOf(int lines, bool middle, Align align) {
  std::unique_ptr<Gdiplus::StringFormat> format(Gdiplus::StringFormat::GenericTypographic()->Clone());
  format->SetFormatFlags(Gdiplus::StringFormatFlagsNoClip |
                         (lines == 1 ? Gdiplus::StringFormatFlagsNoWrap : Gdiplus::StringFormatFlagsLineLimit));
  format->SetTrimming(middle ? Gdiplus::StringTrimmingEllipsisPath : Gdiplus::StringTrimmingEllipsisCharacter);
  format->SetAlignment(align == Align::far_ ? Gdiplus::StringAlignmentFar : align == Align::center ? Gdiplus::StringAlignmentCenter : Gdiplus::StringAlignmentNear);
  format->SetLineAlignment(align == Align::center ? Gdiplus::StringAlignmentCenter : Gdiplus::StringAlignmentNear);
  return format;
}

// Text measured without a window: a 1x1 surface stands in. With no `width`, one line's tight width (no padding).
Gdiplus::SizeF Measure(const std::wstring& text, Type type, float width) {
  if (text.empty() || !Gdi().ok) return {0, 0};
  Gdiplus::Bitmap one(1, 1, PixelFormat32bppPARGB);
  Gdiplus::Graphics g(&one);
  g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
  const auto font = FontOf(type);
  const auto format = FormatOf(width > 0 ? 8 : 1, false, Align::near_);
  Gdiplus::RectF box;
  g.MeasureString(text.c_str(), int(text.size()), font.get(), Gdiplus::RectF(0, 0, width > 0 ? width : 100000.f, 100000.f), format.get(), &box);
  return {box.Width, box.Height};
}
// The height of `text` wrapped to `width` pixels, at most `lines` lines.
int WrappedHeight(const std::wstring& text, Type type, int width, int lines) {
  if (text.empty()) return 0;
  const float line = Measure(L"가", type, 0).Height;
  const float height = Measure(text, type, float(width)).Height;
  return int(std::ceil(std::min(height, line * float(lines))));
}

void Text(Gdiplus::Graphics& g, const std::wstring& text, Type type, const Gdiplus::Color& color, const RECT& box, int lines = 1,
          bool middle = false, Align align = Align::near_) {
  if (text.empty() || box.right <= box.left) return;
  const auto font = FontOf(type);
  const auto format = FormatOf(lines, middle, align);
  Gdiplus::SolidBrush brush(color);
  g.DrawString(text.c_str(), int(text.size()), font.get(), F(box), format.get(), &brush);
}

enum class Hot { none, cancel, close, open, details };

// A rounded command; `primary` is the amber 폴더 열기.
void Button(Gdiplus::Graphics& g, const Palette& p, const RECT& box, const wchar_t* label, bool hot, bool enabled, UINT dpi, bool primary = false) {
  Gdiplus::GraphicsPath path;
  Gdiplus::RectF r = F(box);
  r.X += .5f; r.Y += .5f; r.Width -= 1; r.Height -= 1;
  RoundedPath(path, r, float(Scale(8, dpi)));
  if (primary) { Gdiplus::SolidBrush fill(Tint(p.accent, hot ? 0x38 : 0x1E)); g.FillPath(&fill, &path); }
  else if (hot && enabled) { Gdiplus::SolidBrush fill(p.hover); g.FillPath(&fill, &path); }
  Gdiplus::Pen pen(primary ? Tint(p.accent, 0x8C) : p.buttonEdge, 1.0f);
  g.DrawPath(&pen, &path);
  Text(g, label, {float(Scale(13, dpi)), primary}, primary ? p.accent : enabled ? p.ink : p.faint, box, 1, false, Align::center);
}

// ---- Layout ----------------------------------------------------------------------------------------------

// Every part of the card in pixels at one dpi. Running: badge, title and percent, the line under them, the bar,
// sizes · rate with the time left, 자세히 and 취소, and the 자세히 rows when open. Finished: badge, outcome, the line
// under it, 폴더 열기. Failed: badge, title, cause, next step, 닫기.
struct Layout {
  int width = 0, height = 0;
  RECT badge{}, title{}, percent{}, subtitle{}, bar{}, detail{}, trailing{}, toggle{}, cancel{}, close{}, open{}, message{}, hint{}, panel{};
  std::vector<std::pair<RECT, RECT>> rows;  // label, value
};
Layout LayoutFor(const TransferNoticeContent& c, bool expanded, UINT dpi) {
  using Kind = TransferNoticeContent::Kind;
  Layout l;
  l.width = Scale(kWidth, dpi);
  const auto px = [dpi](int v) { return Scale(v, dpi); };
  if (c.kind == Kind::done || c.kind == Kind::cancelled) {
    const bool open = c.kind == Kind::done && c.openable;
    const int textRight = open ? kRight - 104 : kRight;
    l.badge = Box(kPad, 18, kBadge, kBadge, dpi);
    l.title = Box(kTextX, 15, textRight - kTextX, 20, dpi);
    l.subtitle = Box(kTextX, 37, textRight - kTextX, 18, dpi);
    if (open) l.open = Box(kRight - 96, 18, 96, 32, dpi);
    l.height = px(68);
    return l;
  }
  l.badge = Box(kPad, 14, kBadge, kBadge, dpi);
  if (c.kind == Kind::failed) {
    l.title = Box(kTextX, 14, kRight - kTextX, 20, dpi);
    const int width = px(kRight - kTextX);
    int y = px(40);
    const int message = WrappedHeight(c.message, {float(px(13)), false}, width, 3);
    l.message = {px(kTextX), y, px(kRight), y + message};
    y += message + (c.hint.empty() ? 0 : px(4));
    const int hint = WrappedHeight(c.hint, {float(px(12)), false}, width, 3);
    l.hint = {px(kTextX), y, px(kRight), y + hint};
    y = std::max(y + hint, px(14 + kBadge)) + px(12);
    l.close = {px(kRight - 68), y, px(kRight), y + px(32)};
    l.height = y + px(32) + px(14);
    return l;
  }
  const bool percent = c.percent >= 0;
  l.percent = percent ? Box(kRight - 48, 14, 48, 20, dpi) : RECT{};
  l.title = Box(kTextX, 14, (percent ? kRight - 52 : kRight) - kTextX, 20, dpi);
  l.subtitle = Box(kTextX, 36, kRight - kTextX, 18, dpi);
  l.bar = Box(kPad, 62, kRight - kPad, 6, dpi);
  // Sizes and the time left; nothing known yet (preparing): no empty line.
  const bool stats = !c.detail.empty() || !c.trailing.empty();
  const int trailing = c.trailing.empty() ? 0 : int(std::ceil(Measure(c.trailing, {float(px(12)), false}, 0).Width)) + 2;
  if (stats) {
    l.trailing = {px(kRight) - trailing, px(76), px(kRight), px(94)};
    l.detail = {px(kPad), px(76), px(kRight) - trailing - (trailing ? px(10) : 0), px(94)};
  }
  const int footer = stats ? 102 : 78;
  if (!c.details.empty()) l.toggle = Box(kPad - 6, footer, 84, 32, dpi);
  l.cancel = Box(kRight - 68, footer, 68, 32, dpi);
  int y = px(footer + 32);
  if (expanded && !c.details.empty()) {
    y += px(8);
    const int top = y;
    y += px(8);
    const Type body{float(px(12)), false};
    const float line = Measure(L"가", body, 0).Height;
    for (size_t i = 0; i < c.details.size(); ++i) {
      // A long path or name gets a second line (its middle is cut beyond that).
      const bool two = Measure(c.details[i].second, body, float(px(kRight - 12 - (kPad + 12 + 68)))).Height > line * 1.5f;
      const int h = px(two ? 34 : 18);
      l.rows.push_back({RECT{px(kPad + 12), y, px(kPad + 12 + 64), y + px(18)}, RECT{px(kPad + 12 + 68), y, px(kRight - 12), y + h}});
      y += h + px(4);
    }
    y += px(4);
    l.panel = {px(kPad), top, px(kRight), y};
  }
  l.height = y + px(14);
  return l;
}

// ---- Painting --------------------------------------------------------------------------------------------

Gdiplus::PointF P(const Gdiplus::RectF& r, float x, float y) { return {r.X + r.Width * x, r.Y + r.Height * y}; }

// The round icon: the direction while it runs (an arrow into or out of a tray), then the outcome.
void Badge(Gdiplus::Graphics& g, const Palette& p, const TransferNoticeContent& c, const RECT& box, UINT dpi) {
  using Kind = TransferNoticeContent::Kind;
  const Gdiplus::Color color = c.kind == Kind::done ? p.good : c.kind == Kind::failed ? p.danger : c.kind == Kind::cancelled ? p.muted : p.accent;
  const Gdiplus::RectF disc = F(box);
  Gdiplus::SolidBrush fill(Tint(color, c.dark ? 0x29 : 0x1F));
  g.FillEllipse(&fill, disc);
  Gdiplus::Pen pen(color, float(Scale(2, dpi)));
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  if (c.kind == Kind::done) {
    const Gdiplus::PointF tick[3] = {P(disc, .30f, .52f), P(disc, .44f, .66f), P(disc, .71f, .37f)};
    g.DrawLines(&pen, tick, 3);
  } else if (c.kind == Kind::failed) {
    g.DrawLine(&pen, P(disc, .5f, .28f), P(disc, .5f, .56f));
    Gdiplus::SolidBrush dot(color);
    const float r = float(Scale(2, dpi)) * .75f;
    const auto at = P(disc, .5f, .71f);
    g.FillEllipse(&dot, at.X - r, at.Y - r, r * 2, r * 2);
  } else if (c.kind == Kind::cancelled) {
    g.DrawLine(&pen, P(disc, .36f, .36f), P(disc, .64f, .64f));
    g.DrawLine(&pen, P(disc, .64f, .36f), P(disc, .36f, .64f));
  } else if (c.sending) {
    g.DrawLine(&pen, P(disc, .5f, .30f), P(disc, .5f, .60f));
    const Gdiplus::PointF head[3] = {P(disc, .37f, .43f), P(disc, .5f, .30f), P(disc, .63f, .43f)};
    g.DrawLines(&pen, head, 3);
    g.DrawLine(&pen, P(disc, .33f, .72f), P(disc, .67f, .72f));
  } else {
    g.DrawLine(&pen, P(disc, .5f, .28f), P(disc, .5f, .58f));
    const Gdiplus::PointF head[3] = {P(disc, .37f, .45f), P(disc, .5f, .58f), P(disc, .63f, .45f)};
    g.DrawLines(&pen, head, 3);
    g.DrawLine(&pen, P(disc, .33f, .72f), P(disc, .67f, .72f));
  }
}

// A small folder before the destination's name.
void FolderGlyph(Gdiplus::Graphics& g, const Palette& p, float x, float y, UINT dpi) {
  const float s = float(Scale(100, dpi)) / 100.f;
  Gdiplus::SolidBrush fill(p.accent);
  Gdiplus::GraphicsPath tab, body;
  RoundedPath(tab, {x, y + 2 * s, 6 * s, 3 * s}, 1 * s);
  RoundedPath(body, {x, y + 4 * s, 13 * s, 9 * s}, 1.5f * s);
  g.FillPath(&fill, &tab);
  g.FillPath(&fill, &body);
}

void Chevron(Gdiplus::Graphics& g, const Gdiplus::Color& color, float cx, float cy, bool up, UINT dpi) {
  const float s = float(Scale(100, dpi)) / 100.f;
  Gdiplus::Pen pen(color, 1.6f * s);
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  const float d = up ? -1.f : 1.f;
  const Gdiplus::PointF v[3] = {{cx - 4 * s, cy - 2 * s * d}, {cx, cy + 2 * s * d}, {cx + 4 * s, cy - 2 * s * d}};
  g.DrawLines(&pen, v, 3);
}

void PaintCard(Gdiplus::Graphics& g, const TransferNoticeContent& c, const Layout& l, bool expanded, Hot hot, UINT dpi) {
  using Kind = TransferNoticeContent::Kind;
  const Palette p = PaletteFor(c.dark);
  const auto px = [dpi](int v) { return float(Scale(v, dpi)); };
  Gdiplus::GraphicsPath body;
  RoundedPath(body, {.5f, .5f, float(l.width) - 1, float(l.height) - 1}, px(14));
  Gdiplus::SolidBrush surface(p.surface);
  g.FillPath(&surface, &body);
  Gdiplus::Pen edge(p.edge, 1.0f);
  g.DrawPath(&edge, &body);

  Badge(g, p, c, l.badge, dpi);
  const Type title{px(14), true}, body12{px(12), false};
  Text(g, c.title, title, c.kind == Kind::failed ? p.danger : p.ink, l.title);

  if (c.kind == Kind::failed) {
    Text(g, c.message, {px(13), false}, p.ink, l.message, 3);
    Text(g, c.hint, body12, p.muted, l.hint, 3);
    Button(g, p, l.close, multitap::Tr(L"닫기", L"Close"), hot == Hot::close, true, dpi);
    return;
  }

  // The line under the title: the destination folder first (a glyph, its name in ink), then the rest.
  RECT sub = l.subtitle;
  std::wstring rest = c.subtitle;
  if (c.folder && c.kind == Kind::running) {
    const auto cut = rest.find(L" · ");
    const std::wstring name = cut == std::wstring::npos ? rest : rest.substr(0, cut);
    rest = cut == std::wstring::npos ? std::wstring() : rest.substr(cut);
    FolderGlyph(g, p, float(sub.left), float(sub.top) + px(1), dpi);
    sub.left += LONG(px(18));
    const float limit = float(sub.right - sub.left) * (rest.empty() ? 1.f : .55f);
    const float width = std::min(limit, Measure(name, body12, 0).Width + px(1));
    RECT named = sub;
    named.right = named.left + LONG(width);
    Text(g, name, body12, p.ink, named, 1, true);
    sub.left = named.right;
  }
  Text(g, rest, body12, p.muted, sub, 1, true);

  if (c.kind != Kind::running) {
    if (l.open.right > l.open.left) Button(g, p, l.open, multitap::Tr(L"폴더 열기", L"Open folder"), hot == Hot::open, true, dpi, true);
    return;
  }

  if (l.percent.right > l.percent.left)
    Text(g, std::to_wstring(std::clamp(c.percent, 0, 100)) + L"%", {px(14), true}, p.accent, l.percent, 1, false, Align::far_);

  Gdiplus::GraphicsPath track;
  RoundedPath(track, F(l.bar), px(3));
  Gdiplus::SolidBrush trackBrush(p.track);
  g.FillPath(&trackBrush, &track);
  RECT fill = l.bar;
  if (c.percent >= 0) {
    fill.right = fill.left + MulDiv(l.bar.right - l.bar.left, std::clamp(c.percent, 0, 100), 100);
  } else {
    // Unknown size: a short bar standing in the middle (the card redraws only on change, so it does not slide).
    const int w = (l.bar.right - l.bar.left) / 4;
    fill.left = l.bar.left + (l.bar.right - l.bar.left - w) / 2;
    fill.right = fill.left + w;
  }
  if (fill.right > fill.left) {
    Gdiplus::GraphicsPath filled;
    RoundedPath(filled, F(fill), px(3));
    Gdiplus::SolidBrush accent(p.accent);
    g.FillPath(&accent, &filled);
  }
  Text(g, c.detail, body12, p.muted, l.detail);
  Text(g, c.trailing, body12, p.ink, l.trailing, 1, false, Align::far_);

  if (l.toggle.right > l.toggle.left) {
    if (hot == Hot::details) {
      Gdiplus::GraphicsPath back;
      RoundedPath(back, F(l.toggle), px(8));
      Gdiplus::SolidBrush light(p.hover);
      g.FillPath(&light, &back);
    }
    RECT label = l.toggle;
    label.left += LONG(px(6));
    label.top += LONG(px(8));
    const std::wstring word = expanded ? multitap::Tr(L"간단히", L"Less") : multitap::Tr(L"자세히", L"Details");
    Text(g, word, body12, p.muted, label);
    const float after = float(label.left) + Measure(word, body12, 0).Width + px(8);
    Chevron(g, p.muted, after, float(l.toggle.top + l.toggle.bottom) / 2.f, expanded, dpi);
  }
  Button(g, p, l.cancel, c.cancelling ? multitap::Tr(L"취소 중…", L"Cancelling…") : multitap::Tr(L"취소", L"Cancel"), hot == Hot::cancel, c.cancellable && !c.cancelling, dpi);

  if (expanded && l.panel.bottom > l.panel.top) {
    Gdiplus::GraphicsPath panel;
    RoundedPath(panel, F(l.panel), px(10));
    Gdiplus::SolidBrush back(p.panel);
    g.FillPath(&back, &panel);
    for (size_t i = 0; i < l.rows.size() && i < c.details.size(); ++i) {
      Text(g, c.details[i].first, body12, p.muted, l.rows[i].first);
      const RECT& value = l.rows[i].second;
      Text(g, c.details[i].second, body12, p.ink, value, value.bottom - value.top > LONG(px(20)) ? 2 : 1, true);
    }
  }
}

// What a screen reader hears for the card (its window's name). It changes with the phase only, so a reader is not
// told every tick: while running the direction, then the outcome with its line or the cause and next step.
std::wstring Spoken(const TransferNoticeContent& c) {
  using Kind = TransferNoticeContent::Kind;
  if (c.kind == Kind::running) return c.title + multitap::Tr(L", 복사 중", L", copying");
  std::wstring text = c.title;
  for (const auto* part : {&c.subtitle, &c.message, &c.hint})
    if (!part->empty()) text += L", " + *part;
  return text;
}

POINT SystemCursor() { POINT p{}; GetCursorPos(&p); return p; }
bool SystemOwnerForeground(HWND owner) {
  const HWND foreground = GetForegroundWindow();
  if (!foreground || !owner) return false;
  return foreground == owner || GetAncestor(foreground, GA_ROOTOWNER) == GetAncestor(owner, GA_ROOTOWNER);
}

RECT WorkAreaAt(POINT point, UINT* dpi) {
  const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{sizeof(info)};
  RECT area{0, 0, 1024, 768};
  if (monitor && GetMonitorInfoW(monitor, &info)) area = info.rcWork;
  UINT x = 96, y = 96;
  if (monitor && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) && x >= 96) *dpi = x; else *dpi = 96;
  return area;
}

}  // namespace

std::unique_ptr<TaskbarProgress> CreateTaskbarProgress() { return std::make_unique<SystemTaskbarProgress>(); }

RECT KeepInside(RECT rect, const RECT& area) {
  const LONG width = rect.right - rect.left, height = rect.bottom - rect.top;
  if (rect.right > area.right) rect.left = area.right - width;
  if (rect.bottom > area.bottom) rect.top = area.bottom - height;
  if (rect.left < area.left) rect.left = area.left;
  if (rect.top < area.top) rect.top = area.top;
  rect.right = rect.left + width;
  rect.bottom = rect.top + height;
  return rect;
}

RECT NoticePlacement(const RECT& area, int width, int height, int margin) {
  (void)margin;  // Kept for the existing caller interface; placement is centered.
  const LONG left = area.left + (area.right - area.left - width) / 2;
  const LONG top = area.top + (area.bottom - area.top - height) / 2;
  RECT rect{left, top, left + width, top + height};
  return KeepInside(rect, area);
}

SIZE NoticeSize(const TransferNoticeContent& content, bool expanded, UINT dpi) {
  const Layout l = LayoutFor(content, expanded, dpi);
  return {l.width, l.height};
}
SIZE NoticeSize(TransferNoticeContent::Kind kind, UINT dpi) {
  TransferNoticeContent content;
  content.kind = kind;
  return NoticeSize(content, false, dpi);
}

bool RenderTransferNotice(const TransferNoticeContent& content, bool expanded, UINT dpi, std::vector<uint8_t>& pixels, SIZE& size,
                          int hot) {
  if (!Gdi().ok || dpi < 48) return false;
  const Layout l = LayoutFor(content, expanded, dpi);
  size = {l.width, l.height};
  pixels.assign(size_t(l.width) * size_t(l.height) * 4, 0);
  Gdiplus::Bitmap surface(l.width, l.height, l.width * 4, PixelFormat32bppPARGB, pixels.data());
  Gdiplus::Graphics g(&surface);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
  PaintCard(g, content, l, expanded, Hot(std::clamp(hot, 0, 4)), dpi);
  return true;
}

// ---- The window -------------------------------------------------------------------------------------------

struct TransferNotice::Impl {
  Impl(std::unique_ptr<TaskbarProgress> taskbar_in, TransferNoticeAccess access_in)
      : taskbar(std::move(taskbar_in)), access(std::move(access_in)) {
    if (!access.owner_foreground) access.owner_foreground = SystemOwnerForeground;
    if (!access.owner_nonpainted) access.owner_nonpainted = [](HWND window) {
      return window && IsWindow(window) && (IsIconic(window) || !IsWindowVisible(window));
    };
    if (!access.cursor) access.cursor = SystemCursor;
  }
  ~Impl() {
    life.reset();
    if (OwnPopup()) {
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      DestroyWindow(hwnd);
    }
  }

  std::unique_ptr<TaskbarProgress> taskbar;
  TransferNoticeAccess access;
  NoticeLeaseModel model;
  NoticeTransitionFence transition;
  NoticeSurfaceObservers surface_state;
  TransferNoticeCallbacks callbacks;
  const std::thread::id thread=std::this_thread::get_id();
  std::shared_ptr<int> life=std::make_shared<int>(0);
  bool legacy=false;
  HWND hwnd = nullptr, owner = nullptr;
  TransferNoticeContent content;
  // anchored: the corner is known; moved: the user dragged the card, so it keeps its place for the next transfer too.
  bool have = false, dismissed = false, anchored = false, moved = false;
  int generation = -1;
  UINT dpi = 96;
  POINT anchor{};  // the bottom right corner of the card
  bool expanded = false;  // 자세히 is open (per transfer)
  Hot hot = Hot::none, pressed = Hot::none;
  bool tracking = false;
  std::optional<NoticeStamp> pressed_notice;

  bool OnThread() const {return std::this_thread::get_id()==thread;}
  bool Nonpainted() const {return owner && access.owner_nonpainted(owner);}

  bool Visible() const { return hwnd && IsWindowVisible(hwnd); }
  bool OwnPopup() const {
    return hwnd&&IsWindow(hwnd)&&GetWindowThreadProcessId(hwnd,nullptr)==GetCurrentThreadId()&&
      GetWindowLongPtrW(hwnd,GWLP_USERDATA)==reinterpret_cast<LONG_PTR>(this);
  }
  NoticeSurfaceProof PhysicalProof() const {
    if(!hwnd||!IsWindow(hwnd))return NoticeSurfaceProof::hidden;
    if(!OwnPopup())return NoticeSurfaceProof::unknown;
    return Visible()?NoticeSurfaceProof::visible:NoticeSurfaceProof::hidden;
  }
  bool HidePopup() {
    surface_state.Set(NoticeSurfaceProof::unknown);
    const bool hidden=TryHideOwnedNotice(
      [&]{return PhysicalProof()==NoticeSurfaceProof::hidden;},[&]{return OwnPopup();},
      [&]{SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_HIDEWINDOW|SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER);},
      [&]{DestroyWindow(hwnd);});
    if(hwnd&&!IsWindow(hwnd))hwnd=nullptr;
    surface_state.Set(PhysicalProof());return hidden;
  }
  struct Retirement {
    NoticeStamp state;
    std::function<void(const NoticeStamp&,bool)> notify;
  };
  Retirement CleanupRetired() {
    Retirement retired;if(!model.active()||!model.retiring()||!surface_state.Stamp().inlineAllowed)return retired;
    model.Hidden(model.Stamp(),true);retired={model.Stamp(),callbacks.presentation};
    const HWND oldOwner=owner;model.Detach(model.Stamp().identity.window);callbacks={};
    have=false;owner=nullptr;legacy=false;pressed_notice.reset();
    if(OwnPopup())KillTimer(hwnd,kTimer);
    if(taskbar&&oldOwner)taskbar->Set(oldOwner,TaskbarMode::none,0);
    return retired;
  }
  void NotifyRetired(const Retirement& retired,const NoticeSurfaceStamp& proof) {
    const std::weak_ptr<int> weak=life;
    if(retired.notify){try{retired.notify(retired.state,false);}catch(...){ }}
    if(!weak.expired())surface_state.Notify(proof);
  }

  bool Create() {
    if(hwnd&&!IsWindow(hwnd))hwnd=nullptr;
    if (hwnd) return OwnPopup();
    if (!Gdi().ok) return false;
    static const ATOM atom = [] {
      WNDCLASSEXW cls{sizeof(cls)};
      cls.lpfnWndProc = &Impl::Proc;
      cls.hInstance = GetModuleHandleW(nullptr);
      cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
      cls.lpszClassName = kClassName;
      return RegisterClassExW(&cls);
    }();
    if (!atom) return false;
    hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClassName, multitap::Tr(L"Multitap 전송", L"Multitap transfer"), WS_POPUP, 0, 0, 1,
                           1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hwnd) SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return hwnd != nullptr;
  }

  // Where the card sits now: the remembered corner, else the monitor under the cursor.
  RECT Frame() {
    if (!anchored) {
      UINT monitor_dpi = 96;
      const RECT area = WorkAreaAt(access.cursor(), &monitor_dpi);
      dpi = access.dpi_override ? access.dpi_override() : monitor_dpi;
      const Layout& l = CurrentLayout();
      const RECT rect = NoticePlacement(area, l.width, l.height, Scale(kMargin, dpi));
      anchor = {rect.right, rect.bottom};
      anchored = true;
    }
    const Layout& l = CurrentLayout();
    RECT rect{anchor.x - l.width, anchor.y - l.height, anchor.x, anchor.y};
    UINT ignored = 96;
    return KeepInside(rect, WorkAreaAt({anchor.x - 1, anchor.y - 1}, &ignored));
  }

  // The layout measures text: it is kept until the content, 자세히 or the dpi changes.
  mutable Layout layout;
  mutable bool layout_dirty = true;
  mutable UINT layout_dpi = 0;
  const Layout& CurrentLayout() const {
    if (layout_dirty || layout_dpi != dpi) {
      layout = LayoutFor(content, expanded, dpi);
      layout_dpi = dpi;
      layout_dirty = false;
    }
    return layout;
  }
  // The window's name is what a screen reader says for the card; it changes with the content.
  void Name() {
    if (!OwnPopup()) return;
    const std::wstring spoken = Spoken(content);
    if (spoken == spoken_name) return;
    const bool phase = content.kind != spoken_kind;
    spoken_name = spoken;
    spoken_kind = content.kind;
    SetWindowTextW(hwnd, spoken.c_str());
    if (phase) NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd, OBJID_WINDOW, CHILDID_SELF);
  }
  std::wstring spoken_name;
  TransferNoticeContent::Kind spoken_kind = TransferNoticeContent::Kind::running;

  bool Draw() {
    if (!OwnPopup()) return false;
    const auto rendering=model.Stamp();
    const RECT frame = Frame();
    const int width = frame.right - frame.left, height = frame.bottom - frame.top;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    const HDC screen = GetDC(nullptr);
    const HDC memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
      if (bitmap) DeleteObject(bitmap);
      DeleteDC(memory);
      ReleaseDC(nullptr, screen);
      return false;
    }
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    std::memset(bits, 0, size_t(width) * size_t(height) * 4);
    {
      Gdiplus::Bitmap surface(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
      Gdiplus::Graphics g(&surface);
      g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
      PaintCard(g, content, CurrentLayout(), expanded, hot, dpi);
    }
    Name();
    if (access.frame_sink) access.frame_sink(static_cast<const uint8_t*>(bits), width, height);
    POINT origin{frame.left, frame.top};
    SIZE size{width, height};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const bool drawn=model.Current(rendering) && UpdateLayeredWindow(hwnd, screen, &origin, &size, memory, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return drawn;
  }

  Hot HitTest(POINT client) const {
    using Kind = TransferNoticeContent::Kind;
    const Layout& l = CurrentLayout();
    if (content.kind == Kind::failed && In(l.close, client)) return Hot::close;
    if (content.kind == Kind::done && content.openable && In(l.open, client)) return Hot::open;
    if (content.kind != Kind::running) return Hot::none;
    if (In(l.cancel, client) && content.cancellable && !content.cancelling) return Hot::cancel;
    if (In(l.toggle, client)) return Hot::details;
    return Hot::none;
  }

  void Process(NoticeDecision decision) {
    if(transition.active()||decision.action==NoticeAction::none||!model.Current(decision.state))return;
    const auto state=decision.state;
    if(decision.action==NoticeAction::prepare) {
      const auto prepare=callbacks.prepare;
      if(!prepare){Process(model.Prepared(state,false,access.owner_foreground(owner),Nonpainted(),GetTickCount64()));return;}
      const std::weak_ptr<int> weak=life;const auto ui=thread;
      try {prepare(state,[this,weak,ui,state](bool ready){
        if(std::this_thread::get_id()!=ui||weak.expired())return;
        Prepared(state,ready);
      });}
      catch(...){if(!weak.expired())Process(model.Prepared(state,false,access.owner_foreground(owner),Nonpainted(),GetTickCount64()));}
      return;
    }
    if(decision.action==NoticeAction::show) {
      if(!owner||!IsWindow(owner)){Evaluate();return;}
      // A minimized-owner proof may have expired while the frame ACK travelled.
      const auto fresh=model.Evaluate(access.owner_foreground(owner),Nonpainted(),GetTickCount64());
      if(fresh.action!=NoticeAction::show||!model.Current(state)){Process(fresh);return;}
      const bool drawable=Create()&&Draw();if(!model.Current(state))return;
      NoticeDecision next;NoticeSurfaceStamp proof;
      transition.Finish([&]{
        if(!drawable) {
          if(HidePopup())next=model.Shown(state,false);else {model.Retire();have=false;}
        } else {
          surface_state.Set(NoticeSurfaceProof::unknown);
          SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
          const auto physical=PhysicalProof();surface_state.Set(physical);
          if(physical==NoticeSurfaceProof::unknown){model.Retire();have=false;}
          else next=model.Shown(state,physical==NoticeSurfaceProof::visible);
        }
        proof=surface_state.Stamp();return true;
      },[&]{
        const std::weak_ptr<int> weak=life;Process(next);
        if(!weak.expired())surface_state.Notify(proof);
      });return;
    }
    if(decision.action==NoticeAction::hide) {
      NoticeDecision next;Retirement retired;NoticeSurfaceStamp proof;
      transition.Finish([&]{
        const bool hidden=HidePopup();
        if(!moved)anchored=false;
        if(hidden&&model.retiring())retired=CleanupRetired();
        else next=model.Hidden(state,hidden);
        if(model.have()&&!hwnd)Create();
        if(OwnPopup()&&(model.have()||model.retiring()))SetTimer(hwnd,kTimer,kTimerMs,nullptr);
        proof=surface_state.Stamp();return true;
      },[&]{
        const std::weak_ptr<int> weak=life;
        if(retired.notify)NotifyRetired(retired,proof);
        else {Process(next);if(!weak.expired())surface_state.Notify(proof);}
      });return;
    }
    const auto notify=callbacks.presentation;
    if(notify){try{notify(state,true);}catch(...){ }}
  }
  void Evaluate() {
    if(!OnThread()||transition.active()||!model.active())return;
    if(!owner||!IsWindow(owner)) {
      model.Retire();have=false;Process(model.Evaluate(false,false,GetTickCount64()));
      return;
    }
    have=model.have();
    const auto identity=model.Stamp().identity;
    Process(model.Evaluate(access.owner_foreground(owner),Nonpainted(),GetTickCount64()));
    if(!model.Owns(identity))return;
    if(OwnPopup()){if(model.have()||model.retiring())SetTimer(hwnd,kTimer,kTimerMs,nullptr);else KillTimer(hwnd,kTimer);}
  }
  void Prepared(const NoticeStamp& state,bool ready) {
    if(!OnThread()||transition.active()||!model.Current(state))return;
    if(!owner||!IsWindow(owner)){Evaluate();return;}
    Process(model.Prepared(state,ready,access.owner_foreground(owner),Nonpainted(),GetTickCount64()));
  }

  void Cancel() {
    if(transition.active()||model.retiring())return;
    if (!have || content.kind != TransferNoticeContent::Kind::running || !content.cancellable || content.cancelling) return;
    const auto state=model.Stamp();const auto callback=callbacks.cancel;
    if(callback&&model.Current(state)){try{callback(state);}catch(...){ }}
  }
  void Close() {
    if(transition.active()||model.retiring())return;
    if (have && content.kind == TransferNoticeContent::Kind::running) return;
    dismissed = true;
    const auto state=model.Stamp();const auto callback=callbacks.close;
    Process(model.Dismiss(state.identity));
    if(callback&&model.Owns(state.identity)){try{callback(model.Stamp());}catch(...){ }}
  }
  // 폴더 열기: the session opens the folder and retires the card itself.
  void Open() {
    if(transition.active()||model.retiring())return;
    if (!have || content.kind != TransferNoticeContent::Kind::done || !content.openable) return;
    const auto state=model.Stamp();const auto callback=callbacks.open;
    if(callback&&model.Current(state)){try{callback(state);}catch(...){ }}
  }
  // 자세히 / 간단히: the card grows upward from its bottom edge.
  void ToggleDetails() {
    if(transition.active()||model.retiring())return;
    if (!have || content.kind != TransferNoticeContent::Kind::running || content.details.empty()) return;
    expanded = !expanded;
    layout_dirty = true;
    if (Visible()) Draw();
  }

  static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
      case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &point);
        return self->HitTest(point) != Hot::none ? HTCLIENT : HTCAPTION;
      }
      case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
          SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
          return TRUE;
        }
        break;
      case WM_MOUSEMOVE: {
        const Hot now = self->HitTest({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        if (!self->tracking) {
          TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
          self->tracking = TrackMouseEvent(&track) != FALSE;
        }
        if (now != self->hot) { self->hot = now; self->Draw(); }
        return 0;
      }
      case WM_MOUSELEAVE:
        self->tracking = false;
        if (self->hot != Hot::none) { self->hot = Hot::none; self->Draw(); }
        return 0;
      case WM_LBUTTONDOWN:
        self->pressed = self->HitTest({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        self->pressed_notice=self->model.Stamp();
        if (self->pressed != Hot::none) SetCapture(window);
        return 0;
      case WM_LBUTTONUP: {
        const Hot pressed = self->pressed;
        const auto notice=self->pressed_notice;self->pressed_notice.reset();
        self->pressed = Hot::none;
        if (GetCapture() == window) ReleaseCapture();
        if (notice&&self->model.Current(*notice)&&pressed != Hot::none && self->HitTest({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}) == pressed) {
          switch (pressed) {
            case Hot::cancel: self->Cancel(); break;
            case Hot::close: self->Close(); break;
            case Hot::open: self->Open(); break;
            case Hot::details: self->ToggleDetails(); break;
            default: break;
          }
        }
        return 0;
      }
      case WM_EXITSIZEMOVE: {
        RECT rect{};
        GetWindowRect(window, &rect);
        self->anchor = {rect.right, rect.bottom};
        self->anchored = self->moved = true;
        return 0;
      }
      case WM_DPICHANGED: {
        self->dpi = HIWORD(wparam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested) self->anchor = {suggested->right, suggested->bottom};
        self->Draw();
        return 0;
      }
      case WM_DISPLAYCHANGE:
        self->Draw();
        return 0;
      case WM_TIMER:
        if (wparam == kTimer) self->Evaluate();
        return 0;
      case WM_NCDESTROY: {
        self->hwnd=nullptr;
        self->pressed_notice.reset();
        const auto proof=self->surface_state.Set(NoticeSurfaceProof::hidden);
        if(!self->transition.active()) {
          const std::weak_ptr<int> weak=self->life;
          if(self->model.retiring())self->Evaluate();
          else self->Process(self->model.Unavailable(self->model.Stamp()));
          if(!weak.expired())self->surface_state.Notify(proof);
        }
        SetWindowLongPtrW(window,GWLP_USERDATA,0);
        break;
      }
      default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }
};

TransferNotice::TransferNotice(std::unique_ptr<TaskbarProgress> taskbar, TransferNoticeAccess access)
    : impl_(std::make_unique<Impl>(std::move(taskbar), std::move(access))) {}
TransferNotice::~TransferNotice() = default;

TransferNotice& TransferNotice::Shared() {
  static TransferNotice notice(CreateTaskbarProgress());
  return notice;
}

uint64_t TransferNotice::Observe(HWND window,NoticeSurfaceObservers::Callback callback) {
  auto& s=*impl_;if(!s.OnThread()||!window||!IsWindow(window))return 0;
  return s.surface_state.Observe(reinterpret_cast<std::uintptr_t>(window),std::move(callback));
}
bool TransferNotice::Observes(HWND window,uint64_t token) const {
  return impl_->OnThread()&&impl_->surface_state.Owns(reinterpret_cast<std::uintptr_t>(window),token);
}
NoticeSurfaceStamp TransferNotice::Surface() const {
  return impl_->OnThread()?impl_->surface_state.Stamp():NoticeSurfaceStamp{1,false,true};
}
NoticeSurfaceStamp TransferNotice::SurfaceStatus() {
  auto& s=*impl_;if(!s.OnThread())return {1,false,true};
  if(s.transition.active())return s.surface_state.Stamp();
  const auto physical=s.PhysicalProof();
  // Status does not run a new mutation. Retirement must finish its owned
  // cleanup and revoke the old claim before any hidden proof can unblock Dart.
  s.surface_state.Set(s.model.retiring()&&physical==NoticeSurfaceProof::hidden?NoticeSurfaceProof::unknown:physical);
  return s.surface_state.Stamp();
}

NoticeReply TransferNotice::Acquire(HWND owner,std::string token,int generation,TransferNoticeCallbacks callbacks) {
  auto& s = *impl_;
  if(!s.OnThread()||s.transition.active()||!owner||!IsWindow(owner)||!NoticeOpaque(token)||generation<0)return {};
  const auto old=s.model.Stamp();
  if(s.model.active()&&!s.model.retiring()&&old.identity.window==reinterpret_cast<std::uintptr_t>(owner)&&
      old.identity.owner==token&&old.identity.generation==generation)return s.model.Reply(true);
  NoticeReply reply;Impl::Retirement retired;NoticeSurfaceStamp proof;
  s.transition.Finish([&]{
    // Admission is fenced while owned HWND/COM calls can pump messages. Same
    // old-lease updates get an ownership-only ACK, never an inline grant.
    s.model.Retire();s.have=false;
    if(!s.HidePopup()){
      if(s.OwnPopup())SetTimer(s.hwnd,kTimer,kTimerMs,nullptr);
      proof=s.surface_state.Stamp();return false;
    }
    retired=s.CleanupRetired();proof=s.surface_state.Stamp();
    reply=s.model.Acquire(reinterpret_cast<std::uintptr_t>(owner),std::move(token),generation);
    if(!reply.accepted)return false;
    s.owner=owner;s.generation=generation;s.have=s.dismissed=false;s.content={};
    s.expanded=false;s.layout_dirty=true;
    s.anchored=s.moved=false;s.pressed_notice.reset();s.legacy=false;s.callbacks=std::move(callbacks);
    return true;
  },[]{});
  s.NotifyRetired(retired,proof);
  if(!reply.accepted)return {};
  return s.model.Current(reply.state)?s.model.Reply(true):NoticeReply{};
}

NoticeReply TransferNotice::Update(const NoticeIdentity& identity,const TransferNoticeContent& content) {
  auto& s = *impl_;
  if(!s.OnThread()||!s.model.Owns(identity))return {};
  if(s.transition.active())return s.model.Reply(true);
  // The reply is an ownership ACK, not a content-commit receipt. Invalid
  // same-owner content cannot retire a still-visible current presentation.
  if(!s.model.Update(identity,content))return s.model.Reply(true);
  if(!s.have||content.generation!=s.generation||content.kind!=TransferNoticeContent::Kind::running||content.details.empty())s.expanded=false;
  ApplyNoticeUpdate(s.owner,s.generation,s.content,s.have,s.dismissed,s.anchored,s.moved,
      reinterpret_cast<HWND>(identity.window),content);
  s.layout_dirty=true;
  if(!s.Create()){
    const auto physical=s.PhysicalProof();const auto proof=s.surface_state.Set(physical);
    if(physical==NoticeSurfaceProof::hidden)s.Process(s.model.Unavailable(s.model.Stamp()));
    else {s.model.Retire();s.have=false;}
    s.surface_state.Notify(proof);return s.model.Reply(s.model.Owns(identity));
  }
  const auto drawing=s.model.Stamp();
  if(s.Visible()&&!s.Draw()) {
    const auto state=drawing;
    if(s.model.Current(state)){
      NoticeDecision unavailable;NoticeSurfaceStamp proof;
      s.transition.Finish([&]{
        if(s.HidePopup())unavailable=s.model.Unavailable(state);else {s.model.Retire();s.have=false;}
        proof=s.surface_state.Stamp();return true;
      },[&]{const std::weak_ptr<int> weak=s.life;s.Process(unavailable);if(!weak.expired())s.surface_state.Notify(proof);});
    }
  }
  if(!s.model.Owns(identity))return {};
  s.Evaluate();
  return s.model.Reply(s.model.Owns(identity));
}

NoticeReply TransferNotice::Hide(const NoticeIdentity& identity) {
  auto& s=*impl_;if(!s.OnThread()||!s.model.Owns(identity))return {};
  if(s.transition.active())return s.model.Reply(true);
  const auto hidden=s.model.Hide(identity);s.have=s.model.have();s.Process(hidden);
  if(!s.model.Owns(identity))return {};
  s.Evaluate();
  return s.model.Reply(s.model.Owns(identity));
}
NoticeReply TransferNotice::SetTaskbar(const NoticeIdentity& identity,TaskbarMode mode,int percent) {
  auto& s=*impl_;if(!s.OnThread()||!s.model.Owns(identity))return {};
  if(s.transition.active())return s.model.Reply(true);
  s.transition.Finish([&]{if(s.taskbar)s.taskbar->Set(reinterpret_cast<HWND>(identity.window),mode,percent);return true;},[]{});
  return s.model.Reply(s.model.Owns(identity));
}
void TransferNotice::Prepared(const NoticeStamp& state,bool prepared){impl_->Prepared(state,prepared);}
void TransferNotice::Show(HWND owner,const TransferNoticeContent& content,std::function<void()> on_cancel,std::function<void()> on_close) {
  auto& s=*impl_;if(!s.OnThread()||s.transition.active()||(s.model.active()&&!s.legacy))return;
  TransferNoticeCallbacks callbacks;
  callbacks.prepare=[](const NoticeStamp&,std::function<void(bool)> ready){ready(true);};
  callbacks.cancel=[callback=std::move(on_cancel)](const NoticeStamp&){if(callback)callback();};
  callbacks.close=[callback=std::move(on_close)](const NoticeStamp&){if(callback)callback();};
  const auto reply=Acquire(owner,"00000000000000000000000000000000",content.generation,std::move(callbacks));
  if(!reply.accepted)return;s.legacy=true;Update(reply.state.identity,content);
}
void TransferNotice::Hide(HWND owner) {
  auto& s=*impl_;if(s.owner==owner&&s.legacy)Hide(s.model.Stamp().identity);
}
void TransferNotice::SetTaskbar(HWND owner,TaskbarMode mode,int percent) {
  auto& s=*impl_;if(!s.OnThread()||s.transition.active())return;
  s.transition.Finish([&]{if(s.taskbar)s.taskbar->Set(owner,mode,percent);return true;},[]{});
}
void TransferNotice::Detach(HWND owner,uint64_t observerToken) {
  auto& s=*impl_;if(!s.OnThread())return;
  if(observerToken&&!s.surface_state.Remove(reinterpret_cast<std::uintptr_t>(owner),observerToken))return;
  const bool owns=s.owner==owner&&s.model.active();
  if(owns){s.callbacks={};s.model.Retire();s.have=false;}
  if(s.transition.active())return;
  if(owns) {
    s.Process(s.model.Evaluate(false,false,GetTickCount64()));
    return;
  }
  SetTaskbar(owner,TaskbarMode::none,0);
}

HWND TransferNotice::window() const { return impl_->hwnd; }
bool TransferNotice::visible() const { return impl_->Visible(); }
void TransferNotice::Refresh() { impl_->Evaluate(); }
TransferNoticeContent TransferNotice::content() const { return impl_->content; }
bool TransferNotice::dismissed() const { return impl_->dismissed; }
void TransferNotice::PressCancel() { impl_->Cancel(); }
void TransferNotice::PressClose() { impl_->Close(); }
void TransferNotice::PressOpen() { impl_->Open(); }
void TransferNotice::PressDetails() { impl_->ToggleDetails(); }
bool TransferNotice::expanded() const { return impl_->expanded; }

}  // namespace multitap
