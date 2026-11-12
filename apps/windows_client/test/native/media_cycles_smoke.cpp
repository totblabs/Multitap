// Long-session resource bounds (backlog #60): the real RemoteMediaSession,
// RemoteEngine, RemoteTrackEngine and Windows CPU H.264 decoder driven through
// many reconnects, viewport resizes with a held picture, GPU (WARP) resizes,
// two-output starts and decoder prestarts, fed with H.264 from the Microsoft
// encoder MFT. A Flutter registrar fixture takes and releases every frame the
// way Flutter's raster thread does. After a warm-up the process's handles,
// threads, GDI/USER objects and private bytes are read before and after each
// phase; a phase fails when they grow past a fixed allowance. No VM, relay,
// display, audio device or input is used. `cycles=N` (default 200) per phase;
// `trace` prints private bytes every 100 cycles (a leak climbs, a pool plateaus).
#include "remote_media_session.h"
#include <codecapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <wmcodecdsp.h>
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
void require(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error(reason); }
void check(HRESULT hr, const std::string& reason) { require(SUCCEEDED(hr), reason + " (" + std::to_string(uint32_t(hr)) + ")"); }

// The Microsoft H.264 encoder MFT (as decoder_prestart_smoke): IDR first, no B-frames.
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
  output->SetUINT32(MF_MT_AVG_BITRATE, 2000000); MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height);
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
      check(MFCreateSample(&out), "Encoded sample"); check(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 4 * 1024 * 1024), &memory), "Encoded buffer"); out->AddBuffer(memory.Get());
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
std::vector<uint8_t> legacy(const Unit& unit) {
  std::vector<uint8_t> media(12 + unit.bytes.size());
  media[0] = 1; media[1] = unit.key ? 1 : 0; std::memcpy(media.data() + 4, &unit.pts, 8);
  std::copy(unit.bytes.begin(), unit.bytes.end(), media.begin() + 12);
  return media;
}
std::vector<uint8_t> track(const Unit& unit, uint8_t id, uint32_t topology) {
  multitap::tracks::Packet packet{multitap::tracks::video, uint8_t(unit.key ? 1 : 0), id, topology, 1, unit.pts, unit.bytes.data(), unit.bytes.size()};
  std::vector<uint8_t> bytes; require(multitap::tracks::pack(packet, bytes), "Track packet"); return bytes;
}

// Flutter's registrar as the engines use it: every frame marked available is
// taken and released at once (the raster thread's upload), CPU or GPU texture.
struct Registrar final : flutter::TextureRegistrar {
  std::mutex mutex;
  std::map<int64_t, flutter::TextureVariant*> textures;
  int64_t next = 0;
  uint64_t taken = 0;
  int64_t RegisterTexture(flutter::TextureVariant* texture) override { std::lock_guard<std::mutex> lock(mutex); textures.emplace(++next, texture); return next; }
  bool MarkTextureFrameAvailable(int64_t id) override {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = textures.find(id);
    if (it == textures.end()) return false;
    if (auto* pixels = std::get_if<flutter::PixelBufferTexture>(it->second)) {
      if (const auto* buffer = pixels->CopyPixelBuffer(0, 0)) { ++taken; buffer->release_callback(buffer->release_context); }
    } else if (auto* surface = std::get_if<flutter::GpuSurfaceTexture>(it->second)) {
      if (const auto* descriptor = surface->ObtainDescriptor(0, 0)) { ++taken; if (descriptor->release_callback) descriptor->release_callback(descriptor->release_context); }
    }
    return true;
  }
  void UnregisterTexture(int64_t id, std::function<void()> callback) override { { std::lock_guard<std::mutex> lock(mutex); textures.erase(id); } if (callback) callback(); }
  bool UnregisterTexture(int64_t id) override { UnregisterTexture(id, {}); return true; }
  size_t Registered() { std::lock_guard<std::mutex> lock(mutex); return textures.size(); }
};

