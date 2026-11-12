#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <sddl.h>
#include <uxtheme.h>
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <servprov.h>
#include <string>
#include <filesystem>
#include <thread>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>
#include "product_versions.h"
#include "../../../native/windows_admin/admin_required.hpp"
#include "../../../native/windows_admin/update_handoff.hpp"
#include "../../../native/windows_admin/update_relaunch.hpp"
#include "setup_text.hpp"
#include "setup_payload.hpp"
#include "setup_uninstall.hpp"
#include "setup_uninstall_reason.hpp"
#include "setup_product_icon.hpp"
#ifdef MULTITAP_SETUP_QA
#include "setup_payload_selftest.hpp"
#endif
#pragma comment(lib,"oleaut32.lib")
#pragma comment(lib,"uuid.lib")
#pragma comment(lib,"d2d1.lib")
#pragma comment(lib,"dwrite.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
namespace {
// Every text these windows show comes from setup_text.hpp in the person's
// Windows display language (Korean or English, docs/i18n.md).
using S=multitap::setup_text::Id;
inline const wchar_t* tx(S id){return multitap::setup_text::text(id);}
inline std::wstring txf(S id,std::initializer_list<std::wstring> arguments){return multitap::setup_text::format(id,arguments);}
inline bool koreanUi(){return multitap::ui::korean();}
constexpr UINT ProgressMessage=WM_APP+4;
constexpr UINT ReaderFinishedMessage=WM_APP+6;
constexpr UINT StartFinishedMessage=WM_APP+7;
constexpr int Width=560,Height=470,TotalSteps=6;
constexpr COLORREF Ink=RGB(233,241,243),Muted=RGB(143,161,169),Blue=RGB(245,166,35),Canvas=RGB(14,20,24);
constexpr COLORREF Surface=RGB(20,28,33),Line=RGB(31,43,50),ButtonInk=RGB(30,21,3);
constexpr COLORREF Amber=RGB(255,210,74),AmberSurface=RGB(40,33,20),CompleteSurface=RGB(19,38,32),FailedSurface=RGB(42,22,24);
constexpr COLORREF Success=RGB(98,227,169),Danger=RGB(255,138,122);
constexpr COLORREF Faint=RGB(94,106,115),SuccessLine=RGB(46,96,74),AmberLine=RGB(92,70,30),FailedLine=RGB(96,46,47);
constexpr COLORREF Body=RGB(190,203,208);
Gdiplus::Color color(COLORREF value){return Gdiplus::Color(255,GetRValue(value),GetGValue(value),GetBValue(value));}
// --- Type --------------------------------------------------------------------
// The client's UI font, Pretendard (setup_fonts.rc), private to this process:
// DirectWrite draws every text (GDI+ cannot render its CFF outlines) and GDI
// gets the same faces for the details box. Shapes stay GDI+. Without the
// embedded faces (Windows before 10 1703): Malgun Gothic.
const void* resourceBytes(int id,DWORD& size){
  const HMODULE module=GetModuleHandleW(nullptr);const HRSRC found=FindResourceW(module,MAKEINTRESOURCEW(id),MAKEINTRESOURCEW(10));if(!found)return nullptr;
  const HGLOBAL loaded=LoadResource(module,found);size=SizeofResource(module,found);return loaded&&size?LockResource(loaded):nullptr;
}
struct Type {
  IDWriteFactory* write=nullptr;IDWriteFontCollection* collection=nullptr;ID2D1Factory* d2d=nullptr;ID2D1DCRenderTarget* target=nullptr;
  std::wstring family=L"Malgun Gothic";bool gdiEmbedded=false;double cap[3]{.7,.7,.7};
};
Type loadType(){
  Type t;
  if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&t.write))))t.write=nullptr;
  if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory),nullptr,reinterpret_cast<void**>(&t.d2d))))t.d2d=nullptr;
  IDWriteFactory5* modern=nullptr;
  if(t.write&&SUCCEEDED(t.write->QueryInterface(__uuidof(IDWriteFactory5),reinterpret_cast<void**>(&modern)))){
    IDWriteInMemoryFontFileLoader* loader=nullptr;IDWriteFontSetBuilder1* builder=nullptr;
    // The loader stays registered for the process: the collection refers to it.
    if(SUCCEEDED(modern->CreateInMemoryFontFileLoader(&loader))&&SUCCEEDED(modern->RegisterFontFileLoader(loader))&&SUCCEEDED(modern->CreateFontSetBuilder(&builder))){
      bool complete=true;
      for(const int id:{201,202,203}){
        DWORD size=0;const void* bytes=resourceBytes(id,size);IDWriteFontFile* file=nullptr;
        if(!bytes||FAILED(loader->CreateInMemoryFontFileReference(modern,bytes,size,nullptr,&file))||FAILED(builder->AddFontFile(file)))complete=false;
        if(file)file->Release();
      }
      IDWriteFontSet* set=nullptr;IDWriteFontCollection1* fonts=nullptr;UINT32 index=0;BOOL exists=FALSE;
      if(complete&&SUCCEEDED(builder->CreateFontSet(&set))&&SUCCEEDED(modern->CreateFontCollectionFromFontSet(set,&fonts))&&SUCCEEDED(fonts->FindFamilyName(L"Pretendard",&index,&exists))&&exists){t.collection=fonts;fonts=nullptr;t.family=L"Pretendard";}
      if(fonts)fonts->Release();if(set)set->Release();
    }
    if(builder)builder->Release();modern->Release();
  }
  for(const int id:{201,202,203}){DWORD size=0,count=0;const void* bytes=resourceBytes(id,size);if(bytes&&AddFontMemResourceEx(const_cast<void*>(bytes),size,nullptr,&count)&&count)t.gdiEmbedded=true;}
  // Cap height per weight: a line is centred on its visual middle.
  IDWriteFontCollection* fonts=t.collection;if(!fonts&&t.write)t.write->GetSystemFontCollection(&fonts);
  if(fonts){
    UINT32 index=0;BOOL exists=FALSE;IDWriteFontFamily* family=nullptr;
    if(SUCCEEDED(fonts->FindFamilyName(t.family.c_str(),&index,&exists))&&exists&&SUCCEEDED(fonts->GetFontFamily(index,&family))){
      const DWRITE_FONT_WEIGHT weights[3]{DWRITE_FONT_WEIGHT_REGULAR,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_FONT_WEIGHT_BOLD};
      for(int i=0;i<3;i++){IDWriteFont* font=nullptr;if(SUCCEEDED(family->GetFirstMatchingFont(weights[i],DWRITE_FONT_STRETCH_NORMAL,DWRITE_FONT_STYLE_NORMAL,&font))){DWRITE_FONT_METRICS m{};font->GetMetrics(&m);if(m.designUnitsPerEm&&m.capHeight)t.cap[i]=double(m.capHeight)/m.designUnitsPerEm;font->Release();}}
      family->Release();
    }
    if(fonts!=t.collection)fonts->Release();
  }
  return t;
}
Type& type(){static Type t=loadType();return t;}
// The GDI face (the details box, the system fallback).
const wchar_t* uiFamily(){return type().gdiEmbedded?L"Pretendard":L"Malgun Gothic";}
enum TextFlag:unsigned {Bold=1,CenterH=2,CenterV=4,Right=8,Single=16,Semi=32};
int weightIndex(unsigned flags){return (flags&Bold)?2:(flags&Semi)?1:0;}
IDWriteTextFormat* textFormat(double px,unsigned flags){
  auto& t=type();if(!t.write||px<=0)return nullptr;
  static const DWRITE_FONT_WEIGHT weights[3]{DWRITE_FONT_WEIGHT_REGULAR,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_FONT_WEIGHT_BOLD};
  IDWriteTextFormat* format=nullptr;
  if(FAILED(t.write->CreateTextFormat(t.family.c_str(),t.collection,weights[weightIndex(flags)],DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,float(px),koreanUi()?L"ko-kr":L"en-us",&format)))return nullptr;
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};IDWriteInlineObject* sign=nullptr;
  if(SUCCEEDED(t.write->CreateEllipsisTrimmingSign(format,&sign))){format->SetTrimming(&trimming,sign);sign->Release();}
  return format;
}
float textWidth(IDWriteTextFormat* format,const std::wstring& value){
  IDWriteTextLayout* layout=nullptr;float width=0;
  if(format&&SUCCEEDED(type().write->CreateTextLayout(value.c_str(),UINT32(value.size()),format,1e6f,1e4f,&layout))){DWRITE_TEXT_METRICS m{};if(SUCCEEDED(layout->GetMetrics(&m)))width=m.widthIncludingTrailingWhitespace;layout->Release();}
  return width;
}
// Korean wraps by words like the client's keep-all: GDI+/DirectWrite would
// break between any two Hangul syllables ("필│요한"). Lines break at spaces;
// only a word wider than the whole line breaks inside (longest fitting part).
template<class Measure> std::vector<std::wstring> wrapWith(const std::wstring& value,double width,Measure measure){
  std::vector<std::wstring> lines;size_t start=0;
  while(start<=value.size()){
    const size_t end=std::min(value.find(L'\n',start),value.size());std::wstring paragraph=value.substr(start,end-start);
    while(!paragraph.empty()&&paragraph.back()==L'\r')paragraph.pop_back();
    std::wstring line;size_t at=0;
    while(at<=paragraph.size()){
      const size_t space=std::min(paragraph.find(L' ',at),paragraph.size());std::wstring word=paragraph.substr(at,space-at);
      const std::wstring candidate=line.empty()?word:line+L" "+word;
      if(line.empty()||measure(candidate)<=width){line=candidate;}else{lines.push_back(line);line=word;}
      while(line.size()>1&&measure(line)>width){
        size_t low=1,high=line.size()-1;while(low<high){const size_t mid=(low+high+1)/2;if(measure(line.substr(0,mid))<=width)low=mid;else high=mid-1;}
        lines.push_back(line.substr(0,low));line.erase(0,low);
      }
      at=space+1;
    }
    lines.push_back(line);if(end>=value.size())break;start=end+1;
  }
  return lines;
}
std::vector<std::wstring> wrapLines(const std::wstring& value,double width,double px,unsigned flags){
  IDWriteTextFormat* format=textFormat(px,flags);if(!format)return wrapWith(value,width,[&](const std::wstring& s){return double(s.size())*px;});
  auto lines=wrapWith(value,width,[&](const std::wstring& s){return double(textWidth(format,s));});format->Release();return lines;
}
// The lines a text takes in a box [width] wide (logical units at [scale]).
int lineCount(const std::wstring& value,double width,double size,unsigned flags,double scale){return value.empty()?0:int(wrapLines(value,width*scale,size*scale,flags).size());}
ID2D1DCRenderTarget* textTarget(){
  auto& t=type();
  if(!t.target&&t.d2d){const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96.f,96.f);if(FAILED(t.d2d->CreateDCRenderTarget(&properties,&t.target)))t.target=nullptr;}
  return t.target;
}
// One buffered paint: a 32-bit DIB, GDI+ shapes as they are drawn, then the
// queued texts on top, then one copy to the window. Text is always on top.
class Painter {
 public:
  Painter(int width,int height,double scale,COLORREF fill):width_(std::max(1,width)),height_(std::max(1,height)),scale_(scale){
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=width_;info.bmiHeader.biHeight=-height_;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;dc_=CreateCompatibleDC(nullptr);bitmap_=CreateDIBSection(dc_,&info,DIB_RGB_COLORS,&bits,nullptr,0);old_=SelectObject(dc_,bitmap_);
    graphics_.emplace(dc_);graphics_->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);graphics_->Clear(color(fill));
  }
  ~Painter(){graphics_.reset();SelectObject(dc_,old_);DeleteObject(bitmap_);DeleteDC(dc_);}
  Painter(const Painter&)=delete;
  Painter& operator=(const Painter&)=delete;
  Gdiplus::Graphics& g(){return *graphics_;}
  double scale()const{return scale_;}
  // [lineHeight] 0: 1.5 x size for wrapped text; a Single line fills its box when CenterV.
  void text(std::wstring value,double x,double y,double w,double h,double size,COLORREF ink,unsigned flags=0,double lineHeight=0){items_.push_back({std::move(value),x,y,w,h,size,lineHeight,ink,flags});}
  void present(HDC target,int x=0,int y=0){flush();BitBlt(target,x,y,width_,height_,dc_,0,0,SRCCOPY);}
 private:
  struct Item {std::wstring value;double x,y,w,h,size,lineHeight;COLORREF ink;unsigned flags;};
  int width_,height_;double scale_;HDC dc_=nullptr;HBITMAP bitmap_=nullptr;HGDIOBJ old_=nullptr;std::optional<Gdiplus::Graphics> graphics_;std::vector<Item> items_;
  void flush(){
    graphics_->Flush(Gdiplus::FlushIntentionSync);if(items_.empty())return;
    ID2D1DCRenderTarget* target=textTarget();RECT bounds{0,0,width_,height_};
    if(!target||!type().write||FAILED(target->BindDC(dc_,&bounds))){for(const auto& item:items_)drawGdi(item);items_.clear();return;}
    target->BeginDraw();target->SetTransform(D2D1::Matrix3x2F::Identity());target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    for(const auto& item:items_)draw(target,item);
    if(target->EndDraw()==D2DERR_RECREATE_TARGET){target->Release();type().target=nullptr;}
    items_.clear();
  }
  void draw(ID2D1DCRenderTarget* target,const Item& item){
    const double px=item.size*scale_;IDWriteTextFormat* format=textFormat(px,item.flags);if(!format)return;
    const bool single=(item.flags&Single)!=0;const float boxW=float(item.w*scale_),boxH=float(item.h*scale_);
    const float line=float(item.lineHeight>0?item.lineHeight*scale_:single?((item.flags&CenterV)?boxH:px*1.45):px*1.5);
    std::vector<std::wstring> lines=single?std::vector<std::wstring>{item.value}:wrapWith(item.value,boxW,[&](const std::wstring& s){return double(textWidth(format,s));});
    const size_t fit=single?1:std::max<size_t>(1,size_t((boxH+.5f)/line));bool trimmed=false;
    if(lines.size()>fit){lines.resize(fit);trimmed=true;}
    float top=float(item.y*scale_);if(item.flags&CenterV)top+=(boxH-line*float(lines.size()))/2;
    const float baseline=float(line/2+type().cap[weightIndex(item.flags)]*px/2);
    ID2D1SolidColorBrush* brush=nullptr;
    if(SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(GetRValue(item.ink)/255.f,GetGValue(item.ink)/255.f,GetBValue(item.ink)/255.f),&brush))){
      for(size_t i=0;i<lines.size();i++){
        const std::wstring text=trimmed&&i+1==lines.size()?lines[i]+L"…":lines[i];IDWriteTextLayout* layout=nullptr;
        if(FAILED(type().write->CreateTextLayout(text.c_str(),UINT32(text.size()),format,std::max(1.f,boxW),line,&layout)))continue;
        layout->SetTextAlignment((item.flags&CenterH)?DWRITE_TEXT_ALIGNMENT_CENTER:(item.flags&Right)?DWRITE_TEXT_ALIGNMENT_TRAILING:DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,line,baseline);
        target->DrawTextLayout(D2D1::Point2F(float(item.x*scale_),top+line*float(i)),layout,brush,D2D1_DRAW_TEXT_OPTIONS_NONE);layout->Release();
      }
      brush->Release();
    }
    format->Release();
  }
  // No Direct2D: plain GDI text in the same face.
  void drawGdi(const Item& item){
    const HFONT font=CreateFontW(-int(std::lround(item.size*scale_)),0,0,0,(item.flags&Bold)?FW_BOLD:(item.flags&Semi)?FW_SEMIBOLD:FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,uiFamily());
    const HGDIOBJ old=SelectObject(dc_,font);SetBkMode(dc_,TRANSPARENT);SetTextColor(dc_,item.ink);
    RECT box{int(item.x*scale_),int(item.y*scale_),int((item.x+item.w)*scale_),int((item.y+item.h)*scale_)};
    UINT format=DT_NOPREFIX|((item.flags&CenterH)?DT_CENTER:(item.flags&Right)?DT_RIGHT:DT_LEFT);
    format|=(item.flags&Single)?DT_SINGLELINE|DT_END_ELLIPSIS|((item.flags&CenterV)?DT_VCENTER:0u):DT_WORDBREAK|DT_END_ELLIPSIS;
    DrawTextW(dc_,item.value.c_str(),-1,&box,format);SelectObject(dc_,old);DeleteObject(font);
  }
};
void darkTitleBar(HWND window){
  // Optional title-bar decoration only; no undocumented theme APIs or policy
  // changes. Windows 11 paints the caption in the window's own colour, so an
  // accent-coloured title bar no longer sits on the dark installer.
  if(HMODULE module=LoadLibraryExW(L"dwmapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)){using Attribute=HRESULT(WINAPI*)(HWND,DWORD,LPCVOID,DWORD);const auto attribute=reinterpret_cast<Attribute>(GetProcAddress(module,"DwmSetWindowAttribute"));if(attribute){const BOOL dark=TRUE;attribute(window,20,&dark,sizeof(dark));const COLORREF caption=Canvas,captionText=Muted,border=Line;attribute(window,35,&caption,sizeof(caption));attribute(window,36,&captionText,sizeof(captionText));attribute(window,34,&border,sizeof(border));}FreeLibrary(module);}
}
void roundRect(Gdiplus::Graphics& g,double scale,double x,double y,double w,double h,double radius,COLORREF fill,const COLORREF* stroke=nullptr,double strokeWidth=1){
  const float left=float(x*scale),top=float(y*scale),width=float(w*scale),height=float(h*scale),d=float(std::min(radius*2,std::min(w,h))*scale);Gdiplus::GraphicsPath path;
  path.AddArc(left,top,d,d,180,90);path.AddArc(left+width-d,top,d,d,270,90);path.AddArc(left+width-d,top+height-d,d,d,0,90);path.AddArc(left,top+height-d,d,d,90,90);path.CloseFigure();
  Gdiplus::SolidBrush brush(color(fill));g.FillPath(&brush,&path);if(stroke){Gdiplus::Pen pen(color(*stroke),float(strokeWidth*scale));g.DrawPath(&pen,&path);}
}
// A status badge: success (check), warning/failure (!), or a question (?).
enum class Badge {Success,Warning,Failure,Question};
void drawBadge(Painter& p,double x,double y,double size,Badge kind){
  Gdiplus::Graphics& g=p.g();const double scale=p.scale();
  const COLORREF fill=kind==Badge::Success?Success:kind==Badge::Failure?Danger:Blue;
  Gdiplus::SolidBrush brush(color(fill));g.FillEllipse(&brush,float(x*scale),float(y*scale),float(size*scale),float(size*scale));
  const double cx=x+size/2,cy=y+size/2,u=size/24;
  Gdiplus::Pen pen(color(Canvas),float(2.4*u*scale));pen.SetStartCap(Gdiplus::LineCapRound);pen.SetEndCap(Gdiplus::LineCapRound);pen.SetLineJoin(Gdiplus::LineJoinRound);
  if(kind==Badge::Success){const Gdiplus::PointF points[3]{{float((cx-5.2*u)*scale),float((cy+.2*u)*scale)},{float((cx-1.6*u)*scale),float((cy+3.8*u)*scale)},{float((cx+5.4*u)*scale),float((cy-3.8*u)*scale)}};g.DrawLines(&pen,points,3);}
  else if(kind==Badge::Question)p.text(L"?",x,y,size,size,size*.6,Canvas,Bold|CenterH|CenterV|Single);
  else{g.DrawLine(&pen,float(cx*scale),float((cy-5*u)*scale),float(cx*scale),float((cy+1.2*u)*scale));Gdiplus::SolidBrush dot(color(Canvas));const double r=1.5*u;g.FillEllipse(&dot,float((cx-r)*scale),float((cy+4.6*u-r)*scale),float(2*r*scale),float(2*r*scale));}
}
// The owner-drawn buttons of both windows: amber primary, outlined secondary,
// text-only links (amber, or muted for a quiet action); a rounded focus ring
// only for keyboard focus.
enum class ButtonStyle {Primary,Secondary,Link,QuietLink};
void drawButton(const DRAWITEMSTRUCT& item,double scale,ButtonStyle style,COLORREF background){
  const int pw=item.rcItem.right-item.rcItem.left,ph=item.rcItem.bottom-item.rcItem.top;if(pw<=0||ph<=0)return;
  Painter p(pw,ph,scale,background);Gdiplus::Graphics& g=p.g();
  const bool primary=style==ButtonStyle::Primary,link=style==ButtonStyle::Link||style==ButtonStyle::QuietLink;
  const bool disabled=(item.itemState&ODS_DISABLED)!=0,pressed=(item.itemState&ODS_SELECTED)!=0,focus=(item.itemState&ODS_FOCUS)!=0&&!(item.itemState&ODS_NOFOCUSRECT);
  const double w=pw/scale,h=ph/scale;
  wchar_t caption[100]{};GetWindowTextW(item.hwndItem,caption,100);
  // The quiet action sits at the left edge of its row, the box link at the right.
  if(link){const bool quiet=style==ButtonStyle::QuietLink;const COLORREF ink=disabled?Faint:quiet?(pressed?Ink:Muted):pressed?RGB(221,143,25):Blue;p.text(caption,quiet?4:0,0,w-(quiet?4:6),h,12,ink,Semi|CenterV|Single|(quiet?0u:Right));}
  else{
    const COLORREF fill=disabled?(primary?Line:Surface):primary?(pressed?RGB(221,143,25):Blue):(pressed?RGB(30,42,49):Surface);
    const COLORREF edge=primary&&!disabled?fill:Line;
    roundRect(g,scale,.5,.5,w-1,h-1,10,fill,&edge);
    p.text(caption,0,0,w,h,13,disabled?Faint:primary?ButtonInk:Ink,Semi|CenterH|CenterV|Single);
  }
  if(focus){const COLORREF ring=primary?Amber:Muted;Gdiplus::Pen pen(color(ring),float(1.5*scale));const double inset=link?1.5:2.5,r=link?6:8;Gdiplus::GraphicsPath path;const float l=float(inset*scale),t=float(inset*scale),fw=float((w-2*inset)*scale),fh=float((h-2*inset)*scale),d=float(r*2*scale);
    path.AddArc(l,t,d,d,180,90);path.AddArc(l+fw-d,t,d,d,270,90);path.AddArc(l+fw-d,t+fh-d,d,d,0,90);path.AddArc(l,t+fh-d,d,d,90,90);path.CloseFigure();g.DrawPath(&pen,&path);}
  p.present(item.hDC,item.rcItem.left,item.rcItem.top);
}
#ifdef MULTITAP_SETUP_QA
// --selftest-capture (QA binary only): a window's own paint (WM_PRINTCLIENT)
// and each visible child's (WM_PRINT) composed into one PNG under
// MULTITAP_SETUP_QA_CAPTURE: what the screen shows, without the screen.
namespace capture {
std::wstring folder(){wchar_t value[32768]{};const DWORD length=GetEnvironmentVariableW(L"MULTITAP_SETUP_QA_CAPTURE",value,DWORD(std::size(value)));return length&&length<std::size(value)?std::wstring(value,length):std::wstring();}
bool pngEncoder(CLSID& id){
  UINT count=0,size=0;if(Gdiplus::GetImageEncodersSize(&count,&size)!=Gdiplus::Ok||!size)return false;
  std::vector<BYTE> buffer(size);auto* codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());if(Gdiplus::GetImageEncoders(count,size,codecs)!=Gdiplus::Ok)return false;
  for(UINT i=0;i<count;i++)if(std::wcscmp(codecs[i].MimeType,L"image/png")==0){id=codecs[i].Clsid;return true;}
  return false;
}
bool window(HWND window,const std::wstring& name){
  const auto directory=folder();RECT client{};GetClientRect(window,&client);if(directory.empty()||client.right<=0||client.bottom<=0)return false;
  HDC screen=GetDC(window);HDC target=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,client.right,client.bottom);const HGDIOBJ old=SelectObject(target,bitmap);
  SendMessageW(window,WM_PRINTCLIENT,WPARAM(target),PRF_CLIENT|PRF_ERASEBKGND);
  std::vector<HWND> children;for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))children.push_back(child);
  for(auto it=children.rbegin();it!=children.rend();++it){
    if(!IsWindowVisible(*it))continue;RECT r{};GetWindowRect(*it,&r);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);const int w=r.right-r.left,h=r.bottom-r.top;if(w<=0||h<=0)continue;
    HDC part=CreateCompatibleDC(screen);HBITMAP partBitmap=CreateCompatibleBitmap(screen,w,h);const HGDIOBJ partOld=SelectObject(part,partBitmap);
    SendMessageW(*it,WM_PRINT,WPARAM(part),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND|PRF_CHILDREN);BitBlt(target,r.left,r.top,w,h,part,0,0,SRCCOPY);
    SelectObject(part,partOld);DeleteObject(partBitmap);DeleteDC(part);
  }
  SelectObject(target,old);bool saved=false;
  {Gdiplus::Bitmap image(bitmap,nullptr);CLSID png{};saved=pngEncoder(png)&&image.Save((directory+L"\\"+name+L".png").c_str(),&png,nullptr)==Gdiplus::Ok;}
  DeleteObject(bitmap);DeleteDC(target);ReleaseDC(window,screen);
  std::printf("capture=%ls saved=%d\n",name.c_str(),saved?1:0);return saved;
}
}
#endif
class Confirmation {
 public:
#ifdef MULTITAP_SETUP_QA
  // Capture mode: the next dialog paints itself at this DPI into this PNG and closes (cancel).
  static inline std::wstring qaCaptureName;static inline UINT qaCaptureDpi=96;static inline bool qaCaptureSaved=false;
#endif
  static bool show(HWND owner,const wchar_t* title,const wchar_t* heading,const wchar_t* content,const wchar_t* footer,const wchar_t* action){
    Confirmation dialog(owner,heading,content,footer,action,false);return dialog.run(title);
  }
  static void notice(HWND owner,const wchar_t* title,const wchar_t* content){Confirmation dialog(owner,title,content,tx(S::NoticeFooter),tx(S::Ok),true);dialog.notice_=true;dialog.run(title);}
  static bool selfTests(HWND owner){
    for(int mode=1;mode<=6;++mode){Confirmation dialog(owner,tx(S::QaConfirmHeading),tx(S::QaConfirmBody),tx(S::QaConfirmFooter),tx(S::Ok),mode==1||mode==4||mode==6);dialog.qaMode_=mode;dialog.notice_=mode==6;const bool accepted=dialog.run(tx(S::QaConfirmTitle));std::printf("confirmationMode=%d passed=%d accepted=%d ownerEnabled=%d\n",mode,dialog.qaPassed_?1:0,accepted?1:0,IsWindowEnabled(owner)?1:0);if(!dialog.qaPassed_||accepted!=(mode==2||mode==4)||!IsWindowEnabled(owner))return false;}
    return true;
  }
 private:
  HWND owner_=nullptr,window_=nullptr,accept_=nullptr,cancel_=nullptr;HFONT font_=nullptr;
  const wchar_t *heading_,*content_,*footer_,*action_;bool cancelDefault_,running_=true,accepted_=false,notice_=false;double scale_=1;int qaMode_=0;bool qaPassed_=true;
  Confirmation(HWND owner,const wchar_t* heading,const wchar_t* content,const wchar_t* footer,const wchar_t* action,bool cancelDefault):owner_(owner),heading_(heading),content_(content),footer_(footer),action_(action),cancelDefault_(cancelDefault){}
  int px(int n)const{return int(n*scale_);}
  static constexpr int DialogWidth=520;
  static constexpr double BodySize=13.5,BodyLine=21,NoteLine=19;
  // The dialog measured at [width] (logical units): every block sized to its
  // text, so a short question gets a short dialog and nothing is clipped.
  struct Layout {double headingTop,headingLines,bodyTop,noteTop,noteLines,buttonsTop,height;};
  std::vector<std::wstring> paragraphs()const{std::vector<std::wstring> result;std::wstring value=content_;size_t start=0;while(start<=value.size()){const size_t end=std::min(value.find(L'\n',start),value.size());result.push_back(value.substr(start,end-start));if(end>=value.size())break;start=end+1;}return result;}
  Layout measure(double width,double scale)const{
    Layout l{};l.headingLines=std::max(1,lineCount(heading_,width-102,17,Bold,scale));
    const double headingHeight=std::max(34.0,l.headingLines*24);l.headingTop=26+(headingHeight-l.headingLines*24)/2;
    double y=26+headingHeight+16;l.bodyTop=y;
    for(const auto& paragraph:paragraphs())y+=paragraph.empty()?6:lineCount(paragraph,width-56,BodySize,0,scale)*BodyLine+6;
    y-=6;
    if(footer_&&*footer_){l.noteLines=std::max(1,lineCount(footer_,width-84,12.5,0,scale));l.noteTop=y+16;y=l.noteTop+l.noteLines*NoteLine+22;}
    l.buttonsTop=y+24;l.height=l.buttonsTop+38+24;return l;
  }
  // A badge (question, or notice), the heading beside it, the body and the
  // consequence in an amber note above the buttons.
  void paint(HDC dc){
    RECT bounds{};GetClientRect(window_,&bounds);if(bounds.right<=0||bounds.bottom<=0)return;
    Painter p(bounds.right,bounds.bottom,scale_,Canvas);const double width=bounds.right/scale_;const auto l=measure(width,scale_);
    drawBadge(p,28,26,34,notice_?Badge::Warning:Badge::Question);
    p.text(heading_,74,l.headingTop,width-102,l.headingLines*24,17,Ink,Bold,24);
    double y=l.bodyTop;
    for(const auto& paragraph:paragraphs()){if(paragraph.empty()){y+=6;continue;}const int lines=lineCount(paragraph,width-56,BodySize,0,scale_);p.text(paragraph,28,y,width-56,lines*BodyLine,BodySize,Body,0,BodyLine);y+=lines*BodyLine+6;}
    if(footer_&&*footer_){roundRect(p.g(),scale_,28,l.noteTop,width-56,l.noteLines*NoteLine+22,10,AmberSurface,&AmberLine);p.text(footer_,42,l.noteTop+11,width-84,l.noteLines*NoteLine,12.5,Amber,0,NoteLine);}
    p.present(dc);
  }
  int qaHeight()const{return int(std::ceil(measure(DialogWidth,1).height));}
  bool run(const wchar_t* title){
    scale_=double(GetDpiForWindow(owner_))/96.;WNDCLASSW type{};type.hInstance=GetModuleHandleW(nullptr);type.lpfnWndProc=procedure;type.lpszClassName=L"MultitapSetupConfirmation";type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.hIcon=LoadIconW(type.hInstance,MAKEINTRESOURCEW(1));RegisterClassW(&type);
    // Sized to its content.
    RECT frame{0,0,px(DialogWidth),px(qaHeight())};AdjustWindowRectExForDpi(&frame,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_DLGMODALFRAME,UINT(scale_*96));RECT parent{};GetWindowRect(owner_,&parent);
    window_=CreateWindowExW(WS_EX_DLGMODALFRAME,type.lpszClassName,title,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,parent.left+(parent.right-parent.left-frame.right+frame.left)/2,parent.top+(parent.bottom-parent.top-frame.bottom+frame.top)/2,frame.right-frame.left,frame.bottom-frame.top,owner_,nullptr,type.hInstance,this);
    if(!window_)return false;const bool ownerEnabled=IsWindowEnabled(owner_)!=FALSE;EnableWindow(owner_,FALSE);darkTitleBar(window_);ShowWindow(window_,SW_SHOW);SetFocus(cancelDefault_?cancel_:accept_);if(qaMode_)SetTimer(window_,1,100,nullptr);
#ifdef MULTITAP_SETUP_QA
    if(!qaCaptureName.empty()){qaCaptureSaved=false;SetTimer(window_,3,10,nullptr);}
#endif
    MSG message{};while(running_){const BOOL received=GetMessageW(&message,nullptr,0,0);if(received<=0){if(received==0)PostQuitMessage(int(message.wParam));break;}if(!IsDialogMessageW(window_,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    if(IsWindow(window_))DestroyWindow(window_);if(ownerEnabled)EnableWindow(owner_,TRUE);SetActiveWindow(owner_);DeleteObject(font_);return accepted_;
  }
  void layout(){
    if(font_)DeleteObject(font_);font_=CreateFontW(-px(13),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,uiFamily());
    for(HWND button:{accept_,cancel_})SendMessageW(button,WM_SETFONT,WPARAM(font_),TRUE);
    // Windows can constrain a DPI-expanded dialog to the desktop work area.
    // Anchor actions to the actual client size rather than the requested 520.
    RECT client{};GetClientRect(window_,&client);const int width=int(client.right/scale_),height=int(client.bottom/scale_);
    const int right=std::max(170,width-28),bottom=std::max(0,height-62);
    MoveWindow(cancel_,px(std::max(0,right-266)),px(bottom),px(84),px(38),TRUE);MoveWindow(accept_,px(std::max(0,right-170)),px(bottom),px(170),px(38),TRUE);InvalidateRect(window_,nullptr,TRUE);
    if(notice_){ShowWindow(accept_,SW_HIDE);SetWindowTextW(cancel_,tx(S::Ok));MoveWindow(cancel_,px(std::max(0,right-170)),px(bottom),px(170),px(38),TRUE);}
  }
  LRESULT message(UINT message,WPARAM wp,LPARAM lp){switch(message){
    case WM_CREATE:
      cancel_=CreateWindowExW(0,L"BUTTON",tx(S::Cancel),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window_,reinterpret_cast<HMENU>(IDCANCEL),GetModuleHandleW(nullptr),nullptr);
      accept_=CreateWindowExW(0,L"BUTTON",action_,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window_,reinterpret_cast<HMENU>(IDOK),GetModuleHandleW(nullptr),nullptr);layout();return 0;
    case DM_GETDEFID:return MAKELONG(cancelDefault_?IDCANCEL:IDOK,DC_HASDEFID);
    case WM_COMMAND:if(LOWORD(wp)==IDOK||LOWORD(wp)==IDCANCEL){accepted_=LOWORD(wp)==IDOK;DestroyWindow(window_);}return 0;
    case WM_CLOSE:DestroyWindow(window_);return 0;
    case WM_TIMER:
      if(wp==1&&qaMode_){KillTimer(window_,1);const bool focusGood=GetFocus()==(cancelDefault_?cancel_:accept_),ownerDisabled=!IsWindowEnabled(owner_),defaultGood=LOWORD(SendMessageW(window_,DM_GETDEFID,0,0))==(cancelDefault_?IDCANCEL:IDOK);std::printf("confirmationMode=%d focusGood=%d ownerDisabled=%d defaultGood=%d\n",qaMode_,focusGood?1:0,ownerDisabled?1:0,defaultGood?1:0);qaPassed_&=focusGood&&ownerDisabled&&defaultGood;
        for(UINT dpi:{96u,192u}){RECT frame{0,0,MulDiv(520,dpi,96),MulDiv(360,dpi,96)};AdjustWindowRectExForDpi(&frame,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_DLGMODALFRAME,dpi);RECT current{};GetWindowRect(window_,&current);RECT suggested{current.left,current.top,current.left+frame.right-frame.left,current.top+frame.bottom-frame.top};SendMessageW(window_,WM_DPICHANGED,MAKELONG(dpi,dpi),LPARAM(&suggested));RECT client{};GetClientRect(window_,&client);for(HWND button:{accept_,cancel_}){RECT bounds{};GetWindowRect(button,&bounds);MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&bounds),2);const bool fits=bounds.left>=0&&bounds.top>=0&&bounds.right<=client.right&&bounds.bottom<=client.bottom;qaPassed_&=fits;if(!fits)std::printf("confirmationMode=%d dpi=%u control=%d rect=%ld,%ld,%ld,%ld client=%ld,%ld\n",qaMode_,dpi,int(GetDlgCtrlID(button)),bounds.left,bounds.top,bounds.right,bounds.bottom,client.right,client.bottom);}}
        SetTimer(window_,2,1500,nullptr);if(qaMode_==4)PostMessageW(window_,WM_COMMAND,IDOK,0);else if(qaMode_==5)PostMessageW(window_,WM_CLOSE,0,0);else{PostMessageW(window_,WM_KEYDOWN,qaMode_==3?VK_ESCAPE:VK_RETURN,0);PostMessageW(window_,WM_KEYUP,qaMode_==3?VK_ESCAPE:VK_RETURN,0);}return 0;}
      if(wp==2&&qaMode_){qaPassed_=false;DestroyWindow(window_);return 0;}
#ifdef MULTITAP_SETUP_QA
      if(wp==3){
        KillTimer(window_,3);const UINT dpi=qaCaptureDpi;RECT frame{0,0,MulDiv(520,dpi,96),MulDiv(qaHeight(),dpi,96)};AdjustWindowRectExForDpi(&frame,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_DLGMODALFRAME,dpi);
        RECT current{};GetWindowRect(window_,&current);RECT suggested{current.left,current.top,current.left+frame.right-frame.left,current.top+frame.bottom-frame.top};SendMessageW(window_,WM_DPICHANGED,MAKELONG(dpi,dpi),LPARAM(&suggested));
        qaCaptureSaved=capture::window(window_,qaCaptureName);qaCaptureName.clear();DestroyWindow(window_);return 0;
      }
#endif
      break;
    case WM_DESTROY:running_=false;return 0;
    case WM_SIZE:if(accept_&&cancel_)layout();return 0;
    case WM_DPICHANGED:{const auto* rect=reinterpret_cast<const RECT*>(lp);if(!rect||HIWORD(wp)<48)return 0;scale_=double(HIWORD(wp))/96.;SetWindowPos(window_,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);layout();return 0;}
    case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(window_,&ps);paint(dc);EndPaint(window_,&ps);return 0;}
    case WM_PRINTCLIENT:paint(reinterpret_cast<HDC>(wp));return 0;
    case WM_DRAWITEM:{const auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);drawButton(*item,scale_,item->CtlID==IDOK?ButtonStyle::Primary:ButtonStyle::Secondary,Canvas);return TRUE;}
    case WM_ERASEBKGND:return 1;
  }return DefWindowProcW(window_,message,wp,lp);}
  static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){auto* dialog=reinterpret_cast<Confirmation*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){dialog=static_cast<Confirmation*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);dialog->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,LONG_PTR(dialog));}return dialog?dialog->message(message,wp,lp):DefWindowProcW(window,message,wp,lp);}
};
struct Progress {int completed;std::wstring message;};
enum class View {Welcome,Working,Complete,Failed};
// The Install click's question.
bool confirmInstall(HWND parent,bool preview){
  return Confirmation::show(parent,tx(S::InstallConfirmTitle),tx(S::InstallConfirmHeadingClient),tx(S::InstallConfirmBodyClient),tx(preview?S::InstallConfirmFooterPreview:S::InstallConfirmFooterClient),tx(S::InstallAction));
}
inline const wchar_t* InstallingTitle(){return tx(S::InstallingTitle);}
inline const wchar_t* InstallingNotice(){return tx(S::InstallingNotice);}
std::wstring visibleText(std::wstring value) {
  for(auto& ch:value)if(ch<32&&ch!=L'\r'&&ch!=L'\n'&&ch!=L'\t')ch=L' ';
  if(value.size()>4096){value.resize(4096);if(value.back()>=0xd800&&value.back()<=0xdbff)value.pop_back();value+=L"…";}
  return value;
}
std::wstring diagnosticText(const std::wstring& version,int completed,bool failed,const std::wstring& message) {
  std::wstring result=txf(S::DiagHeader,{L"Client",version,std::to_wstring(std::clamp(completed,0,TotalSteps)),tx(failed?S::DiagFailed:S::DiagDone)});
  if(!message.empty())result+=L"\r\n\r\n"+visibleText(message);
  return result;
}
bool diagnosticFormatTests() {
  const auto text=diagnosticText(L"0.0.0",4,false,L"서비스 준비 완료");
  const auto failure=diagnosticText(L"0.0.0",99,true,L"검사용 오류\r\n한글 설명");
  const auto bounded=visibleText(std::wstring(5000,L'a')+L"\x0001");
  // The steps line in the person's language (Korean: "완료 단계: 4 / 6").
  const auto steps=[](int n){const auto header=txf(S::DiagHeader,{L"",L"",std::to_wstring(n),L""});const auto at=header.find(L"\r\n")+2;return header.substr(at,header.find(L"\r\n",at)-at);};
  return text.find(steps(4))!=std::wstring::npos&&(!koreanUi()||text.find(L"완료 단계: 4 / 6")!=std::wstring::npos)&&text.find(L"서비스 준비 완료")!=std::wstring::npos&&failure.find(steps(6))!=std::wstring::npos&&failure.find(L"검사용 오류\r\n한글 설명")!=std::wstring::npos&&bounded.size()<=4097&&visibleText(L"a\x0001" L"b")==L"a b";
}
bool parseProgressLine(std::string line,Progress& out){
  const auto split=line.find('|');if(split==std::string::npos)return false;
  try{
    const auto stepText=line.substr(0,split);size_t parsed=0;const int step=std::stoi(stepText,&parsed);
    // What Install-Client.ps1 reports: the steps done (0..6), -1 for a failure
    // and -5, its "checks passed, about to replace" marker.
    if(parsed!=stepText.size()||!(step==-5||(step>=-1&&step<=TotalSteps)))return false;
    auto bytes=line.substr(split+1);if(!bytes.empty()&&bytes.back()=='\r')bytes.pop_back();
    const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),int(bytes.size()),nullptr,0);if(length<=0)return false;
    std::wstring text(size_t(length),0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),int(bytes.size()),text.data(),length);
    out={step,text};return true;
  }catch(...){return false;}
}
// What an installer run without a window reported (the payload QA run, the decision checks).
struct UpdateOutcome {int completed=0;bool failureReported=false;};
void applyUpdateProgress(UpdateOutcome& outcome,const Progress& progress){
  if(progress.completed==-1)outcome.failureReported=true;
  else outcome.completed=std::max(outcome.completed,progress.completed);
}
bool updateSucceeded(const UpdateOutcome& outcome,DWORD exitCode){return exitCode==0&&!outcome.failureReported&&outcome.completed==TotalSteps;}
// The elevated PowerShell installer never starts the ordinary client: relaunch
// belongs to this setup's verified shell-user path.
std::wstring installArguments(const std::wstring& script,const std::wstring& pipe){
  return L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+script+L"\" -ProgressPipe "+pipe;
}
// The Working view's step names (also named when an installer stops mid-way).
const wchar_t* stepName(int index){
  static const S steps[TotalSteps]={S::ClientStep1,S::ClientStep2,S::ClientStep3,S::ClientStep4,S::ClientStep5,S::ClientStep6};
  return tx(steps[std::clamp(index,0,TotalSteps-1)]);
}
// The installer script ended without reporting a result (it was ended from
// outside or crashed): the step it stopped in and a possible cause, nothing
// this setup cannot know. Field case: AhnLab Safe Transaction ended it.
std::wstring interruptedMessage(int completed){
  const int step=std::clamp(completed,0,TotalSteps-1);
  return txf(S::Interrupted,{std::to_wstring(step+1),std::to_wstring(TotalSteps),stepName(step)})+tx(S::InterruptedClientRetry);
}
// --- payload-v1 (setup_payload.hpp) -------------------------------------------
// This setup writes every installed program file and shortcut; the script
// lists, requests and verifies them over its progress channel (a behaviour
// detection can end powershell.exe when it creates executables).
std::wstring environmentPath(const wchar_t* name){
  wchar_t value[32768]{};const DWORD length=GetEnvironmentVariableW(name,value,DWORD(std::size(value)));
  return length&&length<std::size(value)?std::wstring(value,length):std::wstring();
}
// Production: this setup's own payload (bin beside it), the %ProgramFiles% /
// %LOCALAPPDATA% the script reads and the desktop folder it used. QA binary:
// every root lies under the isolated harness directory (MULTITAP_SETUP_QA_ROOT);
// without it every request is refused, so it never reaches an installation.
multitap::payload::Roots setupRoots(){
  multitap::payload::Roots roots;
#ifdef MULTITAP_SETUP_QA
  const auto base=environmentPath(L"MULTITAP_SETUP_QA_ROOT");
  if(!multitap::payload::canonicalPath(base))return roots;
  roots.qa=true;roots.source=base+L"\\payload\\bin";roots.programFiles=base+L"\\ProgramFiles";roots.localAppData=base+L"\\LocalAppData";
  roots.desktop=base+L"\\Desktop";
#else
  wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
  roots.source=(std::filesystem::path(executable).parent_path()/L"bin").wstring();
  // Program Files from the machine's known folder, not the environment a
  // user's own variables could change; the installed client must land in the
  // administrator-only folder whatever this process inherited.
  if(const auto folder=multitap::relaunch::KnownFolder(FOLDERID_ProgramFiles))roots.programFiles=folder->wstring();
  roots.localAppData=environmentPath(L"LOCALAPPDATA");
  if(const auto folder=multitap::relaunch::KnownFolder(FOLDERID_Desktop))roots.desktop=folder->wstring();
#endif
  return roots;
}
std::string setupVersion(){
  const auto version=multitap::relaunch::Narrow(std::wstring(MULTITAP_CLIENT_VERSION));
  return version?*version:std::string();
}
// The setup side of payload-v1: file and shortcut placement (setup_payload.hpp)
// and, here, closing the running client and the Apps & Features registration.
struct SetupChannel {
  multitap::payload::Server files;
  // A seamless update: the running client was asked to close and ends its remote windows first.
  DWORD clientStopGraceMs=0;
  SetupChannel():files(setupRoots(),setupVersion()){
    files.extension=[this](const std::vector<std::string>& fields,std::string& reply){
      if(fields.size()>=2&&fields[1]=="client-stop-v1"){
        try{if(fields.size()!=4||fields[2]!="client"||fields[3]!=setupVersion())throw multitap::payload::Failure{"payload_request_invalid"};
#ifdef MULTITAP_SETUP_QA
          throw multitap::payload::Failure{"process_stop_qa_refused"};
#else
          multitap::installation::stopClientProcesses(setupRoots(),clientStopGraceMs);reply="payload-v1|ok\n";
#endif
        }catch(const multitap::payload::Failure& failure){reply=std::string("payload-v1|error|")+failure.code+"\n";}catch(...){reply="payload-v1|error|client_stop_failed\n";}return true;
      }
      if(fields.size()>=2&&fields[1]=="register-installation-v1"){
        try{
          if(fields.size()!=4||fields[2]!="client"||fields[3]!=setupVersion())throw multitap::payload::Failure{"payload_request_invalid"};
#ifdef MULTITAP_SETUP_QA
          // QA cannot write a real Apps & Features key or install a remover.
          throw multitap::payload::Failure{"registration_qa_refused"};
#else
          multitap::installation::registerInstalled(setupRoots(),setupVersion(),multitap::installation::modulePath(),multitap::installation::registration());
          reply="payload-v1|ok\n";
#endif
        }catch(const multitap::payload::Failure& failure){reply=std::string("payload-v1|error|")+failure.code+"\n";}catch(...){reply="payload-v1|error|registration_failed\n";}
        return true;
      }
      return false;
    };
  }
  SetupChannel(const SetupChannel&)=delete;
  SetupChannel& operator=(const SetupChannel&)=delete;
  bool Receive(const std::string& line,std::string& reply){
    return files.Receive(line,reply);
  }
};
// The name must stay what Install-Client.ps1 accepts for its -ProgressPipe.
std::wstring newPipeName(){
  GUID id{};if(FAILED(CoCreateGuid(&id)))return {};
  const auto* bytes=reinterpret_cast<BYTE*>(&id);const wchar_t hex[]=L"0123456789abcdef";std::wstring name=L"MultitapHostSetup-";
  for(size_t i=0;i<sizeof(id);i++){name+=hex[bytes[i]>>4];name+=hex[bytes[i]&15];}
  return name;
}
// The progress channel, two-way since payload-v1 (the script reads replies).
// Only SYSTEM, Administrators and this owner; the QA protocol run lowers the
// label so its low-integrity script can connect.
HANDLE createProgressPipe(const std::wstring& name,bool lowLabel){
  PSECURITY_DESCRIPTOR security=nullptr;
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(lowLabel?L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)S:(ML;;NW;;;LW)":L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)",SDDL_REVISION_1,&security,nullptr))return INVALID_HANDLE_VALUE;
  SECURITY_ATTRIBUTES attributes{sizeof(attributes),security,FALSE};
  const HANDLE pipe=CreateNamedPipeW((L"\\\\.\\pipe\\"+name).c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,&attributes);
  LocalFree(security);return pipe;
}
// One payload-v1 reply on the overlapped progress pipe.
bool writePipeLine(HANDLE pipe,const std::string& text,HANDLE stop){
  OVERLAPPED overlap{};overlap.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!overlap.hEvent)return false;
  DWORD written=0;BOOL ok=WriteFile(pipe,text.data(),DWORD(text.size()),&written,&overlap);
  if(!ok&&GetLastError()==ERROR_IO_PENDING){
    HANDLE waits[]{overlap.hEvent,stop};
    if(WaitForMultipleObjects(stop?2:1,waits,FALSE,30000)!=WAIT_OBJECT_0){CancelIoEx(pipe,&overlap);GetOverlappedResult(pipe,&overlap,&written,TRUE);CloseHandle(overlap.hEvent);return false;}
    ok=GetOverlappedResult(pipe,&overlap,&written,FALSE);
  }
  CloseHandle(overlap.hEvent);return ok&&written==text.size();
}
#ifdef MULTITAP_SETUP_QA
// QA protocol run only: the fixture script runs at low integrity, so every
// file it creates carries a low mandatory label and every file this setup
// creates carries none. The harness reads the labels to tell who wrote what.
bool launchLowIntegrity(const std::filesystem::path& application,std::wstring& command,const std::filesystem::path& directory,PROCESS_INFORMATION& process){
  HANDLE token=nullptr,low=nullptr;
  if(!OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE|TOKEN_QUERY|TOKEN_ASSIGN_PRIMARY|TOKEN_ADJUST_DEFAULT,&token))return false;
  const BOOL duplicated=DuplicateTokenEx(token,MAXIMUM_ALLOWED,nullptr,SecurityImpersonation,TokenPrimary,&low);CloseHandle(token);if(!duplicated)return false;
  PSID sid=nullptr;bool ok=ConvertStringSidToSidW(L"S-1-16-4096",&sid)!=FALSE;
  if(ok){TOKEN_MANDATORY_LABEL label{};label.Label.Attributes=SE_GROUP_INTEGRITY;label.Label.Sid=sid;ok=SetTokenInformation(low,TokenIntegrityLevel,&label,DWORD(sizeof(label)+GetLengthSid(sid)))!=FALSE;LocalFree(sid);}
  STARTUPINFOW startup{};startup.cb=sizeof(startup);
  if(ok)ok=CreateProcessAsUserW(low,application.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&process)!=FALSE;
  CloseHandle(low);return ok;
}
struct HeadlessRun {UpdateOutcome outcome;DWORD exitCode=1;bool started=false,connected=false;std::wstring failure;};
// The started script until it has really ended: the pipe closes first, then
// its exit is waited for however long it takes. It is never ended from here.
class HeldMutationChild {
  multitap::payload::Handle process_;multitap::payload::Handle& pipe_;
 public:
  HeldMutationChild(HANDLE process,multitap::payload::Handle& pipe):process_(process),pipe_(pipe){}
  void waitPhysical()const noexcept{while(WaitForSingleObject(process_.get(),60000)!=WAIT_OBJECT_0)Sleep(100);}
  ~HeldMutationChild(){pipe_.reset();waitPhysical();}
  HeldMutationChild(const HeldMutationChild&)=delete;HeldMutationChild& operator=(const HeldMutationChild&)=delete;
};
// The installer script without any window (the payload QA run), at low
// integrity: progress lines and payload-v1 requests until it exits. Owns [pipe].
HeadlessRun runHeadless(const std::filesystem::path& powershell,std::wstring command,const std::filesystem::path& directory,HANDLE pipe,SetupChannel& server){
  multitap::payload::Handle ownedPipe(pipe);
  HeadlessRun run;PROCESS_INFORMATION process{};
  run.started=launchLowIntegrity(powershell,command,directory,process);
  if(!run.started)return run;
  HeldMutationChild heldChild(process.hProcess,ownedPipe);CloseHandle(process.hThread);
  multitap::payload::Handle ownedEvent(CreateEventW(nullptr,TRUE,FALSE,nullptr));OVERLAPPED overlap{};overlap.hEvent=ownedEvent.get();
  const auto abandon=[&]{CancelIoEx(pipe,&overlap);DWORD ignored=0;GetOverlappedResult(pipe,&overlap,&ignored,TRUE);};
  bool connected=false;
  if(overlap.hEvent){
    connected=ConnectNamedPipe(pipe,&overlap)!=FALSE;
    if(!connected){const DWORD connectError=GetLastError();if(connectError==ERROR_PIPE_CONNECTED)connected=true;else if(connectError==ERROR_IO_PENDING){HANDLE waits[]{overlap.hEvent,process.hProcess};connected=WaitForMultipleObjects(2,waits,FALSE,120000)==WAIT_OBJECT_0;if(!connected)abandon();}}
  }
  ULONG client=0;if(connected&&(!GetNamedPipeClientProcessId(pipe,&client)||client!=process.dwProcessId))connected=false;
  run.connected=connected;
  const ULONGLONG deadline=GetTickCount64()+45ULL*60*1000;std::string pending;char buffer[4096];
  while(connected){
    ResetEvent(overlap.hEvent);DWORD count=0;const BOOL read=ReadFile(pipe,buffer,sizeof(buffer),&count,&overlap);
    if(!read&&GetLastError()==ERROR_IO_PENDING){const ULONGLONG now=GetTickCount64();if(now>=deadline||WaitForSingleObject(overlap.hEvent,DWORD(deadline-now))!=WAIT_OBJECT_0){abandon();break;}if(!GetOverlappedResult(pipe,&overlap,&count,FALSE))break;}
    else if(!read)break;
    if(!count)break;pending.append(buffer,count);if(pending.size()>16384)break;
    size_t newline=0;while((newline=pending.find('\n'))!=std::string::npos){
      const std::string line=pending.substr(0,newline);pending.erase(0,newline+1);
      std::string reply;if(server.Receive(line,reply)){if(!reply.empty())writePipeLine(pipe,reply,nullptr);continue;}
      Progress value{};if(parseProgressLine(line,value)){applyUpdateProgress(run.outcome,value);if(value.completed==-1)run.failure=value.message;}
    }
  }
  ownedPipe.reset();
  const ULONGLONG now=GetTickCount64();
  if(WaitForSingleObject(process.hProcess,now<deadline?DWORD(deadline-now):0)!=WAIT_OBJECT_0)heldChild.waitPhysical();
  DWORD exitCode=1;if(!GetExitCodeProcess(process.hProcess,&exitCode)||exitCode==STILL_ACTIVE)exitCode=1;run.exitCode=exitCode;
  return run;
}
// --selftest-payload-protocol: the payload-v1 channel end to end. The harness
// (tests/native-payload-placement.test.ps1) builds an isolated tree under
// MULTITAP_SETUP_QA_ROOT and a fixture script (MULTITAP_SETUP_QA_SCRIPT) made of
// the actual installer code; this setup serves it exactly like an install,
// with the roots redirected into that tree. No window, service or real path.
int runPayloadProtocolSelfTest(){
  const auto root=environmentPath(L"MULTITAP_SETUP_QA_ROOT"),script=environmentPath(L"MULTITAP_SETUP_QA_SCRIPT");
  if(!multitap::payload::canonicalPath(root)||!multitap::payload::canonicalPath(script)||!multitap::payload::plainFile(script)||!multitap::payload::startsWithText(script,root+L"\\")){std::printf("{\"kind\":\"setup-payload-protocol\",\"passed\":false,\"reason\":\"harness_missing\"}\n");return 1;}
  const auto name=newPipeName();const HANDLE pipe=name.empty()?INVALID_HANDLE_VALUE:createProgressPipe(name,true);
  if(pipe==INVALID_HANDLE_VALUE){std::printf("{\"kind\":\"setup-payload-protocol\",\"passed\":false,\"reason\":\"pipe_failed\"}\n");return 1;}
  wchar_t system[32768]{};GetSystemDirectoryW(system,32768);const auto powershell=std::filesystem::path(system)/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
  const std::wstring command=L"\""+powershell.wstring()+L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+script+L"\" -ProgressPipe "+name;
  SetupChannel server;
  const auto run=runHeadless(powershell,command,std::filesystem::path(script).parent_path(),pipe,server);
  const bool succeeded=updateSucceeded(run.outcome,run.exitCode);const auto& stats=server.files.stats();
  // The script's own failure text, for the harness log only (JSON-escaped UTF-8).
  std::string failure;for(const wchar_t ch:run.failure){if(ch==L'"'||ch==L'\\'){failure+='\\';failure+=char(ch);}else if(ch<32)failure+=' ';else if(ch<128)failure+=char(ch);else{char bytes[8]{};const int n=WideCharToMultiByte(CP_UTF8,0,&ch,1,bytes,sizeof(bytes),nullptr,nullptr);failure.append(bytes,size_t(std::max(n,0)));}}
  std::printf("{\"kind\":\"setup-payload-protocol\",\"product\":\"%s\",\"passed\":%s,\"started\":%s,\"connected\":%s,\"exitCode\":%lu,\"completed\":%d,\"failureReported\":%s,\"requests\":%zu,\"placed\":%zu,\"kept\":%zu,\"shortcuts\":%zu,\"restores\":%zu,\"legacyRemoved\":%zu,\"legacyKept\":%zu,\"failures\":%zu,\"lastCode\":\"%s\",\"scriptFailure\":\"%s\",\"scriptIntegrity\":\"low\",\"realInstallTouched\":false}\n",
      "client",succeeded?"true":"false",run.started?"true":"false",run.connected?"true":"false",run.exitCode,run.outcome.completed,run.outcome.failureReported?"true":"false",
      stats.requests,stats.placed,stats.kept,stats.shortcuts,stats.restores,stats.legacyRemoved,stats.legacyKept,stats.failures,stats.lastCode.empty()?"-":stats.lastCode.c_str(),failure.c_str());
  return succeeded?0:1;
}
#endif
// The fixed code a seamless update reports to its app (update-handoff-v1).
// Never the installer's message text: only an early setup failure or the
// generic failure.
std::string seamlessFailureCode(const std::string& early){
  if(!early.empty()&&multitap::relaunch::ValidCode(early))return early;
  return "install_failed";
}
// An unattended update never installs an older version than the one already
// installed. The explicit interactive installer keeps working for a deliberate
// reinstall of an older release. The installed version comes from Program
// Files (administrator-only), never from the HKCU handoff record.
inline std::string setupOwnVersion(){const auto version=multitap::relaunch::Narrow(std::wstring(MULTITAP_CLIENT_VERSION));return version?*version:std::string();}
inline bool unattendedDowngradeRefusedAt(const std::filesystem::path& programFiles,const std::string& own){
  return multitap::relaunch::DowngradeRefused(multitap::relaunch::Product::Client,own,programFiles);
}
inline bool unattendedDowngradeRefused(){
  const auto programs=multitap::relaunch::KnownFolder(FOLDERID_ProgramFiles);
  return programs&&unattendedDowngradeRefusedAt(*programs,setupOwnVersion());
}
inline const wchar_t* DowngradeMessage(){return tx(S::DowngradeClient);}
// "0.4.62로" / "0.2.60으로": the particle follows how the last number is read.
std::wstring koreanTo(const std::wstring& version){
  const auto dot=version.find_last_of(L'.');const std::wstring last=dot==std::wstring::npos?version:version.substr(dot+1);
  int value=0;for(const wchar_t ch:last){if(ch<L'0'||ch>L'9')return version+L"(으)로";value=value*10+(ch-L'0');if(value>1000000)break;}
  const int digit=value%10;const bool vowelOrRieul=digit!=0&&digit!=3&&digit!=6;
  return version+(vowelOrRieul?L"로":L"으로");
}
// The version as the "updated to" text takes it: Korean adds its particle
// ("1.0.1로"); English says "to 1.0.1" in the sentence itself.
std::wstring updatedTo(const std::wstring& version){return koreanUi()?koreanTo(version):version;}
// Pure decision checks for the update modes; no window, pipe or installer.
bool unattendedSelfTests(){
  bool ok=true;const auto expect=[&](bool value,const char* name){if(!value){std::printf("unattendedCheck=%s passed=0\n",name);ok=false;}};
  Progress p{};
  expect(parseProgressLine("6|완료",p)&&p.completed==6&&p.message==L"완료","parse-complete");
  expect(parseProgressLine("-1|install failed\r",p)&&p.completed==-1&&p.message==L"install failed","parse-failure");
  expect(!parseProgressLine("-6|x",p)&&!parseProgressLine("-4|x",p)&&!parseProgressLine("7|x",p)&&!parseProgressLine("3x|x",p)&&!parseProgressLine("nothing",p)&&!parseProgressLine(std::string("2|\xff\xfe"),p),"parse-reject");
  const auto run=[](std::initializer_list<const char*> lines){UpdateOutcome outcome;for(const char* line:lines){Progress value{};if(parseProgressLine(line,value))applyUpdateProgress(outcome,value);}return outcome;};
  auto success=run({"0|a","3|b","5|c","6|d"});
  expect(updateSucceeded(success,0)&&!updateSucceeded(success,1),"success");
  auto reported=run({"0|a","-1|x"});
  expect(!updateSucceeded(reported,1)&&reported.failureReported&&reported.completed==0,"failure-reported");
  auto failed=run({"0|a","3|b"});expect(!updateSucceeded(failed,0),"incomplete");
  expect(interruptedMessage(3).find(koreanUi()?L"멈춘 단계: 4/6 · 바로가기 생성":L"Stopped at step 4/6 · Create the shortcut")!=std::wstring::npos&&interruptedMessage(9).find(L"6/6")!=std::wstring::npos&&interruptedMessage(1).find(tx(S::InterruptedClientRetry))!=std::wstring::npos,"interrupted-message");
  expect(installArguments(L"x",L"p")==L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"x\" -ProgressPipe p","client-no-elevated-script-launch");
  // Seamless handoff (update-handoff-v1): the client marker and the fixed
  // failure codes written for the app.
  expect(parseProgressLine("-5|preflight-complete-v1",p)&&p.completed==-5&&p.message==L"preflight-complete-v1","parse-client-marker");
  expect(seamlessFailureCode("installer_missing")=="installer_missing"&&seamlessFailureCode("")=="install_failed"&&seamlessFailureCode("C:\\secret")=="install_failed","seamless-codes");
  for(const char* code:{"install_failed","installer_missing","progress_channel_failed","installer_start_failed","update_not_newer"})expect(multitap::relaunch::ValidCode(code),"seamless-code-allowlist");
  // No downgrade: the fixed code reaches the app as itself and the message is text in the person's language.
  expect(seamlessFailureCode("update_not_newer")=="update_not_newer"&&std::wstring(DowngradeMessage()).find(koreanUi()?L"낮은 버전":L"older than the installed")!=std::wstring::npos,"downgrade-code-and-message");
  expect(unattendedDowngradeRefusedAt(std::filesystem::path(),"0.0.1")==false,"downgrade-nothing-installed");
  expect(!multitap::relaunch::ValidCode("C:\\Users\\secret")&&!multitap::relaunch::ValidCode("install failed"),"seamless-code-reject");
  expect(koreanTo(L"0.4.62")==L"0.4.62로"&&koreanTo(L"0.2.60")==L"0.2.60으로"&&koreanTo(L"0.2.53")==L"0.2.53으로"&&koreanTo(L"1.0.1")==L"1.0.1로","korean-particle");
  std::printf("{\"kind\":\"unattended-update-decisions\",\"passed\":%s,\"installerExecuted\":false,\"windowsChanged\":false}\n",ok?"true":"false");
  return ok;
}
// --- Seamless update splash (update-handoff-v1) ------------------------------
// The only window of an ordinary update: small, borderless, centred on the
// monitor the app was on, in the app's dark or light appearance, with the
// installer's real completed steps (indeterminate only before the first
// step is reported). No button: nothing here needs a decision. It morphs into
// the full installer window only when a person must read a result the app cannot show.
class UpdateSplash {
 public:
  static constexpr int Width=440,Height=168;
  static const wchar_t* family(){return uiFamily();}
  ~UpdateSplash(){destroy();}
  bool create(const RECT* anchor,bool dark,const std::wstring& subtitle,const std::wstring& note,const S* steps){
    dark_=dark;subtitle_=subtitle;note_=note;steps_=steps;
    WNDCLASSW type{};type.style=CS_DROPSHADOW;type.hInstance=GetModuleHandleW(nullptr);type.lpfnWndProc=procedure;type.lpszClassName=L"MultitapUpdateSplash";type.hCursor=LoadCursorW(nullptr,IDC_APPSTARTING);type.hIcon=LoadIconW(type.hInstance,MAKEINTRESOURCEW(1));RegisterClassW(&type);
    const HMONITOR monitor=anchor?MonitorFromRect(anchor,MONITOR_DEFAULTTOPRIMARY):MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};if(!GetMonitorInfoW(monitor,&info))return false;work_=info.rcWork;
    // Created on the target monitor first so its DPI (per-monitor v2) applies.
    window_=CreateWindowExW(WS_EX_APPWINDOW,type.lpszClassName,tx(S::SplashWindowTitle),WS_POPUP,work_.left,work_.top,1,1,nullptr,nullptr,type.hInstance,this);
    if(!window_)return false;
    place();
    // Windows 11: rounded corners and a hairline system border; ignored before.
    if(HMODULE module=LoadLibraryExW(L"dwmapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)){using Attribute=HRESULT(WINAPI*)(HWND,DWORD,LPCVOID,DWORD);const auto attribute=reinterpret_cast<Attribute>(GetProcAddress(module,"DwmSetWindowAttribute"));if(attribute){const int round=2;attribute(window_,33,&round,sizeof(round));const COLORREF border=palette().line;attribute(window_,34,&border,sizeof(border));}FreeLibrary(module);}
    ShowWindow(window_,SW_SHOWNORMAL);UpdateWindow(window_);SetForegroundWindow(window_);
    SetTimer(window_,1,33,nullptr);
    return true;
  }
  // Completed installer steps; a negative value is "not reported yet".
  void progress(int completed){
    known_=completed>=1;completed_=std::clamp(completed,0,TotalSteps);target_=double(completed_)/TotalSteps;
    if(window_)InvalidateRect(window_,nullptr,FALSE);
  }
  void destroy(){if(window_){KillTimer(window_,1);const HWND window=window_;window_=nullptr;DestroyWindow(window);}}
  // Design previews only: closing the preview ends the program.
  void previewClosable(){closable_=true;}
#ifdef MULTITAP_SETUP_QA
  void qaSettle(){shown_=target_;phase_=.35;}
#endif
  HWND window()const{return window_;}
  const RECT& work()const{return work_;}
  bool dark()const{return dark_;}
  // QA evidence: logical size, centring on the anchor monitor, visibility.
  bool qaGeometry()const{
    if(!window_)return false;RECT r{};GetWindowRect(window_,&r);
    const double w=(r.right-r.left)/scale_,h=(r.bottom-r.top)/scale_;
    const long cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2,wx=(work_.left+work_.right)/2,wy=(work_.top+work_.bottom)/2;
    return std::abs(w-Width)<=1.5&&std::abs(h-Height)<=1.5&&std::abs(cx-wx)<=1&&std::abs(cy-wy)<=1&&IsWindowVisible(window_);
  }
 private:
  struct Palette {COLORREF surface,line,well,ink,muted,faint,track,amber;};
  Palette palette()const{
    return dark_?Palette{RGB(23,27,31),RGB(38,44,50),RGB(29,34,38),RGB(233,241,243),RGB(154,173,182),RGB(94,106,115),RGB(34,40,45),RGB(245,166,35)}
                :Palette{RGB(255,255,255),RGB(220,225,229),RGB(243,245,246),RGB(25,38,45),RGB(89,108,118),RGB(138,148,156),RGB(237,240,242),RGB(245,166,35)};
  }
  HWND window_=nullptr;RECT work_{};double scale_=1,shown_=0,target_=0,phase_=0;bool dark_=true,known_=false,closable_=false;int completed_=0;
  std::wstring subtitle_,note_;const S* steps_=nullptr;
  float px(double value)const{return float(value*scale_);}
  void place(){
    scale_=double(GetDpiForWindow(window_))/96.;const int w=int(std::lround(Width*scale_)),h=int(std::lround(Height*scale_));
    SetWindowPos(window_,nullptr,work_.left+(work_.right-work_.left-w)/2,work_.top+(work_.bottom-work_.top-h)/2,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
  }
  void pill(Gdiplus::Graphics& g,double x,double y,double w,double h,double radius,COLORREF fill,COLORREF* stroke=nullptr){
    const float left=px(x),top=px(y),width=std::max(px(w),px(radius*2)),height=px(h),d=px(radius*2);Gdiplus::GraphicsPath path;
    path.AddArc(left,top,d,d,180,90);path.AddArc(left+width-d,top,d,d,270,90);path.AddArc(left+width-d,top+height-d,d,d,0,90);path.AddArc(left,top+height-d,d,d,90,90);path.CloseFigure();
    Gdiplus::SolidBrush brush(color(fill));g.FillPath(&brush,&path);if(stroke){Gdiplus::Pen pen(color(*stroke),1);g.DrawPath(&pen,&path);}
  }
  void paint(HDC target){
    RECT client{};GetClientRect(window_,&client);if(client.right<=0||client.bottom<=0)return;
    const auto p=palette();Painter painter(client.right,client.bottom,scale_,p.surface);Gdiplus::Graphics& g=painter.g();
    const auto text=[&](const std::wstring& value,double x,double y,double w,double h,double size,COLORREF ink,bool bold=false,bool right=false){painter.text(value,x,y,w,h,size,ink,(bold?Bold:0u)|(right?Right:0u)|Single|CenterV);};
    {Gdiplus::Pen edge(color(p.line),1);g.DrawRectangle(&edge,0.f,0.f,float(client.right-1),float(client.bottom-1));}
    multitap::setup::drawProductIcon(g,scale_,24,24,40);
    text(tx(completed_>=TotalSteps?S::SplashFinishing:S::SplashApplying),78,23,Width-102,24,16.5,p.ink,true);
    text(subtitle_,78,48,Width-102,20,12.5,p.muted);
    // Track and fill: the share of completed installer steps.
    const double trackX=24,trackY=92,trackW=Width-48,trackH=6;
    pill(g,trackX,trackY,trackW,trackH,3,p.track);
    if(known_)pill(g,trackX,trackY,std::max(trackH,trackW*shown_),trackH,3,p.amber);
    else{
      // Indeterminate: one segment travelling through a clipped track.
      const double segment=trackW*.28,x=trackX-segment+(trackW+segment)*phase_;
      Gdiplus::Region clip(Gdiplus::RectF(px(trackX),px(trackY),px(trackW),px(trackH)));g.SetClip(&clip);
      pill(g,x,trackY,segment,trackH,3,p.amber);g.ResetClip();
    }
    std::wstring step=tx(S::SplashChecking);
    if(known_&&steps_)step=completed_>=TotalSteps?std::wstring(tx(S::SplashDone)):std::wstring(tx(steps_[std::min(completed_,TotalSteps-1)]));
    text(step,24,106,Width-48-72,20,12,p.muted);
    if(known_)text(std::to_wstring(completed_)+L" / "+std::to_wstring(TotalSteps),Width-24-72,106,72,20,12,p.faint,false,true);
    text(note_,24,134,Width-48,18,11.5,p.faint);
    painter.present(target);
  }
  LRESULT message(UINT message,WPARAM wp,LPARAM lp){switch(message){
    case WM_TIMER:{
      bool dirty=false;
      if(!known_){phase_+=.022;if(phase_>1)phase_-=1;dirty=true;}
      else if(std::abs(target_-shown_)>.001){shown_+=(target_-shown_)*.22;dirty=true;}
      if(dirty)InvalidateRect(window_,nullptr,FALSE);return 0;}
    case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(window_,&ps);paint(dc);EndPaint(window_,&ps);return 0;}
    case WM_PRINTCLIENT:paint(reinterpret_cast<HDC>(wp));return 0;
    case WM_ERASEBKGND:return 1;
    // Movable like a dialog, never closable: the update is already running.
    case WM_NCHITTEST:return HTCAPTION;
    case WM_CLOSE:if(closable_){destroy();PostQuitMessage(0);}return 0;
    case WM_DPICHANGED:{const auto* rect=reinterpret_cast<const RECT*>(lp);if(rect&&HIWORD(wp)>=48){scale_=HIWORD(wp)/96.;SetWindowPos(window_,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);}return 0;}
  }return DefWindowProcW(window_,message,wp,lp);}
  static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){auto* splash=reinterpret_cast<UpdateSplash*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){splash=static_cast<UpdateSplash*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);splash->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,LONG_PTR(splash));}if(message==WM_NCDESTROY&&splash){SetWindowLongPtrW(window,GWLP_USERDATA,0);splash->window_=nullptr;return DefWindowProcW(window,message,wp,lp);}return splash&&splash->window_?splash->message(message,wp,lp):DefWindowProcW(window,message,wp,lp);}
};
// The splash's step lines and its closing note.
const S* splashSteps(){
  static const S steps[6]={S::ClientSplash1,S::ClientSplash2,S::ClientSplash3,S::ClientSplash4,S::ClientSplash5,S::ClientSplash6};
  return steps;
}
std::wstring splashNote(){return tx(S::SplashNoteClient);}
// Launches [file] with the signed-in user's own (unelevated) token through
// the desktop shell, for a per-user client (launchApp).
bool shellLaunchUnelevated(const std::wstring& file,const std::wstring& arguments,const std::wstring& directory){
  const HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);bool ok=false;
  IShellWindows* windows=nullptr;
  if(SUCCEEDED(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&windows)))){
    VARIANT location{};location.vt=VT_I4;location.lVal=CSIDL_DESKTOP;VARIANT empty{};empty.vt=VT_EMPTY;long handle=0;IDispatch* dispatch=nullptr;
    if(windows->FindWindowSW(&location,&empty,SWC_DESKTOP,&handle,SWFO_NEEDDISPATCH,&dispatch)==S_OK&&dispatch){
      IServiceProvider* provider=nullptr;
      if(SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&provider)))){
        IShellBrowser* browser=nullptr;
        if(SUCCEEDED(provider->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&browser)))){
          IShellView* view=nullptr;
          if(SUCCEEDED(browser->QueryActiveShellView(&view))){
            IDispatch* background=nullptr;
            if(SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND,IID_PPV_ARGS(&background)))){
              IShellFolderViewDual* folder=nullptr;
              if(SUCCEEDED(background->QueryInterface(IID_PPV_ARGS(&folder)))){
                IDispatch* application=nullptr;
                if(SUCCEEDED(folder->get_Application(&application))&&application){
                  IShellDispatch2* shell=nullptr;
                  if(SUCCEEDED(application->QueryInterface(IID_PPV_ARGS(&shell)))){
                    BSTR target=SysAllocString(file.c_str());VARIANT args{};args.vt=VT_BSTR;args.bstrVal=SysAllocString(arguments.c_str());
                    VARIANT dir{};dir.vt=VT_BSTR;dir.bstrVal=SysAllocString(directory.c_str());VARIANT verb{};verb.vt=VT_EMPTY;VARIANT show{};show.vt=VT_I4;show.lVal=SW_SHOWNORMAL;
                    ok=target&&args.bstrVal&&dir.bstrVal&&SUCCEEDED(shell->ShellExecute(target,args,dir,verb,show));
                    SysFreeString(target);VariantClear(&args);VariantClear(&dir);shell->Release();
                  }
                  application->Release();
                }
                folder->Release();
              }
              background->Release();
            }
            view->Release();
          }
          browser->Release();
        }
        provider->Release();
      }
      dispatch->Release();
    }
    windows->Release();
  }
  if(SUCCEEDED(init))CoUninitialize();
  return ok;
}
// Where this setup installs: the fixed folder, never
// chosen (the client's update elevation trusts only this administrator-only
// Program Files folder). The same root setup_uninstall.hpp rootPath() and the
// placement use: %ProgramFiles%\Multitap\Client, from the machine's known folder.
std::wstring installLocationLine(){
  static const std::wstring folder=[]{
    const auto programs=multitap::relaunch::KnownFolder(FOLDERID_ProgramFiles);
    return programs?(*programs/L"Multitap"/L"Client").wstring():std::wstring();
  }();
  return folder.empty()?std::wstring():txf(S::InstallLocation,{folder});
}
class Setup {
 public:
  int run(int selfTest,int preview,bool updateMode=false,std::optional<multitap::relaunch::Record> handoff={}) {
    // Update mode (a verified staged installer): no Install click and no
    // confirmation.
    autoUpdate_=updateMode&&selfTest==0&&preview<0;
    qaUpdate_=selfTest==5;if(qaUpdate_)autoUpdate_=true;
    // Seamless update (update-handoff-v1): the app that started this update
    // wrote a record and waits for this setup's answer. Only a splash shows.
    qaSeamless_=selfTest==7||selfTest==8||selfTest==15?selfTest:0;
#ifdef MULTITAP_SETUP_QA
    qaStart_=selfTest==12?1:selfTest==13||selfTest==15?2:selfTest==14?3:0;
#endif
    if(qaSeamless_){
      autoUpdate_=true;
      // Isolated QA key, fixture versions and the app's event: no product
      // record, process or installation is touched.
      store_=multitap::relaunch::Store(L"Software\\Multitap\\QA\\UpdateHandoff."+std::to_wstring(GetCurrentProcessId()));
      multitap::relaunch::Record record;record.fromVersion="0.0.1";record.toVersion=ownVersion();record.token=multitap::relaunch::NewToken();
      record.created=multitap::relaunch::NowFileTime();record.dark=qaSeamless_!=8;
      MONITORINFO info{sizeof(info)};GetMonitorInfoW(MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY),&info);
      const LONG cx=(info.rcWork.left+info.rcWork.right)/2,cy=(info.rcWork.top+info.rcWork.bottom)/2;record.bounds={cx-400,cy-300,cx+400,cy+300};record.hasBounds=true;
      qaEvent_=CreateEventW(nullptr,TRUE,FALSE,multitap::relaunch::EventName(record.token).c_str());
      if(!qaEvent_||!store_.Write(product(),record))return 1;
      handoff=store_.Read(product());if(!handoff)return 1;
    }
    // Design previews of the splash (dark at step 2, light before any step).
    if(preview==9||preview==10){
      multitap::relaunch::Record record;record.fromVersion="0.4.61";record.toVersion="0.4.62";record.dark=preview==9;
      handoff_=record;seamless_=true;
    }
    if(handoff&&autoUpdate_&&preview<0&&(selfTest==0||qaSeamless_)){
      handoff_=std::move(handoff);seamless_=true;
      readyEvent_=OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,multitap::relaunch::EventName(handoff_->token).c_str());
    }
    previewConfirm_=preview==4;previewNotice_=preview==8;selfTest_=selfTest!=0;expectedInterrupted_=selfTest==11;expectedFailure_=selfTest==2;preview_=preview>=0;view_=preview==1?View::Working:preview==2?View::Complete:preview==3?View::Failed:View::Welcome;
    if(preview==1){completed_=3;message_=tx(S::PreviewProgress);}
    if(preview==3)message_=tx(S::PreviewFailure);
