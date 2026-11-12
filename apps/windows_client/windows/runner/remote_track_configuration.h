#pragma once
#include "remote_track_decoder.h"
#include <flutter/encodable_value.h>
#include <limits>

namespace multitap {
inline const flutter::EncodableValue* MediaField(const flutter::EncodableMap& map,const char* key){const auto it=map.find(flutter::EncodableValue(key));return it==map.end()?nullptr:&it->second;}
inline bool MediaInteger(const flutter::EncodableValue* value,int64_t& number){
  if(!value)return false;if(const auto n=std::get_if<int32_t>(value)){number=*n;return true;}if(const auto n=std::get_if<int64_t>(value)){number=*n;return true;}return false;
}
inline bool TrackConfiguration(const flutter::EncodableValue* input,tracks::Configuration& result){
  const auto map=input?std::get_if<flutter::EncodableMap>(input):nullptr;
  if(!map||(map->size()!=3&&map->size()!=4))return false;
  int64_t topology=0;if(!MediaInteger(MediaField(*map,"topologyEpoch"),topology)||topology<1||topology>UINT32_MAX)return false;
  const auto audioValue=MediaField(*map,"audioEnabled"),listValue=MediaField(*map,"tracks"),formatValue=MediaField(*map,"audioFormat");
  const auto audio=audioValue?std::get_if<bool>(audioValue):nullptr;
  const auto list=listValue?std::get_if<flutter::EncodableList>(listValue):nullptr;
  // audioFormat: only "opus" (audio-opus-tracks-v1, as the metadata stated);
  // without it the shared audio is s16le PCM as before.
  const auto format=formatValue?std::get_if<std::string>(formatValue):nullptr;
  if(!audio||!list||list->empty()||list->size()>2||(map->size()==4&&(!format||*format!="opus")))return false;
  tracks::Configuration candidate{uint32_t(topology),*audio,{}};candidate.audioOpus=format!=nullptr;
  for(const auto& value:*list){
    const auto fields=std::get_if<flutter::EncodableMap>(&value);if(!fields||fields->size()!=9)return false;
    const auto idValue=MediaField(*fields,"id"),primaryValue=MediaField(*fields,"primary");
    const auto id=idValue?std::get_if<std::string>(idValue):nullptr;const auto primary=primaryValue?std::get_if<bool>(primaryValue):nullptr;
    if(!id||(*id!="0"&&*id!="1")||!primary)return false;
    const char* keys[]={"epoch","width","height","sourceWidth","sourceHeight","left","top"};int64_t n[7]{};
    for(unsigned i=0;i<7;++i){if(!MediaInteger(MediaField(*fields,keys[i]),n[i]))return false;
      if(i<5?(n[i]<0||n[i]>UINT32_MAX):(n[i]<INT32_MIN||n[i]>INT32_MAX))return false;}
    candidate.tracks.push_back({uint8_t((*id)[0]-'0'),uint32_t(n[0]),uint32_t(n[1]),uint32_t(n[2]),uint32_t(n[3]),uint32_t(n[4]),int32_t(n[5]),int32_t(n[6]),*primary});
  }
  if(!tracks::valid(candidate))return false;result=std::move(candidate);return true;
}
}
