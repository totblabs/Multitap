// Secure desktop on independent outputs (track_source.hpp), with synthetic captures and substituted
// desktop observations: the actual Agent worker thread, track pipeline and encoders. Without that branch the first
// capture tick under a UAC prompt would end the stream as capture_failed.
#include "agent.hpp"
#include "x264_encoder_testing.hpp"
#include <iostream>
#include <sstream>
namespace {
unsigned checks=0;
void require(bool ok,const char* reason){++checks;if(!ok)throw std::runtime_error(reason);}
struct Desk {
  std::atomic<bool> available{true},unlocked{true},wgc{false};
  std::atomic<int> failCreate{0};
  std::atomic<unsigned> captures{0},destroyed{0};
};
struct Screen final:host::TrackCapture {
  Desk& desk;std::vector<uint8_t> data;unsigned index=0;
  Screen(Desk& values,const host::OwnedTrackSource& source):desk(values),data(size_t(source.media.width)*source.media.height*3/2,128){
    if(desk.failCreate>0){--desk.failCreate;throw std::runtime_error("No initial virtual display frame");}
    desk.captures++;
  }
  ~Screen(){desk.destroyed++;}
  host::TrackCapturedFrame frame()override{
    if(!desk.available)throw std::runtime_error(desk.wgc?"Windows capture failed (0x80070005)":host::secureDesktopUnavailable);
    data[0]=uint8_t(40+index++%160);return {&data,true,0,0};
  }
  host::cursor::Observation cursorSample()override{return {true,CURSOR_SHOWING,LoadCursorW(nullptr,IDC_ARROW),POINT{-1300,400}};}
};
host::Json command(const std::string& id){
  return host::JsonParser::parse("{\"type\":\"session.start\",\"sessionId\":"+host::jsonQuote(id)+",\"mode\":\"stream\",\"mediaProtocol\":\"tracks-v1\",\"topologyEpoch\":1,\"monitor\":\"all\",\"maxWidth\":1280,\"maxHeight\":720,\"fps\":30,\"bitrate\":8000000,\"audio\":false,\"tracks\":[{\"id\":\"0\",\"width\":640,\"height\":360,\"x\":-640,\"y\":-100,\"epoch\":1},{\"id\":\"1\",\"width\":800,\"height\":600,\"x\":0,\"y\":0,\"epoch\":1}]}");
}
host::DisplayState display(){
  host::DisplayState value;value.installed=value.deviceReady=value.owned=value.active=true;value.foreign=0;value.width=1440;value.height=700;value.left=-1600;value.top=300;value.name=L"\\\\.\\DISPLAY2";
  value.monitors={{"0",L"\\\\.\\DISPLAY2",-1600,300,640,360,false},{"1",L"\\\\.\\DISPLAY3",-960,400,800,600,false}};return value;
}
std::vector<host::CaptureMonitor> observed(){return {{L"\\\\.\\DISPLAY2",L"owned0",{-1600,300,-960,660},false},{L"\\\\.\\DISPLAY3",L"owned1",{-960,400,-160,1000},false}};}
struct CpuEncoders{
  CpuEncoders(){host::x264testing::configurationProvider.store([]{return host::EncoderBackend::x264;});}
  ~CpuEncoders(){host::x264testing::configurationProvider.store(nullptr);}
};
// What the peer of one Agent saw.
struct Peer {
  std::mutex mutex;std::condition_variable arrived;
  std::array<unsigned,2> video{};std::array<unsigned,2> keys{};std::vector<std::string> ended;std::vector<std::pair<uint32_t,uint32_t>> records;
  void packet(bool text,const std::vector<uint8_t>& bytes){
    std::lock_guard lock(mutex);
    if(text){const auto value=host::JsonParser::parse(std::string(bytes.begin(),bytes.end()));if(value.get("type").string()=="stream.unavailable")ended.push_back(value.get("reason").string());}
    else{multitap::tracks::Packet packet;if(multitap::tracks::parse(bytes.data(),bytes.size(),packet)&&packet.kind==multitap::tracks::video&&packet.track<2){video[packet.track]++;if(packet.flags&1)keys[packet.track]++;}}
    arrived.notify_all();
  }
  void reset(){std::lock_guard lock(mutex);video={};keys={};}
  template<class Done> bool wait(Done done,int seconds=30){std::unique_lock lock(mutex);return arrived.wait_for(lock,std::chrono::seconds(seconds),done);}
  unsigned count(uint32_t stage,uint32_t low){std::lock_guard lock(mutex);unsigned n=0;for(const auto& [s,e]:records)if(s==stage&&(e&0xffu)==low)++n;return n;}
};
struct Fixture {
  Desk desk;Peer peer;host::Agent agent;
  Fixture():agent([](const std::wstring&){},[this](bool text,std::vector<uint8_t> bytes){peer.packet(text,bytes);},[](INPUT&)->UINT{return 1;},observed,
      [this](const host::OwnedTrackSource& output)->std::unique_ptr<host::TrackCapture>{return std::make_unique<Screen>(desk,output);},[]{return RECT{-1920,-200,1920,1080};}){
    agent.trackTestDesktop(host::TrackDesktop{[this]{return desk.available.load();},[this]{return desk.unlocked.load();}});
    agent.captureDiagnostic([this](uint32_t stage,uint32_t error){std::lock_guard lock(peer.mutex);peer.records.emplace_back(stage,error);peer.arrived.notify_all();});
  }
  void start(const char* id){auto request=command(id);agent.localTrackStart(request,host::bindOwnedTrackSources(request,display(),observed()));}
  bool flowing(unsigned each=2){return peer.wait([&]{return peer.video[0]>=each&&peer.video[1]>=each;});}
  // Both outputs coded an IDR since the last reset, and video went on after it.
  bool keyed(){return peer.wait([&]{return peer.keys[0]>=1&&peer.keys[1]>=1;});}
  bool quiet(){std::lock_guard lock(peer.mutex);return peer.ended.empty();}
};
// A prompt appears, closes, appears again (as a capture error this time), and the session is locked under it.
void survivesAndResumes(){
  Fixture f;f.start("12345678-1234-4234-8234-00000000e001");
  require(f.flowing(),"The stream did not start");
  f.peer.reset();f.desk.available=false;
  require(f.keyed(),"Both outputs must code the notice (an IDR each) when the secure desktop shows");
  require(f.quiet(),"The secure desktop ended a stream of independent outputs");
  // This worker asks for a helper per output; the synthetic outputs are no DXGI outputs, so each one answers
  // with 6 (no helper) and shows the notice. A worker that may not ask writes 1 (secure_desktop_tracks_test).
  require(f.peer.wait([&]{return f.peer.records.size()>=2;})&&f.peer.count(host::secureDesktopNoticeStage,host::secure::record244HelperLaunchFail)==2&&f.peer.count(host::secureDesktopNoticeStage,1)==0,"Each output must record why it shows the notice");
  // The notice is still: refreshes keep the service's video watchdog (15 s) fed.
  f.peer.reset();require(f.peer.wait([&]{return f.peer.video[0]>=2&&f.peer.video[1]>=2;},10),"The notice must keep sending video on both outputs");
  require(f.desk.captures==2,"No capture is made while the secure desktop shows");
  f.peer.reset();f.desk.available=true;
  require(f.keyed()&&f.flowing(5),"Both outputs must go on with an IDR when the desktop returns");
  require(f.quiet()&&f.desk.captures==4&&f.desk.destroyed==2,"The captures must be made again exactly once when the desktop returns");
  require(f.peer.count(host::secureDesktopNoticeStage,2)==1,"Record 244/2 must be written once when the desktop returns");
  // A second prompt, seen as a WGC error instead of the fixed text.
  f.peer.reset();f.desk.wgc=true;f.desk.available=false;
  require(f.keyed()&&f.quiet(),"A capture error under a second prompt must show the notice again");
  require(f.peer.count(host::secureDesktopNoticeStage,host::secure::record244HelperLaunchFail)==4,"The second notice must be recorded for each output");
  // Win+L under the prompt: the stream ends as for one output.
  f.desk.unlocked=false;
  require(f.peer.wait([&]{return !f.peer.ended.empty();}),"A lock under the notice must end the stream");
  {std::lock_guard lock(f.peer.mutex);require(f.peer.ended.size()==1&&f.peer.ended.front()=="no_interactive_session","A lock must end as no_interactive_session, not capture_failed");}
  require(f.peer.count(host::secureDesktopNoticeStage,3)==1,"Record 244/3 must be written for the lock");
  f.agent.suspendStreaming();f.agent.stop();
  require(f.desk.destroyed==f.desk.captures,"A capture outlived its stream");
}
// The desktop is back but the captures need a moment; then they never come back.
void resumeGrace(){
  Fixture f;f.start("12345678-1234-4234-8234-00000000e002");
  require(f.flowing(),"The stream did not start");
  f.peer.reset();f.desk.available=false;require(f.keyed(),"The notice did not show");
  f.peer.reset();f.desk.failCreate=3;f.desk.available=true;
  require(f.keyed()&&f.flowing(5)&&f.quiet(),"A capture that needs a moment after the desktop returned must not end the stream");
  require(f.peer.count(host::secureDesktopNoticeStage,2)==1,"The return must be recorded once however many attempts the captures needed");
  f.peer.reset();f.desk.available=false;require(f.keyed(),"The second notice did not show");
  f.desk.failCreate=100000;f.desk.available=true;
  const auto began=host::Clock::now();
  require(f.peer.wait([&]{return !f.peer.ended.empty();},20),"Captures that never come back must end the stream");
  const auto took=std::chrono::duration_cast<std::chrono::milliseconds>(host::Clock::now()-began).count();
  {std::lock_guard lock(f.peer.mutex);require(f.peer.ended.front()=="capture_failed","A capture failure after the desktop returned keeps its own reason");}
  require(took>=2500&&took<10000,"The captures get the grace period, and only that");
  f.agent.suspendStreaming();f.agent.stop();
}
// Locked (not a prompt): no notice, the end the stream always had, with the reason the client explains.
void lockedEnds(){
  Fixture f;f.start("12345678-1234-4234-8234-00000000e003");
  require(f.flowing(),"The stream did not start");
  f.desk.unlocked=false;f.desk.available=false;
  require(f.peer.wait([&]{return !f.peer.ended.empty();}),"A locked session must end the stream");
  {std::lock_guard lock(f.peer.mutex);require(f.peer.ended.front()=="no_interactive_session","A locked session must end as no_interactive_session");}
  require(f.peer.count(host::secureDesktopNoticeStage,1)==0,"A locked session shows no notice");
  f.agent.suspendStreaming();f.agent.stop();
}
// The pipeline itself with an owner that supplies pictures (the SYSTEM helper's frames of each output).
void ownerPictures(){
  Desk desk;const auto request=command("12345678-1234-4234-8234-00000000e004");
  auto plan=host::bindOwnedTrackSources(request,display(),observed());
  const auto beginning=host::Clock::now();
  host::Runtime runtime;
  host::TrackSourcePipeline pipeline(plan,false,[beginning]{return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(host::Clock::now()-beginning).count());},observed,
    [&](const host::OwnedTrackSource& output)->std::unique_ptr<host::TrackCapture>{return std::make_unique<Screen>(desk,output);});
  pipeline.secureDesktop(host::TrackDesktop{[&]{return desk.available.load();},[&]{return desk.unlocked.load();}});
  uint32_t shape=0;pipeline.enableCursor([&]{return ++shape;});
  std::array<std::vector<uint8_t>,2> pictures;std::array<int,2> mode{};  // 0 notice, 1 real, 2 hold, 3 wrong size, 4 throws
  unsigned endedCalls=0,asked=0;std::vector<uint32_t> records;
  host::TrackSecureHooks hooks;
  hooks.still=[&](size_t n,const host::OwnedTrackSource& output,host::SecureDesktopNotice::Clock::time_point)->host::TrackSecureStill{
    ++asked;const size_t bytes=size_t(output.media.width)*output.media.height*3/2;
    if(mode[n]==4)throw std::runtime_error("helper");
    if(mode[n]==2)return {nullptr,true,false};
    if(mode[n]==0)return {};
    pictures[n].assign(mode[n]==3?bytes-1:bytes,uint8_t(200+n));return {&pictures[n],false,true};
  };
  hooks.ended=[&]{++endedCalls;};hooks.record=[&](uint32_t error){records.push_back(error);};
  pipeline.onSecure(std::move(hooks));
  struct Seen{std::array<unsigned,2> video{},keys{};unsigned cursor=0;};
  const auto run=[&](unsigned steps,int sleepMs=5){Seen seen;for(unsigned i=0;i<steps;++i){for(const auto& bytes:pipeline.step()){multitap::tracks::Packet packet;
      if(multitap::tracks::parse(bytes.data(),bytes.size(),packet)){if(packet.kind==multitap::tracks::video&&packet.track<2){seen.video[packet.track]++;if(packet.flags&1)seen.keys[packet.track]++;}}
      else if(host::trackCursorPacketValid(bytes.data(),bytes.size()))seen.cursor++;}
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));}return seen;};
  const auto first=run(40);
  require(pipeline.keysReady()&&first.keys[0]==1&&first.keys[1]==1&&first.cursor>=2&&!pipeline.secureActive(),"The pipeline did not start with both key frames and the cursor");
  // The prompt: output 0 has the real picture, output 1 is still waiting for its helper.
  desk.available=false;mode={0,2};
  (void)run(1);require(pipeline.secureActive(),"A capture that cannot read the secure desktop must switch to stills");
  const auto held=run(3);
  require(held.keys[0]==1&&held.video[1]==0,"A held output codes nothing; the other one codes the notice with an IDR");
  require(pipeline.secureWait()==std::chrono::milliseconds(16)&&held.cursor==0,"A held output is asked again soon, and the notice hides no pointer");
  mode={1,2};const auto mixed=run(3);
  require(mixed.video[0]>=1&&mixed.video[1]==0,"The real picture replaces the notice on its output only");
  mode={1,1};const auto both=run(30,20);
  require(both.keys[1]==1&&both.keys[0]==0,"The held output codes its picture with an IDR once it has one");
  require(pipeline.secureWait()==std::chrono::milliseconds(0),"Real pictures are followed without a pause");
  require(mixed.cursor+both.cursor==1,"The client's pointer must be told hidden once while the real prompt shows");
  require(records.empty(),"An owner that supplies pictures writes its own records");
  // A picture of the wrong size and a hook that throws are the notice, never the end.
  mode={3,4};const auto fallback=run(3);
  require(pipeline.secureActive()&&fallback.video[0]>=1&&fallback.video[1]>=1,"A bad picture must become the notice");
  require(pipeline.secureWait()==host::TrackSourcePipeline::secureNoticePeriod,"The notice is refreshed at its own period");
  require(endedCalls==0&&desk.captures==2,"Nothing ends and no capture is made while the secure desktop shows");
  // The desktop returns.
  desk.available=true;const unsigned askedBefore=asked;
  const auto back=run(40);
  require(!pipeline.secureActive()&&endedCalls==1&&asked==askedBefore,"The owner is told once that the desktop returned and is not asked again");
  require(back.keys[0]==1&&back.keys[1]==1&&back.video[0]>5&&back.video[1]>5,"Both outputs go on with one IDR each");
  require(back.cursor>=1,"The real cursor is reported again after the return");
  require(records.size()==1&&(records[0]&0xffu)==2,"The return is recorded");
  require(desk.captures==4&&desk.destroyed==2,"Each capture was made again once");
  // A pipeline destroyed under the prompt tells its owner, so no helper stays.
  desk.available=false;mode={1,1};(void)run(3);
  require(pipeline.secureActive()&&endedCalls==1,"The second prompt did not show");
}
void ownerToldAtEnd(){
  Desk desk;const auto request=command("12345678-1234-4234-8234-00000000e005");unsigned endedCalls=0;
  {
    host::Runtime runtime;const auto beginning=host::Clock::now();
    host::TrackSourcePipeline pipeline(host::bindOwnedTrackSources(request,display(),observed()),false,[beginning]{return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(host::Clock::now()-beginning).count());},observed,
      [&](const host::OwnedTrackSource& output)->std::unique_ptr<host::TrackCapture>{return std::make_unique<Screen>(desk,output);});
    pipeline.secureDesktop(host::TrackDesktop{[&]{return desk.available.load();},[&]{return desk.unlocked.load();}});
    host::TrackSecureHooks hooks;hooks.ended=[&]{++endedCalls;};pipeline.onSecure(std::move(hooks));
    for(int i=0;i<5;++i)(void)pipeline.step();
    desk.available=false;(void)pipeline.step();(void)pipeline.step();
    require(pipeline.secureActive()&&endedCalls==0,"The prompt did not show");
  }
  require(endedCalls==1,"A pipeline that ends under the prompt must tell its owner");
  require(desk.destroyed==desk.captures,"A capture outlived its pipeline");
}
}
int main(){
  try{
    CpuEncoders cpu;
    survivesAndResumes();resumeGrace();lockedEnds();ownerPictures();ownerToldAtEnd();
    std::cout<<"track secure desktop: ok ("<<checks<<" checks)\n";return 0;
  }catch(const std::exception& error){std::cerr<<"track secure desktop: FAILED: "<<error.what()<<'\n';return 1;}
}