#ifdef MULTITAP_SETUP_QA
    if(preview==20){preview_=false;qaCapture_=true;}
#endif
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);scale_=double(GetDpiForSystem())/96.;
    Gdiplus::GdiplusStartupInput input;if(Gdiplus::GdiplusStartup(&graphics_,&input,nullptr)!=Gdiplus::Ok)return 1;
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);
    WNDCLASSW type{};type.hInstance=GetModuleHandleW(nullptr);type.lpfnWndProc=procedure;type.lpszClassName=L"MultitapProductSetup028";type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.hIcon=LoadIconW(type.hInstance,MAKEINTRESOURCEW(1));RegisterClassW(&type);
    RECT rect{0,0,px(Width),px(Height)};AdjustWindowRectExForDpi(&rect,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,UINT(scale_*96));
    const wchar_t* title=tx(preview_?S::TitlePreview:selfTest_?S::TitleSelfTest:S::TitleClient);
    if(seamless_)title=tx(S::TitleClientUpdate);
    window_=CreateWindowExW(0,type.lpszClassName,title,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,type.hInstance,this);
    if(!window_)return 1;darkTitleBar(window_);
    if(seamless_){
      // The full window stays hidden; it appears only if a person must act.
      const std::wstring from=multitap::relaunch::Widen(handoff_->fromVersion),to=multitap::relaunch::Widen(handoff_->toVersion);
      const auto subtitle=std::wstring(L"Multitap  ")+from+L"  →  "+to;
      if(!splash_.create(handoff_->hasBounds?&handoff_->bounds:nullptr,handoff_->dark,subtitle,splashNote(),splashSteps())){
        // No splash: the ordinary update window still shows the progress.
        seamlessWindowFallback_=true;ShowWindow(window_,SW_SHOW);
      }
      qaSplashGeometry_=splash_.qaGeometry();
      if(preview_){splash_.previewClosable();splash_.progress(preview==9?2:-1);}
    }else ShowWindow(window_,SW_SHOW);
    SetFocus(primary_);if(autoUpdate_&&!selfTest_)PostMessageW(window_,WM_APP+121,0,0);MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window_,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    cleanup();splash_.destroy();if(readyEvent_){CloseHandle(readyEvent_);readyEvent_=nullptr;}
