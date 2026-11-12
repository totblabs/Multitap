#include "service.hpp"
#include "stream_encode_size.hpp"
#include "remote_decoder.h"
#include <future>
#include <iostream>
#include <limits>
using namespace host;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
Json requestJson(){return JsonParser::parse(R"({"type":"stream.resize","sessionId":"a5125678-1234-1234-1234-123456789012","requestId":"resize_1","resizeId":"b5125678-1234-1234-1234-123456789012","epoch":1,"monitor":"0","width":1584,"height":884,"fps":60})");}
void requestGuards(){
  const auto valid=requestJson();const auto request=streamResizeRequest(valid);require(request.width==1584&&request.height==884&&request.epoch==1&&request.matches(valid),"Valid resize correlation rejected");
  for(const auto& [key,number]:std::vector<std::pair<std::string,double>>{{"width",1585},{"width",318},{"width",4294967296.0},{"height",179},{"height",4294967296.0},{"fps",59},{"epoch",0},{"epoch",65536}}){auto invalid=valid;std::get<Json::Object>(invalid.value)[key].value=number;bool refused=false;try{streamResizeRequest(invalid);}catch(...){refused=true;}require(refused,"Unsafe viewport request passed the host guard");}
  // stream-size-v1: portrait owned outputs up to 2160x3840, never above one 4K frame.
  {auto portrait=valid;auto& fields=std::get<Json::Object>(portrait.value);fields["width"].value=1080.0;fields["height"].value=2400.0;require(streamResizeRequest(portrait).height==2400,"Portrait owned output refused");
   fields["width"].value=3840.0;fields["height"].value=2162.0;require(streamResizeRequest(portrait).height==2162,"Representable geometry above4K rejected");}
  auto invalid=valid;std::get<Json::Object>(invalid.value)["monitor"].value=std::string("all");bool refused=false;try{streamResizeRequest(invalid);}catch(...){refused=true;}require(refused,"Combined monitor resize was accepted");
}
void capabilityGuards(){
  const auto probe=JsonParser::parse(R"({"viewportProvisioningSupported":true,"minWidth":320,"maxWidth":3840,"minHeight":180,"maxHeight":2160,"widthAlignment":2,"heightAlignment":2,"maxRefreshRate":60})");
  DisplayState display;display.installed=display.deviceReady=display.active=display.owned=true;display.foreign=0;display.width=1920;display.height=1080;display.name=L"\\\\.\\DISPLAY2";display.monitors={{"1",display.name,0,0,1920,1080,false}};
  require(viewportProvisioningReady(probe,display,true,true,true),"An actual advertised connector1-only topology cannot advertise viewport support");
  require(!viewportProvisioningReady(probe,display,false,true,true)&&!viewportProvisioningReady(probe,display,true,false,true)&&!viewportProvisioningReady(probe,display,true,true,false),"Session0, remote session or nonadministrator advertised display changes");
  display.recoveryRequired=true;require(!display.ready()&&!viewportProvisioningReady(probe,display,true,true,true),"Pending recovery was advertised as ready");display.recoveryRequired=false;display.rebootRequired=true;require(!display.ready(),"Required reboot was advertised as ready");display.rebootRequired=false;display.configurationPending=true;require(!display.ready(),"Pending driver configuration was advertised as ready");
}
void applyPolicy(){
  for(int scenario=0;scenario<12;++scenario){
    DisplayState before;before.installed=before.deviceReady=before.active=before.owned=before.viewportProvisioning=true;before.foreign=0;before.width=1920;before.height=1080;before.name=L"\\\\.\\DISPLAY2";before.monitors={{"0",before.name,0,0,1920,1080,false},{"1",L"\\\\.\\DISPLAY3",5000,0,1920,1080,false}};
    auto after=before;after.width=1584;after.height=884;after.name=L"\\\\.\\DISPLAY9";after.monitors[0].name=after.name;after.monitors[0].width=1584;after.monitors[0].height=884;
    const std::vector<CaptureMonitor> observedBefore{{L"\\\\.\\DISPLAY1",L"console",{-1280,0,0,720}},{before.name,L"owned-A",{0,0,1920,1080}},{L"\\\\.\\DISPLAY3",L"owned-B",{5000,0,6920,1080}}};
    auto observedAfter=observedBefore;observedAfter[1]={after.name,L"owned-A-fresh",{0,0,1584,884}};
    if(scenario==1)before.viewportProvisioning=false;
    if(scenario==6)after.monitors[0].width=1280;
    if(scenario==7)observedAfter.push_back({L"\\\\.\\DISPLAY7",L"foreign-clone",{0,0,1584,884}});
    if(scenario==8)after.monitors.pop_back();
    if(scenario==9)observedAfter[0].bounds.right=-2;
    if(scenario==10)after.monitors[1].left=4998;
    auto current=JsonParser::parse(R"({"type":"session.start","sessionId":"a5125678-1234-1234-1234-123456789012","mode":"stream","resizeProtocol":"epoch-v1","epoch":0,"monitor":"0","width":1920,"height":1080,"fps":60,"bitrate":12000000,"audio":true})");attachFreshCapture(current,before,observedBefore);
    if(scenario==4)streamField(current,"displayDeviceName",std::string("\\\\.\\DISPLAY8"));
    if(scenario==11)streamField(current,"epoch",1);
    int applies=0;bool applied=false;
    WorkerViewportAccess access{[&](DWORD){return applied?after:before;},[&]{return applied?observedAfter:observedBefore;},[&]{return scenario!=2;},[&]{return scenario!=3;},[&](const std::wstring& device,int width,int height,DWORD timeout){require(timeout>0&&timeout<=75000,"Helper timeout escaped the shared budget");require(device==before.name&&width==1584&&height==884,"OS helper target escaped the selected owned source");++applies;applied=true;auto result=JsonParser::parse(R"({"viewportApplied":true,"configurationPending":false,"width":1584,"height":884,"refreshRate":60,"targetDisplayDeviceName":"\\\\.\\DISPLAY9","targetMonitorId":"0"})");return DisplayHelperOutput{DWORD(scenario==5?1:0),false,result};}};
    const auto outcome=applyWorkerViewport(current,streamResizeRequest(requestJson()),"0",access);
    require(outcome.ok==(scenario==0),"Unverified or changed topology was accepted as a completed viewport");
    if(scenario==0){require(outcome.command.get("captureWidth").number()==1584&&outcome.command.get("captureHeight").number()==884&&outcome.command.get("displayDeviceName").string()=="\\\\.\\DISPLAY9"&&outcome.command.get("epoch").number()==1,"Fresh capture command retained old geometry or epoch");require(outcome.command.get("audio").boolean()&&outcome.command.get("bitrate").number()==12000000,"Resize changed unrelated stream options");}
    if(scenario==1||scenario==2||scenario==3||scenario==4||scenario==11)require(applies==0,"Rejected resize reached the OS helper");
  }
}
void dualExpansionRoundTrip(){
  DisplayState state;state.installed=state.deviceReady=state.active=state.owned=state.viewportProvisioning=true;state.foreign=0;state.width=1920;state.height=1080;state.name=L"\\\\.\\DISPLAY2";state.monitors={{"0",state.name,0,0,1920,1080,true},{"1",L"\\\\.\\DISPLAY3",1920,0,1920,1080,false}};
  const auto observed=[&]{std::vector<CaptureMonitor> values{{L"\\\\.\\DISPLAY1",L"console",{-1280,0,0,720},false}};for(const auto& monitor:state.monitors)values.push_back({monitor.name,L"owned-"+wide(monitor.id),{monitor.left,monitor.top,monitor.left+LONG(monitor.width),monitor.top+LONG(monitor.height)},monitor.primary});return values;};
  auto current=JsonParser::parse(R"({"type":"session.start","sessionId":"a5125678-1234-1234-1234-123456789012","mode":"stream","resizeProtocol":"epoch-v1","epoch":0,"monitor":"0","width":1920,"height":1080,"fps":60,"bitrate":12000000,"audio":true})");attachFreshCapture(current,state,observed());
  int applies=0;WorkerViewportAccess access{[&](DWORD){return state;},observed,[]{return true;},[]{return true;},[&](const std::wstring& selected,int width,int height,DWORD){require(selected==state.monitors[0].name,"Second resize used a stale GDI target");++applies;state.width=width;state.height=height;state.name=applies==1?L"\\\\.\\DISPLAY9":L"\\\\.\\DISPLAY11";state.monitors[0].name=state.name;state.monitors[0].width=width;state.monitors[0].height=height;state.monitors[1].name=applies==1?L"\\\\.\\DISPLAY10":L"\\\\.\\DISPLAY12";state.monitors[1].left=width;
    Json result;result.value=Json::Object{};std::get<Json::Object>(result.value)["viewportApplied"].value=true;std::get<Json::Object>(result.value)["configurationPending"].value=false;streamField(result,"width",width);streamField(result,"height",height);streamField(result,"refreshRate",60);streamField(result,"targetDisplayDeviceName",utf8(state.name));streamField(result,"targetMonitorId",std::string("0"));return DisplayHelperOutput{0,false,result};}};
  for(int step=0;step<2;++step){auto request=requestJson();streamField(request,"epoch",step+1);streamField(request,"requestId",std::string(step?"resize_2":"resize_1"));streamField(request,"width",step?1920:3188);streamField(request,"height",step?1080:1160);const auto outcome=applyWorkerViewport(current,streamResizeRequest(request),"0",access);require(outcome.ok,"Owned-only layout expansion or restoration was rejected");current=outcome.command;require(state.monitors.size()==2&&state.monitors[0].primary&&!state.monitors[1].primary&&state.monitors[1].width==1920&&state.monitors[1].height==1080,"Layout change altered monitor count, other size or primary");}
  require(applies==2&&state.monitors[1].left==1920&&current.get("captureWidth").number()==1920,"Owned display layout did not restore after shrinking");
}
void resizeBudgetGuards(){
  int64_t now=1000;bool cancelled=false;
  StreamResizeBudget budget;budget.now=[&]{return now;};budget.cancelled=[&]{return cancelled;};budget.deadline=now+streamResizeTimeoutMs;
  require(budget.remaining(75000,streamResizeApplyReserveMs)==70000,"Apply consumed the post-probe/cleanup/IPC reserve");
  now+=10000;require(budget.remaining(75000,streamResizeApplyReserveMs)==60000,"Preflight time did not reduce the helper budget");
  for(const auto value:std::vector<double>{0,-1,double(now),double(now+streamResizeTimeoutMs+1),double(now)+0.5,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
    auto command=requestJson();streamField(command,"workerDeadlineMs",value);bool failed=false;try{workerResizeDeadline(command,now);}catch(...){failed=true;}require(failed,"Untrusted or expired worker deadline passed validation");
  }
  auto command=requestJson();streamField(command,"workerDeadlineMs",double(now+60000));require(workerResizeDeadline(command,now)==now+60000,"Valid service deadline rejected");
  cancelled=true;bool failed=false;try{budget.remaining(1);}catch(...){failed=true;}require(failed,"Cancelled resize retained helper permission");cancelled=false;now=budget.deadline;failed=false;try{budget.remaining(1);}catch(...){failed=true;}require(failed,"Expired budget retained helper permission");
  for(const DWORD timeout:{0UL,75001UL}){failed=false;try{applyViewport(L"\\\\.\\DISPLAY2",1584,884,timeout);}catch(...){failed=true;}require(failed,"Invalid helper timeout reached CreateProcess");}
}
void sharedBudgetApply(){
  for(int scenario=0;scenario<13;++scenario){
    DisplayState before;before.installed=before.deviceReady=before.active=before.owned=before.viewportProvisioning=true;before.foreign=0;before.width=1920;before.height=1080;before.name=L"\\\\.\\DISPLAY2";before.monitors={{"0",before.name,0,0,1920,1080,true}};
    auto after=before;after.width=1584;after.height=884;after.monitors[0].width=1584;after.monitors[0].height=884;
    const std::vector<CaptureMonitor> observedBefore{{before.name,L"owned",{0,0,1920,1080},true}},observedAfter{{after.name,L"owned",{0,0,1584,884},true}};
    auto current=JsonParser::parse(R"({"type":"session.start","sessionId":"a5125678-1234-1234-1234-123456789012","resizeProtocol":"epoch-v1","epoch":0,"monitor":"0"})");attachFreshCapture(current,before,observedBefore);
    int64_t now=1000;bool cancelled=false,applied=false;int probes=0,applies=0;DWORD applyTimeout=0;
    StreamResizeBudget budget;budget.now=[&]{return now;};budget.cancelled=[&]{return cancelled;};budget.deadline=now+(scenario==1?18000:90000);if(scenario==9)now=budget.deadline;
    WorkerViewportAccess access{
      [&](DWORD timeout){require(timeout>0&&timeout<=10000,"Probe budget escaped its per-phase cap");++probes;now+=scenario==3?90000:timeout;if(scenario==2&&probes==1)cancelled=true;if(scenario==5&&probes==2)now=budget.deadline;if(scenario==8&&probes>=2)throw std::runtime_error("fixture_probe_failure");auto display=applied?after:before;if(scenario==7&&applied){display.recoveryRequired=true;display.configurationPending=true;}return display;},
      [&]{if(scenario==6&&applied)now=budget.deadline;return applied?observedAfter:observedBefore;},
      []{return true;},[]{return true;},
      [&](const std::wstring&,int,int,DWORD timeout){++applies;applyTimeout=timeout;require(timeout==60000,"Helper did not receive the service's remaining budget");applied=true;now+=scenario==4?80000:timeout;if(scenario==10)cancelled=true;if(scenario==12)now=budget.deadline;Json value;value.value=Json::Object{};std::get<Json::Object>(value.value)["viewportApplied"].value=true;std::get<Json::Object>(value.value)["configurationPending"].value=scenario>=10;std::get<Json::Object>(value.value)["recoveryRequired"].value=scenario>=10;std::get<Json::Object>(value.value)["rebootRequired"].value=scenario>=10;streamField(value,"width",1584);streamField(value,"height",884);streamField(value,"refreshRate",60);streamField(value,"targetDisplayDeviceName",utf8(after.name));streamField(value,"targetMonitorId",std::string("0"));return DisplayHelperOutput{0,scenario==11,value};}};
    const auto result=applyWorkerViewport(current,streamResizeRequest(requestJson()),"0",access,budget);
    require(result.ok==(scenario==0),"Expired/cancelled/unverified resize reported success");
    if(scenario==0)require(probes==2&&applies==1&&applyTimeout==60000&&now<budget.deadline,"Successful resize did not complete both probes within one budget");
    if(scenario==1||scenario==2||scenario==3||scenario==9)require(applies==0,"Unsubmitted resize launched a mutating helper after insufficient budget/cancellation");
    if(scenario==2||scenario==3||scenario==4||scenario==9)require(probes<2,"Expired/cancelled resize launched a fresh postflight helper");
    if(scenario==7)require(result.display.recoveryRequired&&result.display.configurationPending,"Fresh recovery/pending state was discarded");
    if(scenario>=10)require(result.display.recoveryRequired&&result.display.configurationPending&&result.display.rebootRequired&&!result.display.ready(),"Cancellation/timeout erased acknowledged helper recovery metadata");
    if(scenario==10||scenario==12)require(probes==1,"Cancellation/deadline launched another helper after apply");
    if(scenario==5||scenario==8||scenario==9)require(!result.display.ready(),"Unconfirmed topology remained advertised as ready");
  }
}
void boundedReadOnlyProbe(){
  DisplayState ready;ready.installed=ready.deviceReady=ready.active=ready.owned=true;ready.foreign=0;ready.width=1920;ready.height=1080;ready.name=L"\\\\.\\DISPLAY2";ready.monitors={{"0",ready.name,0,0,1920,1080,true}};
  for(int scenario=0;scenario<12;++scenario){
    int64_t now=1000;int calls=0;bool cancelled=false;std::vector<DWORD> timeouts;
    WorkerViewportProbeAccess access;access.now=[&]{return now;};access.desktop=access.console=access.privileged=true;access.cancelled=[&]{return cancelled;};access.nativeProvider=[]{return false;};
    if(scenario==5)access.console=false;
    access.helper=[&](const std::wstring& action,DWORD timeout){++calls;timeouts.push_back(timeout);require((calls==1&&action==L"topology-status")||(calls==2&&action==L"viewport-status"),"Probe used a mutating/fallback/third helper");now+=scenario==1&&calls==2?timeout:4500;if(scenario==4)now+=6000;if(scenario==9)cancelled=true;DisplayHelperOutput out;out.exitCode=0;
      if(calls==1){auto state=ready;if(scenario==6||scenario>=10)state.recoveryRequired=true;if(scenario==7||scenario>=10)state.rebootRequired=true;if(scenario>=10)state.configurationPending=true;out.value.value=Json::Object{};std::get<Json::Object>(out.value.value)["state"]=JsonParser::parse(workerDisplayJson(state));if(scenario==2||scenario==10)out.timedOut=true;if(scenario==3||scenario==11)out.exitCode=1;}
      else{out.value=JsonParser::parse(R"({"viewportProvisioningSupported":true,"minWidth":320,"maxWidth":3840,"minHeight":180,"maxHeight":2160,"widthAlignment":2,"heightAlignment":2,"maxRefreshRate":60})");if(scenario==8){out.exitCode=1;std::get<Json::Object>(out.value.value)["error"].value=std::string("PRIVATE_SENTINEL");}}
      return out;};
    const auto result=boundedWorkerViewportProbe(scenario==1?7000:10000,access);
    if(result.viewportProvisioning!=(scenario==0))throw std::runtime_error("Read-only probe fixture scenario "+std::to_string(scenario)+" mismatch: "+result.error);
    require(calls<=2&&std::all_of(timeouts.begin(),timeouts.end(),[](DWORD value){return value>0&&value<=5000;}),"Probe exceeded the bounded helper contract");
    if(scenario==1)require(timeouts==std::vector<DWORD>({5000,2500})&&!result.ready(),"Second probe did not consume only its remaining budget");
    if(scenario==2||scenario==3||scenario==4||scenario==9)require(calls==1&&!result.ready(),"Failed/cancelled first probe ran a fallback or retained readiness");
    if(scenario==5)require(calls==0&&result.remoteSession&&!result.ready(),"RDP session invoked a viewport helper or fabricated readiness");
    if(scenario==6)require(result.recoveryRequired&&!result.ready(),"Pending recovery was lost during probe");
    if(scenario==7)require(result.rebootRequired&&!result.ready(),"Required restart was lost during probe");
    if(scenario>=10)require(calls==1&&result.rebootRequired&&result.recoveryRequired&&result.configurationPending&&!result.ready(),"Partial failed/timeout observation lost known pending recovery");
    require(workerDisplayJson(result).find("PRIVATE_SENTINEL")==std::string::npos,"Raw helper text leaked through a bounded probe");
  }
}
void nativeReadOnlyProbe(){
  int nativeCalls=0,legacyCalls=0;bool cancelled=false;
  WorkerViewportProbeAccess access;access.nativeProvider=[]{return true;};
  access.nativeProbe=[&]{++nativeCalls;DisplayState value;value.installed=value.deviceReady=value.owned=true;value.recoveryRequired=true;value.error="native_topology_recovery_required";return value;};
  access.helper=[&](const auto&,DWORD){++legacyCalls;throw std::runtime_error("Native selection called a legacy helper");return DisplayHelperOutput{};};
  access.clipboard=true;access.cancelled=[&]{return cancelled;};
  const auto pending=boundedWorkerViewportProbe(1000,access);
  require(nativeCalls==1&&legacyCalls==0&&pending.clipboardSupported&&pending.recoveryRequired&&!pending.ready(),"Read-only native recovery state was lost or fell back to the console helper");
  cancelled=true;const auto stopped=boundedWorkerViewportProbe(1000,access);
  require(nativeCalls==1&&legacyCalls==0&&!stopped.ready()&&stopped.error=="viewport_probe_failed","Cancelled native probe still queried the device");
  for(const DWORD budget:std::array<DWORD,2>{0,DWORD(streamResizeProbeTimeoutMs+1)}){bool rejected=false;try{(void)boundedWorkerViewportProbe(budget,access);}catch(...){rejected=true;}require(rejected&&nativeCalls==1&&legacyCalls==0,"Invalid native probe budget crossed the device boundary");}
}
void pipeDeadline(){
  const auto name=L"\\\\.\\pipe\\MultitapHost.QA.Resize."+randomName();WinHandle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr));require(server.value!=INVALID_HANDLE_VALUE,"Resize QA pipe failed");WinHandle client(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));require(client.value!=INVALID_HANDLE_VALUE,"Resize QA client failed");ConnectNamedPipe(server.value,nullptr);
  std::atomic<bool> stopped{false};std::atomic<int64_t> extension{0};bool text=false;std::vector<uint8_t> bytes;std::promise<bool> result;
  // Margins sized for a loaded machine: the extension must be set before the
  // pending read's 400 ms idle bound passes (30 ms in), and the completion is
  // written after that bound (600 ms) but well inside the extension (10 s).
  std::thread reader([&]{result.set_value(readPipePacket(server.value,text,bytes,stopped,std::chrono::milliseconds(400),&extension));});Sleep(30);extension=streamMonotonicMillis()+10000;Sleep(600);std::mutex mutex;require(pipePacket(client.value,true,{'o','k'},stopped,mutex),"Extended resize read did not accept its completion");reader.join();require(result.get_future().get()&&text&&bytes==std::vector<uint8_t>({'o','k'}),"Resize timeout did not extend an already pending read");
  extension=streamMonotonicMillis()+150;const auto began=Clock::now();require(!readPipePacket(server.value,text,bytes,stopped,std::chrono::milliseconds(1500),&extension)&&Clock::now()-began<std::chrono::seconds(1),"Absolute resize deadline must expire before a newly started idle timeout");
  extension=streamMonotonicMillis()+90000;stopped=true;require(!readPipePacket(server.value,text,bytes,stopped,std::chrono::milliseconds(100),&extension),"Session stop was ignored during resize wait");
}
// A still screen sends no packet, so a command must leave during the read wait.
void pipeWake(){
  const auto name=L"\\\\.\\pipe\\MultitapHost.QA.Wake."+randomName();WinHandle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr));require(server.value!=INVALID_HANDLE_VALUE,"Wake QA pipe failed");WinHandle client(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));require(client.value!=INVALID_HANDLE_VALUE,"Wake QA client failed");ConnectNamedPipe(server.value,nullptr);
  WinHandle wakeEvent(CreateEventW(nullptr,FALSE,FALSE,nullptr));require(wakeEvent.value!=nullptr,"Wake QA event failed");
  std::atomic<bool> stopped{false};std::atomic<int> runs{0};std::mutex writes;bool text=false;std::vector<uint8_t> bytes;std::promise<bool> served;
  // The read stays pending while the same thread writes the command on this pipe.
  const PipeWake wake{wakeEvent.value,[&]{++runs;return pipePacket(server.value,true,{'g','o'},stopped,writes);}};
  std::thread reader([&]{served.set_value(readPipePacket(server.value,text,bytes,stopped,std::chrono::seconds(30),nullptr,&wake));});
  Sleep(30);SetEvent(wakeEvent.value);
  bool peerText=false;std::vector<uint8_t> peer;std::atomic<bool> peerStopped{false};
  require(readPipePacket(client.value,peerText,peer,peerStopped,std::chrono::seconds(20))&&peerText&&peer==std::vector<uint8_t>({'g','o'}),"A waiting command did not leave during the read");
  require(runs==1,"The wake did not run once");
  std::mutex mutex;require(pipePacket(client.value,true,{'o','k'},peerStopped,mutex),"The peer could not answer");reader.join();
  require(served.get_future().get()&&text&&bytes==std::vector<uint8_t>({'o','k'}),"Serving a wake lost the pending read");
  for(const bool throws:{false,true}){
    const PipeWake ending{wakeEvent.value,[&]()->bool{if(throws)throw std::runtime_error("private");return false;}};SetEvent(wakeEvent.value);
    const auto began=Clock::now();require(!readPipePacket(server.value,text,bytes,stopped,std::chrono::seconds(30),nullptr,&ending)&&Clock::now()-began<std::chrono::seconds(10),"A failed wake did not end the read");
  }
  // The read is cancelled and the pipe stays usable, so a later reader still works.
  require(pipePacket(client.value,true,{'o','k'},peerStopped,mutex)&&readPipePacket(server.value,text,bytes,stopped,std::chrono::seconds(20))&&bytes==std::vector<uint8_t>({'o','k'}),"A failed wake damaged the pipe");
}
// Viewport streams are encoded at their own size within the pixel budget, not
// fitted into a 1920-wide box (2960x760 -> 1920x492 is blurry).
void encodeSizePolicy(){
  const auto size=[](int w,int h,int fps=60,int boxW=3840,int boxH=2160,uint64_t budget=streamPixelBudgetDefault){return streamEncodeSize(w,h,boxW,boxH,fps,budget);};
  using P=std::pair<int,int>;
  // The field viewports and the common wide/tall ones stay native.
  for(const auto& native:std::vector<P>{{2960,760},{2888,788},{3840,1080},{2560,1440},{1920,1080},{1580,952},{3440,1200},{1280,720},{2560,1600}})
    require(size(native.first,native.second)==native,"A viewport within the pixel budget was not encoded at its own size");
  // Over the budget: one scale, aspect kept, within the pixel and macroblock budget.
  for(const auto& large:std::vector<P>{{3840,2160},{3440,1440},{3840,1600},{2880,1800}}){
    const auto encoded=size(large.first,large.second);
    require(encoded.first<large.first&&encoded.second<large.second&&!(encoded.first&1)&&!(encoded.second&1),"An over-budget viewport was not scaled to even dimensions");
    require(uint64_t(encoded.first)*encoded.second<=streamPixelBudgetDefault&&streamMacroblocks(encoded.first,encoded.second)<=streamPixelBudgetDefault/256,"An over-budget viewport exceeded the pixel/macroblock budget");
    require(std::abs(double(encoded.first)/encoded.second-double(large.first)/large.second)<0.01,"Scaling changed the aspect ratio");
    require(uint64_t(encoded.first)*encoded.second>=streamPixelBudgetDefault*97/100,"Scaling went far below the budget");
  }
  require(size(3840,2160)==P{2716,1528}&&size(3440,1440)==P{3148,1318},"Over-budget viewports were not scaled to the budget");
  // 30 fps gets the same macroblock rate: 4K stays native.
  require(size(3840,2160,30)==P{3840,2160},"A 30 fps 4K viewport was scaled although the macroblock rate allows it");
  // The client's box is still a ceiling (a released client asks 1920x1080).
  require(size(2960,760,60,1920,1080)==P{1920,492},"The released 1920x1080 box was not honoured");
  require(size(2560,1440,60,3840,1080)==P{1920,1080},"A box height below the source was not honoured");
  // Administrator budget: a valid value applies, anything else is the default.
  require(streamPixelBudget(JsonParser::parse(R"({"videoPixelBudget":2073600})"))==2073600&&size(2960,760,60,3840,2160,2073600).first<2960,"A configured budget was not applied");
  for(const auto* bad:{R"({})",R"({"videoPixelBudget":100})",R"({"videoPixelBudget":3000000000})",R"({"videoPixelBudget":2073600.5})",R"({"videoPixelBudget":"2073600"})"})
    require(streamPixelBudget(JsonParser::parse(bad))==streamPixelBudgetDefault,"An invalid budget was accepted");
}
void codecEpoch(){
  Runtime runtime;uint16_t epoch=0;
  // stream-size-v1: portrait pictures (a phone's 1080x2400, rotated 4K) through the actual
  // encoder and the client's decoder, which also accepts both orientations up front.
  require(multitap::RemoteDecoder::Accepts(3840,2160)&&multitap::RemoteDecoder::Accepts(2160,3840)&&multitap::RemoteDecoder::Accepts(1080,2400)&&multitap::RemoteDecoder::Accepts(3842,2160)&&multitap::RemoteDecoder::Accepts(2160,3842)&&!multitap::RemoteDecoder::Accepts(INT32_MAX,INT32_MAX),"Client decoder representation acceptance; actual support requires initialization");
  for(const auto [width,height]:std::vector<std::pair<int,int>>{{1920,1080},{1584,884},{2560,1100},{2960,760},{1920,1080},{1080,2400},{2160,3840}}){
    Encoder encoder(width,height,60,12000000);multitap::RemoteDecoder decoder;check(decoder.Initialize(width,height),"Fresh client decoder startup");
    std::vector<uint8_t> bgra(size_t(width)*height*4,0);for(size_t i=0;i<bgra.size();i+=4){bgra[i]=40;bgra[i+1]=90;bgra[i+2]=160;bgra[i+3]=255;}std::vector<uint8_t> nv12(size_t(width)*height*3/2);bgraToNv12(bgra.data(),nv12.data(),width,height);
    int encoded=0,decoded=0;const auto shown=[&](multitap::DecodedFrame&& frame){require(frame.width==uint32_t(width)&&frame.height==uint32_t(height)&&frame.rgba.size()==size_t(width)*height*4,"Client decoder retained stale or padded dimensions");++decoded;};
    const auto output=[&](Frame frame){auto packet=mediaPacket(1,frame.ptsUs,frame.bytes,frame.key);stampMediaEpoch(packet,epoch);require((uint16_t(packet[2])|(uint16_t(packet[3])<<8))==epoch,"Media header retained an old epoch");if(!encoded)require(frame.key&&packet[1]==1,"Fresh resized encoder did not begin with a key frame");++encoded;check(decoder.Decode(packet.data()+12,packet.size()-12,frame.ptsUs,shown),"Actual resized H264 client decode");};
    for(int i=0;i<6;++i)encoder.encode(nv12.data(),uint64_t(i)*16666,output);encoder.finish(output);check(decoder.Drain(shown),"Actual client decoder drain");require(encoded==6&&decoded==6,"Fresh H264 encoder/decoder lost synthetic resized frames");auto audio=mediaPacket(2,0,std::vector<uint8_t>(8),false);stampMediaEpoch(audio,epoch);require(audio[2]==uint8_t(epoch)&&audio[3]==uint8_t(epoch>>8),"Audio packet retained an old epoch");++epoch;
  }
}
}
int wmain(){try{requestGuards();capabilityGuards();applyPolicy();dualExpansionRoundTrip();resizeBudgetGuards();sharedBudgetApply();boundedReadOnlyProbe();nativeReadOnlyProbe();pipeDeadline();pipeWake();encodeSizePolicy();codecEpoch();std::cout<<"{\"kind\":\"native-resize-components\",\"requestGuardCases\":10,\"capabilityGuardCases\":7,\"connector1OnlyAllowed\":true,\"pendingRecoveryNotReady\":true,\"fakeOsTopologyCases\":14,\"sharedBudgetCases\":13,\"boundedReadOnlyProbeCases\":12,\"nativeReadOnlyProbeCases\":4,\"totalResizeBudgetMs\":90000,\"timeoutCancelNeverPass\":true,\"fake3188PixelDualOwnedLayoutRoundtrip\":true,\"ownedPrimaryAndForeignGeometryPreserved\":true,\"realPendingPipeDeadlineExtension\":true,\"boundedDeadlineAndStop\":true,\"encodeSizePolicy\":true,\"actualH264Epochs\":7,\"actualSyntheticEncodedDecodedFrames\":42,\"portraitEncodedDecoded\":true,\"freshEncoderFirstKeyFrame\":true,\"sourceResolutionChanged\":false,\"realDesktopCaptured\":false,\"vmAccessed\":false,\"driverChanged\":false}\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
