#pragma once
#include "encoder_diagnostics.hpp"
#include "capture_selection.hpp"
#include "display_operation.hpp"
#include "frame_encode_policy.hpp"
#include "lossless_tile_source.hpp"
#include "stream_size.hpp"
#include "stream_memory.hpp"
#include "stream_observability.hpp"
#include "capture_topology_watch.hpp"
#include "capture_follow.hpp"
#include "cursor_channel.hpp"
#include "media.hpp"
#include "secure_desktop_notice.hpp"
#include "../../../native/windows_video/track_media.hpp"
#include <memory>
#include <array>
#include <condition_variable>
#include <exception>
#include <functional>
#include <iomanip>
#include <mutex>
#include <optional>
#include <thread>
namespace host {
struct OwnedTrackSource {
  multitap::tracks::Track media;
  CaptureSelection capture;
  int bitrate=0;
  std::optional<StreamSizeBounds> adaptive;
};
struct TrackSourcePlan {
  std::string sessionId;
  std::string requestId,resizeId;
  uint32_t topology=0;
  int fps=0,bitrate=0;
  bool audio=false;
  // audio-opus-tracks-v1 granted (trackAudioOpusGranted); the agent clears it
  // when its Opus encoder does not start, and the stream then sends PCM.
  bool audioOpus=false;
  std::vector<OwnedTrackSource> outputs;
  RECT inputArea{};
  multitap::tracks::Configuration media(bool available)const{
    multitap::tracks::Configuration value;value.topology=topology;value.audioEnabled=audio&&available;value.audioOpus=value.audioEnabled&&audioOpus;
    for(const auto& output:outputs)value.tracks.push_back(output.media);return value;
  }
};
// A tracks-v1 command (session.start, or a resize's restart, which keeps its
// fields) whose broker granted audio-opus-tracks-v1 (opus_audio.hpp).
inline bool trackAudioOpusGranted(const Json& command){return command.get("audioCodec").string()=="opus";}
inline uint32_t trackInteger(const Json& value,uint32_t minimum,uint32_t maximum){
  const auto n=value.number(-1);if(!std::isfinite(n)||n<minimum||n>maximum||n!=std::floor(n))throw std::runtime_error("invalid_track_request");
  return uint32_t(n);
}
inline bool trackFields(const Json& value,std::initializer_list<const char*> names){
  const auto* object=std::get_if<Json::Object>(&value.value);if(!object||object->size()!=names.size())return false;
  for(const auto* name:names)if(!object->contains(name))return false;return true;
}
inline TrackSourcePlan bindOwnedTrackSources(const Json& request,const DisplayState& display,const std::vector<CaptureMonitor>& observed){
  TrackSourcePlan plan;plan.sessionId=request.get("sessionId").string();
  if(!displayOperationUuid(plan.sessionId)||request.get("mediaProtocol").string()!="tracks-v1"||
     request.get("mode").string()!="stream"||!std::holds_alternative<std::nullptr_t>(request.get("resizeProtocol").value)||
     !display.ready())throw std::runtime_error("virtual_display_required");
  plan.topology=trackInteger(request.get("topologyEpoch"),1,UINT32_MAX);
  plan.requestId=request.get("requestId").string();plan.resizeId=request.get("resizeId").string();
  if((!plan.requestId.empty()||!plan.resizeId.empty())&&(!displayOperationUuid(plan.resizeId)||plan.requestId.empty()||plan.requestId.size()>64||plan.requestId.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos))throw std::runtime_error("invalid_track_request");
  plan.fps=int(trackInteger(request.get("fps"),30,60));if(plan.fps!=30&&plan.fps!=60)throw std::runtime_error("invalid_track_request");
  // Fixed total up to the advertised maxBitrate (bitrate-adaptive-v1); released
  // brokers never send more than 20 Mbps. Independent outputs stay fixed-rate.
  plan.bitrate=int(trackInteger(request.get("bitrate"),4000000,uint32_t(encoderMaximumBitrate)));
  if(!std::holds_alternative<bool>(request.get("audio").value))throw std::runtime_error("invalid_track_request");plan.audio=request.get("audio").boolean();plan.audioOpus=trackAudioOpusGranted(request);
  const bool sizesV2=streamSizeV2(request);
  const auto declared=streamSizeRequest(request);
  const auto decoder=sizesV2?declared:std::optional<DecoderLimits>{};
  const auto maximumWidth=trackInteger(request.get("maxWidth"),sizesV2?16:320,sizesV2?ownedOutputMaxSide:3840),maximumHeight=trackInteger(request.get("maxHeight"),sizesV2?16:180,sizesV2?ownedOutputMaxSide:2160);
  const auto* values=std::get_if<Json::Array>(&request.get("tracks").value);
  if(!values||values->empty()||values->size()>2)throw std::runtime_error("invalid_track_request");
  std::optional<LONG> translateX,translateY;
  uint64_t pixels=0;unsigned ids=0;
  for(const auto& item:*values){
    if(!trackFields(item,{"id","width","height","x","y","epoch"}))throw std::runtime_error("invalid_track_request");
    const auto id=item.get("id").string();if(id!="0"&&id!="1")throw std::runtime_error("invalid_track_request");
    const unsigned ordinal=id=="0"?0:1;if(ids&(1u<<ordinal))throw std::runtime_error("invalid_track_request");ids|=1u<<ordinal;
    const auto width=trackInteger(item.get("width"),320,sizesV2?ownedOutputMaxSide:maximumWidth),height=trackInteger(item.get("height"),180,sizesV2?ownedOutputMaxSide:maximumHeight);
    if(!ownedOutputSize(width,height))throw std::runtime_error("invalid_track_request");
    const auto epoch=trackInteger(item.get("epoch"),1,UINT32_MAX);
    const auto x=item.get("x").number(double(INT32_MIN)-1),y=item.get("y").number(double(INT32_MIN)-1);
    if(!std::isfinite(x)||!std::isfinite(y)||x!=std::floor(x)||y!=std::floor(y)||x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX||!displayRectangleFits(int64_t(x),int64_t(y),width,height))throw std::runtime_error("invalid_track_request");
    const auto monitor=std::find_if(display.monitors.begin(),display.monitors.end(),[&](const auto& value){return value.id==id;});
    if(monitor==display.monitors.end())throw std::runtime_error("monitor_not_available");
    if(monitor->width!=width||monitor->height!=height)throw std::runtime_error("track_viewport_not_applied");
    const LONG dx=monitor->left-LONG(x),dy=monitor->top-LONG(y);
    if((translateX&&*translateX!=dx)||(translateY&&*translateY!=dy))throw std::runtime_error("track_viewport_not_applied");
    translateX=dx;translateY=dy;
    OwnedTrackSource output;output.capture=selectOwnedCapture(display,id,observed);
    output.media={uint8_t(ordinal),epoch,width,height,width,height,monitor->left,monitor->top,monitor->primary};
    if(decoder){
      output.adaptive=StreamSizeBounds{int(width),int(height),plan.fps,int(maximumWidth),int(maximumHeight),*decoder,{}};
      const auto picture=EncodeScaleController(*output.adaptive).size();
      output.media.width=uint32_t(picture.first);output.media.height=uint32_t(picture.second);
    }
    pixels+=uint64_t(width)*height;plan.outputs.push_back(std::move(output));
  }
  std::sort(plan.outputs.begin(),plan.outputs.end(),[](const auto& a,const auto& b){return a.media.id<b.media.id;});
  if((values->size()==2?request.get("monitor").string()!="all":request.get("monitor").string()!=std::to_string(plan.outputs.front().media.id))||
     (!sizesV2&&pixels>8294400)||!multitap::tracks::valid(plan.media(false)))throw std::runtime_error("invalid_track_request");
  // Divide the negotiated total, reserving a viable floor for each encoder.
  // The source sizes remain exact; bitrate allocation never scales an output.
  int remaining=plan.bitrate;uint64_t remainingPixels=pixels;
  for(size_t n=0;n<plan.outputs.size();++n){
    auto& output=plan.outputs[n];const auto area=uint64_t(output.media.sourceWidth)*output.media.sourceHeight;
    output.bitrate=n+1==plan.outputs.size()?remaining:std::clamp<int>(int(uint64_t(remaining)*area/remainingPixels),500000,remaining-500000);
    remaining-=output.bitrate;remainingPixels-=area;
    const auto rect=output.capture.bounds;
    if(n==0)plan.inputArea=rect;else{plan.inputArea.left=std::min(plan.inputArea.left,rect.left);plan.inputArea.top=std::min(plan.inputArea.top,rect.top);plan.inputArea.right=std::max(plan.inputArea.right,rect.right);plan.inputArea.bottom=std::max(plan.inputArea.bottom,rect.bottom);}
  }
  return plan;
}
inline std::string trackSourceInfo(const TrackSourcePlan& plan,bool audioAvailable){
  std::ostringstream out;out<<"{\"type\":\"stream.tracks.info\",\"sessionId\":"<<jsonQuote(plan.sessionId)<<",\"codec\":\"h264\",\"topologyEpoch\":"<<plan.topology<<",\"fpsTarget\":"<<plan.fps<<",\"bitrateTarget\":"<<plan.bitrate<<",\"tracks\":[";
  bool first=true;for(const auto& output:plan.outputs){const auto& t=output.media;if(!first)out<<',';first=false;
    out<<"{\"id\":"<<jsonQuote(std::to_string(t.id))<<",\"epoch\":"<<t.epoch<<",\"width\":"<<t.width<<",\"height\":"<<t.height<<",\"sourceWidth\":"<<t.sourceWidth<<",\"sourceHeight\":"<<t.sourceHeight<<",\"left\":"<<t.left<<",\"top\":"<<t.top<<",\"primary\":"<<(t.primary?"true":"false")<<",\"bitrateTarget\":"<<output.bitrate<<'}';
  }
  // The format actually sent: "opus" only where granted and its encoder started.
  const auto media=plan.media(audioAvailable);
  out<<"],\"audio\":{\"sampleRate\":48000,\"channels\":2,\"format\":\""<<(media.audioOpus?"opus":"s16le")<<"\",\"available\":"<<(media.audioEnabled?"true":"false")<<'}';
  if(!plan.resizeId.empty())out<<",\"requestId\":"<<jsonQuote(plan.requestId)<<",\"resizeId\":"<<jsonQuote(plan.resizeId);
  out<<'}';return out.str();
}
// cursor-tracks-v1: one cursor-v1 record in the TVT1 envelope of its topology
// (kind 3, flags 0, the shared connector 255, track epoch 0). Never a
// media-tracks-v1 video/audio packet: tracks::parse() still rejects it.
inline constexpr uint8_t trackCursorKind=3;
inline std::vector<uint8_t> trackCursorPacket(uint32_t topology,uint64_t pts,const std::vector<uint8_t>& record){
  namespace wire=multitap::tracks;
  if(!topology||pts>wire::maximumPts||record.size()<16||record.size()>wire::maximumPacketBytes-wire::headerBytes)throw std::runtime_error("Invalid track cursor record");
  std::vector<uint8_t> bytes(wire::headerBytes+record.size(),0);
  bytes[0]='T';bytes[1]='V';bytes[2]='T';bytes[3]='1';bytes[4]=trackCursorKind;bytes[6]=wire::audioTrack;
  wire::write32(bytes.data()+8,topology);wire::write64(bytes.data()+16,pts);wire::write32(bytes.data()+24,uint32_t(record.size()));
  std::copy(record.begin(),record.end(),bytes.begin()+wire::headerBytes);return bytes;
}
// The envelope the service forwards for a granted session (the relay checks
// the record itself).
inline bool trackCursorPacketValid(const uint8_t* bytes,size_t size){
  namespace wire=multitap::tracks;
  return bytes&&size>=wire::headerBytes+16&&size<=wire::maximumPacketBytes&&bytes[0]=='T'&&bytes[1]=='V'&&bytes[2]=='T'&&bytes[3]=='1'&&
    bytes[4]==trackCursorKind&&!bytes[5]&&bytes[6]==wire::audioTrack&&!bytes[7]&&wire::read32(bytes+8)&&!wire::read32(bytes+12)&&
    wire::read64(bytes+16)<=wire::maximumPts&&wire::read32(bytes+24)==size-wire::headerBytes;
}
// The service's check of one binary worker message of a track session: track
// media (video refreshes its watchdog) or, only when the session.start granted
// cursor-tracks-v1, a cursor record. Anything else ends the relay.
// opusGranted: audio-opus-tracks-v1, whose audio payloads have any length (a
// worker whose encoder did not start sends whole PCM frames, which pass too).
enum class TrackWorkerPacket{invalid,media,video,cursor};
inline TrackWorkerPacket trackWorkerPacket(const std::vector<uint8_t>& bytes,bool cursorGranted,bool opusGranted=false){
  multitap::tracks::Packet packet;
  if(multitap::tracks::parse(bytes.data(),bytes.size(),packet,opusGranted))return packet.kind==multitap::tracks::video?TrackWorkerPacket::video:TrackWorkerPacket::media;
  return cursorGranted&&trackCursorPacketValid(bytes.data(),bytes.size())?TrackWorkerPacket::cursor:TrackWorkerPacket::invalid;
}
struct TrackCapturedFrame {const std::vector<uint8_t>* nv12=nullptr;bool updated=false;double captureMs=0,convertMs=0;std::optional<double> readbackMs;};
class TrackCapture {
 public:virtual ~TrackCapture()=default;virtual TrackCapturedFrame frame()=0;
  // Composed BGRA of the last frame() for lossless tiles; null = not supported.
  virtual ComposedFrame* composed(){return nullptr;}
  // New-frame event for event-driven pacing (Capture::frameEvent); null = none.
  virtual HANDLE frameEvent(){return nullptr;}
  virtual std::optional<uint64_t> adapterLuid(){return {};}
  virtual void observeReadback(bool){}
  // cursor-tracks-v1: whether frame() composes the cursor, and the one
  // GetCursorInfo result the last frame() read (unknown when not read).
  virtual void composeCursor(bool){}
  virtual cursor::Observation cursorSample(){return {};}
  // "wgc" or "gdi" (empty: unknown); a follow retries a WGC capture that came back as GDI.
  virtual std::string_view backend(){return {};}
};
class WindowsTrackCapture final:public TrackCapture {
 public:
  explicit WindowsTrackCapture(const OwnedTrackSource& output):capture_(int(output.media.width),int(output.media.height),output.capture.bounds){}
  TrackCapturedFrame frame()override{const auto& data=capture_.frame();return {&data,capture_.frameUpdated(),capture_.lastCaptureMs(),capture_.lastConvertMs(),capture_.lastReadbackMs()};}
  ComposedFrame* composed()override{return &composed_;}
  HANDLE frameEvent()override{return capture_.frameEvent();}
  std::optional<uint64_t> adapterLuid()override{return capture_.adapterLuid();}
  void observeReadback(bool enabled)override{capture_.observeReadback(enabled);}
  void composeCursor(bool enabled)override{capture_.composeCursor(enabled);}
  cursor::Observation cursorSample()override{const auto& seen=capture_.cursorSample();return {seen.known,seen.flags,seen.handle,seen.position};}
  std::string_view backend()override{return capture_.backend();}
 private:Capture capture_;CapturedComposedFrame<Capture> composed_{capture_};
};
// One capture/encoder owner thread. Audio only accesses the packet gate under
// gateMutex_; all public callbacks happen after a complete bounded step returns.
// The same bounded topology monitor used by the single-output capture loop.
// Keep one synchronous ownership/geometry check before capture construction;
// later checks cannot stall a capture tick. Selections are immutable copies;
// injected monitor callbacks must synchronize any mutable fixture state.
inline std::unique_ptr<CaptureTopologyWatch> watchTrackSourceTopology(const TrackSourcePlan& plan,const std::function<std::vector<CaptureMonitor>()>& observed){
  auto check=[outputs=plan.outputs,observed]{const auto monitors=observed();for(const auto& output:outputs)validateCaptureSelection(output.capture,monitors);};
  check();return std::make_unique<CaptureTopologyWatch>(std::move(check),std::chrono::milliseconds(100));
}
// The thread of one output after the first: its capture and encoder are made,
// used and destroyed on it (a screen DC is released by the thread that got it;
// encoder sessions stay on one thread), in the loop thread's COM apartment and
// Media Foundation (Runtime). start/wait run one job; wait rethrows its failure.
class TrackOutputThread {
 public:
  TrackOutputThread():thread_([this]{loop();}){}
  ~TrackOutputThread(){{std::lock_guard lock(mutex_);stop_=true;}wake_.notify_all();thread_.join();}
  TrackOutputThread(const TrackOutputThread&)=delete;
  TrackOutputThread& operator=(const TrackOutputThread&)=delete;
  void start(std::function<void()> work){std::lock_guard lock(mutex_);if(busy_)throw std::runtime_error("track_output_busy");work_=std::move(work);busy_=true;error_=nullptr;wake_.notify_all();}
  void wait(){std::unique_lock lock(mutex_);done_.wait(lock,[this]{return !busy_;});if(error_){auto error=error_;error_=nullptr;std::rethrow_exception(error);}}
  void run(std::function<void()> work){start(std::move(work));wait();}
 private:
  void loop(){
    std::optional<Runtime> runtime;std::exception_ptr startup;try{runtime.emplace();}catch(...){startup=std::current_exception();}
    std::unique_lock lock(mutex_);
    for(;;){
      wake_.wait(lock,[this]{return stop_||work_;});if(stop_&&!work_)return;
      auto work=std::move(work_);work_=nullptr;lock.unlock();
      std::exception_ptr error=startup;if(!error)try{work();}catch(...){error=std::current_exception();}
      work=nullptr;lock.lock();error_=error;busy_=false;done_.notify_all();
    }
  }
  std::mutex mutex_;std::condition_variable wake_,done_;
  std::function<void()> work_;std::exception_ptr error_;bool busy_=false,stop_=false;
  std::thread thread_;
};
// Secure desktop on independent outputs (secure_desktop_notice.hpp). Without a branch for it in this pipeline
// the first capture tick under a UAC prompt would throw, the stream would end as capture_failed ("Host가 화면을
// 준비하지 못했어요") and its restore could not run under the prompt. So the stream stays:
// while the secure desktop of this unlocked session shows, every output codes a still picture (the notice, or
// what `still` supplies: the SYSTEM helper's frame of that output) on its own encoder, and when the desktop is
// back every capture is made again and each output goes on with an IDR. A lock and the notice limit still end the
// stream, as for one output.
struct TrackDesktop {
  std::function<bool()> available=[]{return interactiveDesktopAvailable();};  // any thread
  std::function<bool()> unlocked=[]{return ownSessionUnlocked();};            // any thread
};
// What stands in for one output's capture in a secure tick. nv12: the picture at the output's coded size (null:
// the notice). hold: code nothing for this output in this tick (its helper was asked for and has not answered).
// real: the picture is the real prompt (it can be answered, so the notice limit does not run). key: the picture
// changed between the notice and the prompt, so this output codes an IDR and no client blends the two.
struct TrackSecureStill {const std::vector<uint8_t>* nv12=nullptr;bool hold=false,real=false,key=false;};
struct TrackSecureHooks {
  std::function<TrackSecureStill(size_t,const OwnedTrackSource&,SecureDesktopNotice::Clock::time_point)> still;  // stepping thread
  std::function<void()> ended;            // the desktop returned, or the stream ends under it (stepping thread)
  std::function<void(uint32_t)> record;   // hostAction 200 stage 244
};
class TrackSourcePipeline {
 public:
  using CaptureFactory=std::function<std::unique_ptr<TrackCapture>(const OwnedTrackSource&)>;
  using Monitors=std::function<std::vector<CaptureMonitor>()>;
  using Micros=std::function<uint64_t()>;
  using Modes=std::function<std::optional<RECT>(const std::wstring&)>;  // a GDI device's current display mode rectangle
  using Followed=std::function<void(uint8_t,RECT)>;
  TrackSourcePipeline(TrackSourcePlan plan,bool audioAvailable,Micros now,Monitors observed=currentCaptureMonitors,
      CaptureFactory capture=[](const auto& output){return std::make_unique<WindowsTrackCapture>(output);},Modes modes=currentDisplayModeRect)
      :plan_(std::move(plan)),now_(std::move(now)),observed_(std::move(observed)),capture_(capture),modes_(std::move(modes)){
    if(!now_||!observed_||!capture||!modes_||!multitap::tracks::valid(plan_.media(audioAvailable)))throw std::runtime_error("invalid_track_request");
    std::array<StreamMemoryShape,2> memory{};size_t memoryCount=0;
    for(const auto& output:plan_.outputs){const auto& t=output.media;memory[memoryCount++]={t.sourceWidth,t.sourceHeight,t.width,t.height,t.width==t.sourceWidth&&t.height==t.sourceHeight};}
    validateStreamMemory({memory.data(),memoryCount});
    auto topology=watchTrackSourceTopology(plan_,observed_);
    if(gate_.configure(plan_.media(audioAvailable))!=multitap::tracks::Configured::changed)throw std::runtime_error("invalid_track_request");
    try{
      for(const auto& output:plan_.outputs){
        if(states_.empty()){states_.push_back(std::make_unique<State>(output,capture(output),plan_.fps));continue;}
        auto& thread=threads_.emplace_back(std::make_unique<TrackOutputThread>());std::unique_ptr<State> state;
        thread->run([&]{state=std::make_unique<State>(output,capture(output),plan_.fps);});states_.push_back(std::move(state));
      }
    }catch(...){release();throw;}
    topology_=std::move(topology);
  }
  ~TrackSourcePipeline(){if(secure_&&!resuming_)secureLeave();topology_.reset();release();}
  TrackSourcePipeline(const TrackSourcePipeline&)=delete;
  TrackSourcePipeline& operator=(const TrackSourcePipeline&)=delete;
  const TrackSourcePlan& plan()const{return plan_;}
  // Captured only after an actual output passes the track packet gate. Agent
  // sends this after media commits to the private pipe; stats are untouched.
  void enableOutputUsage(){for(size_t i=0;i<states_.size();++i){auto* state=states_[i].get();state->encoder.setOutputObserver([this,state,i](const EncoderOutputObservation& output){outputUsage_[i]=EncoderDiagnosticTrack{std::to_string(state->output.media.id),state->output.media.epoch,encoderDiagnostic(output.backend,output.hardware,output.softwareFallbackCode)};});}}
  void enableObservability(){observability_=true;for(auto& state:states_)state->capture->observeReadback(true);}
  std::optional<std::pair<uint64_t,uint64_t>> takeReadbackObservation(){return readback_.take();}
  std::vector<EncoderDiagnosticTrack> takeOutputUsage(){std::vector<EncoderDiagnosticTrack> result;for(auto& track:outputUsage_)if(track){result.push_back(std::move(*track));track.reset();}return result;}
  // Every output's new-frame event (outputs without one are omitted).
  std::vector<HANDLE> frameEvents()const{std::vector<HANDLE> events;for(const auto& state:states_)if(const HANDLE event=state->capture->frameEvent())events.push_back(event);return events;}
  // Whether the client wants tiles-v1 now (opted in, not paused by 게임 마우스);
  // read before every step. Wanted: every output whose capture exposes its
  // composed frame and was never followed gets a tile source from its next
  // tick on. Not wanted (or followed): the output's tiles retire with its next
  // video frame, as in Agent::stream (losslessStreamTick).
  void lossless(bool wanted){
    losslessWanted_=wanted;if(!wanted)return;
    for(auto& state:states_)if(!state->lossless&&state->tilesAllowed&&state->capture&&state->capture->composed()&&state->output.media.width==state->output.media.sourceWidth&&state->output.media.height==state->output.media.sourceHeight){
      const auto& t=state->output.media;
      state->lossless=std::make_unique<LosslessTileSource>(t.width,t.height,LosslessSettings::forBitrate(state->output.bitrate));state->helper=std::make_unique<ParallelWork>();
    }
  }
  // capture_follow.hpp: told (on the stepping thread) where an output's capture
  // now reads, after it followed an application's mode change.
  void onFollow(Followed followed){followed_=std::move(followed);}
  // A follow waiting for the outputs to settle gives up when this turns true.
  void stopWhen(const std::atomic<bool>& stop){stop_=&stop;}
  // Secure desktop: the observations (tests substitute them) and the owner's hooks. Call before the first step.
  void secureDesktop(TrackDesktop desktop){if(desktop.available&&desktop.unlocked)desktop_=std::move(desktop);}
  void onSecure(TrackSecureHooks hooks){secureHooks_=std::move(hooks);}
  // step() coded stills (or nothing) because the secure desktop shows; the caller then waits secureWait()
  // instead of pacing on the captures' frame events.
  bool secureActive()const{return secure_;}
  std::chrono::milliseconds secureWait()const{return secureWait_;}
  static constexpr std::chrono::milliseconds secureNoticePeriod{200};
  // After the desktop returned, the captures may need a moment (WGC loses its item under Winlogon).
  static constexpr std::chrono::seconds secureResumeGrace{3};
  // cursor-compose-v1 (a granted session): the client's request, read before every step.
  void composeRequested(bool on){if(cursor_)cursor_->composeRequested(on);}
  // cursor-tracks-v1 (a granted session.start): no output's video composes the
  // cursor from the next tick on; step() describes it in kind-3 records after
  // every output's first key frame. Call before the first step (idempotent).
  void enableCursor(cursor::Channel::Allocate allocate,cursor::Channel::Extract extractor=cursor::extract){
    if(cursor_)return;std::vector<RECT> areas;for(const auto& output:plan_.outputs)areas.push_back(output.capture.bounds);
    cursor_.emplace(areas,std::move(allocate),std::move(extractor));
    for(auto& state:states_)state->capture->composeCursor(false);
  }
  // One tick of every output. The outputs capture and encode at the same time:
  // the first on this thread, each other one on its own TrackOutputThread. In
  // one thread a tick cost the sum of both outputs' capture (WGC readback, NV12
  // conversion) and encode, so two outputs got at most half the frame budget
  // each. The packets keep the previous wire order (output by output, each in
  // its own gate order) and the same bound per tick.
  std::vector<std::vector<uint8_t>> step(){
    if(secure_)return secureStep();
    // The secure desktop also shows as a topology that cannot be read or a capture that cannot follow: never
    // the end of the stream while this unlocked session's desktop is away.
    const auto followOrSecure=[&](const std::string& why,std::array<bool,2> which){
      try{follow(why,which);return true;}catch(...){for(auto& state:states_)state->packets.clear();if(!secureNow())throw;}
      enterSecure();return false;
    };
    bool verified=true;std::string unverified;
    try{topology_->verify();}catch(const std::exception& error){verified=false;unverified=error.what();}
    if(!verified){if(secureNow()){enterSecure();return {};}if(!followOrSecure(unverified,{}))return {};}
    for(size_t n=1;n<states_.size();++n){auto* state=states_[n].get();threads_[n-1]->start([this,state]{tick(*state);});}
    std::exception_ptr failure;
    try{tick(*states_.front());}catch(...){failure=std::current_exception();}
    for(size_t n=1;n<states_.size();++n)try{threads_[n-1]->wait();}catch(...){if(!failure)failure=std::current_exception();}
    if(failure){for(auto& state:states_)state->packets.clear();std::rethrow_exception(failure);}
    // An output could not capture because the secure desktop shows: from the next step on every output codes
    // stills. What the other output captured in this tick still leaves.
    bool away=false;for(auto& state:states_)away|=std::exchange(state->secureLost,false);
    if(away){for(auto& state:states_)state->followReason.clear();enterSecure();return collect();}
    // An output whose capture saw another frame size (or a WGC error) follows
    // its output; the other outputs' packets of this step still leave.
    std::array<bool,2> changed{};std::string reason;
    for(size_t n=0;n<states_.size();++n)if(!states_[n]->followReason.empty()){changed[n]=true;reason=std::exchange(states_[n]->followReason,{});}
    if(!reason.empty()&&!followOrSecure(reason,changed))return {};
    auto packets=collect();
    // cursor-tracks-v1: the cursor the first output's frame() read, after this
    // tick's video (at most a shape and a state). An unrepresentable cursor is
    // composed from the next tick on by the output it is over.
    if(cursor_){
      if(keysReady())cursor_->keyFrame();
      const auto pts=now_();
      for(const auto& record:cursor_->observe(states_.front()->capture->cursorSample(),{},pts))packets.push_back(trackCursorPacket(plan_.topology,pts,record));
      for(auto& state:states_)state->capture->composeCursor(cursor_->composeFallback());
    }
    return packets;
  }
  // A legacy kind-2 packet (Loopback::run): PCM, or with audio-opus-tracks-v1
  // the u16 sequence + Opus packet; the gate holds it to the configured format.
  std::optional<std::vector<uint8_t>> audio(const std::vector<uint8_t>& legacy){
    if(legacy.size()<=12||legacy[0]!=2||legacy[1]||legacy[2]||legacy[3])throw std::runtime_error("invalid_track_audio");
    const auto pts=multitap::tracks::read64(legacy.data()+4);
    multitap::tracks::Packet packet{multitap::tracks::audio,0,multitap::tracks::audioTrack,plan_.topology,0,pts,legacy.data()+12,legacy.size()-12};
    std::lock_guard lock(gateMutex_);const auto decision=gate_.accept(packet);if(decision==multitap::tracks::Decision::awaitingAllTracks)return {};
    std::vector<uint8_t> bytes;if(decision!=multitap::tracks::Decision::accepted||!multitap::tracks::pack(packet,bytes,gate_.configuration().audioOpus))throw std::runtime_error("invalid_track_audio");return bytes;
  }
  bool keysReady()const{std::lock_guard lock(gateMutex_);return gate_.ready();}
  // tracks-resume-v1 (stream.keyframe on the track tunnel): every output codes
  // its next frame as an IDR. Call between step()s, on the stepping thread.
  void keyFrame(){for(auto& state:states_)state->keyFrame=true;}
  std::string stats(uint32_t milliseconds){
    if(!milliseconds||milliseconds>10000)throw std::runtime_error("invalid_track_stats");
    std::ostringstream out;out<<std::fixed<<std::setprecision(2)<<"{\"type\":\"stream.tracks.stats\",\"sessionId\":"<<jsonQuote(plan_.sessionId)<<",\"topologyEpoch\":"<<plan_.topology<<",\"measuredMs\":"<<milliseconds<<",\"tracks\":[";
    bool first=true;for(auto& state:states_){if(!first)out<<',';first=false;
      out<<"{\"id\":"<<jsonQuote(std::to_string(state->output.media.id))<<",\"epoch\":"<<state->output.media.epoch<<",\"capturedFps\":"<<double(state->captured)*1000/milliseconds<<",\"encodedFps\":"<<double(state->encoded)*1000/milliseconds<<",\"bitrateKbps\":"<<double(state->bytes)*8/milliseconds<<",\"encoderInfo\":"<<encoderDiagnosticJson(encoderDiagnostic(state->encoder.backendName(),state->encoder.hardwareInfo(),state->encoder.fallbackCode()))<<'}';
      state->captured=state->encoded=state->bytes=0;
    }out<<"]}";return out.str();
  }
 private:
  struct State {
    OwnedTrackSource output;std::unique_ptr<TrackCapture> capture;Encoder encoder;FrameEncodePolicy policy;
    std::unique_ptr<LosslessTileSource> lossless;std::unique_ptr<ParallelWork> helper;
    uint64_t captured=0,encoded=0,bytes=0,losslessBytes=0;
    // This tick's packets (in gate order) and readback time, taken by step().
    std::vector<std::vector<uint8_t>> packets;std::optional<double> readbackMs;
    // capture_follow.hpp: why this tick's capture must follow its output (empty:
    // it need not); tiles are never made again once it followed; the client was
    // told to drop every tile (losslessStreamTick); the next tick codes an IDR.
    std::string followReason;bool tilesAllowed=true,losslessRetired=false,keyFrame=false;
    // Secure desktop: this tick's capture failed because of it; this output's capture was made again after the
    // desktop returned; the notice at this output's coded size.
    bool secureLost=false,secureRebuilt=false;std::vector<uint8_t> noticeNv12;
    State(OwnedTrackSource source,std::unique_ptr<TrackCapture> input,int fps):output(std::move(source)),capture(std::move(input)),
      encoder(int(output.media.width),int(output.media.height),fps,output.bitrate,EncoderTuning{},EncoderBackend::automatic,capture?capture->adapterLuid():std::nullopt),policy(size_t(output.media.width)*output.media.height*3/2,FrameRefinement::forCbr(output.bitrate,fps)){if(!capture)throw std::runtime_error("capture_failed");}
  };
  // One output's capture and encode, on the thread that owns that output. Only
  // the gate is shared (under gateMutex_; its order rules are per output).
  void tick(State& state){
    // A frame of another size, or a WGC error (its item may close while the mode changes): step() follows.
    TrackCapturedFrame captured;
    try{captured=state.capture->frame();}
    catch(const CaptureGeometryChanged& error){state.followReason=error.what();return;}
    catch(const std::runtime_error& error){
      // A secure desktop of this unlocked session (a WGC error of the same switch counts): stills, not the end.
      if((std::string_view(error.what())==secureDesktopUnavailable||!desktop_.available())&&desktop_.unlocked()){state.secureLost=true;return;}
      if(std::string_view(error.what()).rfind("Windows capture failed (0x",0)!=0)throw;state.followReason=error.what();return;}
    if(!captured.nv12)throw std::runtime_error("capture_failed");if(captured.updated)state.captured++;
    state.readbackMs=captured.readbackMs;
    const auto pts=now_();
    const auto push=[&](uint8_t flags,uint64_t packetPts,const std::vector<uint8_t>& payload,bool video){pushPacket(state,flags,packetPts,payload,video);};
    const auto emit=[&](Frame frame){emitFrame(state,std::move(frame));};
    const bool keyFrame=std::exchange(state.keyFrame,false);if(keyFrame)state.encoder.requestKeyFrame();
    // Tile packet (pts T-1) strictly before this tick's video (pts T). No
    // longer wanted (paused, or the output followed): one packet drops every
    // tile with this tick's video, then plain video.
    std::optional<LosslessTick> tiled;std::vector<Frame> outputs;
    if(state.lossless&&state.capture->composed())
      tiled=losslessStreamTick(*state.lossless,state.losslessRetired,losslessWanted_&&state.tilesAllowed,*state.helper,*state.capture->composed(),state.policy,*captured.nv12,pts,
        [&]{state.encoder.encode(captured.nv12->data(),pts,[&](Frame frame){outputs.push_back(std::move(frame));});},keyFrame);
    if(tiled){
      if(tiled->payload)push(0,pts-1,*tiled->payload,false);
      for(auto& output:outputs)emit(std::move(output));
    }else{
      const auto decision=state.policy.prepare(*captured.nv12,pts,keyFrame);
      if(decision.submit)state.encoder.encode(captured.nv12->data(),pts,emit);
    }
  }
  // Gate order is wire order: the per-track pts must strictly increase.
  void pushPacket(State& state,uint8_t flags,uint64_t packetPts,const std::vector<uint8_t>& payload,bool video){
    const auto& t=state.output.media;multitap::tracks::Packet packet{multitap::tracks::video,flags,t.id,plan_.topology,t.epoch,packetPts,payload.data(),payload.size()};
    std::vector<uint8_t> bytes;{std::lock_guard lock(gateMutex_);const auto decision=gate_.accept(packet);if(decision==multitap::tracks::Decision::awaitingKey)return;if(decision!=multitap::tracks::Decision::accepted||!multitap::tracks::pack(packet,bytes))throw std::runtime_error("encoder_failed");}
    if(state.packets.size()>=8)throw std::runtime_error("track_output_backpressure");
    (video?state.bytes:state.losslessBytes)+=bytes.size();state.packets.push_back(std::move(bytes));
  }
  void emitFrame(State& state,Frame frame){
    if(!state.policy.output(frame.ptsUs,frame.key,now_(),frame.bytes.size()))throw std::runtime_error("encoder_failed");
    state.encoded++;pushPacket(state,uint8_t(frame.key?1:0),frame.ptsUs,frame.bytes,true);
  }
  // This step's packets of every output, in the previous wire order and bound.
  std::vector<std::vector<uint8_t>> collect(){
    std::vector<std::vector<uint8_t>> packets;size_t total=0;
    for(auto& state:states_){
      if(observability_)readback_.observe(std::exchange(state->readbackMs,std::nullopt));
      for(auto& bytes:state->packets){
        if(packets.size()>=8||total+bytes.size()>8*1024*1024)throw std::runtime_error("track_output_backpressure");
        total+=bytes.size();packets.push_back(std::move(bytes));
      }
      state->packets.clear();
    }
    return packets;
  }
  // ---- secure desktop (TrackDesktop above) ----
  bool secureNow()const{return !desktop_.available()&&desktop_.unlocked();}
  void secureRecord(uint32_t error){if(secureHooks_.record)try{secureHooks_.record(error);}catch(...){ }}
  void secureLeave(){if(secureHooks_.ended)try{secureHooks_.ended();}catch(...){ }}
  void enterSecure(){
    if(notice_.step(false,true,SecureDesktopNotice::Clock::now())!=SecureDesktopStep::notice)throw std::runtime_error(secureDesktopUnavailable);
    secure_=true;resuming_=false;noticeRecorded_=false;secureWait_=std::chrono::milliseconds(0);topology_.reset();
    // The picture changes: an IDR, so no client blends the two.
    for(auto& state:states_){state->keyFrame=true;state->secureLost=false;state->secureRebuilt=false;}
  }
  // One output's still, on the thread that owns its encoder: coded like an idle screen (the policy's refreshes,
  // an IDR on request); the first one drops every tile of the capture with its video.
  void still(State& state,const std::vector<uint8_t>& nv12){
    const auto pts=now_();
    const bool keyFrame=std::exchange(state.keyFrame,false);if(keyFrame)state.encoder.requestKeyFrame();
    std::vector<Frame> outputs;std::optional<LosslessTick> tiled;
    if(state.lossless)tiled=losslessRetireTick(*state.lossless,state.losslessRetired,[&](bool forced){return state.policy.prepare(nv12,pts,forced);},pts,
      [&]{state.encoder.encode(nv12.data(),pts,[&](Frame frame){outputs.push_back(std::move(frame));});},keyFrame);
    if(tiled){if(tiled->payload)pushPacket(state,0,pts-1,*tiled->payload,false);for(auto& output:outputs)emitFrame(state,std::move(output));}
    else{const auto decision=state.policy.prepare(nv12,pts,keyFrame);if(decision.submit)state.encoder.encode(nv12.data(),pts,[&](Frame frame){emitFrame(state,std::move(frame));});}
  }
  const std::vector<uint8_t>& noticeOf(State& state){
    const int width=int(state.output.media.width),height=int(state.output.media.height);const size_t bytes=size_t(width)*size_t(height)*3/2;
    if(state.noticeNv12.size()!=bytes){const auto bgra=secureDesktopNoticePicture(width,height);state.noticeNv12.assign(bytes,0);bgraToNv12(bgra.data(),state.noticeNv12.data(),width,height);}
    return state.noticeNv12;
  }
  // Every capture again, each on its own thread, with its first frame. false: not all of them yet.
  bool resumeCaptures(){
    bool all=true;
    for(size_t n=0;n<states_.size();++n){
      auto& state=*states_[n];if(state.secureRebuilt)continue;
      const auto rebuild=[&]{
        state.capture.reset();state.capture=capture_(state.output);if(!state.capture)throw std::runtime_error("capture_failed");
        if(cursor_)state.capture->composeCursor(cursor_->composeFallback());
        if(observability_)state.capture->observeReadback(true);
        (void)state.capture->frame();
      };
      try{if(n)threads_[n-1]->run(rebuild);else rebuild();state.secureRebuilt=true;}
      catch(const std::exception& error){all=false;resumeError_=error.what();}catch(...){all=false;resumeError_="capture_failed";}
    }
    if(!all)return false;
    for(auto& state:states_){state->secureRebuilt=false;state->keyFrame=true;state->followReason.clear();state->packets.clear();}
    // The outputs as they are now: a rectangle that moved under the prompt is followed like any other change.
    bool armed=true;std::string moved;
    try{topology_=watchTrackSourceTopology(plan_,observed_);}catch(const std::exception& error){armed=false;moved=error.what();}
    if(!armed)follow(moved,{});
    return true;
  }
  std::vector<std::vector<uint8_t>> secureStep(){
    const auto now=SecureDesktopNotice::Clock::now();
    if(!resuming_){
      const auto step=notice_.step(desktop_.available(),desktop_.unlocked(),now);
      if(step==SecureDesktopStep::end){secureRecord(notice_.endedRecord());secureLeave();throw std::runtime_error(secureDesktopUnavailable);}
      if(step!=SecureDesktopStep::notice){secureRecord(notice_.resumedRecord());secureLeave();resuming_=true;resumeUntil_=now+secureResumeGrace;resumeError_.clear();}
    }
    if(resuming_){
      if(resumeCaptures()){secure_=false;resuming_=false;secureWait_=std::chrono::milliseconds(0);return {};}
      // The desktop flipped again (a second prompt): stills again, as a new secure desktop.
      if(secureNow()){
        resuming_=false;noticeRecorded_=false;for(auto& state:states_){state->secureRebuilt=false;state->keyFrame=true;}
        if(notice_.step(false,true,now)!=SecureDesktopStep::notice)throw std::runtime_error(secureDesktopUnavailable);
      }
      else if(now>=resumeUntil_||!desktop_.unlocked())throw std::runtime_error(resumeError_.empty()?std::string("capture_failed"):resumeError_);
      else{secureWait_=std::chrono::milliseconds(50);return {};}  // nothing coded: the client keeps its picture
    }
    std::array<TrackSecureStill,2> stills{};bool real=false,hold=false;
    for(size_t n=0;n<states_.size();++n){
      auto& state=*states_[n];const auto& t=state.output.media;
      if(secureHooks_.still)try{stills[n]=secureHooks_.still(n,state.output,now);}catch(...){stills[n]={};}
      if(stills[n].key)state.keyFrame=true;
      if(stills[n].hold){hold=true;continue;}
      if(!stills[n].nv12||stills[n].nv12->size()!=size_t(t.width)*size_t(t.height)*3/2)stills[n]=TrackSecureStill{&noticeOf(state),false,false,false};
      real|=stills[n].real;
    }
    if(real)notice_.answerable(now);
    // Record 1 (the notice, no helper) once per secure desktop when nobody supplies pictures; an owner that
    // does records what its helpers did (5..8, or 1) itself.
    if(!secureHooks_.still&&!noticeRecorded_){noticeRecorded_=true;secureRecord(1);}
    for(size_t n=1;n<states_.size();++n)if(!stills[n].hold){auto* state=states_[n].get();const auto* nv12=stills[n].nv12;threads_[n-1]->start([this,state,nv12]{still(*state,*nv12);});}
    std::exception_ptr failure;
    if(!stills[0].hold)try{still(*states_.front(),*stills[0].nv12);}catch(...){failure=std::current_exception();}
    for(size_t n=1;n<states_.size();++n)if(!stills[n].hold)try{threads_[n-1]->wait();}catch(...){if(!failure)failure=std::current_exception();}
    if(failure){for(auto& state:states_)state->packets.clear();std::rethrow_exception(failure);}
    auto packets=collect();
    // cursor-tracks-v1: the helper's frame has the secure desktop's pointer composed in; the client is told
    // hidden so it does not draw its own over it. The first capture tick after the return reports it again.
    if(cursor_&&real){const auto pts=now_();for(const auto& record:cursor_->hide(pts))packets.push_back(trackCursorPacket(plan_.topology,pts,record));}
    secureWait_=real?std::chrono::milliseconds(0):hold?std::chrono::milliseconds(16):secureNoticePeriod;
    return packets;
  }
  // capture_follow.hpp for independent outputs: an application changed an
  // output's mode (a game's exclusive fullscreen at another resolution, or
  // back), and Windows may move the other output beside it. Every output must
  // settle as the same owned output within captureFollowSettle. Each one whose
  // rectangle changed, or whose capture saw another frame size (`changed`),
  // gets a new capture on its own thread on that rectangle, scaled into its
  // unchanged picture (the announced tracks stay); its next tick codes an IDR
  // and retires its tiles for good, as Agent::stream does. At most one rebuild
  // per captureFollowGap. Throws `reason`, as before, when the outputs do not
  // settle (another device, an overlapping display, no settled mode).
  void follow(const std::string& reason,std::array<bool,2> changed){
    topology_.reset();
    const auto deadline=Clock::now()+captureFollowSettle;
    std::array<bool,2> wgc{};for(size_t n=0;n<states_.size();++n)wgc[n]=states_[n]->capture&&states_[n]->capture->backend()!="gdi";
    const auto observe=[&]()->std::optional<std::vector<RECT>>{
      try{const auto monitors=observed_();std::vector<RECT> rects;
        for(const auto& state:states_){const auto& selected=state->output.capture.monitors.front();const auto rect=followedOwnedOutput(selected,monitors,modes_(selected.name));if(!rect)return std::nullopt;rects.push_back(*rect);}
        return rects;}catch(...){return std::nullopt;}};
    for(;;){
      const auto settled=settledOwnedOutputs(observe,[](std::chrono::milliseconds step){std::this_thread::sleep_for(step);},[]{return Clock::now();},deadline,*stop_);
      if(!settled)throw std::runtime_error(reason);
      bool moved=false;for(size_t n=0;n<states_.size();++n)moved|=changed[n]||!states_[n]->capture||!sameRect((*settled)[n],states_[n]->output.capture.bounds);
      if(moved&&followedAt_!=Clock::time_point{}&&Clock::now()<followedAt_+captureFollowGap){std::this_thread::sleep_until(std::min(followedAt_+captureFollowGap,deadline));continue;}
      bool rebuilt=true;
      for(size_t n=0;n<states_.size()&&rebuilt;++n){
        auto& state=*states_[n];const RECT rect=(*settled)[n];
        if(!changed[n]&&state.capture&&sameRect(rect,state.output.capture.bounds))continue;
        auto source=state.output;source.capture.bounds=rect;source.capture.monitors.front().bounds=rect;
        const auto rebuild=[&]{
          state.capture.reset();state.capture=capture_(source);if(!state.capture)throw std::runtime_error("capture_failed");
          if(cursor_)state.capture->composeCursor(cursor_->composeFallback());
          if(observability_)state.capture->observeReadback(true);
          // WGC created while Windows still changes the mode can fall back to GDI.
          if(wgc[n]&&state.capture->backend()=="gdi"&&Clock::now()+captureFollowStep<deadline)throw std::runtime_error("capture_backend_changed");
          (void)state.capture->frame();  // the new mode's first frame; throws while it still changes
        };
        try{if(n)threads_[n-1]->run(rebuild);else rebuild();}
        catch(const std::exception&){if(Clock::now()+captureFollowStep>=deadline)throw std::runtime_error(reason);rebuilt=false;continue;}
        state.output.capture=source.capture;plan_.outputs[n].capture=source.capture;changed[n]=false;
        state.tilesAllowed=false;state.keyFrame=true;
        if(cursor_)cursor_->moveOutput(n,rect);
        if(followed_)followed_(state.output.media.id,rect);
      }
      if(!rebuilt){std::this_thread::sleep_for(captureFollowStep);continue;}
      if(moved)followedAt_=Clock::now();
      // A transient unreadable topology that kept every rectangle only re-arms the check.
      try{topology_=watchTrackSourceTopology(plan_,observed_);return;}
      catch(const std::exception&){if(Clock::now()+captureFollowStep>=deadline)throw std::runtime_error(reason);std::this_thread::sleep_for(captureFollowStep);}
    }
  }
  // Every later output's state goes on its own thread, then the threads join.
  void release()noexcept{
    for(size_t n=states_.size();n-->1;)if(n-1<threads_.size())try{threads_[n-1]->run([&]{states_[n].reset();});}catch(...){ }
    threads_.clear();states_.clear();
  }
  TrackSourcePlan plan_;Micros now_;Monitors observed_;CaptureFactory capture_;Modes modes_;std::vector<std::unique_ptr<State>> states_;
  Followed followed_;inline static const std::atomic<bool> never_{false};const std::atomic<bool>* stop_=&never_;
  bool losslessWanted_=false;Clock::time_point followedAt_{};
  std::vector<std::unique_ptr<TrackOutputThread>> threads_;  // threads_[n-1] owns states_[n]
  bool observability_=false;ReadbackObservation readback_;
  // Secure desktop: stills are coded (secure_); the desktop returned and the captures are being made again.
  TrackDesktop desktop_;TrackSecureHooks secureHooks_;SecureDesktopNotice notice_;
  bool secure_=false,resuming_=false,noticeRecorded_=false;std::chrono::milliseconds secureWait_{0};
  SecureDesktopNotice::Clock::time_point resumeUntil_{};std::string resumeError_;
  std::array<std::optional<EncoderDiagnosticTrack>,2> outputUsage_;
  std::optional<cursor::Channel> cursor_;
  mutable std::mutex gateMutex_;multitap::tracks::Gate gate_;
  // Destroy/join the callback before its owner's other members and captures.
  std::unique_ptr<CaptureTopologyWatch> topology_;
};
}
