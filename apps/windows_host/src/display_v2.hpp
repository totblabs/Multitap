#pragma once
#include "native_display_topology.hpp"
#include "owned_output_size.hpp"
#include "native_display_journal.hpp"
#include "native_display_digest.hpp"
#include "display_v2_evidence.hpp"
#include <optional>
#include <span>
#include <tuple>

// Display manager v2 (docs/display-manager-v2.md).
//
// One state file remembers the Windows display configuration from before a
// stream changed it. Start/resize change only the owned virtual output (size +
// primary), or for independent outputs (tracks-v1) both owned outputs (sizes +
// "0" primary, "1" at the client's relative place), and deactivate only a
// positively identified console output. Every
// stop, failure and crash puts the saved configuration back. Positions of other
// displays are not identity; their active state, mode size, refresh rate and
// rotation are. Uncertainty is resolved by restoring, never by a latch.
//
// Everything here is injectable: tests drive it with an in-memory Windows/SDK
// fake. Production glue (pipe RPC to the service, CCD, protected file) lives in
// service.hpp.
namespace host {
struct DisplayV2Console {LUID sourceAdapter{},targetAdapter{};UINT32 source=0,target=0;};
inline bool displayV2ConsoleTarget(const DISPLAYCONFIG_PATH_INFO& path,const DisplayV2Console& console){
  return nativeSameLuid(path.targetInfo.adapterId,console.targetAdapter)&&path.targetInfo.id==console.target;
}
inline bool displayV2Active(const DISPLAYCONFIG_PATH_INFO& path){return (path.flags&DISPLAYCONFIG_PATH_ACTIVE)!=0;}
// True for an adapter positively identified as a retired Multitap-owned output
// (an older driver install). Never foreign: it is not saved, restored or counted
// as an unresolved adapter. An empty function retires nothing.
using DisplayV2Retired=std::function<bool(LUID)>;
inline bool displayV2RetiredPath(const DisplayV2Retired& retired,const DISPLAYCONFIG_PATH_INFO& path){return retired&&retired(path.targetInfo.adapterId);}
inline bool displayV2VirtualMode(const DISPLAYCONFIG_PATH_INFO& path){return (path.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)!=0;}

// The saved pre-stream configuration. `ccd` holds only active paths and the
// modes they reference (exact QueryDisplayConfig structures).
struct DisplayV2State {
  std::string session;
  NativeCcdData ccd;
  std::vector<NativeDisplayTarget> owned;
  std::optional<DisplayV2Console> console;
};

// ---------------------------------------------------------------- CCD helpers
using DisplayV2SourceKey=decltype(nativeSourceKey(DISPLAYCONFIG_PATH_INFO{}));
inline UINT32 displayV2CopyMode(NativeCcdData& out,std::map<UINT32,UINT32>& copied,const NativeCcdData& from,UINT32 index,DISPLAYCONFIG_MODE_INFO_TYPE type){
  const auto found=copied.find(index);if(found!=copied.end())return found->second;
  const auto& mode=nativeCcdMode(from,index,type);const auto destination=UINT32(out.modes.size());
  out.modes.push_back(mode);copied.emplace(index,destination);return destination;
}
// Copies one active path and every mode it references from `from` into `out`.
inline void displayV2CopyPath(NativeCcdData& out,std::map<UINT32,UINT32>& copied,const NativeCcdData& from,DISPLAYCONFIG_PATH_INFO path){
  const bool virtualMode=displayV2VirtualMode(path);
  const UINT32 invalidSource=virtualMode?DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID:DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
  const UINT32 invalidTarget=virtualMode?DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID:DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
  const auto sourceIndex=nativeSourceIndex(path),targetIndex=nativeTargetIndex(path);
  if(sourceIndex==invalidSource)throw std::runtime_error("display_v2_mode_invalid");
  const auto source=displayV2CopyMode(out,copied,from,sourceIndex,DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE);
  const auto target=targetIndex==invalidTarget?invalidTarget:displayV2CopyMode(out,copied,from,targetIndex,DISPLAYCONFIG_MODE_INFO_TYPE_TARGET);
  if(virtualMode){
    path.sourceInfo.sourceModeInfoIdx=source;path.targetInfo.targetModeInfoIdx=target;
    if(path.targetInfo.desktopModeInfoIdx!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID)path.targetInfo.desktopModeInfoIdx=displayV2CopyMode(out,copied,from,path.targetInfo.desktopModeInfoIdx,DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE);
  }else{path.sourceInfo.modeInfoIdx=source;path.targetInfo.modeInfoIdx=target;}
  out.paths.push_back(path);
}
// A shared saved source can return under a different source identity for one
// target. Give that path a separate source mode before rebinding it; mutating
// the shared cache entry would also change paths already copied from it.
inline void displayV2CopyForeignPath(NativeCcdData& out,std::map<UINT32,UINT32>& copied,const NativeCcdData& from,DISPLAYCONFIG_PATH_INFO path,const DISPLAYCONFIG_PATH_INFO& available){
  const bool moved=nativeSourceKey(path)!=nativeSourceKey(available);
  path.sourceInfo.adapterId=available.sourceInfo.adapterId;path.sourceInfo.id=available.sourceInfo.id;
  if(!moved){displayV2CopyPath(out,copied,from,path);return;}
  auto separate=copied;separate.erase(nativeSourceIndex(path));
  displayV2CopyPath(out,separate,from,path);
  auto& mode=out.modes[nativeSourceIndex(out.paths.back())];mode.adapterId=path.sourceInfo.adapterId;mode.id=path.sourceInfo.id;
}
inline NativeCcdData displayV2ActiveCcd(const NativeCcdData& ccd,const DisplayV2Retired& retired={}){
  NativeCcdData out;std::map<UINT32,UINT32> copied;
  for(const auto& path:ccd.paths)if(displayV2Active(path)&&!displayV2RetiredPath(retired,path))displayV2CopyPath(out,copied,ccd,path);
  return out;
}
// The owned output always gets a fresh 32bpp source mode at the requested
// size/position; Windows chooses its target mode (same as the v1 planner).
inline void displayV2AddOwned(NativeCcdData& out,DISPLAYCONFIG_PATH_INFO path,DWORD width,DWORD height,LONG left,LONG top,bool front){
  DISPLAYCONFIG_MODE_INFO mode{};mode.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;mode.adapterId=path.sourceInfo.adapterId;mode.id=path.sourceInfo.id;
  mode.sourceMode={width,height,DISPLAYCONFIG_PIXELFORMAT_32BPP,{left,top}};
  const auto source=UINT32(out.modes.size());out.modes.push_back(mode);
  if(displayV2VirtualMode(path)){path.sourceInfo.cloneGroupId=DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;path.sourceInfo.sourceModeInfoIdx=source;path.targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;path.targetInfo.targetModeInfoIdx=DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;}
  else{path.sourceInfo.modeInfoIdx=source;path.targetInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;}
  path.flags|=DISPLAYCONFIG_PATH_ACTIVE;path.flags&=~DISPLAYCONFIG_PATH_BOOST_REFRESH_RATE;
  path.targetInfo.rotation=DISPLAYCONFIG_ROTATION_IDENTITY;path.targetInfo.scaling=DISPLAYCONFIG_SCALING_IDENTITY;path.targetInfo.refreshRate={60,1};path.targetInfo.scanLineOrdering=DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  if(front)out.paths.insert(out.paths.begin(),path);else out.paths.push_back(path);
}
// Paths only, for SDC_TOPOLOGY_SUPPLIED (Windows picks modes from its database).
inline NativeCcdData displayV2PathsOnly(NativeCcdData ccd){
  for(auto& path:ccd.paths){
    if(displayV2VirtualMode(path)){path.sourceInfo.cloneGroupId=DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;path.sourceInfo.sourceModeInfoIdx=DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;path.targetInfo.targetModeInfoIdx=DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;path.targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;}
    else path.sourceInfo.modeInfoIdx=path.targetInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
  }
  ccd.modes.clear();return ccd;
}
// An available path for an owned target, preferring the active one, then the
// lowest free source on the owned adapter.
inline std::optional<size_t> displayV2OwnedPath(const NativeCcdData& fresh,const NativeDisplayTarget& target,const std::set<DisplayV2SourceKey>& taken){
  std::optional<size_t> best;
  for(size_t i=0;i<fresh.paths.size();++i){const auto& path=fresh.paths[i];
    if(!nativeSameTarget(path,target)||!path.targetInfo.targetAvailable||!nativeSameLuid(path.sourceInfo.adapterId,target.adapter)||taken.count(nativeSourceKey(path)))continue;
    if(!best){best=i;continue;}
    const auto& current=fresh.paths[*best];
    if(displayV2Active(path)!=displayV2Active(current)){if(displayV2Active(path))best=i;continue;}
    if(!displayV2Active(path)&&nativeSourceKey(path)<nativeSourceKey(current))best=i;
  }
  return best;
}
// Only the paths this planner will keep can reserve an available exact source.
// Reservations are not assignments: unchanged clones may share their exact
// source, while a fallback must not consume a later kept path's saved source.
inline std::set<DisplayV2SourceKey> displayV2ExactSources(const NativeCcdData& fresh,std::span<const DISPLAYCONFIG_PATH_INFO> kept){
  std::set<DisplayV2SourceKey> reserved;
  for(const auto& saved:kept)for(const auto& path:fresh.paths){
    if(nativeSameLuid(path.targetInfo.adapterId,saved.targetInfo.adapterId)&&path.targetInfo.id==saved.targetInfo.id&&path.targetInfo.targetAvailable&&nativeSourceKey(path)==nativeSourceKey(saved))reserved.insert(nativeSourceKey(path));
  }
  return reserved;
}
// An available path for a foreign (saved) target, preferring its saved source.
// One-shot console callers have no later foreign batch to reserve.
inline std::optional<size_t> displayV2ForeignPath(const NativeCcdData& fresh,const DISPLAYCONFIG_PATH_INFO& saved,const std::set<DisplayV2SourceKey>& taken,const std::set<DisplayV2SourceKey>& reserved={}){
  std::optional<size_t> best;
  for(size_t i=0;i<fresh.paths.size();++i){const auto& path=fresh.paths[i];
    if(!nativeSameLuid(path.targetInfo.adapterId,saved.targetInfo.adapterId)||path.targetInfo.id!=saved.targetInfo.id||!path.targetInfo.targetAvailable)continue;
    if(nativeSourceKey(path)==nativeSourceKey(saved))return i;
    if(!best&&!taken.count(nativeSourceKey(path))&&!reserved.count(nativeSourceKey(path)))best=i;
  }
  return best;
}
inline RECT displayV2Rect(const DISPLAYCONFIG_SOURCE_MODE& mode){if(!displayRectangleFits(mode.position.x,mode.position.y,mode.width,mode.height))throw std::runtime_error("display_v2_layout_bounds");return {mode.position.x,mode.position.y,LONG(int64_t(mode.position.x)+mode.width),LONG(int64_t(mode.position.y)+mode.height)};}
inline bool displayV2Overlap(const RECT& a,const RECT& b){return std::max(a.left,b.left)<std::min(a.right,b.right)&&std::max(a.top,b.top)<std::min(a.bottom,b.bottom);}
inline bool displayV2TargetActive(const NativeCcdData& ccd,LUID adapter,UINT32 target){
  return std::any_of(ccd.paths.begin(),ccd.paths.end(),[&](const auto& path){return displayV2Active(path)&&nativeSameLuid(path.targetInfo.adapterId,adapter)&&path.targetInfo.id==target;});
}
inline bool displayV2ConsoleActive(const NativeCcdData& ccd,const DisplayV2Console& console){return displayV2TargetActive(ccd,console.targetAdapter,console.target);}
// Re-supplies one display that was active in `from` (the configuration before
// the change) with its own `from` modes on the source Windows offers now.
inline bool displayV2KeepForeign(NativeCcdData& plan,std::map<UINT32,UINT32>& copied,std::set<DisplayV2SourceKey>& taken,const NativeCcdData& fresh,const NativeCcdData& from,const DISPLAYCONFIG_PATH_INFO& path,const std::set<DisplayV2SourceKey>& reserved={}){
  const auto match=displayV2ForeignPath(fresh,path,taken,reserved);
  if(!match){
    const bool available=std::any_of(fresh.paths.begin(),fresh.paths.end(),[&](const auto& candidate){return nativeSameLuid(candidate.targetInfo.adapterId,path.targetInfo.adapterId)&&candidate.targetInfo.id==path.targetInfo.id&&candidate.targetInfo.targetAvailable;});
    if(available)throw std::runtime_error("display_v2_foreign_source_unavailable");
    return false;
  }
  const auto& available=fresh.paths[*match];
  displayV2CopyForeignPath(plan,copied,from,path,available);
  taken.insert(nativeSourceKey(available));return true;
}
// Slides every display already in `plan` as one group so it starts at x =
// `reach` (top edge at y = `top`), right of the owned output(s). Horizontal
// gaps close (the desktop stays contiguous); vertical arrangement and
// left-to-right order are kept.
inline void displayV2SlideRight(NativeCcdData& plan,int64_t reach,LONG top=0){
  if(plan.paths.empty())return;
  if(reach<INT32_MIN||reach>INT32_MAX)throw std::runtime_error("display_v2_layout_bounds");
  LONG anchorX=LONG_MAX,anchorY=LONG_MAX;
  for(const auto& path:plan.paths){const auto& s=nativeCcdSource(plan,path);if(s.position.x<anchorX||(s.position.x==anchorX&&s.position.y<anchorY)){anchorX=s.position.x;anchorY=s.position.y;}}
  const int64_t dx=reach-anchorX,dy=int64_t(top)-anchorY;std::vector<UINT32> sources;
  for(const auto& path:plan.paths){const auto index=nativeSourceIndex(path);if(std::find(sources.begin(),sources.end(),index)!=sources.end())continue;sources.push_back(index);auto& s=plan.modes[index].sourceMode;
    const auto x=int64_t(s.position.x)+dx,y=int64_t(s.position.y)+dy;if(!displayRectangleFits(x,y,s.width,s.height))throw std::runtime_error("display_v2_layout_bounds");
    s.position={LONG(x),LONG(y)};
  }
  std::stable_sort(sources.begin(),sources.end(),[&](UINT32 a,UINT32 b){return plan.modes[a].sourceMode.position.x<plan.modes[b].sourceMode.position.x;});
  for(const auto index:sources){auto& s=plan.modes[index].sourceMode;
    if(s.position.x>reach)s.position.x=LONG(reach);
    reach=std::max<int64_t>(reach,int64_t(s.position.x)+s.width);
    if(!displayRectangleFits(s.position.x,s.position.y,s.width,s.height))throw std::runtime_error("display_v2_layout_bounds");
  }
}

// --------------------------------------------------------- streaming geometry
// Owned output at (0,0) with the requested size (= primary); the identified
// console omitted (= deactivated); every display that was active in
// `baseline` (the configuration before this change) kept with its baseline
// modes, its group slid right of the owned output. Windows reacts to the SDK
// recreating the owned target on its own (in the field it re-activated an
// inactive console and may pick other modes); supplying the baseline puts
// those displays back as they were instead of failing the change. Displays
// that were inactive in the baseline stay inactive because they are not
// supplied.
inline NativeCcdData planDisplayV2Streaming(const NativeCcdData& fresh,const NativeCcdData& baseline,const NativeDisplayTarget& stream,std::span<const NativeDisplayTarget> owned,const std::optional<DisplayV2Console>& console,bool* consoleRemoved=nullptr,const DisplayV2Retired& hidden={}){
  if(consoleRemoved)*consoleRemoved=console&&displayV2ConsoleActive(fresh,*console);
  NativeCcdData plan;std::map<UINT32,UINT32> copied;std::set<DisplayV2SourceKey> taken;
  std::vector<DISPLAYCONFIG_PATH_INFO> kept;
  for(const auto& path:baseline.paths){
    if(!displayV2Active(path)||nativeSameTarget(path,stream))continue;
    // Every generation of the streamed output is ours, never a kept display.
    if(std::any_of(owned.begin(),owned.end(),[&](const auto& target){return target.id==stream.id&&nativeSameTarget(path,target);}))continue;
    if(console&&displayV2ConsoleTarget(path,*console)&&!nativeOwnedPath(path,owned))continue;
    if(displayV2RetiredPath(hidden,path))continue;
    kept.push_back(path);
  }
  const auto reserved=displayV2ExactSources(fresh,kept);
  for(const auto& path:kept)(void)displayV2KeepForeign(plan,copied,taken,fresh,baseline,path,reserved);
  const auto chosen=displayV2OwnedPath(fresh,stream,taken);if(!chosen)throw std::runtime_error("display_v2_owned_target_unavailable");
  displayV2SlideRight(plan,stream.width);
  displayV2AddOwned(plan,fresh.paths[*chosen],stream.width,stream.height,0,0,true);
  return plan;
}
inline bool displayV2StreamingSatisfied(const NativeCcdData& ccd,const NativeDisplayTarget& stream,const std::optional<DisplayV2Console>& console,std::string* why=nullptr){
  const auto fail=[&](const char* reason){if(why)*why=reason;return false;};
  try{
    size_t matches=0;RECT owned{};
    for(const auto& path:ccd.paths){if(!displayV2Active(path))continue;
      if(console&&displayV2ConsoleTarget(path,*console))return fail("display_v2_console_active");
      if(!nativeSameTarget(path,stream))continue;
      const auto& source=nativeCcdSource(ccd,path);
      if(source.width!=stream.width||source.height!=stream.height)return fail("display_v2_owned_size_mismatch");
      if(source.position.x||source.position.y)return fail("display_v2_owned_not_primary");
      owned=displayV2Rect(source);++matches;
    }
    if(matches!=1)return fail("display_v2_owned_inactive");
    for(const auto& path:ccd.paths)if(displayV2Active(path)&&!nativeSameTarget(path,stream)&&displayV2Overlap(owned,displayV2Rect(nativeCcdSource(ccd,path))))return fail("display_v2_overlap");
    if(why)why->clear();return true;
  }catch(...){return fail("display_v2_mode_invalid");}
}
// What other displays must keep: adapter/target identity, active state, mode
// size, refresh, pixel format and rotation. Position is deliberately excluded:
// Windows re-lays out other outputs (e.g. the QEMU console) whenever the owned
// target is recreated, and v2 never models its placement algorithm.
struct DisplayV2Mode {LONG adapterHigh=0;DWORD adapterLow=0;UINT32 target=0,width=0,height=0,refreshNumerator=0,refreshDenominator=0;int rotation=0,format=0;};
inline bool displayV2SameMode(const DisplayV2Mode& a,const DisplayV2Mode& b){
  const bool refresh=(a.refreshDenominator&&b.refreshDenominator)?uint64_t(a.refreshNumerator)*b.refreshDenominator==uint64_t(b.refreshNumerator)*a.refreshDenominator:(a.refreshNumerator==b.refreshNumerator&&a.refreshDenominator==b.refreshDenominator);
  return a.adapterHigh==b.adapterHigh&&a.adapterLow==b.adapterLow&&a.target==b.target&&a.width==b.width&&a.height==b.height&&a.rotation==b.rotation&&a.format==b.format&&refresh;
}
inline std::vector<DisplayV2Mode> displayV2Foreign(const NativeCcdData& ccd,std::span<const NativeDisplayTarget> owned,const std::optional<DisplayV2Console>& console,const DisplayV2Retired& hidden={}){
  std::vector<DisplayV2Mode> out;
  for(const auto& path:ccd.paths){
    if(!displayV2Active(path)||nativeOwnedPath(path,owned)||(console&&displayV2ConsoleTarget(path,*console))||displayV2RetiredPath(hidden,path))continue;
    const auto& source=nativeCcdSource(ccd,path);
    out.push_back({path.targetInfo.adapterId.HighPart,path.targetInfo.adapterId.LowPart,path.targetInfo.id,source.width,source.height,path.targetInfo.refreshRate.Numerator,path.targetInfo.refreshRate.Denominator,int(path.targetInfo.rotation),int(source.pixelFormat)});
  }
  std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return std::tuple{a.adapterHigh,a.adapterLow,a.target}<std::tuple{b.adapterHigh,b.adapterLow,b.target};});
  return out;
}
inline bool displayV2SameForeign(const std::vector<DisplayV2Mode>& before,const std::vector<DisplayV2Mode>& after){
  if(before.size()!=after.size())return false;
  for(size_t i=0;i<before.size();++i)if(!displayV2SameMode(before[i],after[i]))return false;
  return true;
}
// Fixed diagnostic code for the first difference (0 = same).
namespace displayV2ForeignCode {
inline constexpr uint32_t count=1,identity=2,size=3,refresh=4,rotation=5,format=6;
}
inline uint32_t displayV2ForeignDifference(const std::vector<DisplayV2Mode>& before,const std::vector<DisplayV2Mode>& after){
  if(before.size()!=after.size())return displayV2ForeignCode::count;
  for(size_t i=0;i<before.size();++i){const auto& a=before[i];const auto& b=after[i];
    if(a.adapterHigh!=b.adapterHigh||a.adapterLow!=b.adapterLow||a.target!=b.target)return displayV2ForeignCode::identity;
    if(a.width!=b.width||a.height!=b.height)return displayV2ForeignCode::size;
    if(a.rotation!=b.rotation)return displayV2ForeignCode::rotation;
    if(a.format!=b.format)return displayV2ForeignCode::format;
    if(!displayV2SameMode(a,b))return displayV2ForeignCode::refresh;
  }
  return 0;
}

