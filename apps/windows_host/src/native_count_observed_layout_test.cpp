// Reconstructs a field-observed 3704x1134 / 1582x952 left-hand
// owned layout using the real group coordinator before shrinking to one output.
// SDK memory transport / CCD fixture only; it does not operate on a real display.
#define main upstreamNativeDisplayServiceChangeMain
#include "native_display_service_change_test.cpp"
#undef main
// Windows requires one edge-connected desktop region. Earlier CCD fixtures
// accepted separated rectangles, hiding the bridge-output removal failure.
bool observedLayoutConnected(const host::NativeCcdData& ccd){
  std::set<std::tuple<LONG,LONG,DWORD,DWORD>> unique;
  for(const auto& path:ccd.paths)if(path.flags&DISPLAYCONFIG_PATH_ACTIVE){const auto& s=host::nativeCcdSource(ccd,path);unique.emplace(s.position.x,s.position.y,s.width,s.height);}
  std::vector<std::tuple<LONG,LONG,DWORD,DWORD>> screens(unique.begin(),unique.end());if(screens.empty())return false;
  std::set<size_t> seen{0};bool changed=true;
  while(changed){changed=false;for(size_t i=0;i<screens.size();++i)if(!seen.count(i))for(const auto j:seen){
    const auto [ax,ay,aw,ah]=screens[i];const auto [bx,by,bw,bh]=screens[j];
    const bool vertical=(int64_t(ax)+aw==bx||int64_t(bx)+bw==ax)&&std::max<int64_t>(ay,by)<std::min<int64_t>(int64_t(ay)+ah,int64_t(by)+bh);
    const bool horizontal=(int64_t(ay)+ah==by||int64_t(by)+bh==ay)&&std::max<int64_t>(ax,bx)<std::min<int64_t>(int64_t(ax)+aw,int64_t(bx)+bw);
    if(vertical||horizontal){seen.insert(i);changed=true;break;}
  }}return seen.size()==screens.size();
}
int main(){using namespace host;try{
  CountFixture f;
  const auto console=f.ccd.paths.front();auto& source=f.ccd.modes[nativeSourceIndex(console)].sourceMode;source.width=1280;source.height=720;
  auto& signal=f.ccd.modes[nativeTargetIndex(console)].targetMode.targetVideoSignalInfo;signal.activeSize={1280,720};signal.totalSize={1440,750};signal.pixelRate=uint64_t(1440)*750*60;
  auto setup=trackRequest();setup.outputs={{"0",3704,1134,0,0},{"1",1582,952,-1582,0}};
  StreamResizeBudget setupBudget;const auto configured=applyNativeWorkerTracks(trackCurrent(f),setup,trackWorkerAccess(f,setup,setupBudget),setupBudget);
  require(configured.ok,"Could not reconstruct observed layout");const auto before=f.countProbe();require(before.monitors.size()==2&&before.monitors[0].left==2862&&before.monitors[1].left==1280,"Fixture differs from the actual observation");
  auto access=f.countAccess();const auto configure=access.topology.configure;
  access.topology.configure=[configure](const NativeCcdData& ccd,UINT32 flags){if(!observedLayoutConnected(ccd))return LONG(ERROR_BAD_CONFIGURATION);return configure(ccd,flags);};
  const auto exchange=access.exchange;
  access.exchange=[&](const std::string& phase,const std::string& key,bool verified,const std::string& baseline,const StreamResizeBudget& budget){
    auto reply=exchange(phase,key,verified,baseline,budget);
    if(phase=="worker.native.count.prepare"&&reply.accepted){
      // Simulate Windows closing the hole left by the departing bridge output.
      // It preserves the console; an attempt to restore x=2862 must be rejected.
      const auto targets=nativeDisplayTargets(reply.snapshot);for(auto& path:f.ccd.paths)if((path.flags&DISPLAYCONFIG_PATH_ACTIVE)&&nativeOwnedPath(path,targets)){auto& s=f.ccd.modes[nativeSourceIndex(path)].sourceMode;s.position={1280,0};}
    }return reply;
  };
  StreamResizeBudget countBudget;const auto result=applyNativeWorkerCount(f.desired,access,countBudget);
  if(!result.ok)std::cerr<<"count observed layout: failed, rollback="<<result.rollbackAttempted<<'/'<<result.rollbackSucceeded<<" exchanges="<<f.countExchanges<<" reason="<<result.reason<<'\n';
  require(result.ok&&result.display.monitors.size()==1&&result.display.monitors.front().id=="0"&&result.display.monitors.front().width==3704&&result.display.monitors.front().left==1280&&observedLayoutConnected(f.ccd),"Observed-layout count transition failed");f.transport.checkForeign();
  std::cout<<"{\"kind\":\"native-count-vm149-observed-layout\",\"shrinkSucceeded\":true,\"actualSdkDispatcher\":true,\"ccd\":\"injected\",\"actualVm149Accessed\":false}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
