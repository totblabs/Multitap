#pragma once
#include "native_display_change.hpp"
#include "native_track_topology.hpp"
#include "../../../native/windows_video/track_media.hpp"
namespace host {
struct NativeTrackResize {
  std::string sessionId,requestId,resizeId;
  uint32_t topologyEpoch=0;
  int fps=0;
  std::vector<NativeTrackViewport> outputs;
  std::vector<uint32_t> epochs;
};
inline bool legacyTrackRequestSize(const NativeTrackResize& request){
  uint64_t pixels=0;
  for(const auto& output:request.outputs){if(!legacyStreamOutputSize(output.width,output.height)||output.height>2160)return false;pixels+=uint64_t(output.width)*output.height;}
  return pixels<=8294400;
}
inline NativeTrackResize nativeTrackResizeRequest(const Json& value){
  const auto fail=[] {throw std::runtime_error("invalid_track_request");};
  const auto integer=[&](const Json& field,int64_t minimum,int64_t maximum){const auto number=field.number(double(minimum)-1);if(!std::isfinite(number)||number!=std::floor(number)||number<double(minimum)||number>double(maximum))fail();return int64_t(number);};
  NativeTrackResize result;result.sessionId=value.get("sessionId").string();result.requestId=value.get("requestId").string();result.resizeId=value.get("resizeId").string();
  if(!displayOperationUuid(result.sessionId)||!displayOperationUuid(result.resizeId)||result.requestId.empty()||result.requestId.size()>64||result.requestId.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos)fail();
  result.topologyEpoch=uint32_t(integer(value.get("topologyEpoch"),1,UINT32_MAX));result.fps=int(integer(value.get("fps"),30,60));if(result.fps!=30&&result.fps!=60)fail();
  const auto* rows=std::get_if<Json::Array>(&value.get("tracks").value);if(!rows||rows->empty()||rows->size()>2)fail();
  multitap::tracks::Configuration media;media.topology=result.topologyEpoch;
  for(const auto& row:*rows){
    const auto* fields=std::get_if<Json::Object>(&row.value);if(!fields||fields->size()!=6)fail();for(const auto* key:{"id","width","height","x","y","epoch"})if(!fields->contains(key))fail();
    NativeTrackViewport output;output.id=row.get("id").string();if(output.id!="0"&&output.id!="1")fail();
    output.width=uint32_t(integer(row.get("width"),320,ownedOutputMaxSide));output.height=uint32_t(integer(row.get("height"),180,ownedOutputMaxSide));
    output.left=LONG(integer(row.get("x"),INT32_MIN,INT32_MAX));output.top=LONG(integer(row.get("y"),INT32_MIN,INT32_MAX));const auto epoch=uint32_t(integer(row.get("epoch"),1,UINT32_MAX));
    media.tracks.push_back({uint8_t(output.id=="0"?0:1),epoch,output.width,output.height,output.width,output.height,output.left,output.top,false});result.outputs.push_back(std::move(output));result.epochs.push_back(epoch);
  }
  if(!multitap::tracks::valid(media))fail();
  // Canonical order is part of the private request correlation, independent of
  // caller array ordering. Monitor-count changes are checked against the lease.
  if(result.outputs.size()==2&&result.outputs[0].id>result.outputs[1].id){std::swap(result.outputs[0],result.outputs[1]);std::swap(result.epochs[0],result.epochs[1]);}
  return result;
}
inline bool nativeSameTrackResize(const NativeTrackResize& a,const NativeTrackResize& b){
  if(a.sessionId!=b.sessionId||a.requestId!=b.requestId||a.resizeId!=b.resizeId||a.topologyEpoch!=b.topologyEpoch||a.fps!=b.fps||a.outputs.size()!=b.outputs.size()||a.epochs!=b.epochs)return false;
  for(size_t i=0;i<a.outputs.size();++i){const auto& x=a.outputs[i];const auto& y=b.outputs[i];if(x.id!=y.id||x.width!=y.width||x.height!=y.height||x.left!=y.left||x.top!=y.top)return false;}return true;
}
inline std::string nativeTrackResizeFields(const NativeTrackResize& request){
  if(request.outputs.empty()||request.outputs.size()>2||request.epochs.size()!=request.outputs.size())throw std::runtime_error("invalid_track_request");
  if(request.outputs.size()!=request.epochs.size())throw std::runtime_error("invalid_track_request");
  std::string result="\"sessionId\":"+jsonQuote(request.sessionId)+",\"requestId\":"+jsonQuote(request.requestId)+",\"resizeId\":"+jsonQuote(request.resizeId)+",\"topologyEpoch\":"+std::to_string(request.topologyEpoch)+",\"fps\":"+std::to_string(request.fps)+",\"tracks\":[";
  for(size_t i=0;i<request.outputs.size();++i){const auto& value=request.outputs[i];if(i)result+=',';result+="{\"id\":"+jsonQuote(value.id)+",\"width\":"+std::to_string(value.width)+",\"height\":"+std::to_string(value.height)+",\"x\":"+std::to_string(value.left)+",\"y\":"+std::to_string(value.top)+",\"epoch\":"+std::to_string(request.epochs[i])+'}';}return result+']';
}
inline bool nativeTrackResizeValid(const NativeTrackResize& request){try{return nativeSameTrackResize(request,nativeTrackResizeRequest(JsonParser::parse('{'+nativeTrackResizeFields(request)+'}')));}catch(...){return false;}}
inline NativeTrackResize nativeInitialTrackViewport(const Json& command){auto value=command;streamField(value,"requestId",std::string("initial-source"));streamField(value,"resizeId",command.get("sessionId").string());return nativeTrackResizeRequest(value);}
inline bool nativeTrackResizeFollows(const NativeTrackResize& old,const NativeTrackResize& request){
  if(!nativeTrackResizeValid(old)||!nativeTrackResizeValid(request)||old.sessionId!=request.sessionId||old.topologyEpoch==UINT32_MAX||request.topologyEpoch!=old.topologyEpoch+1||old.outputs.size()!=request.outputs.size()||old.epochs.size()!=old.outputs.size())return false;
  for(size_t i=0;i<old.outputs.size();++i){const auto& a=old.outputs[i];const auto& b=request.outputs[i];const bool changed=a.width!=b.width||a.height!=b.height||old.fps!=request.fps;if(a.id!=b.id||(changed&&old.epochs[i]==UINT32_MAX)||request.epochs[i]!=old.epochs[i]+uint32_t(changed))return false;}return true;
}
inline std::string nativeTrackResizeFailure(const NativeTrackResize& request,const std::string& reason){return "{\"type\":\"stream.tracks.resize.failed\",\"sessionId\":"+jsonQuote(request.sessionId)+",\"requestId\":"+jsonQuote(request.requestId)+",\"resizeId\":"+jsonQuote(request.resizeId)+",\"topologyEpoch\":"+std::to_string(request.topologyEpoch)+",\"reason\":"+jsonQuote(reason)+'}';}
inline bool nativeTrackMetadataMatches(const Json& value,const NativeTrackResize& expected,bool correlated,bool scaled=false){
  try{
    if(value.get("type").string()!="stream.tracks.info"||value.get("codec").string()!="h264"||value.get("sessionId").string()!=expected.sessionId||value.get("topologyEpoch").number()!=expected.topologyEpoch||value.get("fpsTarget").number()!=expected.fps||(correlated&&(value.get("requestId").string()!=expected.requestId||value.get("resizeId").string()!=expected.resizeId)))return false;
    const auto* rows=std::get_if<Json::Array>(&value.get("tracks").value);if(!rows||rows->size()!=expected.outputs.size())return false;
    Json converted=JsonParser::parse('{'+nativeTrackResizeFields(expected)+'}');Json::Array tracks;unsigned primary=0;
    for(const auto& row:*rows){
      const auto width=row.get("width").number(-1),height=row.get("height").number(-1);
      if(!nativeDisplayInteger(width,16,ownedOutputMaxSide)||!nativeDisplayInteger(height,16,ownedOutputMaxSide)||!encodedPictureSize(uint64_t(width),uint64_t(height))||width>row.get("sourceWidth").number(-1)||height>row.get("sourceHeight").number(-1)||!std::holds_alternative<bool>(row.get("primary").value)||(!scaled&&(row.get("sourceWidth").number()!=width||row.get("sourceHeight").number()!=height)))return false;
      primary+=row.get("primary").boolean()?1:0;Json item;item.value=Json::Object{};auto& fields=std::get<Json::Object>(item.value);for(const auto* field:{"id","epoch"})fields[field]=row.get(field);fields["width"]=row.get(scaled?"sourceWidth":"width");fields["height"]=row.get(scaled?"sourceHeight":"height");fields["x"]=row.get("left");fields["y"]=row.get("top");tracks.push_back(std::move(item));
    }if(primary>1)return false;
    std::get<Json::Object>(converted.value)["tracks"].value=std::move(tracks);const auto actual=nativeTrackResizeRequest(converted);std::optional<int64_t> dx,dy;
    for(size_t i=0;i<actual.outputs.size();++i){const auto& a=actual.outputs[i];const auto& b=expected.outputs[i];const auto x=int64_t(a.left)-b.left,y=int64_t(a.top)-b.top;if(a.id!=b.id||a.width!=b.width||a.height!=b.height||actual.epochs[i]!=expected.epochs[i]||(dx&&*dx!=x)||(dy&&*dy!=y))return false;dx=x;dy=y;}return true;
  }catch(...){return false;}
}
inline bool nativeTrackMetadataMatchesDisplay(const Json& value,const DisplayState& display,bool scaled=false){
  if(!display.ready()||display.remoteSession)return false;const auto* rows=std::get_if<Json::Array>(&value.get("tracks").value);if(!rows||rows->empty()||rows->size()>2)return false;
  for(const auto& row:*rows){const auto monitor=std::find_if(display.monitors.begin(),display.monitors.end(),[&](const auto& item){return item.id==row.get("id").string();});if(monitor==display.monitors.end()||row.get(scaled?"sourceWidth":"width").number(-1)!=monitor->width||row.get(scaled?"sourceHeight":"height").number(-1)!=monitor->height||row.get("left").number(-65536)!=monitor->left||row.get("top").number(-65536)!=monitor->top||!std::holds_alternative<bool>(row.get("primary").value)||row.get("primary").boolean()!=monitor->primary)return false;}return true;
}
inline bool nativeTrackMetadataMayComplete(const Json& value,const NativeTrackResize& request,const DisplayState& display,bool workerVerified,bool initial,bool scaled=false){
  if(!workerVerified||!nativeTrackMetadataMatchesDisplay(value,display,scaled))return false;
  if(initial&&(!std::holds_alternative<std::nullptr_t>(value.get("requestId").value)||!std::holds_alternative<std::nullptr_t>(value.get("resizeId").value)))return false;
  return nativeTrackMetadataMatches(value,request,!initial,scaled);
}
}
