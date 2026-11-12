#include "native_stream_primary_profile.hpp"
#include <iostream>
using namespace host;
namespace {
int cases=0;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F&& action,const char* message){bool rejected=false;try{action();}catch(...){rejected=true;}require(rejected,message);++cases;}
LUID adapter(DWORD value){return {value,-1};}
std::vector<NativeDisplayTarget> targets(){return {{"0",adapter(33),100,1920,1080},{"1",adapter(33),101,1280,720}};}
void addPath(NativeCcdData& ccd,LUID luid,UINT32 sourceId,UINT32 targetId,DWORD width,DWORD height,LONG x,LONG y,bool virtualMode=false,UINT32 existingSource=UINT32_MAX){
  DISPLAYCONFIG_PATH_INFO path{};path.flags=DISPLAYCONFIG_PATH_ACTIVE|(virtualMode?DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE:0);path.sourceInfo.adapterId=path.targetInfo.adapterId=luid;path.sourceInfo.id=sourceId;path.targetInfo.id=targetId;
  path.sourceInfo.statusFlags=DISPLAYCONFIG_SOURCE_IN_USE;path.targetInfo.statusFlags=DISPLAYCONFIG_TARGET_IN_USE;path.targetInfo.targetAvailable=TRUE;path.targetInfo.outputTechnology=DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;path.targetInfo.rotation=DISPLAYCONFIG_ROTATION_IDENTITY;path.targetInfo.scaling=DISPLAYCONFIG_SCALING_IDENTITY;path.targetInfo.refreshRate={60,1};path.targetInfo.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  UINT32 source=existingSource;
  if(source==UINT32_MAX){source=UINT32(ccd.modes.size());DISPLAYCONFIG_MODE_INFO mode{};mode.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;mode.adapterId=luid;mode.id=sourceId;mode.sourceMode={width,height,DISPLAYCONFIG_PIXELFORMAT_32BPP,{x,y}};ccd.modes.push_back(mode);}
  const auto target=UINT32(ccd.modes.size());DISPLAYCONFIG_MODE_INFO mode{};mode.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;mode.adapterId=luid;mode.id=targetId;auto& signal=mode.targetMode.targetVideoSignalInfo;signal.pixelRate=uint64_t(width+160)*(height+30)*60;signal.hSyncFreq={67500,1};signal.vSyncFreq={60,1};signal.activeSize={width,height};signal.totalSize={width+160,height+30};signal.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;ccd.modes.push_back(mode);
  if(virtualMode){path.sourceInfo.cloneGroupId=4;path.sourceInfo.sourceModeInfoIdx=source;path.targetInfo.targetModeInfoIdx=target;path.targetInfo.desktopModeInfoIdx=UINT32(ccd.modes.size());DISPLAYCONFIG_MODE_INFO image{};image.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE;image.adapterId=luid;image.id=sourceId;image.desktopImageInfo.PathSourceSize={LONG(width),LONG(height)};image.desktopImageInfo.DesktopImageRegion={3,4,LONG(width)-5,LONG(height)-6};image.desktopImageInfo.DesktopImageClip={8,9,LONG(width)-10,LONG(height)-11};ccd.modes.push_back(image);}
  else{path.sourceInfo.modeInfoIdx=source;path.targetInfo.modeInfoIdx=target;}
  ccd.paths.push_back(path);
}
NativeCcdData fixture(){NativeCcdData value;addPath(value,adapter(77),0,9,1600,900,0,0,true);addPath(value,adapter(33),0,100,1920,1080,1600,0);addPath(value,adapter(33),1,101,1280,720,3520,0,true);return value;}
NativeCcdData reindex(NativeCcdData value){const auto length=UINT32(value.modes.size());std::reverse(value.modes.begin(),value.modes.end());for(auto& path:value.paths)if(path.flags&DISPLAYCONFIG_PATH_ACTIVE){
  const auto source=length-1-nativeSourceIndex(path),target=length-1-nativeTargetIndex(path);
  if(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE){path.sourceInfo.sourceModeInfoIdx=source;path.targetInfo.targetModeInfoIdx=target;if(path.targetInfo.desktopModeInfoIdx!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID)path.targetInfo.desktopModeInfoIdx=length-1-path.targetInfo.desktopModeInfoIdx;}else{path.sourceInfo.modeInfoIdx=source;path.targetInfo.modeInfoIdx=target;}}
  return value;
}
void entryCases(){
  const auto original=fixture();const auto profile=planNativeStreamPrimaryProfile(original,targets());
  require(nativeCcdSource(profile.applied,profile.applied.paths[0]).position.x==-1600&&nativeCcdSource(profile.applied,profile.applied.paths[1]).position.x==0&&nativeCcdSource(profile.applied,profile.applied.paths[2]).position.x==1920,"Entry did not translate every source exactly once");
  require(nativeStreamProfileSignatures(original)==nativeStreamProfileSignatures(profile.before)&&nativeStreamPrimaryProfileMatches(profile,profile.applied),"Entry changed baseline or failed exact verification");++cases;
  require(nativeStreamProfileSignatures(original)==nativeStreamProfileSignatures(planNativeStreamPrimaryRestore(profile,profile.applied,targets())),"Exit failed to restore initial topology");++cases;
  {auto current=profile.applied;const auto noop=planNativeStreamPrimaryProfile(current,targets());require(nativeStreamProfileSignatures(noop.before)==nativeStreamProfileSignatures(noop.applied),"Existing monitor zero primary caused needless geometry change");++cases;}
  {auto vertical=original;vertical.modes[nativeSourceIndex(vertical.paths[1])].sourceMode.position={-200,900};vertical.modes[nativeSourceIndex(vertical.paths[2])].sourceMode.position={-200,1980};const auto p=planNativeStreamPrimaryProfile(vertical,targets());require(nativeCcdSource(p.applied,p.applied.paths[0]).position.x==200&&nativeCcdSource(p.applied,p.applied.paths[0]).position.y==-900&&nativeCcdSource(p.applied,p.applied.paths[2]).position.y==1080,"Negative/vertical layout lost its shared translation");require(nativeStreamProfileSignatures(planNativeStreamPrimaryRestore(p,p.applied,targets()))==nativeStreamProfileSignatures(vertical),"Negative/vertical round trip failed");++cases;}
  {const auto p=planNativeStreamPrimaryProfile(reindex(original),targets());require(nativeStreamProfileSignatures(p.applied)==nativeStreamProfileSignatures(profile.applied)&&nativeStreamPrimaryProfileMatches(profile,reindex(profile.applied)),"Mode array ordering affected primary profile");++cases;}
  {auto withInactive=original;DISPLAYCONFIG_PATH_INFO unavailable{};withInactive.paths.push_back(unavailable);withInactive.paths.resize(4096);withInactive.modes.resize(16384);const auto p=planNativeStreamPrimaryProfile(withInactive,targets());require(p.applied.paths.size()==3&&p.applied.modes.size()==8,"Inactive possible paths or orphan modes entered applied profile");++cases;}
  {auto clone=original;const auto source=nativeSourceIndex(clone.paths[0]);addPath(clone,adapter(77),0,10,1600,900,0,0,true,source);const auto p=planNativeStreamPrimaryProfile(clone,targets());require(nativeCcdSource(p.applied,p.applied.paths[0]).position.x==-1600&&nativeCcdSource(p.applied,p.applied.paths[3]).position.x==-1600,"Foreign clone translated shared source twice");require(nativeStreamProfileSignatures(planNativeStreamPrimaryRestore(p,p.applied,targets()))==nativeStreamProfileSignatures(clone),"Foreign clone restoration changed identity/priority");++cases;}
  {auto clone=original;addPath(clone,adapter(77),0,10,1600,900,0,0,true);const auto p=planNativeStreamPrimaryProfile(clone,targets());require(nativeCcdSource(p.applied,p.applied.paths[0]).position.x==-1600&&nativeCcdSource(p.applied,p.applied.paths[3]).position.x==-1600,"Duplicate equal source-mode entries diverged during translation");++cases;}
  {const auto& before=original.modes[original.paths[0].targetInfo.desktopModeInfoIdx];const auto& after=profile.applied.modes[profile.applied.paths[0].targetInfo.desktopModeInfoIdx];require(nativeSameMode(before,after),"Global origin change modified source-local desktop image fields");++cases;}
  {auto reordered=original;std::swap(reordered.paths[0],reordered.paths[1]);const auto p=planNativeStreamPrimaryProfile(reordered,targets());require(p.applied.paths[0].targetInfo.id==100&&p.applied.paths[1].targetInfo.id==9&&p.applied.paths[2].targetInfo.id==101,"Entry reordered path priority");++cases;}
  {auto ownedPrimary=translateNativeStreamProfile(original,-3520,0);const auto p=planNativeStreamPrimaryProfile(ownedPrimary,targets());require(nativeStreamProfileSignatures(planNativeStreamPrimaryRestore(p,p.applied,targets()))==nativeStreamProfileSignatures(ownedPrimary),"Original owned monitor one primary was not restored");++cases;}
}
void rejectionCases(){
  const auto original=fixture();const auto owned=targets();
  {auto invalid=original;invalid.paths.resize(4097);rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Raw path allocation bound absent");}
  {auto invalid=original;invalid.modes.resize(16385);rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Raw mode allocation bound absent");}
  {auto invalid=original;for(UINT32 i=0;i<62;++i)addPath(invalid,adapter(77),0,20+i,1600,900,0,0,false,0);rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Applied active path bound absent");}
  {auto invalid=owned;invalid[0].id="1";rejects([&]{planNativeStreamPrimaryProfile(original,invalid);},"Noncanonical owned identity accepted");}
  {auto invalid=owned;invalid[1]=invalid[0];invalid[1].id="1";rejects([&]{planNativeStreamPrimaryProfile(original,invalid);},"Duplicate owned target accepted");}
  {auto invalid=original;invalid.paths[1].targetInfo.targetAvailable=FALSE;rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Unavailable owned target accepted");}
  {auto invalid=original;invalid.paths[1].flags&=~DISPLAYCONFIG_PATH_ACTIVE;rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Inactive owned target accepted");}
  {auto invalid=original;invalid.paths.push_back(invalid.paths[0]);rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Duplicate target accepted");}
  {auto invalid=original;invalid.modes[nativeSourceIndex(invalid.paths[1])].sourceMode.position={0,0};rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Multiple distinct primary sources accepted");}
  {auto invalid=translateNativeStreamProfile(original,0,200);rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Missing primary source accepted");}
  {auto invalid=original;invalid.modes[nativeSourceIndex(invalid.paths[1])].sourceMode.position={1500,0};rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Owned/foreign rectangle overlap accepted");}
  {auto invalid=original;addPath(invalid,adapter(33),0,444,1920,1080,1600,0,false,nativeSourceIndex(invalid.paths[1]));rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Owned source shared with a foreign target accepted");}
  {auto invalid=original;addPath(invalid,adapter(77),0,444,1602,900,0,0);rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Conflicting duplicate source mode accepted");}
  {auto invalid=original;invalid.modes[nativeSourceIndex(invalid.paths[1])].sourceMode.width=1918;rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Lease/current dimensions mismatch accepted");}
  {auto invalid=original;invalid.modes[nativeSourceIndex(invalid.paths[2])].sourceMode.position.x=32760;rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Source endpoint bounds not checked");}
  {auto invalid=original;addPath(invalid,adapter(88),0,5,800,600,INT32_MIN,-700);rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Translated foreign coordinate underflow accepted");}
  {auto invalid=original;invalid.paths[1].sourceInfo.modeInfoIdx=UINT32_MAX;rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Invalid source mode index accepted");}
  {auto invalid=original;invalid.paths[1].targetInfo.modeInfoIdx=UINT32_MAX;rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Invalid target mode index accepted");}
  {auto invalid=original;invalid.modes[nativeTargetIndex(invalid.paths[1])].id=555;rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Target mode identity mismatch accepted");}
  {auto invalid=original;invalid.paths[1].sourceInfo.modeInfoIdx=nativeSourceIndex(invalid.paths[0]);rejects([&]{planNativeStreamPrimaryProfile(invalid,owned);},"Source identity alias accepted");}
  rejects([&]{translateNativeStreamProfile(original,INT64_MAX,0);},"Extreme translation could overflow intermediate arithmetic");
  rejects([&]{translateNativeStreamProfile(original,0,INT64_MIN);},"Negative extreme translation could overflow intermediate arithmetic");
}
void verificationCases(){
  const auto profile=planNativeStreamPrimaryProfile(fixture(),targets());
  for(int scenario=0;scenario<10;++scenario){auto changed=profile.applied;
    switch(scenario){
      case 0:changed.modes[nativeSourceIndex(changed.paths[0])].sourceMode.position.y+=1;break;
      case 1:changed.modes[nativeSourceIndex(changed.paths[0])].sourceMode.width+=1;break;
      case 2:changed.paths[0].targetInfo.refreshRate={75,1};break;
      case 3:changed.paths[0].sourceInfo.statusFlags=0;break;
      case 4:changed.paths[0].targetInfo.statusFlags=0;break;
      case 5:changed.paths[0].sourceInfo.cloneGroupId=9;break;
      case 6:changed.modes[changed.paths[0].targetInfo.desktopModeInfoIdx].desktopImageInfo.DesktopImageClip.left++;break;
      case 7:std::swap(changed.paths[0],changed.paths[1]);break;
      case 8:changed.paths[0].targetInfo.rotation=DISPLAYCONFIG_ROTATION_ROTATE90;break;
      case 9:changed.paths[0].targetInfo.scaling=DISPLAYCONFIG_SCALING_CENTERED;break;
    }
    require(!nativeStreamPrimaryProfileMatches(profile,changed),"Entry verifier accepted mode/identity/priority mutation");++cases;
    if(scenario!=7)rejects([&]{planNativeStreamPrimaryRestore(profile,changed,targets());},"Exit overwrote a foreign display edit");
  }
  for(int scenario=0;scenario<5;++scenario){auto corrupted=profile;
    switch(scenario){
      case 0:corrupted.applied.modes[nativeSourceIndex(corrupted.applied.paths[0])].sourceMode.position.x++;break;
      case 1:corrupted.before.modes[nativeSourceIndex(corrupted.before.paths[1])].sourceMode.position.x+=2;break;
      case 2:corrupted.targets[0].width+=2;break;
      case 3:corrupted.before.paths.push_back(DISPLAYCONFIG_PATH_INFO{});break;
      case 4:corrupted.applied.paths[0].sourceInfo.statusFlags=0;break;
    }
    rejects([&]{planNativeStreamPrimaryRestore(corrupted,profile.applied,targets());},"Inconsistent/tampered checkpoint used for restoration");
  }
  {auto disconnected=profile.applied;disconnected.paths.erase(disconnected.paths.begin());rejects([&]{planNativeStreamPrimaryRestore(profile,disconnected,targets());},"Missing original foreign primary accepted");}
  {auto fresh=translateNativeStreamProfile(profile.applied,-1920,0);rejects([&]{planNativeStreamPrimaryRestore(profile,fresh,targets());},"Independent primary change overwritten");}
  {auto stale=targets();stale[0].target=200;rejects([&]{planNativeStreamPrimaryRestore(profile,profile.applied,stale);},"Unobserved replacement target accepted");}
  {auto changed=profile.applied;changed.paths[0].targetInfo.id=999;changed.modes[nativeTargetIndex(changed.paths[0])].id=999;rejects([&]{planNativeStreamPrimaryRestore(profile,changed,targets());},"Replacement foreign target accepted");}
  {auto clone=fixture();addPath(clone,adapter(77),0,10,1600,900,0,0,true,0);const auto p=planNativeStreamPrimaryProfile(clone,targets());auto reordered=p.applied;std::swap(reordered.paths[0],reordered.paths[3]);rejects([&]{planNativeStreamPrimaryRestore(p,reordered,targets());},"Foreign path priority edit overwritten");}
  {auto reordered=profile.applied;std::swap(reordered.paths[0],reordered.paths[1]);const auto restored=planNativeStreamPrimaryRestore(profile,reordered,targets());require(restored.paths[0].targetInfo.id==100&&restored.paths[1].targetInfo.id==9,"Exit replayed original full priority instead of preserving fresh path ordering");++cases;}
  {auto saved=profile;saved.before=reindex(saved.before);saved.applied=reindex(saved.applied);validateNativeStreamPrimaryProfile(saved);require(nativeStreamProfileSignatures(planNativeStreamPrimaryRestore(saved,reindex(saved.applied),targets()))==nativeStreamProfileSignatures(profile.before),"Reindexed checkpoint lost normalized identity");++cases;}
}
void resizeCases(){
  const auto profile=planNativeStreamPrimaryProfile(fixture(),targets());auto latest=targets();latest[0].target=200;latest[0].width=2560;latest[0].height=1440;latest[1].target=201;
  auto fresh=nativeForeignCcd(profile.applied,profile.targets);addPath(fresh,adapter(33),2,200,2560,1440,0,0);addPath(fresh,adapter(33),3,201,1280,720,2560,0,true);
  const auto restored=planNativeStreamPrimaryRestore(profile,reindex(fresh),latest);
  require(nativeStreamProfileSignatures(profile.before,profile.targets)==nativeStreamProfileSignatures(restored,latest),"Resize restore changed original foreign geometry");
  const auto& zero=nativeCcdSource(restored,restored.paths[1]);const auto& second=nativeCcdSource(restored,restored.paths[2]);
  require(restored.paths[1].targetInfo.id==200&&restored.paths[2].targetInfo.id==201&&zero.width==2560&&zero.height==1440&&zero.position.x==1600&&second.position.x==4160,"Exit replayed retired targets or resized dimensions");++cases;
  {auto single=fresh;single.paths.pop_back();auto selected=latest;selected.pop_back();const auto result=planNativeStreamPrimaryRestore(profile,single,selected);require(result.paths.size()==2&&nativeCcdSource(result,result.paths[1]).width==2560,"Verified current one-monitor topology was expanded on exit");++cases;}
  {auto one=fixture();one.paths.pop_back();auto selected=targets();selected.pop_back();const auto entered=planNativeStreamPrimaryProfile(one,selected);auto two=nativeForeignCcd(entered.applied,entered.targets);addPath(two,adapter(33),0,100,1920,1080,0,0);addPath(two,adapter(33),1,101,1280,720,1920,0,true);const auto result=planNativeStreamPrimaryRestore(entered,two,targets());require(result.paths.size()==3&&nativeCcdSource(result,result.paths[2]).position.x==3520,"Verified added owned output was discarded on exit");++cases;}
  {auto noForeign=fixture();noForeign.paths.erase(noForeign.paths.begin());noForeign=translateNativeStreamProfile(nativeActiveCcd(noForeign),-3520,0);const auto entered=planNativeStreamPrimaryProfile(noForeign,targets());auto one=entered.applied;one.paths.pop_back();auto selected=targets();selected.pop_back();rejects([&]{planNativeStreamPrimaryRestore(entered,one,selected);},"Removed original owned primary was fabricated during exit");}
  {auto noForeign=fixture();noForeign.paths.erase(noForeign.paths.begin());noForeign=translateNativeStreamProfile(nativeActiveCcd(noForeign),-3520,0);const auto entered=planNativeStreamPrimaryProfile(noForeign,targets());auto changed=entered.applied;changed.modes[nativeSourceIndex(changed.paths[1])].sourceMode.position.x+=200;rejects([&]{planNativeStreamPrimaryRestore(entered,changed,targets());},"Changed original owned primary placement was silently rewritten");}
  {auto overflow=nativeForeignCcd(profile.applied,profile.targets);addPath(overflow,adapter(33),0,100,1920,1080,0,0);addPath(overflow,adapter(33),1,101,1280,720,31000,0,true);rejects([&]{planNativeStreamPrimaryRestore(profile,overflow,targets());},"Reverse translation endpoint overflow accepted");}
}
NativeStreamConsolePath consoleKey(){return {adapter(77),adapter(77),0,9};}
NativeCcdData withInactiveConsole(NativeCcdData current,const NativeStreamPrimaryProfile& profile){
  const auto identity=*profile.suppressedConsole;auto path=*std::find_if(profile.before.paths.begin(),profile.before.paths.end(),[&](const auto& value){return nativeStreamConsolePathMatches(value,identity);});path.flags&=~DISPLAYCONFIG_PATH_ACTIVE;
  if(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE){path.sourceInfo.sourceModeInfoIdx=DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;path.targetInfo.targetModeInfoIdx=DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;path.targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;}
  else path.sourceInfo.modeInfoIdx=path.targetInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;current.paths.push_back(path);return current;
}
void selectiveConsoleCases(){
  auto original=fixture();addPath(original,adapter(88),0,300,800,600,-800,0,true);
  const auto profile=planNativeStreamPrimaryProfile(original,targets(),consoleKey());
  require(profile.suppressedConsole.has_value()&&profile.applied.paths.size()==3&&std::none_of(profile.applied.paths.begin(),profile.applied.paths.end(),[](const auto& path){return nativeStreamConsolePathMatches(path,consoleKey());})&&nativeCcdSource(profile.applied,profile.applied.paths[0]).position.x==0,"Selective profile failed to suppress exact console and make owned zero primary");++cases;
  require(nativeStreamPrimaryProfileMatches(profile,reindex(withInactiveConsole(profile.applied,profile))),"Inactive console path polluted active profile verification");++cases;
  const auto restored=planNativeStreamPrimaryRestore(profile,withInactiveConsole(profile.applied,profile),targets());
  require(nativeStreamProfileSignatures(restored)==nativeStreamProfileSignatures(original),"Selective console restoration failed exact topology round trip");++cases;
  require(nativeStreamProfileSignatures(planNativeStreamPrimaryRestore(profile,original,targets()))==nativeStreamProfileSignatures(original),"Failed entry before OS mutation could not resolve as unchanged");++cases;
  require(nativeStreamProfileSignatures(planNativeStreamPrimaryRestore(profile,restored,targets()))==nativeStreamProfileSignatures(original),"Lost restore acknowledgement triggered a second geometry change");++cases;
  {const auto partial=translateNativeStreamProfile(original,-1600,0);const auto recovered=planNativeStreamPrimaryRestore(profile,partial,targets());require(nativeStreamProfileSignatures(recovered)==nativeStreamProfileSignatures(original),"Known partial entry with translated active console could not roll back");++cases;}
  {auto fresh=nativeForeignCcd(profile.applied,profile.targets);auto latest=targets();latest[0].target=200;latest[0].width=2560;latest[0].height=1440;latest[1].target=201;addPath(fresh,adapter(33),2,200,2560,1440,0,0);addPath(fresh,adapter(33),3,201,1280,720,2560,0,true);
    const auto first=planNativeStreamPrimaryRestore(profile,withInactiveConsole(fresh,profile),latest);require(nativeStreamProfileSignatures(first,latest)==nativeStreamProfileSignatures(original,profile.targets),"Resized profile failed console restore");++cases;
    auto twice=nativeForeignCcd(fresh,latest);latest[0].target=400;latest[0].width=1600;latest[0].height=900;latest[1].target=401;latest[1].width=1920;latest[1].height=1080;addPath(twice,adapter(33),4,400,1600,900,0,0);addPath(twice,adapter(33),5,401,1920,1080,1600,0,true);
    const auto final=planNativeStreamPrimaryRestore(profile,withInactiveConsole(reindex(twice),profile),latest);require(nativeStreamProfileSignatures(final,latest)==nativeStreamProfileSignatures(original,profile.targets)&&std::none_of(final.paths.begin(),final.paths.end(),[](const auto& path){return path.targetInfo.adapterId.LowPart==33&&(path.targetInfo.id==100||path.targetInfo.id==101||path.targetInfo.id==200||path.targetInfo.id==201);}),"Second resize replayed retired owned targets");++cases;
    const auto zero=std::find_if(final.paths.begin(),final.paths.end(),[&](const auto& path){return nativeSameTarget(path,latest[0]);});require(zero!=final.paths.end()&&nativeCcdSource(final,*zero).width==1600&&nativeCcdSource(final,*zero).position.x==1600,"Second resize lost current owned dimensions");++cases;
  }
  {auto unknown=consoleKey();unknown.target=444;rejects([&]{planNativeStreamPrimaryProfile(original,targets(),unknown);},"Unknown exact console key accepted");}
  {NativeStreamConsolePath ownedKey{adapter(33),adapter(33),0,100};rejects([&]{planNativeStreamPrimaryProfile(original,targets(),ownedKey);},"Owned output could be selected for suppression");}
  {auto clone=original;addPath(clone,adapter(77),0,10,1600,900,0,0,true,0);rejects([&]{planNativeStreamPrimaryProfile(clone,targets(),consoleKey());},"Console cloned into another target could be partially suppressed");}
  {auto duplicate=original;duplicate.paths.push_back(original.paths[0]);rejects([&]{planNativeStreamPrimaryProfile(duplicate,targets(),consoleKey());},"Ambiguous duplicated exact console path accepted");}
  {auto forged=profile;forged.suppressedConsole->source++;rejects([&]{planNativeStreamPrimaryRestore(forged,withInactiveConsole(profile.applied,profile),targets());},"Tampered suppressed source key accepted");}
  {auto forged=profile;forged.suppressedConsole.reset();rejects([&]{validateNativeStreamPrimaryProfile(forged);},"Selective profile accepted as legacy translation-only checkpoint");}
  rejects([&]{planNativeStreamPrimaryRestore(profile,profile.applied,targets());},"Missing inactive console availability proof accepted");
  {auto fresh=withInactiveConsole(profile.applied,profile);fresh.paths.back().targetInfo.id++;rejects([&]{planNativeStreamPrimaryRestore(profile,fresh,targets());},"Replaced inactive console target ID replayed");}
  {auto fresh=withInactiveConsole(profile.applied,profile);fresh.paths.back().sourceInfo.id++;rejects([&]{planNativeStreamPrimaryRestore(profile,fresh,targets());},"Replaced inactive console source ID replayed");}
  {auto fresh=withInactiveConsole(profile.applied,profile);fresh.paths.back().sourceInfo.adapterId.LowPart++;rejects([&]{planNativeStreamPrimaryRestore(profile,fresh,targets());},"Replaced inactive adapter LUID replayed");}
  {auto fresh=withInactiveConsole(profile.applied,profile);fresh.paths.back().targetInfo.targetAvailable=FALSE;rejects([&]{planNativeStreamPrimaryRestore(profile,fresh,targets());},"Unavailable console target restored");}
  {auto fresh=withInactiveConsole(profile.applied,profile);fresh.paths.push_back(fresh.paths.back());rejects([&]{planNativeStreamPrimaryRestore(profile,fresh,targets());},"Ambiguous inactive source/target availability accepted");}
  {auto fresh=withInactiveConsole(profile.applied,profile);addPath(fresh,adapter(77),0,10,1600,900,0,1200,true);rejects([&]{planNativeStreamPrimaryRestore(profile,fresh,targets());},"Console source reused by another active target overwritten");}
  {auto fresh=withInactiveConsole(profile.applied,profile);const auto physical=std::find_if(fresh.paths.begin(),fresh.paths.end(),[](const auto& path){return path.targetInfo.adapterId.LowPart==88;});fresh.modes[nativeSourceIndex(*physical)].sourceMode.position.y++;rejects([&]{planNativeStreamPrimaryRestore(profile,fresh,targets());},"Independent physical-display movement overwritten");}
  {auto fresh=translateNativeStreamProfile(original,-1600,0);fresh.paths[0].targetInfo.refreshRate={75,1};rejects([&]{planNativeStreamPrimaryRestore(profile,fresh,targets());},"Arbitrarily reenabled console mode treated as partial entry rollback");}
  {auto inactive=original;inactive.paths[0].flags&=~DISPLAYCONFIG_PATH_ACTIVE;rejects([&]{planNativeStreamPrimaryProfile(inactive,targets(),consoleKey());},"Already inactive candidate accepted for new suppression");}
  {auto fresh=withInactiveConsole(profile.applied,profile);fresh.paths.resize(4097);rejects([&]{planNativeStreamPrimaryRestore(profile,fresh,targets());},"Restore bypassed raw ALL_PATHS bound");}
}
}
int main(){try{entryCases();rejectionCases();verificationCases();resizeCases();selectiveConsoleCases();std::cout<<"{\"kind\":\"native-stream-primary-profile-geometry\",\"cases\":"<<cases<<",\"selectiveConsoleSuppression\":true,\"candidateOnly\":true,\"productionIntegration\":false,\"leaseAndLogonBinding\":\"required-from-future-coordinator\",\"journalImplemented\":false,\"windowsDisplayQueries\":0,\"windowsDisplayChanges\":0,\"driverCalls\":0}\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
