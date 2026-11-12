#pragma once
#include "gpu_h264_policy.hpp"
#include "encoder_gpu_failure.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace host {
struct EncoderDiagnostic {
  std::string backend,adapterName;
  bool initialized=false,hardwareVerified=false;
  uint64_t adapterLuid=0;
  uint32_t vendorId=0,deviceId=0,gpuFallbackCode=0,softwareFallbackCode=0;
  EncoderGpuFailure gpuFailure;  // Private worker detail; never in h264-encoder-v1.
};
inline bool encoderGpuCode(uint32_t code){return !code||(code>=32&&code<=44);}
inline bool encoderBackendKnown(const std::string& value){return value=="x264"||value=="mft"||value=="nvenc"||value=="amf"||value=="qsv-mft";}
inline uint32_t encoderVendor(const std::string& value){return value=="nvenc"?0x10de:value=="amf"?0x1002:value=="qsv-mft"?0x8086:0;}
// At most DXGI Description's128 UTF16 units/512 UTF8 bytes. No malformed UTF8,
// control characters or line separators can become an arbitrary log/UI string.
inline bool encoderAdapterName(const std::string& text){
  if(text.empty()||text.size()>512)return false;size_t units=0;
  for(size_t at=0;at<text.size();){const auto first=static_cast<unsigned char>(text[at++]);uint32_t cp=first;size_t tail=0;
    if(first>=0xc2&&first<=0xdf){cp=first&31;tail=1;}else if(first>=0xe0&&first<=0xef){cp=first&15;tail=2;}else if(first>=0xf0&&first<=0xf4){cp=first&7;tail=3;}else if(first>=0x80)return false;
    if(text.size()-at<tail)return false;for(size_t i=0;i<tail;++i){const auto c=static_cast<unsigned char>(text[at++]);if((c&0xc0)!=0x80)return false;cp=(cp<<6)|(c&63);}
    if((tail==1&&cp<0x80)||(tail==2&&cp<0x800)||(tail==3&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)||cp<32||(cp>=0x7f&&cp<=0x9f)||cp==0x2028||cp==0x2029)return false;
    units+=cp>0xffff?2:1;if(units>128)return false;
  }return true;
}
inline bool encoderDiagnosticValid(const EncoderDiagnostic& value){
  if(!encoderBackendKnown(value.backend)||!encoderGpuCode(value.gpuFallbackCode)||value.softwareFallbackCode>7||(!value.adapterName.empty()&&!encoderAdapterName(value.adapterName)))return false;
  const auto vendor=encoderVendor(value.backend);const bool identified=value.adapterLuid||value.vendorId||value.deviceId||!value.adapterName.empty();
  if((!vendor||!value.initialized)&&identified)return false;
  if(value.vendorId&&vendor&&value.vendorId!=vendor)return false;
  return !value.hardwareVerified||(vendor&&value.initialized&&value.adapterLuid&&value.vendorId==vendor);
}
template<class Info> inline EncoderDiagnostic encoderDiagnostic(const std::string& backend,const Info& raw,uint32_t softwareFallback){
  EncoderDiagnostic value;value.backend=backend;value.initialized=raw.initialized;value.gpuFallbackCode=raw.gpuFallbackCode;value.softwareFallbackCode=softwareFallback;value.gpuFailure=raw.gpuFailure;
  if(encoderVendor(backend)&&raw.initialized){value.hardwareVerified=raw.hardwareVerified;value.adapterLuid=raw.adapterLuid;value.vendorId=raw.vendorId;value.deviceId=raw.deviceId;if(encoderAdapterName(raw.adapterName))value.adapterName=raw.adapterName;}
  return value;
}
inline std::string encoderDiagnosticQuote(const std::string& value){std::string out="\"";for(const auto c:value){if(c=='"'||c=='\\')out+='\\';out+=c;}return out+'"';}
inline std::string encoderDiagnosticJson(const EncoderDiagnostic& value){
  if(!encoderDiagnosticValid(value))return "null";
  const auto bit=[](bool b){return b?"true":"false";};
  return "{\"protocol\":\"h264-encoder-v1\",\"backend\":"+encoderDiagnosticQuote(value.backend)+",\"initialized\":"+bit(value.initialized)+",\"hardwareVerified\":"+bit(value.hardwareVerified)+
    ",\"adapterLuid\":"+(value.adapterLuid?encoderDiagnosticQuote(gpu::adapterLuidText(value.adapterLuid)):"null")+",\"vendorId\":"+(value.vendorId?std::to_string(value.vendorId):"null")+",\"deviceId\":"+(value.deviceId?std::to_string(value.deviceId):"null")+
    ",\"adapterName\":"+(value.adapterName.empty()?"null":encoderDiagnosticQuote(value.adapterName))+",\"gpuFallbackCode\":"+std::to_string(value.gpuFallbackCode)+",\"softwareFallbackCode\":"+std::to_string(value.softwareFallbackCode)+'}';
}
template<class JsonValue> inline std::optional<EncoderDiagnostic> parseEncoderDiagnostic(const JsonValue& raw){
  const auto* object=std::get_if<typename JsonValue::Object>(&raw.value);
  constexpr std::array<const char*,10> keys{"protocol","backend","initialized","hardwareVerified","adapterLuid","vendorId","deviceId","adapterName","gpuFallbackCode","softwareFallbackCode"};
  if(!object||object->size()!=keys.size())return {};for(const auto key:keys)if(!object->contains(key))return {};
  if(raw.get("protocol").string()!="h264-encoder-v1")return {};EncoderDiagnostic value;
  const auto* backend=std::get_if<std::string>(&raw.get("backend").value);const auto* initialized=std::get_if<bool>(&raw.get("initialized").value);const auto* verified=std::get_if<bool>(&raw.get("hardwareVerified").value);
  if(!backend||!initialized||!verified)return {};value.backend=*backend;value.initialized=*initialized;value.hardwareVerified=*verified;
  const auto number=[&](const char* key,uint32_t& target,bool nullable){const auto& item=raw.get(key);if(nullable&&std::holds_alternative<std::nullptr_t>(item.value)){target=0;return true;}const auto* n=std::get_if<double>(&item.value);if(!n||!std::isfinite(*n)||*n<(nullable?1:0)||*n>4294967295.0||std::floor(*n)!=*n)return false;target=uint32_t(*n);return true;};
  if(!number("vendorId",value.vendorId,true)||!number("deviceId",value.deviceId,true)||!number("gpuFallbackCode",value.gpuFallbackCode,false)||!number("softwareFallbackCode",value.softwareFallbackCode,false))return {};
  const auto& luid=raw.get("adapterLuid");if(const auto* text=std::get_if<std::string>(&luid.value)){const auto parsed=gpu::parseAdapterLuid(*text);if(!parsed||!*parsed||gpu::adapterLuidText(*parsed)!=*text)return {};value.adapterLuid=*parsed;}else if(!std::holds_alternative<std::nullptr_t>(luid.value))return {};
  const auto& name=raw.get("adapterName");if(const auto* text=std::get_if<std::string>(&name.value)){if(!encoderAdapterName(*text))return {};value.adapterName=*text;}else if(!std::holds_alternative<std::nullptr_t>(name.value))return {};
  return encoderDiagnosticValid(value)?std::optional<EncoderDiagnostic>(value):std::nullopt;
}
// Optional private detail only. Unknown candidate identity must not erase the
// legacy actual CPU output observation or enter the released ten-field JSON.
inline std::string encoderGpuFailureJson(const EncoderGpuFailure& value){
 return "{\"site\":"+std::to_string(value.site)+",\"nativeStatus\":"+std::to_string(value.nativeStatus)+",\"vendorId\":"+std::to_string(value.vendorId)+",\"deviceId\":"+std::to_string(value.deviceId)+'}';
}
template<class JsonValue>inline std::optional<EncoderGpuFailure> parseEncoderGpuFailure(const JsonValue& raw){
 const auto* fields=std::get_if<typename JsonValue::Object>(&raw.value);constexpr std::array<const char*,4> keys{"site","nativeStatus","vendorId","deviceId"};
 if(!fields||fields->size()!=keys.size())return {};for(const auto key:keys)if(!fields->contains(key))return {};
 EncoderGpuFailure result;const auto number=[&](const char* key,uint32_t& target){const auto* n=std::get_if<double>(&raw.get(key).value);if(!n||!std::isfinite(*n)||*n<0||*n>4294967295.0||std::floor(*n)!=*n)return false;target=uint32_t(*n);return true;};
 if(!number("site",result.site)||!number("nativeStatus",result.nativeStatus)||!number("vendorId",result.vendorId)||!number("deviceId",result.deviceId)||!encoderGpuFailureDetailKnown(result))return {};
 return result;
}
struct EncoderDiagnosticTrack { std::string id;uint32_t epoch=0;EncoderDiagnostic info; };
struct EncoderUsageObservation {std::string session;uint32_t topology=0;EncoderDiagnosticTrack track;};
inline bool encoderUsageSession(const std::string& id){if(id.size()!=36)return false;for(size_t i=0;i<id.size();++i){if(i==8||i==13||i==18||i==23){if(id[i]!='-')return false;}else if(std::string("0123456789abcdef").find(id[i])==std::string::npos)return false;}return true;}
inline bool encoderUsageOutputValid(const EncoderDiagnosticTrack& track){return (track.id=="0"||track.id=="1")&&track.epoch<=65535&&encoderDiagnosticValid(track.info)&&track.info.initialized&&(!encoderVendor(track.info.backend)||track.info.hardwareVerified);}
inline bool encoderUsageGpuFailure(const EncoderDiagnosticTrack& track){
 return (track.info.backend=="x264"||track.info.backend=="mft")&&track.info.initialized&&!track.info.hardwareVerified&&track.info.gpuFallbackCode>=32&&encoderGpuCode(track.info.gpuFallbackCode)&&encoderGpuFailureDetailKnown(track.info.gpuFailure);
}
inline std::string encoderUsagePacket(const EncoderUsageObservation& value){
 if(!encoderUsageSession(value.session)||value.topology>65535||!encoderUsageOutputValid(value.track)||(!value.topology&&value.track.id!="0")||(value.topology&&!value.track.epoch))return {};
 auto out="{\"type\":\"worker.encoder.usage\",\"sessionId\":"+encoderDiagnosticQuote(value.session)+",\"topologyEpoch\":"+std::to_string(value.topology)+",\"id\":"+encoderDiagnosticQuote(value.track.id)+",\"epoch\":"+std::to_string(value.track.epoch)+",\"encoderInfo\":"+encoderDiagnosticJson(value.track.info);
 if(encoderUsageGpuFailure(value.track))out+=",\"gpuFailure\":"+encoderGpuFailureJson(value.track.info.gpuFailure);
 return out+'}';
}
template<class JsonValue>inline std::optional<EncoderUsageObservation> parseEncoderUsagePacket(const JsonValue& value){
 const auto* fields=std::get_if<typename JsonValue::Object>(&value.value);constexpr std::array<const char*,6> keys{"type","sessionId","topologyEpoch","id","epoch","encoderInfo"};
 if(!fields||(fields->size()!=keys.size()&&fields->size()!=keys.size()+1))return {};for(const auto key:keys)if(!fields->contains(key))return {};
 const bool detail=fields->contains("gpuFailure");if(fields->size()!=keys.size()+size_t(detail))return {};
 if(value.get("type").string()!="worker.encoder.usage")return {};const auto info=parseEncoderDiagnostic(value.get("encoderInfo"));if(!info)return {};
 const auto topology=value.get("topologyEpoch").number(-1),epoch=value.get("epoch").number(-1);if(!std::isfinite(topology)||!std::isfinite(epoch)||topology<0||topology>65535||epoch<0||epoch>65535||std::floor(topology)!=topology||std::floor(epoch)!=epoch)return {};
 EncoderUsageObservation result{value.get("sessionId").string(),uint32_t(topology),{value.get("id").string(),uint32_t(epoch),*info}};
 if(detail){const auto failure=parseEncoderGpuFailure(value.get("gpuFailure"));if(!failure)return {};result.track.info.gpuFailure=*failure;if(!encoderUsageGpuFailure(result.track))return {};}
 return encoderUsagePacket(result).empty()?std::nullopt:std::optional(result);
}
inline bool encoderUsageMatches(const EncoderUsageObservation& value,const std::string& session,uint32_t topology,std::span<const EncoderDiagnosticTrack> expected){
 if(!encoderUsageOutputValid(value.track)||value.session!=session||value.topology!=topology||expected.empty()||expected.size()>2)return false;
 for(const auto& track:expected)if(track.id==value.track.id)return track.epoch==value.track.epoch;
 return false;
}
// The existing protected numeric-only journal carries action254: stage is
// backend (1 x264,2 software MFT,3 NVENC,4 AMF,5 Intel MFT), format is vendor,
// fallback/error are the existing GPU/CPU enums. No frame, account or key data.
inline uint32_t encoderUsageBackend(const std::string& backend){return backend=="x264"?1:backend=="mft"?2:backend=="nvenc"?3:backend=="amf"?4:backend=="qsv-mft"?5:0;}
inline const char* encoderUsageBackend(uint32_t backend){return backend==1?"x264":backend==2?"mft":backend==3?"nvenc":backend==4?"amf":backend==5?"qsv-mft":"";}
// action255: stage=site, win32=raw uint32 native status, format/cause=
// failed candidate vendor/device, error=GPU fallback enum, fallback=actual CPU
// backend (1 x264,2 software MFT), items=track and expected=track epoch.
struct EncoderGpuFailureJournalRecord {uint32_t site,nativeStatus,vendorId,deviceId,gpuFallbackCode,cpuBackend,track,epoch;};
inline std::optional<EncoderGpuFailureJournalRecord> encoderGpuFailureJournalRecord(const EncoderUsageObservation& value){
 if(!encoderUsageGpuFailure(value.track)||encoderUsagePacket(value).empty())return {};
 const auto& failure=value.track.info.gpuFailure;
 return EncoderGpuFailureJournalRecord{failure.site,failure.nativeStatus,failure.vendorId,failure.deviceId,value.track.info.gpuFallbackCode,encoderUsageBackend(value.track.info.backend),value.track.id=="0"?0u:1u,value.track.epoch};
}
class EncoderUsageJournalPolicy {
 public:
  bool observe(uint64_t current,uint64_t ticket,const EncoderDiagnosticTrack& track,bool actualOutput){
    if(!current||ticket!=current||!actualOutput||!encoderDiagnosticValid(track.info)||!track.info.initialized||
        (encoderVendor(track.info.backend)&&!track.info.hardwareVerified)||(track.id!="0"&&track.id!="1"))return false;
    const size_t index=track.id=="0"?0:1;
    const auto key=std::to_string(ticket)+":"+std::to_string(track.epoch)+":"+encoderDiagnosticJson(track.info)+(encoderUsageGpuFailure(track)?":"+encoderGpuFailureJson(track.info.gpuFailure):"");
    if(keys_[index]==key)return false;keys_[index]=key;return true;
  }
 private:std::array<std::string,2> keys_;
};
class EncoderDiagnosticCache {
 public:
  using Ticket=uint64_t;
  explicit EncoderDiagnosticCache(uint64_t generation=0):generation_(generation){}
  Ticket begin(){std::lock_guard lock(mutex_);tracks_.clear();if(generation_==UINT64_MAX)return current_=0;return current_=++generation_;}
  bool current(Ticket ticket)const{std::lock_guard lock(mutex_);return ticket&&ticket==current_;}
  void retire(Ticket ticket=0){std::lock_guard lock(mutex_);if(ticket&&ticket!=current_)return;tracks_.clear();current_=0;}
  void invalidate(){std::lock_guard lock(mutex_);tracks_.clear();}
  void clear(Ticket ticket){std::lock_guard lock(mutex_);if(ticket==current_)tracks_.clear();}
  bool observe(Ticket ticket,std::vector<EncoderDiagnosticTrack> tracks,uint64_t now){
    if(!ticket||tracks.empty()||tracks.size()>2)return false;
    for(size_t i=0;i<tracks.size();++i){if((tracks[i].id!="0"&&tracks[i].id!="1")||!encoderDiagnosticValid(tracks[i].info))return false;for(size_t j=0;j<i;++j)if(tracks[i].id==tracks[j].id)return false;}
    std::lock_guard lock(mutex_);if(ticket!=current_)return false;tracks_=std::move(tracks);at_=now;return true;
  }
  std::string json(uint64_t now)const{
    std::lock_guard lock(mutex_);if(!current_||tracks_.empty()||now<at_||now-at_>5000)return "{\"protocol\":\"host-encoder-diagnostics-v1\",\"observed\":false,\"observationAgeMs\":null,\"tracks\":[]}";
    std::string out="{\"protocol\":\"host-encoder-diagnostics-v1\",\"observed\":true,\"observationAgeMs\":"+std::to_string(now-at_)+",\"tracks\":[";
    for(const auto& track:tracks_){if(out.back()!='[')out+=',';out+="{\"id\":"+encoderDiagnosticQuote(track.id)+",\"epoch\":"+std::to_string(track.epoch)+",\"info\":"+encoderDiagnosticJson(track.info)+'}';}return out+"]}";
  }
 private:
  mutable std::mutex mutex_;uint64_t generation_=0,current_=0,at_=0;std::vector<EncoderDiagnosticTrack> tracks_;
};
inline std::string appendEncoderDiagnostic(std::string report,const std::string& encoder){
  if(report.empty()||report.back()!='}'||encoder.empty()||report.size()+encoder.size()+11>6144)return report;
  report.pop_back();return report+",\"encoder\":"+encoder+'}';
}
}
