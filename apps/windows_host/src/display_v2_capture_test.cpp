#include "display_v2_capture.hpp"
#include <iostream>
using namespace host;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
DisplayState state(bool dual){
  DisplayState s;s.installed=s.deviceReady=s.active=s.owned=true;s.foreign=0;s.name=L"\\\\.\\DISPLAY1";s.width=dual?3200:1280;s.height=1080;
  s.monitors={{"0",s.name,0,0,1280,800,true}};
  if(dual)s.monitors.push_back({"1",L"\\\\.\\DISPLAY2",1280,0,1920,1080,false});return s;
}
std::vector<CaptureMonitor> observed(){return {{L"\\\\.\\DISPLAY1",L"owned-zero",{0,0,1280,800},true},{L"\\\\.\\DISPLAY2",L"owned-one",{1280,0,3200,1080},false},{L"\\\\.\\DISPLAY3",L"foreign",{3200,0,5760,1600},false}};}
Json command(){return JsonParser::parse(R"({"monitor":"0","bitrate":12000000,"sessionId":"fixture","clipboard":false})");}
}
int main(){try{unsigned passed=0;
  for(bool dual:{false,true}){auto display=state(dual);auto current=command();require(bindDisplayV2SingleCapture(current,display,1280,800,observed()),"Owned zero capture refused for one/two outputs");
    const auto selection=parseCaptureSelection(current,{0,0,1280,800});validateCaptureSelection(selection,observed());
    require(selection.monitors.size()==1&&selection.monitors.front().identity==L"owned-zero"&&current.get("bitrate").number()==12000000&&display.monitors.size()==(dual?2u:1u),"Single capture changed another output or quality");++passed;}
  {auto display=state(true);std::reverse(display.monitors.begin(),display.monitors.end());auto current=command();require(bindDisplayV2SingleCapture(current,display,1280,800,observed()),"Owned output order selected the wrong capture");++passed;}
  for(unsigned scenario=0;scenario<14;++scenario){auto display=state(true);auto actual=observed();auto current=command();DWORD width=1280,height=800;
    switch(scenario){
      case 0:display.active=false;break;case 1:display.configurationPending=true;break;case 2:display.remoteSession=true;break;
      case 3:display.monitors[0].primary=false;break;case 4:width=1920;break;case 5:height=1080;break;
      case 6:display.monitors[0].id="missing";break;case 7:display.monitors[1].id="0";break;
      case 8:actual[0].identity.clear();break;case 9:actual[0].bounds.right=1279;break;
      case 10:actual[2].bounds={0,0,1280,800};break;case 11:streamField(current,"monitor",std::string("all"));break;
      case 12:streamField(current,"monitor",std::string("1"));break;case 13:display.monitors.push_back(display.monitors[0]);break;
    }
    const auto beforeMonitor=current.get("monitor").string();const auto fields=std::get<Json::Object>(current.value).size();
    require(!bindDisplayV2SingleCapture(current,display,width,height,actual)&&current.get("monitor").string()==beforeMonitor&&current.get("bitrate").number()==12000000&&current.get("sessionId").string()=="fixture"&&std::get<Json::Object>(current.value).size()==fields,"Unproven/foreign capture was bound or command changed on failure");++passed;
  }
  std::cout<<"{\"passed\":"<<passed<<",\"dualOwnedSingleCapture\":true,\"ownedOneRetained\":true,\"windowsQueries\":0,\"captureStarted\":false,\"displayChanges\":0}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
