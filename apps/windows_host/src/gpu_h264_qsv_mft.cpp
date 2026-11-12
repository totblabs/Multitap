// Intel's installed hardware H264 MFT, selected through Windows' per-adapter
// hardware MFT API. This is not a bundled Intel VPL dispatcher or a software MFT.
#include "gpu_h264.hpp"
#include "owned_output_size.hpp"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <utility>

namespace host::gpu {
namespace {
void requireMf(HRESULT value, uint32_t code, const char* operation) {
  if (SUCCEEDED(value)) return;
  if (value == E_OUTOFMEMORY) code = encoderGpuAllocationFailed;
  if (value == DXGI_ERROR_DEVICE_REMOVED || value == DXGI_ERROR_DEVICE_RESET || value == DXGI_ERROR_DEVICE_HUNG) code = encoderGpuDeviceLost;
  throw Failure(code, operation);
}
std::vector<uint8_t> annexB(std::span<const uint8_t> bytes, size_t maximum, bool sequence = false) {
  if (bytes.empty() || bytes.size() > maximum) throw Failure(encoderGpuOutputContract, "Intel H264 buffer size is invalid");
  if (bytes.size() >= 4 && bytes[0] == 0 && bytes[1] == 0 && (bytes[2] == 1 || (bytes[2] == 0 && bytes[3] == 1))) return {bytes.begin(), bytes.end()};
  std::vector<uint8_t> result;
  const auto append = [&](size_t at, size_t count) {
    if (!count || at > bytes.size() || count > bytes.size() - at || result.size() > maximum || maximum - result.size() < 4 || count > maximum - result.size() - 4)
      throw Failure(encoderGpuOutputContract, "Intel H264 NAL length is invalid");
    result.insert(result.end(), {0, 0, 0, 1}); result.insert(result.end(), bytes.begin() + at, bytes.begin() + at + count);
  };
  size_t at = 0;
  if (sequence && bytes.size() >= 7 && bytes[0] == 1) {
    const unsigned sps = bytes[5] & 31; at = 6;
    for (unsigned group = 0; group < 2; ++group) {
      if (group && at >= bytes.size()) throw Failure(encoderGpuOutputContract, "Intel H264 sequence is truncated");
      const unsigned count = group ? bytes[at++] : sps;
      if (!count) throw Failure(encoderGpuOutputContract, "Intel H264 sequence headers missing");
      for (unsigned index = 0; index < count; ++index) {
        if (bytes.size() - at < 2) throw Failure(encoderGpuOutputContract, "Intel H264 sequence length is truncated");
        const size_t countBytes = (size_t(bytes[at]) << 8) | bytes[at + 1]; at += 2; append(at, countBytes); at += countBytes;
      }
    }
  } else {
    while (at < bytes.size()) {
      if (bytes.size() - at < 4) throw Failure(encoderGpuOutputContract, "Intel H264 packet length is truncated");
      const size_t count = (size_t(bytes[at]) << 24) | (size_t(bytes[at + 1]) << 16) | (size_t(bytes[at + 2]) << 8) | bytes[at + 3];
      at += 4; append(at, count); at += count;
    }
  }
  return result;
}
struct Activations {
  IMFActivate** values = nullptr; UINT32 count = 0;
  ~Activations() { if (values) { for (UINT32 index = 0; index < count; ++index) if (values[index]) values[index]->Release(); CoTaskMemFree(values); } }
};
struct QsvState {
  Com<IMFActivate> activation;
  Com<IMFTransform> encoder;
  Com<IMFMediaEventGenerator> events;
  Com<ICodecAPI> codec;
  Com<ID3D11Device> device;
  Com<ID3D11DeviceContext> context;
  Com<IMFDXGIDeviceManager> manager;
  ~QsvState() { if (encoder) { encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0); encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0); } if (activation) activation->ShutdownObject(); }
};
// The capture's device lies on exactly this adapter (LUID, vendor, device) and can run video work.
bool sharedVideoDevice(ID3D11Device* device, const Adapter& adapter) {
  Com<IDXGIDevice> dxgi; Com<IDXGIAdapter> actual; DXGI_ADAPTER_DESC description{}; Com<ID3D11VideoDevice> video;
  if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&actual)) || FAILED(actual->GetDesc(&description))) return false;
  const uint64_t luid = (uint64_t(uint32_t(description.AdapterLuid.HighPart)) << 32) | description.AdapterLuid.LowPart;
  return luid == adapter.luid && description.VendorId == adapter.vendorId && description.DeviceId == adapter.deviceId && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&video)));
}
// gpu-capture-v1 for the hardware MFT: opened on the capture's own device (`shared`), the capture's
// NV12 picture is copied on the GPU into the input surface instead of being read back, converted on the CPU and
// uploaded again. Any picture that cannot go that way (another device, a copy or wrap failure, which also ends
// texture input for this encoder) is encoded from its bytes on the same hardware encoder, so the GPU path can
// only fall back to the upload path. `vendor` is Intel's in the product; a test opens another vendor's
// hardware MFT (NVIDIA's, for one) to run this same code.
class QsvMftEncoder final : public EncoderImplementation {
 public:
  QsvMftEncoder(int width, int height, int fps, int bitrate, const Adapter& adapter, IMFActivate* activation, ID3D11Device* shared, uint32_t vendor)
      : state_(std::make_unique<QsvState>()), width_(width), height_(height), fps_(fps), bitrate_(bitrate), vendor_(vendor) {
    validateFrameMemory(width, height, 0, 12);
    state_->activation = activation;
    requireMf(activation->ActivateObject(IID_PPV_ARGS(&state_->encoder)), encoderGpuInitializationFailed, "Activate selected Intel hardware encoder");
    Com<IMFAttributes> attributes;
    requireMf(state_->encoder->GetAttributes(&attributes), encoderGpuCapabilityUnsupported, "Read Intel hardware encoder attributes");
    UINT32 async = 0, aware = 0, hardwareUrl = 0;
    requireMf(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async), encoderGpuCapabilityUnsupported, "Verify Intel asynchronous hardware encoder");
    requireMf(attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &hardwareUrl), encoderGpuCapabilityUnsupported, "Verify Intel hardware encoder identity");
    requireMf(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware), encoderGpuCapabilityUnsupported, "Verify Intel D3D11 encoder support");
    if (!async || !aware || !hardwareUrl || hardwareUrl > 4096) throw Failure(encoderGpuCapabilityUnsupported, "Selected Intel transform is not the required hardware encoder");
    requireMf(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), encoderGpuInitializationFailed, "Unlock Intel hardware encoder");
    requireMf(state_->encoder.As(&state_->events), encoderGpuCapabilityUnsupported, "Read Intel encoder event interface");
    requireMf(state_->encoder.As(&state_->codec), encoderGpuCapabilityUnsupported, "Read Intel encoder configuration interface");
    DWORD inputs = 0, outputs = 0;
    requireMf(state_->encoder->GetStreamCount(&inputs, &outputs), encoderGpuCapabilityUnsupported, "Read Intel encoder stream count");
    if (inputs != 1 || outputs != 1) throw Failure(encoderGpuCapabilityUnsupported, "Intel encoder is not a single-picture transform");
    const auto streamIds = state_->encoder->GetStreamIDs(1, &inputId_, 1, &outputId_);
    if (streamIds == E_NOTIMPL) inputId_ = outputId_ = 0;
    else requireMf(streamIds, encoderGpuCapabilityUnsupported, "Read Intel encoder stream identity");
    createDevice(adapter, shared);
    requireMf(state_->encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, ULONG_PTR(state_->manager.Get())), encoderGpuInitializationFailed, "Set exact Intel encoder device manager");
    property(CODECAPI_AVLowLatencyMode, 1, true);
    // Zero B pictures where the transform takes the setting. One that refuses it (NVIDIA's MFT) is held to the
    // Baseline profile below, which has no B pictures: the accepted output type and every IDR's SPS are checked.
    bool bPictures = true;
    try { property(CODECAPI_AVEncMPVDefaultBPictureCount, 0); } catch (const Failure&) { bPictures = false; }
    property(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR); property(CODECAPI_AVEncCommonMeanBitRate, ULONG(bitrate));
    property(CODECAPI_AVEncMPVGOPSize, ULONG(fps) * 60);
    if (bPictures && readProperty(CODECAPI_AVEncMPVDefaultBPictureCount) != 0) throw Failure(encoderGpuCapabilityUnsupported, "Intel encoder did not accept zero B pictures");
    auto output = mediaType(MFVideoFormat_H264, true), input = mediaType(MFVideoFormat_NV12, false);
    // Exact SetType/TEST_ONLY checks query the real transform; no fixed 4K or
    // combined-dual picture limit is introduced here.
    requireMf(state_->encoder->SetOutputType(outputId_, output.Get(), MFT_SET_TYPE_TEST_ONLY), encoderGpuCapabilityUnsupported, "Query Intel H264 picture support");
    requireMf(state_->encoder->SetOutputType(outputId_, output.Get(), 0), encoderGpuInitializationFailed, "Set Intel H264 output");
    requireMf(state_->encoder->SetInputType(inputId_, input.Get(), MFT_SET_TYPE_TEST_ONLY), encoderGpuCapabilityUnsupported, "Query Intel NV12 picture support");
    requireMf(state_->encoder->SetInputType(inputId_, input.Get(), 0), encoderGpuInitializationFailed, "Set Intel NV12 input");
    verifyOutputType();
    requireMf(state_->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), encoderGpuInitializationFailed, "Begin Intel encoder stream");
    requireMf(state_->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), encoderGpuInitializationFailed, "Start Intel encoder stream");
    info_.initialized = true; info_.adapterLuid = adapter.luid; info_.vendorId = adapter.vendorId; info_.deviceId = adapter.deviceId; info_.adapterName = adapter.description;
  }
  void encode(const uint8_t* nv12, uint64_t pts, std::vector<Frame>& frames) override {
    if (!nv12) throw Failure(encoderGpuOutputContract, "Intel encoder input timestamp is invalid");
    submit([&] { return inputSample(nv12); }, pts, frames);
  }
  void encodePicture(const EncoderPicture& picture, uint64_t pts, std::vector<Frame>& frames) override {
    if (!picture.texture || !textureInput_ || !onDevice(picture.texture)) { EncoderImplementation::encodePicture(picture, pts, frames); return; }
    Com<IMFSample> sample;
    try { sample = textureSample(picture.texture); }
    catch (...) { textureInput_ = false; EncoderImplementation::encodePicture(picture, pts, frames); return; }
    submit([&] { return sample; }, pts, frames);
    ++textureFrames_;
  }
  ID3D11Device* textureDevice() const override { return textureInput_ ? state_->device.Get() : nullptr; }
  // Tests: pictures that went in as textures.
  uint64_t textureFrames() const { return textureFrames_; }
 private:
  template<class Make> void submit(Make&& make, uint64_t pts, std::vector<Frame>& frames) {
    if (pts > uint64_t(INT64_MAX / 10) || (havePts_ && pts <= lastPts_)) throw Failure(encoderGpuOutputContract, "Intel encoder input timestamp is invalid");
    const auto deadline = Clock::now() + std::chrono::milliseconds(250);
    while (!inputCredits_) { poll(); if (Clock::now() >= deadline) throw Failure(encoderGpuOutputTimeout, "Intel encoder did not request input"); if (!inputCredits_) Sleep(1); }
    if (outputCredits_) throw Failure(encoderGpuOutputContract, "Intel encoder retained an unconsumed picture");
    const bool force = !havePts_ || keyRequested_;
    if (force) property(CODECAPI_AVEncVideoForceKeyFrame, 1);
    Com<IMFSample> sample = make();
    requireMf(sample->SetSampleTime(LONGLONG(pts * 10)), encoderGpuSubmissionFailed, "Set Intel input timestamp");
    requireMf(sample->SetSampleDuration(10000000 / fps_), encoderGpuSubmissionFailed, "Set Intel input duration");
    requireMf(state_->encoder->ProcessInput(inputId_, sample.Get(), 0), encoderGpuSubmissionFailed, "Submit Intel hardware H264 input"); --inputCredits_;
    while (!outputCredits_) { poll(); if (Clock::now() >= deadline) throw Failure(encoderGpuOutputTimeout, "Intel encoder did not produce its picture"); if (!outputCredits_) Sleep(1); }
    Frame frame = readOutput(pts, force); --outputCredits_;
    frames.push_back(std::move(frame)); info_.hardwareVerified = true; havePts_ = true; lastPts_ = pts; keyRequested_ = false;
  }
 public:
  void finish(std::vector<Frame>&) override { /* Every accepted input already produced its exact picture. */ }
  bool setBitrate(int bitrate) override {
    if (bitrate == bitrate_) return true;
    if (bitrate < encoderMinimumBitrate || bitrate > encoderMaximumBitrate) return false;
    try { property(CODECAPI_AVEncCommonMeanBitRate, ULONG(bitrate)); if (readProperty(CODECAPI_AVEncCommonMeanBitRate) != ULONG(bitrate)) return false; }
    catch (...) { return false; }
    bitrate_ = bitrate; return true;
  }
  int reportedBitrate() const override { return bitrate_; }
  EncoderTuning reportedTuning() const override { return {}; }
  void requestKeyFrame() override { keyRequested_ = true; }
  EncoderHardwareInfo hardwareInfo() const override { return info_; }
 private:
  bool onDevice(ID3D11Texture2D* texture) const { Com<ID3D11Device> owner; texture->GetDevice(&owner); return owner.Get() == state_->device.Get(); }
  // The capture's NV12 picture, copied on the GPU into a new input surface of the encoder's device.
  Com<IMFSample> textureSample(ID3D11Texture2D* picture) {
    D3D11_TEXTURE2D_DESC source{}; picture->GetDesc(&source);
    if (source.Format != DXGI_FORMAT_NV12 || source.Width != UINT(width_) || source.Height != UINT(height_) || source.ArraySize != 1 || source.SampleDesc.Count != 1 || source.MipLevels != 1)
      throw Failure(encoderGpuOutputContract, "GPU NV12 picture does not match the Intel encoder");
    D3D11_TEXTURE2D_DESC description{}; description.Width = UINT(width_); description.Height = UINT(height_); description.MipLevels = 1; description.ArraySize = 1;
    description.Format = DXGI_FORMAT_NV12; description.SampleDesc.Count = 1; description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> texture;
    requireMf(state_->device->CreateTexture2D(&description, nullptr, &texture), encoderGpuAllocationFailed, "Allocate Intel NV12 input surface");
    state_->context->CopyResource(texture.Get(), picture);
    requireMf(state_->device->GetDeviceRemovedReason(), encoderGpuDeviceLost, "Copy GPU NV12 picture for Intel");
    return wrap(texture);
  }
  Com<IMFSample> wrap(const Com<ID3D11Texture2D>& texture) {
    Com<IMFMediaBuffer> buffer; requireMf(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture.Get(), 0, FALSE, &buffer), encoderGpuAllocationFailed, "Wrap Intel input surface");
    const auto layout = checkedFrameLayout(uint64_t(width_), uint64_t(height_));
    if (!layout || layout->nv12Bytes > std::numeric_limits<DWORD>::max()) throw Failure(encoderGpuAllocationFailed, "Intel input surface layout is not representable");
    requireMf(buffer->SetCurrentLength(DWORD(layout->nv12Bytes)), encoderGpuSubmissionFailed, "Set Intel input surface length");
    Com<IMFSample> sample; requireMf(MFCreateSample(&sample), encoderGpuAllocationFailed, "Create Intel input sample");
    requireMf(sample->AddBuffer(buffer.Get()), encoderGpuAllocationFailed, "Attach Intel input surface"); return sample;
  }
  void createDevice(const Adapter& adapter, ID3D11Device* shared) {
    if (shared && sharedVideoDevice(shared, adapter)) {
      // Capture and encode run on one thread; WGC and the transform's own threads use the device too.
      state_->device = shared; shared->GetImmediateContext(&state_->context);
      Com<ID3D11Multithread> protection;
      if (SUCCEEDED(state_->device.As(&protection))) protection->SetMultithreadProtected(TRUE);
      UINT token = 0; requireMf(MFCreateDXGIDeviceManager(&token, &state_->manager), encoderGpuInitializationFailed, "Create Intel device manager");
      requireMf(state_->manager->ResetDevice(state_->device.Get(), token), encoderGpuInitializationFailed, "Set Intel device manager adapter");
      textureInput_ = true;
      return;
    }
    Com<IDXGIFactory1> factory; requireMf(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), encoderGpuAdapterUnavailable, "Enumerate selected Intel device");
    Com<IDXGIAdapter1> selected;
    for (UINT ordinal = 0; ordinal < 256; ++ordinal) {
      Com<IDXGIAdapter1> candidate; const auto found = factory->EnumAdapters1(ordinal, &candidate); if (found == DXGI_ERROR_NOT_FOUND) break;
      requireMf(found, encoderGpuAdapterUnavailable, "Read selected Intel adapter"); DXGI_ADAPTER_DESC1 actual{};
      if (FAILED(candidate->GetDesc1(&actual))) continue;
      const uint64_t luid = (uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) | actual.AdapterLuid.LowPart;
      if (luid == adapter.luid && actual.VendorId == vendor_ && actual.DeviceId == adapter.deviceId && !(actual.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { selected = candidate; break; }
    }
    if (!selected) throw Failure(encoderGpuAdapterUnavailable, "Selected Intel adapter disappeared");
    requireMf(D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
      nullptr, 0, D3D11_SDK_VERSION, &state_->device, nullptr, &state_->context), encoderGpuInitializationFailed, "Create exact Intel D3D11 video device");
    UINT token = 0; requireMf(MFCreateDXGIDeviceManager(&token, &state_->manager), encoderGpuInitializationFailed, "Create Intel device manager");
    requireMf(state_->manager->ResetDevice(state_->device.Get(), token), encoderGpuInitializationFailed, "Set Intel device manager adapter");
  }
  void property(const GUID& key, ULONG value, bool boolean = false) {
    VARIANT setting; VariantInit(&setting); setting.vt = VARTYPE(boolean ? VT_BOOL : VT_UI4);
    if (boolean) setting.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE; else setting.ulVal = value;
    const auto result = state_->codec->SetValue(&key, &setting); VariantClear(&setting);
    const char* name = IsEqualGUID(key, CODECAPI_AVLowLatencyMode) ? "Apply Intel low-latency H264 property (low latency mode)"
      : IsEqualGUID(key, CODECAPI_AVEncMPVDefaultBPictureCount) ? "Apply Intel low-latency H264 property (B pictures)"
      : IsEqualGUID(key, CODECAPI_AVEncCommonRateControlMode) ? "Apply Intel low-latency H264 property (rate control)"
      : IsEqualGUID(key, CODECAPI_AVEncCommonMeanBitRate) ? "Apply Intel low-latency H264 property (bitrate)"
      : IsEqualGUID(key, CODECAPI_AVEncMPVGOPSize) ? "Apply Intel low-latency H264 property (GOP)"
      : IsEqualGUID(key, CODECAPI_AVEncVideoForceKeyFrame) ? "Apply Intel low-latency H264 property (key frame)" : "Apply Intel low-latency H264 property";
    requireMf(result, encoderGpuCapabilityUnsupported, name);
  }
  ULONG readProperty(const GUID& key) {
    VARIANT value; VariantInit(&value); const auto result = state_->codec->GetValue(&key, &value);
    const bool valid = SUCCEEDED(result) && value.vt == VT_UI4; const ULONG number = valid ? value.ulVal : ULONG_MAX; VariantClear(&value);
    if (!valid) throw Failure(encoderGpuCapabilityUnsupported, "Intel H264 property could not be verified"); return number;
  }
  Com<IMFMediaType> mediaType(const GUID& subtype, bool encoded) {
    Com<IMFMediaType> type; requireMf(MFCreateMediaType(&type), encoderGpuAllocationFailed, "Create Intel encoder type");
    requireMf(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), encoderGpuInitializationFailed, "Set Intel video major type");
    requireMf(type->SetGUID(MF_MT_SUBTYPE, subtype), encoderGpuInitializationFailed, "Set Intel video subtype");
    requireMf(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, UINT32(width_), UINT32(height_)), encoderGpuCapabilityUnsupported, "Set Intel actual picture dimensions");
    requireMf(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, UINT32(fps_), 1), encoderGpuCapabilityUnsupported, "Set Intel video rate");
    requireMf(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), encoderGpuCapabilityUnsupported, "Set Intel square pixels");
    requireMf(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), encoderGpuCapabilityUnsupported, "Set Intel progressive picture");
    requireMf(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601), encoderGpuCapabilityUnsupported, "Set Intel NV12 colour matrix");
    requireMf(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235), encoderGpuCapabilityUnsupported, "Set Intel NV12 range");
    if (encoded) { requireMf(type->SetUINT32(MF_MT_AVG_BITRATE, UINT32(bitrate_)), encoderGpuCapabilityUnsupported, "Set Intel H264 target"); requireMf(type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base), encoderGpuCapabilityUnsupported, "Set Intel H264 Baseline"); }
    return type;
  }
  void verifyOutputType() {
    Com<IMFMediaType> actual; UINT32 width = 0, height = 0, profile = 0; GUID subtype{};
    requireMf(state_->encoder->GetOutputCurrentType(outputId_, &actual), encoderGpuCapabilityUnsupported, "Read accepted Intel output type");
    requireMf(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width, &height), encoderGpuCapabilityUnsupported, "Read accepted Intel picture size");
    requireMf(actual->GetGUID(MF_MT_SUBTYPE, &subtype), encoderGpuCapabilityUnsupported, "Read accepted Intel codec");
    requireMf(actual->GetUINT32(MF_MT_MPEG2_PROFILE, &profile), encoderGpuCapabilityUnsupported, "Read accepted Intel profile");
    if (width != UINT32(width_) || height != UINT32(height_) || !IsEqualGUID(subtype, MFVideoFormat_H264) || profile != eAVEncH264VProfile_Base)
      throw Failure(encoderGpuCapabilityUnsupported, "Intel transform changed the requested picture or codec");
  }
  Com<IMFSample> inputSample(const uint8_t* nv12) {
    D3D11_TEXTURE2D_DESC description{}; description.Width = UINT(width_); description.Height = UINT(height_); description.MipLevels = 1; description.ArraySize = 1;
    description.Format = DXGI_FORMAT_NV12; description.SampleDesc.Count = 1; description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> texture;
    requireMf(state_->device->CreateTexture2D(&description, nullptr, &texture), encoderGpuAllocationFailed, "Allocate Intel NV12 input surface");
    state_->context->UpdateSubresource(texture.Get(), 0, nullptr, nv12, UINT(width_), 0);
    requireMf(state_->device->GetDeviceRemovedReason(), encoderGpuDeviceLost, "Upload Intel NV12 surface");
    return wrap(texture);
  }
  void poll() {
    for (int count = 0; count < 64; ++count) {
      Com<IMFMediaEvent> event; const auto result = state_->events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
      if (result == MF_E_NO_EVENTS_AVAILABLE) return;
      requireMf(result, encoderGpuSubmissionFailed, "Read Intel encoder event");
      HRESULT status = E_FAIL; MediaEventType type{};
      requireMf(event->GetStatus(&status), encoderGpuSubmissionFailed, "Read Intel encoder event status"); requireMf(status, encoderGpuSubmissionFailed, "Intel hardware encoder reported failure");
      requireMf(event->GetType(&type), encoderGpuSubmissionFailed, "Read Intel encoder event type");
      if (type == METransformNeedInput) { if (++inputCredits_ > 4) throw Failure(encoderGpuOutputContract, "Intel input event queue exceeded bound"); }
      else if (type == METransformHaveOutput) { if (++outputCredits_ > 1) throw Failure(encoderGpuOutputContract, "Intel encoder produced unexpected queued pictures"); }
      else if (type == MEError) throw Failure(encoderGpuSubmissionFailed, "Intel hardware encoder error event");
    }
    throw Failure(encoderGpuOutputContract, "Intel encoder event queue did not settle");
  }
  std::vector<uint8_t> sequence() {
    Com<IMFMediaType> type; requireMf(state_->encoder->GetOutputCurrentType(outputId_, &type), encoderGpuOutputContract, "Read Intel sequence type");
    UINT32 size = 0; requireMf(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size), encoderGpuOutputContract, "Read Intel sequence size");
    if (!size || size > 65536) throw Failure(encoderGpuOutputContract, "Intel sequence size is invalid");
    std::vector<uint8_t> bytes(size); requireMf(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, bytes.data(), size, &size), encoderGpuOutputContract, "Read Intel sequence bytes");
    bytes.resize(size); return annexB(bytes, 65536, true);
  }
  Frame readOutput(uint64_t pts, bool force) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      MFT_OUTPUT_STREAM_INFO requirements{}; requireMf(state_->encoder->GetOutputStreamInfo(outputId_, &requirements), encoderGpuOutputContract, "Read Intel output requirements");
      Com<IMFSample> allocated;
      if (!(requirements.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        const DWORD capacity = std::max<DWORD>(requirements.cbSize, 2 * 1024 * 1024);
        MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
        if (!GlobalMemoryStatusEx(&memory) || uint64_t(capacity) > std::min(memory.ullAvailPhys, memory.ullAvailVirtual)) throw Failure(encoderGpuAllocationFailed, "Intel output memory unavailable");
        Com<IMFMediaBuffer> buffer; requireMf(MFCreateMemoryBuffer(capacity, &buffer), encoderGpuAllocationFailed, "Allocate Intel output memory");
        requireMf(MFCreateSample(&allocated), encoderGpuAllocationFailed, "Allocate Intel output sample");
        requireMf(allocated->AddBuffer(buffer.Get()), encoderGpuAllocationFailed, "Attach Intel output memory");
      }
      MFT_OUTPUT_DATA_BUFFER output{}; output.dwStreamID = outputId_; output.pSample = allocated.Get(); DWORD status = 0;
      const auto result = state_->encoder->ProcessOutput(0, 1, &output, &status);
      if (output.pEvents) output.pEvents->Release();
      Com<IMFSample> sample = allocated; if (!allocated && output.pSample) sample.Attach(output.pSample);
      if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
        auto type = mediaType(MFVideoFormat_H264, true); requireMf(state_->encoder->SetOutputType(outputId_, type.Get(), 0), encoderGpuOutputContract, "Accept exact Intel output format change"); verifyOutputType(); continue;
      }
      requireMf(result, encoderGpuOutputContract, "Read Intel H264 picture");
      if (output.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) throw Failure(encoderGpuOutputContract, "Intel encoder returned an incomplete picture");
      if (!sample) throw Failure(encoderGpuOutputContract, "Intel output sample is missing");
      LONGLONG time = -1; requireMf(sample->GetSampleTime(&time), encoderGpuOutputContract, "Read Intel output PTS");
      if (time < 0 || uint64_t(time) != pts * 10) throw Failure(encoderGpuOutputContract, "Intel output PTS did not match the current input");
      Com<IMFMediaBuffer> buffer; requireMf(sample->ConvertToContiguousBuffer(&buffer), encoderGpuOutputContract, "Read Intel H264 bytes");
      BYTE* bytes = nullptr; DWORD size = 0; requireMf(buffer->Lock(&bytes, nullptr, &size), encoderGpuOutputContract, "Lock Intel H264 output");
      Frame frame;
      try { frame.bytes = annexB(std::span(bytes, size), encoderMaximumFrameBytes); } catch (...) { buffer->Unlock(); throw; }
      requireMf(buffer->Unlock(), encoderGpuOutputContract, "Unlock Intel H264 output");
      auto unit = inspectAnnexB(frame.bytes, encoderMaximumFrameBytes);
      if (!unit.valid || (force && !unit.idr)) throw Failure(encoderGpuOutputContract, "Intel output did not satisfy the AnnexB/IDR contract");
      if (unit.idr && (!unit.sps || !unit.pps)) { auto headers = sequence(); if (headers.size() > encoderMaximumFrameBytes - frame.bytes.size()) throw Failure(encoderGpuOutputContract, "Intel picture plus headers exceeds wire bound"); headers.insert(headers.end(), frame.bytes.begin(), frame.bytes.end()); frame.bytes = std::move(headers); unit = inspectAnnexB(frame.bytes, encoderMaximumFrameBytes); }
      if (unit.idr && (!unit.sps || !unit.pps || unit.spsProfile != 66)) throw Failure(encoderGpuOutputContract, "Intel IDR did not carry Baseline SPS/PPS");
      frame.ptsUs = pts; frame.key = unit.idr; return frame;
    }
    throw Failure(encoderGpuOutputContract, "Intel output repeatedly changed format");
  }
  std::unique_ptr<QsvState> state_;
  int width_, height_, fps_, bitrate_;
  int inputCredits_ = 0, outputCredits_ = 0;
  DWORD inputId_ = 0, outputId_ = 0;
  bool havePts_ = false, keyRequested_ = false, textureInput_ = false;
  uint64_t lastPts_ = 0, textureFrames_ = 0;
  uint32_t vendor_ = 0x8086;
  EncoderHardwareInfo info_;
};
// Every hardware H264 MFT of the adapter, tried in order; with `shared`, each opens on the capture's device first,
// then on its own (a fresh inventory: an activation that failed is not activated twice).
std::unique_ptr<EncoderImplementation> openHardwareMft(int width, int height, int fps, int bitrate, const Adapter& adapter, ID3D11Device* shared, uint32_t vendor);
}
std::unique_ptr<EncoderImplementation> createQsvMftEncoder(int width, int height, int fps, int bitrate, const Adapter& adapter, ID3D11Device* shared) {
  if (adapter.vendorId != 0x8086) throw Failure(encoderGpuVendorUnsupported, "Selected adapter is not an Intel GPU");
  if (shared) { try { return openHardwareMft(width, height, fps, bitrate, adapter, shared, 0x8086); } catch (...) {} }
  return openHardwareMft(width, height, fps, bitrate, adapter, nullptr, 0x8086);
}
// Test only (MultitapHostMftTextureTest): the same encoder on another vendor's hardware MFT, never by the product.
std::unique_ptr<EncoderImplementation> createHardwareMftEncoderForTesting(int width, int height, int fps, int bitrate, const Adapter& adapter, ID3D11Device* shared) {
  return openHardwareMft(width, height, fps, bitrate, adapter, shared, adapter.vendorId);
}
uint64_t hardwareMftTextureFramesForTesting(const EncoderImplementation& encoder) {
  const auto* mft = dynamic_cast<const QsvMftEncoder*>(&encoder); return mft ? mft->textureFrames() : 0;
}
namespace {
std::unique_ptr<EncoderImplementation> openHardwareMft(int width, int height, int fps, int bitrate, const Adapter& adapter, ID3D11Device* shared, uint32_t vendor) {
  using Enumerate = HRESULT(WINAPI*)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*, const MFT_REGISTER_TYPE_INFO*, IMFAttributes*, IMFActivate***, UINT32*);
  const auto symbol = GetProcAddress(GetModuleHandleW(L"mfplat.dll"), "MFTEnum2");
  if (!symbol) throw Failure(encoderGpuApiUnsupported, "Per-adapter hardware encoder API unavailable");
  static_assert(sizeof(Enumerate) == sizeof(symbol)); Enumerate enumerate; std::memcpy(&enumerate, &symbol, sizeof(enumerate));
  Com<IMFAttributes> filters; requireMf(MFCreateAttributes(&filters, 1), encoderGpuAllocationFailed, "Create Intel adapter filter");
  LUID luid{}; luid.HighPart = LONG(uint32_t(adapter.luid >> 32)); luid.LowPart = DWORD(adapter.luid);
  requireMf(filters->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&luid), sizeof(luid)), encoderGpuAdapterUnavailable, "Set exact Intel adapter filter");
  const MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12}, output{MFMediaType_Video, MFVideoFormat_H264}; Activations candidates;
  requireMf(enumerate(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &input, &output, filters.Get(), &candidates.values, &candidates.count), encoderGpuCapabilityUnsupported, "Query exact Intel hardware H264 encoder");
  if (!candidates.count || candidates.count > 256) throw Failure(encoderGpuCapabilityUnsupported, "Selected Intel adapter has no bounded hardware H264 encoder inventory");
  uint32_t failure = encoderGpuInitializationFailed; std::string step = "unknown";
  for (UINT32 index = 0; index < candidates.count; ++index) {
    try { return std::make_unique<QsvMftEncoder>(width, height, fps, bitrate, adapter, candidates.values[index], shared, vendor); }
    catch (const std::exception& error) { failure = failureCode(error); step = error.what(); }
    catch (...) { failure = encoderGpuInitializationFailed; }
  }
  // The last candidate's fixed step (every message above is a literal).
  throw Failure(failure, ("Selected Intel hardware H264 encoders could not initialize: " + step).c_str());
}
}
}
