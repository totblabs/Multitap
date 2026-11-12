// tracks-v1 decode lanes without a VM, relay or Flutter: real H.264 from the
// Microsoft encoder MFT through the real RemoteTrackEngine (CPU decoder) and
// gpu::Converter on WARP.
//  - One output whose picture is held up (its texture notification blocks)
//    must not hold up the other output, and only its own lane fills.
//  - Audio never waits behind, or counts against, a full video lane.
//  - Two converters of one shared device converting at the same time (two
//    lanes on one GPU) produce exactly the CPU pixels.
// `--benchmark` decodes two 1920x1080 outputs as fast as they are accepted
// and prints the decoded rate (informational; this PC's CPU decoder, not the
// hardware decoder of a user's PC). It proves no presentation or VM behaviour.
#include "remote_track_engine.h"
#include "remote_media_session.h"
#include "gpu_video.h"
#include "../../../../native/windows_video/color.hpp"
#include <codecapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using V = flutter::EncodableValue;
namespace {
unsigned checks = 0;
void require(bool ok, const char* reason) { ++checks; if (!ok) throw std::runtime_error(reason); }
void check(HRESULT hr, const char* reason) { require(SUCCEEDED(hr), reason); }

// The Microsoft H.264 encoder MFT, set up like the Host's MFT backend: CBR, no
// B-frames, low latency, SPS/PPS in front of every IDR (the tracks gate needs them).
class H264 {
 public:
  H264(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate) : width_(width), height_(height), fps_(fps) {
    check(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_)), "H.264 encoder MFT");
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(mft_.As(&codec))) {
      set(codec.Get(), CODECAPI_AVLowLatencyMode, 1, true);
      set(codec.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
      set(codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
      set(codec.Get(), CODECAPI_AVEncMPVGOPSize, fps * 60);
    }
    codec_ = codec;
    ComPtr<IMFMediaType> output; check(MFCreateMediaType(&output), "Output type");
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, bitrate); MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, fps, 1); MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    check(mft_->SetOutputType(0, output.Get(), 0), "Set encoder output");
    ComPtr<IMFMediaType> input; check(MFCreateMediaType(&input), "Input type");
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height); MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1); input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    check(mft_->SetInputType(0, input.Get(), 0), "Set encoder input");
    check(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Encoder begin");
    check(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Encoder start");
  }
  ~H264() { if (mft_) mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0); }
  // One access unit per picture (low latency), with its key flag.
  std::vector<uint8_t> Encode(const std::vector<uint8_t>& nv12, uint64_t pts_us, bool& key) {
    if (!frames_++ && codec_) set(codec_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    ComPtr<IMFMediaBuffer> buffer; check(MFCreateMemoryBuffer(DWORD(nv12.size()), &buffer), "Raw buffer");
    BYTE* target = nullptr; check(buffer->Lock(&target, nullptr, nullptr), "Lock raw"); std::memcpy(target, nv12.data(), nv12.size()); buffer->Unlock(); buffer->SetCurrentLength(DWORD(nv12.size()));
    ComPtr<IMFSample> sample; check(MFCreateSample(&sample), "Raw sample"); sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(LONGLONG(pts_us) * 10); sample->SetSampleDuration(10000000 / fps_);
    check(mft_->ProcessInput(0, sample.Get(), 0), "Encode input");
    std::vector<uint8_t> unit;
    for (;;) {
      MFT_OUTPUT_STREAM_INFO info{}; check(mft_->GetOutputStreamInfo(0, &info), "Encoder output info");
      ComPtr<IMFSample> out; ComPtr<IMFMediaBuffer> memory;
      check(MFCreateSample(&out), "Encoded sample"); check(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 4 * 1024 * 1024), &memory), "Encoded buffer"); out->AddBuffer(memory.Get());
      MFT_OUTPUT_DATA_BUFFER data{}; data.pSample = out.Get(); DWORD status = 0;
      const HRESULT hr = mft_->ProcessOutput(0, 1, &data, &status);
      if (data.pEvents) data.pEvents->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
      check(hr, "Encode output");
      ComPtr<IMFMediaBuffer> bytes; check(out->ConvertToContiguousBuffer(&bytes), "Encoded bytes");
      BYTE* p = nullptr; DWORD length = 0; check(bytes->Lock(&p, nullptr, &length), "Lock encoded"); unit.insert(unit.end(), p, p + length); bytes->Unlock();
    }
    require(!unit.empty(), "The low-latency encoder held a picture back");
    key = nal(unit, 5);
    if (key && !(nal(unit, 7) && nal(unit, 8))) {
      if (headers_.empty()) headers();
      unit.insert(unit.begin(), headers_.begin(), headers_.end());
    }
    return unit;
  }

 private:
  static void set(ICodecAPI* codec, const GUID& key, ULONG value, bool boolean = false) {
    VARIANT v; VariantInit(&v); v.vt = VARTYPE(boolean ? VT_BOOL : VT_UI4);
    if (boolean) v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE; else v.ulVal = value;
    codec->SetValue(&key, &v); VariantClear(&v);
  }
  static bool nal(const std::vector<uint8_t>& bytes, uint8_t type) {
    for (size_t i = 0; i + 3 < bytes.size(); ++i) if (!bytes[i] && !bytes[i + 1] && bytes[i + 2] == 1 && (bytes[i + 3] & 31) == type) return true;
    return false;
  }
  void headers() {
    ComPtr<IMFMediaType> type; check(mft_->GetOutputCurrentType(0, &type), "Encoder current type");
    UINT32 length = 0; check(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &length), "Sequence header size");
    std::vector<uint8_t> blob(length); check(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), length, &length), "Sequence header");
    require(blob.size() > 4 && !blob[0] && !blob[1] && (blob[2] == 1 || (!blob[2] && blob[3] == 1)), "Sequence header is not Annex B");
    headers_ = std::move(blob);
  }
  ComPtr<IMFTransform> mft_;
  ComPtr<ICodecAPI> codec_;
  uint32_t width_, height_, fps_;
  uint64_t frames_ = 0;
  std::vector<uint8_t> headers_;
};

