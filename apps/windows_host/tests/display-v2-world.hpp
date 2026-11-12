#pragma once
// In-memory model of Windows display configuration (CCD) plus the SDK's owned
// outputs "0" and "1", shared by the display manager v2 QA
// (display_v2_test.cpp, display_v2_service_test.cpp). Nothing here queries or
// changes this PC's displays, drivers or registry.
#include "../src/display_v2.hpp"
namespace v2world {
using namespace host;
inline void worldRequire(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
inline bool same(LUID a,LUID b){return a.LowPart==b.LowPart&&a.HighPart==b.HighPart;}
inline const LUID consoleAdapter{7,0},ownedAdapter{44,0},extraAdapter{8,0};
inline constexpr UINT32 consoleTarget=9,extraTarget=19;

struct Display {LUID adapter{};UINT32 source=0,target=0;bool virtualMode=true,active=false;LONG x=0,y=0;DWORD width=0,height=0;UINT32 refreshN=60,refreshD=1;DISPLAYCONFIG_ROTATION rotation=DISPLAYCONFIG_ROTATION_IDENTITY;DWORD dbWidth=0,dbHeight=0;DISPLAYCONFIG_PIXELFORMAT format=DISPLAYCONFIG_PIXELFORMAT_32BPP;};
struct World {
  std::vector<Display> displays;
  // SDK output "0" (source 0) and output "1" (source 1; target 0 = absent).
  UINT32 ownedTarget=1;DWORD ownedWidth=1920,ownedHeight=1080;
  UINT32 secondTarget=0,nextSecond=100;DWORD secondWidth=0,secondHeight=0;
  uint64_t clock=1000;bool eligible=true,failResize=false,failCount=false,failSizes=false,autoActivate=false,consoleReturns=true;
  // The console adapter's LUID; a Windows restart gives it a new one.
  LUID consoleLuid=consoleAdapter;
  // Timing model (resize stage measurements): the SDK size call takes sdkMs of
  // world time, and Windows lists a recreated owned target only arrivalMs
  // after it (hidden from query() until then).
  uint64_t sdkMs=0,arrivalMs=0,hiddenUntil=0;UINT32 hiddenTarget=0;
  int applies=0,validations=0,resizes=0,counts=0,topologyApplies=0,sizeCalls=0;
  std::function<LONG(const NativeCcdData&,UINT32)> reject;
  // Windows' own reaction after an SDK size change, after an apply, and after
  // an owned output arrived or departed (count change).
  std::function<void(World&)> afterResize,afterApply,afterCount;
  explicit World(bool extra=false){
    displays.push_back({consoleAdapter,0,consoleTarget,true,true,0,0,1024,768,60,1,DISPLAYCONFIG_ROTATION_IDENTITY,1024,768});
    displays.push_back({ownedAdapter,0,ownedTarget,true,true,1024,0,1920,1080,60,1,DISPLAYCONFIG_ROTATION_IDENTITY,1920,1080});
    if(extra)displays.push_back({extraAdapter,0,extraTarget,false,true,2944,0,800,600,75,1,DISPLAYCONFIG_ROTATION_IDENTITY,800,600});
  }
  Display* find(LUID adapter,UINT32 target){for(auto& d:displays)if(same(d.adapter,adapter)&&d.target==target)return &d;return nullptr;}
  Display& console(){return *find(consoleLuid,consoleTarget);}
  // Windows restart: the console adapter comes back under a new LUID.
  void restartConsoleAdapter(LUID next){for(auto& d:displays)if(same(d.adapter,consoleLuid))d.adapter=next;consoleLuid=next;}
  Display& owned(){return *find(ownedAdapter,ownedTarget);}
  Display& second(){return *find(ownedAdapter,secondTarget);}
  Display& extra(){return *find(extraAdapter,extraTarget);}
  bool hasSecond()const{return secondTarget!=0;}
  NativeDisplayTarget target()const{return {"0",ownedAdapter,ownedTarget,ownedWidth,ownedHeight};}
  std::vector<NativeDisplayTarget> targets()const{std::vector<NativeDisplayTarget> out{target()};if(secondTarget)out.push_back({"1",ownedAdapter,secondTarget,secondWidth,secondHeight});return out;}
  // The SDK size an owned target exposes (Windows cannot pick another mode).
  std::pair<DWORD,DWORD> sdkSize(UINT32 target)const{if(secondTarget&&target==secondTarget)return {secondWidth,secondHeight};return {ownedWidth,ownedHeight};}
  NativeCcdData query()const{
    NativeCcdData ccd;
    for(const auto& d:displays){
      if(hiddenTarget&&clock<hiddenUntil&&same(d.adapter,ownedAdapter)&&d.target==hiddenTarget)continue;
      DISPLAYCONFIG_PATH_INFO p{};p.flags=(d.active?DISPLAYCONFIG_PATH_ACTIVE:0)|(d.virtualMode?DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE:0);
      p.sourceInfo.adapterId=p.targetInfo.adapterId=d.adapter;p.sourceInfo.id=d.source;p.targetInfo.id=d.target;p.targetInfo.targetAvailable=TRUE;p.targetInfo.rotation=d.rotation;p.targetInfo.scaling=DISPLAYCONFIG_SCALING_IDENTITY;p.targetInfo.refreshRate={d.refreshN,d.refreshD};p.targetInfo.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
      UINT32 source=0,target=0;
      if(d.active){
        DISPLAYCONFIG_MODE_INFO s{};s.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;s.adapterId=d.adapter;s.id=d.source;s.sourceMode={d.width,d.height,d.format,{d.x,d.y}};source=UINT32(ccd.modes.size());ccd.modes.push_back(s);
        DISPLAYCONFIG_MODE_INFO t{};t.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;t.adapterId=d.adapter;t.id=d.target;auto& v=t.targetMode.targetVideoSignalInfo;v.activeSize={d.width,d.height};v.totalSize={d.width+160,d.height+30};v.vSyncFreq={d.refreshN,d.refreshD};v.hSyncFreq={67500,1};v.pixelRate=uint64_t(d.width+160)*(d.height+30)*d.refreshN/d.refreshD;v.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;target=UINT32(ccd.modes.size());ccd.modes.push_back(t);
      }
      if(d.virtualMode){p.sourceInfo.cloneGroupId=DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;p.sourceInfo.sourceModeInfoIdx=d.active?source:DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;p.targetInfo.targetModeInfoIdx=d.active?target:DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;p.targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;}
      else{p.sourceInfo.modeInfoIdx=d.active?source:DISPLAYCONFIG_PATH_MODE_IDX_INVALID;p.targetInfo.modeInfoIdx=d.active?target:DISPLAYCONFIG_PATH_MODE_IDX_INVALID;}
      ccd.paths.push_back(p);
    }
    return ccd;
  }
  LONG configure(const NativeCcdData& plan,UINT32 options){
    worldRequire(!(options&(SDC_SAVE_TO_DATABASE|SDC_TOPOLOGY_EXTEND|SDC_TOPOLOGY_CLONE|SDC_TOPOLOGY_INTERNAL|SDC_TOPOLOGY_EXTERNAL|SDC_USE_DATABASE_CURRENT)),"v2 used a persistent or global display flag");
    if(!eligible)return LONG(ERROR_ACCESS_DENIED);
    if(reject)if(const auto code=reject(plan,options))return code;
    const bool topology=(options&SDC_TOPOLOGY_SUPPLIED)!=0;
    if(topology)worldRequire(plan.modes.empty()&&!(options&SDC_USE_SUPPLIED_DISPLAY_CONFIG),"Topology-only apply carried modes");
    else worldRequire((options&SDC_USE_SUPPLIED_DISPLAY_CONFIG)!=0,"Unsupported display apply");
    struct Wanted {Display* display;LONG x=0,y=0;DWORD w=0,h=0;UINT32 rn=60,rd=1;DISPLAYCONFIG_ROTATION rotation=DISPLAYCONFIG_ROTATION_IDENTITY;DISPLAYCONFIG_PIXELFORMAT format=DISPLAYCONFIG_PIXELFORMAT_32BPP;};std::vector<Wanted> wanted;
    for(const auto& p:plan.paths){
      if(!(p.flags&DISPLAYCONFIG_PATH_ACTIVE))continue;
      auto* d=find(p.targetInfo.adapterId,p.targetInfo.id);if(!d)return LONG(ERROR_GEN_FAILURE);
      if(!same(p.sourceInfo.adapterId,d->adapter)||p.sourceInfo.id!=d->source)return LONG(ERROR_INVALID_PARAMETER);
      Wanted w{d,0,0,d->dbWidth,d->dbHeight,d->refreshN,d->refreshD,d->rotation,d->format};
      if(!topology){
        const auto& s=nativeCcdSource(plan,p);w.x=s.position.x;w.y=s.position.y;w.w=s.width;w.h=s.height;w.rotation=p.targetInfo.rotation;w.format=s.pixelFormat;
        const UINT32 invalid=(p.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)?DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID:DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        if(nativeTargetIndex(p)!=invalid){const auto& t=nativeCcdTarget(plan,p).targetVideoSignalInfo;w.rn=t.vSyncFreq.Numerator;w.rd=t.vSyncFreq.Denominator;}
        else if(p.targetInfo.refreshRate.Denominator){w.rn=p.targetInfo.refreshRate.Numerator;w.rd=p.targetInfo.refreshRate.Denominator;}
      }
      wanted.push_back(w);
    }
    if(wanted.empty())return LONG(ERROR_INVALID_PARAMETER);
    // An owned output exposes only its SDK size.
    for(auto& w:wanted)if(same(w.display->adapter,ownedAdapter)){const auto [sw,sh]=sdkSize(w.display->target);if(w.w!=sw||w.h!=sh){if(!(options&SDC_ALLOW_CHANGES))return LONG(ERROR_INVALID_PARAMETER);w.w=sw;w.h=sh;}}
    if(topology){LONG x=0;for(auto& w:wanted){w.x=x;w.y=0;x+=LONG(w.w);}}
    const auto valid=[&]{bool origin=false;for(size_t i=0;i<wanted.size();++i){const RECT a{wanted[i].x,wanted[i].y,wanted[i].x+LONG(wanted[i].w),wanted[i].y+LONG(wanted[i].h)};origin=origin||(!a.left&&!a.top);for(size_t j=0;j<i;++j){const RECT b{wanted[j].x,wanted[j].y,wanted[j].x+LONG(wanted[j].w),wanted[j].y+LONG(wanted[j].h)};if(displayV2Overlap(a,b))return false;}}return origin;};
    if(!valid()){
      if(!(options&SDC_ALLOW_CHANGES))return LONG(ERROR_INVALID_PARAMETER);
      std::stable_sort(wanted.begin(),wanted.end(),[](const auto& a,const auto& b){return std::tuple{a.x,a.y}<std::tuple{b.x,b.y};});
      LONG x=0;for(auto& w:wanted){w.x=x;w.y=0;x+=LONG(w.w);}
    }
    if(options&SDC_VALIDATE){++validations;return LONG(ERROR_SUCCESS);}
    worldRequire((options&SDC_APPLY)!=0,"Neither validate nor apply");
    ++applies;if(topology)++topologyApplies;
    for(auto& d:displays)d.active=false;
    for(const auto& w:wanted){auto& d=*w.display;d.active=true;d.x=w.x;d.y=w.y;d.width=w.w;d.height=w.h;d.refreshN=w.rn;d.refreshD=w.rd;d.rotation=w.rotation;d.format=w.format;if(!same(d.adapter,ownedAdapter)){d.dbWidth=w.w;d.dbHeight=w.h;}}
    if(afterApply)afterApply(*this);
    return LONG(ERROR_SUCCESS);
  }
  // Windows reaction when the last active display disappears.
  void ownedDeparted(bool wasActive){
    if(wasActive&&consoleReturns&&std::none_of(displays.begin(),displays.end(),[](const auto& d){return d.active;})){auto& c=console();c.active=true;c.x=0;c.y=0;c.width=c.dbWidth;c.height=c.dbHeight;}
  }
  void appendOwned(UINT32 source,UINT32 target,DWORD width,DWORD height){
    Display fresh{ownedAdapter,source,target,true,false,0,0,width,height,60,1,DISPLAYCONFIG_ROTATION_IDENTITY,width,height};
    if(autoActivate){LONG right=0;for(const auto& d:displays)if(d.active)right=std::max(right,d.x+LONG(d.width));fresh.active=true;fresh.x=right;}
    displays.push_back(fresh);
  }
  void appendOwned(){appendOwned(0,ownedTarget,ownedWidth,ownedHeight);}
  bool removeTarget(UINT32 target){
    const auto* d=find(ownedAdapter,target);const bool wasActive=d&&d->active;
    displays.erase(std::remove_if(displays.begin(),displays.end(),[&](const auto& item){return same(item.adapter,ownedAdapter)&&item.target==target;}),displays.end());
    return wasActive;
  }
  // The SDK controller removes and re-appends a resized output (new target id).
  std::optional<std::vector<NativeDisplayTarget>> resize(const std::string& id,DWORD width,DWORD height){
    ++resizes;if(failResize)return {};
    clock+=sdkMs;
    if(id=="0"){ownedDeparted(removeTarget(ownedTarget));++ownedTarget;ownedWidth=width;ownedHeight=height;appendOwned();if(arrivalMs){hiddenTarget=ownedTarget;hiddenUntil=clock+arrivalMs;}}
    else if(id=="1"&&secondTarget){ownedDeparted(removeTarget(secondTarget));secondTarget=nextSecond++;secondWidth=width;secondHeight=height;appendOwned(1,secondTarget,width,height);}
    else return {};
    if(afterResize)afterResize(*this);
    return targets();
  }
  // Both owned outputs in one SDK reconfiguration (the real service's
  // setOwnedSizesV2): each changed output is recreated under a new target id;
  // Windows reacts once (afterResize).
  std::optional<std::vector<NativeDisplayTarget>> setSizes(std::span<const DisplayV2Output> outputs){
    ++sizeCalls;if(failSizes)return {};
    for(const auto& o:outputs)if((o.id!="0"&&o.id!="1")||(o.id=="1"&&!secondTarget))return {};
    bool changed=false;
    for(const auto& o:outputs){
      if(o.id=="0"&&(o.width!=ownedWidth||o.height!=ownedHeight)){ownedDeparted(removeTarget(ownedTarget));++ownedTarget;ownedWidth=o.width;ownedHeight=o.height;appendOwned();changed=true;}
      if(o.id=="1"&&(o.width!=secondWidth||o.height!=secondHeight)){ownedDeparted(removeTarget(secondTarget));secondTarget=nextSecond++;secondWidth=o.width;secondHeight=o.height;appendOwned(1,secondTarget,o.width,o.height);changed=true;}
    }
    if(changed&&afterResize)afterResize(*this);
    return targets();
  }
  // SDK owned output count: output 1 is added at output 0's size (as the real
  // service does) or removed; output 0 is never touched.
  std::optional<std::vector<NativeDisplayTarget>> setCount(size_t count){
    ++counts;if(failCount||(count!=1&&count!=2))return {};
    bool changed=false;
    if(count==2&&!secondTarget){secondTarget=nextSecond++;secondWidth=ownedWidth;secondHeight=ownedHeight;appendOwned(1,secondTarget,secondWidth,secondHeight);changed=true;}
    else if(count==1&&secondTarget){const auto old=secondTarget;secondTarget=0;ownedDeparted(removeTarget(old));changed=true;}
    if(changed&&afterCount)afterCount(*this);
    return targets();
  }
  // Follows an external SDK owner's lease (the real NativeDisplayService in the
  // service QA): departed targets disappear, new ones arrive like setCount.
  void sync(std::span<const NativeDisplayTarget> lease){
    bool changed=false;
    for(const auto& target:lease){
      if(target.id=="0"&&(target.target!=ownedTarget||target.width!=ownedWidth||target.height!=ownedHeight)){ownedDeparted(removeTarget(ownedTarget));ownedTarget=target.target;ownedWidth=target.width;ownedHeight=target.height;appendOwned();changed=true;}
      if(target.id=="1"&&(target.target!=secondTarget||target.width!=secondWidth||target.height!=secondHeight)){if(secondTarget)ownedDeparted(removeTarget(secondTarget));secondTarget=target.target;secondWidth=target.width;secondHeight=target.height;appendOwned(1,secondTarget,secondWidth,secondHeight);changed=true;}
    }
    if(secondTarget&&std::none_of(lease.begin(),lease.end(),[](const auto& t){return t.id=="1";})){const auto old=secondTarget;secondTarget=0;ownedDeparted(removeTarget(old));changed=true;}
    if(changed&&afterCount)afterCount(*this);
  }
  // Service crash/restart: the lease dies; the new service re-creates the
  // owned outputs at their committed size under new target ids.
  void serviceRestart(){
    ownedDeparted(removeTarget(ownedTarget));++ownedTarget;appendOwned();
    if(secondTarget){ownedDeparted(removeTarget(secondTarget));secondTarget=nextSecond++;appendOwned(1,secondTarget,secondWidth,secondHeight);}
  }
};
struct Memory {
  std::optional<std::string> bytes;int saves=0,removes=0,discards=0;bool failSave=false,throwLoad=false;
  DisplayV2Store store(){DisplayV2Store s;s.load=[this]{if(throwLoad)throw std::runtime_error("unreadable state");return bytes;};s.save=[this](const std::string& value){++saves;if(failSave)return false;bytes=value;return true;};s.remove=[this]{++removes;bytes.reset();return true;};s.discard=[this]{++discards;bytes.reset();throwLoad=false;};return s;}
};
// Stand-in for the positive std-VGA identification (exact console target).
inline std::optional<DisplayV2Console> identifyFixtureConsole(const NativeCcdData& ccd,std::span<const NativeDisplayTarget>){
  for(const auto& p:ccd.paths)if((p.flags&DISPLAYCONFIG_PATH_ACTIVE)&&same(p.targetInfo.adapterId,consoleAdapter)&&p.targetInfo.id==consoleTarget)return DisplayV2Console{consoleAdapter,consoleAdapter,p.sourceInfo.id,p.targetInfo.id};
  return {};
}
inline bool overlapping(World& w){std::vector<RECT> rects;for(const auto& d:w.displays)if(d.active)rects.push_back({d.x,d.y,d.x+LONG(d.width),d.y+LONG(d.height)});for(size_t i=0;i<rects.size();++i)for(size_t j=0;j<i;++j)if(displayV2Overlap(rects[i],rects[j]))return true;return false;}
// DisplaySessionV2 access over the model: World is both Windows and the SDK.
inline DisplayV2Access worldAccess(World& w,Memory& m,bool console=true){
  DisplayV2Access a;
  a.query=[&w]{return w.query();};a.configure=[&w](const NativeCcdData& ccd,UINT32 flags){return w.configure(ccd,flags);};
  a.owned=[&w]{return w.targets();};
  a.resize=[&w](const std::string& id,DWORD width,DWORD height){return w.resize(id,width,height);};
  a.sizes=[&w](std::span<const DisplayV2Output> outputs){return w.setSizes(outputs);};
  a.count=[&w](size_t count){return w.setCount(count);};
  if(console)a.identifyConsole=identifyFixtureConsole;
  a.store=m.store();a.eligible=[&w]{return w.eligible;};a.now=[&w]{return w.clock;};a.pause=[&w]{w.clock+=50;};
  return a;
}
inline void requireStreaming(World& w,DWORD width,DWORD height,bool consoleOff,const char* reason){
  const auto& o=w.owned();worldRequire(o.active&&o.x==0&&o.y==0&&o.width==width&&o.height==height&&w.console().active==!consoleOff&&!overlapping(w),reason);
}
inline void requireOriginal(World& w,bool extra,const char* reason){
  const auto& c=w.console();const auto& o=w.owned();
  worldRequire(c.active&&c.x==0&&c.y==0&&c.width==1024&&c.height==768&&o.active&&o.x==1024&&o.y==0&&o.width==1920&&o.height==1080&&w.ownedWidth==1920&&!overlapping(w),reason);
  if(extra){const auto& e=w.extra();worldRequire(e.active&&e.width==800&&e.height==600&&e.refreshN==75&&e.refreshD==1&&e.format==DISPLAYCONFIG_PIXELFORMAT_32BPP&&e.rotation==DISPLAYCONFIG_ROTATION_IDENTITY,reason);}
}
inline uint64_t deadline(World& w){return w.clock+90000;}
}
