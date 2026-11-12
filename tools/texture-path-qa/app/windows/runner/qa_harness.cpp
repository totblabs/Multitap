#include "qa_harness.h"
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <timeapi.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace {
using V = flutter::EncodableValue;
using M = flutter::EncodableMap;
void check(HRESULT hr, const char* what) { if (FAILED(hr)) throw std::runtime_error(what); }
void ui4(ICodecAPI* api, const GUID& key, ULONG value) {
  VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = value; api->SetValue(&key, &v);
}
uint32_t hash(uint32_t x, uint32_t y) {
  uint32_t h = x * 374761393u + y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16);
}
// Desktop-like synthetic content: static chrome and text rows, a map-like
// region panning 6x2 px per frame, and a window moving across it.
void paint(std::vector<uint8_t>& nv12, uint32_t w, uint32_t h, int n) {
  uint8_t* y = nv12.data(); uint8_t* uv = y + size_t(w) * h;
  const uint32_t mx0 = w / 8, my0 = h / 6, mx1 = w - w / 8, my1 = h - h / 8;
  const uint32_t bx = (uint32_t(n) * 9) % (w / 2), by = h / 4 + (uint32_t(n) * 3) % (h / 3);
  for (uint32_t r = 0; r < h; ++r) {
    uint8_t* row = y + size_t(r) * w;
    for (uint32_t c = 0; c < w; ++c) {
      uint8_t v;
      if (r < 28) v = 40;                                                  // title bar
      else if (c >= mx0 && c < mx1 && r >= my0 && r < my1) {
        const uint32_t sx = c + uint32_t(n) * 6, sy = r + uint32_t(n) * 2;  // panning map
        v = uint8_t(60 + ((hash(sx / 4, sy / 4) & 0x7f) + ((sx / 32 + sy / 32) & 1) * 40));
      } else v = (((r / 14) % 2) && ((hash(c / 3, r / 14) & 3) == 0)) ? 30 : 230;  // text rows
      if (c >= bx && c < bx + w / 4 && r >= by && r < by + h / 4) v = uint8_t(v / 2 + 90);
      row[c] = v;
    }
  }
  for (uint32_t r = 0; r < h / 2; ++r)
    for (uint32_t c = 0; c < w / 2; ++c) {
      uv[size_t(r) * w + c * 2] = uint8_t(128 + int((hash(c / 8 + n, r / 8) & 15)) - 8);
      uv[size_t(r) * w + c * 2 + 1] = uint8_t(128 + int((hash(c / 8, r / 8 + n) & 15)) - 8);
    }
}
std::vector<uint8_t> annexB(const BYTE* data, DWORD length) { return std::vector<uint8_t>(data, data + length); }
std::vector<std::vector<uint8_t>> encode(uint32_t w, uint32_t h, int frames, int fps, int bitrate) {
  ComPtr<IMFTransform> encoder; check(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder)), "encoder");
  ComPtr<ICodecAPI> codec; encoder.As(&codec);
  ui4(codec.Get(), CODECAPI_AVLowLatencyMode, 1);
  ui4(codec.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
  ui4(codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
  ui4(codec.Get(), CODECAPI_AVEncMPVGOPSize, ULONG(frames));
  ComPtr<IMFMediaType> output; check(MFCreateMediaType(&output), "out type");
  output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  output->SetUINT32(MF_MT_AVG_BITRATE, UINT32(bitrate)); MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, w, h);
  MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, fps, 1); MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  output->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601); output->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
  output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
  check(encoder->SetOutputType(0, output.Get(), 0), "set out");
  ComPtr<IMFMediaType> input; check(MFCreateMediaType(&input), "in type");
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, w, h); MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps, 1);
  MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1); input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  input->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601); input->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
  check(encoder->SetInputType(0, input.Get(), 0), "set in");
  encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0); encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  std::vector<uint8_t> sequence;
  {
    ComPtr<IMFMediaType> type; UINT32 length = 0;
    if (SUCCEEDED(encoder->GetOutputCurrentType(0, &type)) && SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &length)) && length) {
      sequence.resize(length); type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequence.data(), length, &length);
    }
  }
  std::vector<std::vector<uint8_t>> packets; std::vector<uint8_t> nv12(size_t(w) * h * 3 / 2);
  const auto drain = [&] {
    for (;;) {
      MFT_OUTPUT_STREAM_INFO info{}; check(encoder->GetOutputStreamInfo(0, &info), "info");
      ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> memory;
      check(MFCreateSample(&sample), "sample"); check(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 4 << 20), &memory), "mem"); sample->AddBuffer(memory.Get());
      MFT_OUTPUT_DATA_BUFFER out{}; out.pSample = sample.Get(); DWORD status = 0;
      const HRESULT hr = encoder->ProcessOutput(0, 1, &out, &status);
      if (out.pEvents) out.pEvents->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
      check(hr, "output");
      ComPtr<IMFMediaBuffer> bytes; check(sample->ConvertToContiguousBuffer(&bytes), "contig");
      BYTE* data = nullptr; DWORD length = 0; bytes->Lock(&data, nullptr, &length);
      auto encoded = annexB(data, length); bytes->Unlock();
      if (packets.empty() && !sequence.empty()) encoded.insert(encoded.begin(), sequence.begin(), sequence.end());
      packets.push_back(std::move(encoded));
    }
  };
  for (int n = 0; n < frames; ++n) {
    if (n == 0) ui4(codec.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    paint(nv12, w, h, n);
    ComPtr<IMFMediaBuffer> buffer; check(MFCreateMemoryBuffer(DWORD(nv12.size()), &buffer), "in buf");
    BYTE* target = nullptr; buffer->Lock(&target, nullptr, nullptr); std::memcpy(target, nv12.data(), nv12.size()); buffer->Unlock(); buffer->SetCurrentLength(DWORD(nv12.size()));
    ComPtr<IMFSample> sample; MFCreateSample(&sample); sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(LONGLONG(n) * 10000000 / fps); sample->SetSampleDuration(10000000 / fps);
    HRESULT hr = encoder->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) { drain(); hr = encoder->ProcessInput(0, sample.Get(), 0); }
    check(hr, "input"); drain();
  }
  encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0); drain();
  return packets;
}
int64_t field(const M& args, const char* key, int64_t fallback) {
  const auto it = args.find(V(key));
  if (it == args.end()) return fallback;
  if (const auto* i = std::get_if<int32_t>(&it->second)) return *i;
  if (const auto* l = std::get_if<int64_t>(&it->second)) return *l;
  if (const auto* b = std::get_if<bool>(&it->second)) return *b ? 1 : 0;
  return fallback;
}
double filetime_ms(const FILETIME& t) { return double((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 10000.0; }
// The calling thread is Flutter's merged platform/UI thread.
double main_thread_cpu_ms() {
  FILETIME c, e, k, u;
  return GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u) ? filetime_ms(k) + filetime_ms(u) : 0;
}
}

