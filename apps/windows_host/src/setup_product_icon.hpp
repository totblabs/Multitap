#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cmath>
#include <map>

namespace multitap::setup {

// Resource 1 is the product icon (the amber tile with the dark power-strip
// mark, tools/icons) used by the executable and title
// bar. Reuse it instead of maintaining a separately painted product logo.
class ProductIcon {
 public:
  explicit ProductIcon(const int pixels)
      : handle_(static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
            pixels, pixels, LR_DEFAULTCOLOR))) {}
  ~ProductIcon() {
    if (handle_) DestroyIcon(handle_);
  }
  ProductIcon(const ProductIcon&) = delete;
  ProductIcon& operator=(const ProductIcon&) = delete;
  HICON get() const { return handle_; }

 private:
  HICON handle_ = nullptr;
};

inline bool drawProductIcon(Gdiplus::Graphics& graphics, const double scale,
                            const double x, const double y, const double size) {
  const double physicalSize = size * scale;
  if (!std::isfinite(physicalSize) || physicalSize < 1 || physicalSize > 4096) {
    return false;
  }
  const int pixels = static_cast<int>(std::lround(physicalSize));
  // UI-thread cache: animated progress must not repeatedly load and destroy
  // the same Windows icon data. Each cached handle is owned and released.
  static thread_local std::map<int, ProductIcon> icons;
  auto [icon, inserted] = icons.try_emplace(pixels, pixels);
  (void) inserted;
  if (!icon->second.get()) {
    icons.erase(icon);
    return false;
  }
  HDC target = graphics.GetHDC();
  if (!target) return false;
  const BOOL drawn = DrawIconEx(
      target, static_cast<int>(std::lround(x * scale)),
      static_cast<int>(std::lround(y * scale)), icon->second.get(),
      pixels, pixels, 0, nullptr, DI_NORMAL);
  graphics.ReleaseHDC(target);
  return drawn && graphics.GetLastStatus() == Gdiplus::Ok;
}

}  // namespace multitap::setup
