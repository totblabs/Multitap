#include "media.hpp"
#include "audio_stats.hpp"
#include "audio_capture_control.hpp"
#include "opus_audio.hpp"
#include "gpu_h264.hpp"

#ifdef MULTITAP_X264
#include "x264_encoder_testing.hpp"
#endif
#include "owned_output_size.hpp"
#include "capture_session_interval.hpp"
#include "capture_compose.hpp"
#include "capture_dirty.hpp"
#include "capture_scale.hpp"
#include "cursor_patch.hpp"
#include "cursor_channel.hpp"
#include "gpu_capture.hpp"
#include "../../../native/windows_video/bgra_nv12_sse2.hpp"
#include <wmcodecdsp.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <emmintrin.h>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace host {
void check(HRESULT result, const char* operation) {
  if (FAILED(result)) { std::ostringstream text; text << operation << " (0x" << std::hex << uint32_t(result) << ")"; throw std::runtime_error(text.str()); }
}
Runtime::Runtime() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization"); check(MFStartup(MF_VERSION, MFSTARTUP_FULL), "Media Foundation startup"); }
Runtime::~Runtime() { MFShutdown(); CoUninitialize(); }
bool interactiveDesktopAvailable() {
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || session == 0) return false;
  HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!input) return false;
  wchar_t inputName[256]{}, currentName[256]{}; DWORD needed = 0;
  const bool readable = GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), &needed) && GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, currentName, sizeof(currentName), &needed);
  CloseDesktop(input);
  return readable && _wcsicmp(inputName, L"Winlogon") != 0 && _wcsicmp(inputName, currentName) == 0;
}
static std::vector<uint8_t> annexB(const uint8_t* bytes, size_t count) {
  if (count >= 4 && bytes[0] == 0 && bytes[1] == 0 && (bytes[2] == 1 || (bytes[2] == 0 && bytes[3] == 1))) return {bytes, bytes + count};
  std::vector<uint8_t> result;
  size_t at = 0;
  if (count >= 7 && bytes[0] == 1) {
    at = 6; const unsigned spsCount = bytes[5] & 31;
    for (unsigned group = 0; group < 2; group++) {
      unsigned nals = group == 0 ? spsCount : (at < count ? bytes[at++] : 0);
      for (unsigned i = 0; i < nals; i++) {
        if (at + 2 > count) throw std::runtime_error("Invalid codec sequence header");
        const size_t length = (size_t(bytes[at]) << 8) | bytes[at + 1]; at += 2;
        if (!length || at + length > count) throw std::runtime_error("Invalid codec sequence length");
        result.insert(result.end(), {0, 0, 0, 1}); result.insert(result.end(), bytes + at, bytes + at + length); at += length;
      }
    }
    return result;
  }
  while (at < count) {
    if (at + 4 > count) throw std::runtime_error("Invalid H264 packet");
    const size_t length = (size_t(bytes[at]) << 24) | (size_t(bytes[at + 1]) << 16) | (size_t(bytes[at + 2]) << 8) | bytes[at + 3]; at += 4;
    if (!length || at + length > count) throw std::runtime_error("Invalid H264 NAL length");
    result.insert(result.end(), {0, 0, 0, 1}); result.insert(result.end(), bytes + at, bytes + at + length); at += length;
  }
  return result;
}
static bool nalType(const std::vector<uint8_t>& bytes, uint8_t type) {
  for (size_t i = 0; i + 4 < bytes.size(); i++) {
    size_t n = 0;
    if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) n = i + 3;
    else if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 && bytes[i + 3] == 1) n = i + 4;
    if (n && (bytes[n] & 31) == type) return true;
  }
  return false;
}
// Current reference count. A pooled media buffer is reused only when the pool
// holds its sole reference, i.e. the MFT has released every sample that
// carried it; otherwise a fresh buffer is allocated exactly as before.
static ULONG references(IUnknown* object) { object->AddRef(); return object->Release(); }
static void codecValue(ICodecAPI* codec, const GUID& key, ULONG value, bool boolean = false) {
  if (!codec) return; VARIANT variant; VariantInit(&variant);
  variant.vt = VARTYPE(boolean ? VT_BOOL : VT_UI4);
  if (boolean) variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE; else variant.ulVal = value;
  codec->SetValue(&key, &variant); VariantClear(&variant);
}
// The explicit Microsoft software H.264 MFT (the only encoder before x264).
class MftEncoder final : public EncoderImplementation {
 public:
  MftEncoder(int width, int height, int fps, int bitrate, EncoderTuning tuning);
  ~MftEncoder() override;
  void encode(const uint8_t* nv12, uint64_t ptsUs, std::vector<Frame>& frames) override;
  void finish(std::vector<Frame>& frames) override;
  bool setBitrate(int bitrate) override;
  int reportedBitrate() const override;
  EncoderTuning reportedTuning() const override;
  void requestKeyFrame() override { keyFrameRequested_ = true; }
 private:
  void drain(std::vector<Frame>& frames);
  void headers();
  Com<IMFTransform> encoder_;
  Com<ICodecAPI> codec_;
  int width_, height_, fps_, bitrate_;
  uint64_t frameCount_ = 0;
  bool keyFrameRequested_ = false;
  std::vector<uint8_t> sequence_;
  // Reused only while no sample still references them (see encode/drain).
  std::vector<Com<IMFMediaBuffer>> inputs_;
  Com<IMFMediaBuffer> output_;
  DWORD outputCapacity_ = 0;
};
MftEncoder::MftEncoder(int width, int height, int fps, int bitrate, EncoderTuning tuning) : width_(width), height_(height), fps_(fps), bitrate_(bitrate) {
  validateFrameMemory(width,height,0,4);
  if ((tuning.profile != eAVEncH264VProfile_Base && tuning.profile != eAVEncH264VProfile_Main && tuning.profile != eAVEncH264VProfile_High) || tuning.qualityVsSpeed < -1 || tuning.qualityVsSpeed > 100 || (tuning.cabac && tuning.profile == eAVEncH264VProfile_Base)) throw std::runtime_error("Unsupported encoder tuning");
  // This explicit Microsoft software MFT never selects a vendor/GPU encoder.
  check(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder_)), "Create software H264 encoder");
  encoder_.As(&codec_);
  codecValue(codec_.Get(), CODECAPI_AVLowLatencyMode, 1, true);
  codecValue(codec_.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
  codecValue(codec_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
  // Media travels over TCP through the relay. Every new session, resize or
  // track topology creates a fresh encoder whose first output is an IDR; a
  // send backlog that drops video asks for one (requestKeyFrame) instead of
  // waiting for the GOP. A one-second GOP only produced
  // a bitrate/latency spike every second. GOP 0 selects that MFT default, and an
  // unbounded GOP made this MFT's CBR overshoot (1080p motion 12 -> 21 Mbps),
  // so keep a one-minute GOP: measured CBR stays on target, +1.5 dB motion PSNR.
  codecValue(codec_.Get(), CODECAPI_AVEncMPVGOPSize, ULONG(fps) * 60);
  auto explicitProperty = [&](const GUID& key, ULONG value, bool boolean) {
    if (!codec_) throw std::runtime_error("Encoder tuning API unavailable");
    VARIANT requested; VariantInit(&requested); requested.vt = VARTYPE(boolean ? VT_BOOL : VT_UI4);
    if (boolean) requested.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE; else requested.ulVal = value;
    const auto hr = codec_->SetValue(&key, &requested); VariantClear(&requested); check(hr, "Apply explicit encoder tuning");
  };
  if (tuning.qualityVsSpeed >= 0) explicitProperty(CODECAPI_AVEncCommonQualityVsSpeed, ULONG(tuning.qualityVsSpeed), false);
  if (tuning.cabac) explicitProperty(CODECAPI_AVEncH264CABACEnable, 1, true);
  Com<IMFMediaType> output; check(MFCreateMediaType(&output), "Create encoded media type");
  check(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Encoder output major type");
  check(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "Encoder output H264");
  check(output->SetUINT32(MF_MT_AVG_BITRATE, UINT32(bitrate)), "Encoder bitrate");
  check(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, UINT32(width), UINT32(height)), "Encoder frame size");
  check(MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, UINT32(fps), 1), "Encoder frame rate");
  check(MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Encoder pixel aspect");
  check(output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Encoder interlace mode");
  // bgraToNv12 uses the BT.601 studio-range coefficients below. Carry the
  // same interpretation through the encoded stream instead of relying on
  // resolution-dependent decoder defaults (HD does not imply BT.709 here).
  check(output->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601), "Encoder colour matrix");
  check(output->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235), "Encoder nominal range");
  check(output->SetUINT32(MF_MT_MPEG2_PROFILE, tuning.profile), "Encoder profile");
  check(encoder_->SetOutputType(0, output.Get(), 0), "Set encoder output");
  Com<IMFMediaType> input; check(MFCreateMediaType(&input), "Create raw media type");
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, UINT32(width), UINT32(height));
  MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, UINT32(fps), 1); MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  check(input->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601), "Raw colour matrix");
  check(input->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235), "Raw nominal range");
  check(encoder_->SetInputType(0, input.Get(), 0), "Set encoder NV12 input");
  check(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Encoder begin stream");
  check(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Encoder start");
  headers();
}
MftEncoder::~MftEncoder() { if (encoder_) encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0); }
bool MftEncoder::setBitrate(int bitrate) {
  if (bitrate == bitrate_) return true;
  if (!codec_) return false;
  // The Microsoft H.264 MFT accepts a CBR mean-bitrate change while streaming;
  // the next submitted frame is budgeted at the new rate (measured in
  // encoder_bitrate_test.cpp). Nothing else (GOP, profile, type) changes.
  VARIANT value; VariantInit(&value); value.vt = VT_UI4; value.ulVal = ULONG(bitrate);
  const HRESULT result = codec_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &value); VariantClear(&value);
  if (FAILED(result)) return false;
  bitrate_ = bitrate; return true;
}
int MftEncoder::reportedBitrate() const {
  if (!codec_) return 0;
  VARIANT value; VariantInit(&value); int reported = 0;
  if (SUCCEEDED(codec_->GetValue(&CODECAPI_AVEncCommonMeanBitRate, &value)) && value.vt == VT_UI4 && value.ulVal <= ULONG(encoderMaximumBitrate)) reported = int(value.ulVal);
  VariantClear(&value); return reported;
}
EncoderTuning MftEncoder::reportedTuning() const {
  EncoderTuning value;
  Com<IMFMediaType> type; UINT32 profile = 0;
  check(encoder_->GetOutputCurrentType(0, &type), "Read applied encoder profile");
  check(type->GetUINT32(MF_MT_MPEG2_PROFILE, &profile), "Read applied H264 profile");
  value.profile = static_cast<eAVEncH264VProfile>(profile);
  if (codec_) {
    VARIANT setting; VariantInit(&setting);
    if (SUCCEEDED(codec_->GetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &setting)) && setting.vt == VT_UI4) value.qualityVsSpeed = int(setting.ulVal);
    VariantClear(&setting);
    if (SUCCEEDED(codec_->GetValue(&CODECAPI_AVEncH264CABACEnable, &setting)) && setting.vt == VT_BOOL) value.cabac = setting.boolVal == VARIANT_TRUE;
    VariantClear(&setting);
  }
  return value;
}
void MftEncoder::headers() {
  Com<IMFMediaType> type;
  if (FAILED(encoder_->GetOutputCurrentType(0, &type))) return;
  UINT32 length = 0;
  if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &length)) || !length || length > 65536) return;
  std::vector<uint8_t> bytes(length); check(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, bytes.data(), length, &length), "Read sequence headers");
  sequence_ = annexB(bytes.data(), length);
}
void MftEncoder::encode(const uint8_t* nv12, uint64_t ptsUs, std::vector<Frame>& output) {
  // This MFT takes VT_UI4 here; a VT_BOOL request is rejected with E_INVALIDARG.
  if (frameCount_++ == 0 || std::exchange(keyFrameRequested_, false)) codecValue(codec_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
  const auto layout=checkedFrameLayout(uint64_t(width_),uint64_t(height_));
  if(!layout)throw std::runtime_error("encoder_format_unsupported");
  const DWORD count=DWORD(layout->nv12Bytes);
  // Avoid a fresh multi-megabyte allocation (and its page faults) per frame.
  Com<IMFMediaBuffer> buffer;
  for (const auto& pooled : inputs_) if (references(pooled.Get()) == 1) { buffer = pooled; break; }
  if (!buffer) { check(MFCreateMemoryBuffer(count, &buffer), "Allocate raw frame"); if (inputs_.size() < 4) inputs_.push_back(buffer); }
  BYTE* target = nullptr; check(buffer->Lock(&target, nullptr, nullptr), "Lock raw frame"); std::memcpy(target, nv12, count); buffer->Unlock(); buffer->SetCurrentLength(count);
  Com<IMFSample> sample; check(MFCreateSample(&sample), "Create raw sample"); sample->AddBuffer(buffer.Get()); sample->SetSampleTime(LONGLONG(ptsUs * 10)); sample->SetSampleDuration(10000000 / fps_);
  HRESULT result = encoder_->ProcessInput(0, sample.Get(), 0);
  if (result == MF_E_NOTACCEPTING) { drain(output); result = encoder_->ProcessInput(0, sample.Get(), 0); }
  check(result, "Encode H264 input"); drain(output);
}
void MftEncoder::drain(std::vector<Frame>& output) {
  for (;;) {
    MFT_OUTPUT_STREAM_INFO info{}; check(encoder_->GetOutputStreamInfo(0, &info), "Encoder output requirements");
    Com<IMFSample> allocated;
    if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
      check(MFCreateSample(&allocated), "Create encoded sample"); Com<IMFMediaBuffer> memory;
      const DWORD capacity = std::max<DWORD>(info.cbSize, 2 * 1024 * 1024);
      // MFT cbSize is a worst-case output allocation, not the actual wire size.
      // Two native 1080p displays need more than 4 MiB of workspace; packets still
      // retain the separate 4 MiB protocol limit in mediaPacket().
      // Reuse the workspace once its previous sample and our reads released it.
      if(!output_||outputCapacity_<capacity||references(output_.Get())!=1){
        MEMORYSTATUSEX memoryState{};memoryState.dwLength=sizeof(memoryState);
        if(!GlobalMemoryStatusEx(&memoryState))throw std::runtime_error("frame_memory_unavailable");
        if(uint64_t(capacity)>std::min(memoryState.ullAvailPhys,memoryState.ullAvailVirtual))throw std::runtime_error("frame_memory_insufficient");
        output_.Reset();check(MFCreateMemoryBuffer(capacity,&output_),"Allocate encoded frame");outputCapacity_=capacity;
      }
      memory = output_; check(memory->SetCurrentLength(0), "Allocate encoded frame"); allocated->AddBuffer(memory.Get());
    }
    MFT_OUTPUT_DATA_BUFFER result{}; result.pSample = allocated.Get(); DWORD status = 0;
    const HRESULT hr = encoder_->ProcessOutput(0, 1, &result, &status);
    if (result.pEvents) result.pEvents->Release();
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
    check(hr, "Receive H264 output");
    Com<IMFSample> sample = allocated;
    if (!allocated && result.pSample) sample.Attach(result.pSample);
    if (!sample) throw std::runtime_error("Missing encoded sample");
    Com<IMFMediaBuffer> bytes; check(sample->ConvertToContiguousBuffer(&bytes), "Read encoded buffer");
    BYTE* data = nullptr; DWORD length = 0; check(bytes->Lock(&data, nullptr, &length), "Lock encoded buffer");
    std::vector<uint8_t> encoded;
    try { encoded = annexB(data, length); } catch (...) { bytes->Unlock(); throw; } bytes->Unlock();
    if (encoded.empty()) continue;
    Frame frame; frame.key = nalType(encoded, 5); LONGLONG time = 0; sample->GetSampleTime(&time); frame.ptsUs = uint64_t(std::max<LONGLONG>(time, 0) / 10);
    if (frame.key && !(nalType(encoded, 7) && nalType(encoded, 8))) {
      if (sequence_.empty()) headers();
      if (sequence_.empty()) throw std::runtime_error("H264 sequence headers unavailable");
      frame.bytes = sequence_;
      frame.bytes.insert(frame.bytes.end(), encoded.begin(), encoded.end());
    } else frame.bytes = std::move(encoded);
    output.push_back(std::move(frame));
  }
}
void MftEncoder::finish(std::vector<Frame>& output) { encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); check(encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "Drain H264 encoder"); drain(output); }

