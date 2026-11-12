// Pure checks of ToTBSeamless lossless tiles: TQ1 bit-exact round trip and
// malformed-input rejection, host tile diffing/budgets, and the client
// compositor's invalidation rules. Synthetic pixels only; no capture, encoder,
// relay or real session is involved.
#include "lossless_tile_source.hpp"
#include "capture_scale.hpp"
#include "frame_encode_policy.hpp"
#include "media.hpp"
#include <array>
#include <iostream>
#include <map>
#include <random>
#include <tuple>

namespace {
using namespace multitap;
uint64_t checks = 0;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); ++checks; }

// BGRA -> RGBA(alpha 255) expectation of a decoded tile.
bool sameRgb(const uint8_t* bgra, size_t bgraPitch, const uint8_t* rgba, size_t rgbaPitch, uint32_t w, uint32_t h) {
  for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
    const uint8_t* s = bgra + y * bgraPitch + x * 4; const uint8_t* d = rgba + y * rgbaPitch + x * 4;
    if (d[0] != s[2] || d[1] != s[1] || d[2] != s[0] || d[3] != 255) return false;
  }
  return true;
}
void codecRoundTrip() {
  std::mt19937 random(11);
  std::vector<uint8_t> bgra(64 * 64 * 4), rgba(64 * 64 * 4);
  const auto check = [&](uint32_t w, uint32_t h, const char* reason) {
    std::vector<uint8_t> coded; require(lossless::encodeTq1(bgra.data(), 64 * 4, w, h, coded), "Encoder refused a valid tile");
    require(coded.size() <= lossless::maximumTileBytes, "TQ1 exceeded its worst-case bound");
    std::fill(rgba.begin(), rgba.end(), uint8_t(0x5a));
    require(lossless::decodeTq1(coded.data(), coded.size(), w, h, rgba.data(), 64 * 4), "Decoder rejected encoder output");
    require(sameRgb(bgra.data(), 64 * 4, rgba.data(), 64 * 4, w, h), reason);
    // Bytes outside the tile are untouched.
    for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x) if (x >= w || y >= h) for (int c = 0; c < 4; ++c) require(rgba[(y * 64 + x) * 4 + c] == 0x5a, "Decoder wrote outside the tile");
    return coded;
  };
  for (int pattern = 0; pattern < 9; ++pattern) for (uint32_t w : {1u, 2u, 3u, 17u, 63u, 64u}) for (uint32_t h : {1u, 2u, 31u, 64u}) {
    for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x) {
      uint8_t* p = bgra.data() + (y * 64 + x) * 4;
      switch (pattern) {
        case 0: p[0] = uint8_t(random()); p[1] = uint8_t(random()); p[2] = uint8_t(random()); break;          // noise (RGB ops)
        case 1: p[0] = p[1] = p[2] = 255; break;                                                                 // flat white (runs)
        case 2: p[0] = uint8_t(x * 4); p[1] = uint8_t(y * 4); p[2] = uint8_t(x + y); break;                       // gradients (diff/luma)
        case 3: p[0] = uint8_t(x % 7 * 30); p[1] = 200; p[2] = uint8_t(y % 3 * 90); break;                        // vertical structure (above)
        case 4: { const uint8_t v = uint8_t(random() % 4 * 60); p[0] = v; p[1] = uint8_t(v / 2); p[2] = 255; break; } // small palette (index)
        case 5: p[0] = uint8_t(255 - x); p[1] = uint8_t(255 - x); p[2] = uint8_t(255 - (x & 1)); break;            // ClearType-like fringes
        case 6: p[0] = uint8_t((x + y) & 1 ? 0 : 255); p[1] = p[0]; p[2] = uint8_t(255 - p[0]); break;             // checkerboard
        case 7: p[0] = uint8_t(random() % 3 + 100); p[1] = uint8_t(random() % 3 + 100); p[2] = uint8_t(random() % 3 + 100); break; // near-flat noise
        default: p[0] = uint8_t(y < 32 ? 10 : 250); p[1] = uint8_t(x < 32 ? 10 : 250); p[2] = uint8_t(x == y ? 0 : 128); break;
      }
      p[3] = uint8_t(random());  // alpha is never coded
    }
    check(w, h, "TQ1 round trip was not bit-exact");
  }
  // Rejections never write outside and never accept garbage as a tile.
  for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x) { uint8_t* p = bgra.data() + (y * 64 + x) * 4; p[0] = uint8_t(x * 3); p[1] = uint8_t(y); p[2] = uint8_t(x ^ y); }
  std::vector<uint8_t> coded; lossless::encodeTq1(bgra.data(), 256, 64, 64, coded);
  for (size_t cut : {size_t(0), size_t(1), coded.size() / 2, coded.size() - 1}) require(!lossless::decodeTq1(coded.data(), cut, 64, 64, rgba.data(), 256), "Truncated TQ1 accepted");
  auto longer = coded; longer.push_back(0); require(!lossless::decodeTq1(longer.data(), longer.size(), 64, 64, rgba.data(), 256), "Over-long TQ1 accepted");
  const uint8_t aboveFirstRow[] = {0xff, 3}; require(!lossless::decodeTq1(aboveFirstRow, 2, 8, 8, rgba.data(), 256), "Copy-from-above on the first row accepted");
  const uint8_t runOverflow[] = {0xc0 | 61, 0xc0 | 61}; require(!lossless::decodeTq1(runOverflow, 2, 8, 8, rgba.data(), 256), "Run beyond the tile accepted");
  require(!lossless::decodeTq1(coded.data(), coded.size(), 65, 64, rgba.data(), 65 * 4), "Oversized tile accepted");
  std::mt19937 fuzz(5);
  for (int n = 0; n < 4000; ++n) {
    std::vector<uint8_t> junk(1 + fuzz() % 200); for (auto& b : junk) b = uint8_t(fuzz());
    std::vector<uint8_t> guard(64 * 64 * 4 + 64, 0x33);
    (void)lossless::decodeTq1(junk.data(), junk.size(), 1 + fuzz() % 64, 1 + fuzz() % 64, guard.data(), 64 * 4);
    for (size_t i = 64 * 64 * 4; i < guard.size(); ++i) require(guard[i] == 0x33, "Fuzzed TQ1 wrote past its destination");
  }
}

