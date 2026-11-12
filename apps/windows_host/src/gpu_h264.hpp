#pragma once
#include "media.hpp"
#include "gpu_h264_policy.hpp"

namespace host::gpu {
enum class Mode : uint8_t { disabled, automatic, nvenc, amf, qsv };
struct Selection {
  Mode mode = Mode::disabled;
  std::optional<uint64_t> adapterLuid;
  bool valid = true;
};
struct Adapter {
  uint64_t luid = 0;
  uint32_t vendorId = 0, deviceId = 0;
  std::string description;
};
// Preferences are identities observed locally, never a GPU-name performance
// ranking. Inventory and every first output are still checked on each open.
struct Preferences { std::optional<uint64_t> captureAdapter, lastGood; };
EncoderBackend backendForVendor(uint32_t vendorId);
// Enumeration is inventory only. Neither a driver name nor a GPU vendor is
// evidence that encoder initialisation or encoding succeeded.
std::vector<Adapter> enumerateAdapters();
Selection selectionFromConfig(const std::string& bytes);
Selection configuredSelection();
// Deterministic candidate/failure policy, separated from OS enumeration for
// injected memory tests. The supplied opener may open only the given adapter.
std::unique_ptr<EncoderImplementation> createFromAdapters(std::span<const Adapter> adapters, const Selection& selection,
  const std::function<std::unique_ptr<EncoderImplementation>(const Adapter&)>& open);
std::vector<Adapter> orderedCandidates(std::span<const Adapter> adapters, const Selection& selection, Preferences preferences = {});
// Initialization alone is not selection success. The bounded candidates are
// tried using the first real input, before any candidate bytes are published.
std::unique_ptr<EncoderImplementation> createVerifiedFromAdapters(std::span<const Adapter> adapters, const Selection& selection,
  Preferences preferences, const std::function<std::unique_ptr<EncoderImplementation>(const Adapter&)>& open,
  const std::function<void(uint64_t, bool)>& remember = {});
// `captureDevice` (gpu-capture-v1): the capture's device; the candidate on its
// adapter (captureAdapter) opens on it so it can take the capture's textures.
std::unique_ptr<EncoderImplementation> createEncoder(int width, int height, int fps, int bitrate, const Selection& selection,
  std::optional<uint64_t> captureAdapter = {}, ID3D11Device* captureDevice = nullptr);
uint32_t failureCode(const std::exception& error);
inline EncoderGpuFailure failureDetail(uint32_t site, uint32_t nativeStatus = 0) {
  return {site, nativeStatus, 0, 0};
}
inline EncoderGpuFailure failureDetail(EncoderGpuFailureSite site, uint32_t nativeStatus = 0) {
  return failureDetail(uint32_t(site), nativeStatus);
}
EncoderGpuFailure failureInfo(const std::exception& error);
// Candidate ownership is established here, never taken from provider-supplied
// metadata or from the next candidate after the failure.
EncoderGpuFailure failureInfo(const std::exception& error, const Adapter& adapter, EncoderGpuFailureSite otherwise);
class Failure : public std::runtime_error {
 public:
  explicit Failure(uint32_t code, const char* operation, EncoderGpuFailure detail = {})
    : std::runtime_error(operation), code_(code), detail_(detail) {}
  uint32_t code() const { return code_; }
  const EncoderGpuFailure& gpuFailure() const { return detail_; }
 private:
  uint32_t code_;
  EncoderGpuFailure detail_;
};
}
