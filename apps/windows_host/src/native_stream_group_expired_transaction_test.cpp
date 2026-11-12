#define MULTITAP_NATIVE_STREAM_TRACK_LEDGER_FIXTURE_ONLY
#include "native_stream_track_ledger_test.cpp"
#undef MULTITAP_NATIVE_STREAM_TRACK_LEDGER_FIXTURE_ONLY

#ifdef MULTITAP_NATIVE_DISPLAY
namespace {
// Production ledger decoding (including machine DPAPI) and durable Windows file
// transactions are composed here. SDK/process/CCD observations remain injected;
// this test must never contact an installed display service or call real CCD.
struct GroupExpiryFixture:TrackLedgerFixture {
  std::filesystem::path directory;
  NativeStreamExpiredConsoleExecutor executor{{201,21},{202,22}};
  uint64_t now=2000;
  int validations=0,applications=0,authorityReads=0;
  bool loseCompletedAck=false;
  std::string fault;
  std::function<void()> afterValidate,afterApply;
  std::vector<NativeStreamExpiredConsolePrivateSource> readers;
  std::string originalProfile,originalLaunch;

  GroupExpiryFixture(const std::filesystem::path& root,bool committed,bool rollback,int count,bool extra)
      :TrackLedgerFixture(committed,rollback,count,extra),directory(root/temporarySuffix()){
    require(std::filesystem::create_directory(directory),"Group expiry fixture directory collision");
    originalProfile=nativeStreamProfileRecordBytes(profile);originalLaunch=nativeStreamLaunchBytes(launch);
    write(L"profile.json",originalProfile);write(L"launch.json",originalLaunch);
    for(const auto& snapshot:snapshots)write(privateName(snapshot.kind),snapshot.bytes);
    for(const auto kind:{PrivateKind::runtime,PrivateKind::identity,PrivateKind::viewport,PrivateKind::reclaim,PrivateKind::baseline}){
      const size_t limit=kind==PrivateKind::viewport?131072:kind==PrivateKind::baseline?65536:kind==PrivateKind::identity?8192:32768;
      readers.push_back({kind,nativeJournalFileAccess(directory/privateName(kind),limit,[](const auto&){},trust())});
    }
  }
  static std::wstring privateName(PrivateKind kind){
    switch(kind){case PrivateKind::runtime:return L"runtime.json";case PrivateKind::identity:return L"identity.bin";case PrivateKind::viewport:return L"native-stream-viewport-commit.json";case PrivateKind::reclaim:return L"reclaim.json";case PrivateKind::baseline:return L"baseline.json";}
    throw std::runtime_error("Unknown private fixture kind");
  }
  void write(const std::wstring& name,const std::string& bytes){
    std::ofstream output(directory/name,std::ios::binary|std::ios::trunc);output.write(bytes.data(),std::streamsize(bytes.size()));output.close();require(bool(output),"Fixture file write failed");
  }
  std::function<bool(const std::filesystem::path&)> trust(){
    return [root=directory](const auto& path){const auto normalized=std::filesystem::absolute(path).lexically_normal();const auto flags=GetFileAttributesW(normalized.c_str());return (normalized==root||normalized.parent_path()==root)&&flags!=INVALID_FILE_ATTRIBUTES&&!(flags&FILE_ATTRIBUTE_REPARSE_POINT);};
  }
  NativeRuntimeJournalAccess file(const wchar_t* name,size_t maximum=65536){return nativeJournalFileAccess(directory/name,maximum,[](const auto&){},trust());}
  NativeStreamExpiredConsoleTransactionAccess transactionAccess(){
    NativeStreamExpiredConsoleTransactionAccess a;
    auto marker=nativeStreamExpiredConsoleFileAccess(directory/L"native-stream-expired-console.json",trust());
    a.marker=marker;a.marker.save=[&,marker](const auto& expected,const auto& desired){const bool saved=marker.save(expected,desired);return saved&&!(loseCompletedAck&&parseNativeStreamExpiredConsoleRecord(desired).phase=="completed");};
    a.profiles=file(L"profile.json");a.launches=file(L"launch.json");
    a.history=[&](const auto& sha){return nativeStreamExpiredConsoleHistoryAccess(directory,sha,trust());};
    a.observe=[&]{return NativeStreamExpiredConsoleObservation{fresh,scope,machine};};a.identity=Fixture::access();
    a.privateSources=readers;
    a.resolveLedger=[](const auto& saved,const auto& launched,const auto& bytes){return resolveNativeStreamExpiredLedger(saved,launched,bytes);};
    a.authority=[&](const NativeStreamExpiredConsoleAuthorityRequest& request)->std::optional<NativeStreamExpiredConsoleAuthorityProof>{
      ++authorityReads;
      NativeStreamExpiredConsoleAuthorityProof p;
      p.operation=request.record.operation;p.profileSha256=request.record.profileSha256;p.launchSha256=request.record.launchSha256;p.reservation=std::string(64,'f');p.executor=request.executor;p.session=profile.key.session;p.logon=profile.scope.logon;p.tick=now;
      p.processes={{request.launch.service,NativeStreamProcessState::gone},{request.launch.worker,NativeStreamProcessState::gone},{request.executor.service,NativeStreamProcessState::running},{request.executor.worker,NativeStreamProcessState::running}};
      if(request.previousExecutor){if(request.previousExecutor->service!=request.executor.service)p.processes.push_back({request.previousExecutor->service,NativeStreamProcessState::gone});p.processes.push_back({request.previousExecutor->worker,NativeStreamProcessState::gone});}
      for(const auto& mapping:request.ledger.leases){p.leases.push_back({mapping.lease,NativeStreamExpiredSdkObject::absent,0,0});for(const auto& output:mapping.outputs)p.displays.push_back({mapping.lease,output.display,NativeStreamExpiredSdkObject::absent});}
      if(fault=="worker-alive")p.processes[1].state=NativeStreamProcessState::running;
      if(fault=="lease-alive")p.leases.front().state=NativeStreamExpiredSdkObject::present;
      if(fault=="lease-pending")p.leases.front().pendingDepartures=1;
      if(fault=="display-alive")p.displays.back().state=NativeStreamExpiredSdkObject::present;
      if(fault=="display-unknown")p.displays.back().state=NativeStreamExpiredSdkObject::unknown;
      if(fault=="missing-output")p.displays.pop_back();
      if(fault=="executor-pid-reused")++p.processes[3].identity.created;
      return p;
    };
    a.configure=[&](const NativeCcdData& value,UINT32 flags){
      require(!(flags&~(SDC_VALIDATE|SDC_APPLY|SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VIRTUAL_MODE_AWARE)),"Expiry requested persistent/global CCD changes");
      require(std::none_of(value.paths.begin(),value.paths.end(),[&](const auto& path){return nativeOwnedPath(path,profile.profile.targets);}),"Expired group outputs were recreated");
      if(flags&SDC_VALIDATE){++validations;if(afterValidate)afterValidate();return LONG(ERROR_SUCCESS);}
      require(bool(flags&SDC_APPLY),"Unexpected CCD fixture boundary");++applications;fresh=value;rescope();if(afterApply)afterApply();return LONG(ERROR_SUCCESS);
    };a.now=[&]{return now;};return a;
  }
  NativeStreamExpiredConsoleTransactionResult run(){NativeStreamExpiredConsoleTransaction tx(transactionAccess());return tx.restore(std::string(64,'c'),executor);}
  void reserveAfterCompletedGroup(){
    require(!profile.resize,"Reservation fixture requires a completed predecessor");
    profile.resize=NativeStreamProfileResize{std::string(64,'e'),std::string(64,'f'),++profile.resizeSequence};++profile.revision;
    originalProfile=nativeStreamProfileRecordBytes(profile);write(L"profile.json",originalProfile);
  }
  void unchangedAuthorityFiles(){require(file(L"profile.json").load()==std::optional(originalProfile)&&file(L"launch.json").load()==std::optional(originalLaunch),"Expiry rewrote profile or launch authority");}
};
}
#endif

