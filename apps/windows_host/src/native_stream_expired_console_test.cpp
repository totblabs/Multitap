#include "native_stream_expired_console.hpp"
#include <iostream>
using namespace host;
namespace {
int checks=0;
void require(bool condition,const char* text){if(!condition)throw std::runtime_error(text);++checks;}
template<class F>void rejects(F f,const char* text){bool threw=false;try{f();}catch(...){threw=true;}require(threw,text);}
LUID luid(DWORD n){return {n,-1};}
void add(NativeCcdData& c,LUID a,UINT32 source,UINT32 target,DWORD width,DWORD height,LONG x,LONG y,bool virtualMode=false,bool desktop=false){
  DISPLAYCONFIG_PATH_INFO p{};p.flags=DISPLAYCONFIG_PATH_ACTIVE|(virtualMode?DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE:0);p.sourceInfo.adapterId=p.targetInfo.adapterId=a;p.sourceInfo.id=source;p.targetInfo.id=target;p.targetInfo.targetAvailable=TRUE;p.sourceInfo.statusFlags=DISPLAYCONFIG_SOURCE_IN_USE;p.targetInfo.statusFlags=DISPLAYCONFIG_TARGET_IN_USE;
  p.targetInfo.outputTechnology=DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;p.targetInfo.rotation=DISPLAYCONFIG_ROTATION_IDENTITY;p.targetInfo.scaling=DISPLAYCONFIG_SCALING_IDENTITY;p.targetInfo.refreshRate={60,1};p.targetInfo.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  const auto si=UINT32(c.modes.size());DISPLAYCONFIG_MODE_INFO s{};s.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;s.adapterId=a;s.id=source;s.sourceMode={width,height,DISPLAYCONFIG_PIXELFORMAT_32BPP,{x,y}};c.modes.push_back(s);
  const auto ti=UINT32(c.modes.size());DISPLAYCONFIG_MODE_INFO t{};t.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;t.adapterId=a;t.id=target;auto& signal=t.targetMode.targetVideoSignalInfo;signal.pixelRate=uint64_t(width+160)*(height+30)*60;signal.hSyncFreq={67500,1};signal.vSyncFreq={60,1};signal.activeSize={width,height};signal.totalSize={width+160,height+30};signal.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;c.modes.push_back(t);
  if(virtualMode){p.sourceInfo.cloneGroupId=DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;p.sourceInfo.sourceModeInfoIdx=si;p.targetInfo.targetModeInfoIdx=ti;p.targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    if(desktop){p.targetInfo.desktopModeInfoIdx=UINT32(c.modes.size());DISPLAYCONFIG_MODE_INFO image{};image.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE;image.adapterId=a;image.id=source;image.desktopImageInfo.PathSourceSize={LONG(width),LONG(height)};image.desktopImageInfo.DesktopImageRegion={3,4,LONG(width)-5,LONG(height)-6};image.desktopImageInfo.DesktopImageClip={8,9,LONG(width)-10,LONG(height)-11};c.modes.push_back(image);}
  }else{p.sourceInfo.modeInfoIdx=si;p.targetInfo.modeInfoIdx=ti;}c.paths.push_back(p);
}
struct Fixture {
  std::string machine="00112233-4455-6677-8899-aabbccddeeff";
  NativeStreamExpiredConsoleContext context;NativeCcdData fresh;NativeRecoveryScope scope;
  NativeConsoleAdapterIdentity console,owned,foreign;bool monitorChanged=false,resolveOwned=true;
  Fixture(int count=1,bool extra=false,bool virtualMode=false,bool desktop=false){
    console={luid(77),L"\\\\?\\PCI#VEN_1234&DEV_1111#CONSOLE",L"PCI\\VEN_1234&DEV_1111\\00",L"PCI",{L"PCI\\VEN_1234&DEV_1111"},nativeConsoleDisplayClass,true,true};
    owned={luid(33),L"\\\\?\\ROOT#DISPLAY#OWNED",L"ROOT\\DISPLAY\\00A1",L"ROOT",{L"ROOT\\DISPLAY"},nativeConsoleDisplayClass,true,true};
    foreign={luid(88),L"\\\\?\\PCI#VEN_10DE&DEV_0001#FOREIGN",L"PCI\\VEN_10DE&DEV_0001\\00",L"PCI",{L"PCI\\VEN_10DE&DEV_0001"},nativeConsoleDisplayClass,true,true};
    NativeCcdData original;add(original,luid(77),0,9,1600,900,0,0,virtualMode,desktop);std::vector<NativeDisplayTarget> targets;
    for(int i=0;i<count;++i){add(original,luid(33),UINT32(i),UINT32(100+i),1920,1080,LONG(1600+i*1920),0,virtualMode,desktop);targets.push_back({std::to_string(i),luid(33),UINT32(100+i),1920,1080});}
    if(extra)add(original,luid(88),0,50,800,600,-800,0,virtualMode,desktop);
    context.profile=planNativeStreamPrimaryProfile(original,targets,{luid(77),luid(77),0,9});context.console={{machine,"std"},console,{luid(77),luid(77),0,9}};context.ownedInstance="ROOT\\DISPLAY\\00a1";
    context.scope={2,std::string(64,'b'),1000,{}};for(const auto& p:original.paths)if(!nativeOwnedPath(p,targets))context.scope.foreign.push_back(binding(p));
    fresh=nativeForeignCcd(context.profile.applied,targets);
    auto inactive=original.paths.front();inactive.flags&=~DISPLAYCONFIG_PATH_ACTIVE;
    if(inactive.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE){inactive.sourceInfo.sourceModeInfoIdx=DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;inactive.targetInfo.targetModeInfoIdx=DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;inactive.targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;}else inactive.sourceInfo.modeInfoIdx=inactive.targetInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    fresh.paths.push_back(inactive);rescope();
  }
  NativeForeignBinding binding(const DISPLAYCONFIG_PATH_INFO& p)const{
    const auto path=p.targetInfo.adapterId.LowPart==77?utf8(console.interfacePath):p.targetInfo.adapterId.LowPart==88?utf8(foreign.interfacePath):utf8(owned.interfacePath);
    return {p.sourceInfo.adapterId,p.targetInfo.adapterId,p.sourceInfo.id,p.targetInfo.id,1,path,path,monitorChanged?"changed-monitor":"monitor-"+std::to_string(p.targetInfo.adapterId.LowPart),true};
  }
  void rescope(){scope={2,std::string(64,'b'),2000,{}};for(const auto& p:fresh.paths)if(p.flags&DISPLAYCONFIG_PATH_ACTIVE)scope.foreign.push_back(binding(p));}
  NativeStreamExpiredConsoleAccess access(){return {[&](LUID a)->std::optional<NativeConsoleAdapterIdentity>{if(nativeSameLuid(a,console.adapter))return console;if(nativeSameLuid(a,foreign.adapter))return foreign;if(resolveOwned&&nativeSameLuid(a,owned.adapter))return owned;return {};},[&](const auto& p){return binding(p);}};}
  NativeStreamExpiredConsolePlan plan(uint64_t now=2000){return planNativeStreamExpiredConsole(context,fresh,scope,machine,access(),now);}
};
void successful(){
  for(const auto count:{1,2})for(const bool foreign:{false,true})for(const bool virtualMode:{false,true})for(const bool desktop:{false,true}){
    if(desktop&&!virtualMode)continue;
    Fixture f(count,foreign,virtualMode,desktop);const auto p=f.plan();
    require(!p.alreadyRestored&&p.restored.paths.size()==(foreign?2:1),"Expired display planner recreated owned outputs");
    require(nativeStreamProfileSignatures(p.restored)==nativeStreamProfileSignatures(f.context.profile.before,f.context.profile.targets),"Console/foreign original geometry not restored");
    require(std::none_of(p.restored.paths.begin(),p.restored.paths.end(),[&](const auto& x){return nativeOwnedPath(x,f.context.profile.targets);}),"Dead owned target replayed");
    f.fresh=p.restored;f.scope=p.scopeAfter;const auto again=f.plan();require(again.alreadyRestored&&nativeStreamProfileSignatures(again.before)==nativeStreamProfileSignatures(again.restored),"Lost-ACK re-observation was not an exact no-op");
  }
  {Fixture f(2,true,true,true);const auto p=f.plan();const auto& original=f.context.profile.before;const auto expected=original.modes[original.paths.front().targetInfo.desktopModeInfoIdx];auto indexed=p.restored;const auto length=UINT32(indexed.modes.size());std::reverse(indexed.modes.begin(),indexed.modes.end());for(auto& path:indexed.paths){const auto source=length-1-nativeSourceIndex(path),target=length-1-nativeTargetIndex(path);if(path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE){if(path.targetInfo.desktopModeInfoIdx!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID)path.targetInfo.desktopModeInfoIdx=length-1-path.targetInfo.desktopModeInfoIdx;path.sourceInfo.sourceModeInfoIdx=source;path.targetInfo.targetModeInfoIdx=target;}else{path.sourceInfo.modeInfoIdx=source;path.targetInfo.modeInfoIdx=target;}}
    f.fresh=indexed;f.scope=p.scopeAfter;const auto again=f.plan();const auto console=std::find_if(again.restored.paths.begin(),again.restored.paths.end(),[&](const auto& path){return nativeStreamConsolePathMatches(path,*f.context.profile.suppressedConsole);});require(console!=again.restored.paths.end()&&nativeSameMode(expected,again.restored.modes[console->targetInfo.desktopModeInfoIdx]),"DESKTOP_IMAGE/source-local clipping changed across restoration and index reorder");require(again.alreadyRestored,"Reindexed desktop-image no-op reapplied display");}
  {Fixture f;auto possible=f.fresh.paths.back();possible.sourceInfo.id=4;f.fresh.paths.push_back(possible);require(f.plan().restored.paths.size()==1,"Other inactive possible QDC combination treated as another active screen");}
}
void rejected(){
  {Fixture f;f.machine="11112233-4455-6677-8899-aabbccddeeff";rejects([&]{f.plan();},"Different VM accepted");}
  {Fixture f;f.scope.session=3;rejects([&]{f.plan();},"Different Windows session accepted");}
  {Fixture f;f.scope.logon=std::string(64,'c');rejects([&]{f.plan();},"New login inherited old geometry");}
  {Fixture f;f.scope.tick=999;rejects([&]{f.plan();},"Clock/logon regression accepted");}
  {Fixture f;rejects([&]{f.plan(5001);},"Stale CCD/identity observation accepted");}
  {Fixture f;rejects([&]{f.plan(1999);},"Future observation accepted");}
  {Fixture f;f.console.started=false;rejects([&]{f.plan();},"Stopped console adapter accepted");}
  {Fixture f;f.console.instanceId+=L"OTHER";rejects([&]{f.plan();},"Different console instance accepted");}
  {Fixture f;f.console.hardwareIds={L"PCI\\VEN_10DE&DEV_1111"};rejects([&]{f.plan();},"Unverified PCI hardware accepted");}
  {Fixture f;f.monitorChanged=true;rejects([&]{f.plan();},"Inactive console monitor identity change ignored");}
  {Fixture f;f.fresh.paths.clear();rejects([&]{f.plan();},"Missing inactive QEMU availability accepted");}
  {Fixture f;f.fresh.paths.back().targetInfo.targetAvailable=FALSE;rejects([&]{f.plan();},"Unavailable console accepted");}
  {Fixture f;f.fresh.paths.push_back(f.fresh.paths.back());rejects([&]{f.plan();},"Ambiguous exact inactive console accepted");}
  {Fixture f;f.fresh.paths.back().sourceInfo.id++;rejects([&]{f.plan();},"Different console source ID accepted");}
  {Fixture f;f.fresh.paths.back().targetInfo.id++;rejects([&]{f.plan();},"Different console target ID accepted");}
  {Fixture f;add(f.fresh,luid(33),0,100,1920,1080,0,0);f.rescope();rejects([&]{f.plan();},"Still-active original owned output accepted as expired");}
  {Fixture f;add(f.fresh,luid(33),4,444,1920,1080,0,0);f.rescope();rejects([&]{f.plan();},"Unrecorded current owned target accepted as foreign");}
  {Fixture f;add(f.fresh,luid(999),4,444,1920,1080,0,0);f.rescope();rejects([&]{f.plan();},"Unresolved active adapter accepted");}
  {Fixture f(1,true);f.fresh.modes[nativeSourceIndex(f.fresh.paths.front())].sourceMode.position.y++;rejects([&]{f.plan();},"Independent foreign movement overwritten");}
  {Fixture f(1,true);f.fresh.paths.erase(f.fresh.paths.begin());f.rescope();rejects([&]{f.plan();},"Removed foreign monitor recreated");}
  {Fixture f(1,true);f.scope.foreign.front().monitorPath="other";rejects([&]{f.plan();},"Foreign monitor identity change ignored");}
  {Fixture f;const auto p=f.plan();f.fresh=p.restored;f.scope=p.scopeAfter;f.fresh.modes[nativeSourceIndex(f.fresh.paths.front())].sourceMode.position.x=2;rejects([&]{f.plan();},"Changed restored console accepted as no-op");}
  {Fixture f;f.context.profile.before=translateNativeStreamProfile(f.context.profile.before,-1600,0);f.context.profile=planNativeStreamPrimaryProfile(f.context.profile.before,f.context.profile.targets,*f.context.profile.suppressedConsole);rejects([&]{f.plan();},"Fallback invented a primary when original primary was owned");}
  {Fixture f;f.context.scope.foreign.front().complete=false;rejects([&]{f.plan();},"Incomplete saved monitor binding accepted");}
  {Fixture f;f.context.scope.foreign.front().monitorPath.clear();rejects([&]{f.plan();},"Empty saved monitor identity accepted");}
  {Fixture f;f.context.profile.suppressedConsole.reset();rejects([&]{f.plan();},"Non-exclusive profile admitted expired-console fallback");}
}
}
int main(){try{successful();rejected();std::cout<<"{\"checks\":"<<checks<<",\"actualWindowsDisplayChanges\":0,\"sdkLeaseProofInjected\":false,\"plannerOnly\":true}\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
