// Actual production RemoteInput and its INPUT structures. The injector is
// substituted: no OS input, cursor movement or installed display is changed.
#include "agent.hpp"
#include <iostream>
#include <type_traits>
using namespace host;
namespace {
unsigned checks=0;
void require(bool condition,const char* reason){++checks;if(!condition)throw std::runtime_error(reason);}
template<class T>void pointerField(Json& value,const char* name,T field){
 auto& target=std::get<Json::Object>(value.value)[name].value;
 if constexpr(std::is_same_v<T,bool>)target=field;
 else if constexpr(std::is_arithmetic_v<T>)target=double(field);
 else target=std::move(field);
}
RECT desktop(){const auto left=GetSystemMetrics(SM_XVIRTUALSCREEN),top=GetSystemMetrics(SM_YVIRTUALSCREEN);return{left,top,left+GetSystemMetrics(SM_CXVIRTUALSCREEN),top+GetSystemMetrics(SM_CYVIRTUALSCREEN)};}
Json mouse(const std::string& extra){return JsonParser::parse("{\"event\":\"button\",\"button\":\"left\",\"down\":true,\"x\":0.25,\"y\":0.75"+extra+'}');}
void rejectedMoveCannotClick(){
 unsigned moves=0,clicks=0;RemoteInput input([&](INPUT& value)->UINT{
  if(value.mi.dwFlags&MOUSEEVENTF_MOVE){++moves;return 0;}
  if(value.mi.dwFlags&MOUSEEVENTF_LEFTDOWN)++clicks;return 1;
 });input.area(desktop());input.apply(mouse(""));
 require(moves==1&&clicks==0&&input.pending()==0,"Failed absolute movement still pressed at the previous cursor position");
}
void positionedButtonIsOneInput(){
 std::vector<INPUT> calls;RemoteInput input([&](INPUT& value)->UINT{calls.push_back(value);return 1;});
 input.area(desktop());input.apply(mouse(""));
 require(calls.size()==1&&(calls[0].mi.dwFlags&MOUSEEVENTF_MOVE)&&(calls[0].mi.dwFlags&MOUSEEVENTF_LEFTDOWN),"Positioned click used independent movement and press injections");
 input.reset();require(input.pending()==0,"Atomic press was not released on reset");
}
void malformedPointerCannotMove(){
 unsigned calls=0;RemoteInput input([&](INPUT&)->UINT{++calls;return 1;});input.area(desktop());
 input.apply(JsonParser::parse(R"({"event":"wheel","x":0.5,"y":0.5,"delta":"bad"})"));
 require(calls==0,"Invalid wheel payload moved the cursor before validation");
 input.apply(JsonParser::parse(R"({"event":"button","button":"invalid","down":true,"x":0.5,"y":0.5})"));
 require(calls==0,"Invalid button moved the cursor before validation");
 input.apply(JsonParser::parse(R"({"event":"button","button":"left","down":true,"x":"bad","y":0.5})"));
 require(calls==0,"Invalid pointer coordinates still emitted a press");
}
void unboundAreaCannotUseConsole(){
 unsigned calls=0;RemoteInput input([&](INPUT&)->UINT{++calls;return 1;});
 input.apply(JsonParser::parse(R"({"event":"move","x":0.5,"y":0.5})"));
 require(calls==0,"Unbound streaming input fell back to the primary/console display");
}
void geometryAndActionCases(){
 const RECT layouts[]={{0,0,1920,1080},{-1920,-200,2560,1440},{-1200,-1600,1600,900},{-16384,-16384,16384,16384}};
 const std::pair<double,double> points[]={{0,0},{1,1},{.25,.75},{.999,.001}};
 for(const RECT bounds:layouts)for(const auto [x,y]:points)for(const auto kind:{"move","button","wheel"}){
  std::vector<INPUT> calls;unsigned queries=0;
  RemoteInput input([&](INPUT& value)->UINT{calls.push_back(value);return 1;},false,[&]{++queries;return bounds;});
  const RECT source{bounds.left+100,bounds.top+80,bounds.right-50,bounds.bottom-30};input.area(source);
  auto message=JsonParser::parse(std::string("{\"event\":\"")+kind+"\",\"x\":0,\"y\":0}");pointerField(message,"x",x);pointerField(message,"y",y);
  if(std::string(kind)=="button"){pointerField(message,"button",std::string("right"));pointerField(message,"down",true);}
  if(std::string(kind)=="wheel")pointerField(message,"delta",-240);
  require(input.apply(message)&&calls.size()==1&&queries==1,"Positioned action did not use one current-geometry INPUT");
  const auto& value=calls[0].mi;
  require((value.dwFlags&(MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK))==(MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK),"Absolute virtual desktop flags were lost");
  const double px=source.left+x*double(int64_t(source.right)-source.left-1),py=source.top+y*double(int64_t(source.bottom)-source.top-1);
  const double normalizedX=(px-bounds.left)*65535/double(int64_t(bounds.right)-bounds.left-1),normalizedY=(py-bounds.top)*65535/double(int64_t(bounds.bottom)-bounds.top-1);
  require(std::abs(value.dx-normalizedX)<1&&std::abs(value.dy-normalizedY)<1,"Negative-origin/unequal-size source coordinate mapping changed");
  if(std::string(kind)=="button")require((value.dwFlags&MOUSEEVENTF_RIGHTDOWN)&&input.pending()==1,"Positioned right press was not tracked");
  if(std::string(kind)=="wheel")require((value.dwFlags&MOUSEEVENTF_WHEEL)&&LONG(value.mouseData)==-240,"Positioned wheel lost signed delta");
  input.reset();require(input.pending()==0,"Positioned held action survived reset");
 }
}
void invalidGeometryAndTypes(){
 unsigned calls=0,queries=0;RECT bounds{0,0,1920,1080};
 RemoteInput input([&](INPUT&)->UINT{++calls;return 1;},false,[&]{++queries;return bounds;});input.area(bounds);
 for(int fault=0;fault<10;++fault){auto message=mouse("");
  switch(fault){
   case 0:std::get<Json::Object>(message.value).erase("x");break;
   case 1:std::get<Json::Object>(message.value)["x"].value=nullptr;break;
   case 2:pointerField(message,"x",-0.01);break;
   case 3:pointerField(message,"y",1.01);break;
   case 4:pointerField(message,"x",std::numeric_limits<double>::quiet_NaN());break;
   case 5:pointerField(message,"y",std::numeric_limits<double>::infinity());break;
   case 6:pointerField(message,"down",std::string("false"));break;
   case 7:pointerField(message,"button",std::string("unknown"));break;
   case 8:pointerField(message,"event",std::string("wheel"));pointerField(message,"delta",0.5);break;
   case 9:pointerField(message,"event",std::string("wheel"));pointerField(message,"delta",12001);break;
  }
  require(!input.apply(message)&&calls==0&&queries==0,"Invalid pointer fields reached geometry lookup or input injection");
 }
 for(const RECT source:{RECT{},RECT{0,0,1,10},RECT{-1,0,100,100},RECT{0,0,2000,100},RECT{0,-1,100,100},RECT{0,0,100,1200}}){
  input.area(source);require(!input.apply(mouse(""))&&calls==0,"Missing or stale owned source mapped into another display");
 }
 input.area({0,0,100,100});bounds={};require(!input.apply(mouse(""))&&calls==0,"Unavailable virtual desktop accepted positioned input");
 RemoteInput unavailable([&](INPUT&)->UINT{++calls;return 1;},false,[]()->RECT{throw std::runtime_error("fixture_bounds_unavailable");});
 unavailable.area({0,0,100,100});require(!unavailable.apply(mouse(""))&&calls==0,"Geometry observation exception escaped or injected input");
}
void releaseAfterTopologyLoss(){
 std::vector<INPUT> calls;RECT bounds{0,0,1920,1080};bool denyRelease=false;
 RemoteInput input([&](INPUT& value)->UINT{calls.push_back(value);return denyRelease&&(value.mi.dwFlags&MOUSEEVENTF_LEFTUP)?0U:1U;},false,[&]{return bounds;});input.area(bounds);
 require(input.apply(mouse(""))&&input.pending()==1,"Held pointer setup failed");
 auto release=mouse("");pointerField(release,"down",false);bounds={};
 require(!input.apply(release)&&calls.size()==1&&input.pending()==1,"Stale positional release was reported as applied");
 denyRelease=true;input.retry();require(input.pending()==1&&calls.size()==2&&!(calls.back().mi.dwFlags&MOUSEEVENTF_MOVE),"Failed release lost ownership or reused stale coordinates");
 denyRelease=false;input.retry();require(input.pending()==0&&calls.size()==3&&(calls.back().mi.dwFlags&MOUSEEVENTF_LEFTUP)&&!(calls.back().mi.dwFlags&MOUSEEVENTF_MOVE),"Button did not recover without moving the cursor");
 input.retry();require(calls.size()==3,"Recovered button retried input unnecessarily");
}
void bareLegacyAndFailureResults(){
 unsigned calls=0,queries=0;bool accepted=true;
 RemoteInput input([&](INPUT&)->UINT{++calls;return accepted?1U:0U;},false,[&]{++queries;return RECT{};});
 require(input.apply(JsonParser::parse(R"({"event":"button","button":"middle","down":true})"))&&input.pending()==1&&queries==0,"Bare legacy button lost current-cursor behavior");
 input.reset();require(input.pending()==0&&calls==2,"Bare legacy middle release failed");
 accepted=false;require(!input.apply(JsonParser::parse(R"({"event":"wheel","delta":120})"))&&calls==3&&queries==0,"Rejected OS injection was reported as applied");
 require(!input.apply(JsonParser::parse(R"({"event":"move"})"))&&calls==3,"Bare move became an unintended current-cursor event");
}
void sideButtons(){
 const std::pair<const char*,DWORD> sides[]={{"x1",XBUTTON1},{"x2",XBUTTON2}};
 for(const auto [name,number]:sides)for(const std::string position:{"",",\"x\":0.5,\"y\":0.5"}){
  std::vector<INPUT> calls;RemoteInput input([&](INPUT& value)->UINT{calls.push_back(value);return 1;},false,[]{return desktop();});input.area(desktop());
  const auto change=[&](bool down){return JsonParser::parse(std::string("{\"event\":\"button\",\"button\":\"")+name+"\",\"down\":"+(down?"true":"false")+position+'}');};
  const DWORD moved=position.empty()?0:DWORD(MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK);
  require(input.apply(change(true))&&input.pending()==1&&input.buttonHeld(),"A side button press was refused or not tracked");
  require(calls.back().mi.dwFlags==(MOUSEEVENTF_XDOWN|moved)&&calls.back().mi.mouseData==number,"A side button press named the wrong button");
  require(input.apply(change(false))&&input.pending()==0,"A side button release was refused or left it held");
  require(calls.back().mi.dwFlags==(MOUSEEVENTF_XUP|moved)&&calls.back().mi.mouseData==number,"A side button release named the wrong button");
  // Held: focus loss, disconnect and a failed release all use the same path.
  require(input.apply(change(true)),"A side button did not press again");
  calls.clear();input.reset();
  require(calls.size()==1&&calls[0].type==INPUT_MOUSE&&calls[0].mi.dwFlags==MOUSEEVENTF_XUP&&calls[0].mi.mouseData==number&&input.pending()==0,"A held side button was not released as itself");
 }
 std::vector<INPUT> calls;bool deny=false;RemoteInput input([&](INPUT& value)->UINT{calls.push_back(value);return deny?0U:1U;});
 require(input.apply(JsonParser::parse(R"({"event":"button","button":"x2","down":true})")),"Bare side press failed");
 deny=true;require(!input.apply(JsonParser::parse(R"({"event":"button","button":"x2","down":false})"))&&input.pending()==1,"A failed side release lost the held button");
 input.retry();require(input.pending()==1&&calls.back().mi.dwFlags==MOUSEEVENTF_XUP&&calls.back().mi.mouseData==XBUTTON2,"A side release retry named the wrong button");
 deny=false;input.retry();require(input.pending()==0&&calls.back().mi.dwFlags==MOUSEEVENTF_XUP&&calls.back().mi.mouseData==XBUTTON2,"A side release retry did not finish");
 // Together with the others: each is released as itself.
 calls.clear();
 for(const auto* name:{"left","right","middle","x1","x2"})require(input.apply(JsonParser::parse(std::string("{\"event\":\"button\",\"button\":\"")+name+"\",\"down\":true}")),"A press of a held set failed");
 calls.clear();input.reset();
 std::vector<std::pair<DWORD,DWORD>> released;for(const auto& call:calls)released.emplace_back(call.mi.dwFlags,call.mi.mouseData);std::sort(released.begin(),released.end());
 const std::vector<std::pair<DWORD,DWORD>> expected{{MOUSEEVENTF_LEFTUP,0},{MOUSEEVENTF_RIGHTUP,0},{MOUSEEVENTF_MIDDLEUP,0},{MOUSEEVENTF_XUP,XBUTTON1},{MOUSEEVENTF_XUP,XBUTTON2}};
 require(released==expected&&input.pending()==0,"A held set was not released button by button");
 // The three others carry no button number, as before.
 calls.clear();
 for(const auto [name,flag]:{std::pair<const char*,DWORD>{"left",MOUSEEVENTF_LEFTDOWN},{"right",MOUSEEVENTF_RIGHTDOWN},{"middle",MOUSEEVENTF_MIDDLEDOWN}}){
  require(input.apply(JsonParser::parse(std::string("{\"event\":\"button\",\"button\":\"")+name+"\",\"down\":true}"))&&calls.back().mi.dwFlags==flag&&calls.back().mi.mouseData==0,"An ordinary button changed");
 }
 input.reset();
 const auto refused=calls.size();
 for(const auto* bad:{R"({"event":"button","button":"x3","down":true})",R"({"event":"button","button":"X1","down":true})",R"({"event":"button","button":"x1","down":"yes"})"})
  require(!input.apply(JsonParser::parse(bad))&&calls.size()==refused,"An unknown button reached the injector");
}
}
int main(){
 const struct{const char* name;void(*run)();}cases[]={{"rejected-move",rejectedMoveCannotClick},{"single-input",positionedButtonIsOneInput},{"malformed-pointer",malformedPointerCannotMove},{"unbound-area",unboundAreaCannotUseConsole},{"geometry-actions",geometryAndActionCases},{"invalid-types-geometry",invalidGeometryAndTypes},{"topology-loss-release",releaseAfterTopologyLoss},{"legacy-results",bareLegacyAndFailureResults},{"side-buttons",sideButtons}};
 unsigned passed=0,failed=0;
 for(const auto& value:cases)try{value.run();++passed;}catch(const std::exception& error){++failed;std::cerr<<value.name<<": "<<error.what()<<'\n';}
 std::cout<<"{\"kind\":\"positioned-pointer-reliability\",\"passed\":"<<passed<<",\"failed\":"<<failed<<",\"checks\":"<<checks<<",\"productionRemoteInput\":true,\"injector\":\"substituted\",\"osInputEmitted\":false}\n";return failed?1:0;
}
