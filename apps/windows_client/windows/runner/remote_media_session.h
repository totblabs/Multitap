#pragma once
#include "remote_engine.h"
#include "remote_track_engine.h"
#include "udp_path_gate.h"
#include "../../../../native/windows_video/udp_track_media.hpp"
#include "video_bounds.h"
#include <new>

namespace multitap {
// UI-thread dispatcher shared by legacy and independent-track streaming. Every
// command pins its owner, checks its native lease and excludes reentrant change.
// The registrar outlives this dispatcher and its asynchronous unregister work.
class RemoteMediaSession {
 public:
  using FrameNotification=std::function<void(int64_t,uint8_t,uint32_t,uint32_t)>;
  // `render_adapter`: the adapter Flutter renders this window on (may be null).
  // New streams decode in hardware and present on the GPU where it has a
  // hardware decoder and a hardware render adapter, unless the caller asks for
  // the CPU path (the "hardware decoding" setting); see gpu_video.h.
  explicit RemoteMediaSession(flutter::TextureRegistrar* registrar,FrameNotification notification={},Microsoft::WRL::ComPtr<IDXGIAdapter> render_adapter={}):state_(std::make_shared<State>()) {state_->registrar=registrar;state_->notification=std::move(notification);state_->render_adapter=std::move(render_adapter);}
  ~RemoteMediaSession(){Close();}
  RemoteMediaSession(const RemoteMediaSession&)=delete;
  RemoteMediaSession& operator=(const RemoteMediaSession&)=delete;
  int64_t Lease()const{return state_->closed?0:state_->lease;}
  bool Current(int64_t lease)const{return lease>0&&!state_->closed&&state_->lease==lease&&(state_->legacy||state_->tracks);}
  // `hold` (a viewport resize): the previous single-output engine stops
  // decoding but its texture keeps showing its last picture until
  // ReleaseHeld, so the window shows that picture instead of nothing while
  // the new decoder waits for its first frame. One picture is held at most;
  // any later start, stop or close releases it.
  // `audioOpus`: the stream's audio is audio-opus-v1 (stream.info format "opus").
  HRESULT Start(int64_t lease,uint32_t width,uint32_t height,bool audio,int64_t& texture,const std::function<void()>& reset={},bool hardware=false,uint32_t fps=60,uint32_t sourceWidth=0,uint32_t sourceHeight=0,bool hold=false,bool audioOpus=false){
    if(!ValidVideoSize(width,height))return E_INVALIDARG;
    return Begin(lease,false,hardware,OutputAdapters{},[&](const auto& legacy,const auto&){texture=legacy->Start(width,height,audio,audioOpus);return texture>=0?S_OK:E_FAIL;},reset,fps,sourceWidth,sourceHeight,hold);
  }
  // decoder-prestart-v1: the session waits for its stream info: start the CPU
  // decoder of each of its `outputs` (1 or 2) now; width x height at fps is the
  // expected stream (a guess, see RemoteDecoder::Options::prestart). The next
  // Start/StartTracks takes them; a later Prepare replaces them; Close, or
  // DecoderPrestart::kUnused without a start, releases them.
  void Prepare(uint32_t width,uint32_t height,uint32_t fps,size_t outputs){
    const auto state=state_;if(state->closed)return;
    state->prestarts.clear();
    for(size_t i=0;i<outputs&&i<2;++i)if(auto prestart=DecoderPrestart::Begin(width,height,fps))state->prestarts.push_back(std::move(prestart));
  }
  // The held picture's texture is no longer shown: stop that engine.
  bool ReleaseHeld(int64_t texture){
    const auto state=state_;
    if(state->busy||!state->held||texture<0||state->held->TextureId()!=texture)return false;
    auto held=std::move(state->held);held->Stop();return true;
  }
  int64_t HeldTexture()const{return state_->held?state_->held->TextureId():-1;}
  // `output_adapters[id]`: the adapter driving the monitor of the native window
  // that will show output `id` (gpu::AdapterForRect), when known; see
  // RemoteTrackEngine::Options.
  using OutputAdapters=std::array<Microsoft::WRL::ComPtr<IDXGIAdapter>,2>;
  HRESULT StartTracks(int64_t lease,const tracks::Configuration& configuration,std::map<uint8_t,int64_t>& textures,const std::function<void()>& reset={},bool hardware=false,const OutputAdapters& output_adapters={},uint32_t fps=60){
    if(!tracks::valid(configuration))return E_INVALIDARG;
    const auto state=state_;
    return Begin(lease,true,hardware,output_adapters,[&](const auto&,const auto& tracks){if(!tracks->Start(configuration))return E_FAIL;textures=tracks->Textures();state->trackConfiguration=configuration;return S_OK;},reset,fps);
  }
  // `watch` (input-latency-v1) applies to the legacy engine only; independent
  // outputs (tracks-v1) are not measured by that protocol. A full decode queue
  // is kMediaQueueFull (offer the same packet again), distinct from a stopped
  // decoder (kMediaDecoderStopped) and a malformed packet (kMediaPacketInvalid).
  // `path`: udp-media-v1 keys are done, so this packet follows the path rule.
  HRESULT Push(int64_t lease,const std::vector<uint8_t>& bytes,bool watch=false,bool path=false){
    return Apply(lease,[&](State& state){
      if(state.tracks){
        for(const auto& [id,slot]:state.trackGates){
          std::vector<uint8_t> normalized;
          if(udp::trackToUdp(slot.binding,bytes,normalized,state.trackConfiguration.audioOpus))
            return MediaPushHresult(slot.gate->OfferWss(normalized,watch));
        }
        return MediaPushHresult(state.tracks->Offer(bytes));
      }
      return MediaPushHresult(path&&state.gate?state.gate->OfferWss(bytes,watch):state.legacy->Offer(bytes,watch));
    });
  }
  // The legacy gate holds its decoder only while it runs. Independent outputs
  // receive a fresh, separately bound gate below.
  void UsePathGate(std::shared_ptr<UdpPathGate> gate){state_->gate=std::move(gate);}
  // Fresh for every grant, including regrants with unchanged display epochs.
  // A detached old socket can never reach the successor decoder.
  std::shared_ptr<UdpPathGate> TrackPathGate(const udp::TrackBinding& binding){
    const auto state=state_;
    if(state->closed||state->busy||!state->tracks||!udp::validTrackBinding(binding)||state->trackConfiguration.topology!=binding.topology)return {};
    const auto& rows=state->trackConfiguration.tracks;
    const auto found=std::find_if(rows.begin(),rows.end(),[&](const auto& row){return row.id==binding.track&&row.epoch==binding.epoch&&row.primary==binding.primary;});
    if(found==rows.end())return {};
    const auto existing=state->trackGates.find(binding.track);
    if(existing!=state->trackGates.end())existing->second.gate->Attach({});
    auto gate=std::make_shared<UdpPathGate>();
    const auto engine=state->tracks;const bool opus=state->trackConfiguration.audioOpus;
    gate->Attach([engine,binding,opus](const std::vector<uint8_t>& bytes,bool){
      auto framed=bytes;
      // The ordinary gate clears its transport epoch before decoder delivery.
      if(framed.size()<12)return MediaPushResult::invalid;
      framed[2]=uint8_t(binding.epoch);framed[3]=uint8_t(binding.epoch>>8);
      std::vector<uint8_t> track;
      if(!udp::udpToTrack(binding,framed,track,opus))return MediaPushResult::invalid;
      return engine->Offer(track);
    });
    state->trackGates[binding.track]={binding,gate};return gate;
  }
  HRESULT Stats(int64_t lease,flutter::EncodableMap& result){
    return Apply(lease,[&](State& state){result=state.tracks?state.tracks->Stats():state.legacy->Stats();return S_OK;});
  }
  HRESULT LatestTrackFrame(int64_t lease,uint8_t track,uint32_t topology,uint32_t epoch,RemoteTrackEngine::FrameSnapshot& result){
    return Apply(lease,[&](State& state){return state.tracks?state.tracks->LatestFrame(track,topology,epoch,result):E_INVALIDARG;});
  }
  HRESULT Capture(int64_t lease,const std::function<bool()>& capture,bool& accepted){
    return Apply(lease,[&](State&){accepted=capture();return S_OK;});
  }
  // Hidden windows (backlog #62): the window shows nothing of the stream. The
  // current engine and every later one in this window follow it (see
  // RemoteEngine::SetHidden); a held picture needs nothing.
  void SetHidden(bool hidden){
    const auto state=state_;state->hidden=hidden;
    if(const auto legacy=state->legacy)legacy->SetHidden(hidden);
    if(const auto tracks=state->tracks)tracks->SetHidden(hidden);
  }
  // Tests only: GPU presentation on WARP / the Basic Render Driver (otherwise only
  // a hardware render adapter presents on the GPU); `warp` creates WARP when no
  // render adapter was given.
  void AllowSoftwareRenderForTests(bool warp){state_->allow_software_render=true;state_->warp_render=warp;}
  HRESULT Stop(int64_t lease,const std::function<void()>& reset={}){
    const auto state=state_;
    if(!Current(lease))return S_FALSE;if(state->busy)return HRESULT_FROM_WIN32(ERROR_BUSY);
    Command command(state);
    try{if(reset)reset();state->lease=0;Shutdown(*state);return state->closed?E_ABORT:S_OK;}
    catch(...){state->lease=0;Shutdown(*state);return E_FAIL;}
  }
  void Close(){const auto state=state_;state->closed=true;state->lease=0;state->prestarts.clear();if(!state->busy)Shutdown(*state);}
  bool LatestAttempt(int64_t lease)const{return lease>0&&state_->highest==lease;}
  // Receives the last frame of every engine that ends (stop, replacement,
  // failure cleanup or window close), after that engine stopped. It must only
  // queue work; it runs on the UI thread, possibly from a destructor.
  using RetiredFrame=std::function<void(std::shared_ptr<const DecodedFrame>)>;
  void OnRetiredFrame(RetiredFrame retired){state_->retired=std::move(retired);}
  // The current engine's latest (primary) frame without copying pixels, or
  // null while a command is changing the engine.
  std::shared_ptr<const DecodedFrame> Latest(int64_t& lease,uint64_t& serial)const{
    const auto state=state_;if(state->closed||state->busy||state->lease<1)return {};
    lease=state->lease;serial=0;
    try{return state->tracks?state->tracks->Latest(&serial):state->legacy?state->legacy->Latest(&serial):nullptr;}catch(...){return {};}
  }
 private:
  struct State {
    flutter::TextureRegistrar* registrar=nullptr;
    Microsoft::WRL::ComPtr<IDXGIAdapter> render_adapter;
    bool allow_software_render=false,warp_render=false;
    FrameNotification notification;
    RetiredFrame retired;
    std::shared_ptr<UdpPathGate> gate;
    std::shared_ptr<RemoteEngine> legacy;
    std::shared_ptr<RemoteTrackEngine> tracks;
    struct TrackGate {udp::TrackBinding binding;std::shared_ptr<UdpPathGate> gate;};
    std::map<uint8_t,TrackGate> trackGates;
    tracks::Configuration trackConfiguration;
    // A previous engine kept only for its texture (Start with `hold`).
    std::shared_ptr<RemoteEngine> held;
    std::vector<std::shared_ptr<DecoderPrestart>> prestarts;
    int64_t lease=0,highest=0;
    bool closed=false,busy=false,hidden=false;
  };
  struct Command {explicit Command(std::shared_ptr<State> held):state(std::move(held)){state->busy=true;}~Command(){state->busy=false;if(state->closed)Shutdown(*state);}std::shared_ptr<State> state;};
  static void Shutdown(State& state){
    // First: after this no receive thread is inside the engine that stops below.
    if(state.gate)state.gate->Attach({});
    for(auto& [id,slot]:state.trackGates)slot.gate->Attach({});
    state.trackGates.clear();state.trackConfiguration={};
    auto legacy=std::move(state.legacy);auto tracks=std::move(state.tracks);
    std::shared_ptr<const DecodedFrame> last;
    if(state.retired){try{last=tracks?tracks->Latest():legacy?legacy->Latest():nullptr;}catch(...){}}
    if(legacy)legacy->Stop();if(tracks)tracks->Stop();
    if(auto held=std::move(state.held))held->Stop();
    if(last){try{state.retired(std::move(last));}catch(...){}}
  }
  template<class Starter>HRESULT Begin(int64_t lease,bool tracks,bool hardware,const OutputAdapters& output_adapters,Starter begin,const std::function<void()>& reset,uint32_t fps,uint32_t sourceWidth=0,uint32_t sourceHeight=0,bool hold=false){
    const auto state=state_;
    if(lease<1||!state->registrar)return E_INVALIDARG;
    if(state->closed||lease<=state->highest)return E_ABORT;
    if(state->busy)return HRESULT_FROM_WIN32(ERROR_BUSY);
    Command command(state);
    // Taken by this start whatever its outcome; unused ones are released.
    auto prestarts=std::move(state->prestarts);state->prestarts.clear();
    try{
      state->highest=lease;state->lease=0;
      if(hold&&!tracks&&state->legacy&&!state->tracks){
        // No receive thread may be inside the engine that stops decoding.
        if(state->gate)state->gate->Attach({});
        auto previous=std::move(state->legacy);Shutdown(*state);previous->Quiesce();state->held=std::move(previous);
      }else Shutdown(*state);
      if(state->closed)return E_ABORT;
      if(tracks){RemoteTrackEngine::FrameNotification notify;if(state->notification)notify=[callback=state->notification,lease](uint8_t id,uint32_t topology,uint32_t epoch){callback(lease,id,topology,epoch);};RemoteTrackEngine::Options options;options.fps=fps;options.hardware_decode=hardware;options.render_adapter=state->render_adapter;options.output_adapters=output_adapters;options.allow_software_render=state->allow_software_render;options.warp_render=state->warp_render;options.prestarts=prestarts;state->tracks=std::make_shared<RemoteTrackEngine>(state->registrar,std::move(notify),std::move(options));state->tracks->SetHidden(state->hidden);}
      else{RemoteEngine::Options options;options.fps=fps;options.sourceWidth=sourceWidth;options.sourceHeight=sourceHeight;options.hardware_decode=options.gpu_present=hardware;options.render_adapter=state->render_adapter;options.allow_software_render=state->allow_software_render;options.warp_render=state->warp_render;if(!prestarts.empty())options.prestart=prestarts.front();state->legacy=std::make_shared<RemoteEngine>(state->registrar,std::move(options));state->legacy->SetHidden(state->hidden);}
      state->lease=lease;
      // Pin the in-progress engine even if a native callback closes/deletes the
      // dispatcher. No subsequent use of the dispatcher object is required.
      const auto legacy=state->legacy;const auto parallel=state->tracks;
      if(reset)reset();
      if(state->closed||state->lease!=lease)return E_ABORT;
      const HRESULT hr=begin(legacy,parallel);
      if(state->closed||state->lease!=lease)return E_ABORT;
      if(SUCCEEDED(hr)&&legacy&&state->gate)state->gate->Attach([legacy](const std::vector<uint8_t>& bytes,bool watch){return legacy->Offer(bytes,watch);});
      if(FAILED(hr)){state->lease=0;Shutdown(*state);}return hr;
    }catch(const std::bad_alloc&){if(state->lease==lease){state->lease=0;Shutdown(*state);}return E_OUTOFMEMORY;}
    catch(...){if(state->lease==lease){state->lease=0;Shutdown(*state);}return E_FAIL;}
  }
  template<class Action>HRESULT Apply(int64_t lease,Action action){
    const auto state=state_;
    if(!Current(lease))return E_ABORT;if(state->busy)return HRESULT_FROM_WIN32(ERROR_BUSY);
    Command command(state);const auto legacy=state->legacy;const auto tracks=state->tracks;
    try{const HRESULT hr=action(*state);return state->closed||state->lease!=lease?E_ABORT:hr;}
    catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
  }
  std::shared_ptr<State> state_;
};
}
