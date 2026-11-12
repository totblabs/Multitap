#pragma once
// Included by the service transaction QA, using its actual SDK lease store.
namespace {
host::NativeCountRequest countRequest(int count=1){return {{"10000000-0000-0000-0000-000000000001",std::string(43,'c')},count};}
struct CountFixture:WorkerFixture {
  host::NativeCountRequest desired;
  std::string countFault;bool injected=false;int countExchanges=0;
  explicit CountFixture(bool single=false):WorkerFixture(single),desired(countRequest(single?2:1)){}
  host::DisplayState countProbe(){auto value=probe();value.monitorControlSupported=value.ready()&&state().get("countControlAvailable").boolean();return value;}
  host::NativeWorkerCountAccess countAccess(){
    host::NativeWorkerCountAccess access;
    access.eligible=[this]{return eligible;};access.probe=[this]{return countProbe();};access.observed=[this]{return observed();};access.snapshot=[this]{return state();};access.recoveryScope=host::test::recoveryScope;
    access.topology={
      [this]{sync();return ccd;},
      [this](const host::NativeCcdData& value,UINT32 flags){
        require(!(flags&(SDC_ALLOW_CHANGES|SDC_SAVE_TO_DATABASE|SDC_TOPOLOGY_EXTEND|SDC_TOPOLOGY_CLONE)),"Count used global/persistent Windows display flags");
        if(!(flags&SDC_APPLY)){if(countFault=="validate"&&!injected){injected=true;return LONG(ERROR_BAD_CONFIGURATION);}return LONG(ERROR_SUCCESS);}
        ++applies;ccd=materialize(value);if((countFault=="capture"||countFault=="restore-lost")&&applies==1)badCapture=true;return LONG(ERROR_SUCCESS);
      },[this]{return eligible;},[this]{return ++ticks;},[this]{ticks+=50;}
    };
    access.exchange=[this](const std::string& phase,const std::string& key,bool verified,const std::string& baseline,const host::StreamResizeBudget& budget){
      ++countExchanges;std::optional<host::Json> response;
      host::NativeWorkerRpcAccess wire;
      wire.write=[&,this](const std::string& bytes){
        auto value=host::JsonParser::parse(bytes);host::NativeCountContext context;context.request=desired;context.deadline=host::streamMonotonicMillis()+host::nativeCountTimeoutMs;context.applied=context.authorized=context.console=context.unlocked=true;context.scope=host::test::currentScope();
        if(countFault=="unauthorized")context.authorized=false;
        if(countFault=="not-applied")context.applied=false;
        if(countFault=="not-console")context.console=false;
        if(countFault=="locked")context.unlocked=false;
        if(countFault=="expired")context.deadline=host::streamMonotonicMillis()-1;
        if(countFault=="revoked-after-prepare"&&phase!="worker.native.count.prepare")context.authorized=false;
        if(countFault=="wrong-scope")context.scope->logon=std::string(64,'b');
        if(countFault=="wrong-count")host::streamField(value,"count",desired.count==1?2:1);
        if(countFault=="wrong-operation")host::streamField(value,"nonce",std::string(43,'z'));
        if(countFault=="wrong-key")host::streamField(value,"leaseKey",std::string("stale-key"));
        const auto dispatched=host::dispatchNativeCount(value,context,service.get());require(dispatched.handled&&!dispatched.response.empty(),"Count RPC response missing");response=host::JsonParser::parse(dispatched.response);sync();
        if(phase=="worker.native.count.prepare"){
          if(countFault=="reply-count")host::streamField(*response,"count",desired.count==1?2:1);
          if(countFault=="reply-pending")std::get<host::Json::Object>(response->value)["pending"].value=false;
          if(countFault=="reply-generation"){auto& snapshot=std::get<host::Json::Object>(response->value)["snapshot"];host::streamField(snapshot,"generation",snapshot.get("generation").number()+1);}
        }
        if(phase=="worker.native.count.prepare"&&response->get("accepted").boolean()){
          if(countFault=="console")eligible=false;
          if(countFault=="foreign")ccd.modes[host::nativeSourceIndex(ccd.paths.front())].sourceMode.width+=2;
        }
        if(phase=="worker.native.count.commit"&&countFault=="post-commit-capture")badCapture=true;
        if((phase=="worker.native.count.prepare"&&countFault=="prepare-lost")||(phase=="worker.native.count.commit"&&countFault=="commit-lost")||(phase=="worker.native.count.restored"&&countFault=="restore-lost")){response.reset();return false;}
        return true;
      };
      wire.read=[&](DWORD)->std::optional<host::Json>{auto result=response;response.reset();return result;};
      return host::nativeWorkerCountRpc(desired,phase,key,verified,baseline,budget,wire);
    };
    return access;
  }
  host::NativeWorkerCountResult run(bool acknowledge=true){
    host::StreamResizeBudget budget;budget.deadline=host::streamMonotonicMillis()+host::nativeCountTimeoutMs;
    auto result=host::applyNativeWorkerCount(desired,countAccess(),budget);
    if(acknowledge){
      if(eligible&&result.display.ready())require(service->acknowledgeCountOperation(desired,result.ok),"Verified count result was not acknowledged");
      else service->cancelCountOperation(desired.operation);
    }
    transport.checkForeign();return result;
  }
};
void countOwnedPipeCase(){using namespace host;
  Fixture fixture;const auto request=countRequest();NativeCountContext context;context.request=request;context.deadline=streamMonotonicMillis()+nativeCountTimeoutMs;context.applied=context.authorized=context.console=context.unlocked=true;context.scope=test::currentScope();
  const auto name=L"\\\\.\\pipe\\MultitapHost.QA.NativeCount."+randomName();
  WinHandle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));require(server.value!=INVALID_HANDLE_VALUE,"Count QA pipe server unavailable");
  WinHandle client(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));require(client.value!=INVALID_HANDLE_VALUE,"Count QA pipe client unavailable");ConnectNamedPipe(server.value,nullptr);
  std::atomic<bool> stopped=false;std::mutex writing;std::exception_ptr failure;
  std::thread reader([&]{try{for(int i=0;i<2;++i){bool text=false;std::vector<uint8_t> bytes;require(readPipePacket(server.value,text,bytes,stopped,std::chrono::seconds(3))&&text,"Count QA pipe request unavailable");const auto result=dispatchNativeCount(JsonParser::parse(std::string(bytes.begin(),bytes.end())),context,fixture.service.get());require(result.handled&&!result.response.empty()&&pipePacket(server.value,true,{result.response.begin(),result.response.end()},stopped,writing),"Count QA pipe reply failed");}}catch(...){failure=std::current_exception();stopped=true;}});
  std::exception_ptr callerFailure;
  try{
    NativeWorkerRpcAccess io;io.write=[&](const std::string& message){return pipePacket(client.value,true,{message.begin(),message.end()},stopped,writing);};io.read=[&](DWORD timeout)->std::optional<Json>{bool text=false;std::vector<uint8_t> bytes;if(!readPipePacket(client.value,text,bytes,stopped,std::chrono::milliseconds(timeout))||!text)return {};return JsonParser::parse(std::string(bytes.begin(),bytes.end()));};
    StreamResizeBudget budget;budget.deadline=context.deadline;
    const auto changed=nativeWorkerCountRpc(request,"worker.native.count.prepare",fixture.key(),false,fixture.baseline("1"),budget,io);require(changed.received&&changed.accepted&&changed.pending,"Actual count pipe prepare failed");
    const auto restored=nativeWorkerCountRpc(request,"worker.native.count.rollback","",false,"",budget,io);require(restored.received&&restored.accepted&&restored.pending&&nativeDisplayTargets(restored.snapshot).size()==2,"Actual count pipe rollback failed");
  }catch(...){callerFailure=std::current_exception();stopped=true;CancelIoEx(server.value,nullptr);}
  reader.join();if(failure)std::rethrow_exception(failure);if(callerFailure)std::rethrow_exception(callerFailure);
  fixture.service->cancelCountOperation(request.operation);fixture.transport.checkForeign();
}
int nativeCountCases(){using namespace host;int passed=0;
  for(const bool single:{false,true}){
    CountFixture f(single);const auto before=nativeDisplayTargets(f.state());const auto survivor=before[0];const auto foreign=nativeActiveSignatures(nativeForeignCcd(f.ccd,before));const int removes=f.transport.removes,creates=f.transport.creates;
    const auto changed=f.run();const auto after=nativeDisplayTargets(f.state());
    require(changed.ok&&changed.display.ready()&&after.size()==size_t(f.desired.count)&&!changed.rollbackAttempted,"Actual SDK count change did not verify Windows/capture");
    require(after[0].target==survivor.target&&after[0].width==survivor.width&&after[0].height==survivor.height&&f.transport.releases==0,"Count change recreated/resized the surviving output or released its lease");
    require(f.transport.removes-removes==(single?0:1)&&f.transport.creates-creates==(single?1:0),"Count change did more SDK work than the one added/removed output");
    require(nativeActiveSignatures(nativeForeignCcd(f.ccd,after))==foreign&&f.state().get("countControlAvailable").boolean(),"Count changed foreign paths or failed to restore control availability");
    const auto saved=parseNativeRuntimeRecord(*f.journal);require(saved.phase=="stable"&&saved.outputs.size()==after.size(),"Count was not durably committed");
    f.service->shutdown();auto stopped=parseNativeRuntimeRecord(*f.journal);require(stopped.phase=="stopped"&&stopped.outputs.size()==after.size(),"Clean shutdown lost the client count");
    Fixture fresh(single,stopped);require(nativeDisplayTargets(fresh.state()).size()==after.size(),"Installer default overrode a prior client's committed monitor count");++passed;
  }
  for(const bool single:{false,true}){CountFixture f(single);f.desired.count=single?1:2;const auto changed=f.run();require(changed.ok&&f.countExchanges==0&&f.applies==0&&f.transport.removes==0,"No-op count request changed the driver or CCD");++passed;}
  for(const auto* fault:{"validate","capture","prepare-lost","commit-lost","post-commit-capture","revoked-after-prepare","reply-count","reply-pending","reply-generation"}){
    CountFixture f(true);f.countFault=fault;const auto result=f.run();
    if(std::string(fault)=="validate")require(result.failureStage==4&&result.systemError==ERROR_BAD_CONFIGURATION,"Rollback hid the first Windows validation failure");
    if(std::string(fault)=="capture")require(result.failureStage==5&&!result.systemError,"Capture failure lost its distinct stage");
    if(std::string(fault)=="prepare-lost")require(result.failureStage==3,"Lost SDK prepare acknowledgement was misreported");
    if(std::string(fault)=="commit-lost")require(result.failureStage==6,"Lost SDK commit acknowledgement was misreported");
    if(std::string(fault)=="post-commit-capture")require(result.failureStage==7,"Stable capture verification lost its distinct stage");
    if(result.ok||!result.rollbackAttempted||!result.rollbackSucceeded||!result.display.ready()||result.display.recoveryRequired)std::cerr<<"Count fault="<<fault<<" ok="<<result.ok<<" rollback="<<result.rollbackAttempted<<"/"<<result.rollbackSucceeded<<" ready="<<result.display.ready()<<" recovery="<<result.display.recoveryRequired<<" exchanges="<<f.countExchanges<<" state="<<jsonObject(f.state())<<'\n';
    require(!result.ok&&result.rollbackAttempted&&result.rollbackSucceeded&&result.display.ready()&&!result.display.recoveryRequired&&nativeDisplayTargets(f.state()).size()==1,"Failed count change did not verify complete SDK/CCD/capture restoration");++passed;
  }
  for(const auto* fault:{"unauthorized","not-applied","not-console","locked","expired","wrong-scope","wrong-count","wrong-operation","wrong-key"}){
    CountFixture f;f.countFault=fault;const auto result=f.run();require(!result.ok&&f.transport.removes==0&&f.applies==0&&nativeDisplayTargets(f.state()).size()==2,"Unbound count request crossed SDK or Windows mutation boundary");++passed;
  }
  for(const auto* fault:{"console","foreign"}){
    CountFixture f;f.countFault=fault;const auto result=f.run();require(!result.ok&&!result.rollbackSucceeded&&!result.display.ready()&&result.display.recoveryRequired&&f.state().get("recoveryRequired").boolean(),"Unverified count restoration was advertised ready");
    if(f.countFault=="foreign")require(nativeCcdSource(f.ccd,f.ccd.paths.front()).width==1602,"Count rollback overwrote an independent foreign change");++passed;
  }
  {CountFixture f;auto result=f.run(false);require(result.ok&&!f.state().get("countControlAvailable").boolean(),"Worker result discarded rollback before its service acknowledgement");f.service->cancelCountOperation(f.desired.operation);require(f.state().get("recoveryRequired").boolean()&&nativeDisplayTargets(f.state()).size()==2,"Lost final worker result failed to preserve the old SDK count and uncertainty");++passed;}
  {CountFixture f;auto result=f.run(false);require(result.ok,"Fixture count failed");const auto rolled=f.service->rollbackCount(f.desired);require(rolled.accepted&&rolled.pending&&nativeDisplayTargets(f.state()).size()==2,"Post-commit failure could not still restore the previous SDK outputs");++passed;}
  {CountFixture f(true);f.transport.failCreate=f.transport.creates+1;const auto result=f.run();require(!result.ok&&result.rollbackSucceeded&&result.display.ready()&&nativeDisplayTargets(f.state()).size()==1,"Rejected new output did not preserve the original monitor");++passed;}
  {CountFixture f;f.failBaseline=true;const auto result=f.run();require(!result.ok&&f.transport.removes==0&&f.applies==0&&f.state().get("recoveryRequired").boolean(),"Failed count checkpoint still removed a monitor");++passed;}
  {CountFixture f;f.failJournal=true;const auto result=f.run();require(!result.ok&&f.transport.removes==0&&f.applies==0&&f.state().get("recoveryRequired").boolean(),"Failed count journal still removed a monitor");++passed;}
  {CountFixture f(true);f.countFault="restore-lost";const auto result=f.run();require(!result.ok&&!result.rollbackSucceeded&&result.display.recoveryRequired&&!result.display.ready()&&f.state().get("recoveryRequired").boolean(),"Lost restoration acknowledgement was reported safely complete");++passed;}
  {CountFixture f;f.failPublish=true;const auto result=f.run();require(!result.ok&&!result.display.ready()&&f.state().get("recoveryRequired").boolean(),"Unpublished SDK change was reported complete");++passed;}
  {CountFixture f;const auto result=f.run(false);require(result.ok,"Fixture count failed");auto changed=parseNativeRuntimeRecord(*f.journal);changed.generation+=10;f.journal=nativeRuntimeBytes(changed);const auto foreignJournal=f.journal;require(!f.service->acknowledgeCountOperation(f.desired,true)&&f.journal==foreignJournal&&f.state().get("recoveryRequired").boolean(),"Final count acknowledgement overwrote an externally replaced runtime journal");++passed;}
  {CountFixture f;StreamResizeBudget budget;budget.deadline=streamMonotonicMillis()+nativeCountTimeoutMs;budget.cancelled=[]{return true;};const auto result=applyNativeWorkerCount(f.desired,f.countAccess(),budget);require(!result.ok&&f.countExchanges==0&&f.transport.removes==0&&f.applies==0,"Cancelled count reached a mutation boundary");++passed;}
  {CountFixture f;const auto targets=nativeDisplayTargets(f.state());auto& source=f.ccd.modes[nativeSourceIndex(f.ccd.paths.front())].sourceMode;source.position={-1600,0};std::vector<NativeOutputPlacement> placement{{"0",1920,0,false},{"1",0,0,true}};f.ccd=WorkerFixture::materialize(planNativeTopology(f.ccd,targets,placement).supplied);const auto result=f.run();require(result.ok&&result.display.monitors.size()==1&&result.display.monitors.front().primary,"Removing the owned primary did not hand the origin to the owned survivor");++passed;}
  for(const auto invalid:{0,3,-1}){auto request=countRequest(invalid);require(!nativeCountValid(request),"Invalid native count accepted");++passed;}
  countOwnedPipeCase();++passed;
  return passed;
}
}
