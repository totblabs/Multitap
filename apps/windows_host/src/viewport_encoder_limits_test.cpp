// Actual native SDK dispatcher and production viewport orchestration, with
// injected Windows CCD/capture observation. Does not alter installed displays.
#define main upstreamNativeDisplayServiceChangeMain
#include "native_display_service_change_test.cpp"
#undef main
#include "initial_viewport.hpp"
#include "native_stream_profile_capability.hpp"
int main(){using namespace host;int passed=0;try{
  auto command=JsonParser::parse(R"({"type":"session.start","sessionId":"00000000-0000-0000-0000-000000000001","mode":"stream","resizeProtocol":"epoch-v1","viewportEncoderLimitsProtocol":"separate-v1","monitor":"0","width":1280,"height":720,"fps":60,"epoch":0})");
  const auto limits=viewportEncoderLimits(command);require(limits&&limits->width==1280&&limits->height==720,"Quality ceilings not parsed");++passed;
  auto legacy=command;std::get<Json::Object>(legacy.value).erase("viewportEncoderLimitsProtocol");require(!viewportEncoderLimits(legacy),"Legacy command silently opted in");++passed;
  const auto capability=JsonParser::parse("{\"service\":true"+nativeStreamProfileCapabilityFields(true)+'}');require(capability.get("viewportEncoderLimitsProtocol").string()=="separate-v1"&&capability.get("initialViewportProtocol").string()=="epoch-v1"&&nativeStreamProfileCapabilityFields(false).empty(),"Incomplete feature advertisement");++passed;
  const auto narrowInfo=JsonParser::parse(R"({"width":106,"height":720,"sourceWidth":320,"sourceHeight":2160,"fpsTarget":60})");auto narrow=request();narrow.width=320;narrow.height=2160;require(limits->matches(narrowInfo,narrow),"Valid aspect-fitted encoding below128 pixels rejected");++passed;
  for(const auto& [key,value]:std::vector<std::pair<std::string,Json>>{
      {"viewportEncoderLimitsProtocol",JsonParser::parse("null")},{"viewportEncoderLimitsProtocol",JsonParser::parse("true")},{"viewportEncoderLimitsProtocol",JsonParser::parse("\"unknown\"")},
      {"mode",JsonParser::parse("\"rdp\"")},{"monitor",JsonParser::parse("\"1\"")},{"mediaProtocol",JsonParser::parse("\"tracks-v1\"")},{"tracks",JsonParser::parse("[]")},
      {"width",JsonParser::parse("1281")},{"height",JsonParser::parse("0")},{"height",JsonParser::parse("2162")},{"resizeProtocol",JsonParser::parse("\"unknown\"")}}){
    auto bad=command;std::get<Json::Object>(bad.value)[key]=value;bool refused=false;try{(void)viewportEncoderLimits(bad);}catch(...){refused=true;}require(refused,"Malformed negotiated limits accepted");++passed;
  }
  for(const bool separate:{false,true}){
    WorkerFixture fixture(true);auto current=fixture.current();streamField(current,"mode",std::string("stream"));streamField(current,"width",1280);streamField(current,"height",720);streamField(current,"fps",60);streamField(current,"bitrate",8000000);
    if(separate)streamField(current,"viewportEncoderLimitsProtocol",std::string("separate-v1"));
    for(const auto [width,height]:std::vector<std::pair<int,int>>{{960,540},{3840,2160},{720,1280},{1280,720}}){
      fixture.resize.width=width;fixture.resize.height=height;fixture.resize.epoch=uint16_t(current.get("epoch").number()+1);fixture.resize.requestId="quality-"+std::to_string(fixture.resize.epoch);fixture.resize.resizeId="00000000-0000-0000-0000-00000000000"+std::to_string(fixture.resize.epoch);
      StreamResizeBudget budget;const auto result=applyNativeWorkerViewport(current,fixture.resize,"0",fixture.access(budget),budget);
      require(result.ok&&result.display.ready(),"Production viewport transaction failed");
      require(result.command.get("captureWidth").number()==width&&result.command.get("captureHeight").number()==height,"Real Windows source was replaced by encoding geometry");
      require(result.command.get("width").number()==(separate?1280:width)&&result.command.get("height").number()==(separate?720:height),"Encoding ceilings escaped negotiated semantics");
      require(result.command.get("bitrate").number()==8000000&&result.command.get("fps").number()==60,"Viewport changed unrelated quality");fixture.transport.checkForeign();current=result.command;++passed;
      auto info=JsonParser::parse("{\"width\":"+std::to_string(std::min(width,1280))+",\"height\":"+std::to_string(std::min(height,720))+",\"sourceWidth\":"+std::to_string(width)+",\"sourceHeight\":"+std::to_string(height)+",\"fpsTarget\":60}");
      require(limits->matches(info,fixture.resize),"Bounded metadata rejected");++passed;
      for(const auto& [key,size]:std::vector<std::pair<const char*,int>>{{"width",1281},{"height",721},{"sourceWidth",width+2},{"sourceHeight",height+2},{"fpsTarget",30}}){auto invalid=info;streamField(invalid,key,size);require(!limits->matches(invalid,fixture.resize),"Incorrect encoding/source metadata passed service gate");++passed;}
    }
  }
  std::cout<<"{\"kind\":\"viewport-encoder-limits\",\"passed\":"<<passed<<",\"actualSdkDispatcher\":true,\"injectedCcd\":true,\"physicalDisplayChanged\":false,\"realCapture\":false,\"vm149\":false}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<" after "<<passed<<" checks\n";return 1;}}
