// decoder-prestart-v1 on the real Windows CPU H.264 decoder (Microsoft MFT) and
// the real RemoteMediaSession / RemoteEngine / RemoteTrackEngine, fed with
// H.264 from the Microsoft encoder MFT; the Flutter registrar is a fixture and
// WARP stands in for the GPU. No VM, relay, Flutter raster or display.
//  - A prestarted decoder (any provisional size and frame rate) gives exactly
//    the pictures of a new decoder, also across a later hardware fallback.
//  - A size a new decoder refuses is refused the same way; a prestart is taken once.
//  - Sessions: Prepare -> Start / StartTracks / a held-picture start use it; the
//    GPU texture path too; Close and dropping never wait for a start.
// `--benchmark [load=N] [runs=N] [dial=MS] [size=WxH]`: stream info -> first decoded
// picture with and without a prestart, at Idle priority with N busy threads of
// the same priority (this process only; the field had a busy VM). With load
// it fails unless the prestart at least halves the p50.
// Built with MULTITAP_PRESTART_BASELINE (sources without decoder-prestart-v1)
// only the "fresh" timing runs: the before measurement.
#include "remote_media_session.h"
#include <codecapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using V = flutter::EncodableValue;
using Clock = std::chrono::steady_clock;
namespace {
unsigned checks = 0;
void require(bool ok, const std::string& reason) { ++checks; if (!ok) throw std::runtime_error(reason); }
void check(HRESULT hr, const std::string& reason) { require(SUCCEEDED(hr), reason + " (0x" + [&] { char b[16]; snprintf(b, sizeof b, "%08lx", hr); return std::string(b); }() + ")"); }
double ms(Clock::time_point from, Clock::time_point to = Clock::now()) { return std::chrono::duration<double, std::milli>(to - from).count(); }

// The Microsoft H.264 encoder MFT like the Host's MFT backend (see
// track_lanes_smoke): CBR, no B-frames, low latency, SPS/PPS before the IDR.
struct Unit { std::vector<uint8_t> bytes; uint64_t pts = 0; bool key = false; };
std::vector<Unit> encode(uint32_t width, uint32_t height, unsigned count) {
  ComPtr<IMFTransform> mft;
  check(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft)), "H.264 encoder MFT");
  ComPtr<ICodecAPI> codec; mft.As(&codec);
  const auto set = [&](const GUID& key, ULONG value, bool boolean = false) {
    if (!codec) return; VARIANT v; VariantInit(&v); v.vt = VARTYPE(boolean ? VT_BOOL : VT_UI4);
    if (boolean) v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE; else v.ulVal = value;
    codec->SetValue(&key, &v);
  };
  set(CODECAPI_AVLowLatencyMode, 1, true); set(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
  set(CODECAPI_AVEncMPVDefaultBPictureCount, 0); set(CODECAPI_AVEncMPVGOPSize, 3600);
  ComPtr<IMFMediaType> output; check(MFCreateMediaType(&output), "Output type");
  output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  output->SetUINT32(MF_MT_AVG_BITRATE, std::max<uint32_t>(2000000, width * height * 4)); MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height);
  MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, 60, 1); MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
  check(mft->SetOutputType(0, output.Get(), 0), "Encoder output type " + std::to_string(width) + "x" + std::to_string(height));
  ComPtr<IMFMediaType> input; check(MFCreateMediaType(&input), "Input type");
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height); MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 60, 1);
  MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1); input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  check(mft->SetInputType(0, input.Get(), 0), "Encoder input type");
  check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Encoder begin");
  check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Encoder start");
  const auto nal = [](const std::vector<uint8_t>& b, uint8_t type) { for (size_t i = 0; i + 3 < b.size(); ++i) if (!b[i] && !b[i + 1] && b[i + 2] == 1 && (b[i + 3] & 31) == type) return true; return false; };
  std::vector<uint8_t> headers;
  std::vector<Unit> units;
  for (unsigned n = 0; n < count; ++n) {
    // A moving, textured picture (as in track_lanes_smoke).
    std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) nv12[size_t(y) * width + x] = uint8_t(30 + ((x * 7 + y * 3 + n * 11) ^ (x * y + n)) % 64);
    for (size_t i = size_t(width) * height; i < nv12.size(); ++i) nv12[i] = uint8_t(96 + (i * 5 + n) % 64);
    if (!n) set(CODECAPI_AVEncVideoForceKeyFrame, 1);
    ComPtr<IMFMediaBuffer> buffer; check(MFCreateMemoryBuffer(DWORD(nv12.size()), &buffer), "Raw buffer");
    BYTE* target = nullptr; check(buffer->Lock(&target, nullptr, nullptr), "Lock raw"); std::memcpy(target, nv12.data(), nv12.size()); buffer->Unlock(); buffer->SetCurrentLength(DWORD(nv12.size()));
    ComPtr<IMFSample> sample; check(MFCreateSample(&sample), "Raw sample"); sample->AddBuffer(buffer.Get());
    const uint64_t pts = 1000 + uint64_t(n) * 16667;
    sample->SetSampleTime(LONGLONG(pts) * 10); sample->SetSampleDuration(166667);
    check(mft->ProcessInput(0, sample.Get(), 0), "Encode input");
    Unit unit; unit.pts = pts;
    for (;;) {
      MFT_OUTPUT_STREAM_INFO info{}; check(mft->GetOutputStreamInfo(0, &info), "Encoder output info");
      ComPtr<IMFSample> out; ComPtr<IMFMediaBuffer> memory;
      check(MFCreateSample(&out), "Encoded sample"); check(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 8 * 1024 * 1024), &memory), "Encoded buffer"); out->AddBuffer(memory.Get());
      MFT_OUTPUT_DATA_BUFFER data{}; data.pSample = out.Get(); DWORD status = 0;
      const HRESULT hr = mft->ProcessOutput(0, 1, &data, &status);
      if (data.pEvents) data.pEvents->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
      check(hr, "Encode output");
      ComPtr<IMFMediaBuffer> bytes; check(out->ConvertToContiguousBuffer(&bytes), "Encoded bytes");
      BYTE* p = nullptr; DWORD length = 0; check(bytes->Lock(&p, nullptr, &length), "Lock encoded"); unit.bytes.insert(unit.bytes.end(), p, p + length); bytes->Unlock();
    }
    require(!unit.bytes.empty(), "The low-latency encoder held a picture back");
    unit.key = nal(unit.bytes, 5);
    require(unit.key == (n == 0), "Only the first picture is an IDR");
    if (unit.key && !(nal(unit.bytes, 7) && nal(unit.bytes, 8))) {
      if (headers.empty()) {
        ComPtr<IMFMediaType> type; check(mft->GetOutputCurrentType(0, &type), "Encoder current type");
        UINT32 length = 0; check(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &length), "Sequence header size");
        headers.resize(length); check(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, headers.data(), length, &length), "Sequence header");
      }
      unit.bytes.insert(unit.bytes.begin(), headers.begin(), headers.end());
    }
    units.push_back(std::move(unit));
  }
  mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
  return units;
}
// The legacy single-output media-v1 envelope.
std::vector<uint8_t> legacy(const Unit& unit) {
  std::vector<uint8_t> media(12 + unit.bytes.size());
  media[0] = 1; media[1] = unit.key ? 1 : 0; std::memcpy(media.data() + 4, &unit.pts, 8);
  std::copy(unit.bytes.begin(), unit.bytes.end(), media.begin() + 12);
  return media;
}
std::vector<uint8_t> track(const Unit& unit, uint8_t id) {
  multitap::tracks::Packet packet{multitap::tracks::video, uint8_t(unit.key ? 1 : 0), id, 1, 1, unit.pts, unit.bytes.data(), unit.bytes.size()};
  std::vector<uint8_t> bytes; require(multitap::tracks::pack(packet, bytes), "Track packet"); return bytes;
}

