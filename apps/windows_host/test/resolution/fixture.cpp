#include "../../src/owned_output_size.hpp"
#include "../../src/capture_compose.hpp"
#include "../../src/capture_scale.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <utility>

int main(){
  unsigned checks=0,failures=0;
  const auto require=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::cout<<"FAIL "<<name<<'\n';}};
  const auto rejects=[](auto action){try{action();return false;}catch(const std::exception&){return true;}};
  try{
  for(const auto& [w,h]:std::array<std::pair<uint64_t,uint64_t>,6>{{{3840,2160},{5120,1440},{5120,2880},{4320,7680},{3840,3840},{7680,2160}}}){
    const auto layout=host::checkedFrameLayout(w,h);
    require(host::ownedOutputSize(w,h),"above4K representable geometry");
    require(layout&&layout->bgraBytes==w*h*4&&layout->nv12Bytes==w*h*3/2,"exact checked layout");
  }
  require(!host::ownedOutputSize(0,2160)&&!host::ownedOutputSize(319,180)&&!host::ownedOutputSize(5121,1440),"zero/minimum/alignment rejected");
  require(!host::checkedFrameLayout(UINT64_MAX,UINT64_MAX)&&!host::checkedFrameLayout(UINT32_MAX,UINT32_MAX),"overflow rejected before multiply");
  const auto one=host::checkedFrameLayout(3840,2160);
  require(one&&host::frameMemoryFits(*one,uint64_t(one->bgraBytes)*2+uint64_t(one->nv12Bytes)*4,2,4),"dual buffers fit exact runtime budget");
  require(one&&!host::frameMemoryFits(*one,one->bgraBytes-1,1,0),"insufficient runtime memory rejected");
  require(one&&!host::frameMemoryFits(*one,UINT64_MAX,UINT64_MAX,1),"working set count overflow rejected");
  require(host::encodedPictureSize(5120,2880)&&!host::encodedPictureSize(15,2880),"encoder geometry structural only");
  require(host::checkedFrameLayout(16387,1,false).has_value()&&!host::checkedFrameLayout(16387,1),"RGBA source above16K may be odd; NV12 remains even");
  {
    constexpr int width=16386,height=2;
    const auto layout=host::checkedFrameLayout(width,height);
    if(!layout)throw std::runtime_error("fixture layout unavailable");
    std::vector<uint8_t> source(layout->bgraBytes,0x7f),target(layout->bgraBytes,0),nv12(layout->nv12Bytes,0),reference(layout->nv12Bytes,0);
    host::capture::copyUnscaledSurface(target,width,height,source,width,height,0,0);
    require(target==source,"actual CPU composition above16K");
    require(rejects([&]{host::capture::copyUnscaledSurface(std::span<uint8_t>(target).first(target.size()-1),width,height,source,width,height,0,0);}),"composition rejects short actual buffer");
    host::capture::convertBlockToNv12(nv12,width,height,source.data(),layout->bgraStride,{0,0,width,height});
    host::pixels::bgraToNv12PairedRows(source.data(),reference.data(),width,height);
    require(nv12==reference,"above16K block conversion equals complete conversion");
    require(rejects([&]{host::capture::convertBlockToNv12({},INT32_MAX,2,source.data(),layout->bgraStride,{0,0,2,2});}),"NV12 stride overflow rejected without allocation");
  }
  for(const auto& [width,height]:std::array<std::pair<int,int>,2>{{{16386,2},{2,16386}}}){
    host::capture::AreaScaler scaler(width,height,2,2);
    std::vector<uint8_t> source(size_t(width)*height*4,0x7f),actual(16),reference(16);
    scaler.scale(source.data(),size_t(width)*4,actual.data(),8);
    scaler.scaleReference(source.data(),size_t(width)*4,reference.data(),8);
    require(actual==reference&&std::all_of(actual.begin(),actual.end(),[](uint8_t b){return b==0x7f;}),"above16K CPU scale matches reference and flat colour");
  }
  require(rejects([]{host::capture::AreaScaler bad(INT32_MAX,1,1,1);}),"scaler stride overflow rejected before allocation");
  require(rejects([]{host::capture::AreaScaler bad(0,1,1,1);}),"scaler zero source remains invalid");
  {
    host::capture::AreaScaler scaler(1,2,1,1);uint8_t source[8]{},target[4]{};
    require(rejects([&]{scaler.scale(source,SIZE_MAX,target,4);}),"scaler pitched address overflow rejected before read");
  }
  }catch(...){std::cout<<"SETUP_UNEXPECTED_EXCEPTION after "<<checks<<" checks; actualWindows=false\n";return 2;}
  std::cout<<checks<<" checks, "<<failures<<" failures; actualDriver=false; actualCapture=false; actualEncoder=false\n";
  return failures?1:0;
}