#ifdef MULTITAP_SETUP_QA
    if(qaStart_==3){
      // The window went away while the start was pending: cleanup() waited for
      // the starter and released everything; the script it still started
      // found no progress pipe and ended by itself.
      const bool released=!starter_.joinable()&&!reader_.joinable()&&!started_.load()&&!process_&&pipe_==INVALID_HANDLE_VALUE&&!stop_&&!starting_;
      result_=qaStartClosed_&&released&&startDone_&&qaStartAbandonedEnded_?0:1;
      std::printf("startCase=closed closedWhileStarting=%d starterFinished=%d released=%d abandonedScriptEnded=%d result=%d\n",qaStartClosed_?1:0,startDone_?1:0,released?1:0,qaStartAbandonedEnded_?1:0,result_);std::fflush(stdout);
    }
#endif
    if(qaSeamless_){
      // After the loop: every launch of the run is recorded by now.
      const auto record=store_.Read(product());const bool signalled=qaEvent_&&WaitForSingleObject(qaEvent_,0)==WAIT_OBJECT_0;
      std::string launches;for(const auto& entry:qaLaunches_)launches+=(launches.empty()?"":",")+entry;
      const std::string expectedNew="new:"+ownVersion(),expectedPrevious="previous:0.0.1";
      bool passed=qaVerdict_&&confirmations_==0&&qaSplashGeometry_&&!seamlessWindowFallback_&&signalled;
      if(qaSeamless_==7)passed&=launches==expectedNew&&!qaWindowShown_;
      if(qaSeamless_==8)passed&=launches==expectedPrevious&&!qaWindowShown_;
      if(qaSeamless_==15)passed&=launches==expectedPrevious&&!qaWindowShown_;
      std::printf("seamlessCase=%d splashGeometry=%d readySignalled=%d readyStage=%d outcome=%s code=%s stage=%lu launches=%s fullWindowShown=%d installConfirmations=%d recordRemoved=%d passed=%d\n",
          qaSeamless_,qaSplashGeometry_?1:0,signalled?1:0,qaReadyStage_,qaOutcome_.c_str(),qaCode_.c_str(),qaStage_,launches.empty()?"-":launches.c_str(),qaWindowShown_?1:0,confirmations_,store_.ClearRoot()?1:0,passed?1:0);
      std::printf("{\"kind\":\"seamless-update-handoff\",\"case\":%d,\"passed\":%s,\"installerExecuted\":false,\"productLaunched\":false,\"isolatedRegistryKey\":true,\"windowsChanged\":false}\n",qaSeamless_,passed?"true":"false");
      std::fflush(stdout);(void)record;if(qaEvent_)CloseHandle(qaEvent_);result_=passed?0:1;
    }
    DeleteObject(font_);DeleteObject(errorBackground_);DeleteObject(completeBackground_);DeleteObject(attentionBackground_);Gdiplus::GdiplusShutdown(graphics_);return result_;
  }
 private:
  HWND window_=nullptr,primary_=nullptr,secondary_=nullptr,copy_=nullptr,details_=nullptr,progress_=nullptr,error_=nullptr;
  HFONT font_=nullptr;HBRUSH errorBackground_=nullptr,completeBackground_=nullptr,attentionBackground_=nullptr;ULONG_PTR graphics_=0;
  HANDLE pipe_=INVALID_HANDLE_VALUE,stop_=nullptr,process_=nullptr;std::thread reader_;
  // The installer process is started off this window's thread (start): the
  // starter leaves it in started_ until the window (startFinished) or
  // cleanup() takes it.
  std::thread starter_;std::atomic<HANDLE> started_{nullptr};std::atomic<bool> startDone_{false};bool starting_=false;
