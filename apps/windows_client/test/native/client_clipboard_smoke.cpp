#include "client_clipboard.h"
#include "clipboard.hpp"
#include <atomic>
#include <thread>
#include <shlobj.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
namespace clip=multitap::clipboard;
using V=flutter::EncodableValue;
using M=flutter::EncodableMap;
namespace {
void requireAt(bool ok,int line){if(!ok)throw std::runtime_error("Client clipboard adapter QA failed at line "+std::to_string(line));}
#define require(ok) requireAt((ok),__LINE__)
template<class F> void rejects(F operation){bool rejected=false;try{operation();}catch(...){rejected=true;}require(rejected);}
class Isolated {
 public:
  Isolated(){
    originalStation=GetProcessWindowStation();originalDesktop=GetThreadDesktop(GetCurrentThreadId());
    const auto name=L"MultitapClientClipboardQA_"+std::to_wstring(GetCurrentProcessId());
    station=CreateWindowStationW(name.c_str(),0,WINSTA_ALL_ACCESS,nullptr);require(station&&SetProcessWindowStation(station));
    desktop=CreateDesktopW(L"ClipboardOnly",nullptr,nullptr,0,GENERIC_ALL,nullptr);require(desktop&&SetThreadDesktop(desktop));
    wchar_t actual[256]{};DWORD length=0;require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_NAME,actual,sizeof(actual),&length)&&name==actual&&station!=originalStation);
    owner=CreateWindowExW(0,L"STATIC",L"Own clipboard only",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);require(owner!=nullptr);
  }
  ~Isolated(){if(owner)DestroyWindow(owner);SetThreadDesktop(originalDesktop);SetProcessWindowStation(originalStation);if(desktop)CloseDesktop(desktop);if(station)CloseWindowStation(station);}
  HWND owner=nullptr;
 private:HWINSTA station=nullptr,originalStation=nullptr;HDESK desktop=nullptr,originalDesktop=nullptr;
};
void write(const fs::path& path,const std::vector<uint8_t>& bytes){std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));require(bool(file));}
// On-paste captures announce even tiny files; reading their bytes is a separate
// serving call. This uses a substituted clipboard and never calls OleSetClipboard.
void onPasteCapture(const fs::path& root){
  const auto path=root/L"onpaste.bin";write(path,{1,2,3});
  clip::Snapshot board;board.sequence=41;board.owner=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x20));board.files={path};
  multitap::ClientClipboardAccess access;
  access.read=[&](HWND,bool,bool){return board;};access.readFormats=[&](HWND,const clip::ReadOptions&){return board;};
  bool changeDuringAdmission=false;unsigned admissionProbes=0;
  access.sequence=[&]{if(changeDuringAdmission&&++admissionProbes==2)++board.sequence;return board.sequence;};access.owner=[&]{return board.owner;};
  const HWND mine=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x10));
  multitap::ClientClipboardState state(root/L"receive-onpaste",access);
  const auto call=[&](const char* method,M fields=M{}){fields[V("owner")]=V(std::string("qa-onpaste"));return state.Run(method,fields,mine);};
  const auto serve=[&](const char* method,M fields=M{}){fields[V("owner")]=V(std::string("qa-onpaste"));return state.Serve(method,fields);};
  M fields{{V("formats"),V(true)},{V("pull"),V(true)},{V("offerId"),V(std::string("qa-small"))}};
  require(std::get<M>(call("capture",fields)).count(V("offer"))==0);call("release");
  fields[V("onPaste")]=V(true);
  const auto captured=std::get<M>(call("capture",fields));
  require(std::get<bool>(captured.at(V("offer")))&&std::get<std::string>(captured.at(V("format")))=="tree");
  require(state.TakePullEvents().empty());call("release");
  require(std::get<int64_t>(serve("pullOpen",M{{V("offerId"),V(std::string("qa-small"))},{V("index"),V(int64_t(0))},{V("offset"),V(int64_t(0))}}))==3);
  require(std::get<std::vector<uint8_t>>(serve("pullRead",M{{V("length"),V(int32_t(1))}}))==std::vector<uint8_t>({1}));
  // Metadata changes before the watcher/debounce/new capture runs. A new paste
  // cannot reopen the old offer; the already admitted range remains readable.
  ++board.sequence;
  rejects([&]{serve("pullOpen",M{{V("offerId"),V(std::string("qa-small"))},{V("index"),V(int64_t(0))},{V("offset"),V(int64_t(0))}});});
  // One call reads up to a block of chunks (one platform round trip per ~1 MiB): the file's end bounds it, the block caps it.
  rejects([&]{serve("pullRead",M{{V("length"),V(int32_t(clip::formatsChunkBytes*clip::readAheadChunks+1))}});});
  require(std::get<std::vector<uint8_t>>(serve("pullRead",M{{V("length"),V(int32_t(3*clip::formatsChunkBytes))}}))==std::vector<uint8_t>({2,3}));serve("pullClose");
  board.files.clear();board.text=L"small text";++board.sequence;
  require(std::get<M>(call("capture",fields)).count(V("offer"))==0);call("release");
  board.text=std::wstring(1024*1024+1,L'a');++board.sequence;fields[V("offerId")]=V(std::string("qa-text"));
  const auto content=std::get<M>(call("capture",fields));
  require(std::get<bool>(content.at(V("offer")))&&std::get<std::string>(content.at(V("format")))=="text");
  // A copy between the two admission samples must not return a stale Reader.
  admissionProbes=0;changeDuringAdmission=true;
  rejects([&]{serve("pullOpen",M{{V("offerId"),V(std::string("qa-text"))},{V("index"),V(int64_t(0))},{V("offset"),V(int64_t(0))}});});
  require(admissionProbes==2);changeDuringAdmission=false;
  call("release");call("closeOffers");fs::remove(path);
}
// Explorer may put the received list back under its own ownership after a
// paste: automatic sharing still treats exactly that list as the peer's. Runs
// on a substituted clipboard (no window station needed).
void pasteKeepsPeerContent(const fs::path& root,const std::vector<fs::path>& sent){
  clip::Snapshot board;multitap::ClientClipboardAccess access;
  const HWND mine=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x10)),explorer=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x20));
  const auto put=[&](HWND window,const std::vector<fs::path>& files){board=clip::Snapshot{};board.owner=window;board.files=files;return ++board.sequence;};
  access.read=[&](HWND,bool,bool){return board;};access.sequence=[&]{return board.sequence;};access.owner=[&]{return board.owner;};
  access.putFiles=[&](HWND window,const std::vector<fs::path>& files,std::optional<DWORD>){return put(window,files);};
  access.checkPublished=[](const std::vector<fs::path>&){return multitap::PublishCheck{true,0,0};};
  multitap::ClientClipboardState state(root/L"receive-paste",access);const std::string scope="qa-paste-scope";
  auto call=[&](const char* method,M fields=M{}){fields[V("owner")]=V(scope);return state.Run(method,fields,mine);};
  const M request{{V("requestId"),V(std::string("qa-paste"))}},automatic{{V("skipOwn"),V(true)}};
  put(explorer,sent);const auto captured=std::get<M>(call("capture"));const auto items=std::get<flutter::EncodableList>(captured.at(V("items")));
  M begin=request;begin[V("format")]=V(std::string("files"));begin[V("items")]=V(items);call("begin",begin);
  for(size_t index=0;index<items.size();index++){
    uint64_t offset=0;const auto size=static_cast<uint64_t>(std::get<int64_t>(std::get<M>(items[index]).at(V("size"))));
    while(offset<size){M source{{V("index"),V(static_cast<int64_t>(index))},{V("offset"),V(static_cast<int64_t>(offset))},{V("length"),V(int32_t(32768))}};
      const auto chunk=std::get<std::vector<uint8_t>>(call("sourceChunk",source));M append=request;append[V("index")]=V(static_cast<int64_t>(index));append[V("offset")]=V(static_cast<int64_t>(offset));append[V("bytes")]=V(chunk);call("append",append);offset+=chunk.size();}
    M finish=request;finish[V("index")]=V(static_cast<int64_t>(index));finish[V("digest")]=call("sourceDigest",{{V("index"),V(static_cast<int64_t>(index))}});call("finish",finish);
  }
  call("commit",request);call("release");const auto received=board.files;require(board.owner==mine&&received.size()==sent.size());
  put(explorer,received);require(std::get<M>(call("capture",automatic)).count(V("own"))==1);call("release");
  put(explorer,{received[0]});require(std::get<M>(call("capture",automatic)).count(V("own"))==0);call("release");
  // A manual send is the user's explicit choice: always the content itself.
  put(explorer,received);require(std::get<M>(call("capture")).count(V("own"))==0);call("release");
  require(clip::expireFiles(root/L"receive-paste",received));
  for(const auto& entry:fs::directory_iterator(root/L"receive-paste")){require(entry.is_regular_file()&&entry.path().filename().wstring().rfind(L"completed-",0)==0);fs::remove(entry.path());}
  fs::remove(root/L"receive-paste");
}
// A copy made in the VM that is still arriving claims this PC's clipboard (emptied, ours) so that a paste made meanwhile finds nothing
// instead of the older content; the arriving content replaces it. Best effort and only for content that takes long: files, trees, images.
void claimBeforeCommit(const fs::path& root){
  clip::Snapshot board;multitap::ClientClipboardAccess access;DWORD counter=900;std::optional<DWORD> publishedExpected;bool refuse=false;
  const HWND mine=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x10)),user=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x20));
  const auto set=[&](HWND window){board=clip::Snapshot{};board.owner=window;return board.sequence=++counter;};
  access.read=[&](HWND,bool,bool){return board;};access.readFormats=[&](HWND,const clip::ReadOptions&){return board;};
  access.sequence=[&]{return board.sequence;};access.owner=[&]{return board.owner;};
  access.claim=[&](HWND window,std::optional<DWORD> expected){if(refuse)throw std::runtime_error("clipboard_busy");if(expected&&*expected!=board.sequence)throw std::runtime_error("clipboard_changed");return set(window);};
  access.putText=[&](HWND window,const std::wstring& text,std::optional<DWORD> expected){publishedExpected=expected;const auto sequence=set(window);board.text=text;return sequence;};
  access.putFiles=[&](HWND window,const std::vector<fs::path>& files,std::optional<DWORD> expected){publishedExpected=expected;const auto sequence=set(window);board.files=files;return sequence;};
  access.checkPublished=[](const std::vector<fs::path>&){return multitap::PublishCheck{true,0,0};};
  multitap::ClientClipboardState state(root/L"receive-claim",access);const std::string scope="qa-claim-scope";
  auto call=[&](const char* method,M fields=M{}){fields[V("owner")]=V(scope);return state.Run(method,fields,mine);};
  const M request{{V("requestId"),V(std::string("qa-claim"))}};
  const auto claimed=[&]{return std::get<bool>(std::get<M>(call("claim",request)).at(V("claimed")));};
  const auto beginFiles=[&]{
    M begin=request;begin[V("format")]=V(std::string("files"));
    begin[V("items")]=V(flutter::EncodableList{V(M{{V("name"),V(std::string("a.bin"))},{V("size"),V(int64_t(3))}})});call("begin",begin);
  };
  const auto finishFiles=[&]{
    M append=request;append[V("index")]=V(int64_t(0));append[V("offset")]=V(int64_t(0));append[V("bytes")]=V(std::vector<uint8_t>{1,2,3});call("append",append);
    M finish=request;finish[V("index")]=V(int64_t(0));
    finish[V("digest")]=V(std::string("039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81"));call("finish",finish);
  };
  // 1. Old content on the clipboard; files start arriving; the claim empties it; the commit publishes over the claim.
  set(user);board.text=L"older text";publishedExpected.reset();
  beginFiles();
  require(claimed());
  require(board.owner==mine&&!board.text&&board.files.empty());
  const DWORD claimedSequence=board.sequence;
  finishFiles();call("commit",request);
  require(publishedExpected&&*publishedExpected==claimedSequence);   // the publish checks the clipboard is still the claim
  require(board.files.size()==1&&board.owner==mine);call("release");
  // 2. The user copied something on this PC after the transfer began: nothing is claimed and that copy stays.
  set(user);board.text=L"older text";beginFiles();
  set(user);board.text=L"copied meanwhile";
  require(!claimed());require(board.text&&*board.text==L"copied meanwhile"&&board.owner==user);
  call("release");
  // 3. The platform refuses (busy): the transfer is not affected, the old content stays.
  set(user);board.text=L"older text";beginFiles();refuse=true;
  require(!claimed());require(board.text&&*board.text==L"older text");refuse=false;call("release");
  // 4. Text arrives in one piece: never claimed.
  set(user);board.text=L"older text";
  M text=request;text[V("format")]=V(std::string("text"));text[V("items")]=V(flutter::EncodableList{V(M{{V("size"),V(int64_t(3))}})});call("begin",text);
  require(!claimed());require(board.text&&*board.text==L"older text");call("release");
  // 5. A claim with no transfer in progress is refused as a request error, never as a silent success.
  rejects([&]{call("claim",request);});
  for(const auto& entry:fs::directory_iterator(root/L"receive-claim"))fs::remove_all(entry.path());
  fs::remove(root/L"receive-claim");
}// Plain text and flat file lists are not the formats "Outgoing"/"Incoming" objects, but a formats pair moves them in the same
// 45 KiB chunks as everything else (a stop-and-wait pair in 32 KiB). An adapter that refuses 45 KiB for both fails every
// file above 32 KiB and every text above 32 KiB as clipboard_transfer_failed (found by tools/clipboard-e2e-qa).
void flatChunkSizes(const fs::path& root){
  clip::Snapshot board;multitap::ClientClipboardAccess access;DWORD counter=700;
  const HWND mine=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x10));
  access.read=[&](HWND,bool,bool){return board;};access.readFormats=[&](HWND,const clip::ReadOptions&){return board;};
  access.sequence=[&]{return board.sequence;};access.owner=[&]{return board.owner;};
  access.putText=[&](HWND window,const std::wstring& text,std::optional<DWORD>){board=clip::Snapshot{};board.owner=window;board.text=text;return board.sequence=++counter;};
  access.putFiles=[&](HWND window,const std::vector<fs::path>& files,std::optional<DWORD>){board=clip::Snapshot{};board.owner=window;board.files=files;return board.sequence=++counter;};
  access.checkPublished=[](const std::vector<fs::path>&){return multitap::PublishCheck{true,0,0};};
  multitap::ClientClipboardState state(root/L"receive-flat",access);const std::string scope="qa-flat-scope";
  auto call=[&](const char* method,M fields=M{}){fields[V("owner")]=V(scope);return state.Run(method,fields,mine);};
  const M request{{V("requestId"),V(std::string("qa-flat"))}};
  // The captured content goes to the receiving side of the same adapter in formats-sized chunks (sourceChunk -> append).
  const auto transfer=[&](const char* expectedFormat){
    const M wantFormats{{V("formats"),V(true)}};
    const auto captured=std::get<M>(call("capture",wantFormats));require(std::get<std::string>(captured.at(V("format")))==expectedFormat);
    const auto items=std::get<flutter::EncodableList>(captured.at(V("items")));
    M begin=request;begin[V("format")]=V(std::string(expectedFormat));begin[V("items")]=V(items);call("begin",begin);
    for(size_t index=0;index<items.size();index++){
      uint64_t offset=0;const auto size=static_cast<uint64_t>(std::get<int64_t>(std::get<M>(items[index]).at(V("size"))));
      while(offset<size){
        const auto length=static_cast<int32_t>(std::min<uint64_t>(clip::formatsChunkBytes,size-offset));
        M source{{V("index"),V(static_cast<int64_t>(index))},{V("offset"),V(static_cast<int64_t>(offset))},{V("length"),V(length)}};
        const auto chunk=std::get<std::vector<uint8_t>>(call("sourceChunk",source));require(chunk.size()==static_cast<size_t>(length));
        M append=request;append[V("index")]=V(static_cast<int64_t>(index));append[V("offset")]=V(static_cast<int64_t>(offset));append[V("bytes")]=V(chunk);call("append",append);offset+=chunk.size();
      }
      M finish=request;finish[V("index")]=V(static_cast<int64_t>(index));finish[V("digest")]=call("sourceDigest",{{V("index"),V(static_cast<int64_t>(index))}});call("finish",finish);
    }
    call("commit",request);call("release");
  };
  const std::wstring text(100000,L'x');
  board=clip::Snapshot{};board.owner=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x20));board.text=text;board.sequence=1;
  transfer("text");require(board.text==text&&board.owner==mine);
  std::vector<uint8_t> first(100000),second(clip::formatsChunkBytes+1);
  for(size_t i=0;i<first.size();++i)first[i]=uint8_t(i*7);for(size_t i=0;i<second.size();++i)second[i]=uint8_t(i*13+1);
  fs::create_directories(root/L"flat-source");write(root/L"flat-source"/L"a.bin",first);write(root/L"flat-source"/L"b.bin",second);
  board=clip::Snapshot{};board.owner=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x20));board.files={root/L"flat-source"/L"a.bin",root/L"flat-source"/L"b.bin"};board.sequence=2;
  transfer("files");require(board.files.size()==2&&board.owner==mine);
  for(size_t i=0;i<2;++i){clip::FileSource received(board.files[i]);std::vector<uint8_t> got;for(;;){const auto part=received.next();if(part.empty())break;got.insert(got.end(),part.begin(),part.end());}require(got==(i==0?first:second));}
  // A chunk above the formats size is still refused (the bound moved to 45 KiB, it did not disappear).
  board=clip::Snapshot{};board.owner=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x20));board.files={root/L"flat-source"/L"a.bin"};board.sequence=3;
  call("capture",M{{V("formats"),V(true)}});
  rejects([&]{call("sourceChunk",M{{V("index"),V(int64_t(0))},{V("offset"),V(int64_t(0))},{V("length"),V(int32_t(clip::formatsChunkBytes+1))}});});
  call("release");
  const auto expired=board.files;(void)expired;
  fs::remove_all(root/L"flat-source");
  for(const auto& entry:fs::directory_iterator(root/L"receive-flat"))fs::remove_all(entry.path());
  fs::remove(root/L"receive-flat");
}// The formats profile through the production adapter on a substituted clipboard (no window station):
// rich text, an image and a folder tree captured, streamed in 45 KiB chunks and published whole.
void formatsRoundTrip(const fs::path& root){
  clip::Snapshot board;multitap::ClientClipboardAccess access;DWORD counter=900;
  const HWND mine=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x10)),explorer=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x20));
  access.read=[&](HWND,bool,bool){return board;};access.readFormats=[&](HWND,const clip::ReadOptions&){return board;};
  access.sequence=[&]{return board.sequence;};access.owner=[&]{return board.owner;};
  access.publishers.rich=[&](HWND window,const std::wstring& text,const std::vector<uint8_t>* html,const std::vector<uint8_t>* rtf,std::optional<DWORD>){board=clip::Snapshot{};board.owner=window;board.text=text;if(html)board.html=*html;if(rtf)board.rtf=*rtf;return board.sequence=++counter;};
  access.publishers.image=[&](HWND window,const clip::Image& image,std::optional<DWORD>){board=clip::Snapshot{};board.owner=window;board.image=image;return board.sequence=++counter;};
  access.publishers.paths=[&](HWND window,const std::vector<fs::path>& paths,std::optional<DWORD>){board=clip::Snapshot{};board.owner=window;board.files=paths;return board.sequence=++counter;};
  multitap::ClientClipboardState state(root/L"receive-formats",access);const std::string scope="qa-formats-scope";
  auto call=[&](const char* method,M fields=M{}){fields[V("owner")]=V(scope);return state.Run(method,fields,mine);};
  const M request{{V("requestId"),V(std::string("qa-formats"))}},wantFormats{{V("formats"),V(true)}};
  const auto transfer=[&](const M& captured){
    const auto format=std::get<std::string>(captured.at(V("format")));const auto items=std::get<flutter::EncodableList>(captured.at(V("items")));
    M begin=request;begin[V("format")]=V(format);begin[V("items")]=V(items);call("begin",begin);
    int64_t ordinal=0;
    for(const auto& value:items){
      const auto& item=std::get<M>(value);const auto kind=item.find(V("kind"));
      if(kind!=item.end()&&std::get<std::string>(kind->second)=="dir")continue;  // folders carry no data
      const auto size=static_cast<uint64_t>(std::get<int64_t>(item.at(V("size"))));uint64_t offset=0;
      while(offset<size){M source{{V("index"),V(ordinal)},{V("offset"),V(static_cast<int64_t>(offset))},{V("length"),V(int32_t(46080))}};
        const auto chunk=std::get<std::vector<uint8_t>>(call("sourceChunk",source));require(!chunk.empty()&&chunk.size()<=46080);
        M append=request;append[V("index")]=V(ordinal);append[V("offset")]=V(static_cast<int64_t>(offset));append[V("bytes")]=V(chunk);call("append",append);offset+=chunk.size();}
      M finish=request;finish[V("index")]=V(ordinal);finish[V("digest")]=call("sourceDigest",{{V("index"),V(ordinal)}});call("finish",finish);++ordinal;
    }
    call("commit",request);call("release");
  };
  const auto htmlFormat=[](const std::string& fragment){
    const std::string prefix="<html><body><!--StartFragment-->",suffix="<!--EndFragment--></body></html>";
    const auto header=[&](size_t a,size_t b,size_t c,size_t d){char buffer[256];snprintf(buffer,sizeof(buffer),"Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n",a,b,c,d);return std::string(buffer);};
    const size_t h=header(0,0,0,0).size(),start=h+prefix.size(),end=start+fragment.size();return header(h,end+suffix.size(),start,end)+prefix+fragment+suffix;};
  // Rich text.
  const std::string html=htmlFormat("<b>한글</b>"),rtf="{\\rtf1 x}";
  board=clip::Snapshot{};board.owner=explorer;board.sequence=++counter;board.text=L"한글 rich";board.html=std::vector<uint8_t>(html.begin(),html.end());board.rtf=std::vector<uint8_t>(rtf.begin(),rtf.end());
  const auto richCapture=std::get<M>(call("capture",wantFormats));require(std::get<std::string>(richCapture.at(V("format")))=="richtext");require(std::get<flutter::EncodableList>(richCapture.at(V("items"))).size()==2);  // text + html; RTF is never sent
  const auto rich=board;transfer(richCapture);
  require(board.owner==mine&&board.text==rich.text&&board.html==clip::sanitizeHtmlFormat(*rich.html)&&!board.rtf);
  require(std::get<M>(call("capture",M{{V("skipOwn"),V(true)},{V("formats"),V(true)}})).count(V("own"))==1);call("release");
  // Without the profile the same clipboard is plain text only.
  board=rich;board.owner=explorer;require(std::get<std::string>(std::get<M>(call("capture")).at(V("format")))=="text");call("release");
  // An image.
  std::vector<uint8_t> png(120000);for(size_t i=0;i<png.size();++i)png[i]=uint8_t(i*7);png[0]=0x89;png[1]='P';png[2]='N';png[3]='G';
  board=clip::Snapshot{};board.owner=explorer;board.sequence=++counter;board.image=clip::Image{png,200,100};
  const auto imageCapture=std::get<M>(call("capture",wantFormats));require(std::get<std::string>(imageCapture.at(V("format")))=="image");
  {const auto items=std::get<flutter::EncodableList>(imageCapture.at(V("items")));const auto& item=std::get<M>(items.at(0));require(std::get<int64_t>(item.at(V("width")))==200&&std::get<int64_t>(item.at(V("height")))==100&&std::get<int64_t>(item.at(V("size")))==120000);}
  transfer(imageCapture);require(board.owner==mine&&board.image&&board.image->png==png&&board.image->width==200&&board.image->height==100);
  // The same clipboard without the profile is not text or files.
  board=clip::Snapshot{};board.owner=explorer;board.image=clip::Image{png,200,100};rejects([&]{call("capture");});call("release");
  // A folder tree, including an empty folder; a link is not part of it (none created here: the scan counts them natively).
  const auto source=root/L"tree-source";fs::create_directories(source/L"Docs"/L"Empty");fs::create_directories(source/L"Docs"/L"Sub");
  std::vector<uint8_t> big(100001);for(size_t i=0;i<big.size();++i)big[i]=uint8_t(i*13);write(source/L"Docs"/L"big.bin",big);write(source/L"Docs"/L"Sub"/L"한글.txt",{'h','i'});write(source/L"loose.txt",{'l'});
  board=clip::Snapshot{};board.owner=explorer;board.sequence=++counter;board.files={source/L"Docs",source/L"loose.txt"};
  rejects([&]{call("capture");});call("release");  // a folder is refused without the profile
  const auto treeCapture=std::get<M>(call("capture",wantFormats));require(std::get<std::string>(treeCapture.at(V("format")))=="tree"&&std::get<int64_t>(treeCapture.at(V("tops")))==2&&std::get<int64_t>(treeCapture.at(V("skipped")))==0);
  transfer(treeCapture);
  require(board.owner==mine&&board.files.size()==2);
  const auto docs=board.files[0].filename()==L"Docs"?board.files[0]:board.files[1];
  require(fs::is_directory(docs/L"Empty")&&fs::is_directory(docs/L"Sub")&&fs::file_size(docs/L"big.bin")==big.size()&&fs::file_size(docs/L"Sub"/L"한글.txt")==2);
  {std::ifstream in(docs/L"big.bin",std::ios::binary);std::vector<uint8_t> got((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());require(got==big);}
  require(fs::exists(docs/L"big.bin:Zone.Identifier"));
  require(std::get<M>(call("capture",M{{V("skipOwn"),V(true)},{V("formats"),V(true)}})).count(V("own"))==1);call("release");  // our own published tree is not sent back
  // A hostile tree path is refused before anything is created.
  M hostile=request;hostile[V("format")]=V(std::string("tree"));hostile[V("items")]=V(flutter::EncodableList{V(M{{V("size"),V(int64_t(1))},{V("kind"),V(std::string("file"))},{V("name"),V(std::string("../evil"))}})});
  rejects([&]{call("begin",hostile);});call("release");
  fs::remove_all(root/L"receive-formats");fs::remove_all(source);
}
// The paste-pull profile through the production adapter: a large copy is captured as an offer (a small one is not),
// the peer-side adapter publishes it as virtual files on the OLE clipboard of a private window station, and reading a
// FileContents stream pulls the bytes through the adapters' pull API (the session's job, played here by a thread).
void offerRoundTrip(const fs::path& root){
  const auto source=root/L"offer-source";fs::create_directories(source/L"Many");
  std::vector<uint8_t> first(250000);for(size_t i=0;i<first.size();++i)first[i]=uint8_t(i*17);write(source/L"Many"/L"a_first.bin",first);
  for(int i=0;i<300;++i)write(source/L"Many"/(L"f"+std::to_wstring(i)+L".txt"),{uint8_t('a'+i%20)});
  clip::Snapshot board;multitap::ClientClipboardAccess access;
  access.read=[&](HWND,bool,bool){return board;};access.readFormats=[&](HWND,const clip::ReadOptions&){return board;};access.sequence=[&]{return board.sequence;};access.owner=[&]{return board.owner;};
  const HWND mine=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x10)),explorer=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x20));
  multitap::ClientClipboardState offering(root/L"offer-state",access);const std::string scope="qa-offer-scope";
  const auto offer=[&](const char* method,M fields=M{}){fields[V("owner")]=V(scope);return offering.Run(method,fields,mine);};
  const auto serve=[&](const char* method,M fields=M{}){fields[V("owner")]=V(scope);return offering.Serve(method,fields);};
  const M request{{V("formats"),V(true)},{V("pull"),V(true)},{V("offerId"),V(std::string("qa-offer"))}};
  // A small folder stays on the push model (CF_HDROP can be published for any application); a large one is offered.
  board=clip::Snapshot{};board.owner=explorer;board.sequence=5;board.files={source/L"Many"/L"a_first.bin",source/L"Many"/L"f0.txt"};
  require(std::get<M>(offer("capture",request)).count(V("offer"))==0);offer("release");
  board.files={source/L"Many"};
  const auto captured=std::get<M>(offer("capture",request));
  require(std::get<bool>(captured.at(V("offer")))&&std::get<std::string>(captured.at(V("format")))=="tree"&&std::get<int64_t>(captured.at(V("tops")))==1);
  const auto items=std::get<flutter::EncodableList>(captured.at(V("items")));require(items.size()==302);
  for(const auto& value:items){const auto& item=std::get<M>(value);require(item.count(V("mtime"))==1&&std::get<int64_t>(item.at(V("mtime")))>1000000000000&&item.count(V("attrs"))==1);}
  offer("release");  // the end of the transfer keeps the offer
  // The peer's adapter, on the real OLE clipboard of a private window station.
  // The OLE clipboard of the current desktop: this REPLACES the interactive clipboard (set MULTITAP_CLIPBOARD_QA_CURRENT_DESKTOP=1).
  struct Station { HWND owner = nullptr; Station() { owner = CreateWindowExW(0, L"STATIC", L"Multitap client pull QA", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr); require(owner != nullptr); } ~Station() { if (owner) DestroyWindow(owner); } } station;
  require(SUCCEEDED(OleInitialize(nullptr)));
  {
    multitap::ClientClipboardState receiving(root/L"offer-receive");std::atomic<bool> stop{false};std::atomic<size_t> served{0},acks{0};
    const auto receive=[&](const char* method,M fields=M{}){fields[V("owner")]=V(scope);return receiving.Run(method,fields,station.owner);};
    std::atomic<DWORD> ownSequence{0};receiving.SetOwnSequenceSink(&ownSequence);
    const auto published=std::get<M>(receive("publishOffer",M{{V("offerId"),V(std::string("qa-offer"))},{V("items"),V(items)}}));
    require(std::get<int64_t>(published.at(V("sequence")))==int64_t(ownSequence.load())&&ownSequence.load()!=0);
    require(!IsClipboardFormatAvailable(CF_HDROP)&&IsClipboardFormatAvailable(RegisterClipboardFormatW(L"FileGroupDescriptorW")));
    require(served==0&&receiving.TakePullEvents().empty());  // publishing asks for nothing
    // Our own publication is never captured for the peer again.
    require(std::get<M>(receive("capture",M{{V("skipOwn"),V(true)},{V("formats"),V(true)}})).count(V("own"))==1);receive("release");
    std::thread session([&]{
      while(!stop){
        for(const auto& event:receiving.TakePullEvents()){
          if(event.kind=="ack"){++acks;continue;}
          if(event.kind!="request")continue;
          try{
            const auto size=static_cast<uint64_t>(std::get<int64_t>(serve("pullOpen",M{{V("offerId"),V(event.offerId)},{V("index"),V(int64_t(event.index))},{V("offset"),V(int64_t(event.offset))}})));
            for(uint64_t at=event.offset;at<size;){
              const auto count=std::min<uint64_t>(46080,size-at);
              const auto chunk=std::get<std::vector<uint8_t>>(serve("pullRead",M{{V("length"),V(int32_t(count))}}));
              receiving.Pull("pullData",M{{V("rangeId"),V(int64_t(event.rangeId))},{V("offset"),V(int64_t(at))},{V("bytes"),V(chunk)}});at+=count;served+=count;
            }
            const auto digest=std::get<std::string>(serve("pullDigest"));serve("pullClose");
            receiving.Pull("pullEnd",M{{V("rangeId"),V(int64_t(event.rangeId))},{V("digest"),V(digest)}});
          }catch(const std::exception&){receiving.Pull("pullFail",M{{V("rangeId"),V(int64_t(event.rangeId))},{V("reason"),V(std::string("clipboard_selected_file_changed"))}});}
        }
        Sleep(2);
      }
    });
    struct Joiner { std::atomic<bool>& stop; std::thread& thread; ~Joiner() { stop = true; if (thread.joinable()) thread.join(); } } joiner{stop, session};
    IDataObject* data=nullptr;require(SUCCEEDED(OleGetClipboard(&data))&&data);
    FORMATETC descriptor{CLIPFORMAT(RegisterClipboardFormatW(L"FileGroupDescriptorW")),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};STGMEDIUM medium{};require(SUCCEEDED(data->GetData(&descriptor,&medium)));
    const auto* group=static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));require(group->cItems==302);
    LONG firstIndex=-1;for(UINT i=0;i<group->cItems;++i)if(std::wstring(group->fgd[i].cFileName)==L"Many\\a_first.bin")firstIndex=LONG(i);
    GlobalUnlock(medium.hGlobal);ReleaseStgMedium(&medium);require(firstIndex>=0);
    FORMATETC contents{CLIPFORMAT(RegisterClipboardFormatW(L"FileContents")),nullptr,DVASPECT_CONTENT,firstIndex,TYMED_ISTREAM};STGMEDIUM stream{};require(SUCCEEDED(data->GetData(&contents,&stream))&&stream.pstm);
    std::vector<uint8_t> got(first.size());size_t at=0;
    while(at<got.size()){ULONG count=0;require(SUCCEEDED(stream.pstm->Read(got.data()+at,ULONG(got.size()-at),&count))&&count>0);at+=count;}
    require(got==first);ULONG more=0;uint8_t one=0;require(SUCCEEDED(stream.pstm->Read(&one,1,&more))&&more==0);
    for(int i=0;i<200&&acks==0;++i)Sleep(5);require(acks>0);  // consumption produced cumulative acknowledgements
    // A file changed since the copy fails its stream.
    write(source/L"Many"/L"f1.txt",{'z','z'});
    LONG changedIndex=-1;group=nullptr;require(SUCCEEDED(data->GetData(&descriptor,&medium)));group=static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
    for(UINT i=0;i<group->cItems;++i)if(std::wstring(group->fgd[i].cFileName)==L"Many\\f1.txt")changedIndex=LONG(i);
    GlobalUnlock(medium.hGlobal);ReleaseStgMedium(&medium);require(changedIndex>=0);
    FORMATETC changed{contents.cfFormat,nullptr,DVASPECT_CONTENT,changedIndex,TYMED_ISTREAM};STGMEDIUM bad{};require(SUCCEEDED(data->GetData(&changed,&bad)));
    ULONG none=0;require(FAILED(bad.pstm->Read(&one,1,&none)));
    bad.pstm->Release();stream.pstm->Release();data->Release();
    // The session ends: the published files leave the clipboard.
    receiving.Pull("pullShutdown",M{{V("reason"),V(std::string("clipboard_transport_closed"))}});receive("closeOffers");
    require(!IsClipboardFormatAvailable(RegisterClipboardFormatW(L"FileGroupDescriptorW")));
  }
  offer("closeOffers");rejects([&]{serve("pullOpen",M{{V("offerId"),V(std::string("qa-offer"))},{V("index"),V(int64_t(0))},{V("offset"),V(int64_t(0))}});});
  OleUninitialize();fs::remove_all(root/L"offer-state");fs::remove_all(root/L"offer-receive");fs::remove_all(source);
}
}
int wmain(int argc,wchar_t** argv){try{
  require(argc==2);const auto root=fs::absolute(argv[1]);require(root.filename().wstring().rfind(L"run-",0)==0&&!fs::exists(root));fs::create_directories(root);
  std::vector<uint8_t> bytes(150123);for(size_t i=0;i<bytes.size();i++)bytes[i]=static_cast<uint8_t>(i*31%251);write(root/L"한글 파일.bin",bytes);write(root/L"empty.txt",{});
  pasteKeepsPeerContent(root,{root/L"한글 파일.bin",root/L"empty.txt"});
  formatsRoundTrip(root);
  flatChunkSizes(root);
  claimBeforeCommit(root);
  onPasteCapture(root);
  // MULTITAP_CLIPBOARD_QA_SUBSTITUTED_ONLY=1: the scenarios above run on substituted clipboards, so a normal user (no window-station
  // privilege) can run them without touching the real clipboard.
  { wchar_t flag[8]{}; if (GetEnvironmentVariableW(L"MULTITAP_CLIPBOARD_QA_SUBSTITUTED_ONLY", flag, 8) && flag[0] == L'1') { for (const auto& entry : fs::directory_iterator(root)) if (entry.path().filename().wstring().rfind(L"receive", 0) == 0) fs::remove_all(entry.path()); fs::remove_all(root); std::cout << "{\"passed\":true,\"substitutedClipboardsOnly\":true,\"flatChunkSizes\":true}\n"; return 0; } }
  { wchar_t flag[8]{}; if (GetEnvironmentVariableW(L"MULTITAP_CLIPBOARD_QA_CURRENT_DESKTOP", flag, 8) && flag[0] == L'1') { offerRoundTrip(root); fs::remove_all(root); std::cout << "{\"passed\":true,\"pasteExtractionAdapter\":true,\"currentDesktopOnly\":true}\n"; return 0; } }
  std::vector<fs::path> received;
  {
    Isolated own;multitap::ClientClipboardState state(root/L"receive");
    const std::string scope="qa-client-scope",id="qa-request";
    auto call=[&](const char* method,M fields=M{}){fields[V("owner")]=V(scope);return state.Run(method,fields,own.owner);};
    const M request{{V("requestId"),V(id)}};
    const auto capture=[&]{return std::get<M>(call("capture"));};
    const auto transfer=[&](const M& captured){
      const auto format=std::get<std::string>(captured.at(V("format")));const auto items=std::get<flutter::EncodableList>(captured.at(V("items")));
      M begin=request;begin[V("format")]=V(format);begin[V("items")]=V(items);call("begin",begin);
      for(size_t index=0;index<items.size();index++){
        const auto item=std::get<M>(items[index]);const auto size=std::get<int64_t>(item.at(V("size")));uint64_t offset=0;
        while(offset<static_cast<uint64_t>(size)){
          M source{{V("index"),V(static_cast<int64_t>(index))},{V("offset"),V(static_cast<int64_t>(offset))},{V("length"),V(int32_t(32768))}};
          const auto chunk=std::get<std::vector<uint8_t>>(call("sourceChunk",source));require(!chunk.empty());
          M append=request;append[V("index")]=V(static_cast<int64_t>(index));append[V("offset")]=V(static_cast<int64_t>(offset));append[V("bytes")]=V(chunk);call("append",append);offset+=chunk.size();
        }
        M finish=request;finish[V("index")]=V(static_cast<int64_t>(index));finish[V("digest")]=call("sourceDigest",{{V("index"),V(static_cast<int64_t>(index))}});call("finish",finish);
      }
      call("commit",request);call("cancel");call("release");
    };
    const std::wstring text=L"한글 abc\r\n줄 바꿈 \U0001F4BB";clip::putText(own.owner,text);transfer(capture());require(clip::read(own.owner).text==text);
    const std::wstring large(32768,L'한');clip::putText(own.owner,large);transfer(capture());require(clip::read(own.owner).text==large);
    clip::putFiles(own.owner,{root/L"한글 파일.bin",root/L"empty.txt"});transfer(capture());received=clip::read(own.owner).files;require(received.size()==2);
    clip::FileSource source(received[0]);std::vector<uint8_t> actual;for(;;){const auto part=source.next();if(part.empty())break;actual.insert(actual.end(),part.begin(),part.end());}require(actual==bytes);
    // Another window (Explorer after a paste) listing exactly the received
    // files is still the peer's content; a different list is new content.
    {const HWND other=CreateWindowExW(0,L"STATIC",L"Other clipboard owner",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);require(other!=nullptr);
      const M automatic{{V("skipOwn"),V(true)}};
      clip::putFiles(other,received);require(std::get<M>(call("capture",automatic)).count(V("own"))==1);call("release");
      clip::putFiles(other,{received[0]});require(std::get<M>(call("capture",automatic)).count(V("own"))==0);call("release");
      clip::putFiles(own.owner,received);DestroyWindow(other);}
    // Invalid hash/order and cancellation leave the already published clipboard.
    M begin=request;begin[V("format")]=V("text");begin[V("items")]=V(flutter::EncodableList{V(M{{V("size"),V(int64_t(3))}})});call("begin",begin);
    M append=request;append[V("index")]=V(0);append[V("offset")]=V(1);append[V("bytes")]=V(std::vector<uint8_t>{'a'});rejects([&]{call("append",append);});
    append[V("offset")]=V(0);append[V("bytes")]=V(std::vector<uint8_t>{'a','b','c'});call("append",append);
    M finish=request;finish[V("index")]=V(0);finish[V("digest")]=V(std::string(64,'0'));rejects([&]{call("finish",finish);});call("cancel");require(clip::read(own.owner).files==received);
    begin[V("format")]=V("files");begin[V("items")]=V(flutter::EncodableList{V(M{{V("name"),V("../bad")},{V("size"),V(int64_t(1))}})});rejects([&]{call("begin",begin);});call("cancel");
    clip::putText(own.owner,L"done");
  }
  require(clip::expireFiles(root/L"receive",received));
  // Only our validated test directory is removed, after all station handles close.
  for(const auto& entry:fs::directory_iterator(root/L"receive")){require(entry.is_regular_file()&&entry.path().filename().wstring().rfind(L"completed-",0)==0);fs::remove(entry.path());}
  fs::remove(root/L"receive");fs::remove(root/L"한글 파일.bin");fs::remove(root/L"empty.txt");fs::remove(root);
  std::cout<<"{\"passed\":true,\"productionClientClipboardAdapter\":true,\"isolatedWindowStation\":true,\"interactiveClipboardRead\":false,\"interactiveClipboardWritten\":false,\"unicodeMaxUnits\":32768,\"utf8Bytes\":98304,\"actualFileDropRoundtrip\":true,\"fileBytes\":150123,\"emptyFile\":true,\"hashAndOffsetRejection\":true,\"cancelPreservedCompletedPaths\":true,\"remoteConnection\":false}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
