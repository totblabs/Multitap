#include "udp_track_media.hpp"
#include <iostream>
#include <stdexcept>
using namespace multitap;
static void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(){try{
  unsigned checks=0;const std::vector<uint8_t> payload{0,0,0,1,0x65,7,8,9};
  for(uint8_t track=0;track<2;++track){
    const udp::TrackBinding binding{track,7,uint32_t(11+track),track==0};
    std::vector<uint8_t> original,wire,restored;
    require(tracks::pack({tracks::video,1,track,7,binding.epoch,123456,payload.data(),payload.size()},original),"track fixture");
    require(udp::trackToUdp(binding,original,wire)&&udp::udpToTrack(binding,wire,restored)&&restored==original,"per-output round trip changed the codec bytes");++checks;
    auto other=binding;other.track=uint8_t(1-track);require(!udp::trackToUdp(other,original,restored),"another output crossed the leg");++checks;
    other=binding;++other.topology;require(!udp::trackToUdp(other,original,restored),"old topology crossed the leg");++checks;
    other=binding;++other.epoch;require(!udp::trackToUdp(other,original,restored)&&!udp::udpToTrack(other,wire,restored),"old epoch crossed the leg");++checks;
    other=binding;other.epoch=65536;require(!udp::trackToUdp(other,original,restored)&&!udp::udpToTrack(other,wire,restored),"32-bit epoch was truncated");++checks;
    wire.resize(12);require(!udp::udpToTrack(binding,wire,restored),"empty coded frame accepted");++checks;
  }
  for(const bool opus:{false,true}){
    const udp::TrackBinding primary{0,8,13,true},secondary{1,8,14,false};
    const std::vector<uint8_t> audio=opus?std::vector<uint8_t>{1,0,0xf8}:payload;
    std::vector<uint8_t> original,wire,restored;
    require(tracks::pack({tracks::audio,0,tracks::audioTrack,8,0,765432,audio.data(),audio.size()},original,opus),"audio fixture");
    require(udp::trackToUdp(primary,original,wire,opus)&&udp::udpToTrack(primary,wire,restored,opus)&&restored==original,"shared audio changed");++checks;
    require(!udp::trackToUdp(secondary,original,restored,opus)&&!udp::udpToTrack(secondary,wire,restored,opus),"shared audio duplicated on another output");++checks;
  }
  std::cout<<"{\"kind\":\"udp-track-media\",\"checks\":"<<checks<<",\"realInput\":false,\"realDisplay\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
