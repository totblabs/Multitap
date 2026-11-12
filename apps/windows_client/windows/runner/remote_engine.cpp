#include "remote_engine.h"
#include "video_bounds.h"
#include "stream_size_admission.h"
#include "gpu_lossless.h"
#include "session_power_activity.h"
#include "process_load.h"
#include "../../../../native/windows_video/color.hpp"
#include "../../../../native/windows_video/delayed_output.hpp"
#include <cstring>
#include <algorithm>
namespace multitap {


std::shared_ptr<DecodedFrame> RemoteEngine::TakeForFlutter(const std::shared_ptr<Frames>& frames, const std::shared_ptr<Signal>& signal, std::chrono::steady_clock::time_point& taken) {
  std::shared_ptr<DecodedFrame> frame;
  taken = std::chrono::steady_clock::now();
  bool remark = false;
  {
    std::lock_guard<std::mutex> lock(frames->mutex);
    auto& pending = frames->pending;
    if (!pending.empty()) {
      // Frames still waiting from the previous take (a standing backlog)
      // or a frame kept while Flutter drew nothing: show the newest.
      uint32_t skipped = 0;
      if (pending.size() > 1 && (frames->backlog > 0 || taken - pending.front().published > kStaleFrame)) {
        skipped = uint32_t(pending.size() - 1);
        frames->skipped += pending.size() - 1;
        pending.erase(pending.begin(), pending.end() - 1);
      }
      const double wait_ms = std::chrono::duration<double, std::milli>(taken - pending.front().published).count();
      // A take gap the waiting frame spent most of: Flutter did not take it (kind 2).
      // A frame published just now was late itself, which kind 1 records.
      const double gap_ms = std::chrono::duration<double, std::milli>(taken - frames->last_take).count();
      if (frames->last_take != std::chrono::steady_clock::time_point{} && taken - frames->last_take >= kStall && wait_ms * 2 >= gap_ms) {
        Stall stall; stall.serial = ++frames->present_serial; stall.kind = 2; stall.at = taken; stall.gap_ms = gap_ms; stall.wait_ms = wait_ms;
        stall.pending = uint32_t(pending.size()); stall.skipped = skipped;
        frames->present_stalls.push_back(stall);
        while (frames->present_stalls.size() > kStallRecords) frames->present_stalls.pop_front();
      }
      frames->last_take = taken;
      frames->wait_total_ms += wait_ms;
      frame = std::move(pending.front().frame);
      pending.pop_front();
      ++frames->texture_frames;
      remark = !pending.empty();
      frames->backlog = remark ? frames->backlog + 1 : 0;
    } else {
      // Nothing new: Flutter redraws the newest frame.
      frame = frames->latest;
    }
  }
  // The next waiting frame needs its own Flutter frame; the worker asks.
  if (remark) {
    { std::lock_guard<std::mutex> lock(signal->mutex); signal->remark = true; }
    signal->wake.notify_one();
  }
  return frame;
}

int64_t RemoteEngine::Start(uint32_t width, uint32_t height, bool audio_enabled, bool audio_opus) {
  Stop();
  if (!ValidVideoSize(width, height)) return -1;
  // Adapter, decoder tier and presentation kind are fixed for the stream. A
  // texture kind cannot change once Flutter has it, so the GPU path is taken
  // only after a real conversion on this device matched the CPU's pixels.
  selection_ = {}; converter_.reset(); overlay_.reset(); present_gpu_ = false;
  if (options_.hardware_decode || options_.gpu_present) {
    try {
      gpu::SelectionOptions choice;
      choice.hardware_decode = options_.hardware_decode; choice.gpu_present = options_.gpu_present;
      choice.allow_software_render = options_.allow_software_render; choice.warp_render = options_.warp_render;
      selection_ = gpu::Select(options_.render_adapter.Get(), width, height, choice);
      StreamSize shape{width,height,options_.fps,options_.sourceWidth,options_.sourceHeight};StreamMemory memory{};
      if(!StreamMemoryNeeded(&shape,1,memory))return -1;
      const uint64_t decodeBytes=uint64_t(width)*height*3/2*18;
      const bool shared=selection_.render&&selection_.decode&&gpu::SameLuid(selection_.render->info.luid,selection_.decode->info.luid);
      if(selection_.render&&!selection_.render->TextureFits(shape.sourceWidth?shape.sourceWidth:width,shape.sourceHeight?shape.sourceHeight:height,memory.gpuBytes+(shared?decodeBytes:0)))selection_.render.reset();
      if(selection_.decode&&!selection_.decode->DecodeFits(width,height,decodeBytes+(selection_.render&&shared?memory.gpuBytes:0)))selection_.decode.reset();
      if (selection_.render) {
        auto converter = std::make_unique<gpu::Converter>(selection_.render);
        if (gpu::ConversionSelfTest(*converter)) {
          converter_ = std::move(converter); overlay_ = std::make_unique<gpu::Overlay>(selection_.render); present_gpu_ = true;
        } else selection_.render.reset();
      }
    } catch (...) { selection_ = {}; converter_.reset(); overlay_.reset(); present_gpu_ = false; }
  }
  frames_ = std::make_shared<Frames>();
  buffers_ = std::make_shared<FrameBuffers>();
  // Reserved so the release path (possibly Flutter's raster thread) never allocates.
  buffers_->spare.reserve(FrameBuffers::kSpareFrames);
  const auto frames = frames_;
  const auto signal = signal_;
  if (present_gpu_) {
    // Flutter's raster thread calls this once per Flutter frame after a frame
    // was marked available (texture-pacing-v1, see remote_engine.h). The
    // descriptor's shared handle stays valid while `frame` (and its pool) live.
    texture_ = std::make_unique<flutter::TextureVariant>(flutter::GpuSurfaceTexture(
        kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
        [frames, signal](size_t, size_t) -> const FlutterDesktopGpuSurfaceDescriptor* {
          struct HeldSurface {
            FlutterDesktopGpuSurfaceDescriptor surface{};
            std::shared_ptr<DecodedFrame> frame;
            std::shared_ptr<Frames> frames;
            std::chrono::steady_clock::time_point taken;
          };
          auto held = std::make_unique<HeldSurface>();
          held->frame = TakeForFlutter(frames, signal, held->taken);
          if (!held->frame || !held->frame->gpu) return nullptr;
          held->frames = frames;
          auto& surface = held->surface;
          surface.struct_size = sizeof(surface);
          surface.handle = held->frame->gpu->shared;
          surface.width = surface.visible_width = held->frame->width;
          surface.height = surface.visible_height = held->frame->height;
          surface.format = kFlutterDesktopPixelFormatBGRA8888;
          surface.release_context = held.get();
          // Called by Flutter once it has opened the shared texture.
          surface.release_callback = [](void* context) {
            std::unique_ptr<HeldSurface> released(static_cast<HeldSurface*>(context));
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - released->taken).count();
            std::lock_guard<std::mutex> lock(released->frames->mutex);
            released->frames->upload_total_ms += ms; ++released->frames->uploads;
          };
          return &held.release()->surface;
        }));
  } else {
    // Flutter's raster thread calls this once per Flutter frame after a frame
    // was marked available (texture-pacing-v1, see remote_engine.h).
    texture_ = std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
        [frames, signal](size_t, size_t) -> const FlutterDesktopPixelBuffer* {
          struct HeldBuffer {
            FlutterDesktopPixelBuffer buffer{};
            std::shared_ptr<DecodedFrame> frame;
            std::shared_ptr<Frames> frames;
            std::chrono::steady_clock::time_point taken;
          };
          auto held = std::make_unique<HeldBuffer>();
          held->frame = TakeForFlutter(frames, signal, held->taken);
          if (!held->frame) return nullptr;
          held->frames = frames;
          held->buffer.buffer = held->frame->rgba.data();
          held->buffer.width = held->frame->width; held->buffer.height = held->frame->height;
          held->buffer.release_context = held.get();
          // Called by Flutter right after its upload (glTexImage2D) of the buffer.
          held->buffer.release_callback = [](void* context) {
            std::unique_ptr<HeldBuffer> released(static_cast<HeldBuffer*>(context));
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - released->taken).count();
            std::lock_guard<std::mutex> lock(released->frames->mutex);
            released->frames->upload_total_ms += ms; ++released->frames->uploads;
          };
          return &held.release()->buffer;
        }));
  }
  texture_id_ = registrar_->RegisterTexture(texture_.get());
  if (texture_id_ < 0) { texture_.reset(); converter_.reset(); overlay_.reset(); return -1; }
  { std::lock_guard<std::mutex> lock(mutex_); decode_calls_=texture_notify_calls_=texture_remarks_=rgba_buffer_reuses_=0; decode_total_ms_=decode_max_ms_=0; texture_notify_total_ms_=texture_notify_max_ms_=0;
    lossless_packets_=lossless_republishes_=lossless_deferred_=0; lossless_present_=0; lossless_compose_ms_=lossless_republish_ms_=0; lossless_poisoned_=lossless_scaled_=false; watched_.clear(); signal_->remark=signal_->reveal=false; deferred_=0;
    hardware_decode_=decoder_prestarted_=false; decode_tier_=0; decode_vendor_=0; decoder_fallbacks_=gpu_dropped_=0; decoder_fallback_reason_=0; gpu_convert_total_ms_=gpu_convert_max_ms_=0;
    since_={}; stalls_.clear(); stall_serial_=0; publishing_key_=false; }
  error_ = S_OK; running_ = true; audio_enabled_=audio_enabled;
  if (audio_enabled_) audio_.Start(audio_opus);
  worker_ = std::thread([this, width, height] { Run(width, height); });
  return texture_id_;
}
void RemoteEngine::Quiesce() {
  running_ = false; wake_.notify_all(); if (worker_.joinable()) worker_.join(); audio_.Stop();
  { std::lock_guard<std::mutex> lock(mutex_); queue_.clear(); queued_bytes_ = 0; }
}
void RemoteEngine::SetHidden(bool hidden) {
  if (hidden_.exchange(hidden) == hidden || hidden) return;
  // The worker owns the decoder and its kept picture; without one, the newest is shown as it is.
  { std::lock_guard<std::mutex> lock(mutex_);
    if (running_ && SUCCEEDED(error_) && worker_.joinable()) { signal_->reveal = true; wake_.notify_one(); return; } }
  ShowLatest();
}
void RemoteEngine::ShowLatest() {
  const auto frames = frames_;
  if (!frames) return;
  std::deque<Frames::Pending> dropped;  // released outside the lock
  {
    std::lock_guard<std::mutex> lock(frames->mutex);
    if (!frames->latest) return;
    // Whatever still waited from before the window was hidden is older.
    if (frames->pending.empty() || frames->pending.back().frame != frames->latest) {
      dropped.swap(frames->pending);
      frames->pending.push_back({frames->latest, std::chrono::steady_clock::now(), false});
    }
  }
  dropped.clear();
  registrar_->MarkTextureFrameAvailable(texture_id_);
}
void RemoteEngine::NoteDeferred(uint64_t pts) {
  uint64_t skipped_total = 0;
  if (const auto frames = frames_) { std::lock_guard<std::mutex> lock(frames->mutex); skipped_total = frames->skipped; }
  std::lock_guard<std::mutex> lock(mutex_);
  ++deferred_;
  // A publication for the stall records: gaps between a hidden window's pictures are no stalls.
  since_ = {}; since_.at = std::chrono::steady_clock::now(); since_.queue_max = uint32_t(queue_.size()); since_.skipped = skipped_total; since_.pts = pts;
}
void RemoteEngine::Stop() {
  Quiesce();
  if (texture_ && texture_id_ >= 0) {
    auto* texture = texture_.release(); registrar_->UnregisterTexture(texture_id_, [texture] { delete texture; });
  }
  // Pooled textures Flutter may still hold live on through their frames; the
  // converter itself is only used by the (stopped) worker.
  texture_id_ = -1; frames_.reset(); buffers_.reset(); converter_.reset(); overlay_.reset();
}
MediaPushResult RemoteEngine::Offer(const std::vector<uint8_t>& packet, bool watch) {
  if (!running_ || FAILED(error_)) return MediaPushResult::stopped;
  if (packet.size() < 13 || packet.size() > 4 * 1024 * 1024 ||
      packet[2] || packet[3] || (packet[0] != 1 && packet[0] != 2) ||
      (packet[0] == 2 ? packet[1] != 0 : (packet[1] & ~1) != 0)) return MediaPushResult::invalid;
  if (packet[0] == 2) {
    uint64_t pts = 0; std::memcpy(&pts, packet.data() + 4, 8);
    return audio_enabled_ && audio_.Push(pts, packet.data() + 12, packet.size() - 12) ? MediaPushResult::accepted : MediaPushResult::invalid;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.size() >= kQueuePackets || queued_bytes_ + packet.size() > kQueueBytes) { ++since_.full; return MediaPushResult::full; }
  queued_bytes_ += packet.size(); queue_.push_back({packet,std::chrono::steady_clock::now(),watch}); wake_.notify_one();
  ++since_.offers; since_.queue_max = std::max(since_.queue_max, uint32_t(queue_.size()));
  return MediaPushResult::accepted;
}
std::shared_ptr<const DecodedFrame> RemoteEngine::Latest(uint64_t* serial) const {
  const auto frames = frames_;
  if (!frames) return {};
  std::shared_ptr<DecodedFrame> latest;
  uint64_t published = 0;
  {
    std::lock_guard<std::mutex> lock(frames->mutex);
    published = frames->published;
    if (serial) *serial = published;
    latest = frames->latest;
    if (!latest || !latest->gpu || !latest->rgba.empty()) return latest;
    if (frames->cpu_copy && frames->cpu_copy_serial == published) return frames->cpu_copy;
  }
  // A GPU frame is read back once per publication (thumbnails, retired frame).
  auto copy = std::make_shared<DecodedFrame>();
  copy->width = latest->width; copy->height = latest->height; copy->pts_us = latest->pts_us;
  if (!latest->gpu->ReadRgba(copy->rgba)) return {};
  std::lock_guard<std::mutex> lock(frames->mutex);
  frames->cpu_copy = copy; frames->cpu_copy_serial = published;
  return copy;
}
flutter::EncodableMap RemoteEngine::Stats() {
  using V = flutter::EncodableValue;
  const auto load = ProcessLoadSampler::Instance().Latest();  // timer thread's last reading; never waits
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t decoded=0,texture=0,skipped=0,queued=0,uploads=0,present_packets=0;
  double wait_ms=0,upload_ms=0,present_age_ms=0;
  if (frames_) { std::lock_guard<std::mutex> frame_lock(frames_->mutex); decoded=frames_->published; texture=frames_->texture_frames; skipped=frames_->skipped;
    queued=frames_->queued; uploads=frames_->uploads; wait_ms=frames_->wait_total_ms; upload_ms=frames_->upload_total_ms;
    present_packets=frames_->pending.size();
    if (!frames_->pending.empty()) present_age_ms=std::max(0.0,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-frames_->pending.front().published).count()); }
  const double queue_age = queue_.empty() ? 0 : std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-queue_.front().enqueued).count();
  // A hidden window's pictures kept unconverted are decoded pictures too.
  flutter::EncodableMap stats{{V("decodedFrames"), V(static_cast<int64_t>(decoded + deferred_))},
      {V("hidden"), V(hidden_.load())},
      {V("hiddenDeferredFrames"), V(static_cast<int64_t>(deferred_))},
      {V("textureFrames"), V(static_cast<int64_t>(texture))},
      {V("skippedDecodedFrames"), V(static_cast<int64_t>(skipped))},
      // texture-pacing-v1: frames published while an earlier one still waited
      // for Flutter (queued instead of replaced), published -> taken total,
      // and Flutter's per-take upload (callback -> release) totals. Both
      // texture kinds report them; for a GPU surface the "upload" is Flutter
      // opening the shared texture.
      {V("texturePath"), V(present_gpu_ ? kTexturePathGpuSurface : kTexturePathPixelBuffer)},
      {V("textureQueued"), V(static_cast<int64_t>(queued))},
      {V("textureWaitTotalMs"), V(wait_ms)},
      // Waiting for a Flutter texture take, not compositor/physical scan-out.
      {V("presentQueuePackets"), V(static_cast<int64_t>(present_packets))},
      {V("presentQueueOldestMs"), V(present_age_ms)},
      {V("textureUploads"), V(static_cast<int64_t>(uploads))},
      {V("textureUploadTotalMs"), V(upload_ms)},
      {V("textureRemarks"), V(static_cast<int64_t>(texture_remarks_))},
      {V("decodeCalls"), V(static_cast<int64_t>(decode_calls_))},
      {V("decodeTotalMs"), V(decode_total_ms_)},
      {V("decodeMaxMs"), V(decode_max_ms_)},
      {V("textureNotifyTotalMs"), V(texture_notify_total_ms_)},
      {V("textureNotifyMaxMs"), V(texture_notify_max_ms_)},
      {V("textureNotifyCalls"), V(static_cast<int64_t>(texture_notify_calls_))},
      // "dxva-d3d11": the hardware H.264 decoder is in use; otherwise the CPU
      // Media Foundation decoder. decodeTier: 0 CPU, 1 hardware on the render
      // adapter, 2 hardware on another adapter. Vendor IDs only, no names.
      {V("decoderPath"), V(hardware_decode_ ? "dxva-d3d11" : "software-mft")},
      {V("appCpuPct10"), V(static_cast<int64_t>(load.app_cpu))}, {V("systemCpuPct10"), V(static_cast<int64_t>(load.system_cpu))},
      {V("gpuDecodePct10"), V(static_cast<int64_t>(load.gpu_decode))}, {V("gpu3dPct10"), V(static_cast<int64_t>(load.gpu_3d))},
      {V("hardwareDecode"), V(hardware_decode_)},
      // decoder-prestart-v1: the CPU decoder was started before stream.info.
      {V("decoderPrestarted"), V(decoder_prestarted_)},
      {V("decodeTier"), V(static_cast<int64_t>(decode_tier_))},
      {V("decodeVendor"), V(static_cast<int64_t>(decode_vendor_))},
      {V("renderVendor"), V(static_cast<int64_t>(selection_.render_vendor))},
      {V("decoderFallbacks"), V(static_cast<int64_t>(decoder_fallbacks_))},
      {V("decoderFallbackReason"), V(decoder_fallback_reason_)},
      {V("gpuConvertTotalMs"), V(gpu_convert_total_ms_)},
      {V("gpuConvertMaxMs"), V(gpu_convert_max_ms_)},
      {V("gpuDroppedFrames"), V(static_cast<int64_t>(gpu_dropped_))},
      {V("rgbaBufferReuses"), V(static_cast<int64_t>(rgba_buffer_reuses_))},
      {V("queuedPackets"), V(static_cast<int64_t>(queue_.size()))},
      {V("queueOldestMs"), V(queue_age)},
      {V("audioEnabled"), V(audio_enabled_)},
      {V("audioPlayedFrames"), V(static_cast<int64_t>(audio_enabled_ ? audio_.played_frames() : 0))},
      {V("audioReceivedFrames"), V(static_cast<int64_t>(audio_enabled_ ? audio_.received_frames() : 0))},
      // Oldest audio dropped to bound the latency (AudioJitterBuffer overruns).
      {V("audioDroppedFrames"), V(static_cast<int64_t>(audio_enabled_ ? audio_.dropped_frames() : 0))},
      {V("queuedBytes"), V(static_cast<int64_t>(queued_bytes_))},
      {V("error"), V(static_cast<int64_t>(error_.load()))},
      {V("audioError"), V(static_cast<int64_t>(audio_enabled_ ? audio_.error() : S_OK))},
      // ToTBSeamless lossless tiles (zero unless the host sends tiles-v1).
      {V("losslessPackets"), V(static_cast<int64_t>(lossless_packets_))},
      {V("losslessRepublishes"), V(static_cast<int64_t>(lossless_republishes_))},
      {V("losslessDeferred"), V(static_cast<int64_t>(lossless_deferred_))},
      {V("losslessPresentTiles"), V(static_cast<int64_t>(lossless_present_))},
      {V("losslessComposeMs"), V(lossless_compose_ms_)},
      {V("losslessPacketMs"), V(lossless_republish_ms_)},
      {V("losslessPoisoned"), V(lossless_poisoned_)},
      {V("losslessScaledSource"), V(lossless_scaled_)},
      // input-latency-v1: watched video PTS and its queued -> handed-to-texture
      // microseconds (decoder work + publication; not display scan-out).
      {V("inputFrames"), [this] { flutter::EncodableList list; for (const auto& [pts, us] : watched_) list.push_back(V(flutter::EncodableList{V(static_cast<int64_t>(pts)), V(us)})); return V(std::move(list)); }()},
      // Stall records (kStall), each [kind, serial, gapMs, agoMs, key, queue, queueMax,
      // offers, full, decodes, decodeMaxUs, pending, skipped, waitMs, ptsGapMs]; serials per kind.
      {V("stalls"), [this] {
        flutter::EncodableList list; const auto now = std::chrono::steady_clock::now();
        const auto add = [&](const Stall& s) {
          const auto ms = [](double value) { return static_cast<int64_t>(std::clamp(value, 0.0, 600000.0)); };
          list.push_back(V(flutter::EncodableList{V(int64_t(s.kind)), V(static_cast<int64_t>(s.serial)), V(ms(s.gap_ms)), V(ms(std::chrono::duration<double, std::milli>(now - s.at).count())),
            V(int64_t(s.key ? 1 : 0)), V(int64_t(s.queue)), V(int64_t(s.queue_max)), V(int64_t(s.offers)), V(int64_t(s.full)), V(int64_t(s.decodes)), V(ms(s.decode_max_ms * 1000)),
            V(int64_t(s.pending)), V(int64_t(s.skipped)), V(ms(s.wait_ms)), V(ms(s.pts_gap_ms))}));
        };
        for (const auto& s : stalls_) add(s);
        if (frames_) { std::lock_guard<std::mutex> frame_lock(frames_->mutex); for (const auto& s : frames_->present_stalls) add(s); }
        return V(std::move(list));
      }()}};
  if (audio_enabled_) audio_.AppendStats(stats);
  return stats;
}
void RemoteEngine::Run(uint32_t width, uint32_t height) {
  SessionPowerActivity activity;
  activity.Set(true);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com)) { error_ = com; return; }
  HRESULT hr = MFStartup(MF_VERSION);
  if (SUCCEEDED(hr)) {
    {
      const auto buffers = buffers_;
      RemoteDecoder decoder;
      // Watched video packets waiting for their first published frame.
      std::deque<std::pair<uint64_t, std::chrono::steady_clock::time_point>> watching;
      RemoteDecoder::Options decoder_options;
      decoder_options.fps=options_.fps;
      // Hidden: about one picture per kHiddenPicture is converted (the thumbnail
      // and a retired frame stay recent), the others stay NV12 until shown.
      std::chrono::steady_clock::time_point hidden_picture{};
      decoder_options.defer=[this,&watching,&hidden_picture](uint64_t pts){
        if(!hidden_)return false;
        const auto now=std::chrono::steady_clock::now();
        if(now-hidden_picture>=kHiddenPicture){hidden_picture=now;return false;}
        // Nobody sees this picture: a watched packet is not measured by it.
        while(!watching.empty()&&watching.front().first<=pts)watching.pop_front();
        NoteDeferred(pts);
        return true;
      };
      decoder_options.decode_device = selection_.decode; decoder_options.tier = selection_.tier;
      decoder_options.prestart = std::move(options_.prestart);
      decoder_options.converter = present_gpu_ ? converter_.get() : nullptr;
      decoder_options.pending_input=[this]{
        std::lock_guard<std::mutex> lock(mutex_);
        if(!running_)return true;
        // Tiles alone cannot advance the H.264 transform. Keep polling the
        // final video picture in that case; only a queued access unit yields.
        return std::any_of(queue_.begin(),queue_.end(),[](const QueuedPacket& next){
          return video::queuedH264AccessUnit(next.bytes.data(),next.bytes.size());
        });
      };
      hr = decoder.Initialize(width, height, decoder_options);
      GpuWorker gpu_worker{present_gpu_ ? converter_.get() : nullptr, present_gpu_ ? overlay_.get() : nullptr, &decoder};
      const auto publish_decoder_state = [&] {
        std::lock_guard<std::mutex> lock(mutex_);
        hardware_decode_ = decoder.hardware(); decoder_prestarted_ = decoder.prestarted(); decode_tier_ = decoder.tier(); decode_vendor_ = decoder.vendor();
        decoder_fallbacks_ = decoder.fallbacks(); gpu_dropped_ = decoder.gpu_dropped(); decoder_fallback_reason_ = decoder.fallback_reason();
        const double converted = decoder.TakeConvertMs();
        gpu_convert_total_ms_ += converted; gpu_convert_max_ms_ = std::max(gpu_convert_max_ms_, converted);
      };
      if (SUCCEEDED(hr)) publish_decoder_state();
      // Tiles may describe the unscaled source of a downscaled video.
      lossless::Compositor lossless; lossless.Configure(width, height, true);
      // Due drops first, then every present lossless tile, before publication.
      // A scaled source publishes a source-size frame (video upscaled under
      // absent tiles); without tiles the video-size frame is published as before.
      const auto compose = [&](DecodedFrame& frame) -> HRESULT {
        if (frame.gpu) return ComposeGpu(lossless, gpu_worker, frame);
        if (lossless.scaled()) {
          auto source = TakeFrame(lossless.width(), lossless.height(), frame.pts_us);
          lossless.ComposeDecodedScaled(frame.rgba.data(), size_t(frame.width) * 4, source.rgba.data(), size_t(source.width) * 4, frame.pts_us);
          ReturnBuffer(std::move(frame.rgba)); frame = std::move(source);
        } else lossless.ComposeDecoded(frame.rgba.data(), size_t(frame.width) * 4, frame.pts_us);
        return S_OK;
      };
      while (SUCCEEDED(hr) && running_) {
        std::vector<uint8_t> packet; bool watch = false, video_next = false; std::chrono::steady_clock::time_point enqueued;
        { std::unique_lock<std::mutex> lock(mutex_); wake_.wait(lock, [this] { return !running_ || !queue_.empty() || signal_->remark || signal_->reveal; });
          if (!running_) break;
          if (signal_->reveal) {
            // Shown again: the newest decoded picture, converted now if it was kept.
            signal_->reveal = false; lock.unlock();
            HRESULT composed = S_OK;
            const HRESULT shown = decoder.ConvertDeferred([&](DecodedFrame&& frame) {
              if (lossless.active()) composed = compose(frame);
              Publish(std::move(frame));
            });
            hr = FAILED(shown) ? shown : composed;
            if (shown == S_FALSE) ShowLatest();
            if (SUCCEEDED(hr) && lossless.republishPending() && RepublishLossless(lossless, gpu_worker)) { std::lock_guard<std::mutex> relock(mutex_); ++lossless_republishes_; }
            continue;
          }
          if (signal_->remark) {
            // Flutter took a frame and another one waits: request its frame.
            signal_->remark = false; ++texture_remarks_; lock.unlock();
            registrar_->MarkTextureFrameAvailable(texture_id_);
            continue;
          }
          packet = std::move(queue_.front().bytes); watch = queue_.front().watch; enqueued = queue_.front().enqueued; queue_.pop_front(); queued_bytes_ -= packet.size();
          video_next = !queue_.empty() && !lossless::isTilePayload(queue_.front().bytes.data() + 12, queue_.front().bytes.size() - 12); }
        // ToTBSeamless lossless tiles (tiles-v1) arrive only after this client
        // opted in; they never reach the H.264 decoder. The host sends a tick's
        // tile packet (pts T-1) right before its video (pts T): when that video
        // is already queued, its decoded frame shows the new tiles instead of a
        // republished intermediate picture.
        if (lossless::isTilePayload(packet.data() + 12, packet.size() - 12)) { AcceptLossless(lossless, packet, !video_next, gpu_worker); continue; }
        uint64_t pts = 0; std::memcpy(&pts, packet.data() + 4, 8);
        publishing_key_ = (packet[1] & 1) != 0;
        if (watch) { watching.push_back({pts, enqueued}); while (watching.size() > kWatchingPackets) watching.pop_front(); }
        std::vector<uint8_t> released;
        { std::lock_guard<std::mutex> lock(buffers->mutex);
          if (!buffers->spare.empty()) { released = std::move(buffers->spare.back()); buffers->spare.pop_back(); } }
        const bool reused = !released.empty() && decoder.Recycle(std::move(released));
        const auto decoding = std::chrono::steady_clock::now();
        double texture_notify_ms=0,texture_notify_max_ms=0,compose_ms=0;
        uint64_t texture_notify_calls=0;
        HRESULT compose_error = S_OK;
        hr = decoder.Decode(packet.data() + 12, packet.size() - 12, pts, [this,&lossless,&watching,&compose,&compose_ms,&texture_notify_ms,&texture_notify_max_ms,&texture_notify_calls,&compose_error](DecodedFrame&& frame) {
          if (lossless.active()) {
            const auto composing = std::chrono::steady_clock::now();
            const HRESULT composed = compose(frame);
            if (FAILED(composed) && SUCCEEDED(compose_error)) compose_error = composed;
            compose_ms += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-composing).count();
          }
          const auto notifying = std::chrono::steady_clock::now();
          const uint64_t published_pts = frame.pts_us;
          Publish(std::move(frame));
          if (audio_enabled_) audio_.NoteVideo(published_pts);
          const auto published = std::chrono::steady_clock::now();
          const double elapsed=std::chrono::duration<double,std::milli>(published-notifying).count();
          texture_notify_ms+=elapsed;texture_notify_max_ms=std::max(texture_notify_max_ms,elapsed);++texture_notify_calls;
          // The first frame at or after a watched PTS is now available to the
          // texture (a dropped watched frame resolves with the next later one).
          if (!watching.empty() && watching.front().first <= published_pts) {
            std::lock_guard<std::mutex> lock(mutex_);
            while (!watching.empty() && watching.front().first <= published_pts) {
              watched_.push_back({watching.front().first, std::chrono::duration_cast<std::chrono::microseconds>(published - watching.front().second).count()});
              watching.pop_front();
            }
            while (watched_.size() > kWatchedFrames) watched_.pop_front();
          }
        });
        if (SUCCEEDED(hr) && FAILED(compose_error)) hr = compose_error;
        publish_decoder_state();
        if (compose_ms > 0) { std::lock_guard<std::mutex> lock(mutex_); lossless_compose_ms_ += compose_ms; lossless_present_ = lossless.presentTiles(); lossless_poisoned_ = lossless.poisoned(); }
        // Deferred tiles whose video packet produced no frame appear now (a
        // hidden window's appear when it is shown).
        if (SUCCEEDED(hr) && !hidden_ && lossless.republishPending() && RepublishLossless(lossless, gpu_worker)) { std::lock_guard<std::mutex> lock(mutex_); ++lossless_republishes_; }
        // Decoder work includes MFT processing and NV12 -> RGBA conversion (or
        // the GPU conversion); publishing/Flutter notification can wait
        // independently and must not inflate the codec measurement. Neither
        // value measures presentation.
        const double decode_ms=std::max(0.0,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-decoding).count()-texture_notify_ms-compose_ms);
        { std::lock_guard<std::mutex> lock(mutex_); ++decode_calls_; rgba_buffer_reuses_+=reused?1:0; decode_total_ms_+=decode_ms; decode_max_ms_=std::max(decode_max_ms_,decode_ms); texture_notify_total_ms_+=texture_notify_ms; texture_notify_max_ms_=std::max(texture_notify_max_ms_,texture_notify_max_ms);texture_notify_calls_+=texture_notify_calls;
          // A call that published opened the next interval, so it counts there.
          ++since_.decodes; since_.decode_max_ms=std::max(since_.decode_max_ms,decode_ms); }
      }
    }
    MFShutdown();
  }
  error_ = hr; CoUninitialize();
}
void RemoteEngine::Publish(DecodedFrame&& frame, bool replaceable) {
  const uint64_t pts = frame.pts_us;
  // The last owner returns the pixels for reuse instead of freeing them.
  const auto buffers = buffers_;
  std::shared_ptr<DecodedFrame> next(new DecodedFrame(std::move(frame)), [buffers](DecodedFrame* value) noexcept {
    std::unique_ptr<DecodedFrame> owned(value);
    try {
      if (owned->rgba.empty()) return;  // a GPU frame: its texture returns to its pool
      std::lock_guard<std::mutex> lock(buffers->mutex);
      if (buffers->spare.size() < FrameBuffers::kSpareFrames) buffers->spare.push_back(std::move(owned->rgba));
    } catch (...) {}
  });
  std::shared_ptr<DecodedFrame> previous;
  std::deque<Frames::Pending> dropped;  // released outside the lock
  uint32_t waiting = 0; uint64_t skipped_total = 0;
  // A hidden window's picture only becomes the newest (thumbnails, showing it again).
  const bool hidden = hidden_;
  {
    std::lock_guard<std::mutex> lock(frames_->mutex);
    auto& pending = frames_->pending;
    if (hidden) { frames_->skipped += pending.size(); dropped.swap(pending); }
    // The newest waiting frame is always `latest`. A lossless republish is a
    // copy of it plus exact tiles, and a frame after a republish has all of
    // that republish's tiles: either one replaces the waiting frame.
    if (!pending.empty() && (replaceable || pending.back().replaceable)) { dropped.push_back(std::move(pending.back())); pending.pop_back(); ++frames_->skipped; }
    // An earlier frame still waits for Flutter: queue behind it (it would
    // have been replaced, never shown, before texture-pacing-v1).
    if (!pending.empty()) ++frames_->queued;
    while (pending.size() >= kPresentQueue) { dropped.push_back(std::move(pending.front())); pending.pop_front(); ++frames_->skipped; }
    if (!hidden) pending.push_back({next, std::chrono::steady_clock::now(), replaceable});
    previous = std::move(frames_->latest); frames_->latest = std::move(next); ++frames_->published;
    waiting = uint32_t(pending.size()); skipped_total = frames_->skipped;
  }
  {
    // Kind 1 stall: what the engine saw since the last publication.
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hidden && since_.at != std::chrono::steady_clock::time_point{} && now - since_.at >= kStall) {
      Stall stall; stall.serial = ++stall_serial_; stall.kind = 1; stall.at = now;
      stall.gap_ms = std::chrono::duration<double, std::milli>(now - since_.at).count();
      stall.queue = uint32_t(queue_.size()); stall.queue_max = since_.queue_max; stall.offers = since_.offers; stall.full = since_.full;
      stall.decodes = since_.decodes; stall.decode_max_ms = since_.decode_max_ms; stall.pending = waiting;
      stall.skipped = uint32_t(std::min<uint64_t>(skipped_total - since_.skipped, UINT32_MAX)); stall.key = !replaceable && publishing_key_;
      stall.pts_gap_ms = pts > since_.pts ? double(pts - since_.pts) / 1000 : 0;
      stalls_.push_back(stall);
      while (stalls_.size() > kStallRecords) stalls_.pop_front();
    }
    since_ = {}; since_.at = now; since_.queue_max = uint32_t(queue_.size()); since_.skipped = skipped_total; since_.pts = pts;
  }
  previous.reset(); dropped.clear();
  if (!hidden) registrar_->MarkTextureFrameAvailable(texture_id_);
}
DecodedFrame RemoteEngine::TakeFrame(uint32_t width, uint32_t height, uint64_t pts) {
  DecodedFrame frame; frame.width = width; frame.height = height; frame.pts_us = pts;
  const size_t bytes = size_t(width) * height * 4;
  { std::lock_guard<std::mutex> lock(buffers_->mutex);
    if (!buffers_->spare.empty() && buffers_->spare.back().size() == bytes) { frame.rgba = std::move(buffers_->spare.back()); buffers_->spare.pop_back(); } }
  frame.rgba.resize(bytes);
  return frame;
}
void RemoteEngine::ReturnBuffer(std::vector<uint8_t>&& rgba) {
  try { std::lock_guard<std::mutex> lock(buffers_->mutex); if (buffers_->spare.size() < FrameBuffers::kSpareFrames) buffers_->spare.push_back(std::move(rgba)); } catch (...) {}
}
HRESULT RemoteEngine::ComposeGpu(lossless::Compositor& compositor, GpuWorker& gpu_worker, DecodedFrame& frame) {
  if (!gpu_worker.converter || !gpu_worker.overlay || !frame.gpu) return E_UNEXPECTED;
  return gpu::ComposeDecoded(compositor, *gpu_worker.converter, *gpu_worker.overlay, frame);
}
void RemoteEngine::AcceptLossless(lossless::Compositor& compositor, const std::vector<uint8_t>& packet, bool republish, GpuWorker& gpu_worker) {
  const auto began = std::chrono::steady_clock::now();
  const size_t added_before = compositor.addedTiles().size();
  compositor.Accept(packet.data() + 12, packet.size() - 12);
  if (gpu_worker.overlay && compositor.width()) {
    // The tiles go to the overlay texture once, when they arrive; a scaled
    // source makes the decoder produce source-size pictures from now on.
    gpu::UploadAddedTiles(compositor, *gpu_worker.overlay, added_before);
    if (gpu_worker.decoder && compositor.scaled()) gpu_worker.decoder->SetOutputSize(compositor.width(), compositor.height());
  }
  bool republished = false, deferred = false;
  if (compositor.republishPending()) {
    if (republish && !hidden_) republished = RepublishLossless(compositor, gpu_worker);
    else deferred = true;
  }
  const double elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - began).count();
  std::lock_guard<std::mutex> lock(mutex_);
  ++lossless_packets_; lossless_republishes_ += republished ? 1 : 0; lossless_deferred_ += deferred ? 1 : 0; lossless_republish_ms_ += elapsed;
  lossless_present_ = compositor.presentTiles(); lossless_poisoned_ = compositor.poisoned(); lossless_scaled_ = compositor.scaled();
}
bool RemoteEngine::RepublishLossless(lossless::Compositor& compositor, GpuWorker& gpu_worker) {
  // New tiles appear now instead of at the next video frame: a copy of the
  // newest frame (which already has every earlier tile) plus the added tiles.
  std::shared_ptr<const DecodedFrame> latest;
  { std::lock_guard<std::mutex> lock(frames_->mutex); latest = frames_->latest; }
  if (gpu_worker.converter && gpu_worker.overlay) {
    DecodedFrame frame;
    if (!gpu::RepublishLatest(compositor, *gpu_worker.converter, *gpu_worker.overlay, latest.get(), frame)) return false;
    latest.reset();
    Publish(std::move(frame), true); return true;
  }
  if (latest && latest->width == compositor.width() && latest->height == compositor.height() && latest->rgba.size() == size_t(latest->width) * latest->height * 4) {
    auto copy = TakeFrame(latest->width, latest->height, latest->pts_us);
    std::memcpy(copy.rgba.data(), latest->rgba.data(), copy.rgba.size());
    latest.reset();
    compositor.ComposeAdded(copy.rgba.data(), size_t(copy.width) * 4);
    Publish(std::move(copy), true); return true;
  }
  if (latest && compositor.scaled() && latest->width == compositor.videoWidth() && latest->height == compositor.videoHeight() && latest->rgba.size() == size_t(latest->width) * latest->height * 4) {
    // The newest frame is still the video size: upscale it once under the tiles.
    auto source = TakeFrame(compositor.width(), compositor.height(), latest->pts_us);
    compositor.ComposeUpscaled(latest->rgba.data(), size_t(latest->width) * 4, source.rgba.data(), size_t(source.width) * 4);
    latest.reset();
    Publish(std::move(source), true); return true;
  }
  return false;
}
}
