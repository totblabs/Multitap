#include "remote_track_decoder.h"
#include "gpu_lossless.h"
#include "video_memory.h"
#include "../../../../native/windows_video/track_picture.hpp"
#include <algorithm>
#include <new>

namespace multitap {
struct RemoteTrackDecoder::State {
  tracks::Gate gate;
  std::array<std::shared_ptr<RemoteDecoder>,2> decoders;
  std::array<lossless::Compositor,2> lossless;
  // Not owned (see Pipeline): the output's GPU converter / lossless overlay.
  std::array<gpu::Converter*,2> converters{};
  std::array<gpu::Overlay*,2> overlays{};
  unsigned decoded = 0;
  HRESULT failure = S_OK;
  bool closed = false,busy = false,drained = false;
  struct Command {
    explicit Command(std::shared_ptr<State> value):state(std::move(value)){state->busy=true;}
    ~Command(){state->busy=false;}
    std::shared_ptr<State> state;
  };
  bool ready() const {
    if(closed || drained || FAILED(failure) || !gate.ready())return false;
    for(const auto& track:gate.configuration().tracks)if(!(decoded&(1u<<track.id)))return false;
    return true;
  }
  static bool validFrame(const tracks::Track& track,const DecodedFrame& frame) {
    if(frame.width!=track.sourceWidth || frame.height!=track.sourceHeight)return false;
    if(frame.gpu)return frame.rgba.empty() && frame.gpu->width==track.sourceWidth && frame.gpu->height==track.sourceHeight;
    return frame.rgba.size()==size_t(track.sourceWidth)*track.sourceHeight*4;
  }
  static void emit(const std::shared_ptr<State>& state,const tracks::Track track,uint32_t topology,
                   const FrameCallback& callback,DecodedFrame&& frame) {
    if(state->closed || FAILED(state->failure))return;
    if(!validFrame(track,frame)){state->failure=MF_E_INVALIDMEDIATYPE;return;}
    state->decoded|=1u<<track.id;
    callback(track.id,topology,track.epoch,std::move(frame));
  }
  // Decoded video: due tile drops, then present tiles (on the GPU for a GPU
  // picture), then delivery. A picture of an invalid size is composited with
  // nothing and fails delivery.
  static void deliver(const std::shared_ptr<State>& state,const tracks::Track& track,uint32_t topology,
                      const FrameCallback& callback,DecodedFrame&& frame) {
    if(state->closed || FAILED(state->failure))return;
    if(!frame.gpu){
      if(frame.width!=track.width||frame.height!=track.height){state->failure=MF_E_INVALIDMEDIATYPE;return;}
      if(!video::sourcePicture(frame.width,frame.height,track.sourceWidth,track.sourceHeight,frame.rgba,AvailableVideoSystemMemory())){state->failure=E_OUTOFMEMORY;return;}
      frame.width=track.sourceWidth;frame.height=track.sourceHeight;
    }
    auto& compositor=state->lossless[track.id];
    if(compositor.active() && validFrame(track,frame)){
      if(frame.gpu){
        auto* converter=state->converters[track.id];auto* overlay=state->overlays[track.id];
        const HRESULT hr=converter&&overlay?gpu::ComposeDecoded(compositor,*converter,*overlay,frame):E_UNEXPECTED;
        if(FAILED(hr)){state->failure=hr;return;}
      }else compositor.ComposeDecoded(frame.rgba.data(),size_t(frame.width)*4,frame.pts_us);
    }
    emit(state,track,topology,callback,std::move(frame));
  }};
RemoteTrackDecoder::RemoteTrackDecoder():state_(std::make_shared<State>()){}
RemoteTrackDecoder::~RemoteTrackDecoder(){Close();}
bool RemoteTrackDecoder::ready()const{return state_->ready();}
lossless::Compositor::Stats RemoteTrackDecoder::losslessStats(uint8_t track)const{return track<2?state_->lossless[track].stats():lossless::Compositor::Stats{};}
void RemoteTrackDecoder::Close(){const auto state=state_;state->closed=true;state->decoded=0;state->decoders={};state->converters={};state->overlays={};}
bool RemoteTrackDecoder::hardware(uint8_t track)const{const auto& decoder=track<2?state_->decoders[track]:nullptr;return decoder&&decoder->hardware();}
bool RemoteTrackDecoder::prestarted(uint8_t track)const{const auto& decoder=track<2?state_->decoders[track]:nullptr;return decoder&&decoder->prestarted();}
int RemoteTrackDecoder::tier(uint8_t track)const{const auto& decoder=track<2?state_->decoders[track]:nullptr;return decoder?decoder->tier():0;}
uint32_t RemoteTrackDecoder::vendor(uint8_t track)const{const auto& decoder=track<2?state_->decoders[track]:nullptr;return decoder?decoder->vendor():0;}
uint64_t RemoteTrackDecoder::fallbacks(uint8_t track)const{const auto& decoder=track<2?state_->decoders[track]:nullptr;return decoder?decoder->fallbacks():0;}
uint64_t RemoteTrackDecoder::gpuDropped(uint8_t track)const{const auto& decoder=track<2?state_->decoders[track]:nullptr;return decoder?decoder->gpu_dropped():0;}
bool RemoteTrackDecoder::gpu(uint8_t track)const{return track<2&&state_->decoders[track]&&state_->converters[track]!=nullptr;}
bool RemoteTrackDecoder::hardware()const{bool any=false;for(const auto& decoder:state_->decoders)if(decoder){if(!decoder->hardware())return false;any=true;}return any;}
int RemoteTrackDecoder::tier()const{int value=gpu::kTierOther+1;bool any=false;for(const auto& decoder:state_->decoders)if(decoder){value=std::min(value,decoder->tier());any=true;}return any?value:0;}
uint32_t RemoteTrackDecoder::vendor()const{for(const auto& decoder:state_->decoders)if(decoder&&decoder->hardware())return decoder->vendor();return 0;}
uint64_t RemoteTrackDecoder::fallbacks()const{uint64_t total=0;for(const auto& decoder:state_->decoders)if(decoder)total+=decoder->fallbacks();return total;}
HRESULT RemoteTrackDecoder::Configure(tracks::Configuration configuration,const RemoteDecoder::Options& decoding){
  std::array<Pipeline,2> pipelines;for(auto& pipeline:pipelines)pipeline.decoding=decoding;
  return ConfigurePipelines(std::move(configuration),pipelines);
}
HRESULT RemoteTrackDecoder::ConfigurePipelines(tracks::Configuration configuration,const std::array<Pipeline,2>& pipelines){
  const auto state=state_;
  if(state->closed)return E_ABORT;if(state->busy)return E_PENDING;
  State::Command command(state);
  try{
    auto gate=state->gate;const auto result=gate.configure(std::move(configuration));
    if(result==tracks::Configured::invalid)return E_INVALIDARG;
    if(result==tracks::Configured::unchanged)return S_FALSE;
    std::array<std::shared_ptr<RemoteDecoder>,2> decoders;
    for(const auto& track:gate.configuration().tracks){
      auto decoder=std::make_shared<RemoteDecoder>();decoder->SetOutputSize(track.sourceWidth,track.sourceHeight);const HRESULT hr=decoder->Initialize(track.width,track.height,pipelines[track.id].decoding);
      if(FAILED(hr)){state->failure=hr;return hr;}decoders[track.id]=std::move(decoder);
    }
    state->gate=std::move(gate);state->decoders=std::move(decoders);state->decoded=0;
    state->converters={};state->overlays={};
    for(const auto& track:state->gate.configuration().tracks){state->converters[track.id]=pipelines[track.id].decoding.converter;state->overlays[track.id]=pipelines[track.id].overlay;}state->failure=S_OK;state->drained=false;
    // A new topology starts without lossless tiles.
    for(auto& compositor:state->lossless)compositor=lossless::Compositor{};
    for(const auto& track:state->gate.configuration().tracks)state->lossless[track.id].Configure(track.sourceWidth,track.sourceHeight);
    return S_OK;
  }catch(const std::bad_alloc&){state->failure=E_OUTOFMEMORY;return E_OUTOFMEMORY;}
  catch(...){state->failure=E_FAIL;return E_FAIL;}
}
HRESULT RemoteTrackDecoder::Push(const uint8_t* bytes,size_t size,const FrameCallback& frame,
    const AudioCallback& audio,tracks::Decision* decision,const LatestFrame& latest){
  const auto state=state_;
  if(decision)*decision=tracks::Decision::invalid;
  if(state->closed)return E_ABORT;if(state->busy)return E_PENDING;if(FAILED(state->failure))return state->failure;
  if(state->drained)return E_UNEXPECTED;
  State::Command command(state);
  try{
    tracks::Packet packet;
    if(!tracks::parse(bytes,size,packet,state->gate.configuration().audioOpus)){state->failure=E_INVALIDARG;return E_INVALIDARG;}
    // Key flags are not actual decoded frames. Keep shared audio waiting until
    // both configured decoder instances have delivered their current pixels.
    if(packet.kind==tracks::audio && packet.topology==state->gate.configuration().topology &&
       state->gate.configuration().audioEnabled && !state->ready()){
      if(decision)*decision=tracks::Decision::awaitingAllTracks;return S_FALSE;
    }
    const auto result=state->gate.accept(packet);if(decision)*decision=result;
    if(result==tracks::Decision::invalid){state->failure=E_INVALIDARG;return E_INVALIDARG;}
    if(result!=tracks::Decision::accepted)return S_FALSE;
    const auto emit=frame;const auto play=audio;
    HRESULT hr=S_OK;
    if(packet.kind==tracks::audio){
      hr=play?play(packet.payload,packet.size,packet.pts):E_NOTIMPL;
    }else{
      if(!emit){state->failure=E_INVALIDARG;return E_INVALIDARG;}
      const auto decoder=state->decoders[packet.track];tracks::Track track;
      for(const auto& value:state->gate.configuration().tracks)if(value.id==packet.track)track=value;
      if(lossless::isTilePayload(packet.payload,packet.size)){
        // ToTBSeamless tiles-v1 (an opted-in client only): never H.264.
        auto& compositor=state->lossless[packet.track];
        const size_t addedBefore=compositor.addedTiles().size();
        compositor.Accept(packet.payload,packet.size);
        auto* converter=state->converters[packet.track];auto* overlay=state->overlays[packet.track];
        // A GPU output keeps the tile pixels in its overlay texture.
        if(converter&&overlay)gpu::UploadAddedTiles(compositor,*overlay,addedBefore);
        if(compositor.republishPending()&&latest){
          const auto shown=latest(packet.track);
          if(shown&&converter&&overlay){
            DecodedFrame copy;
            if(gpu::RepublishLatest(compositor,*converter,*overlay,shown.get(),copy))State::emit(state,track,packet.topology,emit,std::move(copy));
          }else if(shown&&shown->width==track.sourceWidth&&shown->height==track.sourceHeight&&shown->rgba.size()==size_t(track.sourceWidth)*track.sourceHeight*4){
            DecodedFrame copy;copy.width=shown->width;copy.height=shown->height;copy.pts_us=shown->pts_us;copy.rgba=shown->rgba;
            compositor.ComposeAdded(copy.rgba.data(),size_t(copy.width)*4);
            State::emit(state,track,packet.topology,emit,std::move(copy));
          }
        }      }else hr=decoder->Decode(packet.payload,packet.size,packet.pts,[state,track,topology=packet.topology,emit](DecodedFrame&& value){State::deliver(state,track,topology,emit,std::move(value));});
    }
    if(state->closed)return E_ABORT;
    if(FAILED(hr))state->failure=hr;
    return FAILED(state->failure)?state->failure:hr;
  }catch(const std::bad_alloc&){state->failure=E_OUTOFMEMORY;return E_OUTOFMEMORY;}
  catch(...){state->failure=E_FAIL;return E_FAIL;}
}
HRESULT RemoteTrackDecoder::Drain(const FrameCallback& frame){
  const auto state=state_;
  if(state->closed)return E_ABORT;if(state->busy)return E_PENDING;if(FAILED(state->failure))return state->failure;
  if(state->drained)return S_FALSE;
  if(!state->gate.configuration().topology)return E_UNEXPECTED;
  if(!frame)return E_INVALIDARG;
  State::Command command(state);
  state->drained=true;
  try{
    const auto emit=frame;const auto configuration=state->gate.configuration();
    for(const auto& track:configuration.tracks){
      const auto decoder=state->decoders[track.id];
      const HRESULT hr=decoder->Drain([state,track,topology=configuration.topology,emit](DecodedFrame&& value){State::deliver(state,track,topology,emit,std::move(value));});
      if(state->closed)return E_ABORT;if(FAILED(hr)){state->failure=hr;return hr;}
    }
    return state->failure;
  }catch(const std::bad_alloc&){state->failure=E_OUTOFMEMORY;return E_OUTOFMEMORY;}
  catch(...){state->failure=E_FAIL;return E_FAIL;}
}
}
