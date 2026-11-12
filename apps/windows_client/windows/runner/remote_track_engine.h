#pragma once
#include <flutter/encodable_value.h>
#include <flutter/texture_registrar.h>
#include "remote_track_decoder.h"
#include "remote_track_frame.h"
#include "media_push.h"
#include "remote_audio.h"
#include "gpu_video.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace multitap {
// UI-thread Start/Stop/Push/Stats, one decoding worker and one bounded queue
// per output (a lane), independent Flutter pixel buffers. Texture acquisition
// is not physical presentation. Each output used to wait behind the other on a
// single worker: at 8-10 ms of hardware decode + conversion per picture, two
// 60 fps outputs need about the whole second of that one thread, and a packet
// of one output queued behind the other's (one queue of 8 for both and audio).
class RemoteTrackEngine {
 public:
  using FrameNotification=std::function<void(uint8_t,uint32_t,uint32_t)>;
  using FrameSnapshot=RemoteTrackFrame;
  // Notification runs on the decoder thread and must only enqueue UI work.
  // `hardware_decode` is the user's "hardware decoding" setting: on, every output
  // decodes in hardware where an adapter has an H.264 decoder AND its pictures
  // are converted on the GPU into shared textures (zero copy to the Flutter
  // texture or to a native track window's swap chain); off, everything stays on
  // the CPU (Microsoft software decoder, RGBA pictures). Each output chooses its
  // own adapter (gpu::SelectForTrack): `output_adapters[id]` - the adapter that
  // drives the monitor of the native window that will show output `id`, when the
  // caller knows it - if that adapter decodes H.264, else `render_adapter`
  // (Flutter's), else another hardware adapter, else the CPU. An output whose
  // device is not Flutter's cannot be a Flutter GPU texture; its Flutter texture
  // then reads the picture back (only if something shows it there).
  // Presentation on the GPU needs a hardware render adapter; the test overrides
  // allow WARP / the Basic Render Driver.
  struct Options {
    uint32_t fps=60;
    bool hardware_decode=false;
    Microsoft::WRL::ComPtr<IDXGIAdapter> render_adapter;
    std::array<Microsoft::WRL::ComPtr<IDXGIAdapter>,2> output_adapters;
    bool allow_software_render=false,warp_render=false;
    // decoder-prestart-v1: one per output at most, each lane takes one (see
    // RemoteDecoder::Options::prestart).
    std::vector<std::shared_ptr<DecoderPrestart>> prestarts;
  };
  explicit RemoteTrackEngine(flutter::TextureRegistrar* registrar,FrameNotification notification={},Options options={}):registrar_(registrar),notification_(std::move(notification)),options_(std::move(options)){}
  ~RemoteTrackEngine(){Stop();}
  RemoteTrackEngine(const RemoteTrackEngine&)=delete;
  RemoteTrackEngine& operator=(const RemoteTrackEngine&)=delete;
  bool Start(const tracks::Configuration& configuration);
  void Stop();
  // A full decode queue is `full` (offer the packet again), never a failure.
  // Only the packet's own output can be full: video waits in its output's lane
  // (kLanePackets / kLaneBytes); audio goes straight to playback.
  static constexpr size_t kLanePackets=8,kLaneBytes=12*1024*1024;
  MediaPushResult Offer(const std::vector<uint8_t>& bytes);
  bool Push(const std::vector<uint8_t>& bytes){return Offer(bytes)==MediaPushResult::accepted;}
  flutter::EncodableMap Stats();
  std::map<uint8_t,int64_t> Textures() const;
  HRESULT LatestFrame(uint8_t track,uint32_t topology,uint32_t epoch,FrameSnapshot& result);
  // UI thread: a shared reference to the primary output's current frame (the
  // first decoded output otherwise) and its publication count. Read-only: no
  // pixel copy and no change to texture/native-read accounting.
  std::shared_ptr<const DecodedFrame> Latest(uint64_t* serial=nullptr) const;
  // Hidden windows (backlog #62): decoding goes on, Flutter is asked for no
  // frame until the window is shown again (then for each output's newest).
  void SetHidden(bool hidden);
 private:
  struct Frames {
    std::mutex mutex;
    std::shared_ptr<DecodedFrame> latest;
    // CPU copy of `latest` when it is a GPU picture (Latest(), thumbnails, a
    // Flutter pixel buffer of a GPU output).
    std::shared_ptr<DecodedFrame> cpuCopy;
    uint64_t cpuCopyPublished=0;
    uint64_t published=0,submitted=0,textureFrames=0,skipped=0;
    uint64_t nativeSubmitted=0,nativeReads=0,nativeSkipped=0;
    bool notificationQueued=false;
    std::chrono::steady_clock::time_point lastDecoded{};
  };
  struct Output {
    // Chosen at Start (UI thread), before the worker runs; fixed for the stream.
    gpu::Selection selection;
    std::unique_ptr<gpu::Converter> converter;
    std::unique_ptr<gpu::Overlay> overlay;
    bool gpu=false;            // pictures are shared GPU textures
    bool flutterSurface=false; // ... and the Flutter texture is a GPU surface (same device as Flutter's)
    // Written by the worker, read by Stats under mutex_.
    bool hardware=false,prestarted=false;int tier=0;uint32_t vendor=0;uint64_t fallbacks=0,gpuDropped=0;
    std::shared_ptr<Frames> frames;
    std::unique_ptr<flutter::TextureVariant> texture;
    int64_t id=-1;
    uint64_t decodeCalls=0;
    double decodeTotalMs=0,decodeMaxMs=0,notifyTotalMs=0,notifyMaxMs=0;
  };
  struct Packet {std::vector<uint8_t> bytes;std::chrono::steady_clock::time_point enqueued;};
  // One output's packets and its worker; everything but `worker` under mutex_.
  struct Lane {
    std::deque<Packet> queue;
    size_t bytes=0;
    uint64_t refused=0;  // offers answered `full` (the reader waited)
    bool ready=false;    // its decoder delivered a picture of the current epoch
    std::condition_variable wake;
    std::thread worker;
  };
  // `configuration` holds this lane's output only (its own decoder and gate).
  void Run(tracks::Configuration configuration,bool power) noexcept;
  // The first failure ends every lane (later ones keep it).
  void Fail(HRESULT failure);
  static std::shared_ptr<const DecodedFrame> CpuView(const std::shared_ptr<Frames>& frames);
  flutter::TextureRegistrar* registrar_;
  FrameNotification notification_;
  Options options_;
  tracks::Configuration configuration_;
  std::array<Output,2> outputs_;
  std::atomic<bool> running_{false},ready_{false},hidden_{false};
  std::atomic<HRESULT> error_{S_OK};
  std::mutex mutex_;
  std::array<Lane,2> lanes_;
  uint64_t stalePackets_=0,waitingPackets_=0;
  // Shared audio (UI thread, under mutex_): played once every output has a
  // picture, timestamps strictly increasing like the tracks gate requires.
  std::optional<uint64_t> audioPts_;
  uint64_t losslessPackets_=0;double losslessMs_=0;std::array<uint32_t,2> losslessPresent_{};bool losslessPoisoned_=false;
  RemoteAudio audio_;
};
}