// ------------------------------------------------------------ count geometry
// Owned output count 1 <-> 2 (not streaming-time; a persistent user choice).
// Owned "0" primary at (0,0) and owned "1" (when present) to its right, each at
// its SDK size. Every display that was active in `baseline` (the configuration
// before the change, an active console included) is re-supplied with its
// baseline modes and slid right of the owned outputs. A display that was
// inactive before (e.g. the QEMU console Windows re-activates when the SDK adds
// or removes a target) is not supplied, so it is inactive again: nothing the
// user had on is ever turned off. `targets` are the owned outputs after the
// change, `ours` every owned generation (old and new target IDs).
inline std::vector<NativeDisplayTarget> displayV2OrderedTargets(std::span<const NativeDisplayTarget> targets){
  std::vector<NativeDisplayTarget> ordered(targets.begin(),targets.end());
  std::stable_sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.id<b.id;});
  return ordered;
}
inline NativeCcdData planDisplayV2Count(const NativeCcdData& fresh,const NativeCcdData& baseline,std::span<const NativeDisplayTarget> targets,std::span<const NativeDisplayTarget> ours){
  const auto ordered=displayV2OrderedTargets(targets);
  if(ordered.empty()||ordered.size()>2||ordered.front().id!="0"||(ordered.size()==2&&ordered[1].id!="1"))throw std::runtime_error("display_v2_owned_target_unavailable");
  NativeCcdData plan;std::map<UINT32,UINT32> copied;std::set<DisplayV2SourceKey> taken;
  std::vector<DISPLAYCONFIG_PATH_INFO> kept;
  for(const auto& path:baseline.paths){
    if(!displayV2Active(path)||nativeOwnedPath(path,ours)||nativeOwnedPath(path,targets))continue;
    kept.push_back(path);
  }
  const auto reserved=displayV2ExactSources(fresh,kept);
  for(const auto& path:kept)(void)displayV2KeepForeign(plan,copied,taken,fresh,baseline,path,reserved);
  std::vector<size_t> chosen;int64_t reach=0;
  for(const auto& target:ordered){
    const auto found=displayV2OwnedPath(fresh,target,taken);if(!found)throw std::runtime_error("display_v2_owned_target_unavailable");
    chosen.push_back(*found);taken.insert(nativeSourceKey(fresh.paths[*found]));reach+=target.width;
  }
  displayV2SlideRight(plan,reach);
  // Owned paths first ("0", then "1"), left to right from the origin.
  LONG left=LONG(reach);
  for(size_t i=ordered.size();i-->0;){left-=LONG(ordered[i].width);displayV2AddOwned(plan,fresh.paths[chosen[i]],ordered[i].width,ordered[i].height,left,0,true);}
  return plan;
}
// The owned side of a settled count change: each owned output active exactly
// once at its SDK size and planned place ("0" at the origin, "1" right of it:
// the layout the dual-stream path builds on), no departed owned generation
// still active, no owned output overlapping another display. Other displays
// may sit anywhere Windows put them; their state/modes are compared
// separately (displayV2Foreign without a console exclusion).
inline bool displayV2CountSatisfied(const NativeCcdData& ccd,std::span<const NativeDisplayTarget> targets,std::span<const NativeDisplayTarget> ours,std::string* why=nullptr){
  const auto fail=[&](const char* reason){if(why)*why=reason;return false;};
  try{
    std::vector<std::pair<RECT,DisplayV2SourceKey>> owned;LONG left=0;
    for(const auto& target:displayV2OrderedTargets(targets)){
      size_t matches=0;
      for(const auto& path:ccd.paths){
        if(!displayV2Active(path)||!nativeSameTarget(path,target))continue;
        const auto& source=nativeCcdSource(ccd,path);
        if(source.width!=target.width||source.height!=target.height)return fail("display_v2_owned_size_mismatch");
        if(source.position.x!=left||source.position.y)return fail(target.id=="0"?"display_v2_owned_not_primary":"display_v2_owned_not_placed");
        owned.push_back({displayV2Rect(source),nativeSourceKey(path)});++matches;
      }
      if(matches!=1)return fail("display_v2_owned_inactive");
      left+=LONG(target.width);
    }
    for(const auto& path:ccd.paths)if(displayV2Active(path)&&nativeOwnedPath(path,ours)&&!nativeOwnedPath(path,targets))return fail("display_v2_owned_departure_pending");
    for(const auto& [rect,key]:owned)
      for(const auto& path:ccd.paths)if(displayV2Active(path)&&nativeSourceKey(path)!=key&&displayV2Overlap(rect,displayV2Rect(nativeCcdSource(ccd,path))))return fail("display_v2_overlap");
    if(why)why->clear();return true;
  }catch(...){return fail("display_v2_mode_invalid");}
}