namespace {
int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}
void QaRegistrar::record(int64_t began) {
  const int64_t elapsed = now_ns() - began;
  ++takes; take_total_ns += elapsed;
  int64_t previous = take_max_ns.load();
  while (elapsed > previous && !take_max_ns.compare_exchange_weak(previous, elapsed)) {}
}
int64_t QaRegistrar::RegisterTexture(flutter::TextureVariant* texture) {
  original = texture;
  if (std::get_if<flutter::PixelBufferTexture>(texture)) {
    wrapped = std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture([this](size_t w, size_t h) -> const FlutterDesktopPixelBuffer* {
      raster_thread = GetCurrentThreadId();
      const int64_t began = now_ns();
      const auto* inner = std::get<flutter::PixelBufferTexture>(*original).CopyPixelBuffer(w, h);
      if (!inner) return nullptr;
      auto* held = new Held{*inner, inner, this, began};
      held->buffer.release_context = held;
      held->buffer.release_callback = [](void* context) {
        auto* value = static_cast<Held*>(context);
        if (value->inner->release_callback) value->inner->release_callback(value->inner->release_context);
        value->owner->record(value->began);
        delete value;
      };
      return &held->buffer;
    }));
  } else if (std::get_if<flutter::GpuSurfaceTexture>(texture)) {
    const auto type = std::get<flutter::GpuSurfaceTexture>(*texture).surface_type();
    wrapped = std::make_unique<flutter::TextureVariant>(flutter::GpuSurfaceTexture(type, [this](size_t w, size_t h) -> const FlutterDesktopGpuSurfaceDescriptor* {
      raster_thread = GetCurrentThreadId();
      const int64_t began = now_ns();
      const auto* inner = std::get<flutter::GpuSurfaceTexture>(*original).ObtainDescriptor(w, h);
      if (!inner) return nullptr;
      auto* held = new HeldSurface{*inner, inner, this, began};
      held->descriptor.release_context = held;
      held->descriptor.release_callback = [](void* context) {
        auto* value = static_cast<HeldSurface*>(context);
        if (value->inner->release_callback) value->inner->release_callback(value->inner->release_context);
        value->owner->record(value->began);
        delete value;
      };
      return &held->descriptor;
    }));
  } else return real->RegisterTexture(texture);
  return real->RegisterTexture(wrapped.get());
}
void QaRegistrar::UnregisterTexture(int64_t id, std::function<void()> callback) {
  std::shared_ptr<flutter::TextureVariant> keep(wrapped.release());
  real->UnregisterTexture(id, [keep, callback]() mutable { keep.reset(); if (callback) callback(); });
}