#ifdef MULTITAP_SETUP_QA
  // --selftest-start-* (QA binary only): the start made slow (1), failed
  // (2), or slow with the window gone meanwhile (3), and what was seen.
  int qaStart_=0,qaStartTicks_=0;bool qaStartLine_=true,qaStartClosed_=false,qaStartAbandonedEnded_=false;std::atomic<bool> qaStartAnswered_{false};
#endif
  double scale_=1;View view_=View::Welcome;bool selfTest_=false,preview_=false,failureReported_=false,expectedFailure_=false,previewConfirm_=false,previewNotice_=false;std::atomic<bool> readerDone_{true};ULONGLONG exitObserved_=0;
  int completed_=0,result_=0,progressCount_=0,dpiChecks_=0;bool dpiGood_=true,detailsExpanded_=false,autoUpdate_=false;std::wstring message_;
  // Update-mode QA and evidence: confirmations shown and whether a successful
  // update closed itself without a click.
  bool qaUpdate_=false,autoClosed_=false;int confirmations_=0;
  // An installer script that ended without a result (interruptedMessage).
  bool expectedInterrupted_=false;std::wstring interrupted_;
  // A failed relaunch, for the result notes only.
  bool relaunchFailed_=false;
  // --- Seamless update (update-handoff-v1) state.
  std::optional<multitap::relaunch::Record> handoff_;multitap::relaunch::Store store_;UpdateSplash splash_;HANDLE readyEvent_=nullptr;
  bool seamless_=false,ready_=false,relaunched_=false,seamlessWindowFallback_=false;std::string earlyCode_;
  // Seamless QA (cases 7, 8 and 15): recorded evidence.
  int qaSeamless_=0,qaReadyStage_=-1;HANDLE qaEvent_=nullptr;bool qaSplashGeometry_=false,qaWindowShown_=false,qaVerdict_=false;
  std::vector<std::string> qaLaunches_;std::string qaOutcome_,qaCode_;DWORD qaStage_=0;
  static multitap::relaunch::Product product(){return multitap::relaunch::Product::Client;}
  static std::string ownVersion(){const auto version=multitap::relaunch::Narrow(std::wstring(MULTITAP_CLIENT_VERSION));return version?*version:std::string();}
  // The client update never has an attention or restart result to hand over.
  void writeOutcome(const std::string& outcome,const std::string& code,int stage){
    store_.WriteOutcome(product(),outcome,code,DWORD(std::clamp(stage,0,TotalSteps)),false,false);
    if(outcome!="ready"){qaOutcome_=outcome;qaCode_=code.empty()?"-":code;qaStage_=DWORD(std::clamp(stage,0,TotalSteps));}
  }
  // The installer's own checks passed: the app closes; this setup relaunches.
  void signalReady(){
    if(!seamless_||ready_)return;
    ready_=true;qaReadyStage_=completed_;writeOutcome("ready","",completed_);
    if(readyEvent_)SetEvent(readyEvent_);
  }
  bool appAlive()const{return !selfTest_&&multitap::relaunch::AppAlive(product(),handoff_->processId);}
  // The new version after success, the previous one after a failure; always
  // the installed, verified executable of that version, never a record path.
  bool launchApp(bool previous){
    if(relaunched_)return true;
    const std::string version=previous?handoff_->fromVersion:ownVersion();
    if(selfTest_){qaLaunches_.push_back((previous?"previous:":"new:")+version);relaunched_=true;return true;}
#if !defined(MULTITAP_SETUP_QA)
    const auto app=multitap::relaunch::InstalledApp(product(),version);
    if(!app)return false;
    // A per-user client (an earlier layout, under %LOCALAPPDATA%) is never
    // started with this setup's elevated token: the shell opens it and
    // Windows asks for approval, exactly as when the user starts it.
    if(!(app->elevated?multitap::relaunch::Launch(*app):shellLaunchUnelevated(app->executable.wstring(),app->arguments,app->directory.wstring())))return false;
    relaunched_=true;return true;
#else
    return false;
#endif
  }
  void exitSeamless(){if(selfTest_)return;DestroyWindow(window_);}
  // A person must read a result the app cannot show: the full installer
  // window replaces the splash on the same monitor.
  void morph(View view){
    seamless_=false;view_=view;const RECT work=splash_.window()?splash_.work():RECT{0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
    update();RECT frame{};GetWindowRect(window_,&frame);const LONG w=frame.right-frame.left,h=frame.bottom-frame.top;
    SetWindowPos(window_,nullptr,std::max(work.left,work.left+(work.right-work.left-w)/2),std::max(work.top,work.top+(work.bottom-work.top-h)/2),0,0,SWP_NOZORDER|SWP_NOSIZE);
    splash_.destroy();ShowWindow(window_,SW_SHOW);SetForegroundWindow(window_);if(IsWindowEnabled(primary_))SetFocus(primary_);qaWindowShown_=true;
  }
  void failEarly(const std::wstring& message,const char* code){
    if(!seamless_){fail(message);return;}
    message_=message;earlyCode_=code;failureReported_=true;result_=1;finishSeamless();
  }
  void finishSeamless(){
    const bool succeeded=result_==0&&!failureReported_&&completed_==TotalSteps;
    const std::string code=succeeded?std::string():seamlessFailureCode(earlyCode_);
    if(!succeeded&&!ready_){
      // Nothing was replaced: the app, still open, shows the fixed reason.
      writeOutcome("failed",code,completed_);
      if(readyEvent_)SetEvent(readyEvent_);
      if(appAlive()||launchApp(true)){exitSeamless();return;}
      morph(View::Failed);return;
    }
    if(succeeded){
      writeOutcome("completed","",TotalSteps);
      if(launchApp(false)){exitSeamless();return;}
      relaunchFailed_=true;
      message_=txf(S::RelaunchFailedClient,{updatedTo(multitap::relaunch::Widen(ownVersion()))});
      morph(View::Complete);return;
    }
    writeOutcome("failed",code,completed_);
    if(launchApp(true)){exitSeamless();return;}
    morph(View::Failed);
  }
  int px(int value)const{return int(value*scale_);}
  int logicalWidth()const{RECT r{};GetClientRect(window_,&r);return int(r.right/scale_);}
  int logicalHeight()const{RECT r{};GetClientRect(window_,&r);return int(r.bottom/scale_);}
  int footerY()const{return std::min(405,logicalHeight()-65);}
  // --- Result: what happened and what to do ----------------------------------
  // A Complete or Failed view shows a badge, a title and ONE next action. Only
  // when there is more to say, a box sized to short notes follows: a bold line
  // (what happened) over a quieter one (what to do). The installer's full text
  // stays one click away ("자세히 보기") and in "진단 복사".
  struct Note {Badge kind;std::wstring title,detail;};
  static std::wstring trimmed(std::wstring value){
    while(!value.empty()&&(value.back()==L' '||value.back()==L'\r'||value.back()==L'\n'))value.pop_back();
    size_t start=0;while(start<value.size()&&(value[start]==L' '||value[start]==L'\r'||value[start]==L'\n'))++start;
    return value.substr(start);
  }
  // One line of readable text: no JSON diagnostic snapshot (it stays in the
  // details and the diagnostic copy), no line breaks or runs of spaces.
  static std::wstring readable(const std::wstring& raw){
    std::wstring out;int depth=0;
    for(wchar_t ch:visibleText(raw)){
      if(ch==L'{'){++depth;continue;}if(ch==L'}'&&depth){--depth;continue;}if(depth||ch==L'\r')continue;
      if(ch==L'\n'||ch==L'\t')ch=L' ';if(ch==L' '&&!out.empty()&&out.back()==L' ')continue;out+=ch;
    }
    return trimmed(out);
  }
  // "Head · rest" with a short head, else the first sentence, is the bold line.
  static Note note(Badge kind,const std::wstring& raw){
    const auto text=readable(raw);Note result{kind,text,L""};size_t sentence=std::wstring::npos;
    for(size_t i=0;i+1<text.size();i++)if(text[i]==L'.'&&text[i+1]==L' '){sentence=i;break;}
    const size_t head=text.find(L" · ");
    if(head!=std::wstring::npos&&head<=24&&(sentence==std::wstring::npos||head<sentence)){result.title=text.substr(0,head);result.detail=trimmed(text.substr(head+3));}
    else if(sentence!=std::wstring::npos){result.title=text.substr(0,sentence);result.detail=trimmed(text.substr(sentence+1));}
    else if(!result.title.empty()&&result.title.back()==L'.')result.title.pop_back();
    return result;
  }
  // A finished update whose new version did not open by itself.
  std::vector<Note> completionNotes()const{
    std::vector<Note> notes;
    if(relaunchFailed_)notes.push_back({Badge::Warning,tx(S::NoteRelaunchFailed),L""});
    return notes;
  }
  std::vector<Note> failureNotes()const{return {note(Badge::Failure,visibleText(message_))};}
  // An installer that ended without a result (interruptedMessage).
  std::vector<Note> interruptedNotes()const{
    const int step=std::clamp(completed_,0,TotalSteps-1);
    return {{Badge::Failure,tx(S::NoteInterrupted),txf(S::NoteInterruptedDetail,{std::to_wstring(step+1),std::to_wstring(TotalSteps),stepName(step)})},
            {Badge::Success,tx(S::NoteCanReinstall),tx(S::NoteCanReinstallDetail)}};
  }
  std::vector<Note> notes()const{
    if(view_==View::Complete)return completionNotes();
    if(view_!=View::Failed)return {};
    return !interrupted_.empty()&&message_.rfind(interrupted_,0)==0?interruptedNotes():failureNotes();
  }
  struct Heading {Badge badge;std::wstring title,next;bool notice;};
  Heading resultHeading()const{
    if(view_==View::Failed)return {Badge::Failure,tx(autoUpdate_?S::HeadingUpdateFailed:S::HeadingInstallFailed),tx(S::HeadingFailedNext),false};
    const std::wstring title=tx(autoUpdate_?S::HeadingUpdateDone:S::HeadingInstallDone);
    if(relaunchFailed_)return {Badge::Warning,title,tx(S::NextOpenClient),true};
    return {Badge::Success,title,tx(S::NextSignIn),false};
  }
  bool resultView()const{return view_==View::Complete||view_==View::Failed;}
  // "진단 복사" belongs to results with notes only (a failure, a new version that did not open).
  bool diagnosticOffered()const{return view_==View::Failed||(view_==View::Complete&&!notes().empty());}
  std::wstring summaryText()const{std::wstring value;for(const auto& n:notes())value+=(value.empty()?L"":L"\r\n\r\n")+n.title+(n.detail.empty()?L"":L"\r\n"+n.detail);return value;}
  std::wstring fullDetails()const{return visibleText(view_==View::Complete?completionDetails():message_);}
  // The words of a text, without spacing and punctuation: a toggle is offered
  // only when the full text says more than the notes already do.
  static std::wstring words(const std::wstring& value){std::wstring out;for(const wchar_t ch:value)if(!std::wcschr(L" \r\n\t.,:;·‘’'\"()-/",ch))out+=ch;return out;}
  bool longDetails()const{if(!resultView()||notes().empty())return false;const auto full=fullDetails();return !full.empty()&&words(full)!=words(summaryText());}
  std::wstring displayedDetails()const{return detailsExpanded_&&longDetails()?fullDetails():summaryText();}
  std::wstring completionDetails()const{return message_.empty()?std::wstring(tx(S::DetailClientDone)):message_;}
  void copyDiagnostic(){
    // The two closing lines are part of the report's fixed form: this setup never asks for a restart.
    const auto value=diagnosticText(MULTITAP_CLIENT_VERSION,completed_,view_==View::Failed,message_)+txf(S::DiagRestart,{tx(S::DiagHasNot),tx(S::DiagNo)});
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,(value.size()+1)*sizeof(wchar_t));bool copied=false;
    if(memory){auto* bytes=GlobalLock(memory);if(bytes){std::memcpy(bytes,value.c_str(),(value.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);if(OpenClipboard(window_)){if(EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,memory)){copied=true;memory=nullptr;}CloseClipboard();}}if(memory)GlobalFree(memory);}
    SetWindowTextW(copy_,tx(copied?S::Copied:S::CopyAgain));SetTimer(window_,5,2500,nullptr);
  }