// ------------------------------------------------------------ group geometry
// Independent outputs (tracks-v1, "모든 모니터"): every owned output is one
// stream track at the client's size and relative place. "0" is primary at the
// origin; "1" keeps its requested offset from "0" (left, right, above or
// below; never overlapping). The owned output count is the client's separate
// prior step (DisplaySessionV2::count); a group names exactly the owned outputs.
struct DisplayV2Output {std::string id;DWORD width=0,height=0;LONG left=0,top=0;};
inline RECT displayV2Rect(const DisplayV2Output& output){if(!displayRectangleFits(output.left,output.top,output.width,output.height))throw std::runtime_error("display_v2_layout_bounds");return {output.left,output.top,LONG(int64_t(output.left)+output.width),LONG(int64_t(output.top)+output.height)};}
// Canonical group: ordered by id ("0" first), translated so "0" sits at (0,0).
inline std::vector<DisplayV2Output> displayV2GroupLayout(std::span<const DisplayV2Output> outputs){
  const auto fail=[]{throw std::runtime_error("display_v2_group_invalid");};
  if(outputs.empty()||outputs.size()>2)fail();
  std::vector<DisplayV2Output> group(outputs.begin(),outputs.end());
  std::stable_sort(group.begin(),group.end(),[](const auto& a,const auto& b){return a.id<b.id;});
  if(group.front().id!="0"||(group.size()==2&&group[1].id!="1"))fail();
  const int64_t dx=-int64_t(group.front().left),dy=-int64_t(group.front().top);
  for(auto& output:group){
    if(!ownedOutputSize(output.width,output.height))fail();
    const int64_t x=int64_t(output.left)+dx,y=int64_t(output.top)+dy;
    if(!displayRectangleFits(x,y,output.width,output.height))fail();
    output.left=LONG(x);output.top=LONG(y);
  }
  // Independent outputs have independent buffers/encoders. Their pixel sum
  // is not a per-picture codec limit; each allocation is checked separately.
  if(group.size()==2&&displayV2Overlap(displayV2Rect(group[0]),displayV2Rect(group[1])))fail();
  return group;
}
inline bool displayV2GroupNamesOwned(std::span<const NativeDisplayTarget> owned,std::span<const DisplayV2Output> group){
  if(owned.size()!=group.size())return false;
  for(const auto& output:group)if(std::count_if(owned.begin(),owned.end(),[&](const auto& t){return t.id==output.id;})!=1)return false;
  return true;
}
// The group (canonical layout) at the current SDK `targets` (matched by id);
// the identified console omitted (= deactivated, re-identified when Windows
// re-activates it); every other display active in `baseline` re-supplied with
// its baseline modes and slid right of the group, top-aligned with the group's
// right-most output. Displays inactive in `baseline` stay inactive. `ours` are
// every owned generation (old and new target IDs).
inline NativeCcdData planDisplayV2Group(const NativeCcdData& fresh,const NativeCcdData& baseline,std::span<const DisplayV2Output> group,std::span<const NativeDisplayTarget> targets,std::span<const NativeDisplayTarget> ours,const std::optional<DisplayV2Console>& console,bool* consoleRemoved=nullptr,const DisplayV2Retired& hidden={}){
  if(consoleRemoved)*consoleRemoved=console&&displayV2ConsoleActive(fresh,*console);
  NativeCcdData plan;std::map<UINT32,UINT32> copied;std::set<DisplayV2SourceKey> taken;
  std::vector<DISPLAYCONFIG_PATH_INFO> kept;
  for(const auto& path:baseline.paths){
    if(!displayV2Active(path)||nativeOwnedPath(path,ours)||nativeOwnedPath(path,targets))continue;
    if((console&&displayV2ConsoleTarget(path,*console))||displayV2RetiredPath(hidden,path))continue;
    kept.push_back(path);
  }
  const auto reserved=displayV2ExactSources(fresh,kept);
  for(const auto& path:kept)(void)displayV2KeepForeign(plan,copied,taken,fresh,baseline,path,reserved);
  std::vector<size_t> chosen;int64_t reach=INT64_MIN;LONG top=0;
  for(const auto& output:group){
    const auto target=std::find_if(targets.begin(),targets.end(),[&](const auto& t){return t.id==output.id;});
    if(target==targets.end()||target->width!=output.width||target->height!=output.height)throw std::runtime_error("display_v2_owned_target_unavailable");
    const auto found=displayV2OwnedPath(fresh,*target,taken);if(!found)throw std::runtime_error("display_v2_owned_target_unavailable");
    chosen.push_back(*found);taken.insert(nativeSourceKey(fresh.paths[*found]));
    const int64_t right=int64_t(output.left)+output.width;if(right>reach){reach=right;top=output.top;}
  }
  displayV2SlideRight(plan,reach,top);
  // Owned paths first ("0", then "1").
  for(size_t i=group.size();i-->0;)displayV2AddOwned(plan,fresh.paths[chosen[i]],group[i].width,group[i].height,group[i].left,group[i].top,true);
  return plan;
}
// The owned side of a settled group: each output active exactly once at its
// size and planned place ("0" at the origin = primary), the identified console
// inactive, no departed owned generation still active, no owned output
// overlapping another display. Other displays may sit anywhere Windows put
// them; their state/modes are compared separately (displayV2Foreign).
inline bool displayV2GroupSatisfied(const NativeCcdData& ccd,std::span<const DisplayV2Output> group,std::span<const NativeDisplayTarget> targets,std::span<const NativeDisplayTarget> ours,const std::optional<DisplayV2Console>& console,std::string* why=nullptr){
  const auto fail=[&](const char* reason){if(why)*why=reason;return false;};
  try{
    std::vector<std::pair<RECT,DisplayV2SourceKey>> owned;
    for(const auto& output:group){
      const auto target=std::find_if(targets.begin(),targets.end(),[&](const auto& t){return t.id==output.id;});
      if(target==targets.end())return fail("display_v2_owned_missing");
      size_t matches=0;
      for(const auto& path:ccd.paths){
        if(!displayV2Active(path)||!nativeSameTarget(path,*target))continue;
        const auto& source=nativeCcdSource(ccd,path);
        if(source.width!=output.width||source.height!=output.height)return fail("display_v2_owned_size_mismatch");
        if(source.position.x!=output.left||source.position.y!=output.top)return fail(output.id=="0"?"display_v2_owned_not_primary":"display_v2_owned_not_placed");
        owned.push_back({displayV2Rect(source),nativeSourceKey(path)});++matches;
      }
      if(matches!=1)return fail("display_v2_owned_inactive");
    }
    for(const auto& path:ccd.paths){
      if(!displayV2Active(path)||nativeOwnedPath(path,targets))continue;
      if(console&&displayV2ConsoleTarget(path,*console))return fail("display_v2_console_active");
      if(nativeOwnedPath(path,ours))return fail("display_v2_owned_departure_pending");
    }
    for(const auto& [rect,key]:owned)
      for(const auto& path:ccd.paths)if(displayV2Active(path)&&nativeSourceKey(path)!=key&&displayV2Overlap(rect,displayV2Rect(nativeCcdSource(ccd,path))))return fail("display_v2_overlap");
    if(why)why->clear();return true;
  }catch(...){return fail("display_v2_mode_invalid");}
}

// ---------------------------------------------------------- restore geometry
// Counts cannot identify an adapter after a restart. An unresolved saved
// adapter or console, or an unrecorded active foreign target, must not turn
// into an owned-only supplied topology that switches off the current display.
// Source IDs deliberately do not participate: Windows may reassign them.
// `named`: non-owned adapters the state names; `stale`: those absent now.
struct DisplayV2RestorePreflight {uint32_t stale=0,named=0;bool resolved=false;};
inline DisplayV2RestorePreflight preflightDisplayV2Restore(const DisplayV2State& saved,const NativeCcdData& fresh,std::span<const NativeDisplayTarget> current,const DisplayV2Retired& retired={}){
  using Adapter=std::pair<DWORD,LONG>;using Target=std::tuple<DWORD,LONG,UINT32>;
  const auto adapter=[](LUID value){return Adapter{value.LowPart,value.HighPart};};
  const auto target=[](LUID value,UINT32 id){return Target{value.LowPart,value.HighPart,id};};
  std::set<Adapter> named,present;std::set<Target> known,available,owned;
  for(const auto& item:current)owned.insert(target(item.adapter,item.target));
  for(const auto& path:saved.ccd.paths)if(displayV2Active(path)&&!nativeOwnedPath(path,saved.owned)&&!displayV2RetiredPath(retired,path)){
    named.insert(adapter(path.targetInfo.adapterId));known.insert(target(path.targetInfo.adapterId,path.targetInfo.id));
  }
  if(saved.console){named.insert(adapter(saved.console->targetAdapter));known.insert(target(saved.console->targetAdapter,saved.console->target));}
  for(const auto& path:fresh.paths){
    present.insert(adapter(path.targetInfo.adapterId));
    if(path.targetInfo.targetAvailable)available.insert(target(path.targetInfo.adapterId,path.targetInfo.id));
  }
  DisplayV2RestorePreflight result;result.named=uint32_t(std::min<size_t>(64,named.size()));
  for(const auto& item:named)if(!present.count(item))result.stale=std::min<uint32_t>(64,result.stale+1);
  if(result.stale)return result;
  for(const auto& item:known)if(owned.count(item))return result;
  if(saved.console&&!available.count(target(saved.console->targetAdapter,saved.console->target)))return result;
  // A removed/unavailable ordinary target on an existing adapter can still be
  // skipped, as before. It is not evidence identifying a new active target.
  for(const auto& path:fresh.paths)if(displayV2Active(path)&&!nativeOwnedPath(path,current)&&!nativeOwnedPath(path,saved.owned)&&!displayV2RetiredPath(retired,path)&&
      !known.count(target(path.targetInfo.adapterId,path.targetInfo.id)))return result;
  result.resolved=true;return result;
}
// Every adapter the state names is gone: it was saved before a Windows restart
// (adapter LUIDs last one boot) and can never be restored in this one.
inline bool displayV2StateFromEarlierBoot(const DisplayV2State& saved,const NativeCcdData& fresh,std::span<const NativeDisplayTarget> current,const DisplayV2Retired& retired={}){
  const auto identity=preflightDisplayV2Restore(saved,fresh,current,retired);return identity.named&&identity.stale==identity.named;
}
struct DisplayV2RestoreIdentityError:std::runtime_error {
  uint32_t stale;
  explicit DisplayV2RestoreIdentityError(uint32_t count):std::runtime_error("display_v2_restore_identity_unverified"),stale(count){}
};
struct DisplayV2Expected {LUID adapter{};UINT32 target=0;bool owned=false;DWORD width=0,height=0;LONG left=0,top=0;};
// Active targets alone do not prove the saved owned source size/placement was restored.
// SDK resize is best effort; its failure must not turn the current stream size
// into a successful original-configuration restore.
inline bool displayV2RestoreTargetsMatch(const NativeCcdData& actual,std::span<const DisplayV2Expected> expected){
  for(const auto& item:expected){
    const auto path=std::find_if(actual.paths.begin(),actual.paths.end(),[&](const auto& value){return displayV2Active(value)&&nativeSameLuid(value.targetInfo.adapterId,item.adapter)&&value.targetInfo.id==item.target;});
    if(path==actual.paths.end())return false;
    if(item.owned){const auto& source=nativeCcdSource(actual,*path);if(!item.width||!item.height||source.width!=item.width||source.height!=item.height||source.position.x!=item.left||source.position.y!=item.top)return false;}
  }
  return true;
}
// Saved active paths re-applied with their saved modes; owned outputs mapped by
// public id to the CURRENT SDK targets (IDs change when the SDK recreates a
// target) at their saved position. Targets that no longer exist are skipped.
inline NativeCcdData planDisplayV2Restore(const DisplayV2State& saved,const NativeCcdData& fresh,std::span<const NativeDisplayTarget> current,std::vector<DisplayV2Expected>* expected=nullptr,const DisplayV2Retired& retired={}){
  const auto identity=preflightDisplayV2Restore(saved,fresh,current,retired);if(!identity.resolved)throw DisplayV2RestoreIdentityError(identity.stale);
  NativeCcdData plan;std::map<UINT32,UINT32> copied;std::set<DisplayV2SourceKey> taken;
  struct Owned {NativeDisplayTarget target;LONG left=0,top=0;DWORD width=0,height=0;};std::vector<Owned> owned;
  std::vector<DISPLAYCONFIG_PATH_INFO> kept;
  if(expected)expected->clear();
  for(const auto& path:saved.ccd.paths){
    if(!displayV2Active(path)||displayV2RetiredPath(retired,path))continue;
    const auto previous=std::find_if(saved.owned.begin(),saved.owned.end(),[&](const auto& target){return nativeSameTarget(path,target);});
    if(previous!=saved.owned.end()){
      const auto now=std::find_if(current.begin(),current.end(),[&](const auto& target){return target.id==previous->id;});
      if(now==current.end())continue;
      const auto& source=nativeCcdSource(saved.ccd,path);owned.push_back({*now,source.position.x,source.position.y,source.width,source.height});continue;
    }
    if(std::any_of(current.begin(),current.end(),[&](const auto& target){return nativeSameTarget(path,target);}))continue;
    kept.push_back(path);
  }
  const auto reserved=displayV2ExactSources(fresh,kept);
  for(const auto& path:kept)if(displayV2KeepForeign(plan,copied,taken,fresh,saved.ccd,path,reserved)){
    if(expected)expected->push_back({path.targetInfo.adapterId,path.targetInfo.id});
  }
  for(const auto& item:owned){
    const auto chosen=displayV2OwnedPath(fresh,item.target,taken);if(!chosen)continue;
    displayV2AddOwned(plan,fresh.paths[*chosen],item.width,item.height,item.left,item.top,false);taken.insert(nativeSourceKey(fresh.paths[*chosen]));
    if(expected)expected->push_back({item.target.adapter,item.target.target,true,item.width,item.height,item.left,item.top});
  }
  // An owned output that did not exist at snapshot time (count changed) is
  // never silently dropped: it stays active, appended to the right.
  LONG right=0;for(const auto& path:plan.paths)right=std::max(right,displayV2Rect(nativeCcdSource(plan,path)).right);
  for(const auto& target:current){
    if(std::any_of(saved.owned.begin(),saved.owned.end(),[&](const auto& old){return old.id==target.id;}))continue;
    const auto chosen=displayV2OwnedPath(fresh,target,taken);if(!chosen)continue;
    const LONG left=right;
    displayV2AddOwned(plan,fresh.paths[*chosen],target.width,target.height,left,0,false);taken.insert(nativeSourceKey(fresh.paths[*chosen]));right+=LONG(target.width);
    if(expected)expected->push_back({target.adapter,target.target,true,target.width,target.height,left,0});
  }
  if(plan.paths.empty())throw std::runtime_error("display_v2_restore_unavailable");
  return plan;
}
// Fallback: keep whatever is active now and add the identified console to the
// right of it, using its saved modes when known.
inline std::optional<NativeCcdData> planDisplayV2ConsoleReturn(const NativeCcdData& fresh,const DisplayV2Console& console,const NativeCcdData* saved){
  NativeCcdData plan;std::map<UINT32,UINT32> copied;std::set<DisplayV2SourceKey> taken;LONG right=0,top=0;bool any=false;
  for(const auto& path:fresh.paths){if(!displayV2Active(path))continue;
    displayV2CopyPath(plan,copied,fresh,path);taken.insert(nativeSourceKey(path));
    const auto rect=displayV2Rect(nativeCcdSource(fresh,path));if(!any||rect.right>right){right=rect.right;top=rect.top;}any=true;
  }
  DISPLAYCONFIG_PATH_INFO wanted{};wanted.sourceInfo.adapterId=console.sourceAdapter;wanted.sourceInfo.id=console.source;wanted.targetInfo.adapterId=console.targetAdapter;wanted.targetInfo.id=console.target;
  const auto chosen=displayV2ForeignPath(fresh,wanted,taken);if(!chosen)return {};
  const DISPLAYCONFIG_PATH_INFO* original=nullptr;
  if(saved)for(const auto& path:saved->paths)if(displayV2Active(path)&&displayV2ConsoleTarget(path,console)){original=&path;break;}
  if(!original)return {};
  auto path=*original;const auto& available=fresh.paths[*chosen];
  path.sourceInfo.adapterId=available.sourceInfo.adapterId;path.sourceInfo.id=available.sourceInfo.id;
  std::map<UINT32,UINT32> savedCopies;const auto first=plan.modes.size();
  displayV2CopyPath(plan,savedCopies,*saved,path);
  for(size_t i=first;i<plan.modes.size();++i)if(plan.modes[i].infoType==DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE){plan.modes[i].adapterId=path.sourceInfo.adapterId;plan.modes[i].id=path.sourceInfo.id;plan.modes[i].sourceMode.position={any?right:0,any?top:0};}
  return plan;
}

