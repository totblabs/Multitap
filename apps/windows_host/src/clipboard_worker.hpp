#pragma once
#include "machine.hpp"
#include "clipboard.hpp"
#include "storage.hpp"
#include "change_watch.hpp"
#include "request_sequence.hpp"
#include "ack_window.hpp"
#include "pull.hpp"
#include "clipboard_diagnostic.hpp"
#include "clipboard_reconnect.hpp"
#include "clipboard_operation_state.hpp"
#include "settled_selection.hpp"
#include <condition_variable>
#include <deque>
#include <functional>

namespace host {
namespace clip = multitap::clipboard;
inline bool clipboardRequestId(const std::string& id) { return !id.empty() && id.size() <= 64 && id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos; }
// Standard base64 with padding, appended in place. CryptBinaryToStringA took ~300 us per 45 KiB chunk (150 MB/s), the whole of the
// Host's per-chunk cost; this table loop gives the same text (clipboard_pull_cancel_test compares them) several times faster.
inline void clipboardAppendBase64(std::string& out,const uint8_t* data,size_t size){
  static constexpr char table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t start=out.size();out.resize(start+(size+2)/3*4);char* to=out.data()+start;size_t at=0;
  for(;at+3<=size;at+=3,to+=4){const uint32_t v=(uint32_t(data[at])<<16)|(uint32_t(data[at+1])<<8)|data[at+2];to[0]=table[v>>18];to[1]=table[(v>>12)&63];to[2]=table[(v>>6)&63];to[3]=table[v&63];}
  if(at<size){uint32_t v=uint32_t(data[at])<<16;const bool two=at+1<size;if(two)v|=uint32_t(data[at+1])<<8;to[0]=table[v>>18];to[1]=table[(v>>12)&63];to[2]=two?table[(v>>6)&63]:'=';to[3]='=';}
}
inline std::string clipboardBase64(const std::vector<uint8_t>& bytes){std::string out;clipboardAppendBase64(out,bytes.data(),bytes.size());return out;}
// Canonical base64 only: whole quads of the standard alphabet, padding only at the end, and the unused bits of a padded last quad
// zero; exactly the texts that re-encode to themselves (checked here instead of re-encoding, which cost as much as the decode).
inline std::vector<uint8_t> clipboardChunk(const std::string& text,size_t maxBytes=32768) {
  if(text.empty()||text.size()>(maxBytes+2)/3*4||text.size()%4)throw std::runtime_error("Invalid clipboard chunk size");
  static const auto values=[]{std::array<int8_t,256> v{};v.fill(-1);const char table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";for(int i=0;i<64;++i)v[uint8_t(table[i])]=int8_t(i);return v;}();
  const size_t pad=text.back()!='='?0:text[text.size()-2]=='='?2:1;const size_t length=text.size()/4*3-pad;
  if(!length||length>maxBytes)throw std::runtime_error("Invalid clipboard chunk size");
  std::vector<uint8_t> out(length);const auto* in=reinterpret_cast<const uint8_t*>(text.data());uint8_t* to=out.data();int bad=0;
  for(size_t quad=text.size()/4-1;quad;--quad,in+=4,to+=3){
    const int a=values[in[0]],b=values[in[1]],c=values[in[2]],d=values[in[3]];bad|=a|b|c|d;
    const uint32_t v=(uint32_t(a&63)<<18)|(uint32_t(b&63)<<12)|(uint32_t(c&63)<<6)|uint32_t(d&63);
    to[0]=uint8_t(v>>16);to[1]=uint8_t(v>>8);to[2]=uint8_t(v);
  }
  const int a=values[in[0]],b=values[in[1]],c=pad==2?0:values[in[2]],d=pad?0:values[in[3]];
  if((bad|a|b|c|d)<0||(pad==2&&(b&15))||(pad==1&&(c&3)))throw std::runtime_error("Noncanonical clipboard base64");
  const uint32_t v=(uint32_t(a)<<18)|(uint32_t(b)<<12)|(uint32_t(c)<<6)|uint32_t(d);
  to[0]=uint8_t(v>>16);if(pad<2)to[1]=uint8_t(v>>8);if(!pad)to[2]=uint8_t(v);
  return out;
}
inline std::string clipboardDigest(const std::vector<uint8_t>& bytes) {
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0),"Clipboard SHA256 provider");
  // BCryptDestroyHash must run before its caller-owned object buffer is freed.
  std::vector<BYTE> object;
  struct Cleanup{BCRYPT_ALG_HANDLE algorithm;BCRYPT_HASH_HANDLE* hash;~Cleanup(){if(*hash)BCryptDestroyHash(*hash);BCryptCloseAlgorithmProvider(algorithm,0);}} cleanup{algorithm,&hash};
  DWORD size=0,got=0;check(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&got,0),"Clipboard SHA256 size");object.resize(size);check(BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0),"Clipboard SHA256 create");if(!bytes.empty())check(BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),0),"Clipboard SHA256 data");std::array<BYTE,32> digest{};check(BCryptFinishHash(hash,digest.data(),DWORD(digest.size()),0),"Clipboard SHA256 finish");const char hex[]="0123456789abcdef";std::string out;for(auto value:digest){out+=hex[value>>4];out+=hex[value&15];}return out;
}
inline bool clipboardDesktopReady() {
  DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);if(!session||!interactiveDesktopAvailable())return false;
  wchar_t desktop[64]{};DWORD needed=0;if(!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof(desktop),&needed)||_wcsicmp(desktop,L"Default"))return false;
  return true;
}
inline bool clipboardRuntimeReady() {
  if(!clipboardDesktopReady())return false;
  const auto owner=CreateWindowExW(0,L"STATIC",L"Multitap Clipboard",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);if(!owner)return false;DestroyWindow(owner);return true;
}
inline std::filesystem::path clipboardTransferParent() {
  PWSTR local=nullptr;check(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&local),"Clipboard local storage");std::filesystem::path parent(local);CoTaskMemFree(local);parent/=L"Multitap";parent/=L"ClipboardTransfers";
  return parent;
}
struct ClipboardWorkerAccess {
  std::function<clip::Snapshot(HWND,bool,bool)> read=[](HWND window,bool text,bool files){return clip::read(window,text,files);};
  std::function<DWORD(HWND,const std::wstring&,std::optional<DWORD>)> putText=[](HWND window,const std::wstring& text,std::optional<DWORD> expected){return clip::putText(window,text,expected);};
  std::function<DWORD(HWND,const std::vector<std::filesystem::path>&,std::optional<DWORD>)> putFiles=[](HWND window,const std::vector<std::filesystem::path>& files,std::optional<DWORD> expected){return clip::putFiles(window,files,expected);};
  // The formats profile reads rich text and images as well (see clip::ReadOptions).
  std::function<clip::Snapshot(HWND,const clip::ReadOptions&)> readFormats=[](HWND window,const clip::ReadOptions& options){return clip::read(window,options);};
  // The paste-pull profile publishes the peer's offer as an OLE data object (virtual files) on this worker's STA thread.
  std::function<DWORD(const std::shared_ptr<clip::PullOffer>&,std::optional<DWORD>)> publishOffer=[](const std::shared_ptr<clip::PullOffer>& offer,std::optional<DWORD> expected){return clip::publishPullOffer(offer,expected);};
  std::function<bool(const std::shared_ptr<clip::PullOffer>&)> offerCurrent=[](const std::shared_ptr<clip::PullOffer>& offer){return clip::pullOfferIsCurrent(offer);};
  // Clears the clipboard when (and only when) that offer is still on it; returns the sequence after clearing it, 0 when nothing was touched.
  std::function<DWORD(const std::shared_ptr<clip::PullOffer>&)> withdrawOffer=[](const std::shared_ptr<clip::PullOffer>& offer){return clip::withdrawPullOffer(offer);};
  clip::ChangeWatchAccess watch;
  // Which kinds of content are on the clipboard (a bit mask, diagnostics only; no content is read).
  std::function<uint32_t()> formats=[]{return clip::availableFormats();};
  // How a received rich text / image / folder tree is published; unset members publish on the Windows clipboard (QA stand-ins replace them).
  clip::Publishers publishers;
  std::function<uint64_t()> monotonic=[]{return GetTickCount64();};
};
class ClipboardWorker {
 public:
  using Send=std::function<void(const std::string&)>;
  using Check=std::function<bool()>;
  ClipboardWorker(std::string sessionId,Send send,Check check=clipboardDesktopReady,std::filesystem::path transferParent={},ClipboardWorkerAccess access={}):sessionId_(std::move(sessionId)),send_(std::move(send)),check_(std::move(check)),transferParent_(std::move(transferParent)),access_(std::move(access)),watch_([this]{return !stopped_&&ready_&&watchWanted_&&(textToLocal_||filesToLocal_)&&check_();},[this](const std::string&,DWORD sequence){observedChange(sequence);},access_.watch){winCheck(wake_.value!=nullptr,"Clipboard wake event");
    // A published offer is an OLE data object: no window of ours owns the clipboard, so its sequence marks our own write.
    watch_.ownContent([this](DWORD sequence){const DWORD own=ownSequence_.load();return own!=0&&own==sequence;});
    // A change that was seen and not announced is recorded with its fixed reason (hostAction 253, stage changeSkipped).
    watch_.onSkip([this](uint32_t reason){note(ClipboardDiagnostic::changeSkipped,reason);});
    thread_=std::thread([this]{run();});}
  ~ClipboardWorker(){stop();}
  void stop(){stopped_=true;ready_=false;watchWanted_=false;cancelled_=true;
    // A paste that is waiting inside a read must end now, or the worker thread cannot leave.
    {std::vector<std::shared_ptr<clip::PullOffer>> live;{std::lock_guard lock(pullMutex_);live=pullOffers_;}for(auto& offer:live)offer->shutdown("clipboard_transport_closed");}
    SetEvent(wake_.value);condition_.notify_all();if(thread_.joinable()){CancelSynchronousIo(thread_.native_handle());thread_.join();}}
  void message(const Json& value,uint64_t generation=0){
    const auto type=value.get("type").string(),id=value.get("requestId").string();
    // The channel's count of this connection's messages that reached the socket (ClipboardChannel, never from the relay): see waitRoom.
    if(type=="clipboard.flow"){const double sent=value.get("sent").number(-1);if(sent<0)return;
      {std::lock_guard lock(flowMutex_);flowGeneration_=generation;flowSent_=uint64_t(sent);}SetEvent(wake_.value);return;}
    if(generation&&generation<pairGeneration_)return;
    // Data of a range this Host asked for (pullh-<n>) goes straight to the virtual files: the reader that waits for
    // it runs on the worker thread, so it must never queue behind that thread.
    if(clip::pullRequestId(id)&&id[4]=='h'&&type!="clipboard.pull"&&type!="clipboard.manifest"){if(!stopped_)try{pullIncoming(value);}catch(...){}return;}
    // The PC's clipboard changed: its offer is no longer served. What waits for it ends now (a render blocked on the worker thread must not
    // keep the worker from clearing the clipboard), and the worker clears the clipboard if the offer is still on it.
    // A newer copy is about to replace the clipboard: what is blocked waiting for the older offer's data ends first (it would keep the worker from publishing).
    if(type=="clipboard.offer"||type=="clipboard.begin")supersedeRenders();
    if(type=="clipboard.channel.closed"||(type=="clipboard.error"&&id.empty())){
      ready_=false;
      // A reader can block this STA worker waiting for its old peer. Wake it
      // before queueing teardown, which runs on that same worker.
      std::vector<std::shared_ptr<clip::PullOffer>> live;
      {std::lock_guard lock(pullMutex_);live=pullOffers_;}
      for(auto& offer:live)offer->shutdown("clipboard_transport_closed");
    }
    if(type=="clipboard.offer.withdraw"){
      if(stopped_)return;
      retireOffer(id);
      std::lock_guard lock(mutex_);if(!stopped_){queue_.push_back(queued(value,generation));condition_.notify_all();SetEvent(wake_.value);}
      return;
    }
    std::lock_guard lock(mutex_);
    if(stopped_)return;
    // What is still in flight for a pull this Host served (pullc-<n>) after it ended is dropped. In a pull queue the PC may also cancel
    // a pull that waits behind the one being served: it is skipped when its turn comes (one that already ended is never served again).
    if(clip::pullRequestId(id)&&id!=activeId_&&type!="clipboard.pull"&&type!="clipboard.manifest"){
      // (A list request waits in this Host's own queue too: its cancel is remembered without the relay's pull queue as well.)
      if((pullQueue_||deferred_)&&id[4]=='c'&&(type=="clipboard.cancel"||type=="clipboard.error")){skippedPulls_.push_back(id);while(skippedPulls_.size()>16)skippedPulls_.pop_front();}
      return;
    }
    if(type=="clipboard.watch") {
      const auto* fields=std::get_if<Json::Object>(&value.value);
      if(!fields||fields->size()!=size_t(intent_?4:3)||value.get("sessionId").string()!=sessionId_||!std::holds_alternative<bool>(value.get("enabled").value))return;
      if(intent_){const auto epoch=value.get("watchEpoch").number(-1);if(epoch<1||epoch>0xffffffff||std::floor(epoch)!=epoch)return;
        if(value.get("enabled").boolean()){if(epoch<=double(requestedWatchEpoch_))return;requestedWatchEpoch_=uint32_t(epoch);}
        else if(epoch!=double(requestedWatchEpoch_))return;}
      // Preserve ordering with old-channel teardown and the new ready packet.
      if(!value.get("enabled").boolean())watchWanted_=false;
      if(queue_.size()<64)queue_.push_back(queued(value,generation));
      condition_.notify_all();SetEvent(wake_.value);return;
    }
    if(type=="clipboard.ready") {
      const bool valid=value.get("protocol").string()=="clipboard-v1"&&value.get("sessionId").string()==sessionId_;
      if(!valid)return;
      const auto& policy=value.get("clipboardPolicy");
      textToLocal_=valid&&policy.get("textToLocal").boolean();filesToLocal_=valid&&policy.get("filesToLocal").boolean();
      textToRemote_=valid&&policy.get("textToRemote").boolean();filesToRemote_=valid&&policy.get("filesToRemote").boolean();
      limits_=Limits{};const auto& offered=value.get("limits");
      // The scale revision (relay header X-Multitap-Clipboard-Scale): every number exactly the one both peers know, or the pair keeps limits v2.
      if(offered.get("scaleProfile").number(-1)==1&&offered.get("limitsProfile").number(-1)==2&&offered.get("formatsProfile").number(-1)==2&&offered.get("pullProfile").number(-1)==1&&offered.get("maxOffers").number(-1)==double(clip::maxLiveOffers)&&
         offered.get("chunkBytes").number(-1)==double(clip::formatsChunkBytes)&&offered.get("windowUnits").number(-1)==double(clip::SendWindow::defaultUnits)&&offered.get("windowBytes").number(-1)==double(clip::SendWindow::defaultBytes)&&
         offered.get("maxFiles").number(-1)==double(clip::maxEntries)&&offered.get("maxFileBytes").number(-1)==double(clip::scaleBytes)&&offered.get("maxTotalBytes").number(-1)==double(clip::scaleBytes)&&
         offered.get("maxTextBytes").number(-1)==double(clip::maxTextBytes)&&offered.get("maxTextUnits").number(-1)==double(clip::maxTextUnits)&&offered.get("maxEntries").number(-1)==double(clip::maxEntries)&&
         offered.get("maxDepth").number(-1)==double(clip::maxDepth)&&offered.get("maxPathChars").number(-1)==double(clip::maxPathChars)&&offered.get("maxRichBytes").number(-1)==double(clip::maxRichBytes)){
        limits_=Limits{clip::maxEntries,clip::scaleBytes,clip::scaleBytes,clip::maxTextBytes,clip::maxTextUnits,true,true,true};limits_.scale=true;limits_.maxEntries=clip::maxEntries;limits_.maxDepth=clip::maxDepth;limits_.maxPathChars=clip::maxPathChars;limits_.maxRichBytes=clip::maxRichBytes;
        if(offered.get("onPasteProfile").number(-1)==1&&offered.get("pushTextBytes").number(-1)==double(clip::pushTextBytes)&&offered.get("pushImageBytes").number(-1)==double(clip::pushImageBytes))limits_.onPaste=true;
      }else
      if(offered.get("limitsProfile").number(-1)==2){const double files=offered.get("maxFiles").number(-1),file=offered.get("maxFileBytes").number(-1),total=offered.get("maxTotalBytes").number(-1),textBytes=offered.get("maxTextBytes").number(-1),units=offered.get("maxTextUnits").number(-1);
        if(files>=1&&files<=double(clip::maxFiles)&&file>=0&&file<=double(clip::maxFileBytes)&&total>=file&&total<=double(clip::maxTotalBytes)&&textBytes>=0&&textBytes<=double(clip::maxTextBytes)&&units>=0&&units<=double(clip::maxTextUnits)){limits_=Limits{uint64_t(files),uint64_t(file),uint64_t(total),uint64_t(textBytes),uint64_t(units),true};
          // The formats profile (rich text, images, folder trees, sliding window) announces its own bounds; anything else keeps stop and wait.
          if(offered.get("formatsProfile").number(-1)==2&&offered.get("chunkBytes").number(-1)==double(clip::formatsChunkBytes)&&offered.get("maxEntries").number(-1)==10000&&offered.get("windowUnits").number(-1)==double(clip::SendWindow::defaultUnits)&&offered.get("windowBytes").number(-1)==double(clip::SendWindow::defaultBytes)){limits_.formats=true;
            // The paste-pull profile: files and folders are offered by manifest and pulled when pasted.
            if(offered.get("pullProfile").number(-1)==1&&offered.get("maxOffers").number(-1)==double(clip::maxLiveOffers))limits_.pull=true;}}}
      settled_=limits_.onPaste&&value.get("clipboardSync").boolean()&&value.get("clipboardSettled").string()=="text-image-250-v1";
      intent_=settled_&&value.get("clipboardIntent").string()=="text-image-pending-v1";
      // The relay's pull queue: four pulls per requester, served in order, 4 MiB windows.
      pullQueue_=limits_.onPaste&&value.get("clipboardPullQueue").string()=="pull-queue-4-v1";skippedPulls_.clear();
      // Deferred manifest (like RDP): a copy that is not small is announced without its list, which is walked when a paste asks for it.
      deferred_=limits_.onPaste&&value.get("clipboardDeferred").string()=="manifest-on-paste-v1";
      requestedWatchEpoch_=0;
      if(!activeId_.empty())cancelled_=true;SetEvent(wake_.value);
    }
    if(type=="clipboard.channel.closed"||(type=="clipboard.error"&&id.empty())){ready_=false;settled_=false;intent_=false;pullQueue_=false;deferred_=false;watchWanted_=false;cancelled_=true;queue_.clear();acks_.clear();queue_.push_front(queued(value,generation));condition_.notify_all();SetEvent(wake_.value);return;}
    if(sequenceSync_&&!id.empty()&&id!=activeId_&&type!="clipboard.read"&&type!="clipboard.begin"&&type!="clipboard.offer"&&type!="clipboard.pull"&&type!="clipboard.manifest") {
      if(requestSequence_.inspect(id)==clip::RequestSequenceStatus::Retired)return;
      // A newer copy ends the older one still crossing (sync); a paste this Host serves (pullc-<n>: a file, a list) is the
      // receiver's own request and goes on while the PC's next copy waits its turn in the queue.
      if(!activeId_.empty()&&!clip::pullRequestId(activeId_))cancelled_=true;
    }
    if(type=="clipboard.chunk.ack"||type=="clipboard.item.ack"||(type=="clipboard.complete"&&sending_)){
      if(id==activeId_&&acks_.size()<64)acks_.push_back(value);
      else if(sequenceSync_&&requestSequence_.inspect(id)!=clip::RequestSequenceStatus::Retired){if(!activeId_.empty()&&!clip::pullRequestId(activeId_))cancelled_=true;if(queue_.size()<8)queue_.push_back(queued(value,generation));}
      condition_.notify_all();SetEvent(wake_.value);return;
    }
    if((type=="clipboard.cancel"||type=="clipboard.error")&&id==activeId_){cancelled_=true;peerEnd_=type=="clipboard.error"?clipboardReasonCode(value.get("reason").string()):1000;}
    if(queue_.size()>=64){cancelled_=true;condition_.notify_all();SetEvent(wake_.value);return;}
    queue_.push_back(queued(value,generation));condition_.notify_all();SetEvent(wake_.value);
  }
 private:
  std::string sessionId_;Send send_;Check check_;std::filesystem::path transferParent_;std::atomic<bool> stopped_{false},cancelled_{false};std::thread thread_;HWND owner_=nullptr;
  ClipboardWorkerAccess access_;
  std::atomic<uint64_t> pairGeneration_{0};
  // Flow of the channel's queue (waitRoom): messages emitted for a generation, and the channel's latest count of them sent.
  std::mutex flowMutex_;uint64_t flowGeneration_=0,flowSent_=0,emittedGeneration_=0,emitted_=0;
  static Json queued(const Json& value,uint64_t generation){auto copy=value;if(generation)std::get<Json::Object>(copy.value)["_channelGeneration"].value=double(generation);return copy;}
  // The size limits of this peer pair (clipboard.ready). Released relays send the
  // v1 numbers without limitsProfile; the extended profile also unlocks the newer
  // failure reasons. Written by message() under mutex_, read by the worker.
  struct Limits{uint64_t maxFiles=128,maxFileBytes=256ull*1024*1024,maxTotalBytes=512ull*1024*1024,maxTextBytes=131072,maxTextUnits=32768;bool extended=false,formats=false,pull=false;
    // The on-paste revision: a copy is silent (files and big content are offered, pulled when pasted); only small text and images are pushed.
    bool onPaste=false;
    // The formats profile bounds (released pairs: 10,000 entries, 32 levels, 160 characters, 8 MiB of HTML) and the scale revision.
    bool scale=false;size_t maxEntries=10000,maxDepth=32,maxPathChars=160,maxRichBytes=8*1024*1024;
    size_t chunkBytes()const{return formats?clip::formatsChunkBytes:32768;}
    clip::Bounds bounds()const{clip::Bounds value;value.tops=size_t(maxFiles);value.fileBytes=maxFileBytes;value.totalBytes=maxTotalBytes;value.textBytes=size_t(maxTextBytes);value.entries=maxEntries;value.depth=maxDepth;value.pathChars=maxPathChars;value.richBytes=maxRichBytes;return value;}};
  Limits limits_;
  Limits limits(){std::lock_guard lock(mutex_);return limits_;}
  std::atomic<bool> ready_{false},watchWanted_{false},textToLocal_{false},filesToLocal_{false},textToRemote_{false},filesToRemote_{false};
  std::atomic<bool> settled_{false};
  std::atomic<bool> intent_{false};
  // The relay's pull queue (clipboardPullQueue: pull-queue-4-v1): the PC may queue four pulls; this Host serves them in order.
  std::atomic<bool> pullQueue_{false};
  // The relay's deferred manifest (clipboardDeferred: manifest-on-paste-v1).
  std::atomic<bool> deferred_{false};
  // Lists this Host asked the PC for (pullh-<n>, pullMutex_): the offer waiting for it and the pages received so far.
  struct ManifestIncoming{std::weak_ptr<clip::PullOffer> offer;std::string offerId;bool begun=false;size_t entries=0,files=0,tops=0;uint64_t bytes=0;std::vector<clip::Item> items;std::vector<int64_t> mtimes;};
  std::map<std::string,ManifestIncoming> manifestIn_;
  static constexpr size_t pullQueueWindowUnits=128;static constexpr uint64_t pullQueueWindowBytes=4ull*1024*1024;
  std::deque<std::string> skippedPulls_;  // mutex_: pulls the PC cancelled while they waited behind another
  uint32_t watchEpoch_=0;
  uint32_t requestedWatchEpoch_=0; // submit mutex protects queued watch order.
  std::optional<DWORD> intentSequence_;
  std::string intentKind_;
  clip::ChangeWatch watch_;
  WinHandle wake_{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
  std::mutex mutex_;std::condition_variable condition_;std::deque<Json> queue_,acks_;std::string activeId_;bool sending_=false;
  bool sequenceConfigured_=false,sequenceSync_=false;clip::RequestSequence requestSequence_;
  std::set<std::string> used_;std::unique_ptr<clip::FileReceiver> receiver_;std::shared_ptr<clip::Storage> storage_;std::vector<uint8_t> text_;std::vector<uint64_t> sizes_;size_t index_=0;uint64_t offset_=0;bool textFormat_=false,published_=false;
  std::optional<DWORD> receiveSequence_;
  // Host diagnostics (hostAction 253, clipboard_diagnostic.hpp): direction, format, item count and size of the active transfer.
  bool diagActive_=false,diagSending_=false;uint32_t diagFormat_=0;uint64_t diagItems_=0,diagBytes_=0;
  // Read-time tree bound only, not the actual enumerated entry count. A read
  // can fail before noteBegin knows the content's format/count/bytes.
  uint32_t diagExpectedEntries_=0;
  // Set by message() when the peer cancelled (1000) or failed (its reason code) the active transfer while this thread is inside it.
  std::atomic<uint32_t> peerEnd_{0};
  // Formats profile: rich text, an image or a folder tree being received. A tree is described by
  // clipboard.entries pages first; the receiver is created once they are complete.
  std::unique_ptr<clip::Incoming> incoming_;std::string incomingFormat_;std::vector<clip::Item> pendingItems_;
  struct TreeStage{bool waiting=false;size_t entries=0,files=0,tops=0;uint64_t bytes=0;};TreeStage tree_;
  clip::AckPolicy ackPolicy_{false};
  // Paste-pull. Receiving an offer: `offerMode_` while its manifest arrives; the published virtual files (the newest is
  // current, an older one may still be pasting); the ranges they asked for, by pull id. Offering: the copies this
  // Host kept (latest two) and the file being served.
  bool offerMode_=false;std::vector<int64_t> pendingMtimes_;
  clip::OfferRegistry offers_;std::mutex pullMutex_;std::vector<std::shared_ptr<clip::PullOffer>> pullOffers_;std::shared_ptr<clip::PullOffer> currentOffer_;
  std::map<uint64_t,std::weak_ptr<clip::PullOffer>> rangeOffers_;std::map<uint64_t,std::string> pullIdOfRange_;std::map<std::string,uint64_t> rangeOfPullId_;uint64_t pullCounter_=0;
  std::atomic<DWORD> ownSequence_{0};
  std::set<std::string> withdrawn_;  // offers the PC withdrew (pullMutex_): one withdrawn before it was published is retired as soon as it is
  std::atomic<bool> waitingNow_{false},waitingSent_{false};std::atomic<uint32_t> waitingCounter_{0};
  ClipboardOperationState pasteOperations_;  // pullMutex_: one read-wait flag and operation per published offer/generation
  void cancelReceive(){if(receiver_&&!published_)try{receiver_->cancel();}catch(...){}if(incoming_&&!published_)try{incoming_->cancel();}catch(...){}}
  // The content of the last completed read (sent to the client): names, sizes,
  // SHA-256 per item and the clipboard sequence it was read at. A client that
  // sends exactly this back while the clipboard is unchanged is echoing it.
  struct Sent{bool text=false;std::vector<std::wstring> names;std::vector<uint64_t> sizes;std::vector<std::string> digests;DWORD sequence=0;};
  std::optional<Sent> lastSent_;bool echo_=false;
  // Files being received: names/sizes and each verified SHA-256, in order.
  std::vector<clip::FileDescription> receiveExpected_;std::vector<std::string> receiveDigests_;
  // The clipboard still yields exactly the received content: text lives in
  // the clipboard (an unchanged sequence suffices); listed files are re-read.
  // The files on the clipboard now, for the receive checks; a clipboard this cannot read as files (virtual files, shell-only items) has none.
  std::vector<std::filesystem::path> currentFiles(){try{return access_.read(owner_,false,true).files;}catch(...){return {};}}
  bool holdsReceived(){if(textFormat_)return true;try{return clip::filesHoldContent(currentFiles(),receiveExpected_,receiveDigests_);}catch(...){return false;}}
  void require(bool value){if(!value)throw std::runtime_error("Invalid clipboard transfer");}
  // A transfer ends when the worker stops or the peer cancelled it; a locked screen or a secure desktop (UAC, Ctrl+Alt+Del) is its own
  // condition, so the peer is told "no interactive session" and the record carries its own cause (not a generic write failure).
  void current(){require(!stopped_&&!cancelled_);if(!check_())throw std::runtime_error("clipboard_desktop_unavailable");}
  void permitted(bool sending,bool text){require(sending?(text?textToLocal_.load():filesToLocal_.load()):(text?textToRemote_.load():filesToRemote_.load()));}
  void emit(const std::string& message){emit(message,pairGeneration_.load());}
  void emit(const std::string& message,uint64_t generation){if(stopped_)throw std::runtime_error("Clipboard worker stopped");if(!ready_||generation!=pairGeneration_)return;
    std::string packet;const auto head="{\"type\":\"clip.packet\",\"sessionId\":"+jsonQuote(sessionId_)+",\"generation\":"+std::to_string(generation)+",\"payload\":";
    packet.reserve(head.size()+message.size()+1);packet+=head;packet+=message;packet+='}';send_(packet);
    std::lock_guard lock(flowMutex_);if(emittedGeneration_!=generation){emittedGeneration_=generation;emitted_=0;}++emitted_;}
  // A list's pages have no window: the next page goes out only while at most 96 messages of this worker still wait in the channel's
  // queue (which ends the channel beyond 192 messages or 12 MiB; the relay holds this side back while the receiver catches up). STA:
  // pumps meanwhile; the request's cancel and the desktop are honoured. No word from the channel for 60 s fails the request.
  void waitRoom(){
    auto heard=Clock::now();uint64_t lastSent=0;
    for(;;){
      {std::lock_guard lock(flowMutex_);const auto g=pairGeneration_.load();
        // (A count without a generation, a channel that does not tag its messages, is this connection's.)
        const uint64_t sent=flowGeneration_==g||flowGeneration_==0?flowSent_:0;
        if(emittedGeneration_!=g||emitted_<=sent+96)return;
        if(sent!=lastSent){lastSent=sent;heard=Clock::now();}}
      pump(20);current();
      if(Clock::now()-heard>std::chrono::seconds(60))throw std::runtime_error("clipboard_timeout");
    }
  }
  std::string fields(const std::string& type,const std::string& id){return "{\"type\":"+jsonQuote(type)+",\"requestId\":"+jsonQuote(id);}
  // A data chunk, built once: base64 (A-Z a-z 0-9 + / =) needs no JSON escaping.
  std::string chunkMessage(const std::string& id,size_t index,uint64_t offset,const std::vector<uint8_t>& bytes){
    std::string out=fields("clipboard.chunk",id)+",\"index\":"+std::to_string(index)+",\"offset\":"+std::to_string(offset)+",\"data\":\"";
    out.reserve(out.size()+(bytes.size()+2)/3*4+2);clipboardAppendBase64(out,bytes.data(),bytes.size());out+="\"}";return out;
  }
  // Fixed reasons the relay accepts from a peer (server peerErrors). A read
  // that only met a transient busy clipboard, a folder or a file another
  // program holds is reported as such, not as a generic read failure.
  // The newer reasons (limits, disk full, locked file) go out only when the pair
  // negotiated the extended profile; a released relay would reject them, so the
  // closest released reason is sent instead.
  static const char* failureReason(const std::string& what,bool reading,bool extended=false,bool scale=false){
    if(what=="clipboard_desktop_unavailable")return "no_interactive_session";
    if(what=="clipboard_changed")return "clipboard_changed";
    if(what=="clipboard_busy")return "clipboard_busy";
    if(what=="clipboard_policy_denied")return "clipboard_policy_denied";
    if(what=="clipboard_disk_full")return extended?"clipboard_disk_full":"clipboard_write_failed";
    // The scale revision names the limit that was met (text, entries, depth, path, image); the relay would downgrade them for a peer without it.
    if(scale&&(what=="clipboard_path_too_long"||what=="clipboard_text_too_large"||what=="clipboard_entry_count_limit"||what=="clipboard_depth_limit"||what=="clipboard_image_too_large"))return what=="clipboard_path_too_long"?"clipboard_path_too_long":what=="clipboard_text_too_large"?"clipboard_text_too_large":what=="clipboard_entry_count_limit"?"clipboard_entry_count_limit":what=="clipboard_depth_limit"?"clipboard_depth_limit":"clipboard_image_too_large";
    if(what=="clipboard_text_too_large"||what=="clipboard_file_count_limit"||what=="clipboard_file_name_or_size_limit"||what=="clipboard_total_size_limit"||
       what=="clipboard_image_too_large"||what=="clipboard_entry_count_limit"||what=="clipboard_depth_limit"||what=="clipboard_path_too_long")return extended?"clipboard_limits_exceeded":(reading?"clipboard_read_failed":"clipboard_write_failed");
    if(!reading)return "clipboard_write_failed";
    if(what=="clipboard_file_locked")return extended?"clipboard_file_locked":"clipboard_file_unavailable";
    if(what=="unsupported_clipboard_format"||what=="clipboard_regular_files_only")return "unsupported_clipboard_format";
    if(what=="clipboard_selected_file_unavailable"||what=="clipboard_selected_file_changed")return "clipboard_file_unavailable";
    return "clipboard_read_failed";
  }
  // The reasons a pull may end with (the relay accepts the changed-file and expired-offer reasons for pull ids only).
  static const char* pullReason(const std::string& what){
    if(what=="clipboard_selected_file_changed")return "clipboard_selected_file_changed";
    if(what=="clipboard_offer_expired")return "clipboard_offer_expired";
    if(what=="clipboard_file_locked")return "clipboard_file_locked";
    if(what=="clipboard_selected_file_unavailable")return "clipboard_file_unavailable";
    if(what=="clipboard_policy_denied")return "clipboard_policy_denied";
    return "clipboard_transfer_failed";
  }
  void error(const std::string& id,const char* reason){if(clipboardRequestId(id)&&!stopped_)try{emit(fields("clipboard.error",id)+",\"reason\":"+jsonQuote(reason)+'}');}catch(...) {}}
  // One fixed record for the service's journal (never content, names or exception text); a closed pipe is ignored.
  // win32: why a clipboard write or open failed (clip::writeFailureDetail: Win32 error in the low 16 bits, who held the clipboard,
  // retried after self-heal); 0 when no Windows call failed. Carried only by failures.
  void diagnostic(uint32_t stage,bool ok,uint32_t errorCode=0,uint32_t cause=0,bool pull=false,uint32_t win32=0){
    if(stopped_)return;
    const auto expected=!ok&&cause==clipboardCauseCode("clipboard_entry_count_limit")?diagExpectedEntries_:0;
    try{send_("{\"type\":\"clip.diag\",\"sessionId\":"+jsonQuote(sessionId_)+",\"stage\":"+std::to_string(stage)+",\"ok\":"+(ok?"true":"false")+",\"error\":"+std::to_string(errorCode)+",\"cause\":"+std::to_string(cause)+",\"win32\":"+std::to_string(win32)+",\"format\":"+std::to_string(pull?6u:diagFormat_)+",\"items\":"+std::to_string(pull?0ull:std::min<uint64_t>(diagItems_,65535))+",\"kib\":"+std::to_string(pull?0ull:std::min<uint64_t>(diagBytes_/1024,4294967295ull))+(expected?",\"expected\":"+std::to_string(expected):std::string())+'}');}catch(...){ }
  }
  // An observation of this VM's own clipboard (no transfer): stage, fixed reason, and the formats that were present.
  void note(uint32_t stage,uint32_t reason,uint32_t formats=0){
    if(stopped_)return;
    try{send_("{\"type\":\"clip.diag\",\"sessionId\":"+jsonQuote(sessionId_)+",\"stage\":"+std::to_string(stage)+",\"ok\":"+((stage==ClipboardDiagnostic::changeAnnounced||stage==ClipboardDiagnostic::changeSkipped)?"true":"false")+",\"error\":"+std::to_string(reason)+",\"cause\":0,\"format\":0,\"items\":"+std::to_string(formats)+",\"kib\":0}");}catch(...){ }
  }
  void noteBegin(bool sending,uint32_t format,uint64_t items,uint64_t bytes){diagActive_=true;diagSending_=sending;diagFormat_=format;diagItems_=items;diagBytes_=bytes;diagnostic(sending?ClipboardDiagnostic::sendBegin:ClipboardDiagnostic::receiveBegin,true);}
  void noteEnd(){if(!diagActive_)return;diagActive_=false;diagnostic(diagSending_?ClipboardDiagnostic::sendEnd:ClipboardDiagnostic::receiveEnd,true);}
  // A transfer (or pull) ended in failure: who failed and why. `reason` is what was or will be sent to the peer.
  void noteFailure(const Json& request,const char* what,const char* reason){
    const auto id=request.get("requestId").string();const bool pull=clip::pullRequestId(id);const auto type=request.get("type").string();
    // Only a transfer (or pull) is reported, not a stray packet that failed to parse.
    if(!diagActive_&&!pull&&type!="clipboard.read"&&type!="clipboard.begin"&&type!="clipboard.offer"){peerEnd_=0;return;}
    const uint32_t stage=pull?ClipboardDiagnostic::pullFailed:diagActive_?(diagSending_?ClipboardDiagnostic::sendFailed:ClipboardDiagnostic::receiveFailed):(request.get("type").string()=="clipboard.read"?ClipboardDiagnostic::sendFailed:ClipboardDiagnostic::receiveFailed);
    if(const uint32_t peer=peerEnd_.exchange(0)){diagnostic(peer==1000?ClipboardDiagnostic::peerCancelled:ClipboardDiagnostic::peerFailed,false,peer==1000?0:peer,clipboardCauseCode(what),pull);if(!pull)diagActive_=false;return;}
    diagnostic(stage,false,cancelled_?clipboardReasonCode("clipboard_cancelled"):clipboardReasonCode(reason),clipboardCauseCode(what),pull,clip::writeFailureDetail());if(!pull)diagActive_=false;
  }
  void clearDiagnostic(){diagActive_=false;diagSending_=false;diagFormat_=0;diagItems_=0;diagBytes_=0;diagExpectedEntries_=0;}
  void reset(){clearDiagnostic();peerEnd_=0;offerMode_=false;pendingMtimes_.clear();receiver_.reset();incoming_.reset();incomingFormat_.clear();pendingItems_.clear();tree_=TreeStage{};ackPolicy_=clip::AckPolicy(false);text_.clear();sizes_.clear();index_=0;offset_=0;textFormat_=false;published_=false;echo_=false;receiveExpected_.clear();receiveDigests_.clear();receiveSequence_.reset();std::lock_guard lock(mutex_);activeId_.clear();sending_=false;acks_.clear();cancelled_=false;}
  bool beginId(const std::string& id,bool sending){require(clipboardRequestId(id));std::lock_guard lock(mutex_);
    if(sequenceSync_&&id!=activeId_&&requestSequence_.inspect(id)==clip::RequestSequenceStatus::Retired)return false;
    require(activeId_.empty());
    if(sequenceSync_)require(requestSequence_.accept(id)==clip::RequestSequenceStatus::Accepted);
    else require(used_.size()<4096&&used_.insert(id).second);
    clearDiagnostic();
    activeId_=id;sending_=sending;acks_.clear();cancelled_=false;return true;
  }
  Json wait(const std::string& type,const std::string& id,size_t index=0,uint64_t offset=0){
    const auto deadline=Clock::now()+std::chrono::seconds(30);Json response;
    for(;;){current();permitted(true,textFormat_);{std::lock_guard lock(mutex_);if(!acks_.empty()){response=std::move(acks_.front());acks_.pop_front();break;}}
      require(Clock::now()<deadline);pump(100);
    }
    require(response.get("type").string()==type&&response.get("requestId").string()==id);if(type!="clipboard.complete")require(response.get("index").number(-1)==double(index));if(type=="clipboard.chunk.ack")require(response.get("offset").number(-1)==double(offset));return response;
  }
  std::string sendItem(const std::string& id,size_t index,const std::vector<uint8_t>& bytes){
    uint64_t offset=0;while(offset<bytes.size()){current();permitted(true,textFormat_);const size_t count=std::min<size_t>(32768,bytes.size()-size_t(offset));std::vector<uint8_t> chunk(bytes.begin()+offset,bytes.begin()+offset+count);emit(chunkMessage(id,index,offset,chunk));offset+=count;wait("clipboard.chunk.ack",id,index,offset);}
    const auto digest=clipboardDigest(bytes);
    emit(fields("clipboard.item.end",id)+",\"index\":"+std::to_string(index)+",\"sha256\":"+jsonQuote(digest)+'}');wait("clipboard.item.ack",id,index);
    return digest;
  }
  // Blocks (pumping messages) for the next acknowledgement of the active transfer.
  Json nextAck(std::chrono::seconds idle=std::chrono::seconds(30)){
    const auto deadline=Clock::now()+idle;
    for(;;){current();permitted(true,textFormat_);{std::lock_guard lock(mutex_);if(!acks_.empty()){auto response=std::move(acks_.front());acks_.pop_front();return response;}}require(Clock::now()<deadline);pump(100);}
  }
  void applyAck(clip::SendWindow& window,const std::string& id,const Json& ack){
    require(ack.get("requestId").string()==id);const auto type=ack.get("type").string();const double index=ack.get("index").number(-1);require(index>=0);
    if(type=="clipboard.chunk.ack"){const double offset=ack.get("offset").number(-1);require(offset>=0);window.ackChunk(size_t(index),uint64_t(offset));}
    else if(type=="clipboard.item.ack")window.ackItem(size_t(index));
    else throw std::runtime_error("Invalid clipboard transfer");
  }
  // Sends every item with up to a window of chunks / item ends unacknowledged; acknowledgements are cumulative.
  template<class SizeOf,class ReadChunk,class DigestOf>
  void streamWindowed(const std::string& id,size_t items,SizeOf sizeOf,ReadChunk readChunk,DigestOf digestOf,const Limits& L,std::vector<std::string>* digests=nullptr){
    clip::SendWindow window;
    const auto drain=[&]{for(;;){Json ack;{std::lock_guard lock(mutex_);if(acks_.empty())return;ack=std::move(acks_.front());acks_.pop_front();}applyAck(window,id,ack);}};
    for(size_t index=0;index<items;++index){
      const uint64_t size=sizeOf(index);
      for(uint64_t offset=0;offset<size;){
        current();permitted(true,textFormat_);drain();
        const size_t count=size_t(std::min<uint64_t>(L.chunkBytes(),size-offset));
        while(!window.canSend(count))applyAck(window,id,nextAck());
        const auto chunk=readChunk(index,offset,count);
        emit(chunkMessage(id,index,offset,chunk));
        offset+=count;window.sentChunk(index,offset,count);
      }
      const auto digest=digestOf(index);if(digests)digests->push_back(digest);
      while(window.units()>=clip::SendWindow::defaultUnits)applyAck(window,id,nextAck());
      emit(fields("clipboard.item.end",id)+",\"index\":"+std::to_string(index)+",\"sha256\":"+jsonQuote(digest)+'}');window.sentItemEnd(index);
    }
    while(!window.empty())applyAck(window,id,nextAck());
  }
  // Rich text, an image or a folder tree (formats profile): manifest, entries, data, commit.
  void sendOutgoing(const std::string& id,clip::Outgoing& out,const Limits& L){
    const std::string format=out.format();textFormat_=format=="richtext";permitted(true,textFormat_);noteBegin(true,clipboardFormatCode(format),out.dataCount(),out.totalBytes());
    std::string begin=fields("clipboard.begin",id)+",\"format\":"+jsonQuote(format);
    if(format=="tree"){
      begin+=",\"entries\":"+std::to_string(out.items().size())+",\"files\":"+std::to_string(out.dataCount())+",\"bytes\":"+std::to_string(out.totalBytes())+",\"tops\":"+std::to_string(out.tops())+",\"skipped\":"+std::to_string(out.skipped())+'}';emit(begin);
      size_t start=0;std::string page;size_t inPage=0;
      const auto flush=[&]{if(!inPage)return;emit(fields("clipboard.entries",id)+",\"start\":"+std::to_string(start)+",\"entries\":["+page+"]}");start+=inPage;page.clear();inPage=0;};
      for(const auto& item:out.items()){
        std::wstring wire=item.name;std::replace(wire.begin(),wire.end(),L'\\',L'/');
        if(inPage)page+=',';
        page+="{\"path\":"+jsonQuote(utf8(wire))+",\"kind\":"+jsonQuote(item.kind)+(item.kind=="file"?",\"size\":"+std::to_string(item.size):std::string())+",\"attrs\":"+std::to_string(item.attributes)+'}';
        if(++inPage>=200||page.size()>40000)flush();
      }
      flush();
    }else{
      begin+=",\"items\":[";bool first=true;
      for(const auto& item:out.items()){if(!first)begin+=',';first=false;begin+="{\"kind\":"+jsonQuote(item.kind)+",\"size\":"+std::to_string(item.size)+(item.kind=="png"?",\"width\":"+std::to_string(item.width)+",\"height\":"+std::to_string(item.height):std::string())+'}';}
      begin+="]}";emit(begin);
    }
    streamWindowed(id,out.dataCount(),[&](size_t i){return out.data(i).size;},[&](size_t i,uint64_t offset,size_t count){return out.read(i,offset,count);},[&](size_t i){return out.digest(i);},L);
    current();emit(fields("clipboard.commit",id)+'}');wait("clipboard.complete",id);noteEnd();
  }
  void read(const Json& request){
    const auto id=request.get("requestId").string();if(!beginId(id,true))return;const auto L=limits();diagExpectedEntries_=L.formats?uint32_t(L.maxEntries):0;current();require(textToLocal_||filesToLocal_);
    const auto* requestFields=std::get_if<Json::Object>(&request.value);
    const bool boundRead=requestFields&&(requestFields->count("expectedSequence")||requestFields->count("textImageOnly"));
    const bool textImageOnly=request.get("textImageOnly").boolean();
    std::optional<DWORD> expected;
    if(boundRead){
      require(settled_&&L.onPaste&&std::holds_alternative<bool>(request.get("textImageOnly").value)&&(textImageOnly||intent_)&&requestFields->count("expectedSequence")&&
        requestFields->size()==size_t((intent_?5:4)+requestFields->count("_channelGeneration")));
      if(intent_)require(watchWanted_&&request.get("watchEpoch").number(-1)==double(watchEpoch_)&&watchEpoch_>0);
      expected=DWORD(wholeNumber(request.get("expectedSequence"),0xffffffff));
      if(textImageOnly)clip::validateSettledSelection(*expected,access_.watch.sequence(),access_.formats());
      else if(access_.watch.sequence()!=*expected)throw std::runtime_error("clipboard_changed");
    }
    // Virtual files (zip contents, mail attachments) are staged under the private transfer parent, for pairs with the formats profile.
    clip::ReadOptions readOptions{textToLocal_.load(),filesToLocal_.load(),textToLocal_.load(),filesToLocal_.load(),true};readOptions.richBytes=L.maxRichBytes;
    if(expected){readOptions.expectedSequence=expected;readOptions.textImageOnly=textImageOnly;}
    if(L.formats&&!textImageOnly){clip::VirtualStageOptions stage;stage.parent=transferParent_.empty()?clipboardTransferParent():transferParent_;stage.bounds=L.bounds();readOptions.virtualFiles=std::move(stage);}
    // Automatic sharing (a settled, sequence-bound read) skips a password manager's copy in the VM.
    if(boundRead&&!clip::ownDelayedContentIsCurrent()&&clip::sensitiveContentIsCurrent())throw std::runtime_error("clipboard_policy_denied");
    const auto snapshot=L.formats?access_.readFormats(owner_,readOptions):access_.read(owner_,textToLocal_,filesToLocal_);
    if(expected){if(textImageOnly)clip::validateSettledSelection(*expected,access_.watch.sequence(),access_.formats());if(snapshot.sequence!=*expected||access_.watch.sequence()!=*expected)throw std::runtime_error("clipboard_changed");if(textImageOnly&&(!snapshot.files.empty()||snapshot.staged))throw std::runtime_error("unsupported_clipboard_format");}
    // The paste-pull profile: a large copy of files and folders is offered by its manifest only; the peer pulls the bytes when pasted.
    // Deferred manifest: a copy of files that is not small is announced at once without walking it (like RDP); the list is walked when
    // a paste asks for it. Staged virtual files (an archive's contents) are already local and keep their walked offer.
    if(L.pull&&L.onPaste&&deferred_&&!snapshot.files.empty()&&!snapshot.staged&&!clip::treeFitsWithin(snapshot.files,L.bounds(),clip::eagerManifestEntries,clip::eagerManifestBytes)){
      sendDeferred(id,snapshot.files,snapshot.sequence);lastSent_.reset();reset();return;}
    if(L.pull&&!snapshot.files.empty()){auto scan=clip::scanTree(snapshot.files,L.bounds());if(clip::shouldOffer(scan,L.onPaste)){auto offer=clip::SourceOffer::fromScan(id,std::move(scan));offer->keepAlive(snapshot.staged);sendOffer(id,std::move(offer),L,snapshot.sequence);lastSent_.reset();reset();return;}
      // (an on-paste pair never pushes files: what cannot be offered is refused, as too long a path)
      if(L.onPaste)throw clip::PathTooLongError(scan.longestPath,clip::maxPasteChars);}
    // The on-paste revision: text, rich text and images above the push sizes are offered too (rendered when something pastes); small ones are pushed below, silently.
    if(L.onPaste)if(auto content=clip::SourceOffer::fromContent(id,snapshot,L.bounds(),true)){sendOffer(id,std::move(content),L,snapshot.sequence);lastSent_.reset();reset();return;}
    // Rich text, an image or a folder tree travel by the formats profile; plain text and flat file lists keep their existing path.
    if(L.formats)if(auto outgoing=clip::Outgoing::capture(snapshot,L.bounds())){sendOutgoing(id,*outgoing,L);lastSent_.reset();reset();return;}
    Sent sent;sent.sequence=snapshot.sequence;
    if(!snapshot.files.empty()){
      textFormat_=false;permitted(true,false);
      std::vector<clip::FileSource> sources;std::set<std::wstring> names;uint64_t total=0;std::string manifest="[";
      for(const auto& path:snapshot.files){
        // A folder is not a transferable file; say so instead of a generic read failure.
        const DWORD attributes=GetFileAttributesW(clip::longPath(path).c_str());if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("unsupported_clipboard_format");
        if(snapshot.files.size()>L.maxFiles)throw std::runtime_error("clipboard_file_count_limit");sources.emplace_back(path);const auto& desc=sources.back().description();if(desc.size>L.maxFileBytes)throw std::runtime_error("clipboard_file_name_or_size_limit");if(desc.size>L.maxTotalBytes-total)throw std::runtime_error("clipboard_total_size_limit");total+=desc.size;auto folded=desc.name;CharUpperBuffW(folded.data(),DWORD(folded.size()));require(names.insert(folded).second);if(manifest.size()>1)manifest+=',';manifest+="{\"name\":"+jsonQuote(utf8(desc.name))+",\"size\":"+std::to_string(desc.size)+'}';sent.names.push_back(desc.name);sent.sizes.push_back(desc.size);}manifest+=']';require(manifest.size()<48000);
      noteBegin(true,2,sources.size(),total);
      emit(fields("clipboard.begin",id)+",\"format\":\"files\",\"files\":"+manifest+'}');
      if(L.formats)streamWindowed(id,sources.size(),[&](size_t i){return sources[i].description().size;},[&](size_t i,uint64_t,size_t n){return sources[i].next(n);},[&](size_t i){return sources[i].digest();},L,&sent.digests);
      else for(size_t index=0;index<sources.size();++index){auto& source=sources[index];while(source.position()<source.description().size){current();const auto offset=source.position();auto chunk=source.next(32768);emit(chunkMessage(id,index,offset,chunk));wait("clipboard.chunk.ack",id,index,source.position());}sent.digests.push_back(source.digest());emit(fields("clipboard.item.end",id)+",\"index\":"+std::to_string(index)+",\"sha256\":"+jsonQuote(sent.digests.back())+'}');wait("clipboard.item.ack",id,index);}
    }else if(snapshot.text){textFormat_=true;sent.text=true;permitted(true,true);const auto encoded=utf8(*snapshot.text);std::vector<uint8_t> bytes(encoded.begin(),encoded.end());if(bytes.size()>L.maxTextBytes||snapshot.text->size()>L.maxTextUnits)throw std::runtime_error("clipboard_text_too_large");sent.sizes.push_back(bytes.size());noteBegin(true,1,1,bytes.size());emit(fields("clipboard.begin",id)+",\"format\":\"text\",\"size\":"+std::to_string(bytes.size())+'}');if(L.formats)streamWindowed(id,1,[&](size_t){return uint64_t(bytes.size());},[&](size_t,uint64_t o,size_t n){return std::vector<uint8_t>(bytes.begin()+ptrdiff_t(o),bytes.begin()+ptrdiff_t(o+n));},[&](size_t){return clipboardDigest(bytes);},L,&sent.digests);else sent.digests.push_back(sendItem(id,0,bytes));}
    else{note(ClipboardDiagnostic::readEmpty,0,access_.formats());error(id,"clipboard_empty");reset();return;}
    current();emit(fields("clipboard.commit",id)+'}');wait("clipboard.complete",id);noteEnd();lastSent_=std::move(sent);reset();
  }
  // A deferred offer: only that files were copied. The copied paths and the clipboard sequence are kept for the paste's list request.
  void sendDeferred(const std::string& id,const std::vector<std::filesystem::path>& files,DWORD sequence){
    textFormat_=false;permitted(true,false);noteBegin(true,6,0,0);
    offers_.addDeferred(id,files,sequence);  // kept before the peer can ask
    emit(fields("clipboard.offer",id)+",\"format\":\"tree\",\"deferred\":true}");
    current();wait("clipboard.complete",id);noteEnd();
  }
  // The reasons a list request may end with: the expired copy, the limit a walk met (an on-paste pair is a scale pair), and the usual
  // file and clipboard reasons.
  static const char* manifestReason(const std::string& what){
    if(what=="clipboard_offer_expired"||what=="clipboard_changed")return "clipboard_offer_expired";
    if(what=="clipboard_path_too_long"||what=="clipboard_entry_count_limit"||what=="clipboard_depth_limit")return what=="clipboard_path_too_long"?"clipboard_path_too_long":what=="clipboard_entry_count_limit"?"clipboard_entry_count_limit":"clipboard_depth_limit";
    if(what=="clipboard_file_count_limit"||what=="clipboard_file_name_or_size_limit"||what=="clipboard_total_size_limit")return "clipboard_limits_exceeded";
    if(what=="clipboard_file_locked")return "clipboard_file_locked";
    if(what=="clipboard_busy")return "clipboard_busy";
    if(what=="clipboard_desktop_unavailable")return "no_interactive_session";
    if(what=="clipboard_policy_denied")return "clipboard_policy_denied";
    if(what=="clipboard_selected_file_unavailable"||what=="clipboard_selected_file_changed"||what=="unsupported_clipboard_format"||what=="clipboard_empty")return "clipboard_file_unavailable";
    return "clipboard_transfer_failed";
  }
  // A paste on the PC asks for the list of a deferred copy (clipboard.manifest from the PC): the copy must still be the one selected
  // (the same clipboard sequence, or the clipboard naming the same paths), then it is walked here, with progress and the PC's cancel
  // honoured, and the walked offer takes the copy's place, already pasting. The answer is clipboard.manifest.begin and the entries.
  void serveManifest(const Json& request){
    const auto id=request.get("requestId").string();require(clip::pullRequestId(id)&&id[4]=='c'&&limits().pull&&deferred_);
    const auto* fieldsOf=std::get_if<Json::Object>(&request.value);require(fieldsOf&&fieldsOf->size()==3+fieldsOf->count("_channelGeneration"));
    bool busy=false,skipped=false;
    {std::lock_guard lock(mutex_);
      if(const auto found=std::find(skippedPulls_.begin(),skippedPulls_.end(),id);found!=skippedPulls_.end()){skippedPulls_.erase(found);skipped=true;}
      else if(activeId_.empty()){activeId_=id;sending_=true;acks_.clear();cancelled_=false;}else busy=true;}
    if(skipped)return;
    if(busy){diagnostic(ClipboardDiagnostic::pullFailed,false,clipboardReasonCode("clipboard_busy"),0,true);error(id,"clipboard_busy");return;}
    current();const auto L=limits();textFormat_=false;
    if(!filesToLocal_.load())throw std::runtime_error("clipboard_policy_denied");
    const auto offerId=request.get("offerId").string();require(clipboardRequestId(offerId));
    const auto stub=offers_.deferred(offerId);
    if(!stub)throw std::runtime_error("clipboard_offer_expired");
    // Asked again for a copy already walked (the first answer was cut off or refused): the same list, without walking again. A list
    // being pasted stays served although the clipboard changed meanwhile (pasteHold), as its files are.
    if(stub->listed&&(offers_.pasting(offerId)||access_.watch.sequence()==stub->sequence||clip::sameSelection(currentFiles(),stub->selection))){
      offers_.listing(offerId);serveListed(id,offerId,*stub->listed);return;}
    if(access_.watch.sequence()!=stub->sequence&&!clip::sameSelection(currentFiles(),stub->selection))throw std::runtime_error("clipboard_offer_expired");
    // The request is a paste: a copy made in the VM while this walk runs does not drop it.
    if(!offers_.listing(offerId))throw std::runtime_error("clipboard_offer_expired");
    auto last=Clock::now();
    const auto alive=[&](size_t entries){
      // Every step of the walk (256 entries, a folder's end) lets Explorer's calls into this STA and the PC's cancel through; the
      // progress message (and the desktop check) once a second.
      pump(0);require(!stopped_&&!cancelled_);
      const auto now=Clock::now();if(now-last<clip::manifestProgressEvery)return;last=now;
      current();if(!filesToLocal_.load())throw std::runtime_error("clipboard_policy_denied");
      emit(fields("clipboard.manifest.progress",id)+",\"entries\":"+std::to_string(entries)+'}');
    };
    clip::TreeScanControl control;control.progress=alive;
    auto scan=clip::scanTree(stub->selection,L.bounds(),control);
    if(!clip::shouldOffer(scan,true))throw clip::PathTooLongError(scan.longestPath,clip::maxPasteChars);
    // Recording every entry's identity follows the walk: the count shown stays the number walked while it runs.
    const size_t walked=scan.entries.size();
    std::shared_ptr<clip::SourceOffer> offer(clip::SourceOffer::fromScan(offerId,std::move(scan),[&](size_t){alive(walked);}));
    current();
    if(!filesToLocal_.load())throw std::runtime_error("clipboard_policy_denied");
    if(!offers_.materialize(offerId,offer))throw std::runtime_error("clipboard_offer_expired");
    const auto kept=offers_.deferred(offerId);
    serveListed(id,offerId,kept&&kept->listed?*kept->listed:*offer);
  }
  // The answer to a list request from the walked offer: the counts, then the entry pages.
  void serveListed(const std::string& id,const std::string& offerId,const clip::SourceOffer& offer){
    noteBegin(true,6,offer.fileCount(),offer.totalBytes());
    emit(fields("clipboard.manifest.begin",id)+",\"offerId\":"+jsonQuote(offerId)+",\"entries\":"+std::to_string(offer.items().size())+",\"files\":"+std::to_string(offer.fileCount())+",\"bytes\":"+std::to_string(offer.totalBytes())+",\"tops\":"+std::to_string(offer.tops())+",\"skipped\":"+std::to_string(offer.skipped())+'}');
    sendEntryPages(id,offer);
    noteEnd();reset();
  }
  // The entries of an offer in clipboard.entries pages (at most 200 entries or 40,000 characters each), with write times.
  void sendEntryPages(const std::string& id,const clip::SourceOffer& offer){
    size_t start=0;std::string page;size_t inPage=0;
    const auto flush=[&]{if(!inPage)return;current();waitRoom();emit(fields("clipboard.entries",id)+",\"start\":"+std::to_string(start)+",\"entries\":["+page+"]}");start+=inPage;page.clear();inPage=0;};
    for(size_t at=0;at<offer.items().size();++at){
      const auto& item=offer.items()[at];std::wstring wire=item.name;std::replace(wire.begin(),wire.end(),L'\\',L'/');
      if(inPage)page+=',';
      page+="{\"path\":"+jsonQuote(utf8(wire))+",\"kind\":"+jsonQuote(item.kind)+(item.kind=="file"?",\"size\":"+std::to_string(item.size):std::string())+",\"attrs\":"+std::to_string(item.attributes)+",\"mtime\":"+std::to_string(offer.mtimeMs(at))+'}';
      if(++inPage>=200||page.size()>40000)flush();
    }
    flush();
  }
  uint64_t wholeNumber(const Json& value,uint64_t maximum){const double number=value.number(-1);require(number>=0&&std::floor(number)==number&&number<=double(maximum));return uint64_t(number);}
  void startIncoming(const std::string& format,std::vector<clip::Item> items,const Limits& L){
    if(!storage_)storage_=std::make_shared<clip::Storage>(transferParent_.empty()?clipboardTransferParent():transferParent_);
    sizes_.clear();for(const auto& item:items)if(item.kind!="dir")sizes_.push_back(item.size);
    incoming_=std::make_unique<clip::Incoming>(format,std::move(items),storage_->parent(),storage_.get(),currentFiles(),L.bounds());incomingFormat_=format;
  }
  void beginFormats(const Json& value,const std::string& format,const Limits& L){
    if(format=="tree"){
      tree_=TreeStage{true,size_t(wholeNumber(value.get("entries"),L.maxEntries)),size_t(wholeNumber(value.get("files"),L.maxEntries)),size_t(wholeNumber(value.get("tops"),L.maxFiles)),wholeNumber(value.get("bytes"),L.maxTotalBytes)};
      wholeNumber(value.get("skipped"),L.maxEntries);
      require(tree_.entries>=1&&tree_.tops>=1&&tree_.files<=tree_.entries);pendingItems_.clear();noteBegin(false,offerMode_?6:5,tree_.files,tree_.bytes);return;
    }
    const auto* values=std::get_if<Json::Array>(&value.get("items").value);require(values&&!values->empty()&&values->size()<=3);
    std::vector<clip::Item> items;
    for(const auto& entry:*values){clip::Item item;item.kind=entry.get("kind").string();item.size=wholeNumber(entry.get("size"),format=="image"?clip::maxImageBytes:std::max<uint64_t>(L.maxRichBytes,L.maxTextBytes));
      if(format=="image"){item.width=uint32_t(wholeNumber(entry.get("width"),65535));item.height=uint32_t(wholeNumber(entry.get("height"),65535));}
      items.push_back(std::move(item));}
    uint64_t bytes=0;for(const auto& item:items)bytes+=item.size;noteBegin(false,clipboardFormatCode(format),items.size(),bytes);
    startIncoming(format,std::move(items),L);
  }
  void receiveEntries(const Json& value){
    const auto L=limits();require(tree_.waiting&&!incoming_&&L.formats);
    require(wholeNumber(value.get("start"),L.maxEntries)==pendingItems_.size());
    const auto* values=std::get_if<Json::Array>(&value.get("entries").value);require(values&&!values->empty()&&values->size()<=256&&pendingItems_.size()+values->size()<=tree_.entries);
    for(const auto& entry:*values){
      clip::Item item;item.kind=entry.get("kind").string();require(item.kind=="file"||item.kind=="dir");
      item.name=clip::treePathFromWire(wide(entry.get("path").string()),L.maxPathChars,L.maxDepth);
      // Explorer cannot paste a relative path of 260 or more characters into any folder: refuse before anything is received.
      if(item.name.size()>clip::maxPasteChars)throw clip::PathTooLongError(item.name.size(),clip::maxPasteChars);
      item.size=item.kind=="file"?wholeNumber(entry.get("size"),L.maxFileBytes):0;
      item.attributes=DWORD(wholeNumber(entry.get("attrs"),0x27))&0x27;
      if(offerMode_)pendingMtimes_.push_back(int64_t(wholeNumber(entry.get("mtime"),253402300799999ull)));
      pendingItems_.push_back(std::move(item));
    }
    if(pendingItems_.size()<tree_.entries)return;
    size_t files=0,tops=0;uint64_t bytes=0;for(const auto& item:pendingItems_){if(item.kind=="file"){++files;bytes+=item.size;}if(item.name.find(L'\\')==std::wstring::npos)++tops;}
    require(files==tree_.files&&bytes==tree_.bytes&&tops==tree_.tops);
    tree_.waiting=false;if(offerMode_){publishOffer(L);return;}startIncoming("tree",std::move(pendingItems_),L);pendingItems_.clear();
  }
  // ---- Paste-pull -------------------------------------------------------------------------------
  // Offering: the manifest crosses (clipboard.offer + clipboard.entries pages with write times) and the answer
  // is the peer's clipboard.complete once it has published the virtual files. The bytes follow only as pulls.
  void sendOffer(const std::string& id,std::unique_ptr<clip::SourceOffer> offered,const Limits& L,DWORD sequence){
    std::shared_ptr<clip::SourceOffer> offer(std::move(offered));const bool tree=offer->format()=="tree";
    textFormat_=offer->format()=="text"||offer->format()=="richtext";permitted(true,textFormat_);
    noteBegin(true,6,offer->fileCount(),offer->totalBytes());
    offers_.add(offer,L.onPaste?std::optional<DWORD>(sequence):std::nullopt);  // kept before the peer can ask
    if(!tree){
      // Text, rich text or an image (on-paste revision): the manifest is the whole offer, items are pulled by index.
      std::string manifest=fields("clipboard.offer",id)+",\"format\":"+jsonQuote(offer->format());
      if(offer->format()=="text")manifest+=",\"size\":"+std::to_string(offer->items()[0].size)+'}';
      else{manifest+=",\"items\":[";bool first=true;for(const auto& item:offer->items()){if(!first)manifest+=',';first=false;manifest+="{\"kind\":"+jsonQuote(item.kind)+",\"size\":"+std::to_string(item.size)+(item.kind=="png"?",\"width\":"+std::to_string(item.width)+",\"height\":"+std::to_string(item.height):std::string())+'}';}manifest+="]}";}
      emit(manifest);current();wait("clipboard.complete",id);noteEnd();return;
    }
    emit(fields("clipboard.offer",id)+",\"format\":\"tree\",\"entries\":"+std::to_string(offer->items().size())+",\"files\":"+std::to_string(offer->fileCount())+",\"bytes\":"+std::to_string(offer->totalBytes())+",\"tops\":"+std::to_string(offer->tops())+",\"skipped\":"+std::to_string(offer->skipped())+'}');
    size_t start=0;std::string page;size_t inPage=0;
    const auto flush=[&]{if(!inPage)return;emit(fields("clipboard.entries",id)+",\"start\":"+std::to_string(start)+",\"entries\":["+page+"]}");start+=inPage;page.clear();inPage=0;};
    for(size_t at=0;at<offer->items().size();++at){
      const auto& item=offer->items()[at];std::wstring wire=item.name;std::replace(wire.begin(),wire.end(),L'\\',L'/');
      if(inPage)page+=',';
      page+="{\"path\":"+jsonQuote(utf8(wire))+",\"kind\":"+jsonQuote(item.kind)+(item.kind=="file"?",\"size\":"+std::to_string(item.size):std::string())+",\"attrs\":"+std::to_string(item.attributes)+",\"mtime\":"+std::to_string(offer->mtimeMs(at))+'}';
      if(++inPage>=200||page.size()>40000)flush();
    }
    flush();current();wait("clipboard.complete",id);noteEnd();
  }  // Serving a pull (clipboard.pull from the peer's paste): one file from an offset, windowed, hashed as it is read. The
  // pull ends at the item end; acknowledgements that arrive later are dropped. A file that changed since the copy
  // fails this pull with clipboard_selected_file_changed. A paused paste may leave the window unacknowledged for a long
  // time, so the idle wait is the relay's own (15 minutes).
  void servePull(const Json& request){
    const auto id=request.get("requestId").string();require(clip::pullRequestId(id)&&id[4]=='c'&&limits().pull);
    // Another transfer is active (a receive or a send of this Host): this pull alone is refused. Failing through the
    // catch in run() would cancel and reset that other transfer, which has nothing to do with this request.
    bool busy=false,skipped=false;
    {std::lock_guard lock(mutex_);
      // A pull queue: the PC cancelled this pull while it waited (the relay has retired it; nothing is sent for it).
      if(const auto found=std::find(skippedPulls_.begin(),skippedPulls_.end(),id);found!=skippedPulls_.end()){skippedPulls_.erase(found);skipped=true;}
      else if(activeId_.empty()){activeId_=id;sending_=true;acks_.clear();cancelled_=false;}else busy=true;}
    if(skipped)return;
    if(busy){diagnostic(ClipboardDiagnostic::pullFailed,false,clipboardReasonCode("clipboard_busy"),0,true);error(id,"clipboard_busy");return;}
    current();const auto L=limits();
    const auto index=size_t(wholeNumber(request.get("index"),clip::maxEntries)),offset=wholeNumber(request.get("offset"),clip::scaleBytes);
    const auto offer=offers_.find(request.get("offerId").string());
    if(!offer){diagnostic(ClipboardDiagnostic::pullFailed,false,clipboardReasonCode("clipboard_offer_expired"),0,true);error(id,"clipboard_offer_expired");reset();return;}
    textFormat_=offer->format()=="text"||offer->format()=="richtext";permitted(true,textFormat_);
    std::unique_ptr<clip::SourceOffer::Reader> reader;
    try{reader=offers_.openCurrent(offer->id(),index,offset,access_.watch.sequence,[this]{return currentFiles();});}catch(const std::exception& failure){diagnostic(ClipboardDiagnostic::pullFailed,false,clipboardReasonCode(pullReason(failure.what())),clipboardCauseCode(failure.what()),true);error(id,pullReason(failure.what()));reset();return;}
    // A pull queue's window is 4 MiB / 128 units (the relay enforces the same); a released pair keeps 1 MiB / 32.
    clip::SendWindow window=pullQueue_?clip::SendWindow(pullQueueWindowUnits,pullQueueWindowBytes):clip::SendWindow();
    const auto drain=[&]{for(;;){Json ack;{std::lock_guard lock(mutex_);if(acks_.empty())return;ack=std::move(acks_.front());acks_.pop_front();}applyAck(window,id,ack);}};
    const auto idle=std::chrono::seconds(15*60);
    while(reader->position()<reader->size()){
      // The window can remain writable for a long file, so ACK waits alone do
      // not pump clipboard changes. A new copy must retire this offer now.
      pump(0);
      if(L.onPaste&&!offers_.find(offer->id())){error(id,"clipboard_offer_expired");reset();return;}
      offers_.served(offer->id());  // a long file keeps its paste going (OfferRegistry::pasteHold)
      current();permitted(true,textFormat_);drain();
      const size_t count=size_t(std::min<uint64_t>(L.chunkBytes(),reader->size()-reader->position()));
      while(!window.canSend(count))applyAck(window,id,nextAck(idle));
      const uint64_t at=reader->position();const auto chunk=reader->next(count);
      emit(chunkMessage(id,index,at,chunk));
      window.sentChunk(index,reader->position(),chunk.size());
    }
    const auto digest=reader->digest();current();permitted(true,textFormat_);
    // The item end needs a free unit of this pull's own window (128 in a pull queue), not of the released 32-unit window.
    while(!window.canSendItemEnd())applyAck(window,id,nextAck(idle));
    emit(fields("clipboard.item.end",id)+",\"index\":"+std::to_string(index)+",\"sha256\":"+jsonQuote(digest)+'}');
    offers_.served(offer->id());  // the hold counts from the end of the last file
    reset();
  }
  // Receiving: the peer's offer. Entries (with write times) are collected like a folder tree, then the virtual files are
  // published; nothing is read from the peer until something pastes. An offer of text, rich text or an image (on-paste revision)
  // is complete with its manifest and published at once as a data object that renders the formats when asked.
  void receiveOffer(const Json& value){
    const auto id=value.get("requestId").string();if(!beginId(id,false))return;const auto L=limits();current();
    const auto format=value.get("format").string();
    require(L.pull&&L.formats&&(format=="tree"||(L.onPaste&&(format=="text"||format=="richtext"||format=="image"))));receiveSequence_=access_.watch.sequence();
    textFormat_=format=="text"||format=="richtext";permitted(false,textFormat_);echo_=false;offerMode_=true;pendingMtimes_.clear();
    // A deferred copy (manifest-on-paste-v1): published at once without a list; the first paste asks the PC for it.
    if(value.get("deferred").boolean()){
      const auto* fieldsOf=std::get_if<Json::Object>(&value.value);
      require(L.onPaste&&deferred_&&format=="tree"&&fieldsOf&&fieldsOf->size()==4+fieldsOf->count("_channelGeneration"));
      noteBegin(false,6,0,0);publishOffer(L,"deferred");return;
    }
    if(format=="tree"){beginFormats(value,"tree",L);return;}
    std::vector<clip::Item> items;
    if(format=="text"){clip::Item item;item.kind="text";item.size=wholeNumber(value.get("size"),L.maxTextBytes);items.push_back(std::move(item));}
    else{
      const auto* values=std::get_if<Json::Array>(&value.get("items").value);require(values&&!values->empty()&&values->size()<=2);
      for(const auto& entry:*values){clip::Item item;item.kind=entry.get("kind").string();item.size=wholeNumber(entry.get("size"),format=="image"?clip::maxImageBytes:std::max<uint64_t>(L.maxRichBytes,L.maxTextBytes));
        if(format=="image"){item.width=uint32_t(wholeNumber(entry.get("width"),65535));item.height=uint32_t(wholeNumber(entry.get("height"),65535));}
        items.push_back(std::move(item));}
    }
    uint64_t bytes=0;for(const auto& item:items)bytes+=item.size;noteBegin(false,6,items.size(),bytes);
    publishOffer(L,format,std::move(items));
  }
  // The hooks of a published offer: what a reader needs from the PC becomes a clipboard.pull, the platform's cancel and consumption notices
  // become clipboard.cancel / clipboard.chunk.ack, and (on-paste revision) a reader that waits is reported as a waiting paste.
  clip::PullHooks offerHooks(const std::shared_ptr<std::weak_ptr<clip::PullOffer>>& holder,const Limits& L){
    clip::PullHooks hooks;const auto generation=pairGeneration_.load();
    hooks.request=[this,holder,generation](uint64_t range,const std::string& offerId,size_t index,uint64_t offset){
      std::string pull;bool refused=false;
      {std::lock_guard lock(pullMutex_);if(!ready_||generation!=pairGeneration_)refused=true;else{pasteOperations_.request(offerId,generation);syncPasteWaiting(generation);pull="pullh-"+std::to_string(++pullCounter_);pullIdOfRange_[range]=pull;rangeOfPullId_[pull]=range;rangeOffers_[range]=*holder;}}
      if(refused){if(auto offer=holder->lock())offer->fail(range,"clipboard_transport_closed");return;}
      emit(fields("clipboard.pull",pull)+",\"offerId\":"+jsonQuote(offerId)+",\"index\":"+std::to_string(index)+",\"offset\":"+std::to_string(offset)+'}',generation);
    };
    hooks.cancel=[this,generation](uint64_t range){
      std::string pull;{std::lock_guard lock(pullMutex_);if(!ready_||generation!=pairGeneration_)return;const auto found=pullIdOfRange_.find(range);if(found==pullIdOfRange_.end())return;pull=found->second;pullIdOfRange_.erase(found);rangeOfPullId_.erase(pull);rangeOffers_.erase(range);}
      emit(fields("clipboard.cancel",pull)+'}',generation);
    };
    hooks.ackChunk=[this,generation](uint64_t range,size_t index,uint64_t end){
      std::string pull;{std::lock_guard lock(pullMutex_);if(!ready_||generation!=pairGeneration_)return;const auto found=pullIdOfRange_.find(range);if(found==pullIdOfRange_.end())return;pull=found->second;}
      emit(fields("clipboard.chunk.ack",pull)+",\"index\":"+std::to_string(index)+",\"offset\":"+std::to_string(end)+'}',generation);
    };
    // A deferred offer's list: a clipboard.manifest of its own pull id; the answer is routed back by that id (pullIncoming).
    hooks.manifest=[this,holder,generation](const std::string& offerId){
      std::string pull;bool refused=false;
      {std::lock_guard lock(pullMutex_);if(!ready_||generation!=pairGeneration_)refused=true;else{pasteOperations_.request(offerId,generation);syncPasteWaiting(generation);pull="pullh-"+std::to_string(++pullCounter_);
        for(auto at=manifestIn_.begin();at!=manifestIn_.end();)at=at->second.offerId==offerId?manifestIn_.erase(at):std::next(at);
        manifestIn_[pull].offer=*holder;manifestIn_[pull].offerId=offerId;}}
      if(refused){if(auto offer=holder->lock())offer->failManifest("clipboard_transport_closed");return;}
      emit(fields("clipboard.manifest",pull)+",\"offerId\":"+jsonQuote(offerId)+'}',generation);
    };
    hooks.manifestCancel=[this,generation](const std::string& offerId,const std::string&){
      std::string pull;{std::lock_guard lock(pullMutex_);if(!ready_||generation!=pairGeneration_)return;
        for(auto at=manifestIn_.begin();at!=manifestIn_.end();++at)if(at->second.offerId==offerId){pull=at->first;manifestIn_.erase(at);break;}}
      if(!pull.empty())emit(fields("clipboard.cancel",pull)+'}',generation);
    };
    if(L.onPaste){
      hooks.waiting=[this,holder,generation](bool on){if(auto offer=holder->lock())pasteWaiting(offer->id(),on,generation);};
      hooks.operationState=[this,holder,generation](bool on,HRESULT result){if(auto offer=holder->lock())pasteOperation(offer->id(),on,result,generation);};
    }
    return hooks;
  }
  // Our client card is the only progress (no Explorer dialog) and a small set is also CF_HDROP, in an on-paste pair.
  clip::PullOptions offerOptions(const Limits& L){
    clip::PullOptions options;options.explorerProgress=!L.onPaste;options.prefetchDepth=pullQueue_?clip::maxPrefetchDepth:1;
    if(L.onPaste){if(!storage_)storage_=std::make_shared<clip::Storage>(transferParent_.empty()?clipboardTransferParent():transferParent_);options.stagingParent=storage_->parent();options.storage=storage_;}
    return options;
  }
  void publishOffer(const Limits& L,const std::string& format="tree",std::vector<clip::Item> items={}){
    const auto id=[&]{std::lock_guard lock(mutex_);return activeId_;}();
    const auto generation=pairGeneration_.load();
    auto holder=std::make_shared<std::weak_ptr<clip::PullOffer>>();auto hooks=offerHooks(holder,L);
    auto offer=format=="tree"?clip::PullOffer::create(id,std::move(pendingItems_),std::move(pendingMtimes_),std::move(hooks),{},offerOptions(L)):
               format=="deferred"?clip::PullOffer::createDeferred(id,std::move(hooks),{},offerOptions(L)):clip::PullOffer::createContent(id,format,std::move(items),std::move(hooks));
    *holder=offer;pendingItems_.clear();pendingMtimes_.clear();
    {std::lock_guard lock(pullMutex_);require(ready_&&generation==pairGeneration_);pullOffers_.push_back(offer);pasteOperations_.publish(id,generation);}
    DWORD sequence=0;
    try{sequence=access_.publishOffer(offer,receiveSequence_);}catch(...){
      offer->shutdown(ready_&&generation==pairGeneration_?"clipboard_transfer_failed":"clipboard_transport_closed");
      std::lock_guard lock(pullMutex_);pullOffers_.erase(std::remove(pullOffers_.begin(),pullOffers_.end(),offer),pullOffers_.end());pasteOperations_.retire(id,generation);syncPasteWaiting(generation);throw;}
    {std::lock_guard lock(pullMutex_);currentOffer_=offer;
      // Only the latest two stay answerable; a paste still reading an older one keeps it until then.
      if(pullOffers_.size()>clip::maxLiveOffers){pasteOperations_.retire(pullOffers_.front()->id(),pairGeneration_);pullOffers_.erase(pullOffers_.begin());syncPasteWaiting(pairGeneration_);}}
    ownSequence_=sequence;watch_.applied(sequence);
    emit(fields("clipboard.applied",id)+'}');noteEnd();
    // The PC withdrew this offer before it was published: it goes again at once.
    bool gone=false;{std::lock_guard lock(pullMutex_);gone=withdrawn_.erase(id)>0;}
    if(gone){offer->abortRenders("clipboard_offer_expired",true);offer->shutdown("clipboard_offer_expired");withdrawReceived(id);}
    // The relay answers with clipboard.complete, which resets this transfer; the offer outlives it.
  }  // A clipboard.chunk / item.end / error / cancel of a pull this Host requested (message thread).
  // The answer to a list request (message thread): progress, the counts, the entry pages, or the PC's error. The list goes to the
  // waiting data object once every page is there; anything malformed fails the request and cancels it at the relay.
  bool manifestIncoming(const Json& value){
    const auto type=value.get("type").string(),id=value.get("requestId").string();std::shared_ptr<clip::PullOffer> offer;
    std::vector<clip::Item> items;std::vector<int64_t> mtimes;bool complete=false,failed=false;std::string reason;
    const auto L=limits();  // mutex_ is never taken while pullMutex_ is held
    {
      std::lock_guard lock(pullMutex_);const auto found=manifestIn_.find(id);if(found==manifestIn_.end())return false;
      auto& in=found->second;offer=in.offer.lock();
      try{
        if(!offer)throw std::runtime_error("clipboard_offer_expired");
        if(type=="clipboard.manifest.progress"){offer->manifestProgress();return true;}
        if(type=="clipboard.error"||type=="clipboard.cancel"){reason=type=="clipboard.error"?value.get("reason").string():"clipboard_cancelled";manifestIn_.erase(found);}
        else if(type=="clipboard.manifest.begin"){
          require(!in.begun&&value.get("offerId").string()==in.offerId);
          in.entries=size_t(wholeNumber(value.get("entries"),L.maxEntries));in.files=size_t(wholeNumber(value.get("files"),L.maxEntries));in.tops=size_t(wholeNumber(value.get("tops"),L.maxEntries));
          in.bytes=wholeNumber(value.get("bytes"),L.maxTotalBytes);wholeNumber(value.get("skipped"),L.maxEntries);
          require(in.entries>=1&&in.tops>=1&&in.files<=in.entries);in.begun=true;offer->manifestProgress();return true;
        }else if(type=="clipboard.entries"){
          require(in.begun&&wholeNumber(value.get("start"),L.maxEntries)==in.items.size());
          const auto* values=std::get_if<Json::Array>(&value.get("entries").value);require(values&&!values->empty()&&values->size()<=256&&in.items.size()+values->size()<=in.entries);
          for(const auto& entry:*values){
            clip::Item item;item.kind=entry.get("kind").string();require(item.kind=="file"||item.kind=="dir");
            item.name=clip::treePathFromWire(wide(entry.get("path").string()),L.maxPathChars,L.maxDepth);
            if(item.name.size()>clip::maxPasteChars)throw clip::PathTooLongError(item.name.size(),clip::maxPasteChars);
            item.size=item.kind=="file"?wholeNumber(entry.get("size"),L.maxFileBytes):0;item.attributes=DWORD(wholeNumber(entry.get("attrs"),0x27))&0x27;
            in.mtimes.push_back(int64_t(wholeNumber(entry.get("mtime"),253402300799999ull)));in.items.push_back(std::move(item));
          }
          offer->manifestProgress();
          if(in.items.size()<in.entries)return true;
          size_t files=0,tops=0;uint64_t bytes=0;for(const auto& item:in.items){if(item.kind=="file"){++files;bytes+=item.size;}if(item.name.find(L'\\')==std::wstring::npos)++tops;}
          require(files==in.files&&bytes==in.bytes&&tops==in.tops);
          items=std::move(in.items);mtimes=std::move(in.mtimes);complete=true;manifestIn_.erase(found);
        }else throw std::runtime_error("Unsupported clipboard packet");
      }catch(const clip::PathTooLongError&){failed=true;reason="clipboard_path_too_long";manifestIn_.erase(id);}
      catch(const std::exception&){failed=true;reason="clipboard_transfer_failed";manifestIn_.erase(id);}
    }
    if(failed)try{emit(fields("clipboard.cancel",id)+'}');}catch(...){}
    if(!offer)return true;
    if(complete)offer->deliverManifest(std::move(items),std::move(mtimes));else offer->failManifest(reason.empty()?std::string("clipboard_transfer_failed"):reason);
    return true;
  }
  void pullIncoming(const Json& value){
    if(manifestIncoming(value))return;
    const auto type=value.get("type").string(),id=value.get("requestId").string();uint64_t range=0;std::shared_ptr<clip::PullOffer> offer;
    {std::lock_guard lock(pullMutex_);const auto found=rangeOfPullId_.find(id);if(found==rangeOfPullId_.end())return;range=found->second;const auto owner=rangeOffers_.find(range);if(owner==rangeOffers_.end())return;offer=owner->second.lock();}
    if(!offer)return;
    if(type=="clipboard.chunk"){
      const auto bytes=clipboardChunk(value.get("data").string(),clip::formatsChunkBytes);
      offer->deliver(range,wholeNumber(value.get("offset"),clip::scaleBytes),bytes);
    }else if(type=="clipboard.item.end"){
      offer->finish(range,value.get("sha256").string());
      std::lock_guard lock(pullMutex_);pullIdOfRange_.erase(range);rangeOfPullId_.erase(id);rangeOffers_.erase(range);
    }else if(type=="clipboard.error"||type=="clipboard.cancel"){
      // The user cancelled the paste on the PC (its card): the application waiting here must not carry on with the next file or the next format.
      const bool userCancelled=type=="clipboard.error"&&value.get("reason").string()=="clipboard_cancelled";
      offer->fail(range,type=="clipboard.cancel"?std::string("clipboard_cancelled"):value.get("reason").string());
      if(userCancelled){{std::lock_guard lock(pullMutex_);pasteOperations_.cancel(offer->id(),pairGeneration_);syncPasteWaiting(pairGeneration_);}offer->abortRenders("clipboard_cancelled",true);}
      std::lock_guard lock(pullMutex_);pullIdOfRange_.erase(range);rangeOfPullId_.erase(id);rangeOffers_.erase(range);
    }
  }
  // The session ended or the channel closed: pulls fail, the published virtual files leave the clipboard, kept offers go.
  void closeOffers(bool withdraw){
    std::vector<std::shared_ptr<clip::PullOffer>> old;std::shared_ptr<clip::PullOffer> current;offers_.clear();
    {std::lock_guard lock(pullMutex_);old.swap(pullOffers_);current=std::move(currentOffer_);rangeOffers_.clear();pullIdOfRange_.clear();rangeOfPullId_.clear();withdrawn_.clear();manifestIn_.clear();pasteOperations_.clear();waitingNow_=false;waitingSent_=false;}
    if(withdraw&&current)try{if(const DWORD sequence=access_.withdrawOffer(current)){ownSequence_=sequence;watch_.applied(sequence);}}catch(...) {}
    ownSequence_=0;for(auto& offer:old)offer->shutdown("clipboard_transport_closed");
  }
  // Renders (blocking GetData) of every offer this Host published end: a newer copy replaces what they would hand over.
  // (A paste that already asked for a deferred offer's list keeps waiting for it: it has begun, as a paste reading files has.)
  void supersedeRenders(){std::vector<std::shared_ptr<clip::PullOffer>> live;{std::lock_guard lock(pullMutex_);live=pullOffers_;}for(auto& offer:live)offer->supersedeRenders("clipboard_offer_expired");}
  // The PC withdrew an offer (its clipboard changed). Message thread: what waits for it ends at once.
  void retireOffer(const std::string& id){
    std::shared_ptr<clip::PullOffer> found;
    {std::lock_guard lock(pullMutex_);for(auto& offer:pullOffers_)if(offer->id()==id)found=offer;pasteOperations_.retire(id,pairGeneration_);syncPasteWaiting(pairGeneration_);withdrawn_.insert(id);while(withdrawn_.size()>16)withdrawn_.erase(withdrawn_.begin());}
    if(found){found->abortRenders("clipboard_offer_expired",true);found->shutdown("clipboard_offer_expired");}
  }
  // Worker thread: the withdrawn offer leaves the clipboard if (and only if) it is still what the clipboard holds.
  void withdrawReceived(const std::string& id){
    std::shared_ptr<clip::PullOffer> found;
    {std::lock_guard lock(pullMutex_);for(auto at=pullOffers_.begin();at!=pullOffers_.end();++at)if((*at)->id()==id){found=*at;pullOffers_.erase(at);break;}if(found&&currentOffer_==found)currentOffer_.reset();pasteOperations_.retire(id,pairGeneration_);syncPasteWaiting(pairGeneration_);}
    if(!found)return;
    try{if(const DWORD sequence=access_.withdrawOffer(found)){ownSequence_=sequence;watch_.applied(sequence);}}catch(...) {}
  }
  // This Host's own clipboard changed: the offers it kept are no longer served; the PC clears what it published for them.
  // One a paste on the PC is still reading stays until that paste has been quiet (OfferRegistry::pasteHold), as RDP keeps
  // locked clipboard data: copying something else in the VM does not break a paste already under way.
  void withdrawKeptOffers(){
    for(const auto& id:offers_.dropIdle())try{emit(fields("clipboard.offer.withdraw",id)+'}');}catch(...) {}
  }
  // A paste in the VM is waiting for the PC's data (any thread). The first wait is reported at once; the end only after 1.5 s without one,
  // so that a paste of many small files is one waiting episode.
  void syncPasteWaiting(uint64_t generation){  // caller holds pullMutex_
    if(stopped_||!ready_||generation!=pairGeneration_)return;const bool on=pasteOperations_.waiting();
    if(on){waitingNow_=true;if(!waitingSent_.exchange(true)){const auto number=++waitingCounter_;try{emit("{\"type\":\"clipboard.paste.waiting\",\"requestId\":\"wait-"+std::to_string(number)+"\",\"active\":true}",generation);}catch(...) {}}return;}
    waitingNow_=false;if(owner_)PostMessageW(owner_,WM_APP+41,0,0);
  }
  void pasteWaiting(const std::string& id,bool on,uint64_t generation){
    std::lock_guard lock(pullMutex_);if(stopped_||!ready_||generation!=pairGeneration_)return;
    pasteOperations_.reading(id,generation,on);syncPasteWaiting(generation);
  }
  void pasteOperation(const std::string& id,bool on,HRESULT result,uint64_t generation){
    bool ended=false;
    {std::lock_guard lock(pullMutex_);if(stopped_||!ready_||generation!=pairGeneration_)return;ended=pasteOperations_.operation(id,generation,on);syncPasteWaiting(generation);}
    if(ended&&!on)if(const auto reason=clip::pullOperationFailure(result))diagnostic(ClipboardDiagnostic::pullFailed,false,clipboardReasonCode(reason),0,true);
  }
  void endWaiting(){
    if(owner_)KillTimer(owner_,4);
    if(!waitingNow_&&waitingSent_.exchange(false)){try{emit("{\"type\":\"clipboard.paste.waiting\",\"requestId\":\"wait-"+std::to_string(waitingCounter_.load())+"\",\"active\":false}");}catch(...) {}}
  }
  void receiveBegin(const Json& value){
    const auto id=value.get("requestId").string();if(!beginId(id,false))return;const auto L=limits();current();receiveSequence_=access_.watch.sequence();const auto format=value.get("format").string();ackPolicy_=clip::AckPolicy(L.formats);
    if(format=="richtext"||format=="image"||format=="tree"){require(L.formats);textFormat_=format=="richtext";permitted(false,textFormat_);echo_=false;beginFormats(value,format,L);return;}
    textFormat_=format=="text";require(textFormat_||format=="files");permitted(false,textFormat_);
    std::vector<std::wstring> names;
    if(textFormat_){const double size=value.get("size").number(-1);require(size>=0&&std::floor(size)==size);if(size>double(L.maxTextBytes))throw std::runtime_error("clipboard_text_too_large");sizes_.push_back(uint64_t(size));text_.reserve(size_t(size));noteBegin(false,1,1,uint64_t(size));}
    else{const auto* values=std::get_if<Json::Array>(&value.get("files").value);require(values&&!values->empty());if(values->size()>L.maxFiles)throw std::runtime_error("clipboard_file_count_limit");std::vector<clip::FileDescription> descriptions;uint64_t total=0;for(const auto& item:*values){const double size=item.get("size").number(-1);require(size>=0&&std::floor(size)==size);if(size>double(L.maxFileBytes))throw std::runtime_error("clipboard_file_name_or_size_limit");if(uint64_t(size)>L.maxTotalBytes-total)throw std::runtime_error("clipboard_total_size_limit");total+=uint64_t(size);descriptions.push_back({wide(item.get("name").string()),uint64_t(size)});names.push_back(descriptions.back().name);sizes_.push_back(uint64_t(size));}noteBegin(false,2,descriptions.size(),total);if(!storage_)storage_=std::make_shared<clip::Storage>(transferParent_.empty()?clipboardTransferParent():transferParent_);storage_->prepare(total,currentFiles());receiver_=std::make_unique<clip::FileReceiver>(storage_->parent(),descriptions);receiveExpected_=descriptions;}
    // Candidate echo: same format, names and sizes as our last read; every
    // item digest must also match before the commit may keep the clipboard.
    echo_=lastSent_&&lastSent_->text==textFormat_&&lastSent_->sizes==sizes_&&(textFormat_||lastSent_->names==names);
  }
  void receive(const Json& value){
    const auto type=value.get("type").string(),id=value.get("requestId").string();
    const auto generation=clipboardGenerationNumber(value.get("_channelGeneration").number());
    if(generation&&(generation<pairGeneration_||(type!="clipboard.ready"&&type!="clipboard.channel.closed"&&generation!=pairGeneration_)))return;
    if(type=="clipboard.channel.closed"||(type=="clipboard.error"&&id.empty())){ready_=false;cancelReceive();reset();closeOffers(true);std::lock_guard lock(mutex_);sequenceConfigured_=false;sequenceSync_=false;requestSequence_=clip::RequestSequence{};used_.clear();lastSent_.reset();return;}
    if(type=="clipboard.ready"){require(value.get("protocol").string()=="clipboard-v1"&&value.get("sessionId").string()==sessionId_);std::lock_guard lock(mutex_);const bool sync=value.get("clipboardSync").boolean();require(!sequenceConfigured_||sync==sequenceSync_);sequenceSync_=sync;sequenceConfigured_=true;pairGeneration_=clipboardGenerationNumber(value.get("_channelGeneration").number());settled_=sync&&limits_.onPaste&&value.get("clipboardSettled").string()=="text-image-250-v1";intent_=settled_&&value.get("clipboardIntent").string()=="text-image-pending-v1";watchEpoch_=0;intentSequence_.reset();intentKind_.clear();ready_=true;return;}
    if(type=="clipboard.watch"){
      if(intent_){const auto epoch=uint32_t(wholeNumber(value.get("watchEpoch"),0xffffffff));require(epoch>0);
        if(value.get("enabled").boolean()){if(epoch<=watchEpoch_)return;watchEpoch_=epoch;intentSequence_.reset();intentKind_.clear();}
        else if(epoch!=watchEpoch_)return;}
      watchWanted_=ready_&&value.get("enabled").boolean();return;}
    if(type=="clipboard.offer.withdraw"){withdrawReceived(id);return;}
    require(ready_);
    if(type=="clipboard.read"){read(value);return;}
    if(type=="clipboard.pull"){servePull(value);return;}
    if(type=="clipboard.manifest"){serveManifest(value);return;}
    if(type=="clipboard.offer"){receiveOffer(value);return;}
    if(type=="clipboard.begin"){receiveBegin(value);return;}
    {std::lock_guard lock(mutex_);if(id!=activeId_||activeId_.empty()){
      // A pull that has already ended (the PC's cancel queued behind the read it stopped, a late ack): message() drops these while
      // they arrive; one queued while its pull was still running is dropped here, not reported as a second failure of that pull.
      if(clip::pullRequestId(id))return;
      if(sequenceSync_)require(requestSequence_.inspect(id)==clip::RequestSequenceStatus::Retired);return;}}
    if(type=="clipboard.cancel"||type=="clipboard.error"){
      // The peer ended the active transfer: who reported it and the reason it gave.
      if(diagActive_){diagnostic(type=="clipboard.cancel"?ClipboardDiagnostic::peerCancelled:ClipboardDiagnostic::peerFailed,false,type=="clipboard.error"?clipboardReasonCode(value.get("reason").string()):0);diagActive_=false;}
      cancelReceive();reset();return;}
    if(type=="clipboard.complete"){reset();return;}
    current();permitted(false,textFormat_);
    if(type=="clipboard.entries")receiveEntries(value);
    else if(type=="clipboard.chunk"){
      require(index_<sizes_.size()&&value.get("index").number(-1)==double(index_)&&value.get("offset").number(-1)==double(offset_));const auto bytes=clipboardChunk(value.get("data").string(),limits().chunkBytes());require(bytes.size()<=sizes_[index_]-offset_);if(incoming_)incoming_->append(index_,offset_,bytes);else if(textFormat_)text_.insert(text_.end(),bytes.begin(),bytes.end());else receiver_->append(index_,offset_,bytes);offset_+=bytes.size();
      // A windowed pair is acknowledged cumulatively (every few chunks); stop and wait acknowledges each chunk.
      if(ackPolicy_.chunk(bytes.size()))emit(fields("clipboard.chunk.ack",id)+",\"index\":"+std::to_string(index_)+",\"offset\":"+std::to_string(offset_)+'}');
    }else if(type=="clipboard.item.end"){
      require(index_<sizes_.size()&&value.get("index").number(-1)==double(index_)&&offset_==sizes_[index_]);if(incoming_){incoming_->finish(index_,value.get("sha256").string());}else if(textFormat_){require(clipboardDigest(text_)==value.get("sha256").string());const auto decoded=wide(std::string(text_.begin(),text_.end()));require(decoded.size()<=limits().maxTextUnits&&decoded.find(L'\0')==std::wstring::npos);}else{receiver_->finish(index_,value.get("sha256").string());receiveDigests_.push_back(value.get("sha256").string());}
      if(echo_&&(index_>=lastSent_->digests.size()||lastSent_->digests[index_]!=value.get("sha256").string()))echo_=false;
      ackPolicy_.itemEnd();emit(fields("clipboard.item.ack",id)+",\"index\":"+std::to_string(index_)+'}');index_++;offset_=0;
    }else if(type=="clipboard.commit"){
      require(index_==sizes_.size()&&(!sizes_.empty()||incoming_));current();permitted(false,textFormat_);
      if(incoming_){require(incoming_->complete());const DWORD sequence=incoming_->commit(owner_,receiveSequence_,&access_.publishers);published_=true;
        // A tree served as virtual files (absolute paths of MAX_PATH or more) is a data object no window of ours owns: its sequence marks it as our own, like an offer.
        if(incoming_->publishedVirtual())ownSequence_=sequence;
        watch_.applied(sequence);emit(fields("clipboard.applied",id)+'}');noteEnd();reset();return;}
      // An older client that did not recognise our content sends
      // it straight back. Byte-identical to the last read, and the clipboard
      // has not changed since: it already holds this content. Keep it (and
      // its original paths) and discard the private copy.
      if(echo_&&access_.watch.sequence()==lastSent_->sequence&&holdsReceived()&&access_.watch.sequence()==lastSent_->sequence){if(receiver_)receiver_->cancel();emit(fields("clipboard.applied",id)+'}');noteEnd();reset();return;}
      DWORD sequence=0;if(textFormat_)sequence=access_.putText(owner_,wide(std::string(text_.begin(),text_.end())),receiveSequence_);else{auto paths=receiver_->commit();receiver_.reset();try{storage_->record(paths,sizes_);sequence=access_.putFiles(owner_,paths,receiveSequence_);published_=true;}catch(...){if(clip::expireFiles(storage_->parent(),paths))storage_->forget(paths);throw;}}watch_.applied(sequence);emit(fields("clipboard.applied",id)+'}');noteEnd();reset();
    }else throw std::runtime_error("Unsupported clipboard packet");
  }
  // A copy can change the clipboard several times in a row (the application
  // publishing in steps, delayed rendering, other listeners adding formats).
  // Announce it once, with the latest sequence, after this much quiet, so the
  // client reads the settled content instead of an intermediate state.
  static constexpr UINT changeQuietMillis=250;
  // A copy of files (CF_HDROP, a shell item list or virtual files on the clipboard) is announced after less quiet. Its read is safe
  // against a copy that is still changing: an intent pair reads it bound to the announced sequence (read() refuses it as
  // clipboard_changed before and after reading if the clipboard moved on), every clipboard state is complete (one EmptyClipboard ...
  // CloseClipboard or OleSetClipboard), and a later step is a new change: announced again, the offer of the earlier step withdrawn.
  // Text and images keep 250 ms: the settled hint (text-image-250-v1) promises it and lets the client read them unbound at once.
  static constexpr UINT fileQuietMillis=80;
  static UINT quietFor(uint32_t formats){return (formats&7u)?fileQuietMillis:changeQuietMillis;}
  std::optional<DWORD> pendingChange_;
  uint64_t pendingChangeAt_=0;
  std::string copyKind(uint32_t formats){
    if(formats&7u)return "other";
    if(textToLocal_&&(formats&8u))return (formats&32u)?"richtext":"text";
    if(filesToLocal_&&(formats&16u))return "image";
    return "other";
  }
  void emitIntent(DWORD sequence,const std::string& kind){
    if(!intent_||!watchEpoch_||!ready_||!watchWanted_||stopped_)return;
    if(intentSequence_&&*intentSequence_==sequence&&intentKind_==kind)return;
    intentSequence_=sequence;intentKind_=kind;
    emit("{\"type\":\"clipboard.intent\",\"sessionId\":"+jsonQuote(sessionId_)+",\"sequence\":"+std::to_string(sequence)+",\"kind\":"+jsonQuote(kind)+",\"watchEpoch\":"+std::to_string(watchEpoch_)+'}');
  }
  void observedChange(DWORD sequence){
    pendingChange_=sequence;pendingChangeAt_=access_.monotonic();
    const auto before=access_.watch.sequence();const auto formats=access_.formats();const auto after=access_.watch.sequence();
    if(intent_)emitIntent(sequence,before==sequence&&after==sequence?copyKind(formats):"other");
    if(owner_)SetTimer(owner_,2,quietFor(formats),nullptr);
  }
  void flushChange(){
    if(owner_)KillTimer(owner_,2);
    if(pendingChange_){
      // Text and images: the full 250 ms (settledQuietRemaining). Files: fileQuietMillis, judged on what the clipboard holds now.
      const auto now=access_.monotonic();const auto elapsed=now>=pendingChangeAt_?now-pendingChangeAt_:0;
      const auto remaining=(access_.formats()&7u)?(elapsed>=fileQuietMillis?0u:UINT(fileQuietMillis-elapsed)):clip::settledQuietRemaining(pendingChangeAt_,now);
      if(remaining){if(owner_)SetTimer(owner_,2,remaining,nullptr);return;}}
    const auto sequence=pendingChange_;pendingChange_.reset();
    if(sequence&&!stopped_){
      if(watch_.active()){
        if(access_.watch.sequence()!=*sequence){watch_.changed();return;}
        const auto formats=access_.formats();
        if(access_.watch.sequence()!=*sequence){watch_.changed();return;}
        const bool hint=clip::settledTextImageHint(settled_,textToLocal_,filesToLocal_,formats);
        emitIntent(*sequence,copyKind(formats));
        if(limits().onPaste)withdrawKeptOffers();
        emit("{\"type\":\"clipboard.changed\",\"sessionId\":"+jsonQuote(sessionId_)+",\"sequence\":"+std::to_string(*sequence)+(hint?",\"settled\":\"text-image-250-v1\"":"")+(intent_?",\"watchEpoch\":"+std::to_string(watchEpoch_):"")+'}');note(ClipboardDiagnostic::changeAnnounced,0,formats);}
      else note(ClipboardDiagnostic::changeDropped,ClipboardNote::notActive);
    }
  }
  void syncWatch() {
    try {
      const bool allowed=!stopped_&&ready_&&watchWanted_&&(textToLocal_||filesToLocal_)&&check_();
      if(!allowed){
        // An announcement that was waiting for the copy to settle is lost with the listener: say why.
        if(pendingChange_&&!stopped_)note(ClipboardDiagnostic::changeDropped,(ready_&&!watchWanted_)?ClipboardNote::watchDisabled:ClipboardNote::notAllowed);
        watch_.close();pendingChange_.reset();if(owner_){KillTimer(owner_,1);KillTimer(owner_,2);}return;}
      if(!watch_.active()) {
        const bool listening=watch_.enable(owner_,sessionId_).has_value();
        if(!listening||!SetTimer(owner_,1,100,nullptr)){note(ClipboardDiagnostic::watchFailed,listening?ClipboardNote::timerFailed:ClipboardNote::listenerRejected);watch_.close();watchWanted_=false;return;}
      }
      watch_.validate();
    } catch (...) {note(ClipboardDiagnostic::watchFailed,ClipboardNote::watchException);watch_.close();watchWanted_=false;if(owner_)KillTimer(owner_,1);}
  }
  static LRESULT CALLBACK windowProcedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_NCCREATE)SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    auto* self=reinterpret_cast<ClipboardWorker*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(self&&(message==WM_CLIPBOARDUPDATE||(message==WM_TIMER&&wparam==1))) {
      try {self->syncWatch();if(message==WM_CLIPBOARDUPDATE)self->watch_.changed();}
      catch (...) {self->watch_.close();self->watchWanted_=false;}
      return 0;
    }
    if(self&&message==WM_APP+41){SetTimer(window,4,1500,nullptr);return 0;}
    if(self&&message==WM_TIMER&&wparam==4){self->endWaiting();return 0;}
    if(self&&message==WM_TIMER&&wparam==2) {
      try {self->syncWatch();self->flushChange();} catch (...) {self->pendingChange_.reset();}
      return 0;
    }
    if(message==WM_NCDESTROY)SetWindowLongPtrW(window,GWLP_USERDATA,0);
    return DefWindowProcW(window,message,wparam,lparam);
  }
  void pump(DWORD timeout) {
    const HANDLE event=wake_.value;
    MsgWaitForMultipleObjectsEx(1,&event,timeout,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    syncWatch();
    MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
      if(message.message==WM_QUIT){stopped_=true;break;}
      TranslateMessage(&message);DispatchMessageW(&message);
    }
  }
  void run(){
    if(!check_()){stopped_=true;return;}
    // An STA with OLE: the virtual files published for a paste are an OLE data object that Explorer calls into.
    const HRESULT com=OleInitialize(nullptr);
    static std::once_flag windowClass;
    try {std::call_once(windowClass,[]{WNDCLASSW value{};value.lpfnWndProc=windowProcedure;value.hInstance=GetModuleHandleW(nullptr);value.lpszClassName=L"Multitap.Clipboard.Worker.v1";if(!RegisterClassW(&value)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Clipboard window class failed");});}catch(...){stopped_=true;if(SUCCEEDED(com))OleUninitialize();return;}
    owner_=CreateWindowExW(0,L"Multitap.Clipboard.Worker.v1",L"Multitap Clipboard",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),this);
    if(!owner_){stopped_=true;if(SUCCEEDED(com))OleUninitialize();return;}
    // Content received from the client is published with owner_; while it
    // still owns the clipboard, a change notification is our own write.
    watch_.writer(owner_);
    while(!stopped_){Json value;bool available=false;{std::lock_guard lock(mutex_);if(stopped_)break;if(!queue_.empty()){value=std::move(queue_.front());queue_.pop_front();available=true;}}
      if(!available){pump(INFINITE);continue;}
      try{clip::clearWriteFailure();receive(value);}catch(const std::exception& failure){const auto id=value.get("requestId").string();const char* reason=value.get("type").string()=="clipboard.manifest"?manifestReason(failure.what()):clip::pullRequestId(id)?pullReason(failure.what()):failureReason(failure.what(),value.get("type").string()=="clipboard.read",limits().extended,limits().scale);noteFailure(value,failure.what(),reason);if(!cancelled_)error(id,reason);cancelReceive();reset();}catch(...){const auto id=value.get("requestId").string();noteFailure(value,"","clipboard_write_failed");if(!cancelled_)error(id,"clipboard_write_failed");cancelReceive();reset();}
      pump(0);
    }
    watch_.close();watch_.writer(nullptr);KillTimer(owner_,1);cancelReceive();reset();closeOffers(true);DestroyWindow(owner_);owner_=nullptr;if(SUCCEEDED(com))OleUninitialize();
  }
};
}