#ifdef MULTITAP_SETUP_QA
  // --selftest-capture: each view with the installer script's actual texts at
  // 100% and 150% (some also in the compact 200% client area), each dialog and
  // the update splash, as PNGs in MULTITAP_SETUP_QA_CAPTURE. Nothing installs.
  bool qaCapture_=false;int qaCaptured_=0,qaCaptureFailed_=0;
  void qaDpi(UINT dpi,int width,int height){
    RECT frame{0,0,MulDiv(width,int(dpi),96),MulDiv(height,int(dpi),96)};AdjustWindowRectExForDpi(&frame,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,dpi);
    RECT current{};GetWindowRect(window_,&current);RECT suggested{current.left,current.top,current.left+frame.right-frame.left,current.top+frame.bottom-frame.top};SendMessageW(window_,WM_DPICHANGED,MAKELONG(dpi,dpi),LPARAM(&suggested));
  }
  void qaState(View view,int completed,const std::wstring& message){
    view_=view;completed_=completed;shown_=double(std::clamp(completed,0,TotalSteps))/TotalSteps;spin_=.12;message_=message;
    failureReported_=view==View::Failed;result_=view==View::Failed?1:0;
    detailsExpanded_=false;autoUpdate_=false;
    interrupted_.clear();relaunchFailed_=false;
  }
  // An installer that ended at [completed].
  void qaInterrupted(int completed){
    qaState(View::Failed,completed,L"");interrupted_=interruptedMessage(completed);message_=interrupted_;
  }
  // A fixture text in the language under test: the installer script's own
  // words (Korean) and what an English run shows in their place.
  static std::wstring qa(const wchar_t* korean,const wchar_t* english){return koreanUi()?korean:english;}
  void qaShoot(const std::wstring& name,bool expand=false,bool compact=false){
    const auto one=[&](UINT dpi,int w,int h,const wchar_t* suffix,bool expanded){
      qaDpi(dpi,w,h);detailsExpanded_=expanded&&longDetails();update();KillTimer(window_,7);
      if(capture::window(window_,std::wstring(L"client-")+name+suffix))++qaCaptured_;else ++qaCaptureFailed_;
    };
    one(96,Width,Height,L"-100",false);one(144,Width,Height,L"-150",false);if(compact)one(192,480,380,L"-200-compact",false);
    // "자세히 보기" opened, where the text is long enough to offer it.
    if(expand&&longDetails()){one(96,Width,Height,L"-expanded-100",true);one(144,Width,Height,L"-expanded-150",true);}
    detailsExpanded_=false;qaDpi(96,Width,Height);
  }
  template<class Open> void qaDialog(const std::wstring& name,Open open){
    for(UINT dpi:{96u,144u}){
      Confirmation::qaCaptureName=std::wstring(L"client-")+name+(dpi==96?L"-100":L"-150");Confirmation::qaCaptureDpi=dpi;
      open();if(Confirmation::qaCaptureSaved)++qaCaptured_;else ++qaCaptureFailed_;Confirmation::qaCaptureName.clear();
    }
  }
  void qaSplash(bool dark,int completed,const std::wstring& name){
    UpdateSplash splash;const std::wstring subtitle=std::wstring(L"Multitap  ")+L"0.4.76"+L"  →  "+MULTITAP_CLIENT_VERSION;
    if(!splash.create(nullptr,dark,subtitle,splashNote(),splashSteps())){++qaCaptureFailed_;return;}
    splash.progress(completed);splash.qaSettle();
    if(capture::window(splash.window(),std::wstring(L"client-")+name))++qaCaptured_;else ++qaCaptureFailed_;
    splash.destroy();
  }
  void runCapture(){
    // Texts are the installer script's own.
    qaState(View::Welcome,0,L"");qaShoot(L"01-welcome",false,true);
    qaDialog(L"02-confirm-install",[&]{return confirmInstall(window_,false);});
    qaState(View::Working,2,qa(L"설치 정보를 확인하고 있어요.",L"Checking the installation details."));qaShoot(L"03-working");
    qaDialog(L"04-notice-installing",[&]{Confirmation::notice(window_,InstallingTitle(),InstallingNotice());return false;});
    qaState(View::Complete,6,qa(L"Multitap 설치를 마쳤어요.",L"Multitap is installed."));qaShoot(L"05-complete",false,true);
    qaState(View::Failed,1,qa(L"설치 파일을 확인하지 못했어요. 설치 파일을 다시 받아 실행하세요.",L"Could not verify the setup files. Download the setup file again and run it."));qaShoot(L"06-failed",true,true);
    qaInterrupted(1);qaShoot(L"07-failed-interrupted",true);
    qaState(View::Working,3,qa(L"바로가기를 만들고 있어요.",L"Creating the shortcut."));autoUpdate_=true;qaShoot(L"08-update-working");
    // Install pressed, the installer process not started yet (start).
    qaState(View::Working,0,L"");starting_=true;qaShoot(L"30-starting");starting_=false;
    qaSplash(true,2,L"09-update-splash-dark");qaSplash(false,-1,L"09-update-splash-light-starting");
    std::printf("{\"kind\":\"setup-capture\",\"product\":\"client\",\"captured\":%d,\"failed\":%d,\"installerExecuted\":false,\"windowsChanged\":false}\n",qaCaptured_,qaCaptureFailed_);std::fflush(stdout);
    result_=qaCaptureFailed_==0&&qaCaptured_>0?0:1;DestroyWindow(window_);
  }
