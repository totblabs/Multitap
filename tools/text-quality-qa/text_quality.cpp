#include "media.hpp"
#include <wmcodecdsp.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>

namespace {
using host::Com;using host::check;
constexpr int width=1920,height=1080,frames=12,fps=60;
void require(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
std::string hex(HRESULT result){char out[16]{};sprintf_s(out,"0x%08lx",static_cast<unsigned long>(result));return out;}
Com<IMFMediaType> outputType(UINT32 profile){
  Com<IMFMediaType> type;check(MFCreateMediaType(&type),"Create capability output type");
  check(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Output video type");check(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_H264),"Output H264 type");
  check(type->SetUINT32(MF_MT_AVG_BITRATE,12000000),"Output bitrate");check(type->SetUINT32(MF_MT_MPEG2_PROFILE,profile),"Output profile");
  check(MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,640,360),"Output geometry");check(MFSetAttributeRatio(type.Get(),MF_MT_FRAME_RATE,60,1),"Output fps");
  check(MFSetAttributeRatio(type.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1),"Output aspect");check(type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive),"Output interlace");return type;
}
void capabilities(){
  Com<IMFTransform> transform;check(CoCreateInstance(CLSID_CMSH264EncoderMFT,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&transform)),"Create actual Microsoft software encoder");
  for(const UINT32 profile:{66u,77u,100u,110u,122u,244u}){
    Com<IMFTransform> probe;check(CoCreateInstance(CLSID_CMSH264EncoderMFT,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&probe)),"Create isolated profile probe");
    const auto type=outputType(profile);const auto hr=probe->SetOutputType(0,type.Get(),MFT_SET_TYPE_TEST_ONLY);
    const auto applied=probe->SetOutputType(0,type.Get(),0);
    std::cout<<"{\"kind\":\"mft-profile-test\",\"profile\":"<<profile<<",\"testOnlyHresult\":\""<<hex(hr)<<"\",\"setTypeHresult\":\""<<hex(applied)<<"\",\"declarationAccepted\":"<<(SUCCEEDED(applied)?"true":"false")<<",\"encodedSupportProven\":false}\n";
    if(profile==244&&SUCCEEDED(applied)){
      struct Input{const char* name;GUID id;};
      for(const auto& inputFormat:{Input{"AYUV444",MFVideoFormat_AYUV},Input{"Y410_10bit444",MFVideoFormat_Y410},Input{"ARGB32",MFVideoFormat_ARGB32},Input{"NV12",MFVideoFormat_NV12}}){
        Com<IMFMediaType> input;check(MFCreateMediaType(&input),"Create 444 profile input");input->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);input->SetGUID(MF_MT_SUBTYPE,inputFormat.id);
        MFSetAttributeSize(input.Get(),MF_MT_FRAME_SIZE,640,360);MFSetAttributeRatio(input.Get(),MF_MT_FRAME_RATE,60,1);MFSetAttributeRatio(input.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1);input->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
        const auto inputHr=probe->SetInputType(0,input.Get(),0);
        std::cout<<"{\"kind\":\"mft-444-profile-input\",\"format\":\""<<inputFormat.name<<"\",\"actualSetTypeHresult\":\""<<hex(inputHr)<<"\",\"declarationAccepted\":"<<(SUCCEEDED(inputHr)?"true":"false")<<",\"encodedSupportProven\":false}\n";
      }
    }
  }
  const auto type=outputType(66);check(transform->SetOutputType(0,type.Get(),0),"Set baseline capability output");
  struct Format{const char* name;GUID id;};
  for(const auto& format:{Format{"NV12",MFVideoFormat_NV12},Format{"I420",MFVideoFormat_I420},Format{"IYUV",MFVideoFormat_IYUV},Format{"YV12",MFVideoFormat_YV12},Format{"YUY2",MFVideoFormat_YUY2},Format{"AYUV444",MFVideoFormat_AYUV},Format{"Y410_10bit444",MFVideoFormat_Y410},Format{"ARGB32",MFVideoFormat_ARGB32},Format{"P010_10bit420",MFVideoFormat_P010}}){
    Com<IMFMediaType> input;check(MFCreateMediaType(&input),"Create capability input type");input->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);input->SetGUID(MF_MT_SUBTYPE,format.id);
    MFSetAttributeSize(input.Get(),MF_MT_FRAME_SIZE,640,360);MFSetAttributeRatio(input.Get(),MF_MT_FRAME_RATE,60,1);MFSetAttributeRatio(input.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1);input->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
    const auto hr=transform->SetInputType(0,input.Get(),MFT_SET_TYPE_TEST_ONLY);
    const auto actual=transform->SetInputType(0,input.Get(),0);
    std::cout<<"{\"kind\":\"mft-input-test\",\"format\":\""<<format.name<<"\",\"testOnlyHresult\":\""<<hex(hr)<<"\",\"setTypeHresult\":\""<<hex(actual)<<"\",\"declarationAccepted\":"<<(SUCCEEDED(actual)?"true":"false")<<",\"encodedSupportProven\":false}\n";
  }
  Com<ICodecAPI> codec;check(transform.As(&codec),"Read codec parameter bounds");
  VARIANT minimum,maximum,step;VariantInit(&minimum);VariantInit(&maximum);VariantInit(&step);
  const auto qp=codec->GetParameterRange(&CODECAPI_AVEncVideoEncodeQP,&minimum,&maximum,&step);
  std::cout<<"{\"kind\":\"mft-quality-qp-range\",\"hresult\":\""<<hex(qp)<<"\",\"minimum\":"<<(minimum.vt==VT_UI4?std::to_string(minimum.ulVal):"null")<<",\"maximum\":"<<(maximum.vt==VT_UI4?std::to_string(maximum.ulVal):"null")<<",\"losslessProven\":false}\n";
  VariantClear(&minimum);VariantClear(&maximum);VariantClear(&step);
  bool policyRejects444=false;try{host::Encoder encoder(640,360,60,4000000,{eAVEncH264VProfile_444,-1,false});}catch(const std::exception&){policyRejects444=true;}
  require(policyRejects444,"Product unexpectedly exposes unverified 444 tuning");
}
struct Scene {
  HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=nullptr;HGDIOBJ previous=nullptr;uint8_t* pixels=nullptr;
  std::vector<uint8_t> base,current;
  Scene(){BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);require(dc&&bitmap&&pixels,"Memory-only text scene failed");previous=SelectObject(dc,bitmap);
    RECT bounds{0,0,width,height};FillRect(dc,&bounds,reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));SetBkMode(dc,TRANSPARENT);int y=16;
    for(const int size:{12,14,16,20}){HFONT font=CreateFontW(-size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Malgun Gothic");require(font!=nullptr,"Test font failed");const auto old=SelectObject(dc,font);
      const std::wstring text=L"한글 선명도 가나다라마 ABC abc 0123456789 · "+std::to_wstring(size)+L" px";
      for(const auto color:{RGB(22,31,46),RGB(24,80,220),RGB(210,35,45)}){SetTextColor(dc,color);TextOutW(dc,24,y,text.c_str(),int(text.size()));y+=size+3;}SelectObject(dc,old);DeleteObject(font);y+=5;
    }GdiFlush();base.assign(pixels,pixels+size_t(width)*height*4);current.resize(base.size());
  }
  ~Scene(){if(previous)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}
  void render(int index,bool motion){current=base;if(!motion)return;
    for(int y=320;y<height;++y)for(int x=0;x<width;++x){auto* p=current.data()+(size_t(y)*width+x)*4;p[0]=uint8_t(x+index*7);p[1]=uint8_t(y+index*3);p[2]=uint8_t(((x+index*11)/48+(y+index*3)/48)%2?212:42);p[3]=255;}
  }
};
void write(const std::filesystem::path& path,const std::vector<uint8_t>& bytes){std::ofstream file(path,std::ios::binary);require(bool(file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()))),"Write generated fixture failed");}
void encode(const std::filesystem::path& root,bool motion,int bitrate){
  Scene scene;host::Encoder encoder(width,height,fps,bitrate);const auto applied=encoder.reportedTuning();require(applied.profile==eAVEncH264VProfile_Base,"Product baseline changed");
  const std::string name=std::string(motion?"motion":"static")+"-"+std::to_string(bitrate/1000000);std::ofstream stream(root/(name+".h264"),std::ios::binary);require(bool(stream),"Create fixture stream failed");
  std::vector<uint8_t> nv12(size_t(width)*height*3/2);size_t emitted=0,total=0;double encodeMs=0;
  const auto output=[&](host::Frame frame){++emitted;total+=frame.bytes.size();stream.write(reinterpret_cast<const char*>(frame.bytes.data()),std::streamsize(frame.bytes.size()));};
  for(int n=0;n<frames;++n){scene.render(n,motion);host::bgraToNv12(scene.current.data(),nv12.data(),width,height);const auto began=host::Clock::now();encoder.encode(nv12.data(),uint64_t(n)*1000000/fps,output);encodeMs+=std::chrono::duration<double,std::milli>(host::Clock::now()-began).count();}
  encoder.finish(output);require(emitted==frames&&total>0&&bool(stream),"Fixture stream incomplete");stream.close();
  write(root/(name+".bgra"),scene.current);write(root/(name+".nv12"),nv12);
  std::cout<<"{\"kind\":\"generated-text-encode\",\"name\":\""<<name<<"\",\"scene\":\""<<(motion?"motion":"static")<<"\",\"width\":"<<width<<",\"height\":"<<height<<",\"fpsTarget\":"<<fps<<",\"frames\":"<<emitted<<",\"targetBitrate\":"<<bitrate<<",\"actualBytes\":"<<total<<",\"mediaMbps\":"<<double(total)*8/(double(frames)/fps)/1000000<<",\"encodeMeanMs\":"<<encodeMs/frames<<",\"profileApi\":"<<applied.profile<<",\"desktopCaptured\":false,\"relayUsed\":false}\n";
}
}
int wmain(int argc,wchar_t** argv){try{
  const bool probeOnly=argc==2&&std::wstring(argv[1])==L"--probe";
  require(argc==2&&(probeOnly||std::filesystem::is_directory(argv[1])),"Pass --probe or an existing isolated fixture directory");
  SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);DWORD_PTR processMask=0,systemMask=0;
  if(GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask)){DWORD_PTR chosen=0;unsigned count=0;for(unsigned bit=0;bit<sizeof(processMask)*8&&count<2;++bit)if(processMask&(DWORD_PTR(1)<<bit)){chosen|=DWORD_PTR(1)<<bit;++count;}if(chosen)SetProcessAffinityMask(GetCurrentProcess(),chosen);}
  host::Runtime runtime;capabilities();if(probeOnly)return 0;const std::filesystem::path root(argv[1]);
  // Four short streams: separate chroma sampling loss from compression under
  // both light and moving content without repeating the 36-case profile sweep.
  encode(root,false,12000000);for(const int bitrate:{4000000,12000000,20000000})encode(root,true,bitrate);
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