// A moving, textured picture (decoding it is real work, unlike a flat one).
std::vector<uint8_t> picture(uint32_t width, uint32_t height, unsigned frame, uint8_t tone) {
  std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2);
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
    nv12[size_t(y) * width + x] = uint8_t(tone + ((x * 7 + y * 3 + frame * 11) ^ (x * y + frame)) % 64);
  for (size_t i = size_t(width) * height; i < nv12.size(); ++i) nv12[i] = uint8_t(96 + (i * 5 + frame) % 64);
  return nv12;
}

// One output's packets: an IDR first, then P pictures.
std::vector<std::vector<uint8_t>> encodeTrack(uint8_t id, uint32_t topology, uint32_t epoch, uint32_t width, uint32_t height, unsigned count, uint32_t bitrate) {
  H264 encoder(width, height, 60, bitrate);
  std::vector<std::vector<uint8_t>> packets;
  for (unsigned n = 0; n < count; ++n) {
    bool key = false; const uint64_t pts = 1000 + uint64_t(n) * 16667;
    const auto unit = encoder.Encode(picture(width, height, n, id ? 140 : 30), pts, key);
    require(key == (n == 0), "Only the first picture is an IDR");
    multitap::tracks::Packet packet{multitap::tracks::video, uint8_t(key ? 1 : 0), id, topology, epoch, pts, unit.data(), unit.size()};
    std::vector<uint8_t> bytes; require(multitap::tracks::pack(packet, bytes), "Track packet"); packets.push_back(std::move(bytes));
  }
  return packets;
}
std::vector<uint8_t> audio(uint32_t topology, uint64_t pts) {
  const std::vector<uint8_t> pcm(1920, 0);
  multitap::tracks::Packet packet{multitap::tracks::audio, 0, multitap::tracks::audioTrack, topology, 0, pts, pcm.data(), pcm.size()};
  std::vector<uint8_t> bytes; require(multitap::tracks::pack(packet, bytes), "Audio packet"); return bytes;
}

