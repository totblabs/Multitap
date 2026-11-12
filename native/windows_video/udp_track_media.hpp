#pragma once
#include "track_media.hpp"
#include "udp_media.hpp"

namespace multitap::udp {
// Each output has its own UDP leg, keys and immutable binding. Never truncate
// tracks-v1's 32-bit epoch into the existing 16-bit UDP receiver's epoch.
struct TrackBinding {
  uint8_t track=0;
  uint32_t topology=0,epoch=0;
  bool primary=false;
};
inline bool validTrackBinding(const TrackBinding& binding){
  return binding.track<=1&&binding.topology&&binding.epoch&&binding.epoch<=UINT16_MAX;
}
inline bool trackToUdp(const TrackBinding& binding,const std::vector<uint8_t>& bytes,std::vector<uint8_t>& result,bool opus=false){
  tracks::Packet packet;
  if(!validTrackBinding(binding)||!tracks::parse(bytes.data(),bytes.size(),packet,opus)||packet.topology!=binding.topology)return false;
  if(packet.kind==tracks::video?(packet.track!=binding.track||packet.epoch!=binding.epoch):!binding.primary)return false;
  std::vector<uint8_t> value(12+packet.size);
  value[0]=packet.kind;value[1]=packet.flags;value[2]=uint8_t(binding.epoch);value[3]=uint8_t(binding.epoch>>8);
  for(unsigned n=0;n<8;++n)value[4+n]=uint8_t(packet.pts>>(8*n));
  std::copy_n(packet.payload,packet.size,value.begin()+12);result.swap(value);return true;
}
inline bool udpToTrack(const TrackBinding& binding,const std::vector<uint8_t>& bytes,std::vector<uint8_t>& result,bool opus=false){
  if(!validTrackBinding(binding)||bytes.empty()||(bytes[0]!=1&&bytes[0]!=2)||(bytes[0]==2&&!binding.primary))return false;
  Envelope envelope;const bool video=bytes[0]==1;
  if(!parseEnvelope(bytes.data(),bytes.size(),video?classVideo:classAudio,envelope)||envelope.epoch!=binding.epoch)return false;
  return tracks::pack({bytes[0],uint8_t(envelope.key?1:0),video?binding.track:tracks::audioTrack,binding.topology,video?binding.epoch:0,envelope.pts,bytes.data()+12,bytes.size()-12},result,opus);
}
}
