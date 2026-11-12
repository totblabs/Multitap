#include "remote_track_engine.h"
#include "stream_size_admission.h"
#include "session_power_activity.h"
#include "process_load.h"
#include "../../../../native/windows_video/track_pending_input.hpp"
#include <algorithm>
#include <new>

namespace multitap {
namespace {
// A Flutter GPU surface descriptor and the picture (pool texture) it keeps alive
// until Flutter has opened the shared handle.
struct HeldSurface {FlutterDesktopGpuSurfaceDescriptor surface{};std::shared_ptr<DecodedFrame> frame;};
struct HeldBuffer {FlutterDesktopPixelBuffer buffer{};std::shared_ptr<const DecodedFrame> frame;};
}
bool RemoteTrackEngine::Start(const tracks::Configuration& configuration){
  if(!registrar_ || !tracks::valid(configuration))return false;
  Stop();configuration_=configuration;
  // Flutter's own render device, to tell which outputs may be a GPU texture of its.
  bool flutterKnown=false;LUID flutterLuid{};
  if(options_.render_adapter){const auto info=gpu::Describe(options_.render_adapter.Get());flutterLuid=info.luid;flutterKnown=true;}
  for(const auto& track:configuration_.tracks){
    auto& output=outputs_[track.id];output.frames=std::make_shared<Frames>();
    // Adapter, decoder tier and presentation kind are fixed for the stream. A
    // texture kind cannot change once Flutter has it, so the GPU path is taken
    // only after a real conversion on the output's device matched the CPU's pixels.
    if(options_.hardware_decode){
      try{
        gpu::SelectionOptions choice;choice.hardware_decode=true;choice.gpu_present=true;
        choice.allow_software_render=options_.allow_software_render;choice.warp_render=options_.warp_render;
        output.selection=gpu::SelectForTrack(options_.output_adapters[track.id].Get(),options_.render_adapter.Get(),track.width,track.height,choice);
        std::array<StreamSize,2> sizes{};size_t count=0;for(const auto& t:configuration_.tracks)sizes[count++]={t.width,t.height,options_.fps,t.sourceWidth,t.sourceHeight};
        StreamMemory memory{};if(!StreamMemoryNeeded(sizes.data(),count,memory))return false;
        uint64_t decodeBytes=0;for(const auto& t:configuration_.tracks)decodeBytes+=uint64_t(t.width)*t.height*3/2*18;
        const bool shared=output.selection.render&&output.selection.decode&&gpu::SameLuid(output.selection.render->info.luid,output.selection.decode->info.luid);
        if(output.selection.render&&!output.selection.render->TextureFits(track.sourceWidth,track.sourceHeight,memory.gpuBytes+(shared?decodeBytes:0)))output.selection.render.reset();
        if(output.selection.decode&&!output.selection.decode->DecodeFits(track.width,track.height,decodeBytes+(output.selection.render&&shared?memory.gpuBytes:0)))output.selection.decode.reset();
        if(output.selection.render){
          auto converter=std::make_unique<gpu::Converter>(output.selection.render);
          if(gpu::ConversionSelfTest(*converter)){
            output.converter=std::move(converter);output.overlay=std::make_unique<gpu::Overlay>(output.selection.render);output.gpu=true;
            output.flutterSurface=flutterKnown?gpu::SameLuid(output.selection.render->info.luid,flutterLuid):options_.warp_render;
          }else output.selection.render.reset();  // hardware decode stays, CPU pictures
        }
      }catch(...){output.selection={};output.converter.reset();output.overlay.reset();output.gpu=output.flutterSurface=false;}
    }
    const auto frames=output.frames;
    if(output.flutterSurface){
      output.texture=std::make_unique<flutter::TextureVariant>(flutter::GpuSurfaceTexture(
        kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
        [frames](size_t,size_t)->const FlutterDesktopGpuSurfaceDescriptor*{
          auto held=std::make_unique<HeldSurface>();
          {
            std::lock_guard<std::mutex> lock(frames->mutex);held->frame=frames->latest;
            if(held->frame && frames->submitted!=frames->published){
              frames->skipped+=frames->published-frames->submitted-1;
              frames->submitted=frames->published;++frames->textureFrames;
            }
          }
          if(!held->frame || !held->frame->gpu)return nullptr;
          auto& surface=held->surface;
          surface.struct_size=sizeof(surface);surface.handle=held->frame->gpu->shared;
          surface.width=surface.visible_width=held->frame->width;surface.height=surface.visible_height=held->frame->height;
          surface.format=kFlutterDesktopPixelFormatBGRA8888;
          surface.release_context=held.get();surface.release_callback=[](void* value){delete static_cast<HeldSurface*>(value);};
          return &held.release()->surface;
        }));
    }else{
      output.texture=std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
        [frames](size_t,size_t)->const FlutterDesktopPixelBuffer*{
          auto held=std::make_unique<HeldBuffer>();
          {
            std::lock_guard<std::mutex> lock(frames->mutex);held->frame=frames->latest;
            if(held->frame && frames->submitted!=frames->published){
              frames->skipped+=frames->published-frames->submitted-1;
              frames->submitted=frames->published;++frames->textureFrames;
            }
          }
          // A GPU output on another device than Flutter's has no CPU pixels:
          // read the picture back (only reached if something shows this texture).
          if(held->frame && held->frame->rgba.empty())held->frame=CpuView(frames);
          if(!held->frame || held->frame->rgba.empty())return nullptr;
          held->buffer.buffer=held->frame->rgba.data();held->buffer.width=held->frame->width;held->buffer.height=held->frame->height;
          held->buffer.release_context=held.get();held->buffer.release_callback=[](void* value){delete static_cast<HeldBuffer*>(value);};
          return &held.release()->buffer;
        }));
    }
    output.id=registrar_->RegisterTexture(output.texture.get());
    if(output.id<0){Stop();return false;}
  }  {std::lock_guard<std::mutex> lock(mutex_);stalePackets_=waitingPackets_=0;losslessPackets_=0;losslessMs_=0;losslessPresent_={};losslessPoisoned_=false;
    audioPts_.reset();for(auto& lane:lanes_){lane.queue.clear();lane.bytes=0;lane.refused=0;lane.ready=false;}}
  error_=S_OK;ready_=false;running_=true;
  // audio-opus-tracks-v1: the shared audio decodes from Opus (with its PLC and timer wait).
  if(configuration_.audioEnabled)audio_.Start(configuration_.audioOpus);
  // One worker per output, each with a configuration of that output alone (its
  // own decoder and packet gate); shared audio stays with the engine (Offer).
  try{bool power=true;
    for(const auto& track:configuration_.tracks){
      tracks::Configuration own;own.topology=configuration_.topology;own.audioEnabled=false;own.tracks={track};
      lanes_[track.id].worker=std::thread([this,own,power]{Run(own,power);});power=false;
    }
  }catch(...){Stop();return false;}
  return true;
}
void RemoteTrackEngine::Fail(HRESULT failure){
  HRESULT none=S_OK;error_.compare_exchange_strong(none,failure);ready_=false;
  // A lane between its predicate and its wait sees the failure after this lock.
  {std::lock_guard<std::mutex> lock(mutex_);}
  for(auto& lane:lanes_)lane.wake.notify_all();
}
void RemoteTrackEngine::Stop(){
  running_=false;ready_=false;
  {std::lock_guard<std::mutex> lock(mutex_);}
  for(auto& lane:lanes_)lane.wake.notify_all();
  for(auto& lane:lanes_)if(lane.worker.joinable())lane.worker.join();
  audio_.Stop();
  {std::lock_guard<std::mutex> lock(mutex_);for(auto& lane:lanes_){lane.queue.clear();lane.bytes=0;lane.ready=false;}}
  for(auto& output:outputs_){
    if(output.texture && output.id>=0){auto* texture=output.texture.release();registrar_->UnregisterTexture(output.id,[texture]{delete texture;});}
    output=Output{};
  }
  configuration_={};
}
void RemoteTrackEngine::SetHidden(bool hidden){
  if(hidden_.exchange(hidden)==hidden||hidden)return;
  // Each texture's callback takes its output's newest picture.
  for(const auto& track:configuration_.tracks)if(outputs_[track.id].id>=0&&outputs_[track.id].frames)registrar_->MarkTextureFrameAvailable(outputs_[track.id].id);
}
std::map<uint8_t,int64_t> RemoteTrackEngine::Textures()const{
  std::map<uint8_t,int64_t> result;for(const auto& track:configuration_.tracks)result.emplace(track.id,outputs_[track.id].id);return result;
}
HRESULT RemoteTrackEngine::LatestFrame(uint8_t id,uint32_t topology,uint32_t epoch,FrameSnapshot& result){
  if(!running_||FAILED(error_)||topology!=configuration_.topology)return E_ABORT;
  const auto track=std::find_if(configuration_.tracks.begin(),configuration_.tracks.end(),[&](const auto& value){return value.id==id&&value.epoch==epoch;});
  if(track==configuration_.tracks.end())return E_ABORT;const auto frames=outputs_[id].frames;if(!frames)return E_ABORT;
  std::lock_guard<std::mutex> lock(frames->mutex);frames->notificationQueued=false;
  if(!frames->latest)return S_FALSE;
  if(frames->nativeSubmitted!=frames->published){frames->nativeSkipped+=frames->published-frames->nativeSubmitted-1;frames->nativeSubmitted=frames->published;++frames->nativeReads;}
  result={id,topology,epoch,frames->published,frames->latest};return S_OK;
}
std::shared_ptr<const DecodedFrame> RemoteTrackEngine::CpuView(const std::shared_ptr<Frames>& frames){
  std::shared_ptr<DecodedFrame> latest;uint64_t published=0;
  {
    std::lock_guard<std::mutex> lock(frames->mutex);latest=frames->latest;published=frames->published;
    if(!latest || !latest->rgba.empty() || !latest->gpu)return latest;
    if(frames->cpuCopy && frames->cpuCopyPublished==published)return frames->cpuCopy;
  }
  // A GPU picture is read back once per publication.
  auto copy=std::make_shared<DecodedFrame>();copy->width=latest->width;copy->height=latest->height;copy->pts_us=latest->pts_us;
  if(!latest->gpu->ReadRgba(copy->rgba))return {};
  std::lock_guard<std::mutex> lock(frames->mutex);frames->cpuCopy=copy;frames->cpuCopyPublished=published;return copy;
}
std::shared_ptr<const DecodedFrame> RemoteTrackEngine::Latest(uint64_t* serial)const{
  std::shared_ptr<const DecodedFrame> fallback;uint64_t fallbackSerial=0;
  for(const auto& track:configuration_.tracks){
    const auto frames=outputs_[track.id].frames;if(!frames)continue;
    uint64_t published=0;bool any=false;
    {std::lock_guard<std::mutex> lock(frames->mutex);any=frames->latest!=nullptr;published=frames->published;}
    if(!any)continue;
    auto view=CpuView(frames);if(!view)continue;
    if(track.primary){if(serial)*serial=published;return view;}
    if(!fallback){fallback=std::move(view);fallbackSerial=published;}
  }
  if(serial&&fallback)*serial=fallbackSerial;return fallback;
}MediaPushResult RemoteTrackEngine::Offer(const std::vector<uint8_t>& bytes){
  if(!running_ || FAILED(error_))return MediaPushResult::stopped;
  tracks::Packet packet;
  if(!tracks::parse(bytes.data(),bytes.size(),packet,configuration_.audioOpus))return MediaPushResult::invalid;
  HRESULT failure=E_INVALIDARG;bool play=false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // An earlier topology is history, whichever output it names (the gate's
    // `stale`); it never takes a lane slot.
    if(packet.topology<configuration_.topology){++stalePackets_;return MediaPushResult::accepted;}
    if(packet.kind==tracks::video){
      const bool own=packet.topology==configuration_.topology&&std::any_of(configuration_.tracks.begin(),configuration_.tracks.end(),[&](const auto& t){return t.id==packet.track;});
      if(own){
        // Only this output's lane can be full; the other output keeps going.
        auto& lane=lanes_[packet.track];
        if(lane.queue.size()>=kLanePackets||lane.bytes+bytes.size()>kLaneBytes){++lane.refused;return MediaPushResult::full;}
        lane.queue.push_back({bytes,std::chrono::steady_clock::now()});lane.bytes+=bytes.size();lane.wake.notify_one();return MediaPushResult::accepted;
      }
      // A later topology, or an output this one does not have: the gate's `invalid`.
    }else if(packet.topology==configuration_.topology&&configuration_.audioEnabled){
      // Audio waits for nobody's decoder (it used to share the video queue):
      // dropped until every output has a picture, then played in order.
      if(!ready_){++waitingPackets_;return MediaPushResult::accepted;}
      if(!audioPts_||packet.pts>*audioPts_){audioPts_=packet.pts;play=true;}
    }
  }
  if(play){if(audio_.Push(packet.pts,packet.payload,packet.size))return MediaPushResult::accepted;failure=E_FAIL;}
  Fail(failure);return MediaPushResult::invalid;
}
flutter::EncodableMap RemoteTrackEngine::Stats(){
  const auto load=ProcessLoadSampler::Instance().Latest();
  using V=flutter::EncodableValue;std::lock_guard<std::mutex> lock(mutex_);
  flutter::EncodableList tracks;uint64_t decoded=0,textures=0,skipped=0,calls=0,refused=0;size_t queuedMost=0,queuedBytes=0;
  double decodeTotal=0,decodeMax=0,decodeBusy=0,notifyTotal=0,notifyMax=0,queueAge=0;const auto now=std::chrono::steady_clock::now();bool allGpu=!configuration_.tracks.empty();
  // Overall decoder facts over the configured outputs (each row has its own):
  // hardware only when every output decodes in hardware, the lowest tier, the
  // first hardware vendor, every fallback.
  bool hardware=!configuration_.tracks.empty();int tier=gpu::kTierOther+1;uint32_t vendor=0;uint64_t fallbacks=0;
  for(const auto& track:configuration_.tracks){
    const auto& output=outputs_[track.id];const auto frames=output.frames;const auto& lane=lanes_[track.id];
    const double laneAge=lane.queue.empty()?0:std::chrono::duration<double,std::milli>(now-lane.queue.front().enqueued).count();
    queuedMost=std::max(queuedMost,lane.queue.size());queuedBytes+=lane.bytes;queueAge=std::max(queueAge,laneAge);refused+=lane.refused;
    hardware=hardware&&output.hardware;tier=std::min(tier,output.tier);if(!vendor&&output.hardware)vendor=output.vendor;fallbacks+=output.fallbacks;
    std::lock_guard<std::mutex> frameLock(frames->mutex);
    allGpu=allGpu&&output.flutterSurface;
    decoded+=frames->published;textures+=frames->textureFrames;skipped+=frames->skipped;calls+=output.decodeCalls;
    decodeTotal+=output.decodeTotalMs;decodeMax=std::max(decodeMax,output.decodeMaxMs);decodeBusy=std::max(decodeBusy,output.decodeTotalMs);
    notifyTotal+=output.notifyTotalMs;notifyMax=std::max(notifyMax,output.notifyMaxMs);
    tracks.emplace_back(flutter::EncodableMap{{V("id"),V(std::to_string(track.id))},{V("epoch"),V(int64_t(track.epoch))},
      {V("width"),V(int64_t(track.sourceWidth))},{V("height"),V(int64_t(track.sourceHeight))},
      {V("encodedWidth"),V(int64_t(track.width))},{V("encodedHeight"),V(int64_t(track.height))},{V("textureId"),V(output.id)},
      {V("decodedFrames"),V(int64_t(frames->published))},{V("textureFrames"),V(int64_t(frames->textureFrames))},
      {V("nativeFrameReads"),V(int64_t(frames->nativeReads))},{V("nativeSkippedDecodedFrames"),V(int64_t(frames->nativeSkipped))},
      {V("skippedDecodedFrames"),V(int64_t(frames->skipped))},{V("decodeCalls"),V(int64_t(output.decodeCalls))},
      {V("decodeTotalMs"),V(output.decodeTotalMs)},{V("decodeMaxMs"),V(output.decodeMaxMs)},
      {V("decodeAgeMs"),frames->published?V(std::chrono::duration<double,std::milli>(now-frames->lastDecoded).count()):V()},
      {V("textureNotifyTotalMs"),V(output.notifyTotalMs)},{V("textureNotifyMaxMs"),V(output.notifyMaxMs)},
      // This reports the registered Flutter texture, not the converter's output.
      // A GPU picture on another/unknown Flutter adapter uses PixelBufferTexture
      // and CpuView readback. decodeTier reports hardware decoding independently.
      {V("texturePath"),V(int64_t(output.flutterSurface?1:0))},{V("flutterSurface"),V(output.flutterSurface)},
      {V("hardwareDecode"),V(output.hardware)},{V("decoderPrestarted"),V(output.prestarted)},{V("decodeTier"),V(int64_t(output.tier))},{V("decodeVendor"),V(int64_t(output.vendor))},
      {V("renderVendor"),V(int64_t(output.selection.render_vendor))},{V("decoderFallbacks"),V(int64_t(output.fallbacks))},
      {V("gpuDroppedFrames"),V(int64_t(output.gpuDropped))},
      // This output's lane: packets waiting, the oldest one's age and how often
      // a packet found it full (the relay reader then waited).
      {V("queuedPackets"),V(int64_t(lane.queue.size()))},{V("queueOldestMs"),V(laneAge)},{V("queueFullRefusals"),V(int64_t(lane.refused))}});
  }
  if(!hardware)tier=0;
  flutter::EncodableMap stats{{V("mediaProtocol"),V("tracks-v1")},{V("topologyEpoch"),V(int64_t(configuration_.topology))},{V("tracks"),V(tracks)},
    {V("tracksReady"),V(ready_.load()&&running_&&SUCCEEDED(error_))},{V("decodedFrames"),V(int64_t(decoded))},
    {V("textureFrames"),V(int64_t(textures))},{V("skippedDecodedFrames"),V(int64_t(skipped))},{V("decodeCalls"),V(int64_t(calls))},
    // decodeTotalMs sums every output's decode time; the outputs decode at the
    // same time, so the busy time of the decoder is the busiest lane's.
    {V("decodeTotalMs"),V(decodeTotal)},{V("decodeBusyMs"),V(decodeBusy)},{V("decodeMaxMs"),V(decodeMax)},{V("textureNotifyTotalMs"),V(notifyTotal)},
    {V("textureNotifyMaxMs"),V(notifyMax)},{V("textureNotifyCalls"),V(int64_t(decoded))},
    // The fullest lane (never above kLanePackets), all lanes' bytes, the oldest
    // waiting packet of any lane and the full answers of all lanes.
    {V("queuedPackets"),V(int64_t(queuedMost))},{V("queuedBytes"),V(int64_t(queuedBytes))},{V("queueOldestMs"),V(queueAge)},{V("queueFullRefusals"),V(int64_t(refused))},
    {V("stalePackets"),V(int64_t(stalePackets_))},{V("waitingPackets"),V(int64_t(waitingPackets_))},
    // texturePath 1: every registered Flutter texture is a shared GPU surface;
    // 0: at least one is a CPU pixel buffer. Converted pictures and decoding can
    // still use a GPU in either case. Texture acquisition is not physical scan-out.
    {V("texturePath"),V(int64_t(allGpu?1:0))},{V("decoderPath"),V(hardware?"dxva-d3d11":"software-mft")},{V("appCpuPct10"),V(int64_t(load.app_cpu))},{V("systemCpuPct10"),V(int64_t(load.system_cpu))},{V("gpuDecodePct10"),V(int64_t(load.gpu_decode))},{V("gpu3dPct10"),V(int64_t(load.gpu_3d))},{V("hardwareDecode"),V(hardware)},
    {V("decodeTier"),V(int64_t(tier))},{V("decodeVendor"),V(int64_t(vendor))},{V("decoderFallbacks"),V(int64_t(fallbacks))},
    {V("error"),V(int64_t(error_))},{V("audioEnabled"),V(configuration_.audioEnabled)},
    {V("audioError"),V(int64_t(configuration_.audioEnabled?audio_.error():S_OK))},
    {V("audioPlayedFrames"),V(int64_t(configuration_.audioEnabled?audio_.played_frames():0))},
    {V("audioReceivedFrames"),V(int64_t(configuration_.audioEnabled?audio_.received_frames():0))},
    {V("audioDroppedFrames"),V(int64_t(configuration_.audioEnabled?audio_.dropped_frames():0))},
    // ToTBSeamless lossless tiles (zero unless the host sends tiles-v1).
    {V("losslessPackets"),V(int64_t(losslessPackets_))},{V("losslessPacketMs"),V(losslessMs_)},
    {V("losslessPresentTiles"),V(int64_t(losslessPresent_[0]+losslessPresent_[1]))},{V("losslessPoisoned"),V(losslessPoisoned_)}};
  if(configuration_.audioEnabled)audio_.AppendStats(stats);
  return stats;
}
void RemoteTrackEngine::Run(tracks::Configuration configuration,bool power)noexcept{
  SessionPowerActivity activity;if(power)activity.Set(true);  // one request per engine
  const auto& own=configuration.tracks.front();const uint8_t id=own.id;auto& lane=lanes_[id];
  const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(com)){Fail(com);return;}
  HRESULT hr=MFStartup(MF_VERSION);const bool mf=SUCCEEDED(hr);
  try{
    if(mf){
      // This output's decoder alone: the other output decodes on its own lane
      // at the same time (a shared GPU device serializes only its draws, see
      // gpu::Converter::Pass).
      RemoteTrackDecoder decoder;
      std::array<RemoteTrackDecoder::Pipeline,2> pipelines;
      {
        auto& output=outputs_[id];auto& pipeline=pipelines[id];
        pipeline.decoding.decode_device=output.selection.decode;pipeline.decoding.tier=output.selection.tier;
        pipeline.decoding.fps=options_.fps;
        pipeline.decoding.converter=output.converter.get();pipeline.overlay=output.overlay.get();
        {std::lock_guard<std::mutex> lock(mutex_);if(!options_.prestarts.empty()){pipeline.decoding.prestart=std::move(options_.prestarts.back());options_.prestarts.pop_back();}}
        pipeline.decoding.pending_input=[this,&lane,topology=configuration.topology,id,epoch=own.epoch]{
          std::lock_guard<std::mutex> lock(mutex_);
          if(!running_||FAILED(error_))return true;
          // The single engine already yields its delayed-output poll to queued
          // H.264. Do the same for this exact independent decoder, without
          // letting a stale epoch suppress its final drain. The other output
          // never waits for this poll: it has its own lane.
          return std::any_of(lane.queue.begin(),lane.queue.end(),[&](const Packet& next){
            return video::queuedTrackH264AccessUnit(next.bytes.data(),next.bytes.size(),topology,id,epoch);
          });
        };
      }
      hr=decoder.ConfigurePipelines(configuration,pipelines);
      const auto publishDecoder=[&]{
        std::lock_guard<std::mutex> lock(mutex_);auto& output=outputs_[id];
        output.hardware=decoder.hardware(id);output.prestarted=decoder.prestarted(id);output.tier=decoder.tier(id);output.vendor=decoder.vendor(id);
        output.fallbacks=decoder.fallbacks(id);output.gpuDropped=decoder.gpuDropped(id);
      };
      if(SUCCEEDED(hr))publishDecoder();
      while(SUCCEEDED(hr)&&running_){
        Packet queued;
        {std::unique_lock<std::mutex> lock(mutex_);lane.wake.wait(lock,[&]{return !running_||FAILED(error_)||!lane.queue.empty();});
          if(!running_||FAILED(error_))break;queued=std::move(lane.queue.front());lane.queue.pop_front();lane.bytes-=queued.bytes.size();}
        tracks::Packet packet;if(!tracks::parse(queued.bytes.data(),queued.bytes.size(),packet)){hr=E_INVALIDARG;break;}
        const auto beginning=std::chrono::steady_clock::now();double notify=0,notifyMax=0;
        tracks::Decision decision;const bool tile=packet.kind==tracks::video&&lossless::isTilePayload(packet.payload,packet.size);
        hr=decoder.Push(queued.bytes.data(),queued.bytes.size(),[this,&notify,&notifyMax](uint8_t track,uint32_t topology,uint32_t epoch,DecodedFrame&& frame){
          if(!running_)return;
          const auto beginning=std::chrono::steady_clock::now();const auto frames=outputs_[track].frames;
          auto value=std::make_shared<DecodedFrame>(std::move(frame));
          bool signal=false;
          {std::lock_guard<std::mutex> lock(frames->mutex);frames->latest=std::move(value);++frames->published;frames->lastDecoded=beginning;if(notification_&&!frames->notificationQueued){frames->notificationQueued=true;signal=true;}}
          if(!hidden_&&!registrar_->MarkTextureFrameAvailable(outputs_[track].id))throw std::runtime_error("Track texture unavailable");
          if(signal)notification_(track,topology,epoch);
          const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-beginning).count();notify+=elapsed;notifyMax=std::max(notifyMax,elapsed);
        },{},&decision,
        [this](uint8_t output)->std::shared_ptr<const DecodedFrame>{const auto frames=outputs_[output].frames;if(!frames)return {};std::lock_guard<std::mutex> lock(frames->mutex);return frames->latest;});
        publishDecoder();
        const double work=std::max(0.0,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-beginning).count()-notify);
        {std::lock_guard<std::mutex> lock(mutex_);
          // Ready once every configured output's own decoder delivered a picture.
          lane.ready=decoder.ready()&&SUCCEEDED(hr);bool all=running_&&SUCCEEDED(error_);
          for(const auto& track:configuration_.tracks)all=all&&lanes_[track.id].ready;
          ready_=all;
          if(decision==tracks::Decision::stale)++stalePackets_;
          else if(decision!=tracks::Decision::accepted)++waitingPackets_;
          else if(tile){++losslessPackets_;losslessMs_+=work;}  // not a decode call
          else if(packet.kind==tracks::video){auto& output=outputs_[packet.track];++output.decodeCalls;output.decodeTotalMs+=work;
            output.decodeMaxMs=std::max(output.decodeMaxMs,work);output.notifyTotalMs+=notify;output.notifyMaxMs=std::max(output.notifyMaxMs,notifyMax);}
          if(packet.kind==tracks::video&&packet.track<2){const auto tiles=decoder.losslessStats(packet.track);losslessPresent_[packet.track]=tiles.presentTiles;losslessPoisoned_=losslessPoisoned_||tiles.poisoned;}}
      }
      decoder.Close();
    }
  }catch(const std::bad_alloc&){hr=E_OUTOFMEMORY;}catch(...){hr=E_FAIL;}
  // A stopped engine leaves S_OK; a failed lane ends the others too.
  ready_=false;if(FAILED(hr))Fail(hr);
  {std::lock_guard<std::mutex> lock(mutex_);lane.ready=false;}
  if(mf)MFShutdown();CoUninitialize();
}
}