// A BGRA frame with deterministic content per "scene" value.
struct Canvas {
  int width, height; std::vector<uint8_t> bgra;
  Canvas(int w, int h) : width(w), height(h), bgra(size_t(w) * h * 4, 255) {}
  void fill(int x0, int y0, int x1, int y1, uint8_t b, uint8_t g, uint8_t r) {
    for (int y = std::max(0, y0); y < std::min(height, y1); ++y) for (int x = std::max(0, x0); x < std::min(width, x1); ++x) { auto* p = &bgra[(size_t(y) * width + x) * 4]; p[0] = b; p[1] = g; p[2] = r; p[3] = 255; }
  }
  void texture(int seed) { for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) { auto* p = &bgra[(size_t(y) * width + x) * 4]; p[0] = uint8_t((x * 7 + seed) ^ y); p[1] = uint8_t(x + y * 3 + seed * 5); p[2] = uint8_t((x ^ (y * 5)) + seed); p[3] = 255; } }
};

// Simulated client: a "video" frame is the source with a visible distortion,
// so tile pixels (exact) and video pixels are distinguishable.
struct Client {
  lossless::Compositor compositor; std::vector<uint8_t> published; bool havePublished = false; uint64_t republishes = 0;
  Client(int w, int h) { require(compositor.Configure(uint32_t(w), uint32_t(h)), "Compositor configuration"); published.resize(size_t(w) * h * 4); }
  static std::vector<uint8_t> video(const Canvas& source) {
    std::vector<uint8_t> rgba(source.bgra.size());
    for (size_t i = 0; i < source.bgra.size(); i += 4) { rgba[i] = uint8_t(source.bgra[i + 2] ^ 1); rgba[i + 1] = source.bgra[i + 1]; rgba[i + 2] = source.bgra[i]; rgba[i + 3] = 255; }
    return rgba;
  }
  void tile(const std::vector<uint8_t>& payload) {
    require(offer(payload) == lossless::Compositor::Outcome::accepted, "Valid tile packet rejected");
  }
  // Like the engine: whatever the compositor makes of it (a gap poisons).
  lossless::Compositor::Outcome offer(const std::vector<uint8_t>& payload) {
    const auto outcome = compositor.Accept(payload.data(), payload.size());
    if (compositor.republishPending() && havePublished) { compositor.ComposeAdded(published.data(), size_t(compositor.width()) * 4); ++republishes; }
    return outcome;
  }
  void decoded(std::vector<uint8_t> rgba, uint64_t pts) {
    if (compositor.active()) compositor.ComposeDecoded(rgba.data(), size_t(compositor.width()) * 4, pts);
    published = std::move(rgba); havePublished = true;
  }
};
// Every published pixel is exact content of `source` (or of `alternative`) or
// the video pixel of `video`: never content older than the shown video frame.
void requireNoStale(const Client& client, const Canvas& source, const Canvas& video, const Canvas* alternative, const char* reason) {
  for (size_t i = 0; i < source.bgra.size(); i += 4) {
    const uint8_t* d = &client.published[i];
    const auto same = [&](const Canvas& c, uint8_t flip) { const uint8_t* s = &c.bgra[i]; return d[0] == uint8_t(s[2] ^ flip) && d[1] == s[1] && d[2] == s[0]; };
    if (!same(source, 0) && !same(video, 1) && !(alternative && same(*alternative, 0))) throw std::runtime_error(reason);
  }
  ++checks;
}
bool exact(const Client& client, const Canvas& source) {
  for (size_t i = 0; i < source.bgra.size(); i += 4) if (client.published[i] != source.bgra[i + 2] || client.published[i + 1] != source.bgra[i + 1] || client.published[i + 2] != source.bgra[i]) return false;
  return true;
}

