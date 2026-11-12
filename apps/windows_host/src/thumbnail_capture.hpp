#pragma once
// thumbnail-capture-v1: one
// small picture of the signed-in Windows desktop for the launcher cards. It is
// taken only when the broker asks (a client shows its launcher, about once a
// minute), never continuously and never while nobody looks.
//
// Path. The Session 0 service checks the console session first: no signed-in
// session answers `signin`, a locked one `locked`, and nothing is started. Only
// an unlocked console session gets one short helper run of this executable
// (--thumbnail-capture) with the signed-in user's own unelevated token, the
// drive/print helper path (drive::runDriveMountHelper): job-bound, no window,
// 8 s limit, 64 KiB answer. The helper sits on winsta0\default; with a user
// token it cannot open Winlogon's secure desktop, so the lock screen, the
// sign-in screen and a UAC prompt are never captured: when the input desktop
// is not Default it answers `locked` without taking a picture. A WTS change
// during the capture (the privacy epoch moved) discards the picture.
//
// Bounds. The primary monitor, scaled to at most 480x270 (GDI HALFTONE), JPEG
// at most 40,000 bytes; at most one capture per 10 s on the Host (the broker
// asks at most every 30 s per desktop). The picture exists only in memory on
// its way to the broker: never on disk, in a log or in diagnostics.
// Diagnostics: hostAction 280, stage = state code, kib = size bucket, on a
// state change only (the broker keeps the newest eight Host events).
#include "support.hpp"
#include "operational_log.hpp"
#include "thumbnail_privacy.hpp"
#include <wincodec.h>
#include <objidl.h>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _MSC_VER
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "crypt32.lib")
#endif

