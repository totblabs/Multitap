#pragma once
#include "remote_decoder.h"
#include "../../../../native/windows_video/track_media.hpp"
#include "../../../../native/windows_video/lossless_tiles.hpp"
#include <array>
#include <memory>

namespace multitap {
// Candidate worker-thread component. MF/COM startup belongs to that worker.
// It neither creates windows nor proves display ownership/presentation.
class RemoteTrackDecoder {
 public:
  using FrameCallback = std::function<void(uint8_t,uint32_t,uint32_t,DecodedFrame&&)>;
  using AudioCallback = std::function<HRESULT(const uint8_t*,size_t,uint64_t)>;
  // The frame an output shows now (for republishing lossless tiles), or null.
  using LatestFrame = std::function<std::shared_ptr<const DecodedFrame>(uint8_t)>;
  // One output's decoding and presentation: `decoding.decode_device` names a
  // hardware decoder device, `decoding.converter` (its own, never shared with
  // the other output) makes the pictures leave as shared GPU textures
  // (DecodedFrame::gpu) and `overlay` (same device) holds the lossless tiles on
  // the GPU. Neither is owned: the caller keeps them alive until Close.
  struct Pipeline {
    RemoteDecoder::Options decoding;
    gpu::Overlay* overlay = nullptr;
  };
  RemoteTrackDecoder();
  ~RemoteTrackDecoder();
  RemoteTrackDecoder(const RemoteTrackDecoder&) = delete;
  RemoteTrackDecoder& operator=(const RemoteTrackDecoder&) = delete;
  // `decoding` may name a hardware decoder device (CPU pixel output: the track
  // windows draw RGBA); a track whose decoder fails continues on the software one.
  HRESULT Configure(tracks::Configuration configuration,const RemoteDecoder::Options& decoding = {});
  // Each output with its own decoder device / GPU converter (see Pipeline).
  HRESULT ConfigurePipelines(tracks::Configuration configuration,const std::array<Pipeline,2>& pipelines);
  // A tiles-v1 video packet never reaches H.264: its tiles are composited on
  // every later decoded frame and, given `latest`, republished at once.
  HRESULT Push(const uint8_t* bytes,size_t size,const FrameCallback& frame,
               const AudioCallback& audio = {},tracks::Decision* decision = nullptr,const LatestFrame& latest = {});
  HRESULT Drain(const FrameCallback& frame);
  bool ready() const;
  void Close();
  lossless::Compositor::Stats losslessStats(uint8_t track) const;
  // Every configured decoder is a hardware one / the lowest tier among them
  // (0 as soon as one runs on the CPU), the adapter vendor, and fallbacks so far.
  bool hardware() const;
  int tier() const;
  uint32_t vendor() const;
  uint64_t fallbacks() const;
  // The same for one output (false / 0 for an output that is not configured), and
  // whether its pictures are GPU textures.
  bool hardware(uint8_t track) const;
  bool prestarted(uint8_t track) const;
  int tier(uint8_t track) const;
  uint32_t vendor(uint8_t track) const;
  uint64_t fallbacks(uint8_t track) const;
  uint64_t gpuDropped(uint8_t track) const;
  bool gpu(uint8_t track) const;
 private:
  struct State;
  std::shared_ptr<State> state_;
};
}