// Host source + policy + simulated client over a scripted scene.
struct Loop {
  Canvas canvas; host::LosslessTileSource source; host::FrameEncodePolicy policy; Client client; host::ParallelWork helper;
  uint64_t now = 0, videos = 0, packets = 0; size_t tileBytes = 0, maxPacket = 0;
  Loop(int w, int h, host::LosslessSettings settings) : canvas(w, h), source(uint32_t(w), uint32_t(h), settings), policy(size_t(w) * h * 3 / 2), client(w, h) {}
  std::vector<uint8_t> nv12() const { std::vector<uint8_t> value(size_t(canvas.width) * canvas.height * 3 / 2); host::bgraToNv12(canvas.bgra.data(), value.data(), canvas.width, canvas.height); return value; }
  void tick(std::optional<std::vector<host::capture::PixelRect>> dirty = std::nullopt) {
    now += 16667;
    host::BufferComposedFrame frame(canvas.bgra.data(), canvas.width, canvas.height, dirty);
    const auto frameBytes = nv12(); bool encoded = false;
    const auto result = host::losslessTick(source, helper, frame, policy, frameBytes, now, [&] { encoded = true; });
    const auto& decision = result.decision;
    require(decision.submit == encoded, "Encode call does not match the decision");
    require(!result.payload || !lossless::read16(result.payload->data() + 20) || decision.submit, "A drop was not accompanied by a video frame");
    if (const auto& payload = result.payload) {
      require(payload->size() <= 256 * 1024, "Tile packet exceeded its bound"); ++packets; tileBytes += payload->size(); maxPacket = std::max(maxPacket, payload->size());
      client.tile(*payload);
      // Before the tick's video: new tiles carry current content, pending drops
      // still show the content of the shown video frame.
      if (client.havePublished) requireNoStale(client, canvas, shown, &shown, "Republished composite shows content older than the shown video");
      if (client.havePublished) {  // every tile added by this packet is exactly the current frame
        const uint8_t* p = payload->data(); size_t at = lossless::headerBytes + size_t(lossless::read16(p + 20)) * 2;
        for (uint32_t n = 0; n < lossless::read16(p + 22); ++n) {
          const uint32_t index = lossless::read16(p + at); const auto r = lossless::tileRect(index, uint32_t(canvas.width), uint32_t(canvas.height));
          require(sameRgb(&canvas.bgra[(size_t(r.y) * canvas.width + r.x) * 4], size_t(canvas.width) * 4, &client.published[(size_t(r.y) * canvas.width + r.x) * 4], size_t(canvas.width) * 4, r.width, r.height), "An added tile did not carry the current pixels");
          at += lossless::addHeaderBytes + lossless::read32(p + at + 4);
        }
      }
    }
    if (decision.submit) { require(policy.output(now, !policy.hasUsableIdr(), now, 100), "Synthetic output"); ++videos; client.decoded(Client::video(canvas), now); shown = canvas; }
    if (client.havePublished && decision.submit) requireNoStale(client, canvas, canvas, nullptr, "A stale lossless tile overlaid newer video");
  }
  void until(int maximum) { for (int n = 0; n < maximum && !exact(client, canvas); ++n) tick(); }
  Canvas shown{1, 1};
  // Agent::stream's tick (losslessStreamTick): tiles while `wanted`, else
  // retired with a forced video frame and then plain video; `force` is a
  // requested IDR; `deliver` false loses this tick's tile packet in transit.
  bool retired = false; std::vector<uint8_t> lastPayload;
  void streamTick(bool wanted, bool force = false, bool deliver = true) {
    now += 16667;
    host::BufferComposedFrame frame(canvas.bgra.data(), canvas.width, canvas.height);
    const auto frameBytes = nv12(); bool encoded = false;
    const auto result = host::losslessStreamTick(source, retired, wanted, helper, frame, policy, frameBytes, now, [&] { encoded = true; }, force);
    host::FrameEncodeDecision decision;
    if (result) {
      decision = result->decision;
      if (result->payload) { ++packets; lastPayload = *result->payload; if (deliver) client.offer(*result->payload); }
    } else { decision = policy.prepare(frameBytes, now, force); encoded = decision.submit; }
    require(decision.submit == encoded, "Encode call does not match the decision");
    if (decision.submit) { require(policy.output(now, !policy.hasUsableIdr(), now, 100), "Synthetic output"); ++videos; client.decoded(Client::video(canvas), now); shown = canvas; }
    if (client.havePublished && decision.submit) requireNoStale(client, canvas, canvas, nullptr, "A stale lossless tile overlaid newer video");
  }
  void settle(int maximum) { for (int n = 0; n < maximum && !exact(client, canvas); ++n) streamTick(true); }
  // The capture's composed view shows `view` while the encoder gets the picture
  // of `canvas`: a view that no longer follows the capture.
  void divergentTick(const Canvas& view, std::optional<std::vector<host::capture::PixelRect>> dirty = std::nullopt) {
    now += 16667;
    host::BufferComposedFrame frame(view.bgra.data(), view.width, view.height, dirty);
    const auto frameBytes = nv12(); bool encoded = false;
    const auto result = host::losslessTick(source, helper, frame, policy, frameBytes, now, [&] { encoded = true; });
    require(result.decision.submit == encoded, "Encode call does not match the decision");
    if (result.payload) {
      ++packets; client.tile(*result.payload);
      if (client.havePublished) requireNoStale(client, canvas, shown, &shown, "A tile that is not the encoder's picture was republished");
    }
    if (result.decision.submit) { require(policy.output(now, !policy.hasUsableIdr(), now, 100), "Synthetic output"); ++videos; client.decoded(Client::video(canvas), now); shown = canvas; }
    if (client.havePublished && result.decision.submit) requireNoStale(client, canvas, canvas, nullptr, "A tile that is not the encoder's picture overlaid its video");
  }
};

host::LosslessSettings deterministic() { auto value = host::LosslessSettings::forBitrate(12000000); value.tickBudgetMs = 0; return value; }

void tileSourceDiffing() {
  // Settle, exact equality, then single-byte and NV12-invisible changes.
  Loop loop(640, 360, deterministic()); loop.canvas.texture(1);
  loop.until(240);
  require(loop.source.presentTiles() == loop.source.tileCount() && exact(loop.client, loop.canvas), "Static frame did not become lossless and exact");
  for (int n = 0; n < 10; ++n) loop.tick();
  const auto videosBefore = loop.videos;
  // One blue-channel step on a white pixel is invisible in BT.601 NV12, yet a BGRA change.
  loop.canvas.fill(0, 64, 64, 128, 255, 255, 255); loop.until(240); for (int n = 0; n < 70; ++n) loop.tick();
  const auto settled = loop.videos;
  {
    const auto before = loop.nv12();
    loop.canvas.bgra[(size_t(100) * 640 + 10) * 4] = 254;
    require(before == loop.nv12(), "Fixture change was visible in NV12");
  }
  loop.tick();
  require(!loop.source.present((100 / 64) * lossless::tileColumns(640)), "A changed lossless tile was not dropped");
  require(loop.videos == settled + 1 && settled >= videosBefore, "The drop did not force exactly one video frame");
  loop.until(120);
  require(exact(loop.client, loop.canvas), "Changed tile did not return to exact lossless");
  // Dirty hint: an empty hint means no comparison; a real change is still caught where hinted.
  for (int n = 0; n < 5; ++n) loop.tick();
  const auto packets = loop.packets;
  for (int n = 0; n < 20; ++n) loop.tick(std::vector<host::capture::PixelRect>{});
  require(loop.packets == packets, "Static ticks with an empty dirty hint produced tile packets");
  loop.canvas.fill(10, 10, 20, 20, 0, 0, 255);
  loop.tick(std::vector<host::capture::PixelRect>{{10, 10, 20, 20}});
  require(!loop.source.present(0), "Hinted change was not dropped");
  for (int n = 0; n < 60 && !exact(loop.client, loop.canvas); ++n) loop.tick(std::vector<host::capture::PixelRect>{});
  require(exact(loop.client, loop.canvas), "Hinted change did not settle to exact");
}

