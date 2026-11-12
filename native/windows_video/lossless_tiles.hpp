#pragma once
#include "rgba_upscale.hpp"
#include "frame_layout.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// ToTBSeamless lossless tiles ("build to lossless"), wire format tiles-v1.
// A payload rides inside an ordinary NON-KEY video media packet (legacy
// media-v1/epoch or tracks-v1 envelope) and is sent only after the client opted
// in, so released peers never see one. H.264 Annex-B payloads always begin with
// a zero start-code byte; this payload begins with 'T'. Layout (little endian):
//   0  'T''S''L''1'
//   4  u16 width, u16 height          (exact decoder frame size)
//   8  u32 sequence                   (1, 2, 3 ... per host encoder)
//   12 u64 dropPts                    (drops apply to decoded video pts >= this)
//   20 u16 dropCount, u16 addCount
//   24 u16 drop tile indices
//      adds: u16 index, u8 codec (1 = TQ1), u8 0, u32 size, size bytes
// Tiles are 64x64, row-major, edge tiles partial. Header-only C++17: shared by
// the host (C++20) and the Flutter Windows runner. See docs/lossless-tiles-design.md.
namespace multitap::lossless {
constexpr uint32_t tileSize = 64;
constexpr size_t headerBytes = 24, addHeaderBytes = 8;
// Both media envelopes (12-byte legacy, 28-byte tracks) must stay <= 4 MiB.
constexpr size_t maximumPayloadBytes = 4 * 1024 * 1024 - 28;
// TQ1 worst case: one 4-byte RGB op per pixel.
constexpr size_t maximumTileBytes = size_t(tileSize) * tileSize * 4;
constexpr uint8_t codecTq1 = 1;
constexpr uint64_t noDrop = ~uint64_t(0);

// tiles-v1 has uint16 dimensions and uint16 tile indices. Preserve those real
// wire limits; resolution/memory admission is otherwise per actual device.
inline bool validFrame(uint32_t width, uint32_t height) {
  video::FrameLayout layout{};
  return width>=16&&height>=16&&width<=UINT16_MAX&&height<=UINT16_MAX&&video::frameLayout(width,height,layout)&&
    uint64_t((width+tileSize-1)/tileSize)*((height+tileSize-1)/tileSize)<=uint64_t(UINT16_MAX)+1;
}
inline uint32_t tileColumns(uint32_t width) { return (width + tileSize - 1) / tileSize; }
inline uint32_t tileRows(uint32_t height) { return (height + tileSize - 1) / tileSize; }
struct TileRect { uint32_t x = 0, y = 0, width = 0, height = 0; };
inline TileRect tileRect(uint32_t index, uint32_t width, uint32_t height) {
  const uint32_t columns = tileColumns(width), x = (index % columns) * tileSize, y = (index / columns) * tileSize;
  return {x, y, std::min(tileSize, width - x), std::min(tileSize, height - y)};
}
inline bool isTilePayload(const uint8_t* payload, size_t size) {
  return payload && size >= 4 && payload[0] == 'T' && payload[1] == 'S' && payload[2] == 'L' && payload[3] == '1';
}
inline uint16_t read16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t read32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint64_t read64(const uint8_t* p) { return uint64_t(read32(p)) | (uint64_t(read32(p + 4)) << 32); }
inline void write16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void write32(uint8_t* p, uint32_t v) { for (int n = 0; n < 4; ++n) p[n] = uint8_t(v >> (8 * n)); }
inline void write64(uint8_t* p, uint64_t v) { for (int n = 0; n < 8; ++n) p[n] = uint8_t(v >> (8 * n)); }

// TQ1: QOI-style byte ops (own implementation) plus a copy-from-above run, for
// RGB screen content. Alpha is not coded; decoded pixels have alpha 255 exactly
// like decoded video. Pixel value = r | g << 8 | b << 16.
namespace tq1 {
constexpr uint8_t opIndex = 0x00, opDiff = 0x40, opLuma = 0x80, opRun = 0xc0, opRgb = 0xfe, opAbove = 0xff;
inline uint32_t hash(uint32_t px) { return ((px & 255) * 3 + ((px >> 8) & 255) * 5 + ((px >> 16) & 255) * 7 + 255 * 11) & 63; }
}
// Appends the TQ1 coding of a width x height (each 1..64) BGRA block.
inline bool encodeTq1(const uint8_t* bgra, size_t pitch, uint32_t width, uint32_t height, std::vector<uint8_t>& out) {
  using namespace tq1;
  if (!bgra || !width || !height || width > tileSize || height > tileSize || pitch < size_t(width) * 4) return false;
  std::array<uint32_t, tileSize * tileSize> px;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row = bgra + size_t(y) * pitch;
    for (uint32_t x = 0; x < width; ++x) px[y * width + x] = uint32_t(row[x * 4 + 2]) | (uint32_t(row[x * 4 + 1]) << 8) | (uint32_t(row[x * 4]) << 16);
  }
  std::array<uint32_t, 64> index{};
  const uint32_t n = width * height;
  uint32_t prev = 0;
  for (uint32_t p = 0; p < n;) {
    uint32_t run = 0, above = 0;
    while (p + run < n && run < 62 && px[p + run] == prev) ++run;
    if (p >= width) while (p + above < n && above < 256 && px[p + above] == px[p + above - width]) ++above;
    if (above >= 2 && above > run) { out.push_back(opAbove); out.push_back(uint8_t(above - 1)); p += above; prev = px[p - 1]; }
    else if (run) { out.push_back(uint8_t(opRun | (run - 1))); p += run; }
    else {
      const uint32_t current = px[p], h = hash(current);
      if (index[h] == current) out.push_back(uint8_t(opIndex | h));
      else {
        const int dr = int8_t(uint8_t((current & 255) - (prev & 255))), dg = int8_t(uint8_t(((current >> 8) & 255) - ((prev >> 8) & 255))),
                  db = int8_t(uint8_t(((current >> 16) & 255) - ((prev >> 16) & 255)));
        if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) out.push_back(uint8_t(opDiff | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
        else if (dg >= -32 && dg <= 31 && dr - dg >= -8 && dr - dg <= 7 && db - dg >= -8 && db - dg <= 7) {
          out.push_back(uint8_t(opLuma | (dg + 32))); out.push_back(uint8_t(((dr - dg + 8) << 4) | (db - dg + 8)));
        } else { out.push_back(opRgb); out.push_back(uint8_t(current)); out.push_back(uint8_t(current >> 8)); out.push_back(uint8_t(current >> 16)); }
      }
      prev = current; ++p;
    }
    index[hash(prev)] = prev;
  }
  return true;
}
// Decodes into RGBA (alpha 255). Never writes outside width x height; false on
// any malformed, truncated or over-long input.
inline bool decodeTq1(const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint8_t* rgba, size_t pitch) {
  using namespace tq1;
  if (!data || !rgba || !width || !height || width > tileSize || height > tileSize || pitch < size_t(width) * 4) return false;
  std::array<uint32_t, 64> index{};
  const uint32_t n = width * height;
  uint32_t prev = 0, p = 0, x = 0, y = 0;
  size_t at = 0;
  const auto put = [&](uint32_t value) {
    uint8_t* target = rgba + size_t(y) * pitch + size_t(x) * 4;
    target[0] = uint8_t(value); target[1] = uint8_t(value >> 8); target[2] = uint8_t(value >> 16); target[3] = 255;
    if (++x == width) { x = 0; ++y; }
    ++p;
  };
  while (p < n) {
    if (at >= size) return false;
    const uint8_t op = data[at++];
    if (op == opRgb) {
      if (size - at < 3) return false;
      prev = uint32_t(data[at]) | (uint32_t(data[at + 1]) << 8) | (uint32_t(data[at + 2]) << 16); at += 3; put(prev);
    } else if (op == opAbove) {
      if (at >= size || y == 0) return false;
      const uint32_t count = uint32_t(data[at++]) + 1;
      if (count > n - p) return false;
      for (uint32_t k = 0; k < count; ++k) {
        const uint8_t* source = rgba + size_t(y - 1) * pitch + size_t(x) * 4;
        prev = uint32_t(source[0]) | (uint32_t(source[1]) << 8) | (uint32_t(source[2]) << 16); put(prev);
      }
    } else if ((op & 0xc0) == opIndex) { prev = index[op & 63]; put(prev); }
    else if ((op & 0xc0) == opDiff) {
      const uint32_t r = ((prev & 255) + ((op >> 4) & 3) - 2) & 255, g = (((prev >> 8) & 255) + ((op >> 2) & 3) - 2) & 255, b = (((prev >> 16) & 255) + (op & 3) - 2) & 255;
      prev = r | (g << 8) | (b << 16); put(prev);
    } else if ((op & 0xc0) == opLuma) {
      if (at >= size) return false;
      const uint8_t second = data[at++];
      const int dg = int(op & 63) - 32, drdg = int(second >> 4) - 8, dbdg = int(second & 15) - 8;
      const uint32_t r = uint32_t(int(prev & 255) + dg + drdg) & 255, g = uint32_t(int((prev >> 8) & 255) + dg) & 255, b = uint32_t(int((prev >> 16) & 255) + dg + dbdg) & 255;
      prev = r | (g << 8) | (b << 16); put(prev);
    } else {
      const uint32_t count = uint32_t(op & 63) + 1;
      if (count > n - p) return false;
      for (uint32_t k = 0; k < count; ++k) put(prev);
    }
    index[hash(prev)] = prev;
  }
  return at == size;
}

// Host side: one tiles-v1 payload, drops first, then encoded adds.
class PayloadBuilder {
 public:
  PayloadBuilder(uint32_t width, uint32_t height, uint32_t sequence, uint64_t dropPts, const std::vector<uint16_t>& drops) : width_(width), height_(height) {
    bytes_.resize(headerBytes + drops.size() * 2);
    bytes_[0] = 'T'; bytes_[1] = 'S'; bytes_[2] = 'L'; bytes_[3] = '1';
    write16(bytes_.data() + 4, uint16_t(width)); write16(bytes_.data() + 6, uint16_t(height));
    write32(bytes_.data() + 8, sequence); write64(bytes_.data() + 12, dropPts);
    write16(bytes_.data() + 20, uint16_t(drops.size()));
    for (size_t n = 0; n < drops.size(); ++n) write16(bytes_.data() + headerBytes + n * 2, drops[n]);
  }
  // Encodes tile `index` from its BGRA origin. Returns the bytes added.
  size_t add(uint16_t index, const uint8_t* bgra, size_t pitch) {
    const auto rect = tileRect(index, width_, height_);
    const size_t start = bytes_.size();
    bytes_.resize(start + addHeaderBytes);
    write16(bytes_.data() + start, index); bytes_[start + 2] = codecTq1; bytes_[start + 3] = 0;
    encodeTq1(bgra, pitch, rect.width, rect.height, bytes_);
    write32(bytes_.data() + start + 4, uint32_t(bytes_.size() - start - addHeaderBytes));
    ++adds_;
    return bytes_.size() - start;
  }
  size_t size() const { return bytes_.size(); }
  uint16_t adds() const { return adds_; }
  std::vector<uint8_t> finish() { write16(bytes_.data() + 22, adds_); return std::move(bytes_); }
 private:
  uint32_t width_, height_;
  uint16_t adds_ = 0;
  std::vector<uint8_t> bytes_;
};

// Client side, one per decoder (legacy engine or one track). Not thread-safe:
// the decoder worker thread owns it. A new epoch/topology/resize creates a new
// engine and therefore a fresh compositor.
//   * a tile packet records its drops (due when a decoded frame with pts >=
//     dropPts is composed) and decodes its adds into the lossless layer;
//   * every decoded frame first applies due drops, then gets every present tile,
//     so a tile is never composited on the frame carrying its change or later;
//   * a malformed packet, size mismatch or sequence gap poisons the compositor:
//     no tile is composited again (plain video), never a stale tile.
class Compositor {
 public:
  enum class Outcome { notTile, accepted, ignored };
  struct Stats { uint64_t packets = 0, addedTiles = 0, droppedTiles = 0, bytes = 0; uint32_t presentTiles = 0; bool poisoned = false; };
  // `width` x `height` is the decoded video. With `allowScaledSource` (legacy
  // single output only) the first tile packet may declare a larger unscaled
  // source; tiles then live in source coordinates and every composition
  // produces a source-size frame, video upscaled where no tile is present.
  bool Configure(uint32_t width, uint32_t height, bool allowScaledSource = false) {
    *this = Compositor{};
    if (!validFrame(width, height)) return false;
    video_width_ = width; video_height_ = height; allow_scaled_ = allowScaledSource;
    if (!allowScaledSource) establish(width, height);
    return true;
  }
  Outcome Accept(const uint8_t* payload, size_t size) {
    if (!isTilePayload(payload, size)) return Outcome::notTile;
    if (poisoned_ || (!count_ && !allow_scaled_) || !video_width_) return Outcome::ignored;
    if (!parse(payload, size)) { poison(); return Outcome::ignored; }
    return Outcome::accepted;
  }
  // Adds received since the last composition wait to be republished.
  bool republishPending() const { return !poisoned_ && !added_.empty(); }
  // True when a decoded frame needs composition (tiles present or a poison to apply).
  bool active() const { return present_count_ > 0 || !added_.empty(); }
  // Tiles describe a source larger than the video: use the *Scaled/Upscaled calls.
  bool scaled() const { return count_ && (width_ != video_width_ || height_ != video_height_); }
  // A freshly decoded, exclusively owned RGBA frame of the configured size.
  void ComposeDecoded(uint8_t* rgba, size_t pitch, uint64_t pts) {
    added_.clear();
    if (poisoned_ || !present_count_) { if (poisoned_) clearPresent(); return; }
    for (uint32_t i = 0; i < count_; ++i) {
      if (!present_[i] || due(i, pts)) continue;
      copyTile(i, rgba, pitch);
    }
  }
  // Scaled source: the published source-size frame for a decoded video frame:
  // due drops applied, present tiles exact, everything else upscaled video.
  void ComposeDecodedScaled(const uint8_t* video, size_t videoPitch, uint8_t* frame, size_t framePitch, uint64_t pts) {
    if (poisoned_) clearPresent();
    else for (uint32_t i = 0; i < count_; ++i) if (present_[i]) due(i, pts);
    ComposeUpscaled(video, videoPitch, frame, framePitch);
  }
  // Scaled source, no drops: present tiles over the upscaled `video` (also the
  // republish of a shown frame that is still video-size).
  void ComposeUpscaled(const uint8_t* video, size_t videoPitch, uint8_t* frame, size_t framePitch) {
    added_.clear();
    const uint32_t columns = tileColumns(width_), rows = tileRows(height_);
    for (uint32_t r = 0; r < rows; ++r) {
      const uint32_t y0 = r * tileSize, y1 = std::min(height_, y0 + tileSize);
      for (uint32_t c = 0; c < columns;) {
        const uint32_t index = r * columns + c;
        if (!poisoned_ && present_[index]) { copyTile(index, frame, framePitch); ++c; continue; }
        uint32_t end = c + 1;
        while (end < columns && (poisoned_ || !present_[r * columns + end])) ++end;
        upscaler_.Resample(video, videoPitch, frame, framePitch, c * tileSize, y0, std::min(width_, end * tileSize), y1);
        c = end;
      }
    }
  }
  // Writes tiles added since the last composition into a COPY of the latest
  // published frame (which already shows every earlier present tile).
  void ComposeAdded(uint8_t* rgba, size_t pitch) {
    if (!poisoned_) for (const auto i : added_) if (present_[i]) copyTile(i, rgba, pitch);
    added_.clear();
  }
  // GPU composition (the client's GPU presentation keeps the tiles in a texture
  // instead of copying them into an RGBA frame). Same state changes as
  // ComposeDecoded without the pixel copy: due drops are applied and the added
  // list is consumed; present(i) then says which tiles go over the picture.
  void ApplyDecoded(uint64_t pts) {
    added_.clear();
    if (poisoned_ || !present_count_) { if (poisoned_) clearPresent(); return; }
    for (uint32_t i = 0; i < count_; ++i) if (present_[i]) due(i, pts);
  }
  // Scaled source: same, for a source-size frame (ComposeDecodedScaled minus pixels).
  void ApplyDecodedScaled(uint64_t pts) {
    added_.clear();
    if (poisoned_) clearPresent();
    else for (uint32_t i = 0; i < count_; ++i) if (present_[i]) due(i, pts);
  }
  // Tiles added by packets since the last composition, and their pixels in the
  // compositor's layer (the channel order of the frames, `width() * 4` bytes per row).
  const std::vector<uint32_t>& addedTiles() const { return added_; }
  void clearAdded() { added_.clear(); }
  const uint8_t* tilePixels(uint32_t index) const {
    const auto rect = tileRect(index, width_, height_);
    return layer_.data() + size_t(rect.y) * width_ * 4 + size_t(rect.x) * 4;
  }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t videoWidth() const { return video_width_; }
  uint32_t videoHeight() const { return video_height_; }
  uint32_t presentTiles() const { return present_count_; }
  bool present(uint32_t index) const { return index < count_ && !present_.empty() && present_[index]; }
  bool poisoned() const { return poisoned_; }
  Stats stats() const { auto value = stats_; value.presentTiles = present_count_; value.poisoned = poisoned_; return value; }
 private:
  void establish(uint32_t width, uint32_t height) { width_ = width; height_ = height; count_ = tileColumns(width) * tileRows(height); }
  // Applies a due drop of a present tile; true when it was dropped.
  bool due(uint32_t i, uint64_t pts) {
    if (drop_at_[i] == noDrop || pts < drop_at_[i]) return false;
    present_[i] = 0; drop_at_[i] = noDrop; --present_count_; ++stats_.droppedTiles; return true;
  }
  bool parse(const uint8_t* p, size_t size) {
    if (size < headerBytes || size > maximumPayloadBytes) return false;
    if (!count_) {
      // First packet of a scaled-capable compositor: the unscaled source size,
      // never smaller than the video. Wire/stride/memory checks replace an
      // arbitrary magnification ratio; a low encoded size is not a small desktop.
      const uint32_t w = read16(p + 4), h = read16(p + 6);
      if (!validFrame(w, h) || w < video_width_ || h < video_height_) return false;
      establish(w, h);
      if (scaled() && !upscaler_.Configure(video_width_, video_height_, w, h)) return false;
    }
    if (read16(p + 4) != width_ || read16(p + 6) != height_) return false;
    const uint32_t sequence = read32(p + 8);
    if (sequence != sequence_ + 1) return false;
    const uint64_t dropPts = read64(p + 12);
    const uint32_t drops = read16(p + 20), adds = read16(p + 22);
    if (size < headerBytes + size_t(drops) * 2) return false;
    // Allocated on the first tile packet: sessions with released hosts pay nothing.
    if (layer_.empty()) { layer_.assign(size_t(width_) * height_ * 4, 0); present_.assign(count_, 0); drop_at_.assign(count_, noDrop); }
    for (uint32_t n = 0; n < drops; ++n) {
      const uint32_t index = read16(p + headerBytes + n * 2);
      if (index >= count_) return false;
      if (present_[index]) drop_at_[index] = std::min(drop_at_[index], dropPts);
    }
    size_t at = headerBytes + size_t(drops) * 2;
    for (uint32_t n = 0; n < adds; ++n) {
      if (size - at < addHeaderBytes) return false;
      const uint32_t index = read16(p + at), length = read32(p + at + 4);
      if (index >= count_ || p[at + 2] != codecTq1 || p[at + 3] || length > size - at - addHeaderBytes) return false;
      const auto rect = tileRect(index, width_, height_);
      if (present_[index]) { present_[index] = 0; --present_count_; }
      if (!decodeTq1(p + at + addHeaderBytes, length, rect.width, rect.height, layer_.data() + (size_t(rect.y) * width_ + rect.x) * 4, size_t(width_) * 4)) return false;
      // Newer content than any pending drop for this tile: it replaces it.
      present_[index] = 1; drop_at_[index] = noDrop; ++present_count_; added_.push_back(index); ++stats_.addedTiles;
      at += addHeaderBytes + length;
    }
    if (at != size) return false;
    sequence_ = sequence; ++stats_.packets; stats_.bytes += size;
    return true;
  }
  void copyTile(uint32_t index, uint8_t* rgba, size_t pitch) const {
    const auto rect = tileRect(index, width_, height_);
    const size_t stride = size_t(width_) * 4, bytes = size_t(rect.width) * 4;
    const uint8_t* source = layer_.data() + size_t(rect.y) * stride + size_t(rect.x) * 4;
    uint8_t* target = rgba + size_t(rect.y) * pitch + size_t(rect.x) * 4;
    for (uint32_t row = 0; row < rect.height; ++row) std::memcpy(target + row * pitch, source + row * stride, bytes);
  }
  void clearPresent() { std::fill(present_.begin(), present_.end(), uint8_t(0)); present_count_ = 0; }
  void poison() { poisoned_ = true; added_.clear(); }
  uint32_t width_ = 0, height_ = 0, count_ = 0, sequence_ = 0, present_count_ = 0, video_width_ = 0, video_height_ = 0;
  bool poisoned_ = false, allow_scaled_ = false;
  std::vector<uint8_t> layer_, present_;
  std::vector<uint64_t> drop_at_;
  std::vector<uint32_t> added_;
  video::BilinearUpscaler upscaler_;
  Stats stats_;
};
}
