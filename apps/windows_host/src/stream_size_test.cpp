// stream-size-v1 (stream_size.hpp): negotiated decoder limits, full-size
// encoding within them, the measured-load scale controller, the client decoder
// report hand-over and the portrait owned-output rule. Pure logic: no capture,
// encoder, display or network is used.
#include "stream_size.hpp"
#include "stream_resize.hpp"
#include "initial_viewport.hpp"
#include <iostream>

namespace {
using namespace host;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Json parse(const std::string& text) { return JsonParser::parse(text); }
using Clock = EncodeScaleController::Clock;
const DecoderLimits windows{3840, 3840, 8294400, 0};           // software decoder: no declared rate
const DecoderLimits phone{3840, 3840, 8294400, 248832000};     // 4K30-class hardware decoder
const DecoderLimits fast{3840, 3840, 8294400, 497664000};      // 4K60-class hardware decoder
StreamSizeBounds bounds(int width, int height, const DecoderLimits& limits, int fps = 60, std::optional<uint64_t> cap = {}) {
  return StreamSizeBounds{width, height, fps, 3840, 3840, limits, cap};
}
// A window with continuous motion: `frames` encoded, each `fixed` ms capture
// plus `scalable` ms convert/encode; the thread busy for `busy` of the second.
EncodeHostWindow motion(uint32_t frames, double fixed, double scalable, double busy = 0.99) {
  return {1.0, busy * 1000, fixed * frames, scalable * frames, frames};
}
EncodeHostWindow still() { return {1.0, 20, 0, 0, 0}; }
}