// SSE2 bilinear upscaling equals its scalar definition byte for byte and only
// writes the requested rectangle.
void upscalerMatchesReference() {
  std::mt19937 random(3);
  const std::array<uint32_t, 4> cases[] = {{320, 180, 480, 270}, {64, 36, 97, 55}, {2, 2, 5, 7}, {100, 50, 100, 50}, {33, 17, 99, 51}, {640, 360, 1280, 720}, {1920, 724, 2948, 1112}};
  for (const auto& [sw, sh, tw, th] : cases) {
    std::vector<uint8_t> source(size_t(sw) * sh * 4); for (size_t i = 0; i < source.size(); ++i) source[i] = i % 4 == 3 ? 255 : uint8_t(random());
    video::BilinearUpscaler upscaler; require(upscaler.Configure(sw, sh, tw, th), "Upscaler configuration");
    std::vector<uint8_t> simd(size_t(tw) * th * 4, 0x77), reference(simd.size(), 0x77);
    std::vector<std::array<uint32_t, 4>> rects{{0, 0, tw, th}};
    for (int n = 0; n < 6; ++n) { const uint32_t x0 = random() % tw, y0 = random() % th; rects.push_back({x0, y0, x0 + 1 + random() % (tw - x0), y0 + 1 + random() % (th - y0)}); }
    for (const auto& [x0, y0, x1, y1] : rects) {
      std::fill(simd.begin(), simd.end(), uint8_t(0x77)); std::fill(reference.begin(), reference.end(), uint8_t(0x77));
      upscaler.Resample(source.data(), size_t(sw) * 4, simd.data(), size_t(tw) * 4, x0, y0, x1, y1);
      video::BilinearUpscaler::ReferenceResample(source.data(), size_t(sw) * 4, sw, sh, reference.data(), size_t(tw) * 4, tw, th, x0, y0, x1, y1);
      require(simd == reference, "SIMD bilinear upscale differs from its scalar definition");
    }
    if (sw == tw && sh == th) { upscaler.Resample(source.data(), size_t(sw) * 4, simd.data(), size_t(tw) * 4, 0, 0, tw, th); require(simd == source, "1:1 upscale was not the identity"); }
  }
  video::BilinearUpscaler invalid; require(!invalid.Configure(320, 180, 300, 180) && !invalid.Configure(1, 180, 300, 180), "Downscale/degenerate upscaler accepted");
}

