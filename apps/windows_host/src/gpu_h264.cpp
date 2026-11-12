#include "gpu_h264.hpp"
#include "support.hpp"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <mutex>

namespace host::gpu {
EncoderBackend backendForVendor(uint32_t vendorId) {
  const auto vendor = vendorFromId(vendorId);
  if (vendor == Vendor::nvidia) return EncoderBackend::nvenc;
  if (vendor == Vendor::amd) return EncoderBackend::amf;
  if (vendor == Vendor::intel) return EncoderBackend::qsvMft;
  throw Failure(encoderGpuVendorUnsupported, "Selected GPU vendor has no H264 backend");
}
std::vector<Adapter> enumerateAdapters() {
  Com<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) throw Failure(encoderGpuAdapterUnavailable, "GPU adapter inventory unavailable");
  // OS preference is not an H264 benchmark. Older factories keep their actual
  // EnumAdapters1 order; no new exported API/driver or invented model ranking.
  Com<IDXGIFactory6> performance;
  (void)factory.As(&performance);
  std::vector<Adapter> result;
  for (UINT ordinal = 0; ordinal < 256; ++ordinal) {
    Com<IDXGIAdapter1> adapter;
    const HRESULT status = performance ? performance->EnumAdapterByGpuPreference(ordinal, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) : factory->EnumAdapters1(ordinal, &adapter);
    if (status == DXGI_ERROR_NOT_FOUND) return result;
    if (FAILED(status)) throw Failure(encoderGpuAdapterUnavailable, "GPU adapter inventory failed");
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapter->GetDesc1(&description))) continue;
    if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    Adapter value;
    value.luid = (uint64_t(uint32_t(description.AdapterLuid.HighPart)) << 32) | description.AdapterLuid.LowPart;
    value.vendorId = description.VendorId; value.deviceId = description.DeviceId;
    const size_t length = wcsnlen_s(description.Description, std::size(description.Description));
    if (length == std::size(description.Description)) continue;
    value.description = utf8(std::wstring(description.Description, length));
    if (value.description.size() > 512) value.description.clear();
    result.push_back(std::move(value));
  }
  throw Failure(encoderGpuAdapterUnavailable, "GPU adapter inventory exceeded its bounded enumeration");
}
Selection selectionFromConfig(const std::string& bytes) {
  Selection result;
  try {
    const auto object = JsonParser::parse(bytes);
    const auto* fields = std::get_if<Json::Object>(&object.value);
    if (!fields) return result;
    const auto found = fields->find("videoEncoder");
    const auto backend = found == fields->end() ? std::string("auto") : found->second.string();
    if (backend != "auto" && backend != "gpu" && backend != "nvenc" && backend != "amf" && backend != "qsv" && backend != "qsv-mft") return result;
    result.mode = backend == "nvenc" ? Mode::nvenc : backend == "amf" ? Mode::amf : backend == "qsv" || backend == "qsv-mft" ? Mode::qsv : Mode::automatic;
    const auto& selected = object.get("gpuAdapter");
    if (backend == "auto") { result.valid = std::holds_alternative<std::nullptr_t>(selected.value); return result; }
    if (std::holds_alternative<std::nullptr_t>(selected.value)) return result;
    result.adapterLuid = parseAdapterLuid(selected.string());
    result.valid = result.adapterLuid.has_value();
  } catch (...) { return {}; }
  return result;
}
Selection configuredSelection() {
  try {
    const auto path = machineDirectory() / L"encoder.json";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      const auto error = GetLastError();
      return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? selectionFromConfig("{}") : Selection{};
    }
    if (!trustedMachineFile(path)) return {};
    std::ifstream file(path, std::ios::binary); std::string bytes(8193, '\0');
    file.read(bytes.data(), std::streamsize(bytes.size())); bytes.resize(size_t(file.gcount()));
    if (bytes.size() > 8192) return {};
    return selectionFromConfig(bytes);
  } catch (...) { return {}; }
}
#ifdef MULTITAP_NVENC
std::unique_ptr<EncoderImplementation> createNvencEncoder(int width, int height, int fps, int bitrate, const Adapter& adapter, ID3D11Device* sharedDevice);
#endif
std::unique_ptr<EncoderImplementation> createAmfEncoder(int width, int height, int fps, int bitrate, const Adapter& adapter, ID3D11Device* sharedDevice);
std::unique_ptr<EncoderImplementation> createQsvMftEncoder(int width, int height, int fps, int bitrate, const Adapter& adapter, ID3D11Device* sharedDevice);
std::unique_ptr<EncoderImplementation> createFromAdapters(std::span<const Adapter> adapters, const Selection& selection,
    const std::function<std::unique_ptr<EncoderImplementation>(const Adapter&)>& open) {
  if (selection.mode == Mode::disabled || !selection.valid) throw Failure(encoderGpuAdapterUnavailable, "GPU adapter selection is invalid");
  if (adapters.size() > 256 || !open) throw Failure(encoderGpuAdapterUnavailable, "GPU adapter inventory is invalid");
  bool selected = false, supportedVendor = false;
  uint32_t last = encoderGpuAdapterUnavailable;
  EncoderGpuFailure lastFailure;
  for (const auto& adapter : adapters) {
    if (selection.adapterLuid && *selection.adapterLuid != adapter.luid) continue;
    selected = true;
    const auto vendor = vendorFromId(adapter.vendorId);
    if (vendor == Vendor::unknown || (selection.mode == Mode::nvenc && vendor != Vendor::nvidia) ||
        (selection.mode == Mode::amf && vendor != Vendor::amd) || (selection.mode == Mode::qsv && vendor != Vendor::intel)) continue;
    supportedVendor = true;
    try {
      auto encoder = open(adapter); if (!encoder) throw Failure(encoderGpuInitializationFailed, "GPU backend did not initialize", failureDetail(EncoderGpuFailureSite::candidateOpen));
      const auto actual = encoder->hardwareInfo();
      if (!actual.initialized || actual.hardwareVerified || actual.adapterLuid != adapter.luid || actual.vendorId != adapter.vendorId || actual.deviceId != adapter.deviceId)
        throw Failure(encoderGpuInitializationFailed, "GPU backend did not bind the selected adapter before its first frame", failureDetail(EncoderGpuFailureSite::candidateIdentity));
      return encoder;
    }
    catch (const std::exception& error) { last = failureCode(error); lastFailure = failureInfo(error, adapter, EncoderGpuFailureSite::candidateOpen); }
    catch (...) { last = encoderGpuInitializationFailed; lastFailure = {uint32_t(EncoderGpuFailureSite::candidateOpen), 0, adapter.vendorId, adapter.deviceId}; }
    // Explicit selection never migrates silently to another physical GPU.
    if (selection.adapterLuid) break;
  }
  if (selected && !supportedVendor) last = encoderGpuVendorUnsupported;
  throw Failure(last, "No selected GPU H264 encoder could be initialized", lastFailure);
}
std::vector<Adapter> orderedCandidates(std::span<const Adapter> adapters, const Selection& selection, Preferences preferences) {
  if (selection.mode == Mode::disabled || !selection.valid || adapters.size() > 256) throw Failure(encoderGpuAdapterUnavailable, "GPU selection is unavailable");
  std::vector<Adapter> candidates;
  for (const auto& adapter : adapters) {
    if (!adapter.luid || (selection.adapterLuid && *selection.adapterLuid != adapter.luid)) continue;
    const auto vendor = vendorFromId(adapter.vendorId);
    if (vendor == Vendor::unknown || (selection.mode == Mode::nvenc && vendor != Vendor::nvidia) ||
        (selection.mode == Mode::amf && vendor != Vendor::amd) || (selection.mode == Mode::qsv && vendor != Vendor::intel)) continue;
    if (std::any_of(candidates.begin(), candidates.end(), [&](const auto& item) { return item.luid == adapter.luid; }))
      throw Failure(encoderGpuAdapterUnavailable, "GPU inventory contains duplicate identities");
    candidates.push_back(adapter);
  }
  const auto rank = [&](uint64_t luid) { return preferences.captureAdapter == luid ? 0 : preferences.lastGood == luid ? 1 : 2; };
  // Keep the OS high-performance preference (or legacy DXGI enumeration) for
  // equal locality/history ranks. A numeric LUID is never a performance score.
  std::stable_sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) { return rank(a.luid) < rank(b.luid); });
  // This bounds API attempts, not driver call latency. There is no connection
  // benchmark and no claim that an inventory name proves the fastest GPU.
  if (candidates.size() > 3) candidates.resize(3);
  if (candidates.empty()) {
    const bool present = std::any_of(adapters.begin(), adapters.end(), [&](const auto& a) { return !selection.adapterLuid || a.luid == *selection.adapterLuid; });
    throw Failure(present ? encoderGpuVendorUnsupported : encoderGpuAdapterUnavailable, "No matching H264 GPU candidate");
  }
  return candidates;
}
namespace {
class VerifiedCandidates final : public EncoderImplementation {
 public:
  VerifiedCandidates(std::vector<Adapter> candidates, std::function<std::unique_ptr<EncoderImplementation>(const Adapter&)> open,
      std::function<void(uint64_t, bool)> remember) : candidates_(std::move(candidates)), open_(std::move(open)), remember_(std::move(remember)) { next(); }
  void encode(const uint8_t* source, uint64_t pts, std::vector<Frame>& frames) override {
    encodeWith(pts, frames, [&](std::vector<Frame>& pending) { encoder_->encode(source, pts, pending); });
  }
  // gpu-capture-v1: the same verification; a later candidate that cannot take
  // the texture encodes its bytes (EncoderPicture::read).
  void encodePicture(const EncoderPicture& picture, uint64_t pts, std::vector<Frame>& frames) override {
    encodeWith(pts, frames, [&](std::vector<Frame>& pending) { encoder_->encodePicture(picture, pts, pending); });
  }
  ID3D11Device* textureDevice() const override { return encoder_ ? encoder_->textureDevice() : nullptr; }
  template<class Submit> void encodeWith(uint64_t pts, std::vector<Frame>& frames, Submit&& submit) {
    for (;;) {
      std::vector<Frame> pending;
      bool accepted = false;
      try {
        submit(pending);
        const auto info = encoder_->hardwareInfo();
        if (!identity(info) || !info.hardwareVerified)
          throw Failure(encoderGpuOutputContract, "GPU output identity was not verified", failureDetail(EncoderGpuFailureSite::outputIdentity));
        if (pending.size() != 1 || pending.front().ptsUs != pts)
          throw Failure(encoderGpuOutputContract, "GPU output timing was not verified", failureDetail(EncoderGpuFailureSite::outputTiming));
        const auto& frame = pending.front(); const auto unit = inspectAnnexB(frame.bytes, encoderMaximumFrameBytes);
        if (!unit.valid || frame.key != unit.idr)
          throw Failure(encoderGpuOutputContract, "GPU output did not satisfy the AnnexB contract", failureDetail(EncoderGpuFailureSite::outputAnnexB));
        if ((unit.sps && unit.spsProfile != 66) || ((!verified_ || frame.key) && (!unit.idr || !unit.sps || !unit.pps)))
          throw Failure(encoderGpuOutputContract, "GPU output did not satisfy the streaming H264 headers contract", failureDetail(EncoderGpuFailureSite::outputHeaders));
        if (!verified_) { verified_ = true; if (remember_) remember_(candidates_[at_ - 1].luid, true); }
        accepted = true;
      } catch (const std::exception& error) { last_ = failureCode(error); lastFailure_ = failureInfo(error, candidates_[at_ - 1], EncoderGpuFailureSite::encoderSubmit); bindEncoderGpuObservationPhase(lastFailure_, verified_); }
      catch (...) { last_ = encoderGpuSubmissionFailed; lastFailure_ = boundFailure(EncoderGpuFailureSite::encoderSubmit); }
      // Destination allocation/caller behavior must not be mistaken for a
      // failed device and must never repeat an already successful input.
      if (accepted) { frames.push_back(std::move(pending.front())); return; }
      if (remember_) remember_(candidates_[at_ - 1].luid, false);
      // After any output was published, use the existing CPU fallback rather
      // than oscillating adapters in an established decoder epoch.
      if (verified_) throw Failure(last_, "Running GPU encoder failed", lastFailure_);
      next();
    }
  }
  void finish(std::vector<Frame>& frames) override {
    try {
      std::vector<Frame> pending; encoder_->finish(pending);
      if (!pending.empty()) throw Failure(encoderGpuOutputContract, "GPU encoder delayed output", failureDetail(EncoderGpuFailureSite::outputDelayed));
    } catch (const std::exception& error) { throw Failure(failureCode(error), "GPU encoder finish failed", failureInfo(error, candidates_[at_ - 1], EncoderGpuFailureSite::outputDelayed)); }
    (void)frames;
  }
  bool setBitrate(int value) override {
    try { if (encoder_->setBitrate(value)) { changedBitrate_ = value; return true; } }
    catch (const std::exception& error) { if (remember_) remember_(candidates_[at_ - 1].luid, false); throw Failure(failureCode(error), "GPU candidate reconfiguration failed", failureInfo(error, candidates_[at_ - 1], EncoderGpuFailureSite::candidateReconfigure)); }
    catch (...) { if (remember_) remember_(candidates_[at_ - 1].luid, false); throw Failure(encoderGpuReconfigureFailed, "GPU candidate reconfiguration failed", boundFailure(EncoderGpuFailureSite::candidateReconfigure)); }
    if (remember_) remember_(candidates_[at_ - 1].luid, false); return false;
  }
  int reportedBitrate() const override { return encoder_->reportedBitrate(); }
  EncoderTuning reportedTuning() const override { return encoder_->reportedTuning(); }
  void requestKeyFrame() override { encoder_->requestKeyFrame(); }
  // Before the first verified output another candidate may still take over: only an IDR then.
  bool recover(uint64_t lastGood) override { return verified_ && encoder_->recover(lastGood); }
  bool widenReferences() override { return verified_ && encoder_->widenReferences(); }
  EncoderHardwareInfo hardwareInfo() const override { auto info = encoder_ ? encoder_->hardwareInfo() : EncoderHardwareInfo{}; info.hardwareVerified = verified_ && info.hardwareVerified; return info; }
 private:
  bool identity(const EncoderHardwareInfo& info) const { const auto& a = candidates_[at_ - 1]; return info.initialized && info.adapterLuid == a.luid && info.vendorId == a.vendorId && info.deviceId == a.deviceId; }
  EncoderGpuFailure boundFailure(EncoderGpuFailureSite site) const { const auto& a = candidates_[at_ - 1]; return {uint32_t(site), 0, a.vendorId, a.deviceId}; }
  void next() {
    encoder_.reset();
    while (at_ < candidates_.size()) {
      const auto& adapter = candidates_[at_++];
      try { encoder_ = open_(adapter); if (!encoder_ || !identity(encoder_->hardwareInfo()) || encoder_->hardwareInfo().hardwareVerified)
          throw Failure(encoderGpuInitializationFailed, "GPU candidate initialization identity is invalid", failureDetail(EncoderGpuFailureSite::candidateIdentity));
        if (changedBitrate_ && !encoder_->setBitrate(*changedBitrate_)) throw Failure(encoderGpuReconfigureFailed, "GPU candidate cannot retain the current bitrate", failureDetail(EncoderGpuFailureSite::candidateReconfigure)); return;
      } catch (const std::exception& error) {
        const auto detail = failureInfo(error, adapter, EncoderGpuFailureSite::candidateOpen);
        // A failed NVENC teardown can fence later NVIDIA opens in this process.
        // That admission refusal is a consequence, not a replacement for the
        // concrete failure that caused the previous candidate to be retired.
        if (detail.site != uint32_t(EncoderGpuFailureSite::nvencEndOfStream) || !lastFailure_.site) {
          last_ = failureCode(error); lastFailure_ = detail;
        }
      }
      catch (...) { last_ = encoderGpuInitializationFailed; lastFailure_ = boundFailure(EncoderGpuFailureSite::candidateOpen); }
      if (remember_) remember_(adapter.luid, false); encoder_.reset();
    }
    throw Failure(last_, "Bounded GPU candidates did not produce a valid first output", lastFailure_);
  }
  std::vector<Adapter> candidates_; size_t at_ = 0; bool verified_ = false; uint32_t last_ = encoderGpuAdapterUnavailable;
  EncoderGpuFailure lastFailure_;
  std::function<std::unique_ptr<EncoderImplementation>(const Adapter&)> open_; std::function<void(uint64_t, bool)> remember_;
  std::unique_ptr<EncoderImplementation> encoder_;
  std::optional<int> changedBitrate_;
};
std::mutex lastGoodMutex;
std::optional<uint64_t> lastGoodAdapter;
}
std::unique_ptr<EncoderImplementation> createVerifiedFromAdapters(std::span<const Adapter> adapters, const Selection& selection,
    Preferences preferences, const std::function<std::unique_ptr<EncoderImplementation>(const Adapter&)>& open,
    const std::function<void(uint64_t, bool)>& remember) {
  if (!open) throw Failure(encoderGpuInitializationFailed, "GPU opener is absent");
  return std::make_unique<VerifiedCandidates>(orderedCandidates(adapters, selection, preferences), open, remember);
}
std::unique_ptr<EncoderImplementation> createEncoder(int width, int height, int fps, int bitrate, const Selection& selection, std::optional<uint64_t> captureAdapter, ID3D11Device* captureDevice) {
  const auto adapters = enumerateAdapters();
  Preferences preferences; preferences.captureAdapter = captureAdapter;
  { std::lock_guard lock(lastGoodMutex); preferences.lastGood = lastGoodAdapter; }
  // The capture device is referenced by the opener: a later candidate can
  // open after the capture that supplied it was rebuilt.
  const Com<ID3D11Device> shared(captureAdapter ? captureDevice : nullptr);
  // Capture dimensions are copied: the first real encode can happen after this
  // factory returns, so no references to the factory stack may be retained.
  return createVerifiedFromAdapters(adapters, selection, preferences, [=](const Adapter& adapter) -> std::unique_ptr<EncoderImplementation> {
    if (adapter.vendorId == 0x1002) return createAmfEncoder(width, height, fps, bitrate, adapter, captureAdapter == adapter.luid ? shared.Get() : nullptr);
    if (adapter.vendorId == 0x8086) return createQsvMftEncoder(width, height, fps, bitrate, adapter, captureAdapter == adapter.luid ? shared.Get() : nullptr);
#ifdef MULTITAP_NVENC
    return createNvencEncoder(width, height, fps, bitrate, adapter, captureAdapter == adapter.luid ? shared.Get() : nullptr);
#else
    (void)width; (void)height; (void)fps; (void)bitrate; (void)adapter; (void)shared;
    throw Failure(encoderGpuNotBuilt, "NVENC was not built into this Host");
#endif
  }, [](uint64_t luid, bool valid) { std::lock_guard lock(lastGoodMutex); if (valid) lastGoodAdapter = luid; else if (lastGoodAdapter == luid) lastGoodAdapter.reset(); });
}
uint32_t failureCode(const std::exception& error) {
  if (const auto* failure = dynamic_cast<const Failure*>(&error)) return failure->code();
  if (dynamic_cast<const std::bad_alloc*>(&error)) return encoderGpuAllocationFailed;
  return encoderGpuInitializationFailed;
}
EncoderGpuFailure failureInfo(const std::exception& error) {
  if (const auto* failure = dynamic_cast<const Failure*>(&error)) return failure->gpuFailure();
  return {};
}
EncoderGpuFailure failureInfo(const std::exception& error, const Adapter& adapter, EncoderGpuFailureSite otherwise) {
  auto detail = failureInfo(error);
  if (!detail.site) detail.site = uint32_t(otherwise);
  detail.vendorId = adapter.vendorId; detail.deviceId = adapter.deviceId;
  return detail;
}
}
