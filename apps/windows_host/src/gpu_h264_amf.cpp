// Multitap implementation against AMD's unmodified MIT public API declarations.
// No AMF sample implementation, SDK runtime, FFmpeg or driver is redistributed.
#include "gpu_h264.hpp"
#include "gpu_h264_amf_policy.hpp"
#include "owned_output_size.hpp"
#include <core/Factory.h>
#include <components/VideoEncoderVCE.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace host::gpu {
namespace {
void requireAmf(AMF_RESULT result, uint32_t code, const char* operation) {
  if (result == AMF_OK) return;
  if (result == AMF_OUT_OF_MEMORY) code = encoderGpuAllocationFailed;
  else if (result == AMF_NO_DEVICE || result == AMF_DIRECTX_FAILED) code = encoderGpuDeviceLost;
  else if (result == AMF_NO_INTERFACE || result == AMF_NOT_IMPLEMENTED) code = encoderGpuApiUnsupported;
  throw Failure(code, operation);
}
template<class Function> Function exported(HMODULE module, const char* name) {
  const FARPROC address = GetProcAddress(module, name);
  if (!address) throw Failure(encoderGpuApiUnsupported, "AMF API entry unavailable");
  static_assert(sizeof(Function) == sizeof(address));
  Function result; std::memcpy(&result, &address, sizeof(result)); return result;
}
bool integer(amf::AMFPropertyStorage* object, const wchar_t* name, amf_int64& result) {
  amf::AMFVariant value;
  if (!object || object->GetProperty(name, &value) != AMF_OK || value.type != amf::AMF_VARIANT_INT64) return false;
  result = value.int64Value; return true;
}
void setInteger(amf::AMFPropertyStorage* object, const wchar_t* name, amf_int64 expected, uint32_t code = encoderGpuCapabilityUnsupported) {
  requireAmf(object->SetProperty(name, expected), code, "AMF integer property was refused");
  amf_int64 actual = 0;
  if (!integer(object, name, actual) || actual != expected) throw Failure(code, "AMF integer property was not applied");
}
void setBoolean(amf::AMFPropertyStorage* object, const wchar_t* name, bool expected, uint32_t code = encoderGpuCapabilityUnsupported) {
  requireAmf(object->SetProperty(name, expected), code, "AMF boolean property was refused");
  amf::AMFVariant value;
  if (object->GetProperty(name, &value) != AMF_OK || value.type != amf::AMF_VARIANT_BOOL || value.boolValue != expected)
    throw Failure(code, "AMF boolean property was not applied");
}
void sameAdapter(ID3D11Device* device, const Adapter& selected) {
  if (!device) throw Failure(encoderGpuAdapterUnavailable, "AMF D3D11 device is unavailable");
  Com<IDXGIDevice> dxgi; Com<IDXGIAdapter> adapter;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)))
    throw Failure(encoderGpuAdapterUnavailable, "AMF device adapter identity unavailable");
  DXGI_ADAPTER_DESC actual{};
  if (FAILED(adapter->GetDesc(&actual))) throw Failure(encoderGpuAdapterUnavailable, "AMF device adapter identity unavailable");
  const uint64_t luid = (uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) | actual.AdapterLuid.LowPart;
  if (luid != selected.luid || actual.VendorId != selected.vendorId || actual.DeviceId != selected.deviceId)
    throw Failure(encoderGpuAdapterUnavailable, "AMF did not retain the selected physical adapter");
}
struct State {
  HMODULE module = nullptr;
  amf::AMFFactory* factory = nullptr; // Factory singleton has no Acquire/Release.
  amf::AMFContextPtr context;
  amf::AMFComponentPtr encoder;
  Com<ID3D11Device> device;
  Com<ID3D11DeviceContext> deviceContext;
  ~State() {
    // Driver Terminate/Release cannot be promised bounded by an application
    // timer. The250ms below bounds output polling, not arbitrary driver calls.
    if (encoder) { encoder->Terminate(); encoder.Release(); }
    if (context) { context->Terminate(); context.Release(); }
    deviceContext.Reset(); device.Reset(); factory = nullptr;
    if (module) FreeLibrary(module);
  }
};
bool onAdapter(ID3D11Device* device, const Adapter& selected) { try { sameAdapter(device, selected); return true; } catch (...) { return false; } }
// gpu-capture-v1 for AMF: opened on the capture's own device (`shared`), the capture's NV12 picture
// is copied on the GPU into a new NV12 texture that AMF wraps (CreateSurfaceFromDX11Native) instead of being read
// back to the CPU and uploaded again. Every encode is synchronous (its output is awaited), so the texture is free
// when encode returns. A device AMF refuses opens the encoder on its own device (createAmfEncoder), and a copy
// or wrap failure ends texture input for this encoder: its bytes go the upload path.
// This route has not run on an AMD GPU (none was available); the same texture route of the Intel MFT is measured
// on an NVIDIA GPU (MultitapHostMftTextureTest).
class AmfEncoder final : public EncoderImplementation {
 public:
  AmfEncoder(int width, int height, int fps, int bitrate, const Adapter& adapter, ID3D11Device* shared)
      : state_(std::make_unique<State>()), width_(width), height_(height), fps_(fps), bitrate_(bitrate) {
    static_assert(sizeof(void*) == 8, "The installed AMF runtime path is64bit only");
    if (adapter.vendorId != 0x1002) throw Failure(encoderGpuVendorUnsupported, "Selected adapter is not an AMD GPU");
    if (width < 16 || height < 16 || fps < 1 || fps > 60 || bitrate < encoderMinimumBitrate || bitrate > encoderMaximumBitrate ||
        !checkedFrameLayout(uint64_t(width), uint64_t(height))) throw Failure(encoderGpuCapabilityUnsupported, "AMF input representation is invalid");
    validateFrameMemory(width, height, 0, 12);
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) throw Failure(encoderGpuAdapterUnavailable, "AMD adapter inventory unavailable");
    Com<IDXGIAdapter1> selected;
    for (UINT ordinal = 0; ordinal < 256; ++ordinal) {
      Com<IDXGIAdapter1> candidate;
      const HRESULT found = factory->EnumAdapters1(ordinal, &candidate);
      if (found == DXGI_ERROR_NOT_FOUND) break;
      if (FAILED(found)) throw Failure(encoderGpuAdapterUnavailable, "AMD adapter inventory failed");
      DXGI_ADAPTER_DESC1 actual{};
      if (FAILED(candidate->GetDesc1(&actual))) continue;
      const uint64_t luid = (uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) | actual.AdapterLuid.LowPart;
      if (luid == adapter.luid && actual.VendorId == adapter.vendorId && actual.DeviceId == adapter.deviceId && !(actual.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { selected = candidate; break; }
    }
    if (!selected) throw Failure(encoderGpuAdapterUnavailable, "Selected AMD adapter disappeared");
    if (shared && onAdapter(shared, adapter)) {
      // Capture and encode run on one thread; WGC and AMF's own threads use the device too.
      state_->device = shared; shared->GetImmediateContext(&state_->deviceContext);
      Com<ID3D11Multithread> protection;
      if (SUCCEEDED(state_->device.As(&protection))) protection->SetMultithreadProtected(TRUE);
      textureInput_ = true;
    } else {
      const HRESULT created = D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &state_->device, nullptr, &state_->deviceContext);
      if (FAILED(created)) throw Failure(encoderGpuInitializationFailed, "Selected AMD D3D11 device could not be created");
    }
    sameAdapter(state_->device.Get(), adapter);
    state_->module = LoadLibraryExW(L"amfrt64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!state_->module) throw Failure(encoderGpuDriverUnavailable, "Installed AMD AMF runtime is unavailable");
    amf_uint64 version = 0;
    requireAmf(exported<AMFQueryVersion_Fn>(state_->module, AMF_QUERY_VERSION_FUNCTION_NAME)(&version), encoderGpuApiUnsupported, "AMF runtime version query failed");
    if (version < AMF_FULL_VERSION) throw Failure(encoderGpuApiUnsupported, "Installed AMF runtime predates the pinned API");
    requireAmf(exported<AMFInit_Fn>(state_->module, AMF_INIT_FUNCTION_NAME)(AMF_FULL_VERSION, &state_->factory), encoderGpuApiUnsupported, "AMF API initialization failed");
    if (!state_->factory) throw Failure(encoderGpuApiUnsupported, "AMF did not return its factory");
    requireAmf(state_->factory->CreateContext(&state_->context), encoderGpuInitializationFailed, "AMF context creation failed");
    if (!state_->context) throw Failure(encoderGpuInitializationFailed, "AMF context unavailable");
    requireAmf(state_->context->InitDX11(state_->device.Get(), amf::AMF_DX11_0), encoderGpuInitializationFailed, "AMF selected-device initialization failed");
    sameAdapter(static_cast<ID3D11Device*>(state_->context->GetDX11Device(amf::AMF_DX11_0)), adapter);
    requireAmf(state_->factory->CreateComponent(state_->context, AMFVideoEncoderVCE_AVC, &state_->encoder), encoderGpuCapabilityUnsupported, "AMD H264 hardware component unavailable");
    if (!state_->encoder) throw Failure(encoderGpuCapabilityUnsupported, "AMD H264 component unavailable");
    auto* encoder = state_->encoder.operator->();
    amf::AMFCapsPtr caps; amf::AMFIOCapsPtr input;
    requireAmf(encoder->GetCaps(&caps), encoderGpuCapabilityUnsupported, "AMD H264 capabilities unavailable");
    if (!caps || (caps->GetAccelerationType() != amf::AMF_ACCEL_HARDWARE && caps->GetAccelerationType() != amf::AMF_ACCEL_GPU))
      throw Failure(encoderGpuCapabilityUnsupported, "AMD H264 component is not hardware accelerated");
    requireAmf(caps->GetInputCaps(&input), encoderGpuCapabilityUnsupported, "AMD H264 input capabilities unavailable");
    if (!input) throw Failure(encoderGpuCapabilityUnsupported, "AMD H264 input capabilities unavailable");
    amf_int32 minWidth = 0, maxWidth = 0, minHeight = 0, maxHeight = 0;
    input->GetWidthRange(&minWidth, &maxWidth); input->GetHeightRange(&minHeight, &maxHeight);
    if (minWidth < 0 || minHeight < 0 || maxWidth < 1 || maxHeight < 1 ||
        !pictureFits({uint32_t(minWidth), uint32_t(minHeight), uint32_t(maxWidth), uint32_t(maxHeight), 0, 0}, uint32_t(width), uint32_t(height), uint32_t(fps)))
      throw Failure(encoderGpuCapabilityUnsupported, "Requested picture exceeds actual AMD H264 dimensions");
    bool nv12 = false, dx11 = false;
    const auto formats = input->GetNumOfFormats(), memories = input->GetNumOfMemoryTypes();
    if (formats < 1 || formats > 256 || memories < 1 || memories > 256) throw Failure(encoderGpuCapabilityUnsupported, "AMD H264 input inventory is invalid");
    for (amf_int32 i = 0; i < formats; ++i) { amf::AMF_SURFACE_FORMAT format = amf::AMF_SURFACE_UNKNOWN; amf_bool native = false;
      requireAmf(input->GetFormatAt(i, &format, &native), encoderGpuCapabilityUnsupported, "AMD H264 surface query failed"); if (format == amf::AMF_SURFACE_NV12 && native) nv12 = true; }
    for (amf_int32 i = 0; i < memories; ++i) { amf::AMF_MEMORY_TYPE memory = amf::AMF_MEMORY_UNKNOWN; amf_bool native = false;
      requireAmf(input->GetMemoryTypeAt(i, &memory, &native), encoderGpuCapabilityUnsupported, "AMD H264 memory query failed"); if (memory == amf::AMF_MEMORY_DX11 && native) dx11 = true; }
    amf_int64 maximumBitrate = 0, maximumProfile = 0, maximumLevel = 0, maximumThroughput = 0, currentThroughput = 0;
    if (!nv12 || !dx11 || !integer(caps, AMF_VIDEO_ENCODER_CAP_MAX_BITRATE, maximumBitrate) || maximumBitrate < bitrate ||
        !integer(caps, AMF_VIDEO_ENCODER_CAP_MAX_PROFILE, maximumProfile) || maximumProfile < AMF_VIDEO_ENCODER_PROFILE_BASELINE ||
        !integer(caps, AMF_VIDEO_ENCODER_CAP_MAX_LEVEL, maximumLevel) ||
        !integer(caps, AMF_VIDEO_ENCODER_CAP_MAX_THROUGHPUT, maximumThroughput)) throw Failure(encoderGpuCapabilityUnsupported, "AMD H264 Baseline/NV12/rate capability unavailable");
    if (!integer(caps, AMF_VIDEO_ENCODER_CAP_REQUESTED_THROUGHPUT, currentThroughput)) currentThroughput = 0;
    if (!amf_detail::throughputFits(amf_detail::macroblocksPerSecond(uint32_t(width), uint32_t(height), uint32_t(fps)), maximumThroughput, currentThroughput))
      throw Failure(encoderGpuCapabilityUnsupported, "Requested H264 rate exceeds available AMD macroblock throughput");
    maximumBitrate_ = maximumBitrate;
    const auto level = amf_detail::selectLevel(uint32_t(width), uint32_t(height), uint32_t(fps), uint64_t(bitrate), uint64_t(vbvBits(bitrate)), maximumLevel);
    if (!level) throw Failure(encoderGpuCapabilityUnsupported, "AMD H264 level cannot represent the requested picture/rate");
    level_ = *level;
    setInteger(encoder, AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY);
    setInteger(encoder, AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_BASELINE);
    setInteger(encoder, AMF_VIDEO_ENCODER_PROFILE_LEVEL, level_.value);
    setInteger(encoder, AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES, 1);
    setInteger(encoder, AMF_VIDEO_ENCODER_MAX_LTR_FRAMES, 0);
    setInteger(encoder, AMF_VIDEO_ENCODER_MAX_NUM_TEMPORAL_LAYERS, 1);
    setInteger(encoder, AMF_VIDEO_ENCODER_NUM_TEMPORAL_ENHANCMENT_LAYERS, 1);
    setInteger(encoder, AMF_VIDEO_ENCODER_SCANTYPE, AMF_VIDEO_ENCODER_SCANTYPE_PROGRESSIVE);
    setInteger(encoder, AMF_VIDEO_ENCODER_COLOR_BIT_DEPTH, AMF_COLOR_BIT_DEPTH_8);
    setInteger(encoder, AMF_VIDEO_ENCODER_QUALITY_PRESET, AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
    setInteger(encoder, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
    requireAmf(encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize(width, height)), encoderGpuCapabilityUnsupported, "AMD H264 picture was refused");
    requireAmf(encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(uint32_t(fps), 1)), encoderGpuCapabilityUnsupported, "AMD H264 frame rate was refused");
    setBoolean(encoder, AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
    setInteger(encoder, AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0);
    setInteger(encoder, AMF_VIDEO_ENCODER_MAX_CONSECUTIVE_BPICTURES, 0);
    setBoolean(encoder, AMF_VIDEO_ENCODER_B_REFERENCE_ENABLE, false);
    setBoolean(encoder, AMF_VIDEO_ENCODER_ADAPTIVE_MINIGOP, false);
    setBoolean(encoder, AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, false);
    setInteger(encoder, AMF_VIDEO_ENCODER_PREENCODE_ENABLE, AMF_VIDEO_ENCODER_PREENCODE_DISABLED);
    if (encoder->HasProperty(AMF_VIDEO_ENCODER_ENABLE_SMART_ACCESS_VIDEO)) setBoolean(encoder, AMF_VIDEO_ENCODER_ENABLE_SMART_ACCESS_VIDEO, false);
    setBoolean(encoder, AMF_VIDEO_ENCODER_RATE_CONTROL_SKIP_FRAME_ENABLE, false);
    setBoolean(encoder, AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
    setBoolean(encoder, AMF_VIDEO_ENCODER_ENABLE_VBAQ, false);
    setBoolean(encoder, AMF_VIDEO_ENCODER_ENFORCE_HRD, true);
    setInteger(encoder, AMF_VIDEO_ENCODER_CABAC_ENABLE, AMF_VIDEO_ENCODER_CALV);
    // The API's automatic IDR spacing range is0..1000. Keep the common60s GOP
    // without inventing a resolution/fps ceiling: submit each scheduled IDR.
    setInteger(encoder, AMF_VIDEO_ENCODER_IDR_PERIOD, 0);
    setInteger(encoder, AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, 0);
    setInteger(encoder, AMF_VIDEO_ENCODER_INTRA_REFRESH_NUM_MBS_PER_SLOT, 0);
    setInteger(encoder, AMF_VIDEO_ENCODER_OUTPUT_MODE, AMF_VIDEO_ENCODER_OUTPUT_MODE_FRAME);
    setInteger(encoder, AMF_VIDEO_ENCODER_QUERY_TIMEOUT, 0); // QueryOutput must not block; own polling deadline below.
    setInteger(encoder, AMF_VIDEO_ENCODER_INPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_601);
    setInteger(encoder, AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_601);
    setBoolean(encoder, AMF_VIDEO_ENCODER_FULL_RANGE_COLOR, false);
    setInteger(encoder, AMF_VIDEO_ENCODER_INPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_UNSPECIFIED);
    setInteger(encoder, AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_UNSPECIFIED);
    setInteger(encoder, AMF_VIDEO_ENCODER_INPUT_TRANSFER_CHARACTERISTIC, AMF_COLOR_TRANSFER_CHARACTERISTIC_UNSPECIFIED);
    setInteger(encoder, AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC, AMF_COLOR_TRANSFER_CHARACTERISTIC_UNSPECIFIED);
    setRate(bitrate, true);
    requireAmf(encoder->Init(amf::AMF_SURFACE_NV12, width, height), encoderGpuInitializationFailed, "Selected AMD H264 encoder initialization failed");
    checkConfiguration();
    dynamicBitrate_ = runtimeWrite(AMF_VIDEO_ENCODER_TARGET_BITRATE) && runtimeWrite(AMF_VIDEO_ENCODER_PEAK_BITRATE) && runtimeWrite(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE);
    info_.initialized = true; info_.adapterLuid = adapter.luid; info_.vendorId = adapter.vendorId; info_.deviceId = adapter.deviceId;
    info_.adapterName = adapter.description.size() <= 512 ? adapter.description : std::string{};
  }
  void encode(const uint8_t* nv12, uint64_t ptsUs, std::vector<Frame>& output) override {
    const auto timing = amf_detail::timing(ptsUs, uint32_t(fps_), havePts_, lastPts_);
    if (!nv12 || !timing || frameCount_ == UINT64_MAX) throw Failure(encoderGpuOutputContract, "AMF input timestamp is invalid");
    checkDevice();
    amf::AMFSurfacePtr surface;
    requireAmf(state_->context->AllocSurface(amf::AMF_MEMORY_HOST, amf::AMF_SURFACE_NV12, width_, height_, &surface), encoderGpuAllocationFailed, "Allocate AMD NV12 upload surface");
    if (!surface || surface->GetFormat() != amf::AMF_SURFACE_NV12 || surface->GetMemoryType() != amf::AMF_MEMORY_HOST || surface->GetPlanesCount() != 2)
      throw Failure(encoderGpuOutputContract, "AMF NV12 host surface is invalid");
    auto* y = surface->GetPlane(amf::AMF_PLANE_Y); auto* uv = surface->GetPlane(amf::AMF_PLANE_UV);
    const auto yLayout = plane(y, uint32_t(width_), uint32_t(height_), 1), uvLayout = plane(uv, uint32_t(width_ / 2), uint32_t(height_ / 2), 2);
    MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
    if (!yLayout || !uvLayout || !GlobalMemoryStatusEx(&memory) || !amf_detail::planesFit(*yLayout, *uvLayout, std::min(memory.ullAvailPhys, memory.ullAvailVirtual)))
      throw Failure(encoderGpuAllocationFailed, "AMF padded NV12 allocation exceeds available memory");
    const auto source = checkedFrameLayout(uint64_t(width_), uint64_t(height_));
    if (!source) throw Failure(encoderGpuOutputContract, "AMF source NV12 layout is invalid");
    const size_t yBytes = size_t(width_) * size_t(height_);
    if (yBytes > source->nv12Bytes || source->nv12Bytes - yBytes != yBytes / 2) throw Failure(encoderGpuOutputContract, "AMF source NV12 span is invalid");
    copyPlane(y, *yLayout, nv12, size_t(width_)); copyPlane(uv, *uvLayout, nv12 + yBytes, size_t(width_));
    requireAmf(surface->Convert(amf::AMF_MEMORY_DX11), encoderGpuSubmissionFailed, "Upload NV12 to selected AMD D3D11 device");
    if (surface->GetMemoryType() != amf::AMF_MEMORY_DX11 || surface->GetFormat() != amf::AMF_SURFACE_NV12) throw Failure(encoderGpuOutputContract, "AMF did not produce native DX11 NV12 input");
    submit(surface, *timing, ptsUs, output);
  }
  void encodePicture(const EncoderPicture& picture, uint64_t ptsUs, std::vector<Frame>& output) override {
    if (!picture.texture || !textureInput_ || !onDevice(picture.texture)) { EncoderImplementation::encodePicture(picture, ptsUs, output); return; }
    const auto timing = amf_detail::timing(ptsUs, uint32_t(fps_), havePts_, lastPts_);
    if (!timing || frameCount_ == UINT64_MAX) throw Failure(encoderGpuOutputContract, "AMF input timestamp is invalid");
    checkDevice();
    amf::AMFSurfacePtr surface;
    try { surface = textureSurface(picture.texture); }
    catch (...) { textureInput_ = false; EncoderImplementation::encodePicture(picture, ptsUs, output); return; }
    submit(surface, *timing, ptsUs, output);
  }
  ID3D11Device* textureDevice() const override { return textureInput_ ? state_->device.Get() : nullptr; }
 private:
  bool onDevice(ID3D11Texture2D* texture) const { Com<ID3D11Device> owner; texture->GetDevice(&owner); return owner.Get() == state_->device.Get(); }
  // The capture's NV12 picture, copied on the GPU into a new plain NV12 texture that AMF wraps.
  amf::AMFSurfacePtr textureSurface(ID3D11Texture2D* picture) {
    D3D11_TEXTURE2D_DESC source{}; picture->GetDesc(&source);
    if (source.Format != DXGI_FORMAT_NV12 || source.Width != UINT(width_) || source.Height != UINT(height_) || source.ArraySize != 1 || source.SampleDesc.Count != 1 || source.MipLevels != 1)
      throw Failure(encoderGpuOutputContract, "GPU NV12 picture does not match the AMD encoder");
    D3D11_TEXTURE2D_DESC description{}; description.Width = UINT(width_); description.Height = UINT(height_); description.MipLevels = 1; description.ArraySize = 1;
    description.Format = DXGI_FORMAT_NV12; description.SampleDesc.Count = 1; description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> texture;
    if (FAILED(state_->device->CreateTexture2D(&description, nullptr, &texture))) {
      description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      if (FAILED(state_->device->CreateTexture2D(&description, nullptr, &texture))) throw Failure(encoderGpuAllocationFailed, "Allocate AMD NV12 input texture");
    }
    state_->deviceContext->CopyResource(texture.Get(), picture);
    checkDevice();
    amf::AMFSurfacePtr surface;
    requireAmf(state_->context->CreateSurfaceFromDX11Native(texture.Get(), &surface, nullptr), encoderGpuSubmissionFailed, "Wrap GPU NV12 picture for AMD");
    if (!surface || surface->GetMemoryType() != amf::AMF_MEMORY_DX11 || surface->GetFormat() != amf::AMF_SURFACE_NV12) throw Failure(encoderGpuOutputContract, "AMF did not wrap native DX11 NV12 input");
    return surface;
  }
  template<class Timing> void submit(amf::AMFSurfacePtr& surface, const Timing& timing, uint64_t ptsUs, std::vector<Frame>& output) {
    surface->SetPts(timing.pts); surface->SetDuration(timing.duration); surface->SetFrameType(amf::AMF_FRAME_PROGRESSIVE);
    const bool forceIdr = frameCount_ == 0 || keyRequested_ || frameCount_ % (uint64_t(fps_) * 60) == 0;
    setInteger(surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, forceIdr ? AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR : AMF_VIDEO_ENCODER_PICTURE_TYPE_P, encoderGpuSubmissionFailed);
    setBoolean(surface, AMF_VIDEO_ENCODER_INSERT_SPS, forceIdr, encoderGpuSubmissionFailed); setBoolean(surface, AMF_VIDEO_ENCODER_INSERT_PPS, forceIdr, encoderGpuSubmissionFailed);
    setInteger(surface, AMF_VIDEO_ENCODER_PICTURE_STRUCTURE, AMF_VIDEO_ENCODER_PICTURE_STRUCTURE_FRAME, encoderGpuSubmissionFailed);
    const AMF_RESULT submitted = state_->encoder->SubmitInput(surface);
    if (submitted == AMF_NEED_MORE_INPUT) throw Failure(encoderGpuOutputContract, "AMF delayed a zero-reorder picture");
    requireAmf(submitted, encoderGpuSubmissionFailed, "Submit AMD H264 picture");
    amf::AMFDataPtr data; const auto deadline = Clock::now() + std::chrono::milliseconds(250);
    for (;;) {
      checkDevice();
      const AMF_RESULT result = state_->encoder->QueryOutput(&data);
      if (result == AMF_OK && data) break;
      if (result != AMF_REPEAT && result != AMF_OK) requireAmf(result, encoderGpuOutputContract, "AMD H264 output query failed");
      if (Clock::now() >= deadline) throw Failure(encoderGpuOutputTimeout, "AMD H264 output did not complete before its deadline");
      Sleep(1);
    }
    amf::AMFBufferPtr buffer(data);
    if (!buffer || buffer->GetPts() != timing.pts || !buffer->GetSize() || buffer->GetSize() > encoderMaximumFrameBytes)
      throw Failure(encoderGpuOutputContract, "AMF output timestamp, type or size is invalid");
    requireAmf(buffer->Convert(amf::AMF_MEMORY_HOST), encoderGpuOutputContract, "Read AMD H264 host bitstream");
    amf_int64 type = -1;
    if (!buffer->GetNative() || !integer(buffer, AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, type) ||
        (type != AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR && type != AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I && type != AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_P))
      throw Failure(encoderGpuOutputContract, "AMF output is not a zero-reorder H264 picture");
    const auto bytes = std::span(static_cast<const uint8_t*>(buffer->GetNative()), size_t(buffer->GetSize()));
    const auto unit = inspectAnnexB(bytes, encoderMaximumFrameBytes);
    if (!unit.valid || (forceIdr && !unit.idr) || (unit.sps && (unit.spsProfile != 66 || unit.spsLevel != level_.value)) ||
        (unit.idr && (!unit.sps || !unit.pps)) || (unit.idr != (type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR)))
      throw Failure(encoderGpuOutputContract, "AMD H264 output violates Baseline AnnexB or IDR/header requirements");
    amf::AMFDataPtr extra;
    const AMF_RESULT remaining = state_->encoder->QueryOutput(&extra);
    if (extra || (remaining != AMF_REPEAT && remaining != AMF_OK)) throw Failure(encoderGpuOutputContract, "AMF produced an unexpected additional picture");
    Frame frame; frame.bytes.assign(bytes.begin(), bytes.end()); frame.ptsUs = ptsUs; frame.key = unit.idr;
    output.push_back(std::move(frame));
    // Actual validated bytes, not inventory/API initialization, establish this.
    info_.hardwareVerified = true; havePts_ = true; lastPts_ = ptsUs; ++frameCount_; keyRequested_ = false;
  }
 public:
  void finish(std::vector<Frame>&) override { /* Each accepted input already produced exactly its own output. */ }
  bool setBitrate(int bitrate) override {
    if (bitrate == bitrate_) return true;
    if (!dynamicBitrate_ || bitrate < encoderMinimumBitrate || bitrate > encoderMaximumBitrate || bitrate > maximumBitrate_ ||
        uint64_t(bitrate) > level_.bitrate || uint64_t(vbvBits(bitrate)) > level_.cpb) return false;
    try { checkDevice(); setRate(bitrate, false); bitrate_ = bitrate; return true; }
    catch (...) { return false; } // The facade discards this instance and retries on CPU.
  }
  int reportedBitrate() const override {
    amf_int64 value = 0;
    return integer(state_->encoder, AMF_VIDEO_ENCODER_TARGET_BITRATE, value) && value >= encoderMinimumBitrate && value <= encoderMaximumBitrate ? int(value) : 0;
  }
  EncoderTuning reportedTuning() const override { return {}; }
  void requestKeyFrame() override { keyRequested_ = true; }
  EncoderHardwareInfo hardwareInfo() const override { return info_; }
 private:
  bool runtimeWrite(const wchar_t* property) const {
    const amf::AMFPropertyInfo* info = nullptr;
    return state_->encoder->GetPropertyInfo(property, &info) == AMF_OK && info && (info->accessType & amf::AMF_PROPERTY_ACCESS_WRITE_RUNTIME) != 0;
  }
  void checkDevice() const {
    if (FAILED(state_->device->GetDeviceRemovedReason())) throw Failure(encoderGpuDeviceLost, "Selected AMD D3D11 device was lost");
  }
  amf_int64 vbvBits(int bitrate) const {
    return std::max<amf_int64>(1000, std::min<amf_int64>(amf_int64(bitrate) / fps_, amf_int64(encoderMaximumFrameBytes) * 8));
  }
  void setRate(int bitrate, bool initial) {
    const amf_int64 target = bitrate;
    const amf_int64 maximumAuBits = amf_int64(encoderMaximumFrameBytes) * 8;
    setInteger(state_->encoder, AMF_VIDEO_ENCODER_TARGET_BITRATE, target);
    setInteger(state_->encoder, AMF_VIDEO_ENCODER_PEAK_BITRATE, target);
    setInteger(state_->encoder, AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, vbvBits(bitrate));
    if (initial) setInteger(state_->encoder, AMF_VIDEO_ENCODER_MAX_AU_SIZE, maximumAuBits);
  }
  void checkConfiguration() const {
    auto* encoder = state_->encoder.operator->(); amf_int64 value = -1;
    for (const auto& expected : {std::pair{AMF_VIDEO_ENCODER_PROFILE, amf_int64(AMF_VIDEO_ENCODER_PROFILE_BASELINE)},
        std::pair{AMF_VIDEO_ENCODER_PROFILE_LEVEL, amf_int64(level_.value)},
        std::pair{AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES, amf_int64(1)}, std::pair{AMF_VIDEO_ENCODER_MAX_LTR_FRAMES, amf_int64(0)},
        std::pair{AMF_VIDEO_ENCODER_MAX_NUM_TEMPORAL_LAYERS, amf_int64(1)}, std::pair{AMF_VIDEO_ENCODER_NUM_TEMPORAL_ENHANCMENT_LAYERS, amf_int64(1)},
        std::pair{AMF_VIDEO_ENCODER_SCANTYPE, amf_int64(AMF_VIDEO_ENCODER_SCANTYPE_PROGRESSIVE)}, std::pair{AMF_VIDEO_ENCODER_COLOR_BIT_DEPTH, amf_int64(AMF_COLOR_BIT_DEPTH_8)},
        std::pair{AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf_int64(0)}, std::pair{AMF_VIDEO_ENCODER_MAX_CONSECUTIVE_BPICTURES, amf_int64(0)},
        std::pair{AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, amf_int64(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR)},
        std::pair{AMF_VIDEO_ENCODER_TARGET_BITRATE, amf_int64(bitrate_)}, std::pair{AMF_VIDEO_ENCODER_QUERY_TIMEOUT, amf_int64(0)},
        std::pair{AMF_VIDEO_ENCODER_INPUT_COLOR_PROFILE, amf_int64(AMF_VIDEO_CONVERTER_COLOR_PROFILE_601)},
        std::pair{AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, amf_int64(AMF_VIDEO_CONVERTER_COLOR_PROFILE_601)},
        std::pair{AMF_VIDEO_ENCODER_INPUT_COLOR_PRIMARIES, amf_int64(AMF_COLOR_PRIMARIES_UNSPECIFIED)},
        std::pair{AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES, amf_int64(AMF_COLOR_PRIMARIES_UNSPECIFIED)},
        std::pair{AMF_VIDEO_ENCODER_INPUT_TRANSFER_CHARACTERISTIC, amf_int64(AMF_COLOR_TRANSFER_CHARACTERISTIC_UNSPECIFIED)},
        std::pair{AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC, amf_int64(AMF_COLOR_TRANSFER_CHARACTERISTIC_UNSPECIFIED)}}) {
      if (!integer(encoder, expected.first, value) || value != expected.second) throw Failure(encoderGpuInitializationFailed, "AMD H264 negotiated settings differ from the requested contract");
    }
    for (const auto& expected : {std::pair{AMF_VIDEO_ENCODER_FULL_RANGE_COLOR, false}, std::pair{AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true},
        std::pair{AMF_VIDEO_ENCODER_ADAPTIVE_MINIGOP, false}, std::pair{AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, false},
        std::pair{AMF_VIDEO_ENCODER_RATE_CONTROL_SKIP_FRAME_ENABLE, false}}) {
      amf::AMFVariant actual;
      if (encoder->GetProperty(expected.first, &actual) != AMF_OK || actual.type != amf::AMF_VARIANT_BOOL || actual.boolValue != expected.second)
        throw Failure(encoderGpuInitializationFailed, "AMD H264 latency or limited-range configuration changed during initialization");
    }
    amf::AMFVariant picture, rate;
    if (encoder->GetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, &picture) != AMF_OK || picture.type != amf::AMF_VARIANT_SIZE || picture.sizeValue.width != width_ || picture.sizeValue.height != height_ ||
        encoder->GetProperty(AMF_VIDEO_ENCODER_FRAMERATE, &rate) != AMF_OK || rate.type != amf::AMF_VARIANT_RATE || rate.rateValue.num != uint32_t(fps_) || rate.rateValue.den != 1)
      throw Failure(encoderGpuInitializationFailed, "AMD H264 dimensions/frame rate were not applied");
  }
  static std::optional<amf_detail::PlaneLayout> plane(amf::AMFPlane* value, uint32_t pixels, uint32_t rows, uint32_t pixelBytes) {
    if (!value || !value->GetNative() || value->IsTiled()) return {};
    const auto layout = amf_detail::planeLayout(pixels, rows, pixelBytes, value->GetWidth(), value->GetHeight(), value->GetPixelSizeInBytes(), value->GetOffsetX(), value->GetOffsetY(), value->GetHPitch(), value->GetVPitch());
    if (!layout || reinterpret_cast<uintptr_t>(value->GetNative()) > UINTPTR_MAX - layout->bytes) return {};
    return layout;
  }
  static void copyPlane(amf::AMFPlane* target, const amf_detail::PlaneLayout& layout, const uint8_t* source, size_t rowBytes) {
    auto* destination = static_cast<uint8_t*>(target->GetNative());
    for (size_t row = 0; row < layout.rows; ++row) std::memcpy(destination + row * layout.pitch, source + row * rowBytes, rowBytes);
  }
  std::unique_ptr<State> state_;
  int width_, height_, fps_, bitrate_;
  amf_int64 maximumBitrate_ = 0;
  amf_detail::Level level_{};
  uint64_t frameCount_ = 0, lastPts_ = 0;
  bool havePts_ = false, keyRequested_ = false, dynamicBitrate_ = false, textureInput_ = false;
  EncoderHardwareInfo info_;
};
}
std::unique_ptr<EncoderImplementation> createAmfEncoder(int width, int height, int fps, int bitrate, const Adapter& adapter, ID3D11Device* shared) {
  // The capture's device first; an AMF runtime that refuses it opens on its own device.
  if (shared) { try { return std::make_unique<AmfEncoder>(width, height, fps, bitrate, adapter, shared); } catch (...) {} }
  return std::make_unique<AmfEncoder>(width, height, fps, bitrate, adapter, nullptr);
}
}