int64_t integer(const flutter::EncodableMap& map, const char* key) {
  const auto it = map.find(V(key)); require(it != map.end(), std::string("Missing statistic ") + key);
  if (const auto* value = std::get_if<int64_t>(&it->second)) return *value;
  if (const auto* value = std::get_if<int32_t>(&it->second)) return *value;
  throw std::runtime_error("Statistic is not an integer");
}
flutter::EncodableMap stats(multitap::RemoteMediaSession& owner, int64_t lease) { flutter::EncodableMap value; check(owner.Stats(lease, value), "Stats"); return value; }
void push(multitap::RemoteMediaSession& owner, int64_t lease, const std::vector<uint8_t>& bytes) {
  const auto end = Clock::now() + std::chrono::seconds(60);
  for (;;) {
    const HRESULT hr = owner.Push(lease, bytes);
    if (hr == S_OK) return;
    require(hr == multitap::kMediaQueueFull && Clock::now() < end, "Packet rejected");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
void decoded(multitap::RemoteMediaSession& owner, int64_t lease, int64_t frames) {
  const auto end = Clock::now() + std::chrono::seconds(60);
  while (Clock::now() < end) { if (integer(stats(owner, lease), "decodedFrames") >= frames) return; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  throw std::runtime_error("Frames were not decoded");
}

struct Usage { int64_t handles = 0, gdi = 0, user = 0, threads = 0, privateBytes = 0; };
Usage now() {
  Usage value;
  DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles); value.handles = handles;
  value.gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS); value.user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
      if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++value.threads;
    CloseHandle(snapshot);
  }
  PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
  value.privateBytes = int64_t(memory.PrivateUsage);
  return value;
}
// Detached prestart threads and Media Foundation's own pool settle a little after a stop.
Usage settled() {
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  return now();
}

// Allowances over a whole phase, whatever its cycle count: a resource leaked once
// per cycle (an engine, decoder, thread, texture or frame buffer) exceeds them.
constexpr int64_t kHandles = 64, kThreads = 8, kGui = 8, kPrivateBytes = 48ll * 1024 * 1024;
bool trace = false;

void phase(const char* name, int cycles, const std::function<void(int)>& cycle) {
  for (int i = 0; i < 8; ++i) cycle(i);  // warm-up: pools, caches and MF worker threads
  const auto before = settled();
  const auto began = Clock::now();
  for (int i = 0; i < cycles; ++i) {
    cycle(8 + i);
    if (trace && (i + 1) % 100 == 0) std::cout << "  " << name << " " << (i + 1) << " privateMB=" << now().privateBytes / 1048576.0 << std::endl;
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - began).count();
  const auto after = settled();
  const auto delta = [&](int64_t Usage::*field) { return after.*field - before.*field; };
  std::string json = std::string("{\"phase\":\"") + name + "\",\"cycles\":" + std::to_string(cycles) + ",\"seconds\":" + std::to_string(seconds) +
      ",\"handles\":[" + std::to_string(before.handles) + "," + std::to_string(after.handles) + "],\"threads\":[" + std::to_string(before.threads) + "," + std::to_string(after.threads) +
      "],\"gdi\":[" + std::to_string(before.gdi) + "," + std::to_string(after.gdi) + "],\"user\":[" + std::to_string(before.user) + "," + std::to_string(after.user) +
      "],\"privateMB\":[" + std::to_string(before.privateBytes / 1048576.0) + "," + std::to_string(after.privateBytes / 1048576.0) + "]}";
  std::cout << json << std::endl;
  require(delta(&Usage::handles) <= kHandles, std::string(name) + ": handles grew");
  require(delta(&Usage::threads) <= kThreads, std::string(name) + ": threads grew");
  require(delta(&Usage::gdi) <= kGui && delta(&Usage::user) <= kGui, std::string(name) + ": GDI/USER objects grew");
  require(delta(&Usage::privateBytes) <= kPrivateBytes, std::string(name) + ": private bytes grew");
}
}  // namespace

