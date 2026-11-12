// Real area scaler, Windows WIC JPEG encoder/decoder and rename-by-handle
// replacement (links refused) under a temporary folder. Synthetic frames: this measures thumbnail cost and
// file behaviour, not VM capture, decode or relay behaviour.
#include "thumbnail_writer.h"
#include <winioctl.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

multitap::DecodedFrame frame(uint32_t width, uint32_t height, uint32_t seed) {
  multitap::DecodedFrame result;
  result.width = width;
  result.height = height;
  result.rgba.resize(static_cast<size_t>(width) * height * 4);
  uint32_t state = seed * 2654435761u + 1;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint8_t* p = result.rgba.data() + (static_cast<size_t>(y) * width + x) * 4;
      // Desktop-like content: a wallpaper gradient, a window and "text" rows.
      p[0] = static_cast<uint8_t>(20 + x * 60 / width);
      p[1] = static_cast<uint8_t>(40 + y * 80 / height);
      p[2] = static_cast<uint8_t>(140 + (x + y) * 90 / (width + height));
      if (x > width / 6 && x < width * 4 / 6 && y > height / 6 && y < height * 5 / 6) {
        p[0] = p[1] = p[2] = 246;
        if ((y / 9) % 2 == 0 && (x / 5) % 7 != 0) {
          state = state * 1664525u + 1013904223u;
          const uint8_t ink = static_cast<uint8_t>(30 + (state >> 27));
          p[0] = p[1] = p[2] = ink;
        }
      }
      p[3] = 255;
    }
  }
  return result;
}

multitap::DecodedFrame halves(uint32_t width, uint32_t height) {
  multitap::DecodedFrame result;
  result.width = width;
  result.height = height;
  result.rgba.resize(static_cast<size_t>(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) {
      uint8_t* p = result.rgba.data() + (static_cast<size_t>(y) * width + x) * 4;
      const bool left = x < width / 2;
      p[0] = left ? 255 : 0; p[1] = 0; p[2] = left ? 0 : 255; p[3] = 255;
    }
  return result;
}

struct Decoded { uint32_t width = 0, height = 0; std::vector<uint8_t> bgr; };
Decoded decode(IWICImagingFactory* factory, const std::wstring& path) {
  ComPtr<IWICBitmapDecoder> decoder;
  require(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnDemand, &decoder)), "Picture is not decodable");
  GUID container{};
  require(SUCCEEDED(decoder->GetContainerFormat(&container)) && IsEqualGUID(container, GUID_ContainerFormatJpeg), "Picture is not JPEG");
  ComPtr<IWICBitmapFrameDecode> picture;
  require(SUCCEEDED(decoder->GetFrame(0, &picture)), "No picture frame");
  ComPtr<IWICFormatConverter> converter;
  require(SUCCEEDED(factory->CreateFormatConverter(&converter)), "No converter");
  require(SUCCEEDED(converter->Initialize(picture.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                          WICBitmapPaletteTypeCustom)), "Conversion failed");
  Decoded result;
  require(SUCCEEDED(converter->GetSize(&result.width, &result.height)), "No size");
  result.bgr.resize(static_cast<size_t>(result.width) * result.height * 3);
  require(SUCCEEDED(converter->CopyPixels(nullptr, result.width * 3, static_cast<UINT>(result.bgr.size()), result.bgr.data())),
          "Copy failed");
  return result;
}

bool similar(const Decoded& image, uint32_t x, uint32_t y, int b, int g, int r) {
  const uint8_t* p = image.bgr.data() + (static_cast<size_t>(y) * image.width + x) * 3;
  return std::abs(p[0] - b) < 40 && std::abs(p[1] - g) < 40 && std::abs(p[2] - r) < 40;
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

// A directory junction (mount point) at |link| resolving to |target|; no
// privilege is needed, so it stands in for every link case.
bool junction(const std::wstring& link, const std::wstring& target) {
  if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
  const HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (directory == INVALID_HANDLE_VALUE) return false;
  const std::wstring substitute = L"\\??\\" + target, print = target;
  const size_t names = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
  std::vector<unsigned char> buffer(16 + names, 0);
  const auto put16 = [&](size_t at, size_t value) { buffer[at] = BYTE(value & 0xff); buffer[at + 1] = BYTE((value >> 8) & 0xff); };
  const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
  std::memcpy(buffer.data(), &tag, 4);
  put16(4, 8 + names); put16(8, 0); put16(10, substitute.size() * 2); put16(12, (substitute.size() + 1) * 2); put16(14, print.size() * 2);
  std::memcpy(buffer.data() + 16, substitute.c_str(), (substitute.size() + 1) * 2);
  std::memcpy(buffer.data() + 16 + (substitute.size() + 1) * 2, print.c_str(), (print.size() + 1) * 2);
  DWORD returned = 0;
  const bool made = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, buffer.data(), DWORD(buffer.size()), nullptr, 0, &returned, nullptr) != FALSE;
  CloseHandle(directory);
  return made;
}
bool empty(const std::filesystem::path& folder) {
  return std::filesystem::is_directory(folder) && std::filesystem::directory_iterator(folder) == std::filesystem::directory_iterator();
}
std::wstring Ascii(const std::string& value) { return std::wstring(value.begin(), value.end()); }
bool isLink(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
}  // namespace

