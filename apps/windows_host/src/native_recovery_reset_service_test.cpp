// NativeDisplayService side of the administrator record reset: refusal while
// a controller/change/reservation exists, SDK-presence refusal, archive
// failure, and a successful reinitialisation that starts a fresh controller
// with a monotonic generation. Actual SDK controller/store over the memory
// transport; files and Windows CCD are injected (no machine directory use).
#define main upstreamNativeDisplayServiceChangeMain
#include "native_display_service_change_test.cpp"
#undef main

int main(){using namespace host;int passed=0;try{
  {Fixture f(true);int moves=0;NativeResetIo io;io.leases=[]{return std::optional<NativeResetLeaseSet>(NativeResetLeaseSet{{ownedLease},{71,72}});};io.sdk=[](const auto&){return std::string("absent");};io.move=[&](size_t&){++moves;return std::string("moved");};f.service->testResetIo(io);
    const auto before=f.journal;const auto pre=f.service->resetPreflight();const auto commit=f.service->resetCommit();
    require(pre.pending=="controller_active"&&!commit.ok&&commit.steps.size()==1&&commit.steps[0].code=="controller_active"&&!moves&&f.journal==before&&f.state().get("leaseActive").boolean(),"Live controller reset was not refused");++passed;}
  {NativeRuntimeRecord boot{"ROOT\\DISPLAY\\00a1","recovery",5,{{"0",1920,1080},{"1",1280,720}},""};Fixture f(false,boot,false);f.profilePending=true;f.service->tick();
    require(!f.state().get("leaseActive").boolean()&&!f.transport.creates,"Blocked fixture started a controller");
    int moves=0,sdkCalls=0;std::string sdk="present";NativeResetIo io;io.leases=[]{return std::optional<NativeResetLeaseSet>(NativeResetLeaseSet{{ownedLease},{71,72}});};
    io.sdk=[&](const NativeResetLeaseSet& known){++sdkCalls;(void)known;return sdk;}; // actual SDK queries: native_recovery_reset_test.cpp
    std::string moveResult="backup_failed";io.move=[&](size_t& moved){++moves;if(moveResult=="moved"){f.journal.reset();f.profilePending=false;moved=3;}return moveResult;};f.service->testResetIo(io);
    // SDK still reports a Multitap lease/output: refuse before any move.
    {const auto commit=f.service->resetCommit();require(!commit.ok&&commit.steps.back().step=="sdk"&&commit.steps.back().code=="present"&&!moves,"SDK presence did not refuse the reset");++passed;}
    // Owner identity unreadable: refuse.
    {auto io2=io;io2.leases=[]{return std::optional<NativeResetLeaseSet>{};};f.service->testResetIo(io2);require(f.service->resetPreflight().sdk=="identity_unavailable"&&!f.service->resetCommit().ok&&!moves,"Missing owner identity did not refuse");f.service->testResetIo(io);++passed;}
    // Archive failure keeps the service blocked and reports it.
    sdk="absent";{const auto commit=f.service->resetCommit();require(sdkCalls>=1&&!commit.ok&&moves==1&&commit.steps.back().code=="backup_failed"&&f.journal&&!f.state().get("leaseActive").boolean(),"Archive failure was not reported or changed state");++passed;}
    // Success: records moved, fresh "stopped" runtime with a monotonic
    // generation, in-memory state reinitialised, next tick starts a controller.
    moveResult="moved";const auto commit=f.service->resetCommit();
    require(commit.ok&&commit.moved==3&&commit.steps.size()==2&&commit.steps[0].code=="moved"&&commit.steps[1].code=="reinitialized","Reset commit did not succeed");
    require(f.journal&&parseNativeRuntimeRecord(*f.journal).phase=="stopped"&&parseNativeRuntimeRecord(*f.journal).generation==6&&parseNativeRuntimeRecord(*f.journal).outputs.size()==2,"Fresh runtime record not monotonic/preserved outputs");
    f.service->tick();const auto generation=DWORD(f.state().get("generation").number());
    require(f.state().get("leaseActive").boolean()&&generation==7&&f.service->activationReady(generation)&&!f.state().get("recoveryRequired").boolean()&&f.service->diagnosticSnapshot().get("phase").string()=="stable","Fresh controller did not start after the reset");
    // Afterwards the live lease refuses another reset.
    require(f.service->resetPreflight().pending=="controller_active","Second reset was not refused while the fresh lease is live");
    f.transport.checkForeign();++passed;
  }
  std::cout<<"{\"kind\":\"native-recovery-reset-service\",\"cases\":"<<passed<<",\"actualSdkControllerAndStore\":true,\"journals\":\"memory\",\"machineDirectoryTouched\":false,\"windowsDisplayChanges\":0}\n";return 0;
}catch(const std::exception& error){std::cerr<<"native recovery reset service fixture failed after "<<passed<<" cases: "<<error.what()<<'\n';return 1;}}