// Flutter's registrar, as far as the engine uses it; `held` blocks the
// notification of one texture (that output's lane stops inside its decode).
struct Registrar final : flutter::TextureRegistrar {
  std::mutex mutex; std::condition_variable changed;
  std::map<int64_t, flutter::TextureVariant*> textures;
  int64_t next = 0, held = -1; bool holding = false, release = false;
  int64_t RegisterTexture(flutter::TextureVariant* texture) override { std::lock_guard<std::mutex> lock(mutex); textures.emplace(++next, texture); return next; }
  bool MarkTextureFrameAvailable(int64_t id) override {
    std::unique_lock<std::mutex> lock(mutex);
    if (id == held && !release) { holding = true; changed.notify_all(); changed.wait(lock, [&] { return release; }); }
    return textures.count(id) != 0;
  }
  void UnregisterTexture(int64_t id, std::function<void()> callback) override { { std::lock_guard<std::mutex> lock(mutex); textures.erase(id); } if (callback) callback(); }
  bool UnregisterTexture(int64_t id) override { UnregisterTexture(id, {}); return true; }
  bool WaitHolding() { std::unique_lock<std::mutex> lock(mutex); return changed.wait_for(lock, std::chrono::seconds(10), [&] { return holding; }); }
  void Release() { std::lock_guard<std::mutex> lock(mutex); release = true; changed.notify_all(); }
};

