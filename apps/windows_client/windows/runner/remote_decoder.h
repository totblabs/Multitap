#ifndef MULTITAP_REMOTE_DECODER_H_
#define MULTITAP_REMOTE_DECODER_H_
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include "gpu_video.h"

namespace multitap {
// One decoded picture. CPU path: `rgba` (width x height x 4, R G B A). GPU
// path: `gpu` is a shared BGRA texture and `rgba` is empty (the lossless
// compositor and thumbnails read it back on demand).
struct DecodedFrame { uint32_t width = 0, height = 0; uint64_t pts_us = 0; std::vector<uint8_t> rgba; std::shared_ptr<gpu::Frame> gpu; };
// decoder-prestart-v1: the CPU decoder (Microsoft software H.264 MFT) started
// on its own thread before its stream's size is known, while a session still
// waits for its stream info. Its MFT_MESSAGE_NOTIFY_BEGIN_STREAMING waits for
// the decoder's worker threads, whatever the size: 0.1-1.8 s on a loaded VM
// (connect-time study), 2-3 ms idle and 0.5-3.5 s with busy CPUs on a
// GPU-less VM (decoder_prestart_smoke --benchmark). See
// RemoteDecoder::Options::prestart for how a stream uses it.
class DecoderPrestart {
 public:
  // Returns at once; null when the size/fps is invalid or no thread started.
  static std::shared_ptr<DecoderPrestart> Begin(uint32_t width, uint32_t height, uint32_t fps);
  ~DecoderPrestart() { Cancel(); }
  DecoderPrestart(const DecoderPrestart&) = delete;
  DecoderPrestart& operator=(const DecoderPrestart&) = delete;
  // Once: the started decoder, waiting while it still starts; null when its
  // start failed, it expired or it was taken or cancelled. The calling thread
  // must have COM (multithreaded) and Media Foundation started.
  Microsoft::WRL::ComPtr<IMFTransform> Take();
  // Not wanted: its own thread releases it (never blocks).
  void Cancel();
  // A session that never got its stream info: released after this long.
  static constexpr std::chrono::seconds kUnused{120};
  struct State;
 private:
  explicit DecoderPrestart(std::shared_ptr<State> state) : state_(std::move(state)) {}
  std::shared_ptr<State> state_;
};
class RemoteDecoder {
 public:
  using FrameCallback = std::function<void(DecodedFrame&&)>;
  // What a decoder may use besides the CPU. Both optional and independent:
  //  * decode_device: hardware (DXVA / D3D11) decoding on that device. Failure to
  //    set it up, a stalled or failing hardware decoder: the Microsoft software
  //    H.264 MFT takes over (see Decode).
  //  * converter: frames leave as shared GPU textures (DecodedFrame::gpu)
  //    converted by this converter instead of CPU RGBA.
  struct Options {
    uint32_t fps = 60;
    std::shared_ptr<gpu::Device> decode_device;
    gpu::Converter* converter = nullptr;
    // Reported tier of decode_device (gpu::kTierRender: Flutter's adapter, kTierOther: another one).
    int tier = gpu::kTierRender;
    // Tests only: a software decoder that pretends to be the hardware one and fails
    // its Nth Decode call (0-based; -1 = off), to exercise the fallback.
    int test_fail_hardware_after = -1;
    // Consumer hint, checked only while polling late software output. True
    // means another complete H.264 access unit already waits (or stop was
    // requested). No encoded packet is skipped; absent hint preserves the
    // recorded-fixture/tracks decoder's existing output-wait behaviour.
    std::function<bool()> pending_input;
    // decoder-prestart-v1: used instead of a new MFT by the first CPU decoder
    // this Initialize creates (no hardware decoder, or its start failed), once
    // this stream's size passed the same admission a new decoder gets (a fresh
    // MFT given these types, not started). Released unused when the hardware
    // decoder starts. Its input type keeps the provisional size: a new one
    // would stop the threads its start waited for (the cost came back). The
    // stream's first SPS replaces its output type (MF_E_TRANSFORM_STREAM_CHANGE,
    // raised at every stream's first SPS), as it does for a new decoder.
    std::shared_ptr<DecoderPrestart> prestart;
    // Hidden windows (backlog #62): asked with each CPU picture's PTS before its
    // RGBA conversion (GPU frames are not asked). True keeps the picture as NV12
    // instead, without a callback, until ConvertDeferred or the next picture.
    std::function<bool(uint64_t pts_us)> defer;
  };
  HRESULT Initialize(uint32_t width, uint32_t height) { return Initialize(width, height, Options{}); }
  HRESULT Initialize(uint32_t width, uint32_t height, const Options& options);
  // stream-size-v1: whether the decoder this engine uses (the Microsoft
  // software H.264 MFT, DXVA off) can be created and accepts a width x height
  // stream. Needs COM and Media Foundation on the calling thread.
  static bool Accepts(uint32_t width, uint32_t height);
  // Exact type negotiation with the same Microsoft H.264 MFT and optional
  // selected DXVA manager used by Initialize. No packet/frame allocation,
  // ProcessInput/Output, texture creation or throughput claim.
  static HRESULT ValidateShape(uint32_t width,uint32_t height,uint32_t fps,const Options& options,const char** observedStage=nullptr);
  // A hardware decoder that fails (or never produces a picture) is replaced by
  // the software decoder inside this call: the packets since the last IDR are
  // fed to it, so the picture continues without waiting for a key frame.
  HRESULT Decode(const uint8_t* bytes, size_t size, uint64_t pts_us, const FrameCallback& callback);
  HRESULT Drain(const FrameCallback& callback);
  // Converts the picture Options::defer kept (S_OK, through `callback`); S_FALSE
  // when there is none or a later picture superseded it.
  HRESULT ConvertDeferred(const FrameCallback& callback);
  // Pictures kept as NV12 since Initialize (converted later or never).
  uint64_t deferred() const { return deferred_count_; }
  // Optional: hands back the RGBA vector of a frame nobody references any more
  // so the next frame reuses it instead of allocating/zero-filling a new one.
  // Takes ownership only when it returns true (no spare held, exact frame size).
  // Every byte is overwritten by conversion, so output is unchanged.
  bool Recycle(std::vector<uint8_t>&& rgba);
  // GPU frames are produced at this size (the lossless source, larger than the
  // video); 0 = the video size.
  void SetOutputSize(uint32_t width, uint32_t height) { out_width_ = width; out_height_ = height; }

