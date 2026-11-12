// Multitap implementation using NVIDIA's MIT-licensed public API declarations.
// No SDK sample implementation, CUDA dependency or driver binary is bundled.
#include "gpu_h264.hpp"
#include "gpu_h264_nvenc_policy.hpp"
#include "gpu_h264_nvenc_config.hpp"
#include "owned_output_size.hpp"
#include <nvEncodeAPI.h>
#include <optional>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

namespace host::gpu {
namespace {
void requireNv(NVENCSTATUS value, uint32_t code, const char* operation,
    EncoderGpuFailureSite site = EncoderGpuFailureSite::nvencInitialize) {
  if (value == NV_ENC_SUCCESS) return;
  if (value == NV_ENC_ERR_INVALID_VERSION) code = encoderGpuApiUnsupported;
  else if (value == NV_ENC_ERR_OUT_OF_MEMORY) code = encoderGpuAllocationFailed;
  else if (value == NV_ENC_ERR_DEVICE_NOT_EXIST || value == NV_ENC_ERR_INVALID_ENCODERDEVICE || value == NV_ENC_ERR_INVALID_DEVICE) code = encoderGpuDeviceLost;
  throw Failure(code, operation, failureDetail(site, uint32_t(value)));
}
template<class Function> Function exported(HMODULE module, const char* name) {
  const FARPROC address = GetProcAddress(module, name);
  if (!address) throw Failure(encoderGpuApiUnsupported, "NVENC API entry unavailable");
  static_assert(sizeof(Function) == sizeof(address));
  Function result; std::memcpy(&result, &address, sizeof(result)); return result;
}
struct Slot {
  NV_ENC_INPUT_PTR input = nullptr;
  NV_ENC_OUTPUT_PTR output = nullptr;
  HANDLE event = nullptr;
  bool registered = false;
};
struct Resources {
  HMODULE module = nullptr;
  void* encoder = nullptr;
  NV_ENCODE_API_FUNCTION_LIST api{};
  Com<ID3D11Device> device;
  Com<ID3D11DeviceContext> context;
  std::array<Slot, 4> slots;
  HANDLE flushEvent = nullptr;
  bool initialized = false, flushRegistered = false, pictureMayBePending = false;
  // gpu-capture-v1: the capture's NV12 textures registered with this session
  // and the one mapped for the picture in flight (released after the drain).
  NvencRegistrationCache<Com<ID3D11Texture2D>, NV_ENC_REGISTERED_PTR, 4> registrations;
  NV_ENC_INPUT_PTR mapped = nullptr;
};
struct RetainedResources {
  Resources value;
  RetainedResources* next = nullptr;
};
struct ProcessQuarantine {
  std::mutex mutex;
  NvencProcessAdmission admission;
  RetainedResources* first = nullptr;
  uint32_t lastNativeStatus = 0;
};
static_assert(NV_ENC_PIC_TYPE_P == 0 && NV_ENC_PIC_TYPE_I == 2 && NV_ENC_PIC_TYPE_IDR == 3 && NV_ENC_PIC_TYPE_NONREF_P == 7);
ProcessQuarantine& processQuarantine() {
  // Intentionally no static destruction: an incomplete driver operation must
  // retain its DLL, D3D references, buffers and event handles until process exit.
  static auto* state = new ProcessQuarantine;
  return *state;
}
NvencCloseResult closeResources(Resources& state) noexcept {
  auto& api = state.api;
  const bool hadEncoder = state.encoder != nullptr;
  if (state.encoder) {
    const auto closed = nvencCloseInSdkOrder(state.pictureMayBePending, [&]() -> NvencDrainResult {
      if (!state.initialized || !state.flushRegistered) return {};
      if (!ResetEvent(state.flushEvent)) return {true, false, false, GetLastError()};
      const NvencOutputDeadline deadline{GetTickCount64()};
      NV_ENC_PIC_PARAMS end{}; end.version = NV_ENC_PIC_PARAMS_VER;
      end.encodePicFlags = NV_ENC_PIC_FLAG_EOS; end.completionEvent = state.flushEvent;
      const NVENCSTATUS result = api.nvEncEncodePicture(state.encoder, &end);
      if (result != NV_ENC_SUCCESS) return {true, false, false, uint32_t(result)};
      const DWORD remaining = deadline.remaining(GetTickCount64());
      const DWORD wait = remaining ? WaitForSingleObject(state.flushEvent, remaining) : WAIT_TIMEOUT;
      return {true, true, wait == WAIT_OBJECT_0, wait};
    }, [&]() -> NvencNativeResult {
      // NVIDIA requires completed flush, then resource release, then destroy.
      // A failed step retains the rest; destroy is not a substitute for drain.
      if (state.mapped) {
        const auto result = api.nvEncUnmapInputResource(state.encoder, state.mapped);
        if (result != NV_ENC_SUCCESS) return {false, uint32_t(result)};
        state.mapped = nullptr;
      }
      for (auto& registration : state.registrations.entries) {
        if (!registration.live) continue;
        const auto result = api.nvEncUnregisterResource(state.encoder, registration.handle);
        if (result != NV_ENC_SUCCESS) return {false, uint32_t(result)};
        registration = {};
      }
      for (auto& slot : state.slots) {
        if (slot.registered) {
          NV_ENC_EVENT_PARAMS event{}; event.version = NV_ENC_EVENT_PARAMS_VER; event.completionEvent = slot.event;
          const auto result = api.nvEncUnregisterAsyncEvent(state.encoder, &event);
          if (result != NV_ENC_SUCCESS) return {false, uint32_t(result)};
          slot.registered = false;
        }
        if (slot.output) {
          const auto result = api.nvEncDestroyBitstreamBuffer(state.encoder, slot.output);
          if (result != NV_ENC_SUCCESS) return {false, uint32_t(result)};
          slot.output = nullptr;
        }
        if (slot.input) {
          const auto result = api.nvEncDestroyInputBuffer(state.encoder, slot.input);
          if (result != NV_ENC_SUCCESS) return {false, uint32_t(result)};
          slot.input = nullptr;
        }
      }
      if (state.flushRegistered) {
        NV_ENC_EVENT_PARAMS event{}; event.version = NV_ENC_EVENT_PARAMS_VER; event.completionEvent = state.flushEvent;
        const auto result = api.nvEncUnregisterAsyncEvent(state.encoder, &event);
        if (result != NV_ENC_SUCCESS) return {false, uint32_t(result)};
        state.flushRegistered = false;
      }
      return {true, 0};
    }, [&]() -> NvencNativeResult {
      const auto result = api.nvEncDestroyEncoder(state.encoder);
      return {result == NV_ENC_SUCCESS, uint32_t(result)};
    });
    if (!closed.released) return closed;
    state.encoder = nullptr;
  }
  for (auto& slot : state.slots) if (slot.event) { CloseHandle(slot.event); slot.event = nullptr; }
  if (state.flushEvent) { CloseHandle(state.flushEvent); state.flushEvent = nullptr; }
  if (state.module) { FreeLibrary(state.module); state.module = nullptr; }
  return {true, 0, hadEncoder ? NvencObservedClose::released : NvencObservedClose::noEncoder};
}
struct State : Resources {
  // Optional observation is allocated before driver resources. Its failure
  // is swallowed; it has no authority over encoding or cleanup.
  std::shared_ptr<NvencObservationState> observation = tryNvencObservation();
  // Allocate retention storage before creating any fallible driver resource.
  // Quarantine itself performs no allocation during exception unwinding.
  std::unique_ptr<RetainedResources> retention = std::make_unique<RetainedResources>();
  State() {
    auto& process = processQuarantine(); std::lock_guard lock(process.mutex);
    if (!process.admission.acquire()) throw Failure(encoderGpuDeviceLost,
      "NVENC admission fenced after incomplete teardown", failureDetail(EncoderGpuFailureSite::nvencEndOfStream, process.lastNativeStatus));
  }
  ~State() {
    const auto closed = closeResources(*this);
    auto& process = processQuarantine(); std::lock_guard lock(process.mutex);
    if (closed.released) {
      (void)process.admission.release();
      if (observation) observation->closed(closed.disposition, closed.nativeStatus, process.admission.fenced);
      return;
    }
    (void)process.admission.retain(); process.lastNativeStatus = closed.nativeStatus;
    if (observation) observation->closed(closed.disposition, closed.nativeStatus, process.admission.fenced);
    retention->value = std::move(static_cast<Resources&>(*this));
    retention->next = process.first; process.first = retention.release();
  }
};
// The capture's device lies on exactly this adapter (LUID, vendor, device).
bool deviceOnAdapter(ID3D11Device* device, const Adapter& adapter) {
  Com<IDXGIDevice> dxgi; Com<IDXGIAdapter> actual; DXGI_ADAPTER_DESC description{};
  if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&actual)) || FAILED(actual->GetDesc(&description))) return false;
  const uint64_t luid = (uint64_t(uint32_t(description.AdapterLuid.HighPart)) << 32) | description.AdapterLuid.LowPart;
  return luid == adapter.luid && description.VendorId == adapter.vendorId && description.DeviceId == adapter.deviceId;
}
class NvencEncoder final : public EncoderImplementation {
 public:
  NvencEncoder(int width, int height, int fps, int bitrate, const Adapter& adapter, ID3D11Device* shared)
      : state_(std::make_unique<State>()), width_(width), height_(height), fps_(fps), bitrate_(bitrate) {
    validateFrameMemory(width, height, 0, 12);
    if (adapter.vendorId != 0x10de) throw Failure(encoderGpuVendorUnsupported, "Selected adapter is not an NVIDIA GPU");
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) throw Failure(encoderGpuAdapterUnavailable, "NVENC adapter inventory unavailable");
    Com<IDXGIAdapter1> selected;
    for (UINT ordinal = 0; ordinal < 256; ++ordinal) {
      Com<IDXGIAdapter1> candidate;
      const HRESULT found = factory->EnumAdapters1(ordinal, &candidate);
      if (found == DXGI_ERROR_NOT_FOUND) break;
      if (FAILED(found)) throw Failure(encoderGpuAdapterUnavailable, "NVENC adapter inventory failed");
      DXGI_ADAPTER_DESC1 actual{};
      if (FAILED(candidate->GetDesc1(&actual))) continue;
      const uint64_t luid = (uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) | actual.AdapterLuid.LowPart;
      if (luid == adapter.luid && actual.VendorId == adapter.vendorId && actual.DeviceId == adapter.deviceId && !(actual.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { selected = candidate; break; }
    }
    if (!selected) throw Failure(encoderGpuAdapterUnavailable, "Selected NVENC adapter disappeared");
    if (shared && deviceOnAdapter(shared, adapter)) {
      // gpu-capture-v1: the session opens on the capture's own device, so the
      // capture's NV12 textures go in without a copy. Capture and encode run on
      // one thread; WGC and the driver may use the device from others.
      state_->device = shared; shared->GetImmediateContext(&state_->context);
      Com<ID3D11Multithread> protection;
      if (SUCCEEDED(state_->device.As(&protection))) protection->SetMultithreadProtected(TRUE);
      sharedDevice_ = true;
    } else {
      // UNKNOWN plus the exact DXGI adapter never silently selects WARP or a
      // different GPU. Capture's existing WARP fallback remains independent.
      const HRESULT device = D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &state_->device, nullptr, &state_->context);
      if (FAILED(device)) throw Failure(encoderGpuInitializationFailed, "Selected NVENC D3D11 device could not be created");
    }
    state_->module = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!state_->module) throw Failure(encoderGpuDriverUnavailable, "Installed NVENC driver API unavailable");
    using Version = NVENCSTATUS(NVENCAPI*)(uint32_t*);
    using Create = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    uint32_t maximum = 0;
    requireNv(exported<Version>(state_->module, "NvEncodeAPIGetMaxSupportedVersion")(&maximum), encoderGpuApiUnsupported, "Query NVENC driver API version");
    // The pinned 12.0 structure ABI supports RTX4090's original driver line;
    // newer 13.x drivers negotiate this existing API. Never relabel newer
    // structures as an older version or require 13.x for ordinary H264.
    if (maximum < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) throw Failure(encoderGpuApiUnsupported, "Installed NVENC API is older than this pinned structure ABI");
    auto& api = state_->api; api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    requireNv(exported<Create>(state_->module, "NvEncodeAPICreateInstance")(&api), encoderGpuApiUnsupported, "Create NVENC API function table");
    if (!api.nvEncOpenEncodeSessionEx || !api.nvEncDestroyEncoder || !api.nvEncGetEncodeCaps || !api.nvEncGetEncodeProfileGUIDCount || !api.nvEncGetEncodeProfileGUIDs ||
        !api.nvEncGetInputFormatCount || !api.nvEncGetInputFormats || !api.nvEncGetEncodePresetConfigEx || !api.nvEncInitializeEncoder ||
        !api.nvEncCreateInputBuffer || !api.nvEncDestroyInputBuffer || !api.nvEncCreateBitstreamBuffer || !api.nvEncDestroyBitstreamBuffer ||
        !api.nvEncLockInputBuffer || !api.nvEncUnlockInputBuffer || !api.nvEncEncodePicture || !api.nvEncLockBitstream || !api.nvEncUnlockBitstream ||
        !api.nvEncRegisterAsyncEvent || !api.nvEncUnregisterAsyncEvent || !api.nvEncReconfigureEncoder) throw Failure(encoderGpuApiUnsupported, "NVENC function table is incomplete");
    // Texture input is an addition: without these entries the byte path stays.
    textureInput_ = sharedDevice_ && api.nvEncRegisterResource && api.nvEncUnregisterResource && api.nvEncMapInputResource && api.nvEncUnmapInputResource;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{}; open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.device = state_->device.Get(); open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX; open.apiVersion = NVENCAPI_VERSION;
    requireNv(api.nvEncOpenEncodeSessionEx(&open, &state_->encoder), encoderGpuInitializationFailed, "Open selected GPU NVENC session");
    const auto cap = [&](NV_ENC_CAPS name, bool optional = false) -> uint32_t {
      NV_ENC_CAPS_PARAM query{}; query.version = NV_ENC_CAPS_PARAM_VER; query.capsToQuery = name; int value = 0;
      const auto result = api.nvEncGetEncodeCaps(state_->encoder, NV_ENC_CODEC_H264_GUID, &query, &value);
      if (optional && result != NV_ENC_SUCCESS) return 0;
      requireNv(result, encoderGpuCapabilityUnsupported, "Query selected GPU H264 capability");
      if (value < 0) throw Failure(encoderGpuCapabilityUnsupported, "GPU H264 capability is invalid");
      return uint32_t(value);
    };
    Limits limits{cap(NV_ENC_CAPS_WIDTH_MIN, true), cap(NV_ENC_CAPS_HEIGHT_MIN, true), cap(NV_ENC_CAPS_WIDTH_MAX), cap(NV_ENC_CAPS_HEIGHT_MAX),
      cap(NV_ENC_CAPS_MB_NUM_MAX, true), cap(NV_ENC_CAPS_MB_PER_SEC_MAX, true)};
    if (!pictureFits(limits, uint32_t(width), uint32_t(height), uint32_t(fps)) || !cap(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT) || !cap(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE))
      throw Failure(encoderGpuCapabilityUnsupported, "Selected GPU cannot satisfy the requested low-latency H264 picture");
    dynamicBitrate_ = cap(NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE, true) != 0;
    caps_ = {cap(NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION, true) != 0 && api.nvEncInvalidateRefFrames, cap(NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES, true) != 0};
    uint32_t count = 0, actual = 0;
    requireNv(api.nvEncGetEncodeProfileGUIDCount(state_->encoder, NV_ENC_CODEC_H264_GUID, &count), encoderGpuCapabilityUnsupported, "Query GPU H264 profiles");
    if (!count || count > 256) throw Failure(encoderGpuCapabilityUnsupported, "GPU H264 profile inventory is invalid");
    std::vector<GUID> profiles(count);
    requireNv(api.nvEncGetEncodeProfileGUIDs(state_->encoder, NV_ENC_CODEC_H264_GUID, profiles.data(), count, &actual), encoderGpuCapabilityUnsupported, "Read GPU H264 profiles");
    if (actual > count || std::none_of(profiles.begin(), profiles.begin() + actual, [](const GUID& value) { return IsEqualGUID(value, NV_ENC_H264_PROFILE_BASELINE_GUID); }))
      throw Failure(encoderGpuCapabilityUnsupported, "GPU H264 Baseline profile unavailable");
    requireNv(api.nvEncGetInputFormatCount(state_->encoder, NV_ENC_CODEC_H264_GUID, &count), encoderGpuCapabilityUnsupported, "Query GPU H264 input formats");
    if (!count || count > 256) throw Failure(encoderGpuCapabilityUnsupported, "GPU H264 input inventory is invalid");
    std::vector<NV_ENC_BUFFER_FORMAT> formats(count); actual = 0;
    requireNv(api.nvEncGetInputFormats(state_->encoder, NV_ENC_CODEC_H264_GUID, formats.data(), count, &actual), encoderGpuCapabilityUnsupported, "Read GPU H264 input formats");
    if (actual > count || std::find(formats.begin(), formats.begin() + actual, NV_ENC_BUFFER_FORMAT_NV12) == formats.begin() + actual)
      throw Failure(encoderGpuCapabilityUnsupported, "GPU NV12 input unavailable");
    NV_ENC_PRESET_CONFIG preset{}; preset.version = NV_ENC_PRESET_CONFIG_VER; preset.presetCfg.version = NV_ENC_CONFIG_VER;
    requireNv(api.nvEncGetEncodePresetConfigEx(state_->encoder, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset),
      encoderGpuCapabilityUnsupported, "Query GPU low-latency H264 preset");
    config_ = preset.presetCfg; nvencStreamingConfig(config_, width, height, fps, bitrate, caps_);
    init_.version = NV_ENC_INITIALIZE_PARAMS_VER; init_.encodeGUID = NV_ENC_CODEC_H264_GUID; init_.presetGUID = NV_ENC_PRESET_P1_GUID;
    init_.encodeWidth = uint32_t(width); init_.encodeHeight = uint32_t(height); init_.darWidth = uint32_t(width); init_.darHeight = uint32_t(height);
    init_.frameRateNum = uint32_t(fps); init_.frameRateDen = 1; init_.enableEncodeAsync = 1; init_.enablePTD = 1;
    init_.maxEncodeWidth = uint32_t(width); init_.maxEncodeHeight = uint32_t(height);
    init_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY; init_.encodeConfig = &config_;
    requireNv(api.nvEncInitializeEncoder(state_->encoder, &init_), encoderGpuInitializationFailed, "Initialize selected GPU H264 encoder");
    state_->initialized = true;
    state_->flushEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state_->flushEvent) throw Failure(encoderGpuAllocationFailed, "Allocate GPU flush event",
      failureDetail(EncoderGpuFailureSite::nvencEndOfStream, GetLastError()));
    NV_ENC_EVENT_PARAMS flush{}; flush.version = NV_ENC_EVENT_PARAMS_VER; flush.completionEvent = state_->flushEvent;
    requireNv(api.nvEncRegisterAsyncEvent(state_->encoder, &flush), encoderGpuInitializationFailed,
      "Register GPU flush event", EncoderGpuFailureSite::nvencEndOfStream); state_->flushRegistered = true;
    for (auto& slot : state_->slots) {
      NV_ENC_CREATE_INPUT_BUFFER input{}; input.version = NV_ENC_CREATE_INPUT_BUFFER_VER; input.width = uint32_t(width); input.height = uint32_t(height); input.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
      requireNv(api.nvEncCreateInputBuffer(state_->encoder, &input), encoderGpuAllocationFailed, "Allocate GPU H264 input"); slot.input = input.inputBuffer;
      NV_ENC_CREATE_BITSTREAM_BUFFER output{}; output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
      requireNv(api.nvEncCreateBitstreamBuffer(state_->encoder, &output), encoderGpuAllocationFailed, "Allocate GPU H264 output"); slot.output = output.bitstreamBuffer;
      slot.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!slot.event) throw Failure(encoderGpuAllocationFailed, "Allocate GPU completion event");
      NV_ENC_EVENT_PARAMS event{}; event.version = NV_ENC_EVENT_PARAMS_VER; event.completionEvent = slot.event;
      requireNv(api.nvEncRegisterAsyncEvent(state_->encoder, &event), encoderGpuInitializationFailed, "Register GPU completion event"); slot.registered = true;
    }
    info_.initialized = true; info_.adapterLuid = adapter.luid; info_.vendorId = adapter.vendorId; info_.deviceId = adapter.deviceId; info_.adapterName = adapter.description;
  }
  void encode(const uint8_t* nv12, uint64_t ptsUs, std::vector<Frame>& output) override {
    if (!nv12) throw Failure(encoderGpuOutputContract, "GPU input timestamp is invalid", failureDetail(EncoderGpuFailureSite::nvencInputTimestamp));
    requireTimestamp(ptsUs);
    auto& api = state_->api; auto& slot = state_->slots[size_t(frameCount_ % state_->slots.size())];
    NV_ENC_LOCK_INPUT_BUFFER input{}; input.version = NV_ENC_LOCK_INPUT_BUFFER_VER; input.inputBuffer = slot.input; input.doNotWait = 1;
    requireNv(api.nvEncLockInputBuffer(state_->encoder, &input), encoderGpuSubmissionFailed, "Lock GPU NV12 input", EncoderGpuFailureSite::nvencSubmit);
    if (!input.bufferDataPtr || input.pitch < uint32_t(width_)) { api.nvEncUnlockInputBuffer(state_->encoder, slot.input);
      throw Failure(encoderGpuOutputContract, "GPU NV12 pitch is invalid", failureDetail(EncoderGpuFailureSite::nvencInputPitch)); }
    auto* target = static_cast<uint8_t*>(input.bufferDataPtr);
    for (int row = 0; row < height_ + height_ / 2; ++row) std::memcpy(target + size_t(row) * input.pitch, nv12 + size_t(row) * size_t(width_), size_t(width_));
    requireNv(api.nvEncUnlockInputBuffer(state_->encoder, slot.input), encoderGpuSubmissionFailed, "Unlock GPU NV12 input", EncoderGpuFailureSite::nvencSubmit);
    submit(slot.input, NV_ENC_BUFFER_FORMAT_NV12, input.pitch, ptsUs, output, NvencObservedInput::bytes);
  }
  // gpu-capture-v1: the capture's NV12 texture of this session's own device is
  // registered (once, NvencRegistrationCache), mapped and encoded as it is.
  // Any other picture is encoded from its bytes.
  void encodePicture(const EncoderPicture& picture, uint64_t ptsUs, std::vector<Frame>& output) override {
    if (!picture.texture || !textureInput_ || !onDevice(picture.texture)) { EncoderImplementation::encodePicture(picture, ptsUs, output); return; }
    requireTimestamp(ptsUs);
    D3D11_TEXTURE2D_DESC desc{}; picture.texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12 || desc.Width != UINT(width_) || desc.Height != UINT(height_) || desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
      throw Failure(encoderGpuOutputContract, "GPU NV12 picture does not match the encoder", failureDetail(EncoderGpuFailureSite::textureRegister));
    auto& api = state_->api;
    const auto handle = state_->registrations.acquire(Com<ID3D11Texture2D>(picture.texture), [&](const Com<ID3D11Texture2D>& texture) {
      NV_ENC_REGISTER_RESOURCE resource{}; resource.version = NV_ENC_REGISTER_RESOURCE_VER; resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
      resource.width = uint32_t(width_); resource.height = uint32_t(height_); resource.pitch = 0; resource.subResourceIndex = 0;
      resource.resourceToRegister = texture.Get(); resource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12; resource.bufferUsage = NV_ENC_INPUT_IMAGE;
      requireNv(api.nvEncRegisterResource(state_->encoder, &resource), encoderGpuAllocationFailed, "Register GPU NV12 picture", EncoderGpuFailureSite::textureRegister);
      if (!resource.registeredResource) throw Failure(encoderGpuOutputContract, "GPU NV12 picture registration is empty", failureDetail(EncoderGpuFailureSite::textureRegister));
      return resource.registeredResource;
    }, [&](NV_ENC_REGISTERED_PTR registered) {
      requireNv(api.nvEncUnregisterResource(state_->encoder, registered), encoderGpuSubmissionFailed, "Unregister GPU NV12 picture", EncoderGpuFailureSite::textureRegister);
    });
    NV_ENC_MAP_INPUT_RESOURCE map{}; map.version = NV_ENC_MAP_INPUT_RESOURCE_VER; map.registeredResource = handle;
    requireNv(api.nvEncMapInputResource(state_->encoder, &map), encoderGpuSubmissionFailed, "Map GPU NV12 picture", EncoderGpuFailureSite::textureMap);
    if (!map.mappedResource) throw Failure(encoderGpuOutputContract, "GPU NV12 picture mapping is empty", failureDetail(EncoderGpuFailureSite::textureMap));
    // A failed picture stays mapped: the close path unmaps it after its drain.
    state_->mapped = map.mappedResource;
    submit(map.mappedResource, map.mappedBufferFmt, uint32_t(width_), ptsUs, output, NvencObservedInput::texture);
    requireNv(api.nvEncUnmapInputResource(state_->encoder, map.mappedResource), encoderGpuSubmissionFailed, "Unmap GPU NV12 picture", EncoderGpuFailureSite::textureUnmap);
    state_->mapped = nullptr;
  }
  ID3D11Device* textureDevice() const override { return textureInput_ ? state_->device.Get() : nullptr; }
 private:
  bool onDevice(ID3D11Texture2D* texture) const { Com<ID3D11Device> owner; texture->GetDevice(&owner); return owner.Get() == state_->device.Get(); }
  void requireTimestamp(uint64_t ptsUs) const {
    if (ptsUs > uint64_t(INT64_MAX) || (havePts_ && ptsUs <= lastPts_) || frameCount_ == UINT64_MAX)
      throw Failure(encoderGpuOutputContract, "GPU input timestamp is invalid", failureDetail(EncoderGpuFailureSite::nvencInputTimestamp));
  }
  // One picture (an input buffer, or a mapped texture) to one validated output.
  EncoderGpuFailure observedFailure(EncoderGpuFailureSite site, uint32_t nativeStatus) const {
    auto detail = failureDetail(site, nativeStatus);
    detail.nvencPending = state_->observation;
    return detail;
  }
  void submit(NV_ENC_INPUT_PTR inputBuffer, NV_ENC_BUFFER_FORMAT format, uint32_t pitch, uint64_t ptsUs, std::vector<Frame>& output, NvencObservedInput input) {
    auto& api = state_->api; auto& slot = state_->slots[size_t(frameCount_ % state_->slots.size())];
    const bool forceIdr = frameCount_ == 0 || keyRequested_;
    NV_ENC_PIC_PARAMS picture{}; picture.version = NV_ENC_PIC_PARAMS_VER; picture.inputBuffer = inputBuffer; picture.outputBitstream = slot.output;
    picture.completionEvent = slot.event; picture.inputWidth = uint32_t(width_); picture.inputHeight = uint32_t(height_); picture.inputPitch = pitch;
    picture.bufferFmt = format; picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputTimeStamp = ptsUs; picture.inputDuration = uint64_t(1000000 / fps_); picture.frameIdx = uint32_t(frameCount_ & UINT32_MAX);
    if (forceIdr) picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    state_->pictureMayBePending = true;
    const NVENCSTATUS submitted = api.nvEncEncodePicture(state_->encoder, &picture);
    // Queued/delayed pictures cannot be replayed honestly by this synchronous
    // facade. Reject them before publication and retry this same input on CPU.
    if (submitted == NV_ENC_ERR_NEED_MORE_INPUT) throw Failure(encoderGpuOutputContract, "GPU delayed a zero-reorder picture",
      failureDetail(EncoderGpuFailureSite::nvencNeedMoreInput, uint32_t(submitted)));
    requireNv(submitted, encoderGpuSubmissionFailed, "Submit GPU H264 picture", EncoderGpuFailureSite::nvencSubmit);
    if (state_->observation) state_->observation->submitted(input);
    const NvencOutputDeadline deadline{GetTickCount64()};
    const uint64_t waitStarted = state_->observation ? GetTickCount64() : 0;
    const DWORD wait = WaitForSingleObject(slot.event, deadline.remaining(GetTickCount64()));
    if (state_->observation) state_->observation->waited(waitStarted, GetTickCount64());
    if (wait != WAIT_OBJECT_0) throw Failure(encoderGpuOutputTimeout, "GPU H264 output did not complete before its deadline",
      observedFailure(EncoderGpuFailureSite::nvencOutputWait, wait));
    NV_ENC_LOCK_BITSTREAM encoded{};
    const auto locked = nvencLockOutputBounded(deadline, uint32_t(NV_ENC_SUCCESS), uint32_t(NV_ENC_ERR_LOCK_BUSY),
      [] { return GetTickCount64(); }, [&] {
        encoded = {}; encoded.version = NV_ENC_LOCK_BITSTREAM_VER; encoded.outputBitstream = slot.output; encoded.doNotWait = 1;
        return api.nvEncLockBitstream(state_->encoder, &encoded);
      }, [](uint32_t delay) { Sleep(delay); });
    if (locked.expired) throw Failure(encoderGpuOutputTimeout, "GPU H264 output lock did not complete before its deadline",
      observedFailure(EncoderGpuFailureSite::nvencOutputLock, locked.nativeStatus));
    requireNv(NVENCSTATUS(locked.nativeStatus), encoderGpuOutputContract, "Read GPU H264 output", EncoderGpuFailureSite::nvencOutputLock);
    Frame frame;
    try {
      const auto metadata = nvencOutputMetadataFailure(encoded.bitstreamBufferPtr != nullptr,
        encoded.bitstreamSizeInBytes, encoderMaximumFrameBytes, encoded.outputTimeStamp, ptsUs, uint32_t(encoded.pictureType));
      if (metadata != NvencMetadataFailure::none) {
        const auto site = metadata == NvencMetadataFailure::buffer ? EncoderGpuFailureSite::nvencOutputBuffer :
          metadata == NvencMetadataFailure::timestamp ? EncoderGpuFailureSite::nvencOutputTimestamp : EncoderGpuFailureSite::nvencOutputPictureType;
        throw Failure(encoderGpuOutputContract, "GPU H264 output timestamp, type or size is invalid",
          failureDetail(site));
      }
      const auto bytes = std::span(static_cast<const uint8_t*>(encoded.bitstreamBufferPtr), size_t(encoded.bitstreamSizeInBytes));
      const auto unit = inspectAnnexB(bytes, encoderMaximumFrameBytes);
      if (!unit.valid)
        throw Failure(encoderGpuOutputContract, "GPU H264 output did not satisfy the Baseline, IDR and AnnexB contract",
          failureDetail(EncoderGpuFailureSite::nvencOutputAnnexB));
      if (forceIdr && !unit.idr) throw Failure(encoderGpuOutputContract, "GPU H264 forced IDR is missing",
        failureDetail(EncoderGpuFailureSite::nvencOutputForcedIdr));
      if (unit.idr && (!unit.sps || !unit.pps)) throw Failure(encoderGpuOutputContract, "GPU H264 IDR headers are missing",
        failureDetail(EncoderGpuFailureSite::nvencOutputHeaders));
      if (unit.sps && unit.spsProfile != 66) throw Failure(encoderGpuOutputContract, "GPU H264 Baseline profile is missing",
        failureDetail(EncoderGpuFailureSite::nvencOutputBaseline));
      frame.bytes.assign(bytes.begin(), bytes.end()); frame.ptsUs = ptsUs; frame.key = unit.idr;
    } catch (...) { api.nvEncUnlockBitstream(state_->encoder, slot.output); throw; }
    requireNv(api.nvEncUnlockBitstream(state_->encoder, slot.output), encoderGpuOutputContract, "Release GPU H264 output", EncoderGpuFailureSite::nvencOutputUnlock);
    state_->pictureMayBePending = false;
    if (history_) history_->encoded(ptsUs, frame.key);
    output.push_back(std::move(frame));
    if (state_->observation) state_->observation->output();
    // GPU inventory and successful initialization alone never set this flag.
    info_.hardwareVerified = true; havePts_ = true; lastPts_ = ptsUs; ++frameCount_; keyRequested_ = false;
  }
 public:
  // NVENC marks each stale reference, and what was reconstructed from it, unusable, and codes
  // the next frame from older ones, intra when none is left (nvEncodeAPI.h). inputTimeStamp
  // is the PTS, so the client's PTS names the frame.
  bool recover(uint64_t lastGood) override {
    const auto stale = history_ ? history_->after(lastGood) : std::nullopt;
    if (!stale) return false;
    for (const uint64_t pts : *stale) if (state_->api.nvEncInvalidateRefFrames(state_->encoder, pts) != NV_ENC_SUCCESS) return false;
    return true;
  }
  // The larger DPB through a reconfiguration that resets the encoder at an IDR (NVIDIA lists
  // GOP, sync mode, maximum size and PTD as what cannot change; not run on a GPU here). A
  // refusal keeps the running configuration: every LOSS then costs an IDR, as before.
  bool widenReferences() override {
    if (history_ || !nvencRecovers(caps_)) return false;
    auto next = config_; nvencStreamingConfig(next, width_, height_, fps_, bitrate_, caps_, true);
    NV_ENC_RECONFIGURE_PARAMS request{}; request.version = NV_ENC_RECONFIGURE_PARAMS_VER; request.reInitEncodeParams = init_; request.reInitEncodeParams.encodeConfig = &next;
    request.resetEncoder = 1; request.forceIDR = 1;
    if (state_->api.nvEncReconfigureEncoder(state_->encoder, &request) != NV_ENC_SUCCESS) return false;
    config_ = next; init_.encodeConfig = &config_; keyRequested_ = true;
    history_.emplace(config_.encodeCodecConfig.h264Config.maxNumRefFrames);
    return true;
  }
  void finish(std::vector<Frame>&) override { /* Every accepted input already produced its exact output. */ }
  bool setBitrate(int bitrate) override {
    if (bitrate == bitrate_) return true;
    if (!dynamicBitrate_ || bitrate < encoderMinimumBitrate || bitrate > encoderMaximumBitrate) return false;
    auto next = config_; nvencRate(next, bitrate, fps_);
    NV_ENC_RECONFIGURE_PARAMS request{}; request.version = NV_ENC_RECONFIGURE_PARAMS_VER; request.reInitEncodeParams = init_; request.reInitEncodeParams.encodeConfig = &next;
    const auto status = state_->api.nvEncReconfigureEncoder(state_->encoder, &request);
    requireNv(status, encoderGpuReconfigureFailed, "Reconfigure GPU H264 bitrate", EncoderGpuFailureSite::nvencReconfigure);
    config_ = next; init_.encodeConfig = &config_; bitrate_ = bitrate; return true;
  }
  int reportedBitrate() const override { return bitrate_; }
  EncoderTuning reportedTuning() const override { return {}; }
  void requestKeyFrame() override { keyRequested_ = true; }
  EncoderHardwareInfo hardwareInfo() const override { return info_; }
 private:
  std::unique_ptr<State> state_;
  int width_, height_, fps_, bitrate_;
  bool dynamicBitrate_ = false, keyRequested_ = false, havePts_ = false, sharedDevice_ = false, textureInput_ = false;
  NvencCaps caps_;
  std::optional<ReferenceHistory> history_;  // only once widened (widenReferences)
  uint64_t frameCount_ = 0, lastPts_ = 0;
  NV_ENC_CONFIG config_{};
  NV_ENC_INITIALIZE_PARAMS init_{};
  EncoderHardwareInfo info_;
};
}
std::unique_ptr<EncoderImplementation> createNvencEncoder(int width, int height, int fps, int bitrate, const Adapter& adapter, ID3D11Device* sharedDevice) {
  return std::make_unique<NvencEncoder>(width, height, fps, bitrate, adapter, sharedDevice);
}
}
