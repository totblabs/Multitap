#include "media_qa.h"
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
using V = flutter::EncodableValue;
using M = flutter::EncodableMap;
void check(HRESULT hr, const char* what) { if (FAILED(hr)) throw std::runtime_error(what); }
void ui4(ICodecAPI* api, const GUID& key, ULONG value) { VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = value; api->SetValue(&key, &v); }
uint32_t hash(uint32_t x, uint32_t y) { uint32_t h = x * 374761393u + y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }
// Desktop-like synthetic content (the texture-path QA fixture): static chrome
// and text rows, a panning map-like region and a moving window.
void paint(std::vector<uint8_t>& nv12, uint32_t w, uint32_t h, int n) {
  uint8_t* y = nv12.data(); uint8_t* uv = y + size_t(w) * h;
  const uint32_t mx0 = w / 8, my0 = h / 6, mx1 = w - w / 8, my1 = h - h / 8;
  const uint32_t bx = (uint32_t(n) * 9) % (w / 2), by = h / 4 + (uint32_t(n) * 3) % (h / 3);
  for (uint32_t r = 0; r < h; ++r) {
    uint8_t* row = y + size_t(r) * w;
    for (uint32_t c = 0; c < w; ++c) {
      uint8_t v;
      if (r < 28) v = 40;
      else if (c >= mx0 && c < mx1 && r >= my0 && r < my1) { const uint32_t sx = c + uint32_t(n) * 6, sy = r + uint32_t(n) * 2; v = uint8_t(60 + ((hash(sx / 4, sy / 4) & 0x7f) + ((sx / 32 + sy / 32) & 1) * 40)); }
      else v = (((r / 14) % 2) && ((hash(c / 3, r / 14) & 3) == 0)) ? 30 : 230;
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
std::vector<std::vector<uint8_t>> encode(uint32_t w, uint32_t h, int frames, int fps, int bitrate) {
  ComPtr<IMFTransform> encoder; check(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder)), "encoder");
  ComPtr<ICodecAPI> codec; encoder.As(&codec);
  ui4(codec.Get(), CODECAPI_AVLowLatencyMode, 1); ui4(codec.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
  ui4(codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0); ui4(codec.Get(), CODECAPI_AVEncMPVGOPSize, ULONG(frames));
  ComPtr<IMFMediaType> output; check(MFCreateMediaType(&output), "out type");
  output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  output->SetUINT32(MF_MT_AVG_BITRATE, UINT32(bitrate)); MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, w, h);
  MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, fps, 1); MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
  check(encoder->SetOutputType(0, output.Get(), 0), "set out");
  ComPtr<IMFMediaType> input; check(MFCreateMediaType(&input), "in type");
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, w, h); MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps, 1);
  MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1); input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  check(encoder->SetInputType(0, input.Get(), 0), "set in");
  encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0); encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  std::vector<uint8_t> sequence;
  { ComPtr<IMFMediaType> type; UINT32 length = 0;
    if (SUCCEEDED(encoder->GetOutputCurrentType(0, &type)) && SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &length)) && length) { sequence.resize(length); type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequence.data(), length, &length); } }
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
      std::vector<uint8_t> encoded(data, data + length); bytes->Unlock();
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
const V* field(const M& args, const char* key) { const auto it = args.find(V(key)); return it == args.end() ? nullptr : &it->second; }
int64_t integer(const M& args, const char* key, int64_t fallback) {
  const auto* value = field(args, key); if (!value) return fallback;
  if (const auto* i = std::get_if<int32_t>(value)) return *i;
  if (const auto* l = std::get_if<int64_t>(value)) return *l;
  if (const auto* b = std::get_if<bool>(value)) return *b ? 1 : 0;
  return fallback;
}
double filetime_ms(const FILETIME& t) { return double((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 10000.0; }
}

MediaQa::MediaQa(flutter::FlutterViewController* controller) : controller_(controller) {
  registrar_ = std::make_unique<flutter::PluginRegistrarWindows>(controller->engine()->GetRegistrarForPlugin("MediaQa"));
  channel_ = std::make_unique<flutter::MethodChannel<V>>(controller->engine()->messenger(), "qa", &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    try {
      const auto* args = std::get_if<M>(call.arguments());
      const auto& method = call.method_name();
      if (method == "push") {
        // The product's push: the packet bytes and an optional frame watch.
        if (!engine_ || !args) { result->Error("session_replaced"); return; }
        const auto* bytes = field(*args, "bytes"); const auto* packet = bytes ? std::get_if<std::vector<uint8_t>>(bytes) : nullptr;
        if (!packet) { result->Error("invalid_arguments"); return; }
        const auto outcome = engine_->Offer(*packet, integer(*args, "watch", 0) != 0);
        if (outcome == multitap::MediaPushResult::accepted) result->Success();
        else if (outcome == multitap::MediaPushResult::full) result->Error("media_queue_full");
        else result->Error("media_backpressure");
        return;
      }
      if (method == "start") {
        if (!args) { result->Error("args"); return; }
        Stop();
        width_ = uint32_t(integer(*args, "width", 1580)); height_ = uint32_t(integer(*args, "height", 952));
        check(MFStartup(MF_VERSION), "mf");
        packets_ = encode(width_, height_, int(integer(*args, "frames", 120)), 60, int(integer(*args, "bitrate", 8000000)));
        engine_ = std::make_unique<multitap::RemoteEngine>(registrar_->texture_registrar());
        const int64_t texture = engine_->Start(width_, height_, integer(*args, "audio", 0) != 0);
        if (texture < 0) { result->Error("engine"); return; }
        flutter::EncodableList list; for (const auto& packet : packets_) list.push_back(V(packet));
        result->Success(V(M{{V("texture"), V(texture)}, {V("packets"), V(std::move(list))}}));
        return;
      }
      if (method == "stats") {
        if (!engine_) { result->Error("idle"); return; }
        auto stats = engine_->Stats();
        FILETIME created, exited, kernel, user; GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        stats[V("qaCpuMs")] = V(filetime_ms(kernel) + filetime_ms(user));
        stats[V("qaPriorityClass")] = V(int64_t(GetPriorityClass(GetCurrentProcess())));
        result->Success(V(stats));
        return;
      }
      if (method == "priority") {
        // Candidate client fix under test: the whole process ABOVE_NORMAL.
        const bool raise = args && integer(*args, "raise", 0) != 0;
        if (raise) { if (!previous_priority_) previous_priority_ = GetPriorityClass(GetCurrentProcess()); SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS); }
        else if (previous_priority_) { SetPriorityClass(GetCurrentProcess(), previous_priority_); previous_priority_ = 0; }
        result->Success(V(int64_t(GetPriorityClass(GetCurrentProcess()))));
        return;
      }
      if (method == "burn") {
        // N normal-priority busy threads in a child process (a loaded PC, such
        // as parallel builds), killed with its job when the harness exits.
        const int threads = args ? int(integer(*args, "threads", 0)) : 0;
        if (threads > 0 && !burner_job_) {
          burner_job_ = CreateJobObjectW(nullptr, nullptr); JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
          SetInformationJobObject(burner_job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
          wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH); std::wstring command = L"\"" + std::wstring(path) + L"\" --burner=" + std::to_wstring(threads);
          STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
          if (CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
            AssignProcessToJobObject(burner_job_, child.hProcess); ResumeThread(child.hThread); CloseHandle(child.hThread); CloseHandle(child.hProcess);
          }
        }
        result->Success();
        return;
      }
      if (method == "stop") { Stop(); result->Success(); return; }
      result->NotImplemented();
    } catch (const std::exception& error) { result->Error("failed", error.what()); }
  });
}
MediaQa::~MediaQa() { Stop(); channel_->SetMethodCallHandler(nullptr); if (burner_job_) { TerminateJobObject(burner_job_, 0); CloseHandle(burner_job_); } }
void MediaQa::Stop() { if (engine_) { engine_->Stop(); engine_.reset(); } }