int main(int argc,char** argv){
#ifdef MULTITAP_NATIVE_DISPLAY
  try{
    require(argc==2,"Pass a dedicated existing QA artifact directory");
    const auto root=std::filesystem::canonical(argv[1]);const auto flags=GetFileAttributesW(root.c_str());
    require(flags!=INVALID_FILE_ATTRIBUTES&&(flags&FILE_ATTRIBUTE_DIRECTORY)&&!(flags&FILE_ATTRIBUTE_REPARSE_POINT),"QA artifact directory is not regular");
    int workflows=0,restored=0,refused=0;
    for(const int count:{1,2})for(const bool extra:{false,true})for(const bool committed:{false,true})for(const bool rollback:{false,true}){
      GroupExpiryFixture f(root,committed,rollback,count,extra);const auto before=f.snapshots;
      auto result=f.run();require(result.ok&&result.completedAcknowledged&&result.consoleVerified&&f.validations==1&&f.applications==1,"Actual typed group ledger failed durable console recovery");
      require(nativeStreamProfileSignatures(f.fresh)==nativeStreamProfileSignatures(f.context.profile.before,f.context.profile.targets),"Group expiry changed original console or unrelated output");
      require(f.run().ok&&f.applications==1&&f.validations==1,"Completed group expiry reapplied CCD");
      for(const auto& bytes:before)require(readNativeJournalFile(f.directory/f.privateName(bytes.kind),f.trust(),bytes.kind==PrivateKind::viewport?131072:65536)==std::optional(bytes.bytes),"Expiry rewrote SDK/receipt/baseline source");
      f.unchangedAuthorityFiles();++restored;++workflows;
    }
    for(const bool committed:{false,true})for(const auto* fault:{"worker-alive","lease-alive","lease-pending","display-alive","display-unknown","missing-output","executor-pid-reused"}){
      GroupExpiryFixture f(root,committed,false,2,true);f.fault=fault;const auto result=f.run();require(!result.ok&&!result.applyAttempted&&f.validations==0&&f.applications==0,"Partial group absence or stale process identity authorized recovery");f.unchangedAuthorityFiles();++refused;++workflows;
    }
    for(const bool committed:{false,true})for(const auto kind:{PrivateKind::runtime,PrivateKind::identity,PrivateKind::viewport,PrivateKind::baseline}){
      GroupExpiryFixture f(root,committed,false,2,true);f.write(f.privateName(kind),"changed-private-source");const auto result=f.run();require(!result.ok&&f.validations==0&&f.applications==0,"Corrupt real private group file authorized recovery");++refused;++workflows;
    }
    for(const auto kind:{PrivateKind::runtime,PrivateKind::identity,PrivateKind::viewport,PrivateKind::baseline}){
      GroupExpiryFixture f(root,true,false,2,true);f.afterValidate=[&]{f.write(f.privateName(kind),"changed-after-validate");};const auto result=f.run();require(!result.ok&&f.validations==1&&f.applications==0,"Real protected source changed after VALIDATE but APPLY proceeded");++refused;++workflows;
    }
    {GroupExpiryFixture f(root,true,false,2,true);f.afterValidate=[&]{f.fault="display-alive";};const auto result=f.run();require(!result.ok&&f.validations==1&&f.applications==0,"Reappearing second SDK output reached APPLY");++refused;++workflows;}
    {GroupExpiryFixture f(root,true,false,2,true);f.afterValidate=[&]{f.scope.logon=std::string(64,'d');};const auto result=f.run();require(!result.ok&&f.validations==1&&f.applications==0,"A new Windows login inherited old group restoration");++refused;++workflows;}
    {GroupExpiryFixture f(root,true,false,2,true);f.loseCompletedAck=true;const auto first=f.run();require(!first.ok&&first.consoleVerified&&!first.completedAcknowledged&&f.applications==1,"Lost real completion ACK claimed success");f.loseCompletedAck=false;require(f.run().ok&&f.applications==1,"Lost completion ACK caused a second CCD application");f.unchangedAuthorityFiles();++restored;++workflows;}
    {GroupExpiryFixture f(root,true,true,2,true);f.afterApply=[&]{f.fault="display-alive";};const auto first=f.run();require(!first.ok&&first.applyAttempted&&!first.completedAcknowledged&&f.applications==1,"Post-APPLY SDK output reappearance was committed");f.fault.clear();f.afterApply={};f.executor.worker={203,23};const auto next=f.run();require(next.ok&&!next.applyAttempted&&f.applications==1&&next.record->executor==f.executor,"New executor did not reconcile exact existing console restoration");f.unchangedAuthorityFiles();++restored;++workflows;}
    for(const int count:{1,2})for(const bool extra:{false,true})for(const bool rollback:{false,true}){
      GroupExpiryFixture f(root,true,rollback,count,extra);f.reserveAfterCompletedGroup();const auto result=f.run();require(result.ok&&f.applications==1&&f.validations==1,"Pending reservation with a proven group predecessor failed durable console restoration");require(f.run().ok&&f.applications==1,"Pending-reservation completion was not idempotent");f.unchangedAuthorityFiles();++restored;++workflows;
    }
    for(const auto* fault:{"worker-alive","lease-alive","display-alive","missing-output"}){
      GroupExpiryFixture f(root,true,false,2,true);f.reserveAfterCompletedGroup();f.fault=fault;const auto result=f.run();require(!result.ok&&f.applications==0&&f.validations==0,"Pending-reservation predecessor bypassed complete expired authority");++refused;++workflows;
    }
    {GroupExpiryFixture f(root,true,false,2,true);f.reserveAfterCompletedGroup();f.afterValidate=[&]{f.write(f.privateName(PrivateKind::viewport),"changed-predecessor");};const auto result=f.run();require(!result.ok&&f.validations==1&&f.applications==0,"Pending predecessor changed after VALIDATE but authorized APPLY");++refused;++workflows;}
    std::cout<<"{\"kind\":\"group-expired-console-transaction\",\"workflows\":"<<workflows<<",\"restored\":"<<restored<<",\"refused\":"<<refused<<",\"checks\":"<<checks<<",\"actualWindowsJournalFiles\":true,\"actualMachineDpapi\":true,\"actualGroupLedgerResolver\":true,\"sdkProcessCcdAndAcl\":\"injected\",\"installedSdkContacted\":false,\"windowsDisplayChanges\":0,\"sourceFilesDeleted\":false,\"physicalDualAcceptance\":false}\n";return 0;
  }catch(const std::exception& error){std::cerr<<"group expiry: "<<error.what()<<'\n';return 1;}
#else
  (void)argc;(void)argv;return 2;
#endif
}