#endif
  void cleanup(){
    // A start still pending: ShellExecuteExW cannot be cancelled, so this waits
    // for it as the window's thread used to. The progress pipe goes first: a
    // script that still starts cannot connect and ends before it changes anything.
    if(starter_.joinable()){if(pipe_!=INVALID_HANDLE_VALUE){CloseHandle(pipe_);pipe_=INVALID_HANDLE_VALUE;}starter_.join();}
    starting_=false;
    if(const HANDLE abandoned=started_.exchange(nullptr)){
#ifdef MULTITAP_SETUP_QA
      if(qaStart_==3)qaStartAbandonedEnded_=WaitForSingleObject(abandoned,20000)==WAIT_OBJECT_0;
#endif
      CloseHandle(abandoned);
    }
    if(reader_.joinable()){SetEvent(stop_);CancelIoEx(pipe_,nullptr);reader_.join();}
    if(pipe_!=INVALID_HANDLE_VALUE){CloseHandle(pipe_);pipe_=INVALID_HANDLE_VALUE;}if(stop_){CloseHandle(stop_);stop_=nullptr;}if(process_){CloseHandle(process_);process_=nullptr;}}
  // Working view animation: the current step's spinner and the progress bar
  // easing towards the completed share (timer 7, Working view only).
  double spin_=0,shown_=0;
  // One step of the Working list: done (green check), current (amber ring
  // with a turning arc) or waiting (its number in a hairline ring).
  void stepMark(Painter& p,double cx,double cy,double r,int index){
    Gdiplus::Graphics& g=p.g();const double s=scale_;
    if(index<completed_){drawBadge(p,cx-r,cy-r,r*2,Badge::Success);return;}
    if(index==completed_){
      Gdiplus::Pen track(color(AmberLine),float(2*s));g.DrawEllipse(&track,float((cx-r+1)*s),float((cy-r+1)*s),float((2*r-2)*s),float((2*r-2)*s));
      Gdiplus::Pen arc(color(Blue),float(2*s));arc.SetStartCap(Gdiplus::LineCapRound);arc.SetEndCap(Gdiplus::LineCapRound);
      g.DrawArc(&arc,float((cx-r+1)*s),float((cy-r+1)*s),float((2*r-2)*s),float((2*r-2)*s),float(spin_*360),110.f);return;
    }
    Gdiplus::Pen ring(color(Line),float(1.5*s));g.DrawEllipse(&ring,float((cx-r+1)*s),float((cy-r+1)*s),float((2*r-2)*s),float((2*r-2)*s));
    p.text(std::to_wstring(index+1),cx-r,cy-r,2*r,2*r,std::max(12.0,r*1.05),Muted,Semi|CenterH|CenterV|Single);
  }
  // --- Layout (logical units) ---------------------------------------------
  bool compactLayout()const{return logicalHeight()<430;}
  int headingY()const{return compactLayout()?82:92;}
  int subtitleY()const{return compactLayout()?112:128;}
  static constexpr double NoteTitleSize=13.5,NoteTitleLine=21,NoteDetailSize=12.5,NoteDetailLine=19,LocationRow=24;
  double noteWidth()const{return logicalWidth()-56-46-18;}
  // The result box: its notes with their wrapped line counts, sized to them
  // (fewer detail lines first when space is short), or the whole space for
  // the opened details. The same numbers place the controls and the paint.
  struct NoteRows {int title,detail;};
  struct ResultLayout {std::vector<Note> notes;std::vector<NoteRows> rows;double top=0,height=0,gap=14;bool link=false;};
  ResultLayout resultLayout()const{
    ResultLayout r;r.notes=notes();r.top=compactLayout()?144:168;r.link=longDetails();
    // A finished install keeps one line under the box for its location.
    const double available=std::max(48.0,footerY()-14-r.top-(view_==View::Complete?LocationRow:0));r.height=available;
    if(r.notes.empty()||(detailsExpanded_&&r.link))return r;
    r.gap=r.notes.size()>=3?10:14;
    for(const auto& n:r.notes)r.rows.push_back({std::max(1,lineCount(n.title,noteWidth(),NoteTitleSize,Semi,scale_)),std::min(3,lineCount(n.detail,noteWidth(),NoteDetailSize,0,scale_))});
    const auto height=[&]{double h=16;for(size_t i=0;i<r.rows.size();i++)h+=(i?r.gap:0)+r.rows[i].title*NoteTitleLine+(r.rows[i].detail?2+r.rows[i].detail*NoteDetailLine:0);return h+(r.link?10+24+12:16);};
    // Short of space: one detail line less at a time, from the longest (the
    // later note on a tie); the full text stays behind "자세히 보기".
    while(height()>available){size_t pick=r.rows.size();for(size_t i=0;i<r.rows.size();i++)if(r.rows[i].detail>0&&(pick==r.rows.size()||r.rows[i].detail>=r.rows[pick].detail))pick=i;if(pick==r.rows.size())break;--r.rows[pick].detail;}
    r.height=std::min(height(),available);
    return r;
  }
  Badge severity(const std::vector<Note>& notes)const{Badge worst=Badge::Success;for(const auto& n:notes){if(n.kind==Badge::Failure)return Badge::Failure;if(n.kind==Badge::Warning)worst=Badge::Warning;}return worst;}
  COLORREF boxFill()const{const auto worst=severity(notes());return worst==Badge::Failure?FailedSurface:worst==Badge::Warning?AmberSurface:CompleteSurface;}
  HBRUSH boxBrush()const{const auto fill=boxFill();return fill==FailedSurface?errorBackground_:fill==AmberSurface?attentionBackground_:completeBackground_;}
  // The full text in the details box, wrapped by words like everything else.
  std::wstring editText(const std::wstring& value)const{
    RECT box{};GetClientRect(error_,&box);const int width=box.right-px(2);if(width<=0)return value;
    const HDC dc=GetDC(error_);const HGDIOBJ old=SelectObject(dc,font_);
    const auto lines=wrapWith(value,double(width),[&](const std::wstring& s){SIZE size{};GetTextExtentPoint32W(dc,s.c_str(),int(s.size()),&size);return double(size.cx);});
    SelectObject(dc,old);ReleaseDC(error_,dc);
    std::wstring out;for(size_t i=0;i<lines.size();i++){if(i)out+=L"\r\n";out+=lines[i];}return out;
  }
  // The Working view's line above the bar: the installer's latest message;
  // before its first one, that the installer is starting or getting ready.
  std::wstring workingStatus()const{
    if(!message_.empty())return readable(message_);
    return tx(starting_?(autoUpdate_?S::StartingUpdate:S::StartingInstall):S::PreparingInstall);
  }
  // Header (product mark, product · version · what this window does) over a
  // hairline, then the view. One buffered paint: no flicker while animating.
  void paint(HDC target){
    RECT client{};GetClientRect(window_,&client);if(client.right<=0||client.bottom<=0)return;
    Painter p(client.right,client.bottom,scale_,Canvas);Gdiplus::Graphics& g=p.g();
    const double s=scale_;const int width=logicalWidth(),content=width-56;const bool compact=compactLayout();
    const int heading=headingY(),subtitle=subtitleY();const double titleSize=compact?20:23;
    multitap::setup::drawProductIcon(g,s,28,20,40);
    p.text(L"Multitap",80,19,220,24,17,Ink,Bold|Single|CenterV);
    p.text(std::wstring(L"Client  " MULTITAP_CLIENT_VERSION)+tx(autoUpdate_?S::HeaderUpdate:S::HeaderInstall),80,43,260,18,12,Muted,Single|CenterV);
    if(preview_){const double w=100;roundRect(g,s,width-28-w,28,w,24,12,AmberSurface,&AmberLine);p.text(tx(S::PreviewBadge),width-28-w,28,w,24,12,Amber,Semi|CenterH|CenterV|Single);}
    {Gdiplus::Pen rule(color(Line),float(s));g.DrawLine(&rule,0.f,float(72*s),float(width*s),float(72*s));}
    if(view_==View::Welcome){
      const int card=compact?146:174;
      p.text(tx(S::WelcomeTitleClient),28,heading,content,32,titleSize,Ink,Bold|Single|CenterV);
      p.text(tx(S::WelcomeSubtitleClient),28,subtitle,content,22,13,Muted,Single|CenterV);
      const int cardHeight=compact?118:156;roundRect(g,s,28,card,content,cardHeight,14,Surface,&Line);
      const wchar_t* names[3]={tx(S::CardClient1),tx(S::CardClient2),tx(S::CardClient3)};
      const wchar_t* details[3]={tx(S::CardClientDetail1),tx(S::CardClientDetail2),tx(S::CardClientDetail3)};
      // What gets installed, as a plain list: these are parts, not steps, so
      // no numbers and no amber tiles.
      const double row=compact?35:46;
      for(int i=0;i<3;i++){const double y=card+(compact?8:13)+i*row;
        p.text(names[i],48,y,content-40,19,13.5,Ink,Semi|Single|CenterV);p.text(details[i],48,y+19,content-40,17,12,Muted,Single|CenterV);}
      p.text(tx(S::WelcomeNoteClient),30,card+cardHeight+12,content,36,12,Muted,0,18);
      // The install location, one line under the note, before the Install button.
      {const int noteLines=std::clamp(lineCount(tx(S::WelcomeNoteClient),content,12,0,s),1,2);
       p.text(installLocationLine(),30,card+cardHeight+12+noteLines*18+2,content,18,12,Muted,Single|CenterV);}
      p.text(tx(S::WelcomeFooterClient),28,footerY()+9,width-270,20,12,Muted,CenterV|Single);
    }else if(view_==View::Working){
      // The step list, the latest installer message (up to two lines, keep-all,
      // without the JSON diagnostic) right above the bar, and the bar.
      const int card=compact?140:166,bar=footerY()-22,statusLines=compact?1:2;const double statusLine=18,bottom=bar-10-statusLines*statusLine-10;
      p.text(tx(autoUpdate_?S::WorkingTitleUpdate:S::WorkingTitleInstall),28,heading,content,32,titleSize,Ink,Bold|Single|CenterV);
      p.text(tx(autoUpdate_?S::WorkingSubtitleClientUpdate:S::WorkingSubtitleInstall),28,subtitle,content,22,13,Muted,Single|CenterV);
      roundRect(g,s,28,card,content,bottom-card,14,Surface,&Line);
      const double row=std::min(28.0,(bottom-card-14)/double(TotalSteps)),mark=std::min(9.0,row/2-2),x=52;
      for(int i=0;i<TotalSteps;i++){
        const double cy=card+7+row*i+row/2;
        if(i+1<TotalSteps){Gdiplus::Pen link(color(i<completed_?SuccessLine:Line),float(1.5*s));g.DrawLine(&link,float(x*s),float((cy+mark+2)*s),float(x*s),float((cy+row-mark-2)*s));}
        stepMark(p,x,cy,mark,i);
        p.text(stepName(i),x+mark+14,cy-row/2,content-mark-40,row,compact?12:13,i==completed_?Ink:Muted,(i==completed_?Semi:0u)|CenterV|Single);
      }
      const std::wstring status=workingStatus();
      const int lines=compact?1:std::clamp(lineCount(status,content-64,12.5,0,s),1,statusLines);const double top=bar-10-lines*statusLine;
      p.text(status,28,top,content-64,lines*statusLine,12.5,Muted,compact?(Single|CenterV):0u,statusLine);
      p.text(std::to_wstring(completed_)+L" / "+std::to_wstring(TotalSteps),width-28-56,top,56,statusLine,12,Muted,Right|CenterV|Single);
      roundRect(g,s,28,bar,content,6,3,Line);const double fill=content*std::clamp(shown_,0.0,1.0);if(fill>.5)roundRect(g,s,28,bar,std::max(6.0,fill),6,3,Blue);
    }else{
      const auto r=resultLayout();const auto h=resultHeading();
      if(r.notes.empty()){
        // A plain success needs no box: a large mark, the title and the next step.
        const bool location=view_==View::Complete;const double top=80,bottom=footerY()-8,block=56+20+32+8+22+(location?24:0);
        const double y=top+std::max(0.0,(bottom-top-block)/2);
        drawBadge(p,width/2.0-28,y,56,h.badge);
        p.text(h.title,28,y+76,content,32,compact?21:24,Ink,Bold|Single|CenterH|CenterV);
        p.text(h.next,28,y+116,content,22,13.5,Muted,Single|CenterH|CenterV);
        if(location)p.text(installLocationLine(),28,y+144,content,20,12,Muted,Single|CenterH|CenterV);
      }else{
        drawBadge(p,28,heading+2,28,h.badge);
        p.text(h.title,66,heading,content-38,32,titleSize,Ink,Bold|Single|CenterV);
        p.text(h.next,28,subtitle+4,content,22,13,h.notice?Amber:Muted,Single|CenterV);
        const auto worst=severity(r.notes);const COLORREF edge=worst==Badge::Failure?FailedLine:worst==Badge::Warning?AmberLine:SuccessLine;
        roundRect(g,s,28,r.top,content,r.height,14,boxFill(),&edge);
        if(view_==View::Complete)p.text(installLocationLine(),30,r.top+r.height+6,content,18,12,Muted,Single|CenterV);
        if(!(detailsExpanded_&&r.link)){
          double y=r.top+16;const double limit=r.top+r.height-(r.link?46:8);
          for(size_t i=0;i<r.rows.size();i++){
            const auto& n=r.notes[i];const auto& row=r.rows[i];if(y+row.title*NoteTitleLine>limit+1)break;
            drawBadge(p,46,y+2,17,n.kind);
            p.text(n.title,74,y,noteWidth(),row.title*NoteTitleLine,NoteTitleSize,Ink,Semi,NoteTitleLine);y+=row.title*NoteTitleLine;
            if(row.detail){y+=2;p.text(n.detail,74,y,noteWidth(),row.detail*NoteDetailLine,NoteDetailSize,Muted,0,NoteDetailLine);y+=row.detail*NoteDetailLine;}
            y+=r.gap;
          }
        }else if(error_&&IsWindowVisible(error_)){
          // The details box has no system scroll bar (it would be light on this
          // dark window): a thin indicator shows the scrolled share instead.
          const int total=int(SendMessageW(error_,EM_GETLINECOUNT,0,0)),first=int(SendMessageW(error_,EM_GETFIRSTVISIBLELINE,0,0));
          RECT box{};GetWindowRect(error_,&box);MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&box),2);
          TEXTMETRICW metrics{};const HDC dc=GetDC(error_);const HGDIOBJ old=SelectObject(dc,font_);GetTextMetricsW(dc,&metrics);SelectObject(dc,old);ReleaseDC(error_,dc);
          const int visible=std::max(1,int((box.bottom-box.top)/std::max(1L,metrics.tmHeight)));
          if(total>visible){
            const double trackTop=box.top/s,trackHeight=(box.bottom-box.top)/s,thumb=std::max(18.0,trackHeight*visible/total),offset=(trackHeight-thumb)*std::clamp(double(first)/std::max(1,total-visible),0.0,1.0);
            roundRect(g,s,width-28-13,trackTop,3,trackHeight,1.5,Line);roundRect(g,s,width-28-13,trackTop+offset,3,thumb,1.5,Muted);
          }
        }
      }
    }
    p.present(target);
  }
  HWND button(const wchar_t* title,int id,int x,int width){HWND result=CreateWindowExW(0,L"BUTTON",title,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,px(x),px(405),px(width),px(38),window_,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);SendMessageW(result,WM_SETFONT,WPARAM(font_),TRUE);return result;}
  void layoutControls(){
    const HFONT old=font_;font_=CreateFontW(-int(std::lround(12.5*scale_)),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,uiFamily());
    for(HWND control:{primary_,secondary_,copy_,details_,error_})if(control)SendMessageW(control,WM_SETFONT,WPARAM(font_),TRUE);
    if(error_)SendMessageW(error_,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);
    if(old)DeleteObject(old);
    if(primary_)MoveWindow(primary_,px(logicalWidth()-176),px(footerY()),px(148),px(38),TRUE);
    if(secondary_)MoveWindow(secondary_,px(logicalWidth()-258),px(footerY()),px(70),px(38),TRUE);
    // A quiet text action at the left of the button row.
    if(copy_)MoveWindow(copy_,px(24),px(footerY()+7),px(100),px(24),TRUE);
    if(progress_)MoveWindow(progress_,px(200),px(compactLayout()?289:390),px(logicalWidth()-228),px(5),TRUE);
    // "자세히 보기" at the bottom right inside the result box; the full text
    // fills the box above it when opened.
    const auto r=resultLayout();const int right=logicalWidth()-28;
    if(details_)MoveWindow(details_,px(right-14-118),px(int(r.top+r.height-12-24)),px(118),px(24),TRUE);
    if(error_)MoveWindow(error_,px(46),px(int(r.top+14)),px(logicalWidth()-56-18-30),px(int(std::max(24.0,r.height-14-46))),TRUE);
    if(error_&&IsWindowVisible(error_))SetWindowTextW(error_,editText(displayedDetails()).c_str());
    InvalidateRect(window_,nullptr,TRUE);
  }
  void checkDpiLayouts(){
    const UINT originalDpi=GetDpiForWindow(window_);const int originalCompleted=completed_;
    for(UINT dpi:{96u,120u,144u,192u}){
      RECT frame{0,0,MulDiv(Width,dpi,96),MulDiv(Height,dpi,96)};AdjustWindowRectExForDpi(&frame,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,dpi);
      RECT current{};GetWindowRect(window_,&current);RECT suggested{current.left,current.top,current.left+frame.right-frame.left,current.top+frame.bottom-frame.top};
      SendMessageW(window_,WM_DPICHANGED,MAKELONG(dpi,dpi),LPARAM(&suggested));
      RECT client{};GetClientRect(window_,&client);
      for(HWND control:{primary_,secondary_,copy_,details_,progress_,error_}){RECT rect{};GetWindowRect(control,&rect);MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&rect),2);const bool fits=rect.left>=0&&rect.top>=0&&rect.right<=client.right&&rect.bottom<=client.bottom;dpiGood_&=fits;if(!fits)std::printf("dpi=%u control=%d rect=%ld,%ld,%ld,%ld client=%ld,%ld\n",dpi,int(GetDlgCtrlID(control)),rect.left,rect.top,rect.right,rect.bottom,client.right,client.bottom);}
      dpiGood_&=completed_==originalCompleted&&SendMessageW(progress_,PBM_GETPOS,0,0)==originalCompleted;dpiChecks_++;
    }
    {const UINT dpi=192;RECT frame{0,0,MulDiv(480,dpi,96),MulDiv(380,dpi,96)};AdjustWindowRectExForDpi(&frame,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,dpi);
    RECT current{};GetWindowRect(window_,&current);RECT suggested{current.left,current.top,current.left+frame.right-frame.left,current.top+frame.bottom-frame.top};SendMessageW(window_,WM_DPICHANGED,MAKELONG(dpi,dpi),LPARAM(&suggested));
    RECT client{};GetClientRect(window_,&client);for(HWND control:{primary_,secondary_,copy_,details_,progress_,error_}){RECT rect{};GetWindowRect(control,&rect);MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&rect),2);dpiGood_&=rect.left>=0&&rect.top>=0&&rect.right<=client.right&&rect.bottom<=client.bottom;}
    dpiGood_&=logicalHeight()<430&&completed_==originalCompleted&&SendMessageW(progress_,PBM_GETPOS,0,0)==originalCompleted;dpiChecks_++;}
    RECT frame{0,0,MulDiv(Width,originalDpi,96),MulDiv(Height,originalDpi,96)};AdjustWindowRectExForDpi(&frame,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,originalDpi);
    RECT current{};GetWindowRect(window_,&current);RECT suggested{current.left,current.top,current.left+frame.right-frame.left,current.top+frame.bottom-frame.top};SendMessageW(window_,WM_DPICHANGED,MAKELONG(originalDpi,originalDpi),LPARAM(&suggested));
  }
  void update(){
    const bool working=view_==View::Working&&!preview_;
    EnableWindow(primary_,!working);EnableWindow(secondary_,!working);
    SetWindowTextW(primary_,tx(preview_?S::ButtonClosePreview:view_==View::Working?(autoUpdate_?S::ButtonUpdating:S::ButtonInstalling):view_==View::Welcome?S::ButtonStartInstall:view_==View::Failed?S::ButtonTryAgain:S::Close));
    SetWindowTextW(secondary_,tx(view_==View::Welcome?S::Cancel:S::Close));ShowWindow(secondary_,view_==View::Complete||preview_?SW_HIDE:SW_SHOW);
    // "진단 복사" only with a result that has notes; "자세히 보기" only
    // when the installer's full text says more than the notes.
    const bool result=resultView(),hasDetails=result&&longDetails();
    ShowWindow(copy_,result&&diagnosticOffered()?SW_SHOW:SW_HIDE);EnableWindow(copy_,!working);
    ShowWindow(details_,hasDetails?SW_SHOW:SW_HIDE);EnableWindow(details_,TRUE);SetWindowTextW(details_,tx(detailsExpanded_?S::ButtonCollapse:S::ButtonDetails));
    // The bar is drawn with the step list; the common control keeps the
    // position for QA only and stays hidden.
    ShowWindow(progress_,SW_HIDE);SendMessageW(progress_,PBM_SETPOS,completed_,0);
    if(view_==View::Working)SetTimer(window_,7,33,nullptr);else KillTimer(window_,7);
    ShowWindow(error_,hasDetails&&detailsExpanded_?SW_SHOW:SW_HIDE);
    layoutControls();InvalidateRect(window_,nullptr,TRUE);
  }
  bool confirm(){
    ++confirmations_;
    return confirmInstall(window_,preview_);
  }
  bool waitIo(OVERLAPPED& overlap){HANDLE events[]{stop_,overlap.hEvent};return WaitForMultipleObjects(2,events,FALSE,INFINITE)==WAIT_OBJECT_0+1;}
  void fail(const std::wstring& message){message_=message;view_=View::Failed;result_=1;update();}
  void launch(){
    if(starting_||(view_==View::Working&&!selfTest_))return;
    if(preview_){DestroyWindow(window_);return;}
    if(!selfTest_&&!autoUpdate_&&!confirm())return;
    cleanup();completed_=0;progressCount_=0;failureReported_=false;detailsExpanded_=false;message_.clear();result_=0;exitObserved_=0;autoClosed_=false;
    interrupted_.clear();relaunchFailed_=false;
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);const auto directory=std::filesystem::path(executable).parent_path();const auto script=directory/L"Install-Client.ps1";
    if(!selfTest_&&!std::filesystem::is_regular_file(script)){failEarly(tx(S::SetupFileMissing),"installer_missing");return;}
    // No downgrade through an unattended update (see unattendedDowngradeRefused).
