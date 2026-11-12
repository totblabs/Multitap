// Reuse the SDK's in-memory transport/production service fixture. Its original
// entry point is not executed by this target. No installed SDK/device is used.
#define main sessionStopPriorFixtureMain
#include "native_display_service_change_test.cpp"
#undef main
#include "service.hpp"
#include "wake_counter.hpp"

namespace host {
struct SessionStopTestAccess {
  static void state(ServiceBridge& bridge,const std::string& id,bool quiescent){
    bridge.id_=id;bridge.sessionModeIsStream_=true;bridge.workerQuiescent_=quiescent;
    bridge.nativeStopGuard_=false;bridge.profileSession_=false;
  }
  static void rejected(ServiceBridge& bridge,const std::string& id){bridge.rejectedStream_=id;}
  static bool sameCurrent(ServiceBridge& bridge,const std::string& id){return bridge.id_==id;}
  static void unobservedNative(ServiceBridge& bridge){bridge.nativeStopGuard_=true;bridge.nativeStopInstance_.clear();}
  // A display manager v2 stream (single output or independent tracks) with a
  // console proof: native, profile-carrying, worker created; no reservation.
  static void v2Stream(ServiceBridge& bridge,const std::string& id,bool quiescent){
    state(bridge,id,quiescent);bridge.v2Session_=true;bridge.nativeStopGuard_=true;bridge.nativeStopInstance_="ROOT\\DISPLAY\\00a1";bridge.sessionHadProfile_=true;bridge.workerCreatedForSession_=true;
  }
  // The stream thread's actual completion for a created/dispatched worker whose
  // exit was not observed in its bounds; `process` is the retained wait handle.
  static void unexitedWorker(ServiceBridge& bridge,const std::string& id,bool v1Profile,HANDLE process){
    state(bridge,id,false);bridge.v2Session_=!v1Profile;bridge.profileSession_=v1Profile;
    bridge.completeProfileWorkerExit(false,true,true,process);
    bridge.profileSession_=false;
  }
  static bool quiescentNow(ServiceBridge& bridge){return bridge.workerQuiescentNow();}
  static void qemuUnexited(ServiceBridge& bridge,const DisplayOperationKey& key,HANDLE process){
    auto op=std::make_shared<ServiceBridge::ConsoleOperation>();op->key=key;op->qemu=true;op->finished=true;op->result={false,true,"qemu_console_unavailable"};op->unexited=WinHandle(process);
    bridge.consoleOperation_=std::move(op);bridge.consoleRunning_=true;
  }
  static void qemuModel(ServiceBridge& bridge,const DisplayOperationKey& key,bool finished,bool quiet,bool held){
    auto op=std::make_shared<ServiceBridge::ConsoleOperation>();op->key=key;op->qemu=true;op->finished=finished;op->quiet=quiet;op->result={held,held,held?"qemu_console_restored":"qemu_console_unavailable"};
    if(held){op->hold.begin(uint64_t(streamMonotonicMillis()));op->hadHold=true;}bridge.consoleOperation_=std::move(op);bridge.consoleRunning_=true;bridge.qemuAdmissionModel_=true;
  }
  static uint64_t holdDeadline(ServiceBridge& bridge){return bridge.consoleOperation_->hold.deadline();}
  static bool holding(ServiceBridge& bridge){return bridge.qemuConsoleHolding();}
  static bool expireHold(ServiceBridge& bridge){return bridge.qemuConsoleQuiescent(holdDeadline(bridge));}
  static bool probeBlocked(ServiceBridge& bridge){bridge.probeDisplay(1,true);return bridge.qemuProbeAdmissions_==0&&!bridge.probe_.joinable()&&!bridge.probing_;}
  static bool probeAdmitted(ServiceBridge& bridge){const unsigned before=bridge.qemuProbeAdmissions_;bridge.probeDisplay(1,true);return bridge.qemuProbeAdmissions_==before+1;}
  static bool consoleRestorePending(ServiceBridge& bridge){return bridge.consoleRestorePending_;}
  // boot-ready-v1: probes the loop's boot probe admits (the model starts no worker).
  static unsigned bootProbeAdmissions(ServiceBridge& bridge){bridge.qemuAdmissionModel_=true;const unsigned before=bridge.qemuProbeAdmissions_;bridge.bootProbe();return bridge.qemuProbeAdmissions_-before;}
  static void observedNotReady(ServiceBridge& bridge,DWORD session){bridge.displayCache_.complete(bridge.displayCache_.begin(session),DisplayState{},Clock::now(),false);}
  // A probe's ready result under the change signals of now (backlog #70).
  static void observedReady(ServiceBridge& bridge,DWORD session){
    DisplayState display;display.installed=display.deviceReady=display.active=display.owned=true;display.foreign=0;display.width=1920;display.height=1080;display.name=L"\\\\.\\DISPLAY2";display.monitors.push_back({"0",display.name,0,0,1920,1080,true});
    bridge.displayCache_.complete(bridge.displayCache_.begin(session,displayProbeKey()),display,Clock::now(),true);
  }
  static void completeQemuModel(ServiceBridge& bridge){auto& op=*bridge.consoleOperation_;op.finished=true;op.quiet=true;op.result={false,true,"qemu_console_cancelled"};}
  static bool streamAdmitted(ServiceBridge& bridge){return bridge.qemuStreamAdmissions_==1;}
  // capability_inputs.hpp: substituted session view and console identities; the
  // admission model keeps any display probe from starting a worker.
  static void capabilityAccess(ServiceBridge& bridge,CapabilityInputAccess access){bridge.capabilityInputs_.access_=std::move(access);bridge.capabilityInputs_.invalidate();bridge.qemuAdmissionModel_=true;}
  static uint64_t capabilityInvalidations(ServiceBridge& bridge){return bridge.capabilityInputs_.invalidations();}
  static void activeStream(ServiceBridge& bridge,DWORD session,bool active){bridge.session_=session;bridge.stopped_=!active;bridge.streaming_=active;}
  static void admissionModel(ServiceBridge& bridge){bridge.qemuAdmissionModel_=true;}
  // qemu_console_route.hpp: the Windows session a console request finds and this
  // machine's identity. Never the worker's state: that would start a real worker.
  static void consoleSession(ServiceBridge& bridge,QemuConsoleSessionFacts facts,std::string machine){
    if(qemuConsoleRoute(facts)==QemuConsoleRoute::worker)throw std::runtime_error("The park fixture must not reach a real worker");
    bridge.qemuFactsFixture_=[facts]{return facts;};bridge.qemuMachineFixture_=[machine]{return machine;};bridge.qemuAdmissionModel_=true;
  }
  static void machine(ServiceBridge& bridge,std::string machine){bridge.qemuMachineFixture_=[machine]{return machine;};}
  // display_v2_state_rpc.hpp through the bridge: the file and the Windows session view are substituted.
  static void stateFixture(ServiceBridge& bridge,std::function<DisplayV2Store()> store,std::function<bool(DWORD,bool)> session){bridge.displayV2StateStoreFixture_=std::move(store);bridge.displayV2StateSessionFixture_=std::move(session);}
  static DisplayV2Dispatch statePacket(ServiceBridge& bridge,const Json& value,bool authorized,DWORD session){return bridge.displayV2Packet(value,authorized,session);}
  static void invalidateInputs(ServiceBridge& bridge){bridge.capabilityInputs_.invalidate();}
};
}
namespace {
using namespace host;
int stopChecks=0;
void stopCheck(bool value,const char* reason){++stopChecks;if(!value)throw std::runtime_error(reason);}
const std::string stopSessionId="00000000-0000-0000-0000-000000000001",stopRequestId="00000000-0000-0000-0000-000000000002",otherId="00000000-0000-0000-0000-000000000003";
Json stopCommand(bool status=false){return JsonParser::parse("{\"type\":"+jsonQuote(status?"host.session.stop.status":"session.stop")+",\"sessionId\":"+jsonQuote(stopSessionId)+",\"sessionStopProtocol\":\"stop-v1\",\"stopRequestId\":"+jsonQuote(stopRequestId)+(status?"":",\"reason\":\"client_stopped\"")+'}');}
struct StopBridge final:AgentBridge {
  int starts=0,stops=0,queries=0,qemuCommands=0;SessionStopEvidence proof{true,true,false};
  void start(const Json&,Send)override{++starts;}void control(const Json&)override{}
  void stop()override{++stops;}std::string capabilities()override{return "{\"service\":true}";}
  void displayControl(const Json&,Send)override{}
  void qemuConsoleControl(const Json&,Send)override{++qemuCommands;}
  SessionStopEvidence stopSession(const std::string&)override{++stops;return proof;}
  SessionStopEvidence stoppedSession(const std::string&)override{++queries;return proof;}
};
void protocolCases(){
  for(const bool status:{false,true}){
    const auto original=stopCommand(status);stopCheck(parseSessionStopRequest(original).has_value(),"Valid stop correlation rejected");
    for(int fault=0;fault<9;++fault){auto changed=original;auto& fields=std::get<Json::Object>(changed.value);
      switch(fault){case 0:fields.erase("stopRequestId");break;case 1:streamField(changed,"stopRequestId",std::string("bad"));break;case 2:streamField(changed,"sessionId",otherId+'x');break;case 3:streamField(changed,"sessionStopProtocol",std::string("future"));break;case 4:streamField(changed,"extra",1);break;case 5:streamField(changed,"type",std::string("session.start"));break;case 6:fields["sessionId"].value=true;break;case 7:fields["stopRequestId"].value=true;break;case 8:if(status)streamField(changed,"reason",std::string("client_stopped"));else fields["reason"].value=false;break;}
      stopCheck(!parseSessionStopRequest(changed),"Malformed stop reached lifecycle mutation");
    }
  }
  SessionStopBinding binding;const auto stop=*parseSessionStopRequest(stopCommand()),query=*parseSessionStopRequest(stopCommand(true));
  stopCheck(!binding.retire(stop,otherId)&&!binding.query(query),"Foreign session bound retirement");
  stopCheck(binding.retire(stop,stopSessionId)&&binding.query(query),"Exact retirement/query failed");
  auto stale=query;stale.request=otherId;stopCheck(!binding.query(stale),"Stale query nonce accepted");
  const auto reply=JsonParser::parse(sessionStopPacket(stop,{false,true,true}));
  stopCheck(!reply.get("workerQuiescent").boolean()&&!reply.get("profileRestored").boolean(),"Unverified process exit became restored ACK");
}
void agentCases(){
  StopBridge bridge;std::vector<Json> packets;
  Agent agent([](const std::wstring&){},&bridge,[&](bool text,std::vector<uint8_t> bytes){if(text)packets.push_back(JsonParser::parse(std::string(bytes.begin(),bytes.end())));});
  const auto start=[&](const std::string& id,const std::string& mode){agent.localControl("{\"type\":\"session.start\",\"sessionId\":"+jsonQuote(id)+",\"mode\":"+jsonQuote(mode)+'}');};
  start(stopSessionId,"stream");const int stopped=bridge.stops;
  auto wrong=stopCommand();streamField(wrong,"sessionId",otherId);agent.localControl(jsonObject(wrong));
  stopCheck(bridge.stops==stopped&&packets.empty(),"Foreign stop terminated current capture");
  agent.localControl(jsonObject(stopCommand()));stopCheck(bridge.stops==stopped+1,"Exact stop did not stop bridge once");
  stopCheck(packets.size()==2&&packets.front().get("type").string()=="host.session.stopped"&&!packets.front().get("profileRestored").boolean(),"Cleanup result lost or fabricated");
  bridge.proof.profileRestored=true;agent.localControl(jsonObject(stopCommand(true)));
  stopCheck(bridge.queries==1&&packets[2].get("profileRestored").boolean()&&packets[3].get("type").string()=="host.capabilities","Status completion omitted ordered fresh capabilities");
  start(otherId,"stream");const int laterStops=bridge.stops;
  agent.localControl(jsonObject(stopCommand()));stopCheck(bridge.stops==laterStops,"Late stop of the earlier session terminated the later one");
  agent.localControl(jsonObject(stopCommand(true)));stopCheck(bridge.stops==laterStops&&bridge.queries==2,"Old cleanup status mutated the later session");
  for(const auto* type:{"host.qemu-console.prepare","host.qemu-console.cancel","host.qemu-console.status","host.qemu-console.renew"})agent.localControl("{\"type\":"+jsonQuote(type)+'}');
  stopCheck(bridge.qemuCommands==4,"QEMU preparation/status/cancel/renew did not reach service bridge");
}
void serviceCases(){
  ServiceBridge service(Credentials{});
  SessionStopTestAccess::state(service,stopSessionId,false);
  stopCheck(!service.stopSession(stopSessionId).workerQuiescent,"Nonjoinable thread was mistaken for verified worker-process exit");
  SessionStopTestAccess::state(service,stopSessionId,true);
  const auto clean=service.stopSession(stopSessionId);
  stopCheck(clean.workerQuiescent&&clean.profileRestored&&!clean.profileRequired,"Proved legacy worker retirement rejected");
  stopCheck(!service.stoppedSession(otherId).workerQuiescent,"Different session inherited retirement proof");
  SessionStopTestAccess::state(service,otherId,false);SessionStopTestAccess::rejected(service,stopSessionId);
  stopCheck(!service.stopSession(stopSessionId).workerQuiescent&&SessionStopTestAccess::sameCurrent(service,otherId),"Rejected request hid an older unverified process or stopped its owner");
  SessionStopTestAccess::state(service,otherId,true);
  const auto rejected=service.stopSession(stopSessionId);
  stopCheck(rejected.workerQuiescent&&rejected.profileRestored&&SessionStopTestAccess::sameCurrent(service,otherId),"Unstarted rejection stranded its reservation");
  // boot-ready-v1: a readiness change wakes the service loop at once (it used to
  // wait for the loop's next one-second pass before announcing capabilities).
  {ServiceBridge woken(Credentials{});WinHandle wake(CreateEventW(nullptr,FALSE,FALSE,nullptr));stopCheck(wake.value!=nullptr,"Own wake event unavailable");woken.readinessWake(wake.value);
    stopCheck(WaitForSingleObject(wake.value,0)==WAIT_TIMEOUT,"Registering the wake event woke the loop");
    woken.maintenanceChanged();stopCheck(WaitForSingleObject(wake.value,0)==WAIT_OBJECT_0&&woken.readinessChanged(),"A readiness change waited for the next service loop pass");}
  // boot-ready-v1: the loop's boot probe reads the live session and keeps the
  // existing probe gates, including the cache's 5 s re-probe interval.
  {ServiceBridge booting(Credentials{});const DWORD session=interactiveSession();const bool unlocked=sessionUnlocked(session);
    stopCheck(SessionStopTestAccess::bootProbeAdmissions(booting)==(unlocked?1u:0u),"The boot probe skipped the live session or its probe gates");
    SessionStopTestAccess::observedNotReady(booting,session);stopCheck(SessionStopTestAccess::bootProbeAdmissions(booting)==0,"The boot probe re-probed a fresh observation before it was due");}
  // backlog #70: a ready observation keeps the probe (a worker process) away
  // until a display or session change signal; each one makes it due at once.
  {ServiceBridge kept(Credentials{});const DWORD session=interactiveSession();
    if(sessionUnlocked(session)){
      SessionStopTestAccess::observedReady(kept,session);stopCheck(SessionStopTestAccess::bootProbeAdmissions(kept)==0,"A kept ready observation was probed again");
      noteHostDisplayChange();stopCheck(SessionStopTestAccess::bootProbeAdmissions(kept)==1,"A display change did not make the probe due");
      SessionStopTestAccess::observedReady(kept,session);noteHostSessionChange();stopCheck(SessionStopTestAccess::bootProbeAdmissions(kept)==1,"A session change did not make the probe due");
    }}
  ServiceBridge unstarted(Credentials{});SessionStopTestAccess::state(unstarted,stopSessionId,true);SessionStopTestAccess::unobservedNative(unstarted);
  stopCheck(unstarted.stopSession(stopSessionId).profileRestored,"Unobserved native device fabricated a mutation for an unstarted worker");
  // v2 (tracks too): the worker's own stop restored the state file; a failed
  // restore belongs to automatic recovery, so the broker's barrier follows
  // only the verified worker exit. No v1 profile reservation is consulted.
  for(const bool quiescent:{false,true}){
    ServiceBridge v2(Credentials{});SessionStopTestAccess::v2Stream(v2,stopSessionId,quiescent);
    const auto evidence=v2.stopSession(stopSessionId);
    stopCheck(evidence.workerQuiescent==quiescent&&evidence.profileRequired&&evidence.profileRestored==quiescent,"v2 stream stop evidence does not follow the verified worker exit");
    const auto later=v2.stoppedSession(stopSessionId);stopCheck(later.workerQuiescent==quiescent&&later.profileRestored==quiescent,"v2 stop status differs from the stop evidence");
    stopCheck(!v2.stoppedSession(otherId).workerQuiescent,"Another session inherited v2 stop evidence");
  }
}
// An own child process standing in for a stream worker that outlived the
// 8 s + 5 s exit bounds (stuck in a driver call, a starved VM).
struct LingeringWorker {
  PROCESS_INFORMATION child{};
  LingeringWorker(){
    wchar_t executable[32768]{};const auto size=GetModuleFileNameW(nullptr,executable,32768);stopCheck(size&&size<32768,"Own QA executable path unavailable");
    std::wstring command=L"\""+std::wstring(executable)+L"\" --linger";STARTUPINFOW startup{};startup.cb=sizeof(startup);
    stopCheck(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)!=FALSE,"Own QA lingering child creation failed");
    CloseHandle(child.hThread);child.hThread=nullptr;
  }
  ~LingeringWorker(){if(child.hProcess){if(WaitForSingleObject(child.hProcess,0)!=WAIT_OBJECT_0){TerminateProcess(child.hProcess,1);WaitForSingleObject(child.hProcess,30000);}CloseHandle(child.hProcess);}}
  HANDLE retained()const{return retainUnexitedWorker(child.hProcess);}
  void exit(){stopCheck(TerminateProcess(child.hProcess,0)!=FALSE&&WaitForSingleObject(child.hProcess,30000)==WAIT_OBJECT_0,"Own QA lingering child did not exit");}
};
void qemuWorkerCases(){
  const DisplayOperationKey key{stopRequestId,std::string(43,'a')};
  const auto status="{\"type\":\"host.qemu-console.status\",\"protocol\":\"qemu-console-v2\","+displayOperationFields(key)+'}';
  LingeringWorker worker;ServiceBridge bridge(Credentials{});
  SessionStopTestAccess::qemuUnexited(bridge,key,worker.retained());
  stopCheck(bridge.hostUpdateSessionActive()&&!SessionStopTestAccess::quiescentNow(bridge),"Unexited QEMU worker did not block update and stream admission");
  bridge.stop();
  stopCheck(bridge.hostUpdateSessionActive(),"Disconnect hid an unexited QEMU worker from Host update gating");
  Json reply;const auto send=[&](bool text,std::vector<uint8_t> bytes){stopCheck(text,"QEMU status was not text");reply=JsonParser::parse(std::string(bytes.begin(),bytes.end()));};
  bridge.qemuConsoleControl(JsonParser::parse(status),send);
  stopCheck(!reply.get("workerQuiescent").boolean()&&!reply.get("ok").boolean(),"Pending exact worker exit became a successful QEMU status");
  worker.exit();bridge.qemuConsoleControl(JsonParser::parse(status),send);
  stopCheck(reply.get("workerQuiescent").boolean()&&!reply.get("ok").boolean()&&!bridge.hostUpdateSessionActive()&&SessionStopTestAccess::quiescentNow(bridge),"Late QEMU worker exit did not release only the quiescence gate");
}
void qemuHoldModelCases(){
  const DisplayOperationKey key{stopRequestId,std::string(43,'a')};
  const auto command=[&](const char* type,const DisplayOperationKey& identity){return JsonParser::parse("{\"type\":"+jsonQuote(type)+",\"protocol\":\"qemu-console-v2\","+displayOperationFields(identity)+'}');};
  for(const bool finished:{false,true}){
    ServiceBridge bridge(Credentials{});SessionStopTestAccess::qemuModel(bridge,key,finished,finished,finished);
    stopCheck(bridge.hostUpdateSessionActive(),"QEMU pending/held stopped blocking Host updates");
    stopCheck(SessionStopTestAccess::probeBlocked(bridge),"Idle display probe started while the QEMU operation/hold owned topology");
    Json packet;const auto send=[&](bool text,std::vector<uint8_t> bytes){stopCheck(text,"QEMU model packet was not text");packet=JsonParser::parse(std::string(bytes.begin(),bytes.end()));};
    bridge.start(JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":"+jsonQuote(stopSessionId)+'}'),send);
    stopCheck(packet.get("type").string()=="stream.unavailable"&&packet.get("reason").string()=="host_maintenance","Pending or held QEMU admitted a stream before release");
    if(finished){
      const auto deadline=SessionStopTestAccess::holdDeadline(bridge);bridge.qemuConsoleControl(command("host.qemu-console.status",key),send);
      stopCheck(packet.get("ok").boolean()&&packet.get("workerQuiescent").boolean()&&SessionStopTestAccess::holding(bridge),"Finished/quiet worker lost its active display hold");
      stopCheck(SessionStopTestAccess::holdDeadline(bridge)==deadline,"A status request renewed the display hold");
      auto wrong=key;wrong.nonce=std::string(43,'b');bridge.qemuConsoleControl(command("host.qemu-console.cancel",wrong),send);
      stopCheck(SessionStopTestAccess::holding(bridge),"A stale operation nonce released a display hold");
    }
    bridge.qemuConsoleControl(command("host.qemu-console.cancel",key),send);
    stopCheck(!packet.get("ok").boolean()&&packet.get("reason").string()=="qemu_console_cancelled"&&packet.get("workerQuiescent").boolean()==finished,"Cancel fabricated worker exit or lost its explicit release reason");
    if(!finished){
      stopCheck(bridge.hostUpdateSessionActive(),"Pending cancelled worker was treated as quiescent");SessionStopTestAccess::completeQemuModel(bridge);
      bridge.qemuConsoleControl(command("host.qemu-console.status",key),send);
      stopCheck(packet.get("workerQuiescent").boolean()&&packet.get("reason").string()=="qemu_console_cancelled","Exact late model completion did not provide release proof");
    }
    stopCheck(!bridge.hostUpdateSessionActive(),"Quiet release left the Host update/idle gate latched");
    bridge.start(JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":"+jsonQuote(stopSessionId)+'}'),send);
    stopCheck(SessionStopTestAccess::streamAdmitted(bridge),"Quiet console release did not reopen explicit stream preparation admission");
  }
  {ServiceBridge bridge(Credentials{});SessionStopTestAccess::qemuModel(bridge,key,true,true,true);stopCheck(SessionStopTestAccess::expireHold(bridge),"Expired hold lost its already verified worker-exit proof");Json result;
    bridge.qemuConsoleControl(command("host.qemu-console.status",key),[&](bool,std::vector<uint8_t> bytes){result=JsonParser::parse(std::string(bytes.begin(),bytes.end()));});
    stopCheck(!result.get("ok").boolean()&&result.get("workerQuiescent").boolean()&&result.get("reason").string()=="qemu_console_timeout"&&!bridge.hostUpdateSessionActive(),"Monotonic TTL did not release the hold with an honest unknown-result reason");}
  {ServiceBridge bridge(Credentials{});SessionStopTestAccess::qemuModel(bridge,key,true,true,true);bridge.stop();stopCheck(!bridge.hostUpdateSessionActive(),"Host connection shutdown retained a completed display hold");}
  // The held Console ends: its restore point (the topology from before it, foreign displays included) is the
  // v2 recovery's to restore first. The display probe, whose activation re-adds the owned outputs, waits until
  // recoverDisplayV2 has looked once, and only that long (here: no display manager v2, so the look ends at once).
  {ServiceBridge bridge(Credentials{});SessionStopTestAccess::qemuModel(bridge,key,true,true,true);
    stopCheck(!SessionStopTestAccess::consoleRestorePending(bridge),"A console restore was pending before the console ended");
    stopCheck(SessionStopTestAccess::expireHold(bridge)&&SessionStopTestAccess::consoleRestorePending(bridge)&&!bridge.hostUpdateSessionActive(),"The console's end did not hand its restore point to the v2 recovery");
    stopCheck(SessionStopTestAccess::probeBlocked(bridge),"The display probe ran before the v2 recovery looked at the console's restore point");
    bridge.recoverDisplayV2();
    stopCheck(!SessionStopTestAccess::consoleRestorePending(bridge)&&SessionStopTestAccess::probeAdmitted(bridge),"The display probe stayed held after the v2 recovery looked");}
}
void unexitedWorkerCases(){
  // v2 stream: sessions stay refused while the worker lives, and reopen (with
  // completed stop evidence) once that exact process has exited. The old code
  // judged the exit once and refused every later start until a service restart.
  {
    LingeringWorker worker;ServiceBridge bridge(Credentials{});
    SessionStopTestAccess::unexitedWorker(bridge,stopSessionId,false,worker.retained());
    const auto held=bridge.stopSession(stopSessionId);
    stopCheck(!held.workerQuiescent&&!held.profileRestored&&!SessionStopTestAccess::quiescentNow(bridge),"A live unexited worker was reported quiescent");
    std::vector<Json> replies;
    bridge.start(JsonParser::parse("{\"type\":\"session.start\",\"sessionId\":"+jsonQuote(otherId)+",\"mode\":\"stream\"}"),[&](bool text,std::vector<uint8_t> bytes){if(text)replies.push_back(JsonParser::parse(std::string(bytes.begin(),bytes.end())));});
    stopCheck(replies.size()==1&&replies[0].get("type").string()=="stream.unavailable"&&replies[0].get("reason").string()=="host_maintenance","A start was admitted beside a live previous worker");
    worker.exit();
    const auto later=bridge.stoppedSession(stopSessionId);
    stopCheck(later.workerQuiescent&&later.profileRestored&&SessionStopTestAccess::quiescentNow(bridge),"A late worker exit did not reopen sessions or complete the stop evidence");
  }
  // The exit happens before the broker's stop arrives: the stop reports it.
  {
    LingeringWorker worker;ServiceBridge bridge(Credentials{});
    SessionStopTestAccess::unexitedWorker(bridge,stopSessionId,false,worker.retained());worker.exit();
    const auto evidence=bridge.stopSession(stopSessionId);
    stopCheck(evidence.workerQuiescent&&evidence.profileRestored,"A worker that exited before the stop was reported running");
  }
  // A v1 profile stream keeps its own recovery path: no late release here.
  {
    LingeringWorker worker;ServiceBridge bridge(Credentials{});
    SessionStopTestAccess::unexitedWorker(bridge,stopSessionId,true,worker.retained());worker.exit();
    stopCheck(!SessionStopTestAccess::quiescentNow(bridge),"A v1 profile stream's unobserved exit bypassed its profile recovery");
  }
  // No retained handle (handle duplication failed): nothing is invented.
  {
    ServiceBridge bridge(Credentials{});SessionStopTestAccess::unexitedWorker(bridge,stopSessionId,false,nullptr);
    stopCheck(!SessionStopTestAccess::quiescentNow(bridge)&&!bridge.stopSession(stopSessionId).workerQuiescent,"A missing exit handle was treated as an observed exit");
  }
}
// The capability report keeps its session view and console identities
// (capability_inputs.hpp) until a change signal: two reports read them once;
// a WTS notification counted by the control handler, a display change, Host
// settings' capabilities request and a session change the loop handles each
// make the next report read again. Substituted reads report no interactive
// session, so no probe or worker starts; the rest of the report reads this
// machine's state without changing it.
void capabilityInputCases(){
  ServiceBridge bridge(Credentials{});int sessions=0,identities=0;
  CapabilityInputAccess access;access.session=[&]{++sessions;return CapabilitySessionView{};};
  access.identity=[&](DWORD){++identities;return StreamConsoleIdentity{};};access.account=[]{return std::optional<MachineFileStamp>{};};
  SessionStopTestAccess::capabilityAccess(bridge,access);
  // Identities are read only where the native provider is selected (as on a Host).
  const int perRead=nativeDisplaySelected()?1:0;
  const auto reads=[&](int expected){return sessions==expected&&identities==expected*perRead;};
  (void)bridge.capabilities();(void)bridge.capabilities();stopCheck(reads(1),"Two capability reports read the session view or console identities twice");
  noteHostSessionChange();(void)bridge.capabilities();stopCheck(reads(2),"A counted WTS session notification left the report's inputs kept");
  bridge.maintenanceChanged();(void)bridge.capabilities();stopCheck(reads(3),"A display change (invalidateDisplay) left the report's inputs kept");
  bridge.refreshDeviceObservations();(void)bridge.capabilities();stopCheck(reads(4),"Host settings' capabilities request left the report's inputs kept");
  bridge.sessionChanged({{WTS_SESSION_LOCK,0xffffffff}});(void)bridge.capabilities();stopCheck(reads(5),"A session change the loop handled left the report's inputs kept");
  // A change that leaves an active stream running still drops them, before the
  // privacy epoch rotates (it returns before invalidateDisplay). Reachable only
  // while this machine's interactive console is unlocked.
  const DWORD live=interactiveSession();
  if(live!=0xffffffff&&sessionUnlocked(live)&&sameInteractiveConsole(live)){
    SessionStopTestAccess::activeStream(bridge,live,true);const auto before=SessionStopTestAccess::capabilityInvalidations(bridge);
    bridge.sessionChanged({{WTS_SESSION_LOGON,live+1000}});const auto after=SessionStopTestAccess::capabilityInvalidations(bridge);
    SessionStopTestAccess::activeStream(bridge,0xffffffff,false);
    stopCheck(after==before+1,"A session change beside a running stream left the report's inputs kept");
  }else std::cout<<"Capability inputs: the running-stream session change case needs an unlocked console session; not reached here\n";
  // The control handler counts a session notification, and a status report
  // drops the kept SCM status, at once (before the loop runs).
  const auto sessionEpoch=hostSessionChangeEpoch().load();WTSSESSION_NOTIFICATION note{sizeof(note),1};
  (void)serviceHandler(SERVICE_CONTROL_SESSIONCHANGE,WTS_SESSION_LOCK,&note,nullptr);{std::lock_guard lock(sessionEventsMutex);sessionEvents.clear();}
  stopCheck(hostSessionChangeEpoch().load()==sessionEpoch+1,"The control handler did not count a WTS session notification");
  const auto statusEpoch=hostServiceStatusEpoch().load();reportService(SERVICE_RUNNING);
  stopCheck(hostServiceStatusEpoch().load()==statusEpoch+1,"A status report did not drop the kept SCM status");
}
// --capability-cost (measurement only): wakeups of this thread for one capability
// report with this machine's real inputs, read afresh (as every report did) and
// kept (nothing changed). Reports are 2.1 s apart, so each reads a lease snapshot
// the Host service has rewritten since, as a heartbeat does. No probe or worker
// starts. Outside the Host service the SCM status is never kept, and the v1
// recovery check reads the session list once more; both are printed to subtract.
// Refused elevated, so the stored Windows account is never decrypted here.
int capabilityCost(){
  if(elevated()){std::cerr<<"Run --capability-cost unelevated\n";return 2;}
  const auto own=[]{const auto tid=GetCurrentThreadId();for(const auto& thread:wake::sampleProcess(GetCurrentProcessId()).threads)if(thread.tid==tid)return uint64_t(thread.switches);return uint64_t(0);};
  ServiceBridge bridge(Credentials{});SessionStopTestAccess::admissionModel(bridge);(void)bridge.capabilities();
  constexpr int reports=6;uint64_t fresh=0,kept=0,scm=0,sessionList=0;
  for(int i=0;i<reports;++i){
    Sleep(2100);SessionStopTestAccess::invalidateInputs(bridge);auto before=own();(void)bridge.capabilities();fresh+=own()-before;
    Sleep(2100);before=own();(void)bridge.capabilities();kept+=own()-before;
    before=own();try{(void)queryHostServiceStatus();}catch(...){ }scm+=own()-before;
    before=own();(void)interactiveSession();sessionList+=own()-before;
  }
  const auto mean=[&](uint64_t value){return double(value)/reports;};
  const bool native=nativeDisplaySelected();
  std::cout<<"{\"kind\":\"capability-report-cost\",\"reports\":"<<reports<<",\"nativeSelected\":"<<(native?"true":"false")<<",\"inputsRead\":"<<mean(fresh)<<",\"inputsKept\":"<<mean(kept)
    <<",\"scmStatusRead\":"<<mean(scm)<<",\"scmStatusReadsPerReport\":"<<(native?3:0)<<",\"sessionListRead\":"<<mean(sessionList)<<",\"v1RecoverySessionListReads\":1}\n";
  return 0;
}
#ifdef MULTITAP_NATIVE_DISPLAY
void nativeCases(){
  for(const bool single:{false,true})for(int fault=0;fault<8;++fault){
    Fixture f(single);const auto instance=f.config.get("deviceInstanceId").string();
    const int creates=f.transport.creates,removes=f.transport.removes,releases=f.transport.releases,feeds=f.transport.feeds;
    switch(fault){case 1:f.profilePending=true;break;case 2:stopCheck(f.service->reserveStreamProfile(stopSessionId,nativeDisplayLeaseKey(f.state())),"Profile hold setup failed");break;case 3:f.transport.queryFault=1;break;case 4:f.transport.queryFault=6;break;case 5:streamField(f.config,"deviceInstanceId",std::string("ROOT\\DISPLAY\\00a2"));break;case 6:f.service->activationRecovery(uint32_t(f.state().get("generation").number()));break;case 7:streamField(f.config,"provider",std::string("untrusted"));break;}
    stopCheck(f.service->streamStopClean(instance)==(fault==0),"Fresh SDK/private profile cleanup verdict differs");
    stopCheck(!f.service->streamStopClean("ROOT\\DISPLAY\\foreign"),"Another device inherited cleanup proof");
    stopCheck(f.transport.creates==creates&&f.transport.removes==removes&&f.transport.releases==releases&&f.transport.feeds==feeds,"Cleanup query mutated SDK ownership");f.transport.checkForeign();
  }
}
// A console opened while nobody is at an unlocked administrator's console
// session (qemu_console_route.hpp), through the production bridge and SDK owner
// over the in-memory driver: signed out, locked, and a standard user signed in.
// No worker process, no Windows session and no display call is involved.
struct ParkReplies {
  std::mutex mutex;std::vector<Json> values;
  AgentBridge::Send send(){return [this](bool,std::vector<uint8_t> bytes){std::lock_guard lock(mutex);try{values.push_back(JsonParser::parse(std::string(bytes.begin(),bytes.end())));}catch(...){values.emplace_back();}};}
  Json wait(size_t count){for(int i=0;i<1000;++i){{std::lock_guard lock(mutex);if(values.size()>=count)return values[count-1];}Sleep(10);}throw std::runtime_error("A console answer did not arrive");}
  size_t count(){std::lock_guard lock(mutex);return values.size();}
};
void qemuParkCases(){
  const std::string machine="01234567-89ab-cdef-8123-456789abcdef",otherMachine="11234567-89ab-cdef-8123-456789abcdef";
  const DisplayOperationKey key{stopRequestId,std::string(43,'a')};
  const auto command=[&](const char* type,const std::string& extra={}){return JsonParser::parse("{\"type\":"+jsonQuote(type)+",\"protocol\":\"qemu-console-v2\","+displayOperationFields(key)+extra+'}');};
  const auto prepare=[&]{return command("host.qemu-console.prepare",",\"machineUuid\":"+jsonQuote(machine)+",\"model\":\"std\"");};
  const auto stream=[]{return JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":"+jsonQuote(stopSessionId)+'}');};
  const QemuConsoleSessionFacts signedOut{true,false,false,false},locked{true,true,false,false},standardUser{true,true,true,false};
  const auto result=[](const Json& reply,bool ok,bool attempted,const char* reason){return reply.get("type").string()=="host.qemu-console.result"&&reply.get("ok").boolean()==ok&&reply.get("attempted").boolean()==attempted&&reply.get("workerQuiescent").boolean()&&reply.get("reason").string()==reason;};
  // Each state: the console is prepared and held, streams wait, and closing it brings the outputs back.
  for(const auto& facts:{signedOut,locked,standardUser}){
    Fixture f(true);f.service->setDisplayManagerV2(true);ServiceBridge bridge(Credentials{},nullptr,f.service.get());SessionStopTestAccess::consoleSession(bridge,facts,machine);
    ParkReplies replies;const int releases=f.transport.releases,creates=f.transport.creates;
    bridge.qemuConsoleControl(prepare(),replies.send());
    stopCheck(result(replies.wait(1),true,true,"qemu_console_restored"),"A console without an administrator's unlocked session was not prepared");
    stopCheck(f.transport.releases==releases+1&&f.service->consoleParked()&&!f.state().get("leaseActive").boolean()&&!f.state().get("recoveryRequired").boolean(),"The owned outputs stayed while the console was prepared");
    stopCheck(bridge.hostUpdateSessionActive()&&SessionStopTestAccess::holding(bridge)&&SessionStopTestAccess::probeBlocked(bridge),"A parked console did not hold the display");
    bridge.start(stream(),replies.send());
    stopCheck(replies.wait(2).get("type").string()=="stream.unavailable"&&replies.wait(2).get("reason").string()=="host_maintenance","A stream was admitted under a parked console");
    const auto deadline=SessionStopTestAccess::holdDeadline(bridge);
    bridge.qemuConsoleControl(command("host.qemu-console.status"),replies.send());
    stopCheck(result(replies.wait(3),true,true,"qemu_console_restored")&&SessionStopTestAccess::holdDeadline(bridge)==deadline,"A status request lost or renewed a parked hold");
    bridge.qemuConsoleControl(command("host.qemu-console.renew",",\"sequence\":1"),replies.send());
    stopCheck(replies.wait(4).get("type").string()=="host.qemu-console.held"&&replies.wait(4).get("sequence").number()==1&&f.service->consoleParked(),"A parked hold could not be renewed");
    for(int i=0;i<3;++i)f.service->tick();
    stopCheck(f.transport.creates==creates&&!f.state().get("leaseActive").boolean(),"The outputs came back under a held console");
    // A second console request for the same operation only repeats the answer.
    bridge.qemuConsoleControl(prepare(),replies.send());
    stopCheck(result(replies.wait(5),true,true,"qemu_console_restored")&&f.transport.releases==releases+1,"A repeated prepare parked twice");
    bridge.qemuConsoleControl(command("host.qemu-console.cancel"),replies.send());
    stopCheck(result(replies.wait(6),false,true,"qemu_console_cancelled"),"Closing a parked console did not give its release");
    stopCheck(!f.service->consoleParked()&&!bridge.hostUpdateSessionActive()&&!SessionStopTestAccess::holding(bridge),"A closed console kept the display parked");
    f.service->tick();
    stopCheck(f.transport.creates==creates+1&&f.state().get("leaseActive").boolean()&&!f.state().get("recoveryRequired").boolean(),"The outputs did not come back after the console closed");
    bridge.start(stream(),replies.send());
    stopCheck(SessionStopTestAccess::streamAdmitted(bridge),"A stream was still refused after the console closed");
    f.transport.checkForeign();
  }
  // The hold runs out (no renewal), the broker connection ends, or the machine is another: parked no longer.
  {Fixture f(true);f.service->setDisplayManagerV2(true);ServiceBridge bridge(Credentials{},nullptr,f.service.get());SessionStopTestAccess::consoleSession(bridge,signedOut,machine);ParkReplies replies;
    bridge.qemuConsoleControl(prepare(),replies.send());stopCheck(result(replies.wait(1),true,true,"qemu_console_restored"),"Fixture park failed");
    stopCheck(SessionStopTestAccess::expireHold(bridge)&&!f.service->consoleParked()&&!bridge.hostUpdateSessionActive(),"An expired hold kept the display parked");
    bridge.qemuConsoleControl(command("host.qemu-console.status"),replies.send());stopCheck(result(replies.wait(2),false,true,"qemu_console_timeout"),"An expired parked hold did not say so");}
  {Fixture f(true);f.service->setDisplayManagerV2(true);ServiceBridge bridge(Credentials{},nullptr,f.service.get());SessionStopTestAccess::consoleSession(bridge,locked,machine);ParkReplies replies;
    bridge.qemuConsoleControl(prepare(),replies.send());stopCheck(result(replies.wait(1),true,true,"qemu_console_restored"),"Fixture park failed");
    bridge.stop();stopCheck(!f.service->consoleParked()&&!bridge.hostUpdateSessionActive(),"A lost broker connection kept the display parked");}
  {Fixture f(true);f.service->setDisplayManagerV2(true);ServiceBridge bridge(Credentials{},nullptr,f.service.get());SessionStopTestAccess::consoleSession(bridge,standardUser,machine);ParkReplies replies;
    bridge.qemuConsoleControl(prepare(),replies.send());stopCheck(result(replies.wait(1),true,true,"qemu_console_restored"),"Fixture park failed");
    SessionStopTestAccess::machine(bridge,otherMachine);bridge.qemuConsoleControl(command("host.qemu-console.renew",",\"sequence\":1"),replies.send());
    stopCheck(result(replies.wait(2),false,true,"qemu_console_identity_changed")&&!f.service->consoleParked()&&!bridge.hostUpdateSessionActive(),"A hold renewed for another machine");}
  // Nothing to park, the wrong machine, or an SDK owner that refuses: nothing changes and nothing is held.
  {Fixture f(true);f.service->setDisplayManagerV2(true);ServiceBridge bridge(Credentials{},nullptr,f.service.get());SessionStopTestAccess::consoleSession(bridge,signedOut,otherMachine);ParkReplies replies;const int releases=f.transport.releases;
    bridge.qemuConsoleControl(prepare(),replies.send());
    stopCheck(result(replies.wait(1),false,false,"qemu_console_identity_changed")&&f.transport.releases==releases&&!f.service->consoleParked()&&!bridge.hostUpdateSessionActive(),"A console for another machine parked this one");}
  {Fixture f(true);f.service->setDisplayManagerV2(true);ServiceBridge bridge(Credentials{},nullptr,f.service.get());SessionStopTestAccess::consoleSession(bridge,signedOut,machine);ParkReplies replies;const int releases=f.transport.releases;
    f.service->activationRecovery(uint32_t(f.state().get("generation").number()),true);
    bridge.qemuConsoleControl(prepare(),replies.send());
    stopCheck(result(replies.wait(1),false,false,"qemu_console_unavailable")&&f.transport.releases==releases&&!f.service->consoleParked(),"A display in recovery was parked, or the refusal was not the unknown fallback");
    for(int i=0;i<200&&bridge.hostUpdateSessionActive();++i)Sleep(10);
    stopCheck(!bridge.hostUpdateSessionActive(),"A refused park left the Host busy");}
  {Fixture f(true);ServiceBridge bridge(Credentials{},nullptr,f.service.get());SessionStopTestAccess::consoleSession(bridge,QemuConsoleSessionFacts{false,false,false,false},machine);ParkReplies replies;const int releases=f.transport.releases;
    bridge.qemuConsoleControl(prepare(),replies.send());
    stopCheck(result(replies.wait(1),false,false,"qemu_console_unavailable")&&f.transport.releases==releases&&f.state().get("leaseActive").boolean()&&!bridge.hostUpdateSessionActive(),"A Host without display manager v2 did not give the old answer");}
}
// The state file requests of a worker that is not an administrator, through
// the bridge (display_v2_state_rpc.hpp): which pipe, which Windows session and
// which owned outputs the service accepts. The file is a string here.
void stateRpcBridgeCases(){
  struct File{std::optional<std::string> bytes;int saves=0,removes=0;
    DisplayV2Store store(){DisplayV2Store s;s.load=[this]{return bytes;};s.save=[this](const std::string& value){++saves;bytes=value;return true;};s.remove=[this]{++removes;bytes.reset();return true;};s.discard=[this]{bytes.reset();};return s;}};
  constexpr DWORD consoleSession=7,otherSession=8;unsigned serial=0;
  const auto nonce=[&]{char text[33]{};sprintf_s(text,"%032x",++serial);return std::string(text);};
  const auto ask=[&](ServiceBridge& bridge,DisplayV2StateRequest request,bool authorized,DWORD session){
    request.nonce=nonce();const auto reply=SessionStopTestAccess::statePacket(bridge,JsonParser::parse(displayV2StateRequestBytes(request)),authorized,session);
    stopCheck(reply.handled&&!reply.response.empty(),"A well-formed state request got no answer from the bridge");
    const auto parsed=parseDisplayV2StateReply(JsonParser::parse(reply.response),request.nonce);stopCheck(parsed.has_value(),"The bridge's state answer did not parse");return *parsed;};
  Fixture f(true);f.service->setDisplayManagerV2(true);ServiceBridge bridge(Credentials{},nullptr,f.service.get());File file;bool unlocked=true;
  SessionStopTestAccess::stateFixture(bridge,[&]{return file.store();},[&](DWORD session,bool needUnlocked){return session==consoleSession&&(!needUnlocked||unlocked);});
  // A display state naming exactly the outputs the SDK owner holds.
  NativeCcdData ccd;{auto path=WorkerFixture::path({7,0},0,9);path.flags=DISPLAYCONFIG_PATH_ACTIVE;path.sourceInfo.modeInfoIdx=0;DISPLAYCONFIG_MODE_INFO mode{};mode.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;mode.adapterId={7,0};mode.id=0;mode.sourceMode={1600,900,DISPLAYCONFIG_PIXELFORMAT_32BPP,{0,0}};ccd.paths.push_back(path);ccd.modes.push_back(mode);}
  const auto held=nativeDisplayTargets(f.state());
  const auto state=displayV2StateBytes({"stream-x",ccd,held,std::nullopt}),digest=nativeBaselineDigest(state);
  auto foreign=held;foreign.front().target+=1;const auto foreignState=displayV2StateBytes({"stream-x",ccd,foreign,std::nullopt});
  // Not the running operation, another Windows session, or the v1 display manager: refused, nothing touched.
  stopCheck(!ask(bridge,{{},DisplayV2StateOp::load},false,consoleSession).ok,"A pipe that is not the running operation read the state");
  stopCheck(!ask(bridge,{{},DisplayV2StateOp::load},true,otherSession).ok&&!ask(bridge,{{},DisplayV2StateOp::save,{},state},true,otherSession).ok,"A worker of another Windows session was served");
  f.service->setDisplayManagerV2(false);
  stopCheck(!ask(bridge,{{},DisplayV2StateOp::save,{},state},true,consoleSession).ok,"The v1 display manager served a state request");
  f.service->setDisplayManagerV2(true);
  stopCheck(!file.bytes&&!file.saves,"A refused request stored a state");
  // The running operation's worker in the console session.
  stopCheck(ask(bridge,{{},DisplayV2StateOp::load},true,consoleSession).ok,"The running operation could not read the (absent) state");
  stopCheck(!ask(bridge,{{},DisplayV2StateOp::save,{},foreignState},true,consoleSession).ok&&!file.bytes,"A state naming outputs the SDK owner does not hold was stored");
  const auto saved=ask(bridge,{{},DisplayV2StateOp::save,{},state},true,consoleSession);
  stopCheck(saved.ok&&saved.sha256==digest&&file.bytes==state&&file.saves==1,"The running operation's state was not stored as sent");
  stopCheck(!ask(bridge,{{},DisplayV2StateOp::save,{},state},true,consoleSession).ok&&file.saves==1,"A replayed save was stored again");
  // Locked: the state can be read (to be kept), not replaced or removed.
  unlocked=false;
  const auto read=ask(bridge,{{},DisplayV2StateOp::load},true,consoleSession);
  stopCheck(read.ok&&read.sha256==digest&&read.state&&parseDisplayV2State(*read.state).session==displayV2StateForeignLabel,"A locked console session could not read the state, or read its stream label");
  stopCheck(!ask(bridge,{{},DisplayV2StateOp::remove,digest},true,consoleSession).ok&&file.bytes==state,"A locked console session removed the state");
  unlocked=true;
  stopCheck(!ask(bridge,{{},DisplayV2StateOp::remove,digest},true,otherSession).ok&&!ask(bridge,{{},DisplayV2StateOp::remove,digest},false,consoleSession).ok&&file.bytes==state,"Another session or pipe removed the state");
  stopCheck(ask(bridge,{{},DisplayV2StateOp::remove,digest},true,consoleSession).ok&&!file.bytes&&file.removes==1,"The running operation could not remove the state it restored");
  // The size request of the same pipe still goes to the SDK owner.
  const auto sized=SessionStopTestAccess::statePacket(bridge,JsonParser::parse(displayV2SizeRequestBytes({nonce(),"0",1600,900})),true,consoleSession);
  stopCheck(sized.handled&&JsonParser::parse(sized.response).get("ok").boolean()&&nativeDisplayTargets(f.state())[0].width==1600,"The size request stopped reaching the SDK owner");
  f.transport.checkForeign();
}
#endif
}
int main(int argc,char** argv){if(argc==2&&std::string(argv[1])=="--qemu-hold-model"){try{qemuHoldModelCases();std::cout<<stopChecks<<" QEMU service hold model checks passed; no worker/device/input operation\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
 if(argc==2&&std::string(argv[1])=="--linger"){Sleep(120000);return 0;}
 if(argc==2&&std::string(argv[1])=="--capability-cost")return capabilityCost();
 try{protocolCases();agentCases();serviceCases();unexitedWorkerCases();qemuWorkerCases();capabilityInputCases();
#ifdef MULTITAP_NATIVE_DISPLAY
 nativeCases();qemuParkCases();stateRpcBridgeCases();
#endif
 std::cout<<"{\"kind\":\"session-stop-correlation\",\"checks\":"<<stopChecks<<",\"lateWorkerExitReopensSessions\":true,\"agentBridgeAndServiceProductionMethods\":true,\"sdkAndWorkerFacts\":\"injected\",\"installedDriverContacted\":false,\"windowsDisplayChanges\":0}\n";return 0;
 }catch(const std::exception& error){std::cerr<<"Session stop: "<<error.what()<<'\n';return 1;}}
