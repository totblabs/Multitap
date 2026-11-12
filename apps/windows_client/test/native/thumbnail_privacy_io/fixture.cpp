#include "thumbnail_writer.h"
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using W=multitap::ThumbnailWriter;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
multitap::DecodedFrame frame(){multitap::DecodedFrame f;f.width=640;f.height=360;f.rgba.resize(size_t(f.width)*f.height*4);for(size_t i=0;i<f.rgba.size();i+=4){f.rgba[i]=57;f.rgba[i+1]=120;f.rgba[i+2]=220;f.rgba[i+3]=255;}return f;}
void idle(W& writer){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);while(!writer.Idle()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(2));require(writer.Idle(),"Writer did not complete its bounded own-file operations");}
std::string epoch(const std::wstring& path){std::ifstream in(std::filesystem::path(path),std::ios::binary);std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),{});require(bytes.size()>70&&bytes[0]==0xff&&bytes[1]==0xd8&&bytes[2]==0xff&&bytes[3]==0xfe,"JPEG has no atomic provenance marker");const size_t length=size_t(bytes[4])*256+bytes[5];require(length>=2&&length+4<bytes.size(),"Malformed provenance segment");return std::string(bytes.begin()+6,bytes.begin()+4+length);}
std::string marker(const W::Target& target){std::ifstream in(std::filesystem::path(target.path+L".revoked"),std::ios::binary);return std::string((std::istreambuf_iterator<char>(in)),{});}
}
int main(){
 const auto root=std::filesystem::temp_directory_path()/(L"multitap-thumbnail-privacy-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
 const auto anchor=std::filesystem::temp_directory_path().lexically_normal();
 int code=0;bool ownsRoot=false;const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 try{
  require(SUCCEEDED(com),"COM initialization failed");
  require(std::filesystem::equivalent(root.parent_path(),anchor)&&!std::filesystem::exists(root),"Fresh own temporary folder required");
  require(std::filesystem::create_directory(root),"Could not create own temporary folder");
  ownsRoot=true;
  ComPtr<IWICImagingFactory> factory;require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))),"WIC unavailable");
  const std::string key="0123456789abcdef",a(32,'a'),b(32,'b');
  const auto target=W::Resolve(key,"pve-149",root.wstring(),a),replacement=W::Resolve(key,"pve-149",root.wstring(),b),other=W::Resolve(key,"pve-150",root.wstring(),a);
  require(target&&replacement&&other,"Valid synthetic target rejected");
  require(!W::Resolve(key,"pve-149",root.wstring(),"bad"),"Malformed epoch accepted");
  W writer;auto ticket=writer.Own(*target);const auto picture=frame();
  require(SUCCEEDED(W::Write(*target,picture,factory.Get(),nullptr,&ticket)),"Normal completed thumbnail could not be written");
  require(epoch(target->path)=="Multitap:thumbnail-privacy-v1:"+a,"Persisted image epoch changed");
  ComPtr<IWICBitmapDecoder> decoder;require(SUCCEEDED(factory->CreateDecoderFromFilename(target->path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)),"Stamped JPEG is not a real decodable image");
  ComPtr<IWICBitmapFrameDecode> decoded;UINT width=0,height=0;require(SUCCEEDED(decoder->GetFrame(0,&decoded))&&SUCCEEDED(decoded->GetSize(&width,&height))&&width==480&&height==270,"Stamped JPEG geometry changed");decoded.Reset();decoder.Reset();
  const auto old=ticket;require(writer.Invalidate(*target,ticket),"Own generation revoke failed");idle(writer);
  require(multitap::ThumbnailGeneration::DurablyRevoked(ticket)&&marker(*target)==a,"Revocation was acknowledged without its flushed epoch marker");
  require(!std::filesystem::exists(target->path),"Privacy revoke left its completed own file");
  require(W::Write(*target,picture,factory.Get(),nullptr,&old)==E_ABORT&&!writer.Submit(*target,std::make_shared<const multitap::DecodedFrame>(picture),old),"Old queued/retired frame republished after revoke");
  auto next=writer.Own(*replacement);require(SUCCEEDED(W::Write(*replacement,picture,factory.Get(),nullptr,&next)),"Fresh owner could not publish");
  require(!std::filesystem::exists(replacement->path+L".revoked"),"Fresh authorized publication did not remove the previous marker");
  require(writer.InvalidateTarget(*target),"Stale Host epoch cancellation failed");idle(writer);
  require(multitap::ThumbnailGeneration::Current(next)&&epoch(replacement->path)=="Multitap:thumbnail-privacy-v1:"+b,"Old Host epoch revoked/deleted its replacement");
  auto otherTicket=writer.Own(*other);require(SUCCEEDED(W::Write(*other,picture,factory.Get(),nullptr,&otherTicket)),"Other desktop write failed");
  require(writer.Submit(*replacement,std::make_shared<const multitap::DecodedFrame>(picture),next),"Actual asynchronous job rejected");
  require(writer.Invalidate(*replacement,next),"Queued job could not be revoked");idle(writer);
  require(!std::filesystem::exists(replacement->path)&&std::filesystem::exists(other->path),"Queued picture returned or another desktop was purged");
  const auto notices=writer.TakeNotices();require(!notices.empty()&&std::all_of(notices.begin(),notices.end(),[](const auto& notice){return notice.invalidated;}),"Revoked publication emitted a success notice");
  require(writer.Submit(*other,std::make_shared<const multitap::DecodedFrame>(picture),otherTicket),"Ordinary disconnect-style publication failed");idle(writer);
  require(epoch(other->path)=="Multitap:thumbnail-privacy-v1:"+a,"Normal final frame was not preserved");
  // Restart/disconnected launcher: no live owner, but an authenticated old
  // epoch can purge only the matching persisted file through its exact handle.
  {W restarted;require(restarted.InvalidateTarget(*other),"Ownerless persisted revoke rejected");idle(restarted);require(!std::filesystem::exists(other->path),"Ownerless old picture survived its purge");}
  require(FAILED(W::Write(*replacement,picture,factory.Get(),nullptr,&next)),"Retired production generation unexpectedly became allowed");
  {W fresh;auto active=fresh.Own(*replacement);require(SUCCEEDED(W::Write(*replacement,picture,factory.Get(),nullptr,&active)),"Fresh persisted image failed");}
  {W oldCleanup;require(oldCleanup.InvalidateTarget(*target),"Ownerless old-epoch cleanup failed");idle(oldCleanup);require(std::filesystem::exists(replacement->path)&&epoch(replacement->path)=="Multitap:thumbnail-privacy-v1:"+b,"Old-epoch cleanup deleted a fresh image at the same path");}
  // A held JPEG can refuse deletion; its flushed marker must still survive.
  {W locked;auto lockedTicket=locked.Own(*replacement);
   HANDLE held=CreateFileW(replacement->path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
   require(held!=INVALID_HANDLE_VALUE,"Could not hold the synthetic JPEG against deletion");
   struct Close{HANDLE handle;~Close(){CloseHandle(handle);}} close{held};
   require(locked.Invalidate(*replacement,lockedTicket),"Locked-file revoke rejected");idle(locked);
   require(multitap::ThumbnailGeneration::DurablyRevoked(lockedTicket)&&std::filesystem::exists(replacement->path)&&marker(*replacement)==b,"Delete failure did not leave a durable revocation");
   {W stale;require(stale.InvalidateTarget(*target),"Stale marker test rejected");idle(stale);require(marker(*replacement)==b,"Old epoch overwrote the newer revocation marker");}
  }
  {W reopened;auto fresh=reopened.Own(*replacement);require(SUCCEEDED(W::Write(*replacement,picture,factory.Get(),nullptr,&fresh))&&!std::filesystem::exists(replacement->path+L".revoked"),"Fresh publication could not recover from a persisted revocation");}
  // An actual native sign-in window locks its shared generation before Start
  // can retire an old picture. Later decoder starts cannot re-arm publication.
  {W signin;auto privateTicket=signin.Own(*replacement);const auto oldPrivate=privateTicket;
   require(multitap::ThumbnailGeneration::Lock(privateTicket),"Sign-in lifetime lock failed");
   require(signin.Invalidate(*replacement,privateTicket),"Sign-in durable purge was not queued");idle(signin);
   const auto durable=privateTicket.stamp;
   for(unsigned restart=0;restart<4;++restart)privateTicket=multitap::ThumbnailGeneration::Change(privateTicket,true);
   require(privateTicket.stamp==durable&&multitap::ThumbnailGeneration::DurablyRevoked(privateTicket),"Decoder restart discarded the durable sign-in revoke");
   require(W::Write(*replacement,picture,factory.Get(),nullptr,&privateTicket)==E_ABORT&&!signin.Submit(*replacement,std::make_shared<const multitap::DecodedFrame>(picture),oldPrivate),"Sign-in or retired frame reached actual WIC/file publication");
   require(!std::filesystem::exists(replacement->path)&&marker(*replacement)==b,"Sign-in restart republished the revoked file");
  }
  std::cout<<"{\"passed\":true,\"checkpoints\":"<<checks<<",\"actualWicAndOwnFileIO\":true,\"syntheticPixels\":true,\"actualVmStream\":false,\"userFilesChanged\":false}\n";
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';code=1;}
 // The only deletion target is the verified fresh direct child of the temp
 // directory created above. No product, user or driver path is inspected.
 if(ownsRoot){std::error_code ignored;if(std::filesystem::equivalent(root.parent_path(),anchor,ignored)&&!ignored)std::filesystem::remove_all(root,ignored);}
 if(SUCCEEDED(com))CoUninitialize();return code;
}
