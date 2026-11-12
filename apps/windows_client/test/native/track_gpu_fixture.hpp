#pragma once
// Test support: the client's tracks-v1 GPU presentation path on the WARP
// software rasterizer. WARP stands in for a GPU: it proves the plumbing (decoder
// -> per-output converter -> shared BGRA texture, lossless overlay on the GPU)
// and the pixel maths, not a real GPU's speed. Production does not present on
// WARP (gpu::Select); the tests opt in explicitly.
#include "remote_track_decoder.h"
#include "gpu_video.h"
#include <array>
#include <memory>
#include <stdexcept>
#include <vector>

namespace trackgpu {
// One WARP device shared by both outputs, an own converter (and lossless
// overlay) per output. Declare it before the decoder that uses it: the decoder
// does not own the converters.
struct Pipelines {
  std::shared_ptr<multitap::gpu::Device> device;
  std::array<std::unique_ptr<multitap::gpu::Converter>, 2> converters;
  std::array<std::unique_ptr<multitap::gpu::Overlay>, 2> overlays;
  std::array<multitap::RemoteTrackDecoder::Pipeline, 2> pipelines;
  Pipelines() {
    device = multitap::gpu::Device::Create(nullptr, true);
    if (!device) throw std::runtime_error("The WARP device could not be created");
    for (unsigned id = 0; id < 2; ++id) {
      converters[id] = std::make_unique<multitap::gpu::Converter>(device);
      if (!multitap::gpu::ConversionSelfTest(*converters[id])) throw std::runtime_error("GPU conversion self-test failed");
      overlays[id] = std::make_unique<multitap::gpu::Overlay>(device);
      pipelines[id].decoding.converter = converters[id].get();
      pipelines[id].overlay = overlays[id].get();
    }
  }
};

// The picture as CPU RGBA, whichever path produced it.
inline std::vector<uint8_t> rgba(const multitap::DecodedFrame& frame) {
  if (!frame.gpu) return frame.rgba;
  std::vector<uint8_t> pixels;
  if (!frame.rgba.empty() || !frame.gpu->ReadRgba(pixels)) throw std::runtime_error("A GPU picture could not be read back");
  return pixels;
}
}  // namespace trackgpu