int main() {
  int checks = 0;
  try {
    // Owned outputs: portrait up to 2160x3840, longer side 3840, 8,294,400 pixels.
    require(ownedOutputSize(1080, 2400) && ownedOutputSize(2160, 3840) && ownedOutputSize(3840, 2160) && ownedOutputSize(320, 180), "Valid owned sizes refused");
    require(ownedOutputSize(2162,3840)&&ownedOutputSize(3840,3840)&&ownedOutputSize(3842,2160)&&ownedOutputSize(2400,3458)&&!ownedOutputSize(1081,2400)&&!ownedOutputSize(318,2400)&&!ownedOutputSize(UINT64_MAX,UINT64_MAX), "Owned geometry representability/alignment rule");
    require(encodedPictureSize(16,16)&&encodedPictureSize(1080,2400)&&encodedPictureSize(2160,3842)&&!encodedPictureSize(14,16), "Encoded picture rule");
    ++checks;

    // session.start parsing: exact marker, separate-v1 and four limits.
    const std::string base = R"({"type":"session.start","sessionId":"00000000-0000-0000-0000-000000000001","mode":"stream","resizeProtocol":"epoch-v1","viewportEncoderLimitsProtocol":"separate-v1","monitor":"0","width":3840,"height":3840,"fps":60,"epoch":0)";
    const auto request = streamSizeRequest(parse(base + R"(,"streamSizeProtocol":"stream-size-v1","decoderLimits":{"maxWidth":3840,"maxHeight":3840,"maxPixels":8294400,"maxPixelRate":0}})"));
    require(request && request->maxWidth == 3840 && request->maxHeight == 3840 && request->maxPixels == 8294400 && request->maxPixelRate == 0, "Decoder limits not parsed");
    require(!streamSizeRequest(parse(base + "}")), "A released session.start opted in");
    for (const auto* bad : {R"(,"streamSizeProtocol":"stream-size-unknown","decoderLimits":{"maxWidth":3840,"maxHeight":3840,"maxPixels":8294400,"maxPixelRate":0}})",
                            R"(,"streamSizeProtocol":"stream-size-v1"})",
                            R"(,"decoderLimits":{"maxWidth":3840,"maxHeight":3840,"maxPixels":8294400,"maxPixelRate":0}})",
                            R"(,"streamSizeProtocol":"stream-size-v1","decoderLimits":{"maxWidth":4096,"maxHeight":3840,"maxPixels":8294400,"maxPixelRate":0}})",
                            R"(,"streamSizeProtocol":"stream-size-v1","decoderLimits":{"maxWidth":3840,"maxHeight":3840,"maxPixels":8294400,"maxPixelRate":1}})",
                            R"(,"streamSizeProtocol":"stream-size-v1","decoderLimits":{"maxWidth":3840,"maxHeight":3840,"maxPixels":8294401,"maxPixelRate":0}})",
                            R"(,"streamSizeProtocol":"stream-size-v1","decoderLimits":{"maxWidth":3840,"maxHeight":3840,"maxPixels":8294400}})",
                            R"(,"streamSizeProtocol":"stream-size-v1","decoderLimits":{"maxWidth":3840,"maxHeight":3840,"maxPixels":8294400,"maxPixelRate":0,"extra":1}})",
                            R"(,"streamSizeProtocol":"stream-size-v1","mediaProtocol":"tracks-v1","decoderLimits":{"maxWidth":3840,"maxHeight":3840,"maxPixels":8294400,"maxPixelRate":0}})"}) {
      bool refused = false; try { (void)streamSizeRequest(parse(base + bad)); } catch (...) { refused = true; }
      require(refused, "Malformed stream size request accepted");
    }
    {
      bool refused = false; auto without = std::string(base); without.replace(without.find("separate-v1"), 11, "separate-v2");
      try { (void)streamSizeRequest(parse(without + R"(,"streamSizeProtocol":"stream-size-v1","decoderLimits":{"maxWidth":3840,"maxHeight":3840,"maxPixels":8294400,"maxPixelRate":0}})")); } catch (...) { refused = true; }
      require(refused, "stream-size-v1 without separate-v1 accepted");
    }
    // The administrator cap is optional: unset or invalid means none.
    require(!streamPixelCap(parse("{}")) && !streamPixelCap(parse(R"({"videoPixelBudget":5})")) && streamPixelCap(parse(R"({"videoPixelBudget":2073600})")) == 2073600ull, "Administrator cap parsing");
    ++checks;

    // A portrait box (two per-side ceilings) only with the marker.
    const auto portraitBox = parse(base + R"(,"streamSizeProtocol":"stream-size-v1","decoderLimits":{"maxWidth":3840,"maxHeight":3840,"maxPixels":8294400,"maxPixelRate":0}})");
    require(viewportEncoderLimits(portraitBox)->height == 3840, "Portrait box refused with the marker");
    { bool refused = false; try { (void)viewportEncoderLimits(parse(base + "}")); } catch (...) { refused = true; } require(refused, "A 3840-row box accepted without the marker"); }
    // A portrait initial viewport (a phone's own pixels) with the portrait box.
    {
      const std::string initial = R"({"type":"session.start","sessionId":"00000000-0000-0000-0000-000000000001","mode":"stream","resizeProtocol":"epoch-v1","viewportEncoderLimitsProtocol":"separate-v1","monitor":"0","width":3840,"height":3840,"fps":60,"epoch":0,"initialViewport":{"protocol":"epoch-v1","sessionId":"00000000-0000-0000-0000-000000000001","requestId":"00000000-0000-0000-0000-000000000003","resizeId":"00000000-0000-0000-0000-000000000004","monitor":"0","width":1080,"height":2400,"fps":60,"epoch":1})";
      const auto viewport = initialViewportRequest(parse(initial + R"(,"streamSizeProtocol":"stream-size-v1","decoderLimits":{"maxWidth":3840,"maxHeight":3840,"maxPixels":8294400,"maxPixelRate":248832000}})"));
      require(viewport && viewport->width == 1080 && viewport->height == 2400, "Portrait initial viewport refused");
      bool refused = false; try { (void)initialViewportRequest(parse(initial + "}")); } catch (...) { refused = true; }
      require(refused, "A portrait box without stream-size-v1 accepted");
    }
    // Resize requests carry owned sizes: portrait accepted, beyond refused.
    const auto resize = [](int w, int h) { return parse(R"({"type":"stream.resize","sessionId":"00000000-0000-0000-0000-000000000001","requestId":"r1","resizeId":"00000000-0000-0000-0000-000000000002","monitor":"0","width":)" + std::to_string(w) + R"(,"height":)" + std::to_string(h) + R"(,"fps":60,"epoch":2})"); };
    require(streamResizeRequest(resize(1080, 2400)).height == 2400 && streamResizeRequest(resize(2160, 3840)).height == 3840, "Portrait resize refused");
    require(streamResizeRequest(resize(2400,3458)).height==3458,"Representable resize above4K refused");
    {bool refused=false;try{(void)streamResizeRequest(resize(2401,3458));}catch(...){refused=true;}require(refused,"Odd owned resize accepted");}
    // Capability modes stay within the released bounds.
    require(advertisedOwnedModes(1920, 1080) == R"(,"modes":[{"width":1920,"height":1080,"refreshRate":60}])", "Landscape mode changed");
    require(advertisedOwnedModes(1080, 2400) == R"(,"modes":[{"width":1920,"height":1080,"refreshRate":60}])", "Portrait output advertised a mode released brokers refuse");
    require(advertisedOwnedModes(2560, 1440).find("2560") != std::string::npos && advertisedOwnedModes(1081, 2400).empty(), "Owned mode advertisement");
    ++checks;

    // Ceilings: a declared 4K30 decoder lets a portrait phone stream 1:1.
    {
      EncodeScaleController phoneStream(bounds(1080, 2400, phone));
      require(phoneStream.size() == std::pair{1080, 2400} && phoneStream.reason() == EncodeLimit::none && phoneStream.scalePermille() == 1000, "A portrait phone was not encoded 1:1");
      // 1440x3200 at 60 fps exceeds a 4K30-class decoder slightly; a 4K60 one takes it.
      EncodeScaleController hidpi(bounds(1440, 3200, phone));
      require(hidpi.reason() == EncodeLimit::decoderRate && hidpi.size().first >= 1340 && hidpi.size().first < 1440, "1440x3200 at 60 fps within a 4K30 decoder rate");
      require(EncodeScaleController(bounds(1440, 3200, fast)).size() == std::pair{1440, 3200}, "1440x3200 reduced for a 4K60 decoder");
      // 4K60 exceeds the declared rate: about 71 %, decoder_rate.
      EncodeScaleController uhd(bounds(3840, 2160, phone));
      const auto [w, h] = uhd.size();
      require(uhd.reason() == EncodeLimit::decoderRate && streamMacroblocks(w, h) * 256 * 60 <= phone.maxPixelRate && streamMacroblocks(w, h) * 256 * 60 >= phone.maxPixelRate * 95 / 100, "Declared decoder rate not applied");
      // At 30 fps the same decoder takes 4K.
      require(EncodeScaleController(bounds(3840, 2160, phone, 30)).size() == std::pair{3840, 2160}, "4K30 reduced for a 4K30 decoder");
      // A decoder limited to 2304 rows scales a portrait output by its height.
      DecoderLimits tall = phone; tall.maxHeight = 2304;
      EncodeScaleController limited(bounds(1440, 3200, tall));
      require(limited.size().second <= 2304 && limited.size().second >= 2290 && limited.reason() == EncodeLimit::decoderSize, "Decoder height limit");
      // Padding to whole macroblocks never exceeds the frame limit.
      EncodeScaleController padded(bounds(2398, 3458, windows, 30));
      require(streamMacroblocks(padded.size().first, padded.size().second) <= 32400 && padded.size().first >= 2380, "Macroblock padding exceeded one 4K frame");
      // The administrator cap binds where set.
      EncodeScaleController capped(bounds(2960, 760, phone, 60, 1036800));
      require(capped.reason() == EncodeLimit::adminCap && uint64_t(capped.size().first) * capped.size().second <= 1036800, "Administrator cap not applied");
      // Without a cap or a declared rate limit nothing below 4K is reduced.
      require(EncodeScaleController(bounds(3440, 1440, fast)).size() == std::pair{3440, 1440} && EncodeScaleController(bounds(3840, 2160, fast)).size() == std::pair{3840, 2160}, "Reduced without a reason");
      // A Fold connected folded (1248x1868), then unfolded (2448x1848): the box
      // stays the per-side 3840 ceilings, so the unfolded output is encoded
      // whole (field data: a first-viewport box gave 1248x942).
      require(EncodeScaleController(bounds(1248, 1868, fast)).size() == std::pair{1248, 1868} && EncodeScaleController(bounds(2448, 1848, fast)).size() == std::pair{2448, 1848}, "A Fold unfolded after connecting folded was not encoded whole");
    }
    ++checks;

    // A software decoder without a declared rate starts at the released safe
    // budget (today's behaviour) and goes to full size only with measured
    // headroom from the client.
    {
      const auto start = Clock::now();
      EncodeScaleController soft(bounds(3840, 2160, windows), {}, start);
      const auto released = streamEncodeSize(3840, 2160, 3840, 2160, 60, streamPixelBudgetDefault);
      require(std::abs(soft.size().first - released.first) <= 16 && soft.reason() == EncodeLimit::decoderRate, "Undeclared rate did not start at the released budget");
      require(!soft.observe(still(), std::nullopt, start + std::chrono::seconds(3)) && !soft.observe(still(), std::nullopt, start + std::chrono::seconds(4)), "Stepped above the safe budget without client measurement");
      require(soft.observe(still(), ClientDecodeReport{150, 0}, start + std::chrono::seconds(5)), "Client headroom did not lift the safe budget");
      require(soft.size() == std::pair{3840, 2160} && soft.reason() == EncodeLimit::none, "Full size not reached");
      // 4K60 at 30 fps is within the safe budget: full size at once.
      require(EncodeScaleController(bounds(3840, 2160, windows, 30)).size() == std::pair{3840, 2160}, "4K30 reduced for a software decoder");
    }
    ++checks;

    // Host overload, a hardware encoder (the rule is every encoder's): after two
    // windows, at most every three seconds, the rate goes first (no new encoder),
    // to the highest rung the measured work fits at 75 %; the size only when the
    // lowest rate still cannot be held, to 0.75 first; after ten seconds of calm
    // the size comes back first, then the rate one rung at a time.
    {
      const auto start = Clock::now();
      EncodeScaleController c(bounds(1080, 2400, phone), {}, start);
      const auto overloaded = motion(43, 3, 20);  // 23 ms a frame: fits 30 fps at 75 %, not 45
      require(!c.observe(overloaded, std::nullopt, start + std::chrono::seconds(1)), "One overloaded window changed something");
      require(!c.observe(overloaded, std::nullopt, start + std::chrono::milliseconds(2000)) && c.fps() == 60, "Changed within three seconds of the start");
      require(!c.observe(overloaded, std::nullopt, start + std::chrono::seconds(3)), "A rate step restarted the encoder");
      require(c.fps() == 30 && c.size() == std::pair{1080, 2400} && c.reason() == EncodeLimit::hostCpu && c.memory().fps == 30, "Sustained overload did not lower the rate first");
      // Overload right after a change: no second change before three seconds.
      const auto still30 = motion(26, 3, 60);  // 63 ms a frame: 30 fps not held either
      require(!c.observe(still30, std::nullopt, start + std::chrono::seconds(4)) && !c.observe(still30, std::nullopt, start + std::chrono::seconds(5)) && c.size().first == 1080, "Changed again within three seconds");
      // Those two windows counted; three seconds after the rate step the size goes, to 0.75.
      require(c.observe(still30, std::nullopt, start + std::chrono::seconds(7)) && std::abs(c.size().first - 1080 * 0.75) <= 4 && c.fps() == 30, "30 fps not held did not step the size to 0.75");
      require(c.memory().pixels == uint64_t(c.size().first) * c.size().second && c.memory().reason == EncodeLimit::hostCpu, "Overload not remembered");
      // Calm: the size back first, then the rate one rung at a time.
      const auto light = motion(30, 1, 4, 0.2);
      auto at = start + std::chrono::seconds(17);
      (void)c.observe(light, std::nullopt, at); at += std::chrono::seconds(2);
      require(c.observe(light, std::nullopt, at) && c.size() == std::pair{1080, 2400} && c.fps() == 30, "Calm did not restore the size before the rate");
      at += std::chrono::seconds(2); (void)c.observe(light, std::nullopt, at); require(c.fps() == 45, "Calm did not raise the rate to 45");
      at += std::chrono::seconds(2); (void)c.observe(light, std::nullopt, at);
      require(c.fps() == 60 && c.reason() == EncodeLimit::none && c.memory().pixels == 0 && c.memory().fps == 0, "Calm did not restore 60 fps");
    }
    ++checks;

    // The client's decoder at 60 fps: the rate first as well, to the rung its busy share fits at 750 per mille.
    {
      const auto start = Clock::now();
      EncodeScaleController c(bounds(2560, 1440, fast), {}, start);
      require(!c.observe(still(), ClientDecodeReport{960, 10}, start + std::chrono::seconds(3)), "One busy report changed something");
      require(!c.observe(still(), ClientDecodeReport{960, 10}, start + std::chrono::seconds(4)), "A rate step restarted the encoder");
      require(c.fps() == 45 && c.size() == std::pair{2560, 1440} && c.reason() == EncodeLimit::decoderLoad, "A busy client decoder did not lower the rate first (960 * 45/60 = 720)");
    }
    ++checks;

    // Field case (a VM composed on Microsoft Basic Render): capture alone over the budget is never
    // answered with a smaller picture (it reads the source at full size anyway, so text only blurs), and a
    // picture made small earlier comes back when the thread has room, even while capture paces the frames.
    {
      const auto start = Clock::now();
      EncodeScaleController c(bounds(2460, 1240, fast), {}, start);
      const auto captureBound = motion(40, 14, 2);  // 14 ms of capture, 2 ms of encode: 40 of 60 fps, thread saturated
      auto at = start;
      for (int i = 0; i < 6; ++i) { at += std::chrono::seconds(2); require(!c.observe(captureBound, std::nullopt, at), "A capture-bound overload made the picture smaller"); }
      require(c.size() == std::pair{2460, 1240}, "The size changed under a capture-bound overload");
      // Made small before (carried from an earlier step down), then capture still slow but the thread at 40 %:
      // the per-frame projection has no room (8 + 3.5 ms of 11.7), the busy share does.
      EncodeScaleController shrunk(bounds(2460, 1240, fast), {1864ull * 938, EncodeLimit::hostCpu}, start);
      require(shrunk.size().first < 2000, "Carried size setup");
      const auto paced = motion(35, 8, 3.5, 0.40);
      at = start + std::chrono::seconds(11);
      (void)shrunk.observe(paced, std::nullopt, at); at += std::chrono::seconds(2);
      require(shrunk.observe(paced, std::nullopt, at) && shrunk.size() == std::pair{2460, 1240} && shrunk.reason() == EncodeLimit::none, "A slow-capture stream with room did not come back to full size");
      // The same frames with the thread saturated stay small (no step up into an overload).
      EncodeScaleController busy(bounds(2460, 1240, fast), {1864ull * 938, EncodeLimit::hostCpu}, start);
      const auto saturated = motion(35, 8, 3.5, 0.69);
      at = start + std::chrono::seconds(11);
      (void)busy.observe(saturated, std::nullopt, at); at += std::chrono::seconds(2);
      require(!busy.observe(saturated, std::nullopt, at) && busy.size().first < 2000, "Stepped up with the thread at 69 %");
    }
    ++checks;

    // A software encoder's own overload lowers the capture rate first (60 -> 30
    // here: the measured 30 ms fits neither 45 nor 30 fps at 75 %), then the
    // size to 0.75, below that only while 30 fps still cannot be held, never
    // below 0.5. Calm restores the size first, then the rate. (Size first took a
    // field stream to a quarter of 5120x1440 under x264.)
    {
      const auto start = Clock::now();
      EncodeScaleController c(bounds(5120, 1440, fast), {}, start);
      require(c.fps() == 60 && c.size() == std::pair{3840, 1080}, "Start");  // the 4K decoder bound
      EncodeScaleController x(StreamSizeBounds{5120, 1440, 60, 5120, 5120, DecoderLimits{5120, 5120, 7372800, 7372800ull * 60}, {}}, {}, start);
      require(x.fps() == 60 && x.size() == std::pair{5120, 1440} && x.reason() == EncodeLimit::none, "Full size at the start");
      const auto heavy = motion(33, 3, 27);  // 30 ms a frame at 60 fps: busy, 33 fps
      require(!x.observe(heavy, std::nullopt, start + std::chrono::seconds(2), true), "One overloaded window changed something");
      require(!x.observe(heavy, std::nullopt, start + std::chrono::seconds(3), true), "A rate step restarted the encoder");
      require(x.fps() == 30 && x.size() == std::pair{5120, 1440} && x.scalePermille() == 1000, "The rate did not go first");
      require(x.reason() == EncodeLimit::hostCpu && x.memory().fps == 30, "The rate step was not reported or remembered");
      // At 30 fps the same frames are 30 of 33 ms: still overloaded, so the size, to 0.75.
      const auto still30 = motion(26, 3, 60);
      require(!x.observe(still30, std::nullopt, start + std::chrono::seconds(5), true), "One window");
      require(x.observe(still30, std::nullopt, start + std::chrono::seconds(6), true), "30 fps not held did not step the size");
      require(x.fps() == 30 && std::abs(x.size().first - 5120 * 0.75) <= 8, "The size went below 0.75 while one step could hold 30 fps");
      // Still not held: below 0.75, never below 0.5.
      auto at = start + std::chrono::seconds(6);
      for (int i = 0; i < 20; ++i) { at += std::chrono::seconds(2); (void)x.observe(still30, std::nullopt, at, true); }
      require(x.size().first >= 2560 - 8 && x.size().first < 3840 - 8 && x.fps() == 30, "Software floor is half the source");
      // Calm: the size back first, then the rate one rung at a time.
      const auto light = motion(30, 1, 4, 0.2);
      at += std::chrono::seconds(10);
      (void)x.observe(light, std::nullopt, at, true); at += std::chrono::seconds(2);
      require(x.observe(light, std::nullopt, at, true) && x.size() == std::pair{5120, 1440} && x.fps() == 30, "Calm did not restore the size before the rate");
      at += std::chrono::seconds(2); require(!x.observe(light, std::nullopt, at, true) && x.fps() == 45, "Calm did not raise the rate to 45");
      at += std::chrono::seconds(2); (void)x.observe(light, std::nullopt, at, true);
      require(x.fps() == 60 && x.reason() == EncodeLimit::none && x.memory().pixels == 0 && x.memory().fps == 0, "Calm did not restore 60 fps");
      // A resize restart keeps the rate it learned; a hardware encoder follows the same rate-first rule.
      EncodeScaleController carried(StreamSizeBounds{2560, 1440, 60, 5120, 5120, DecoderLimits{5120, 5120, 7372800, 7372800ull * 60}, {}}, {0, EncodeLimit::hostCpu, 30}, start);
      require(carried.fps() == 30 && carried.size() == std::pair{2560, 1440} && carried.reason() == EncodeLimit::hostCpu, "A remembered rate was not applied");
      EncodeScaleController gpu(StreamSizeBounds{5120, 1440, 60, 5120, 5120, DecoderLimits{5120, 5120, 7372800, 7372800ull * 60}, {}}, {}, start);
      (void)gpu.observe(heavy, std::nullopt, start + std::chrono::seconds(2));
      require(!gpu.observe(heavy, std::nullopt, start + std::chrono::seconds(3)) && gpu.fps() == 30 && gpu.size().first == 5120, "A hardware encoder's overload did not lower the rate first");
    }
    ++checks;

    // Client decoder overload (busy share, or a growing backlog when the
    // client cannot measure busy time) steps down with decoder_load.
    {
      const auto start = Clock::now();
      EncodeScaleController busy(bounds(3840, 2160, phone, 30), {}, start);
      require(!busy.observe(still(), ClientDecodeReport{960, 10}, start + std::chrono::seconds(3)), "One busy report changed the scale");
      require(busy.observe(still(), ClientDecodeReport{960, 10}, start + std::chrono::seconds(4)), "A saturated client decoder did not step down");
      require(busy.reason() == EncodeLimit::decoderLoad && std::abs(busy.size().first - 3840 * std::sqrt(750.0 / 960)) <= 8, "Client busy step size");
      EncodeScaleController backlog(bounds(3840, 2160, phone, 30), {}, start);
      (void)backlog.observe(still(), ClientDecodeReport{-1, 600}, start + std::chrono::seconds(3));
      require(backlog.observe(still(), ClientDecodeReport{-1, 600}, start + std::chrono::seconds(4)) && std::abs(backlog.size().first - 3072) <= 8, "Client backlog step");
      // Repeated overload never goes below a quarter of the source.
      EncodeScaleController floor(bounds(3840, 2160, phone, 30), {}, start);
      auto at = start;
      for (int i = 0; i < 60; ++i) { at += std::chrono::seconds(2); (void)floor.observe(still(), ClientDecodeReport{1000, 5000}, at); }
      require(floor.size().first >= 960 && floor.size().first <= 1000, "Minimum scale");
    }
    ++checks;

    // What a session learned survives a resize restart (the next controller
    // starts at the sustained pixels) and is cleared by a full-size step up.
    {
      const auto start = Clock::now();
      EncodeScaleController carried(bounds(2400, 1080, phone), {1000000, EncodeLimit::hostCpu}, start);
      require(carried.reason() == EncodeLimit::hostCpu && uint64_t(carried.size().first) * carried.size().second <= 1000000 && uint64_t(carried.size().first) * carried.size().second >= 950000, "Remembered load not applied after a resize");
      require(!carried.observe(still(), std::nullopt, start + std::chrono::seconds(5)) && !carried.observe(still(), std::nullopt, start + std::chrono::seconds(6)), "Stepped up within ten seconds of a remembered overload");
      require(carried.observe(still(), std::nullopt, start + std::chrono::seconds(10)), "Calm did not step up ten seconds after a remembered overload");
      require(carried.size() == std::pair{2400, 1080} && carried.memory().pixels == 0, "Calm after a remembered overload did not restore full size");
    }
    ++checks;

    // Client decoder report hand-over (service latch -> worker).
    {
      const auto packed = streamDecoderReport(parse(R"({"type":"stream.decoder","sessionId":"s","busy":930,"backlogMs":40})"));
      require(packed && *packed && unpackDecoderReport(*packed).busy == 930 && unpackDecoderReport(*packed).backlogMs == 40, "Decoder report packing");
      const auto unknown = streamDecoderReport(parse(R"({"type":"stream.decoder","sessionId":"s","busy":-1,"backlogMs":0})"));
      require(unknown && *unknown && unpackDecoderReport(*unknown).busy == -1, "Unmeasured busy share");
      require(streamDecoderMessage("s", *packed) == R"({"type":"stream.decoder","sessionId":"s","busy":930,"backlogMs":40})", "Worker message");
      for (const auto* bad : {R"({"type":"stream.decoder","sessionId":"s","busy":1001,"backlogMs":0})", R"({"type":"stream.decoder","sessionId":"s","busy":-2,"backlogMs":0})",
                              R"({"type":"stream.decoder","sessionId":"s","busy":5.5,"backlogMs":0})", R"({"type":"stream.decoder","sessionId":"s","busy":5,"backlogMs":60001})",
                              R"({"type":"stream.decoder","busy":5,"backlogMs":0})", R"({"type":"stream.decoder","sessionId":"s","busy":5,"backlogMs":0,"x":1})", R"({"type":"stream.decoders","sessionId":"s","busy":5,"backlogMs":0})"})
        require(!streamDecoderReport(parse(bad)), "Malformed decoder report accepted");
    }
    ++checks;
    std::cout << "{\"kind\":\"stream-size-v1\",\"checks\":" << checks << ",\"capture\":false,\"encoder\":false,\"display\":false,\"network\":false}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