// ------------------------------------------------------------- state codec
inline std::string displayV2Hex(const void* data,size_t size){
  static const char digits[]="0123456789abcdef";std::string out;out.reserve(size*2);const auto* bytes=static_cast<const uint8_t*>(data);
  for(size_t i=0;i<size;++i){out+=digits[bytes[i]>>4];out+=digits[bytes[i]&15];}return out;
}
inline std::vector<uint8_t> displayV2Unhex(const std::string& text){
  if(text.size()%2)throw std::runtime_error("display_v2_state_invalid");
  const auto nibble=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("display_v2_state_invalid");};
  std::vector<uint8_t> out(text.size()/2);for(size_t i=0;i<out.size();++i)out[i]=uint8_t((nibble(text[2*i])<<4)|nibble(text[2*i+1]));return out;
}
inline void displayV2ValidateTargets(std::span<const NativeDisplayTarget> owned){
  if(owned.empty()||owned.size()>2)throw std::runtime_error("display_v2_state_invalid");
  for(size_t i=0;i<owned.size();++i){const auto& t=owned[i];
    if((t.id!="0"&&t.id!="1")||!ownedOutputSize(t.width,t.height))throw std::runtime_error("display_v2_state_invalid");
    for(size_t j=0;j<i;++j)if(owned[j].id==t.id)throw std::runtime_error("display_v2_state_invalid");
  }
}
inline void displayV2ValidateCcd(const NativeCcdData& ccd){
  if(ccd.paths.empty()||ccd.paths.size()>64||ccd.modes.size()>256)throw std::runtime_error("display_v2_state_invalid");
  for(const auto& path:ccd.paths){
    if(!displayV2Active(path))throw std::runtime_error("display_v2_state_invalid");
    (void)nativeCcdSource(ccd,path);
    const UINT32 invalidTarget=displayV2VirtualMode(path)?DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID:DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    if(nativeTargetIndex(path)!=invalidTarget)(void)nativeCcdTarget(ccd,path);
    if(displayV2VirtualMode(path)&&path.targetInfo.desktopModeInfoIdx!=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID)(void)nativeCcdMode(ccd,path.targetInfo.desktopModeInfoIdx,DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE);
  }
}
inline std::string displayV2StateBytes(const DisplayV2State& state){
  displayV2ValidateCcd(state.ccd);displayV2ValidateTargets(state.owned);
  if(state.session.size()>128||state.session.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:-")!=std::string::npos)throw std::runtime_error("display_v2_state_invalid");
  std::string owned="[";
  for(const auto& t:state.owned){if(owned.size()>1)owned+=',';owned+="{\"id\":"+jsonQuote(t.id)+",\"adapterLow\":"+std::to_string(t.adapter.LowPart)+",\"adapterHigh\":"+std::to_string(t.adapter.HighPart)+",\"target\":"+std::to_string(t.target)+",\"width\":"+std::to_string(t.width)+",\"height\":"+std::to_string(t.height)+'}';}
  owned+=']';
  std::string console="null";
  if(state.console){const auto& c=*state.console;console="{\"sourceAdapterLow\":"+std::to_string(c.sourceAdapter.LowPart)+",\"sourceAdapterHigh\":"+std::to_string(c.sourceAdapter.HighPart)+",\"source\":"+std::to_string(c.source)+",\"targetAdapterLow\":"+std::to_string(c.targetAdapter.LowPart)+",\"targetAdapterHigh\":"+std::to_string(c.targetAdapter.HighPart)+",\"target\":"+std::to_string(c.target)+'}';}
  return "{\"schema\":\"multitap-display-v2\",\"version\":1,\"session\":"+jsonQuote(state.session)+",\"owned\":"+owned+",\"console\":"+console+
    ",\"paths\":"+jsonQuote(displayV2Hex(state.ccd.paths.data(),state.ccd.paths.size()*sizeof(DISPLAYCONFIG_PATH_INFO)))+
    ",\"modes\":"+jsonQuote(displayV2Hex(state.ccd.modes.data(),state.ccd.modes.size()*sizeof(DISPLAYCONFIG_MODE_INFO)))+'}';
}
inline DisplayV2State parseDisplayV2State(const std::string& bytes){
  const auto fail=[]{throw std::runtime_error("display_v2_state_invalid");};
  if(bytes.empty()||bytes.size()>65536)fail();
  const auto root=JsonParser::parse(bytes);
  if(root.get("schema").string()!="multitap-display-v2"||root.get("version").number()!=1)fail();
  DisplayV2State state;state.session=root.get("session").string();
  const auto integer=[&](const Json& value,double low,double high){const auto n=value.number(low-1);if(!nativeDisplayInteger(n,low,high))fail();return n;};
  const auto* owned=std::get_if<Json::Array>(&root.get("owned").value);if(!owned)fail();
  for(const auto& item:*owned)state.owned.push_back({item.get("id").string(),{DWORD(integer(item.get("adapterLow"),0,4294967295.0)),LONG(integer(item.get("adapterHigh"),-2147483648.0,2147483647.0))},UINT32(integer(item.get("target"),0,4294967295.0)),DWORD(integer(item.get("width"),320,ownedOutputMaxSide)),DWORD(integer(item.get("height"),180,ownedOutputMaxSide))});
  displayV2ValidateTargets(state.owned);
  const auto& console=root.get("console");
  if(std::holds_alternative<Json::Object>(console.value)){
    DisplayV2Console c;c.sourceAdapter={DWORD(integer(console.get("sourceAdapterLow"),0,4294967295.0)),LONG(integer(console.get("sourceAdapterHigh"),-2147483648.0,2147483647.0))};c.source=UINT32(integer(console.get("source"),0,4294967295.0));
    c.targetAdapter={DWORD(integer(console.get("targetAdapterLow"),0,4294967295.0)),LONG(integer(console.get("targetAdapterHigh"),-2147483648.0,2147483647.0))};c.target=UINT32(integer(console.get("target"),0,4294967295.0));state.console=c;
  }else if(!std::holds_alternative<std::nullptr_t>(console.value))fail();
  const auto paths=displayV2Unhex(root.get("paths").string()),modes=displayV2Unhex(root.get("modes").string());
  if(paths.empty()||paths.size()%sizeof(DISPLAYCONFIG_PATH_INFO)||modes.size()%sizeof(DISPLAYCONFIG_MODE_INFO))fail();
  state.ccd.paths.resize(paths.size()/sizeof(DISPLAYCONFIG_PATH_INFO));state.ccd.modes.resize(modes.size()/sizeof(DISPLAYCONFIG_MODE_INFO));
  std::memcpy(state.ccd.paths.data(),paths.data(),paths.size());if(!modes.empty())std::memcpy(state.ccd.modes.data(),modes.data(),modes.size());
  displayV2ValidateCcd(state.ccd);
  if(displayV2StateBytes(state)!=bytes)fail();
  return state;
}

// ------------------------------------------------------------- state store
struct DisplayV2Store {
  std::function<std::optional<std::string>()> load;
  std::function<bool(const std::string&)> save;
  std::function<bool()> remove;
  // Moves an unreadable/malformed state aside so it can never block a stream.
  std::function<void()> discard=[]{};
};
inline DisplayV2Store displayV2FileStore(const std::filesystem::path& path,std::function<bool(const std::filesystem::path&)> trusted=trustedMachineFile){
  DisplayV2Store store;
  store.load=[path,trusted]{return readNativeJournalFile(path,trusted,65536);};
  store.save=[path,trusted](const std::string& bytes){
    std::filesystem::path temporary;bool created=false;
    try{
      if(bytes.empty()||bytes.size()>65536||!trusted(path.parent_path()))return false;
      const auto attributes=GetFileAttributesW(path.c_str());if(attributes!=INVALID_FILE_ATTRIBUTES&&((attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||!trusted(path)))return false;
      temporary=path.wstring()+temporarySuffix();
      WinHandle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));created=file.value!=INVALID_HANDLE_VALUE;DWORD written=0;
      if(!created||!WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(file.value))throw std::runtime_error("display_v2_state_write_failed");
      file=WinHandle();
      if(!trusted(temporary)||!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("display_v2_state_write_failed");
      return true;
    }catch(...){if(created&&!temporary.empty())DeleteFileW(temporary.c_str());return false;}
  };
  store.remove=[path]{return DeleteFileW(path.c_str())||GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND;};
  store.discard=[path]{const auto aside=path.wstring()+L".bad";if(!MoveFileExW(path.c_str(),aside.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))DeleteFileW(path.c_str());};
  return store;
}

// --------------------------------------------------------------- session
struct DisplayV2Access {
  std::function<NativeCcdData()> query;                        // QDC_ALL_PATHS|QDC_VIRTUAL_MODE_AWARE
  std::function<LONG(const NativeCcdData&,UINT32)> configure;  // SetDisplayConfig
  std::function<std::vector<NativeDisplayTarget>()> owned;     // current SDK targets
  // Cleanup may finish an already held lease while SCM is STOP_PENDING. New
  // starts/count changes continue to use the normal RUNNING-only observation.
  std::function<std::vector<NativeDisplayTarget>()> restoreOwned;
  // SDK size change of one owned output; returns the new owned targets.
  std::function<std::optional<std::vector<NativeDisplayTarget>>(const std::string&,DWORD,DWORD)> resize;
  // Optional: several owned outputs' sizes in one SDK reconfiguration (fewer
  // target recreations for Windows to react to); else `resize` per output.
  std::function<std::optional<std::vector<NativeDisplayTarget>>(std::span<const DisplayV2Output>)> sizes;
  // SDK owned output count (1 or 2); returns the new owned targets. Count
  // changes only (DisplaySessionV2::count); streams never call it.
  std::function<std::optional<std::vector<NativeDisplayTarget>>(size_t)> count;
  // Optional final proof for a count change that Windows' GDI/capture side
  // lists exactly these owned outputs (checked before the change is kept).
  std::function<bool(std::span<const NativeDisplayTarget>)> usable;
  // Positive console identification only (nullopt = deactivate nothing).
  std::function<std::optional<DisplayV2Console>(const NativeCcdData&,std::span<const NativeDisplayTarget>)> identifyConsole;
  // Optional: adapters positively identified as retired Multitap legacy outputs.
  DisplayV2Retired retired;
  // Displays off for the stream, back with the saved state. The product sets this
  // to all of them: a stream has its virtual outputs and nothing else, one per
  // client monitor, so no window can land where no client sees it (a vGPU's own
  // head). Unset, other displays stay on, as in most of these tests.
  DisplayV2Retired hidden;
  // Optional: repeated attempts found a saved adapter gone; stop retrying that state.
  std::function<bool()> discardStale;
  DisplayV2Store store;
  std::function<bool()> eligible=[]{return true;};
  std::function<uint64_t()> now=[]{return GetTickCount64();};
  std::function<void()> pause=[]{Sleep(50);};
  // Optional: the wait between two reads while waiting for Windows to show a
  // change (waitUntil); unset, `pause`. Streams poll faster: every 50 ms poll
  // added 25 ms on average to each of a resize's serial waits.
  std::function<void()> poll;
  // Optional pure admission of the selected provider's actual mode ABI. No
  // topology/state/SDK mutation may precede a definite unsupported result.
  std::function<bool(DWORD,DWORD)> modeSupported;
};
enum class DisplayV2Outcome {nothing=0,restored=1,consoleOnly=2,failed=3};
struct DisplayV2RestoreResult {
  DisplayV2Outcome outcome=DisplayV2Outcome::nothing;uint32_t stage=0;DWORD error=0;
  // Diagnostics only (fixed numbers; field case: recovery failed at
  // verify every minute): targets the exact restore expected active, how many
  // of them (and of the owned ones) were still inactive when it gave up,
  // whether the saved state had the console active and whether it is active
  // afterwards, and the console fallback (0 not tried, 1 returned, 2 failed).
  uint32_t expected=0,missing=0,missingOwned=0,fallback=0;bool consoleExpected=false,consoleActive=false;
  // Saved non-owned adapters that no longer exist. Counts never authorize a
  // remap; this field stays false without a separate verified binding.
  uint32_t stale=0;bool remapped=false;
  bool ok()const{return outcome!=DisplayV2Outcome::failed;}
};
// Fixed diagnostic stages (no free text leaves the host).
namespace displayV2Stage {
inline constexpr uint32_t ineligible=1,query=2,state=3,sdk=4,target=5,apply=6,verify=7,foreign=8,internal=9;
}
struct DisplayV2Result {
  bool ok=false;uint32_t stage=0;DWORD error=0;std::string reason;bool consoleRemoved=false,consoleReturned=false;
  // Diagnostics only: console proof supplied / positively identified, active
  // non-owned displays before the change, displayV2ForeignCode at stage 8.
  bool consoleProof=false,consoleIdentified=false;uint32_t foreignBefore=0,foreignDifference=0;
  // Count change only: the SDK count RPC was sent (so a failure puts the
  // previous count back), Windows activated a display that was inactive before
  // the change (typically the QEMU console), and a state file remains afterwards
  // (an older unrestored original, or a failed delete/restore) for the
  // service's ordinary automatic recovery.
  bool countAttempted=false,displaysReturned=false,stateKept=false;
  bool modeUnsupported=false;
  // Diagnostics only: SDK size calls asked again after a refused or lost
  // reply, and confirmation rounds (capture binding) that needed a re-plan.
  uint32_t sdkRetries=0,confirmRetries=0;
  // Diagnostics only (resize_stages.hpp): time in the single-output SDK size
  // call and in Windows settling after it, summed over confirmation rounds.
  uint64_t sdkMs=0,settleMs=0;
  std::vector<NativeDisplayTarget> owned;std::optional<DisplayV2RestoreResult> restore;
};
// Adds the time from construction to destruction to `total` (access.now()).
class DisplayV2Timer {
 public:
  DisplayV2Timer(uint64_t& total,const std::function<uint64_t()>& now):total_(total),now_(now){try{from_=now_();}catch(...){from_=0;}}
  ~DisplayV2Timer(){try{const auto to=now_();if(from_&&to>=from_)total_+=to-from_;}catch(...){ }}
  DisplayV2Timer(const DisplayV2Timer&)=delete;DisplayV2Timer& operator=(const DisplayV2Timer&)=delete;
 private:
  uint64_t& total_;const std::function<uint64_t()>& now_;uint64_t from_=0;
};
// Re-plans from fresh CCD before a change counts as failed: Windows may still
// be re-laying displays from the previous change when the next one starts
// (rapid window/fullscreen changes), so three attempts were not always enough.
inline constexpr int displayV2SettleAttempts=5;
// Confirmation rounds (the caller's capture binding) per change.
inline constexpr uint32_t displayV2ConfirmRounds=3;
// A stream's reads while Windows shows a change (DisplayV2Access::poll and the
// capture proof): read-only CCD/GDI queries, so a short wait costs little.
inline constexpr DWORD displayV2StreamPollMs=15;
inline constexpr UINT32 displayV2Supplied=SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VIRTUAL_MODE_AWARE;

class DisplaySessionV2 {
 public:
  explicit DisplaySessionV2(DisplayV2Access access):access_(std::move(access)){}
  bool active()const{return active_;}
  const std::vector<NativeDisplayTarget>& owned()const{return owned_;}
  const std::optional<DisplayV2Console>& console()const{return console_;}
  const DisplayV2SnapshotEvidence& snapshotEvidence()const noexcept{return snapshotEvidence_;}
  // Called by the worker after restore() has returned, outside the display
  // transaction. Read failures stay unknown and cannot affect cleanup.
  DisplayV2GeometrySet currentGeometryEvidence()noexcept{
    try{return displayV2GeometryEvidence(access_.query(),owned_);}catch(...){return {};}
  }
  // Last presence observed through restore's existing load/remove operations.
  // No extra SDK enumeration or file read on the stop/ACK path.
  std::optional<bool> savedStatePresentEvidence()const noexcept{return restoreStatePresence_;}

  // `confirm` (optional) is the caller's last proof that the settled change
  // can be streamed (capture bound to exactly the owned outputs). It is part
  // of the change: a failed confirmation re-plans and confirms again
  // (settleConfirmed) before the change counts as failed.
  using Confirm=std::function<bool()>;
  // A successor worker borrows an already prepared single owned output and
  // the EXACT original state sealed by its service. This path never captures
  // or edits a new baseline and never applies a display configuration. The
  // mandatory confirmation must use the authenticated service lease and the
  // successor's real capture binding, not a peer-supplied readiness flag.
  DisplayV2Result attachPrepared(const std::string& logicalSession,const std::string& originalSha256,const DisplayV2Output& expected,const Confirm& confirm){
    DisplayV2Result result;
    try{
      if(active_||logicalSession.empty()||logicalSession.size()>80||!eligible()||!confirm)return fail(result,displayV2Stage::ineligible,"display_v2_handoff_ineligible");
      if(originalSha256.size()!=64||!std::all_of(originalSha256.begin(),originalSha256.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})||
         expected.id!="0"||expected.left||expected.top||!ownedOutputSize(expected.width,expected.height))return fail(result,displayV2Stage::state,"display_v2_handoff_binding_invalid");
      const auto original=access_.store.load();
      if(!original||nativeBaselineDigest(*original)!=originalSha256)return fail(result,displayV2Stage::state,"display_v2_handoff_original_changed");
      const auto saved=parseDisplayV2State(*original);
      const auto current=access_.owned();
      if(current.size()!=1||current.front().id!="0")return fail(result,displayV2Stage::verify,"display_v2_handoff_output_changed");
      const auto sameTargets=[](const auto& a,const auto& b){
        if(a.size()!=b.size())return false;
        return std::all_of(a.begin(),a.end(),[&](const auto& one){return std::count_if(b.begin(),b.end(),[&](const auto& two){return one.id==two.id&&nativeSameLuid(one.adapter,two.adapter)&&one.target==two.target&&one.width==two.width&&one.height==two.height;})==1;});
      };
      const auto prepared=[&](const NativeCcdData& ccd){
        if(std::count_if(ccd.paths.begin(),ccd.paths.end(),displayV2Active)!=1)return false;
        return displayV2GroupSatisfied(ccd,{&expected,1},current,current,saved.console)&&
          std::all_of(ccd.paths.begin(),ccd.paths.end(),[](const auto& path){return !displayV2Active(path)||path.targetInfo.rotation==DISPLAYCONFIG_ROTATION_IDENTITY;});
      };
      const auto before=access_.query();
      if(!prepared(before)||!confirm()||!eligible()||access_.store.load()!=original||!sameTargets(current,access_.owned())||!prepared(access_.query()))
        return fail(result,displayV2Stage::verify,"display_v2_handoff_output_changed");
      // No snapshot(), save(), configure(), remove() or SDK change occurs here.
      // The same original file remains the only source of final restoration.
      owned_=current;console_=saved.console;identify_=bool(console_);
      snapshotEvidence_={};snapshotEvidence_.before=displayV2GeometryEvidence(before,current);
      snapshotEvidence_.saved=displayV2GeometryEvidence(saved.ccd,saved.owned);
      snapshotEvidence_.reusedExisting=true;snapshotEvidence_.olderSession=saved.session!=logicalSession;
      active_=true;result.ok=true;result.owned=owned_;result.consoleProof=bool(console_);return result;
    }catch(...){return fail(result,displayV2Stage::internal,"display_v2_handoff_unavailable");}
  }
  // The exact old worker is about to exit under a private service handoff
  // grant. Closing ordinary streams still calls restore(). The caller must
  // skip that cleanup only after this succeeds AND the service retains the
  // lease and observes the exact child exit. No broker JSON flag can grant it.
  bool retainForHandoff(const std::string& originalSha256,const Confirm& serviceRetains){
    try{
      if(!active_||!serviceRetains||originalSha256.size()!=64)return false;
      const auto original=access_.store.load();
      if(!original||nativeBaselineDigest(*original)!=originalSha256||!serviceRetains()||access_.store.load()!=original)return false;
      active_=false;return true;
    }catch(...){return false;}
  }
  // snapshot -> owned size -> owned primary -> console off -> verify.
  DisplayV2Result start(const std::string& session,DWORD width,DWORD height,bool identifyConsole,uint64_t deadline,const Confirm& confirm={}){
    DisplayV2Result result;
    try{
      if(!eligible())return fail(result,displayV2Stage::ineligible,"display_v2_ineligible");
      if(!modeSupported(width,height)){result.modeUnsupported=true;result.error=ERROR_NOT_SUPPORTED;return fail(result,displayV2Stage::sdk,"unsupported_display_mode");}
      owned_=access_.owned();
      if(std::none_of(owned_.begin(),owned_.end(),[](const auto& t){return t.id=="0";}))return fail(result,displayV2Stage::ineligible,"display_v2_owned_missing");
      NativeCcdData before;
      try{before=access_.query();}catch(const NativeTopologyWindowsError& error){result.error=error.code;return fail(result,displayV2Stage::query,"display_v2_query_failed");}
      console_.reset();identify_=identifyConsole;result.consoleProof=identify_&&bool(access_.identifyConsole);
      if(result.consoleProof)try{console_=access_.identifyConsole(before,owned_);}catch(...){console_.reset();}
      // Remember how to put Windows back before the first change.
      if(!snapshot(session,before))return fail(result,displayV2Stage::state,"display_v2_state_unavailable");
      active_=true;result.consoleRemoved=console_&&displayV2ConsoleActive(before,*console_);
      const auto previous=owned_;
      settleConfirmed(result,confirm,deadline,[&]{change(result,before,width,height,deadline,false,&previous);});
    }catch(...){if(!result.stage)result.stage=displayV2Stage::internal;result.ok=false;if(result.reason.empty())result.reason="display_v2_internal";}
    if(!result.ok&&active_)result.restore=restore();
    result.owned=owned_;return result;
  }
  // SDK size -> owned primary at the new size (console kept off) -> verify.
  DisplayV2Result resize(DWORD width,DWORD height,uint64_t deadline,const Confirm& confirm={}){
    DisplayV2Result result;
    try{
      if(!active_)return fail(result,displayV2Stage::ineligible,"display_v2_inactive");
      if(!modeSupported(width,height)){result.modeUnsupported=true;result.error=ERROR_NOT_SUPPORTED;return fail(result,displayV2Stage::sdk,"unsupported_display_mode");}
      if(!eligible()){fail(result,displayV2Stage::ineligible,"display_v2_ineligible");}
      else{
        NativeCcdData before;
        try{before=access_.query();}catch(const NativeTopologyWindowsError& error){result.error=error.code;fail(result,displayV2Stage::query,"display_v2_query_failed");}
        if(!result.stage){const auto previous=owned_;settleConfirmed(result,confirm,deadline,[&]{change(result,before,width,height,deadline,true,&previous);});}
      }
    }catch(...){if(!result.stage)result.stage=displayV2Stage::internal;result.ok=false;if(result.reason.empty())result.reason="display_v2_internal";}
    if(!result.ok)result.restore=restore();
    result.owned=owned_;return result;
  }
  // Independent outputs (tracks-v1): snapshot -> SDK sizes -> "0" primary at
  // the origin, "1" at the client's relative place -> identified console off
  // -> verify. The group must name exactly the owned outputs (the count is the
  // client's separate prior step), else nothing changes. Failure restores.
  DisplayV2Result startGroup(const std::string& session,std::span<const DisplayV2Output> outputs,bool identifyConsole,uint64_t deadline,const Confirm& confirm={}){
    DisplayV2Result result;
    try{
      if(!eligible())return fail(result,displayV2Stage::ineligible,"display_v2_ineligible");
      std::vector<DisplayV2Output> group;
      try{group=displayV2GroupLayout(outputs);}catch(...){return fail(result,displayV2Stage::ineligible,"display_v2_group_invalid");}
      for(const auto& output:group)if(!modeSupported(output.width,output.height)){result.modeUnsupported=true;result.error=ERROR_NOT_SUPPORTED;return fail(result,displayV2Stage::sdk,"unsupported_display_mode");}
      owned_=access_.owned();
      if(!displayV2GroupNamesOwned(owned_,group))return fail(result,displayV2Stage::ineligible,"display_v2_group_mismatch");
      NativeCcdData before;
      try{before=access_.query();}catch(const NativeTopologyWindowsError& error){result.error=error.code;return fail(result,displayV2Stage::query,"display_v2_query_failed");}
      console_.reset();identify_=identifyConsole;result.consoleProof=identify_&&bool(access_.identifyConsole);
      if(result.consoleProof)try{console_=access_.identifyConsole(before,owned_);}catch(...){console_.reset();}
      if(!snapshot(session,before))return fail(result,displayV2Stage::state,"display_v2_state_unavailable");
      active_=true;result.consoleRemoved=console_&&displayV2ConsoleActive(before,*console_);
      const auto previous=owned_;
      settleConfirmed(result,confirm,deadline,[&]{changeGroup(result,before,group,deadline,false,&previous);});
    }catch(...){if(!result.stage)result.stage=displayV2Stage::internal;result.ok=false;if(result.reason.empty())result.reason="display_v2_internal";}
    if(!result.ok&&active_)result.restore=restore();
    result.owned=owned_;return result;
  }
  // SDK sizes -> the group at the client's new places (console kept off) ->
  // verify. No new snapshot. A failure restores the saved configuration, or
  // with `keepOnFailure` leaves Windows as it is so the caller can put the
  // stream's previous group back in place first (the restored-group path).
  DisplayV2Result resizeGroup(std::span<const DisplayV2Output> outputs,uint64_t deadline,const Confirm& confirm={},bool keepOnFailure=false){
    DisplayV2Result result;
    try{
      if(!active_)return fail(result,displayV2Stage::ineligible,"display_v2_inactive");
      std::vector<DisplayV2Output> group;bool valid=true;
      try{group=displayV2GroupLayout(outputs);}catch(...){valid=false;}
      if(valid)for(const auto& output:group)if(!modeSupported(output.width,output.height)){result.modeUnsupported=true;result.error=ERROR_NOT_SUPPORTED;return fail(result,displayV2Stage::sdk,"unsupported_display_mode");}
      if(!eligible())fail(result,displayV2Stage::ineligible,"display_v2_ineligible");
      else if(!valid||!displayV2GroupNamesOwned(owned_,group))fail(result,displayV2Stage::ineligible,"display_v2_group_mismatch");
      else{
        NativeCcdData before;
        try{before=access_.query();}catch(const NativeTopologyWindowsError& error){result.error=error.code;fail(result,displayV2Stage::query,"display_v2_query_failed");}
        if(!result.stage){const auto previous=owned_;settleConfirmed(result,confirm,deadline,[&]{changeGroup(result,before,group,deadline,true,&previous);});}
      }
    }catch(...){if(!result.stage)result.stage=displayV2Stage::internal;result.ok=false;if(result.reason.empty())result.reason="display_v2_internal";}
    if(!result.ok&&!keepOnFailure)result.restore=restore();
    result.owned=owned_;return result;
  }
  // Owned output count 1 <-> 2 outside a stream: snapshot -> SDK count ->
  // owned "0" primary, "1" to its right, every display active before the change
  // back with its pre-change mode (an inactive console stays inactive) ->
  // verify -> GDI/capture proof. Success leaves the new topology as Windows'
  // current configuration (a persistent user choice, the baseline of the next
  // stream): the snapshot this call created is deleted; an older unrestored
  // original is kept for the ordinary automatic recovery. Any failure puts the
  // previous SDK count back and restores the snapshot. Never latches.
  DisplayV2Result count(const std::string& session,size_t wanted,bool identifyConsole,uint64_t deadline,uint64_t restoreBudgetMs=12000){
    DisplayV2Result result;bool created=false;std::vector<NativeDisplayTarget> previous;
    try{
      if(wanted!=1&&wanted!=2)return fail(result,displayV2Stage::internal,"display_v2_count_invalid");
      if(!eligible())return fail(result,displayV2Stage::ineligible,"display_v2_ineligible");
      owned_=access_.owned();
      if(std::none_of(owned_.begin(),owned_.end(),[](const auto& t){return t.id=="0";}))return fail(result,displayV2Stage::ineligible,"display_v2_owned_missing");
      // Already the requested count: nothing to change (same as v1).
      if(owned_.size()==wanted){result.ok=true;result.owned=owned_;return result;}
      if(!access_.count)return fail(result,displayV2Stage::sdk,"display_v2_sdk_unavailable");
      NativeCcdData before;
      try{before=access_.query();}catch(const NativeTopologyWindowsError& error){result.error=error.code;return fail(result,displayV2Stage::query,"display_v2_query_failed");}
      console_.reset();identify_=identifyConsole;result.consoleProof=identify_&&bool(access_.identifyConsole);
      if(result.consoleProof)try{console_=access_.identifyConsole(before,owned_);}catch(...){console_.reset();}
      result.consoleIdentified=console_.has_value();result.foreignBefore=uint32_t(displayV2Foreign(before,owned_,std::nullopt).size());
      if(!snapshot(session,before,&created))return fail(result,displayV2Stage::state,"display_v2_state_unavailable");
      active_=true;previous=owned_;
      changeCount(result,before,wanted,deadline);
    }catch(...){if(!result.stage)result.stage=displayV2Stage::internal;result.ok=false;if(result.reason.empty())result.reason="display_v2_internal";}
    if(!result.ok&&active_){
      // Previous SDK count first (a no-op when the service never changed it),
      // then the saved Windows configuration.
      if(result.countAttempted&&access_.count)try{if(const auto back=access_.count(previous.size()))owned_=*back;}catch(...){ }
      result.restore=restore(restoreBudgetMs);
      result.stateKept=hasState();
    }else if(result.ok&&active_){
      active_=false;
      result.stateKept=!created||!removeState();
    }
    result.owned=owned_;return result;
  }
  // Stop / failure / crash recovery. Idempotent: no state file = nothing to do.
  DisplayV2RestoreResult restore(uint64_t budgetMs=12000){
    DisplayV2RestoreResult out;active_=false;restoreStatePresence_.reset();
    const auto deadline=access_.now()+budgetMs;
    std::optional<DisplayV2State> saved;
    try{const auto bytes=access_.store.load();restoreStatePresence_=bytes.has_value();if(!bytes)return out;saved=parseDisplayV2State(*bytes);}
    catch(...){
      // Unusable state cannot block anything. Without it, only the identified
      // console of this live session can still be brought back.
      restoreStatePresence_.reset();try{access_.store.discard();}catch(...){ }
      if(console_&&returnConsole(*console_,nullptr,deadline)){out.outcome=DisplayV2Outcome::consoleOnly;return out;}
      return out;
    }
    // A locked/foreign session changes nothing (not even the SDK size); the
    // state stays for the service's next automatic attempt.
    if(!eligible()){out.outcome=DisplayV2Outcome::failed;out.stage=displayV2Stage::ineligible;return out;}
    if(!saved->console&&console_)saved->console=console_;
    const auto console=saved->console?saved->console:console_;
    const bool consoleExpected=console&&displayV2ConsoleActive(saved->ccd,*console);
    out.consoleExpected=consoleExpected;
    std::vector<NativeDisplayTarget> current;
    try{
      current=access_.restoreOwned?access_.restoreOwned():access_.owned();const auto fresh=access_.query();
      out.consoleActive=console&&displayV2ConsoleActive(fresh,*console);
      const auto identity=preflightDisplayV2Restore(*saved,fresh,current,access_.retired);
      out.stale=identity.stale;
      if(!identity.resolved){
        // Field case: a saved adapter stayed gone and the state failed
        // every minute, which also blocks later cleanup. Time stamps did not prove a
        // reboot there, so the caller decides from repeated attempts.
        if(identity.stale&&access_.discardStale&&access_.discardStale()){restoreStatePresence_.reset();try{access_.store.discard();}catch(...){ }return out;}
        out.outcome=DisplayV2Outcome::failed;out.stage=displayV2Stage::foreign;return out;
      }
    }catch(const NativeTopologyWindowsError& error){out.outcome=DisplayV2Outcome::failed;out.stage=displayV2Stage::query;out.error=error.code;return out;}
    catch(...){out.outcome=DisplayV2Outcome::failed;out.stage=displayV2Stage::query;return out;}
    // Put the owned outputs back to their saved size first (best effort):
    // several in one SDK reconfiguration when available, else one by one.
    std::vector<DisplayV2Output> back;
    for(const auto& old:saved->owned){
      // The SDK's cached target size may differ from the desktop Windows was
      // actually using. Prefer the saved active CCD source, including for old
      // state files; inactive outputs have only their saved SDK size.
      DWORD width=old.width,height=old.height;
      for(const auto& path:saved->ccd.paths)if(displayV2Active(path)&&nativeSameTarget(path,old)){
        const auto& source=nativeCcdSource(saved->ccd,path);width=source.width;height=source.height;break;
      }
      const auto now=std::find_if(current.begin(),current.end(),[&](const auto& t){return t.id==old.id;});
      if(now!=current.end()&&(now->width!=width||now->height!=height))back.push_back({old.id,width,height});
    }
    if(back.size()>1&&access_.sizes)try{if(const auto changed=access_.sizes(back))current=*changed;}catch(...){ }
    if(access_.resize)for(const auto& old:back){
      const auto now=std::find_if(current.begin(),current.end(),[&](const auto& t){return t.id==old.id;});
      if(now==current.end()||(now->width==old.width&&now->height==old.height))continue;
      try{if(const auto changed=access_.resize(old.id,old.width,old.height))current=*changed;}catch(...){ }
    }
    owned_=current;
    // The SDK may have recreated targets; wait (bounded) until Windows lists them.
    waitUntil(std::min(deadline,access_.now()+3000),[&](const NativeCcdData& ccd){return std::all_of(current.begin(),current.end(),[&](const auto& t){return displayV2OwnedPath(ccd,t,{}).has_value();});});
    std::vector<DisplayV2Expected> expected;
    try{
      const auto plan=planDisplayV2Restore(*saved,access_.query(),current,&expected,access_.retired);
      const auto foreign=displayV2Foreign(plan,current,std::nullopt);
      LONG code=apply(plan,displayV2Supplied);if(code!=ERROR_SUCCESS)code=apply(plan,displayV2Supplied|SDC_ALLOW_CHANGES);
      out.error=DWORD(code);
      if(code==ERROR_SUCCESS&&waitUntil(std::min(deadline,access_.now()+2500),[&](const NativeCcdData& ccd){
          out.consoleActive=console&&displayV2ConsoleActive(ccd,*console);
          return displayV2RestoreTargetsMatch(ccd,expected)&&displayV2SameForeign(foreign,displayV2Foreign(ccd,current,std::nullopt))&&(!consoleExpected||out.consoleActive);})){
        if(removeState()){out.outcome=DisplayV2Outcome::restored;out.error=0;return out;}
        out.stage=displayV2Stage::state;
      }else out.stage=code==ERROR_SUCCESS?displayV2Stage::verify:displayV2Stage::apply;
    }catch(const DisplayV2RestoreIdentityError& error){out.outcome=DisplayV2Outcome::failed;out.stage=displayV2Stage::foreign;out.stale=error.stale;return out;}
    catch(const NativeTopologyWindowsError& error){out.outcome=DisplayV2Outcome::failed;out.stage=displayV2Stage::query;out.error=error.code;return out;}
    catch(...){out.stage=displayV2Stage::internal;}
    observe(out,expected,console);
    // Exact restore failed: never leave the VM without its console.
    if(consoleExpected){
      const bool returned=returnConsole(*console,&*saved,deadline);out.fallback=returned?1:2;
      if(returned){out.consoleActive=true;if(removeState()){out.outcome=DisplayV2Outcome::consoleOnly;return out;}}
    }
    out.outcome=DisplayV2Outcome::failed;return out;
  }

 private:
  bool modeSupported(DWORD width,DWORD height)const{return !access_.modeSupported||access_.modeSupported(width,height);}
  DisplayV2Access access_;bool active_=false,identify_=false;std::vector<NativeDisplayTarget> owned_;std::optional<DisplayV2Console> console_;DisplayV2SnapshotEvidence snapshotEvidence_;std::optional<bool> restoreStatePresence_;
  bool eligible(){try{return !access_.eligible||access_.eligible();}catch(...){return false;}}
  // A display the stream hides (access_.hidden) that is still on. Without a
  // console proof (the broker has none for a QXL/VirtIO/passthrough display, or
  // Proxmox unreadable) nothing else made a start whose owned output already
  // sat at the requested size and place apply its plan, and the console stayed
  // on. Never a new failure: after the retries the stream keeps what Windows
  // shows, as before.
  bool hiddenActive(const NativeCcdData& ccd,std::span<const NativeDisplayTarget> ours)const{
    if(!access_.hidden)return false;
    for(const auto& path:ccd.paths)if(displayV2Active(path)&&!nativeOwnedPath(path,ours)&&displayV2RetiredPath(access_.hidden,path))return true;
    return false;
  }
  static DisplayV2Result& fail(DisplayV2Result& result,uint32_t stage,const char* reason){result.ok=false;result.stage=stage;result.reason=reason;return result;}
  bool removeState(){try{const bool removed=access_.store.remove();restoreStatePresence_=removed?std::optional<bool>(false):std::nullopt;return removed;}catch(...){restoreStatePresence_.reset();return false;}}
  // Diagnostics only: which expected targets (and the console) are active now.
  void observe(DisplayV2RestoreResult& out,const std::vector<DisplayV2Expected>& expected,const std::optional<DisplayV2Console>& console){
    out.expected=uint32_t(expected.size());
    try{
      const auto ccd=access_.query();
      for(const auto& item:expected)if(!displayV2TargetActive(ccd,item.adapter,item.target)){++out.missing;if(item.owned)++out.missingOwned;}
      out.consoleActive=console&&displayV2ConsoleActive(ccd,*console);
    }catch(...){ }
  }
  bool hasState(){try{return access_.store.load().has_value();}catch(...){return true;}}
  template<class Predicate> bool waitUntil(uint64_t until,Predicate&& ready){
    for(;;){
      try{if(ready(access_.query()))return true;}catch(...){ }
      if(access_.now()>=until)return false;if(access_.poll)access_.poll();else access_.pause();
    }
  }
  LONG apply(const NativeCcdData& plan,UINT32 flags){
    if(!eligible())return LONG(ERROR_ACCESS_DENIED);
    const auto validated=access_.configure(plan,SDC_VALIDATE|flags);if(validated!=ERROR_SUCCESS)return validated;
    if(!eligible())return LONG(ERROR_ACCESS_DENIED);
    return access_.configure(plan,SDC_APPLY|flags);
  }
  // `created` = this call wrote a new snapshot (no older original existed).
  bool snapshot(const std::string& session,const NativeCcdData& before,bool* created=nullptr){
    if(created)*created=false;
    snapshotEvidence_={};snapshotEvidence_.before=displayV2GeometryEvidence(before,owned_);
    std::optional<std::string> existing;
    try{existing=access_.store.load();}catch(...){try{access_.store.discard();}catch(...){ }existing.reset();}
    if(existing){
      // An older unrestored snapshot is the true original; keep it. One from an
      // earlier boot is not: it cannot be restored, and keeping it lost this
      // stream's own original (field case: the stream turned the console
      // off, its stop failed on the old state, recovery then discarded that state
      // and the console stayed off). The current configuration replaces it.
      try{
        auto state=parseDisplayV2State(*existing);
        if(!displayV2StateFromEarlierBoot(state,before,owned_,access_.retired)){
          snapshotEvidence_.saved=displayV2GeometryEvidence(state.ccd,state.owned);
          snapshotEvidence_.reusedExisting=true;snapshotEvidence_.olderSession=state.session!=session;
          if(!state.console&&console_){state.console=console_;return access_.store.save(displayV2StateBytes(state));}
          return true;
        }
      }catch(...){try{access_.store.discard();}catch(...){ }}
    }
    try{const DisplayV2State state{session,displayV2ActiveCcd(before,access_.retired),owned_,console_};if(!access_.store.save(displayV2StateBytes(state)))return false;snapshotEvidence_.saved=displayV2GeometryEvidence(state.ccd,state.owned);snapshotEvidence_.reusedExisting=false;snapshotEvidence_.olderSession=false;if(created)*created=true;return true;}catch(...){return false;}
  }
  // The caller's confirmation is part of a settled change. GDI may list a
  // recreated output a moment after CCD does, and Windows may re-lay the
  // displays once more after the CCD settled (a late re-apply of its own
  // layout, more likely while the previous change is still settling). A failed
  // confirmation therefore runs the change again from fresh state (no SDK call
  // when the sizes are already right; no apply when the layout is still
  // right) and confirms again, before the change counts as failed.
  template<class Change> void settleConfirmed(DisplayV2Result& result,const Confirm& confirm,uint64_t deadline,Change&& change){
    for(uint32_t round=0;;++round){
      change();
      if(!result.ok||!confirm)return;
      bool confirmed=false;try{confirmed=confirm();}catch(...){confirmed=false;}
      if(confirmed)return;
      if(round+1>=displayV2ConfirmRounds||access_.now()>=deadline||!eligible()){fail(result,displayV2Stage::verify,"display_v2_capture_unavailable");return;}
      ++result.confirmRetries;result.ok=false;access_.pause();
    }
  }
  // The SDK owner's size change is idempotent (an output already at its size
  // is left alone and only re-published), so a refused or lost reply is asked
  // again before the change counts as failed: a transient lease publication or
  // controller hiccup must not end a stream.
  template<class Call> std::optional<std::vector<NativeDisplayTarget>> sdkCall(DisplayV2Result& result,uint64_t deadline,Call&& call){
    for(uint32_t attempt=0;;++attempt){
      std::optional<std::vector<NativeDisplayTarget>> changed;
      try{changed=call();}catch(...){changed.reset();}
      if(changed||attempt>=2||access_.now()>=deadline||!eligible())return changed;
      ++result.sdkRetries;for(int i=0;i<4;++i)access_.pause();
    }
  }
  // `earlier` = the owned targets before this change (a confirmation round
  // runs change() again after owned_ already holds the new generation).
  void change(DisplayV2Result& result,const NativeCcdData& before,DWORD width,DWORD height,uint64_t deadline,bool resizing,const std::vector<NativeDisplayTarget>* earlier=nullptr){
    const auto previous=earlier?*earlier:owned_;
    auto stream=std::find_if(owned_.begin(),owned_.end(),[](const auto& t){return t.id=="0";});
    if(stream==owned_.end()){fail(result,displayV2Stage::ineligible,"display_v2_owned_missing");return;}
    if(stream->width!=width||stream->height!=height){
      DisplayV2Timer sdkTime(result.sdkMs,access_.now);
      if(!access_.resize){fail(result,displayV2Stage::sdk,"display_v2_sdk_unavailable");return;}
      const auto changed=sdkCall(result,deadline,[&]{return access_.resize("0",width,height);});
      if(!changed){fail(result,displayV2Stage::sdk,"display_v2_sdk_resize_failed");return;}
      owned_=*changed;
    }
    DisplayV2Timer settleTime(result.settleMs,access_.now);
    stream=std::find_if(owned_.begin(),owned_.end(),[](const auto& t){return t.id=="0";});
    if(stream==owned_.end()||stream->width!=width||stream->height!=height){fail(result,displayV2Stage::sdk,"display_v2_sdk_size_mismatch");return;}
    const auto target=*stream;
    // Both the old and the new owned targets are ours, never "foreign".
    std::vector<NativeDisplayTarget> ours=previous;ours.insert(ours.end(),owned_.begin(),owned_.end());
    if(!waitUntil(std::min(deadline,access_.now()+5000),[&](const NativeCcdData& ccd){return displayV2OwnedPath(ccd,target,{}).has_value();})){fail(result,displayV2Stage::target,"display_v2_owned_target_unavailable");return;}
    // Windows may apply its own layout after a monitor arrival; re-plan from
    // fresh state up to three times within the deadline.
    // `streaming` = the owned output is right but other displays still differ;
    // kept so a final failure reports what differs (stage 8) instead of 7.
    std::optional<NativeCcdData> verified,streaming;LONG lastError=ERROR_SUCCESS;std::string why;
    const auto settled=[&](const NativeCcdData& ccd){
      if(!displayV2StreamingSatisfied(ccd,target,console_,&why))return false;
      if(hiddenActive(ccd,ours)){why="display_v2_hidden_active";streaming=ccd;return false;}
      if(displayV2SameForeign(displayV2Foreign(before,ours,console_,access_.hidden),displayV2Foreign(ccd,ours,console_,access_.hidden))){verified=ccd;return true;}
      streaming=ccd;return false;
    };
    for(int attempt=0;attempt<displayV2SettleAttempts&&!verified;++attempt){
      if(!eligible()){fail(result,displayV2Stage::ineligible,"display_v2_ineligible");return;}
      if(access_.now()>=deadline)break;
      NativeCcdData fresh;
      try{fresh=access_.query();}catch(const NativeTopologyWindowsError& error){lastError=LONG(error.code);continue;}
      // A console that was inactive before (so not identifiable then) and that
      // Windows has just activated can be positively identified now.
      if(!console_&&identify_&&access_.identifyConsole)try{console_=access_.identifyConsole(fresh,ours);}catch(...){console_.reset();}
      if(settled(fresh))break;
      bool removed=false;NativeCcdData plan;
      try{plan=planDisplayV2Streaming(fresh,before,target,ours,console_,&removed,access_.hidden);}catch(...){why="display_v2_plan_failed";access_.pause();continue;}
      if(removed&&resizing)result.consoleReturned=true;
      LONG code=apply(plan,displayV2Supplied);if(code!=ERROR_SUCCESS)code=apply(plan,displayV2Supplied|SDC_ALLOW_CHANGES);
      if(code!=ERROR_SUCCESS){lastError=code;access_.pause();continue;}
      // Settled only when the baseline displays are back as they were too;
      // otherwise the next attempt re-plans from what Windows did meanwhile.
      waitUntil(std::min(deadline,access_.now()+2000),settled);
    }
    result.consoleIdentified=console_.has_value();result.foreignBefore=uint32_t(displayV2Foreign(before,ours,console_,access_.hidden).size());
    if(!verified){streaming.reset();try{(void)settled(access_.query());}catch(...){ }if(!verified)verified=streaming;}
    if(!verified){result.error=DWORD(lastError);fail(result,lastError!=ERROR_SUCCESS?displayV2Stage::apply:displayV2Stage::verify,lastError!=ERROR_SUCCESS?"display_v2_apply_failed":"display_v2_verify_failed");return;}
    if(const auto difference=displayV2ForeignDifference(displayV2Foreign(before,ours,console_,access_.hidden),displayV2Foreign(*verified,ours,console_,access_.hidden))){result.foreignDifference=difference;fail(result,displayV2Stage::foreign,"display_v2_foreign_changed");return;}
    result.ok=true;result.stage=0;result.error=0;result.reason.clear();
  }
  // `group` is canonical (displayV2GroupLayout) and names exactly owned_.
  void changeGroup(DisplayV2Result& result,const NativeCcdData& before,std::span<const DisplayV2Output> group,uint64_t deadline,bool resizing,const std::vector<NativeDisplayTarget>* earlier=nullptr){
    const auto previous=earlier?*earlier:owned_;
    const auto find=[&](const std::string& id){return std::find_if(owned_.begin(),owned_.end(),[&](const auto& t){return t.id==id;});};
    std::vector<DisplayV2Output> changes;
    for(const auto& output:group){const auto target=find(output.id);if(target==owned_.end()){fail(result,displayV2Stage::ineligible,"display_v2_owned_missing");return;}if(target->width!=output.width||target->height!=output.height)changes.push_back(output);}
    if(!changes.empty()){
      // One SDK reconfiguration for both outputs when the service offers it:
      // Windows reacts to every target recreation (it re-activates the console, for one).
      std::optional<std::vector<NativeDisplayTarget>> changed;
      if(access_.sizes&&(changes.size()>1||!access_.resize)){changed=sdkCall(result,deadline,[&]{return access_.sizes(changes);});if(!changed){fail(result,displayV2Stage::sdk,"display_v2_sdk_resize_failed");return;}owned_=*changed;}
      else{
        if(!access_.resize){fail(result,displayV2Stage::sdk,"display_v2_sdk_unavailable");return;}
        for(const auto& output:changes){changed=sdkCall(result,deadline,[&]{return access_.resize(output.id,output.width,output.height);});if(!changed){fail(result,displayV2Stage::sdk,"display_v2_sdk_resize_failed");return;}owned_=*changed;}
      }
    }
    std::vector<NativeDisplayTarget> targets;
    if(owned_.size()!=group.size()){fail(result,displayV2Stage::sdk,"display_v2_sdk_size_mismatch");return;}
    for(const auto& output:group){const auto target=find(output.id);if(target==owned_.end()||target->width!=output.width||target->height!=output.height){fail(result,displayV2Stage::sdk,"display_v2_sdk_size_mismatch");return;}targets.push_back(*target);}
    // Both the old and the new owned targets are ours, never "foreign".
    std::vector<NativeDisplayTarget> ours=previous;ours.insert(ours.end(),owned_.begin(),owned_.end());
    if(!waitUntil(std::min(deadline,access_.now()+5000),[&](const NativeCcdData& ccd){return std::all_of(targets.begin(),targets.end(),[&](const auto& t){return displayV2OwnedPath(ccd,t,{}).has_value();});})){fail(result,displayV2Stage::target,"display_v2_owned_target_unavailable");return;}
    // Windows may re-lay displays after every target arrival; re-plan from
    // fresh state up to three times within the deadline (same as change()).
    std::optional<NativeCcdData> verified,partial;LONG lastError=ERROR_SUCCESS;std::string why;
    const auto settled=[&](const NativeCcdData& ccd){
      if(!displayV2GroupSatisfied(ccd,group,targets,ours,console_,&why))return false;
      if(hiddenActive(ccd,ours)){why="display_v2_hidden_active";partial=ccd;return false;}
      if(displayV2SameForeign(displayV2Foreign(before,ours,console_,access_.hidden),displayV2Foreign(ccd,ours,console_,access_.hidden))){verified=ccd;return true;}
      partial=ccd;return false;
    };
    for(int attempt=0;attempt<displayV2SettleAttempts&&!verified;++attempt){
      if(!eligible()){fail(result,displayV2Stage::ineligible,"display_v2_ineligible");return;}
      if(access_.now()>=deadline)break;
      NativeCcdData fresh;
      try{fresh=access_.query();}catch(const NativeTopologyWindowsError& error){lastError=LONG(error.code);continue;}
      // A console that was inactive before and that Windows has just
      // activated can be positively identified now (with a proof).
      if(!console_&&identify_&&access_.identifyConsole)try{console_=access_.identifyConsole(fresh,ours);}catch(...){console_.reset();}
      if(settled(fresh))break;
      bool removed=false;NativeCcdData plan;
      try{plan=planDisplayV2Group(fresh,before,group,targets,ours,console_,&removed,access_.hidden);}catch(...){why="display_v2_plan_failed";access_.pause();continue;}
      if(removed&&resizing)result.consoleReturned=true;
      LONG code=apply(plan,displayV2Supplied);if(code!=ERROR_SUCCESS)code=apply(plan,displayV2Supplied|SDC_ALLOW_CHANGES);
      if(code!=ERROR_SUCCESS){lastError=code;access_.pause();continue;}
      waitUntil(std::min(deadline,access_.now()+2000),settled);
    }
    result.consoleIdentified=console_.has_value();result.foreignBefore=uint32_t(displayV2Foreign(before,ours,console_,access_.hidden).size());
    if(!verified){partial.reset();try{(void)settled(access_.query());}catch(...){ }if(!verified)verified=partial;}
    if(!verified){result.error=DWORD(lastError);fail(result,lastError!=ERROR_SUCCESS?displayV2Stage::apply:displayV2Stage::verify,lastError!=ERROR_SUCCESS?"display_v2_apply_failed":"display_v2_verify_failed");return;}
    if(const auto difference=displayV2ForeignDifference(displayV2Foreign(before,ours,console_,access_.hidden),displayV2Foreign(*verified,ours,console_,access_.hidden))){result.foreignDifference=difference;fail(result,displayV2Stage::foreign,"display_v2_foreign_changed");return;}
    result.ok=true;result.stage=0;result.error=0;result.reason.clear();
  }
  void changeCount(DisplayV2Result& result,const NativeCcdData& before,size_t wanted,uint64_t deadline){
    const auto previous=owned_;
    std::optional<std::vector<NativeDisplayTarget>> changed;
    // From here on the service may have changed the SDK count even when its
    // reply is lost; a failure therefore always asks for the previous count.
    result.countAttempted=true;
    try{changed=access_.count(wanted);}catch(...){changed.reset();}
    if(!changed){fail(result,displayV2Stage::sdk,"display_v2_sdk_count_failed");return;}
    owned_=displayV2OrderedTargets(*changed);
    const auto survivor=std::find_if(previous.begin(),previous.end(),[](const auto& t){return t.id=="0";});
    if(owned_.size()!=wanted||owned_.front().id!="0"||(wanted==2&&owned_[1].id!="1")||survivor==previous.end()||owned_.front().width!=survivor->width||owned_.front().height!=survivor->height){fail(result,displayV2Stage::sdk,"display_v2_sdk_count_mismatch");return;}
    // Old and new owned targets are ours, never "foreign".
    std::vector<NativeDisplayTarget> ours=previous;ours.insert(ours.end(),owned_.begin(),owned_.end());
    if(!waitUntil(std::min(deadline,access_.now()+5000),[&](const NativeCcdData& ccd){return std::all_of(owned_.begin(),owned_.end(),[&](const auto& t){return displayV2OwnedPath(ccd,t,{}).has_value();});})){fail(result,displayV2Stage::target,"display_v2_owned_target_unavailable");return;}
    // Every other display, the console included, must be back exactly as it
    // was before the change (active state, mode size, refresh, rotation,
    // format); positions are re-laid (always applied once, so the owned group
    // and the displays right of it are contiguous). Windows may apply its own
    // layout after a monitor arrival/departure, so re-plan from fresh state up
    // to 3 times.
    const auto expected=displayV2Foreign(before,ours,std::nullopt);
    std::optional<NativeCcdData> verified,partial;LONG lastError=ERROR_SUCCESS;std::string why;
    const auto settled=[&](const NativeCcdData& ccd){
      if(!displayV2CountSatisfied(ccd,owned_,ours,&why))return false;
      if(displayV2SameForeign(expected,displayV2Foreign(ccd,ours,std::nullopt))){verified=ccd;return true;}
      partial=ccd;return false;
    };
    const auto returned=[&](const NativeCcdData& ccd){
      for(const auto& path:ccd.paths)if(displayV2Active(path)&&!nativeOwnedPath(path,ours)&&!displayV2TargetActive(before,path.targetInfo.adapterId,path.targetInfo.id))return true;
      return false;
    };
    for(int attempt=0;attempt<3&&!verified;++attempt){
      if(!eligible()){fail(result,displayV2Stage::ineligible,"display_v2_ineligible");return;}
      if(access_.now()>=deadline)break;
      NativeCcdData fresh;
      try{fresh=access_.query();}catch(const NativeTopologyWindowsError& error){lastError=LONG(error.code);continue;}
      if(returned(fresh))result.displaysReturned=true;
      // A console Windows has just activated can be positively identified now
      // (with a proof); it is still only put back to its pre-change state.
      if(!console_&&identify_&&access_.identifyConsole)try{console_=access_.identifyConsole(fresh,ours);}catch(...){console_.reset();}
      if(attempt&&settled(fresh))break;
      NativeCcdData plan;
      try{plan=planDisplayV2Count(fresh,before,owned_,ours);}catch(...){why="display_v2_plan_failed";access_.pause();continue;}
      LONG code=apply(plan,displayV2Supplied);if(code!=ERROR_SUCCESS)code=apply(plan,displayV2Supplied|SDC_ALLOW_CHANGES);
      if(code!=ERROR_SUCCESS){lastError=code;access_.pause();continue;}
      waitUntil(std::min(deadline,access_.now()+2000),settled);
    }
    result.consoleIdentified=console_.has_value();result.foreignBefore=uint32_t(expected.size());
    if(!verified){partial.reset();try{(void)settled(access_.query());}catch(...){ }if(!verified)verified=partial;}
    if(!verified){result.error=DWORD(lastError);fail(result,lastError!=ERROR_SUCCESS?displayV2Stage::apply:displayV2Stage::verify,lastError!=ERROR_SUCCESS?"display_v2_apply_failed":"display_v2_verify_failed");return;}
    if(const auto difference=displayV2ForeignDifference(expected,displayV2Foreign(*verified,ours,std::nullopt))){result.foreignDifference=difference;fail(result,displayV2Stage::foreign,"display_v2_foreign_changed");return;}
    // Windows' CCD is settled; the change is kept only when GDI/capture also
    // lists exactly the owned outputs (never a console or foreign display).
    if(access_.usable){bool usable=false;try{usable=access_.usable(owned_);}catch(...){usable=false;}if(!usable){fail(result,displayV2Stage::verify,"display_v2_capture_unavailable");return;}}
    result.ok=true;result.stage=0;result.error=0;result.reason.clear();
  }
  bool returnConsole(const DisplayV2Console& console,const DisplayV2State* saved,uint64_t deadline){
    try{
      if(!eligible())return false;
      const auto fresh=access_.query();if(saved&&!preflightDisplayV2Restore(*saved,fresh,owned_,access_.retired).resolved)return false;
      if(displayV2ConsoleActive(fresh,console))return true;
      const auto ready=[&](const NativeCcdData& ccd){return displayV2ConsoleActive(ccd,console);};
      if(saved)if(const auto plan=planDisplayV2ConsoleReturn(fresh,console,&saved->ccd)){
        LONG code=apply(*plan,displayV2Supplied);if(code!=ERROR_SUCCESS)code=apply(*plan,displayV2Supplied|SDC_ALLOW_CHANGES);
        if(code==ERROR_SUCCESS&&waitUntil(std::min(deadline,access_.now()+2000),ready))return true;
      }
      // Last resort: supply only the topology and let Windows pick the modes.
      NativeCcdData topology=displayV2ActiveCcd(fresh);std::set<DisplayV2SourceKey> taken;for(const auto& path:topology.paths)taken.insert(nativeSourceKey(path));
      DISPLAYCONFIG_PATH_INFO wanted{};wanted.sourceInfo.adapterId=console.sourceAdapter;wanted.sourceInfo.id=console.source;wanted.targetInfo.adapterId=console.targetAdapter;wanted.targetInfo.id=console.target;
      const auto chosen=displayV2ForeignPath(fresh,wanted,taken);if(!chosen)return false;
      auto path=fresh.paths[*chosen];path.flags|=DISPLAYCONFIG_PATH_ACTIVE;topology.paths.push_back(path);
      const auto code=apply(displayV2PathsOnly(topology),SDC_TOPOLOGY_SUPPLIED|SDC_ALLOW_PATH_ORDER_CHANGES|SDC_VIRTUAL_MODE_AWARE);
      return code==ERROR_SUCCESS&&waitUntil(std::min(deadline,access_.now()+2000),ready);
    }catch(...){return false;}
  }
};
}