  bool hardware() const { return hardware_; }
  // The CPU decoder in use is a DecoderPrestart's.
  bool prestarted() const { return prestarted_; }
  // gpu::kTierCpu / kTierRender / kTierOther; vendor ID of the hardware decoder's adapter.
  int tier() const { return hardware_ ? tier_ : 0; }
  uint32_t vendor() const { return hardware_ ? vendor_ : 0; }
  // Lifetime counts: hardware -> software replacements, pictures dropped because
  // every pooled GPU texture was still in use, replayed packets after a fallback.
  uint64_t fallbacks() const { return fallbacks_; }
  uint64_t gpu_dropped() const { return gpu_dropped_; }
  // Milliseconds spent converting to GPU textures since the last call.
  double TakeConvertMs() { const double value = convert_ms_; convert_ms_ = 0; return value; }
  // HRESULT of the failure that ended hardware decoding (S_OK: none yet).
  HRESULT fallback_reason() const { return fallback_reason_; }

 private:
  friend class DecoderPrestart;
  HRESULT Create(bool hardware,bool beginStreaming=true);
  HRESULT SelectOutput();
  HRESULT DecodeOnce(const uint8_t* bytes, size_t size, uint64_t pts_us, const FrameCallback& callback);
  HRESULT FallBack(HRESULT reason, const uint8_t* bytes, size_t size, uint64_t pts_us, const FrameCallback& callback);
  HRESULT ReadOutput(const FrameCallback& callback);
  HRESULT Convert(IMFSample* sample, const FrameCallback& callback);
  HRESULT Defer(IMFMediaBuffer* buffer, LONGLONG pts);
  HRESULT ConvertGpu(IMFSample* sample, LONGLONG pts, const FrameCallback& callback);
  Microsoft::WRL::ComPtr<IMFTransform> decoder_;
  Options options_;
  // Software MFT output memory is consumed synchronously by Convert, so one
  // buffer is reused per stream; each ProcessOutput still gets a fresh sample.
  Microsoft::WRL::ComPtr<IMFMediaBuffer> output_buffer_;
  DWORD output_capacity_ = 0;
  std::vector<uint8_t> nv12_, spare_rgba_;
  uint32_t width_ = 0, height_ = 0, coded_width_ = 0, coded_height_ = 0, out_width_ = 0, out_height_ = 0;
  // Old hosts emitted BT.601 limited pixels without colour metadata.
  bool matrix_709_ = false, full_range_ = false;
  // Hardware decoding state. The replay list holds the packets since the last
  // IDR (bounded) so a fallback can rebuild the software decoder's references.
  struct Packet { std::vector<uint8_t> bytes; uint64_t pts_us = 0; };
  bool hardware_ = false, produced_ = false, replay_valid_ = true, discard_ = false;
  // provisional_: the output type is a prestart's, not yet this stream's.
  bool prestarted_ = false, provisional_ = false;
  int tier_ = 0;
  uint32_t vendor_ = 0, inputs_without_output_ = 0;
  int test_calls_ = 0;
  std::vector<Packet> replay_;
  size_t replay_bytes_ = 0;
  uint64_t fallbacks_ = 0, gpu_dropped_ = 0, frames_out_ = 0, samples_out_ = 0;  // samples_out_: pictures the MFT released (also discarded replays)
  // Actual last sample timestamp, independent of the newest ProcessInput.
  std::optional<uint64_t> last_sample_pts_;
  uint32_t late_misses_ = 0;  // consecutive packets whose picture never came
  double convert_ms_ = 0;
  // The deferred picture: in nv12_, with the geometry and colour it was decoded in.
  struct Deferred { bool held = false; uint32_t width = 0, height = 0, coded_width = 0, coded_height = 0; bool matrix_709 = false, full_range = false; uint64_t pts_us = 0; } deferred_;
  uint64_t deferred_count_ = 0;
  HRESULT fallback_reason_ = S_OK;
  // Fixed local metadata only; never an arbitrary driver/COM error string.
  const char* validation_stage_ = "not_queried";
};
}
#endif
