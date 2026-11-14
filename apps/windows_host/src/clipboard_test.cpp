#include "clipboard_worker.hpp"
#include <iostream>
#include <map>
#include <winioctl.h>
#include <shlobj.h>
#include <shobjidl.h>
using namespace host;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
bool reparseGateChecked=true,currentDesktopRun=false;
void storageCheck(const std::filesystem::path& base){
  const auto parent=base/L"storage";
  {
    clip::Storage storage(parent);std::vector<std::filesystem::path> paths;
    {clip::FileReceiver receiver(parent,{{L"retained.bin",3}});receiver.append(0,0,{'a','b','c'});receiver.finish(0,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");paths=receiver.commit();storage.record(paths,{3});}
    // Mark-of-the-Web: a received file carries the Internet zone with fixed URLs only.
    {std::ifstream zone(paths[0].wstring()+L":Zone.Identifier",std::ios::binary);std::string mark((std::istreambuf_iterator<char>(zone)),std::istreambuf_iterator<char>());
     require(mark=="[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=about:internet\r\nHostUrl=about:internet\r\n","Received clipboard file lacks the Internet-zone mark");
     std::ifstream content(paths[0],std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(content)),std::istreambuf_iterator<char>());require(bytes=="abc","The mark changed the received file content");}
    const auto receipt=parent/(L"completed-"+paths[0].parent_path().filename().wstring().substr(9)+L".bin");
    FILETIME now{};GetSystemTimeAsFileTime(&now);ULARGE_INTEGER clock{};clock.LowPart=now.dwLowDateTime;clock.HighPart=now.dwHighDateTime;uint64_t expired=clock.QuadPart/10000-2*clip::Storage::retentionMillis;
    {std::fstream file(receipt,std::ios::binary|std::ios::in|std::ios::out);require(bool(file),"Own receipt fixture cannot be opened");file.seekp(24);file.write(reinterpret_cast<const char*>(&expired),sizeof(expired));require(bool(file),"Own receipt fixture cannot be aged");}
    storage.prepare(0,paths);require(std::filesystem::exists(paths[0]),"Expiry removed current clipboard paths");storage.prepare(0,{});require(std::filesystem::is_empty(parent),"Expired committed paths or receipt survived");
    const auto unknown=parent/L"unrecognized.bin";{std::ofstream file(unknown,std::ios::binary);file<<"do not remove";}
    storage.prepare(0,{});require(std::filesystem::exists(unknown),"Unknown storage file was deleted");std::filesystem::remove(unknown);
    const auto sparse=parent/L"quota.bin";WinHandle file(CreateFileW(sparse.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));require(file.value!=INVALID_HANDLE_VALUE,"Own quota fixture cannot be created");DWORD count=0;require(DeviceIoControl(file.value,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&count,nullptr)!=FALSE,"Own quota fixture cannot be sparse");LARGE_INTEGER end{};end.QuadPart=clip::Storage::quotaBytes;require(SetFilePointerEx(file.value,end,nullptr,FILE_BEGIN)&&SetEndOfFile(file.value),"Own quota fixture cannot be sized");bool refused=false;try{storage.prepare(1,{});}catch(...){refused=true;}require(refused,"Logical retained-byte quota was not enforced");CloseHandle(file.value);file.value=INVALID_HANDLE_VALUE;std::filesystem::remove(sparse);
    // Creating a symbolic link needs the symlink privilege (an elevated run or Developer Mode); without it the gate is reported as not checked.
    const auto link=base/L"storage-link";if(CreateSymbolicLinkW(link.c_str(),parent.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY)!=FALSE){bool reparseRefused=false;try{clip::Storage linked(link);}catch(...){reparseRefused=true;}require(reparseRefused,"A reparse storage parent was accepted");require(RemoveDirectoryW(link.c_str())!=FALSE,"Own reparse fixture cleanup failed");}else{require(GetLastError()==ERROR_PRIVILEGE_NOT_HELD,"Own reparse fixture cannot be created");reparseGateChecked=false;}
  }
  require(std::filesystem::is_empty(parent),"Storage QA leaked files");std::filesystem::remove(parent);
}
class Isolated {
 public:
  // Without the privilege to create a window station (a normal user), a developer may run the same scenarios on the
  // current desktop by setting MULTITAP_CLIPBOARD_QA_CURRENT_DESKTOP=1: this REPLACES the interactive clipboard content.
  Isolated(){if(const wchar_t* flag=_wgetenv(L"MULTITAP_CLIPBOARD_QA_CURRENT_DESKTOP");flag&&*flag==L'1'){current=currentDesktopRun=true;owner=CreateWindowExW(0,L"STATIC",L"Multitap current-desktop QA clipboard",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);require(owner!=nullptr,"Own QA owner failed");return;}
    originalStation=GetProcessWindowStation();originalDesktop=GetThreadDesktop(GetCurrentThreadId());name=L"MultitapHostClipboardQA_"+std::to_wstring(GetCurrentProcessId());station=CreateWindowStationW(name.c_str(),0,WINSTA_ALL_ACCESS,nullptr);winCheck(station!=nullptr,"Create own QA window station");winCheck(SetProcessWindowStation(station),"Select own QA window station");desktop=CreateDesktopW(L"ClipboardOnly",nullptr,nullptr,0,GENERIC_ALL,nullptr);require(desktop&&SetThreadDesktop(desktop),"Own QA desktop failed");owner=CreateWindowExW(0,L"STATIC",L"Multitap isolated QA clipboard",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);require(owner!=nullptr,"Own QA owner failed");}
  ~Isolated(){if(owner)DestroyWindow(owner);if(current)return;SetThreadDesktop(originalDesktop);SetProcessWindowStation(originalStation);if(desktop)CloseDesktop(desktop);if(station)CloseWindowStation(station);}
  bool workerCheck(){if(current)return true;if(GetThreadDesktop(GetCurrentThreadId())!=desktop&&!SetThreadDesktop(desktop))return false;wchar_t actual[256]{};DWORD size=0;return GetUserObjectInformationW(GetProcessWindowStation(),UOI_NAME,actual,sizeof(actual),&size)&&name==actual;}
  HWND owner=nullptr;HDESK desktop=nullptr;
 bool current=false;
 private:HWINSTA originalStation=nullptr,station=nullptr;HDESK originalDesktop=nullptr;std::wstring name;
};
class Protocol {
 public:
  Protocol(Isolated& isolated,const std::filesystem::path& parent):worker("isolated-qa-session",[this](const std::string& packet){output(packet);},[&]{return isolated.workerCheck();},parent){send(R"({"type":"clipboard.ready","sessionId":"isolated-qa-session","protocol":"clipboard-v1","clipboardPolicy":{"textToRemote":true,"filesToRemote":true,"textToLocal":true,"filesToLocal":true}})");}
  ~Protocol(){worker.stop();}
  // Formats profile: the worker's outgoing transfer `ackId` is received here. Chunks are acknowledged cumulatively
  // (every fourth chunk) and each item end by itself, like a windowed client does.
  // Paste-pull: what the other peer does with every message the worker emits (called on the worker thread).
  std::function<void(const Json&)> peer;
  std::string ackId;std::map<size_t,std::vector<uint8_t>> sentBlobs;std::vector<Json> sentMeta;size_t chunksSinceAck=0,chunkAcks=0,chunksSeen=0;
  ClipboardWorker worker;std::mutex mutex;std::condition_variable condition;std::deque<Json> messages;std::vector<uint8_t> pulled;std::vector<std::vector<uint8_t>> pulledFiles{2};std::atomic<bool> acknowledge{false};
  void send(const std::string& json){worker.message(JsonParser::parse(json));}
  Json wait(const char* type,const char* id){std::unique_lock lock(mutex);
    // The owner window of this thread must keep dispatching: the worker's EmptyClipboard() sends it WM_DESTROYCLIPBOARD.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool found=false;
    while(!(found=std::any_of(messages.begin(),messages.end(),[&](const Json& item){return item.get("type").string()==type&&item.get("requestId").string()==id;}))&&std::chrono::steady_clock::now()<deadline){lock.unlock();MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE))DispatchMessageW(&message);lock.lock();condition.wait_for(lock,std::chrono::milliseconds(5));}
    if(!found){std::string seen;for(const auto& item:messages)seen+=" "+item.get("type").string()+"/"+item.get("requestId").string()+"/"+item.get("reason").string();throw std::runtime_error(std::string("Native clipboard protocol response timeout waiting for ")+type+" "+id+"; seen:"+seen);}for(auto at=messages.begin();at!=messages.end();++at)if(at->get("type").string()==type&&at->get("requestId").string()==id){auto result=*at;messages.erase(at);return result;}throw std::runtime_error("Missing expected clipboard response");}
  void push(const char* id,const std::vector<uint8_t>& bytes,bool badHash=false){send("{\"type\":\"clipboard.begin\",\"requestId\":"+jsonQuote(id)+",\"format\":\"text\",\"size\":"+std::to_string(bytes.size())+'}');for(size_t offset=0;offset<bytes.size();){const size_t length=std::min<size_t>(32768,bytes.size()-offset);std::vector<uint8_t> chunk(bytes.begin()+offset,bytes.begin()+offset+length);send("{\"type\":\"clipboard.chunk\",\"requestId\":"+jsonQuote(id)+",\"index\":0,\"offset\":"+std::to_string(offset)+",\"data\":"+jsonQuote(clipboardBase64(chunk))+'}');offset+=length;require(wait("clipboard.chunk.ack",id).get("offset").number()==offset,"Native receiver offset ACK differed");}send("{\"type\":\"clipboard.item.end\",\"requestId\":"+jsonQuote(id)+",\"index\":0,\"sha256\":"+jsonQuote(badHash?std::string(64,'0'):clipboardDigest(bytes))+'}');if(badHash){wait("clipboard.error",id);return;}wait("clipboard.item.ack",id);send("{\"type\":\"clipboard.commit\",\"requestId\":"+jsonQuote(id)+'}');wait("clipboard.applied",id);}
  void pushFiles(const std::vector<uint8_t>& bytes,HWND busyOwner=nullptr){
    send("{\"type\":\"clipboard.begin\",\"requestId\":\"files\",\"format\":\"files\",\"files\":[{\"name\":\"한글 파일.bin\",\"size\":"+std::to_string(bytes.size())+"},{\"name\":\"empty.bin\",\"size\":0}]}");
    for(size_t offset=0;offset<bytes.size();){const size_t length=std::min<size_t>(32768,bytes.size()-offset);std::vector<uint8_t> chunk(bytes.begin()+offset,bytes.begin()+offset+length);send("{\"type\":\"clipboard.chunk\",\"requestId\":\"files\",\"index\":0,\"offset\":"+std::to_string(offset)+",\"data\":"+jsonQuote(clipboardBase64(chunk))+'}');offset+=length;wait("clipboard.chunk.ack","files");}
    for(size_t index=0;index<2;++index){send("{\"type\":\"clipboard.item.end\",\"requestId\":\"files\",\"index\":"+std::to_string(index)+",\"sha256\":"+jsonQuote(clipboardDigest(index?std::vector<uint8_t>{}:bytes))+'}');wait("clipboard.item.ack","files");}
    if(busyOwner)require(OpenClipboard(busyOwner)!=FALSE,"Own QA clipboard could not be held busy");
    send(R"({"type":"clipboard.commit","requestId":"files"})");
    try{wait(busyOwner?"clipboard.error":"clipboard.applied","files");}catch(...){if(busyOwner)CloseClipboard();throw;}
    if(busyOwner)CloseClipboard();
  }
  // hostAction 253: the worker's diagnostic packets (clip.diag), kept for the assertions of diagnosticsScenarios().
  std::vector<Json> diagnostics;
  bool diagnosed(double stage,double format=-1,double error=-1,double cause=-1){
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do{{std::lock_guard lock(mutex);for(const auto& item:diagnostics)if(item.get("stage").number()==stage&&(format<0||item.get("format").number()==format)&&(error<0||item.get("error").number()==error)&&(cause<0||item.get("cause").number()==cause))return true;}Sleep(5);}while(std::chrono::steady_clock::now()<end);
    return false;}
 private:
  void output(const std::string& packet){const auto wrapper=JsonParser::parse(packet);
    if(wrapper.get("type").string()=="clip.diag"){require(wrapper.get("sessionId").string()=="isolated-qa-session","A diagnostic left the private session envelope");{std::lock_guard lock(mutex);diagnostics.push_back(wrapper);}return;}
    require(wrapper.get("type").string()=="clip.packet"&&wrapper.get("sessionId").string()=="isolated-qa-session","Clipboard escaped the private session envelope");const auto value=wrapper.get("payload");const auto type=value.get("type").string(),id=value.get("requestId").string();
    if(peer){const auto handler=peer;handler(value);}
    if(!ackId.empty()&&id==ackId){
      if(type=="clipboard.begin"||type=="clipboard.entries")sentMeta.push_back(value);
      else if(type=="clipboard.chunk"){
        const size_t index=size_t(value.get("index").number());const auto bytes=clipboardChunk(value.get("data").string(),clip::formatsChunkBytes);auto& blob=sentBlobs[index];
        require(value.get("offset").number()==blob.size(),"Formats sender chunk order differed");blob.insert(blob.end(),bytes.begin(),bytes.end());++chunksSeen;
        if(++chunksSinceAck>=4){chunksSinceAck=0;++chunkAcks;send("{\"type\":\"clipboard.chunk.ack\",\"requestId\":"+jsonQuote(id)+",\"index\":"+std::to_string(index)+",\"offset\":"+std::to_string(blob.size())+'}');}
      }else if(type=="clipboard.item.end"){
        const size_t index=size_t(value.get("index").number());require(value.get("sha256").string()==clipboardDigest(sentBlobs[index]),"Formats sender digest differed");chunksSinceAck=0;
        send("{\"type\":\"clipboard.item.ack\",\"requestId\":"+jsonQuote(id)+",\"index\":"+std::to_string(index)+'}');
      }else if(type=="clipboard.commit")send("{\"type\":\"clipboard.complete\",\"requestId\":"+jsonQuote(id)+'}');
    }
    if(acknowledge&&id=="pull"){
      if(type=="clipboard.chunk"){const auto bytes=clipboardChunk(value.get("data").string());require(value.get("offset").number()==pulled.size(),"Native sender chunk order differed");pulled.insert(pulled.end(),bytes.begin(),bytes.end());send("{\"type\":\"clipboard.chunk.ack\",\"requestId\":\"pull\",\"index\":0,\"offset\":"+std::to_string(pulled.size())+'}');}
      else if(type=="clipboard.item.end"){require(value.get("sha256").string()==clipboardDigest(pulled),"Native sender digest differed");send(R"({"type":"clipboard.item.ack","requestId":"pull","index":0})");}
      else if(type=="clipboard.commit")send(R"({"type":"clipboard.complete","requestId":"pull"})");
    }
    if(acknowledge&&id=="pull-files"){
      const size_t index=size_t(value.get("index").number());require(index<2,"Native file sender index differed");
      if(type=="clipboard.begin"){const auto* files=std::get_if<Json::Array>(&value.get("files").value);require(files&&files->size()==2,"Native file manifest differed");for(const auto& file:*files)require(clip::safeLeaf(wide(file.get("name").string())),"Native manifest exposed a non-leaf path");}
      else if(type=="clipboard.chunk"){const auto bytes=clipboardChunk(value.get("data").string());require(value.get("offset").number()==pulledFiles[index].size(),"Native file sender offset differed");pulledFiles[index].insert(pulledFiles[index].end(),bytes.begin(),bytes.end());send("{\"type\":\"clipboard.chunk.ack\",\"requestId\":\"pull-files\",\"index\":"+std::to_string(index)+",\"offset\":"+std::to_string(pulledFiles[index].size())+'}');}
      else if(type=="clipboard.item.end"){require(value.get("sha256").string()==clipboardDigest(pulledFiles[index]),"Native file sender SHA differed");send("{\"type\":\"clipboard.item.ack\",\"requestId\":\"pull-files\",\"index\":"+std::to_string(index)+'}');}
      else if(type=="clipboard.commit")send(R"({"type":"clipboard.complete","requestId":"pull-files"})");
    }
    {std::lock_guard lock(mutex);messages.push_back(value);}condition.notify_all();
  }
};
std::string htmlFormat(const std::string& fragment){
  const std::string prefix="<html><body><!--StartFragment-->",suffix="<!--EndFragment--></body></html>";
  const auto header=[&](size_t a,size_t b,size_t c,size_t d){char buffer[256];snprintf(buffer,sizeof(buffer),"Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n",a,b,c,d);return std::string(buffer);};
  const size_t headerSize=header(0,0,0,0).size(),startFragment=headerSize+prefix.size(),endFragment=startFragment+fragment.size();
  return header(headerSize,endFragment+suffix.size(),startFragment,endFragment)+prefix+fragment+suffix;
}
std::vector<uint8_t> pattern(size_t size,unsigned seed){std::vector<uint8_t> out(size);uint32_t x=seed*2654435761u+1;for(auto& b:out){x=x*1664525u+1013904223u;b=uint8_t(x>>24);}return out;}
const char* formatsReady=R"({"type":"clipboard.ready","sessionId":"isolated-qa-session","protocol":"clipboard-v1","limits":{"chunkBytes":46080,"maxFiles":128,"maxFileBytes":4294967296,"maxTotalBytes":8589934592,"maxTextUnits":1048576,"maxTextBytes":1048576,"limitsProfile":2,"formatsProfile":2,"maxEntries":10000,"maxDepth":32,"maxPathChars":160,"maxRichBytes":8388608,"maxImageBytes":268435456,"maxImagePixels":64000000,"windowBytes":1048576,"windowUnits":32},"clipboardPolicy":{"textToRemote":true,"filesToRemote":true,"textToLocal":true,"filesToLocal":true},"clipboardSync":false})";
// Receives `items` (each streamed in formats-sized chunks) under the begin message, then commits.
void pushFormats(Protocol& protocol,const char* id,const std::string& begin,const std::vector<std::vector<uint8_t>>& items,const std::string& entries=std::string()){
  protocol.send(begin);if(!entries.empty())protocol.send(entries);
  for(size_t index=0;index<items.size();++index){
    for(size_t offset=0;offset<items[index].size();){const size_t length=std::min<size_t>(clip::formatsChunkBytes,items[index].size()-offset);std::vector<uint8_t> chunk(items[index].begin()+ptrdiff_t(offset),items[index].begin()+ptrdiff_t(offset+length));
      protocol.send("{\"type\":\"clipboard.chunk\",\"requestId\":"+jsonQuote(id)+",\"index\":"+std::to_string(index)+",\"offset\":"+std::to_string(offset)+",\"data\":"+jsonQuote(clipboardBase64(chunk))+'}');offset+=length;}
    protocol.send("{\"type\":\"clipboard.item.end\",\"requestId\":"+jsonQuote(id)+",\"index\":"+std::to_string(index)+",\"sha256\":"+jsonQuote(clipboardDigest(items[index]))+'}');
    protocol.wait("clipboard.item.ack",id);
  }
  protocol.send("{\"type\":\"clipboard.commit\",\"requestId\":"+jsonQuote(id)+'}');protocol.wait("clipboard.applied",id);
}
void formatsScenarios(Isolated& station,const std::filesystem::path& root){
  const auto all=clip::ReadOptions{true,true,true,true};
  // A released relay (no formats profile) is not offered the new formats: the worker refuses them.
  {Protocol old(station,root);old.send(R"({"type":"clipboard.begin","requestId":"rich-old","format":"richtext","items":[{"kind":"text","size":1}]})");
   require(old.wait("clipboard.error","rich-old").get("reason").string()=="clipboard_write_failed","Rich text without the formats profile was not refused");}
  Protocol protocol(station,root);protocol.send(formatsReady);
  // Rich text received: plain text and sanitised HTML are published; the RTF item is received and dropped (never published).
  {
    const std::wstring text=L"rich \xAC00\xB098";const auto utf8Text=utf8(text);const auto html=htmlFormat("<b>bold</b>");const std::string rtf="{\\rtf1 hello}";
    const std::vector<uint8_t> t(utf8Text.begin(),utf8Text.end()),h(html.begin(),html.end()),r(rtf.begin(),rtf.end());
    pushFormats(protocol,"rich-in","{\"type\":\"clipboard.begin\",\"requestId\":\"rich-in\",\"format\":\"richtext\",\"items\":[{\"kind\":\"text\",\"size\":"+std::to_string(t.size())+"},{\"kind\":\"html\",\"size\":"+std::to_string(h.size())+"},{\"kind\":\"rtf\",\"size\":"+std::to_string(r.size())+"}]}",{t,h,r});
    const auto board=clip::read(station.owner,all);require(board.text==text&&board.html&&*board.html==clip::sanitizeHtmlFormat(h)&&!board.rtf,"Received rich text was not published as text + sanitised HTML without RTF");
    // A malformed HTML header is refused and leaves the clipboard alone.
    auto bad=h;bad[bad.size()-2]=0xff;protocol.send("{\"type\":\"clipboard.begin\",\"requestId\":\"rich-bad\",\"format\":\"richtext\",\"items\":[{\"kind\":\"text\",\"size\":1},{\"kind\":\"html\",\"size\":"+std::to_string(bad.size())+"}]}");
    protocol.send(R"({"type":"clipboard.chunk","requestId":"rich-bad","index":0,"offset":0,"data":"YQ=="})");protocol.send("{\"type\":\"clipboard.item.end\",\"requestId\":\"rich-bad\",\"index\":0,\"sha256\":"+jsonQuote(clipboardDigest({'a'}))+'}');protocol.wait("clipboard.item.ack","rich-bad");
    protocol.send("{\"type\":\"clipboard.chunk\",\"requestId\":\"rich-bad\",\"index\":1,\"offset\":0,\"data\":"+jsonQuote(clipboardBase64(bad))+'}');protocol.send("{\"type\":\"clipboard.item.end\",\"requestId\":\"rich-bad\",\"index\":1,\"sha256\":"+jsonQuote(clipboardDigest(bad))+'}');protocol.wait("clipboard.item.ack","rich-bad");
    protocol.send(R"({"type":"clipboard.commit","requestId":"rich-bad"})");protocol.wait("clipboard.error","rich-bad");require(clip::read(station.owner,all).text==text,"A malformed HTML payload replaced the clipboard");
  }
  // An image received: PNG + DIBV5 + DIB.
  {
    auto rows=pattern(size_t(200)*600,7);std::vector<uint8_t> dib(40);dib[0]=40;dib[4]=200;dib[8]=200;dib[12]=1;dib[14]=24;dib.insert(dib.end(),rows.begin(),rows.end());
    const auto image=clip::encodePngFromDib(dib.data(),dib.size());require(image.png.size()>2*clip::formatsChunkBytes,"Image fixture too small to need several chunks");
    pushFormats(protocol,"img-in","{\"type\":\"clipboard.begin\",\"requestId\":\"img-in\",\"format\":\"image\",\"items\":[{\"kind\":\"png\",\"size\":"+std::to_string(image.png.size())+",\"width\":200,\"height\":200}]}",{image.png});
    require(IsClipboardFormatAvailable(CF_DIBV5)&&IsClipboardFormatAvailable(CF_DIB),"Received image lacks DIB formats");
    const auto board=clip::read(station.owner,all);require(board.image&&board.image->png==image.png&&!board.text,"Received image differs");
  }
  // A folder tree received.
  {
    const auto big=pattern(300000,3);
    const std::string begin=R"({"type":"clipboard.begin","requestId":"tree-in","format":"tree","entries":6,"files":3,"bytes":)"+std::to_string(big.size()+5)+R"(,"tops":2,"skipped":1})";
    const std::string entries=R"({"type":"clipboard.entries","requestId":"tree-in","start":0,"entries":[{"path":"Docs","kind":"dir","attrs":0},{"path":"Docs/big.bin","kind":"file","size":300000,"attrs":32},{"path":"Docs/Empty","kind":"dir","attrs":0},{"path":"Docs/zero.txt","kind":"file","size":0,"attrs":0},{"path":"same.txt","kind":"file","size":5,"attrs":0},{"path":"Docs/Sub","kind":"dir","attrs":0}]})";
    pushFormats(protocol,"tree-in",begin,{big,{},{'h','e','l','l','o'}},entries);
    const auto board=clip::read(station.owner,all);require(board.files.size()==2,"Received tree was not published as its two top-level items");
    const auto docs=board.files[0].filename()==L"Docs"?board.files[0]:board.files[1];
    require(std::filesystem::is_directory(docs/L"Empty")&&std::filesystem::is_directory(docs/L"Sub")&&std::filesystem::file_size(docs/L"big.bin")==big.size(),"Received tree lacks its folders or files");
    {std::ifstream in(docs/L"big.bin",std::ios::binary);std::vector<uint8_t> got((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());require(got==big,"Received tree file differs");}
    require(protocol.wait("clipboard.chunk.ack","tree-in").get("offset").number()>0,"No cumulative chunk acknowledgement was sent");
    // Hostile path and a wrong declared total are refused before anything is created.
    protocol.send(R"({"type":"clipboard.begin","requestId":"tree-bad","format":"tree","entries":1,"files":1,"bytes":1,"tops":1,"skipped":0})");
    protocol.send(R"({"type":"clipboard.entries","requestId":"tree-bad","start":0,"entries":[{"path":"../evil","kind":"file","size":1,"attrs":0}]})");protocol.wait("clipboard.error","tree-bad");
  }
  // Sending: rich text, an image and a tree are read from the clipboard and streamed with a sliding window.
  {
    const std::wstring text=L"send \xAC00";const auto html=htmlFormat("<i>x</i>");const std::vector<uint8_t> h(html.begin(),html.end());
    clip::putRich(station.owner,text,&h,nullptr,std::nullopt);
    protocol.ackId="rich-out";protocol.acknowledge=false;protocol.send(R"({"type":"clipboard.read","requestId":"rich-out"})");protocol.wait("clipboard.commit","rich-out");
    require(protocol.sentMeta.size()==1&&protocol.sentMeta[0].get("format").string()=="richtext"&&protocol.sentBlobs.size()==2&&protocol.sentBlobs[1]==h,"Rich text was not sent as text + HTML");
  }
  {
    Protocol sender(station,root);sender.send(formatsReady);
    auto rows=pattern(size_t(300)*900,9);std::vector<uint8_t> dib(40);dib[0]=40;dib[4]=0x2c;dib[5]=1;dib[8]=0x2c;dib[9]=1;dib[12]=1;dib[14]=24;dib.insert(dib.end(),rows.begin(),rows.end());
    const auto image=clip::encodePngFromDib(dib.data(),dib.size());clip::putImage(station.owner,image,std::nullopt);
    sender.ackId="img-out";sender.send(R"({"type":"clipboard.read","requestId":"img-out"})");sender.wait("clipboard.commit","img-out");
    require(sender.sentMeta.size()==1&&sender.sentMeta[0].get("format").string()=="image"&&sender.sentBlobs[0]==image.png,"The image was not sent as its PNG");
    require(sender.chunksSeen>=4&&sender.chunkAcks>=1&&sender.chunkAcks<sender.chunksSeen,"The sender did not use cumulative acknowledgements");
  }
  // A screenshot tool (Snipping Tool) leaves the bitmap AND its saved PNG file on the clipboard: the VM's copy goes out as the image;
  // next to a non-image file the files stay. Hostile to neither: the same shared rule as the PC client.
  {
    const auto shots=root/L"screenshots";std::filesystem::create_directories(shots);std::ofstream(shots/L"Screenshot.png",std::ios::binary)<<"png file";std::ofstream(shots/L"report.docx",std::ios::binary)<<"doc";
    auto rows=pattern(size_t(40)*120,3);std::vector<uint8_t> dib(40);dib[0]=40;dib[4]=40;dib[8]=40;dib[12]=1;dib[14]=24;dib.insert(dib.end(),rows.begin(),rows.end());
    const auto image=clip::encodePngFromDib(dib.data(),dib.size());
    const auto publish=[&](const std::vector<std::filesystem::path>& files){
      clip::putImage(station.owner,image,std::nullopt);
      require(OpenClipboard(station.owner)!=FALSE,"Clipboard busy");
      std::vector<uint8_t> drop(sizeof(DROPFILES),0);DROPFILES header{};header.pFiles=sizeof(header);header.fWide=TRUE;std::memcpy(drop.data(),&header,sizeof(header));
      for(const auto& file:files){const auto text=file.wstring();const auto* bytes=reinterpret_cast<const uint8_t*>(text.c_str());drop.insert(drop.end(),bytes,bytes+(text.size()+1)*2);}drop.push_back(0);drop.push_back(0);
      HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,drop.size());std::memcpy(GlobalLock(memory),drop.data(),drop.size());GlobalUnlock(memory);SetClipboardData(CF_HDROP,memory);CloseClipboard();
    };
    publish({shots/L"Screenshot.png"});
    require((clip::availableFormats()&(1|16))==(1|16),"The screenshot fixture lacks the bitmap or the file list");
    {Protocol sender(station,root);sender.send(formatsReady);sender.ackId="shot-out";sender.send(R"({"type":"clipboard.read","requestId":"shot-out"})");sender.wait("clipboard.commit","shot-out");
     require(sender.sentMeta.size()==1&&sender.sentMeta[0].get("format").string()=="image"&&sender.sentBlobs[0]==image.png,"A screenshot (bitmap + its PNG file) was not sent as the image");
     require(sender.diagnosed(2,4),"The screenshot send was not diagnosed as an image (hostAction 253 format 4)");}
    publish({shots/L"report.docx"});
    {Protocol sender(station,root);sender.send(formatsReady);sender.ackId="doc-out";sender.send(R"({"type":"clipboard.read","requestId":"doc-out"})");sender.wait("clipboard.commit","doc-out");
     require(sender.sentMeta.size()==1&&sender.sentMeta[0].get("format").string()=="files","A document next to a bitmap was not sent as the file");}
    // Its own source folder goes too (the check below admits only transfer folders and receipts).
    std::filesystem::remove_all(shots);
  }
  {
    Protocol sender(station,root);sender.send(formatsReady);
    const auto source=root/L"tree-source";std::filesystem::create_directories(source/L"Docs"/L"Empty");const auto big=pattern(500000,5);
    {std::ofstream(source/L"Docs"/L"big.bin",std::ios::binary).write(reinterpret_cast<const char*>(big.data()),std::streamsize(big.size()));std::ofstream(source/L"Docs"/L"small.txt")<<"small";std::ofstream(source/L"loose.txt")<<"loose";}
    clip::putPaths(station.owner,{source/L"Docs",source/L"loose.txt"},std::nullopt);
    sender.ackId="tree-out";sender.send(R"({"type":"clipboard.read","requestId":"tree-out"})");sender.wait("clipboard.commit","tree-out");
    require(sender.sentMeta.size()>=2&&sender.sentMeta[0].get("format").string()=="tree"&&sender.sentMeta[0].get("tops").number()==2&&sender.sentMeta[1].get("type").string()=="clipboard.entries","The tree manifest or entries differed");
    require(sender.sentBlobs.size()==3&&sender.sentBlobs[0]==big&&sender.chunksSeen>=11&&sender.chunkAcks<sender.chunksSeen,"The tree files were not streamed in order with cumulative acks");
    std::filesystem::remove_all(source);
  }
  // Flat file lists and plain text of a formats pair travel in formats-sized chunks (45 KiB), not the 32 KiB of stop and wait: the
  // field failure (the client's native adapter refused 45 KiB chunks of exactly these) was found by an end-to-end run.
  {
    const auto a=pattern(100000,41),b=pattern(clip::formatsChunkBytes+1,42);
    pushFormats(protocol,"flat-in","{\"type\":\"clipboard.begin\",\"requestId\":\"flat-in\",\"format\":\"files\",\"files\":[{\"name\":\"a.bin\",\"size\":100000},{\"name\":\"b.bin\",\"size\":"+std::to_string(b.size())+"}]}",{a,b});
    const auto board=clip::read(station.owner,all);require(board.files.size()==2&&!board.text,"Flat files received in formats-sized chunks were not published");
    for(size_t i=0;i<2;++i){std::ifstream in(board.files[i],std::ios::binary);std::vector<uint8_t> got((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());require(got==(board.files[i].filename()==L"a.bin"?a:b),"A flat file received in formats-sized chunks differs");}
    require(protocol.diagnosed(1,2)&&protocol.diagnosed(3,2),"A flat files receive was not diagnosed (hostAction 253 begin and end)");
    const auto text=pattern(100000,43);std::vector<uint8_t> ascii(text.size());for(size_t i=0;i<text.size();++i)ascii[i]=uint8_t('a'+text[i]%26);
    pushFormats(protocol,"text-in","{\"type\":\"clipboard.begin\",\"requestId\":\"text-in\",\"format\":\"text\",\"size\":"+std::to_string(ascii.size())+"}",{ascii});
    require(clip::read(station.owner,all).text==std::wstring(ascii.begin(),ascii.end()),"Plain text received in formats-sized chunks differs");
  }
  {
    Protocol sender(station,root);sender.send(formatsReady);
    const auto source=root/L"flat-source";std::filesystem::create_directories(source);const auto a=pattern(100000,44),b=pattern(clip::formatsChunkBytes+1,45);
    {std::ofstream(source/L"a.bin",std::ios::binary).write(reinterpret_cast<const char*>(a.data()),std::streamsize(a.size()));std::ofstream(source/L"b.bin",std::ios::binary).write(reinterpret_cast<const char*>(b.data()),std::streamsize(b.size()));}
    clip::putPaths(station.owner,{source/L"a.bin",source/L"b.bin"},std::nullopt);
    sender.ackId="flat-out";sender.send(R"({"type":"clipboard.read","requestId":"flat-out"})");sender.wait("clipboard.commit","flat-out");
    require(sender.sentMeta.size()==1&&sender.sentMeta[0].get("format").string()=="files"&&sender.sentBlobs.size()==2&&sender.sentBlobs[0]==a&&sender.sentBlobs[1]==b,"Flat files were not sent in order and in full");
    require(sender.diagnosed(2,2)&&sender.diagnosed(4,2),"A flat files send was not diagnosed (hostAction 253 begin and end)");
    std::filesystem::remove_all(source);
  }
  protocol.ackId.clear();protocol.worker.stop();
  // Received trees stay on disk until their receipt expires; only transfer folders and receipts may remain.
  for(const auto& entry:std::filesystem::directory_iterator(root)){
    const auto name=entry.path().filename().wstring();require(name.rfind(L"transfer-",0)==0||name.rfind(L"completed-",0)==0,"A formats scenario left an unexpected entry");
    std::filesystem::remove_all(entry.path());
  }
}
const char* pullReady=R"({"type":"clipboard.ready","sessionId":"isolated-qa-session","protocol":"clipboard-v1","limits":{"chunkBytes":46080,"maxFiles":128,"maxFileBytes":4294967296,"maxTotalBytes":8589934592,"maxTextUnits":1048576,"maxTextBytes":1048576,"limitsProfile":2,"formatsProfile":2,"maxEntries":10000,"maxDepth":32,"maxPathChars":160,"maxRichBytes":8388608,"maxImageBytes":268435456,"maxImagePixels":64000000,"windowBytes":1048576,"windowUnits":32,"pullProfile":1,"maxOffers":2},"clipboardPolicy":{"textToRemote":true,"filesToRemote":true,"textToLocal":true,"filesToLocal":true},"clipboardSync":false})";
std::string fileHash(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());return clipboardDigest(bytes);}
// The peer of a paste-pull exchange, in one object: serves pulls of a SourceOffer through the sliding window and records
// what the worker asked for. Runs on its own threads because the worker's messages arrive on the worker thread.
struct PullPeer {
  Protocol& protocol;std::shared_ptr<clip::SourceOffer> offer;std::mutex m;std::condition_variable cv;
  struct Pull{clip::SendWindow window;bool cancelled=false;std::thread thread;};std::map<std::string,std::unique_ptr<Pull>> pulls;
  std::atomic<uint64_t> sent{0};std::atomic<size_t> requests{0},cancels{0};std::vector<std::pair<size_t,uint64_t>> asked;
  explicit PullPeer(Protocol& protocol,std::shared_ptr<clip::SourceOffer> offer):protocol(protocol),offer(std::move(offer)){
    protocol.peer=[this](const Json& value){onMessage(value);};
  }
  ~PullPeer(){protocol.peer={};{std::lock_guard lock(m);for(auto& pull:pulls)pull.second->cancelled=true;}cv.notify_all();for(auto& pull:pulls)if(pull.second->thread.joinable())pull.second->thread.join();}
  void onMessage(const Json& value){
    const auto type=value.get("type").string(),id=value.get("requestId").string();
    if(type=="clipboard.pull"){
      ++requests;const size_t index=size_t(value.get("index").number());const uint64_t offset=uint64_t(value.get("offset").number());
      auto pull=std::make_unique<Pull>();auto* raw=pull.get();{std::lock_guard lock(m);asked.emplace_back(index,offset);pulls[id]=std::move(pull);}
      raw->thread=std::thread([this,raw,id,index,offset]{serve(*raw,id,index,offset);});
    }else if(type=="clipboard.chunk.ack"){
      {std::lock_guard lock(m);if(const auto found=pulls.find(id);found!=pulls.end())try{found->second->window.ackChunk(size_t(value.get("index").number()),uint64_t(value.get("offset").number()));}catch(...) {}}cv.notify_all();
    }else if(type=="clipboard.cancel"){
      {std::lock_guard lock(m);if(const auto found=pulls.find(id);found!=pulls.end())found->second->cancelled=true;}++cancels;cv.notify_all();
    }
  }
  void serve(Pull& pull,const std::string& id,size_t index,uint64_t offset){
    try{
      auto reader=offer->open(index,offset);
      while(reader->position()<reader->size()){
        const size_t count=size_t(std::min<uint64_t>(clip::formatsChunkBytes,reader->size()-reader->position()));
        {std::unique_lock lock(m);cv.wait(lock,[&]{return pull.cancelled||pull.window.canSend(count);});if(pull.cancelled)return;}
        const uint64_t at=reader->position();const auto bytes=reader->next(count);{std::lock_guard lock(m);pull.window.sentChunk(index,reader->position(),bytes.size());}sent+=bytes.size();
        protocol.send("{\"type\":\"clipboard.chunk\",\"requestId\":"+jsonQuote(id)+",\"index\":"+std::to_string(index)+",\"offset\":"+std::to_string(at)+",\"data\":"+jsonQuote(clipboardBase64(bytes))+'}');
      }
      const auto digest=reader->digest();if(!pull.cancelled)protocol.send("{\"type\":\"clipboard.item.end\",\"requestId\":"+jsonQuote(id)+",\"index\":"+std::to_string(index)+",\"sha256\":"+jsonQuote(digest)+'}');
    }catch(const std::exception& failure){protocol.send("{\"type\":\"clipboard.error\",\"requestId\":"+jsonQuote(id)+",\"reason\":"+jsonQuote(HostPullReason(failure.what()))+'}');}
  }
  static std::string HostPullReason(const std::string& what){return what=="clipboard_selected_file_changed"||what=="clipboard_file_locked"?what:"clipboard_transfer_failed";}
};
std::string offerJson(const char* id,const clip::SourceOffer& offer){
  return std::string("{\"type\":\"clipboard.offer\",\"requestId\":")+jsonQuote(id)+",\"format\":\"tree\",\"entries\":"+std::to_string(offer.items().size())+",\"files\":"+std::to_string(offer.fileCount())+",\"bytes\":"+std::to_string(offer.totalBytes())+",\"tops\":"+std::to_string(offer.tops())+",\"skipped\":"+std::to_string(offer.skipped())+'}';
}
std::string entriesJson(const char* id,const clip::SourceOffer& offer,size_t from,size_t to){
  std::string page;for(size_t at=from;at<to;++at){const auto& item=offer.items()[at];std::wstring wire=item.name;std::replace(wire.begin(),wire.end(),L'\\',L'/');if(at>from)page+=',';
    page+="{\"path\":"+jsonQuote(utf8(wire))+",\"kind\":"+jsonQuote(item.kind)+(item.kind=="file"?",\"size\":"+std::to_string(item.size):std::string())+",\"attrs\":"+std::to_string(item.attributes)+",\"mtime\":"+std::to_string(offer.mtimeMs(at))+'}';}
  return std::string("{\"type\":\"clipboard.entries\",\"requestId\":")+jsonQuote(id)+",\"start\":"+std::to_string(from)+",\"entries\":["+page+"]}";
}
void pumpFor(std::chrono::milliseconds span){const auto end=std::chrono::steady_clock::now()+span;MSG message{};while(std::chrono::steady_clock::now()<end){while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,0);}}
// The paste-pull profile: the Host publishes the peer's offer as virtual files and an Explorer-style extraction
// pulls the bytes; the Host offers its own large copies and serves the peer's pulls.
void pullScenarios(Isolated& station,const std::filesystem::path& root){
  const auto all=clip::ReadOptions{true,true,true,true};
  // The PC's files.
  const auto peerDir=root/L"peer";std::filesystem::create_directories(peerDir/L"Photos"/L"Empty");
  const auto big=pattern(3*1024*1024+11,21),mid=pattern(200000,22);
  {std::ofstream(peerDir/L"Photos"/L"big.bin",std::ios::binary).write(reinterpret_cast<const char*>(big.data()),std::streamsize(big.size()));std::ofstream(peerDir/L"Photos"/L"mid.bin",std::ios::binary).write(reinterpret_cast<const char*>(mid.data()),std::streamsize(mid.size()));std::ofstream(peerDir/L"note.txt")<<"note";std::ofstream(peerDir/L"zero.bin",std::ios::binary);}
  auto peerOffer=std::shared_ptr<clip::SourceOffer>(clip::SourceOffer::capture("offer-pc",{peerDir/L"Photos",peerDir/L"note.txt",peerDir/L"zero.bin"},clip::Bounds{}));
  // Without the pull profile the worker refuses an offer, as a released Host would.
  {Protocol old(station,root);old.send(formatsReady);old.send(offerJson("offer-old",*peerOffer));require(old.wait("clipboard.error","offer-old").get("reason").string()=="clipboard_write_failed","An offer without the pull profile was not refused");}
  {
    Protocol protocol(station,root);protocol.send(pullReady);PullPeer peer(protocol,peerOffer);
    protocol.send(offerJson("offer-pc",*peerOffer));protocol.send(entriesJson("offer-pc",*peerOffer,0,peerOffer->items().size()));
    protocol.wait("clipboard.applied","offer-pc");protocol.send(R"({"type":"clipboard.complete","requestId":"offer-pc"})");
    // The offer is on the OLE clipboard as virtual files; nothing was requested at copy time and no CF_HDROP was published.
    pumpFor(std::chrono::milliseconds(300));
    require(IsClipboardFormatAvailable(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW))&&IsClipboardFormatAvailable(RegisterClipboardFormatW(CFSTR_FILECONTENTS))&&!IsClipboardFormatAvailable(CF_HDROP),"The offer was not published as virtual files without CF_HDROP");
    require(peer.requests==0&&peer.sent==0,"Copying an offer pulled data");
    // Explorer-style extraction in another apartment (the data object lives in the worker's STA).
    const auto destination=root/L"pasted";std::filesystem::create_directories(destination);std::atomic<bool> finished{false};std::string failure;
    std::thread paste([&]{
      const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
      try{
        IDataObject* data=nullptr;require(SUCCEEDED(OleGetClipboard(&data))&&data,"OleGetClipboard failed");
        IShellItem* item=nullptr;require(SUCCEEDED(SHCreateItemFromParsingName(destination.c_str(),nullptr,IID_PPV_ARGS(&item))),"Destination item failed");
        IDropTarget* target=nullptr;require(SUCCEEDED(item->BindToHandler(nullptr,BHID_SFUIObject,IID_PPV_ARGS(&target))),"Folder drop target failed");
        POINTL where{0,0};DWORD effect=DROPEFFECT_COPY;require(SUCCEEDED(target->DragEnter(data,MK_LBUTTON,where,&effect)),"DragEnter failed");effect=DROPEFFECT_COPY;require(SUCCEEDED(target->Drop(data,0,where,&effect)),"Drop failed");
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(90);
        const auto size=[&]{uint64_t sum=0;std::error_code ec;for(const auto& e:std::filesystem::recursive_directory_iterator(destination,ec))if(e.is_regular_file(ec))sum+=e.file_size(ec);return sum;};
        while(std::chrono::steady_clock::now()<end&&size()<peerOffer->totalBytes())std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));target->Release();item->Release();data->Release();
      }catch(const std::exception& error){failure=error.what();}
      if(SUCCEEDED(com))CoUninitialize();finished=true;
    });
    while(!finished)pumpFor(std::chrono::milliseconds(50));
    paste.join();require(failure.empty(),failure.c_str());
    require(fileHash(destination/L"Photos"/L"big.bin")==clipboardDigest(big)&&fileHash(destination/L"Photos"/L"mid.bin")==clipboardDigest(mid)&&std::filesystem::is_directory(destination/L"Photos"/L"Empty")&&std::filesystem::file_size(destination/L"note.txt")==4&&std::filesystem::exists(destination/L"zero.bin"),"The pasted tree differs from the offered one");
    require(peer.requests>=3&&peer.sent>=big.size()+mid.size(),"The paste did not pull the files");
    // The Host withdrew nothing by itself, and its own offer is never announced back (the change was its own write).
    protocol.send(R"({"type":"clipboard.channel.closed"})");pumpFor(std::chrono::milliseconds(300));
    require(!IsClipboardFormatAvailable(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)),"The offer outlived the clipboard channel");
    protocol.worker.stop();
  }
  // The Host's own large copy is offered and the peer pulls it.
  {
    Protocol protocol(station,root);protocol.send(pullReady);
    const auto src=root/L"vm-source";std::filesystem::create_directories(src/L"Many");const auto first=pattern(250000,31);
    {std::ofstream(src/L"Many"/L"a_first.bin",std::ios::binary).write(reinterpret_cast<const char*>(first.data()),std::streamsize(first.size()));for(int i=0;i<300;++i)std::ofstream(src/L"Many"/(L"f"+std::to_wstring(i)+L".txt"))<<"x"<<i;}
    clip::putPaths(station.owner,{src/L"Many"},std::nullopt);
    std::mutex m;std::vector<Json> entries;Json offerMessage;std::map<std::string,std::vector<uint8_t>> pulled;std::map<std::string,uint64_t> startOf;std::map<std::string,std::string> digests,errors;size_t chunksSinceAck=0;std::atomic<bool> cancelNow{false};std::atomic<size_t> chunks{0};
    protocol.peer=[&](const Json& value){
      const auto type=value.get("type").string(),id=value.get("requestId").string();
      if(type=="clipboard.offer"||type=="clipboard.entries"){std::lock_guard lock(m);(type=="clipboard.offer"?offerMessage:entries.emplace_back())=value;}
      else if(type=="clipboard.chunk"){const auto bytes=clipboardChunk(value.get("data").string(),clip::formatsChunkBytes);std::lock_guard lock(m);auto& into=pulled[id];require(value.get("offset").number()==double(startOf[id]+into.size()),"A served chunk was out of order");into.insert(into.end(),bytes.begin(),bytes.end());++chunks;
        if(++chunksSinceAck>=4){chunksSinceAck=0;protocol.send("{\"type\":\"clipboard.chunk.ack\",\"requestId\":"+jsonQuote(id)+",\"index\":"+std::to_string(size_t(value.get("index").number()))+",\"offset\":"+std::to_string(startOf[id]+into.size())+'}');}
        if(cancelNow&&into.size()>100000){cancelNow=false;protocol.send("{\"type\":\"clipboard.cancel\",\"requestId\":"+jsonQuote(id)+'}');}}
      else if(type=="clipboard.item.end"){std::lock_guard lock(m);digests[id]=value.get("sha256").string();}
      else if(type=="clipboard.error"&&!id.empty()){std::lock_guard lock(m);errors[id]=value.get("reason").string();}
      else if(type=="clipboard.applied"){std::lock_guard lock(m);errors["applied:"+id]="1";}
    };
    protocol.send(R"({"type":"clipboard.read","requestId":"offer-vm"})");
    // Both pages (302 entries, 200 per page): the check below needs the second, which may follow the first a moment later.
    for(int i=0;i<200;++i){{std::lock_guard lock(m);if(entries.size()>=2)break;}pumpFor(std::chrono::milliseconds(10));}
    {std::lock_guard lock(m);require(offerMessage.get("format").string()=="tree"&&offerMessage.get("files").number()==301&&offerMessage.get("entries").number()==302,"The large copy was not offered as a tree manifest");require(entries.size()>=2,"The manifest entries were not paged");
      const auto* page=std::get_if<Json::Array>(&entries[0].get("entries").value);require(page&&page->size()==200&&page->at(0).get("mtime").number()>1.0e12,"Offer entries carry no write time");}
    for(int i=0;i<50&&!pulled.empty();++i)pumpFor(std::chrono::milliseconds(10));require(pulled.empty(),"The offered copy sent file data on its own");
    protocol.send(R"({"type":"clipboard.complete","requestId":"offer-vm"})");
    // A paste on the peer pulls file 0 (a_first.bin sorts first), then a range from an offset.
    const auto pullMessage=[&](const char* id,size_t index,uint64_t offset,const char* offerId="offer-vm"){{std::lock_guard lock(m);startOf[id]=offset;}protocol.send("{\"type\":\"clipboard.pull\",\"requestId\":"+jsonQuote(id)+",\"offerId\":"+jsonQuote(offerId)+",\"index\":"+std::to_string(index)+",\"offset\":"+std::to_string(offset)+'}');};
    pullMessage("pullc-1",0,0);
    for(int i=0;i<500;++i){{std::lock_guard lock(m);if(digests.count("pullc-1"))break;}pumpFor(std::chrono::milliseconds(10));}
    {std::lock_guard lock(m);require(digests["pullc-1"]==clipboardDigest(first)&&pulled["pullc-1"]==first,"The served file differs");}
    pullMessage("pullc-2",0,200000);
    for(int i=0;i<500;++i){{std::lock_guard lock(m);if(digests.count("pullc-2"))break;}pumpFor(std::chrono::milliseconds(10));}
    {std::lock_guard lock(m);const std::vector<uint8_t> tail(first.begin()+200000,first.end());require(pulled["pullc-2"]==tail&&digests["pullc-2"]==clipboardDigest(tail),"The served range differs");}
    // An unknown offer, a bad range and a file changed since the copy end the pull with their reason.
    pullMessage("pullc-3",0,0,"nothing");
    for(int i=0;i<500;++i){{std::lock_guard lock(m);if(errors.count("pullc-3"))break;}pumpFor(std::chrono::milliseconds(10));}
    {std::lock_guard lock(m);require(errors["pullc-3"]=="clipboard_offer_expired","An unknown offer was not reported as expired");}
    {std::ofstream append(src/L"Many"/L"a_first.bin",std::ios::binary|std::ios::app);append<<"!";}
    pullMessage("pullc-4",0,0);
    for(int i=0;i<500;++i){{std::lock_guard lock(m);if(errors.count("pullc-4"))break;}pumpFor(std::chrono::milliseconds(10));}
    {std::lock_guard lock(m);require(errors["pullc-4"]=="clipboard_selected_file_changed","A changed file was not reported");}
    // The other files still work; the requester cancelling stops the sender.
    pullMessage("pullc-5",1,0);
    for(int i=0;i<500;++i){{std::lock_guard lock(m);if(digests.count("pullc-5"))break;}pumpFor(std::chrono::milliseconds(10));}
    {std::lock_guard lock(m);require(!digests["pullc-5"].empty(),"Another file of the offer was not served");}
    // A pull that arrives while another transfer is being received is refused alone (clipboard_busy); that transfer is not
    // cancelled or reset and still commits.
    protocol.send(R"({"type":"clipboard.begin","requestId":"busy-text","format":"text","size":1})");
    pullMessage("pullc-7",1,0);
    for(int i=0;i<500;++i){{std::lock_guard lock(m);if(errors.count("pullc-7"))break;}pumpFor(std::chrono::milliseconds(10));}
    {std::lock_guard lock(m);require(errors["pullc-7"]=="clipboard_busy","A pull during an active transfer was not refused as busy");require(!errors.count("busy-text"),"The refused pull cancelled the other transfer");}
    protocol.send(R"({"type":"clipboard.chunk","requestId":"busy-text","index":0,"offset":0,"data":"YQ=="})");protocol.send("{\"type\":\"clipboard.item.end\",\"requestId\":\"busy-text\",\"index\":0,\"sha256\":"+jsonQuote(clipboardDigest({(uint8_t)97}))+'}');
    protocol.send(R"({"type":"clipboard.commit","requestId":"busy-text"})");
    for(int i=0;i<500;++i){{std::lock_guard lock(m);if(errors.count("applied:busy-text")||errors.count("busy-text"))break;}pumpFor(std::chrono::milliseconds(10));}
    {std::lock_guard lock(m);require(errors.count("applied:busy-text")&&!errors.count("busy-text"),"The transfer that was running during the refused pull did not complete");}
    // Revoked permission: a pull with the files direction withdrawn is refused.
    protocol.send(R"({"type":"clipboard.ready","sessionId":"isolated-qa-session","protocol":"clipboard-v1","limits":{"chunkBytes":46080,"maxFiles":128,"maxFileBytes":4294967296,"maxTotalBytes":8589934592,"maxTextUnits":1048576,"maxTextBytes":1048576,"limitsProfile":2,"formatsProfile":2,"maxEntries":10000,"maxDepth":32,"maxPathChars":160,"maxRichBytes":8388608,"maxImageBytes":268435456,"maxImagePixels":64000000,"windowBytes":1048576,"windowUnits":32,"pullProfile":1,"maxOffers":2},"clipboardPolicy":{"textToRemote":true,"filesToRemote":true,"textToLocal":true,"filesToLocal":false},"clipboardSync":false})");
    pullMessage("pullc-6",1,0);
    for(int i=0;i<500;++i){{std::lock_guard lock(m);if(errors.count("pullc-6"))break;}pumpFor(std::chrono::milliseconds(10));}
    {std::lock_guard lock(m);require(errors["pullc-6"]=="clipboard_transfer_failed"||errors["pullc-6"]=="clipboard_policy_denied","A pull without the files permission was served");}
    protocol.peer={};protocol.worker.stop();std::filesystem::remove_all(src);
  }
  std::filesystem::remove_all(peerDir);std::filesystem::remove_all(root/L"pasted");
  for(const auto& entry:std::filesystem::directory_iterator(root)){
    const auto name=entry.path().filename().wstring();require(name.rfind(L"transfer-",0)==0||name.rfind(L"completed-",0)==0,"A pull scenario left an unexpected entry");std::filesystem::remove_all(entry.path());
  }
}
// What Explorer puts on the clipboard for a file copy that the VM user made: the shell's own list ("Shell IDList Array") with or
// without CF_HDROP, and virtual files (FileGroupDescriptorW, archives / attachments / phones) that exist only as a data object.
void shellListScenarios(Isolated& station,const std::filesystem::path& base){
  const auto dir=base/L"shell-list";std::filesystem::create_directories(dir);const auto file=dir/L"한글 복사.txt";{std::ofstream out(file,std::ios::binary);out<<"abc";}
  const auto put=[&](const std::function<void()>& fill){require(OpenClipboard(station.owner)!=FALSE,"Own QA clipboard could not be opened");EmptyClipboard();fill();CloseClipboard();};
  const auto global=[](const void* data,size_t size){HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,size);void* at=GlobalLock(memory);std::memcpy(at,data,size);GlobalUnlock(memory);return memory;};
  // CIDA: item count, the offsets of the parent folder and each item, then the item ID lists.
  const auto idList=[&](const std::filesystem::path& path){
    PIDLIST_ABSOLUTE absolute=nullptr;winCheck(SUCCEEDED(SHParseDisplayName(path.c_str(),nullptr,&absolute,0,nullptr)),"Own QA path has no shell item");
    PIDLIST_ABSOLUTE parent=ILClone(absolute);ILRemoveLastID(parent);PCUITEMID_CHILD child=ILFindLastID(absolute);
    const UINT parentSize=ILGetSize(parent),childSize=ILGetSize(child),header=sizeof(UINT)*3;std::vector<uint8_t> bytes(header+parentSize+childSize);
    const UINT count=1,offsets[2]{header,header+parentSize};std::memcpy(bytes.data(),&count,4);std::memcpy(bytes.data()+4,offsets,8);std::memcpy(bytes.data()+header,parent,parentSize);std::memcpy(bytes.data()+header+parentSize,child,childSize);
    ILFree(parent);ILFree(absolute);return bytes;
  };
  const UINT shellList=RegisterClipboardFormatW(CFSTR_SHELLIDLIST),virtualFiles=RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
  const auto list=idList(file);
  put([&]{SetClipboardData(shellList,global(list.data(),list.size()));});
  require((clip::availableFormats()&2)==2&&(clip::availableFormats()&1)==0,"The shell list alone was not reported as such");
  const auto fromList=clip::read(station.owner,clip::ReadOptions{true,true,false,false});
  require(fromList.files.size()==1&&std::filesystem::equivalent(fromList.files[0],file),"A copy that carries only the shell list did not yield its file");
  // Text wins nothing here: the list is the copy. A list whose offsets lie outside the data yields nothing and is refused as unsupported.
  auto broken=list;const UINT outside=UINT(broken.size()+64);std::memcpy(broken.data()+8,&outside,4);
  put([&]{SetClipboardData(shellList,global(broken.data(),broken.size()));});
  bool refused=false;try{clip::read(station.owner,clip::ReadOptions{true,true,false,false});}catch(const std::exception& failure){refused=std::string(failure.what())=="unsupported_clipboard_format";}
  require(refused,"A malformed shell list was not refused as unsupported");
  // Virtual files only: say "unsupported", not "nothing copied", and report the format.
  const std::vector<uint8_t> descriptor(4+sizeof(FILEDESCRIPTORW),0);
  put([&]{SetClipboardData(virtualFiles,global(descriptor.data(),descriptor.size()));});
  require((clip::availableFormats()&4)==4,"Virtual files were not reported as such");
  refused=false;try{clip::read(station.owner,clip::ReadOptions{true,true,false,false});}catch(const std::exception& failure){refused=std::string(failure.what())=="unsupported_clipboard_format";}
  require(refused,"Virtual files were reported as an empty clipboard");
  // Text with the virtual files (a mail attachment) still sends the text.
  const std::wstring words=L"text";put([&]{SetClipboardData(virtualFiles,global(descriptor.data(),descriptor.size()));SetClipboardData(CF_UNICODETEXT,global(words.c_str(),(words.size()+1)*sizeof(wchar_t)));});
  require(clip::read(station.owner,clip::ReadOptions{true,true,false,false}).text==words,"Text next to virtual files was lost");
  put([&]{});
  std::filesystem::remove_all(dir);
}
}
int wmain(int argc,wchar_t** argv){try{  require(argc==2,"Pass a new own QA directory");const auto root=std::filesystem::absolute(argv[1]);require(!std::filesystem::exists(root)&&root.filename().wstring().rfind(L"run-",0)==0,"Unsafe QA directory");std::filesystem::create_directories(root);
  storageCheck(root);
  {Isolated station;shellListScenarios(station,root);}
  require(clipboardBase64({1,2,3})=="AQID"&&clipboardChunk("AQID")==std::vector<uint8_t>({1,2,3}),"Canonical base64 vector failed");require(clipboardDigest({'a','b','c'})=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 vector failed");
  {
    Isolated station;Protocol protocol(station,root);const std::wstring text(32768,L'한');const auto encoded=utf8(text);const std::vector<uint8_t> bytes(encoded.begin(),encoded.end());protocol.push("unicode",bytes);require(clip::read(station.owner).text==text,"Native worker Unicode clipboard differed");
    // hostAction 253: a receive is announced and finished with its format; sizes only, never content.
    require(protocol.diagnosed(1,1)&&protocol.diagnosed(3,1),"A text receive was not diagnosed (hostAction 253 begin and end)");protocol.acknowledge=true;protocol.send(R"({"type":"clipboard.read","requestId":"pull"})");protocol.wait("clipboard.commit","pull");require(protocol.pulled==bytes,"Native worker explicit read bytes differed");protocol.push("bad-hash",{'b','a','d'},true);require(clip::read(station.owner).text==text,"Bad SHA replaced the clipboard");
    // A failed receive names its side, the reason sent to the peer (clipboard_write_failed = 5) and the internal cause (1: the worker's own check).
    require(protocol.diagnosed(5,1,5,1),"A rejected hash was not diagnosed as a failed receive (hostAction 253)");
    std::vector<uint8_t> fileBytes(150123);for(size_t i=0;i<fileBytes.size();++i)fileBytes[i]=uint8_t(i*31%251);protocol.pushFiles(fileBytes);const auto drop=clip::read(station.owner);require(drop.files.size()==2&&!drop.text,"Native worker CF_HDROP differed");
    {clip::FileSource received(drop.files[0]);std::vector<uint8_t> actual;while(received.position()<received.description().size){auto part=received.next(32768);actual.insert(actual.end(),part.begin(),part.end());}require(actual==fileBytes,"Native worker staged file bytes differed");}
    protocol.send(R"({"type":"clipboard.read","requestId":"pull-files"})");protocol.wait("clipboard.commit","pull-files");require(protocol.pulledFiles[0]==fileBytes&&protocol.pulledFiles[1].empty(),"Native worker file read bytes differed");
    clip::putText(station.owner,text);require(clip::expireFiles(root,drop.files),"Own committed QA file expiry failed");{clip::Storage storage(root);storage.forget(drop.files);}
    protocol.send(R"({"type":"clipboard.begin","requestId":"partial","format":"files","files":[{"name":"partial.bin","size":3}]})");protocol.send(R"({"type":"clipboard.chunk","requestId":"partial","index":0,"offset":0,"data":"AQID"})");protocol.wait("clipboard.chunk.ack","partial");protocol.send(R"({"type":"clipboard.cancel","requestId":"partial"})");
    // The peer ended it: reported as the peer's cancel, not as a failure of this Host.
    require(protocol.diagnosed(8,2),"A cancel by the peer was not diagnosed (hostAction 253)");
    for(int i=0;i<100&&!std::filesystem::is_empty(root);++i)Sleep(10);require(std::filesystem::is_empty(root),"Cancelled native file transfer leaked partial data");protocol.worker.stop();require(clip::read(station.owner).text==text,"Cancelled transfer replaced the clipboard");
    {Protocol busy(station,root);busy.pushFiles(fileBytes,station.owner);busy.worker.stop();require(std::filesystem::is_empty(root),"Post-commit clipboard failure leaked staged files or its receipt");require(clip::read(station.owner).text==text,"Busy clipboard failure replaced prior content");}
    // Size limits. Without a limits profile in clipboard.ready the released limits apply; the extended profile
    // (4 GiB per file, 8 GiB per transfer, 1 MiB text) unlocks larger transfers and the specific reason.
    {
      Protocol limits(station,root);
      limits.send(R"({"type":"clipboard.begin","requestId":"too-long-v1","format":"text","size":131073})");
      require(limits.wait("clipboard.error","too-long-v1").get("reason").string()=="clipboard_write_failed","Released limits did not refuse long text with the released reason");
      limits.send(R"({"type":"clipboard.ready","sessionId":"isolated-qa-session","protocol":"clipboard-v1","limits":{"chunkBytes":32768,"maxFiles":128,"maxFileBytes":4294967296,"maxTotalBytes":8589934592,"maxTextUnits":1048576,"maxTextBytes":1048576,"limitsProfile":2},"clipboardPolicy":{"textToRemote":true,"filesToRemote":true,"textToLocal":true,"filesToLocal":true},"clipboardSync":false})");
      limits.send(R"({"type":"clipboard.begin","requestId":"too-big","format":"files","files":[{"name":"huge.bin","size":4294967297}]})");
      require(limits.wait("clipboard.error","too-big").get("reason").string()=="clipboard_limits_exceeded","Extended limits did not report the specific limits reason for a file above 4 GiB");
      limits.send(R"({"type":"clipboard.begin","requestId":"too-many","format":"files","files":[{"name":"a.bin","size":4294967296},{"name":"b.bin","size":4294967296},{"name":"c.bin","size":1}]})");
      require(limits.wait("clipboard.error","too-many").get("reason").string()=="clipboard_limits_exceeded","Extended limits did not refuse a transfer above 8 GiB");
      const std::string longText(300000,'a');limits.push("long-text",std::vector<uint8_t>(longText.begin(),longText.end()));
      require(clip::read(station.owner).text==std::wstring(300000,L'a'),"Extended limits did not accept 300000 characters of text");
      require(std::filesystem::is_empty(root),"A refused limits transfer leaked files");
    }
    formatsScenarios(station,root);
    pullScenarios(station,root);
  }
  require(std::filesystem::is_empty(root),"Native clipboard QA leaked files");std::filesystem::remove(root);std::cout<<"{\"kind\":\"host-worker-clipboard-own-station\",\"passed\":true,\"isolatedWindowStation\":"<<(currentDesktopRun?"false":"true")<<",\"productionDesktopGuardBypassedOnlyByTestAdapter\":true,\"unicodeUnits\":32768,\"utf8Bytes\":98304,\"actualFileDropRoundtrip\":true,\"fileBytes\":150123,\"emptyFile\":true,\"explicitPullWithChunkAck\":true,\"badShaPreservedClipboard\":true,\"cancelPreservedClipboard\":true,\"partialFileCleanup\":true,\"protectedStorage\":true,\"reparseStorageParentGate\":"<<(reparseGateChecked?"true":"false")<<",\"formatsProfile\":true,\"pullProfile\":true,\"pasteExtractionThroughOleDataObject\":true,\"richTextRoundtrip\":true,\"imageRoundtrip\":true,\"folderTreeRoundtrip\":true,\"slidingWindowCumulativeAcks\":true,\"currentFilesPreservedDuringExpiry\":true,\"expiredFilesAndReceiptRemoved\":true,\"logical2GiBQuota\":true,\"unknownFilesPreserved\":true,\"postCommitBusyClipboardCleanup\":true,\"interactiveClipboardRead\":"<<(currentDesktopRun?"true":"false")<<",\"interactiveClipboardWritten\":"<<(currentDesktopRun?"true":"false")<<",\"remoteConnection\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
