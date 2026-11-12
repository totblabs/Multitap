#ifndef MULTITAP_REMOTE_ENGINE_H_
#define MULTITAP_REMOTE_ENGINE_H_
#include <flutter/encodable_value.h>
#include <flutter/texture_registrar.h>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include "media_push.h"
#include "remote_audio.h"
#include "remote_decoder.h"
#include "gpu_video.h"
#include "../../../../native/windows_video/lossless_tiles.hpp"
namespace multitap {
class RemoteEngine {
 public:
  // How this engine decodes and presents. The defaults are the CPU path (the
  // Microsoft software decoder, RGBA pixel buffer); the Windows client asks for
  // hardware decoding and GPU presentation explicitly.
  struct Options {
    uint32_t fps = 60;
    uint32_t sourceWidth=0,sourceHeight=0;
    // The user's "하드웨어 디코딩" setting: DXVA / D3D11 decoding where an
    // adapter has an H.264 decoder, with the software decoder as the fallback.
    bool hardware_decode = false;
    // Convert on the GPU and hand Flutter a DXGI shared texture instead of a
    // pixel buffer. Needs a hardware render adapter (see gpu::Select).
    bool gpu_present = false;
    // The adapter Flutter renders this window on (AddRef'd copy; may be null).
    Microsoft::WRL::ComPtr<IDXGIAdapter> render_adapter;
    // Tests only: GPU presentation on WARP / the Basic Render Driver.
    bool allow_software_render = false, warp_render = false;
    // decoder-prestart-v1: see RemoteDecoder::Options::prestart.
    std::shared_ptr<DecoderPrestart> prestart;
  };
  explicit RemoteEngine(flutter::TextureRegistrar* registrar, Options options = {}) : registrar_(registrar), options_(std::move(options)) {}
  ~RemoteEngine() { Stop(); }
  RemoteEngine(const RemoteEngine&) = delete;
  RemoteEngine& operator=(const RemoteEngine&) = delete;
  // `audio_opus`: audio packets are audio-opus-v1 (RemoteAudio::Start).
  int64_t Start(uint32_t width, uint32_t height, bool audio_enabled = true, bool audio_opus = false);
  void Stop();
  // A resize's held picture: decoding and audio stop, the texture stays
  // registered and keeps showing the newest frame until Stop.
  void Quiesce();
  // Hidden windows (backlog #62): the window shows nothing of this stream
  // (minimized, cloaked, covered). Decoding and audio go on; Flutter is asked
  // for nothing and a CPU picture is converted at most every kHiddenPicture
  // (the thumbnail's). Shown again, the newest decoded picture is converted
  // and handed to Flutter at once. UI thread; kept across Start.
  void SetHidden(bool hidden);
  static constexpr std::chrono::milliseconds kHiddenPicture{1000};
  int64_t TextureId() const { return texture_id_; }
  // `watch` (input-latency-v1): report when the first decoded frame at or after
  // this video packet's PTS has been handed to the texture (Stats inputFrames).
  // A full decode queue is `full` (offer the packet again), never a failure.
  MediaPushResult Offer(const std::vector<uint8_t>& packet, bool watch = false);
  bool Push(const std::vector<uint8_t>& packet, bool watch = false) { return Offer(packet, watch) == MediaPushResult::accepted; }
  // Bounded decode queue: at most kQueuePackets packets / kQueueBytes bytes.
  static constexpr size_t kQueuePackets = 8, kQueueBytes = 12 * 1024 * 1024;
  flutter::EncodableMap Stats();
  // Bounded results: [pts, queued -> published microseconds], newest last.
  static constexpr size_t kWatchedFrames = 32, kWatchingPackets = 8;
  // UI thread: a shared reference to the newest published frame and its
  // publication count. The CPU path copies no pixels and does not touch the
  // decoder; a GPU frame is read back once per publication (thumbnails).
  std::shared_ptr<const DecodedFrame> Latest(uint64_t* serial = nullptr) const;
  // Presentation (texture-pacing-v1). Flutter takes at most one frame per
  // display refresh. A frame published while an earlier one still waits for
  // Flutter is queued (not replaced), so two frames that arrive within one
  // refresh are both shown, on consecutive refreshes. Bounds keep latency:
  // at most kPresentQueue frames wait; a take that finds frames waiting again
  // (a standing backlog) or a frame older than kStaleFrame shows the newest.
  // The same rules apply to both texture kinds.
  static constexpr size_t kPresentQueue = 2;
  static constexpr std::chrono::milliseconds kStaleFrame{100};
  // 0 = CPU pixel buffer (glTexImage2D upload by Flutter's raster thread),
  // 1 = GPU surface (DXGI shared handle of a BGRA texture, no CPU copy).
  static constexpr int64_t kTexturePathPixelBuffer = 0, kTexturePathGpuSurface = 1;
  // Stall forensics: a gap of kStall or more between two published frames (kind 1), or
  // between two frames Flutter took while the later one already waited (kind 2), leaves
  // a record of fixed numbers (Stats "stalls", newest last, at most kStallRecords each).
  static constexpr std::chrono::milliseconds kStall{250};
  static constexpr size_t kStallRecords = 8;
  struct Stall {
    uint64_t serial = 0; int kind = 1;
    std::chrono::steady_clock::time_point at;  // when it ended
    // pts_gap_ms (kind 1): how far apart the Host stamped the two frames around the gap
    // (an idle screen's 1 s refresh has about the whole gap; a late frame has little of it).
    double gap_ms = 0, decode_max_ms = 0, wait_ms = 0, pts_gap_ms = 0;
    uint32_t queue = 0, queue_max = 0, offers = 0, full = 0, decodes = 0, pending = 0, skipped = 0;
    bool key = false;
  };
 private:
  struct Frames {
    std::mutex mutex;
    // Newest published frame: thumbnails, the lossless republish base, and
    // what Flutter redraws when it asks again with nothing new waiting.
    std::shared_ptr<DecodedFrame> latest;
    struct Pending {
      std::shared_ptr<DecodedFrame> frame;
      std::chrono::steady_clock::time_point published;
      // A lossless republish: it replaces the waiting frame it copied, and
      // the next published frame (which has all its tiles) replaces it.
      bool replaceable = false;
    };
    std::deque<Pending> pending;
    // Consecutive takes that left a frame waiting.
    uint32_t backlog = 0;
    uint64_t published = 0, texture_frames = 0, skipped = 0, queued = 0, uploads = 0;
    // Published -> taken by Flutter; Flutter's callback -> buffer release
    // (the raster thread's texture upload). Lifetime totals since Start.
    double wait_total_ms = 0, upload_total_ms = 0;
    // CPU copy of `latest` when it is a GPU frame (Latest(), thumbnails).
    std::shared_ptr<DecodedFrame> cpu_copy;
    uint64_t cpu_copy_serial = 0;
    // Kind 2 stalls: the last take of a new frame, and the records.
    std::chrono::steady_clock::time_point last_take{};
    std::deque<Stall> present_stalls;
    uint64_t present_serial = 0;
  };
  // The worker's wait state, shared with the texture callback: Flutter may
  // still call it after Stop until its unregister completes, so the callback
  // only touches state it co-owns. A take that leaves a frame waiting asks
  // the worker to request one more Flutter frame (`remark`).
  struct Signal {
    std::mutex mutex;
    std::condition_variable wake;
    bool remark = false;
    // The window was shown again: the worker hands Flutter the newest picture.
    bool reveal = false;
  };
  struct QueuedPacket {
    std::vector<uint8_t> bytes;
    std::chrono::steady_clock::time_point enqueued;
    bool watch = false;
  };
  // RGBA vectors of frames whose last reference (engine or Flutter's held
  // pixel buffer) was dropped. The decoder reuses them for later frames; a
  // still-referenced frame is never written. Bounded to kSpareFrames, so at
  // most one more frame buffer is resident than with per-frame allocation.
  struct FrameBuffers {
    static constexpr size_t kSpareFrames = 1;
    std::mutex mutex;
    std::vector<std::vector<uint8_t>> spare;
  };
  // The GPU side of one stream's worker: converter, lossless overlay and the
  // decoder that feeds them (worker thread only).
  struct GpuWorker {
    gpu::Converter* converter = nullptr;
    gpu::Overlay* overlay = nullptr;
    RemoteDecoder* decoder = nullptr;
  };
  void Run(uint32_t width, uint32_t height);
  // Flutter's take, shared by both texture kinds: the next frame to show (see
  // kPresentQueue) or the newest again; null before the first frame.
  static std::shared_ptr<DecodedFrame> TakeForFlutter(const std::shared_ptr<Frames>& frames, const std::shared_ptr<Signal>& signal, std::chrono::steady_clock::time_point& taken);
  // Worker thread only: frame publication, and a tiles-v1 packet (decoded into
  // the compositor, republished over a copy of the newest frame). A tile
  // packet directly followed by its tick's video packet is not republished:
  // that decoded frame composes every present tile.
  void Publish(DecodedFrame&& frame, bool replaceable = false);
  // Hidden windows: a decoded picture kept unconverted, and the newest one shown again.
  void NoteDeferred(uint64_t pts);
  void ShowLatest();
  void AcceptLossless(lossless::Compositor& compositor, const std::vector<uint8_t>& packet, bool republish, GpuWorker& gpu);
  bool RepublishLossless(lossless::Compositor& compositor, GpuWorker& gpu);
  // GPU path: present tiles over a freshly converted frame (due drops first).
  HRESULT ComposeGpu(lossless::Compositor& compositor, GpuWorker& gpu, DecodedFrame& frame);
  DecodedFrame TakeFrame(uint32_t width, uint32_t height, uint64_t pts);
  void ReturnBuffer(std::vector<uint8_t>&& rgba);
  uint64_t lossless_packets_ = 0, lossless_republishes_ = 0;
  uint32_t lossless_present_ = 0;
  double lossless_compose_ms_ = 0, lossless_republish_ms_ = 0;
  bool lossless_poisoned_ = false, lossless_scaled_ = false;
  flutter::TextureRegistrar* registrar_;
  Options options_;
  // Chosen at Start (UI thread), before the worker runs.
  gpu::Selection selection_;
  std::unique_ptr<gpu::Converter> converter_;
  std::unique_ptr<gpu::Overlay> overlay_;
  bool present_gpu_ = false;
  std::shared_ptr<Frames> frames_;
  std::shared_ptr<FrameBuffers> buffers_;
  std::unique_ptr<flutter::TextureVariant> texture_;
  int64_t texture_id_ = -1;
  std::atomic<bool> running_{false}, hidden_{false};
  std::atomic<HRESULT> error_{S_OK};
  // Guarded by mutex_: decoded pictures kept unconverted while hidden.
  uint64_t deferred_ = 0;
  std::thread worker_;
  const std::shared_ptr<Signal> signal_ = std::make_shared<Signal>();
  std::mutex& mutex_ = signal_->mutex;
  std::condition_variable& wake_ = signal_->wake;
  std::deque<QueuedPacket> queue_;
  size_t queued_bytes_ = 0;
  uint64_t decode_calls_ = 0;
  uint64_t texture_notify_calls_ = 0, texture_remarks_ = 0;
  uint64_t lossless_deferred_ = 0;
  uint64_t rgba_buffer_reuses_ = 0;
  // Decoder MFT + colour conversion excludes the synchronous publish callback.
  // Notification includes shared-frame publication and Flutter's availability
  // call. These lifetime counters reset on Start; none measures presentation.
  double decode_total_ms_ = 0, decode_max_ms_ = 0;
  double texture_notify_total_ms_ = 0, texture_notify_max_ms_ = 0;
  // Hardware decode / GPU conversion (guarded by mutex_, written by the worker).
  bool hardware_decode_ = false, decoder_prestarted_ = false;
  int decode_tier_ = 0;
  uint32_t decode_vendor_ = 0;
  uint64_t decoder_fallbacks_ = 0, gpu_dropped_ = 0;
  int64_t decoder_fallback_reason_ = 0;
  double gpu_convert_total_ms_ = 0, gpu_convert_max_ms_ = 0;
  // Guarded by mutex_: watched packet PTS and its queued -> published time.
  std::deque<std::pair<uint64_t, int64_t>> watched_;
  // Guarded by mutex_: what happened since the last publication (kind 1 stalls), and the records.
  struct SincePublish { std::chrono::steady_clock::time_point at{}; uint32_t queue_max = 0, offers = 0, full = 0, decodes = 0; double decode_max_ms = 0; uint64_t skipped = 0, pts = 0; } since_;
  std::deque<Stall> stalls_;
  uint64_t stall_serial_ = 0;
  bool publishing_key_ = false;  // worker only: the packet being decoded is a key frame
  bool audio_enabled_ = true;
  RemoteAudio audio_;
};
}
#endif
