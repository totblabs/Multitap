#include "media.hpp"
#include "remote_decoder.h"
#include "../../native/windows_video/color.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numeric>
#include <string>

// Independent scalar BT.601 studio-range reference: RGB is averaged before
// converting each 2x2 chroma block. No screen capture or remote connection.
static void reference(const uint8_t* source,uint8_t* target,int w,int h) {
  for(int y=0;y<h;++y)for(int x=0;x<w;++x){const auto* p=source+(size_t(y)*w+x)*4;target[size_t(y)*w+x]=uint8_t(((66*p[2]+129*p[1]+25*p[0]+128)>>8)+16);}
  auto* uv=target+size_t(w)*h;
  for(int y=0;y<h;y+=2)for(int x=0;x<w;x+=2){int b=0,g=0,r=0;for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){const auto* p=source+(size_t(y+dy)*w+x+dx)*4;b+=p[0];g+=p[1];r+=p[2];}b/=4;g/=4;r/=4;uv[size_t(y/2)*w+x]=uint8_t(((-38*r-74*g+112*b+128)>>8)+128);uv[size_t(y/2)*w+x+1]=uint8_t(((112*r-94*g-18*b+128)>>8)+128);}
}
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
static void inverseReference(const uint8_t* nv12,uint8_t* rgba,uint32_t w,uint32_t h,uint32_t stride,uint32_t planeHeight,bool bt709,bool full){
  const int ys=full?256:298,rv=full?(bt709?403:359):(bt709?459:409),gu=full?(bt709?48:88):(bt709?55:100),gv=full?(bt709?120:183):(bt709?136:208),bu=full?(bt709?475:454):(bt709?541:516);
  const auto* uv=nv12+size_t(stride)*planeHeight;const auto clip=[](int n){return uint8_t(std::clamp(n,0,255));};
  for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const int yy=std::max(0,int(nv12[size_t(y)*stride+x])-(full?0:16));const size_t index=size_t(y/2)*stride+(x&~1u);const int u=uv[index]-128,v=uv[index+1]-128;const auto indexOut=(size_t(y)*w+x)*4;rgba[indexOut]=clip((ys*yy+rv*v+128)>>8);rgba[indexOut+1]=clip((ys*yy-gu*u-gv*v+128)>>8);rgba[indexOut+2]=clip((ys*yy+bu*u+128)>>8);rgba[indexOut+3]=255;}
}
static void exactInverse(){
  uint32_t random=0x793151af;uint64_t compared=0;
  for(const uint32_t w:{16u,18u,30u,66u,320u,1030u})for(const uint32_t h:{2u,6u,64u})for(const bool padded:{false,true}){
    const auto stride=padded?w+6:w,planeHeight=padded?h+4:h;const size_t size=size_t(w)*h*4;
    std::vector<uint8_t> nv12(size_t(stride)*planeHeight*3/2+2),expected(size),actual(size+6,0xa7);
    for(int pattern=0;pattern<4;++pattern){for(size_t n=1;n+1<nv12.size();++n){random=random*1664525u+1013904223u;nv12[n]=pattern==0?uint8_t(random>>24):pattern==1?0:pattern==2?255:uint8_t(n*29);}
      for(const bool bt709:{false,true})for(const bool full:{false,true}){
        inverseReference(nv12.data()+1,expected.data(),w,h,stride,planeHeight,bt709,full);multitap::video::nv12ToRgba(nv12.data()+1,actual.data()+3,w,h,stride,planeHeight,bt709,full);
        require(std::equal(expected.begin(),expected.end(),actual.begin()+3),"inverse SIMD differs from scalar matrix/range reference");require(actual.front()==0xa7&&actual[2]==0xa7&&actual[size+3]==0xa7&&actual.back()==0xa7,"inverse conversion guard overwritten");compared+=size;
      }
    }
  }
  std::cout<<"{\"test\":\"inverse_scalar_simd_exact_bytes\",\"comparedBytes\":"<<compared<<",\"matrices\":[\"BT601\",\"BT709\"],\"ranges\":[\"limited\",\"full\"],\"paddedPlanes\":true,\"unalignedSourceDestination\":true,\"passed\":true}\n";
}
static void exactConversion(){
  uint32_t random=0x8e518727;uint64_t compared=0;
  for(const int w:{16,18,30,66,320,1030,1920,3840})for(const int h:{2,6,64}){
    const size_t pixels=size_t(w)*h,bytes=pixels*3/2;
    std::vector<uint8_t> bgra(pixels*4+2),expected(bytes),actual(bytes+6,0xa7);
    for(int pattern=0;pattern<5;++pattern){
      for(size_t n=0;n<pixels*4;++n){random=random*1664525u+1013904223u;bgra[n+1]=pattern==0?uint8_t(random>>24):pattern==1?0:pattern==2?255:pattern==3?uint8_t((n*17)%256):uint8_t((n%7)*42);}
      reference(bgra.data()+1,expected.data(),w,h);host::bgraToNv12(bgra.data()+1,actual.data()+3,w,h);
      require(std::equal(expected.begin(),expected.end(),actual.begin()+3),"SIMD bytes differ from scalar BT601 reference");
      require(actual[0]==0xa7&&actual[1]==0xa7&&actual[2]==0xa7&&actual[bytes+3]==0xa7&&actual[bytes+4]==0xa7&&actual[bytes+5]==0xa7,"conversion guard overwritten");compared+=bytes;
    }
  }
  std::cout<<"{\"test\":\"scalar_simd_exact_bytes\",\"comparedBytes\":"<<compared<<",\"unalignedSourceDestination\":true,\"tailWidths\":true,\"passed\":true}\n";
}
static double milliseconds(host::Clock::time_point a,host::Clock::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();}
static double p95(std::vector<double> values){std::sort(values.begin(),values.end());return values[size_t(std::ceil(.95*double(values.size())))-1];}
static void conversionPerformance(int w,int h){
  std::vector<uint8_t> source(size_t(w)*h*4),output(size_t(w)*h*3/2);
  for(size_t n=0;n<source.size();++n)source[n]=uint8_t(n*17+n/67);
  constexpr int frames=100;std::vector<double> scalar,simd;uint64_t guard=0;
  for(int n=0;n<frames+10;++n){const auto a=host::Clock::now();reference(source.data(),output.data(),w,h);const auto b=host::Clock::now();guard+=output[size_t(n)%output.size()];host::bgraToNv12(source.data(),output.data(),w,h);const auto c=host::Clock::now();guard+=output[size_t(n)%output.size()];if(n>=10){scalar.push_back(milliseconds(a,b));simd.push_back(milliseconds(b,c));}}
  const double oldMean=std::accumulate(scalar.begin(),scalar.end(),0.)/frames,newMean=std::accumulate(simd.begin(),simd.end(),0.)/frames;
  std::cout<<"{\"test\":\"cpu_conversion_microbenchmark\",\"width\":"<<w<<",\"height\":"<<h<<",\"frames\":"<<frames<<",\"scalarMeanMs\":"<<oldMean<<",\"simdMeanMs\":"<<newMean<<",\"scalarP95Ms\":"<<p95(scalar)<<",\"simdP95Ms\":"<<p95(simd)<<",\"speedup\":"<<oldMean/newMean<<",\"guard\":"<<guard<<",\"remoteSessionBenchmark\":false}\n";
}
static void codecRoundtrip(){
  constexpr int w=320,h=128,frames=16;
  const std::array<std::array<uint8_t,3>,8> colors{{{{255,0,0}},{{0,255,0}},{{0,0,255}},{{255,255,255}},{{0,0,0}},{{128,128,128}},{{255,255,0}},{{0,255,255}}}};
  std::vector<uint8_t> bgra(size_t(w)*h*4),raw(size_t(w)*h*3/2);
  for(int y=0;y<h;++y)for(int x=0;x<w;++x){auto* p=bgra.data()+(size_t(y)*w+x)*4;const auto& color=colors[size_t(x/40)];p[0]=color[2];p[1]=color[1];p[2]=color[0];p[3]=255;}
  host::bgraToNv12(bgra.data(),raw.data(),w,h);host::Encoder encoder(w,h,60,4000000);multitap::RemoteDecoder decoder;host::check(decoder.Initialize(w,h),"Initialize production decoder");
  size_t decoded=0,encoded=0;int maximumError=0;
  const auto receive=[&](multitap::DecodedFrame&& frame){require(frame.width==w&&frame.height==h,"decoded size differs");++decoded;for(size_t band=0;band<colors.size();++band)for(int y=32;y<96;y+=8)for(int x=int(band)*40+12;x<int(band)*40+28;x+=4){const auto* p=frame.rgba.data()+(size_t(y)*w+x)*4;for(int c=0;c<3;++c)maximumError=std::max(maximumError,std::abs(int(p[c])-colors[band][size_t(c)]));require(p[3]==255,"decoded alpha differs");}};
  const auto output=[&](host::Frame frame){++encoded;host::check(decoder.Decode(frame.bytes.data(),frame.bytes.size(),frame.ptsUs,receive),"Decode production encoder packet");};
  for(int n=0;n<frames;++n)encoder.encode(raw.data(),uint64_t(n)*1000000/60,output);encoder.finish(output);host::check(decoder.Drain(receive),"Drain production decoder");
  require(encoded==frames&&decoded==frames,"incomplete codec roundtrip");require(maximumError<=5,"encoded colour bars deviate beyond 5 RGB levels");
  std::cout<<"{\"test\":\"production_h264_colour_roundtrip\",\"encodedFrames\":"<<encoded<<",\"decodedFrames\":"<<decoded<<",\"maxRgbChannelError\":"<<maximumError<<",\"matrix\":\"BT601_limited\",\"syntheticPixelsOnly\":true,\"passed\":true}\n";
}
static void codecPerformance(){
  constexpr int w=1920,h=1080,frames=90;
  std::vector<uint8_t> pixels(size_t(w)*h*4),raw(size_t(w)*h*3/2);
  for(int y=0;y<h;++y)for(int x=0;x<w;++x){auto* p=pixels.data()+(size_t(y)*w+x)*4;const bool ink=y%27<2&&x%17<9;p[0]=p[1]=p[2]=ink?24:244;p[3]=255;}
  host::Encoder encoder(w,h,60,12000000);multitap::RemoteDecoder decoder;host::check(decoder.Initialize(w,h),"Initialize benchmark decoder");
  std::vector<double> conversion,encoding,decoding;size_t decoded=0,encoded=0;uint64_t checksum=0,bytes=0;
  const auto receive=[&](multitap::DecodedFrame&& frame){++decoded;checksum+=frame.rgba[(decoded*7919)%frame.rgba.size()];};
  const auto output=[&](host::Frame frame){++encoded;bytes+=frame.bytes.size();const auto a=host::Clock::now();host::check(decoder.Decode(frame.bytes.data(),frame.bytes.size(),frame.ptsUs,receive),"Decode benchmark packet");decoding.push_back(milliseconds(a,host::Clock::now()));};
  for(int n=0;n<frames;++n){pixels[size_t(n)*4096]=uint8_t(n);const auto a=host::Clock::now();host::bgraToNv12(pixels.data(),raw.data(),w,h);const auto b=host::Clock::now();const auto before=std::accumulate(decoding.begin(),decoding.end(),0.);encoder.encode(raw.data(),uint64_t(n)*1000000/60,output);const auto c=host::Clock::now();conversion.push_back(milliseconds(a,b));encoding.push_back(milliseconds(b,c)-(std::accumulate(decoding.begin(),decoding.end(),0.)-before));}
  encoder.finish(output);host::check(decoder.Drain(receive),"Drain benchmark decoder");require(encoded==frames&&decoded==frames,"incomplete codec benchmark");
  std::cout<<"{\"test\":\"cpu_codec_pipeline_microbenchmark\",\"width\":"<<w<<",\"height\":"<<h<<",\"frames\":"<<frames<<",\"convertMeanMs\":"<<std::accumulate(conversion.begin(),conversion.end(),0.)/frames<<",\"encodeMeanMs\":"<<std::accumulate(encoding.begin(),encoding.end(),0.)/frames<<",\"decodeConvertMeanMs\":"<<std::accumulate(decoding.begin(),decoding.end(),0.)/double(decoding.size())<<",\"encodeP95Ms\":"<<p95(encoding)<<",\"decodeConvertP95Ms\":"<<p95(decoding)<<",\"checksum\":"<<checksum<<",\"bytes\":"<<bytes<<",\"actualCapture\":false,\"networkTransport\":false,\"flutterPresentation\":false}\n";
}
int main(){try{host::Runtime runtime;exactConversion();exactInverse();codecRoundtrip();conversionPerformance(1920,1080);conversionPerformance(3840,1080);codecPerformance();return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
