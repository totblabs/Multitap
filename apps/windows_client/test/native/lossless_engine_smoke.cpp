// Lossless tiles on the single-output engine (RemoteMediaSession / RemoteEngine)
// with this PC's real GPU: the hardware H.264 decoder where there is one and
// shared GPU textures for Flutter (texturePath 1), as a Windows client streams
// one monitor. Field case (2026-11-19, Windows app 1.1.32, one 1920x1080
// output on an NVIDIA vGPU): the Host reported every tile present, yet the
// client's picture of a still page had the 4:2:0 colour bleed of the video on
// every coloured element. H.264 comes from the Microsoft encoder MFT; tiles are
// a coloured pattern like a web page's label (orange, white strokes). The
// published picture is read back (RemoteEngine::Latest) and compared exactly
// with what the CPU compositor makes of the same packets. The Flutter registrar
// is a fixture; nothing is shown.
#include "remote_media_session.h"
#include "../../../../native/windows_video/lossless_tiles.hpp"
#include <codecapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <algorithm>
#include <chrono>
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

struct Unit { std::vector<uint8_t> bytes; uint64_t pts = 0; bool key = false; };
// The Microsoft H.264 encoder MFT (as decoder_prestart_smoke): CBR, no B-frames,
// low latency, SPS/PPS before the IDR. A textured picture that changes per frame.
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
  check(mft->SetOutputType(0, output.Get(), 0), "Encoder output type");
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
// The legacy single-output media-v1 envelope: kind 1, key flag, pts, payload.
std::vector<uint8_t> envelope(const uint8_t* bytes, size_t size, uint64_t pts, bool key) {
  std::vector<uint8_t> media(12 + size);
  media[0] = 1; media[1] = key ? 1 : 0; std::memcpy(media.data() + 4, &pts, 8);
  std::copy(bytes, bytes + size, media.begin() + 12);
  return media;
}
struct Registrar final : flutter::TextureRegistrar {
  std::mutex mutex; std::map<int64_t, flutter::TextureVariant*> textures; int64_t next = 0;
  int64_t RegisterTexture(flutter::TextureVariant* texture) override { std::lock_guard<std::mutex> lock(mutex); textures.emplace(++next, texture); return next; }
  bool MarkTextureFrameAvailable(int64_t id) override { std::lock_guard<std::mutex> lock(mutex); return textures.count(id) != 0; }
  void UnregisterTexture(int64_t id, std::function<void()> callback) override { { std::lock_guard<std::mutex> lock(mutex); textures.erase(id); } if (callback) callback(); }
  bool UnregisterTexture(int64_t id) override { UnregisterTexture(id, {}); return true; }
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
template <class F> bool until(F done, std::chrono::milliseconds limit = std::chrono::milliseconds(20000)) {
  const auto end = Clock::now() + limit;
  while (Clock::now() < end) { if (done()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
  return done();
}

// A web page's coloured label: orange ground, white strokes, a blue line.
std::vector<uint8_t> label(uint32_t width, uint32_t height) {
  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
    const bool stroke = (x % 7 == 2 || y % 9 == 4) && (x / 32 + y / 32) % 2 == 0;
    const bool line = y % 64 == 50;
    if (stroke) { p[0] = 255; p[1] = 255; p[2] = 255; }
    else if (line) { p[0] = 52; p[1] = 92; p[2] = 160; }
    else { p[0] = 252; p[1] = 131; p[2] = 2; }
    p[3] = 255;
  }
  return rgba;
}

struct Result { std::string json; };
// `tiles`: tile indices sent as one packet (pts one before the next picture).
std::string run(uint32_t width, uint32_t height, bool hardware) {
  const auto units = encode(width, height, 3);
  const auto source = label(width, height);
  const uint32_t columns = multitap::lossless::tileColumns(width), rows = multitap::lossless::tileRows(height);
  std::vector<uint16_t> tiles;
  for (uint32_t row = 1; row < std::min<uint32_t>(rows, 4); ++row) for (uint32_t column = 1; column < std::min<uint32_t>(columns, 6); ++column) tiles.push_back(uint16_t(row * columns + column));
  multitap::lossless::PayloadBuilder builder(width, height, 1, multitap::lossless::noDrop, {});
  for (const auto index : tiles) { const auto r = multitap::lossless::tileRect(index, width, height); builder.add(index, &source[(size_t(r.y) * width + r.x) * 4], size_t(width) * 4); }
  const auto payload = builder.finish();
  // What the CPU compositor makes of the same packet: the exact tile pixels.
  multitap::lossless::Compositor reference; reference.Configure(width, height, true);
  require(reference.Accept(payload.data(), payload.size()) == multitap::lossless::Compositor::Outcome::accepted && reference.presentTiles() == tiles.size(), "Reference compositor");

  // As the app: Flutter's render adapter (here the first hardware adapter of
  // this PC) decides whether pictures become shared GPU textures.
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (hardware) {
    ComPtr<IDXGIFactory1> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<IDXGIAdapter1> candidate;
    for (UINT i = 0; factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; ++i, candidate.Reset()) {
      DXGI_ADAPTER_DESC1 desc{}; candidate->GetDesc1(&desc);
      if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId != 0x1414) { adapter = candidate; break; }
    }
    require(bool(adapter), "No hardware adapter on this PC");
  }
  Registrar registrar; multitap::RemoteMediaSession owner(&registrar, {}, adapter);
  int64_t lease = 1, texture = -1;
  check(owner.Start(lease, width, height, false, texture, {}, hardware), "Start");
  push(owner, lease, envelope(units[0].bytes.data(), units[0].bytes.size(), units[0].pts, true));
  require(until([&] { return integer(stats(owner, lease), "decodedFrames") >= 1; }), "First picture");
  const auto first = stats(owner, lease);
  const int64_t path = integer(first, "texturePath");
  const bool hw = flag(first, "hardwareDecode");
  // Differences of the published picture from the tile pixels, over the tiles.
  const auto mismatches = [&](uint64_t& bled) {
    int64_t at = 0; uint64_t serial = 0;
    const auto latest = owner.Latest(at, serial);
    require(latest && latest->width == width && latest->height == height && latest->rgba.size() == size_t(width) * height * 4, "Published picture readback");
    uint64_t wrong = 0; bled = 0;
    for (const auto index : tiles) {
      const auto r = multitap::lossless::tileRect(index, width, height);
      const uint8_t* want = reference.tilePixels(index);
      for (uint32_t y = 0; y < r.height; ++y) for (uint32_t x = 0; x < r.width; ++x) {
        const uint8_t* g = &latest->rgba[(size_t(r.y + y) * width + r.x + x) * 4];
        const uint8_t* w = want + size_t(y) * width * 4 + size_t(x) * 4;
        if (g[0] != w[0] || g[1] != w[1] || g[2] != w[2]) { ++wrong; if (std::abs(int(g[0]) - w[0]) + std::abs(int(g[1]) - w[1]) + std::abs(int(g[2]) - w[2]) > 60) ++bled; }
      }
    }
    return wrong;
  };
  // 1) The tile packet alone: republished over the newest picture.
  const auto republishes = integer(stats(owner, lease), "losslessRepublishes");
  push(owner, lease, envelope(payload.data(), payload.size(), units[1].pts - 1, false));
  require(until([&] { return integer(stats(owner, lease), "losslessRepublishes") > republishes; }), "The tile packet was not republished");
  uint64_t bledAlone = 0; const auto wrongAlone = mismatches(bledAlone);
  // 2) The next pictures: every present tile over each decoded picture.
  for (unsigned n = 1; n < units.size(); ++n) push(owner, lease, envelope(units[n].bytes.data(), units[n].bytes.size(), units[n].pts, false));
  require(until([&] { return integer(stats(owner, lease), "decodedFrames") >= int64_t(units.size()); }), "Later pictures");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint64_t bledVideo = 0; const auto wrongVideo = mismatches(bledVideo);
  const auto last = stats(owner, lease);
  const auto present = integer(last, "losslessPresentTiles"); const bool poisoned = flag(last, "losslessPoisoned");
  // 3) tiles-resync-v1. A tile packet lost on the way (sequence 2 never comes):
  // packet 3 poisons, the next picture is plain video. The Host's new source
  // (packet 1 again, asked for by the client) restarts the compositor: exact again.
  const auto packet = [&](uint32_t sequence) {
    multitap::lossless::PayloadBuilder next(width, height, sequence, multitap::lossless::noDrop, {});
    for (const auto index : tiles) { const auto r = multitap::lossless::tileRect(index, width, height); next.add(index, &source[(size_t(r.y) * width + r.x) * 4], size_t(width) * 4); }
    return next.finish();
  };
  const auto more = encode(width, height, 6);
  uint64_t at = units.back().pts;
  const auto picture = [&](unsigned n) { at += 16667; return envelope(more[n].bytes.data(), more[n].bytes.size(), at, more[n].key); };
  push(owner, lease, picture(0));  // an IDR of the second encoding: the decoder goes on from it
  const auto gap = packet(3);
  push(owner, lease, envelope(gap.data(), gap.size(), at + 16666, false));
  push(owner, lease, picture(1));
  require(until([&] { return flag(stats(owner, lease), "losslessPoisoned"); }), "A sequence gap did not poison the tiles");
  require(until([&] { return integer(stats(owner, lease), "decodedFrames") >= int64_t(units.size() + 2); }), "Picture after the gap");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint64_t bledPoisoned = 0; const auto wrongPoisoned = mismatches(bledPoisoned);
  require(wrongPoisoned > 0 && integer(stats(owner, lease), "losslessRestarts") == 0, "Poisoned tiles still showed");
  const auto restart = packet(1);
  push(owner, lease, envelope(restart.data(), restart.size(), at + 16666, false));
  push(owner, lease, picture(2));
  require(until([&] { const auto s = stats(owner, lease); return !flag(s, "losslessPoisoned") && integer(s, "losslessRestarts") == 1 && integer(s, "decodedFrames") >= int64_t(units.size() + 3); }), "The restarted source was not taken");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint64_t bledRestarted = 0; const auto wrongRestarted = mismatches(bledRestarted);
  // The source goes on from its packet 1: packet 2 is no gap.
  const auto second = packet(2);
  push(owner, lease, envelope(second.data(), second.size(), at + 16666, false));
  push(owner, lease, picture(3));
  require(until([&] { return integer(stats(owner, lease), "decodedFrames") >= int64_t(units.size() + 4); }), "Picture after the restarted source's packet 2");
  const auto after = stats(owner, lease);
  owner.Close();
  std::cout << "{\"size\":\"" << width << "x" << height << "\",\"case\":\"resync\",\"wrongPoisoned\":" << wrongPoisoned << ",\"wrongRestarted\":" << wrongRestarted << ",\"poisonedAfterPacket2\":" << (flag(after, "losslessPoisoned") ? "true" : "false") << "}" << std::endl;
  require(!wrongRestarted, "The tiles of the restarted source are not exact");
  require(!flag(after, "losslessPoisoned") && integer(after, "losslessRestarts") == 1, "The restarted source's packet 2 poisoned or restarted again");
  const std::string json = "{\"size\":\"" + std::to_string(width) + "x" + std::to_string(height) + "\",\"hardwareRequested\":" + (hardware ? "true" : "false") +
      ",\"hardwareDecode\":" + (hw ? "true" : "false") + ",\"texturePath\":" + std::to_string(path) + ",\"tiles\":" + std::to_string(tiles.size()) +
      ",\"presentTiles\":" + std::to_string(present) + ",\"poisoned\":" + (poisoned ? "true" : "false") +
      ",\"wrongAfterTiles\":" + std::to_string(wrongAlone) + ",\"bledAfterTiles\":" + std::to_string(bledAlone) +
      ",\"wrongAfterVideo\":" + std::to_string(wrongVideo) + ",\"bledAfterVideo\":" + std::to_string(bledVideo) + "}";
  std::cout << json << std::endl;
  require(present == int64_t(tiles.size()) && !poisoned, "The engine does not hold the tiles");
  require(!wrongAlone, "The tiles alone are not exact in the published picture");
  require(!wrongVideo, "The tiles are not exact over a decoded picture");
  return json;
}
}  // namespace

int main() {
  try {
    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM");
    check(MFStartup(MF_VERSION), "Media Foundation");
    int failures = 0;
    for (const bool hardware : {false, true}) for (const auto size : {std::pair<uint32_t, uint32_t>{640, 360}, {1920, 1080}}) {
      try { run(size.first, size.second, hardware); }
      catch (const std::exception& error) { ++failures; std::cout << "{\"size\":\"" << size.first << "x" << size.second << "\",\"hardwareRequested\":" << (hardware ? "true" : "false") << ",\"failed\":\"" << error.what() << "\"}" << std::endl; }
    }
    std::cout << "{\"kind\":\"lossless-engine-smoke\",\"checks\":" << checks << ",\"failures\":" << failures << "}" << std::endl;
    MFShutdown();
    return failures ? 1 : 0;
  } catch (const std::exception& error) { std::cerr << "lossless engine smoke failed: " << error.what() << std::endl; return 1; }
}
