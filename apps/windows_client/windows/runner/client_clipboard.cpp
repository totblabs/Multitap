#include "client_clipboard.h"
#include "clipboard_capture_serial.hpp"
#include "clipboard.hpp"
#include "storage.hpp"
#include <bcrypt.h>
#include <objbase.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace multitap {
namespace clip = clipboard;
using V = flutter::EncodableValue;
using M = flutter::EncodableMap;
namespace {
// Not reset by source retirement or ClientClipboardState destruction. Receipts
// are local to this native process and are never stored or sent over the relay.
std::atomic<uint64_t> lastCaptureSerial{0};
void Require(bool condition) { if (!condition) throw std::runtime_error("clipboard_transfer_failed"); }
void ExactFields(const M& args,std::initializer_list<const char*> fields) {
  Require(args.size()==fields.size());
  for(const auto* field:fields)Require(args.find(V(field))!=args.end());
}
void AllowedFields(const M& args,std::initializer_list<const char*> fields) {
  for(const auto& entry:args) {
    const auto* key=std::get_if<std::string>(&entry.first);Require(key!=nullptr);
    Require(std::any_of(fields.begin(),fields.end(),[key](const char* field){return *key==field;}));
  }
}
const V& Argument(const M& map, const char* key) {
  const auto found = map.find(V(key)); Require(found != map.end()); return found->second;
}
std::string String(const M& map, const char* key) { const auto* value = std::get_if<std::string>(&Argument(map,key)); Require(value != nullptr); return *value; }
uint64_t Number(const M& map, const char* key) {
  const auto& value = Argument(map,key); int64_t number = -1;
  if (const auto* n = std::get_if<int32_t>(&value)) number = *n;
  if (const auto* n = std::get_if<int64_t>(&value)) number = *n;
  Require(number >= 0); return static_cast<uint64_t>(number);
}
bool Identifier(const std::string& value) { return !value.empty() && value.size() <= 80 && value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos; }

PendingClipboardLease PendingLease(const M& args) {
  PendingClipboardLease lease;lease.owner=String(args,"owner");lease.token=String(args,"pendingToken");lease.kind=String(args,"pendingKind");lease.epoch=Number(args,"pendingEpoch");
  const auto source=Number(args,"pendingSourceSequence"),sequence=Number(args,"pendingSequence");Require(Identifier(lease.owner)&&Identifier(lease.token)&&lease.epoch>0&&source<=UINT32_MAX&&sequence<=UINT32_MAX&&(lease.kind=="text"||lease.kind=="richtext"||lease.kind=="image"));lease.sourceSequence=DWORD(source);lease.sequence=DWORD(sequence);return lease;
}
std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  Require(text.size() <= clip::maxTextBytes);
  const int count = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0); Require(count > 0);
  std::wstring out(count,L'\0'); Require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),count) == count); return out;
}
std::string Utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int count = WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr); Require(count > 0);
  std::string out(count,'\0'); Require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),count,nullptr,nullptr) == count); return out;
}
std::string Digest(const std::vector<uint8_t>& bytes) {
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
  Require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0) >= 0);
  // CNG retains this buffer until the hash handle is destroyed, including on
  // exception paths. Its lifetime must therefore exceed the guard's lifetime.
  std::vector<uint8_t> object;
  struct Guard { BCRYPT_ALG_HANDLE a; BCRYPT_HASH_HANDLE* h; ~Guard(){ if(*h) BCryptDestroyHash(*h); BCryptCloseAlgorithmProvider(a,0); } } guard{algorithm,&hash};
  DWORD length = 0, got = 0; Require(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&got,0) >= 0);
  object.resize(length); Require(BCryptCreateHash(algorithm,&hash,object.data(),length,nullptr,0,0) >= 0);
  if (!bytes.empty()) Require(BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0) >= 0);
  std::array<uint8_t,32> digest{}; Require(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0) >= 0);
  std::string out; constexpr char hex[]="0123456789abcdef"; for (const auto b : digest) { out += hex[b >> 4]; out += hex[b & 15]; } return out;
}
}
// Maps a native failure reason to the code the Dart side shows. A size, count or
// length violation keeps its real reason as limits_exceeded plus a detail
// ("text", "count", "file", "total") instead of a generic read failure.
// The files on the clipboard now, for the receive checks; a clipboard this cannot read as files (virtual files, shell-only items) has none.
static std::vector<std::filesystem::path> CurrentFiles(const ClientClipboardAccess& access,HWND owner) { try { return access.read(owner,false,true).files; } catch(...) { return {}; } }
static std::string FailureCode(const std::string& reason, bool capture, std::string& detail, bool pull = false) {
  // A pull tells the peer (and the user) that the file changed since the copy or that the offer is gone.
  if (pull&&(reason=="clipboard_selected_file_changed"||reason=="clipboard_offer_expired")) return reason;
  // Two causes of a busy pending observation keep the busy code; the reason travels as the message.
  if (reason=="clipboard_sequence_unavailable"||reason=="clipboard_owners_full") return "clipboard_busy";
  if (reason=="clipboard_empty"||reason=="unsupported_clipboard_format"||reason=="clipboard_busy"||reason=="clipboard_changed"||
      reason=="clipboard_policy_denied"||reason=="clipboard_disk_full"||reason=="clipboard_file_locked") return reason;
  if (reason=="clipboard_text_too_large") { detail="text"; return "clipboard_limits_exceeded"; }
  if (reason=="clipboard_file_count_limit") { detail="count"; return "clipboard_limits_exceeded"; }
  if (reason=="clipboard_file_name_or_size_limit") { detail="file"; return "clipboard_limits_exceeded"; }
  if (reason=="clipboard_total_size_limit") { detail="total"; return "clipboard_limits_exceeded"; }
  if (reason=="clipboard_image_too_large") { detail="image"; return "clipboard_limits_exceeded"; }
  if (reason=="clipboard_entry_count_limit") { detail="entries"; return "clipboard_limits_exceeded"; }
  if (reason=="clipboard_depth_limit") { detail="depth"; return "clipboard_limits_exceeded"; }
  if (reason=="clipboard_path_too_long") { detail="path"; return "clipboard_limits_exceeded"; }
  if (reason=="clipboard_selected_file_unavailable"||reason=="clipboard_selected_file_changed") return "clipboard_file_unavailable";
  if (reason=="clipboard_regular_files_only") return "unsupported_clipboard_format";
  // Writing this PC's clipboard failed (SetClipboardData, EmptyClipboard, memory): its own reason, with the Win32 error as the detail.
  if (reason=="clipboard_write_failed"||reason=="clipboard_empty_failed"||reason=="clipboard_allocation_failed") return capture?"clipboard_read_failed":"clipboard_write_failed";
  return capture?"clipboard_read_failed":"clipboard_transfer_failed";
}
namespace {
struct OwnedHandle { HANDLE value=nullptr; ~OwnedHandle(){ if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value); } };
std::vector<BYTE> TokenUserOf(HANDLE token) {
  DWORD size=0; GetTokenInformation(token,TokenUser,nullptr,0,&size);
  std::vector<BYTE> user(size); if(!size||size>65536||!GetTokenInformation(token,TokenUser,user.data(),size,&size)) user.clear(); return user;
}
}
DWORD ClaimClipboard(HWND window, std::optional<DWORD> expected) {
  for(int attempt=0;attempt<50;++attempt) {
    if(OpenClipboard(window)) {
      struct Close{~Close(){CloseClipboard();}} close;
      if(expected&&GetClipboardSequenceNumber()!=*expected) throw std::runtime_error("clipboard_changed");
      if(!EmptyClipboard()) throw std::runtime_error("clipboard_write_failed");
      return GetClipboardSequenceNumber();
    }
    Sleep(attempt<10?10:25);
  }
  throw std::runtime_error("clipboard_busy");
}
// The client runs unelevated like Explorer, so it publishes in this process.
DWORD PublishReceivedFiles(HWND window, const std::vector<std::filesystem::path>& files, std::optional<DWORD> expected) {
  return clip::putFiles(window, files, expected);
}
PublishCheck CheckDesktopRead(const std::vector<std::filesystem::path>& files) {
  PublishCheck check;
  for(const auto& path:files) if(path.native().size()>=MAX_PATH) check.flags|=8;
  OwnedHandle own; if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&own.value)) {
    TOKEN_ELEVATION elevation{}; DWORD size=0;
    if(GetTokenInformation(own.value,TokenElevation,&elevation,sizeof(elevation),&size)&&elevation.TokenIsElevated) check.flags|=1;
  }
  // The desktop shell's process is the Explorer that pastes. Test with a
  // duplicate of its token (read-only opens of our own files, then revert).
  DWORD pid=0; if(const HWND shell=GetShellWindow()) GetWindowThreadProcessId(shell,&pid);
  OwnedHandle process{pid?OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid):nullptr}, desktop, impersonation;
  if(!process.value||!OpenProcessToken(process.value,TOKEN_QUERY|TOKEN_DUPLICATE,&desktop.value)) { check.flags|=4; check.error=GetLastError(); return check; }
  const auto mine=own.value?TokenUserOf(own.value):std::vector<BYTE>(), theirs=TokenUserOf(desktop.value);
  if(mine.empty()||theirs.empty()||!EqualSid(reinterpret_cast<const TOKEN_USER*>(mine.data())->User.Sid,reinterpret_cast<const TOKEN_USER*>(theirs.data())->User.Sid)) check.flags|=2;
  if(!DuplicateTokenEx(desktop.value,TOKEN_IMPERSONATE|TOKEN_QUERY,nullptr,SecurityImpersonation,TokenImpersonation,&impersonation.value)||!SetThreadToken(nullptr,impersonation.value)) { check.flags|=4; check.error=GetLastError(); return check; }
  check.readable=true;
  for(const auto& path:files) {
    const HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) { check.readable=false; check.error=GetLastError(); break; }
    CloseHandle(file);
  }
  RevertToSelf();
  return check;
}
struct ClientClipboardState::State {
  explicit State(std::filesystem::path parent, ClientClipboardAccess access) : parent(std::move(parent)), access(std::move(access)), pending(this->access.pending) {}
  std::filesystem::path parent;
  ClientClipboardAccess access;
  PendingClipboard pending;
  V PendingObserve(const M& args) { return V(int64_t(pending.Observe(String(args,"owner"),Number(args,"epoch")))); }
  std::string source_owner, receiver_owner, request, format;
  enum class SourceKind { none, ordinary, offerOnly };
  uint64_t sourceSerial = 0;
  SourceKind sourceKind = SourceKind::none;
  // The session scope whose received content this adapter last published. It
  // outlives the transfer (release/cancel) on purpose: while the clipboard is
  // still owned by our window, that content came from this scope's peer.
  std::string committed_scope;
  // The received files that publish listed. Explorer may put the same list
  // back under its own ownership when it pastes; that is still the peer's.
  std::vector<std::filesystem::path> committed_files;
  std::vector<clip::FileSource> sources;
  // The formats profile: a captured rich text / image / folder tree, and one being received.
  std::unique_ptr<clip::Outgoing> outgoing;
  std::unique_ptr<clip::Incoming> incoming;
  std::vector<uint8_t> source_text, text;
  uint64_t source_offset = 0, text_size = 0;
  bool text_finished = false;
  std::optional<DWORD> receive_sequence;
  std::unique_ptr<clip::Storage> store;
  std::vector<uint64_t> sizes;
  // Files being received: names/sizes and each verified SHA-256.
  std::vector<clip::FileDescription> expected;
  std::vector<std::string> digests;
  std::unique_ptr<clip::FileReceiver> receiver;
  // Paste-pull. Offered: the copies this client kept (latest two) and the file being served. Received: the
  // virtual-file offers published on the clipboard (the newest is current; an older one may still be pasting).
  clip::OfferRegistry offers;
  std::mutex serveMutex; std::unique_ptr<clip::SourceOffer::Reader> reader; std::string readerOffer;  // the offer the reader belongs to
  std::mutex pullMutex; std::vector<std::shared_ptr<clip::PullOffer>> pullOffers; std::shared_ptr<clip::PullOffer> currentOffer; std::map<const clip::PullOffer*,std::string> pullOwners;
  std::shared_ptr<clip::Storage> offerStore;  // receipts of the files staged for CF_HDROP (on-paste revision)
  std::map<uint64_t, std::weak_ptr<clip::PullOffer>> ranges;
  std::atomic<uint64_t> nextOperation{0};
  // The walks of deferred copies no longer wanted (manifestCancel), by the VM's request id: a walk (manifestWalk, on the serve thread)
  // stops at its next step, also when the cancel came before it started. The latest few are kept.
  std::mutex walkMutex; std::deque<std::string> cancelledWalks;
  bool WalkCancelled(const std::string& walk) { std::lock_guard lock(walkMutex); return std::find(cancelledWalks.begin(),cancelledWalks.end(),walk)!=cancelledWalks.end(); }
  std::mutex eventMutex; std::deque<PullEvent> events; std::function<void()> notify;
  std::atomic<DWORD>* ownSequence = nullptr;
  void Post(PullEvent event) { std::lock_guard lock(eventMutex); events.push_back(std::move(event)); if (notify) notify(); }
  void CloseOffers(bool withdraw,const std::string& scope={}) {
    std::vector<std::shared_ptr<clip::PullOffer>> old;std::shared_ptr<clip::PullOffer> current;bool haveCurrent=false;
    offers.clear();
    {std::lock_guard lock(serveMutex);reader.reset();}
    {std::lock_guard lock(pullMutex);
      for(auto at=pullOffers.begin();at!=pullOffers.end();){const auto who=pullOwners.find(at->get());if(scope.empty()||(who!=pullOwners.end()&&who->second==scope)){old.push_back(*at);pullOwners.erase(at->get());at=pullOffers.erase(at);}else ++at;}
      if(currentOffer&&std::find(old.begin(),old.end(),currentOffer)!=old.end())current=std::move(currentOffer);
      for(auto at=ranges.begin();at!=ranges.end();){const auto live=at->second.lock();if(!live||std::find(old.begin(),old.end(),live)!=old.end())at=ranges.erase(at);else ++at;}
      haveCurrent=bool(currentOffer);
    }
    if(withdraw&&current){try{access.withdrawOffer(current);}catch(...){}}
    if(ownSequence&&!haveCurrent)ownSequence->store(0);
    for(auto& offer:old)offer->shutdown("clipboard_transport_closed");
  }
  ~State() { CloseOffers(false); }
  // Virtual files staged for the copy being sent (deleted with the last reader of them).
  std::shared_ptr<clip::StagedSource> staged;
  void ReleaseSource() { sources.clear(); outgoing.reset(); staged.reset(); source_text.clear(); source_owner.clear(); source_offset = 0; sourceSerial=0;sourceKind=SourceKind::none; }
  void CancelReceive() { receiver.reset(); incoming.reset(); store.reset(); sizes.clear(); expected.clear(); digests.clear(); receiver_owner.clear(); request.clear(); format.clear(); text.clear(); text_size=0; text_finished=false; receive_sequence.reset(); }
  std::filesystem::path Parent();
  std::shared_ptr<clip::PullOffer> MakeOffer(const M& args,clip::PullRenderPolicy policy={});
};
std::filesystem::path ClientClipboardState::State::Parent() {
  if (!parent.empty()) return parent;
  PWSTR local = nullptr; Require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&local)));
  parent=std::filesystem::path(local)/L"Multitap"/L"ClipboardReceives"; CoTaskMemFree(local); return parent;
}
std::shared_ptr<clip::PullOffer> ClientClipboardState::State::MakeOffer(const M& args,clip::PullRenderPolicy policy) {
  auto& s=*this;
    const auto offerId=String(args,"offerId"); Require(Identifier(offerId));
    std::string offerFormat="tree"; if(args.find(V("format"))!=args.end()) offerFormat=String(args,"format");
    Require(offerFormat=="tree"||offerFormat=="text"||offerFormat=="richtext"||offerFormat=="image");
    // The on-paste revision: our card is the only progress (the descriptors carry no FD_PROGRESSUI) and a small set is also CF_HDROP.
    bool ownProgress=false; if(args.find(V("ownProgress"))!=args.end()) { const auto* value=std::get_if<bool>(&Argument(args,"ownProgress")); Require(value!=nullptr); ownProgress=*value; }
    // A deferred copy (manifest-on-paste-v1): no list yet; the paste asks for it (a "manifest" pull event).
    bool deferred=false; if(args.find(V("deferred"))!=args.end()) { const auto* value=std::get_if<bool>(&Argument(args,"deferred")); Require(value!=nullptr); deferred=*value; }
    Require(!deferred||offerFormat=="tree");
    const flutter::EncodableList noItems;
    const auto* list=deferred?&noItems:std::get_if<flutter::EncodableList>(&Argument(args,"items")); Require(list&&(deferred||!list->empty())&&list->size()<=clip::maxEntries);
    std::vector<clip::Item> entries; std::vector<int64_t> mtimes;
    for(const auto& value:*list) {
      const auto* item=std::get_if<M>(&value); Require(item!=nullptr);
      clip::Item part; part.size=Number(*item,"size"); part.kind=String(*item,"kind");
      if(offerFormat=="tree") {
        Require(part.kind=="file"||part.kind=="dir");
        part.name=clip::treePathFromWire(Wide(String(*item,"name"))); part.attributes=static_cast<DWORD>(Number(*item,"attrs"))&0x27;
        mtimes.push_back(static_cast<int64_t>(Number(*item,"mtime")));
      } else if(part.kind=="png") { part.width=static_cast<uint32_t>(Number(*item,"width")); part.height=static_cast<uint32_t>(Number(*item,"height")); }
      entries.push_back(std::move(part));
    }
    auto holder=std::make_shared<std::weak_ptr<clip::PullOffer>>(); State* st=&s;
    clip::PullHooks hooks;
    hooks.request=[st,holder](uint64_t id,const std::string& offer,size_t index,uint64_t offset) {
      if(auto live=holder->lock()) {
        std::lock_guard lock(st->pullMutex);
        for(auto at=st->ranges.begin();at!=st->ranges.end();) at=at->second.expired()?st->ranges.erase(at):std::next(at);
        st->ranges[id]=live;
      }
      PullEvent event; event.kind="request"; event.rangeId=id; event.offerId=offer; event.index=index; event.offset=offset; st->Post(std::move(event));
    };
    hooks.cancel=[st](uint64_t id) { { std::lock_guard lock(st->pullMutex); st->ranges.erase(id); } PullEvent event; event.kind="cancel"; event.rangeId=id; st->Post(std::move(event)); };
    hooks.ackChunk=[st](uint64_t id,size_t index,uint64_t end) { PullEvent event; event.kind="ack"; event.rangeId=id; event.index=index; event.end=end; st->Post(std::move(event)); };
    hooks.failed=[st](uint64_t id,size_t index,const std::string& reason) { { std::lock_guard lock(st->pullMutex); st->ranges.erase(id); } PullEvent event; event.kind="failed"; event.rangeId=id; event.index=index; event.reason=reason; st->Post(std::move(event)); };
    auto operation=std::make_shared<uint64_t>(0);
    hooks.operationState=[st,offerId,operation](bool started,HRESULT result) {
      if(started)*operation=++st->nextOperation;
      if(!*operation)return;
      PullEvent event;event.kind="operation";event.offerId=offerId;event.rangeId=*operation;
      const auto failure=clip::pullOperationFailure(result);
      event.reason=started?"start":!failure?"end":std::string(failure)=="clipboard_cancelled"?"end-cancelled":"end-failed";
      if(!started)*operation=0;
      st->Post(std::move(event));
    };
    hooks.manifest=[st](const std::string& offer) { PullEvent event; event.kind="manifest"; event.offerId=offer; st->Post(std::move(event)); };
    hooks.manifestCancel=[st](const std::string& offer,const std::string& reason) { PullEvent event; event.kind="manifestCancel"; event.offerId=offer; event.reason=reason; st->Post(std::move(event)); };
    std::shared_ptr<clip::PullOffer> offer;
    // The relay's pull queue: files asked for ahead of their reader (1 when absent: one pull at a time).
    size_t prefetchDepth=1; if(args.find(V("prefetchDepth"))!=args.end()) { const auto depth=Number(args,"prefetchDepth"); Require(depth>=1&&depth<=clip::maxPrefetchDepth); prefetchDepth=size_t(depth); }
    if(deferred) {
      clip::PullOptions options; options.explorerProgress=!ownProgress; options.prefetchDepth=prefetchDepth;
      offer=clip::PullOffer::createDeferred(offerId,std::move(hooks),{},options);
    } else if(offerFormat=="tree") {
      clip::PullOptions options; options.explorerProgress=!ownProgress; options.prefetchDepth=prefetchDepth;
      if(ownProgress) { if(!s.offerStore) s.offerStore=std::make_shared<clip::Storage>(s.Parent()); options.stagingParent=s.offerStore->parent(); options.storage=s.offerStore; }
      offer=clip::PullOffer::create(offerId,std::move(entries),std::move(mtimes),std::move(hooks),{},options);
    } else offer=clip::PullOffer::createContent(offerId,offerFormat,std::move(entries),std::move(hooks),{},std::move(policy));
    *holder=offer;
    return offer;
}
ClientClipboardState::ClientClipboardState(std::filesystem::path parent, ClientClipboardAccess access) : state_(std::make_unique<State>(std::move(parent), std::move(access))) {}
ClientClipboardState::~ClientClipboardState() = default;
V ClientClipboardState::RunJob(const std::string& method,const M& args,HWND owner) {
  try { return Run(method,args,owner); }
  catch(...) {
    // Capture owns an exact-attempt guard. Scoped operations must never widen
    // malformed/stale errors into an owner-level release of a newer transfer.
    if(!detail::IsClipboardSourceJob(method)) {
      try { Run("release",args,owner); } catch(...) {}
    }
    throw;
  }
}
V ClientClipboardState::Run(const std::string& method, const M& args, HWND owner) {
  auto& s=*state_; const auto scope=String(args,"owner"); Require(Identifier(scope));
  if(method=="retireSource") {
    ExactFields(args,{"owner","captureSerial"});const auto serial=Number(args,"captureSerial");Require(serial>0);
    const bool matches=s.source_owner==scope&&s.sourceSerial==serial;
    if(matches)s.ReleaseSource();
    return V(M{{V("retired"),V(matches)}});
  }
  if(method=="pendingOwnerBarrier")return {};
  // On this clipboard thread (its own STA and message window), after any queued
  // cancellation barrier. GetClipboardSequenceNumber can read 0 right after a
  // stream starts: a PC that has not copied anything since it booted has a
  // clipboard number of 0, for as long as nothing is copied there
  // (PendingClipboard seeds it). A zero that
  // remains is retried briefly; repeating the same owner/epoch observation
  // changes nothing.
  if(method=="observePendingSequence"){
    for(int attempt=0;;++attempt){
      try{return s.PendingObserve(args);}
      catch(const std::exception& e){if(std::string(e.what())!="clipboard_sequence_unavailable"||attempt>=10)throw;}
      Sleep(50);
    }
  }
  if(method=="beginPending") {
    const auto expected=Number(args,"expectedLocalSequence"),source=Number(args,"sourceSequence"),epoch=Number(args,"epoch");Require(expected<=UINT32_MAX&&source<=UINT32_MAX&&epoch>0);
    const auto lease=s.pending.Begin(scope,epoch,DWORD(expected),DWORD(source),String(args,"kind"));if(!lease)return {};
    s.committed_scope=scope;s.committed_files.clear();
    return V(M{{V("token"),V(lease->token)},{V("sequence"),V(int64_t(lease->sequence))},{V("epoch"),V(int64_t(lease->epoch))},{V("sourceSequence"),V(int64_t(lease->sourceSequence))},{V("kind"),V(lease->kind)}});
  }
  if (method=="release") { if(s.source_owner==scope)s.ReleaseSource(); if(s.receiver_owner==scope)s.CancelReceive(); return {}; }
  if (method=="cancel") { if(s.receiver_owner==scope)s.CancelReceive(); return {}; }
  // The session ended: the offers this client kept are retired and what it published for the peer's offer leaves the clipboard.
  if (method=="closeOffers") { s.CloseOffers(true,scope); return {}; }
  // The peer's offer (files and folders announced by their manifest): published as virtual files; the bytes are
  // pulled when something pastes (a PullHooks request becomes a PullEvent for the session).
  if (method=="publishOffer") {
    auto offer=s.MakeOffer(args);
    const DWORD sequence=s.access.publishOffer(offer,std::nullopt);
    { std::lock_guard lock(s.pullMutex);
      s.pullOffers.push_back(offer); s.currentOffer=offer;s.pullOwners[offer.get()]=scope;
      // Only the latest two stay answerable: a paste that is still reading an older one keeps it until then.
      if(s.pullOffers.size()>clip::maxLiveOffers){s.pullOwners.erase(s.pullOffers.front().get());s.pullOffers.erase(s.pullOffers.begin());} }
    if(s.ownSequence) s.ownSequence->store(sequence);
    s.committed_scope=scope; s.committed_files.clear();
    return V(M{{V("sequence"),V(int64_t(sequence))}});
  }
  // The peer withdrew an offer (its clipboard changed): ours leaves the clipboard if it is still what the clipboard holds. (pullRetire has
  // already ended whatever was reading it.)
  if (method=="withdrawOffer") {
    const auto offerId=String(args,"offerId"); std::shared_ptr<clip::PullOffer> found;
    { std::lock_guard lock(s.pullMutex);
      for(auto at=s.pullOffers.begin();at!=s.pullOffers.end();++at) if((*at)->id()==offerId&&s.pullOwners[at->get()]==scope) { found=*at;s.pullOwners.erase(at->get());s.pullOffers.erase(at); break; }
      if(found&&s.currentOffer==found) s.currentOffer.reset(); }
    DWORD sequence=0; if(found) { try { sequence=s.access.withdrawOffer(found); } catch(...) {} }
    if(sequence&&s.ownSequence) s.ownSequence->store(sequence);
    return V(M{{V("sequence"),V(int64_t(sequence))}});
  }
  // This client's own clipboard changed: the offers it kept are no longer served. The ids come back so that the session can say so.
  // One a paste in the VM is still reading stays until that paste is quiet (OfferRegistry::pasteHold), as RDP keeps locked clipboard
  // data: copying something else on the PC does not break a paste already under way.
  if (method=="dropOffers") {
    flutter::EncodableList ids; const auto dropped=s.offers.dropIdle();
    for(const auto& id:dropped) ids.emplace_back(V(id));
    { std::lock_guard lock(s.serveMutex); if(std::find(dropped.begin(),dropped.end(),s.readerOffer)!=dropped.end()){ s.reader.reset(); s.readerOffer.clear(); } }
    return V(std::move(ids));
  }  // Diagnostics: is what this scope published still on the clipboard?
  if (method=="published") {
    const HWND current=s.access.owner();
    return V(M{{V("owned"),V(owner!=nullptr&&current==owner&&s.committed_scope==scope)},{V("sequence"),V(int64_t(s.access.sequence()))}});
  }
  if (method=="capture") {
    Require(s.source_owner.empty() || s.source_owner==scope);
    AllowedFields(args,{"owner","allowedText","allowedFiles","skipOwn","formats","pull","offerId","onPaste","richBytes","deferred"});
    bool allowedText=true,allowedFiles=true;
    if(args.find(V("allowedText"))!=args.end()||args.find(V("allowedFiles"))!=args.end()) {
      const auto* text=std::get_if<bool>(&Argument(args,"allowedText"));
      const auto* files=std::get_if<bool>(&Argument(args,"allowedFiles"));
      Require(text&&files&&(*text||*files));allowedText=*text;allowedFiles=*files;
    }
    bool skipOwn=false;
    if(args.find(V("skipOwn"))!=args.end()) { const auto* value=std::get_if<bool>(&Argument(args,"skipOwn")); Require(value!=nullptr); skipOwn=*value; }
    bool formats=false;
    if(args.find(V("formats"))!=args.end()) { const auto* value=std::get_if<bool>(&Argument(args,"formats")); Require(value!=nullptr); formats=*value; }
    // The paste-pull profile: a large copy of files and folders is offered under the transfer's id.
    bool pull=false; std::string offerId;
    if(args.find(V("pull"))!=args.end()) { const auto* value=std::get_if<bool>(&Argument(args,"pull")); Require(value!=nullptr); pull=*value; }
    if(args.find(V("offerId"))!=args.end()) { offerId=String(args,"offerId"); Require(Identifier(offerId)); }
    // The on-paste revision: a copy is silent. Files and folders are always offered; text, rich text and images only when they are too big to push.
    bool onPaste=false; if(args.find(V("onPaste"))!=args.end()) { const auto* value=std::get_if<bool>(&Argument(args,"onPaste")); Require(value!=nullptr); onPaste=*value; }
    // Deferred manifest (manifest-on-paste-v1): a copy of files that is not small is offered without walking it.
    bool deferred=false; if(args.find(V("deferred"))!=args.end()) { const auto* value=std::get_if<bool>(&Argument(args,"deferred")); Require(value!=nullptr); deferred=*value; }
    // Validate even when skipOwn will return before reading any data. Failed
    // pre-admission options preserve the old source and consume no receipt.
    clip::ReadOptions readOptions{allowedText,allowedFiles,allowedText,allowedFiles,true};
    if(args.find(V("richBytes"))!=args.end()) { const auto rich=Number(args,"richBytes"); Require(rich>=1&&rich<=clip::maxRichBytes); readOptions.richBytes=static_cast<size_t>(rich); }
    const auto reserved=detail::ReserveClipboardCaptureSerial(lastCaptureSerial);Require(reserved.has_value());
    const auto serial=*reserved;
    s.ReleaseSource();s.source_owner=scope;s.sourceSerial=serial;
    struct CaptureGuard {
      State& state;const std::string& ownerScope;uint64_t serial;bool complete=false;
      ~CaptureGuard(){if(!complete&&state.source_owner==ownerScope&&state.sourceSerial==serial)state.ReleaseSource();}
    } guard{s,scope,serial};
    // The guard covers every return and the receipt's final map allocation.
    V captured=[&]() -> V {
    // Automatic sharing: a data object this client published for the peer's offer is on the clipboard (no
    // window of ours owns it then): it is the peer's content and is never announced back.
    if(skipOwn) {
      std::shared_ptr<clip::PullOffer> current; { std::lock_guard lock(s.pullMutex); current=s.currentOffer; }
      if((current&&s.access.offerCurrent(current))||s.access.localTreeCurrent()) { s.ReleaseSource(); return V(M{{V("own"),V(true)},{V("sequence"),V(int64_t(s.access.sequence()))}}); }
    }
    // Our own window owns the clipboard: what this client published for the VM's copy (a placeholder for it, or the copy itself).
    // Reading it makes our own window render it on this thread: a placeholder still waiting for the VM's data holds this thread until
    // the data comes, one that was cancelled has none and fails the read. Automatic sharing never sends it back.
    if(owner&&s.access.owner()==owner) {
      if(skipOwn&&!s.committed_scope.empty()&&s.committed_scope==scope) { s.ReleaseSource(); return V(M{{V("own"),V(true)},{V("sequence"),V(int64_t(s.access.sequence()))}}); }
      if(s.pending.Unrenderable()) throw std::runtime_error("clipboard_empty");
    }
    // Virtual files (zip contents, mail attachments) are staged under the private transfer parent, for pairs with the formats profile.
    // The pair's own limits are enforced by the Dart session (it knows the negotiated profile); the native walk uses the open ceilings, so a
    // limit is always reported by the session with its precise reason and, for a path, the length of the longest one.
    if(formats) { clip::VirtualStageOptions stage; stage.parent=s.Parent(); stage.bounds=clip::Bounds::scale(); readOptions.virtualFiles=std::move(stage); }
    // A password manager's copy (marked for no monitoring) is not shared automatically; our own offers carry the same mark
    // and were recognised above.
    if(skipOwn&&!clip::ownDelayedContentIsCurrent()&&clip::sensitiveContentIsCurrent()) throw std::runtime_error("clipboard_policy_denied");
    const auto snapshot=formats?s.access.readFormats(owner,readOptions):s.access.read(owner,allowedText,allowedFiles); flutter::EncodableList items;
    s.staged=snapshot.staged;
    // Automatic sharing: content this scope received and published is still
    // on the clipboard (our window owns it), so the peer already has it.
    // Report that without opening any file or keeping a source.
    const auto sameFiles=[&]{
      if(s.committed_files.empty()||snapshot.files.size()!=s.committed_files.size())return false;
      for(size_t at=0;at<snapshot.files.size();++at) if(_wcsicmp(snapshot.files[at].c_str(),s.committed_files[at].c_str())!=0)return false;
      return true;
    };
    if(skipOwn&&!s.committed_scope.empty()&&s.committed_scope==scope&&((owner&&snapshot.owner==owner)||sameFiles())) {
      s.ReleaseSource(); return V(M{{V("own"),V(true)},{V("sequence"),V(int64_t(snapshot.sequence))}});
    }
    if(!((allowedFiles||snapshot.files.empty())&&(allowedText||!snapshot.text)))throw std::runtime_error("clipboard_policy_denied");
    // Not small: only that files were copied goes out now; the list is walked when the VM pastes (manifestWalk). Staged virtual files
    // are already local and keep their walked offer.
    if(formats&&pull&&onPaste&&deferred&&!offerId.empty()&&!snapshot.files.empty()&&!snapshot.staged&&
       !clip::treeFitsWithin(snapshot.files,clip::Bounds::scale(),clip::eagerManifestEntries,clip::eagerManifestBytes)) {
      s.offers.addDeferred(offerId,snapshot.files,snapshot.sequence);
      s.source_owner=scope;s.sourceKind=State::SourceKind::offerOnly;
      return V(M{{V("format"),V("tree")},{V("deferred"),V(true)},{V("items"),V(flutter::EncodableList{})},{V("sequence"),V(int64_t(snapshot.sequence))},
                 {V("tops"),V(int64_t(snapshot.files.size()))},{V("skipped"),V(int64_t(0))},{V("offer"),V(true)}});
    }
    if(formats&&pull&&!offerId.empty()&&!snapshot.files.empty()) {
      auto scan=clip::scanTree(snapshot.files,clip::Bounds::scale());
      if(clip::shouldOffer(scan,onPaste)) {
        // Only the manifest crosses now: every entry with its size, attributes and write time. The files are opened
        // when a pull reads them, and must still be the files that were copied.
        auto offer=clip::SourceOffer::fromScan(offerId,std::move(scan));
        for(size_t at=0;at<offer->items().size();++at) {
          const auto& item=offer->items()[at];
          std::wstring wire=item.name; std::replace(wire.begin(),wire.end(),L'\\',L'/');
          items.emplace_back(M{{V("size"),V(static_cast<int64_t>(item.size))},{V("kind"),V(item.kind)},{V("name"),V(Utf8(wire))},
              {V("attrs"),V(static_cast<int64_t>(item.attributes))},{V("mtime"),V(offer->mtimeMs(at))}});
        }
        M result{{V("format"),V("tree")},{V("items"),V(std::move(items))},{V("sequence"),V(int64_t(snapshot.sequence))},{V("tops"),V(static_cast<int64_t>(offer->tops()))},
            {V("skipped"),V(static_cast<int64_t>(offer->skipped()))},{V("offer"),V(true)}};
        offer->keepAlive(snapshot.staged);
        s.offers.add(std::shared_ptr<clip::SourceOffer>(std::move(offer)),onPaste?std::optional<DWORD>(snapshot.sequence):std::nullopt); s.source_owner=scope;s.sourceKind=State::SourceKind::offerOnly; return V(std::move(result));
      }
      // (an on-paste pair never pushes files: what cannot be offered is refused as too long a path)
      if(onPaste) throw clip::PathTooLongError(scan.longestPath,clip::maxPasteChars);
    }
    if(formats&&pull&&onPaste&&!offerId.empty()) if(auto content=clip::SourceOffer::fromContent(offerId,snapshot,clip::Bounds::scale(),true)) {
      for(const auto& item:content->items()) {
        M entry{{V("size"),V(static_cast<int64_t>(item.size))},{V("kind"),V(item.kind)}};
        if(item.kind=="text") entry[V("units")]=V(static_cast<int64_t>(item.units));
        if(item.kind=="png") { entry[V("width")]=V(static_cast<int64_t>(item.width)); entry[V("height")]=V(static_cast<int64_t>(item.height)); }
        items.emplace_back(std::move(entry));
      }
      M result{{V("format"),V(content->format())},{V("items"),V(std::move(items))},{V("sequence"),V(int64_t(snapshot.sequence))},{V("tops"),V(int64_t(0))},{V("skipped"),V(int64_t(0))},{V("offer"),V(true)}};
      s.offers.add(std::shared_ptr<clip::SourceOffer>(std::move(content)),snapshot.sequence); s.source_owner=scope;s.sourceKind=State::SourceKind::offerOnly; return V(std::move(result));
    }
    if(formats) if(auto outgoing=clip::Outgoing::capture(snapshot,clip::Bounds::scale())) {
      // Rich text, an image or a folder tree: the data items are addressed by ordinal; a tree lists every entry.
      for(const auto& item:outgoing->items()) {
        std::wstring wire=item.name; std::replace(wire.begin(),wire.end(),L'\\',L'/');
        M entry{{V("size"),V(static_cast<int64_t>(item.size))},{V("kind"),V(item.kind)}};
        if(!item.name.empty()) entry[V("name")]=V(Utf8(wire));
        if(item.kind=="text") entry[V("units")]=V(static_cast<int64_t>(item.units));
        if(item.kind=="png") { entry[V("width")]=V(static_cast<int64_t>(item.width)); entry[V("height")]=V(static_cast<int64_t>(item.height)); }
        if(item.kind=="file"||item.kind=="dir") entry[V("attrs")]=V(static_cast<int64_t>(item.attributes));
        items.emplace_back(std::move(entry));
      }
      M result{{V("format"),V(outgoing->format())},{V("items"),V(std::move(items))},{V("sequence"),V(int64_t(snapshot.sequence))},{V("tops"),V(static_cast<int64_t>(outgoing->tops()))},{V("skipped"),V(static_cast<int64_t>(outgoing->skipped()))}};
      s.outgoing=std::move(outgoing); s.source_owner=scope;s.sourceKind=State::SourceKind::ordinary; return V(std::move(result));
    }
    if (!snapshot.files.empty()) {
      uint64_t total=0;
      for (const auto& path:snapshot.files) {
        const DWORD attributes=GetFileAttributesW(clip::longPath(path).c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("unsupported_clipboard_format");
        s.sources.emplace_back(path); const auto& d=s.sources.back().description(); if(d.size>clip::scaleBytes-total)throw std::runtime_error("clipboard_total_size_limit"); total+=d.size;
        items.emplace_back(M{{V("name"),V(Utf8(d.name))},{V("size"),V(static_cast<int64_t>(d.size))}});
      }
    } else if(snapshot.text) {
      const auto encoded=Utf8(*snapshot.text); s.source_text.assign(encoded.begin(),encoded.end());
      items.emplace_back(M{{V("size"),V(static_cast<int64_t>(s.source_text.size()))},{V("units"),V(static_cast<int64_t>(snapshot.text->size()))}});
    } else throw std::runtime_error("clipboard_empty");
    s.source_owner=scope;s.sourceKind=State::SourceKind::ordinary; return V(M{{V("format"),V(s.sources.empty()?"text":"files")},{V("items"),V(std::move(items))},{V("sequence"),V(int64_t(snapshot.sequence))}});
    }();
    auto* result=std::get_if<M>(&captured);Require(result!=nullptr);
    result->emplace(V("sourceReceipt"),V(M{{V("owner"),V(scope)},{V("captureSerial"),V(int64_t(serial))}}));
    guard.complete=true;
    return captured;
  }
  // Chunk sizes: a stop-and-wait pair moves up to 32768 bytes per chunk, a formats pair formatsChunkBytes (46080), and that holds
  // for plain text and flat file lists too (they are not the formats "Outgoing"/"Incoming" objects). Both bounds below must admit it.
  if (method=="sourceChunk" || method=="sourceDigest" || method=="sourceChunkScoped" || method=="sourceDigestScoped") {
    const bool scoped=method=="sourceChunkScoped"||method=="sourceDigestScoped";
    const bool digest=method=="sourceDigest"||method=="sourceDigestScoped";
    if(scoped) {
      if(digest)ExactFields(args,{"owner","captureSerial","index"});
      else ExactFields(args,{"owner","captureSerial","index","offset","length"});
      const auto serial=Number(args,"captureSerial");Require(serial>0);
      if(scope!=s.source_owner||serial!=s.sourceSerial||s.sourceKind!=State::SourceKind::ordinary)throw std::runtime_error("clipboard_changed");
    }
    Require(scope==s.source_owner); const auto index=Number(args,"index");
    if (s.outgoing) {
      if(digest) return V(s.outgoing->digest(static_cast<size_t>(index)));
      const auto offset=Number(args,"offset"), length=Number(args,"length"); Require(length>0&&length<=clip::maxChunkBytes);
      return V(s.outgoing->read(static_cast<size_t>(index),offset,static_cast<size_t>(length)));
    }
    if (s.sources.empty()) {
      Require(index==0);
      if(digest) { Require(s.source_offset==s.source_text.size()); return V(Digest(s.source_text)); }
      const auto offset=Number(args,"offset"), length=Number(args,"length"); Require(offset==s.source_offset&&length>0&&length<=clip::formatsChunkBytes&&offset<=s.source_text.size());
      const size_t count=std::min<size_t>(static_cast<size_t>(length),s.source_text.size()-static_cast<size_t>(offset));
      std::vector<uint8_t> bytes(s.source_text.begin()+static_cast<size_t>(offset),s.source_text.begin()+static_cast<size_t>(offset)+count); s.source_offset+=count; return V(std::move(bytes));
    }
    Require(index<s.sources.size()); auto& source=s.sources[static_cast<size_t>(index)];
    if(digest) { Require(source.position()==source.description().size); return V(source.digest()); }
    const auto offset=Number(args,"offset"), length=Number(args,"length"); Require(offset==source.position()&&length>0&&length<=clip::formatsChunkBytes); return V(source.next(static_cast<size_t>(length)));
  }
  if(method=="begin") {
    Require(s.receiver_owner.empty()); const auto request=String(args,"requestId"), format=String(args,"format"); const bool newFormat=format=="richtext"||format=="image"||format=="tree";
    Require(Identifier(request)&&(format=="text"||format=="files"||newFormat));
    s.CancelReceive();
    s.receive_sequence=s.access.sequence();
    s.receiver_owner=scope; s.request=request; s.format=format;
    if(newFormat) {
      // Items: text/html/rtf parts, the one png, or every tree entry (folders have no data; files are numbered by ordinal).
      const auto* list=std::get_if<flutter::EncodableList>(&Argument(args,"items")); Require(list&&!list->empty()&&list->size()<=clip::maxEntries);
      std::vector<clip::Item> parts;
      for(const auto& value:*list) {
        const auto* item=std::get_if<M>(&value); Require(item!=nullptr);
        clip::Item part; part.size=Number(*item,"size");
        if(const auto kind=item->find(V("kind")); kind!=item->end()) { const auto* text=std::get_if<std::string>(&kind->second); Require(text!=nullptr); part.kind=*text; }
        if(format=="tree") {
          Require(part.kind=="file"||part.kind=="dir");
          part.name=clip::treePathFromWire(Wide(String(*item,"name")));
          if(item->find(V("attrs"))!=item->end()) part.attributes=static_cast<DWORD>(Number(*item,"attrs"))&0x27;
        }
        if(format=="image") { part.width=static_cast<uint32_t>(Number(*item,"width")); part.height=static_cast<uint32_t>(Number(*item,"height")); }
        parts.push_back(std::move(part));
      }
      if(format=="tree") s.store=std::make_unique<clip::Storage>(s.Parent());
      s.incoming=std::make_unique<clip::Incoming>(format,std::move(parts),s.Parent(),s.store.get(),CurrentFiles(s.access,owner),clip::Bounds::scale());
      return {};
    }
    const auto* items=std::get_if<flutter::EncodableList>(&Argument(args,"items")); Require(items&&!items->empty()&&items->size()<=clip::maxEntries);
    if(format=="text") { Require(items->size()==1); const auto* item=std::get_if<M>(&items->front()); Require(item!=nullptr); s.text_size=Number(*item,"size"); Require(s.text_size<=clip::maxTextBytes); s.text.reserve(static_cast<size_t>(s.text_size)); }
    else {
      std::vector<clip::FileDescription> descriptions; uint64_t total=0;
      for(const auto& value:*items) { const auto* item=std::get_if<M>(&value); Require(item!=nullptr); const auto size=Number(*item,"size"); Require(size<=clip::scaleBytes && size<=clip::scaleBytes-total); total+=size;
        descriptions.push_back({Wide(String(*item,"name")),size}); s.sizes.push_back(size); }
      s.store=std::make_unique<clip::Storage>(s.Parent());
      s.store->prepare(total,CurrentFiles(s.access,owner));
      s.receiver=std::make_unique<clip::FileReceiver>(s.Parent(),descriptions);
      s.expected=descriptions; s.digests.assign(descriptions.size(),std::string());
    }
    s.receiver_owner=scope; s.request=request; s.format=format; return {};
  }
  Require(s.receiver_owner==scope && String(args,"requestId")==s.request);
  // A copy made in the VM is still arriving: this PC's clipboard is claimed for it (emptied, ours), so a paste made meanwhile finds nothing
  // instead of the older content. Best effort: never an error; nothing is claimed when the clipboard changed since the transfer began.
  if(method=="claim") {
    if(!(s.format=="files"||s.format=="tree"||s.format=="image")||!s.receive_sequence||s.access.sequence()!=*s.receive_sequence)
      return V(M{{V("claimed"),V(false)}});
    try { const DWORD sequence=s.access.claim(owner,s.receive_sequence); s.receive_sequence=sequence; return V(M{{V("claimed"),V(true)},{V("sequence"),V(int64_t(sequence))}}); }
    catch(const std::exception&) { return V(M{{V("claimed"),V(false)}}); }
  }
  if(method=="append") {
    const auto index=Number(args,"index"), offset=Number(args,"offset"); const auto* bytes=std::get_if<std::vector<uint8_t>>(&Argument(args,"bytes")); Require(bytes&&!bytes->empty()&&bytes->size()<=(s.incoming?clip::maxChunkBytes:clip::formatsChunkBytes));
    if(s.incoming) { s.incoming->append(static_cast<size_t>(index),offset,*bytes); return {}; }
    if(s.format=="text") { Require(index==0&&!s.text_finished&&offset==s.text.size()&&bytes->size()<=s.text_size-offset); s.text.insert(s.text.end(),bytes->begin(),bytes->end()); }
    else s.receiver->append(static_cast<size_t>(index),offset,*bytes); return {};
  }
  if(method=="finish") {
    const auto index=Number(args,"index"); const auto digest=String(args,"digest");
    if(s.incoming) { s.incoming->finish(static_cast<size_t>(index),digest); return {}; }
    if(s.format=="text") { Require(index==0&&!s.text_finished&&s.text.size()==s.text_size&&Digest(s.text)==digest); const auto wide=Wide(std::string(s.text.begin(),s.text.end())); Require(wide.size()<=clip::maxTextUnits&&wide.find(L'\0')==std::wstring::npos); s.text_finished=true; }
    else { s.receiver->finish(static_cast<size_t>(index),digest); s.digests.at(static_cast<size_t>(index))=digest; } return {};
  }
  if(method=="commit") {
    // The peer sent back byte-identical content (the caller compared names,
    // sizes and every SHA-256 with what it sent) and the clipboard has not
    // changed since that capture: it already holds this content. Leave the
    // user's clipboard (and its original file paths) untouched and discard
    // the private copy. Text lives in the clipboard itself; listed files are
    // re-read, since a file may have been edited on disk since it was sent.
    if(args.find(V("unlessSequence"))!=args.end()) {
      const auto unless=Number(args,"unlessSequence"); Require(unless<=0xffffffffull&&(s.format!="text"||s.text_finished));
      const auto holds=[&]{
        if(s.format=="text")return true;
        try { return clip::filesHoldContent(CurrentFiles(s.access,owner),s.expected,s.digests); } catch(...) { return false; }
      };
      if(s.access.sequence()==DWORD(unless)&&holds()&&s.access.sequence()==DWORD(unless)) { s.CancelReceive(); return V(M{{V("sequence"),V(int64_t(unless))},{V("skipped"),V(true)}}); }
    }
    if(s.incoming) {
      // Rich text, image or folder tree: published whole by the library (a tree is stored privately; its top level goes on the clipboard).
      const DWORD sequence=s.incoming->commit(owner,s.receive_sequence,&s.access.publishers);
      // A tree served as virtual files (absolute paths of MAX_PATH or more) is a data object no window of ours owns: mark its sequence as our own.
      if(s.incoming->publishedVirtual()&&s.ownSequence) s.ownSequence->store(sequence);
      s.committed_files=s.incoming->publishedPaths(); s.committed_scope=scope; s.CancelReceive();
      return V(M{{V("sequence"),V(int64_t(sequence))}});
    }
    DWORD sequence=0; std::optional<PublishCheck> check;
    if(s.format=="text") { Require(s.text_finished); sequence=s.access.putText(owner,Wide(std::string(s.text.begin(),s.text.end())),s.receive_sequence); s.committed_files.clear(); }
    else {
      const auto paths=s.receiver->commit(); try { s.store->record(paths,s.sizes); sequence=s.access.putFiles(owner,paths,s.receive_sequence); } catch(...) { s.receiver.reset(); if(clip::expireFiles(s.Parent(),paths))s.store->forget(paths); throw; }
      s.committed_files=paths;
      // Published; diagnostics only from here, never a reason to fail.
      try { check=s.access.checkPublished(paths); } catch(...) { check=PublishCheck{false,0,4}; }
    }
    s.committed_scope=scope; s.CancelReceive();
    M result{{V("sequence"),V(int64_t(sequence))}};
    if(check) { result[V("readable")]=V(check->readable); result[V("readError")]=V(int64_t(check->error)); result[V("readFlags")]=V(int64_t(check->flags)); }
    return V(std::move(result));
  }
  throw std::runtime_error("clipboard_transfer_failed");
}
// What the session hands back for a range the virtual files asked for. Safe from any thread: nothing here waits
// for the reader and nothing here queues behind it.
void ClientClipboardState::Pull(const std::string& method, const M& args) {
  auto& s=*state_;
  std::string owner;if(args.find(V("owner"))!=args.end()){owner=String(args,"owner");Require(Identifier(owner));}
  if (method=="pullShutdown") {
    std::vector<std::shared_ptr<clip::PullOffer>> live; { std::lock_guard lock(s.pullMutex); for(const auto& offer:s.pullOffers)if(owner.empty()||s.pullOwners[offer.get()]==owner)live.push_back(offer); }
    const auto reason=String(args,"reason");
    for (auto& offer:live) offer->shutdown(reason);
    return;
  }
  if (method=="pullAbort"||method=="pullSupersede") {
    std::vector<std::shared_ptr<clip::PullOffer>> live; { std::lock_guard lock(s.pullMutex); for(const auto& offer:s.pullOffers)if(owner.empty()||s.pullOwners[offer.get()]==owner)live.push_back(offer); }
    // A newer copy passes over a paste already waiting for a deferred offer's list (it has begun); the user's cancel ends it too.
    for (auto& offer:live) { if (method=="pullAbort") offer->abortRenders("clipboard_cancelled",true); else offer->supersedeRenders("clipboard_offer_expired"); }
    return;
  }
  if (method=="pullRetire") {
    const auto offerId=String(args,"offerId"); std::shared_ptr<clip::PullOffer> found;
    { std::lock_guard lock(s.pullMutex); for (auto& offer:s.pullOffers) if (offer->id()==offerId&&(owner.empty()||s.pullOwners[offer.get()]==owner)) found=offer; }
    if (found) { found->abortRenders("clipboard_offer_expired",true); found->shutdown("clipboard_offer_expired"); }
    return;
  }
  // The walk of this client's deferred copy for the VM's paste (manifestWalk) is no longer wanted: it stops at its next step.
  if (method=="manifestCancel") {
    const auto walk=String(args,"walk"); Require(!walk.empty()&&walk.size()<=64);
    std::lock_guard lock(s.walkMutex); s.cancelledWalks.push_back(walk); while(s.cancelledWalks.size()>32) s.cancelledWalks.pop_front();
    return;
  }
  // The VM's answer to a list request of its deferred offer: the list, a failure, or progress (the wait's idle clock restarts).
  if (method=="manifestDeliver"||method=="manifestFail"||method=="manifestProgress") {
    const auto offerId=String(args,"offerId"); std::shared_ptr<clip::PullOffer> found;
    { std::lock_guard lock(s.pullMutex); for (auto& offer:s.pullOffers) if (offer->id()==offerId&&(owner.empty()||s.pullOwners[offer.get()]==owner)) found=offer; }
    if (!found) return;
    if (method=="manifestProgress") { found->manifestProgress(); return; }
    if (method=="manifestFail") { found->failManifest(String(args,"reason")); return; }
    const auto* list=std::get_if<flutter::EncodableList>(&Argument(args,"items"));
    if (!list||list->empty()||list->size()>clip::maxEntries) { found->failManifest("clipboard_transfer_failed"); return; }
    std::vector<clip::Item> entries; std::vector<int64_t> mtimes;
    try {
      for (const auto& value:*list) {
        const auto* item=std::get_if<M>(&value); Require(item!=nullptr);
        clip::Item part; part.size=Number(*item,"size"); part.kind=String(*item,"kind"); Require(part.kind=="file"||part.kind=="dir");
        part.name=clip::treePathFromWire(Wide(String(*item,"name"))); part.attributes=static_cast<DWORD>(Number(*item,"attrs"))&0x27;
        mtimes.push_back(static_cast<int64_t>(Number(*item,"mtime"))); entries.push_back(std::move(part));
      }
    } catch (const clip::PathTooLongError&) { found->failManifest("clipboard_path_too_long"); return; }
    catch (...) { found->failManifest("clipboard_transfer_failed"); return; }
    // A list the data object refuses fails its paste; the session hears why (the copy stays deferred: the next paste asks again).
    const auto refused=found->deliverManifest(std::move(entries),std::move(mtimes));
    if (!refused.empty()) throw std::runtime_error(refused);
    return;
  }
  const auto range=Number(args,"rangeId");
  std::shared_ptr<clip::PullOffer> offer;
  { std::lock_guard lock(s.pullMutex); const auto found=s.ranges.find(range); if(found!=s.ranges.end()) offer=found->second.lock(); }
  if (!offer) return;  // a range that was cancelled or whose offer is gone
  if (method=="pullData") {
    const auto* bytes=std::get_if<std::vector<uint8_t>>(&Argument(args,"bytes")); Require(bytes!=nullptr);
    offer->deliver(range,Number(args,"offset"),*bytes);
  } else if (method=="pullEnd") {
    offer->finish(range,String(args,"digest"));
    std::lock_guard lock(s.pullMutex); s.ranges.erase(range);
  } else if (method=="pullFail") {
    offer->fail(range,String(args,"reason"));
    std::lock_guard lock(s.pullMutex); s.ranges.erase(range);
  } else throw std::runtime_error("clipboard_transfer_failed");
}
// Serving the peer's pull of an offer this client kept: one file at a time, from an offset, hashed as it is read.
V ClientClipboardState::Serve(const std::string& method, const M& args) {
  auto& s=*state_; Require(Identifier(String(args,"owner")));
  // The VM pastes a deferred copy of this client: it must still be the copy selected (the same clipboard sequence, or the clipboard
  // naming the same paths), then it is walked now, progress every second (a "manifestProgress" pull event), and the walked offer takes
  // its place. The copy is pasting from the start of the walk (a newer copy on the PC does not drop it). The list comes back for the
  // session to send as clipboard.manifest.begin and the entry pages. A copy asked for again (the first answer was cut off or refused)
  // answers with the list walked before. The walk holds no lock the clipboard thread takes (dropOffers, closeOffers).
  if (method=="manifestWalk") {
    const auto offerId=String(args,"offerId"),walk=String(args,"walk"); Require(Identifier(offerId)&&!walk.empty()&&walk.size()<=64);
    const auto stub=s.offers.deferred(offerId);
    if (!stub) throw std::runtime_error("clipboard_offer_expired");
    std::shared_ptr<clip::SourceOffer> offer=stub->listed;
    if (!offer||!(s.offers.pasting(offerId)||s.access.sequence()==stub->sequence||clip::sameSelection(CurrentFiles(s.access,nullptr),stub->selection))) {
      if (s.access.sequence()!=stub->sequence&&!clip::sameSelection(CurrentFiles(s.access,nullptr),stub->selection)) throw std::runtime_error("clipboard_offer_expired");
      if (!s.offers.listing(offerId)) throw std::runtime_error("clipboard_offer_expired");
      auto last=std::chrono::steady_clock::now();
      const auto alive=[&](size_t entries) {
        if (s.WalkCancelled(walk)) throw std::runtime_error("clipboard_cancelled");
        const auto now=std::chrono::steady_clock::now(); if (now-last<clip::manifestProgressEvery) return; last=now;
        PullEvent event; event.kind="manifestProgress"; event.offerId=offerId; event.index=entries; s.Post(std::move(event));
      };
      clip::TreeScanControl control; control.progress=alive;
      auto scan=clip::scanTree(stub->selection,clip::Bounds::scale(),control);
      if (!clip::shouldOffer(scan,true)) throw clip::PathTooLongError(scan.longestPath,clip::maxPasteChars);
      const size_t walked=scan.entries.size();
      offer=std::shared_ptr<clip::SourceOffer>(clip::SourceOffer::fromScan(offerId,std::move(scan),[&](size_t){alive(walked);}));
      if (!s.offers.materialize(offerId,offer)) throw std::runtime_error("clipboard_offer_expired");
      const auto kept=s.offers.deferred(offerId); if (kept&&kept->listed) offer=kept->listed;
    } else s.offers.listing(offerId);
    if (s.WalkCancelled(walk)) throw std::runtime_error("clipboard_cancelled");
    flutter::EncodableList items;
    for (size_t at=0;at<offer->items().size();++at) {
      const auto& item=offer->items()[at];
      std::wstring wire=item.name; std::replace(wire.begin(),wire.end(),L'\\',L'/');
      items.emplace_back(M{{V("size"),V(static_cast<int64_t>(item.size))},{V("kind"),V(item.kind)},{V("name"),V(Utf8(wire))},
          {V("attrs"),V(static_cast<int64_t>(item.attributes))},{V("mtime"),V(offer->mtimeMs(at))}});
    }
    return V(M{{V("items"),V(std::move(items))},{V("tops"),V(static_cast<int64_t>(offer->tops()))},{V("skipped"),V(static_cast<int64_t>(offer->skipped()))}});
  }
  std::lock_guard lock(s.serveMutex);
  if (method=="pullOpen") {
    const auto offer=s.offers.find(String(args,"offerId"));
    if (!offer) throw std::runtime_error("clipboard_offer_expired");
    // Still the copy selected: the same clipboard sequence, or the clipboard naming the same paths (Windows renumbers it for more than a
    // new copy).
    auto reader=s.offers.openCurrent(offer->id(),static_cast<size_t>(Number(args,"index")),Number(args,"offset"),s.access.sequence,
                                     [&s]{ return CurrentFiles(s.access,nullptr); });
    s.reader=std::move(reader); s.readerOffer=offer->id();
    return V(static_cast<int64_t>(s.reader->size()));
  }
  if (method=="pullClose") { if(!s.readerOffer.empty())s.offers.served(s.readerOffer); s.reader.reset(); s.readerOffer.clear(); return {}; }
  Require(s.reader!=nullptr);
  // A long file keeps its paste going (OfferRegistry::pasteHold).
  if(!s.readerOffer.empty())s.offers.served(s.readerOffer);
  if (method=="pullRead") {
    // Up to readAheadChunks chunks per call (one platform round trip per ~1 MiB, clipboard_pull.dart); the reader hands them out in order.
    const auto length=Number(args,"length"); Require(length>0&&length<=clip::formatsChunkBytes*clip::readAheadChunks);
    std::vector<uint8_t> bytes; bytes.reserve(static_cast<size_t>(length));
    while (bytes.size()<static_cast<size_t>(length)) {
      const auto piece=s.reader->next(std::min<size_t>(clip::formatsChunkBytes,static_cast<size_t>(length)-bytes.size()));
      if (piece.empty()) break;
      bytes.insert(bytes.end(),piece.begin(),piece.end());
    }
    return V(std::move(bytes));
  }
  if (method=="pullDigest") return V(s.reader->digest());
  throw std::runtime_error("clipboard_transfer_failed");
}
std::vector<PullEvent> ClientClipboardState::TakePullEvents() {
  std::vector<PullEvent> out; std::lock_guard lock(state_->eventMutex);
  while (!state_->events.empty()) { out.push_back(std::move(state_->events.front())); state_->events.pop_front(); }
  return out;
}
void ClientClipboardState::SetPullNotify(std::function<void()> notify) { std::lock_guard lock(state_->eventMutex); state_->notify=std::move(notify); }
void ClientClipboardState::SetOwnSequenceSink(std::atomic<DWORD>* sink) { state_->ownSequence=sink;state_->pending.SetOwnSequenceSink([sink](DWORD sequence){if(sink)sink->store(sequence);}); }
void ClientClipboardState::AttachPending(HWND writer){state_->pending.Attach(writer);}
void ClientClipboardState::SetPendingRenderQueue(std::function<bool(std::function<void()>)> queue){state_->pending.SetRenderQueue(std::move(queue));}
void ClientClipboardState::DetachPending(){state_->pending.Detach();}
void ClientClipboardState::StopPending(){state_->pending.Shutdown();}
void ClientClipboardState::CloseAllOffers(){state_->CloseOffers(true);}
void ClientClipboardState::FailPending(const M& args){try{state_->pending.Cancel(PendingLease(args));}catch(...){}}
V ClientClipboardState::PendingControl(const std::string& method,const M& args){
  const auto scope=String(args,"owner");Require(Identifier(scope));
  if(method=="observePendingSequence")return V(int64_t(state_->pending.Observe(scope,Number(args,"epoch"))));
  if(method=="cancelPendingOwner"){state_->pending.CancelOwner(scope,Number(args,"epoch"));return {};}
  const auto lease=PendingLease(args);
  if(method=="bindPending")return V(state_->pending.Bind(lease,String(args,"requestId")));
  if(method=="cancelPending"){state_->pending.Cancel(lease);return {};}
  throw std::runtime_error("clipboard_transfer_failed");
}
V ClientClipboardState::PendingTransfer(const std::string& method,const M& args){
  const auto lease=PendingLease(args);const auto request=String(args,"requestId");Require(Identifier(request));
  if(method=="publishPendingOffer"){
    Require(String(args,"offerId")==request&&String(args,"format")==lease.kind);
    auto policy=state_->pending.DelegatePolicy(lease);auto current=policy.allowed;
    auto offer=state_->MakeOffer(args,std::move(policy));offer->setCurrentCheck(std::move(current));
    state_->pending.BindDelegate(lease,request,offer);
    {std::lock_guard lock(state_->pullMutex);state_->pullOffers.push_back(offer);state_->currentOffer=offer;state_->pullOwners[offer.get()]=lease.owner;if(state_->pullOffers.size()>clip::maxLiveOffers){state_->pullOwners.erase(state_->pullOffers.front().get());state_->pullOffers.erase(state_->pullOffers.begin());}}
    return V(M{{V("sequence"),V(int64_t(lease.sequence))},{V("pending"),V(true)}});
  }
  if(method=="begin"){
    const auto* items=std::get_if<flutter::EncodableList>(&Argument(args,"items"));Require(items&&!items->empty()&&items->size()<=3);std::vector<clip::Item> entries;
    for(const auto& value:*items){const auto* item=std::get_if<M>(&value);Require(item!=nullptr);clip::Item entry;entry.size=Number(*item,"size");if(item->find(V("kind"))!=item->end())entry.kind=String(*item,"kind");if(item->find(V("width"))!=item->end()){const auto n=Number(*item,"width");Require(n<=UINT32_MAX);entry.width=uint32_t(n);}if(item->find(V("height"))!=item->end()){const auto n=Number(*item,"height");Require(n<=UINT32_MAX);entry.height=uint32_t(n);}entries.push_back(std::move(entry));}
    state_->pending.BeginData(lease,request,String(args,"format"),std::move(entries));return {};
  }
  if(method=="append"){const auto* bytes=std::get_if<std::vector<uint8_t>>(&Argument(args,"bytes"));Require(bytes!=nullptr);const auto index=Number(args,"index");Require(index<3);state_->pending.Append(lease,request,size_t(index),Number(args,"offset"),*bytes);return {};}
  if(method=="finish"){const auto index=Number(args,"index");Require(index<3);state_->pending.Finish(lease,request,size_t(index),String(args,"digest"));return {};}
  if(method=="commit"){const DWORD sequence=state_->pending.Commit(lease,request);return V(M{{V("sequence"),V(int64_t(sequence))},{V("pending"),V(true)}});}
  throw std::runtime_error("clipboard_transfer_failed");
}

struct ClientClipboardChannel::State {
  struct Job { std::string method; M args; std::unique_ptr<flutter::MethodResult<V>> result; V value; std::string error; std::string detail; std::function<void()> work; };
  HWND window;
  explicit State(HWND window,clip::ChangeWatchAccess access,std::function<bool()> foreground,ClientClipboardAccess state_access,std::filesystem::path state_parent): window(window), watch(
      [this,foreground=std::move(foreground)]{return !stopped&&this->window&&(foreground?foreground():GetAncestor(GetForegroundWindow(),GA_ROOT)==GetAncestor(this->window,GA_ROOT));},
      [this](const std::string& owner,DWORD sequence){if(onChanged)onChanged(owner,sequence);},std::move(access)), core(std::move(state_parent),std::move(state_access)), thread([this]{Work();}), serveThread([this]{Serve();}),pendingThread([this]{DeliverPending();}) {
    // The virtual files published for the peer's offer are an OLE data object: no window of ours owns the
    // clipboard, so the publisher's own sequence is what marks them as ours.
    core.SetOwnSequenceSink(&ownSequence);
    core.SetPendingRenderQueue([this](std::function<void()> work){
      {std::lock_guard lock(mutex);if(stopped||pendingJobs.size()>=8)return false;Job job;job.work=std::move(work);pendingJobs.push_back(std::move(job));}
      pendingChanged.notify_one();return true;
    });
    watch.ownContent([this](DWORD sequence){ const DWORD own=ownSequence.load(); return own!=0&&own==sequence; });
    core.SetPullNotify([this]{ PostMessageW(this->window,ClientClipboardChannel::pull_message,0,0); });
  }
  clip::ChangeWatch watch;
  std::function<void(const std::string&,DWORD)> onChanged;
  std::function<void(const PullEvent&)> onPull;
  std::mutex mutex; std::deque<Job> jobs, controlJobs, serveJobs, pendingJobs, complete; std::atomic<bool> stopped{false};
  HANDLE wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  std::condition_variable serveChanged,pendingChanged;
  std::atomic<DWORD> ownSequence{0};
  ClientClipboardState core;
  std::thread thread, serveThread,pendingThread;
  ~State() { if(wake)CloseHandle(wake); }
  static void Pump() { MSG message; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } }
  static bool IsServe(const std::string& method) { return method=="pullOpen"||method=="pullRead"||method=="pullDigest"||method=="pullClose"||method=="manifestWalk"; }
  void Finish(Job& job) { {std::lock_guard lock(mutex);complete.push_back(std::move(job));} PostMessageW(window,ClientClipboardChannel::completed_message,0,0); }
  // The thread that owns the clipboard: an STA with a message loop, because the virtual files published for a
  // paste are an OLE data object that Explorer calls into for as long as they are on the clipboard.
  void Work() {
    const HRESULT ole=OleInitialize(nullptr);
    const HWND owner=CreateWindowExW(0,L"STATIC",L"Multitap Clipboard",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    // Received content is published with this window; the change listener
    // (the Flutter window) must recognise that as our own write.
    bool pendingAttached=false;try{core.AttachPending(owner);pendingAttached=true;}catch(...){}
    watch.writer(owner);
    for(;;) {
      Job job; bool have=false;
      {std::lock_guard lock(mutex); if(stopped&&jobs.empty()&&controlJobs.empty())break; if(!controlJobs.empty()){job=std::move(controlJobs.front());controlJobs.pop_front();have=true;}else if(!jobs.empty()){job=std::move(jobs.front()); jobs.pop_front(); have=true;}}
      if(!have) { const HANDLE handles[]={wake}; MsgWaitForMultipleObjectsEx(1,handles,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE); Pump(); continue; }
      clip::clearWriteFailure();
      try {
        if(!owner||stopped) {
          // RunJob was not entered, so preserve legacy preflight cleanup here.
          if(!detail::IsClipboardSourceJob(job.method)&&job.method!="observePendingSequence"){try{core.Run("release",job.args,owner);}catch(...){}}
          throw std::runtime_error("clipboard_transfer_failed");
        }
        // An observation changes no transfer: a failed one releases nothing (RunJob would).
        job.value=job.method=="observePendingSequence"?core.Run(job.method,job.args,owner):core.RunJob(job.method,job.args,owner);
      }
      catch(const std::exception& e) { const std::string reason=e.what();
        job.error=FailureCode(reason,job.method=="capture",job.detail);
        // The named causes of a busy pending observation travel as the message (fixed names, never free text).
        if(job.detail.empty()&&(reason=="clipboard_sequence_unavailable"||reason=="clipboard_owners_full"))job.detail=reason;
        // Why a write or an open of the clipboard failed (clip::writeFailureDetail): "win32:<detail>", for the diagnostics.
        if(job.detail.empty()&&(job.error=="clipboard_write_failed"||job.error=="clipboard_busy")) if(const uint32_t why=clip::writeFailureDetail()) job.detail="win32:"+std::to_string(why);
        // A path above what can be pasted names its length: "path:<characters>".
        if(job.detail=="path") if(const auto* longPath=dynamic_cast<const clip::PathTooLongError*>(&e)) job.detail="path:"+std::to_string(longPath->chars);
      }
      Finish(job); Pump();
    }
    core.CloseAllOffers();
    core.StopPending();if(pendingAttached)core.DetachPending();
    watch.writer(nullptr);
    if(owner)DestroyWindow(owner); if(SUCCEEDED(ole))OleUninitialize();
  }
  // Independent from the clipboard STA and file-serving worker. A paste may
  // block WM_RENDERFORMAT while these exact-token messages finish its payload.
  void DeliverPending(){
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    for(;;){Job job;{std::unique_lock lock(mutex);pendingChanged.wait(lock,[this]{return stopped||!pendingJobs.empty();});if(stopped&&pendingJobs.empty())break;job=std::move(pendingJobs.front());pendingJobs.pop_front();}
      try{if(stopped)throw std::runtime_error("clipboard_transport_closed");if(job.work)job.work();else job.value=core.PendingTransfer(job.method,job.args);}
      catch(const std::exception& e){core.FailPending(job.args);job.error=FailureCode(e.what(),false,job.detail);}
      if(job.result)Finish(job);
    }
    if(SUCCEEDED(initialized))CoUninitialize();
  }
  // Reads of the files this client offered, for the peer's paste. A separate thread: the clipboard thread may be
  // inside a read that waits for the peer, and a pull in the other direction must not wait for it.
  void Serve() {
    for(;;) {
      Job job; {std::unique_lock lock(mutex); serveChanged.wait(lock,[this]{return stopped||!serveJobs.empty();}); if(stopped&&serveJobs.empty())break; job=std::move(serveJobs.front()); serveJobs.pop_front();}
      try { if(stopped)throw std::runtime_error("clipboard_transfer_failed"); job.value=core.Serve(job.method,job.args); }
      catch(const std::exception& e) { job.error=FailureCode(e.what(),false,job.detail,true);
        // A walk that met a path above what can be pasted names its length: "path:<characters>".
        if(job.detail=="path") if(const auto* longPath=dynamic_cast<const clip::PathTooLongError*>(&e)) job.detail="path:"+std::to_string(longPath->chars);
        if(std::string(e.what())=="clipboard_cancelled") job.error="clipboard_cancelled"; }
      Finish(job);
    }
  }
};
ClientClipboardChannel::ClientClipboardChannel(HWND window,clip::ChangeWatchAccess access,std::function<bool()> foreground,ClientClipboardAccess state_access,std::filesystem::path state_parent):state_(std::make_unique<State>(window,std::move(access),std::move(foreground),std::move(state_access),std::move(state_parent))){}
ClientClipboardChannel::~ClientClipboardChannel() {
  state_->watch.close(); {std::lock_guard lock(state_->mutex);state_->stopped=true;} SetEvent(state_->wake); state_->serveChanged.notify_all();state_->pendingChanged.notify_all();state_->core.StopPending();
  // Anything waiting for the peer must end now so the clipboard thread can leave.
  state_->core.Pull("pullShutdown",M{{V("reason"),V("clipboard_transport_closed")}});
  CancelSynchronousIo(state_->thread.native_handle()); if(state_->thread.joinable())state_->thread.join(); if(state_->serveThread.joinable())state_->serveThread.join();if(state_->pendingThread.joinable())state_->pendingThread.join(); Complete();
}
DWORD ClientClipboardChannel::Watch(const std::string& owner,bool enabled) {
  Require(Identifier(owner));
  if(!enabled){const auto previous=state_->watch.lastSequence();state_->watch.disable(owner);return previous;}
  const auto sequence=state_->watch.enable(state_->window,owner);
  if(!sequence)throw std::runtime_error("clipboard_not_active");
  return *sequence;
}
bool ClientClipboardChannel::WatchFoundOwn(){return state_->watch.ownCurrent();}
void ClientClipboardChannel::ClipboardChanged(){try{state_->watch.changed();}catch(...){state_->watch.close();}}
void ClientClipboardChannel::ForegroundLost(){state_->watch.close();}
void ClientClipboardChannel::SetChanged(std::function<void(const std::string&,DWORD)> changed){state_->onChanged=std::move(changed);}
void ClientClipboardChannel::SetPullHandler(std::function<void(const PullEvent&)> handler){state_->onPull=std::move(handler);}
void ClientClipboardChannel::DeliverPullEvents(){ if(!state_->onPull)return; for(const auto& event:state_->core.TakePullEvents()) state_->onPull(event); }
void ClientClipboardChannel::Enqueue(const flutter::MethodCall<V>& call,std::unique_ptr<flutter::MethodResult<V>> result) {
  const auto* args=std::get_if<M>(call.arguments()); if(!args){result->Error("invalid_arguments","Clipboard operation arguments required.");return;}
  const auto& method=call.method_name();
  // Data, end and failure notices of a pulled range go straight to the library: they must not wait behind a read
  // that is waiting for exactly them.
  if(method=="pullData"||method=="pullEnd"||method=="pullFail"||method=="pullShutdown"||method=="pullAbort"||method=="pullSupersede"||method=="pullRetire"||
     method=="manifestCancel"||method=="manifestDeliver"||method=="manifestFail"||method=="manifestProgress") {
    try { state_->core.Pull(method,*args); result->Success(); }
    catch(const std::exception& e) {
      // A list the data object refused (manifestDeliver) names its clipboard_* reason; anything else is a failed operation.
      const std::string reason=e.what();
      if(method=="manifestDeliver"&&reason.rfind("clipboard_",0)==0&&reason.size()<=64){std::string detail;const auto code=FailureCode(reason,false,detail);result->Error(code,detail.empty()?"Windows clipboard operation failed.":detail);}
      else result->Error("clipboard_transfer_failed","Windows clipboard operation failed.");
    }
    return;
  }
  if(method=="observePendingSequence"){
    // Read on the clipboard thread, in order after a queued cancellation barrier (see Run).
    bool queued=false;{std::lock_guard lock(state_->mutex);if(!state_->stopped&&state_->controlJobs.size()<8){state_->controlJobs.push_back({method,*args,std::move(result),{}, {}});queued=true;}}
    if(queued)SetEvent(state_->wake);else result->Error(state_->stopped?"clipboard_transport_closed":"clipboard_busy","clipboard_cancel_pending");
    return;
  }
  if(method=="bindPending"||method=="cancelPending"||method=="cancelPendingOwner"){
    try{if(state_->stopped)throw std::runtime_error("clipboard_transport_closed");
      const V value=state_->core.PendingControl(method,*args);
      if(method=="cancelPendingOwner"){
        // Invalidate immediately, but acknowledge only after the STA's current
        // admitted publication finishes. No clipboard side effect may arrive
        // after this cancellation ACK; the platform thread never waits here.
        bool queued=false;{std::lock_guard lock(state_->mutex);if(!state_->stopped&&state_->controlJobs.size()<8){state_->controlJobs.push_back({"pendingOwnerBarrier",*args,std::move(result),{}, {}});queued=true;}}
        if(queued)SetEvent(state_->wake);else result->Error("clipboard_busy","clipboard_cancel_pending");
      }else result->Success(value);
    }
    catch(const std::exception& e){
      std::string detail;const std::string reason=e.what();
      // A fixed reason name (never free text) tells which busy cause it was.
      const bool named=reason=="clipboard_sequence_unavailable"||reason=="clipboard_owners_full";
      result->Error(FailureCode(reason,false,detail),named?reason:"Windows clipboard operation failed.");}return;
  }
  // Source operations always reach the normal STA's closed argument checks.
  // Mixed pending fields must not cancel an unrelated valid pending lease.
  if(!detail::IsClipboardSourceJob(method)&&args->find(V("pendingToken"))!=args->end()){
    const auto invalid=[&]{state_->core.FailPending(*args);result->Error("clipboard_transfer_failed","Invalid pending clipboard message.");};
    if(method!="begin"&&method!="append"&&method!="finish"&&method!="commit"&&method!="publishPendingOffer"){invalid();return;}
    if(method=="append"){const auto found=args->find(V("bytes"));const auto* bytes=found==args->end()?nullptr:std::get_if<std::vector<uint8_t>>(&found->second);if(!bytes||bytes->empty()||bytes->size()>clip::maxChunkBytes){invalid();return;}}
    if(method=="finish"){const auto found=args->find(V("digest"));const auto* digest=found==args->end()?nullptr:std::get_if<std::string>(&found->second);if(!digest||digest->size()!=64||digest->find_first_not_of("0123456789abcdef")!=std::string::npos){invalid();return;}}
    if(method=="begin"||method=="publishPendingOffer"){const auto found=args->find(V("items"));const auto* items=found==args->end()?nullptr:std::get_if<flutter::EncodableList>(&found->second);if(!items||items->empty()||items->size()>3){invalid();return;}}
    bool queued=false;{std::lock_guard lock(state_->mutex);if(!state_->stopped&&state_->pendingJobs.size()<8){state_->pendingJobs.push_back({method,*args,std::move(result),{}, {}});queued=true;}}
    if(queued)state_->pendingChanged.notify_one();else{state_->core.FailPending(*args);result->Error("clipboard_busy","Clipboard delivery worker is busy.");}return;
  }
  const bool serve=State::IsServe(method);
  {std::lock_guard lock(state_->mutex);
    auto& queue=serve?state_->serveJobs:state_->jobs;
    if(state_->stopped||queue.size()>=8){result->Error("clipboard_busy","Clipboard worker is busy.");return;}
    queue.push_back({method,*args,std::move(result),{}, {}});}
  if(serve)state_->serveChanged.notify_one(); else SetEvent(state_->wake);
}
void ClientClipboardChannel::Complete(){std::deque<State::Job> jobs;{std::lock_guard lock(state_->mutex);jobs.swap(state_->complete);}for(auto& job:jobs){if(job.error.empty())job.result->Success(job.value);else job.result->Error(job.error,job.detail.empty()?"Windows clipboard operation failed.":job.detail);}}
}
