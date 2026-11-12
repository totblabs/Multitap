#pragma once
#include "udp_probe_leg.hpp"
#include "direct_input.hpp"
#include "direct_input_validation.hpp"
#include "json_object.hpp"
#include "../../../native/windows_video/udp_track_media.hpp"
#include <map>

namespace host {
inline constexpr char udpTracksProtocol[]="udp-tracks-v1";
struct UdpTrackScope {
  std::string track;
  multitap::udp::TrackBinding binding;
  uint32_t generation=0;
  int bitrate=0;
};
inline std::optional<uint32_t> udpTrackNumber(const Json& value,uint32_t minimum,uint32_t maximum=UINT32_MAX){
  const auto* n=std::get_if<double>(&value.value);if(!n||!std::isfinite(*n)||std::floor(*n)!=*n||*n<minimum||*n>maximum)return {};return uint32_t(*n);
}
inline bool udpTrackScopeMatches(const Json& value,const UdpTrackScope& scope){
  return value.get("track").string()==scope.track&&udpTrackNumber(value.get("topologyEpoch"),1)==scope.binding.topology&&udpTrackNumber(value.get("epoch"),1,65535)==scope.binding.epoch&&udpTrackNumber(value.get("generation"),1)==scope.generation;
}
inline bool validDirectTrackInput(const Json& value,const UdpTrackScope& scope,bool relative){
  const auto* fields=std::get_if<Json::Object>(&value.value);if(!fields)return false;
  if(value.get("type").string()=="input.reset")return scope.binding.primary;
  if(udpTrackNumber(value.get("topologyEpoch"),1)!=scope.binding.topology||!validDirectInput(value,relative))return false;
  const auto event=value.get("event").string();const bool global=event=="key"||event=="moveRelative"||(!fields->contains("x")&&!fields->contains("y"));
  if(global)return scope.binding.primary&&!fields->contains("track")&&!fields->contains("epoch")&&!fields->contains("x")&&!fields->contains("y");
  return value.get("track").string()==scope.track&&udpTrackNumber(value.get("epoch"),1)==scope.binding.epoch;
}

// Service-owned track transport. Capture, display ownership and encoder setup
// remain with the existing worker. Every output keeps an independent UDP path.
class UdpTracksSession {
 public:
  struct Callbacks {
    std::function<bool(const std::string&)> send;
    std::function<void(const Json&)> input;
    std::function<void()> keyFrame;
  };
  UdpTracksSession(std::string session,Callbacks callbacks,bool relative=false):session_(std::move(session)),callbacks_(std::move(callbacks)),relative_(relative),input_([this](const DirectInputItem& item){emitInput(item);}){}
  ~UdpTracksSession(){close();}
  UdpTracksSession(const UdpTracksSession&)=delete;UdpTracksSession& operator=(const UdpTracksSession&)=delete;
  void close()noexcept{
    if(closed_.exchange(true))return;
    try{std::lock_guard operation(operation_);clearLegs();input_.stop();}catch(...){ }
  }
  void suspend()noexcept{try{std::lock_guard operation(operation_);clearLegs();input_.pause();}catch(...){ }}
  // Called only on the worker's validated outbound track messages. False
  // leaves the exact original packet on its existing authenticated WSS tunnel.
  bool outgoing(bool text,std::vector<uint8_t>& bytes)noexcept{
    try{
      if(closed_)return false;
      if(text){const auto value=JsonParser::parse(std::string(bytes.begin(),bytes.end()));if(value.get("type").string()=="stream.tracks.info")observe(value);return false;}
      multitap::tracks::Packet packet;bool opus=false;
      {std::lock_guard lock(mutex_);opus=opus_;}
      if(!multitap::tracks::parse(bytes.data(),bytes.size(),packet,opus))return false;
      std::optional<Row> row;
      {std::lock_guard lock(mutex_);for(const auto& [id,current]:legs_)if(packet.kind==multitap::tracks::video?current.scope.binding.track==packet.track:current.scope.binding.primary){row=current;break;}}
      if(!row||!row->leg)return false;
      std::vector<uint8_t> normalized;if(!multitap::udp::trackToUdp(row->scope.binding,bytes,normalized,opus))return false;
      return row->leg->outgoing(false,normalized);
    }catch(...){return false;}
  }
  // All calls arrive on the track tunnel, in the same order as ready/reset.
  // true consumes a UDP/ordered-input control; ordinary worker controls stay.
  bool control(const Json& message)noexcept{
    const auto type=message.get("type").string();const bool ours=type.starts_with("udp.tracks.")||type=="input.direct";
    try{
      if(closed_)return ours;
      if(!ours){if(type=="stream.tracks.resize")suspend();else if(type=="input"||type=="input.reset"||type=="cursor.compose"||type=="cursor.echo"||type=="caret.watch"||type=="display.scale"){std::lock_guard operation(operation_);input_.pause();}return false;}
      if(message.get("sessionId").string()!=session_)return true;
      std::lock_guard operation(operation_);if(closed_)return true;
      if(type=="input.direct"){
        const auto sequence=udpTrackNumber(message.get("sequence"),1),epoch=udpTrackNumber(message.get("epoch"),0,65535);const auto& value=message.get("input");const auto inner=value.get("type").string();
        if(message.get("protocol").string()!="udp-direct-v1"||!sequence||!epoch||(inner!="input"&&inner!="input.reset"&&inner!="input.fence"))return true;
        const auto body=jsonObject(value);if(body.size()<=8192)input_.submit(true,{*sequence,body,udpProbeNowUs()});return true;
      }
      if(type=="udp.tracks.grant"){grant(message);return true;}
      if(type=="udp.tracks.lease"){
        if(message.get("protocol").string()!=udpTracksProtocol)return true;
        std::vector<std::string> ids;{std::lock_guard lock(mutex_);if(udpTrackNumber(message.get("generation"),1)!=generation_||udpTrackNumber(message.get("topologyEpoch"),1)!=topology_)return true;for(auto& [id,row]:legs_){row.leg->wssHeard();ids.push_back(id);}}
        for(const auto& id:ids)replyKey(id);return true;
      }
      const auto id=message.get("track").string();std::optional<Row> row;
      {std::lock_guard lock(mutex_);const auto found=legs_.find(id);if(found!=legs_.end()&&udpTrackScopeMatches(message,found->second.scope))row=found->second;}
      if(!row)return true;
      if(type=="udp.tracks.peer"){row->leg->peer(message);return true;}
      if(type=="udp.tracks.key"&&message.get("protocol").string()==udpMediaProtocol){
        multitap::udp::PublicKey key;if(!udpBase64(message.get("publicKey").string(),key))return true;
        {std::lock_guard lock(mutex_);auto& actual=legs_.at(id);const auto value=message.get("publicKey").string();if(!actual.clientKey.empty()&&actual.clientKey!=value)return true;actual.clientKey=value;}
        replyKey(id);
      }
    }catch(...){ }
    return ours;
  }
 private:
  struct Row {UdpTrackScope scope;std::shared_ptr<UdpProbeLeg> leg;std::shared_ptr<std::atomic<bool>> onUdp;std::string clientKey,reply;};
  struct Output {multitap::udp::TrackBinding binding;int bitrate=0;};
  void emitInput(const DirectInputItem& item){
    if(closed_)return;auto value=JsonParser::parse(item.body);if(value.get("type").string()=="input.fence")return;
    if(auto* object=std::get_if<Json::Object>(&value.value))(*object)["sessionId"]=Json{session_};
    if(callbacks_.input)callbacks_.input(value);
  }
  bool send(const std::string& value)noexcept{try{return !closed_&&callbacks_.send&&callbacks_.send(value);}catch(...){return false;}}
  std::string scoped(const char* type,const UdpTrackScope& scope,const std::string& fields)const{
    return "{\"type\":"+jsonQuote(type)+",\"sessionId\":"+jsonQuote(session_)+",\"track\":"+jsonQuote(scope.track)+",\"topologyEpoch\":"+std::to_string(scope.binding.topology)+",\"epoch\":"+std::to_string(scope.binding.epoch)+",\"generation\":"+std::to_string(scope.generation)+fields+'}';
  }
  void clearLegs(){
    std::map<std::string,Row> old;{std::lock_guard lock(mutex_);old.swap(legs_);}
    for(auto& [id,row]:old)if(row.leg)row.leg->close();
  }
  void observe(const Json& message){
    std::lock_guard operation(operation_);if(closed_)return;clearLegs();input_.pause();
    std::map<std::string,Output> next;const auto topology=udpTrackNumber(message.get("topologyEpoch"),1);const auto* rows=std::get_if<Json::Array>(&message.get("tracks").value);unsigned primary=0;
    if(topology&&message.get("sessionId").string()==session_&&rows&&!rows->empty()&&rows->size()<=2){
      for(const auto& value:*rows){const auto id=value.get("id").string();const auto epoch=udpTrackNumber(value.get("epoch"),1);const auto rate=udpTrackNumber(value.get("bitrateTarget"),1,100000000);const auto* isPrimary=std::get_if<bool>(&value.get("primary").value);
        if((id!="0"&&id!="1")||!epoch||!rate||!isPrimary||next.contains(id)){next.clear();break;}
        primary+=*isPrimary?1u:0u;next.emplace(id,Output{{uint8_t(id[0]-'0'),*topology,*epoch,*isPrimary},int(*rate)});
      }
    }
    const auto format=message.get("audio").get("format").string();if(primary!=1||(format!="s16le"&&format!="opus"))next.clear();
    std::lock_guard lock(mutex_);outputs_=std::move(next);topology_=topology.value_or(0);opus_=format=="opus";
  }
  void grant(const Json& message){
    if(message.get("protocol").string()!=udpTracksProtocol)return;
    const auto generation=udpTrackNumber(message.get("generation"),1),topology=udpTrackNumber(message.get("topologyEpoch"),1);const auto* rows=std::get_if<Json::Array>(&message.get("tracks").value);
    std::map<std::string,std::pair<UdpTrackScope,UdpGrant>> ready;
    {std::lock_guard lock(mutex_);if(!generation||*generation<=generation_||!topology||*topology!=topology_||!rows||rows->empty()||rows->size()!=outputs_.size())return;
      for(const auto& value:*rows){const auto id=value.get("track").string();const auto output=outputs_.find(id);const auto nested=jsonObject(value.get("grant"));auto leg=parseUdpGrant(value.get("grant"),nested.size());const auto* primary=std::get_if<bool>(&value.get("primary").value);
        if(output==outputs_.end()||ready.contains(id)||!multitap::udp::validTrackBinding(output->second.binding)||udpTrackNumber(value.get("epoch"),1,65535)!=output->second.binding.epoch||!primary||*primary!=output->second.binding.primary||udpTrackNumber(value.get("bitrateTarget"),1,100000000)!=uint32_t(output->second.bitrate)||!leg)return;
        ready.emplace(id,std::make_pair(UdpTrackScope{id,output->second.binding,*generation,output->second.bitrate},std::move(*leg)));
      }
    }
    // Detach hooks before dropping queued inputs: a callback already past its
    // scope check must not enqueue an old event after the generation fence.
    clearLegs();input_.pause();{std::lock_guard lock(mutex_);generation_=*generation;}
    for(auto& [id,item]:ready){
      const auto scope=item.first;const auto up=std::make_shared<std::atomic<bool>>(false);UdpProbeLegOptions options;
      options.media.keyFrame=[this]{if(!closed_&&callbacks_.keyFrame)callbacks_.keyFrame();};
      options.media.feedback=[rate=scope.bitrate](uint64_t,uint32_t,uint32_t){return rate;};options.media.path=[up,rate=scope.bitrate](bool value){*up=value;return rate;};
      options.media.local=[this,scope](const std::string& address,uint16_t port){send(scoped("udp.tracks.local",scope,",\"protocol\":\"udp-direct-v1\",\"address\":"+jsonQuote(address)+",\"port\":"+std::to_string(port)));};
      options.media.reflexive=[this,scope](const std::string& address,uint16_t port){send(scoped("udp.tracks.local",scope,",\"protocol\":\"udp-direct-v1\",\"candidateType\":\"srflx\",\"address\":"+jsonQuote(address)+",\"port\":"+std::to_string(port)));};
      options.media.input=[this,scope](uint32_t sequence,uint16_t epoch,const std::string& body){try{if(closed_||epoch!=scope.binding.epoch)return;{std::lock_guard lock(mutex_);if(scope.generation!=generation_||scope.binding.topology!=topology_||!legs_.contains(scope.track))return;}if(validDirectTrackInput(JsonParser::parse(body),scope,relative_))input_.submit(false,{sequence,body,udpProbeNowUs()});}catch(...){ }};
      if(auto leg=UdpProbeLeg::start(std::move(item.second),std::move(options))){leg->wssHeard();std::lock_guard lock(mutex_);legs_.emplace(id,Row{scope,std::move(leg),up,{},{}});}
    }
  }
  void replyKey(const std::string& id){
    std::optional<Row> copy;{std::lock_guard lock(mutex_);if(const auto found=legs_.find(id);found!=legs_.end())copy=found->second;}
    if(!copy||copy->clientKey.empty()||*copy->onUdp)return;
    if(copy->reply.empty()){
      const auto key=JsonParser::parse("{\"protocol\":\"udp-media-v1\",\"publicKey\":"+jsonQuote(copy->clientKey)+'}');
      const auto result=copy->leg->exchange(key,session_,copy->scope.bitrate);if(!result)return;
      copy->reply=scoped("udp.tracks.key",copy->scope,",\"protocol\":\"udp-media-v1\",\"publicKey\":"+jsonQuote(JsonParser::parse(*result).get("publicKey").string()));
      std::lock_guard lock(mutex_);const auto found=legs_.find(id);if(found==legs_.end()||found->second.leg!=copy->leg)return;found->second.reply=copy->reply;
    }
    send(copy->reply);
  }
  const std::string session_;Callbacks callbacks_;bool relative_=false;std::atomic<bool> closed_{false};
  std::mutex operation_,mutex_;std::map<std::string,Output> outputs_;std::map<std::string,Row> legs_;uint32_t topology_=0,generation_=0;bool opus_=false;
  DirectInputDispatcher input_;
};
}