#ifndef MULTITAP_SETUP_QA
    if(autoUpdate_&&!selfTest_&&unattendedDowngradeRefused()){failEarly(DowngradeMessage(),"update_not_newer");return;}
#endif
    start();
  }
  // Starts the installer script. Once the start is under way the Working view
  // shows, and the process or the failure arrives later (startFinished).
  void start(){
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);const auto directory=std::filesystem::path(executable).parent_path();const auto script=directory/L"Install-Client.ps1";
    const std::wstring name=newPipeName();if(name.empty()){failEarly(tx(S::PrepareFailed),"progress_channel_failed");return;}
    pipe_=createProgressPipe(name,false);
    if(pipe_==INVALID_HANDLE_VALUE){failEarly(tx(S::PrepareFailed),"progress_channel_failed");return;}
    stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);wchar_t system[32768]{};GetSystemDirectoryW(system,32768);const auto powershell=std::filesystem::path(system)/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
    // Seamless QA fixtures: the client's "checks passed" marker, then the
    // result or a failure after it.
    const std::wstring marker=L"$w.WriteLine('-5|preflight-complete-v1');";
    const std::wstring seamlessTail=qaSeamless_==7?marker+L"$w.WriteLine('6|검사용 업데이트 완료 · 실제 설치 없음');$w.Dispose();$p.Dispose()":
        marker+L"$w.WriteLine('-1|검사용 업데이트 실패 · 이전 버전 유지 · 실제 설치 없음');$w.Dispose();$p.Dispose();exit 1";
    const std::wstring testPrefix=L"$w.WriteLine('0|설치 화면 검사');$w.WriteLine('3|실제 설치와 Windows 설정 변경 없음');";
    const std::wstring testTail=qaSeamless_?seamlessTail:qaUpdate_?L"$w.WriteLine('6|검사용 업데이트 완료 · 실제 설치 없음');$w.Dispose();$p.Dispose()":expectedFailure_?L"$w.WriteLine('-1|검사용 설치 실패 · 실제 설치 없음');$w.Dispose();$p.Dispose();exit 1":expectedInterrupted_?L"$w.Dispose();$p.Dispose();exit 3":L"$w.WriteLine('6|진행 상태 전달 검사 완료');$w.Dispose();$p.Dispose()";
    const std::wstring arguments=selfTest_?L"-NoProfile -NonInteractive -Command \"$p=New-Object IO.Pipes.NamedPipeClientStream('.', '"+name+L"', [IO.Pipes.PipeDirection]::Out);$p.Connect(5000);$w=New-Object IO.StreamWriter($p,[Text.UTF8Encoding]::new($false));$w.AutoFlush=$true;"+testPrefix+testTail+L"\"":
        installArguments(script.wstring(),name);
    // ShellExecuteExW returns when it likes (the shell and whatever inspects a
    // new process run inside it), and a window that waits for it is frozen and
    // unpainted for as long. It runs on its own thread (an apartment, as the
    // shell asks; the thread ends right after, so the call must not return
    // early), and the window shows the Working view and keeps answering.
    // This setup is already elevated; an update adds no second approval prompt.
    // The script's progress words in this window's language (Install-Client.ps1 reads MULTITAP_LANG).
    SetEnvironmentVariableW(L"MULTITAP_LANG",koreanUi()?L"ko":L"en");
    starting_=true;startDone_=false;
    try{
      starter_=std::thread([this,window=window_,file=powershell.wstring(),arguments,folder=directory.wstring()]{
        HANDLE process=nullptr;
#ifdef MULTITAP_SETUP_QA
        if(qaStart_==1||qaStart_==3){Sleep(600);DWORD_PTR ignored=0;qaStartAnswered_=SendMessageTimeoutW(window,WM_NULL,0,0,SMTO_ABORTIFHUNG,2000,&ignored)!=0;Sleep(900);}
        if(qaStart_!=2)
#endif
        {
          const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
          SHELLEXECUTEINFOW info{sizeof(info)};info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC;info.lpFile=file.c_str();info.lpParameters=arguments.c_str();info.lpDirectory=folder.c_str();info.nShow=SW_HIDE;
          if(ShellExecuteExW(&info))process=info.hProcess;
          if(SUCCEEDED(com))CoUninitialize();
        }
        started_=process;startDone_=true;PostMessageW(window,StartFinishedMessage,0,0);
      });
    }catch(...){starting_=false;cleanup();failEarly(tx(S::StartFailed),"installer_start_failed");return;}
    view_=View::Working;update();
  }
  // The start came back (StartFinishedMessage; timer 7 if that post was lost):
  // the failure, or the process with its progress reader and exit timer.
  void startFinished(){
    if(!starting_||!startDone_)return;
    starter_.join();starting_=false;process_=started_.exchange(nullptr);
    if(!process_){
      cleanup();
      failEarly(tx(S::StartFailed),"installer_start_failed");
#ifdef MULTITAP_SETUP_QA
      if(qaStart_==2){SetTimer(window_,3,200,nullptr);return;}
#endif
      if(selfTest_){result_=1;DestroyWindow(window_);}
      return;
    }
    InvalidateRect(window_,nullptr,FALSE);SetTimer(window_,1,100,nullptr);
    readerDone_=false;reader_=std::thread([this]{
      struct Done {std::atomic<bool>& flag;HWND window;~Done(){flag=true;PostMessageW(window,ReaderFinishedMessage,0,0);}} done{readerDone_,window_};
      // payload-v1: this setup, not the script, writes the files it requests.
      SetupChannel server;
      if(autoUpdate_)server.clientStopGraceMs=20000;
      OVERLAPPED overlap{};overlap.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);bool connected=ConnectNamedPipe(pipe_,&overlap)!=FALSE;const DWORD error=GetLastError();if(!connected&&error==ERROR_IO_PENDING)connected=waitIo(overlap);else if(!connected&&error==ERROR_PIPE_CONNECTED)connected=true;
      ULONG client=0;if(!connected||!GetNamedPipeClientProcessId(pipe_,&client)||client!=GetProcessId(process_)){CloseHandle(overlap.hEvent);return;}
      std::string pending;char buffer[4096];while(WaitForSingleObject(stop_,0)==WAIT_TIMEOUT){ResetEvent(overlap.hEvent);DWORD count=0;BOOL read=ReadFile(pipe_,buffer,sizeof(buffer),&count,&overlap);if(!read&&GetLastError()==ERROR_IO_PENDING){if(!waitIo(overlap)||!GetOverlappedResult(pipe_,&overlap,&count,FALSE))break;}else if(!read)break;if(!count)break;pending.append(buffer,count);if(pending.size()>16384)break;
        size_t newline=0;while((newline=pending.find('\n'))!=std::string::npos){const auto line=pending.substr(0,newline);pending.erase(0,newline+1);
          std::string reply;if(server.Receive(line,reply)){if(!reply.empty())writePipeLine(pipe_,reply,stop_);continue;}
          Progress parsed{};if(!parseProgressLine(line,parsed))continue;
          try{auto* value=new Progress{parsed};if(!PostMessageW(window_,ProgressMessage,0,LPARAM(value)))delete value;}catch(...){}}
      }CloseHandle(overlap.hEvent);
    });
  }
  // The result view is set (the installer ended): show it and do what follows it.
  void conclude(){
    update();
    // Seamless: answer the app, relaunch it, and show a window only when
    // a person must act (update-handoff-v1).
    if(seamless_){finishSeamless();if(selfTest_)SetTimer(window_,3,200,nullptr);return;}
    if(result_==0&&!selfTest_&&!launchApp(false)){
      message_+=tx(S::ClientOpenFailed);update();
    }
    // A plain successful update closes itself.
    if(autoUpdate_&&result_==0){if(selfTest_)autoClosed_=true;else{DestroyWindow(window_);return;}}
    if(selfTest_)SetTimer(window_,3,200,nullptr);
  }
  LRESULT message(UINT message,WPARAM wp,LPARAM lp){
    switch(message){
      case WM_CREATE:
        font_=CreateFontW(-px(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,uiFamily());errorBackground_=CreateSolidBrush(FailedSurface);completeBackground_=CreateSolidBrush(CompleteSurface);attentionBackground_=CreateSolidBrush(AmberSurface);
        primary_=button(tx(S::ButtonStartInstall),1,384,148);secondary_=button(tx(S::Cancel),2,302,70);copy_=button(tx(S::CopyDiagnostic),3,28,132);details_=button(tx(S::ButtonDetails),4,28,142);
        progress_=CreateWindowExW(0,PROGRESS_CLASSW,L"",WS_CHILD|PBS_SMOOTH,px(200),px(390),px(332),px(5),window_,nullptr,GetModuleHandleW(nullptr),nullptr);SetWindowTheme(progress_,L"",L"");SendMessageW(progress_,PBM_SETRANGE32,0,TotalSteps);SendMessageW(progress_,PBM_SETBARCOLOR,0,Blue);SendMessageW(progress_,PBM_SETBKCOLOR,0,Line);
        // The full installer text ("자세히 보기"): selectable, keyboard and wheel
        // scrolling, without the light system scroll bar (paint draws a thin one).
        error_=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,px(45),px(192),px(468),px(140),window_,reinterpret_cast<HMENU>(INT_PTR(5)),GetModuleHandleW(nullptr),nullptr);SendMessageW(error_,WM_SETFONT,WPARAM(font_),TRUE);
        layoutControls();update();if(selfTest_)SetTimer(window_,2,150,nullptr);if(preview_&&(previewConfirm_||previewNotice_))SetTimer(window_,4,150,nullptr);
#ifdef MULTITAP_SETUP_QA
        if(qaCapture_)PostMessageW(window_,WM_APP+130,0,0);
#endif
        return 0;
#ifdef MULTITAP_SETUP_QA
      case WM_APP+130:runCapture();return 0;
#endif
      case WM_PRINTCLIENT:paint(reinterpret_cast<HDC>(wp));return 0;
      case WM_DPICHANGED:{
        const auto* suggested=reinterpret_cast<const RECT*>(lp);if(!suggested||HIWORD(wp)<48)return 0;
        scale_=double(HIWORD(wp))/96.;SetWindowPos(window_,nullptr,suggested->left,suggested->top,suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);layoutControls();return 0;
      }
      case WM_SETFOCUS:if(primary_&&IsWindowEnabled(primary_))SetFocus(primary_);return 0;
      case WM_SIZE:if(primary_)layoutControls();return 0;
      case WM_APP+121:if(autoUpdate_)launch();return 0;
      case WM_MOUSEWHEEL:if(error_&&IsWindowVisible(error_)){SendMessageW(error_,EM_LINESCROLL,0,-GET_WHEEL_DELTA_WPARAM(wp)/40);InvalidateRect(window_,nullptr,FALSE);return 0;}break;
      case WM_COMMAND:
        // The details box scrolled: move the drawn scroll indicator.
        if(error_&&reinterpret_cast<HWND>(lp)==error_){if(HIWORD(wp)==EN_VSCROLL||HIWORD(wp)==EN_UPDATE)InvalidateRect(window_,nullptr,FALSE);return 0;}
        if(LOWORD(wp)==1){if(view_==View::Complete||preview_)DestroyWindow(window_);else launch();}
        else if(LOWORD(wp)==2&&view_!=View::Working)DestroyWindow(window_);
        else if(LOWORD(wp)==3&&(view_==View::Complete||view_==View::Failed))copyDiagnostic();
        else if(LOWORD(wp)==4&&(view_==View::Complete||view_==View::Failed)&&longDetails()){detailsExpanded_=!detailsExpanded_;update();SetFocus(details_);}return 0;
      case ReaderFinishedMessage:if(resultView())update();return 0;
      case StartFinishedMessage:startFinished();return 0;
      case ProgressMessage:{auto* progress=reinterpret_cast<Progress*>(lp);
        progressCount_++;
        // The client installer passed its own package and version checks and
        // is about to replace files: the app closes now (update-handoff-v1).
        if(progress->completed==-5){if(progress->message==L"preflight-complete-v1")signalReady();delete progress;return 0;}
        message_=progress->message;if(progress->completed==-1){failureReported_=true;result_=1;}else if(progress->completed>=completed_)completed_=progress->completed;delete progress;
        if(seamless_)splash_.progress(completed_);
        update();if(selfTest_&&!qaSeamless_&&completed_==3&&!failureReported_&&dpiChecks_==0)checkDpiLayouts();return 0;}
      case WM_TIMER:
#ifdef MULTITAP_SETUP_QA
        // --selftest-start-*: this timer keeps ticking while the start is
        // pending and the starting line shows; case 3 ends the window now
        // (a person's close gets the notice, as during any installation).
        if(wp==7&&starting_&&qaStart_){
          ++qaStartTicks_;qaStartLine_&=view_==View::Working&&workingStatus()==tx(S::StartingInstall)&&!IsWindowEnabled(primary_)&&!IsWindowEnabled(secondary_);
          if(qaStart_==3&&qaStartTicks_==8){qaStartClosed_=!startDone_&&view_==View::Working&&!preview_;DestroyWindow(window_);return 0;}
        }
#endif
        if(wp==7&&starting_&&startDone_){startFinished();return 0;}
        if(wp==7){spin_+=.022;if(spin_>=1)spin_-=1;const double target=double(completed_)/TotalSteps;if(std::abs(target-shown_)>.001)shown_+=(target-shown_)*.18;else shown_=target;InvalidateRect(window_,nullptr,FALSE);return 0;}
        if(wp==5){KillTimer(window_,5);SetWindowTextW(copy_,tx(S::CopyDiagnostic));return 0;}
        if(wp==4){KillTimer(window_,4);if(previewNotice_)Confirmation::notice(window_,InstallingTitle(),InstallingNotice());else confirm();return 0;}
        if(wp==2){KillTimer(window_,2);view_=View::Welcome;launch();if(!process_&&!starting_){result_=1;DestroyWindow(window_);}}
        else if(wp==3){
          KillTimer(window_,3);
          if(qaSeamless_){
            // Seamless handoff fixtures: the answer written for the app, the
            // ready signal relative to the installer steps, and the window.
            bool ok=confirmations_==0;
            if(qaSeamless_==7)ok&=ready_&&qaReadyStage_>=0&&qaReadyStage_<TotalSteps&&qaOutcome_=="completed"&&qaCode_=="-"&&completed_==TotalSteps&&!IsWindowVisible(window_);
            if(qaSeamless_==8)ok&=ready_&&qaOutcome_=="failed"&&qaCode_=="install_failed"&&qaStage_==3&&!IsWindowVisible(window_);
            // The start failed: the app, still open, gets the fixed code; nothing stays behind.
            if(qaSeamless_==15)ok&=!ready_&&qaOutcome_=="failed"&&qaCode_=="installer_start_failed"&&qaStage_==0&&!IsWindowVisible(window_)&&!process_&&pipe_==INVALID_HANDLE_VALUE&&!stop_&&!starter_.joinable();
            const bool decisions=unattendedSelfTests();qaVerdict_=ok&&decisions;
            std::printf("seamlessChecks=%d decisions=%d\n",ok?1:0,decisions?1:0);
            DestroyWindow(window_);
            return 0;
          }
#ifdef MULTITAP_SETUP_QA
          // --selftest-start-failed: the Failed view with its one sentence and
          // "다시 시도", and nothing of the attempt left (no script ran).
          if(qaStart_==2){
            const bool released=!process_&&pipe_==INVALID_HANDLE_VALUE&&!stop_&&!starter_.joinable()&&!reader_.joinable()&&!starting_;
            const bool shown=view_==View::Failed&&message_==tx(S::StartFailed)&&notes().size()==1&&IsWindowEnabled(primary_)&&IsWindowEnabled(secondary_)&&progressCount_==0;
            result_=released&&shown?0:1;
            std::printf("startCase=failed failedView=%d released=%d records=%d result=%d\n",shown?1:0,released?1:0,progressCount_,result_);std::fflush(stdout);DestroyWindow(window_);return 0;
          }
#endif
          if(qaUpdate_)result_=progressCount_==3&&completed_==TotalSteps&&!failureReported_&&view_==View::Complete&&autoClosed_&&confirmations_==0?0:1;
          else if(expectedFailure_)result_=progressCount_==3&&completed_==3&&failureReported_&&view_==View::Failed?0:1;
          // An installer that ends without a result: the stopped step and a
          // possible cause.
          else if(expectedInterrupted_){
            const bool k=koreanUi();
            result_=progressCount_==2&&completed_==3&&!failureReported_&&view_==View::Failed&&message_.find(k?L"멈춘 단계: 4/6":L"Stopped at step 4/6")!=std::wstring::npos&&message_.find(k?L"보안 프로그램":L"security program")!=std::wstring::npos&&
                message_.find(k?L"설치 파일을 다시 실행":L"run the setup file again")!=std::wstring::npos?0:1;
            std::printf("interrupted=1 stoppedStep=%d interruptedResult=%d\n",completed_+1,result_);
          }
          else if(progressCount_!=3||completed_!=TotalSteps)result_=1;
          // "진단 복사" with failure results only; the full text box
          // only while "자세히 보기" is open; a plain success has neither.
          if(dpiChecks_!=5||!dpiGood_||!diagnosticFormatTests()||bool(IsWindowVisible(copy_))!=diagnosticOffered()||IsWindowVisible(error_)||!(GetWindowLongPtrW(error_,GWL_STYLE)&WS_TABSTOP))result_=1;
          if((expectedFailure_||expectedInterrupted_)&&(!diagnosticOffered()||notes().empty()))result_=1;
          if(diagnosticOffered()){SetFocus(copy_);if(GetFocus()!=copy_)result_=1;}
          if(longDetails()){if(!IsWindowVisible(details_)||detailsExpanded_)result_=1;SendMessageW(window_,WM_COMMAND,4,0);if(!detailsExpanded_||!IsWindowVisible(error_)||displayedDetails()!=fullDetails()||GetFocus()!=details_)result_=1;SendMessageW(window_,WM_COMMAND,4,0);if(detailsExpanded_||IsWindowVisible(error_)||displayedDetails()==fullDetails())result_=1;}
#ifdef MULTITAP_SETUP_QA
          // --selftest-start-slow: while the start took 1.5 s the window kept
          // its timer, answered another thread and showed the starting line.
          if(qaStart_==1){const bool good=qaStartTicks_>=10&&qaStartLine_&&qaStartAnswered_;if(!good)result_=1;std::printf("startCase=slow ticksWhileStarting=%d startingLine=%d answeredWhileStarting=%d good=%d\n",qaStartTicks_,qaStartLine_?1:0,qaStartAnswered_?1:0,good?1:0);}
#endif
          const bool confirmationFlow=Confirmation::selfTests(window_);if(!confirmationFlow)result_=1;
          if(qaUpdate_){const bool decisions=unattendedSelfTests();if(!decisions)result_=1;std::printf("updateMode=1 installConfirmations=%d autoClosed=%d decisions=%d\n",confirmations_,autoClosed_?1:0,decisions?1:0);}
          std::printf("completed=%d records=%d dpiChecks=%d dpiGood=%d diagnosticFormat=%d confirmationFlow=%d clipboardChanged=0 result=%d\n",completed_,progressCount_,dpiChecks_,dpiGood_?1:0,diagnosticFormatTests()?1:0,confirmationFlow?1:0,result_);std::fflush(stdout);DestroyWindow(window_);
        }
        else if(process_&&WaitForSingleObject(process_,0)==WAIT_OBJECT_0){if(!exitObserved_)exitObserved_=GetTickCount64();if(!readerDone_&&GetTickCount64()-exitObserved_<2000)return 0;DWORD code=1;GetExitCodeProcess(process_,&code);KillTimer(window_,1);
          result_=code==0&&!failureReported_&&completed_==TotalSteps?0:1;view_=result_==0?View::Complete:View::Failed;
          if(result_!=0&&!failureReported_)message_=interrupted_=interruptedMessage(completed_);
          conclude();}return 0;
      // 1 main (amber), 2 and 3 outlined, 4 "자세히 보기" as a text link.
      case WM_DRAWITEM:{auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);if(item->CtlID<1||item->CtlID>4)return 0;
        const ButtonStyle style=item->CtlID==1?ButtonStyle::Primary:item->CtlID==2?ButtonStyle::Secondary:item->CtlID==3?ButtonStyle::QuietLink:ButtonStyle::Link;
        drawButton(*item,scale_,style,item->CtlID==4?boxFill():Canvas);return TRUE;}
      case WM_CTLCOLOREDIT:case WM_CTLCOLORSTATIC:if(reinterpret_cast<HWND>(lp)==error_){SetBkColor(reinterpret_cast<HDC>(wp),boxFill());SetTextColor(reinterpret_cast<HDC>(wp),Body);return LRESULT(boxBrush());}break;
      case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(window_,&ps);paint(dc);EndPaint(window_,&ps);return 0;}
      case WM_ERASEBKGND:return 1;
      case WM_CLOSE:if(view_==View::Working&&!preview_){Confirmation::notice(window_,InstallingTitle(),InstallingNotice());return 0;}DestroyWindow(window_);return 0;
      case WM_DESTROY:PostQuitMessage(0);return 0;
    }return DefWindowProcW(window_,message,wp,lp);
  }
  static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){auto* setup=reinterpret_cast<Setup*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){setup=static_cast<Setup*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);setup->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,LONG_PTR(setup));}return setup?setup->message(message,wp,lp):DefWindowProcW(window,message,wp,lp);}
};
#include "setup_uninstall_ui.hpp"
int runProductRemoval(bool worker,HANDLE parent=nullptr,HANDLE ready=nullptr,bool preview=false){
  namespace install=multitap::installation;
  const auto roots=setupRoots();const auto reg=install::registration();const auto version=setupVersion();
  if(preview){RemovalWindow window;return window.run(false,{},true);}
#ifdef MULTITAP_SETUP_QA
  (void)worker;(void)parent;(void)ready;(void)roots;(void)reg;(void)version;return ERROR_ACCESS_DENIED;
#else
  if(!worker){RemovalWindow window;return window.run(false,[&]{(void)install::preflight(roots,reg,version);install::launchRemovalWorker(roots);});}
  std::wstring directory;
  try{directory=install::verifyRemovalWorker(roots,parent,ready);}catch(...){return ERROR_ACCESS_DENIED;}
  CloseHandle(parent);CloseHandle(ready);
  RemovalWindow window;const auto result=window.run(true,[&]{
    install::InstallationGate gate;
    const auto plan=install::preflight(roots,reg,version);
    install::removeInstalled(roots,reg,plan);
  });
  install::cleanupRemovalWorker(directory);return result;
#endif
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
  // First thing: a setup is started from Downloads, where anyone may leave a
  // DLL next to it. Libraries load from System32 only; the non-KnownDLL imports are delay-loaded
  // (CMakeLists.txt) so none of them is resolved before this line.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
  int count=0;wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&count);int selfTest=0;int preview=-1;
  const std::wstring first=argv&&count==2?std::wstring(argv[1]):std::wstring();
  bool updateMode=count==2&&first==L"--update";
  // payload-v1 QA: the native placement cases, and the channel end to end.
  const bool payloadSelfTest=count==2&&first==L"--selftest-payload",payloadProtocolSelfTest=count==2&&first==L"--selftest-payload-protocol";
  const bool uninstall=count==2&&first==L"--uninstall",uninstallWorker=count==4&&argv&&std::wstring(argv[1])==L"--uninstall-worker",uninstallPreview=count==2&&first==L"--preview-uninstall",uninstallCheck=count==2&&first==L"--uninstall-check";
  HANDLE uninstallParent=nullptr,uninstallReady=nullptr;
  if(uninstallWorker)try{const auto handle=[](const wchar_t* text){const std::wstring value(text);if(value.empty()||value.size()>20||value.find_first_not_of(L"0123456789")!=std::wstring::npos)throw 1;size_t used=0;const auto number=std::stoull(value,&used);if(used!=value.size()||!number||number>UINTPTR_MAX)throw 1;return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(number));};uninstallParent=handle(argv[2]);uninstallReady=handle(argv[3]);}catch(...){if(argv)LocalFree(argv);return ERROR_INVALID_PARAMETER;}
  if(argv&&count==2){const std::wstring arg=argv[1];selfTest=arg==L"--selftest"?1:arg==L"--selftest-failure"?2:arg==L"--selftest-update"?5:
      arg==L"--selftest-update-splash"?7:arg==L"--selftest-update-splash-failure"?8:arg==L"--selftest-interrupted"?11:0;if(arg==L"--preview-confirm")preview=4;else if(arg==L"--preview")preview=0;else if(arg==L"--preview-progress")preview=1;else if(arg==L"--preview-complete")preview=2;else if(arg==L"--preview-failure")preview=3;else if(arg==L"--preview-notice")preview=8;else if(arg==L"--preview-update-splash")preview=9;else if(arg==L"--preview-update-splash-light")preview=10;}
  if(argv)LocalFree(argv);
