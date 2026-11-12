#include "service.hpp"
#include <iostream>
using namespace host;
namespace {
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
// Generous ceiling for a loaded release machine; returns as soon as it holds.
template<class F>void await(F predicate,const char* reason){const auto deadline=Clock::now()+std::chrono::seconds(30);while(!predicate()&&Clock::now()<deadline)Sleep(1);require(predicate(),reason);}
struct Latch {std::mutex mutex;std::condition_variable changed;bool entered=false,released=false;};
void enterAndWait(const std::shared_ptr<Latch>& latch,const std::shared_ptr<CatalogOperation>& operation){operation->setAbort([latch]{latch->changed.notify_all();});std::unique_lock lock(latch->mutex);latch->entered=true;latch->changed.notify_all();latch->changed.wait(lock,[&]{return latch->released||operation->cancelled;});}
void release(const std::shared_ptr<Latch>& latch){std::lock_guard lock(latch->mutex);latch->released=true;latch->changed.notify_all();}
bool entered(const std::shared_ptr<Latch>& latch){std::lock_guard lock(latch->mutex);return latch->entered;}
void eligibilityAndNoop(){
  std::atomic<int> checks{0},applies{0};CatalogMaintenance manager([&](const auto&,const auto&){++checks;return CatalogOutcome{CatalogPhase::Ready,""};});
  require(!manager.consider(0,true)&&!manager.consider(1,false)&&!checks,"Ineligible/session0 probe launched maintenance");
  auto first=manager.reserveSession();require(first&&!manager.consider(1,true),"Active stream/RDP failed to reserve the idle gate");
  auto replacement=manager.reserveSession();first.reset();require(replacement&&!manager.consider(1,true)&&!manager.reserveTopology(),"Replacing a session opened an idle mutation gap");
  replacement.reset();auto topology=manager.reserveTopology();require(topology&&!manager.reserveSession()&&!manager.consider(1,true),"Topology operation overlapped a new session or catalog");topology.reset();
  require(manager.consider(1,true),"Fresh eligible console probe did not schedule");await([&]{return !manager.busy();},"Read-only no-op never completed");
  require(checks==1&&!applies&&manager.result().phase==CatalogPhase::Ready,"An already-ready catalog performed an apply");manager.cancel();require(!manager.consider(1,true)&&checks==1,"Ready catalog was applied repeatedly");
}
void asynchronousBusyAndCancellation(){
  const auto latch=std::make_shared<Latch>();std::atomic<int> commits{0};CatalogMaintenance manager([latch,&commits](const auto& operation,const auto& commit){enterAndWait(latch,operation);if(commit())++commits;return CatalogOutcome{CatalogPhase::Ready,""};});
  require(manager.consider(3,true),"Pending catalog did not reserve");await([&]{return entered(latch);},"Maintenance worker did not start");
  require(!manager.reserveSession()&&!manager.reserveTopology()&&manager.reason()=="host_maintenance","Busy catalog allowed another operation");
  Credentials unused;ServiceBridge bridge(unused,&manager);
  const auto begun=Clock::now();
  for(const auto* mode:{"stream","rdp"}){
    int replies=0;const auto request=JsonParser::parse("{\"type\":\"session.start\",\"sessionId\":\"12345678-1234-1234-1234-123456789abc\",\"mode\":"+jsonQuote(mode)+'}');
    bridge.start(request,[&](bool text,std::vector<uint8_t> bytes){++replies;const auto response=JsonParser::parse(std::string(bytes.begin(),bytes.end()));require(text&&response.get("sessionId").string()==request.get("sessionId").string()&&response.get("reason").string()==(std::string(mode)=="rdp"?"rdp_not_supported":"host_maintenance"),"Busy rejection lost its session or explicit cause");});
    require(replies==1,"Busy start opened a transport instead of rejecting (RDP is refused outright since its removal)");
  }
  bridge.stop();require(manager.busy()&&Clock::now()-begun<std::chrono::milliseconds(2000),"Ordinary bridge stop/heartbeat path waited for long catalog work");
  manager.cancel();await([&]{return !manager.busy();},"Cancelled prepare worker did not drain");
  require(commits==0&&manager.result().phase==CatalogPhase::Deferred&&!manager.result().rollbackSucceeded,"Cancelled prepare committed or claimed rollback success");
}
void committedFailureAndDeferredRetry(){
  std::atomic<int> calls{0};CatalogMaintenance failed([&](const auto&,const auto& commit){++calls;require(commit(),"Idle maintenance commit failed");CatalogOutcome result{CatalogPhase::Failed,"mode_catalog_not_verified"};result.rollbackAttempted=result.rollbackSucceeded=true;return result;});
  require(failed.consider(4,true),"Failed transaction did not begin");await([&]{return !failed.busy();},"Failed transaction did not finish");failed.cancel();
  require(!failed.consider(4,true)&&calls==1&&failed.result().rollbackAttempted&&failed.result().rollbackSucceeded,"Same revision retried a failed transaction or lost rollback evidence");
  std::atomic<int> deferredCalls{0};CatalogMaintenance deferred([&](const auto&,const auto& commit){++deferredCalls;if(deferredCalls==1){require(commit(),"Deferred commit reservation failed");CatalogOutcome result{CatalogPhase::Deferred,"interactive_console_required"};result.deferredWithoutMutation=true;return result;}return CatalogOutcome{CatalogPhase::Ready,""};});
  require(deferred.consider(5,true),"Deferred transaction did not begin");await([&]{return !deferred.busy();},"Deferred transaction did not finish");require(!deferred.consider(5,true),"Same session auto-retried a deferred check continuously");deferred.cancel();
  require(deferred.consider(5,true),"A fresh session event could not retry proven non-mutating deferral");await([&]{return !deferred.busy();},"Deferred recovery check did not finish");require(deferredCalls==2&&deferred.result().phase==CatalogPhase::Ready,"Deferred retry failed to become ready without another apply");
}
DisplayHelperOutput helperFixture(bool ready,bool readOnly){
  DisplayState display;display.installed=display.deviceReady=display.active=display.owned=true;display.foreign=0;display.width=1920;display.height=1080;display.name=L"\\\\.\\DISPLAY2";display.monitors={{"0",display.name,0,0,1920,1080,true}};
  DisplayHelperOutput result;result.exitCode=0;result.value=JsonParser::parse("{\"modeCatalogRevision\":1,\"requiredModeCount\":18,\"modeCatalogReady\":"+std::string(ready?"true":"false")+",\"catalogApplied\":"+(!readOnly&&ready?"true":"false")+",\"readOnly\":"+(readOnly?"true":"false")+",\"configurationPending\":"+(ready?"false":"true")+",\"verifiedModeCount\":"+(ready?"18":"2")+",\"verifiedSourceCount\":1,\"reason\":"+jsonQuote(ready?"":"mode_catalog_update_required")+",\"state\":"+workerDisplayJson(display)+'}');return result;
}
void resultProofs(){
  const auto good=helperFixture(true,true);require(catalogReadyResult(good),"Verified ready catalog was not accepted");
  for(const auto* key:{"modeCatalogRevision","requiredModeCount","verifiedModeCount","verifiedSourceCount"}){auto changed=good;std::get<Json::Object>(changed.value.value)[key].value=7.0;require(!catalogReadyResult(changed),"Incomplete/wrong catalog proof was accepted");}
  auto changed=good;changed.timedOut=true;require(catalogOutcome(changed).phase==CatalogPhase::Unknown,"Timed-out helper claimed completion");changed=good;changed.exitCode=1;require(!catalogReadyResult(changed),"Nonzero helper became a ready catalog");
  auto pending=helperFixture(false,true);require(catalogPendingResult(pending),"Exact read-only pending proof rejected");std::get<Json::Object>(pending.value.value)["modeCatalogRevision"].value=2.0;require(!catalogPendingResult(pending),"Future revision triggered an old automatic apply");
  changed=helperFixture(false,false);changed.exitCode=1;auto& fields=std::get<Json::Object>(changed.value.value);fields["recoveryRequired"].value=true;fields["rollbackAttempted"].value=true;fields["rollbackSucceeded"].value=false;fields["reason"].value=std::string("viewport_rollback_incomplete");const auto uncertain=catalogOutcome(changed);require(uncertain.phase==CatalogPhase::Unknown&&uncertain.reason=="viewport_recovery_required"&&uncertain.rollbackAttempted&&!uncertain.rollbackSucceeded,"Partial rollback was mislabeled successful");
}
void realPipeTransactions(){
  for(int scenario=0;scenario<9;++scenario){
    const auto name=L"\\\\.\\pipe\\MultitapHost.Catalog.QA."+randomName();WinHandle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));require(server.value!=INVALID_HANDLE_VALUE,"Catalog QA pipe creation failed");
    WinHandle client(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));require(client.value!=INVALID_HANDLE_VALUE,"Catalog QA pipe open failed");ConnectNamedPipe(server.value,nullptr);
    std::atomic<bool> stopped{false},interactive{true};std::atomic<int> applyCalls{0},readCalls{0};std::mutex serverWrite,clientWrite;
    const auto request=JsonParser::parse(R"({"type":"worker.catalog.prepare","operationId":"12345678-1234-1234-1234-123456789abc","nonce":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})");
    WorkerCatalogAccess access{[&](bool readOnly){if(readOnly){++readCalls;auto result=helperFixture(scenario==0,true);if(scenario==8)std::get<Json::Object>(result.value.value)["modeCatalogRevision"].value=2.0;return result;}++applyCalls;auto result=helperFixture(true,false);if(scenario==6)result.timedOut=true;if(scenario==7){result.exitCode=1;auto& fields=std::get<Json::Object>(result.value.value);fields["recoveryRequired"].value=true;fields["rollbackAttempted"].value=true;fields["rollbackSucceeded"].value=false;}return result;},[&]{return interactive.load();},[&]{return scenario!=5;}};
    std::atomic<bool> workerFailed{false};std::jthread worker([&]{try{configureWorkerCatalog(server.value,request,stopped,serverWrite,access);}catch(...){workerFailed=true;}});
    bool text=false;std::vector<uint8_t> bytes;require(readPipePacket(client.value,text,bytes,stopped,std::chrono::seconds(30))&&text,"Catalog prepare reply missing");const auto prepared=JsonParser::parse(std::string(bytes.begin(),bytes.end()));require(prepared.get("type").string()=="worker.catalog.prepared"&&displayOperationKey(request).matches(prepared),"Catalog prepare lost operation correlation");
    if(scenario==0||scenario==5||scenario==8){worker.join();require(applyCalls==0&&!workerFailed,"No-op/privilege/revision gate applied hardware changes");continue;}
    if(scenario==4)interactive=false;
    auto commit=request;auto& fields=std::get<Json::Object>(commit.value);fields["type"].value=std::string(scenario==2?"worker.catalog.cancel":"worker.catalog.apply");if(scenario==3)fields["nonce"].value=std::string(43,'b');const auto payload=jsonObject(commit);require(pipePacket(client.value,true,{payload.begin(),payload.end()},stopped,clientWrite),"Catalog commit write failed");
    // The worker has decided (and written any reply into the pipe buffer) once
    // joined, so the short read below is exact for both reply and no-reply cases.
    worker.join();require(!workerFailed,"Catalog worker fixture threw unexpectedly");const bool completed=readPipePacket(client.value,text,bytes,stopped,std::chrono::milliseconds(150));
    if(scenario==2||scenario==3){require(!completed&&applyCalls==0,"Cancel/wrong proof reached catalog apply");continue;}
    require(completed&&text,"Catalog result reply missing");const auto response=JsonParser::parse(std::string(bytes.begin(),bytes.end()));require(displayOperationKey(request).matches(response),"Catalog result lost operation identity");DisplayHelperOutput output;output.exitCode=DWORD(response.get("exitCode").number(1));output.timedOut=response.get("timedOut").boolean();output.value=response.get("helper");
    if(scenario==1)require(applyCalls==1&&catalogOutcome(output).phase==CatalogPhase::Ready,"One confirmed apply did not become ready");
    if(scenario==4)require(applyCalls==0&&catalogOutcome(output).deferredWithoutMutation,"Console changed after prepare but hardware apply proceeded");
    if(scenario==6)require(applyCalls==1&&catalogOutcome(output).phase==CatalogPhase::Unknown,"Timeout claimed success through IPC");
    if(scenario==7)require(applyCalls==1&&catalogOutcome(output).reason=="viewport_recovery_required","Partial rollback detail was discarded through IPC");
  }
}
void ownedJobCancellation(){
  const auto entered=std::make_shared<std::promise<void>>();auto started=entered->get_future();const auto exited=std::make_shared<std::atomic<bool>>(false);
  CatalogMaintenance manager([entered,exited](const auto& operation,const auto& commit){
    auto job=std::make_shared<WinHandle>(CreateJobObjectW(nullptr,nullptr));require(job->value!=nullptr,"QA job create failed");JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;require(SetInformationJobObject(job->value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=FALSE,"QA job policy failed");
    wchar_t path[32768]{};require(GetModuleFileNameW(nullptr,path,32768)!=0,"QA executable path failed");std::wstring command=L"\""+std::wstring(path)+L"\" --catalog-silent";STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup,&child)!=FALSE,"Owned QA child failed");WinHandle process(child.hProcess),thread(child.hThread);
    if(!AssignProcessToJobObject(job->value,process.value)){TerminateProcess(process.value,42);throw std::runtime_error("Owned QA child job assignment failed");}require(ResumeThread(thread.value)!=DWORD(-1),"QA child resume failed");operation->setAbort([job]{TerminateJobObject(job->value,ERROR_CANCELLED);});require(commit(),"QA owned job commit denied");entered->set_value();
    require(WaitForSingleObject(process.value,5000)==WAIT_OBJECT_0,"Only the owned QA job was not terminated");*exited=true;return CatalogOutcome{CatalogPhase::Ready,""};
  });
  require(manager.consider(6,true),"Owned job QA did not schedule");require(started.wait_for(std::chrono::seconds(30))==std::future_status::ready,"Owned QA job did not reach running stage");manager.cancel();await([&]{return !manager.busy();},"Owned job cancellation did not finish");
  require(*exited&&manager.result().phase==CatalogPhase::Unknown&&!manager.result().rollbackSucceeded,"Interrupted helper was claimed as success/rollback or owned child remained alive");
}
void boundedShutdownLifetime(){
  const auto latch=std::make_shared<Latch>();const auto finished=std::make_shared<std::atomic<bool>>(false);
  auto manager=std::make_unique<CatalogMaintenance>([latch,finished](const auto&,const auto& commit){std::unique_lock lock(latch->mutex);latch->entered=true;latch->changed.notify_all();latch->changed.wait(lock,[&]{return latch->released;});require(!commit(),"Late helper committed after manager shutdown");*finished=true;return CatalogOutcome{CatalogPhase::Ready,""};});
  require(manager->consider(7,true),"Shutdown lifetime QA did not schedule");await([&]{return entered(latch);},"Lifetime runner did not begin");const auto begun=Clock::now();manager.reset();require(Clock::now()-begun<std::chrono::seconds(2),"Uncooperative OS fixture blocked service shutdown indefinitely");release(latch);await([&]{return finished->load();},"Detached cancelled state did not finish safely");
}
void shutdownClosesGateBeforeProbeTeardown(){
  const auto latch=std::make_shared<Latch>();std::atomic<bool> shutdownEntered{false},lateCommitted{true},lateAccepted{true};std::atomic<int> calls{0};CatalogMaintenance::Commit preparedCommit;
  CatalogMaintenance manager([&](const auto& operation,const auto& commit){
    ++calls;operation->setAbort([&]{shutdownEntered=true;});
    std::unique_lock lock(latch->mutex);preparedCommit=commit;latch->entered=true;latch->changed.notify_all();latch->changed.wait(lock,[&]{return latch->released;});
    lateCommitted=commit();return CatalogOutcome{CatalogPhase::Ready,""};
  });
  require(manager.consider(8,true),"Shutdown gate fixture did not schedule");await([&]{return entered(latch);},"Prepared shutdown fixture did not begin");
  std::jthread stopping([&]{manager.shutdown();});
  struct Unblock {std::shared_ptr<Latch> latch;~Unblock(){release(latch);}} unblock{latch};
  await([&]{return shutdownEntered.load();},"Shutdown did not cancel the prepared operation");
  require(!manager.reserveSession()&&!manager.reserveTopology()&&!preparedCommit(),"Stopping manager admitted a session/topology or prepared commit before probe teardown");
  release(latch);
  // An old bridge probe may finish while shutdown drains its previous worker.
  // It must never create a new maintenance worker, even for a different session.
  std::jthread lateProbe([&]{lateAccepted=manager.consider(9,true);});stopping.join();lateProbe.join();
  require(!lateAccepted&&!lateCommitted&&calls==1&&!manager.busy(),"A late probe/commit restarted maintenance during service stop");
  manager.shutdown();require(!manager.consider(10,true)&&!manager.reserveSession()&&!manager.reserveTopology()&&calls==1,"Repeated shutdown/destructor reopened the stopping gate");
}
}
int wmain(int argc,wchar_t** argv){try{if(argc==2&&std::wstring(argv[1])==L"--catalog-silent"){Sleep(30000);return 0;}require(argc==1,"Unknown catalog QA argument");eligibilityAndNoop();asynchronousBusyAndCancellation();committedFailureAndDeferredRetry();resultProofs();realPipeTransactions();ownedJobCancellation();boundedShutdownLifetime();shutdownClosesGateBeforeProbeTeardown();std::cout<<"{\"kind\":\"service-catalog-maintenance-qa\",\"groups\":8,\"realPipeTransactions\":9,\"actualOwnedQaJobCancelled\":true,\"activeStreamRdpTopologyExcluded\":true,\"busySessionRejectionBeforeTransport\":true,\"readOnlyProbeSeparated\":true,\"generationAndOneAttemptPolicy\":true,\"shutdownLifetimeBounded\":true,\"shutdownGateBeforeProbeTeardown\":true,\"helperCallsAreInjected\":true,\"actualCatalogOrDriverChanged\":false,\"installedServiceChanged\":false,\"registryChanged\":false}\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