// Flutter's registrar as far as the engines use it; it keeps when each texture
// was first marked available (its first published picture).
struct Registrar final : flutter::TextureRegistrar {
  std::mutex mutex;
  std::map<int64_t, flutter::TextureVariant*> textures;
  std::map<int64_t, Clock::time_point> first;
  int64_t next = 0;
  int64_t RegisterTexture(flutter::TextureVariant* texture) override { std::lock_guard<std::mutex> lock(mutex); textures.emplace(++next, texture); return next; }
  bool MarkTextureFrameAvailable(int64_t id) override { const auto now = Clock::now(); std::lock_guard<std::mutex> lock(mutex); first.emplace(id, now); return textures.count(id) != 0; }
  void UnregisterTexture(int64_t id, std::function<void()> callback) override { { std::lock_guard<std::mutex> lock(mutex); textures.erase(id); } if (callback) callback(); }
  bool UnregisterTexture(int64_t id) override { UnregisterTexture(id, {}); return true; }
  // The first picture's time of `id`, waiting up to `limit`.
  bool FirstPicture(int64_t id, Clock::time_point& at, std::chrono::milliseconds limit = std::chrono::milliseconds(60000)) {
    const auto end = Clock::now() + limit;
    for (;;) {
      { std::lock_guard<std::mutex> lock(mutex); if (const auto it = first.find(id); it != first.end()) { at = it->second; return true; } }
      if (Clock::now() > end) return false;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }
};
int64_t integer(const flutter::EncodableMap& map, const char* key) {
  const auto it = map.find(V(key)); require(it != map.end(), std::string("Missing statistic ") + key);
  if (const auto* value = std::get_if<int64_t>(&it->second)) return *value;
  if (const auto* value = std::get_if<int32_t>(&it->second)) return *value;
  throw std::runtime_error("Statistic is not an integer");
}
bool flag(const flutter::EncodableMap& map, const char* key) {
  const auto it = map.find(V(key)); require(it != map.end() && std::holds_alternative<bool>(it->second), std::string("Missing flag ") + key); return std::get<bool>(it->second);
}
flutter::EncodableMap stats(multitap::RemoteMediaSession& owner, int64_t lease) { flutter::EncodableMap value; check(owner.Stats(lease, value), "Stats"); return value; }
void push(multitap::RemoteMediaSession& owner, int64_t lease, const std::vector<uint8_t>& bytes) {
  const auto end = Clock::now() + std::chrono::seconds(30);
  for (;;) {
    const HRESULT hr = owner.Push(lease, bytes);
    if (hr == S_OK) return;
    require(hr == multitap::kMediaQueueFull && Clock::now() < end, "Packet rejected");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
bool waitDecoded(multitap::RemoteMediaSession& owner, int64_t lease, int64_t frames) {
  const auto end = Clock::now() + std::chrono::seconds(60);
  while (Clock::now() < end) { if (integer(stats(owner, lease), "decodedFrames") >= frames) return true; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  return false;
}

struct Picture {
  uint32_t width = 0, height = 0; uint64_t pts = 0; std::vector<uint8_t> rgba;
  bool operator==(const Picture& b) const { return width == b.width && height == b.height && pts == b.pts && rgba == b.rgba; }
};
std::vector<Picture> decodeAll(const std::vector<Unit>& units, uint32_t width, uint32_t height, const multitap::RemoteDecoder::Options& options, bool* prestarted = nullptr, uint64_t* fallbacks = nullptr) {
  multitap::RemoteDecoder decoder; std::vector<Picture> pictures;
  check(decoder.Initialize(width, height, options), "Decoder start");
#ifndef MULTITAP_PRESTART_BASELINE
  if (prestarted) *prestarted = decoder.prestarted();
#else
  (void)prestarted;
#endif
  const auto keep = [&](multitap::DecodedFrame&& frame) { pictures.push_back({frame.width, frame.height, frame.pts_us, std::move(frame.rgba)}); };
  for (const auto& unit : units) check(decoder.Decode(unit.bytes.data(), unit.bytes.size(), unit.pts, keep), "Decode");
  check(decoder.Drain(keep), "Drain");
  if (fallbacks) *fallbacks = decoder.fallbacks();
  return pictures;
}

#ifndef MULTITAP_PRESTART_BASELINE
// A prestart's decoder gives the pictures of a new one, whatever the provisional size.
std::string sameAsNewDecoder() {
  struct Case { uint32_t provisionalWidth, provisionalHeight, provisionalFps, width, height; };
  const Case cases[] = {{1920, 1080, 60, 640, 360}, {640, 360, 30, 1920, 1080}, {1280, 720, 60, 1280, 720}, {640, 360, 60, 2560, 1440}, {1920, 1080, 60, 3840, 2160}};
  std::string sizes;
  for (const auto& c : cases) {
    const auto units = encode(c.width, c.height, 4);
    const auto reference = decodeAll(units, c.width, c.height, {});
    require(reference.size() == units.size() && reference.back().width == c.width && reference.back().height == c.height, "Reference decode");
    multitap::RemoteDecoder::Options options; options.prestart = multitap::DecoderPrestart::Begin(c.provisionalWidth, c.provisionalHeight, c.provisionalFps);
    require(options.prestart != nullptr, "Prestart did not begin");
    bool prestarted = false;
    const auto pictures = decodeAll(units, c.width, c.height, options, &prestarted);
    require(prestarted, "The decoder did not use its prestart");
    require(pictures == reference, "A prestarted decoder's pictures differ from a new decoder's at " + std::to_string(c.width) + "x" + std::to_string(c.height));
    sizes += (sizes.empty() ? "\"" : ",\"") + std::to_string(c.provisionalWidth) + "x" + std::to_string(c.provisionalHeight) + "->" + std::to_string(c.width) + "x" + std::to_string(c.height) + "\"";
  }
  return "{\"case\":\"sameAsNewDecoder\",\"sizes\":[" + sizes + "]}";
}
// Admission, single use and the hardware -> software fallback stay as they were.
std::string rulesKept() {
  // The MFT refuses 8192x8192 at SetInputType (a fresh one; a started one's
  // test-only check would accept it): the same refusal with or without a prestart.
  multitap::RemoteDecoder plain; const HRESULT refused = plain.Initialize(8192, 8192, {});
  require(FAILED(refused), "A new decoder accepted 8192x8192 (the case needs a size it refuses)");
  multitap::RemoteDecoder::Options options; options.prestart = multitap::DecoderPrestart::Begin(1920, 1080, 60);
  multitap::RemoteDecoder big; require(big.Initialize(8192, 8192, options) == refused && !big.prestarted(), "A prestart changed the admission of a refused size");
  require(options.prestart->Take() == nullptr, "A refused size left its prestart for another decoder");
  // One prestart, two decoders: the first takes it, the second starts its own.
  const auto units = encode(640, 360, 4);
  const auto reference = decodeAll(units, 640, 360, {});
  options.prestart = multitap::DecoderPrestart::Begin(640, 360, 60);
  bool first = false, second = true;
  require(decodeAll(units, 640, 360, options, &first) == reference && decodeAll(units, 640, 360, options, &second) == reference, "Shared prestart decode");
  require(first && !second, "A prestart was used twice or not at all");
  // A prestarted decoder standing in for the hardware one fails mid-stream: the
  // new software decoder replays from the IDR, the pictures are unchanged.
  for (const int failAt : {0, 2}) {
    multitap::RemoteDecoder::Options failing; failing.test_fail_hardware_after = failAt; failing.prestart = multitap::DecoderPrestart::Begin(1920, 1080, 60);
    bool prestarted = false; uint64_t fallbacks = 0;
    require(decodeAll(units, 640, 360, failing, &prestarted, &fallbacks) == reference && prestarted && fallbacks == 1, "The fallback after a prestarted decoder changed the pictures");
  }
  // Dropped or cancelled while it still starts: nothing waits for it.
  auto began = Clock::now();
  for (int i = 0; i < 4; ++i) multitap::DecoderPrestart::Begin(1920, 1080, 60).reset();
  auto cancelled = multitap::DecoderPrestart::Begin(1920, 1080, 60); cancelled->Cancel();
  require(ms(began) < 200, "Dropping a starting prestart waited for it");
  require(cancelled->Take() == nullptr, "A cancelled prestart was taken");
  require(!multitap::DecoderPrestart::Begin(0, 0, 60) && !multitap::DecoderPrestart::Begin(640, 360, 0), "An invalid prestart began");
  return "{\"case\":\"rulesKept\",\"refusedSize\":\"8192x8192\",\"takenOnce\":true,\"fallbackIdentical\":true}";
}
// Sessions: the start after Prepare uses it (legacy, held-picture and track
// starts, CPU and WARP GPU textures); Close never waits for it.
std::string sessions() {
  const auto units = encode(640, 360, 6);
  const auto reference = decodeAll(units, 640, 360, {});
  Registrar registrar;
  int64_t lease = 0, texture = -1;
  {
    multitap::RemoteMediaSession owner(&registrar);
    // No Prepare: as before.
    check(owner.Start(++lease, 640, 360, false, texture), "Start without a prestart");
    for (const auto& unit : units) push(owner, lease, legacy(unit));
    require(waitDecoded(owner, lease, int64_t(units.size())) && !flag(stats(owner, lease), "decoderPrestarted"), "A start without Prepare reported a prestart");
    // Prepare (another size), then the stream: the engine's decoder is the prestart's.
    owner.Prepare(1920, 1080, 60, 1);
    check(owner.Start(++lease, 640, 360, false, texture), "Start after Prepare");
    for (const auto& unit : units) push(owner, lease, legacy(unit));
    require(waitDecoded(owner, lease, int64_t(units.size())), "Prestarted engine decode");
    require(flag(stats(owner, lease), "decoderPrestarted"), "The engine after Prepare did not use the prestart");
    int64_t at = 0; uint64_t serial = 0; const auto latest = owner.Latest(at, serial);
    require(latest && latest->rgba == reference.back().rgba, "The prestarted engine's picture differs");
    // A viewport resize's held-picture start uses one too; the next start without Prepare does not.
    const int64_t heldTexture = texture;
    owner.Prepare(1280, 720, 60, 1);
    check(owner.Start(++lease, 640, 360, false, texture, {}, false, 60, 0, 0, true), "Held-picture start after Prepare");
    require(owner.HeldTexture() == heldTexture, "The previous picture was not held");
    for (const auto& unit : units) push(owner, lease, legacy(unit));
    require(waitDecoded(owner, lease, 1) && flag(stats(owner, lease), "decoderPrestarted"), "The held-picture start did not use the prestart");
    check(owner.Start(++lease, 640, 360, false, texture), "Start after a used prestart");
    for (const auto& unit : units) push(owner, lease, legacy(unit));
    require(waitDecoded(owner, lease, 1) && !flag(stats(owner, lease), "decoderPrestarted"), "A used prestart was used again");
    // Two outputs: each lane's decoder is one of the two prestarts.
    owner.Prepare(1280, 720, 60, 2);
    multitap::tracks::Configuration configuration; configuration.topology = 1;
    configuration.tracks = {{0, 1, 640, 360, 640, 360, -640, 0, true}, {1, 1, 640, 360, 640, 360, 0, 0, false}};
    std::map<uint8_t, int64_t> textures;
    check(owner.StartTracks(++lease, configuration, textures), "Track start after Prepare");
    for (const auto& unit : units) { push(owner, lease, track(unit, 0)); push(owner, lease, track(unit, 1)); }
    require(waitDecoded(owner, lease, 2 * int64_t(units.size())), "Track decode");
    for (const auto& value : std::get<flutter::EncodableList>(stats(owner, lease).at(V("tracks"))))
      require(flag(std::get<flutter::EncodableMap>(value), "decoderPrestarted"), "A track lane did not use a prestart");
    // The session ends before its stream info: Close releases the prestart without waiting.
    owner.Prepare(1920, 1080, 60, 2);
    const auto closing = Clock::now();
    owner.Close();
    require(ms(closing) < 2000, "Close waited for a starting prestart");
  }
  // The GPU texture path (WARP): CPU decoding into GPU textures, or the
  // hardware decoder where this PC has one (then the prestart goes unused).
  multitap::RemoteMediaSession gpuOwner(&registrar); gpuOwner.AllowSoftwareRenderForTests(true);
  gpuOwner.Prepare(1920, 1080, 60, 1);
  check(gpuOwner.Start(++lease, 640, 360, false, texture, {}, true), "GPU start after Prepare");
  for (const auto& unit : units) push(gpuOwner, lease, legacy(unit));
  require(waitDecoded(gpuOwner, lease, int64_t(units.size())), "GPU decode");
  const auto gpu = stats(gpuOwner, lease);
  require(integer(gpu, "texturePath") == multitap::RemoteEngine::kTexturePathGpuSurface, "The WARP start did not present GPU textures");
  const bool hardware = flag(gpu, "hardwareDecode");
  require(flag(gpu, "decoderPrestarted") == !hardware, "The prestart was used by the hardware decoder, or not by the CPU decoder");
  int64_t at = 0; uint64_t serial = 0; const auto latest = gpuOwner.Latest(at, serial);
  require(hardware || (latest && latest->rgba == reference.back().rgba), "The prestarted decoder's GPU picture differs");
  gpuOwner.Close();
  return std::string("{\"case\":\"sessions\",\"legacy\":true,\"held\":true,\"tracks\":2,\"gpuTexture\":true,\"hardwareDecoder\":") + (hardware ? "true" : "false") + "}";
}
#endif

double percentile(std::vector<double> values, double fraction) {
  std::sort(values.begin(), values.end());
  return values[std::max<size_t>(size_t(std::ceil(fraction * double(values.size()))), 1) - 1];
}
// stream info -> first decoded picture: the session's Start (what decoderMs
// spans), then the IDR and the following pictures pushed at once.
int benchmark(int argc, char** argv) {
  int load = 0, runs = 8, dial = 300; uint32_t width = 1920, height = 1080;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("load=", 0) == 0) load = std::clamp(atoi(a.c_str() + 5), 0, 64);
    else if (a.rfind("runs=", 0) == 0) runs = std::clamp(atoi(a.c_str() + 5), 1, 100);
    else if (a.rfind("dial=", 0) == 0) dial = std::clamp(atoi(a.c_str() + 5), 0, 10000);
    else if (a.rfind("size=", 0) == 0) { width = uint32_t(atoi(a.c_str() + 5)); height = uint32_t(atoi(a.c_str() + a.find('x') + 1)); }
    else throw std::runtime_error("Usage: --benchmark [load=N] [runs=N] [dial=MS] [size=WxH]");
  }
  // This PC may be someone's desktop: only Idle-class threads compete here.
  require(SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS) != 0, "Idle priority");
  const auto units = encode(width, height, 4);
  std::atomic<bool> stop{false}; std::vector<std::thread> burners;
  for (int i = 0; i < load; ++i) burners.emplace_back([&] { volatile uint64_t spin = 0; while (!stop) ++spin; });
  struct Stop { std::atomic<bool>& stop; std::vector<std::thread>& burners; ~Stop() { stop = true; for (auto& b : burners) b.join(); } } joiner{stop, burners};
  Registrar registrar; multitap::RemoteMediaSession owner(&registrar);
  std::map<std::string, std::vector<double>> start, first;
  int64_t lease = 0;
  const auto measure = [&](const char* mode, bool prepare) {
#ifndef MULTITAP_PRESTART_BASELINE
    // The session's dial and the Host's stream start, during which the prestart runs.
    if (prepare) owner.Prepare(1920, 1080, 60, 1);
#else
    require(!prepare, "The baseline has no prestart");
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(dial));
    int64_t texture = -1;
    const auto info = Clock::now();
    check(owner.Start(++lease, width, height, false, texture), "Start");
    const auto started = Clock::now();
    for (const auto& unit : units) push(owner, lease, legacy(unit));
    Clock::time_point picture;
    require(registrar.FirstPicture(texture, picture), "No first picture");
#ifndef MULTITAP_PRESTART_BASELINE
    require(flag(stats(owner, lease), "decoderPrestarted") == prepare, "Prestart use differs from the mode");
#endif
    start[mode].push_back(ms(info, started)); first[mode].push_back(ms(info, picture));
  };
  for (int run = 0; run < runs; ++run) {
    measure("fresh", false);
#ifndef MULTITAP_PRESTART_BASELINE
    measure("prestarted", true);
#endif
  }
  owner.Close();
  std::cout << "{\"kind\":\"decoder-prestart-benchmark\",\"size\":\"" << width << 'x' << height << "\",\"loadThreads\":" << load << ",\"cpus\":" << std::thread::hardware_concurrency()
            << ",\"runs\":" << runs << ",\"dialMs\":" << dial << ",\"priority\":\"idle\",\"modes\":{";
  bool comma = false;
  for (const auto& [mode, values] : first) {
    std::cout << (comma ? "," : "") << "\n  \"" << mode << "\":{\"startMs\":{\"p50\":" << percentile(start[mode], 0.5) << ",\"max\":" << percentile(start[mode], 1.0)
              << "},\"infoToFirstPictureMs\":{\"p50\":" << percentile(values, 0.5) << ",\"p90\":" << percentile(values, 0.9) << ",\"max\":" << percentile(values, 1.0) << "}}";
    comma = true;
  }
  std::cout << "\n}}" << std::endl;
#ifndef MULTITAP_PRESTART_BASELINE
  // Only meaningful while the decoder start is slow (a busy PC). A dial shorter
  // than the prestart's own start hides only part of it.
  const double fresh = percentile(first["fresh"], 0.5), prestarted = percentile(first["prestarted"], 0.5);
  if (load > 0) require(fresh >= 100 && prestarted * 2 < fresh, "The prestart did not take the decoder start out of the first picture (fresh p50 " + std::to_string(fresh) + " ms, prestarted p50 " + std::to_string(prestarted) + " ms)");
#endif
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
  int code = 0;
  try {
    check(MFStartup(MF_VERSION), "Media Foundation");
    if (argc > 1 && std::string(argv[1]) == "--benchmark") code = benchmark(argc, argv);
    else {
#ifndef MULTITAP_PRESTART_BASELINE
      const auto a = sameAsNewDecoder(), b = rulesKept(), c = sessions();
      std::cout << "{\"kind\":\"decoder-prestart\",\"checks\":" << checks << ",\"cases\":[" << a << ',' << b << ',' << c
                << "],\"actualWindowsCpuDecoder\":true,\"flutterRegistrarFixture\":true,\"gpuAdapter\":\"warp\",\"actualVm\":false}" << std::endl;
#else
      throw std::runtime_error("The baseline build only benchmarks");
#endif
    }
    MFShutdown();
  } catch (const std::exception& error) {
    std::cerr << "decoder prestart: " << error.what() << std::endl;
    code = 2;
  }
  CoUninitialize();
  return code;
}