namespace host {
inline constexpr char thumbnailCaptureProtocol[] = "thumbnail-capture-v1";
inline constexpr uint32_t thumbnailAction = 280;
inline constexpr uint32_t thumbnailMaxWidth = 480, thumbnailMaxHeight = 270;
inline constexpr size_t thumbnailMaxBytes = 40000;
inline constexpr std::chrono::seconds thumbnailMinInterval{10};
inline constexpr DWORD thumbnailHelperTimeoutMs = 8000;
inline constexpr uint8_t thumbnailMagic[4] = {'T', 'V', 'P', '1'};

// Wire names and diagnostic stage codes (hostAction 280).
enum class ThumbnailState : uint32_t { failed = 0, captured = 1, blank = 2, locked = 3, signin = 4, busy = 5 };
inline const char* thumbnailStateName(ThumbnailState state) {
  switch (state) {
    case ThumbnailState::captured: return "captured";
    case ThumbnailState::blank: return "blank";
    case ThumbnailState::locked: return "locked";
    case ThumbnailState::signin: return "signin";
    case ThumbnailState::busy: return "busy";
    default: return "failed";
  }
}
// One answer to the broker. `epoch` is the thumbnail-privacy-v1 epoch the
// picture belongs to (captured/blank only).
struct ThumbnailShot {
  ThumbnailState state = ThumbnailState::failed;
  std::string epoch;
  std::vector<uint8_t> jpeg;
  uint32_t width = 0, height = 0;
};

inline bool validThumbnailRequestId(const std::string& value) {
  if (value.size() != 32) return false;
  for (const char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
// Fits inside 480x270 keeping the aspect; never enlarges.
inline std::pair<uint32_t, uint32_t> thumbnailScaledSize(uint32_t width, uint32_t height) {
  if (!width || !height) return {0, 0};
  const double scale = std::min({1.0, double(thumbnailMaxWidth) / width, double(thumbnailMaxHeight) / height});
  return {std::max<uint32_t>(1, uint32_t(std::lround(width * scale))), std::max<uint32_t>(1, uint32_t(std::lround(height * scale)))};
}
// A powered-down or blanked output reads as black: keep the last picture.
inline bool thumbnailBlank(const uint8_t* bgra, size_t pixels) {
  for (size_t i = 0; i < pixels; ++i) if (bgra[i * 4] > 12 || bgra[i * 4 + 1] > 12 || bgra[i * 4 + 2] > 12) return false;
  return true;
}
inline bool thumbnailJpeg(const std::vector<uint8_t>& bytes) {
  return bytes.size() >= 4 && bytes.size() <= thumbnailMaxBytes && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[bytes.size() - 2] == 0xff && bytes.back() == 0xd9;
}
// Binary record on the broker channel, right before its result: "TVP1", the
// 32-character request id, then the JPEG. The first byte keeps it out of the
// media classes of the send queue (never dropped, never sent over UDP).
inline std::vector<uint8_t> thumbnailPacket(const std::string& requestId, const std::vector<uint8_t>& jpeg) {
  std::vector<uint8_t> out(std::begin(thumbnailMagic), std::end(thumbnailMagic));
  out.insert(out.end(), requestId.begin(), requestId.end());
  out.insert(out.end(), jpeg.begin(), jpeg.end());
  return out;
}
inline std::string thumbnailResultJson(const std::string& requestId, const ThumbnailShot& shot) {
  std::string out = "{\"type\":\"host.thumbnail.result\",\"requestId\":" + jsonQuote(requestId) + ",\"state\":\"" + thumbnailStateName(shot.state) + '"';
  if ((shot.state == ThumbnailState::captured || shot.state == ThumbnailState::blank) && ThumbnailPrivacyEpoch::valid(shot.epoch)) out += ",\"epoch\":" + jsonQuote(shot.epoch);
  if (shot.state == ThumbnailState::captured) out += ",\"bytes\":" + std::to_string(shot.jpeg.size()) + ",\"width\":" + std::to_string(shot.width) + ",\"height\":" + std::to_string(shot.height);
  return out + '}';
}
inline std::string thumbnailCaptureCapabilityFields() { return std::string(",\"thumbnailCaptureProtocol\":\"") + thumbnailCaptureProtocol + '"'; }

inline std::string thumbnailBase64(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) return {};
  DWORD length = 0;
  if (!CryptBinaryToStringA(bytes.data(), DWORD(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length)) return {};
  std::string out(length, '\0');
  if (!CryptBinaryToStringA(bytes.data(), DWORD(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &length)) return {};
  out.resize(length);
  while (!out.empty() && out.back() == '\0') out.pop_back();
  return out;
}
inline std::optional<std::vector<uint8_t>> thumbnailUnbase64(const std::string& text) {
  if (text.empty() || text.size() > (thumbnailMaxBytes + 2) / 3 * 4) return std::nullopt;
  for (const char c : text) if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) return std::nullopt;
  DWORD length = 0;
  if (!CryptStringToBinaryA(text.data(), DWORD(text.size()), CRYPT_STRING_BASE64, nullptr, &length, nullptr, nullptr) || length > thumbnailMaxBytes) return std::nullopt;
  std::vector<uint8_t> out(length);
  if (!CryptStringToBinaryA(text.data(), DWORD(text.size()), CRYPT_STRING_BASE64, out.data(), &length, nullptr, nullptr)) return std::nullopt;
  out.resize(length);
  return out;
}

// The helper's one JSON line: {"state":"captured","width":W,"height":H,
// "jpeg":"<base64>"}, or {"state":"blank"|"locked"|"failed"}.
inline ThumbnailShot parseThumbnailHelperAnswer(const Json& value) {
  ThumbnailShot shot;
  const auto state = value.get("state").string();
  if (state == "blank") shot.state = ThumbnailState::blank;
  else if (state == "locked") shot.state = ThumbnailState::locked;
  else if (state == "captured") {
    const double width = value.get("width").number(-1), height = value.get("height").number(-1);
    auto jpeg = thumbnailUnbase64(value.get("jpeg").string());
    if (!jpeg || !thumbnailJpeg(*jpeg) || width < 1 || height < 1 || width > thumbnailMaxWidth || height > thumbnailMaxHeight || width != std::floor(width) || height != std::floor(height)) return shot;
    shot.state = ThumbnailState::captured; shot.jpeg = std::move(*jpeg); shot.width = uint32_t(width); shot.height = uint32_t(height);
  }
  return shot;
}

// ---- helper side (the signed-in user's session and token) ----
inline std::vector<uint8_t> encodeThumbnailJpeg(const uint8_t* bgra, uint32_t width, uint32_t height, float quality) {
  struct Release { IUnknown* value = nullptr; ~Release() { if (value) value->Release(); } };
  IWICImagingFactory* factory = nullptr; IStream* stream = nullptr; IWICBitmapEncoder* encoder = nullptr; IWICBitmapFrameEncode* frame = nullptr; IPropertyBag2* options = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  Release a{factory};
  if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  Release b{stream};
  if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
  Release c{encoder};
  if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
  if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
  Release d{frame}, e{options};
  if (SUCCEEDED(hr)) { PROPBAG2 name{}; name.pstrName = const_cast<LPOLESTR>(L"ImageQuality"); VARIANT q; VariantInit(&q); q.vt = VT_R4; q.fltVal = quality; hr = options->Write(1, &name, &q); }
  if (SUCCEEDED(hr)) hr = frame->Initialize(options);
  if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
  if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
  const UINT stride = (width * 3 + 3) & ~3u; std::vector<uint8_t> bgr(size_t(stride) * height);
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) { const auto* from = bgra + (size_t(y) * width + x) * 4; auto* to = bgr.data() + size_t(y) * stride + x * 3; to[0] = from[0]; to[1] = from[1]; to[2] = from[2]; }
  if (SUCCEEDED(hr)) hr = frame->WritePixels(height, stride, UINT(bgr.size()), bgr.data());
  if (SUCCEEDED(hr)) hr = frame->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  STATSTG stat{};
  if (SUCCEEDED(hr)) hr = stream->Stat(&stat, STATFLAG_NONAME);
  HGLOBAL memory = nullptr;
  if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stream, &memory);
  if (FAILED(hr) || !stat.cbSize.QuadPart || stat.cbSize.QuadPart > 4 * 1024 * 1024 || stat.cbSize.QuadPart > GlobalSize(memory)) return {};
  const auto* bytes = static_cast<const uint8_t*>(GlobalLock(memory));
  if (!bytes) return {};
  std::vector<uint8_t> out(bytes, bytes + size_t(stat.cbSize.QuadPart));
  GlobalUnlock(memory);
  return out;
}
// Pixels of the scaled primary monitor (BGRA, top-down) to the helper's answer.
inline std::string thumbnailHelperAnswer(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height) {
  if (!width || !height || width > thumbnailMaxWidth || height > thumbnailMaxHeight || bgra.size() != size_t(width) * height * 4) return "{\"state\":\"failed\"}";
  if (thumbnailBlank(bgra.data(), size_t(width) * height)) return "{\"state\":\"blank\"}";
  // Text-heavy screens compress worse: step the quality down to fit the bound.
  for (const float quality : {0.72f, 0.55f, 0.4f, 0.28f}) {
    const auto jpeg = encodeThumbnailJpeg(bgra.data(), width, height, quality);
    if (jpeg.empty()) break;
    if (thumbnailJpeg(jpeg)) return "{\"state\":\"captured\",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) + ",\"jpeg\":\"" + thumbnailBase64(jpeg) + "\"}";
  }
  return "{\"state\":\"failed\"}";
}
// Only this session's Default desktop, never Winlogon's (lock, sign-in, UAC).
inline bool thumbnailInputDesktopIsDefault() {
  const HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!input) return false;
  wchar_t name[64]{}; DWORD needed = 0;
  const bool ok = GetUserObjectInformationW(input, UOI_NAME, name, sizeof(name) - sizeof(wchar_t), &needed) != FALSE;
  CloseDesktop(input);
  return ok && _wcsicmp(name, L"Default") == 0;
}
inline std::string thumbnailScreenAnswer() {
  if (!thumbnailInputDesktopIsDefault()) return "{\"state\":\"locked\"}";
  const int screenWidth = GetSystemMetrics(SM_CXSCREEN), screenHeight = GetSystemMetrics(SM_CYSCREEN);
  if (screenWidth < 16 || screenHeight < 16 || screenWidth > 16384 || screenHeight > 16384) return "{\"state\":\"failed\"}";
  const auto [width, height] = thumbnailScaledSize(uint32_t(screenWidth), uint32_t(screenHeight));
  const HDC screen = GetDC(nullptr);
  if (!screen) return "{\"state\":\"failed\"}";
  const HDC memory = CreateCompatibleDC(screen);
  BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = LONG(width); info.bmiHeader.biHeight = -LONG(height); info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  const HBITMAP bitmap = memory ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
  bool copied = false;
  if (bitmap && bits) {
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    SetStretchBltMode(memory, HALFTONE); SetBrushOrgEx(memory, 0, 0, nullptr);
    copied = StretchBlt(memory, 0, 0, int(width), int(height), screen, 0, 0, screenWidth, screenHeight, SRCCOPY) != FALSE;
    GdiFlush(); SelectObject(memory, previous);
  }
  std::vector<uint8_t> pixels;
  if (copied) pixels.assign(static_cast<const uint8_t*>(bits), static_cast<const uint8_t*>(bits) + size_t(width) * height * 4);
  if (bitmap) DeleteObject(bitmap);
  if (memory) DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  // The secure desktop may have come up during the copy.
  if (!copied || !thumbnailInputDesktopIsDefault()) return copied ? "{\"state\":\"locked\"}" : "{\"state\":\"failed\"}";
  return thumbnailHelperAnswer(pixels, width, height);
}
// --thumbnail-capture: one answer line on stdout, then exit.
inline int thumbnailCaptureHelper() {
  try {
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) return 2;
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string answer;
    try { answer = thumbnailScreenAnswer(); } catch (...) { answer = "{\"state\":\"failed\"}"; }
    if (SUCCEEDED(com)) CoUninitialize();
    answer += '\n';
    DWORD written = 0;
    return WriteFile(output, answer.data(), DWORD(answer.size()), &written, nullptr) && written == answer.size() ? 0 : 1;
  } catch (...) { return 1; }
}