// Scaled source (legacy encoder smaller than the remote): tiles in source
// coordinates, video upscaled elsewhere, drops at their video pts.
void scaledCompositorRules() {
  const uint32_t vw = 320, vh = 180, sw = 480, sh = 270; Canvas source{int(sw), int(sh)}; source.texture(5);
  std::mt19937 random(8);
  const auto video = [&] { std::vector<uint8_t> v(size_t(vw) * vh * 4); for (size_t i = 0; i < v.size(); ++i) v[i] = i % 4 == 3 ? 255 : uint8_t(random()); return v; };
  const auto payload = [&](uint32_t w, uint32_t h, uint32_t sequence, uint64_t dropPts, std::vector<uint16_t> drops, std::vector<uint16_t> adds) {
    lossless::PayloadBuilder builder(w, h, sequence, dropPts, drops);
    for (const auto i : adds) { const auto r = lossless::tileRect(i, w, h); builder.add(i, source.bgra.data() + (size_t(r.y) * sw + r.x) * 4, size_t(sw) * 4); }
    return builder.finish();
  };
  const auto expectFrame = [&](const std::vector<uint8_t>& frame, const std::vector<uint8_t>& v, const std::vector<uint16_t>& tiles, const char* reason) {
    std::vector<uint8_t> expected(size_t(sw) * sh * 4);
    video::BilinearUpscaler::ReferenceResample(v.data(), size_t(vw) * 4, vw, vh, expected.data(), size_t(sw) * 4, sw, sh, 0, 0, sw, sh);
    for (const auto i : tiles) { const auto r = lossless::tileRect(i, sw, sh); for (uint32_t y = r.y; y < r.y + r.height; ++y) for (uint32_t x = r.x; x < r.x + r.width; ++x) { const auto* s = &source.bgra[(size_t(y) * sw + x) * 4]; auto* d = &expected[(size_t(y) * sw + x) * 4]; d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; } }
    require(frame == expected, reason);
  };
  lossless::Compositor c; require(c.Configure(vw, vh, true), "Scaled compositor configuration");
  auto first = payload(sw, sh, 1, 0, {}, {0, 3, 7});
  require(c.Accept(first.data(), first.size()) == lossless::Compositor::Outcome::accepted && c.scaled() && c.width() == sw && c.videoWidth() == vw, "Source-size tiles were not accepted over a smaller video");
  std::vector<uint8_t> frame(size_t(sw) * sh * 4);
  auto v1 = video(); c.ComposeUpscaled(v1.data(), size_t(vw) * 4, frame.data(), size_t(sw) * 4);  // republish of a video-size shown frame
  expectFrame(frame, v1, {0, 3, 7}, "Republished scaled frame is not tiles over the upscaled video");
  auto drop = payload(sw, sh, 2, 2000, {3}, {});
  require(c.Accept(drop.data(), drop.size()) == lossless::Compositor::Outcome::accepted, "Drop rejected");
  auto v2 = video(); c.ComposeDecodedScaled(v2.data(), size_t(vw) * 4, frame.data(), size_t(sw) * 4, 1999);
  expectFrame(frame, v2, {0, 3, 7}, "Scaled drop applied before its video frame");
  auto v3 = video(); c.ComposeDecodedScaled(v3.data(), size_t(vw) * 4, frame.data(), size_t(sw) * 4, 2000);
  expectFrame(frame, v3, {0, 7}, "A stale scaled tile overlaid newer video");
  auto add = payload(sw, sh, 3, 0, {}, {5});
  require(c.Accept(add.data(), add.size()) == lossless::Compositor::Outcome::accepted && c.republishPending(), "Scaled add not pending");
  c.ComposeAdded(frame.data(), size_t(sw) * 4); expectFrame(frame, v3, {0, 5, 7}, "Scaled add was not republished over the shown frame");
  auto wrong = payload(sw - 2, sh, 4, 0, {}, {1});
  require(c.Accept(wrong.data(), wrong.size()) == lossless::Compositor::Outcome::ignored && c.poisoned(), "Changed source size was accepted");
  auto v4 = video(); c.ComposeDecodedScaled(v4.data(), size_t(vw) * 4, frame.data(), size_t(sw) * 4, 3000);
  expectFrame(frame, v4, {}, "A poisoned scaled compositor composited a tile");
  // Sizes it must refuse: smaller than the video, an invalid frame layout
  // (odd width; the old 4x magnification limit was replaced by wire/stride/
  // memory checks, see Compositor::parse), or any scaling for tracks.
  for (const auto& [w, h, allow] : {std::tuple<uint32_t, uint32_t, bool>{300, 170, true}, {481, 270, true}, {sw, sh, false}}) {
    lossless::Compositor other; require(other.Configure(vw, vh, allow), "Compositor configuration");
    lossless::PayloadBuilder builder(w, h, 1, 0, {}); auto bytes = builder.finish();
    require(other.Accept(bytes.data(), bytes.size()) == lossless::Compositor::Outcome::ignored && !other.scaled(), "An invalid source size was accepted");
  }
  // Scale 1 through the scaled-capable path stays in place (no upscaling).
  lossless::Compositor same; require(same.Configure(sw, sh, true), "Compositor configuration");
  auto unscaled = payload(sw, sh, 1, 0, {}, {2});
  require(same.Accept(unscaled.data(), unscaled.size()) == lossless::Compositor::Outcome::accepted && !same.scaled(), "Unscaled tiles took the scaled path");
}

// A settled tile that is not sent yet changes invisibly to NV12 (no video, no
// content-change tick) exactly when an add packet is built: the host must send
// the CURRENT pixels, not its older mirror.
void candidateVerification() {
  Loop loop(960, 540, deterministic()); std::mt19937 random(21);
  for (auto& b : loop.canvas.bgra) b = uint8_t(random());
  const auto corner = [&](uint32_t i) { const auto r = lossless::tileRect(i, 960, 540); return &loop.canvas.bgra[(size_t(r.y) * 960 + r.x) * 4]; };
  for (uint32_t i = 0; i < loop.source.tileCount(); ++i) { const auto r = lossless::tileRect(i, 960, 540); loop.canvas.fill(int(r.x), int(r.y), int(r.x) + 2, int(r.y) + 2, 255, 255, 255); }
  int guard = 0;
  while (loop.packets == 0 && guard++ < 200) loop.tick();
  require(loop.packets == 1 && loop.source.presentTiles() < loop.source.tileCount(), "Fixture needs a partially sent frame");
  loop.tick(); loop.tick();  // the next add packet is due on the third tick
  const auto before = loop.nv12(); uint32_t changed = 0, unsent = 0;
  for (uint32_t i = 0; i < loop.source.tileCount(); ++i) if (!loop.source.present(i) && unsent++ % 2 == 0) { corner(i)[0] = 254; ++changed; }
  require(changed && before == loop.nv12(), "Fixture change was visible in NV12");
  const auto packets = loop.packets; loop.tick();
  require(loop.packets == packets + 1, "Fixture missed the add tick");
  // A visible change in a sent tile makes the client decode a frame now.
  const uint32_t sent = [&] { for (uint32_t i = 0; i < loop.source.tileCount(); ++i) if (loop.source.present(i)) return i; return 0u; }();
  corner(sent)[1] = uint8_t(corner(sent)[1] ^ 0x80);
  loop.tick();
  loop.until(600);
  require(exact(loop.client, loop.canvas), "Verified candidates did not settle to exact");
}

