#include "remote_cursor.hpp"
#include <cstring>
#include <limits>
#include <new>

namespace multitap {
namespace {
size_t maskStride(unsigned width){return ((size_t(width)+31)/32)*4;}
bool canonicalMask(const RemoteCursorShape& shape){
  const auto stride=maskStride(shape.width),used=(size_t(shape.width)+7)/8;
  for(size_t row=0;row<shape.mask.size()/stride;++row){const auto* bytes=shape.mask.data()+row*stride;if(shape.width%8&&(bytes[used-1]&uint8_t(0xffu>>(shape.width%8))))return false;for(size_t i=used;i<stride;++i)if(bytes[i])return false;}
  return true;
}
struct BitmapOwner {HBITMAP value=nullptr;~BitmapOwner(){if(value)DeleteObject(value);}};
HBITMAP bitmap(unsigned width,unsigned height,bool monochrome,const std::vector<uint8_t>& bytes){
  struct Info {BITMAPINFOHEADER header{};RGBQUAD colors[2]{};} info;
  info.header.biSize=sizeof(BITMAPINFOHEADER);info.header.biWidth=LONG(width);info.header.biHeight=-LONG(height);info.header.biPlanes=1;info.header.biBitCount=WORD(monochrome?1:32);info.header.biCompression=BI_RGB;if(monochrome){info.header.biClrUsed=2;info.colors[1]={255,255,255,0};}
  void* pixels=nullptr;HBITMAP result=CreateDIBSection(nullptr,reinterpret_cast<BITMAPINFO*>(&info),DIB_RGB_COLORS,&pixels,nullptr,0);
  if(!result||!pixels){if(result)DeleteObject(result);return nullptr;}std::memcpy(pixels,bytes.data(),bytes.size());return result;
}
}
size_t RemoteCursorMaskStride(unsigned width) noexcept {return maskStride(width);}
HCURSOR CreateRemoteCursorHandle(const RemoteCursorShape& shape) noexcept {
  try{
    if(!validRemoteCursorShape(shape))return nullptr;
    BitmapOwner mask{bitmap(shape.width,unsigned(shape.height)*(shape.format==RemoteCursorFormat::MonochromeAndXor?2:1),true,shape.mask)};
    if(!mask.value)return nullptr;
    BitmapOwner color;if(shape.format==RemoteCursorFormat::ColorBgraAnd){color.value=bitmap(shape.width,shape.height,false,shape.color);if(!color.value)return nullptr;}
    ICONINFO info{};info.fIcon=FALSE;info.xHotspot=shape.hotspotX;info.yHotspot=shape.hotspotY;info.hbmMask=mask.value;info.hbmColor=color.value;
    return reinterpret_cast<HCURSOR>(CreateIconIndirect(&info));
  }catch(...){return nullptr;}
}
bool validRemoteCursorShape(const RemoteCursorShape& shape) noexcept {
  if(!shape.width||!shape.height||shape.width>256||shape.height>256||shape.hotspotX>=shape.width||shape.hotspotY>=shape.height)return false;
  const auto rows=size_t(shape.height)*(shape.format==RemoteCursorFormat::MonochromeAndXor?2:1);
  if(shape.mask.size()!=maskStride(shape.width)*rows)return false;
  if(shape.format==RemoteCursorFormat::ColorBgraAnd){if(shape.color.size()!=size_t(shape.width)*shape.height*4)return false;}
  else if(shape.format!=RemoteCursorFormat::MonochromeAndXor||!shape.color.empty())return false;
  return canonicalMask(shape);
}
RemoteCursor::~RemoteCursor(){clear();}
void RemoteCursor::clear(){
  active_=visible_=false;
  if(cursor_){if(GetCurrentThreadId()==owner_&&GetCursor()==cursor_)SetCursor(nullptr);DestroyIcon(reinterpret_cast<HICON>(cursor_));cursor_=nullptr;}
}
bool RemoteCursor::matches(uint64_t lease,uint16_t epoch)const{return GetCurrentThreadId()==owner_&&bound_&&lease==lease_&&epoch==epoch_;}
bool RemoteCursor::Bind(uint64_t lease,uint16_t epoch){
  if(GetCurrentThreadId()!=owner_||!lease||lease>uint64_t((std::numeric_limits<int64_t>::max)()))return false;
  if(bound_&&lease==lease_&&epoch==epoch_)return true;
  if(lease<highestLease_||(lease==highestLease_&&(!bound_||epoch<=epoch_)))return false;
  clear();lease_=highestLease_=lease;epoch_=epoch;bound_=true;return true;
}
HRESULT RemoteCursor::SetShape(uint64_t lease,uint16_t epoch,const RemoteCursorShape& shape){
  if(!matches(lease,epoch)||!validRemoteCursorShape(shape))return E_INVALIDARG;
  const auto created=CreateRemoteCursorHandle(shape);if(!created)return HRESULT_FROM_WIN32(GetLastError()?GetLastError():ERROR_NOT_ENOUGH_MEMORY);
  const bool wasActive=active_,wasVisible=visible_;clear();cursor_=created;active_=wasActive;visible_=wasVisible;return S_OK;
}
bool RemoteCursor::SetActive(uint64_t lease,uint16_t epoch,bool active,bool visible){if(!matches(lease,epoch)||(active&&visible&&!cursor_))return false;active_=active;visible_=visible;return true;}
bool RemoteCursor::Unbind(uint64_t lease){if(GetCurrentThreadId()!=owner_||!bound_||lease!=lease_)return false;clear();bound_=false;return true;}
RemoteCursorSelection RemoteCursor::Selection(uint64_t lease,uint16_t epoch)const{if(!matches(lease,epoch)||!active_||(visible_&&!cursor_))return {};return {true,visible_?cursor_:nullptr};}
}