int64_t integer(const flutter::EncodableMap& map, const char* key) {
  const auto it = map.find(V(key)); require(it != map.end(), "Missing statistic");
  if (const auto* value = std::get_if<int64_t>(&it->second)) return *value;
  if (const auto* value = std::get_if<int32_t>(&it->second)) return *value;
  throw std::runtime_error("Statistic is not an integer");
}
bool flag(const flutter::EncodableMap& map, const char* key) {
  const auto it = map.find(V(key)); require(it != map.end() && std::holds_alternative<bool>(it->second), "Missing flag"); return std::get<bool>(it->second);
}
const flutter::EncodableMap& row(const flutter::EncodableMap& stats, uint8_t id) {
  const auto& rows = std::get<flutter::EncodableList>(stats.at(V("tracks")));
  for (const auto& value : rows) { const auto& r = std::get<flutter::EncodableMap>(value); if (std::get<std::string>(r.at(V("id"))) == std::to_string(id)) return r; }
  throw std::runtime_error("Missing output row");
}
template <class Predicate> bool until(Predicate predicate, std::chrono::milliseconds limit) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  return predicate();
}
// The relay reader's behaviour: a full lane is offered the same packet again.
bool offer(multitap::RemoteTrackEngine& engine, const std::vector<uint8_t>& bytes, std::chrono::milliseconds limit = std::chrono::milliseconds(10000)) {
  const auto end = std::chrono::steady_clock::now() + limit;
  for (;;) {
    const auto result = engine.Offer(bytes);
    if (result == multitap::MediaPushResult::accepted) return true;
    if (result != multitap::MediaPushResult::full || std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

multitap::tracks::Configuration outputs(uint32_t width, uint32_t height, bool sound) {
  multitap::tracks::Configuration value; value.topology = 1; value.audioEnabled = sound;
  value.tracks = {{0, 1, width, height, width, height, -int32_t(width), 0, true}, {1, 1, width, height, width, height, 0, 0, false}};
  return value;
}

void independentLanes() {
  constexpr unsigned count = 16;
  const auto first = encodeTrack(0, 1, 1, 640, 360, count, 2000000), second = encodeTrack(1, 1, 1, 640, 360, count, 2000000);
  Registrar registrar; multitap::RemoteTrackEngine engine(&registrar);
  // A failed check must not leave a lane held (Stop joins it).
  struct Unhold { Registrar& registrar; ~Unhold() { registrar.Release(); } } unhold{registrar};
  require(engine.Start(outputs(640, 360, true)), "Engine start");
  { std::lock_guard<std::mutex> lock(registrar.mutex); registrar.held = engine.Textures().at(0); }
  // Output 0's first picture is decoded, then its lane stops in the notification.
  require(engine.Offer(first[0]) == multitap::MediaPushResult::accepted && registrar.WaitHolding(), "Output 0 did not reach its held notification");
  for (unsigned n = 1; n <= multitap::RemoteTrackEngine::kLanePackets; ++n) require(engine.Offer(first[n]) == multitap::MediaPushResult::accepted, "Output 0's lane refused a bounded slot");
  require(engine.Offer(first[multitap::RemoteTrackEngine::kLanePackets + 1]) == multitap::MediaPushResult::full, "Output 0's lane took more than its bound");
  // Output 1 decodes every picture while output 0 is held up.
  for (unsigned n = 0; n < count; ++n) {
    require(offer(engine, second[n], std::chrono::milliseconds(3000)), "Output 1 waited behind output 0's held picture");
    require(until([&] { return integer(row(engine.Stats(), 1), "decodedFrames") == int64_t(n + 1); }, std::chrono::seconds(10)), "Output 1 stopped decoding while output 0 was held");
  }
  // Audio neither waits for a full lane nor plays before every output has a picture.
  require(engine.Offer(audio(1, 5)) == multitap::MediaPushResult::accepted, "Audio waited behind a full video lane");
  {
    const auto stats = engine.Stats();
    require(!flag(stats, "tracksReady") && integer(stats, "waitingPackets") == 1, "Audio played before output 0 had a picture");
    require(integer(row(stats, 0), "queuedPackets") == int64_t(multitap::RemoteTrackEngine::kLanePackets) && integer(row(stats, 0), "queueFullRefusals") == 1 &&
            integer(row(stats, 1), "queuedPackets") == 0 && integer(row(stats, 1), "queueFullRefusals") == 0, "Lane accounting crossed outputs");
    require(integer(stats, "queuedPackets") == int64_t(multitap::RemoteTrackEngine::kLanePackets), "Overall queue depth is not the fullest lane");
  }
  registrar.Release();
  for (unsigned n = multitap::RemoteTrackEngine::kLanePackets + 1; n < count; ++n) require(offer(engine, first[n]), "Output 0 did not resume");
  require(until([&] { const auto s = engine.Stats(); return integer(row(s, 0), "decodedFrames") == count && flag(s, "tracksReady"); }, std::chrono::seconds(10)), "Output 0 lost pictures after it resumed");
  require(engine.Offer(audio(1, 400000)) == multitap::MediaPushResult::accepted && integer(engine.Stats(), "audioReceivedFrames") == 480, "Audio did not play once both outputs had pictures");
  // The tracks gate's audio rules stay: a timestamp regression ends the stream.
  require(engine.Offer(audio(1, 400000)) == multitap::MediaPushResult::invalid && engine.Offer(second[0]) == multitap::MediaPushResult::stopped, "An audio timestamp regression was accepted");
  engine.Stop();
}

// Packets of an earlier topology are counted as stale without taking a slot;
// an output this topology does not have ends the stream (the gate's rules).
void topologyRules() {
  const auto stale = encodeTrack(0, 1, 1, 320, 180, 1, 500000), unknown = encodeTrack(0, 2, 1, 320, 180, 1, 500000);
  Registrar registrar; multitap::RemoteTrackEngine engine(&registrar);
  auto configuration = outputs(320, 180, false); configuration.topology = 2; configuration.tracks.erase(configuration.tracks.begin());
  require(engine.Start(configuration), "Sparse engine start");
  require(engine.Offer(stale[0]) == multitap::MediaPushResult::accepted && integer(engine.Stats(), "stalePackets") == 1, "An earlier topology was not stale");
  require(engine.Offer(unknown[0]) == multitap::MediaPushResult::invalid && FAILED(HRESULT(integer(engine.Stats(), "error"))), "An output outside the topology was accepted");
  engine.Stop();
}

// Two lanes converting on one shared device at the same time.
void sharedDeviceConversions() {
  HRESULT failure = S_OK;
  const auto device = multitap::gpu::Device::Create(nullptr, true, &failure);
  require(device != nullptr, "WARP device");
  constexpr uint32_t width = 192, height = 108; constexpr unsigned rounds = 300;
  std::array<std::vector<uint8_t>, 2> nv12{picture(width, height, 3, 30), picture(width, height, 9, 150)}, reference;
  for (unsigned id = 0; id < 2; ++id) { reference[id].resize(size_t(width) * height * 4); multitap::video::nv12ToRgba(nv12[id].data(), reference[id].data(), width, height, width, height, false, false); }
  require(reference[0] != reference[1], "The two outputs' pictures must differ");
  std::array<std::atomic<unsigned>, 2> wrong{}, done{};
  std::array<std::thread, 2> lanes;
  for (unsigned id = 0; id < 2; ++id) lanes[id] = std::thread([&, id] {
    multitap::gpu::Converter converter(device);
    for (unsigned n = 0; n < rounds; ++n) {
      std::shared_ptr<multitap::gpu::Frame> frame; std::vector<uint8_t> rgba;
      if (FAILED(converter.FromSystem(nv12[id].data(), width, height, width, height, width, height, multitap::gpu::Colour{}, frame)) || !frame || !frame->ReadRgba(rgba) || rgba != reference[id]) ++wrong[id];
      ++done[id];
    }
  });
  for (auto& lane : lanes) lane.join();
  require(done[0] == rounds && done[1] == rounds, "Conversions did not finish");
  require(wrong[0] == 0 && wrong[1] == 0, "Concurrent conversions on one device mixed the outputs' draws");
}

// A decoder/converter on a monitor GPU is not necessarily a shared Flutter
// surface. An unknown Flutter adapter must use an actual CPU pixel buffer.
// No windows, input injection or display topology changes are involved.
bool flutterPresentationStatistics() {
  ComPtr<IDXGIFactory1> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Presentation adapter enumeration");
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> candidate;
    if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
    if (multitap::gpu::Describe(candidate.Get()).software) continue;
    const auto device = multitap::gpu::Device::Create(candidate.Get());
    if (device && device->SupportsH264(640, 360)) { adapter = candidate; break; }
  }
  const auto packets = encodeTrack(0, 1, 1, 640, 360, 1, 2000000);
  auto verify = [&](multitap::RemoteTrackEngine::Options options, bool gpuPicture, bool flutterGpu) {
    Registrar registrar;
    multitap::RemoteTrackEngine engine(&registrar, {}, std::move(options));
    auto configuration = outputs(640, 360, false); configuration.tracks.pop_back();
    require(engine.Start(configuration), "Presentation statistics engine start");
    require(offer(engine, packets.front()), "Presentation statistics video offer");
    require(until([&] { return integer(engine.Stats(), "decodedFrames") == 1; }, std::chrono::seconds(10)), "Presentation statistics decode");
    multitap::RemoteTrackEngine::FrameSnapshot frame;
    require(engine.LatestFrame(0, 1, 1, frame) == S_OK && frame.pixels && bool(frame.pixels->gpu) == gpuPicture, "Decoded picture did not use the intended CPU/GPU conversion path");
    const auto stats = engine.Stats();
    require(integer(stats, "texturePath") == (flutterGpu ? 1 : 0), "GPU conversion was mistaken for Flutter GPU presentation");
    require(integer(row(stats, 0), "texturePath") == (flutterGpu ? 1 : 0) && flag(row(stats, 0), "flutterSurface") == flutterGpu, "Per-output path does not match Flutter texture kind");
    const auto texture = registrar.textures.at(engine.Textures().at(0));
    require(std::holds_alternative<flutter::GpuSurfaceTexture>(*texture) == flutterGpu, "Registered Flutter texture differs from the reported path");
    if (!flutterGpu) {
      const auto* pixels = std::get<flutter::PixelBufferTexture>(*texture).CopyPixelBuffer(640, 360);
      require(pixels && pixels->buffer && pixels->width == 640 && pixels->height == 360, "Cross-device pixel-buffer path did not read back the GPU picture");
      pixels->release_callback(pixels->release_context);
    }
    engine.Stop();
  };
  verify({}, false, false);
  multitap::RemoteTrackEngine::Options warp;
  warp.hardware_decode = warp.allow_software_render = warp.warp_render = true;
  verify(std::move(warp), true, true);
  if (!adapter) return false;  // CPU-only CI still checks real CPU/WARP textures.
  for (const bool knownFlutterAdapter : {false, true}) {
    multitap::RemoteTrackEngine::Options options;
    options.hardware_decode = true;
    options.output_adapters[0] = adapter;
    if (knownFlutterAdapter) options.render_adapter = adapter;
    verify(std::move(options), true, knownFlutterAdapter);
  }
  return true;
}

// Real H.264 through the session's UDP normalization and each output's gate.
void dualUdpAdmission() {
  const std::array<std::vector<std::vector<uint8_t>>,2> packets{
    encodeTrack(0,1,1,640,360,3,2000000),encodeTrack(1,1,1,640,360,3,2000000)};
  Registrar registrar;multitap::RemoteMediaSession session(&registrar);
  std::map<uint8_t,int64_t> textures;
  require(session.StartTracks(1,outputs(640,360,false),textures)==S_OK,"UDP track decoders start");
  std::array<std::shared_ptr<multitap::UdpPathGate>,2> gates;
  for(uint8_t id=0;id<2;++id){gates[id]=session.TrackPathGate({id,1,1,id==0});require(bool(gates[id]),"Output receive gate");gates[id]->Stream(1,1);gates[id]->Engage(false);}
  for(unsigned frame=0;frame<3;++frame)for(uint8_t id=0;id<2;++id){
    std::vector<uint8_t> normalized;
    require(multitap::udp::trackToUdp({id,1,1,id==0},packets[id][frame],normalized),"Normalize real video");
    const uint64_t pts=1000+uint64_t(frame)*16667;
    require(gates[id]->OfferUdp(normalized,true,frame==0,1,pts)==multitap::UdpPathGate::Delivery::accepted,"Epoch stripped by gate must restore into exact TVT1 output");
  }
  require(until([&]{flutter::EncodableMap stats;return session.Stats(1,stats)==S_OK&&integer(stats,"decodedFrames")==6&&flag(stats,"tracksReady");},std::chrono::seconds(10)),"Both UDP outputs decoded real frames");
  // One output falls back on WSS while the other remains on UDP.
  auto fallback=packets[0][0];multitap::tracks::write64(fallback.data()+16,1000000);
  require(session.Push(1,fallback)==S_OK&&gates[0]->view().active==0&&gates[1]->view().active==1,"WSS and UDP must share each output's path rule");
  require(until([&]{flutter::EncodableMap stats;return session.Stats(1,stats)==S_OK&&integer(stats,"decodedFrames")==7;},std::chrono::seconds(10)),"WSS fallback decoded");
  auto retired=gates[0];gates[0]=session.TrackPathGate({0,1,1,true});
  require(gates[0]&&gates[0]!=retired&&!retired->attached(),"Regrant cannot reuse the old receive gate");
  gates[0]->Stream(1,2);gates[0]->Engage(true);
  auto fresh=packets[0][0];multitap::tracks::write64(fresh.data()+16,2000000);
  std::vector<uint8_t> normalized;require(multitap::udp::trackToUdp({0,1,1,true},fresh,normalized),"Normalize regrant key");
  require(gates[0]->OfferUdp(normalized,true,true,1,2000000)==multitap::UdpPathGate::Delivery::accepted,"Fresh regrant key accepted");
  require(until([&]{flutter::EncodableMap stats;return session.Stats(1,stats)==S_OK&&integer(stats,"decodedFrames")==8;},std::chrono::seconds(10)),"Regrant decoded with continuous real PTS");
  require(session.Stop(1)==S_OK&&!gates[0]->attached()&&!gates[1]->attached(),"Both sockets detach before decoder stop");
}

// Two 1080p outputs, every picture offered as soon as its lane takes it.
void benchmark() {
  constexpr unsigned count = 180; constexpr uint32_t width = 1920, height = 1080;
  std::cerr << "encoding " << count << " pictures per output...\n";
  const std::array<std::vector<std::vector<uint8_t>>, 2> packets{encodeTrack(0, 1, 1, width, height, count, 12000000), encodeTrack(1, 1, 1, width, height, count, 12000000)};
  for (const unsigned outputs_used : {1u, 2u}) {
    Registrar registrar; multitap::RemoteTrackEngine engine(&registrar);
    auto configuration = outputs(width, height, false); if (outputs_used == 1) configuration.tracks.pop_back();
    require(engine.Start(configuration), "Benchmark engine start");
    const auto began = std::chrono::steady_clock::now();
    for (unsigned n = 0; n < count; ++n) for (unsigned id = 0; id < outputs_used; ++id) require(offer(engine, packets[id][n], std::chrono::seconds(30)), "Benchmark packet");
    require(until([&] { return integer(engine.Stats(), "decodedFrames") == int64_t(count * outputs_used); }, std::chrono::seconds(60)), "Benchmark decode");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    const auto stats = engine.Stats();
    std::cout << "{\"kind\":\"track-lanes-benchmark\",\"outputs\":" << outputs_used << ",\"size\":\"1920x1080\",\"decoder\":\"software-mft\",\"pictures\":" << count * outputs_used
              << ",\"seconds\":" << seconds << ",\"decodedFps\":" << count * outputs_used / seconds << ",\"perOutputFps\":" << count / seconds
              << ",\"decodeMsPerCall\":" << std::get<double>(stats.at(V("decodeTotalMs"))) / double(count * outputs_used) << "}" << std::endl;
    engine.Stop();
  }
}
}  // namespace

int main(int argc, char** argv) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com) || FAILED(MFStartup(MF_VERSION))) { std::cerr << "COM/MF startup failed\n"; return 2; }
  int code = 0;
  try {
    if (argc == 2 && std::string(argv[1]) == "--benchmark") benchmark();
    else {
      independentLanes(); topologyRules(); sharedDeviceConversions();dualUdpAdmission();
      const bool hardwarePresentation = flutterPresentationStatistics();
      std::cout << "{\"kind\":\"flutter-presentation-statistics\",\"hardwareCaseVerified\":" << (hardwarePresentation ? "true" : "false") << ",\"physicalPresentation\":false}" << std::endl;
      std::cout << "{\"kind\":\"track-lanes-smoke\",\"checks\":" << checks << ",\"independentLanes\":true,\"audioOutsideVideoLanes\":true,\"sharedDeviceConversions\":true,\"dualUdpDecoded\":true,\"mixedWssUdp\":true,\"hardwareDecoder\":false,\"presentation\":false}" << std::endl;
    }
  } catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << std::endl; code = 1; }
  MFShutdown(); CoUninitialize();
  return code;
}
