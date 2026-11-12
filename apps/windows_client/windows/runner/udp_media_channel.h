#pragma once
#include <flutter/binary_messenger.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <memory>
#include <map>
#include <functional>
#include "../../../../native/windows_video/udp_track_media.hpp"
#include "remote_track_configuration.h"
#include "udp_media_leg.h"

namespace multitap {
// `multitap/udp`: Dart stays in charge of control and starts, reads and stops the leg
// here. Every call carries the leg's number, so a late call of a closed session cannot
// reach its successor. Reading is Dart's timer, which bounds the rate of arrival reports.
class UdpMediaChannel {
 public:
  using TrackGate = std::function<std::shared_ptr<UdpPathGate>(const udp::TrackBinding&)>;
  explicit UdpMediaChannel(flutter::BinaryMessenger* messenger, TrackGate trackGate = {}) : gate_(std::make_shared<UdpPathGate>()), trackGate_(std::move(trackGate)) {
    channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(messenger, "multitap/udp", &flutter::StandardMethodCodec::GetInstance());
    channel_->SetMethodCallHandler([this](const auto& call, auto result) { Handle(call, *result); });
  }
  ~UdpMediaChannel() { channel_->SetMethodCallHandler(nullptr); }
  UdpMediaChannel(const UdpMediaChannel&) = delete;
  UdpMediaChannel& operator=(const UdpMediaChannel&) = delete;
  const std::shared_ptr<UdpPathGate>& gate() const { return gate_; }

