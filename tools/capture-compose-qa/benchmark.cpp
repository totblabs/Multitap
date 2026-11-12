#include "capture_compose.hpp"
#include "media.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numeric>
namespace {
using host::Clock;
unsigned checks=0;
void require(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);++checks;}
struct Dib {
  HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=nullptr;HGDIOBJ previous=nullptr;
  uint8_t* pixels=nullptr;BITMAPINFO info{};int width,height;
  Dib(int w,int h):width(w),height(h){
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
    require(dc&&bitmap&&pixels,"Cannot create memory-only capture DIB");
    previous=SelectObject(dc,bitmap);SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);
  }
  ~Dib(){if(previous)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}
  std::span<uint8_t> bytes(){return {pixels,size_t(width)*height*4};}
};
std::vector<uint8_t> pattern(int width,int height,uint32_t seed=0x91fabb21){
  std::vector<uint8_t> result(size_t(width)*height*4);
  for(auto& value:result){seed=seed*1664525+1013904223;value=uint8_t(seed>>24);}return result;
}
double percentile(std::vector<double> values,double fraction){std::sort(values.begin(),values.end());return values[size_t(std::ceil(values.size()*fraction))-1];}
void reference(Dib& target,std::span<const uint8_t> source,int width,int height,int left=0,int top=0){
  auto info=target.info;info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
  if(!StretchDIBits(target.dc,left,top,width,height,0,0,width,height,source.data(),&info,DIB_RGB_COLORS,SRCCOPY))
    throw std::runtime_error("Memory-only GDI composition failed");
  GdiFlush();
}
void equalRgb(std::span<const uint8_t> first,std::span<const uint8_t> second){
  require(first.size()==second.size(),"Composition geometry changed");
  for(size_t n=0;n<first.size();++n)if(n%4!=3&&first[n]!=second[n])throw std::runtime_error("Composition changed RGB pixels");++checks;
}
void equalNv12(std::span<const uint8_t> first,std::span<const uint8_t> second,int width,int height){
  std::vector<uint8_t> left(size_t(width)*height*3/2),right(left.size());
  host::bgraToNv12(first.data(),left.data(),width,height);host::bgraToNv12(second.data(),right.data(),width,height);
  require(left==right,"Composition changed the actual encoder NV12 input");
}
void guardTests(){
  constexpr size_t guard=17;constexpr int width=18,height=14;
  auto source=pattern(10,6);std::vector<uint8_t> stored(size_t(width)*height*4+guard*2,0xa5);
  const auto target=std::span(stored).subspan(guard,stored.size()-guard*2);
  host::capture::copyUnscaledSurface(target,width,height,source,10,6,4,2);
  for(int y=0;y<height;++y)for(int x=0;x<width;++x)for(int c=0;c<4;++c){const auto actual=target[(size_t(y)*width+x)*4+c];const auto expected=(x>=4&&x<14&&y>=2&&y<8)?source[(size_t(y-2)*10+x-4)*4+c]:0xa5;require(actual==expected,"Offset surface overwrote a neighbouring pixel");}
  require(std::all_of(stored.begin(),stored.begin()+guard,[](uint8_t n){return n==0xa5;})&&std::all_of(stored.end()-guard,stored.end(),[](uint8_t n){return n==0xa5;}),"Unaligned DIB guard changed");
  std::fill(target.begin(),target.end(),uint8_t(0));host::capture::copyUnscaledSurface(target,width,height,source,10,6,4,2);
  const auto base=std::vector<uint8_t>(target.begin(),target.end());target[(size_t(3)*width+5)*4]=255;
  host::capture::copyUnscaledSurface(target,width,height,source,10,6,4,2);
  require(std::equal(target.begin(),target.end(),base.begin()),"Cursor changed cached surface on a duplicate frame");
  const auto invalid=[&](int tw,int th,int sw,int sh,int x,int y,size_t targetSize,size_t sourceSize){bool threw=false;const auto before=stored;try{host::capture::copyUnscaledSurface(target.first(targetSize),tw,th,std::span(source).first(sourceSize),sw,sh,x,y);}catch(const std::runtime_error&){threw=true;}require(threw&&before==stored,"Invalid geometry wrote outside its declared surface");};
  invalid(0,height,10,6,0,0,target.size(),source.size());invalid(width,-1,10,6,0,0,target.size(),source.size());
  invalid(16385,height,10,6,0,0,target.size(),source.size());invalid(width,height,-1,6,0,0,target.size(),source.size());
  invalid(width,height,10,0,0,0,target.size(),source.size());invalid(width,height,20,6,0,0,target.size(),source.size());
  invalid(width,height,10,16,0,0,target.size(),source.size());invalid(width,height,10,6,-1,0,target.size(),source.size());
  invalid(width,height,10,6,0,-1,target.size(),source.size());invalid(width,height,10,6,9,0,target.size(),source.size());
  invalid(width,height,10,6,0,9,target.size(),source.size());invalid(width,height,10,6,0,0,target.size()-1,source.size());
  invalid(width,height,10,6,0,0,target.size(),source.size()-1);
}
void dualTests(){for(const bool vertical:{false,true}){
  const int sw=640,sh=360,width=vertical?sw:sw*2,height=vertical?sh*2:sh;
  auto first=pattern(sw,sh),second=pattern(sw,sh,42);Dib before(width,height),after(width,height);
  const int left=vertical?0:sw,top=vertical?sh:0;
  reference(before,first,sw,sh);reference(before,second,sw,sh,left,top);
  host::capture::copyUnscaledSurface(after.bytes(),width,height,first,sw,sh,0,0);
  host::capture::copyUnscaledSurface(after.bytes(),width,height,second,sw,sh,left,top);
  equalRgb(before.bytes(),after.bytes());equalNv12(before.bytes(),after.bytes(),width,height);
}}
}
int main(){try{
  guardTests();dualTests();uint64_t checksum=0;
  for(const auto& geometry:{std::pair{1280,720},std::pair{1920,1080},std::pair{2560,1440},std::pair{3840,1080}}){
    const auto [width,height]=geometry;Dib before(width,height),after(width,height);const auto source=pattern(width,height);
    std::vector<double> gdi,copy;
    for(int n=0;n<180;++n){const auto trial=[&](bool legacy){const auto began=Clock::now();
      if(legacy)reference(before,source,width,height);else {GdiFlush();host::capture::copyUnscaledSurface(after.bytes(),width,height,source,width,height,0,0);}
      const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-began).count();
      checksum+=(legacy?before.pixels:after.pixels)[(size_t(n)*7919)%source.size()];
      if(n>=30)(legacy?gdi:copy).push_back(elapsed);
    };trial(n%2==0);trial(n%2!=0);}
    equalRgb(before.bytes(),after.bytes());equalNv12(before.bytes(),after.bytes(),width,height);
    const auto mean=[](const auto& values){return std::accumulate(values.begin(),values.end(),0.0)/values.size();};
    std::cout<<"{\"kind\":\"capture-unscaled-compose\",\"width\":"<<width<<",\"height\":"<<height<<",\"samplesPerPath\":"<<copy.size()
      <<",\"legacyMeanMs\":"<<mean(gdi)<<",\"legacyP50Ms\":"<<percentile(gdi,.5)<<",\"legacyP95Ms\":"<<percentile(gdi,.95)
      <<",\"directMeanMs\":"<<mean(copy)<<",\"directP50Ms\":"<<percentile(copy,.5)<<",\"directP95Ms\":"<<percentile(copy,.95)
      <<",\"rgbMismatchBytes\":0,\"encoderInputIdentical\":true,\"checksum\":"<<checksum<<",\"desktopCaptured\":false,\"relayUsed\":false}\n";
  }
  std::cout<<"{\"kind\":\"capture-unscaled-compose-regression\",\"checks\":"<<checks<<",\"dualHorizontalVertical\":true,\"guardAndInvalidGeometry\":true,\"cachedCursorBaseRestored\":true,\"serviceDisplaySettingsChanged\":false}\n";
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
