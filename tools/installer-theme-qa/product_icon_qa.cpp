#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <winver.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "../../apps/windows_host/src/setup_product_icon.hpp"

namespace {
#ifndef MULTITAP_ICON_MISSING_TEST
bool matchesColor(const Gdiplus::Color& pixel, int red, int green, int blue) {
  return std::abs(int(pixel.GetR()) - red) <= 35 &&
         std::abs(int(pixel.GetG()) - green) <= 35 &&
         std::abs(int(pixel.GetB()) - blue) <= 35;
}

bool sample(Gdiplus::Bitmap& image, const double scale, const double x,
            const double y, int red, int green, int blue) {
  Gdiplus::Color pixel;
  const auto status = image.GetPixel(
      static_cast<int>(std::lround((4 + x / 120 * 40) * scale)),
      static_cast<int>(std::lround((4 + y / 120 * 40) * scale)), &pixel);
  return status == Gdiplus::Ok && matchesColor(pixel, red, green, blue);
}

bool save(Gdiplus::Bitmap& image, const std::filesystem::path& path) {
  UINT count = 0, bytes = 0;
  if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok) return false;
  std::vector<BYTE> buffer(bytes);
  auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
  if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return false;
  for (UINT index = 0; index < count; ++index) {
    if (std::wstring(encoders[index].MimeType) == L"image/png") {
      return image.Save(path.c_str(), &encoders[index].Clsid, nullptr) == Gdiplus::Ok;
    }
  }
  return false;
}

bool metadata(const bool host) {
  wchar_t executable[32768]{};
  if (!GetModuleFileNameW(nullptr, executable, 32768)) return false;
  DWORD ignored = 0;
  const auto size = GetFileVersionInfoSizeW(executable, &ignored);
  if (!size) return false;
  std::vector<BYTE> bytes(size);
  if (!GetFileVersionInfoW(executable, 0, size, bytes.data())) return false;
  const auto equals = [&](const wchar_t* name, const wchar_t* expected) {
    void* value = nullptr;
    UINT length = 0;
    const auto key = std::wstring(L"\\StringFileInfo\\040904e4\\") + name;
    return VerQueryValueW(bytes.data(), key.c_str(), &value, &length) &&
           length && std::wstring(static_cast<wchar_t*>(value)) == expected;
  };
  return equals(L"ProductName", host ? L"Multitap Host" : L"Multitap") &&
         equals(L"FileDescription", host ? L"Multitap Host Settings" : L"Multitap Windows Client") &&
         equals(L"InternalName", host ? L"MultitapHostSettings" : L"Multitap") &&
         equals(L"OriginalFilename", host ? L"MultitapHostSettings.exe" : L"Multitap.exe");
}
#endif
}  // namespace

int wmain(int argc, wchar_t** argv) {
#ifdef MULTITAP_ICON_MISSING_TEST
  (void) argc;
  (void) argv;
#endif
  Gdiplus::GdiplusStartupInput startup;
  ULONG_PTR token = 0;
  if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) return 1;
  int failed = 0;
  {
    for (const double scale : {1., 1.5, 2.}) {
      Gdiplus::Bitmap image(static_cast<int>(48 * scale), static_cast<int>(48 * scale), PixelFormat32bppARGB);
      Gdiplus::Graphics graphics(&image);
      graphics.Clear(Gdiplus::Color(255, 7, 11, 17));
      const bool drawn = multitap::setup::drawProductIcon(graphics, scale, 4, 4, 40);
      graphics.Flush(Gdiplus::FlushIntentionSync);
#ifdef MULTITAP_ICON_MISSING_TEST
      if (drawn) ++failed;
#else
      // The amber tile with the dark power strip, in 120 units of the icon:
      // tile above and below the mark and through the middle hole; the bar
      // left of the first hole and above and below the holes; the cord.
      // (The 40, 48 and 256 px grids all agree at these points.)
      if (!drawn || !sample(image, scale, 60, 15, 245, 166, 35) ||
          !sample(image, scale, 60, 105, 245, 166, 35) ||
          !sample(image, scale, 67, 58.5, 245, 166, 35) ||
          !sample(image, scale, 36, 58.5, 30, 21, 3) ||
          !sample(image, scale, 60, 46, 30, 21, 3) ||
          !sample(image, scale, 60, 71, 30, 21, 3) ||
          !sample(image, scale, 19, 80, 30, 21, 3)) ++failed;
      if (argc >= 3 && !save(image, std::filesystem::path(argv[2]) /
          (L"product-mark-" + std::to_wstring(int(scale * 100)) + L".png"))) ++failed;
#endif
      // Animated repaint reuses an owned handle without changing appearance.
      const DWORD handles = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
      for (int paint = 0; paint < 100; ++paint) {
        if (multitap::setup::drawProductIcon(graphics, scale, 4, 4, 40) != drawn) ++failed;
      }
      if (GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) > handles) ++failed;
    }
#ifndef MULTITAP_ICON_MISSING_TEST
    if (argc < 2 || !metadata(std::wstring(argv[1]) == L"host")) ++failed;
#endif
  }
  Gdiplus::GdiplusShutdown(token);
  std::printf("{\"passed\":%s,\"dpiCases\":3,\"repaints\":300,\"windowsCreated\":0,\"failures\":%d}\n",
              failed ? "false" : "true", failed);
  return failed ? 1 : 0;
}