 private:
  using V = flutter::EncodableValue;
  template <size_t N> static bool Bytes(const flutter::EncodableMap& args, const char* key, std::array<uint8_t, N>& out) {
    const auto* field = MediaField(args, key);
    const auto* bytes = field ? std::get_if<std::vector<uint8_t>>(field) : nullptr;
    if (!bytes || bytes->size() != N) return false;
    std::copy(bytes->begin(), bytes->end(), out.begin());
    return true;
  }
  static bool ParseGrant(const flutter::EncodableMap& args, UdpMediaLeg::Grant& grant) {
    const auto* relaysField = MediaField(args, "relays");
    const auto* relays = relaysField ? std::get_if<flutter::EncodableList>(relaysField) : nullptr;
    const auto* sessionField = MediaField(args, "sessionId");
    const auto* session = sessionField ? std::get_if<std::string>(sessionField) : nullptr;
    if (!relays || relays->empty() || !session || session->empty() || !Bytes(args, "id", grant.id) || !Bytes(args, "ticket", grant.ticket)) return false;
    for (const auto& value : *relays) {
      const auto* relay = std::get_if<flutter::EncodableMap>(&value);
      const auto* hostField = relay ? MediaField(*relay, "host") : nullptr;
      const auto* host = hostField ? std::get_if<std::string>(hostField) : nullptr;
      int64_t port = 0;
      if (!host || !MediaInteger(MediaField(*relay, "port"), port) || port < 1 || port > 65535) return false;
      grant.relays.push_back({*host, uint16_t(port)});
    }
    grant.sessionId = *session;
    const auto* direct=MediaField(args,"direct");grant.direct=direct&&std::get_if<bool>(direct)&&std::get<bool>(*direct);
    const auto* serversField=MediaField(args,"stun");const auto* servers=serversField?std::get_if<flutter::EncodableList>(serversField):nullptr;
    if(grant.direct&&servers){if(servers->size()>2)return false;for(const auto& item:*servers){const auto* row=std::get_if<flutter::EncodableMap>(&item);const auto* field=row?MediaField(*row,"host"):nullptr;const auto* host=field?std::get_if<std::string>(field):nullptr;int64_t port=0;if(!host||!MediaInteger(MediaField(*row,"port"),port)||port<1||port>65535)return false;grant.stun.push_back({*host,uint16_t(port)});}}
    return true;
  }
  static V Encode(const UdpMediaLeg::Reading& reading) {
    const auto& r = reading.report;
    flutter::EncodableMap report;
    const auto add = [&](const char* name, int64_t value) { report[V(name)] = V(value); };
    if (r.code) {
      add("udpProbe", r.code);
      if (r.relay >= 0) add("udpRelayIndex", r.relay);
      if (r.rttP50Ms >= 0) { add("udpRttP50Ms", r.rttP50Ms); add("udpRttP95Ms", r.rttP95Ms); }
    }
    if (r.lossPct >= 0) { add("udpLossPct", r.lossPct); add("udpKeepSent", r.keepSent); add("udpKeepLost", r.keepLost); }
    if (reading.media) {
      add("udpActivePath", reading.path.active); add("udpFallbacks", reading.path.fallbacks); add("udpGaps", reading.gaps);
      add("udpQueueWaits", reading.queueWaits); add("udpQueueSkips", reading.queueSkips); add("udpKeyRequests", reading.keyRequests); add("udpNacks", reading.nacks);
    }
    if(reading.directPath){add("udpDirectPath",reading.directPath);if(reading.directRttMs>=0)add("udpDirectRttMs",reading.directRttMs);if(reading.relayRttMs>=0)add("udpRelayRttMs",reading.relayRttMs);}
    // A plain list: this codec writes no alignment for an empty typed list, Dart's reader
    // aligns anyway, and everything after it then reads as garbage.
    flutter::EncodableList arrivals(reading.arrivals.begin(), reading.arrivals.end());
    flutter::EncodableMap value{{V("report"), V(report)}, {V("running"), V(reading.running)}, {V("arrivals"), V(arrivals)},
      {V("audioBytes"), V(reading.audioBytes)}, {V("keyedStream"), V(int64_t(reading.path.keyedStream))}};
    if (reading.publicKey) value[V("publicKey")] = V(std::vector<uint8_t>(reading.publicKey->begin(), reading.publicKey->end()));
    if(reading.local)value[V("local")]=V(flutter::EncodableMap{{V("address"),V(reading.local->first)},{V("port"),V(int32_t(reading.local->second))}});
    if(reading.reflexive)value[V("reflexive")]=V(flutter::EncodableMap{{V("address"),V(reading.reflexive->first)},{V("port"),V(int32_t(reading.reflexive->second))}});
    return V(value);
  }
  void Handle(const flutter::MethodCall<V>& call, flutter::MethodResult<V>& result) {
    const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
    int64_t number = 0;
    if (!args || !MediaInteger(MediaField(*args, "leg"), number) || number < 1) { result.Error("invalid_arguments", "UDP leg required."); return; }
    const auto& method = call.method_name();
    if (method == "start") {
      UdpMediaLeg::Grant grant;
      int64_t clockUs = 0;
      if (!ParseGrant(*args, grant) || !MediaInteger(MediaField(*args, "clockUs"), clockUs)) { result.Error("invalid_arguments", "UDP grant required."); return; }
      std::shared_ptr<UdpPathGate> gate=gate_;
      int track=-1;
      if(const auto* field=MediaField(*args,"track")){
        const auto* id=std::get_if<std::string>(field);int64_t topology=0,epoch=0,generation=0;
        const auto* primaryField=MediaField(*args,"primary");const auto* primary=primaryField?std::get_if<bool>(primaryField):nullptr;
        if(!id||(*id!="0"&&*id!="1")||!MediaInteger(MediaField(*args,"topologyEpoch"),topology)||topology<1||topology>UINT32_MAX||!MediaInteger(MediaField(*args,"epoch"),epoch)||epoch<1||epoch>UINT16_MAX||!MediaInteger(MediaField(*args,"generation"),generation)||generation<1||!primary||!trackGate_){result.Error("invalid_arguments","Output binding required.");return;}
        track=(*id)[0]-'0';gate=trackGate_({uint8_t(track),uint32_t(topology),uint32_t(epoch),*primary});
        if(!gate){result.Error("session_replaced","Output decoder replaced.");return;}
      }
      // One ordinary leg or two independent outputs, never an unbounded registry.
      for(auto it=legs_.begin();it!=legs_.end();){if(track<0||it->second.track<0||it->second.track==track){it->second.gate->Disengage();it=legs_.erase(it);}else ++it;}
      gate->Disengage();
      if(const auto pending=pending_.find(number);pending!=pending_.end()){gate->Stream(pending->second.first,pending->second.second);pending_.erase(pending);}
      try { legs_.emplace(number,Slot{track,gate,std::make_unique<UdpMediaLeg>(std::move(grant),gate,clockUs)}); result.Success(); }
      catch (...) { gate->Disengage(); result.Error("udp_leg_failed", "The UDP leg did not start."); }
      return;
    }
    if (method == "stream") {
      int64_t epoch = 0, generation = 0;
      if (!MediaInteger(MediaField(*args, "epoch"), epoch) || epoch < 0 || epoch > 65535 || !MediaInteger(MediaField(*args, "generation"), generation) || generation < 1) { result.Error("invalid_arguments", "Stream epoch required."); return; }
      if(const auto found=legs_.find(number);found!=legs_.end())found->second.gate->Stream(uint16_t(epoch),uint64_t(generation));
      else {if(pending_.size()>=3)pending_.erase(pending_.begin());pending_[number]={uint16_t(epoch),uint64_t(generation)};}
      result.Success();
      return;
    }
    const auto found=legs_.find(number);
    if(found==legs_.end()){pending_.erase(number);result.Success();return;}
    auto& leg_=found->second.leg;auto& gate=found->second.gate;
    if (method == "poll") { result.Success(Encode(leg_->Read())); return; }
    if(method=="input"){
      int64_t sequence=0,epoch=0;const auto* field=MediaField(*args,"bytes");const auto* bytes=field?std::get_if<std::vector<uint8_t>>(field):nullptr;
      if(!MediaInteger(MediaField(*args,"sequence"),sequence)||sequence<1||sequence>0xffffffffLL||!MediaInteger(MediaField(*args,"epoch"),epoch)||epoch<0||epoch>65535||!bytes||bytes->empty()||bytes->size()>udp::maxDirectInput){result.Error("invalid_arguments","Direct input required.");return;}
      leg_->Input(uint32_t(sequence),uint16_t(epoch),*bytes);result.Success();return;
    }
    if(method=="peer"){
      const auto* field=MediaField(*args,"candidates");const auto* list=field?std::get_if<flutter::EncodableList>(field):nullptr;
      if(!list||list->size()>5){result.Error("invalid_arguments","Peer candidates required.");return;}
      std::vector<std::pair<std::string,uint16_t>> candidates;
      for(const auto& value:*list){const auto* row=std::get_if<flutter::EncodableMap>(&value);const auto* a=row?MediaField(*row,"address"):nullptr;const auto* address=a?std::get_if<std::string>(a):nullptr;int64_t port=0;if(!row||!address||!MediaInteger(MediaField(*row,"port"),port)||port<1||port>65535){result.Error("invalid_arguments","Peer candidate required.");return;}candidates.push_back({*address,uint16_t(port)});}
      leg_->Peer(candidates);result.Success();return;
    }
    if (method == "hostKey") {
      std::array<uint8_t, 32> key{};
      const auto* keyedField = MediaField(*args, "keyed");
      const auto* keyed = keyedField ? std::get_if<bool>(keyedField) : nullptr;
      if (!Bytes(*args, "key", key) || !keyed) { result.Error("invalid_arguments", "Host key required."); return; }
      // Before the leg can use the key: from this call on every WSS push follows the rule.
      gate->Engage(*keyed);
      leg_->HostKey(key);
      result.Success();
      return;
    }
    if (method == "stop") { gate->Disengage(); legs_.erase(found); result.Success(); return; }
    result.NotImplemented();
  }

  std::unique_ptr<flutter::MethodChannel<V>> channel_;
  std::shared_ptr<UdpPathGate> gate_;
  struct Slot {int track;std::shared_ptr<UdpPathGate> gate;std::unique_ptr<UdpMediaLeg> leg;};
  std::map<int64_t,Slot> legs_;
  std::map<int64_t,std::pair<uint16_t,uint64_t>> pending_;
  TrackGate trackGate_;
};
}
