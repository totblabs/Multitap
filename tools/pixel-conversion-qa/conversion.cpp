#include <windows.h>
#include "baseline.hpp"
#include "../../native/windows_video/bgra_nv12_sse2.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
using Converter=void(*)(const uint8_t*,uint8_t*,int,int);
using Clock=std::chrono::steady_clock;
uint64_t checks=0,compared=0;
void require(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);++checks;}
__declspec(noinline) void candidate(const uint8_t* source,uint8_t* target,int width,int height){host::pixels::bgraToNv12PairedRows(source,target,width,height);}
// The paired-row SSE2 kernel alone (the candidate uses AVX2 where available).
__declspec(noinline) void sse2Kernel(const uint8_t* source,uint8_t* target,int width,int height){host::pixels::forceSse2Kernel=true;host::pixels::bgraToNv12PairedRows(source,target,width,height);host::pixels::forceSse2Kernel=false;}
void reference(const uint8_t* source,uint8_t* target,int width,int height) {
  const auto clip=[](int n){return uint8_t(std::clamp(n,0,255));};
  for(int y=0;y<height;++y)for(int x=0;x<width;++x){const auto* p=source+(size_t(y)*width+x)*4;target[size_t(y)*width+x]=clip(((66*p[2]+129*p[1]+25*p[0]+128)>>8)+16);}
  auto* uv=target+size_t(width)*height;
  for(int y=0;y<height;y+=2)for(int x=0;x<width;x+=2){int r=0,g=0,b=0;for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){const auto* p=source+(size_t(y+dy)*width+x+dx)*4;b+=p[0];g+=p[1];r+=p[2];}r/=4;g/=4;b/=4;uv[size_t(y/2)*width+x]=clip(((-38*r-74*g+112*b+128)>>8)+128);uv[size_t(y/2)*width+x+1]=clip(((112*r-94*g-18*b+128)>>8)+128);}
}
void fill(std::vector<uint8_t>& input,int pattern,uint32_t seed) {
  for(size_t i=0;i<input.size();++i){seed=seed*1664525u+1013904223u;input[i]=pattern==0?uint8_t(seed>>24):pattern==1?0:pattern==2?255:pattern==3?uint8_t(i*17):uint8_t((i%7)*42);}
}
struct Guarded {
  void* base=nullptr;uint8_t* data=nullptr;
  explicit Guarded(size_t bytes) {
    SYSTEM_INFO info{};GetSystemInfo(&info);const size_t page=info.dwPageSize,pages=(bytes+page-1)/page,total=(pages+1)*page;
    base=VirtualAlloc(nullptr,total,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);require(base!=nullptr,"Guarded allocation failed");DWORD old=0;
    if(!VirtualProtect(static_cast<uint8_t*>(base)+pages*page,page,PAGE_NOACCESS,&old)){VirtualFree(base,0,MEM_RELEASE);base=nullptr;throw std::runtime_error("Guard page protection failed");}
    data=static_cast<uint8_t*>(base)+pages*page-bytes;
  }
  ~Guarded(){if(base)VirtualFree(base,0,MEM_RELEASE);}
  Guarded(const Guarded&)=delete;Guarded& operator=(const Guarded&)=delete;
};
void verify(Converter convert) {
  const std::vector<std::pair<int,int>> sizes={{2,2},{4,2},{6,2},{10,6},{18,10},{66,34},{320,180},{1280,720},{1582,952},{1920,1080},{3704,1134},{3840,2160}};
  for(const auto [width,height]:sizes) {
    const size_t pixels=size_t(width)*height,bytes=pixels*3/2;
    std::vector<uint8_t> source(pixels*4+7),expected(bytes),old(bytes+13,0xa7),actual(bytes+13,0xa7);
    for(int pattern=0;pattern<5;++pattern) {
      fill(source,pattern,uint32_t(width*height+pattern));const auto original=source;
      reference(source.data()+1,expected.data(),width,height);
      baseline::bgraToNv12SeparatePasses(source.data()+1,old.data()+3,width,height);
      convert(source.data()+1,actual.data()+3,width,height);
      require(std::equal(expected.begin(),expected.end(),old.begin()+3),"Frozen baseline differs from independent scalar reference");
      require(std::equal(expected.begin(),expected.end(),actual.begin()+3),"Paired rows differ from independent scalar reference");
      require(source==original,"Source buffer changed");
      for(size_t i=0;i<3;++i)require(actual[i]==0xa7&&old[i]==0xa7,"Leading sentinel changed");
      for(size_t i=bytes+3;i<actual.size();++i)require(actual[i]==0xa7&&old[i]==0xa7,"Trailing sentinel changed");
      compared+=bytes;
    }
    Guarded input(pixels*4),output(bytes);fill(source,0,uint32_t(width));std::memcpy(input.data,source.data()+1,pixels*4);
    reference(input.data,expected.data(),width,height);convert(input.data,output.data,width,height);
    require(std::memcmp(output.data,expected.data(),bytes)==0,"Guarded conversion differs");
    for(size_t i=3;i<pixels*4;i+=4)input.data[i]^=0xff;
    convert(input.data,output.data,width,height);require(std::memcmp(output.data,expected.data(),bytes)==0,"Source alpha changed YUV pixels");
  }
  std::cout<<"{\"test\":\"paired_rows_conversion_exactness\",\"checks\":"<<checks<<",\"comparedBytes\":"<<compared<<",\"independentScalarReference\":true,\"unalignedBuffers\":true,\"tailWidths\":true,\"windowsGuardPages\":true,\"sourceAlphaIgnored\":true,\"screenCapture\":false}\n";
}
uint64_t fileTime(const FILETIME& time){return (uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;}
uint64_t cpuTime(){FILETIME created{},exited{},kernel{},user{};if(!GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user))throw std::runtime_error("Thread CPU clock unavailable");return fileTime(kernel)+fileTime(user);}
double percentile(std::vector<double> values,double quantile){std::sort(values.begin(),values.end());return values[std::min(values.size()-1,size_t(quantile*values.size()))];}
struct Timing{double wallMs=0,cpuMs=0;uint64_t checksum=0;};
Timing measure(Converter convert,const std::vector<uint8_t>& input,std::vector<uint8_t>& output,int width,int height,int frames) {
  const auto beforeCpu=cpuTime();const auto before=Clock::now();
  for(int i=0;i<frames;++i){convert(input.data(),output.data(),width,height);std::atomic_signal_fence(std::memory_order_seq_cst);}
  const auto after=Clock::now();const auto afterCpu=cpuTime();
  Timing result;result.wallMs=std::chrono::duration<double,std::milli>(after-before).count()/frames;result.cpuMs=double(afterCpu-beforeCpu)/10000/frames;
  for(const auto value:output)result.checksum=result.checksum*131+value;return result;
}
void benchmark(const char* name,Converter firstConverter,Converter secondConverter,int width,int height) {
  std::vector<uint8_t> input(size_t(width)*height*4),a(size_t(width)*height*3/2),b(a.size());fill(input,0,42);
  constexpr int frames=80,rounds=10;std::vector<double> oldWall,newWall,oldCpu,newCpu;uint64_t checksum=0;
  for(int i=0;i<20;++i){firstConverter(input.data(),a.data(),width,height);secondConverter(input.data(),b.data(),width,height);}
  for(int round=0;round<rounds;++round){Timing first,second;if(round&1){second=measure(secondConverter,input,b,width,height,frames);first=measure(firstConverter,input,a,width,height,frames);}else{first=measure(firstConverter,input,a,width,height,frames);second=measure(secondConverter,input,b,width,height,frames);}require(first.checksum==second.checksum&&a==b,"Timed conversion bytes differ");checksum+=first.checksum+uint64_t(round);oldWall.push_back(first.wallMs);newWall.push_back(second.wallMs);oldCpu.push_back(first.cpuMs);newCpu.push_back(second.cpuMs);}
  const auto average=[](const auto& v){return std::accumulate(v.begin(),v.end(),0.)/double(v.size());};
  std::cout<<"{\"test\":\""<<name<<"\",\"width\":"<<width<<",\"height\":"<<height<<",\"framesPerImplementation\":"<<frames*rounds<<",\"alternatingOrder\":true,\"baselineMeanMs\":"<<average(oldWall)<<",\"candidateMeanMs\":"<<average(newWall)<<",\"baselineP95RoundMs\":"<<percentile(oldWall,.95)<<",\"candidateP95RoundMs\":"<<percentile(newWall,.95)<<",\"baselineThreadCpuMeanMs\":"<<average(oldCpu)<<",\"candidateThreadCpuMeanMs\":"<<average(newCpu)<<",\"speedup\":"<<average(oldWall)/average(newWall)<<",\"checksum\":"<<checksum<<",\"actualCapture\":false,\"network\":false,\"vm149\":false}\n";
}
}
int main(int argc,char** argv){try{const bool only=argc==2&&std::string(argv[1])=="--verify-only";if(argc>2||(argc==2&&!only))throw std::runtime_error("Unsupported QA argument");SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);verify(candidate);verify(sse2Kernel);std::cout<<"{\"avx2Kernel\":"<<(host::pixels::avx2Supported()?"true":"false")<<"}\n";if(!only)for(const auto [w,h]:std::vector<std::pair<int,int>>{{1280,720},{1920,1080},{2560,1440},{3704,1134},{3840,2160},{5120,1440}}){benchmark("paired_rows_conversion_ab",baseline::bgraToNv12SeparatePasses,candidate,w,h);benchmark("avx2_kernel_ab",sse2Kernel,candidate,w,h);}return 0;}catch(const std::exception& error){std::cerr<<"Pixel conversion QA: "<<error.what()<<'\n';return 1;}}
