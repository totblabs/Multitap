#define MULTITAP_INITIAL_PROFILE_TRACKS_FIXTURE_ONLY
#include "initial_profile_tracks_test.cpp"
#include "profile_track_capability.hpp"

int main(){using namespace host;int passed=0;try{
  for(const bool single:{false,true}){
    WorkerFixture fixture(single);auto config=fixture.config;
    setFixtureFlag(config,"streamProfileTrialEnabled",true);setFixtureFlag(config,"streamProfileTracksTrialEnabled",true);
    auto display=fixture.probe();display.viewportProvisioning=display.monitorControlSupported=true;
    const auto snapshot=fixture.state();
    const auto allowed=[&](const DisplayState& d,const Json& c,const Json& s,bool active=false){
      return nativeStreamProfileTracksCapability(d,c,true,true,active,s,GetCurrentProcessId());};
    require(allowed(display,config,snapshot),"Verified passive owned group did not advertise");++passed;
    {auto wrong=config;streamField(wrong,"deviceInstanceId",std::string("ROOT\\DISPLAY\\9999"));require(!allowed(display,wrong,snapshot),"Different configured native device advertised");++passed;}
    require(!nativeStreamProfileTracksCapability(display,config,false,true,false,snapshot,GetCurrentProcessId())&&
      !nativeStreamProfileTracksCapability(display,config,true,false,false,snapshot,GetCurrentProcessId()),"Unsupported or unavailable group advertised");++passed;
    for(const auto* flag:{"enabled","streamProfileTrialEnabled","streamProfileTracksTrialEnabled"}){
      for(const auto& invalid:{JsonParser::parse("null"),JsonParser::parse("false"),JsonParser::parse("1")}){
        auto wrong=config;std::get<Json::Object>(wrong.value)[flag]=invalid;
        require(!allowed(display,wrong,snapshot),"Absent/disabled/mistyped private capability flag accepted");++passed;
      }
    }
    for(int variant=0;variant<14;++variant){auto wrong=display;switch(variant){
      case 0:wrong.monitors.clear();break;case 1:wrong.monitors.front().id="2";break;
      case 2:wrong.monitors.front().width+=2;break;case 3:wrong.monitors.front().height=179;break;
      case 4:wrong.monitors.front().width=641;break;case 5:wrong.viewportProvisioning=false;break;
      case 6:wrong.monitorControlSupported=false;break;case 7:wrong.recoveryRequired=true;break;
      case 8:wrong.configurationPending=true;break;case 9:wrong.remoteSession=true;break;
      case 10:wrong.monitors.push_back(wrong.monitors.front());break;case 11:wrong.owned=false;break;
      case 12:wrong.active=false;break;case 13:wrong.deviceReady=false;break;}
      require(!allowed(wrong,config,snapshot),"Invalid actual group display advertised");++passed;
    }
    for(int variant=0;variant<11;++variant){auto wrong=snapshot;switch(variant){
      case 0:streamField(wrong,"servicePid",GetCurrentProcessId()+1);break;case 1:streamField(wrong,"generation",0);break;
      case 2:streamField(wrong,"deviceInstanceId",std::string("invalid-instance"));break;
      case 3:setFixtureFlag(wrong,"leaseActive",false);break;case 4:setFixtureFlag(wrong,"activationVerified",false);break;
      case 5:setFixtureFlag(wrong,"configurationPending",true);break;case 6:setFixtureFlag(wrong,"recoveryRequired",true);break;
      case 7:streamField(wrong,"leaseActive",1);break;case 8:streamField(wrong,"activationVerified",1);break;
      case 9:streamField(wrong,"configurationPending",0);break;case 10:streamField(wrong,"recoveryRequired",0);break;}
      require(!allowed(display,config,wrong),"Stale/foreign/mistyped native lease advertised");++passed;
    }
    require(!allowed(display,config,snapshot,true),"Active profile allowed a non-primary owned root");++passed;
    auto active=display;const auto root=std::find_if(active.monitors.begin(),active.monitors.end(),[](const auto& m){return m.id=="0";});
    require(root!=active.monitors.end(),"Fixture lacks owned zero");const LONG dx=root->left,dy=root->top;
    for(auto& m:active.monitors){m.left-=dx;m.top-=dy;m.primary=m.id=="0";}
    active.monitorControlSupported=false;
    require(allowed(active,config,snapshot,true)&&!allowed(active,config,snapshot,false),"Active profile lost viewport support or allowed count changes");++passed;
    // Display manager v2 streams hold no profile reservation or journal: their
    // stream flag alone keeps the active-group advertisement (same wire fields).
    require(allowed(active,config,snapshot,nativeStreamProfileTracksActive(false,true,false,true))&&!allowed(active,config,snapshot,nativeStreamProfileTracksActive(false,true,true,true)),"A v2 stream without a reservation lost (or a stopped one kept) the active group capability");++passed;
    active.monitors.front().left+=2;require(!allowed(active,config,snapshot,true),"Active profile left the owned origin");++passed;
  }
  for(int bits=0;bits<16;++bits){const bool profile=bits&1,v2=bits&2,stopped=bits&4,streaming=bits&8;
    require(nativeStreamProfileTracksActive(profile,v2,stopped,streaming)==((profile||v2)&&!stopped&&streaming),"Active group rule is wrong");}++passed;
  for(const bool declared:{false,true}){
    const auto fields=nativeStreamProfileTracksCapabilityFields(true,declared);
    const auto all=JsonParser::parse(std::string("{\"service\":true")+(declared?nativeStreamProfileCapabilityFields(true):"")+fields+'}');
    require(all.get("streamProfileProtocol").string()=="console-profile-v1"&&all.get("streamProfileTracksProtocol").string()=="group-v1"&&
      all.get("videoTrackProtocol").string()=="tracks-v1"&&all.get("trackViewportProvisioning").boolean()&&
      nativeStreamProfileTracksCapabilityFields(false,declared).empty(),"Incomplete or conflicting group feature fields");++passed;
  }
  std::cout<<"{\"kind\":\"profile-track-capability\",\"checks\":"<<passed<<",\"actualLeaseDecoder\":true,\"ccdAndSdk\":\"injected\",\"defaultGateEnabled\":false,\"windowsDisplayChanges\":0}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<" after "<<passed<<" checks\n";return 1;}}
