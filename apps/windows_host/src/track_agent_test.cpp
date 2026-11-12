#include "agent.hpp"
#include "remote_track_decoder.h"
#include "x264_encoder_testing.hpp"
#include "../../windows_client/test/native/track_gpu_fixture.hpp"
#include <iostream>
#include <sstream>
#include <psapi.h>
#include <tlhelp32.h>
namespace {
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
struct Observations {
  std::atomic<unsigned> captures=0,destroyed=0,comAliveOnDestroy=0,pressed=0,released=0,pointerPressed=0,pointerReleased=0,pointerMismatch=0;
};
struct Pixels final:host::TrackCapture {
  Observations& observations;std::vector<uint8_t> data;unsigned index=0;bool moving=false;
  Pixels(Observations& values,const host::OwnedTrackSource& source,bool animate):observations(values),data(size_t(source.media.width)*source.media.height*3/2,128),moving(animate){observations.captures++;}
  ~Pixels(){APTTYPE type{};APTTYPEQUALIFIER qualifier{};if(SUCCEEDED(CoGetApartmentType(&type,&qualifier)))observations.comAliveOnDestroy++;observations.destroyed++;}
  host::TrackCapturedFrame frame()override{if(moving)data[0]=uint8_t(40+index++%160);return {&data,true,0,0};}
};
host::Json command(const std::string& id){
  return host::JsonParser::parse("{\"type\":\"session.start\",\"sessionId\":"+host::jsonQuote(id)+",\"mode\":\"stream\",\"mediaProtocol\":\"tracks-v1\",\"topologyEpoch\":1,\"monitor\":\"all\",\"maxWidth\":1280,\"maxHeight\":720,\"fps\":30,\"bitrate\":8000000,\"audio\":false,\"tracks\":[{\"id\":\"0\",\"width\":640,\"height\":360,\"x\":-640,\"y\":-100,\"epoch\":1},{\"id\":\"1\",\"width\":800,\"height\":600,\"x\":0,\"y\":0,\"epoch\":1}]}");
}
host::DisplayState display(){
  host::DisplayState value;value.installed=value.deviceReady=value.owned=value.active=true;value.foreign=0;value.width=1440;value.height=700;value.left=-1600;value.top=300;value.name=L"\\\\.\\DISPLAY2";
  value.monitors={{"0",L"\\\\.\\DISPLAY2",-1600,300,640,360,false},{"1",L"\\\\.\\DISPLAY3",-960,400,800,600,false}};return value;
}
std::vector<host::CaptureMonitor> observed(){return {{L"\\\\.\\DISPLAY2",L"owned0",{-1600,300,-960,660},false},{L"\\\\.\\DISPLAY3",L"owned1",{-960,400,-160,1000},false}};}
unsigned threadCount(){
  const HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);if(snapshot==INVALID_HANDLE_VALUE)return 0;
  THREADENTRY32 entry{};entry.dwSize=sizeof(entry);unsigned count=0;const DWORD self=GetCurrentProcessId();
  if(Thread32First(snapshot,&entry))do{if(entry.th32OwnerProcessID==self)++count;}while(Thread32Next(snapshot,&entry));
  CloseHandle(snapshot);return count;
}
DWORD handleCount(){DWORD count=0;GetProcessHandleCount(GetCurrentProcess(),&count);return count;}
size_t privateBytes(){PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters));return counters.PrivateUsage;}
// Live allocations in every process heap (the CRT allocates from the process
// heap). Private bytes alone also count committed free heap space, which the
// heap keeps after large frees: it rose to a ~40 MB plateau over 1000 sessions
// (and fell back) while the live block count stayed flat.
struct LiveHeap{size_t blocks=0,bytes=0;};
LiveHeap liveHeap(){
  LiveHeap value;std::array<HANDLE,256> heaps{};const DWORD count=GetProcessHeaps(DWORD(heaps.size()),heaps.data());
  for(DWORD i=0;i<count&&i<heaps.size();++i){if(!HeapLock(heaps[i]))continue;PROCESS_HEAP_ENTRY entry{};while(HeapWalk(heaps[i],&entry))if(entry.wFlags&PROCESS_HEAP_ENTRY_BUSY){++value.blocks;value.bytes+=entry.cbData;}HeapUnlock(heaps[i]);}
  return value;
}
// The soak measures this process's own session lifecycle, so its encoders are
// the CPU ones (the test-only provider; it also forbids GPU opening). With the
// automatic backend a PC with an NVIDIA GPU opens NVENC per output: every
// session then creates and releases a D3D11 device on that adapter, which loads
// and unloads NVIDIA's user-mode driver stack (nvldumdx, nvwgf2umx, NvContainer
// MessageBus, NvCamera), and the driver leaves ~60 heap blocks (~4 KB, protobuf
// descriptors and similar) behind per cycle: +7,100 blocks over 60 sessions on a
// GRID P40 (driver 31.0.15.3941), +120 with one device held for the whole soak.
// Those blocks are the driver's, not this code's, and vary by machine.
struct CpuEncoders{
  CpuEncoders(){host::x264testing::configurationProvider.store([]{return host::EncoderBackend::x264;});}
  ~CpuEncoders(){host::x264testing::configurationProvider.store(nullptr);}
};
std::string keyboard(const std::string& id){return "{\"type\":\"input\",\"sessionId\":"+host::jsonQuote(id)+",\"topologyEpoch\":1,\"event\":\"key\",\"vk\":91,\"scan\":0,\"extended\":false,\"down\":true}";}
std::string pointer(const std::string& id){return "{\"type\":\"input\",\"sessionId\":"+host::jsonQuote(id)+",\"topologyEpoch\":1,\"event\":\"button\",\"button\":\"left\",\"down\":true,\"track\":\"0\",\"epoch\":1,\"x\":0.25,\"y\":0.75}";}
// Synthetic soak of a long-running worker: many session start/stop cycles of
// the actual Agent worker thread, track pipeline and CPU encoders (synthetic
// owned-output captures). After a warm-up the process's handles, threads and
// private memory must stay on a plateau; anything left behind per session
// grows with the cycle count and fails here.
std::string soak(){
  CpuEncoders cpuEncoders;
  std::mutex peer;std::condition_variable arrived;std::array<unsigned,2> video{};Observations observations;
  host::Agent agent([](const std::wstring&){},
    [&](bool text,std::vector<uint8_t> bytes){
      if(text)return;multitap::tracks::Packet packet;
      if(!multitap::tracks::parse(bytes.data(),bytes.size(),packet)||packet.kind!=multitap::tracks::video||packet.track>1)return;
      std::lock_guard lock(peer);video[packet.track]++;arrived.notify_all();
    },
    [](INPUT&)->UINT{return 1;},observed,
    [&](const host::OwnedTrackSource& output)->std::unique_ptr<host::TrackCapture>{return std::make_unique<Pixels>(observations,output,true);},
    []{return RECT{-1920,-200,1920,1080};});
  const auto cycle=[&](unsigned n){
    char id[48]{};sprintf_s(id,"12345678-1234-4234-8234-%012x",0xa000u+n);
    {std::lock_guard lock(peer);video={};}
    auto start=command(id);agent.localTrackStart(start,host::bindOwnedTrackSources(start,display(),observed()));
    bool flowing=false;{std::unique_lock lock(peer);flowing=arrived.wait_for(lock,std::chrono::seconds(60),[&]{return video[0]>=2&&video[1]>=2;});}
    agent.suspendStreaming();require(flowing,"Soak session did not deliver video on both outputs");
  };
  constexpr unsigned warmup=5,cycles=60;
  for(unsigned n=0;n<warmup;++n)cycle(n);  // codec DLLs, Media Foundation work queues, allocator pools
  const auto handles=handleCount();const auto threads=threadCount();const auto memory=privateBytes();const auto heap=liveHeap();
  const auto began=host::Clock::now();
  for(unsigned n=warmup;n<warmup+cycles;++n)cycle(n);
  const double seconds=std::chrono::duration<double>(host::Clock::now()-began).count();
  const auto handlesAfter=handleCount();const auto threadsAfter=threadCount();const auto memoryAfter=privateBytes();const auto heapAfter=liveHeap();
  // One long session: per-tick state (encode policy, refinement, packet gate,
  // stats) must not accumulate while it runs (~150 changed frames per output).
  LiveHeap longEarly,longLate;
  {
    {std::lock_guard lock(peer);video={};}
    auto start=command("12345678-1234-4234-8234-00000000b000");agent.localTrackStart(start,host::bindOwnedTrackSources(start,display(),observed()));
    const auto delivered=[&](unsigned count){std::unique_lock lock(peer);return arrived.wait_for(lock,std::chrono::seconds(60),[&]{return video[0]>=count&&video[1]>=count;});};
    const bool started=delivered(30);if(started)longEarly=liveHeap();
    const bool continued=started&&delivered(180);if(continued)longLate=liveHeap();
    agent.suspendStreaming();require(started&&continued,"Long soak session stopped delivering video");
  }
  agent.stop();
  require(observations.captures==2*(warmup+cycles+1)&&observations.destroyed==observations.captures&&observations.comAliveOnDestroy==observations.captures,"Soak sessions leaked or outlived their captures");
  std::ostringstream out;
  out<<"{\"kind\":\"track-agent-soak\",\"sessions\":"<<cycles<<",\"seconds\":"<<seconds<<",\"handles\":["<<handles<<','<<handlesAfter<<"],\"threads\":["<<threads<<','<<threadsAfter<<"],\"liveHeapBlocks\":["<<heap.blocks<<','<<heapAfter.blocks<<"],\"liveHeapBytes\":["<<heap.bytes<<','<<heapAfter.bytes<<"],\"privateBytes\":["<<memory<<','<<memoryAfter<<"],\"longSessionHeapBlocks\":["<<longEarly.blocks<<','<<longLate.blocks<<"],\"actualEncoders\":true,\"encoders\":\"cpu\",\"actualWindowsCapture\":false}";
  std::cout<<out.str()<<'\n';
  // One handle, thread or heap allocation left per session exceeds these bounds
  // (60 sessions); private bytes only has to stay below a gross ceiling.
  require(handlesAfter<=handles+24,"Soak: handles grew with the number of sessions");
  require(threadsAfter<=threads+4,"Soak: threads grew with the number of sessions");
  require(heapAfter.blocks<=heap.blocks+32&&heapAfter.bytes<=heap.bytes+size_t(4)*1024*1024,"Soak: live heap allocations grew with the number of sessions");
  require(memoryAfter<=memory+size_t(160)*1024*1024,"Soak: private memory grew without bound");
  require(longLate.blocks<=longEarly.blocks+32&&longLate.bytes<=longEarly.bytes+size_t(4)*1024*1024,"Soak: a long session accumulated live allocations");
  // Machine paths are resolved on short-lived threads (each session's capture
  // thread in x264 builds): a shell folder query per thread leaves a block
  // behind (support.hpp), so they must come from the per-process cache.
  const auto folders=liveHeap();
  for(unsigned n=0;n<60;++n)std::thread([]{(void)host::machineDirectory();}).join();
  require(liveHeap().blocks<=folders.blocks+8,"Soak: resolving the machine directory on new threads leaked heap blocks");
  return out.str();
}
// cursor-tracks-v1 through the actual Agent worker thread: a session.start
// carrying both granted markers leaves the cursor out of both outputs and sends
// the system arrow (real extraction, nothing on the desktop changes) as records
// after both first key frames; one without them composes it as before.
struct Pointing final:host::TrackCapture {
  Pixels pixels;std::array<std::atomic<int>,2>& composed;uint8_t id;
  Pointing(Observations& values,const host::OwnedTrackSource& source,std::array<std::atomic<int>,2>& state):pixels(values,source,true),composed(state),id(source.media.id){}
  host::TrackCapturedFrame frame()override{return pixels.frame();}
  void composeCursor(bool enabled)override{composed[id]=enabled?1:0;}
  host::cursor::Observation cursorSample()override{return {true,CURSOR_SHOWING,LoadCursorW(nullptr,IDC_ARROW),POINT{-1300,400}};}
};
void cursorRecords(){
  for(const bool granted:{false,true}){
    Observations observations;std::array<std::atomic<int>,2> composed;composed[0]=-1;composed[1]=-1;
    std::mutex peer;std::condition_variable arrived;unsigned video=0,shapes=0,states=0;bool early=false,foreign=false;std::array<bool,2> keyed{};
    host::Agent agent([](const std::wstring&){},
      [&](bool text,std::vector<uint8_t> bytes){
        if(text)return;std::lock_guard lock(peer);multitap::tracks::Packet packet;
        if(multitap::tracks::parse(bytes.data(),bytes.size(),packet)){if(packet.kind==multitap::tracks::video){video++;if(packet.flags&1)keyed[packet.track]=true;}}
        else if(host::trackCursorPacketValid(bytes.data(),bytes.size())){early|=!keyed[0]||!keyed[1];(bytes[28]==host::cursor::shapeRecordType?shapes:states)++;}
        else foreign=true;
        arrived.notify_all();
      },
      [](INPUT&)->UINT{return 1;},observed,
      [&](const host::OwnedTrackSource& output)->std::unique_ptr<host::TrackCapture>{return std::make_unique<Pointing>(observations,output,composed);},
      []{return RECT{-1920,-200,1920,1080};});
    auto start=command("12345678-1234-4234-8234-00000000c000");
    if(granted){auto& fields=std::get<host::Json::Object>(start.value);fields["cursorProtocol"].value=std::string("cursor-v1");fields["cursorTracksProtocol"].value=std::string("cursor-tracks-v1");}
    agent.localTrackStart(start,host::bindOwnedTrackSources(start,display(),observed()));
    bool flowing=false;{std::unique_lock lock(peer);flowing=arrived.wait_for(lock,std::chrono::seconds(60),[&]{return video>=8&&(!granted||states>=1);});}
    agent.suspendStreaming();agent.stop();
    std::lock_guard lock(peer);
    require(flowing&&!foreign&&!early,"Track cursor records were missing, malformed or ahead of the first key frames");
    if(granted)require(shapes==1&&states==1&&composed[0]==0&&composed[1]==0,"A granted session did not leave the cursor of both outputs to records");
    else require(shapes==0&&states==0&&composed[0]==-1&&composed[1]==-1,"An ungranted session sent cursor records or changed composition");
  }
  std::cout<<"{\"kind\":\"track-agent-cursor\",\"granted\":true,\"ungranted\":true,\"systemArrowExtracted\":true,\"desktopCursorMoved\":false}\n";
}
// 게임 마우스 on independent outputs (relative-mouse-tracks-v1 with
// cursor-compose-v1) through the actual Agent worker thread: movement is
// injected as relative movement and the outputs compose the cursor while the
// client asks, only for a session.start carrying the granted markers.
void gameMouse(){
  for(const bool granted:{false,true}){
    Observations observations;std::array<std::atomic<int>,2> composed;composed[0]=-1;composed[1]=-1;
    std::mutex peer;std::condition_variable arrived;std::array<bool,2> keyed{};std::vector<INPUT> injected;
    host::Agent agent([](const std::wstring&){},
      [&](bool text,std::vector<uint8_t> bytes){
        if(text)return;std::lock_guard lock(peer);multitap::tracks::Packet packet;
        if(multitap::tracks::parse(bytes.data(),bytes.size(),packet)&&packet.kind==multitap::tracks::video&&(packet.flags&1))keyed[packet.track]=true;
        arrived.notify_all();
      },
      [&](INPUT& input)->UINT{std::lock_guard lock(peer);injected.push_back(input);return 1;},observed,
      [&](const host::OwnedTrackSource& output)->std::unique_ptr<host::TrackCapture>{return std::make_unique<Pointing>(observations,output,composed);},
      []{return RECT{-1920,-200,1920,1080};});
    const std::string id="12345678-1234-4234-8234-00000000d000";
    auto start=command(id);auto& fields=std::get<host::Json::Object>(start.value);
    fields["cursorProtocol"].value=std::string("cursor-v1");fields["cursorTracksProtocol"].value=std::string("cursor-tracks-v1");
    if(granted){fields["cursorComposeProtocol"].value=std::string("cursor-compose-v1");fields["relativeMouseProtocol"].value=std::string("relative-mouse-v1");fields["relativeMouseTracksProtocol"].value=std::string("relative-mouse-tracks-v1");}
    agent.localTrackStart(start,host::bindOwnedTrackSources(start,display(),observed()));
    bool flowing=false;{std::unique_lock lock(peer);flowing=arrived.wait_for(lock,std::chrono::seconds(60),[&]{return keyed[0]&&keyed[1];});}
    agent.localControl("{\"type\":\"stream.tracks.ready\",\"sessionId\":"+host::jsonQuote(id)+",\"topologyEpoch\":1,\"tracks\":[{\"id\":\"0\",\"epoch\":1},{\"id\":\"1\",\"epoch\":1}]}");
    agent.localControl("{\"type\":\"input\",\"sessionId\":"+host::jsonQuote(id)+",\"topologyEpoch\":1,\"event\":\"moveRelative\",\"dx\":12,\"dy\":-3}");
    agent.localControl("{\"type\":\"cursor.compose\",\"sessionId\":"+host::jsonQuote(id)+",\"on\":true}");
    const auto both=[&](int value){const auto until=host::Clock::now()+std::chrono::seconds(30);while(host::Clock::now()<until&&(composed[0]!=value||composed[1]!=value))std::this_thread::sleep_for(std::chrono::milliseconds(10));return composed[0]==value&&composed[1]==value;};
    const bool composing=both(1);
    agent.localControl("{\"type\":\"cursor.compose\",\"sessionId\":"+host::jsonQuote(id)+",\"on\":false}");
    const bool separated=both(0);
    agent.suspendStreaming();agent.stop();
    std::lock_guard lock(peer);
    require(flowing,"Game mouse session did not deliver both key frames");
    const auto relative=std::count_if(injected.begin(),injected.end(),[](const INPUT& input){return input.type==INPUT_MOUSE&&input.mi.dwFlags==MOUSEEVENTF_MOVE&&input.mi.dx==12&&input.mi.dy==-3;});
    if(granted)require(relative==1&&composing&&separated,"A granted session did not inject movement or compose the cursor while asked");
    else require(relative==0&&!composing,"An ungranted session injected movement or composed the cursor");
  }
  std::cout<<"{\"kind\":\"track-agent-game-mouse\",\"granted\":true,\"ungranted\":true,\"desktopCursorMoved\":false}\n";
}
// tracks-resume-v1 through the actual Agent worker thread: the relay's
// stream.keyframe after a resume (forwarded by the service from the track
// tunnel) makes the next video of every output an IDR.
void keyFrames(){
  Observations observations;std::mutex peer;std::condition_variable arrived;std::array<unsigned,2> keys{},video{};
  host::Agent agent([](const std::wstring&){},
    [&](bool text,std::vector<uint8_t> bytes){
      if(text)return;std::lock_guard lock(peer);multitap::tracks::Packet packet;
      if(multitap::tracks::parse(bytes.data(),bytes.size(),packet)&&packet.kind==multitap::tracks::video){video[packet.track]++;if(packet.flags&1)keys[packet.track]++;}
      arrived.notify_all();
    },
    [](INPUT&)->UINT{return 1;},observed,
    [&](const host::OwnedTrackSource& output)->std::unique_ptr<host::TrackCapture>{return std::make_unique<Pixels>(observations,output,true);},
    []{return RECT{-1920,-200,1920,1080};});
  const std::string id="12345678-1234-4234-8234-00000000e000";
  auto start=command(id);agent.localTrackStart(start,host::bindOwnedTrackSources(start,display(),observed()));
  const auto wait=[&](auto predicate,std::chrono::milliseconds limit){std::unique_lock lock(peer);return arrived.wait_for(lock,limit,predicate);};
  const bool flowing=wait([&]{return keys[0]&&keys[1]&&video[0]>=6&&video[1]>=6;},std::chrono::seconds(60));
  // Moving pictures: no IDR comes by itself meanwhile.
  std::array<unsigned,2> before{};{std::lock_guard lock(peer);before=keys;}
  const bool spontaneous=wait([&]{return keys!=before;},std::chrono::milliseconds(500));
  agent.localControl("{\"type\":\"stream.keyframe\",\"sessionId\":"+host::jsonQuote(id)+'}');
  const bool restarted=wait([&]{return keys[0]>before[0]&&keys[1]>before[1];},std::chrono::seconds(30));
  agent.suspendStreaming();agent.stop();
  require(flowing&&!spontaneous,"Key frame fixture did not stream, or coded IDRs without being asked");
  require(restarted,"stream.keyframe did not restart every output at an IDR");
  std::cout<<"{\"kind\":\"track-agent-keyframe\",\"restartedOutputs\":2}\n";
}
}
// gpu: the client decodes through the GPU presentation path (WARP stands in for a
// GPU, see track_gpu_fixture.hpp); the soak runs once, on the CPU pass.
int run(bool gpu){
  try{
    host::Runtime runtime;Observations observations;host::Agent* agent=nullptr;
    std::unique_ptr<trackgpu::Pipelines> gpuPipelines;if(gpu)gpuPipelines=std::make_unique<trackgpu::Pipelines>();
    std::unique_ptr<multitap::RemoteTrackDecoder> decoder;
    std::mutex peer;std::promise<void>* done=nullptr;bool signalled=false,failSecond=false,moving=true;unsigned information=0;std::array<unsigned,2> decoded{};std::string current;
    host::Agent source([](const std::wstring&){},
      [&](bool text,std::vector<uint8_t> bytes){
        std::lock_guard lock(peer);
        if(text){const auto message=host::JsonParser::parse(std::string(bytes.begin(),bytes.end()));
          if(message.get("type").string()=="stream.tracks.info"){
            information++;current=message.get("sessionId").string();decoder=std::make_unique<multitap::RemoteTrackDecoder>();
            const auto plan=host::bindOwnedTrackSources(command(current),display(),observed());host::check(gpu?decoder->ConfigurePipelines(plan.media(false),gpuPipelines->pipelines):decoder->Configure(plan.media(false)),"Local agent decoder start");
          }else if(message.get("type").string()=="stream.unavailable"&&!signalled){signalled=true;if(done)done->set_value();}
          return;
        }
        require(decoder!=nullptr,"Agent video preceded metadata");
        host::check(decoder->Push(bytes.data(),bytes.size(),[&](uint8_t id,uint32_t topology,uint32_t epoch,multitap::DecodedFrame&& frame){require(topology==1&&epoch==1,"Agent crossed track generations");require(gpu?(frame.gpu&&frame.rgba.empty()):!frame.gpu,"The client delivered the wrong picture kind");decoded[id]++;}),"Agent video decode");
        if(decoder->ready()&&!signalled){
          agent->localControl("{\"type\":\"stream.tracks.ready\",\"sessionId\":"+host::jsonQuote(current)+",\"topologyEpoch\":1,\"tracks\":[{\"id\":\"0\",\"epoch\":1},{\"id\":\"1\",\"epoch\":1}]}");
          agent->localControl(keyboard(current));agent->localControl(pointer(current));signalled=true;if(done)done->set_value();
        }
      },
      [&](INPUT& input)->UINT{
        if(input.type==INPUT_KEYBOARD){if(input.ki.dwFlags&KEYEVENTF_KEYUP)observations.released++;else observations.pressed++;}
        else if(input.type==INPUT_MOUSE){
          if(input.mi.dwFlags&MOUSEEVENTF_LEFTDOWN){
            observations.pointerPressed++;
            const auto expectedX=LONG((-1600+.25*639+1920)*65535/3839),expectedY=LONG((300+.75*359+200)*65535/1279);
            if((input.mi.dwFlags&(MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK))!=(MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK)||std::abs(input.mi.dx-expectedX)>1||std::abs(input.mi.dy-expectedY)>1)observations.pointerMismatch++;
          }
          if(input.mi.dwFlags&MOUSEEVENTF_LEFTUP){observations.pointerReleased++;if(input.mi.dwFlags&MOUSEEVENTF_MOVE)observations.pointerMismatch++;}
        }return 1;
      },
      observed,[&](const host::OwnedTrackSource& output)->std::unique_ptr<host::TrackCapture>{
        if(failSecond&&output.media.id==1)throw std::runtime_error("Fixture capture failure");
        return std::make_unique<Pixels>(observations,output,moving);
      },[]{return RECT{-1920,-200,1920,1080};});
    agent=&source;
    // Session 0 animates; later sessions are static. The static startup bound is
    // a performance claim (priming, not the 1 s idle refresh): a broken priming
    // path takes >= 1 s on every attempt regardless of load, so the best of up
    // to three static attempts keeps the claim while a loaded release machine's
    // one slow attempt no longer fails it. Waits use generous ceilings.
    constexpr auto ceiling=std::chrono::seconds(60);
    const std::array<const char*,4> ids{"12345678-1234-4234-8234-123456789abc","12345678-1234-4234-8234-123456789abd","12345678-1234-4234-8234-123456789ab0","12345678-1234-4234-8234-123456789ab1"};
    unsigned sessions=0;double staticBestMs=0;std::array<double,4> startupMs{};
    for(unsigned n=0;n<ids.size();++n){
      const auto id=ids[n];
      std::promise<void> completion;auto ready=completion.get_future();{std::lock_guard lock(peer);done=&completion;signalled=false;decoded={};moving=n==0;}
      const auto began=host::Clock::now();
      auto start=command(id);source.localTrackStart(start,host::bindOwnedTrackSources(start,display(),observed()));
      if(n>=1){source.localControl(keyboard(ids[0]));source.localControl(pointer(ids[0]));}
      const bool completed=ready.wait_for(ceiling)==std::future_status::ready;startupMs[n]=std::chrono::duration<double,std::milli>(host::Clock::now()-began).count();source.suspendStreaming();++sessions;
      require(completed&&decoded[0]&&decoded[1],"Actual agent thread did not deliver two decoded outputs");
      require(observations.pressed==n+1&&observations.released==n+1,"Shared input ownership leaked or admitted an old session");
      require(observations.pointerPressed==n+1&&observations.pointerReleased==n+1&&observations.pointerMismatch==0,"Positioned track click lost geometry/atomic flags or leaked ownership");
      source.localControl(keyboard(id));source.localControl(pointer(id));require(observations.pressed==n+1&&observations.pointerPressed==n+1,"Input was admitted after source shutdown");
      if(n>=1){staticBestMs=n==1?startupMs[n]:std::min(staticBestMs,startupMs[n]);if(staticBestMs<750)break;}
    }
    require(information==sessions&&observations.destroyed==2*sessions&&observations.comAliveOnDestroy==2*sessions,"Source captures outlived their worker COM apartment");
    std::cout<<"{\"movingSyntheticStartupMs\":"<<startupMs[0]<<",\"staticSyntheticStartupMs\":"<<staticBestMs<<",\"staticAttempts\":"<<sessions-1<<",\"actualNetwork\":false,\"actualWindowsCapture\":false}\n";
    require(staticBestMs<750,"Static source startup waited for the idle refresh instead of priming decoder output");
    std::promise<void> failure;auto failed=failure.get_future();{std::lock_guard lock(peer);done=&failure;signalled=false;failSecond=true;}
    auto start=command("12345678-1234-4234-8234-123456789abe");source.localTrackStart(start,host::bindOwnedTrackSources(start,display(),observed()));
    const bool failedAsExpected=failed.wait_for(ceiling)==std::future_status::ready;source.suspendStreaming();
    std::cout<<"{\"partialFailureReported\":"<<(failedAsExpected?"true":"false")<<",\"metadataCount\":"<<information<<",\"destroyedCaptures\":"<<observations.destroyed<<",\"comAliveOnDestroy\":"<<observations.comAliveOnDestroy<<"}\n";
    require(failedAsExpected&&information==sessions&&observations.destroyed==2*sessions+1&&observations.comAliveOnDestroy==2*sessions+1,"Partial source initialization escaped its COM lifetime or published false metadata");
    source.stop();decoder.reset();
    if(!gpu)(void)soak();
    std::cout<<"{\"gpuPresentation\":"<<(gpu?"true":"false")<<",\"actualAgentWorkerThread\":true,\"soakSessionsWithoutResourceGrowth\":"<<(gpu?0:60)<<",\"sessions\":"<<sessions<<",\"partialCaptureFailure\":true,\"sourceCapturesDestroyed\":"<<2*sessions+1<<",\"allDestroyedWithComActive\":true,\"actualClientDecoderOutputs\":true,\"sharedInputPressReleasePairs\":"<<sessions<<",\"positionedPointerPressReleasePairs\":"<<sessions<<",\"positionedPointerGeometryVerified\":true,\"lateSessionInputRejected\":true,\"capture\":\"synthetic factory\",\"input\":\"recorded injector\",\"desktopGeometry\":\"injected\",\"actualWindowsCapture\":false,\"actualPhysicalPresentation\":false,\"windowsDisplayChanges\":0}\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
int main(){
  int cursor=0;try{host::Runtime runtime;cursorRecords();gameMouse();keyFrames();}catch(const std::exception& error){std::cerr<<"cursor-records: "<<error.what()<<'\n';cursor=1;}
  const int cpu=run(false);const int gpu=run(true);return cursor==0&&cpu==0&&gpu==0?0:1;
}
