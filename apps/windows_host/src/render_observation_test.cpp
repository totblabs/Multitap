// Pure render-adapter observation checks. Nothing here reads this PC's counters.
#include "render_observation.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>
using namespace host;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
int run(){
  int passed=0;
  {const auto sample=parseGpuEngineInstance(L"pid_1824_luid_0x00000000_0x00008633_phys_0_eng_3_engtype_3D",77);
    require(sample&&sample->pid==1824&&sample->luid==0x8633&&sample->phys==0&&sample->engine==3&&sample->running==77,"A 3D engine instance was not parsed");++passed;}
  {const auto sample=parseGpuEngineInstance(L"pid_9_luid_0x00000001_0x0000AB00_phys_1_eng_0_engtype_3D",1);
    require(sample&&sample->luid==0x10000AB00ull&&sample->phys==1,"The LUID high part was not read first");++passed;}
  require(!parseGpuEngineInstance(L"pid_9_luid_0x00000000_0x00008633_phys_0_eng_4_engtype_VideoDecode",1),"A non-render engine counted");++passed;
  require(!parseGpuEngineInstance(L"pid_9_luid_0x00000000_0x00008633_phys_0_eng_0_engtype_3DExtra",1),"A longer engine type counted");++passed;
  require(!parseGpuEngineInstance(L"luid_0x00000000_0x00008633",1)&&!parseGpuEngineInstance(L"",1)&&!parseGpuEngineInstance(nullptr,1),"A malformed instance was parsed");++passed;
  const uint64_t basic=0x8633,nvidia=0x9000;constexpr uint32_t dwm=10,game=20;
  {const std::vector<GpuEngineSample> before{{dwm,nvidia,0,0,100},{dwm,basic,0,0,100},{game,nvidia,0,0,100}},after{{dwm,nvidia,0,0,105},{dwm,basic,0,0,190},{game,nvidia,0,0,900}};
    const auto reading=readRenderAdapters(before,after,dwm);
    require(reading.compositor==basic,"The compositor adapter was not the one that worked most");++passed;
    require(reading.busiest==nvidia,"The busiest adapter ignored other processes");++passed;}
  {const std::vector<GpuEngineSample> before{{dwm,basic,0,0,100}},after{{dwm,basic,0,0,100},{game,nvidia,0,0,5000}};
    const auto reading=readRenderAdapters(before,after,dwm);
    require(!reading.compositor&&!reading.busiest,"No work, or an engine without a baseline, produced a reading");++passed;}
  {const std::vector<GpuEngineSample> before{{dwm,basic,0,0,500}},after{{dwm,basic,0,0,10}};
    require(!readRenderAdapters(before,after,dwm).busiest,"A counter that went back counted as work");++passed;}
  {const std::vector<GpuEngineSample> before{{dwm,basic,0,0,100},{dwm,basic,0,1,100}},after{{dwm,basic,0,0,110},{dwm,basic,0,1,110},{dwm,nvidia,0,0,0}};
    require(readRenderAdapters(before,after,dwm).compositor==basic,"Engines of one adapter were not summed");++passed;}
  {std::vector<std::vector<GpuEngineSample>> script{{{dwm,basic,0,0,100}},{{dwm,basic,0,0,110}},{{dwm,basic,0,0,120}},{{dwm,nvidia,0,0,1}},{{dwm,nvidia,0,0,9}}};
    size_t at=0;int samples=0;RenderObservation observation([&]{++samples;return script[at++];},[]{return dwm;},30000);
    observation.streaming(true);
    require(!observation.tick(0),"The first sample had a baseline");
    require(!observation.tick(1000)&&samples==1,"The interval was not honored");
    const auto first=observation.tick(30000);require(first&&first->compositor==basic,"A first reading was not reported");
    require(!observation.tick(60000),"An unchanged reading was reported again");
    require(!observation.tick(90000),"An engine without a baseline was reported");
    const auto changed=observation.tick(120000);require(changed&&changed->compositor==nvidia&&samples==5,"A changed adapter was not reported");++passed;}
  // An idle compositor between two readings is not a change (seen on a real VM).
  {std::vector<std::vector<GpuEngineSample>> script{{{dwm,basic,0,0,100},{game,nvidia,0,0,100}},{{dwm,basic,0,0,110},{game,nvidia,0,0,200}},{{dwm,basic,0,0,110},{game,nvidia,0,0,300}},{{dwm,basic,0,0,120},{game,nvidia,0,0,300}},{{dwm,basic,0,0,120},{game,nvidia,0,0,300}}};
    size_t at=0;RenderObservation observation([&]{return script[at++];},[]{return dwm;},1);
    observation.streaming(true);
    require(!observation.tick(0),"The first sample had a baseline");
    const auto first=observation.tick(1);require(first&&first->compositor==basic&&first->busiest==nvidia,"A first reading was not reported");
    require(!observation.tick(2),"An idle compositor counted as a change");
    const auto busy=observation.tick(3);require(busy&&busy->compositor==basic&&busy->busiest==basic,"A real change of the busiest adapter was lost");
    require(!observation.tick(4),"An idle machine counted as a change");++passed;}
  // Two adapters taking turns: each pair is recorded once, so the log stays bounded.
  {std::vector<std::vector<GpuEngineSample>> script{{{dwm,basic,0,0,0},{game,nvidia,0,0,0}}};
    uint64_t a=0,b=0;for(int i=0;i<8;++i){if(i%2)b+=50;else a+=50;script.push_back({{dwm,basic,0,0,1+uint64_t(i)},{game,nvidia,0,0,b},{game,basic,0,1,a}});}
    script[0].push_back({game,basic,0,1,0});
    size_t at=0;int reports=0;RenderObservation observation([&]{return script[at++];},[]{return dwm;},1);
    observation.streaming(true);
    for(uint64_t now=0;now<script.size();++now)if(observation.tick(now))++reports;
    require(reports==2,"Two adapters taking turns were recorded again and again");++passed;}
  // Each stream records its own pair, once, however long it runs; idle time is never sampled.
  {uint64_t work=100;int samples=0;RenderObservation observation([&]{++samples;work+=10;return std::vector<GpuEngineSample>{{dwm,nvidia,0,0,work}};},[]{return dwm;},1);
    uint64_t now=0;const auto tick=[&]{return observation.tick(now++);};
    require(!tick()&&!tick()&&samples==0,"Idle time was sampled or recorded");
    observation.streaming(true);require(!tick()&&samples==1,"A stream did not take its own baseline first");
    require(tick().has_value(),"A stream did not record its pair");
    require(!tick(),"One stream recorded the same pair twice");
    observation.streaming(true);require(!tick(),"A running stream counted as a new one");
    observation.streaming(false);require(!tick(),"The end of a stream recorded a pair");
    observation.streaming(true);require(!tick()&&tick().has_value(),"The next stream did not record its own pair");++passed;}
  // Work done before a stream (the console display on the basic adapter) never reaches its first reading.
  // The counters as time goes on: idle (basic works), the stream starts, the stream runs (nvidia works).
  {const std::vector<std::vector<GpuEngineSample>> counters{{{dwm,basic,0,0,0},{dwm,nvidia,0,0,0}},{{dwm,basic,0,0,1000},{dwm,nvidia,0,0,10}},{{dwm,basic,0,0,1001},{dwm,nvidia,0,0,90}}};
    size_t phase=0;RenderObservation observation([&]{return counters[phase];},[]{return dwm;},1);
    require(!observation.tick(0),"Idle time was recorded");
    phase=1;observation.streaming(true);require(!observation.tick(1),"The stream's baseline was a reading");
    phase=2;const auto first=observation.tick(2);require(first&&first->compositor==nvidia,"Pre-stream work decided the stream's compositor");++passed;}
  return passed;
}
}
// --live: reads this PC's counters twice. Read-only, changes nothing.
int live(){
  const auto before=queryGpuEngineSamples();Sleep(2000);const auto after=queryGpuEngineSamples();
  const auto compositor=consoleCompositorPid();const auto reading=readRenderAdapters(before,after,compositor);
  std::cout<<std::hex<<"{\"compositorPid\":"<<std::dec<<compositor<<",\"samples\":"<<after.size()<<",\"compositorLuid\":\"0x"<<std::hex<<reading.compositor.value_or(0)<<"\",\"busiestLuid\":\"0x"<<reading.busiest.value_or(0)<<"\"}\n";
  return after.empty()||!compositor?1:0;
}
int main(int argc,char** argv){
  if(argc==2&&std::string_view(argv[1])=="--live")return live();
  try{std::cout<<"{\"kind\":\"render-observation\",\"passed\":"<<run()<<",\"countersRead\":0}\n";return 0;}
  catch(const std::exception& error){std::cerr<<"Render observation check failed: "<<error.what()<<"\n";return 1;}
}
