// Differential tests for the direct capture path: every result is compared
// byte-for-byte with the previous full-frame implementation. Synthetic frames
// and system/owned cursors only; no desktop pixels are read.
#include "media.hpp"
#include "cursor_patch.hpp"
#include "frame_encode_policy.hpp"
#include <emmintrin.h>
#include <iostream>
#include <random>

namespace {
uint64_t checks = 0;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); ++checks; }
template<class F> void rejects(F action, const char* reason) { bool failed = false; try { action(); } catch (const std::exception&) { failed = true; } require(failed, reason); }

// The pre-change FrameEncodePolicy, verbatim apart from its name.
class ReferencePolicy {
 public:
  static constexpr uint64_t refreshIntervalUs = 1000000;
  static constexpr size_t maximumPendingInputs = 1024;
  explicit ReferencePolicy(size_t frameBytes) : frameBytes_(frameBytes) {}
  host::FrameEncodeDecision prepare(std::span<const uint8_t> frame, uint64_t nowUs) {
    if (frame.size() != frameBytes_ || (haveClock_ && nowUs < clockUs_)) throw std::runtime_error("Invalid encoding policy frame or clock");
    const bool changed = snapshot_.empty() || std::memcmp(snapshot_.data(), frame.data(), frameBytes_) != 0;
    const bool refresh = haveIdr_ && !requiredOutputPts_ && haveOutput_ && nowUs - lastOutputUs_ >= refreshIntervalUs;
    const bool submit = changed || refresh || !haveIdr_ || requiredOutputPts_.has_value();
    if (submit && (pending_.size() >= maximumPendingInputs || (haveSubmission_ && nowUs <= lastSubmissionUs_))) throw std::runtime_error("Encoder output timestamps did not progress");
    clockUs_ = nowUs; haveClock_ = true;
    if (!submit) return {};
    if (changed) snapshot_.assign(frame.begin(), frame.end());
    if (changed || refresh) requiredOutputPts_ = nowUs;
    pending_.push_back(nowUs); lastSubmissionUs_ = nowUs; haveSubmission_ = true;
    return {true, changed, refresh};
  }
  bool output(uint64_t ptsUs, bool keyFrame, uint64_t nowUs) {
    if (!haveSubmission_ || (haveClock_ && nowUs < clockUs_) || ptsUs > nowUs || (haveOutput_ && ptsUs <= lastOutputPts_)) return false;
    const auto found = std::find(pending_.begin(), pending_.end(), ptsUs);
    if (found == pending_.end()) return false;
    pending_.erase(pending_.begin(), std::next(found));
    clockUs_ = nowUs; haveClock_ = true; lastOutputUs_ = nowUs; lastOutputPts_ = ptsUs; haveOutput_ = true;
    if (keyFrame) haveIdr_ = true;
    if (requiredOutputPts_ && ptsUs >= *requiredOutputPts_) requiredOutputPts_.reset();
    return true;
  }
  bool hasUsableIdr() const { return haveIdr_; }
  bool awaitingOutput() const { return !haveIdr_ || requiredOutputPts_.has_value(); }
  size_t pendingInputs() const { return pending_.size(); }
 private:
  const size_t frameBytes_; std::vector<uint8_t> snapshot_; std::deque<uint64_t> pending_; std::optional<uint64_t> requiredOutputPts_;
  uint64_t clockUs_ = 0, lastSubmissionUs_ = 0, lastOutputUs_ = 0, lastOutputPts_ = 0; bool haveClock_ = false, haveSubmission_ = false, haveOutput_ = false, haveIdr_ = false;
};
void policyMatchesReference() {
  std::mt19937 random(7);
  for (const size_t bytes : {size_t(6), size_t(4095), size_t(4096), size_t(4097), size_t(96), size_t(640 * 360 * 3 / 2)}) {
    host::FrameEncodePolicy actual(bytes); ReferencePolicy expected(bytes);
    std::vector<uint8_t> frame(bytes, 9); std::vector<std::vector<uint8_t>> history; uint64_t now = 0; std::deque<uint64_t> queued;
    for (int step = 0; step < 900; ++step) {
      const auto action = random() % 10;
      if (action < 3) frame[random() % bytes] ^= uint8_t(1 + random() % 255);
      else if (action == 3) frame[0] ^= 1;
      else if (action == 4) frame[bytes - 1] ^= 0x80;
      else if (action == 5 && !history.empty()) frame = history[random() % history.size()];
      else if (action == 6) std::fill(frame.begin() + std::ptrdiff_t(random() % bytes), frame.end(), uint8_t(random()));
      history.push_back(frame); if (history.size() > 8) history.erase(history.begin());
      const auto delta = random() % 23 == 0 ? 0 : uint64_t(1 + random() % 40000); now += delta;
      std::optional<host::FrameEncodeDecision> a, b; bool aThrew = false, bThrew = false;
      try { a = actual.prepare(frame, now); } catch (const std::exception&) { aThrew = true; }
      try { b = expected.prepare(frame, now); } catch (const std::exception&) { bThrew = true; }
      require(aThrew == bThrew, "Policy rejection differs from the previous implementation");
      if (a) require(a->submit == b->submit && a->contentChanged == b->contentChanged && a->refresh == b->refresh, "Policy decision differs from the previous implementation");
      if (a && a->submit) queued.push_back(now);
      // Delayed, sometimes dropped encoder outputs.
      while (queued.size() > size_t(random() % 3)) {
        const auto pts = queued.front(); queued.pop_front(); if (random() % 17 == 0) continue;
        const bool key = random() % 5 == 0; const auto at = now + random() % 3;
        require(actual.output(pts, key, at) == expected.output(pts, key, at), "Policy output acceptance differs"); now = at;
      }
      require(actual.awaitingOutput() == expected.awaitingOutput() && actual.hasUsableIdr() == expected.hasUsableIdr() && actual.pendingInputs() == expected.pendingInputs(), "Policy state differs from the previous implementation");
    }
  }
}
// The previous paired-row SSE2 kernel, verbatim (4 pixels per row per step).
void previousKernel(const uint8_t* source, uint8_t* target, int width, int height) {
  const auto zero=_mm_setzero_si128();
  const auto yw=_mm_setr_epi16(25,129,66,0,25,129,66,0);
  const auto uw=_mm_setr_epi16(112,-74,-38,0,112,-74,-38,0);
  const auto vw=_mm_setr_epi16(-18,-94,112,0,-18,-94,112,0);
  const auto pair=[](__m128i value){return _mm_add_epi32(value,_mm_shuffle_epi32(value,_MM_SHUFFLE(2,3,0,1)));};
  const auto luma=[&](__m128i bytes) {
    const auto lo=pair(_mm_madd_epi16(_mm_unpacklo_epi8(bytes,zero),yw));
    const auto hi=pair(_mm_madd_epi16(_mm_unpackhi_epi8(bytes,zero),yw));
    auto sums=_mm_unpacklo_epi64(_mm_shuffle_epi32(lo,_MM_SHUFFLE(2,0,2,0)),_mm_shuffle_epi32(hi,_MM_SHUFFLE(2,0,2,0)));
    sums=_mm_add_epi32(_mm_srli_epi32(_mm_add_epi32(sums,_mm_set1_epi32(128)),8),_mm_set1_epi32(16));
    return uint32_t(_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(sums,zero),zero)));
  };
  const auto clamp=[](int value){return uint8_t(std::clamp(value,0,255));};
  const size_t stride=size_t(width)*4;
  auto* chroma=target+size_t(width)*height;
  for(int y=0;y<height;y+=2) {
    const auto* top=source+size_t(y)*stride;const auto* bottom=top+stride;
    auto* y0=target+size_t(y)*width;auto* y1=y0+width;auto* uv=chroma+size_t(y/2)*width;
    int x=0;
    for(;x+4<=width;x+=4) {
      const auto a=_mm_loadu_si128(reinterpret_cast<const __m128i*>(top+size_t(x)*4));
      const auto b=_mm_loadu_si128(reinterpret_cast<const __m128i*>(bottom+size_t(x)*4));
      const uint32_t first=luma(a),second=luma(b);
      std::memcpy(y0+x,&first,sizeof(first));std::memcpy(y1+x,&second,sizeof(second));
      auto lo=_mm_add_epi16(_mm_unpacklo_epi8(a,zero),_mm_unpacklo_epi8(b,zero));
      auto hi=_mm_add_epi16(_mm_unpackhi_epi8(a,zero),_mm_unpackhi_epi8(b,zero));
      lo=_mm_srli_epi16(_mm_add_epi16(lo,_mm_srli_si128(lo,8)),2);
      hi=_mm_srli_epi16(_mm_add_epi16(hi,_mm_srli_si128(hi,8)),2);
      const auto rgb=_mm_unpacklo_epi64(lo,hi);
      const auto u=pair(_mm_madd_epi16(rgb,uw)),v=pair(_mm_madd_epi16(rgb,vw));
      auto sums=_mm_unpacklo_epi64(_mm_unpacklo_epi32(u,v),_mm_unpackhi_epi32(u,v));
      sums=_mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(sums,_mm_set1_epi32(128)),8),_mm_set1_epi32(128));
      const uint32_t packed=uint32_t(_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(sums,zero),zero)));
      std::memcpy(uv+x,&packed,sizeof(packed));
    }
    for(;x<width;x+=2) {
      int red=0,green=0,blue=0;
      for(int row=0;row<2;++row)for(int dx=0;dx<2;++dx) {
        const auto* pixel=(row?bottom:top)+size_t(x+dx)*4;
        (row?y1:y0)[x+dx]=clamp(((66*pixel[2]+129*pixel[1]+25*pixel[0]+128)>>8)+16);
        red+=pixel[2];green+=pixel[1];blue+=pixel[0];
      }
      red/=4;green/=4;blue/=4;
      uv[x]=clamp(((-38*red-74*green+112*blue+128)>>8)+128);
      uv[x+1]=clamp(((112*red-94*green-18*blue+128)>>8)+128);
    }
  }
}
// Independent scalar BT.601 limited-range reference.
void scalarReference(const uint8_t* source, uint8_t* target, int width, int height) {
  const auto clip = [](int n) { return uint8_t(std::clamp(n, 0, 255)); };
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) { const auto* p = source + (size_t(y) * width + x) * 4; target[size_t(y) * width + x] = clip(((66 * p[2] + 129 * p[1] + 25 * p[0] + 128) >> 8) + 16); }
  auto* uv = target + size_t(width) * height;
  for (int y = 0; y < height; y += 2) for (int x = 0; x < width; x += 2) {
    int r = 0, g = 0, b = 0; for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) { const auto* p = source + (size_t(y + dy) * width + x + dx) * 4; b += p[0]; g += p[1]; r += p[2]; }
    r /= 4; g /= 4; b /= 4; uv[size_t(y / 2) * width + x] = clip(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128); uv[size_t(y / 2) * width + x + 1] = clip(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
  }
}
uint64_t kernelBytes = 0;
void kernelMatchesPrevious() {
  std::mt19937 random(23);
  std::vector<std::pair<int, int>> sizes{{2, 2}, {4, 2}, {6, 2}, {8, 2}, {10, 4}, {12, 2}, {14, 6}, {16, 2}, {18, 10}, {66, 34}, {1366, 768}, {1920, 1080}, {2560, 1440}};
  for (int n = 0; n < 40; ++n) sizes.push_back({2 + 2 * int(random() % 80), 2 + 2 * int(random() % 20)});
  for (const auto [width, height] : sizes) for (int pattern = 0; pattern < 6; ++pattern) {
    std::vector<uint8_t> source(size_t(width) * height * 4 + 5);
    for (size_t i = 0; i < source.size(); ++i) { const auto r = uint8_t(random()); source[i] = pattern == 0 ? r : pattern == 1 ? 0 : pattern == 2 ? 255 : pattern == 3 ? uint8_t(i * 17) : pattern == 4 ? uint8_t(r & 1 ? 255 : 0) : uint8_t((i % 7) * 42); }
    const size_t bytes = size_t(width) * height * 3 / 2;
    std::vector<uint8_t> expected(bytes), previous(bytes), actual(bytes + 9, 0xa7);
    scalarReference(source.data() + 1, expected.data(), width, height); previousKernel(source.data() + 1, previous.data(), width, height);
    host::bgraToNv12(source.data() + 1, actual.data() + 3, width, height);
    require(previous == expected, "Previous kernel differs from the scalar reference");
    require(std::equal(expected.begin(), expected.end(), actual.begin() + 3), "Current kernel differs from the previous kernel/scalar reference");
    require(actual[0] == 0xa7 && actual[1] == 0xa7 && actual[2] == 0xa7 && std::all_of(actual.begin() + 3 + std::ptrdiff_t(bytes), actual.end(), [](uint8_t v) { return v == 0xa7; }), "Kernel wrote outside its frame");
    kernelBytes += bytes;
  }
  // Every 24-bit BGR value once (luma exhaustive; 4M structured chroma blocks).
  constexpr int side = 4096; std::vector<uint8_t> all(size_t(side) * side * 4), expected(size_t(side) * side * 3 / 2), actual(expected.size());
  for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) { auto* p = all.data() + (size_t(y) * side + x) * 4; p[0] = uint8_t(x); p[1] = uint8_t((x >> 8) | ((y & 15) << 4)); p[2] = uint8_t(y >> 4); p[3] = uint8_t(x ^ y); }
  scalarReference(all.data(), expected.data(), side, side); host::bgraToNv12(all.data(), actual.data(), side, side);
  require(actual == expected, "Current kernel differs from the scalar reference over all 24-bit colours");
  kernelBytes += expected.size();
}
std::vector<uint8_t> randomFrame(int width, int height, size_t pitch, uint32_t seed) {
  std::vector<uint8_t> frame(pitch * height); std::mt19937 random(seed);
  for (auto& value : frame) value = uint8_t(random());
  // Add flat areas and extremes so clamping paths are exercised too.
  for (int y = 0; y < height / 3; ++y) for (int x = 0; x < width / 4; ++x) { auto* p = frame.data() + size_t(y) * pitch + size_t(x) * 4; p[0] = p[1] = p[2] = uint8_t(y & 1 ? 255 : 0); }
  return frame;
}
std::vector<uint8_t> fullConvert(const std::vector<uint8_t>& frame, int width, int height, size_t pitch) {
  std::vector<uint8_t> packed(size_t(width) * height * 4), nv12(size_t(width) * height * 3 / 2);
  for (int y = 0; y < height; ++y) std::memcpy(packed.data() + size_t(y) * width * 4, frame.data() + size_t(y) * pitch, size_t(width) * 4);
  host::bgraToNv12(packed.data(), nv12.data(), width, height); return nv12;
}
void blockConversionMatchesFullFrame() {
  std::mt19937 random(11);
  for (const auto [width, height] : {std::pair{2, 2}, std::pair{6, 4}, std::pair{18, 10}, std::pair{646, 362}, std::pair{1920, 1080}}) {
    const size_t pitch = size_t(width) * 4 + 64 * (width % 3);
    const auto frame = randomFrame(width, height, pitch, uint32_t(width)); const auto expected = fullConvert(frame, width, height, pitch);
    std::vector<uint8_t> whole(expected.size(), 0xa5);
    host::capture::convertBlockToNv12(whole, width, height, frame.data(), pitch, {0, 0, width, height});
    require(whole == expected, "Pitched full-surface conversion differs from bgraToNv12");
    for (int n = 0; n < 60; ++n) {
      auto actual = expected; int l = int(random() % width) & ~1, t = int(random() % height) & ~1;
      int r = std::min(width, l + 2 + (int(random() % 130) & ~1)), b = std::min(height, t + 2 + (int(random() % 70) & ~1));
      const host::capture::PixelRect block{l, t, r, b};
      for (int y = t; y < b; ++y) std::fill_n(actual.begin() + std::ptrdiff_t(size_t(y) * width + l), r - l, uint8_t(0x5a));
      for (int y = t / 2; y < b / 2; ++y) std::fill_n(actual.begin() + std::ptrdiff_t(size_t(width) * height + size_t(y) * width + l), r - l, uint8_t(0x5a));
      host::capture::convertBlockToNv12(actual, width, height, frame.data() + size_t(t) * pitch + size_t(l) * 4, pitch, block);
      require(actual == expected, "Even block conversion differs from the full-frame bytes or touched other bytes");
    }
  }
  std::vector<uint8_t> nv12(64 * 32 * 3 / 2), source(64 * 32 * 4);
  rejects([&] { host::capture::convertBlockToNv12(nv12, 64, 32, source.data(), 256, {1, 0, 9, 8}); }, "Odd block accepted");
  rejects([&] { host::capture::convertBlockToNv12(nv12, 64, 32, source.data(), 256, {0, 0, 66, 8}); }, "Block outside the frame accepted");
  rejects([&] { host::capture::convertBlockToNv12(nv12, 64, 32, source.data(), 8, {0, 0, 8, 8}); }, "Short source pitch accepted");
  rejects([&] { host::capture::convertBlockToNv12(std::span(nv12).first(100), 64, 32, source.data(), 256, {0, 0, 8, 8}); }, "Short NV12 frame accepted");
}
void cursorPatchGeometry() {
  using host::capture::evenCursorPatch;
  require(!evenCursorPatch(-40, 5, 32, 32, 100, 60) && !evenCursorPatch(100, 5, 32, 32, 100, 60) && !evenCursorPatch(5, -32, 32, 32, 100, 60) && !evenCursorPatch(5, 60, 32, 32, 100, 60), "Invisible cursor produced a patch");
  require(!evenCursorPatch(5, 5, 0, 32, 100, 60) && !evenCursorPatch(5, 5, 32, 32, 101, 60), "Invalid patch input accepted");
  for (int x = -35; x < 105; ++x) for (int y = -35; y < 65; y += 3) for (const int size : {1, 2, 31, 32, 33}) {
    const auto patch = evenCursorPatch(x, y, size, size, 100, 60);
    const bool visible = x + size > 0 && y + size > 0 && x < 100 && y < 60; require(bool(patch) == visible, "Patch visibility differs from the cursor stamp rule");
    if (!patch) continue;
    require(host::capture::evenRect(*patch) && patch->left >= 0 && patch->top >= 0 && patch->right <= 100 && patch->bottom <= 60, "Patch is not an even in-frame rectangle");
    require(patch->left <= std::max(0, x) && patch->top <= std::max(0, y) && patch->right >= std::min(100, x + size) && patch->bottom >= std::min(60, y + size) && patch->width() <= size + 2 && patch->height() <= size + 2, "Patch misses cursor pixels or is oversized");
  }
}

