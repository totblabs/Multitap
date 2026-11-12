#include <stream_size_admission.h>
#include <track_media.hpp>
#include <track_picture.hpp>
#include <lossless_tiles.hpp>
#include <cstdio>
#include <cstring>
int main(){
  using namespace multitap;int checks=0,fail=0;
  const auto check=[&](bool ok,const char* name){++checks;if(!ok){++fail;std::printf("FAIL %s\n",name);}};
  check(ValidVideoSize(7680,4320),"8K arithmetic is not blocked by a 4K product cap");
  check(ValidVideoSize(4320,7680),"portrait uses the same arithmetic rule");
  check(!ValidVideoSize(0,4320)&&!ValidVideoSize(7681,4320),"zero and odd NV12 geometry rejected");
  check(!ValidVideoSize(UINT32_MAX,UINT32_MAX),"stride and product overflow rejected before allocation");
  StreamSize dual[]={{7680,4320,60},{7680,4320,60}};StreamMemory need{};
  check(StreamMemoryNeeded(dual,2,need),"two 8K tracks have checked aggregate memory");
  size_t queried=0;auto exact=[&](const StreamSize&,size_t)->const char*{++queried;return nullptr;};
  check(AdmitStreamSizes(dual,2,need.systemBytes,exact)==nullptr&&queried==2,"each exact track is probed without a rectangular inference");
  queried=0;check(std::strcmp(AdmitStreamSizes(dual,2,need.systemBytes-1,exact),"stream_memory_budget")==0&&queried==0,"dual aggregate rejected before any decoder call");
  StreamSize portrait[]={{4320,7680,60}};
  auto perShape=[](const StreamSize& s,size_t)->const char*{return s.height>s.width?"stream_decoder_type":nullptr;};
  check(std::strcmp(AdmitStreamSizes(portrait,1,need.systemBytes,perShape),"stream_decoder_type")==0,"landscape acceptance never implies portrait decoder support");
  check(std::strcmp(AdmitStreamSizes(dual,2,0,exact),"stream_memory_unknown")==0,"unknown available memory is not admitted");
  StreamSize scaled[]={{1920,1080,60,7680,4320}};StreamMemory scaledNeed{};
  check(StreamMemoryNeeded(scaled,1,scaledNeed)&&scaledNeed.systemBytes>uint64_t(1920)*1080*4*8,"workspace memory is not confused with small encoded picture");
  auto encodedOnly=[](const StreamSize& s,size_t)->const char*{return s.width==1920&&s.height==1080?nullptr:"stream_decoder_type";};
  check(AdmitStreamSizes(scaled,1,scaledNeed.systemBytes,encodedOnly)==nullptr,"8K workspace survives exact 1080p decoder admission");
  StreamSize incomplete[]={{1920,1080,60,7680,0}};
  check(!StreamMemoryNeeded(incomplete,1,scaledNeed),"half workspace geometry is rejected");
  StreamSize enormous[]={{1920,1080,60,536870910,2147483646}};
  check(!StreamMemoryNeeded(enormous,1,scaledNeed),"aggregate byte multiplication cannot wrap into small admission");
  auto rate=[](const StreamSize& s,size_t)->const char*{return s.fps==120?"stream_decoder_type":nullptr;};
  StreamSize highRate[]={{1920,1080,120}};
  check(std::strcmp(AdmitStreamSizes(highRate,1,need.systemBytes,rate),"stream_decoder_type")==0,"type probe uses candidate rate instead of a 60fps test rectangle");
  tracks::Configuration config;config.topology=1;config.tracks={{0,1,7680,4320,7680,4320,0,0,true},{1,1,7680,4320,7680,4320,7680,0,false}};
  check(tracks::valid(config),"two actual 8K tracks are not restricted to one old 4K pixel sum");
  config.tracks[0].width=1920;config.tracks[0].height=1080;
  check(tracks::valid(config),"initial v2 source and encoded shapes remain distinct");
  config.tracks[1].left=0;check(!tracks::valid(config),"larger dimensions do not relax overlapping-source rejection");
  check(lossless::validFrame(7680,4320),"8K tiles pass unchanged uint16 wire representation");
  check(!lossless::validFrame(65536,4320)&&!lossless::validFrame(32768,32768),"real uint16 dimension and tile-index bounds remain enforced");
  std::vector<uint8_t> tiny(2*2*4);for(size_t i=0;i<tiny.size();i+=4){tiny[i]=7;tiny[i+1]=19;tiny[i+2]=31;tiny[i+3]=255;}
  check(video::sourcePicture(2,2,4,6,tiny,4*6*4)&&tiny.size()==4*6*4&&tiny[0]==7&&tiny.back()==255,"production initial-track helper emits source-sized pixels with no native API");
  const auto saved=tiny;check(!video::sourcePicture(4,6,8,12,tiny,1)&&tiny==saved,"insufficient-memory resize preserves original decoded bytes");
  check(!video::sourcePicture(4,6,2,2,tiny,UINT64_MAX),"source smaller than encoded is not silently cropped");
  tracks::Configuration narrow;narrow.topology=1;narrow.tracks={{0,1,16,16,320,180,0,0,true}};
  check(tracks::valid(narrow),"v2 minimum encoded 16x16 preserves a larger source workspace without claiming device acceptance");
  narrow.tracks[0].width=14;check(!tracks::valid(narrow),"encoded width below 16 is rejected");
  narrow.tracks[0].width=16;narrow.tracks[0].height=14;check(!tracks::valid(narrow),"encoded height below 16 is rejected");
  narrow.tracks[0].height=16;narrow.tracks[0].width=17;check(!tracks::valid(narrow),"encoded odd width still violates NV12 representation");
  narrow.tracks[0].width=16;narrow.tracks[0].sourceWidth=8;check(!tracks::valid(narrow),"smaller encoded minimum never permits source smaller than encoded");
  std::printf("stream_size_admission checks=%d failures=%d windowsApi=0 allocationProbe=0\n",checks,fail);return fail?1:0;
}