// ---------------------------------------------------------------- backend facade
#ifdef MULTITAP_X264
namespace x264testing { std::atomic<ConfigurationProvider> configurationProvider{nullptr}; }
// x264_encoder.cpp: throws X264Failure (EncoderFallback code) on any failure.
std::unique_ptr<EncoderImplementation> createX264Encoder(int width, int height, int fps, int bitrate, const X264Settings& settings);
uint32_t x264FailureCode(const std::exception& error);
#else
bool x264EncoderAvailable() { return false; }
EncoderBackend configuredEncoderBackend() { return EncoderBackend::mft; }
X264Settings configuredX264Settings() { return X264Settings::production(); }
EncoderBackend encoderBackendFromConfig(const std::string&) { return EncoderBackend::mft; }
#endif
namespace {
std::mutex fallbackObserverMutex;
std::function<void(uint32_t)> fallbackObserver;
void notifyFallback(uint32_t code) noexcept {
  try { std::function<void(uint32_t)> observer; { std::lock_guard lock(fallbackObserverMutex); observer = fallbackObserver; } if (observer) observer(code); } catch (...) {}
}
}
void validateFrameMemory(int width,int height,uint64_t bgraCopies,uint64_t nv12Copies,bool even){
  multitap::video::FrameLayout raw;
  if(width<=0||height<=0||!multitap::video::frameLayout(uint32_t(width),uint32_t(height),raw,even))throw std::runtime_error("frame_layout_unsupported");
  const FrameLayout layout{raw.rgbaBytes,raw.nv12Bytes,raw.stride};
  MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
  if(!GlobalMemoryStatusEx(&memory))throw std::runtime_error("frame_memory_unavailable");
  if(!frameMemoryFits(layout,std::min(memory.ullAvailPhys,memory.ullAvailVirtual),bgraCopies,nv12Copies))throw std::runtime_error("frame_memory_insufficient");
}
void validateStreamMemory(std::span<const StreamMemoryShape> shapes){
  const auto forecast=streamMemoryForecast(shapes);
  if(!forecast)throw std::runtime_error("frame_layout_unsupported");
  MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
  const auto available=GlobalMemoryStatusEx(&memory)?std::optional<uint64_t>(std::min(memory.ullAvailPhys,memory.ullAvailVirtual)):std::nullopt;
  if(const auto* failure=streamMemoryFailure(streamMemoryAdmission(forecast,available)))throw std::runtime_error(failure);
}
namespace {
void validateEncoderFormat(int width, int height, int fps, int bitrate) {
  if (!encodedPictureSize(uint64_t(std::max(width, 0)), uint64_t(std::max(height, 0))) || fps < 1 || fps > 60 || bitrate < encoderMinimumBitrate || bitrate > encoderMaximumBitrate) throw std::runtime_error("Unsupported capture format");
}
bool defaultTuning(const EncoderTuning& tuning) { return tuning.profile == eAVEncH264VProfile_Base && tuning.qualityVsSpeed == -1 && !tuning.cabac; }
}
void setEncoderFallbackObserver(std::function<void(uint32_t)> observer) { std::lock_guard lock(fallbackObserverMutex); fallbackObserver = std::move(observer); }
X264RateControl x264RateControl(int width, int height, int fps, int bitrate, const X264Settings& settings) {
  X264RateControl value;
  value.maximumKbps = std::max(1, bitrate / 1000);
  // A buffer below one frame period is raised to it by x264 itself.
  const int64_t buffer = int64_t(value.maximumKbps) * std::max(1, settings.bufferMs) / 1000;
  value.bufferKbit = int(std::clamp<int64_t>(buffer, std::max(1, value.maximumKbps / std::max(1, fps)), std::max(1, settings.maximumBufferKbit)));
  if (settings.crf > 0) value.crf = settings.crf;
  else {
    // Quality cap; VBV (the target) bounds every frame. CRF 12 measured better
    // than 14-18 at 8 and 20 Mbps for +3-30 % bits (docs/x264-encoder.md). A
    // target above 0.16 bit per pixel-frame (1080p60 at 20 Mbps) lowers the
    // cap 2 steps per doubling, to 10, so granted bandwidth buys sharper text.
    const double bpp = double(bitrate) / (double(width) * double(height) * double(std::max(1, fps)));
    value.crf = std::clamp(12.0 - 2.0 * std::log2(std::max(bpp, 1e-6) / 0.16), 10.0, 12.0);
  }
  return value;
}
Encoder::Encoder(int width, int height, int fps, int bitrate, EncoderTuning tuning, EncoderBackend backend, std::optional<uint64_t> captureAdapter, ID3D11Device* captureDevice) : width_(width), height_(height), fps_(fps), bitrate_(bitrate) {
  validateEncoderFormat(width, height, fps, bitrate);
  const bool tuned = !defaultTuning(tuning);
  if ((backend == EncoderBackend::x264 || hardwareEncoderBackend(backend)) && tuned) throw std::runtime_error("Encoder tuning applies to the Microsoft MFT only");
  bool testConfiguration = false;
  EncoderBackend testBackend = EncoderBackend::x264;
#ifdef MULTITAP_X264
  if (const auto provider = x264testing::configurationProvider.load()) {
    testConfiguration = true;
    if (hardwareEncoderBackend(backend)) throw std::runtime_error("GPU opening is prohibited by the CPU test configuration");
    if (backend == EncoderBackend::automatic && !tuned) {
      testBackend = provider();
      if (testBackend != EncoderBackend::x264 && testBackend != EncoderBackend::mft) throw std::runtime_error("CPU test configuration returned a non-CPU backend");
    }
  }
#endif
  const EncoderBackend wanted = backend == EncoderBackend::automatic ? (tuned ? EncoderBackend::mft : testConfiguration ? testBackend : configuredEncoderBackend()) : backend;
  strict_ = backend == EncoderBackend::x264;
  if (!testConfiguration && !tuned && (backend == EncoderBackend::automatic || hardwareEncoderBackend(backend))) {
    auto selection = gpu::configuredSelection();
    if (backend == EncoderBackend::nvenc) selection.mode = gpu::Mode::nvenc;
    else if (backend == EncoderBackend::amf) selection.mode = gpu::Mode::amf;
    else if (backend == EncoderBackend::qsvMft) selection.mode = gpu::Mode::qsv;
    if (selection.mode != gpu::Mode::disabled) {
      try {
        auto candidate = gpu::createEncoder(width, height, fps, bitrate, selection, captureAdapter, captureDevice);
        const auto observed = candidate->hardwareInfo();
        if (!observed.initialized) throw gpu::Failure(encoderGpuInitializationFailed, "GPU backend did not complete initialization");
        const auto active = gpu::backendForVendor(observed.vendorId);
        impl_ = std::move(candidate); backend_ = active;
        // encoder_gpu_retry.hpp: the same selection again, on the encoder's own
        // device (the capture's device may be gone by then; pictures are read back).
        gpuReopen_ = [width, height, fps, selection, captureAdapter](int rate) { return gpu::createEncoder(width, height, fps, rate, selection, captureAdapter, nullptr); };
      }
      catch (const std::exception& error) {
        gpuFallback_ = gpu::failureCode(error); gpuFailure_ = gpu::failureInfo(error);
        // A failed candidate created during construction has already closed.
        freezeEncoderGpuObservation(gpuFailure_);
        if (!gpuFailure_.site) gpuFailure_.site = uint32_t(EncoderGpuFailureSite::encoderInitialize);
      }
      catch (...) { gpuFallback_ = encoderGpuInitializationFailed; gpuFailure_ = gpu::failureDetail(EncoderGpuFailureSite::encoderInitialize); }
    }
  }

  if (!impl_ && (wanted == EncoderBackend::x264 || hardwareEncoderBackend(wanted))) {
#ifdef MULTITAP_X264
    try { impl_ = createX264Encoder(width, height, fps, bitrate, configuredX264Settings()); backend_ = EncoderBackend::x264; }
    catch (const std::exception& error) { if (strict_) throw; fallback_ = x264FailureCode(error); }
    catch (...) { if (strict_) throw; fallback_ = encoderFallbackX264Unknown; }
    if (fallback_) notifyFallback(fallback_);
#else
    if (strict_) throw std::runtime_error("This host was built without the x264 encoder");
#endif
  }
  if (!impl_) { impl_ = std::make_unique<MftEncoder>(width, height, fps, bitrate, tuning); backend_ = EncoderBackend::mft; }
}
Encoder::Encoder(int width, int height, int fps, int bitrate, const X264Settings& settings) : width_(width), height_(height), fps_(fps), bitrate_(bitrate) {
  validateEncoderFormat(width, height, fps, bitrate);
  strict_ = true;

#ifdef MULTITAP_X264
  impl_ = createX264Encoder(width, height, fps, bitrate, settings); backend_ = EncoderBackend::x264;
#else
  (void)settings; throw std::runtime_error("This host was built without the x264 encoder");
#endif
}
Encoder::~Encoder() = default;
void Encoder::fallBack(uint32_t code) {
  // The client keeps decoding: the MFT's first output is an IDR with its own
  // SPS/PPS at the pts of the frame x264 failed on, so the epoch continues.
  impl_.reset(); ++outputGeneration_; backend_ = EncoderBackend::mft; fallback_ = code ? code : encoderFallbackX264Unknown;
  notifyFallback(fallback_);
  impl_ = std::make_unique<MftEncoder>(width_, height_, fps_, bitrate_, EncoderTuning{});
}
uint64_t Encoder::gpuRetryNow() const {
  if (gpuRetryClock_) return gpuRetryClock_();
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
void Encoder::notifyGpuRetry(uint32_t kind, uint32_t code) noexcept {
  if (gpuRetryObserver_) try { gpuRetryObserver_(kind, gpuRetry_.attempts(), code); } catch (...) { }
}
void Encoder::gpuRetryFailed(uint32_t kind, uint32_t code) {
  notifyGpuRetry(kind, code);
  gpuRetry_.left(gpuRetryTransient(code, true), gpuRetryNow());
  if (!gpuRetry_.pending()) notifyGpuRetry(gpuRetryEnded, code);
}
// encoder_gpu_retry.hpp. The CPU encoder stays untouched until the GPU encoder
// has coded this very picture: one valid IDR at this pts. Anything else leaves
// the stream exactly as it was (no extra IDR, no gap) and the attempt is over.
bool Encoder::returnToGpu(const uint8_t* nv12, uint64_t ptsUs) {
  if (!gpuCandidate_.valid()) {
    if (!gpuRetry_.due(gpuRetryNow())) return false;
    gpuRetry_.begin(); notifyGpuRetry(gpuRetryStarted, gpuFallback_);
    // Opening can take long on a busy GPU: never on the encoding thread.
    gpuCandidateBitrate_ = bitrate_;
    try {
      gpuCandidate_ = std::async(std::launch::async, [open = gpuReopen_, rate = bitrate_] {
        // A Media Foundation backend (QSV) is created through COM.
        struct Apartment { HRESULT entered = CoInitializeEx(nullptr, COINIT_MULTITHREADED); ~Apartment() { if (SUCCEEDED(entered)) CoUninitialize(); } } apartment;
        return open(rate);
      });
    }
    catch (...) { gpuCandidate_ = {}; gpuRetryFailed(gpuRetryOpenFailed, encoderGpuAllocationFailed); }
    return false;
  }
  if (gpuCandidate_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
  std::unique_ptr<EncoderImplementation> candidate; uint32_t failure = 0;
  try { candidate = gpuCandidate_.get(); if (!candidate || !candidate->hardwareInfo().initialized) failure = encoderGpuInitializationFailed; }
  catch (const std::exception& error) { failure = gpu::failureCode(error); }
  catch (...) { failure = encoderGpuInitializationFailed; }
  gpuCandidate_ = {};
  if (failure) { candidate.reset(); gpuRetryFailed(gpuRetryOpenFailed, failure); return false; }
  std::vector<Frame> frames;
  try {
    // The target moved while it opened: it must take the current one.
    if (gpuCandidateBitrate_ != bitrate_ && !candidate->setBitrate(bitrate_)) failure = encoderGpuReconfigureFailed;
    else {
      candidate->encode(nv12, ptsUs, frames);
      if (frames.size() != 1 || !frames.front().key || frames.front().ptsUs != ptsUs || frames.front().bytes.empty()) failure = encoderGpuOutputContract;
    }
  }
  catch (const std::exception& error) { failure = gpu::failureCode(error); }
  catch (...) { failure = encoderGpuSubmissionFailed; }
  if (failure) { frames.clear(); candidate.reset(); gpuRetryFailed(gpuRetryOutputFailed, failure ? failure : uint32_t(encoderGpuSubmissionFailed)); return false; }
  impl_ = std::move(candidate); ++outputGeneration_;
  backend_ = gpu::backendForVendor(impl_->hardwareInfo().vendorId);
  gpuFallback_ = encoderGpuNone; gpuFailure_ = {};
  pending_ = std::move(frames);
  notifyGpuRetry(gpuRetryReturned, encoderGpuNone);
  return true;
}
void Encoder::fallBackGpu(uint32_t code, EncoderGpuFailure detail) {
  // This encoder emits one validated output per input. Failed GPU output is
  // discarded; the replacement CPU encoder starts with SPS/PPS + IDR at the
  // same input PTS, preserving the stream/epoch and every submitted picture.
  // The candidate wrapper already bound failures to the failed adapter. Only
  // a direct implementation without that evidence needs its current identity.
  if (!detail.vendorId && impl_) try {
    const auto failed = impl_->hardwareInfo(); detail.vendorId = failed.vendorId; detail.deviceId = failed.deviceId;
  } catch (...) { }
  impl_.reset();
  // Existing reset performs the EOS/free/destroy sequence and possible
  // process quarantine. Snapshot only after it returns, never at throw time.
  freezeEncoderGpuObservation(detail); gpuFailure_ = std::move(detail);
  ++outputGeneration_; gpuFallback_ = code ? code : uint32_t(encoderGpuInitializationFailed);
  // A missed output deadline may be tried again later (encoder_gpu_retry.hpp).
  gpuRetry_.left(bool(gpuReopen_) && gpuRetryTransient(gpuFallback_, false), gpuRetryNow());

#ifdef MULTITAP_X264
  try { impl_ = createX264Encoder(width_, height_, fps_, bitrate_, configuredX264Settings()); backend_ = EncoderBackend::x264; return; }
  catch (const std::exception& error) { fallback_ = x264FailureCode(error); }
  catch (...) { fallback_ = encoderFallbackX264Unknown; }
  notifyFallback(fallback_);
#endif
  impl_ = std::make_unique<MftEncoder>(width_, height_, fps_, bitrate_, EncoderTuning{}); backend_ = EncoderBackend::mft;
}
void Encoder::encode(const uint8_t* nv12, uint64_t ptsUs, const std::function<void(Frame)>& output) {
  pending_.clear();
  if (!hardwareEncoderBackend(backend_) && (gpuCandidate_.valid() || gpuRetry_.pending()) && returnToGpu(nv12, ptsUs)) {
    // The reopened GPU encoder coded this picture as its first IDR.
  } else if (hardwareEncoderBackend(backend_)) {
    uint32_t failure = 0;
    EncoderGpuFailure detail;
    try { impl_->encode(nv12, ptsUs, pending_); }
    catch (const std::exception& error) { failure = gpu::failureCode(error); detail = gpu::failureInfo(error); }
    catch (...) { failure = encoderGpuSubmissionFailed; }
    if (failure) {
      if (!detail.site) detail.site = uint32_t(EncoderGpuFailureSite::encoderSubmit);
      pending_.clear(); fallBackGpu(failure, detail); encode(nv12, ptsUs, output); return;
    }
    // Auto may reject the first candidate's output and select another vendor
    // before publishing anything. Report the identity of that actual output.
    backend_ = gpu::backendForVendor(impl_->hardwareInfo().vendorId);
  } else if (backend_ == EncoderBackend::x264 && !strict_) {
#ifdef MULTITAP_X264
    uint32_t failure = 0;
    try { impl_->encode(nv12, ptsUs, pending_); }
    catch (const std::exception& error) { failure = x264FailureCode(error); }
    catch (...) { failure = encoderFallbackX264Unknown; }
    if (failure) { pending_.clear(); fallBack(failure); impl_->encode(nv12, ptsUs, pending_); }
#endif
  } else impl_->encode(nv12, ptsUs, pending_);
  for (auto& frame : pending_) deliverOutput(std::move(frame),output);
  pending_.clear();
}
void Encoder::encode(const EncoderPicture& picture, uint64_t ptsUs, const std::function<void(Frame)>& output) {
  // CPU pictures and CPU backends: exactly the byte path above.
  if (!picture.texture || !hardwareEncoderBackend(backend_)) { encode(picture.bytes(), ptsUs, output); return; }
  pending_.clear();
  uint32_t failure = 0;
  EncoderGpuFailure detail;
  try { impl_->encodePicture(picture, ptsUs, pending_); }
  catch (const std::exception& error) { failure = gpu::failureCode(error); detail = gpu::failureInfo(error); }
  catch (...) { failure = encoderGpuSubmissionFailed; }
  if (failure) {
    if (!detail.site) detail.site = uint32_t(EncoderGpuFailureSite::encoderSubmit);
    // The CPU encoder codes this same picture (read back) as its first IDR.
    pending_.clear(); fallBackGpu(failure, detail); encode(picture.bytes(), ptsUs, output); return;
  }
  backend_ = gpu::backendForVendor(impl_->hardwareInfo().vendorId);
  for (auto& frame : pending_) deliverOutput(std::move(frame),output);
  pending_.clear();
}
ID3D11Device* Encoder::textureDevice() const { return hardwareEncoderBackend(backend_) && impl_ ? impl_->textureDevice() : nullptr; }
void Encoder::finish(const std::function<void(Frame)>& output) {
  std::vector<Frame> frames; impl_->finish(frames);
  for (auto& frame : frames) deliverOutput(std::move(frame),output);
}
void Encoder::deliverOutput(Frame frame,const std::function<void(Frame)>& output) {
  std::optional<EncoderOutputObservation> observation;
  const auto generation=outputGeneration_;
  // No extra header scan/info allocation on ordinary frames. A failed
  // optional observer may retry at a later keyframe without changing media.
  if(outputObserver_&&observedOutputGeneration_!=generation)try{
    const auto unit=gpu::inspectAnnexB(frame.bytes,encoderMaximumFrameBytes);
    const auto info=hardwareInfo();
    if(info.initialized&&(!hardwareEncoderBackend(backend_)||info.hardwareVerified)&&frame.key&&unit.valid&&unit.idr&&unit.sps&&unit.pps&&unit.spsProfile==66)
      observation=EncoderOutputObservation{backendName(),info,fallback_};
  }catch(...){ }
  output(std::move(frame));
  if(observation&&outputObserver_)try{outputObserver_(*observation);observedOutputGeneration_=generation;}catch(...){ }
}
bool Encoder::setBitrate(int bitrate) {
  if (bitrate < encoderMinimumBitrate || bitrate > encoderMaximumBitrate) throw std::runtime_error("Unsupported encoder bitrate");
  if (bitrate == bitrate_) return true;
  if (hardwareEncoderBackend(backend_)) {
    bool applied = false;
    EncoderGpuFailure detail;
    try { applied = impl_->setBitrate(bitrate); }
    catch (const std::exception& error) { detail = gpu::failureInfo(error); }
    catch (...) { }
    if (!applied) {
      if (!detail.site) detail.site = uint32_t(EncoderGpuFailureSite::encoderReconfigure);
      fallBackGpu(encoderGpuReconfigureFailed, detail); if (!impl_->setBitrate(bitrate)) return false;
    }
  } else if (!impl_->setBitrate(bitrate)) return false;
  bitrate_ = bitrate; return true;
}
int Encoder::reportedBitrate() const { return impl_->reportedBitrate(); }
EncoderTuning Encoder::reportedTuning() const { return impl_->reportedTuning(); }
EncoderHardwareInfo Encoder::hardwareInfo() const {
  EncoderHardwareInfo value;
  if (hardwareEncoderBackend(backend_) && impl_) value = impl_->hardwareInfo();
  else value.initialized = bool(impl_);
  value.gpuFallbackCode = gpuFallback_; value.gpuFailure = gpuFailure_; return value;
}
void bgraToNv12(const uint8_t* source, uint8_t* target, int width, int height) {
  pixels::bgraToNv12PairedRows(source, target, width, height);
}
// A new capture's own device: hardware, default adapter, BGRA (the one place
// that creates it, so a warmed device is exactly what a capture would create).
// Video support first: a hardware H264 MFT (Intel QSV) takes the capture's own device only when it
// can run video work (gpu_h264_qsv_mft.cpp). A driver that refuses the flag gets the device without it.
static HRESULT createCaptureHardwareDevice(Com<ID3D11Device>& device,Com<ID3D11DeviceContext>& context){
  const HRESULT video=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
  if(SUCCEEDED(video))return video;
  device.Reset();context.Reset();
  return D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
}
std::shared_ptr<ID3D11Device> createCaptureDevice(){
  Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;
  if(FAILED(createCaptureHardwareDevice(device,context))||!device)return {};
  ID3D11Device* raw=device.Get();
  return std::shared_ptr<ID3D11Device>(raw,[keep=std::move(device)](ID3D11Device*)mutable{keep.Reset();});
}
std::optional<uint64_t> captureDeviceAdapter(ID3D11Device* device){
  if(!device)return std::nullopt;
  Com<IDXGIDevice> dxgi;Com<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC description{};
  if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))||FAILED(dxgi->GetAdapter(&adapter))||FAILED(adapter->GetDesc(&description)))return std::nullopt;
  return (uint64_t(uint32_t(description.AdapterLuid.HighPart))<<32)|description.AdapterLuid.LowPart;
}
std::optional<uint64_t> defaultCaptureAdapter(){
  // D3D11CreateDevice with no adapter uses the first one IDXGIFactory1 enumerates.
  Com<IDXGIFactory1> factory;Com<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 description{};
  if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))||FAILED(factory->EnumAdapters1(0,&adapter))||FAILED(adapter->GetDesc1(&description)))return std::nullopt;
  return (uint64_t(uint32_t(description.AdapterLuid.HighPart))<<32)|description.AdapterLuid.LowPart;
}
bool captureDeviceRemoved(ID3D11Device* device){return !device||FAILED(device->GetDeviceRemovedReason());}
// GraphicsCaptureSession::IsSupported() took 18 ms per call on a GPU-less VM
// (MultitapHostConnectStartBenchmark media mode), and every capture
// asked it: each stream start, resize restart and capture rebuild. A yes holds
// for the process; a no is asked again next time.
static CaptureSupportCache& graphicsCaptureSupport(){static CaptureSupportCache cache;return cache;}
bool graphicsCaptureSupported(){return graphicsCaptureSupport().supported([]{return winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported();});}
void warmCaptureRuntime(ID3D11Device* device){
  struct Apartment{HRESULT result=RoInitialize(RO_INIT_MULTITHREADED);~Apartment(){if(SUCCEEDED(result))RoUninitialize();}}apartment;
  if(FAILED(apartment.result)&&apartment.result!=RPC_E_CHANGED_MODE)return;
  using namespace winrt::Windows::Graphics::Capture;
  try{
    if(!graphicsCaptureSupported())return;
    (void)winrt::get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
    // The WinRT wrapper a frame pool takes; created and released, nothing captured.
    Com<IDXGIDevice> dxgi;winrt::com_ptr<IInspectable> inspectable;
    if(device&&SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))))(void)CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),inspectable.put());
  }catch(...){ }
}
void warmLoopbackAudio(){
  const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  try{Loopback opened;(void)opened.available();}catch(...){ }
  if(SUCCEEDED(com))CoUninitialize();
}
// WGC reads compositor frames without a synchronous GetDC(screen) readback per frame.
// Its D3D11 device explicitly falls back to WARP on GPUless guests. Encoder
// selection remains independent. Every pool captures an exact selected monitor.
struct Capture::Modern {
  struct Apartment { Apartment(){check(RoInitialize(RO_INIT_MULTITHREADED),"Capture WinRT startup");} ~Apartment(){RoUninitialize();} } apartment;
  struct Signal { HANDLE handle=CreateEventW(nullptr,FALSE,FALSE,nullptr); ~Signal(){if(handle)CloseHandle(handle);} };
  struct Surface {
    RECT bounds{};
    capture::PixelRect placed{}; // Unscaled frame-space position.
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
    winrt::event_token arrived{};
    Com<ID3D11Texture2D> staging;
    std::vector<uint8_t> pixels;
    bool ready=false;
    // Scaled path: `pixels` holds a frame not yet resampled into the DIB.
    bool fresh=false;
    // Direct path: converted into the CPU NV12 by this render().
    bool converted=false;
    // capture-dirty-v1 (capture_dirty.hpp; WARP device only): the session
    // reports dirty rectangles, the staging texture holds a complete frame
    // (false after GPU ticks consumed frames without a readback), and the
    // sweep band of the next partial tick.
    bool dirtyReporting=false,stagingComplete=false;int sweepIndex=0;
    std::unique_ptr<capture::AreaScaler> scaler;
    ~Surface(){try{if(pool)pool.FrameArrived(arrived);if(session)session.Close();if(pool)pool.Close();}catch(...) {}}
  };
  std::shared_ptr<Signal> signal=std::make_shared<Signal>();
  Com<ID3D11Device> device;
  Com<ID3D11DeviceContext> context;
  std::vector<std::unique_ptr<Surface>> surfaces;
  bool warp=false;
  std::optional<uint64_t> adapter;
  capture::Nv12CursorPatch cursor;
  // gpu-capture-v1 (gpu_capture.hpp): the armed composer (null: CPU path only),
  // the CPU picture being stale (GPU ticks consumed frames: the next CPU tick
  // reads every surface back from its GPU copy), a GPU tick that failed (the
  // composer goes after that refresh) and a cursor the GPU cannot compose.
  std::unique_ptr<gpucapture::FrameComposer> gpu;
  gpucapture::CursorOverlayRenderer overlay;
  bool refreshAll=false,gpuFailed=false;
  HCURSOR blockedCursor=nullptr;
  Modern(RECT source,ID3D11Device* reuse) {
    using namespace winrt::Windows::Graphics::Capture;
    requireSupported();
    // A follow keeps the previous capture's device (an encoder may be on it).
    HRESULT hr=E_FAIL;
    if(reuse&&SUCCEEDED(reuse->GetDeviceRemovedReason())){device=reuse;device->GetImmediateContext(&context);hr=S_OK;}
    if(FAILED(hr))hr=createCaptureHardwareDevice(device,context);
    if(FAILED(hr)){warp=true;check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Capture WARP device");}
    Com<IDXGIDevice> dxgi;check(device.As(&dxgi),"Capture DXGI device");winrt::com_ptr<IInspectable> inspectable;
    {Com<IDXGIAdapter> actual;Com<IDXGIAdapter1> described;DXGI_ADAPTER_DESC1 description{};
      if(SUCCEEDED(dxgi->GetAdapter(&actual))&&SUCCEEDED(actual.As(&described))&&SUCCEEDED(described->GetDesc1(&description))){
        // The Basic Render Driver however the device came about (a kept WARP
        // device, the default adapter being the software one) is WARP: no GPU
        // path, no GPU thread priority, dirty-rectangle readback.
        if(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)warp=true;
        if(!warp){const auto luid=(uint64_t(uint32_t(description.AdapterLuid.HighPart))<<32)|description.AdapterLuid.LowPart;if(luid)adapter=luid;}}}
    check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),inspectable.put()),"Capture WinRT D3D device");
    const auto winDevice=inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    std::vector<std::pair<HMONITOR,RECT>> monitors;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT bounds,LPARAM value)->BOOL{reinterpret_cast<decltype(monitors)*>(value)->push_back({monitor,*bounds});return TRUE;},reinterpret_cast<LPARAM>(&monitors));
    const auto factory=winrt::get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
    uint64_t covered=0;
    for(const auto& [monitor,bounds]:monitors){RECT overlap{};if(!IntersectRect(&overlap,&source,&bounds))continue;
      // Selected monitors must be complete; never capture a neighbouring display
      // through an overlapping or partially clipped topology.
      if(!EqualRect(&overlap,&bounds))throw std::runtime_error("Selected display rectangle cuts a monitor");
      auto surface=std::make_unique<Surface>();surface->bounds=bounds;
      surface->placed={int(bounds.left-source.left),int(bounds.top-source.top),int(bounds.right-source.left),int(bounds.bottom-source.top)};
      check(factory->CreateForMonitor(monitor,winrt::guid_of<GraphicsCaptureItem>(),winrt::put_abi(surface->item)),"Capture selected monitor");
      const auto size=surface->item.Size();if(size.Width!=bounds.right-bounds.left||size.Height!=bounds.bottom-bounds.top)throw std::runtime_error("Capture monitor pixel geometry mismatch");
      surface->pool=Direct3D11CaptureFramePool::CreateFreeThreaded(winDevice,winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
      const auto shared=signal;surface->arrived=surface->pool.FrameArrived([shared](auto const&,auto const&){SetEvent(shared->handle);});
      surface->session=surface->pool.CreateCaptureSession(surface->item);surface->session.IsCursorCaptureEnabled(false);
      // The loop's own pacing caps the rate; WGC's 16 ms default would hold a
      // jittered frame until the next composition (capture_session_interval.hpp).
      deliverEveryComposedFrame(surface->session);
      // capture-dirty-v1: a WARP device copies with the CPU, so the CPU path
      // reads back only the rectangles WGC reports as changed. Not on a GPU
      // device (capture_dirty.hpp: its reports were stale on 23% of frames).
      if(warp&&Capture::dirtyReadbackEnabled)surface->dirtyReporting=enableDirtyRegionReports(surface->session);
      surface->session.StartCapture();
      covered+=uint64_t(size.Width)*size.Height;surfaces.push_back(std::move(surface));
    }
    if(surfaces.empty()||surfaces.size()>16||covered!=uint64_t(source.right-source.left)*(source.bottom-source.top))throw std::runtime_error("Selected displays are not contiguous");
  }
  static void requireSupported(){if(!graphicsCaptureSupported())throw std::runtime_error("Windows Graphics Capture unavailable");}
  // Asks the session to report dirty rectangles (24H2, IGraphicsCaptureSession4);
  // true only when it confirms the mode. Older Windows: false, full readback.
  static bool enableDirtyRegionReports(const winrt::Windows::Graphics::Capture::GraphicsCaptureSession& session){
    using namespace winrt::Windows::Graphics::Capture;
    const auto reporting=session.try_as<IGraphicsCaptureSession4>();if(!reporting)return false;
    try{reporting.DirtyRegionMode(GraphicsCaptureDirtyRegionMode::ReportOnly);return reporting.DirtyRegionMode()==GraphicsCaptureDirtyRegionMode::ReportOnly;}catch(...){return false;}
  }
  // Appends `frame`'s dirty rectangles (surface coordinates). false: the frame
  // cannot say (no interface, reading failed), so this tick reads everything.
  static bool collectDirtyRegions(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame& frame,std::vector<capture::PixelRect>& out){
    const auto reporting=frame.try_as<winrt::Windows::Graphics::Capture::IDirect3D11CaptureFrame2>();if(!reporting)return false;
    try{for(const auto& r:reporting.DirtyRegions())if(r.Width>0&&r.Height>0)out.push_back({r.X,r.Y,r.X+r.Width,r.Y+r.Height});}catch(...){return false;}
    return true;
  }
  capture::DirtyPolicy dirtyPolicy;
  // Direct NV12 conversion requires the unscaled path and even-aligned
  // surfaces, so every 2x2 chroma block lies inside exactly one surface.
  bool evenLayout(int width,int height,RECT source)const{
    if(width!=source.right-source.left||height!=source.bottom-source.top)return false;
    return std::all_of(surfaces.begin(),surfaces.end(),[](const auto& s){return capture::evenRect(s->placed);});
  }
  // Calls f(pixelAtOverlapOrigin,rowPitch,overlap) with each surface's latest
  // readback (the staging texture keeps the last copied frame).
  template<class F> void visitBase(const capture::PixelRect& rect,F&& f){
    for(const auto& surface:surfaces){
      const auto overlap=capture::intersect(rect,surface->placed);if(!overlap)continue;
      D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(surface->staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Capture frame readback");
      const auto* origin=static_cast<const uint8_t*>(mapped.pData)+size_t(overlap->top-surface->placed.top)*mapped.RowPitch+size_t(overlap->left-surface->placed.left)*4;
      try{f(origin,size_t(mapped.RowPitch),*overlap);}catch(...){context->Unmap(surface->staging.Get(),0);throw;}
      context->Unmap(surface->staging.Get(),0);
    }
  }
  // Composed pixels of the last converted frame: base readback outside the
  // cursor patch, the composed cursor canvas inside it. Maps each surface once.
  void visitComposed(std::span<const capture::PixelRect> rects,const capture::ComposedVisitor& visitor){
    std::vector<D3D11_MAPPED_SUBRESOURCE> mapped(surfaces.size());size_t count=0;
    struct Unmap{Modern& owner;size_t& count;~Unmap(){for(size_t n=0;n<count;++n)owner.context->Unmap(owner.surfaces[n]->staging.Get(),0);}}unmap{*this,count};
    std::vector<capture::ComposedSurface> bases;
    for(;count<surfaces.size();++count){
      if(!surfaces[count]->staging)throw std::runtime_error("No initial virtual display frame");
      check(context->Map(surfaces[count]->staging.Get(),0,D3D11_MAP_READ,0,&mapped[count]),"Capture frame readback");
      bases.push_back({surfaces[count]->placed,static_cast<const uint8_t*>(mapped[count].pData),size_t(mapped[count].RowPitch)});
    }
    capture::visitComposedPieces(rects,bases,cursor.patch(),cursor.patchPixels(),cursor.patchStride(),visitor);
  }
  // Scaled capture (encoder smaller than the source): lossless tiles use the
  // UNSCALED source, i.e. the surface copies the scaled DIB was drawn from plus
  // the cursor composed at source resolution with the same DrawIconEx/GDI state.
  capture::CursorComposer sourceCursor;
  std::vector<capture::ComposedSurface> sourceBases()const{
    std::vector<capture::ComposedSurface> bases;
    for(const auto& surface:surfaces){if(surface->pixels.size()!=size_t(surface->placed.width())*surface->placed.height()*4)throw std::runtime_error("No initial virtual display frame");bases.push_back({surface->placed,surface->pixels.data(),size_t(surface->placed.width())*4});}
    return bases;
  }
  void composeSourceCursor(const std::optional<capture::CursorDraw>& draw,int width,int height,HDC compatible){sourceCursor.compose(draw,width,height,sourceBases(),compatible);}
  void visitSourceComposed(std::span<const capture::PixelRect> rects,const capture::ComposedVisitor& visitor){
    capture::visitComposedPieces(rects,sourceBases(),sourceCursor.patch(),sourceCursor.pixels(),sourceCursor.stride(),visitor);
  }
  // A non-empty nv12 selects direct conversion: each fresh surface is
  // converted straight from its mapped readback into its place in the frame,
  // without the intermediate BGRA copies; target/pixels are then untouched.
  // gpu-capture-v1: after GPU ticks, every surface this render() did not
  // convert is read back from its GPU copy (the CPU NV12 and the staging
  // copies predate them), and the CPU cursor patch starts over.
  void refresh(int width,int height,std::span<uint8_t> nv12,double& convertMs,double* readbackMs){
    for(size_t index=0;index<surfaces.size();++index){auto& surface=surfaces[index];if(surface->converted)continue;
      if(!gpu||!gpu->hasFrame(index))throw std::runtime_error("No initial virtual display frame");
      ID3D11Texture2D* copy=gpu->latest(index);
      if(!surface->staging){D3D11_TEXTURE2D_DESC desc{};copy->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;check(device->CreateTexture2D(&desc,nullptr,&surface->staging),"Capture readback texture");}
      const auto readbackBegan=readbackMs?Clock::now():Clock::time_point{};
      context->CopyResource(surface->staging.Get(),copy);surface->stagingComplete=true;D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(surface->staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Capture frame readback");
      if(readbackMs)*readbackMs+=std::chrono::duration<double,std::milli>(Clock::now()-readbackBegan).count();
      const auto began=Clock::now();
      try{capture::convertBlockToNv12(nv12,width,height,static_cast<const uint8_t*>(mapped.pData),mapped.RowPitch,surface->placed);}catch(...){context->Unmap(surface->staging.Get(),0);throw;}
      convertMs+=std::chrono::duration<double,std::milli>(Clock::now()-began).count();
      context->Unmap(surface->staging.Get(),0);surface->converted=true;
    }
    cursor.reset();refreshAll=false;
  }
  // gpuTick: the frames of this tick only go to the composer (no readback).
  bool render(HDC target,std::span<uint8_t> pixels,int width,int height,RECT source,std::span<uint8_t> nv12,double& convertMs,double* readbackMs=nullptr,bool gpuTick=false) {
    bool updated=false;const bool direct=!nv12.empty();
    for(auto& surface:surfaces)surface->converted=false;
    auto read=[&]{
      // Frames taken below must not wake the capture loop again (frameEvent()).
      ResetEvent(signal->handle);
      for(size_t index=0;index<surfaces.size();++index){auto& surface=surfaces[index];
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame latest{nullptr};
        // capture-dirty-v1: the dirty rectangles of every frame taken; their
        // union describes the newest frame against the last one read back.
        std::vector<capture::PixelRect> dirty;bool dirtyKnown=surface->dirtyReporting;
        for(int n=0;n<3;++n){auto frame=surface->pool.TryGetNextFrame();if(!frame)break;if(dirtyKnown)dirtyKnown=collectDirtyRegions(frame,dirty);if(latest)latest.Close();latest=std::move(frame);}
        if(!latest)continue;
        const auto size=latest.ContentSize();const int sw=surface->bounds.right-surface->bounds.left,sh=surface->bounds.bottom-surface->bounds.top;
        if(size.Width!=sw||size.Height!=sh)throw CaptureGeometryChanged();
        auto access=latest.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();Com<ID3D11Texture2D> texture;
        check(access->GetInterface(IID_PPV_ARGS(&texture)),"Capture frame texture");
        // gpu-capture-v1: an armed capture keeps every frame on the GPU too
        // (a GPU copy); a GPU tick reads nothing back.
        if(gpu)gpu->surfaceFrame(index,texture.Get());
        // The copy is submitted before the frame goes back to its pool (the CPU
        // path below returns it only after Map waited for its copy).
        if(gpuTick){context->Flush();latest.Close();surface->ready=true;surface->stagingComplete=false;updated=true;continue;}
        if(!surface->staging){D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;check(device->CreateTexture2D(&desc,nullptr,&surface->staging),"Capture readback texture");surface->stagingComplete=false;}
        // The pieces of the surface this tick reads back and converts: the
        // whole surface, or the merged dirty rectangles plus the sweep band.
        // The staging texture and the NV12 then hold the complete frame.
        const auto plan=capture::planDirtyCopy({surface->stagingComplete,dirtyKnown,dirty,sw,sh,0,surface->sweepIndex,dirtyPolicy});
        std::vector<capture::PixelRect> pieces;
        if(plan.full)pieces.push_back({0,0,sw,sh});else{pieces=plan.rects;if(plan.sweep)pieces.push_back(*plan.sweep);++surface->sweepIndex;}
        const auto readbackBegan=readbackMs?Clock::now():Clock::time_point{};
        if(plan.full)context->CopyResource(surface->staging.Get(),texture.Get());
        else for(const auto& r:pieces){const D3D11_BOX box{UINT(r.left),UINT(r.top),0,UINT(r.right),UINT(r.bottom),1};context->CopySubresourceRegion(surface->staging.Get(),0,UINT(r.left),UINT(r.top),0,texture.Get(),0,&box);}
        surface->stagingComplete=true;
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(surface->staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Capture frame readback");
        if(readbackMs)*readbackMs+=std::chrono::duration<double,std::milli>(Clock::now()-readbackBegan).count();
        const auto* base=static_cast<const uint8_t*>(mapped.pData);
        if(direct){
          // Map waited for the copy, so the pool frame can be returned first.
          latest.Close();const auto began=Clock::now();
          try{for(const auto& r:pieces)capture::convertBlockToNv12(nv12,width,height,base+size_t(r.top)*mapped.RowPitch+size_t(r.left)*4,mapped.RowPitch,{surface->placed.left+r.left,surface->placed.top+r.top,surface->placed.left+r.right,surface->placed.top+r.bottom});}catch(...){context->Unmap(surface->staging.Get(),0);throw;}
          convertMs+=std::chrono::duration<double,std::milli>(Clock::now()-began).count();
          context->Unmap(surface->staging.Get(),0);surface->converted=true;
        } else {
          surface->pixels.resize(size_t(sw)*sh*4);
          for(const auto& r:pieces)for(int y=r.top;y<r.bottom;++y)std::memcpy(surface->pixels.data()+(size_t(y)*sw+size_t(r.left))*4,base+size_t(y)*mapped.RowPitch+size_t(r.left)*4,size_t(r.width())*4);
          context->Unmap(surface->staging.Get(),0);latest.Close();surface->fresh=true;
        }
        surface->ready=true;updated=true;
      }
    };
    read();
    for(int wait=0;wait<10&&std::any_of(surfaces.begin(),surfaces.end(),[](const auto& s){return !s->ready;});++wait){WaitForSingleObject(signal->handle,100);read();}
    if(direct){for(const auto& surface:surfaces)if(!surface->ready)throw std::runtime_error("No initial virtual display frame");
      if(!gpuTick&&refreshAll){refresh(width,height,nv12,convertMs,readbackMs);updated=true;}
      return updated;}
    (void)target;
    const bool unscaled=width==source.right-source.left&&height==source.bottom-source.top;
    // The viewport normally matches the virtual output. GDI's HALFTONE path
    // still performs costly work at 1:1, so preserve those pixels directly.
    // Keep the cached base surface separate from the cursor drawn afterwards.
    // Scaled (encoder smaller than the output): the DIB keeps the resampled
    // base between ticks (Capture::frame puts back what the cursor covered),
    // so only a surface with a new frame is resampled, by the area-average
    // AreaScaler instead of a full StretchDIBits HALFTONE on every tick.
    GdiFlush();
    for(const auto& surface:surfaces){if(!surface->ready)throw std::runtime_error("No initial virtual display frame");
      const int sw=surface->bounds.right-surface->bounds.left,sh=surface->bounds.bottom-surface->bounds.top;
      const int left=int((int64_t(surface->bounds.left)-source.left)*width/(int64_t(source.right)-source.left)),top=int((int64_t(surface->bounds.top)-source.top)*height/(int64_t(source.bottom)-source.top));
      const int right=int((int64_t(surface->bounds.right)-source.left)*width/(int64_t(source.right)-source.left)),bottom=int((int64_t(surface->bounds.bottom)-source.top)*height/(int64_t(source.bottom)-source.top));
      if(unscaled){capture::copyUnscaledSurface(pixels,width,height,surface->pixels,sw,sh,left,top);continue;}
      if(!surface->fresh)continue;
      if(left<0||top<0||right>width||bottom>height||right<=left||bottom<=top||surface->pixels.size()!=size_t(sw)*sh*4)throw std::runtime_error("Capture frame compose failed");
      if(!surface->scaler||surface->scaler->sourceWidth()!=sw||surface->scaler->sourceHeight()!=sh||surface->scaler->targetWidth()!=right-left||surface->scaler->targetHeight()!=bottom-top)
        surface->scaler=std::make_unique<capture::AreaScaler>(sw,sh,right-left,bottom-top);
      surface->scaler->scale(surface->pixels.data(),size_t(sw)*4,pixels.data()+(size_t(top)*width+size_t(left))*4,size_t(width)*4);
      surface->fresh=false;
    }
    return updated;
  }
};
#ifdef MULTITAP_CAPTURE_PATH_PROBE
bool captureProbeLegacyPath=false;
#endif
static size_t captureBytes(int width,int height){
  validateFrameMemory(width,height,4,3);
  return checkedFrameLayout(uint64_t(width),uint64_t(height))->nv12Bytes;
}
static int captureSourceSide(LONG first,LONG last){const auto side=int64_t(last)-first;if(side<16||side>INT32_MAX)throw std::runtime_error("Invalid capture source rectangle");return int(side);}
Capture::Capture(int width,int height):Capture(width,height,RECT{0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)}){}
Capture::Capture(int width, int height,RECT source,std::shared_ptr<ID3D11Device> reuse) : width_(width), height_(height), sourceWidth_(captureSourceSide(source.left,source.right)), sourceHeight_(captureSourceSide(source.top,source.bottom)),sourceLeft_(source.left),sourceTop_(source.top), nv12_(captureBytes(width,height)) {
  if (!interactiveDesktopAvailable()) throw std::runtime_error("Interactive desktop is locked, secure or unavailable");
  if(sourceWidth_<16||sourceHeight_<16||!checkedFrameLayout(uint64_t(sourceWidth_),uint64_t(sourceHeight_),false))throw std::runtime_error("Invalid capture source rectangle");
  validateFrameMemory(sourceWidth_,sourceHeight_,4,0,false);
  try {
  screen_ = GetDC(nullptr); memory_ = CreateCompatibleDC(screen_);
  BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
  bitmap_ = CreateDIBSection(screen_, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels_), nullptr, 0);
  if (!screen_ || !memory_ || !bitmap_ || !pixels_) throw std::runtime_error("Desktop capture is unavailable");
  previous_ = SelectObject(memory_, bitmap_); SetStretchBltMode(memory_,HALFTONE);SetBrushOrgEx(memory_,0,0,nullptr);
  try{modern_=std::make_unique<Modern>(source,reuse.get());}catch(const winrt::hresult_error& error){std::ostringstream code;code<<"wgc_hresult_0x"<<std::hex<<uint32_t(error.code());backendError_=code.str();modern_.reset();}catch(const std::exception& error){backendError_=error.what();modern_.reset();}
  // Unscaled even layouts convert straight from WGC readback and compose only
  // the cursor patch; otherwise (scaling, odd placement) keep the DIB path.
  direct_=modern_&&modern_->evenLayout(width,height,source)&&
          modern_->cursor.reserve(std::max(64,GetSystemMetrics(SM_CXCURSOR)+2),std::max(64,GetSystemMetrics(SM_CYCURSOR)+2),screen_);
#ifdef MULTITAP_CAPTURE_PATH_PROBE
  if(captureProbeLegacyPath)direct_=false; // Probe-only A/B against the DIB path.
#endif
  } catch (...) { clear(); throw; }
}
void Capture::clear() noexcept { modern_.reset(); if (memory_ && previous_) SelectObject(memory_, previous_); if (bitmap_) DeleteObject(bitmap_); if (memory_) DeleteDC(memory_); if (screen_) ReleaseDC(nullptr, screen_); bitmap_ = nullptr; memory_ = screen_ = nullptr; previous_ = nullptr; pixels_ = nullptr; }
Capture::~Capture() { clear(); }
const char* Capture::backend()const{return modern_?(modern_->warp?"wgc-warp":"wgc-d3d11"):"gdi";}
bool Capture::dirtyReadback()const{return modern_&&!modern_->surfaces.empty()&&std::all_of(modern_->surfaces.begin(),modern_->surfaces.end(),[](const auto& s){return s->dirtyReporting;});}
HANDLE Capture::frameEvent()const{return modern_?modern_->signal->handle:nullptr;}
std::optional<uint64_t> Capture::adapterLuid()const{return modern_?modern_->adapter:std::nullopt;}
bool Capture::raiseGpuThreadPriority(){if(!modern_||modern_->warp)return false;Com<IDXGIDevice> dxgi;return SUCCEEDED(modern_->device.As(&dxgi))&&SUCCEEDED(dxgi->SetGPUThreadPriority(7));}
ID3D11Device* Capture::gpuDevice()const{return modern_&&!modern_->warp?modern_->device.Get():nullptr;}
std::shared_ptr<ID3D11Device> Capture::sharedDevice()const{
  if(!gpuDevice())return {};
  Com<ID3D11Device> keep=modern_->device;ID3D11Device* raw=keep.Get();
  return std::shared_ptr<ID3D11Device>(raw,[keep](ID3D11Device*)mutable{keep.Reset();});
}
std::optional<uint32_t> Capture::armGpuOutput(ID3D11Device* encoderDevice,bool hardwareEncoder,bool textureBackend){
  using namespace gpucapture;
  if(modern_&&modern_->gpu)return gpuPathRecord(gpuPathActive);
  const auto decision=gpuArmDecision({hardwareEncoder,textureBackend,gpuDevice()!=nullptr,direct_,encoderDevice&&encoderDevice==gpuDevice()});
  if(!decision)return std::nullopt;
  if(*decision!=gpuPathActive)return gpuPathRecord(*decision);
  try{
    std::vector<capture::PixelRect> rects;for(const auto& surface:modern_->surfaces)rects.push_back(surface->placed);
    auto composer=std::make_unique<FrameComposer>(modern_->device.Get(),width_,height_,std::move(rects));
    // Surfaces the CPU path already read start from their readback copy.
    for(size_t index=0;index<modern_->surfaces.size();++index)if(modern_->surfaces[index]->staging)composer->surfaceFrame(index,modern_->surfaces[index]->staging.Get());
    modern_->gpu=std::move(composer);modern_->gpuFailed=false;modern_->refreshAll=false;
  }catch(const GpuFailure& failure){return gpuPathRecord(gpuPathConverter,failure.step,failure.result);}
  catch(const std::bad_alloc&){return gpuPathRecord(gpuPathConverter,GpuStep::none,E_OUTOFMEMORY);}
  return gpuPathRecord(gpuPathActive);
}
ID3D11Texture2D* Capture::gpuPicture()const{return gpuTick_&&modern_&&modern_->gpu?modern_->gpu->picture():nullptr;}
EncoderPicture Capture::encoderPicture(){
  if(!gpuPicture())return EncoderPicture{nv12_.data(),nullptr,{}};
  // A CPU encoder (or a fallback) reads the GPU picture into the CPU buffer.
  return EncoderPicture{nullptr,modern_->gpu->picture(),[this]()->const uint8_t*{modern_->gpu->read(nv12_);return nv12_.data();}};
}
// One GPU tick: this tick's WGC frames go to the composer, the cursor sample is
// read as on the CPU path and composed over the picture on the GPU. false: the
// tick must take the CPU path instead (an uncomposable cursor, or a failure,
// which ends the GPU path of this capture); render() then reads every surface
// back from its GPU copy. WGC and geometry errors propagate as on the CPU path.
bool Capture::gpuFrame(Clock::time_point began){
  using namespace gpucapture;
  double unusedMs=0;
  try{frameUpdated_=modern_->render(memory_,{pixels_,size_t(width_)*height_*4},width_,height_,RECT{sourceLeft_,sourceTop_,sourceLeft_+sourceWidth_,sourceTop_+sourceHeight_},std::span<uint8_t>(nv12_),unusedMs,nullptr,true);}
  catch(const winrt::hresult_error& error){std::ostringstream code;code<<"Windows capture failed (0x"<<std::hex<<uint32_t(error.code())<<")";throw std::runtime_error(code.str());}
  // The encoder last received a CPU picture (or none): this one counts as changed.
  const bool afterCpu=!modern_->refreshAll;
  modern_->refreshAll=true;
  CURSORINFO cursor{sizeof(CURSORINFO)};
  const bool cursorKnown=GetCursorInfo(&cursor)!=FALSE;
  cursorSample_=cursorKnown?CursorSample{true,cursor.flags,cursor.hCursor,cursor.ptScreenPos}:CursorSample{};
  // Same draw as the direct CPU path (geometry, hotspot, size).
  std::optional<capture::CursorDraw> draw;
  if(capture::drawsCursor(composeCursor_,cursorKnown,cursor.flags)){
    ICONINFO icon{};
    if(GetIconInfo(cursor.hCursor,&icon)){
      const auto shown=videoCursorDraw(cursor.hCursor,cursor.ptScreenPos,icon,width_,height_);
      draw=capture::CursorDraw{shown.shape,shown.x,shown.y,shown.width,shown.height};
      if(icon.hbmMask)DeleteObject(icon.hbmMask);if(icon.hbmColor)DeleteObject(icon.hbmColor);
    }
  }
  const auto converting=Clock::now();
  try{
    const CursorOverlay* composed=draw?modern_->overlay.render(*draw,width_,height_,screen_):nullptr;
    gpuChanged_=modern_->gpu->compose(composed)||afterCpu;
  }catch(const CursorUnrepresentable&){modern_->blockedCursor=cursor.hCursor;return false;}
  catch(const GpuFailure& failure){gpuFailure_=gpuPathRecord(gpuPathConvertFailed,failure.step,failure.result);modern_->gpuFailed=true;return false;}
  catch(const std::bad_alloc&){gpuFailure_=gpuPathRecord(gpuPathConvertFailed,GpuStep::none,E_OUTOFMEMORY);modern_->gpuFailed=true;return false;}
  composedValid_=false;gpuTick_=true;
  const auto ended=Clock::now();
  lastConvertMs_=std::chrono::duration<double,std::milli>(ended-converting).count();
  lastCaptureMs_=std::chrono::duration<double,std::milli>(converting-began).count();
  return true;
}
const std::vector<uint8_t>& Capture::frame() {
  const auto began = Clock::now();
  if (!interactiveDesktopAvailable()) throw std::runtime_error("Interactive desktop is locked, secure or unavailable");
  double directConvertMs = 0, readbackMs = 0;
  lastReadbackMs_.reset();
  gpuTick_ = gpuChanged_ = false;
  // gpu-capture-v1: an armed capture whose consumer reads no CPU pixels this
  // tick, unless the cursor shown last tick cannot be composed on the GPU.
  if (gpuWanted_ && direct_ && modern_ && modern_->gpu && !modern_->gpuFailed &&
      !(cursorSample_.handle && cursorSample_.handle == modern_->blockedCursor && capture::drawsCursor(composeCursor_, cursorSample_.known, cursorSample_.flags)) &&
      gpuFrame(began)) return nv12_;
  // Scaled path: the DIB keeps last tick's resampled base; put back the
  // pixels the cursor drawn last tick covered before resampling new frames.
  if (composedAtSource()) restoreScaledCursor();
  if(modern_){try{frameUpdated_=modern_->render(memory_,{pixels_,size_t(width_)*height_*4},width_,height_,RECT{sourceLeft_,sourceTop_,sourceLeft_+sourceWidth_,sourceTop_+sourceHeight_},direct_?std::span<uint8_t>(nv12_):std::span<uint8_t>(),directConvertMs,observeReadback_?&readbackMs:nullptr);if(observeReadback_)lastReadbackMs_=readbackMs;}catch(const winrt::hresult_error& error){std::ostringstream code;code<<"Windows capture failed (0x"<<std::hex<<uint32_t(error.code())<<")";throw std::runtime_error(code.str());}}
  else {if (!StretchBlt(memory_, 0, 0, width_, height_, screen_, sourceLeft_, sourceTop_, sourceWidth_, sourceHeight_, SRCCOPY | CAPTUREBLT)) throw std::runtime_error("Desktop capture failed; check the interactive desktop");frameUpdated_=true;}
  // gpu-capture-v1: a failed GPU path is dropped once its last picture was read back.
  if (modern_ && modern_->gpu && modern_->gpuFailed && !modern_->refreshAll) modern_->gpu.reset();
  CURSORINFO cursor{sizeof(CURSORINFO)};
  bool cursorKnown = GetCursorInfo(&cursor) != FALSE;
  cursorSample_ = cursorKnown ? CursorSample{true, cursor.flags, cursor.hCursor, cursor.ptScreenPos} : CursorSample{};
  // cursor-v1: a separated session draws no cursor in any path below; the
  // previous patch (if any) is restored from the base like a hidden cursor.
  const bool drawCursor = capture::drawsCursor(composeCursor_, cursorKnown, cursor.flags);
  std::array<uint64_t, 6> cursorStamp{};
  if (direct_) {
    // Same cursor query, geometry and DrawIconEx call as the DIB path below,
    // composed over the exact base pixels of the cursor's even patch.
    std::optional<capture::CursorDraw> draw; ICONINFO icon{};
    if (drawCursor) {
      if (GetIconInfo(cursor.hCursor, &icon)) {
        const auto shown = videoCursorDraw(cursor.hCursor, cursor.ptScreenPos, icon, width_, height_);
        draw = capture::CursorDraw{shown.shape, shown.x, shown.y, shown.width, shown.height};
      } else cursorKnown = false;
    }
    const auto releaseIcon = [&] { if (icon.hbmMask) DeleteObject(icon.hbmMask); if (icon.hbmColor) DeleteObject(icon.hbmColor); icon.hbmMask = icon.hbmColor = nullptr; };
    composedValid_ = false; cursorBefore_ = modern_->cursor.patch();
    try { if (!modern_->cursor.apply(nv12_, width_, height_, draw, [&](const capture::PixelRect& rect, auto&& visit) { modern_->visitBase(rect, visit); }, screen_)) cursorKnown = false; }
    catch (...) { releaseIcon(); throw; }
    releaseIcon();
    cursorAfter_ = modern_->cursor.patch(); composedValid_ = true;
    const auto ended = Clock::now();
    lastConvertMs_ = directConvertMs;
    lastCaptureMs_ = std::max(0.0, std::chrono::duration<double,std::milli>(ended-began).count() - directConvertMs);
    return nv12_;
  }
  composedValid_ = false; cursorBefore_ = cursorAfter_; cursorAfter_.reset();
  const bool atSource = composedAtSource(); std::optional<capture::CursorDraw> sourceDraw;
  if (drawCursor) {
    ICONINFO icon{}; if (GetIconInfo(cursor.hCursor, &icon)) {
      const auto shown = videoCursorDraw(cursor.hCursor, cursor.ptScreenPos, icon, width_, height_);
      const int x = shown.x, y = shown.y, cursorWidth = shown.width, cursorHeight = shown.height;
      if (int64_t(x) + cursorWidth > 0 && int64_t(y) + cursorHeight > 0 && x < width_ && y < height_)
        cursorStamp = {uint64_t(reinterpret_cast<uintptr_t>(shown.shape)), uint64_t(uint32_t(x)), uint64_t(uint32_t(y)), uint64_t(cursorWidth), uint64_t(cursorHeight), 1};
      // DrawIconEx writes only inside its destination rectangle.
      if (atSource) {
        // The source-resolution picture (lossless tiles) gets the cursor at its shown size in source pixels.
        const auto atSourceSize = videoCursorDraw(cursor.hCursor, cursor.ptScreenPos, icon, sourceWidth_, sourceHeight_);
        sourceDraw = capture::CursorDraw{atSourceSize.shape, atSourceSize.x, atSourceSize.y, atSourceSize.width, atSourceSize.height}; saveScaledCursor(x, y, cursorWidth, cursorHeight);
      }
      else cursorAfter_ = capture::evenCursorPatch(x, y, cursorWidth, cursorHeight, width_, height_);
      if (!DrawIconEx(memory_, x, y, shown.shape, cursorWidth, cursorHeight, 0, nullptr, DI_NORMAL)) cursorKnown = false;
      if (icon.hbmMask) DeleteObject(icon.hbmMask); if (icon.hbmColor) DeleteObject(icon.hbmColor);
    } else cursorKnown = false;
  }
  GdiFlush();
  if (atSource) { modern_->composeSourceCursor(sourceDraw, sourceWidth_, sourceHeight_, screen_); cursorAfter_ = modern_->sourceCursor.patch(); }
  const auto converted = Clock::now();
  const bool eligible = modern_ && !frameUpdated_ && cursorKnown && cursorStampValid_ && cursorStamp == cursorStamp_;
  if (!conversionCache_.reuse(pixels_, size_t(width_) * height_ * 4, eligible))
    bgraToNv12(pixels_, nv12_.data(), width_, height_);
  cursorStamp_ = cursorStamp;
  cursorStampValid_ = cursorKnown;
  composedValid_ = true;
  const auto ended = Clock::now();
  lastCaptureMs_ = std::chrono::duration<double,std::milli>(converted-began).count();
  lastConvertMs_ = std::chrono::duration<double,std::milli>(ended-converted).count();
  return nv12_;
}
bool Capture::composedAtSource() const { return modern_ && !direct_ && (width_ != sourceWidth_ || height_ != sourceHeight_); }
Capture::VideoCursorDraw Capture::videoCursorDraw(HCURSOR handle, POINT position, const ICONINFO& icon, int width, int height) {
  if (!videoCursor_) videoCursor_ = std::make_unique<cursor::VideoCursor>();
  const auto shown = videoCursor_->shown(handle, position, icon);
  VideoCursorDraw draw;
  draw.x = int((int64_t(position.x) - sourceLeft_ - shown.hotspotX) * width / sourceWidth_);
  draw.y = int((int64_t(position.y) - sourceTop_ - shown.hotspotY) * height / sourceHeight_);
  draw.width = std::max(1, int(int64_t(shown.width) * width / sourceWidth_));
  draw.height = std::max(1, int(int64_t(shown.height) * height / sourceHeight_));
  draw.shape = videoCursor_->shape(handle, icon, draw.width, draw.height);
  return draw;
}
void Capture::saveScaledCursor(int x, int y, int width, int height) {
  scaledCursor_.reset();
  const int64_t left = std::max<int64_t>(0, x), top = std::max<int64_t>(0, y);
  const int64_t right = std::min<int64_t>(width_, int64_t(x) + width), bottom = std::min<int64_t>(height_, int64_t(y) + height);
  if (left >= right || top >= bottom) return;
  const capture::PixelRect rect{int(left), int(top), int(right), int(bottom)};
  const size_t rowBytes = size_t(rect.width()) * 4;
  scaledCursorPixels_.resize(rowBytes * size_t(rect.height()));
  for (int row = 0; row < rect.height(); ++row) std::memcpy(scaledCursorPixels_.data() + size_t(row) * rowBytes, pixels_ + (size_t(rect.top + row) * width_ + size_t(rect.left)) * 4, rowBytes);
  scaledCursor_ = rect;
}
void Capture::restoreScaledCursor() {
  if (!scaledCursor_) return;
  GdiFlush();
  const auto rect = *scaledCursor_; scaledCursor_.reset();
  const size_t rowBytes = size_t(rect.width()) * 4;
  for (int row = 0; row < rect.height(); ++row) std::memcpy(pixels_ + (size_t(rect.top + row) * width_ + size_t(rect.left)) * 4, scaledCursorPixels_.data() + size_t(row) * rowBytes, rowBytes);
}
int Capture::composedWidth() const { return composedAtSource() ? sourceWidth_ : width_; }
int Capture::composedHeight() const { return composedAtSource() ? sourceHeight_ : height_; }
void Capture::visitComposed(std::span<const capture::PixelRect> rects, const capture::ComposedVisitor& visitor) {
  if (!composedValid_ || !visitor) throw std::runtime_error("No composed capture frame");
  for (const auto& rect : rects) if (rect.empty() || rect.left < 0 || rect.top < 0 || rect.right > composedWidth() || rect.bottom > composedHeight()) throw std::runtime_error("Invalid composed capture rectangle");
  if (direct_) { modern_->visitComposed(rects, visitor); return; }
  if (composedAtSource()) { modern_->visitSourceComposed(rects, visitor); return; }
  // The DIB holds base + DrawIconEx cursor, exactly what bgraToNv12 converted.
  GdiFlush();
  const size_t stride = size_t(width_) * 4;
  for (const auto& rect : rects) visitor(pixels_ + size_t(rect.top) * stride + size_t(rect.left) * 4, stride, rect);
}
std::optional<std::vector<capture::PixelRect>> Capture::composedDirty() const {
  // Without a new surface frame only the previous/current cursor rectangles
  // can differ (WGC paths). GDI reads the screen every frame: anywhere.
  if (!composedValid_ || !modern_ || frameUpdated_) return std::nullopt;
  std::vector<capture::PixelRect> rects;
  if (cursorBefore_) rects.push_back(*cursorBefore_);
  if (cursorAfter_ && cursorAfter_ != cursorBefore_) rects.push_back(*cursorAfter_);
  return rects;
}
std::vector<uint8_t> mediaPacket(uint8_t kind, uint64_t ptsUs, const std::vector<uint8_t>& payload, bool key, bool recovery) {
  if (payload.empty() || payload.size() + 12 > 4 * 1024 * 1024) throw std::runtime_error("Media frame exceeds protocol bounds");
  std::vector<uint8_t> packet(12 + payload.size()); packet[0] = kind; packet[1] = uint8_t((key ? 1 : 0) | (recovery ? 2 : 0));
  for (int i = 0; i < 8; i++) packet[4 + i] = uint8_t(ptsUs >> (i * 8)); std::memcpy(packet.data() + 12, payload.data(), payload.size()); return packet;
}
namespace {
// The default render endpoint's changes (eRender, eConsole: the role Loopback opens). Windows calls it on
// one of its own threads; it only sets a flag and wakes the capture loop. Reference counted, as COM wants.
class DefaultRenderWatch final : public IMMNotificationClient {
 public:
  DefaultRenderWatch() : wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
  std::atomic<bool> changed{false};
  HANDLE wake() const { return wake_; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override { const ULONG left = --references_; if (!left) delete this; return left; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) { *out = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && role == eConsole) { changed = true; if (wake_) SetEvent(wake_); }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
 private:
  ~DefaultRenderWatch() { if (wake_) CloseHandle(wake_); }
  std::atomic<ULONG> references_{1};
  HANDLE wake_;
};
}
// One opened loopback client of the default render endpoint (eRender, eConsole). Opened again after
// a failure or a default device change (audio_capture_control.hpp); never throws.
struct Loopback::Impl {
  Com<IAudioClient> client; Com<IAudioCaptureClient> capture; WAVEFORMATEX* format = nullptr;
  bool ready = false, floating = false, events = false, started = false;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  AudioCaptureStep failedStep = AudioCaptureStep::none; HRESULT failedResult = S_OK;
  ~Impl() { close(); if (event) CloseHandle(event); }
  void close() {
    if (client && started) client->Stop();
    started = false; ready = false; events = false; capture.Reset(); client.Reset();
    if (format) { CoTaskMemFree(format); format = nullptr; }
  }
  bool fail(AudioCaptureStep step, HRESULT result) { failedStep = step; failedResult = result; close(); return false; }
  AudioCaptureSample sample() const {
    if (floating) return AudioCaptureSample::float32;
    return format->wBitsPerSample == 16 ? AudioCaptureSample::pcm16 : format->wBitsPerSample == 24 ? AudioCaptureSample::pcm24 : AudioCaptureSample::pcm32;
  }
  bool open() {
    close(); failedStep = AudioCaptureStep::none; failedResult = S_OK;
    Com<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) return fail(AudioCaptureStep::enumerator, result);
    Com<IMMDevice> device; result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(result)) return fail(AudioCaptureStep::endpoint, result);
    result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(result)) return fail(AudioCaptureStep::activate, result);
    result = client->GetMixFormat(&format);
    if (FAILED(result)) return fail(AudioCaptureStep::mixFormat, result);
    if (!format->nChannels || format->nChannels > 8 || format->nSamplesPerSec < 8000 || format->nSamplesPerSec > 192000) return fail(AudioCaptureStep::formatUnsupported, S_OK);
    GUID subtype{};
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) subtype = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat;
    floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || subtype == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    const bool pcm = format->wFormatTag == WAVE_FORMAT_PCM || subtype == KSDATAFORMAT_SUBTYPE_PCM;
    if (!(floating && format->wBitsPerSample == 32) && !(pcm && (format->wBitsPerSample == 16 || format->wBitsPerSample == 24 || format->wBitsPerSample == 32))) return fail(AudioCaptureStep::formatUnsupported, S_OK);
    // Event-driven where Windows supports it for loopback (Windows 10 1703 and later; earlier ones never
    // signal it, which the loop finds out and polls instead). A client that refuses it is made again without.
    if (event) {
      result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, format, nullptr);
      if (SUCCEEDED(result)) result = client->SetEventHandle(event);
      events = SUCCEEDED(result);
    }
    if (!events) {
      client.Reset();
      result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
      if (FAILED(result)) return fail(AudioCaptureStep::activate, result);
      result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, format, nullptr);
      if (FAILED(result)) return fail(AudioCaptureStep::initialize, result);
    }
    result = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(result)) return fail(AudioCaptureStep::service, result);
    ready = true; return true;
  }
};
Loopback::Loopback() : impl_(std::make_unique<Impl>()) { impl_->open(); }
Loopback::~Loopback() = default;
bool Loopback::available() const { return impl_->ready; }
uint32_t Loopback::failureRecord() const { return impl_->ready || impl_->failedStep == AudioCaptureStep::none ? 0 : audioCaptureFailed(impl_->failedStep, impl_->failedResult); }
void Loopback::run(std::atomic<bool>& stop, Clock::time_point start, const std::function<void(std::vector<uint8_t>)>& output, AudioCaptureWindow* window, OpusAudioEncoder* opus, const std::function<void(uint32_t)>& record) {
  if (!available()) return;
  Impl& impl = *impl_;
  AudioCaptureRestart restart; AudioPtsClock pts;
  const auto journal = [&](uint32_t value) { if (record && restart.record(Clock::now())) { try { record(value); } catch (...) {} } };
  // The default endpoint's changes for the life of this run (a missing watch only loses the prompt reopen).
  DefaultRenderWatch* watch = new DefaultRenderWatch();
  Com<IMMDeviceEnumerator> notifier; bool registered = false;
  {
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&notifier));
    if (SUCCEEDED(result)) result = notifier->RegisterEndpointNotificationCallback(watch);
    registered = SUCCEEDED(result);
    if (!registered) journal(audioCaptureFailed(AudioCaptureStep::notification, result));
  }
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  struct Release {
    Com<IMMDeviceEnumerator>& notifier; DefaultRenderWatch* watch; bool registered; HANDLE timer;
    ~Release() { if (registered) notifier->UnregisterEndpointNotificationCallback(watch); watch->Release(); if (timer) CloseHandle(timer); }
  } release{notifier, watch, registered, timer};
  LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);

  // The open client's shape and the resampler to 48 kHz stereo s16 (reset at each open).
  int channels = 0, bytesPerSample = 0, blockAlign = 0; uint32_t rate = 48000; bool floating = false;
  double next = 0, step = 1; uint64_t sourceFrame = 0; float previousLeft = 0, previousRight = 0;
  uint32_t eventsSeen = 0; bool discontinuity = true;
  const auto sendFrame = [&output](uint64_t ptsUs, std::vector<uint8_t> payload) { output(mediaPacket(2, ptsUs, payload)); };
  // audio-opus-v1: the loopback sends nothing while the VM is silent, so the
  // part of a frame it left behind goes once a packet period has passed without more.
  auto lastPacket = Clock::now();
  const auto value = [&](const BYTE* pointer) {
    if (floating) { float f = 0; std::memcpy(&f, pointer, 4); return std::isfinite(f) ? std::clamp(f, -1.0f, 1.0f) : 0.0f; }
    int32_t n = 0;
    if (bytesPerSample == 2) { int16_t s = 0; std::memcpy(&s, pointer, 2); return float(s) / 32768.0f; }
    if (bytesPerSample == 3) { n = int32_t(pointer[0]) | (int32_t(pointer[1]) << 8) | (int32_t(pointer[2]) << 16); if (n & 0x800000) n |= int32_t(0xff000000); return float(n) / 8388608.0f; }
    std::memcpy(&n, pointer, 4); return float(double(n) / 2147483648.0);
  };
  const auto flushHeld = [&] { if (opus && opus->held()) { try { opus->flush(sendFrame); } catch (...) {} } };
  // The client went (an error, a changed default device): what was held leaves, the client closes, and
  // the next open waits for the backoff.
  const auto broke = [&](AudioCaptureStep where, HRESULT result) {
    journal(audioCaptureFailed(where, result));
    flushHeld(); impl.close(); restart.failed(Clock::now()); pts.reset(); discontinuity = true;
  };
  const auto startCapture = [&]() -> bool {
    const HRESULT result = impl.client->Start();
    if (FAILED(result)) { broke(AudioCaptureStep::start, result); return false; }
    impl.started = true; restart.opened(Clock::now());
    const auto* format = impl.format;
    channels = format->nChannels; bytesPerSample = format->wBitsPerSample / 8; blockAlign = format->nBlockAlign; rate = format->nSamplesPerSec; floating = impl.floating;
    step = double(rate) / 48000.0; next = 0; sourceFrame = 0; previousLeft = previousRight = 0; eventsSeen = 0; discontinuity = true;
    journal(audioCaptureStarted(rate, uint32_t(channels), impl.events, impl.sample()));
    return true;
  };
  // Sleeps at most `period`: the capture's event, a default device change or the timer ends it.
  // True when the capture's own event did.
  const auto wait = [&](std::chrono::microseconds period, bool capture) {
    HANDLE handles[3]; DWORD count = 0;
    const bool withEvent = capture && impl.events && impl.event;
    if (withEvent) handles[count++] = impl.event;
    if (watch->wake()) handles[count++] = watch->wake();
    DWORD timeout = DWORD(period.count() / 1000 + 1);
    if (timer) { LARGE_INTEGER due{}; due.QuadPart = -std::max<LONGLONG>(1, LONGLONG(period.count()) * 10); if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) { handles[count++] = timer; timeout = 1000; } }
    if (!count) { std::this_thread::sleep_for(period); return false; }
    const DWORD result = WaitForMultipleObjects(count, handles, FALSE, timeout);
    if (timer) CancelWaitableTimer(timer);
    return withEvent && result == WAIT_OBJECT_0;
  };

  (void)startCapture();
  while (!stop) {
    if (watch->changed.exchange(false)) {
      journal(audioCaptureDeviceChanged(impl.ready));
      flushHeld(); impl.close(); restart.changed(Clock::now()); pts.reset(); discontinuity = true;
    }
    if (!impl.ready) {
      const auto now = Clock::now();
      if (now < restart.due()) { wait(std::min<std::chrono::microseconds>(std::chrono::duration_cast<std::chrono::microseconds>(restart.due() - now) + std::chrono::microseconds(1), std::chrono::milliseconds(10)), false); continue; }
      journal(audioCaptureRestarting(restart.attempt()));
      if (!impl.open()) { journal(audioCaptureFailed(impl.failedStep, impl.failedResult)); restart.failed(Clock::now()); continue; }
      if (!startCapture()) continue;
    }
    const bool byEvent = wait(audioWakePeriod(eventsSeen >= audioEventsProven), true);
    if (stop || watch->changed.load()) continue;
    UINT32 pending = 0; HRESULT result = impl.capture->GetNextPacketSize(&pending);
    if (FAILED(result)) { broke(AudioCaptureStep::packetSize, result); continue; }
    if (!pending) {
      if (opus && opus->held() && Clock::now() - lastPacket > std::chrono::milliseconds(20)) flushHeld();
      continue;
    }
    if (byEvent && eventsSeen < audioEventsProven) ++eventsSeen;
    bool failed = false;
    while (pending && !stop) {
      lastPacket = Clock::now();
      BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 position = 0;
      result = impl.capture->GetBuffer(&data, &frames, &flags, nullptr, &position);
      if (FAILED(result)) { broke(AudioCaptureStep::buffer, result); failed = true; break; }
      if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) position = 0;
      if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) discontinuity = true;
      try {
        std::vector<uint8_t> pcm; pcm.reserve(size_t(frames) * 4 * 48000 / rate + 16);
        for (UINT32 i = 0; i < frames; i++, sourceFrame++) {
          float left = 0, right = 0;
          if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) { const BYTE* sample = data + size_t(i) * blockAlign; left = value(sample); right = channels > 1 ? value(sample + bytesPerSample) : left; if (channels > 2) { const float center = value(sample + 2 * bytesPerSample) * 0.7071f; left += center; right += center; } }
          while (next <= double(sourceFrame)) {
            const float fraction = sourceFrame == 0 ? 1.0f : float(std::clamp(next - double(sourceFrame - 1), 0.0, 1.0));
            const int16_t l = int16_t(std::clamp(previousLeft + (left - previousLeft) * fraction, -1.0f, 1.0f) * 32767);
            const int16_t r = int16_t(std::clamp(previousRight + (right - previousRight) * fraction, -1.0f, 1.0f) * 32767);
            pcm.push_back(uint8_t(l)); pcm.push_back(uint8_t(uint16_t(l) >> 8)); pcm.push_back(uint8_t(r)); pcm.push_back(uint8_t(uint16_t(r) >> 8)); next += step;
          }
          previousLeft = left; previousRight = right;
        }
        result = impl.capture->ReleaseBuffer(frames);
        if (FAILED(result)) { broke(AudioCaptureStep::release, result); failed = true; break; }
        if (!pcm.empty()) {
          // The packet's first frame on the stream clock: from the QPC time WASAPI gave it, else the read
          // time minus its length. AudioPtsClock keeps consecutive packets exactly their length apart.
          LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
          const int64_t latency = audioCaptureLatencyUs(counter.QuadPart, frequency.QuadPart, position);
          const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
          const int64_t measured = latency >= 0 ? nowUs - latency : nowUs - int64_t(frames) * 1000000 / int64_t(rate);
          const uint64_t ptsUs = uint64_t(pts.stamp(measured, discontinuity)); discontinuity = false;
          pts.advance(pcm.size() / 4);
          if (opus) opus->push(ptsUs, pcm.data(), pcm.size(), sendFrame); else output(mediaPacket(2, ptsUs, pcm));
          if (window && latency >= 0) window->add(uint64_t(latency));
        }
      } catch (...) {
        broke(AudioCaptureStep::output, E_UNEXPECTED); failed = true; break;
      }
      result = impl.capture->GetNextPacketSize(&pending);
      if (FAILED(result)) { broke(AudioCaptureStep::packetSize, result); failed = true; break; }
    }
    (void)failed;
  }
  impl.close();
}
int encodeSelfTest(const std::wstring& path, bool captureDesktop, int frames) {
  Runtime runtime; const int width = 1280, height = 720, fps = 60;
  Encoder encoder(width, height, fps, 6000000); std::unique_ptr<Capture> capture;
  if (captureDesktop) capture = std::make_unique<Capture>(width, height);
  std::ofstream stream(path, std::ios::binary), index(path + L".index.json"), framed(path + L".frames", std::ios::binary);
  if (!stream || !index || !framed) throw std::runtime_error("Cannot open self-test output");
  index << "{\"width\":1280,\"height\":720,\"fpsTarget\":60,\"encoder\":\"" << encoder.backendName() << "\",\"source\":\"" << (captureDesktop ? "desktop" : "synthetic") << "\",\"frames\":[";
  size_t total = 0, emitted = 0; bool hasKey = false;
  auto write = [&](Frame frame) { if (emitted++) index << ','; index << "{\"offset\":" << total << ",\"length\":" << frame.bytes.size() << ",\"ptsUs\":" << frame.ptsUs << ",\"key\":" << (frame.key ? "true" : "false") << '}'; stream.write(reinterpret_cast<const char*>(frame.bytes.data()), std::streamsize(frame.bytes.size())); const uint32_t length = uint32_t(frame.bytes.size()); framed.write(reinterpret_cast<const char*>(&length), 4); framed.write(reinterpret_cast<const char*>(frame.bytes.data()), std::streamsize(frame.bytes.size())); total += frame.bytes.size(); hasKey |= frame.key; };
  std::vector<uint8_t> pixels(size_t(width) * height * 4), nv12(size_t(width) * height * 3 / 2);
  const auto start = Clock::now();
  for (int n = 0; n < frames; n++) {
    const uint8_t* input;
    if (capture) input = capture->frame().data(); else {
      for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) { auto* p = pixels.data() + (size_t(y) * width + x) * 4; p[0] = uint8_t(x + n * 3); p[1] = uint8_t(y + n * 2); p[2] = uint8_t((x / 64 + y / 64 + n / 10) % 2 ? 230 : 30); p[3] = 255; }
      bgraToNv12(pixels.data(), nv12.data(), width, height); input = nv12.data();
    }
    encoder.encode(input, uint64_t(n) * 1000000 / fps, write);
  }
  encoder.finish(write);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  index << "],\"elapsedMs\":" << elapsed << ",\"encodedFrames\":" << emitted << ",\"bytes\":" << total << '}';
  if (emitted != size_t(frames) || !hasKey || !total) throw std::runtime_error("Encoder self-test produced an incomplete stream");
  return int(emitted);
}
}