// Owned cursors: a monochrome AND/XOR cursor (inverts) and a 32bpp alpha cursor.
HCURSOR monochromeCursor() {
  std::vector<uint8_t> andPlane(32 * 4, 0xff), xorPlane(32 * 4, 0);
  for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) { const size_t bit = size_t(y) * 32 + x; if ((x + y) % 5 == 0) { andPlane[bit / 8] &= uint8_t(~(0x80 >> (bit % 8))); } else if (x > 8 && x < 20 && y > 4) { xorPlane[bit / 8] |= uint8_t(0x80 >> (bit % 8)); } }
  return CreateCursor(GetModuleHandleW(nullptr), 3, 2, 32, 32, andPlane.data(), xorPlane.data());
}
HCURSOR alphaCursor(int size) {
  BITMAPV5HEADER header{}; header.bV5Size = sizeof(header); header.bV5Width = size; header.bV5Height = -size; header.bV5Planes = 1; header.bV5BitCount = 32; header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00ff0000; header.bV5GreenMask = 0x0000ff00; header.bV5BlueMask = 0x000000ff; header.bV5AlphaMask = 0xff000000;
  HDC screen = GetDC(nullptr); void* bits = nullptr; HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0); ReleaseDC(nullptr, screen);
  require(color && bits, "Alpha cursor bitmap");
  auto* p = static_cast<uint32_t*>(bits);
  for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) { const uint32_t alpha = uint32_t((x * 255) / std::max(1, size - 1)) & 0xff; const uint32_t red = (alpha * 200) / 255, green = (alpha * uint32_t(y * 255 / size)) / 255; p[size_t(y) * size + x] = (alpha << 24) | (red << 16) | (green << 8) | (alpha / 3); }
  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO info{}; info.fIcon = FALSE; info.xHotspot = 1; info.yHotspot = 1; info.hbmMask = mask; info.hbmColor = color;
  const HCURSOR cursor = CreateIconIndirect(&info); DeleteObject(mask); DeleteObject(color); return cursor;
}
// The pre-change compose: full-frame DIB built like Capture's (screen DC,
// HALFTONE, brush origin 0), base copied in, DrawIconEx, GdiFlush, convert.
class LegacyComposer {
 public:
  LegacyComposer(int width, int height) : width_(width), height_(height) {
    screen_ = GetDC(nullptr); memory_ = CreateCompatibleDC(screen_);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(screen_, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels_), nullptr, 0);
    require(screen_ && memory_ && bitmap_ && pixels_, "Legacy DIB"); previous_ = SelectObject(memory_, bitmap_); SetStretchBltMode(memory_, HALFTONE); SetBrushOrgEx(memory_, 0, 0, nullptr);
  }
  ~LegacyComposer() { SelectObject(memory_, previous_); DeleteObject(bitmap_); DeleteDC(memory_); ReleaseDC(nullptr, screen_); }
  HDC screen() const { return screen_; }
  const uint8_t* bgra() const { return pixels_; }
  std::vector<uint8_t> compose(const std::vector<std::pair<host::capture::PixelRect, const std::vector<uint8_t>*>>& surfaces, const std::vector<size_t>& pitches, const std::optional<host::capture::CursorDraw>& cursor) {
    GdiFlush();
    for (size_t i = 0; i < surfaces.size(); ++i) {
      const auto& [placed, pixels] = surfaces[i]; std::vector<uint8_t> packed(size_t(placed.width()) * placed.height() * 4);
      for (int y = 0; y < placed.height(); ++y) std::memcpy(packed.data() + size_t(y) * placed.width() * 4, pixels->data() + size_t(y) * pitches[i], size_t(placed.width()) * 4);
      host::capture::copyUnscaledSurface({pixels_, size_t(width_) * height_ * 4}, width_, height_, packed, placed.width(), placed.height(), placed.left, placed.top);
    }
    if (cursor) DrawIconEx(memory_, cursor->x, cursor->y, cursor->shape, cursor->width, cursor->height, 0, nullptr, DI_NORMAL);
    GdiFlush();
    std::vector<uint8_t> nv12(size_t(width_) * height_ * 3 / 2); host::bgraToNv12(pixels_, nv12.data(), width_, height_); return nv12;
  }
 private:
  int width_, height_; HDC screen_ = nullptr, memory_ = nullptr; HBITMAP bitmap_ = nullptr; HGDIOBJ previous_ = nullptr; uint8_t* pixels_ = nullptr;
};
size_t cursorSequences = 0;
void cursorPatchMatchesFullFrame(int width, int height, std::vector<host::capture::PixelRect> layout, uint32_t seed) {
  std::mt19937 random(seed);
  std::vector<size_t> pitches; std::vector<std::vector<uint8_t>> pixels;
  for (const auto& placed : layout) { pitches.push_back(size_t(placed.width()) * 4 + 128 * (random() % 2)); pixels.push_back(randomFrame(placed.width(), placed.height(), pitches.back(), random())); }
  std::vector<std::pair<host::capture::PixelRect, const std::vector<uint8_t>*>> surfaces; for (size_t i = 0; i < layout.size(); ++i) surfaces.push_back({layout[i], &pixels[i]});
  LegacyComposer legacy(width, height); host::capture::Nv12CursorPatch patch; require(patch.reserve(34, 34, legacy.screen()), "Cursor canvas");
  host::capture::CursorComposer composer;
  const auto visit = [&](const host::capture::PixelRect& rect, auto&& f) {
    for (size_t i = 0; i < layout.size(); ++i) if (const auto overlap = host::capture::intersect(rect, layout[i]))
      f(pixels[i].data() + size_t(overlap->top - layout[i].top) * pitches[i] + size_t(overlap->left - layout[i].left) * 4, pitches[i], *overlap);
  };
  const HCURSOR mono = monochromeCursor(), alpha32 = alphaCursor(32), alpha48 = alphaCursor(48);
  require(mono && alpha32 && alpha48, "Owned test cursors");
  const std::vector<HCURSOR> shapes{LoadCursorW(nullptr, IDC_ARROW), LoadCursorW(nullptr, IDC_IBEAM), LoadCursorW(nullptr, IDC_WAIT), LoadCursorW(nullptr, IDC_APPSTARTING), LoadCursorW(nullptr, IDC_HAND),
    LoadCursorW(nullptr, IDC_CROSS), LoadCursorW(nullptr, IDC_SIZEALL), LoadCursorW(nullptr, IDC_NO), LoadCursorW(nullptr, IDC_HELP), LoadCursorW(nullptr, IDC_SIZENWSE), mono, alpha32, alpha48};
  std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2, 0);
  for (size_t i = 0; i < layout.size(); ++i) host::capture::convertBlockToNv12(nv12, width, height, pixels[i].data(), pitches[i], layout[i]);
  std::optional<host::capture::CursorDraw> cursor;
  for (int step = 0; step < 260; ++step) {
    const auto action = random() % 12;
    if (action < 2) cursor.reset();
    else if (action < 9 || !cursor) {
      host::capture::CursorDraw next; next.shape = shapes[random() % shapes.size()];
      const int sizes[] = {32, 32, 32, 48, 24, 1, 64, 40};
      next.width = sizes[random() % 8]; next.height = random() % 4 ? next.width : sizes[random() % 8];
      // Edges, corners, surface seams and arbitrary positions; odd and even.
      const int mode = int(random() % 5);
      if (mode == 0) { next.x = int(random() % (width + 80)) - 40; next.y = int(random() % (height + 80)) - 40; }
      else if (mode == 1) { next.x = random() % 2 ? -int(random() % 40) : width - int(random() % 40); next.y = random() % 2 ? -int(random() % 40) : height - int(random() % 40); }
      else if (mode == 2 && layout.size() > 1) { const auto& seam = layout[1]; next.x = seam.left - int(random() % 40); next.y = seam.top - int(random() % 40); if (seam.top == 0) next.y = int(random() % height); else next.x = int(random() % width); }
      else { next.x = int(random() % width); next.y = int(random() % height); }
      if (cursor && random() % 4 == 0) next = *cursor; // unchanged cursor
      cursor = next;
    }
    // Sometimes a surface delivers a new frame (only part of it changes).
    std::vector<bool> updated(layout.size(), false);
    if (random() % 3 == 0) {
      const size_t i = random() % layout.size(); updated[i] = true; const auto& placed = layout[i];
      const int x0 = int(random() % placed.width()), y0 = int(random() % placed.height());
      for (int y = y0; y < std::min(placed.height(), y0 + 50); ++y) for (int x = x0; x < std::min(placed.width(), x0 + 90); ++x) pixels[i][size_t(y) * pitches[i] + size_t(x) * 4 + (x % 3)] ^= uint8_t(random());
      if (random() % 4 == 0) pixels[i] = randomFrame(placed.width(), placed.height(), pitches[i], random());
    }
    const auto expected = legacy.compose(surfaces, pitches, cursor);
    for (size_t i = 0; i < layout.size(); ++i) if (updated[i]) host::capture::convertBlockToNv12(nv12, width, height, pixels[i].data(), pitches[i], layout[i]);
    patch.apply(nv12, width, height, cursor, visit, legacy.screen());
    require(nv12 == expected, "Cursor patch NV12 differs from the full-frame DIB composition");
    // Lossless tiles read the composed BGRA as pieces: base surfaces outside the
    // patch, the cursor canvas inside. Disjoint, complete, and equal to the DIB.
    std::vector<host::capture::ComposedSurface> bases;
    for (size_t i = 0; i < layout.size(); ++i) bases.push_back({layout[i], pixels[i].data(), pitches[i]});
    std::vector<host::capture::PixelRect> rects;
    for (int y = 0; y < height; y += 64) for (int x = 0; x < width; x += 64) rects.push_back({x, y, std::min(width, x + 64), std::min(height, y + 64)});
    std::vector<uint8_t> assembled(size_t(width) * height * 4, 0), coverage(size_t(width) * height, 0);
    host::capture::visitComposedPieces(rects, bases, patch.patch(), patch.patchPixels(), patch.patchStride(), [&](const uint8_t* origin, size_t pitch, const host::capture::PixelRect& piece) {
      for (int y = 0; y < piece.height(); ++y) for (int x = 0; x < piece.width(); ++x) {
        const size_t at = size_t(piece.top + y) * width + piece.left + x; ++coverage[at];
        std::memcpy(&assembled[at * 4], origin + size_t(y) * pitch + size_t(x) * 4, 3);
      }
    });
    require(std::all_of(coverage.begin(), coverage.end(), [](uint8_t n) { return n == 1; }), "Composed pieces are not a disjoint cover");
    bool same = true; const uint8_t* dib = legacy.bgra();
    for (size_t at = 0; at < size_t(width) * height && same; ++at) same = std::memcmp(&assembled[at * 4], dib + at * 4, 3) == 0;
    require(same, "Composed pieces differ from the full-frame DIB composition");
    // The scaled-capture source view composes its cursor without NV12: same result.
    composer.compose(cursor, width, height, bases, legacy.screen());
    std::fill(assembled.begin(), assembled.end(), uint8_t(0));
    host::capture::visitComposedPieces(rects, bases, composer.patch(), composer.pixels(), composer.stride(), [&](const uint8_t* origin, size_t pitch, const host::capture::PixelRect& piece) {
      for (int y = 0; y < piece.height(); ++y) std::memcpy(&assembled[(size_t(piece.top + y) * width + piece.left) * 4], origin + size_t(y) * pitch, size_t(piece.width()) * 4);
    });
    for (size_t at = 0; at < size_t(width) * height && same; ++at) same = std::memcmp(&assembled[at * 4], dib + at * 4, 3) == 0;
    require(same, "Source cursor composition differs from the full-frame DIB composition");
  }
  DestroyCursor(mono); DestroyIcon(alpha32); DestroyIcon(alpha48); ++cursorSequences;
}
}
int main() {
  try {
    policyMatchesReference();
    // The AVX2 kernel (when this CPU has it) and the SSE2 kernel must give the same bytes.
    const bool avx2 = host::pixels::avx2Supported();
    for (const bool sse2Only : {false, true}) {
      host::pixels::forceSse2Kernel = sse2Only;
      kernelMatchesPrevious();
      blockConversionMatchesFullFrame();
      if (!avx2) break;
    }
    host::pixels::forceSse2Kernel = false;
    cursorPatchGeometry();
    cursorPatchMatchesFullFrame(640, 360, {{0, 0, 640, 360}}, 1);
    cursorPatchMatchesFullFrame(646, 362, {{0, 0, 646, 362}}, 2);
    cursorPatchMatchesFullFrame(640, 240, {{0, 0, 320, 240}, {320, 0, 640, 240}}, 3);
    cursorPatchMatchesFullFrame(320, 480, {{0, 0, 320, 240}, {0, 240, 320, 480}}, 4);
    cursorPatchMatchesFullFrame(1280, 720, {{0, 0, 1280, 720}}, 5);
    std::cout << "{\"kind\":\"capture-direct-path-differential\",\"checks\":" << checks << ",\"kernelComparedBytes\":" << kernelBytes << ",\"cursorSequences\":" << cursorSequences
              << ",\"avx2KernelTested\":" << (avx2 ? "true" : "false") << ",\"sse2KernelTested\":true"
              << ",\"policyMatchesPreviousImplementation\":true,\"blockConversionMatchesFullFrame\":true,\"cursorPatchMatchesFullFrameDib\":true,\"systemAndOwnedCursors\":true,\"desktopCaptured\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
