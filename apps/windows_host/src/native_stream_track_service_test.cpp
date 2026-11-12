// Real Controller/private RPC/group worker/runtime/receipt publication with the
// upstream SDK memory transport. Windows CCD/capture/identity are injected.
#define main upstreamNativeDisplayServiceChangeMain
#include "native_display_service_change_test.cpp"
#undef main
namespace {
int profileTrackServiceCases(){using namespace host;int passed=0;
  for(const bool single:{false,true})for(int scenario=0;scenario<6;++scenario){
    WorkerFixture f(single);auto resize=trackRequest();if(single){resize.outputs.resize(1);resize.epochs.resize(1);}else resize.outputs[1].left=-1280;
    const auto state=f.state();const auto targets=nativeDisplayTargets(state);const auto consolePath=f.ccd.paths.front();
    NativeStreamProfileRecord profile;profile.key={resize.sessionId,std::string(64,'b'),state.get("deviceInstanceId").string(),17};profile.phase=NativeStreamProfilePhase::active;profile.revision=3;profile.resizeSequence=1;profile.entry=profile.current={GetCurrentProcessId(),uint32_t(state.get("generation").number()),targets};
    profile.console={{"00112233-4455-6677-8899-aabbccddeeff","std"},{{7,0},L"\\\\?\\PCI#VEN_1234&DEV_1111#0",L"PCI\\VEN_1234&DEV_1111\\3&0",L"PCI",{L"PCI\\VEN_1234&DEV_1111"},nativeConsoleDisplayClass,true,true},{consolePath.sourceInfo.adapterId,consolePath.targetInfo.adapterId,consolePath.sourceInfo.id,consolePath.targetInfo.id}};
    profile.profile=planNativeStreamPrimaryProfile(f.ccd,targets,NativeStreamConsolePath{consolePath.sourceInfo.adapterId,consolePath.targetInfo.adapterId,consolePath.sourceInfo.id,consolePath.targetInfo.id});profile.profileSha256=nativeBaselineDigest(nativeStreamProfilePayloadBytes(profile.profile));profile.scope=test::recoveryScope(captureNativeTopologyBaseline(profile.profile.before,targets,"0"));profile.scope.foreign[0].sourcePath=profile.scope.foreign[0].targetPath=utf8(profile.console.device.interfacePath);profile.resize=NativeStreamProfileResize{std::string(64,'c'),nativeBaselineDigest(nativeTrackResizeFields(resize)),1};profile.launchSha256=std::string(64,'d');
    require(f.service->reserveStreamProfile(resize.sessionId,f.key()),"Group SDK fixture could not reserve profile");f.ccd=profile.profile.applied;
    std::optional<std::string> profileBytes=nativeStreamProfileRecordBytes(profile),receiptBytes;
    f.service->testProfileViewportStores({[&]{return profileBytes;},[&](const auto& expected,const auto& desired){if(profileBytes!=expected)return false;profileBytes=desired;return true;}},{[&]{return receiptBytes;},[&](const auto& expected,const auto& desired){if(scenario==2||receiptBytes!=expected)return false;receiptBytes=desired;return true;}});
    const auto creates=f.transport.creates;
    if(scenario==1)f.failValidate=true;
    if(scenario==3){auto changed=profile;changed.resize->requestSha256=std::string(64,'f');profileBytes=nativeStreamProfileRecordBytes(changed);}
    if(scenario==4)f.fault="prepare-lost";
    if(scenario==5)f.fault="capture";
    StreamResizeBudget budget;const auto result=applyNativeWorkerTracks(trackCurrent(f),resize,trackWorkerAccess(f,resize,budget),budget);
    if(scenario==3){require(!result.ok&&f.transport.creates==creates&&!receiptBytes,"Unreserved group reached SDK mutation");++passed;continue;}
    if(scenario==2){require(!result.ok&&(f.state().get("configurationPending").boolean()||f.state().get("recoveryRequired").boolean()),"Unstored group receipt admitted a stable media source");f.service->cancelViewportSession(resize.sessionId,true);require(f.state().get("recoveryRequired").boolean(),"Group recovery state disappeared on cancellation");++passed;continue;}
    require(receiptBytes&&f.journal&&f.baselineBytes,"Protected SDK group receipt/runtime/baseline omitted");const auto receipt=parseNativeStreamTrackCommit(*receiptBytes);const auto lease=nativeStreamProfileLease(f.state(),GetCurrentProcessId());NativeStreamProfileEvidence proof{profile.key,lease,test::recoveryScope(captureNativeTopologyBaseline(nativeActiveCcd(f.ccd),lease.targets,"0")),profile.console,f.ccd};proof.scope.tick=receipt.scope.tick+1;
    require(nativeStreamTrackCommitProof(profile,proof,resize,receipt,*f.journal,*f.baselineBytes)&&receipt.lease==ownedLease&&receipt.outputs.size()==(single?1U:2U)&&receipt.outputs[0].display==71&&receipt.rollback==(scenario!=0),"SDK group completion did not bind both outputs, baseline and request");
    require(result.ok==(scenario==0)&&!f.state().get("configurationPending").boolean(),"SDK group result/rollback mismatch");f.transport.checkForeign();++passed;
  }
  return passed;
}
}
int main(){try{const auto cases=profileTrackServiceCases();std::cout<<"{\"kind\":\"native-stream-group-service\",\"cases\":"<<cases<<",\"actualSdkControllerAndPrivateRpc\":true,\"actualGroupWorker\":true,\"ccdAndCapture\":\"injected\",\"installedDisplaysChanged\":false,\"runtimeEnabled\":false}\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