#ifdef MULTITAP_SETUP_QA
  // QA binary only: MULTITAP_QA_LANG=en (or ko) runs every view, dialog and
  // check in that language whatever this PC's Windows language is.
  {wchar_t value[8]{};const DWORD length=GetEnvironmentVariableW(L"MULTITAP_QA_LANG",value,DWORD(std::size(value)));
   if(length==2&&std::wstring(value)==L"en")multitap::ui::forceLanguageForTest(multitap::ui::Language::english);
   else if(length==2&&std::wstring(value)==L"ko")multitap::ui::forceLanguageForTest(multitap::ui::Language::korean);}
  if(uninstallPreview)return runProductRemoval(false,nullptr,nullptr,true);
  // Every view and dialog as PNGs (MULTITAP_SETUP_QA_CAPTURE); QA binary only.
  if(count==2&&first==L"--selftest-capture")preview=20;
  // Starting the installer process: slow, failed (window, splash), or the
  // window gone meanwhile; QA binary only.
  if(count==2)selfTest=first==L"--selftest-start-slow"?12:first==L"--selftest-start-failed"?13:first==L"--selftest-start-closed"?14:first==L"--selftest-update-splash-start-failed"?15:selfTest;
#endif
  (void)uninstall;
  (void)uninstallCheck;
  std::optional<multitap::relaunch::Record> handoff;
#ifdef MULTITAP_SETUP_QA
  // This unprivileged QA target cannot enter a real installation flow.
  if(!selfTest&&preview<0&&!payloadSelfTest&&!payloadProtocolSelfTest)return ERROR_ACCESS_DENIED;
  // Only the unprivileged QA binary accepts an isolated-harness output file.
  // The production installer does not compile this environment-variable path.
  wchar_t qaLog[32768]{};const DWORD qaLogSize=GetEnvironmentVariableW(L"MULTITAP_SETUP_QA_LOG",qaLog,DWORD(std::size(qaLog)));
  if(qaLogSize){FILE* stream=nullptr;if(qaLogSize>=std::size(qaLog)||_wfreopen_s(&stream,qaLog,L"w",stdout))return ERROR_WRITE_FAULT;}
  // Headless: isolated temporary trees only, never a real install path.
  if(payloadSelfTest){const bool passed=multitap::payload::qa::Run();std::fflush(stdout);return passed?0:1;}
  if(payloadProtocolSelfTest){const int code=runPayloadProtocolSelfTest();std::fflush(stdout);return code;}
#else
  (void)payloadSelfTest;(void)payloadProtocolSelfTest;
  if(!multitap::requireAdministratorWindow())return ERROR_ELEVATION_REQUIRED;
  if(uninstallCheck){try{namespace install=multitap::installation;const auto roots=setupRoots();install::ensure(multitap::payload::sameText(install::modulePath(),install::rootPath(roots)+L"\\Uninstall.exe"),"uninstall_installed_entry_required");const auto plan=install::preflight(roots,install::registration(),setupVersion());std::printf("{\"kind\":\"native-uninstall-files-registration-preflight\",\"product\":\"client\",\"version\":\"%s\",\"ownedVersions\":%zu,\"changes\":0,\"serviceStops\":0}\n",setupVersion().c_str(),plan.versions.size());return 0;}catch(...){return ERROR_INVALID_DATA;}}
  if(uninstall||uninstallWorker)return runProductRemoval(uninstallWorker,uninstallParent,uninstallReady);
  if(uninstallPreview)return ERROR_ACCESS_DENIED;
  // Released clients started the staged installer without a flag. "--manual"
  // (from /MultitapManual) has two arguments and keeps the graphical installer.
  if(!updateMode&&count==1)updateMode=multitap::verifiedUpdateParent(L"Multitap-" MULTITAP_CLIENT_VERSION L"-windows-x64.exe");
  // An update skips the Install click only when started from a verified
  // protected stage; "--update" from a download folder keeps every confirmation.
  else if(updateMode){
    // The unelevated client reaches this stage through its own protected EXE
    // (product_update.hpp); there is no recognition of a user-writable folder.
    updateMode=multitap::verifiedUpdateParent(L"Multitap-" MULTITAP_CLIENT_VERSION L"-windows-x64.exe");
  }
  // The app that started this update waits for its answer (update-handoff-v1).
  if(updateMode){
    const multitap::relaunch::Store store;
    auto record=store.Read(multitap::relaunch::Product::Client);
    const auto own=multitap::relaunch::Narrow(std::wstring(MULTITAP_CLIENT_VERSION));
    if(record&&own&&record->toVersion==*own&&record->outcome.empty()&&multitap::relaunch::Fresh(*record,multitap::relaunch::NowFileTime()))handoff=std::move(record);
  }
#endif
  Setup setup;return setup.run(selfTest,preview,updateMode,std::move(handoff));
}