QaHarness::QaHarness(flutter::FlutterViewController* controller) : controller_(controller) {
  registrar_ = std::make_unique<flutter::PluginRegistrarWindows>(controller->engine()->GetRegistrarForPlugin("TextureQa"));
  proxy_.real = registrar_->texture_registrar();
  channel_ = std::make_unique<flutter::MethodChannel<V>>(controller->engine()->messenger(), "qa", &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    try {
      if (call.method_name() == "start") {
        const auto* args = std::get_if<M>(call.arguments());
        if (!args) { result->Error("args"); return; }
        Stop();
        width_ = uint32_t(field(*args, "width", 1264)); height_ = uint32_t(field(*args, "height", 760));
        fps_ = int(field(*args, "fps", 60)); pattern_ = int(field(*args, "burst", 0)); jitter_ms_ = int(field(*args, "jitter", 4)); drift_ppm_ = int(field(*args, "drift", 0)); timer_ = field(*args, "timer", 0) != 0;
        const auto encoding = std::chrono::steady_clock::now();
        if (packets_.empty() || fixture_width_ != width_ || fixture_height_ != height_) {
          check(MFStartup(MF_VERSION), "mf");
          packets_ = encode(width_, height_, 240, 60, 10000000); fixture_width_ = width_; fixture_height_ = height_;
        }
        const double encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - encoding).count();
        if (timer_) timeBeginPeriod(1);
        engine_ = std::make_unique<multitap::RemoteEngine>(&proxy_);
        const int64_t texture = engine_->Start(width_, height_, false);
        if (texture < 0) { result->Error("engine"); return; }
        running_ = true; pushed_ = 0; rejected_ = 0;
        pusher_ = std::thread([this] { Push(); });
        result->Success(V(M{{V("texture"), V(texture)}, {V("encodeMs"), V(encode_ms)}, {V("packets"), V(int64_t(packets_.size()))}}));
        return;
      }
      if (call.method_name() == "stats") {
        if (!engine_) { result->Error("idle"); return; }
        auto stats = engine_->Stats();
        FILETIME created, exited, kernel, user; GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        stats[V("qaPushed")] = V(int64_t(pushed_)); stats[V("qaRejected")] = V(int64_t(rejected_));
        stats[V("qaCpuMs")] = V(filetime_ms(kernel) + filetime_ms(user));
        // Thread times are clock-tick sampled: the platform/UI thread total
        // is indicative only. WARP rasterizes on its own worker threads.
        stats[V("qaMainCpuMs")] = V(main_thread_cpu_ms());
        stats[V("qaTakes")] = V(int64_t(proxy_.takes.load()));
        stats[V("qaTakeTotalMs")] = V(double(proxy_.take_total_ns.load()) / 1e6);
        stats[V("qaTakeMaxMs")] = V(double(proxy_.take_max_ns.load()) / 1e6);
        stats[V("qaWallMs")] = V(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count());
        result->Success(V(stats));
        return;
      }
      if (call.method_name() == "stop") { Stop(); result->Success(); return; }
      result->NotImplemented();
    } catch (const std::exception& error) { result->Error("failed", error.what()); }
  });
}
QaHarness::~QaHarness() { Stop(); channel_->SetMethodCallHandler(nullptr); }
void QaHarness::Stop() {
  running_ = false;
  if (pusher_.joinable()) pusher_.join();
  if (engine_) { engine_->Stop(); engine_.reset(); if (timer_) timeEndPeriod(1); }
}
void QaHarness::Push() {
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const auto start = std::chrono::steady_clock::now();
  // drift: the source clock runs fast by drift_ppm_, sweeping the arrival phase across the vsync grid.
  const double scale = 1.0 - double(drift_ppm_) / 1e6;
  const auto period = std::chrono::nanoseconds(int64_t(1e9 / fps_ * scale));
  // pattern 2: a 60 Hz host loop that encodes fps_ of its ticks (evenly
  // spread), each packet arriving 0..jitter ms after its tick.
  const auto tick = std::chrono::nanoseconds(int64_t(1e9 / 60 * scale));
  uint32_t random = 12345, credit = 0; uint64_t host_tick = 0;
  auto next_due = start;
  for (uint64_t n = 0; running_; ) {
    std::chrono::steady_clock::time_point due;
    if (pattern_ == 2) {
      for (;;) { ++host_tick; credit += uint32_t(fps_); if (credit >= 60) { credit -= 60; break; } }
      random = random * 1664525u + 1013904223u;
      const auto jitter = std::chrono::microseconds((random >> 8) % uint32_t(jitter_ms_ * 1000 + 1));
      due = std::max(next_due, start + tick * int64_t(host_tick) + jitter); next_due = due;
    } else due = start + period * int64_t(n);
    const auto wait = due - std::chrono::steady_clock::now();
    if (wait.count() > 0) {
      LARGE_INTEGER relative; relative.QuadPart = -std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count() / 100);
      SetWaitableTimer(timer, &relative, 0, nullptr, nullptr, FALSE); WaitForSingleObject(timer, 100);
    }
    const int count = pattern_ == 1 ? 2 : 1;
    for (int k = 0; k < count && running_; ++k, ++n) {
      const auto& payload = packets_[n % packets_.size()];
      std::vector<uint8_t> packet(12 + payload.size());
      packet[0] = 1; packet[1] = (n % packets_.size()) == 0 ? 1 : 0;
      const uint64_t pts = n * 1000000ULL / uint64_t(fps_); std::memcpy(packet.data() + 4, &pts, 8);
      std::memcpy(packet.data() + 12, payload.data(), payload.size());
      if (engine_->Push(packet)) ++pushed_; else ++rejected_;
    }
  }
  CloseHandle(timer);
}
