#include "pending_clipboard.hpp"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace multitap {
namespace {
void pendingRequire(bool value,const char* code="clipboard_transfer_failed") { if(!value)throw std::runtime_error(code); }
bool pendingId(const std::string& value) { return !value.empty()&&value.size()<=80&&value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos; }
bool pendingEpoch(uint64_t value) { return value>0&&value<=uint64_t((std::numeric_limits<int64_t>::max)()); }
struct NativePendingMedium final : PendingClipboardMedium {
  HGLOBAL value=nullptr;
  ~NativePendingMedium() override { if(value)GlobalFree(value); }
};
PendingClipboardMediumPtr preparePendingMedium(std::vector<uint8_t> bytes) {
  pendingRequire(!bytes.empty(),"clipboard_allocation_failed");
  auto out=std::make_shared<NativePendingMedium>();out->value=GlobalAlloc(GMEM_MOVEABLE,bytes.size());
  pendingRequire(out->value!=nullptr,"clipboard_allocation_failed");
  void* memory=GlobalLock(out->value);pendingRequire(memory!=nullptr,"clipboard_allocation_failed");
  std::memcpy(memory,bytes.data(),bytes.size());GlobalUnlock(out->value);return out;
}
std::string pendingToken() {std::array<uint8_t,32> bytes{};pendingRequire(BCryptGenRandom(nullptr,bytes.data(),ULONG(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0);std::string out;constexpr char hex[]="0123456789abcdef";for(auto b:bytes){out+=hex[b>>4];out+=hex[b&15];}return out;}
// Incoming has already validated UTF-8. Keep the Windows text contract strict.
std::vector<uint8_t> pendingWideText(const std::wstring& text) {
  pendingRequire(text.size()<=clipboard::maxTextUnits&&text.find(L'\0')==std::wstring::npos,"clipboard_text_too_large");
  std::vector<uint8_t> out;out.reserve((text.size()+1)*2);
  for(size_t i=0;i<text.size();++i){const uint16_t c=uint16_t(text[i]);if(c>=0xd800&&c<=0xdbff){pendingRequire(i+1<text.size()&&uint16_t(text[i+1])>=0xdc00&&uint16_t(text[i+1])<=0xdfff);}else if(c>=0xdc00&&c<=0xdfff)pendingRequire(i&&uint16_t(text[i-1])>=0xd800&&uint16_t(text[i-1])<=0xdbff);out.push_back(uint8_t(c));out.push_back(uint8_t(c>>8));}
  out.push_back(0);out.push_back(0);return out;
}
std::vector<uint8_t> pendingDib(const std::vector<uint8_t>& bgra,uint32_t width,uint32_t height,bool v5) {
  const uint64_t stride=v5?uint64_t(width)*4:((uint64_t(width)*3+3)&~uint64_t(3));
  const size_t header=v5?sizeof(BITMAPV5HEADER):sizeof(BITMAPINFOHEADER);
  pendingRequire(width&&height&&width<=uint32_t((std::numeric_limits<LONG>::max)())&&height<=uint32_t((std::numeric_limits<LONG>::max)())&&
    uint64_t(width)*height<=clipboard::maxImagePixels&&bgra.size()==uint64_t(width)*height*4&&stride*height<=(std::numeric_limits<DWORD>::max)()&&stride*height<=size_t(-1)-header,"clipboard_image_too_large");
  std::vector<uint8_t> out(header+size_t(stride)*height,0);
  if(v5){BITMAPV5HEADER h{};h.bV5Size=sizeof(h);h.bV5Width=LONG(width);h.bV5Height=-LONG(height);h.bV5Planes=1;h.bV5BitCount=32;h.bV5Compression=BI_BITFIELDS;h.bV5SizeImage=DWORD(stride*height);h.bV5RedMask=0x00ff0000;h.bV5GreenMask=0x0000ff00;h.bV5BlueMask=0x000000ff;h.bV5AlphaMask=0xff000000;h.bV5CSType=LCS_sRGB;h.bV5Intent=LCS_GM_IMAGES;std::memcpy(out.data(),&h,sizeof(h));std::memcpy(out.data()+header,bgra.data(),bgra.size());}
  else {BITMAPINFOHEADER h{};h.biSize=sizeof(h);h.biWidth=LONG(width);h.biHeight=-LONG(height);h.biPlanes=1;h.biBitCount=24;h.biCompression=BI_RGB;h.biSizeImage=DWORD(stride*height);std::memcpy(out.data(),&h,sizeof(h));for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){const size_t from=(size_t(y)*width+x)*4,to=header+size_t(y)*size_t(stride)+size_t(x)*3;std::memcpy(out.data()+to,bgra.data()+from,3);}}
  return out;
}
constexpr wchar_t pendingProperty[]=L"Multitap.PendingClipboard.State";
}
struct PendingClipboard::State {
  enum class Phase { awaiting,bound,receiving,preparing,ready,delegated,failed };
  struct Owner {uint64_t epoch=0,closedThrough=0;};
  struct Record {PendingClipboardLease lease;DWORD latest=0;std::set<DWORD> ownChain;std::string request;Phase phase=Phase::awaiting;std::shared_ptr<clipboard::Incoming> incoming;std::shared_ptr<clipboard::PullOffer> delegate;std::set<UINT> requested;std::map<UINT,PendingClipboardMediumPtr> media;bool waitStarted=false;std::chrono::steady_clock::time_point deadline;};
  PendingClipboardAccess access;std::chrono::milliseconds timeout;HWND writer=nullptr;WNDPROC previous=nullptr;
  std::mutex mutex;std::condition_variable changed;std::map<std::string,Owner> owners;std::shared_ptr<Record> current;std::function<void(DWORD)> ownSequence;std::function<bool(std::function<void()>)> queue;bool stopped=false;UINT png=0,html=0;
  // Owners are one per clipboard session and are never reused. A closed one
  // (closedThrough reached its epoch) that holds no record only remembers its
  // closing; it is dropped when the table is full so later sessions still fit.
  std::map<std::string,Owner>::iterator Admit(const std::string& owner) {
    auto found=owners.find(owner);if(found!=owners.end())return found;
    if(owners.size()>=64)for(auto it=owners.begin();it!=owners.end();){
      const bool held=current&&current->lease.owner==it->first;
      if(!held&&it->second.closedThrough>=it->second.epoch)it=owners.erase(it);else ++it;
    }
    pendingRequire(owners.size()<64,"clipboard_owners_full");return owners.emplace(owner,Owner{}).first;
  }
  bool Live(const PendingClipboardLease& lease) const {const auto found=owners.find(lease.owner);return !stopped&&found!=owners.end()&&found->second.epoch==lease.epoch&&lease.epoch>found->second.closedThrough;}
  bool Same(const PendingClipboardLease& lease) const {return current&&current->lease.token==lease.token&&current->lease.owner==lease.owner&&current->lease.epoch==lease.epoch&&current->lease.sourceSequence==lease.sourceSequence&&current->lease.kind==lease.kind&&current->lease.sequence==lease.sequence;}
  std::shared_ptr<Record> RequireRecord(const PendingClipboardLease& lease,const std::string& request) {pendingRequire(Same(lease)&&Live(lease)&&current->request==request&&current->phase!=Phase::failed,"clipboard_changed");return current;}
  // Ownership never left our window: anyone else's EmptyClipboard makes them the owner (and fails the record by WM_DESTROYCLIPBOARD). A
  // number that moved meanwhile is Windows finishing our own publication (it can move it when a clipboard closes, adding the formats it
  // derives from text) or a render; it is adopted, not taken for a PC copy. An exact-number test would refuse every binding.
  bool Current(Record& r) {
    if(!writer||access.owner()!=writer)return false;
    const DWORD now=access.sequence();if(!now)return false;
    if(now!=r.latest){r.latest=now;r.ownChain.insert(now);if(ownSequence)ownSequence(now);}
    return true;
  }
  void Fail(const std::shared_ptr<Record>& r) {if(r){r->phase=Phase::failed;r->incoming.reset();r->media.clear();if(r->delegate)r->delegate->shutdown("clipboard_offer_expired");}changed.notify_all();}
  // A sequence number of 0 is a clipboard nothing has written since the window station was created: the PC right after it
  // booted. Windows counts from 0 and only a change moves the number, so it stays 0 until something is copied on the PC,
  // which never happens while the user works inside the VM; the VM's copies cannot be published because this arm never
  // completes (every arm fails for as long as that lasts, and the automatic PC -> VM send finds
  // the clipboard empty). Not unavailable at all: the clipboard is empty and opens. Emptying it once makes the number 1,
  // changes no content, and makes the writer the owner, so the watch takes the change as this client's own. A 0 for any
  // other reason (no clipboard access, content with a 0 number) fails as before. Called with the mutex held; releases it
  // around the clipboard calls (EmptyClipboard can notify a previous owner synchronously) and holds it again on return.
  DWORD Seed(std::unique_lock<std::mutex>& lock,const std::string& owner,uint64_t epoch) {
    const HWND target=writer;if(!target)return 0;
    lock.unlock();DWORD sequence=0;
    if(access.open(target)){
      struct Close {std::function<void()> close;~Close(){close();}} close{access.close};
      // Between the read and this open the user may have copied: a number that moved is theirs, kept as it is.
      if(access.count()==0&&access.sequence()==0)(void)access.empty();
    }
    sequence=access.sequence();
    lock.lock();
    const auto found=owners.find(owner);
    pendingRequire(!stopped&&found!=owners.end()&&found->second.epoch==epoch&&epoch>found->second.closedThrough,"clipboard_changed");
    if(sequence&&access.owner()==target&&ownSequence)ownSequence(sequence);
    return sequence;
  }
};
PendingClipboard::PendingClipboard(PendingClipboardAccess a,std::chrono::milliseconds timeout):state_(std::make_shared<State>()) {
  auto& s=*state_;s.timeout=(std::max)(std::chrono::milliseconds(1),(std::min)(timeout,std::chrono::milliseconds(5000)));
  if(!a.sequence)a.sequence=[] {return GetClipboardSequenceNumber();};if(!a.owner)a.owner=[] {return GetClipboardOwner();};
  if(!a.open)a.open=[](HWND w){return OpenClipboard(w)!=FALSE;};if(!a.close)a.close=[] {CloseClipboard();};if(!a.empty)a.empty=[] {return EmptyClipboard()!=FALSE;};
  if(!a.count)a.count=[] {SetLastError(ERROR_SUCCESS);const int n=CountClipboardFormats();return n==0&&GetLastError()!=ERROR_SUCCESS?-1:n;};
  if(!a.pngFormat)a.pngFormat=[] {return RegisterClipboardFormatW(L"PNG");};
  if(!a.htmlFormat)a.htmlFormat=[] {return RegisterClipboardFormatW(L"HTML Format");};
  if(!a.delayed)a.delayed=[](UINT f){SetLastError(ERROR_SUCCESS);SetClipboardData(f,nullptr);return IsClipboardFormatAvailable(f)!=FALSE;};
  if(!a.prepare)a.prepare=preparePendingMedium;
  if(!a.publish)a.publish=[](UINT f,const PendingClipboardMediumPtr& value){auto block=std::dynamic_pointer_cast<NativePendingMedium>(value);if(!block||!block->value||!SetClipboardData(f,block->value))return false;block->value=nullptr;return true;};
  if(!a.adopt)a.adopt=[](HGLOBAL memory){auto out=std::make_shared<NativePendingMedium>();out->value=memory;return out;};
  if(!a.decodeImage)a.decodeImage=clipboard::decodePngToBgra;if(!a.token)a.token=pendingToken;s.access=std::move(a);
}
PendingClipboard::~PendingClipboard(){Shutdown();}
void PendingClipboard::Attach(HWND writer) {
  auto& s=*state_;pendingRequire(writer!=nullptr);s.writer=writer;
  if(s.access.installWindowHook){if(!SetPropW(writer,pendingProperty,this)){s.writer=nullptr;throw std::runtime_error("clipboard_write_failed");}SetLastError(ERROR_SUCCESS);s.previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(writer,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&WindowProc)));if(!s.previous){RemovePropW(writer,pendingProperty);s.writer=nullptr;throw std::runtime_error("clipboard_write_failed");}}
}
void PendingClipboard::Detach(){auto& s=*state_;RenderAll();if(s.writer&&s.previous){SetWindowLongPtrW(s.writer,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(s.previous));RemovePropW(s.writer,pendingProperty);}s.previous=nullptr;s.writer=nullptr;}
void PendingClipboard::SetOwnSequenceSink(std::function<void(DWORD)> sink){std::lock_guard lock(state_->mutex);state_->ownSequence=std::move(sink);}
void PendingClipboard::SetRenderQueue(std::function<bool(std::function<void()>)> queue){std::lock_guard lock(state_->mutex);state_->queue=std::move(queue);}
clipboard::PullRenderPolicy PendingClipboard::DelegatePolicy(const PendingClipboardLease& lease){std::weak_ptr<State> weak=state_;return {true,[weak,lease]{const auto s=weak.lock();if(!s)return false;std::lock_guard lock(s->mutex);return s->Same(lease)&&s->Live(lease)&&s->current->phase!=State::Phase::failed&&s->Current(*s->current)&&(!s->current->waitStarted||std::chrono::steady_clock::now()<s->current->deadline);}};}
void PendingClipboard::BindDelegate(const PendingClipboardLease& lease,const std::string& request,std::shared_ptr<clipboard::PullOffer> offer){auto& s=*state_;std::lock_guard lock(s.mutex);auto r=s.RequireRecord(lease,request);pendingRequire(offer&&offer->id()==request&&offer->format()==lease.kind&&r->phase==State::Phase::bound&&s.Current(*r)&&bool(s.queue),"clipboard_changed");r->delegate=std::move(offer);r->phase=State::Phase::delegated;s.changed.notify_all();}
DWORD PendingClipboard::Observe(const std::string& owner,uint64_t epoch) {
  pendingRequire(pendingId(owner)&&pendingEpoch(epoch));auto& s=*state_;std::unique_lock lock(s.mutex);pendingRequire(!s.stopped,"clipboard_changed");
  auto found=s.Admit(owner);
  pendingRequire(epoch>found->second.closedThrough&&epoch>=found->second.epoch,"clipboard_changed");
  if(epoch>found->second.epoch&&s.current&&s.current->lease.owner==owner&&s.current->phase!=State::Phase::ready)s.Fail(s.current);found->second.epoch=epoch;
  // Its own code (a plain clipboard_busy would say the same for every cause). A never-written clipboard is seeded (State::Seed).
  DWORD sequence=s.access.sequence();if(!sequence)sequence=s.Seed(lock,owner,epoch);
  pendingRequire(sequence!=0,"clipboard_sequence_unavailable");return sequence;
}
std::optional<PendingClipboardLease> PendingClipboard::Begin(const std::string& owner,uint64_t epoch,DWORD expected,DWORD source,const std::string& kind) {
  pendingRequire(pendingId(owner)&&pendingEpoch(epoch)&&(kind=="text"||kind=="richtext"||kind=="image"));auto& s=*state_;PendingClipboardLease lease{s.access.token(),owner,kind,epoch,source,0};pendingRequire(pendingId(lease.token));
  {std::lock_guard lock(s.mutex);if(!s.Live(lease)||!s.writer||!expected)return {};}
  if(!s.access.open(s.writer))return {};struct Close {std::function<void()> close;bool open=true;void now(){if(open){open=false;close();}}~Close(){now();}} close{s.access.close};
  {std::lock_guard lock(s.mutex);if(!s.Live(lease))return {};const DWORD actual=s.access.sequence();bool matches=actual==expected;
    // Only this exact session/epoch's own render may advance its old anchor.
    if(!matches&&s.current&&s.current->lease.owner==owner&&(s.current->lease.epoch==epoch||s.current->phase==State::Phase::ready)&&s.current->ownChain.count(expected)&&s.Current(*s.current))matches=true;
    if(!matches)return {};s.Fail(s.current);}
  // EmptyClipboard can synchronously notify the previous owner. Never hold our
  // state mutex across that foreign callback; cancellation remains independent.
  pendingRequire(s.access.empty(),"clipboard_write_failed");
  std::unique_lock lock(s.mutex);if(!s.Live(lease))return {};
  if(kind=="image"){s.png=s.access.pngFormat();pendingRequire(s.png!=0,"clipboard_write_failed");}
  pendingRequire(s.access.delayed(kind=="image"?s.png:CF_UNICODETEXT),"clipboard_write_failed");
  if(kind=="image"){pendingRequire(s.access.delayed(CF_DIBV5)&&s.access.delayed(CF_DIB),"clipboard_write_failed");}
  if(kind=="richtext"){s.html=s.access.htmlFormat();pendingRequire(s.html&&s.access.delayed(s.html),"clipboard_write_failed");}
  const DWORD written=s.access.sequence();pendingRequire(written!=0&&s.access.owner()==s.writer,"clipboard_changed");
  // The lease names the number readers see, after the close (which can move it). CloseClipboard notifies viewers: not under our mutex.
  lock.unlock();close.now();lock.lock();if(!s.Live(lease))return {};
  lease.sequence=s.access.sequence();pendingRequire(lease.sequence!=0&&s.access.owner()==s.writer,"clipboard_changed");
  auto record=std::make_shared<State::Record>();record->lease=lease;record->latest=lease.sequence;record->ownChain.insert(written);record->ownChain.insert(lease.sequence);s.current=record;if(s.ownSequence)s.ownSequence(lease.sequence);return lease;
}
bool PendingClipboard::Bind(const PendingClipboardLease& lease,const std::string& request){pendingRequire(pendingId(request));auto& s=*state_;std::lock_guard lock(s.mutex);if(!s.Same(lease)||!s.Live(lease)||s.current->phase!=State::Phase::awaiting||!s.Current(*s.current))return false;s.current->request=request;s.current->phase=State::Phase::bound;return true;}
void PendingClipboard::BeginData(const PendingClipboardLease& lease,const std::string& request,const std::string& format,std::vector<clipboard::Item> items){auto& s=*state_;std::shared_ptr<State::Record> r;
  {std::lock_guard lock(s.mutex);r=s.RequireRecord(lease,request);pendingRequire(r->phase==State::Phase::bound&&format==lease.kind&&s.Current(*r),"clipboard_changed");}
  pendingRequire(format=="text"||format=="richtext"||format=="image");
  if(format=="text"){pendingRequire(items.size()==1&&items[0].size<=clipboard::maxTextBytes&&!items[0].width&&!items[0].height,"clipboard_text_too_large");items[0].kind="text";}
  auto incoming=std::make_shared<clipboard::Incoming>(format=="text"?"richtext":format,std::move(items),std::filesystem::path{},nullptr,std::vector<std::filesystem::path>{},clipboard::Bounds::scale());
  std::lock_guard lock(s.mutex);pendingRequire(s.Same(lease)&&s.Live(lease)&&s.current==r&&r->phase==State::Phase::bound&&s.Current(*r),"clipboard_changed");r->incoming=std::move(incoming);r->phase=State::Phase::receiving;
}
void PendingClipboard::Append(const PendingClipboardLease& lease,const std::string& request,size_t index,uint64_t offset,const std::vector<uint8_t>& bytes){auto& s=*state_;std::shared_ptr<State::Record> r;std::shared_ptr<clipboard::Incoming> incoming;
  {std::lock_guard lock(s.mutex);r=s.RequireRecord(lease,request);pendingRequire(r->phase==State::Phase::receiving&&s.Current(*r),"clipboard_changed");incoming=r->incoming;}
  incoming->append(index,offset,bytes);
  std::lock_guard lock(s.mutex);pendingRequire(s.Same(lease)&&s.Live(lease)&&r->phase==State::Phase::receiving&&s.Current(*r),"clipboard_changed");
}
void PendingClipboard::Finish(const PendingClipboardLease& lease,const std::string& request,size_t index,const std::string& digest){auto& s=*state_;std::shared_ptr<State::Record> r;std::shared_ptr<clipboard::Incoming> incoming;
  {std::lock_guard lock(s.mutex);r=s.RequireRecord(lease,request);pendingRequire(r->phase==State::Phase::receiving&&s.Current(*r),"clipboard_changed");incoming=r->incoming;}
  incoming->finish(index,digest);
  std::lock_guard lock(s.mutex);pendingRequire(s.Same(lease)&&s.Live(lease)&&r->phase==State::Phase::receiving&&s.Current(*r),"clipboard_changed");
}
DWORD PendingClipboard::Commit(const PendingClipboardLease& lease,const std::string& request){auto& s=*state_;std::shared_ptr<State::Record> r;std::shared_ptr<clipboard::Incoming> incoming;UINT png=0,html=0;
  {std::lock_guard lock(s.mutex);r=s.RequireRecord(lease,request);if(r->phase==State::Phase::ready&&s.Current(*r))return r->latest;pendingRequire(r->phase==State::Phase::receiving&&s.Current(*r),"clipboard_changed");r->phase=State::Phase::preparing;incoming=r->incoming;png=s.png;html=s.html;}
  try {std::map<UINT,PendingClipboardMediumPtr> prepared;clipboard::Publishers publishers;
    publishers.rich=[&](HWND,const std::wstring& text,const std::vector<uint8_t>* htmlBytes,const std::vector<uint8_t>*,std::optional<DWORD>){prepared.emplace(CF_UNICODETEXT,s.access.prepare(pendingWideText(text)));if(htmlBytes){pendingRequire(lease.kind=="richtext"&&html,"clipboard_transfer_failed");auto bytes=*htmlBytes;bytes.push_back(0);prepared.emplace(html,s.access.prepare(std::move(bytes)));}return DWORD(0);};
    publishers.image=[&](HWND,const clipboard::Image& image,std::optional<DWORD>){uint32_t width=0,height=0;pendingRequire(lease.kind=="image"&&clipboard::readPngHeader(image.png,width,height)&&width==image.width&&height==image.height,"clipboard_image_invalid");auto bgra=s.access.decodeImage(image.png,width,height);prepared.emplace(CF_DIBV5,s.access.prepare(pendingDib(bgra,width,height,true)));prepared.emplace(CF_DIB,s.access.prepare(pendingDib(bgra,width,height,false)));prepared.emplace(png,s.access.prepare(image.png));return DWORD(0);};
    // Both permitted production Incoming callbacks are replaced. Tree/files
    // were refused before construction: no default clipboard/file publisher.
    (void)incoming->commit(nullptr,std::nullopt,&publishers);pendingRequire(!prepared.empty());for(const auto& value:prepared)pendingRequire(value.second!=nullptr,"clipboard_allocation_failed");
    std::lock_guard lock(s.mutex);pendingRequire(s.Same(lease)&&s.Live(lease)&&s.current==r&&r->phase==State::Phase::preparing&&s.Current(*r)&&(!r->waitStarted||std::chrono::steady_clock::now()<r->deadline),"clipboard_changed");r->media=std::move(prepared);r->incoming.reset();r->phase=State::Phase::ready;s.changed.notify_all();return r->latest;
  }catch(...){std::lock_guard lock(s.mutex);s.Fail(r);throw;}
}
void PendingClipboard::Cancel(const PendingClipboardLease& lease){auto& s=*state_;std::lock_guard lock(s.mutex);if(s.Same(lease)&&s.current->phase!=State::Phase::ready)s.Fail(s.current);}
void PendingClipboard::CancelOwner(const std::string& owner,uint64_t epoch){pendingRequire(pendingId(owner)&&pendingEpoch(epoch));auto& s=*state_;std::lock_guard lock(s.mutex);auto found=s.Admit(owner);found->second.closedThrough=(std::max)(found->second.closedThrough,epoch);if(s.current&&s.current->lease.owner==owner&&s.current->lease.epoch<=epoch&&s.current->phase!=State::Phase::ready)s.Fail(s.current);}
void PendingClipboard::Shutdown(){auto& s=*state_;std::lock_guard lock(s.mutex);s.stopped=true;if(s.current&&s.current->phase!=State::Phase::ready)s.Fail(s.current);}
void PendingClipboard::LostOwnership(){auto& s=*state_;std::lock_guard lock(s.mutex);s.Fail(s.current);}
bool PendingClipboard::Unrenderable(){auto& s=*state_;std::lock_guard lock(s.mutex);const auto r=s.current;return r&&s.Current(*r)&&(r->phase==State::Phase::failed||(!s.Live(r->lease)&&r->phase!=State::Phase::ready));}
bool PendingClipboard::Render(UINT format){auto s=state_;std::unique_lock lock(s->mutex);auto r=s->current;if(!r||(!s->Live(r->lease)&&r->phase!=State::Phase::ready)||!s->Current(*r)||r->phase==State::Phase::failed)return false;
  const bool offered=r->lease.kind=="image"?(format==s->png||format==CF_DIBV5||format==CF_DIB):(format==CF_UNICODETEXT||(r->lease.kind=="richtext"&&format==s->html));if(!offered)return false;
  if(!r->waitStarted&&r->phase!=State::Phase::ready){r->waitStarted=true;r->deadline=std::chrono::steady_clock::now()+s->timeout;}
  if(r->phase!=State::Phase::ready&&std::chrono::steady_clock::now()>=r->deadline){s->Fail(r);return false;}
  for(;;){
  if(r->phase==State::Phase::delegated&&!r->media.count(format)&&!r->requested.count(format)){
    r->requested.insert(format);
    auto queue=s->queue;
    auto work=[s,r,format]{
      {std::lock_guard guard(s->mutex);if(s->current!=r||!s->Live(r->lease)||r->phase==State::Phase::failed||!s->Current(*r)||std::chrono::steady_clock::now()>=r->deadline){s->Fail(r);return;}}
      STGMEDIUM medium{};IDataObject* data=r->delegate->dataObject();
      try {FORMATETC requested{CLIPFORMAT(format),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};const HRESULT result=data->GetData(&requested,&medium);data->Release();data=nullptr;
        pendingRequire(SUCCEEDED(result)&&medium.tymed==TYMED_HGLOBAL&&medium.hGlobal&&!medium.pUnkForRelease,"clipboard_transfer_failed");auto ready=s->access.adopt(medium.hGlobal);pendingRequire(ready!=nullptr,"clipboard_allocation_failed");medium.hGlobal=nullptr;medium.tymed=TYMED_NULL;
        std::lock_guard guard(s->mutex);pendingRequire(s->current==r&&s->Live(r->lease)&&r->phase==State::Phase::delegated&&s->Current(*r)&&std::chrono::steady_clock::now()<r->deadline,"clipboard_changed");r->media.emplace(format,std::move(ready));s->changed.notify_all();
      }catch(...){if(data)data->Release();if(medium.tymed!=TYMED_NULL)ReleaseStgMedium(&medium);std::lock_guard guard(s->mutex);s->Fail(r);}
    };
    lock.unlock();const bool queued=queue&&queue(std::move(work));lock.lock();if(!queued){s->Fail(r);return false;}
  }
  if(!s->changed.wait_until(lock,r->deadline,[&]{return r->phase==State::Phase::ready||r->media.count(format)||(r->phase==State::Phase::delegated&&!r->requested.count(format))||r->phase==State::Phase::failed||!s->Live(r->lease)||s->current!=r;})){s->Fail(r);return false;}
  if(r->phase==State::Phase::delegated&&!r->media.count(format)&&!r->requested.count(format))continue;
  break;
  }
  if(s->current!=r||(r->phase!=State::Phase::ready&&r->phase!=State::Phase::delegated)||(!s->Live(r->lease)&&r->phase!=State::Phase::ready)||!s->Current(*r)||(r->phase!=State::Phase::ready&&std::chrono::steady_clock::now()>=r->deadline)){s->Fail(r);return false;}
  const auto found=r->media.find(format);if(found==r->media.end())return false;
  // The delivery worker prepared/adopted this HGLOBAL before waking us. No
  // allocation, decoding, COM pumping, OpenClipboard or network work here.
  if(!s->access.publish(format,found->second)){s->Fail(r);return false;}r->media.erase(found);r->latest=s->access.sequence();if(!r->latest){s->Fail(r);return false;}r->ownChain.insert(r->latest);if(r->phase==State::Phase::delegated)r->waitStarted=false;if(s->ownSequence)s->ownSequence(r->latest);return true;
}
void PendingClipboard::RenderAll(){auto& s=*state_;std::lock_guard lock(s.mutex);auto r=s.current;if(!r||(r->phase!=State::Phase::ready&&r->phase!=State::Phase::delegated)||(!s.Live(r->lease)&&r->phase!=State::Phase::ready)||!s.writer)return;
  // WM_RENDERALLFORMATS never waits for missing bytes and does not pump jobs.
  if(!s.access.open(s.writer))return;struct Close{std::function<void()> close;~Close(){close();}} close{s.access.close};
  if(!s.Current(*r))return;for(auto at=r->media.begin();at!=r->media.end();){if(!s.Current(*r)||!s.access.publish(at->first,at->second)){s.Fail(r);return;}at=r->media.erase(at);r->latest=s.access.sequence();if(!r->latest){s.Fail(r);return;}r->ownChain.insert(r->latest);if(s.ownSequence)s.ownSequence(r->latest);}
}
LRESULT CALLBACK PendingClipboard::WindowProc(HWND window,UINT message,WPARAM w,LPARAM l){auto* self=static_cast<PendingClipboard*>(GetPropW(window,pendingProperty));if(!self)return DefWindowProcW(window,message,w,l);const auto previous=self->state_->previous;
  try {if(message==WM_RENDERFORMAT){self->Render(UINT(w));return 0;}if(message==WM_RENDERALLFORMATS){self->RenderAll();return 0;}if(message==WM_DESTROYCLIPBOARD){self->LostOwnership();return 0;}}catch(...){self->LostOwnership();return 0;}
  return previous?CallWindowProcW(previous,window,message,w,l):DefWindowProcW(window,message,w,l);
}
}