// ---- service side ----
struct ThumbnailSession { DWORD id = 0xffffffff; bool unlocked = false, console = false; };
class ThumbnailShooter {
 public:
  using Clock = std::chrono::steady_clock;
  using Observe = std::function<ThumbnailSession()>;
  using Capture = std::function<Json(DWORD session)>;
  using Privacy = std::function<ThumbnailPrivacyEpoch::Snapshot(bool allowed)>;
  using Record = std::function<void(const ThumbnailShot&)>;
  ThumbnailShooter(Observe observe, Capture capture, Record record = recordShot, std::function<Clock::time_point()> now = Clock::now)
      : observe_(std::move(observe)), capture_(std::move(capture)), record_(std::move(record)), now_(std::move(now)) {}
  // Blocking (one helper run at most); never on a channel thread.
  ThumbnailShot take(const Privacy& privacy) {
    auto shot = decide(privacy);
    try { if (record_ && changed(shot.state)) record_(shot); } catch (...) {}
    return shot;
  }
  static void recordShot(const ThumbnailShot& shot) {
    recordHostEvent("hostAction", {{"action", thumbnailAction}, {"stage", uint32_t(shot.state)}, {"ok", shot.state != ThumbnailState::failed}, {"kib", int64_t((shot.jpeg.size() + 1023) / 1024)}});
  }
 private:
  ThumbnailShot decide(const Privacy& privacy) {
    ThumbnailShot shot;
    ThumbnailSession before;
    try { before = observe_(); } catch (...) { return shot; }
    if (!before.id || before.id == 0xffffffff) { shot.state = ThumbnailState::signin; return shot; }
    if (!before.unlocked) { shot.state = ThumbnailState::locked; return shot; }
    if (!before.console) return shot;
    {
      std::lock_guard lock(mutex_);
      const auto now = now_();
      if (captured_ != Clock::time_point{} && now - captured_ < thumbnailMinInterval) { shot.state = ThumbnailState::busy; return shot; }
      captured_ = now;
    }
    Json answer;
    try { answer = capture_(before.id); } catch (...) { return shot; }
    ThumbnailSession after;
    try { after = observe_(); } catch (...) { after = {}; }
    const bool allowed = after.id == before.id && after.unlocked && after.console;
    ThumbnailPrivacyEpoch::Snapshot epoch;
    try { if (privacy) epoch = privacy(allowed); } catch (...) { epoch = {}; }
    // Locked, signed out or switched during the capture: the picture is dropped.
    if (!allowed) { shot.state = !after.id || after.id == 0xffffffff ? ThumbnailState::signin : ThumbnailState::locked; return shot; }
    if (!epoch.allowed || !ThumbnailPrivacyEpoch::valid(epoch.epoch)) return shot;
    shot = parseThumbnailHelperAnswer(answer);
    if (shot.state == ThumbnailState::captured || shot.state == ThumbnailState::blank) shot.epoch = epoch.epoch;
    return shot;
  }
  bool changed(ThumbnailState state) {
    std::lock_guard lock(mutex_);
    const bool first = !recorded_.has_value() || *recorded_ != state;
    recorded_ = state;
    return first;
  }
  Observe observe_; Capture capture_; Record record_; std::function<Clock::time_point()> now_;
  std::mutex mutex_; Clock::time_point captured_{}; std::optional<ThumbnailState> recorded_;
};

// One capture at a time, off the broker channel's threads. Its owner (the
// Agent) declares it last, so it is joined before anything the work uses.
class ThumbnailRunner {
 public:
  ThumbnailRunner() = default;
  ThumbnailRunner(const ThumbnailRunner&) = delete;
  ThumbnailRunner& operator=(const ThumbnailRunner&) = delete;
  ~ThumbnailRunner() { join(); }
  bool start(std::function<void()> work) {
    std::lock_guard lock(mutex_);
    if (busy_) return false;
    if (thread_.joinable()) thread_.join(); // finished: it set busy_ false last
    busy_ = true;
    thread_ = std::thread([this, work = std::move(work)] { try { work(); } catch (...) {} std::lock_guard done(mutex_); busy_ = false; });
    return true;
  }
  void join() { std::thread thread; { std::lock_guard lock(mutex_); thread = std::move(thread_); } if (thread.joinable()) thread.join(); }
 private:
  std::mutex mutex_; std::thread thread_; bool busy_ = false;
};
}
