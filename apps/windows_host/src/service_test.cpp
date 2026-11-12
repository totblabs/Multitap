#include "settings_rpc.hpp"
#include <iostream>
#include <future>
#include <sstream>
#include <tuple>
using namespace host;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void serviceStatusReadOnly(){
  const auto absent=readHostServiceSnapshot((L"MultitapHost.QA.Absent."+randomName()).c_str());
  require(absent.presenceKnown&&absent.runningKnown&&!absent.installed&&!absent.running&&absent.error==0&&std::string(absent.key())=="not-installed","Absent service must be a valid known status, not an exception");
  HostServiceSnapshot denied;denied.error=ERROR_ACCESS_DENIED;require(std::string(denied.key())=="unknown"&&serviceLabel(denied)!=serviceLabel(absent),"Access denied was mistaken for a missing service");
  const auto status=JsonParser::parse(settingsStatus());require(status.get("service").get("state").string().size()>0,"Actual read-only status did not contain service state");
  std::istringstream input("{\"action\":\"status\"}");std::ostringstream output;
  auto* previousInput=std::cin.rdbuf(input.rdbuf());auto* previousOutput=std::cout.rdbuf(output.rdbuf());const int result=settingsRpc();std::cin.rdbuf(previousInput);std::cout.rdbuf(previousOutput);
  const auto response=JsonParser::parse(output.str());require(result==0&&response.get("ok").boolean()&&!response.get("status").get("version").string().empty(),"Actual status RPC produced a partial or invalid JSON response");
}
void displayGate(){
  const std::string valid=R"({"installed":true,"deviceReady":true,"activeDisplay":true,"owned":true,"foreignDeviceCount":0,"width":1920,"height":1080,"left":1920,"top":0,"displayDeviceName":"\\\\.\\DISPLAY2","displays":[{"id":"0","displayDeviceName":"\\\\.\\DISPLAY2","left":1920,"top":0,"width":1920,"height":1080,"primary":false,"virtual":true}]})";
  auto parsed=JsonParser::parse(valid);require(parseDisplayState(parsed).ready(),"Owned virtual topology was rejected");auto& values=std::get<Json::Object>(parsed.value);values["owned"].value=false;require(!parseDisplayState(parsed).ready(),"Foreign display passed readiness");values["owned"].value=true;values["foreignDeviceCount"].value=1.0;require(!parseDisplayState(parsed).ready(),"Mixed foreign device passed readiness");values["foreignDeviceCount"].value=0.0;values["activeDisplay"].value=false;require(!parseDisplayState(parsed).ready(),"Inactive display passed readiness");
}
void displayOperationPolicy(){
  const auto valid=JsonParser::parse(R"({"operationId":"a5125678-1234-1234-1234-123456789012","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","count":1})");
  const auto key=displayOperationKey(valid);require(key.matches(valid)&&displayOperationCount(valid)==1,"Valid display operation rejected");
  auto changed=valid;auto& object=std::get<Json::Object>(changed.value);object["nonce"].value=std::string(43,'b');require(!key.matches(changed),"Unrelated nonce could cancel another operation");
  object["nonce"].value=std::string(43,'a');object["operationId"].value=std::string("a5125678-1234-1234-1234-123456789013");require(!key.matches(changed),"Stale operation could cancel a new operation");
  const auto rejects=[&](const char* source,bool count){bool rejected=false;try{const auto value=JsonParser::parse(source);if(count)displayOperationCount(value);else displayOperationKey(value);}catch(...){rejected=true;}require(rejected,"Invalid display request was accepted");};
  rejects(R"({"operationId":"bad","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})",false);
  rejects(R"({"operationId":"a5125678-1234-1234-1234-123456789012","nonce":"short"})",false);
  rejects(R"({"count":3})",true);rejects(R"({"count":1.5})",true);rejects(R"({"count":"1"})",true);
  DisplayState display;display.installed=display.deviceReady=display.active=display.owned=true;display.foreign=0;display.width=1920;display.height=1080;display.name=L"\\\\.\\DISPLAY2";display.monitors.push_back({"0",display.name,0,0,1920,1080,true});
  const auto runtime=JsonParser::parse(R"({"topologySupported":true,"ownedSourceCount":2})");
  require(monitorControlReady(runtime,display,true,true),"Verified console prerequisite was rejected");
  require(!monitorControlReady(JsonParser::parse(R"({"helperVersion":"0.1.4"})"),display,true,true),"Helper version alone advertised monitor control");
  require(!monitorControlReady(runtime,display,false,true)&&!monitorControlReady(runtime,display,true,false),"Locked or RDP session advertised monitor control");
  display.rebootRequired=true;require(!monitorControlReady(runtime,display,true,true),"Pending reboot advertised monitor control");display.rebootRequired=false;display.owned=false;require(!monitorControlReady(runtime,display,true,true),"Foreign adapter advertised monitor control");
  display.owned=true;display.monitorControlSupported=true;const auto roundtrip=parseDisplayState(JsonParser::parse(workerDisplayJson(display)));require(roundtrip.ready()&&roundtrip.monitorControlSupported&&roundtrip.monitors.size()==1,"Worker display policy was lost during private IPC serialization");
}
bool helperProcesses(){
  if(nativeDisplaySelected()){
    bool blocked=false;try{(void)executeDisplayHelper(executablePath(),L"--helper-fixture-large",3000);}catch(const std::exception& error){blocked=std::string(error.what())=="native_display_legacy_operation_blocked";}
    require(blocked,"Selected native provider allowed a direct legacy helper process");return false;
  }
  const auto large=executeDisplayHelper(executablePath(),L"--helper-fixture-large",3000);require(!large.timedOut&&!large.exitCode&&large.value.get("payload").string().size()==48000,"Display helper response blocked above pipe capacity");
  const auto began=Clock::now();const auto silent=executeDisplayHelper(executablePath(),L"--helper-fixture-silent",150);require(silent.timedOut&&Clock::now()-began<std::chrono::seconds(3),"Timed-out owned helper did not terminate within its bound");
  return true;
}
void displayWorkerTransactions(){
  for (int scenario=0;scenario<8;++scenario) {
    const auto name=L"\\\\.\\pipe\\MultitapHost.QA.Display."+randomName();
    WinHandle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));
    require(server.value!=INVALID_HANDLE_VALUE,"Display transaction pipe creation failed");
    WinHandle client(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));require(client.value!=INVALID_HANDLE_VALUE,"Display transaction client failed");ConnectNamedPipe(server.value,nullptr);
    std::atomic<bool> stopped{false},interactive{true};std::atomic<int> applyCalls{0},probeCalls{0};std::mutex serverWrite,clientWrite;
    DisplayState model;model.installed=model.deviceReady=model.active=model.owned=model.monitorControlSupported=true;model.foreign=0;model.width=1920;model.height=1080;model.name=L"\\\\.\\DISPLAY2";model.monitors={{"0",model.name,0,0,1920,1080,true},{"1",L"\\\\.\\DISPLAY3",1920,0,1920,1080,false}};
    const auto request=JsonParser::parse(R"({"operationId":"a5125678-1234-1234-1234-123456789012","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","count":1})");
    WorkerDisplayAccess access{[&]{probeCalls++;return model;},[&]{return interactive.load();},[&]{return scenario!=6;},[&](int count){applyCalls++;DisplayHelperOutput output;output.exitCode=scenario==7?1:0;output.value=JsonParser::parse(R"({"topologyApplied":true,"activeDisplayCountRequested":1,"configurationPending":false})");if(scenario!=4&&scenario!=7)model.monitors.resize(count);if(scenario==5)model.owned=false;return output;}};
    std::jthread worker([&]{configureWorkerDisplays(server.value,request,stopped,serverWrite,access);});
    bool text=false;std::vector<uint8_t> bytes;require(readPipePacket(client.value,text,bytes,stopped,std::chrono::seconds(2))&&text,"Prepared display response missing");const auto prepared=JsonParser::parse(std::string(bytes.begin(),bytes.end()));
    if(scenario==6){require(!prepared.get("ok").boolean()&&applyCalls==0,"Unprivileged worker attempted a display change");worker.join();continue;}
    require(prepared.get("type").string()=="worker.display.prepared"&&prepared.get("ok").boolean(),"Display transaction failed to prepare");
    auto commit=request;auto& object=std::get<Json::Object>(commit.value);object["type"].value=std::string(scenario==0?"worker.display.cancel":"worker.display.apply");
    if(scenario==1)object["nonce"].value=std::string(43,'b');if(scenario==2)interactive=false;
    const auto command=jsonObject(commit);require(pipePacket(client.value,true,{command.begin(),command.end()},stopped,clientWrite),"Display transaction commit write failed");
    const bool completed=readPipePacket(client.value,text,bytes,stopped,std::chrono::milliseconds(500));worker.join();
    if(scenario==1){require(!completed&&applyCalls==0,"Mismatched nonce reached display configuration");continue;}
    require(completed&&text,"Display transaction completion missing");const auto result=JsonParser::parse(std::string(bytes.begin(),bytes.end()));require(displayOperationKey(request).matches(result),"Display transaction lost operation correlation");
    if(scenario==3){require(result.get("ok").boolean()&&applyCalls==1&&probeCalls==2&&parseDisplayState(result.get("display")).monitors.size()==1,"Applied count was not confirmed by a fresh probe");}
    else if(scenario==0||scenario==2){require(!result.get("ok").boolean()&&applyCalls==0&&result.get("reason").string()==(scenario==0?"operation_cancelled":"no_interactive_session"),"Cancellation or session switch reached the OS apply action");}
    else {require(!result.get("ok").boolean()&&applyCalls==1,"Helper exit or stale/foreign topology was accepted as success");}
  }
}
struct ProbeFixturePipe {
  WinHandle server, client;
  std::atomic<bool> stopped{false};
  std::mutex writing;
  ProbeFixturePipe() {
    const auto name=L"\\\\.\\pipe\\MultitapHost.QA.Probe."+randomName();
    server=WinHandle(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));
    require(server.value!=INVALID_HANDLE_VALUE,"Probe fixture pipe creation failed");
    client=WinHandle(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
    require(client.value!=INVALID_HANDLE_VALUE,"Probe fixture connection failed");
    const BOOL connected=ConnectNamedPipe(server.value,nullptr);
    require(connected||GetLastError()==ERROR_PIPE_CONNECTED,"Probe fixture peer did not connect");
  }
  bool send(const std::string& payload,bool text=true) {
    return pipePacket(server.value,text,{payload.begin(),payload.end()},stopped,writing);
  }
};
DisplayState probeFixtureDisplay() {
  DisplayState display;display.installed=display.deviceReady=display.active=display.owned=true;
  display.foreign=0;display.width=1920;display.height=1080;display.name=L"\\\\.\\DISPLAY2";
  display.monitors={{"0",display.name,0,0,1920,1080,true}};
  return display;
}
std::string probeFixtureResponse(const DisplayState& display) {
  return "{\"type\":\"worker.display\",\"desktopAvailable\":true,\"privileged\":true,\"display\":"+workerDisplayJson(display)+'}';
}
void probeReplyRecovery() {
  static_assert(workerDisplayHelperTimeoutMs==5000);
  static_assert(workerDisplayProbeReplyTimeoutMs==15000);
  const auto ready=probeFixtureDisplay();
  const auto response=probeFixtureResponse(ready);
  {
    ProbeFixturePipe pipe;std::atomic<bool> sent{false};
    std::jthread responder([&]{std::this_thread::sleep_for(std::chrono::milliseconds(10500));sent=pipe.send(response);});
    const auto began=Clock::now();const auto result=readWorkerDisplayProbe(pipe.client.value,pipe.stopped);
    const auto elapsed=Clock::now()-began;responder.join();
    require(sent&&elapsed>std::chrono::seconds(10)&&elapsed<std::chrono::seconds(15),"Delayed probe fixture did not cross the old deadline inside the new bound");
    require(result.display.ready()&&result.desktopAvailable&&result.privileged&&!result.cancelled,"Healthy slow worker was discarded after the old ten-second deadline");
  }
  {
    ProbeFixturePipe pipe;const auto began=Clock::now();
    const auto result=readWorkerDisplayProbe(pipe.client.value,pipe.stopped,150);
    require(!result.display.ready()&&result.display.error=="viewport_probe_timeout"&&Clock::now()-began<std::chrono::seconds(1),"Silent probe timeout was unbounded or advertised ready");
  }
  {
    ProbeFixturePipe pipe;
    std::jthread responder([&]{
      std::this_thread::sleep_for(std::chrono::milliseconds(180));
      uint32_t length=uint32_t(response.size()+1);BYTE kind=0;
      if(!pipeBytes(pipe.server.value,true,&length,4,pipe.stopped)||!pipeBytes(pipe.server.value,true,&kind,1,pipe.stopped))return;
      // The body follows long after the reader's 250 ms absolute deadline even
      // when the reader starts late on a loaded machine; stop ends the wait.
      for(int n=0;n<300&&!pipe.stopped;++n)std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if(pipe.stopped)return;
      auto body=response;
      pipeBytes(pipe.server.value,true,body.data(),DWORD(body.size()),pipe.stopped);
    });
    const auto began=Clock::now();const auto result=readWorkerDisplayProbe(pipe.client.value,pipe.stopped,250);
    pipe.stopped=true;responder.join();
    require(result.display.error=="viewport_probe_timeout"&&!result.display.ready()&&Clock::now()-began<std::chrono::seconds(1),"Partial worker reply reset the absolute message deadline");
  }
  {
    ProbeFixturePipe pipe;std::jthread cancel([&]{std::this_thread::sleep_for(std::chrono::milliseconds(100));pipe.stopped=true;});
    const auto began=Clock::now();const auto result=readWorkerDisplayProbe(pipe.client.value,pipe.stopped);cancel.join();
    require(result.cancelled&&!result.display.ready()&&Clock::now()-began<std::chrono::seconds(1),"Probe cancellation waited for the fifteen-second reply deadline");
  }
  for(const auto& payload:std::vector<std::string>{"{broken","{\"type\":\"wrong\"}","{\"type\":\"worker.display\",\"desktopAvailable\":true,\"privileged\":true,\"display\":{}}",std::string(65537,'x')}) {
    ProbeFixturePipe pipe;std::atomic<bool> sent{false};std::jthread responder([&]{sent=pipe.send(payload);});
    const auto result=readWorkerDisplayProbe(pipe.client.value,pipe.stopped,2000);responder.join();
    require(sent&&!result.display.ready()&&result.display.error=="viewport_probe_failed","Malformed or oversized worker probe became ready or lost its failure reason");
  }
  {
    ProbeFixturePipe pipe;std::jthread responder([&]{pipe.send(response,false);});
    const auto result=readWorkerDisplayProbe(pipe.client.value,pipe.stopped,2000);responder.join();
    require(result.display.error=="viewport_probe_failed"&&!result.display.ready(),"Binary worker probe was accepted as JSON display evidence");
  }
  {
    auto rdp=ready;rdp.active=false;rdp.monitors.clear();rdp.remoteSession=true;rdp.error="interactive_console_required";
    ProbeFixturePipe pipe;std::jthread responder([&]{pipe.send(probeFixtureResponse(rdp));});
    const auto result=readWorkerDisplayProbe(pipe.client.value,pipe.stopped,2000);responder.join();
    require(result.display.remoteSession&&!result.display.ready()&&result.display.error=="interactive_console_required", "RDP/console deferral was mislabeled as worker timeout or readiness");
  }
  {
    DisplayReadinessCache cache;const auto old=cache.begin(2);cache.invalidate();
    require(!cache.complete(old,ready),"A cancelled old worker resurrected invalidated display evidence");
    auto timeout=DisplayState{};timeout.error="viewport_probe_timeout";const auto failed=cache.begin(2);cache.complete(failed,timeout);
    require(!cache.snapshot(2,true).ready(),"Failed probe kept an old ready display");
    ProbeFixturePipe pipe;std::jthread responder([&]{pipe.send(response);});
    const auto result=readWorkerDisplayProbe(pipe.client.value,pipe.stopped,2000);responder.join();
    const auto fresh=cache.begin(2);require(cache.complete(fresh,result.display,Clock::now(),result.desktopAvailable)&&cache.snapshot(2,true).ready(),"A fresh valid probe did not recover without an OS restart");
  }
}
void readinessRecovery(){
  DisplayState display;display.installed=display.deviceReady=display.active=display.owned=true;display.foreign=0;display.width=1920;display.height=1080;display.name=L"\\\\.\\DISPLAY2";display.monitors.push_back({"0",display.name,0,0,1920,1080,true});
  DisplayReadinessCache cache;const DisplayProbeKey key{};const auto begun=Clock::now();const auto before=cache.begin(2);require(cache.complete(before,display,begun)&&cache.snapshot(2,true).ready(),"Fresh session probe not ready");
  cache.invalidate();require(!cache.snapshot(2,true).ready(),"Session change retained stale ready display");require(!cache.complete(before,display,begun),"Late probe resurrected invalidated topology");
  const auto after=cache.begin(3);require(cache.complete(after,display,begun)&&cache.snapshot(3,true).ready(),"New session did not recover");require(!cache.snapshot(2,true).ready()&&!cache.snapshot(3,false).ready(),"Foreign or locked session inherited readiness");
  cache.complete(after,display,begun,false);require(cache.snapshot(3,true).ready()&&!cache.captureAllowed(3,true)&&cache.due(3,key,begun+std::chrono::seconds(6)),"Secure desktop was advertised as capturable");
  const auto failed=cache.begin(3);cache.complete(failed,DisplayState{},begun);require(!cache.snapshot(3,true).ready()&&cache.due(3,key,begun+std::chrono::seconds(6)),"Failed worker did not become eligible for reprobe");
  {
    DisplayReadinessCache activation;auto staged=display;
    staged.viewportProvisioning=false;staged.monitorControlSupported=false;
    const auto ticket=activation.begin(3,key);
    require(activation.complete(ticket,staged,begun,true,true)&&activation.due(3,key,begun),"Committed activation kept pre-ACK capabilities for thirty seconds");
    const auto partial=activation.snapshot(3,true);
    require(partial.ready()&&!partial.viewportProvisioning&&!partial.monitorControlSupported,"Reprobe scheduling fabricated viewport/count readiness");
    auto fresh=display;fresh.viewportProvisioning=fresh.monitorControlSupported=true;
    require(activation.complete(activation.begin(3,key),fresh,begun+std::chrono::seconds(1))&&!activation.due(3,key,begun+std::chrono::seconds(2)),"Fresh post-ACK capabilities remained in a busy probe loop");
    require(activation.snapshot(3,true).viewportProvisioning&&activation.snapshot(3,true).monitorControlSupported,"Fresh post-ACK capability flags were lost");
    const auto old=activation.begin(3);activation.invalidate();
    require(!activation.complete(old,fresh,begun,true,true)&&!activation.snapshot(3,true).ready(),"Retired activation revived an old session");
    require(activation.complete(activation.begin(4,key),fresh,begun)&&!activation.due(4,key,begun+std::chrono::seconds(2)),"Invalidation retained the previous activation refresh marker");
  }
  // backlog #70: a ready observation is kept until a change signal or a
  // minute (it was probed again every 30 s, a worker process each time), and a
  // probe that found nothing new is not announced.
  {
    using std::chrono::seconds;DisplayReadinessCache kept;const DisplayProbeKey signals{7,9};bool changed=false;
    require(kept.complete(kept.begin(3,signals),display,begun,true,false,&changed)&&changed,"A first observation was not announced");
    require(!kept.due(3,signals,begun+seconds(31))&&!kept.due(3,signals,begun+seconds(59)),"An unchanged ready display was probed again before a minute");
    require(kept.due(3,signals,begun+seconds(60)),"A ready display outlived a minute");
    require(kept.due(3,{8,9},begun+seconds(1))&&kept.due(3,{7,10},begun+seconds(1)),"A session or display change kept the observation");
    require(kept.complete(kept.begin(3,signals),display,begun+seconds(1),true,false,&changed)&&!changed,"An unchanged probe result was announced");
    auto moved=display;moved.width=moved.monitors[0].width=1280;
    require(kept.complete(kept.begin(3,signals),moved,begun+seconds(2),true,false,&changed)&&changed,"A changed probe result was not announced");
    require(kept.complete(kept.begin(3,signals),moved,begun+seconds(3),false,false,&changed)&&changed,"A desktop that became unavailable was not announced");
    kept.invalidate();require(kept.complete(kept.begin(3,signals),moved,begun+seconds(4),false,false,&changed)&&changed,"A probe after an invalidation was not announced");
    // A signal during the probe: its key was read before it, so it is due again.
    require(kept.complete(kept.begin(3,signals),moved,begun+seconds(5))&&kept.due(3,{7,11},begun+seconds(6)),"A change during a probe was kept as seen");
    // Stream and display operation results take the signals of their start: a
    // display operation's own change makes the next report probe.
    const auto operation=kept.begin(3);noteHostDisplayChange();
    require(kept.complete(operation,moved,begun+seconds(5))&&kept.due(3,displayProbeKey(),begun+seconds(6)),"A result begun before a display change was kept as current");
    require(kept.complete(kept.begin(3,signals),DisplayState{},begun)&&!kept.due(3,signals,begun+seconds(4))&&kept.due(3,signals,begun+seconds(5)),"A not-ready display lost its 5 s re-probe");
  }
  require(workerSessionMustRestart(3,3,true,{{WTS_REMOTE_CONNECT,3}}),"Same-ID RDP reconnect kept old worker");require(workerSessionMustRestart(3,4,true,{}),"Changed interactive session kept old worker");require(workerSessionMustRestart(3,3,false,{}),"Locked session kept old worker");require(!workerSessionMustRestart(3,3,true,{{WTS_SESSION_LOGON,4}}),"Unrelated user event interrupted worker");
  // The reason a session change gives the client for the ended stream.
  require(std::string(workerSessionStopReason(3,3,false,true))=="no_interactive_session","A locked session was reported as a capture failure");
  require(std::string(workerSessionStopReason(3,3,true,false))=="no_interactive_session","The console moving to RDP was reported as a capture failure");
  require(std::string(workerSessionStopReason(3,4,true,true))=="no_interactive_session","A user switch was reported as a capture failure");
  require(std::string(workerSessionStopReason(3,0xffffffff,false,false))=="no_interactive_session","A sign-out was reported as a capture failure");
  require(std::string(workerSessionStopReason(3,3,true,true))=="capture_failed","A desktop change in the same unlocked console lost its capture reason");
}
// serviceMain's loop steps run contained: a throwing step neither stops the
// service nor floods the journal; it is retried every tick and its recovery is
// recorded. (An escaped exception used to end the service with an error the
// SCM recovery actions ignore.)
void serviceLoopContainment(){
  std::vector<std::tuple<uint32_t,bool,uint32_t>> records;
  ServiceLoopGuard guard([&](uint32_t step,bool ok,uint32_t failures){records.emplace_back(step,ok,failures);},60000);
  int ran=0;
  require(guard.run(ServiceLoopStep::capabilities,[&]{++ran;},1000)&&ran==1&&records.empty(),"A healthy loop step was not run or was reported");
  int attempts=0;
  for(uint64_t tick=0;tick<130;++tick)require(!guard.run(ServiceLoopStep::displayV2Recovery,[&]{++attempts;throw std::runtime_error("fixture");},10000+tick*1000),"A throwing loop step escaped or reported success");
  require(attempts==130&&guard.failures(ServiceLoopStep::displayV2Recovery)==130&&guard.failures(ServiceLoopStep::capabilities)==0,"A failing step was not retried every tick or affected another step");
  using Record=std::tuple<uint32_t,bool,uint32_t>;
  require(records==std::vector<Record>{{5,false,1},{5,false,61},{5,false,121}},"Loop step failures were not recorded once per window with their fixed step code");
  require(guard.run(ServiceLoopStep::displayV2Recovery,[]{},200000)&&records.size()==4&&records.back()==Record{5,true,130}&&guard.failures(ServiceLoopStep::displayV2Recovery)==0,"A recovered loop step did not record its recovery and reset");
  require(!guard.run(ServiceLoopStep::sessionChange,[]{throw 42;},300000)&&records.back()==Record{1,false,1},"A non-standard exception escaped the loop step guard");
  ServiceLoopGuard throwingRecorder([](uint32_t,bool,uint32_t){throw std::runtime_error("journal");});
  require(!throwingRecorder.run(ServiceLoopStep::reload,[]{throw std::runtime_error("fixture");},1)&&throwingRecorder.run(ServiceLoopStep::reload,[]{},2),"A failing diagnostic record escaped the loop step guard");
}
void ownedCaptureSelection(){
  DisplayState display;display.installed=display.deviceReady=display.active=display.owned=true;display.foreign=0;display.width=1000;display.height=1000;display.name=L"\\\\.\\DISPLAY2";display.monitors={{"0",L"\\\\.\\DISPLAY2",1000,0,1000,1000,false},{"1",L"\\\\.\\DISPLAY3",2000,0,1000,1000,false}};
  std::vector<CaptureMonitor> observed{{L"\\\\.\\DISPLAY1",L"console",{0,0,1000,1000}},{L"\\\\.\\DISPLAY2",L"owned-A",{1000,0,2000,1000}},{L"\\\\.\\DISPLAY3",L"owned-B",{2000,0,3000,1000}}};
  const auto selected=selectOwnedCapture(display,"all",observed);require(selected.monitors.size()==2&&selected.bounds.left==1000&&selected.bounds.right==3000,"All selection included console or missed owned monitors");require(selectOwnedCapture(display,"0",observed).monitors.size()==1,"Single owned monitor selection failed");
  const auto rejects=[&](auto work){bool rejected=false;try{work();}catch(...){rejected=true;}require(rejected,"Foreign or noncontiguous capture area was accepted");};
  auto gapped=display;gapped.monitors[1].left=3000;auto gapObserved=observed;gapObserved[2].bounds={3000,0,4000,1000};gapObserved.push_back({L"\\\\.\\DISPLAY4",L"second-console",{2000,0,3000,1000}});rejects([&]{selectOwnedCapture(gapped,"all",gapObserved);});
  auto cloned=observed;cloned.push_back({L"\\\\.\\DISPLAY4",L"console-clone",{1000,0,2000,1000}});rejects([&]{selectOwnedCapture(display,"0",cloned);});
  auto replaced=observed;replaced[1].identity=L"foreign-adapter";rejects([&]{validateCaptureSelection(selected,replaced);});
  auto foreign=display;foreign.owned=false;rejects([&]{selectOwnedCapture(foreign,"0",observed);});rejects([&]{selectOwnedCapture(display,"2",observed);});
  auto vertical=display;vertical.monitors[1].left=1000;vertical.monitors[1].top=1000;auto verticalObserved=observed;verticalObserved[2].bounds={1000,1000,2000,2000};require(selectOwnedCapture(vertical,"all",verticalObserved).bounds.bottom==2000,"Adjacent vertical owned monitors failed");
  auto diagonal=display;diagonal.monitors[1].top=1000;auto diagonalObserved=observed;diagonalObserved[2].bounds={2000,1000,3000,2000};rejects([&]{selectOwnedCapture(diagonal,"all",diagonalObserved);});
}
void pipeRoundTrip(){
  const auto name=L"\\\\.\\pipe\\MultitapHost.QA."+randomName();WinHandle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));require(server.value!=INVALID_HANDLE_VALUE,"Own QA pipe creation failed");
  std::atomic<bool> stopped{false};std::promise<bool> clientResult;auto result=clientResult.get_future();
  std::thread client([&]{try{WinHandle pipe(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));require(pipe.value!=INVALID_HANDLE_VALUE,"Own QA client open failed");std::vector<uint8_t> payload(256*1024,0x5a);std::mutex mutex;require(pipePacket(pipe.value,false,payload,stopped,mutex),"Bounded real pipe write failed");bool text=true;std::vector<uint8_t> returned;require(readPipePacket(pipe.value,text,returned,stopped)&&!text&&returned==payload,"Bounded pipe roundtrip differed");clientResult.set_value(true);}catch(...){clientResult.set_value(false);}});
  WinHandle connected(CreateEventW(nullptr,TRUE,FALSE,nullptr));OVERLAPPED operation{};operation.hEvent=connected.value;BOOL ready=ConnectNamedPipe(server.value,&operation);if(!ready&&GetLastError()==ERROR_IO_PENDING){require(WaitForSingleObject(connected.value,3000)==WAIT_OBJECT_0,"Own QA pipe connect timeout");DWORD count=0;ready=GetOverlappedResult(server.value,&operation,&count,FALSE);}else if(!ready&&GetLastError()==ERROR_PIPE_CONNECTED)ready=TRUE;
  bool text=true;std::vector<uint8_t> bytes;std::mutex mutex;bool passed=ready&&readPipePacket(server.value,text,bytes,stopped)&&!text&&bytes.size()==256*1024&&pipePacket(server.value,false,bytes,stopped,mutex);client.join();require(passed&&result.get(),"Real pipe transfer failed");
}
void pipeDeadline(){
  const auto name=L"\\\\.\\pipe\\MultitapHost.QA.Deadline."+randomName();WinHandle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr));require(server.value!=INVALID_HANDLE_VALUE,"Deadline QA pipe creation failed");
  WinHandle client(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));require(client.value!=INVALID_HANDLE_VALUE,"Deadline QA client failed");ConnectNamedPipe(server.value,nullptr);
  std::atomic<bool> stopped{false};bool text=false;std::vector<uint8_t> bytes;const auto began=Clock::now();require(!readPipePacket(server.value,text,bytes,stopped,std::chrono::milliseconds(150)),"Silent worker read never timed out");require(Clock::now()-began<std::chrono::seconds(2),"Silent worker read exceeded its bound");
}
void machineDpapi(){std::string dummy="not-a-real-host-credential";DATA_BLOB source{DWORD(dummy.size()),reinterpret_cast<BYTE*>(dummy.data())},sealed{},plain{};winCheck(CryptProtectData(&source,L"Multitap QA dummy",nullptr,nullptr,nullptr,CRYPTPROTECT_LOCAL_MACHINE|CRYPTPROTECT_UI_FORBIDDEN,&sealed),"Dummy machine encryption");DATA_BLOB encrypted{sealed.cbData,sealed.pbData};winCheck(CryptUnprotectData(&encrypted,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain),"Dummy machine decryption");const bool matches=plain.cbData==dummy.size()&&std::memcmp(plain.pbData,dummy.data(),dummy.size())==0;SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);LocalFree(sealed.pbData);require(matches,"Machine DPAPI roundtrip failed");}
void streamPowerActivity(){
  std::vector<EXECUTION_STATE> calls;
  {StreamPowerActivity guard([&](EXECUTION_STATE flags){calls.push_back(flags);return EXECUTION_STATE(ES_CONTINUOUS);});require(guard.active(),"Power activity success was not recorded");}
  require(calls==std::vector<EXECUTION_STATE>{ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED,ES_CONTINUOUS},"Power activity flags were not restored");calls.clear();
  {StreamPowerActivity guard([&](EXECUTION_STATE flags){calls.push_back(flags);return EXECUTION_STATE(0);});require(!guard.active(),"Failed power activity was reported as successful");}require(calls.size()==1,"Failed power request changed prior state on cleanup");
  calls.clear();std::vector<EXECUTION_STATE> returned;
  {StreamPowerActivity guard([&](EXECUTION_STATE flags){calls.push_back(flags);const auto old=SetThreadExecutionState(flags);returned.push_back(old);return old;});require(guard.active(),"Actual scoped Windows power activity request failed");}
  require(calls.size()==2&&returned.size()==2&&calls[1]==returned[0]&&returned[1]==calls[0],"Actual Windows power activity was not restored to its prior flags");
}
}
int wmain(int argc,wchar_t** argv){try{if(argc==2&&std::wstring(argv[1])==L"--probe-recovery-only"){probeReplyRecovery();std::cout<<"{\"probeRecoveryCases\":11,\"actualOwnedPipes\":true,\"serviceOrDriverChanges\":false,\"pass\":true}\n";return 0;}if(argc==2&&std::wstring(argv[1])==L"--helper-fixture-large"){std::cout<<"{\"payload\":\""<<std::string(48000,'x')<<"\"}\n";return 0;}if(argc==2&&std::wstring(argv[1])==L"--helper-fixture-silent"){Sleep(10000);return 0;}if(argc==2&&std::wstring(argv[1])==L"--topology-read-benchmark"){const auto start=Clock::now();size_t monitors=0;for(int i=0;i<10;i++)monitors=currentCaptureMonitors().size();const double milliseconds=std::chrono::duration<double,std::milli>(Clock::now()-start).count()/10;std::cout<<"{\"kind\":\"actual-monitor-enumeration\",\"iterations\":10,\"monitors\":"<<monitors<<",\"averageMs\":"<<milliseconds<<",\"capturePerformed\":false,\"windowsSettingsChanged\":false}\n";return 0;}serviceStatusReadOnly();displayGate();displayOperationPolicy();displayWorkerTransactions();const bool helperIo=helperProcesses();readinessRecovery();serviceLoopContainment();ownedCaptureSelection();pipeRoundTrip();pipeDeadline();machineDpapi();streamPowerActivity();std::cout<<"{\"kind\":\"native-service-components\",\"actualReadOnlyStatusRpc\":true,\"actualAbsentServiceProbe\":true,\"virtualDisplayOwnershipGates\":4,\"displayOperationPolicyCases\":14,\"realPipeDisplayTransactionsWithFakeOs\":8,\"realHelperDrainBytes\":"<<(helperIo?48000:0)<<",\"realHelperTimeoutCleanup\":"<<(helperIo?"true":"null")<<",\"helperProcessIoExcludedByNativeIsolation\":"<<(helperIo?"false":"true")<<",\"sessionRecoveryPolicyCases\":16,\"serviceLoopStepsContained\":true,\"ownedCaptureSelectionCases\":9,\"staleProbeRejected\":true,\"silentWorkerPipeDeadline\":true,\"realNamedPipeBytes\":262144,\"machineDpapiDummyRoundtrip\":true,\"scopedPowerActivityApiAndRestore\":true,\"powerPlanChanged\":false,\"windowsRdpChanged\":false,\"serviceInstalled\":false}\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
