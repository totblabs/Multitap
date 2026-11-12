// Actual production parsers/coordinator over memory-only CCD/SDK snapshots.
// No capture/encoder/driver/registry/window/network API is executed.
#include "stream_resize.hpp"
#include "track_source.hpp"
#include "native_track_change.hpp"
#include "cursor_channel.hpp"
#include "native_display_baseline_codec.hpp"
#include "native_stream_profile_payload.hpp"
#include "stream_prepare_diagnostics.hpp"
#include "stream_memory.hpp"
#include "../tests/display-v2-world.hpp"
#include "../../../drivers/display_bridge/lease_recovery.hpp"
#include <iostream>

namespace {
using namespace host;
const std::string session="00000000-0000-0000-0000-000000000001";
Json parse(const std::string& value){return JsonParser::parse(value);}
std::string fixtureJson(const Json& value){
  if(const auto* array=std::get_if<Json::Array>(&value.value)){std::string out="[";for(const auto& child:*array){if(out.size()>1)out+=',';out+=fixtureJson(child);}return out+']';}
  if(const auto* object=std::get_if<Json::Object>(&value.value)){std::string out="{";for(const auto& [key,child]:*object){if(out.size()>1)out+=',';out+=jsonQuote(key)+':'+fixtureJson(child);}return out+'}';}
  if(const auto* text=std::get_if<std::string>(&value.value))return jsonQuote(*text);
  if(const auto* number=std::get_if<double>(&value.value)){std::ostringstream out;out<<std::setprecision(16)<<*number;return out.str();}
  if(const auto* flag=std::get_if<bool>(&value.value))return *flag?"true":"false";
  return "null";
}
std::string limits(int width,int height,uint64_t pixels,uint64_t rate=0){return "{\"maxWidth\":"+std::to_string(width)+",\"maxHeight\":"+std::to_string(height)+",\"maxPixels\":"+std::to_string(pixels)+",\"maxPixelRate\":"+std::to_string(rate)+'}';}
Json start(const std::string& protocol="stream-size-v2"){
  return parse("{\"type\":\"session.start\",\"sessionId\":\""+session+"\",\"mode\":\"stream\",\"resizeProtocol\":\"epoch-v1\",\"viewportEncoderLimitsProtocol\":\"separate-v1\",\"streamSizeProtocol\":\""+protocol+"\",\"decoderLimits\":"+limits(1280,720,1280*720)+",\"monitor\":\"0\",\"width\":1280,\"height\":720,\"fps\":60,\"epoch\":1}");
}
Json resize(){return parse("{\"type\":\"stream.resize\",\"sessionId\":\""+session+"\",\"requestId\":\"r2\",\"resizeId\":\"00000000-0000-0000-0000-000000000002\",\"monitor\":\"0\",\"width\":5120,\"height\":1440,\"fps\":60,\"epoch\":2,\"decoderLimits\":"+limits(5120,1440,5120ull*1440)+'}');}
template<class Action> bool rejects(Action action){try{action();return false;}catch(...){return true;}}
template<class Action> bool succeeds(Action action){try{return action();}catch(...){return false;}}
NativeRecoveryBaseline baselineFixture(DWORD width=1024,DWORD height=768){
  v2world::World world;world.console().width=width;world.console().height=height;world.owned().x=LONG(width);
  auto ccd=world.query();auto& foreign=ccd.paths.front();
  DISPLAYCONFIG_MODE_INFO image{};image.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE;image.adapterId=foreign.sourceInfo.adapterId;image.id=foreign.sourceInfo.id;
  image.desktopImageInfo.PathSourceSize={LONG(width),LONG(height)};image.desktopImageInfo.DesktopImageRegion=image.desktopImageInfo.DesktopImageClip={0,0,LONG(width),LONG(height)};
  foreign.targetInfo.desktopModeInfoIdx=UINT32(ccd.modes.size());ccd.modes.push_back(image);
  return {"ROOT\\DISPLAY\\00a1",88,captureNativeTopologyBaseline(ccd,world.targets(),"0"),{}};
}
}
int main(){
  unsigned checks=0,failures=0;
  const auto require=[&](bool value,const char* label){++checks;if(!value){++failures;std::cout<<"FAIL "<<label<<'\n';}};
  try{
  for(const auto& [width,height]:std::array<std::pair<uint32_t,uint32_t>,5>{{{3840,2160},{5120,1440},{5120,2880},{4320,7680},{3840,3840}}})require(ownedOutputSize(width,height),"geometry has no product4K ceiling");
  {
    const StreamMemoryShape one{3840,2160,1920,1080,true};
    const auto single=streamMemoryForecast({&one,1});
    const auto source=checkedFrameLayout(one.sourceWidth,one.sourceHeight,false),encoded=checkedFrameLayout(one.width,one.height);
    require(single&&*single==uint64_t(source->bgraBytes)*5+uint64_t(encoded->bgraBytes)*4+uint64_t(encoded->nv12Bytes)*16,"forecast includes simultaneous source, encoded, codec, policy and eligible lossless workspaces");
    const std::array<StreamMemoryShape,2> pair{one,one};const auto dual=streamMemoryForecast(pair);
    require(single&&dual&&*dual==*single*2,"two tracks aggregate before owner creation");
    const StreamMemoryShape largerSource{7680,4320,1920,1080,true};const auto large=streamMemoryForecast({&largerSource,1});
    require(single&&large&&*large>*single,"small encoded picture cannot hide a larger source forecast");
    auto noTiles=one;noTiles.losslessSource=false;const auto withoutTiles=streamMemoryForecast({&noTiles,1});
    require(single&&withoutTiles&&*single-*withoutTiles==source->bgraBytes,"lossless forecast is source-sized and explicit");
    require(!streamMemoryForecast({}),"empty output plan has no admitted forecast");
    const std::array<StreamMemoryShape,3> tooMany{one,one,one};require(!streamMemoryForecast(tooMany),"existing maximum two-output contract remains bounded");
    auto invalid=one;invalid.sourceWidth=0;require(!streamMemoryForecast({&invalid,1}),"invalid source geometry fails before availability or allocation");
    invalid=one;invalid.width=1919;require(!streamMemoryForecast({&invalid,1}),"encoded NV12 alignment remains required");
    invalid=one;invalid.width=7680;require(!streamMemoryForecast({&invalid,1}),"encoded picture cannot exceed its source");
    const StreamMemoryShape overflowing{ownedOutputMaxSide,uint32_t(INT32_MAX),16,16,true};
    require(!streamMemoryForecast({&overflowing,1}),"working-set multiplication overflow is rejected without allocating");
    const StreamMemoryShape individuallyLarge{ownedOutputMaxSide,1000000000u,16,16,true};const auto individual=streamMemoryForecast({&individuallyLarge,1});
    require(individual.has_value(),"individually representable forecast remains integer-only");
    const std::array<StreamMemoryShape,2> sumOverflow{individuallyLarge,individuallyLarge};require(!streamMemoryForecast(sumOverflow),"two valid individual forecasts cannot overflow aggregate admission");
    require(streamMemoryAdmission(single,single)==StreamMemoryAdmission::accepted,"single exact available budget is accepted");
    require(streamMemoryAdmission(dual,dual)==StreamMemoryAdmission::accepted,"dual exact aggregate available budget is accepted");
    require(streamMemoryAdmission(single,single)==StreamMemoryAdmission::accepted&&streamMemoryAdmission(dual,single)==StreamMemoryAdmission::insufficient,"each output fitting alone does not admit an insufficient dual sum");
    require(streamMemoryAdmission(single,{})==StreamMemoryAdmission::unavailable,"unknown system memory is an honest refusal");
    require(streamMemoryAdmission(single,uint64_t(0))==StreamMemoryAdmission::insufficient,"zero available memory is insufficient");
    require(streamMemoryAdmission({},UINT64_MAX)==StreamMemoryAdmission::invalidLayout,"invalid forecast cannot be admitted by a large available number");
    require(streamMemoryFailure(StreamMemoryAdmission::accepted)==nullptr&&std::string_view(streamMemoryFailure(StreamMemoryAdmission::unavailable))=="frame_memory_unavailable"&&std::string_view(streamMemoryFailure(StreamMemoryAdmission::insufficient))=="frame_memory_insufficient","admission exposes only fixed resource failure codes");
    const StreamMemoryShape growth{3840,2160,2560,1440,true},equal{3840,2160,1080,1920,true},shrink{3840,2160,1280,720,true};
    require(streamMemoryGrowthRequiresAdmission(one,growth)==std::optional<bool>(true),"larger checked forecast requires a fresh pre-teardown aggregate admission");
    require(streamMemoryGrowthRequiresAdmission(one,equal)==std::optional<bool>(false),"equal checked demand needs no guessed live-owner budget credit");
    require(streamMemoryGrowthRequiresAdmission(one,shrink)==std::optional<bool>(false),"shrinking checked forecast remains available for adaptive downscale under pressure");
    require(!streamMemoryGrowthRequiresAdmission(one,invalid).has_value(),"invalid candidate cannot bypass admission as a shrink");
    require(!streamMemoryGrowthRequiresAdmission(one,largerSource).has_value(),"a different source cannot inherit the adaptive single-picture exemption");
  }
  {
    require(displayRectangleFits(INT32_MIN,0,UINT32_MAX,1),"DWORD source span may cover both signed LONG endpoints");
    require(!displayRectangleFits(INT32_MAX,0,1,1)&&!displayRectangleFits(0,INT32_MAX,1,1)&&!displayRectangleFits(INT64_MAX,0,UINT64_MAX,1),"rectangle endpoint overflow is rejected before addition");
    const auto baselineSmall=baselineFixture();const auto oldBytes=nativeRecoveryBaselineBytes(baselineSmall);
    require(nativeRecoveryBaselineBytes(parseNativeRecoveryBaseline(oldBytes))==oldBytes,"existing canonical journal encoding remains byte stable");
    const auto large=baselineFixture(70000,70000);
    require(succeeds([&]{const auto bytes=nativeRecoveryBaselineBytes(large);const auto decoded=parseNativeRecoveryBaseline(bytes);return nativeActiveSignatures(decoded.topology.ccd)==nativeActiveSignatures(large.topology.ccd)&&decoded.topology.placements[0].left==70000&&nativeRecoveryBaselineBytes(decoded)==bytes;}),"large foreign source and owned placement survive the actual baseline codec and planner");
    require(succeeds([&]{const auto profile=planNativeStreamPrimaryProfile(large.topology.ccd,large.topology.targets);const auto decoded=parseNativeStreamProfilePayload(nativeStreamProfilePayloadBytes(profile));return nativeStreamProfileSignatures(decoded.before)==nativeStreamProfileSignatures(profile.before);}),"large foreign source survives primary-profile translation and its separate payload codec");
    const auto edited=[&](size_t field,const char* text){auto value=parse(oldBytes);auto& paths=std::get<Json::Array>(std::get<Json::Object>(value.value)["paths"].value);std::get<Json::Array>(paths[0].value)[field].value=std::string(text);return fixtureJson(value);};
    for(const auto field:{size_t(24),size_t(25),size_t(26),size_t(27)})
      require(succeeds([&]{const auto decoded=parseNativeRecoveryBaseline(edited(field,"4294967295"));return nativeBaselinePathRow(decoded.topology.ccd,decoded.topology.ccd.paths[0])[field]==UINT32_MAX;}),"signal active/total sizes retain UINT32 fields independently of source geometry");
    for(const auto field:{size_t(34),size_t(35)})
      require(succeeds([&]{const auto decoded=parseNativeRecoveryBaseline(edited(field,"2147483647"));return nativeBaselinePathRow(decoded.topology.ccd,decoded.topology.ccd.paths[0])[field]==INT32_MAX;}),"desktop image PathSourceSize retains positive LONG fields");
    require(succeeds([&]{const auto decoded=parseNativeRecoveryBaseline(edited(46,"65535"));return decoded.topology.ccd.paths[0].sourceInfo.cloneGroupId==65535;}),"actual16bit clone field still accepts its documented sentinel");
    for(const auto& [field,text]:std::array<std::pair<size_t,const char*>,9>{{{14,"4294967296"},{15,"4294967296"},{24,"4294967296"},{27,"4294967296"},{34,"2147483648"},{35,"0"},{46,"65536"},{17,"2147483647"},{14,"0"}}})
      require(rejects([&]{(void)parseNativeRecoveryBaseline(edited(field,text));}),"journal refuses field ABI overflow, zero geometry, endpoint overflow and16bit overflow");
  }
  {
    DisplayState display;display.installed=display.deviceReady=display.active=display.owned=true;display.foreign=0;display.width=1280;display.height=720;display.name=L"\\\\.\\DISPLAY1";display.monitors={{"0",display.name,70000,-70000,1280,720,false}};
    const std::vector<CaptureMonitor> observed{{display.name,L"synthetic-owned",{70000,-70000,71280,-69280},false}};
    auto command=start();const auto selected=attachFreshCapture(command,display,observed);const auto parsed=parseCaptureSelection(command,selected.bounds);validateCaptureSelection(parsed,observed);
    require(sameRect(selected.bounds,observed.front().bounds)&&sameRect(parsed.monitors[0].bounds,observed.front().bounds),"capture selection preserves large signed positions without observing Windows");
    auto overflow=display;overflow.monitors[0].left=INT32_MAX-10;
    require(rejects([&]{(void)selectOwnedCapture(overflow,"0",observed);}),"capture selection rejects overflow before narrowing endpoints");
    auto track=parse("{\"sessionId\":\""+session+"\",\"requestId\":\"bounds\",\"resizeId\":\"00000000-0000-0000-0000-000000000002\",\"topologyEpoch\":2,\"fps\":60,\"tracks\":[{\"id\":\"0\",\"width\":1280,\"height\":720,\"x\":70000,\"y\":-70000,\"epoch\":2}]}");
    require(nativeTrackResizeRequest(track).outputs[0].left==70000,"native track request retains signed LONG placement");
    auto& output=std::get<Json::Array>(std::get<Json::Object>(track.value)["tracks"].value)[0];streamField(output,"x",INT32_MAX-10);
    require(rejects([&]{(void)nativeTrackResizeRequest(track);}),"native track endpoint overflow remains rejected");
  }
  {
    for(const auto* code:{"frame_layout_unsupported","frame_memory_unavailable","frame_memory_insufficient","encoder_format_unsupported"}){
      require(std::string_view(streamCapturePublicFailure(code,"capture_failed",true))==code,"v2 exposes only fixed exact failure reason");
      require(std::string_view(streamCapturePublicFailure(code,"capture_failed",false))=="capture_failed","legacy failure category remains unchanged");
    }
    require(std::string_view(streamCapturePublicFailure("private arbitrary exception text","encoder_failed",true))=="encoder_failed","arbitrary native exception text never becomes a public reason");
    require(streamInitialSdkUnsupported(true,true,true,4,ERROR_NOT_SUPPORTED),"current negotiated v2 SDK unsupported failure has a fixed public category");
    require(!streamInitialSdkUnsupported(false,true,true,4,ERROR_NOT_SUPPORTED),"legacy SDK failure retains its previous category");
    require(!streamInitialSdkUnsupported(true,false,true,4,ERROR_NOT_SUPPORTED),"uncorrelated or retired reply cannot select the SDK reason");
    require(!streamInitialSdkUnsupported(true,true,false,4,ERROR_NOT_SUPPORTED),"successful or untyped reply cannot select the SDK reason");
    require(!streamInitialSdkUnsupported(true,true,true,5,ERROR_NOT_SUPPORTED),"non-SDK stage is not inferred to be unsupported mode");
    require(!streamInitialSdkUnsupported(true,true,true,4,ERROR_ACCESS_DENIED),"other SDK errors are not inferred to be unsupported mode");
  }
  {
    DisplayState display;display.installed=display.deviceReady=display.active=display.owned=true;display.foreign=0;display.width=32768;display.height=180;display.name=L"\\\\.\\DISPLAY1";display.monitors={{"0",display.name,0,0,32768,180,true}};
    require(display.ready(),"readiness follows representable source rather than16K product limit");
    display.width=UINT32_MAX;require(!display.ready(),"readiness still rejects signed stride overflow");
    const auto value=parse("{\"installed\":true,\"deviceReady\":true,\"activeDisplay\":true,\"owned\":true,\"foreignDeviceCount\":0,\"width\":32768,\"height\":180,\"displayDeviceName\":\"\\\\\\\\.\\\\DISPLAY1\",\"displays\":[{\"id\":\"0\",\"virtual\":true,\"displayDeviceName\":\"\\\\\\\\.\\\\DISPLAY1\",\"width\":32768,\"height\":180}]}");
    const auto decoded=parseDisplayState(value);require(decoded.ready()&&decoded.monitors.size()==1&&decoded.monitors[0].width==32768,"helper source dimensions above16K survive parsing");
    auto invalid=value;streamField(invalid,"width",double(UINT32_MAX)+1);require(!parseDisplayState(invalid).ready(),"helper dimension overflow does not wrap to readiness");
  }
  {
    const cursor::Channel::Extract extractor=[](HCURSOR)->std::optional<cursor::Shape>{return {};};
    cursor::Channel wide({0,0,32768,180},[]{return 1u;},extractor);
    require(!wide.ready(),"cursor source above16K admitted without cursor OS calls");
    require(rejects([&]{cursor::Channel bad({LONG_MIN,0,LONG_MAX,180},[]{return 1u;},extractor);}),"cursor source subtracts endpoints without signed overflow");
    require(rejects([&]{cursor::Channel bad({0,0,0,180},[]{return 1u;},extractor);}),"cursor source still rejects empty bounds");
  }
  auto old=start(),request=resize();
  const auto candidate=resizeDecoderCommand(old,request);
  require(candidate.get("width").number()==5120&&candidate.get("decoderLimits").get("maxWidth").number()==5120,"later viewport refreshes old small encoder box");
  require(old.get("width").number()==1280&&old.get("decoderLimits").get("maxWidth").number()==1280,"candidate does not commit old command");
  auto prepared=old;streamField(prepared,"epoch",2);streamField(prepared,"requestId",request.get("requestId").string());streamField(prepared,"resizeId",request.get("resizeId").string());const auto committed=preparedResizeDecoderCommand(old,prepared,request);
  require(committed.get("width").number()==5120&&prepared.get("width").number()==1280,"only prepared successful copy carries new decoder limits");
  auto foreign=request;streamField(foreign,"sessionId",std::string("00000000-0000-0000-0000-000000000009"));
  require(rejects([&]{(void)resizeDecoderCommand(old,foreign);}),"foreign session cannot replace limits");
  auto stale=request;streamField(stale,"epoch",1);
  require(rejects([&]{(void)resizeDecoderCommand(old,stale);}),"stale epoch cannot replace limits");
  require(rejects([&]{(void)resizeDecoderCommand(start("stream-size-v1"),request);}),"v1 optional decoder field refused");
  auto malformed=request;std::get<Json::Object>(malformed.value)["decoderLimits"]=parse("{\"maxWidth\":5120,\"maxHeight\":1440,\"maxPixels\":7372800,\"maxPixelRate\":0,\"extra\":1}");
  require(rejects([&]{(void)resizeDecoderCommand(old,malformed);}),"resize requires exact four limits");
  require(streamDecoderLimits(parse(limits(16,16,256,256)),true).maxHeight==16,"v2 encoded minima independent of workspace minima");
  require(rejects([&]{(void)streamDecoderLimits(parse(limits(16,16,256,256)),false);}),"v1 encoded minima unchanged");
  {
    auto tinyBox=start();streamField(tinyBox,"width",1920);streamField(tinyBox,"height",64);
    std::get<Json::Object>(tinyBox.value)["decoderLimits"]=parse(limits(1920,64,1920*64));
    require(viewportEncoderLimits(tinyBox)->height==64,"v2 encoded box below128 independent of source geometry");
    streamField(tinyBox,"streamSizeProtocol",std::string("stream-size-v1"));
    require(rejects([&]{(void)viewportEncoderLimits(tinyBox);}),"v1 encoder box minimum128 unchanged");
  }
  const auto decoder=streamDecoderLimits(parse(limits(7680,4320,7680ull*4320,7680ull*4320*60)),true);
  const auto picture=EncodeScaleController(StreamSizeBounds{5120,2880,60,7680,4320,decoder,{}}).size();
  require(picture==std::pair{5120,2880},"capable decoder permits above4K picture");
  const auto limited=EncodeScaleController(StreamSizeBounds{5120,2880,60,7680,4320,DecoderLimits{1920,1080,1920ull*1080,1920ull*1080*60},{}}).size();
  require(limited.first<=1920&&limited.second<=1080,"decoder only limits encoded picture");
  {
    auto command=parse("{\"type\":\"session.start\",\"sessionId\":\""+session+"\",\"mode\":\"stream\",\"mediaProtocol\":\"tracks-v1\",\"streamSizeProtocol\":\"stream-size-v2\",\"decoderLimits\":"+limits(1920,1080,1920*1088,1920ull*1088*60)+",\"topologyEpoch\":1,\"fps\":60,\"bitrate\":8000000,\"audio\":false,\"monitor\":\"all\",\"maxWidth\":1920,\"maxHeight\":1080,\"tracks\":[{\"id\":\"0\",\"width\":3840,\"height\":2160,\"x\":0,\"y\":0,\"epoch\":1},{\"id\":\"1\",\"width\":3840,\"height\":2160,\"x\":3840,\"y\":0,\"epoch\":1}]}");
    DisplayState display;display.installed=display.deviceReady=display.active=display.owned=true;display.foreign=0;display.width=7680;display.height=2160;display.name=L"\\\\.\\DISPLAY1";
    display.monitors={{"0",L"\\\\.\\DISPLAY1",0,0,3840,2160,true},{"1",L"\\\\.\\DISPLAY2",3840,0,3840,2160,false}};
    const std::vector<CaptureMonitor> monitors{{L"\\\\.\\DISPLAY1",L"owned-0",{0,0,3840,2160},true},{L"\\\\.\\DISPLAY2",L"owned-1",{3840,0,7680,2160},false}};
    const auto plan=bindOwnedTrackSources(command,display,monitors);
    require(plan.outputs.size()==2&&plan.outputs[0].media.sourceWidth==3840&&plan.outputs[1].media.sourceWidth==3840,"dual2x4K source not reduced to pixel-sum budget");
    require(plan.outputs[0].media.width==1920&&plan.outputs[0].media.height==1080&&plan.outputs[1].media.width==1920,"dual encoded pictures respect decoder independently");
    const auto metadata=parse(trackSourceInfo(plan,false));const auto physical=nativeInitialTrackViewport(command);
    require(nativeTrackMetadataMayComplete(metadata,physical,display,true,true,true),"v2 scaled metadata keeps exact physical ownership");
    require(!nativeTrackMetadataMayComplete(metadata,physical,display,true,true,false),"legacy metadata still requires exact source picture");
  }
  {
    v2world::World world;v2world::Memory memory;auto access=v2world::worldAccess(world,memory);
    access.modeSupported=multitap::vdisplay::ownedOutputModeSupported;
    DisplaySessionV2 display(access);const auto unsupported=display.start(session,4320,7680,true,v2world::deadline(world));
    require(unsupported.modeUnsupported&&unsupported.reason=="unsupported_display_mode"&&unsupported.error==ERROR_NOT_SUPPORTED,"actual provider ABI unsupported reason");
    require(!memory.bytes&&world.resizes==0&&world.applies==0,"unsupported provider mode rejected before first side effect");
    const auto started=display.start(session,5120,1440,true,v2world::deadline(world));
    require(started.ok&&world.owned().width==5120&&world.owned().height==1440,"production coordinator changes workspace not video zoom");
    const auto baseline=memory.bytes;const auto resizes=world.resizes,applies=world.applies;
    const auto refused=display.resize(4320,7680,v2world::deadline(world));
    require(refused.modeUnsupported&&world.resizes==resizes&&world.applies==applies&&memory.bytes==baseline&&world.owned().width==5120,"unsupported resize leaves current mode and original recovery state unchanged");
    require(display.restore().ok(),"large workspace can restore saved topology in fake coordinator");
    v2world::requireOriginal(world,false,"large workspace restoration mismatch");
  }
  }catch(...){std::cout<<"SETUP_UNEXPECTED_EXCEPTION after "<<checks<<" checks; actualWindows=false\n";return 2;}
  std::cout<<checks<<" checks, "<<failures<<" failures; actualWindows=false; actualCapture=false; actualCodec=false\n";
  return failures?1:0;
}