void budgetsAndMotion() {
  // Continuous motion: tiles of the moving band are never sent while it moves,
  // so submissions equal a plain policy and packets stay drop-only/rare.
  auto settings = deterministic();
  Loop loop(1280, 720, settings); loop.canvas.texture(3);
  host::FrameEncodePolicy plain(size_t(1280) * 720 * 3 / 2); uint64_t plainSubmits = 0, plainNow = 0;
  const auto plainTick = [&](const Loop& l) { plainNow += 16667; const auto d = plain.prepare(l.nv12(), plainNow); if (d.submit) { (void)plain.output(plainNow, !plain.hasUsableIdr(), plainNow, 100); ++plainSubmits; } };
  for (int n = 0; n < 400 && !exact(loop.client, loop.canvas); ++n) { loop.tick(); plainTick(loop); }
  require(exact(loop.client, loop.canvas), "Initial frame was not exact before motion");
  const auto videos0 = loop.videos, plain0 = plainSubmits, packets0 = loop.packets;
  for (int n = 0; n < 120; ++n) {  // a 200 px band scrolling every tick
    for (int y = 300; y < 500; ++y) for (int x = 0; x < 1280; ++x) { auto* p = &loop.canvas.bgra[(size_t(y) * 1280 + x) * 4]; p[0] = uint8_t(x + n * 9); p[1] = uint8_t(y * 2 + n); p[2] = uint8_t((x ^ y) + n * 3); }
    loop.tick(); plainTick(loop);
  }
  require(loop.videos - videos0 == plainSubmits - plain0, "Motion changed the number of video submissions");
  require(loop.packets - packets0 <= 2, "Motion produced more than the initial drop packet(s)");
  loop.until(400);
  require(exact(loop.client, loop.canvas), "Moved band did not settle to exact");
  require(loop.maxPacket <= settings.packetBytes, "A packet exceeded the configured packet bound");
  // Byte rate: noise needs many packets; bytes stay inside the token bucket
  // (plus at most one tile of overshoot) and packets at <= 20 per second.
  Loop heavy(960, 540, settings); std::mt19937 random(9);
  for (auto& b : heavy.canvas.bgra) b = uint8_t(random());
  int ticks = 0; while (!exact(heavy.client, heavy.canvas) && ticks < 1200) { heavy.tick(); ++ticks; }
  require(exact(heavy.client, heavy.canvas), "Noise frame never became exact");
  const double seconds = ticks * 16667 / 1e6;
  require(double(heavy.tileBytes) <= double(settings.burstBytes) + double(settings.rateBytesPerSecond) * seconds + double(lossless::maximumTileBytes + 64), "Tile bytes exceeded the token bucket");
  require(heavy.packets <= uint64_t(seconds * 20) + 2, "Add packets exceeded 20 per second");
}

// Tiles stop under a running client decoder (Agent::stream): 게임 마우스 holds
// the mouse, or the capture was rebuilt for another mode (capture_follow,
// field case: League of Legends left the old desktop's tiles over the
// game until reconnect). The client drops every tile with the next video
// frame, shows none while they are off, and gets them back exactly when they
// resume on the same decoder.
void retirement() {
  for (const bool follow : {false, true}) {
    Loop loop(640, 360, deterministic()); loop.canvas.texture(1);
    loop.settle(240);
    require(exact(loop.client, loop.canvas) && loop.source.presentTiles() == loop.source.tileCount(), "Static frame did not become lossless before the pause");
    const auto videos = loop.videos, packets = loop.packets;
    // A follow also requests an IDR for its first tick (force).
    loop.streamTick(false, follow);
    // A game moves where the tiles were: never under an old tile.
    for (int n = 0; n < 30; ++n) { loop.canvas.texture(100 + n); loop.streamTick(false); }
    require(loop.packets == packets + 1 && loop.videos >= videos + 31, "Retirement was not one packet with a video frame, then plain video");
    const auto& p = loop.lastPayload;
    require(lossless::read16(p.data() + 20) == loop.source.tileCount() && lossless::read16(p.data() + 22) == 0 && lossless::read64(p.data() + 12) == loop.now - 30 * 16667, "Retirement did not drop every tile with its own video frame");
    require(loop.client.compositor.presentTiles() == 0 && loop.source.presentTiles() == 0 && !loop.client.compositor.poisoned(), "Tiles survived the retirement");
    // A retired source holds no frame-sized buffer (a follow never resumes it).
    require(loop.source.frameBytesHeld() == 0, "A retired tile source kept its mirror and picture");
    // A requested IDR while retired retires again, in sequence.
    loop.streamTick(false, true);
    require(loop.packets == packets + 2 && !loop.client.compositor.poisoned() && loop.client.compositor.presentTiles() == 0, "A forced tick while retired did not retire again in sequence");
    // Resumed (게임 마우스 let go) on the same decoder: same sequence, exact again.
    loop.canvas.texture(7); loop.settle(240);
    require(exact(loop.client, loop.canvas) && !loop.client.compositor.poisoned() && loop.source.presentTiles() == loop.source.tileCount(), "Tiles did not come back after the pause");
    require(loop.source.frameBytesHeld() >= size_t(640) * 360 * 4, "A resumed tile source did not recreate its mirror");
  }
  {  // The retirement packet is lost (send queue): the forced tick that follows
     // the dropped media makes the client stop compositing tiles at all.
    Loop loop(640, 360, deterministic()); loop.canvas.texture(2);
    loop.settle(240);
    require(exact(loop.client, loop.canvas), "Static frame did not become lossless before the loss");
    loop.streamTick(false, false, false);
    require(loop.client.compositor.presentTiles() == loop.source.tileCount(), "Fixture: the lost packet reached the client");
    loop.streamTick(false, true);
    require(loop.client.compositor.poisoned() && loop.client.compositor.presentTiles() == 0, "A retirement after a lost one left tiles on the client");
    for (int n = 0; n < 10; ++n) { loop.canvas.texture(200 + n); loop.streamTick(false); }
  }
}