int main(int argc, char** argv) {
  // This PC may be someone's desktop.
  SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
  int cycles = 200;
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "cycles=", 7) == 0) cycles = std::max(1, std::atoi(argv[i] + 7));
    if (std::strcmp(argv[i], "trace") == 0) trace = true;
  }
  int code = 0;
  try {
    check(MFStartup(MF_VERSION), "Media Foundation");
    const auto low = encode(640, 360, 4), high = encode(800, 448, 4);
    Registrar registrar;
    int64_t lease = 0;
    {
      // Reconnect: every stream ends and the next starts on a new lease.
      multitap::RemoteMediaSession owner(&registrar);
      phase("reconnect", cycles, [&](int i) {
        const auto& units = i % 2 ? high : low;
        int64_t texture = -1;
        check(owner.Start(++lease, i % 2 ? 800 : 640, i % 2 ? 448 : 360, false, texture), "Start");
        for (const auto& unit : units) push(owner, lease, legacy(unit));
        decoded(owner, lease, int64_t(units.size()));
        check(owner.Stop(lease), "Stop");
        require(registrar.Registered() == 0, "A stopped stream kept its texture");
      });
      // Viewport resize: prestart, the new size beside the held picture, then its release.
      int64_t texture = -1;
      check(owner.Start(++lease, 640, 360, false, texture), "First stream");
      for (const auto& unit : low) push(owner, lease, legacy(unit));
      decoded(owner, lease, int64_t(low.size()));
      phase("resizeHeld", cycles, [&](int i) {
        const bool big = i % 2 == 0;
        const auto& units = big ? high : low;
        const int64_t held = texture;
        owner.Prepare(big ? 800 : 640, big ? 448 : 360, 60, 1);
        check(owner.Start(++lease, big ? 800 : 640, big ? 448 : 360, false, texture, {}, false, 60, 0, 0, true), "Held start");
        require(owner.HeldTexture() == held, "The previous picture was not held");
        for (const auto& unit : units) push(owner, lease, legacy(unit));
        decoded(owner, lease, int64_t(units.size()));
        require(owner.ReleaseHeld(held) && registrar.Registered() == 1, "The held picture was not released");
      });
      owner.Close();
      require(registrar.Registered() == 0, "Close kept a texture");
    }
    {
      // GPU presentation (WARP): shared textures, pools and converters per size.
      multitap::RemoteMediaSession owner(&registrar); owner.AllowSoftwareRenderForTests(true);
      int64_t texture = -1;
      check(owner.Start(++lease, 640, 360, false, texture, {}, true), "GPU stream");
      for (const auto& unit : low) push(owner, lease, legacy(unit));
      decoded(owner, lease, int64_t(low.size()));
      require(integer(stats(owner, lease), "texturePath") == multitap::RemoteEngine::kTexturePathGpuSurface, "Not on GPU textures");
      phase("gpuResizeHeld", cycles, [&](int i) {
        const bool big = i % 2 == 0;
        const auto& units = big ? high : low;
        const int64_t held = texture;
        check(owner.Start(++lease, big ? 800 : 640, big ? 448 : 360, false, texture, {}, true, 60, 0, 0, true), "GPU held start");
        for (const auto& unit : units) push(owner, lease, legacy(unit));
        decoded(owner, lease, int64_t(units.size()));
        // Thumbnail read-back of a GPU frame (staging texture) once per stream.
        int64_t at = 0; uint64_t serial = 0; require(owner.Latest(at, serial) != nullptr, "No latest frame");
        require(owner.ReleaseHeld(held), "The held GPU picture was not released");
      });
      owner.Close();
      require(registrar.Registered() == 0, "GPU close kept a texture");
    }
    {
      // Two outputs (tracks-v1): two lanes, decoders and textures per start.
      multitap::RemoteMediaSession owner(&registrar);
      phase("tracks", std::max(1, cycles / 2), [&](int i) {
        multitap::tracks::Configuration configuration; configuration.topology = uint32_t(i + 1);
        configuration.tracks = {{0, 1, 640, 360, 640, 360, -640, 0, true}, {1, 1, 640, 360, 640, 360, 0, 0, false}};
        std::map<uint8_t, int64_t> textures;
        owner.Prepare(640, 360, 60, 2);
        check(owner.StartTracks(++lease, configuration, textures), "Track start");
        for (const auto& unit : low) { push(owner, lease, track(unit, 0, configuration.topology)); push(owner, lease, track(unit, 1, configuration.topology)); }
        decoded(owner, lease, 2 * int64_t(low.size()));
      });
      owner.Close();
      require(registrar.Registered() == 0, "Track close kept a texture");
    }
    {
      // Prestart hints that no stream takes (a session that keeps asking).
      multitap::RemoteMediaSession owner(&registrar);
      phase("prestartUnused", cycles, [&](int) { owner.Prepare(1920, 1080, 60, 2); std::this_thread::sleep_for(std::chrono::milliseconds(5)); });
      owner.Close();
    }
    MFShutdown();
    std::cout << "{\"kind\":\"media-cycles\",\"cycles\":" << cycles << ",\"framesTaken\":" << registrar.taken
              << ",\"actualWindowsCpuDecoder\":true,\"gpuAdapter\":\"warp\",\"flutterRegistrarFixture\":true,\"actualVm\":false}" << std::endl;
  } catch (const std::exception& error) {
    std::cerr << "media cycles: " << error.what() << std::endl;
    code = 2;
  }
  CoUninitialize();
  return code;
}