int main() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com)) return 3;
  const auto root = std::filesystem::temp_directory_path() /
                    (L"multitap-thumbnail-smoke-" + std::to_wstring(GetCurrentProcessId()));
  int code = 0;
  try {
    std::filesystem::create_directories(root);
    ComPtr<IWICImagingFactory> factory;
    require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))),
            "WIC unavailable");
    using W = multitap::ThumbnailWriter;
    const std::string key = "0123456789abcdef";
    // Path validation: only a 16-hex account key and a pve-<digits> or
    // pc-<lowercase UUID> desktop.
    const auto target = W::Resolve(key, "pve-149", root.wstring());
    require(target && target->path == (root / L"0123456789abcdef" / L"pve-149.jpg").wstring(), "Unexpected picture path");
    const auto pc = W::Resolve(key, "pc-0123abcd-4567-89ef-0123-456789abcdef", root.wstring());
    require(pc && pc->path == (root / L"0123456789abcdef" / L"pc-0123abcd-4567-89ef-0123-456789abcdef.jpg").wstring(),
            "Unexpected PC picture path");
    for (const char* bad : {"0123456789ABCDEF", "0123456789abcde", "0123456789abcdeg", "../456789abcdef0", ""})
      require(!W::Resolve(bad, "pve-149", root.wstring()), "Accepted an invalid account key");
    for (const char* bad : {"pve-", "pve-1a", "../pve-1", "pve-1234567890123", "PVE-1", "pve-1\\x",
                            "pc-", "pc-0123ABCD-4567-89ef-0123-456789abcdef", "pc-0123abcd-4567-89ef-0123-456789abcde",
                            "pc-0123abcd-4567-89ef-0123-456789abcdef0", "pc-0123abcd_4567-89ef-0123-456789abcdef",
                            "pc-0123abcd-4567-89ef-0123-456789abcde\\", "pc-../../../../../../../../../../../../"})
      require(!W::Resolve(key, bad, root.wstring()), "Accepted an invalid desktop id");

    // Aspect is kept inside 480x270 and small frames are never enlarged.
    const struct { uint32_t w, h, tw, th; } sizes[] = {
        {1920, 1080, 480, 270}, {2560, 1440, 480, 270}, {3840, 2160, 480, 270},
        {1280, 800, 432, 270}, {800, 600, 360, 270}, {320, 200, 320, 200}, {1080, 1920, 152, 270}};
    for (const auto& size : sizes) {
      uint32_t w = 0, h = 0, stride = 0;
      std::vector<uint8_t> bgr;
      require(W::Scale(frame(size.w, size.h, 1), w, h, stride, bgr), "Scale rejected a valid frame");
      require(w == size.tw && h == size.th && stride % 4 == 0 && bgr.size() == static_cast<size_t>(stride) * h,
              "Unexpected thumbnail size");
    }
    multitap::DecodedFrame broken = frame(64, 64, 1);
    broken.rgba.pop_back();
    uint32_t w = 0, h = 0, stride = 0;
    std::vector<uint8_t> bgr;
    require(!W::Scale(broken, w, h, stride, bgr), "Scale accepted a short buffer");

    // Channel order: RGBA in, the JPEG decodes red on the left, blue on the right.
    require(SUCCEEDED(W::Write(*target, halves(1920, 1080), factory.Get())), "Write failed");
    auto picture = decode(factory.Get(), target->path);
    require(picture.width == 480 && picture.height == 270, "Decoded size mismatch");
    require(similar(picture, 40, 135, 0, 0, 255) && similar(picture, 440, 135, 255, 0, 0), "Colour channels swapped");
    // Replacement keeps one complete file and leaves no temporary behind.
    require(SUCCEEDED(W::Write(*target, frame(1280, 800, 2), factory.Get())), "Rewrite failed");
    picture = decode(factory.Get(), target->path);
    require(picture.width == 432 && picture.height == 270, "Replacement not visible");
    require(!std::filesystem::exists(target->path + L".tmp"), "Temporary file left behind");
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(target->folder))
      if (entry.is_regular_file()) ++files;
    require(files == 1, "Unexpected files in the account folder");

    // Links: this elevated writer never writes through a junction. An account
    // folder replaced by a junction is refused and its target stays empty; a
    // junction planted under the temporary name is removed as the link
    // itself; an anchor this user does not own (the Windows folder) refuses
    // before anything is created.
    {
      const auto elsewhere = root / L"elsewhere";
      std::filesystem::create_directories(elsewhere);
      const std::string linkedKey = "fedcba9876543210";
      require(junction((root / Ascii(linkedKey)).wstring(), elsewhere.wstring()), "Junction fixture");
      const auto linked = W::Resolve(linkedKey, "pve-149", root.wstring());
      require(linked.has_value(), "Linked target");
      const HRESULT refused = W::Write(*linked, frame(640, 360, 4), factory.Get());
      require(refused == HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK), "Wrote through a junctioned account folder");
      require(empty(elsewhere) && isLink(root / Ascii(linkedKey)), "The junction target changed");

      const auto planted = std::filesystem::path(target->path + L".tmp");
      const auto plantedTarget = root / L"planted";
      std::filesystem::create_directories(plantedTarget);
      std::filesystem::remove(target->path);
      require(junction(planted.wstring(), plantedTarget.wstring()), "Temporary-name junction fixture");
      require(SUCCEEDED(W::Write(*target, frame(640, 360, 5), factory.Get())), "A planted temporary link blocked the picture");
      require(empty(plantedTarget) && !std::filesystem::exists(planted) && std::filesystem::is_regular_file(target->path),
              "The planted link was followed or left behind");

      wchar_t windows[MAX_PATH + 1]{};
      GetWindowsDirectoryW(windows, MAX_PATH + 1);
      const auto foreign = W::Resolve(key, "pve-149", windows);
      require(foreign.has_value(), "Foreign anchor target");
      require(W::Write(*foreign, frame(640, 360, 6), factory.Get()) == E_ACCESSDENIED, "Wrote under a folder this user does not own");
      require(!std::filesystem::exists(std::filesystem::path(windows) / Ascii(key)), "Created a folder under a foreign anchor");
      RemoveDirectoryW((root / Ascii(linkedKey)).c_str());  // the link itself, never its target
    }

    // The asynchronous writer coalesces pictures for one file, reports the
    // identifiers once and releases the engine's frame reference.
    {
      W writer;
      const auto shared = std::make_shared<const multitap::DecodedFrame>(frame(1920, 1080, 3));
      const auto second = W::Resolve(key, "pve-150", root.wstring());
      require(second.has_value(), "Second target");
      for (int i = 0; i < 3; ++i) require(writer.Submit(*target, shared), "Submit refused");
      require(writer.Submit(*second, shared), "Submit refused");
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!writer.Idle() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      require(writer.Idle(), "Writer did not finish");
      const auto done = writer.TakeCompleted();
      require(!done.empty() && done.size() <= 2, "Unexpected completion count");
      require(std::all_of(done.begin(), done.end(), [&](const auto& item) { return item.first == key; }), "Wrong key reported");
      require(shared.use_count() == 1, "Writer kept the frame reference");
      require(writer.TakeCompleted().empty(), "Completions repeated");
      require(std::filesystem::exists(second->path), "Second picture missing");
    }

    // Cost per thumbnail on this PC (worker-thread work only).
    std::cout << "{\"thumbnails\":[";
    const struct { uint32_t w, h; } timed[] = {{1920, 1080}, {2560, 1440}, {3840, 2160}};
    bool first = true;
    for (const auto& size : timed) {
      const auto source = frame(size.w, size.h, 7);
      std::vector<double> scale, encode, write, total;
      W::Timing timing;
      for (int i = 0; i < 15; ++i) {
        require(SUCCEEDED(W::Write(*target, source, factory.Get(), &timing)), "Timed write failed");
        if (i < 3) continue;  // Exclude warm-up (code/page faults, first WIC use).
        scale.push_back(timing.scaleMs);
        encode.push_back(timing.encodeMs);
        write.push_back(timing.writeMs);
        total.push_back(timing.scaleMs + timing.encodeMs + timing.writeMs);
      }
      std::cout << (first ? "" : ",") << "{\"source\":\"" << size.w << "x" << size.h << "\",\"output\":\"" << timing.width << "x"
                << timing.height << "\",\"bytes\":" << timing.bytes << ",\"medianScaleMs\":" << median(scale)
                << ",\"medianEncodeMs\":" << median(encode) << ",\"medianWriteMs\":" << median(write)
                << ",\"medianTotalMs\":" << median(total) << ",\"maxTotalMs\":" << *std::max_element(total.begin(), total.end())
                << "}";
      first = false;
    }
    std::cout << "],\"syntheticFrames\":true,\"actualVmStream\":false}" << std::endl;
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    code = 2;
  }
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  CoUninitialize();
  return code;
}