// The capture's composed BGRA view stops following the picture the encoder
// gets (field case, League of Legends: 540 of 540 tiles of
// the old desktop stayed present while 32 fps of the game were encoded). The
// picture anchors the tiles: each tile whose part of the picture changes drops
// with that video frame, also when the dirty hint says nothing changed; none is
// added while its BGRA does not convert to the picture; they return when the
// view follows the capture again.
void pictureAnchorsTiles() {
  for (const bool hinted : {false, true}) {
    Loop loop(640, 360, deterministic()); loop.canvas.texture(1);
    loop.until(240);
    require(exact(loop.client, loop.canvas) && loop.source.presentTiles() == loop.source.tileCount(), "Static frame did not become lossless before the divergence");
    for (int n = 0; n < 10; ++n) loop.tick(std::vector<host::capture::PixelRect>{});
    const Canvas desktop = loop.canvas;
    const auto hint = [&]() -> std::optional<std::vector<host::capture::PixelRect>> { if (hinted) return std::vector<host::capture::PixelRect>{}; return std::nullopt; };
    // The game moves; the view still shows the desktop.
    for (int n = 0; n < 30; ++n) { loop.canvas.texture(100 + n); loop.divergentTick(desktop, hint()); }
    require(loop.source.presentTiles() == 0 && loop.client.compositor.presentTiles() == 0 && !loop.client.compositor.poisoned(), "Desktop tiles stayed over the moving picture");
    // The game holds still for a second: no desktop tile comes back.
    for (int n = 0; n < 60; ++n) loop.divergentTick(desktop, hint());
    require(loop.source.presentTiles() == 0 && loop.client.compositor.presentTiles() == 0, "A tile that is not the encoder's picture was added");
    require(loop.source.stats().mismatchTiles > 0, "The view that is not the picture's source was not counted");
    // The view follows the capture again: exact.
    loop.tick(); loop.until(240);
    require(exact(loop.client, loop.canvas) && loop.source.presentTiles() == loop.source.tileCount(), "Tiles did not return once the view followed the picture");
  }
  {  // A consistent capture never counts a mismatch.
    Loop loop(640, 360, deterministic()); loop.canvas.texture(4); loop.until(240);
    for (int n = 0; n < 20; ++n) { loop.canvas.fill(0, 0, 200, 100, uint8_t(n * 9), 30, 60); loop.tick(); }
    loop.until(240);
    require(exact(loop.client, loop.canvas) && loop.source.stats().mismatchTiles == 0, "A consistent capture counted a mismatch");
  }
}

// The same over a downscaled picture (legacy encoder smaller than the source;
// tiles in source pixels, the capture's AreaScaler). The part of the picture a
// tile alone makes anchors it: motion right next to a tile does not drop it; a
// frozen view neither keeps tiles over a moving picture nor adds them while the
// picture holds still.
void pictureAnchorsScaledTiles() {
  constexpr uint32_t sw = 640, sh = 360, pw = 320, ph = 180;
  Canvas picture{int(sw), int(sh)}, view{int(sw), int(sh)}; picture.texture(1);
  host::LosslessTileSource source(sw, sh, deterministic(), pw, ph); host::FrameEncodePolicy policy(size_t(pw) * ph * 3 / 2); host::ParallelWork helper;
  lossless::Compositor client; require(client.Configure(pw, ph, true), "Scaled compositor configuration");
  host::capture::AreaScaler scaler{int(sw), int(sh), int(pw), int(ph)};
  std::vector<uint8_t> reduced(size_t(pw) * ph * 4), nv12(size_t(pw) * ph * 3 / 2), decoded(size_t(pw) * ph * 4, 0), shown(size_t(sw) * sh * 4);
  uint64_t now = 0;
  // follow: the view is the picture's source; else it keeps what it showed.
  const auto tick = [&](bool follow, bool changed) {
    now += 16667; if (follow) view = picture;
    scaler.scale(picture.bgra.data(), size_t(sw) * 4, reduced.data(), size_t(pw) * 4); host::bgraToNv12(reduced.data(), nv12.data(), int(pw), int(ph));
    host::BufferComposedFrame frame(view.bgra.data(), int(sw), int(sh), changed ? std::nullopt : std::optional<std::vector<host::capture::PixelRect>>(std::vector<host::capture::PixelRect>{}));
    bool encoded = false;
    const auto result = host::losslessTick(source, helper, frame, policy, nv12, now, [&] { encoded = true; });
    require(result.decision.submit == encoded, "Encode call does not match the decision");
    if (result.payload) require(client.Accept(result.payload->data(), result.payload->size()) == lossless::Compositor::Outcome::accepted, "Scaled tile packet rejected");
    if (result.decision.submit) {
      require(policy.output(now, !policy.hasUsableIdr(), now, 100), "Synthetic output");
      if (client.active() && client.scaled()) client.ComposeDecodedScaled(decoded.data(), size_t(pw) * 4, shown.data(), size_t(sw) * 4, now);
    }
  };
  const auto settle = [&] { tick(true, true); for (int n = 0; n < 240 && source.presentTiles() < source.tileCount(); ++n) tick(true, false); return source.presentTiles() == source.tileCount(); };
  require(settle(), "Scaled static frame did not become lossless");
  // Motion right of x = 320, a tile column edge: columns 0-4 stay lossless.
  for (int n = 0; n < 30; ++n) {
    for (uint32_t y = 0; y < sh; ++y) for (uint32_t x = 320; x < sw; ++x) { auto* p = &picture.bgra[(size_t(y) * sw + x) * 4]; p[0] = uint8_t(x * 3 + n * 7); p[1] = uint8_t(y + n * 11); p[2] = uint8_t((x ^ y) + n); }
    tick(true, true);
    for (uint32_t row = 0; row < lossless::tileRows(sh); ++row) for (uint32_t column = 0; column < 5; ++column) require(source.present(row * lossless::tileColumns(sw) + column), "Motion beside a scaled tile dropped it");
  }
  require(settle(), "Scaled frame did not settle after the motion");
  // The view freezes while the picture moves everywhere, then holds still.
  for (int n = 0; n < 30; ++n) {
    picture.texture(300 + n); tick(false, true);
    require(source.presentTiles() == 0 && client.presentTiles() == 0, "Scaled tiles stayed over the moving picture");
  }
  for (int n = 0; n < 60; ++n) tick(false, true);
  require(source.presentTiles() == 0 && client.presentTiles() == 0 && !client.poisoned(), "A scaled tile was added from a view that is not the picture's source");
  require(settle(), "Scaled tiles did not return once the view followed the picture");
}

