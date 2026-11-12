#define MULTITAP_INITIAL_PROFILE_TRACKS_FIXTURE_ONLY
#include "initial_profile_tracks_test.cpp"
#undef MULTITAP_INITIAL_PROFILE_TRACKS_FIXTURE_ONLY

namespace {
struct ResizingProfileFixture:PreparedProfileFixture {
  std::optional<std::string> receipt;
  bool denyReceipt=false,failFreshCapture=false,returnConsole=false;
  ResizingProfileFixture(WorkerFixture& f,const host::NativeTrackResize& request):PreparedProfileFixture(f,request){
    f.service->testProfileViewportStores(store(),{[&]{return receipt;},[&](const auto& expected,const auto& desired){if(denyReceipt||expected!=receipt)return false;receipt=desired;return true;}});
  }
  host::NativeStreamProfileWorkerAccess access(){using namespace host;auto a=PreparedProfileFixture::access();
    a.trackViewportCommit=[&]()->std::optional<NativeStreamTrackCommitEvidence>{if(!receipt||!f.journal||!f.baselineBytes)return {};try{const auto value=parseNativeStreamViewportReceipt(*receipt);if(const auto* group=std::get_if<NativeStreamTrackCommit>(&value))return NativeStreamTrackCommitEvidence{*group,*f.journal,*f.baselineBytes};}catch(...){ }return {};};
    a.unchangedViewport=[&](const NativeStreamProfileRecord& profile,const NativeStreamCancelRequest& request,const NativeStreamProfileEvidence& proof)->std::optional<NativeStreamViewportCancelEvidence>{
      const auto baseline=captureNativeTopologyBaseline(nativeActiveCcd(proof.topology),profile.current.targets,"0");const auto checkpoint=nativeRecoveryBaselineBytes({profile.key.instance,profile.current.generation,baseline,proof.scope});
      const auto reply=f.service->confirmUnchangedViewport(request,f.key(),streamMonotonicMillis()+30000,checkpoint,proof.scope);
      if(!reply.accepted||!receipt||!f.journal)return {};return NativeStreamViewportCancelEvidence{parseNativeStreamViewportCancel(*receipt),*f.journal};
    };return a;
  }
  host::NativeWorkerTrackAccess workerAccess(const host::NativeTrackResize& request,const host::StreamResizeBudget& budget,host::NativeStreamProfileWorker* profile=nullptr){using namespace host;
    auto result=trackWorkerAccess(f,request,budget);const auto scope=access().scope;
    if(profile){result.console=[this,profile,request](const NativeTopologyBaseline& baseline){return profile->viewportConsole(request,baseline);};result.consoleCurrent=[profile,request](const NativeConsoleBinding& binding){return profile->viewportConsoleCurrent(request,binding);};}
    const auto configure=result.topology.configure;result.topology.configure=[&,configure](const NativeCcdData& value,UINT32 flags){const auto status=configure(value,flags);if(status==ERROR_SUCCESS&&(flags&SDC_APPLY)&&failFreshCapture){failFreshCapture=false;f.badCapture=true;}return status;};
    result.recoveryScope=scope;result.probe=[&]{auto display=f.probe();display.viewportProvisioning=true;return display;};
    result.exchange=[&,request](const std::string& phase,const std::string& key,bool verified,const std::string& checkpoint,const StreamResizeBudget& phaseBudget){
      ++f.exchanges;std::optional<Json> response;
      NativeWorkerRpcAccess io;
      io.write=[&](const std::string& packet){auto context=trackContext(request);context.deadline=phaseBudget.deadline;context.scope=NativeRecoveryScope{2,std::string(64,'b'),tick++,{}};
        const auto dispatched=dispatchNativeTrackViewport(JsonParser::parse(packet),context,f.service.get());if(dispatched.handled&&!dispatched.response.empty())response=JsonParser::parse(dispatched.response);f.sync();
        if(returnConsole&&phase=="worker.native.tracks.resize.prepare"){
          const auto record=parseNativeStreamProfileRecord(*bytes);const auto& original=record.profile.before;const auto console=std::find_if(original.paths.begin(),original.paths.end(),[&](const auto& path){return nativeStreamConsolePathMatches(path,*record.profile.suppressedConsole);});require(console!=original.paths.end(),"Returned group console was not positively identified");
          auto returned=*console;require(!(returned.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE),"Fixture expected ordinary console modes");
          returned.sourceInfo.modeInfoIdx=UINT32(f.ccd.modes.size());f.ccd.modes.push_back(nativeCcdMode(original,nativeSourceIndex(*console),DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE));returned.targetInfo.modeInfoIdx=UINT32(f.ccd.modes.size());f.ccd.modes.push_back(nativeCcdMode(original,nativeTargetIndex(*console),DISPLAYCONFIG_MODE_INFO_TYPE_TARGET));
          f.ccd.paths.erase(std::remove_if(f.ccd.paths.begin(),f.ccd.paths.end(),[&](const auto& path){return nativeStreamConsolePathMatches(path,*record.profile.suppressedConsole);}),f.ccd.paths.end());f.ccd.paths.insert(f.ccd.paths.begin(),returned);
        }
        return true;};
      io.read=[&](DWORD)->std::optional<Json>{auto value=response;response.reset();return value;};
      return nativeWorkerTrackViewportRpc(request,phase,key,verified,phaseBudget,std::move(io),checkpoint);
    };return result;
  }
};
host::NativeTrackResize nextProfileRequest(const host::Json& current){using namespace host;auto request=nativeInitialTrackViewport(current);request.requestId="next-profile-viewport";request.resizeId="00000000-0000-0000-0000-000000000350";++request.topologyEpoch;request.outputs[0].width+=160;++request.epochs[0];return request;}
}
int main(){using namespace host;int workflows=0,changedCount=0,resumed=0,terminal=0;try{
  for(const bool single:{false,true})for(int scenario=0;scenario<7;++scenario){
    WorkerFixture f(single);const auto command=profileTracksCommand(f);const auto initial=*initialProfileTrackRequest(command);StreamResizeBudget initialBudget;
    auto initialAccess=trackWorkerAccess(f,initial,initialBudget);initialAccess.probe=[&]{auto display=f.probe();display.viewportProvisioning=true;return display;};const auto prepared=applyInitialProfileTracks(command,std::move(initialAccess),initialBudget,true);require(prepared.ok,"Resize fixture initial SDK group failed");
    InitialProfileTrackCompletion initialCompletion;require(initialCompletion.arm(command,true)&&initialCompletion.acceptWorker(groupPreparedReply(initial,prepared),true,[](const auto& display){return display.ready();}),"Resize fixture did not accept the exact initial worker ACK");
    ResizingProfileFixture entry(f,initial);const auto current=entry.start(prepared.command);require(f.service->reserveStreamProfile(initial.sessionId,f.key()),"Actual service profile reservation failed");
    NativeStreamProfileWorker profile(GetCurrentProcessId(),2,entry.store(),entry.access());require(profile.begin(current,entry.launch.key.operation,true),"Resize fixture prepared profile failed");
    const auto request=nextProfileRequest(current);StreamResizeBudget budget;const auto beforeCreates=f.transport.creates;
    if(scenario==1)f.failValidate=true;if(scenario==2)entry.failFreshCapture=true;if(scenario==4)entry.denyReceipt=true;
    if(scenario==6)entry.returnConsole=true;
    int stopped=0;ProfileTrackResizeAccess access;
    access.eligible=[&]{return f.eligible;};access.reserve=[&](const auto& value){return profile.reserveTracks(value);};access.stop=[&]{++stopped;};
    access.apply=[&]{if(scenario==3){NativeWorkerTrackResult untouched;untouched.display=f.probe();return untouched;}return applyNativeWorkerTracks(current,request,entry.workerAccess(request,budget,&profile),budget);};
    access.complete=[&](const auto& value,bool applied){return profile.completeTracks(value,applied);};access.probe=[&]{auto display=f.probe();display.viewportProvisioning=true;if(scenario==5)display.monitors.front().width+=2;return display;};access.observed=[&]{return f.observed();};
    const auto result=changeProfileTracks(current,request,std::move(access));require(stopped==1,"Resize did not stop the old stream after reservation");
    if(scenario<=3||scenario==6){const auto profileRecord=parseNativeStreamProfileRecord(*entry.bytes);require(entry.receipt&&f.journal,"Actual resize producer omitted completion evidence");const auto receipt=parseNativeStreamViewportReceipt(*entry.receipt);const auto lease=nativeStreamProfileLease(f.state(),GetCurrentProcessId());
      require(profileTrackRestoreReceiptMatches(profileRecord,request,receipt,*f.journal,f.baselineBytes.value_or(""),lease)==(scenario!=0&&scenario!=6),"Service accepted an applied change as rollback or rejected the completed recovery receipt");
      if(scenario!=0&&scenario!=6){require(!profileTrackRestoreReceiptMatches(profileRecord,request,receipt,*f.journal+" ",f.baselineBytes.value_or(""),lease),"Service restore accepted a changed runtime");auto wrong=request;wrong.resizeId="00000000-0000-0000-0000-000000000351";require(!profileTrackRestoreReceiptMatches(profileRecord,wrong,receipt,*f.journal,f.baselineBytes.value_or(""),lease),"Service restore adopted another pending request");auto changed=lease;++changed.generation;require(!profileTrackRestoreReceiptMatches(profileRecord,request,receipt,*f.journal,f.baselineBytes.value_or(""),changed),"Service restore trusted a retired lease");}
    }
    if(scenario==0||scenario==6){require(result.changed&&!result.restored&&result.source&&result.applied&&profile.active(),"Completed SDK group resize did not bind a fresh source");require(result.source->topology==request.topologyEpoch&&result.source->outputs[0].media.width==request.outputs[0].width,"Changed source used obsolete topology or dimensions");++changedCount;}
    else if(scenario>=1&&scenario<=3){const auto restored=profileTrackRestoredRequest(current,request);require(result.restored&&!result.changed&&result.source&&result.applied&&profile.active(),"Verified rollback/unchanged profile did not produce a resumable source");require(nativeSameTrackResize(*result.applied,restored)&&result.source->topology==request.topologyEpoch&&result.source->outputs[0].media.width==initial.outputs[0].width,"Rollback replayed an old topology or speculative dimensions");const auto packet=JsonParser::parse(profileTrackRestoredPacket(*result.applied,result.reason));require(nativeSameTrackResize(nativeTrackResizeRequest(packet),restored)&&std::get<Json::Object>(packet.value).size()==8&&profileTrackRestoredMatches(nativeInitialTrackViewport(current),request,packet),"Restored protocol descriptor did not bind exact pending request");auto changed=packet;streamField(changed,"topologyEpoch",initial.topologyEpoch);require(!profileTrackRestoredMatches(nativeInitialTrackViewport(current),request,changed),"Restored descriptor replayed an old media/input topology");if(scenario==3)require(f.transport.creates==beforeCreates,"Unchanged cancellation mutated SDK outputs");++resumed;}
    else{require(!result.changed&&!result.restored&&!result.source&&!result.applied,"Unproved completion or changed fresh geometry resumed media");++terminal;}
    if(scenario!=4){require(profile.finish(),"Profile restore failed after group resize disposition");const auto record=parseNativeStreamProfileRecord(*entry.bytes);require(record.phase==NativeStreamProfilePhase::restored&&nativeStreamOriginalPrimaryMatches(record.profile,f.ccd,nativeDisplayTargets(f.state()))&&nativeStreamProfileSignatures(record.profile.before,record.profile.targets)==nativeStreamProfileSignatures(f.ccd,nativeDisplayTargets(f.state())),"Group resize exit changed the console or foreign output");require(f.service->releaseStreamProfile(initial.sessionId),"Restored service reservation was not released");}
    f.transport.checkForeign();++workflows;
  }
  std::cout<<"{\"kind\":\"profile-track-resize\",\"workflows\":"<<workflows<<",\"changed\":"<<changedCount<<",\"resumableSources\":"<<resumed<<",\"terminal\":"<<terminal<<",\"actualSdkAndPrivateDispatcher\":true,\"actualProfileWorkerJournalAndReceipts\":true,\"journalStorage\":\"memory\",\"ccdCaptureProcessAndAcl\":\"injected\",\"actualAgentStarted\":false,\"actualPrivatePipeExchanges\":0,\"installedSdkAuthority\":false,\"windowsDisplayChanges\":0,\"runtimeEnabled\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<" after "<<workflows<<" workflows\n";return 1;}}