void compositorRules() {
  const int w = 256, h = 128; Canvas a(w, h), b(w, h); a.texture(1); b.texture(2);
  const auto payload = [&](uint32_t sequence, uint64_t dropPts, std::vector<uint16_t> drops, const Canvas& source, std::vector<uint16_t> adds) {
    lossless::PayloadBuilder builder(w, h, sequence, dropPts, drops);
    for (const auto i : adds) { const auto r = lossless::tileRect(i, w, h); builder.add(i, source.bgra.data() + (size_t(r.y) * w + r.x) * 4, size_t(w) * 4); }
    return builder.finish();
  };
  const auto tileOf = [&](const std::vector<uint8_t>& rgba, const Canvas& source, uint16_t i) { const auto r = lossless::tileRect(i, w, h); return sameRgb(source.bgra.data() + (size_t(r.y) * w + r.x) * 4, size_t(w) * 4, rgba.data() + (size_t(r.y) * w + r.x) * 4, size_t(w) * 4, r.width, r.height); };
  {  // A stale tile never overlays newer video: drop applies at pts >= dropPts.
    Client c(w, h); c.decoded(Client::video(a), 1000);
    c.tile(payload(1, 0, {}, a, {0, 1, 5}));
    require(c.republishes == 1 && tileOf(c.published, a, 0) && tileOf(c.published, a, 5), "Added tiles were not republished");
    c.tile(payload(2, 3000, {0}, a, {}));
    c.decoded(Client::video(a), 2000);  // a video frame older than the change
    require(tileOf(c.published, a, 0), "Drop applied before its video frame");
    c.decoded(Client::video(b), 3000);
    require(!tileOf(c.published, a, 0) && !tileOf(c.published, b, 0) && tileOf(c.published, a, 1), "Dropped tile still overlays the newer video");
    c.decoded(Client::video(b), 4000);
    require(!tileOf(c.published, a, 0) && c.compositor.presentTiles() == 2, "Dropped tile came back");
    // An add newer than a pending drop replaces it.
    c.tile(payload(3, 9000, {1}, a, {}));
    c.tile(payload(4, 0, {}, b, {1}));
    c.decoded(Client::video(b), 9000);
    require(tileOf(c.published, b, 1), "Newer add lost to an older pending drop");
  }
  {  // Epoch/resize: a fresh compositor has nothing; a size mismatch is ignored and poisons.
    Client c(w, h); c.decoded(Client::video(a), 1);
    c.tile(payload(1, 0, {}, a, {2}));
    require(c.compositor.Configure(w, h) && c.compositor.presentTiles() == 0 && !c.compositor.active(), "Reconfiguration kept tiles");
    lossless::PayloadBuilder other(w + 2, h, 1, 0, {}); auto wrong = other.finish();
    require(c.compositor.Accept(wrong.data(), wrong.size()) == lossless::Compositor::Outcome::ignored && c.compositor.poisoned(), "Wrong-size tile packet was accepted");
    c.decoded(Client::video(a), 2);
    require(!tileOf(c.published, a, 2), "A poisoned compositor composited a tile");
  }
  {  // Lost burst (sequence gap) and partial packets are harmless: no tile is shown again.
    Client c(w, h); c.decoded(Client::video(a), 1);
    c.tile(payload(1, 0, {}, a, {3, 4}));
    auto lost = payload(3, 0, {}, a, {6});
    require(c.compositor.Accept(lost.data(), lost.size()) == lossless::Compositor::Outcome::ignored && c.compositor.poisoned(), "Sequence gap not detected");
    c.decoded(Client::video(b), 2);
    require(!tileOf(c.published, a, 3) && !tileOf(c.published, a, 6) && c.compositor.presentTiles() == 0, "Tiles survived a lost burst");
    Client d(w, h); d.decoded(Client::video(a), 1);
    auto partial = payload(1, 0, {}, a, {3, 4}); partial.resize(partial.size() - 5);
    require(d.compositor.Accept(partial.data(), partial.size()) == lossless::Compositor::Outcome::ignored && d.compositor.poisoned(), "Truncated burst accepted");
    d.decoded(Client::video(a), 2);
    require(!tileOf(d.published, a, 3), "Truncated burst composited a tile");
    // H.264 payloads are never claimed.
    const uint8_t annexB[] = {0, 0, 0, 1, 0x65, 1, 2};
    require(d.compositor.Accept(annexB, sizeof(annexB)) == lossless::Compositor::Outcome::notTile, "H.264 payload claimed as a tile packet");
  }
  {  // Drops for absent tiles and out-of-range indices.
    Client c(w, h); c.decoded(Client::video(a), 1);
    c.tile(payload(1, 5, {7}, a, {}));  // absent: no effect
    auto bad = payload(2, 5, {9999}, a, {});
    require(c.compositor.Accept(bad.data(), bad.size()) == lossless::Compositor::Outcome::ignored && c.compositor.poisoned(), "Out-of-range drop accepted");
  }
}
}

int main() {
  try {
    codecRoundTrip(); compositorRules(); upscalerMatchesReference(); scaledCompositorRules(); tileSourceDiffing(); candidateVerification(); budgetsAndMotion(); retirement();
    pictureAnchorsTiles(); pictureAnchorsScaledTiles();
    std::cout << "{\"kind\":\"lossless-tiles-unit\",\"checks\":" << checks << ",\"syntheticPixels\":true,\"encoder\":false,\"realSessionAcceptance\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "lossless tiles: " << error.what() << '\n'; return 1; }
}
