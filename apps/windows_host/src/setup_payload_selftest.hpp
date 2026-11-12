#pragma once
// QA only (MultitapClientSetupQA.exe --selftest-payload and the console
// fixtures): the payload-v1 placement, shortcut and request rules on throwaway
// trees under %TEMP%. Junctions stand in for every link case (no privilege is
// needed to make one). No product path, shortcut or setting is touched.
#include "setup_payload.hpp"
#include "../../../native/windows_admin/update_relaunch.hpp"
#include <winioctl.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace multitap::payload::qa {

inline std::string narrow(const std::wstring& value){return utf8(value);}
inline std::string hex(const Digest& digest){
  constexpr char digits[]="0123456789abcdef";std::string out;
  for(const unsigned char b:digest){out+=digits[b>>4];out+=digits[b&15];}
  return out;
}
inline Digest digestOf(const std::vector<char>& bytes){Sha256 hash;hash.add(bytes.data(),DWORD(bytes.size()));return hash.finish();}
inline std::vector<char> pattern(size_t size,unsigned seed){
  std::vector<char> bytes(size);unsigned value=seed*2654435761u+1;
  for(auto& b:bytes){value=value*1664525u+1013904223u;b=char(value>>24);}
  if(size>=2){bytes[0]='M';bytes[1]='Z';}
  return bytes;
}
inline bool writeBytes(const std::wstring& path,const std::vector<char>& bytes){
  Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
  DWORD written=0;
  return file&&(bytes.empty()||(WriteFile(file.get(),bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()));
}
inline std::optional<std::vector<char>> readBytes(const std::wstring& path){
  Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(!file)return std::nullopt;
  LARGE_INTEGER size{};if(!GetFileSizeEx(file.get(),&size)||size.QuadPart>(64<<20))return std::nullopt;
  std::vector<char> bytes(size_t(size.QuadPart));DWORD read=0;
  if(!bytes.empty()&&(!ReadFile(file.get(),bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size()))return std::nullopt;
  return bytes;
}
inline std::optional<Identity> identityAt(const std::wstring& path){
  Handle entry(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
  if(!entry)return std::nullopt;
  return identityOf(entry.get());
}
inline bool makeDirectory(const std::wstring& path){
  if(CreateDirectoryW(path.c_str(),nullptr))return true;
  if(GetLastError()==ERROR_ALREADY_EXISTS)return true;
  const size_t slash=path.find_last_of(L'\\');
  return slash!=std::wstring::npos&&slash>2&&makeDirectory(path.substr(0,slash))&&CreateDirectoryW(path.c_str(),nullptr);
}
// A directory junction (mount point) at [link] that resolves to [target].
inline bool makeJunction(const std::wstring& link,const std::wstring& target){
  if(!CreateDirectoryW(link.c_str(),nullptr))return false;
  Handle directory(CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(!directory)return false;
  const std::wstring substitute=L"\\??\\"+target,print=target;
  const size_t names=(substitute.size()+1+print.size()+1)*sizeof(wchar_t);
  std::vector<unsigned char> buffer(16+names,0);
  const auto put16=[&](size_t at,size_t value){buffer[at]=BYTE(value&0xff);buffer[at+1]=BYTE((value>>8)&0xff);};
  const DWORD tag=IO_REPARSE_TAG_MOUNT_POINT;std::memcpy(buffer.data(),&tag,4);
  put16(4,8+names);put16(8,0);put16(10,substitute.size()*2);put16(12,(substitute.size()+1)*2);put16(14,print.size()*2);
  std::memcpy(buffer.data()+16,substitute.c_str(),(substitute.size()+1)*2);
  std::memcpy(buffer.data()+16+(substitute.size()+1)*2,print.c_str(),(print.size()+1)*2);
  DWORD returned=0;
  return DeviceIoControl(directory.get(),FSCTL_SET_REPARSE_POINT,buffer.data(),DWORD(buffer.size()),nullptr,0,&returned,nullptr)!=FALSE;
}
// Never follows a link: a junction is removed as the link itself.
inline void removeTree(const std::wstring& path){
  const DWORD attributes=GetFileAttributesW(path.c_str());
  if(attributes==INVALID_FILE_ATTRIBUTES)return;
  if(attributes&FILE_ATTRIBUTE_REPARSE_POINT){if(attributes&FILE_ATTRIBUTE_DIRECTORY)RemoveDirectoryW(path.c_str());else DeleteFileW(path.c_str());return;}
  if(!(attributes&FILE_ATTRIBUTE_DIRECTORY)){SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);DeleteFileW(path.c_str());return;}
  WIN32_FIND_DATAW entry{};const HANDLE find=FindFirstFileExW((path+L"\\*").c_str(),FindExInfoBasic,&entry,FindExSearchNameMatch,nullptr,0);
  if(find!=INVALID_HANDLE_VALUE){do{if(std::wcscmp(entry.cFileName,L".")&&std::wcscmp(entry.cFileName,L".."))removeTree(path+L"\\"+entry.cFileName);}while(FindNextFileW(find,&entry));FindClose(find);}
  RemoveDirectoryW(path.c_str());
}
// Every entry below [root] ("name@" for a link, which is not entered), sorted.
inline void collect(const std::wstring& root,const std::wstring& relative,std::vector<std::wstring>& out){
  WIN32_FIND_DATAW entry{};const std::wstring at=relative.empty()?root:root+L"\\"+relative;
  const HANDLE find=FindFirstFileExW((at+L"\\*").c_str(),FindExInfoBasic,&entry,FindExSearchNameMatch,nullptr,0);
  if(find==INVALID_HANDLE_VALUE)return;
  do{
    if(!std::wcscmp(entry.cFileName,L".")||!std::wcscmp(entry.cFileName,L".."))continue;
    const std::wstring name=relative.empty()?entry.cFileName:relative+L"\\"+entry.cFileName;
    if(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){out.push_back(name+L"@");continue;}
    out.push_back(name);
    if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)collect(root,name,out);
  }while(FindNextFileW(find,&entry));
  FindClose(find);
}
inline std::vector<std::wstring> listTree(const std::wstring& root){std::vector<std::wstring> out;collect(root,L"",out);std::sort(out.begin(),out.end());return out;}
inline bool noLeftovers(const std::wstring& root){
  for(const auto& name:listTree(root))if(name.find(L".multitap-")!=std::wstring::npos)return false;
  return true;
}
inline bool sameWriteTime(const std::wstring& a,const std::wstring& b){
  WIN32_FILE_ATTRIBUTE_DATA x{},y{};
  return GetFileAttributesExW(a.c_str(),GetFileExInfoStandard,&x)&&GetFileAttributesExW(b.c_str(),GetFileExInfoStandard,&y)&&CompareFileTime(&x.ftLastWriteTime,&y.ftLastWriteTime)==0;
}
struct LinkFields {std::wstring target,arguments,directory;bool runAs=false;};
inline std::optional<LinkFields> readLink(const std::wstring& path){
  const auto bytes=readBytes(path);if(!bytes)return std::nullopt;
  try{
    Apartment apartment;Com<IShellLinkW> link;
    if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put()))))return std::nullopt;
    loadShortcut(link.get(),*bytes);
    LinkFields out;std::wstring buffer(32768,L'\0');
    if(FAILED(link->GetPath(buffer.data(),int(buffer.size()),nullptr,0)))return std::nullopt;
    out.target=buffer.c_str();
    if(FAILED(link->GetArguments(buffer.data(),int(buffer.size()))))return std::nullopt;
    out.arguments=buffer.c_str();
    if(FAILED(link->GetWorkingDirectory(buffer.data(),int(buffer.size()))))return std::nullopt;
    out.directory=buffer.c_str();
    {Com<IShellLinkDataList> data;DWORD flags=0;if(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(data.put())))&&SUCCEEDED(data->GetFlags(&flags)))out.runAs=(flags&SLDF_RUNAS_USER)!=0;}
    return out;
  }catch(...){return std::nullopt;}
}
// An earlier shortcut that carried "Run as administrator" (SLDF_RUNAS_USER).
inline bool plantRunAs(const std::wstring& path){
  const auto bytes=readBytes(path);if(!bytes)return false;
  try{
    Apartment apartment;Com<IShellLinkW> link;
    if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put()))))return false;
    loadShortcut(link.get(),*bytes);
    Com<IShellLinkDataList> data;DWORD flags=0;
    if(FAILED(link->QueryInterface(IID_PPV_ARGS(data.put())))||FAILED(data->GetFlags(&flags))||FAILED(data->SetFlags(flags|SLDF_RUNAS_USER)))return false;
    Com<IPersistStream> persist;Com<IStream> stream;
    if(FAILED(link->QueryInterface(IID_PPV_ARGS(persist.put())))||FAILED(CreateStreamOnHGlobal(nullptr,TRUE,stream.put()))||FAILED(persist->Save(stream.get(),TRUE)))return false;
    STATSTG stat{};LARGE_INTEGER zero{};
    if(FAILED(stream->Stat(&stat,STATFLAG_NONAME))||FAILED(stream->Seek(zero,STREAM_SEEK_SET,nullptr)))return false;
    std::vector<char> out(size_t(stat.cbSize.QuadPart));ULONG read=0;
    if(FAILED(stream->Read(out.data(),ULONG(out.size()),&read))||read!=out.size())return false;
    replaceFile(path,out);return readLink(path)&&readLink(path)->runAs;
  }catch(...){return false;}
}
// The fixture payload: executables, a multi-chunk DLL, a script, an empty file
// and nested directories (one name with a space).
struct Fixture {std::wstring relative;std::vector<char> bytes;};
inline std::vector<Fixture> fixtureFiles(){
  return {{L"MultitapHost.exe",pattern(70000,1)},{L"flutter_windows.dll",pattern(3*1024*1024+17,2)},{L"Helper.ps1",{'#',' ','q','a','\r','\n'}},
          {L"empty.bin",{}},{L"driver\\MttVDD.sys",pattern(5000,3)},{L"data\\flutter_assets\\fonts\\a b.ttf",pattern(1234,4)}};
}
inline bool writeFixture(const std::wstring& root){
  for(const auto& file:fixtureFiles()){
    const size_t slash=file.relative.find_last_of(L'\\');
    if(slash!=std::wstring::npos&&!makeDirectory(root+L"\\"+file.relative.substr(0,slash)))return false;
    if(!writeBytes(root+L"\\"+file.relative,file.bytes))return false;
  }
  return true;
}
inline std::vector<Entry> fixtureEntries(){
  std::vector<Entry> out;
  for(const auto& file:fixtureFiles())out.push_back(Entry{file.relative,file.bytes.size(),digestOf(file.bytes)});
  return out;
}
inline bool sameAsFixture(const std::wstring& root){
  for(const auto& file:fixtureFiles()){const auto bytes=readBytes(root+L"\\"+file.relative);if(!bytes||*bytes!=file.bytes)return false;}
  return true;
}
inline std::vector<std::wstring> fixtureTree(){
  return {L"Helper.ps1",L"MultitapHost.exe",L"data",L"data\\flutter_assets",L"data\\flutter_assets\\fonts",L"data\\flutter_assets\\fonts\\a b.ttf",L"driver",L"driver\\MttVDD.sys",L"empty.bin",L"flutter_windows.dll"};
}
inline std::string fileLines(const std::vector<Entry>& entries,bool forward=true){
  std::string out;
  for(const auto& entry:entries){std::wstring relative=entry.relative;if(forward)std::replace(relative.begin(),relative.end(),L'\\',L'/');out+="-6|file|"+std::to_string(entry.size)+"|"+hex(entry.sha256)+"|"+narrow(relative)+"\n";}
  return out;
}
// Feeds [text] line by line; returns the replies in order.
inline std::vector<std::string> feed(Server& server,const std::string& text){
  std::vector<std::string> replies;size_t start=0;
  while(start<text.size()){
    size_t end=text.find('\n',start);if(end==std::string::npos)end=text.size();
    std::string reply;if(server.Receive(text.substr(start,end-start),reply)&&!reply.empty())replies.push_back(reply);
    start=end+1;
  }
  return replies;
}

class Tree {
 public:
  Tree(){
    wchar_t temp[MAX_PATH+1]{};const DWORD length=GetTempPathW(MAX_PATH+1,temp);
    std::wstring base(temp,length);while(!base.empty()&&base.back()==L'\\')base.pop_back();
    std::wstring full(32768,L'\0');const DWORD expanded=GetLongPathNameW(base.c_str(),full.data(),DWORD(full.size()));
    if(expanded&&expanded<full.size())base.assign(full.c_str(),expanded);
    root_=base+L"\\Multitap-SetupPayloadQA-"+randomTag();
    ready_=CreateDirectoryW(root_.c_str(),nullptr)!=FALSE;
  }
  Tree(const Tree&)=delete;
  Tree& operator=(const Tree&)=delete;
  ~Tree(){removeTree(root_);}
  bool ready()const{return ready_;}
  std::wstring operator/(const std::wstring& relative)const{return root_+L"\\"+relative;}
  const std::wstring& root()const{return root_;}
 private:
  std::wstring root_;bool ready_=false;
};

inline bool Run(){
  int cases=0,failed=0;
  const auto report=[&](const char* name,bool passed,const std::string& code){++cases;if(!passed)++failed;std::printf("payloadCase=%s passed=%d code=%s\n",name,passed?1:0,code.empty()?"-":code.c_str());std::fflush(stdout);};
  try{
    Tree tree;if(!tree.ready()){std::printf("{\"kind\":\"setup-payload-placement\",\"passed\":false,\"reason\":\"temp_unavailable\"}\n");return false;}
    const std::wstring source=tree/L"src";
    if(!makeDirectory(source)||!writeFixture(source)){std::printf("{\"kind\":\"setup-payload-placement\",\"passed\":false,\"reason\":\"fixture_failed\"}\n");return false;}
    const auto entries=fixtureEntries();
    const auto plan=[&](const std::wstring& destination,std::vector<Entry> list){Plan out;out.source=source;out.destination=destination;out.chainTop=tree.root();out.entries=std::move(list);return out;};

    // An empty folder: every byte, every directory, the source write time, nothing extra.
    {const auto d=tree/L"d1";makeDirectory(d);const auto r=Place(plan(d,entries));
     report("fresh-placement",r.ok&&r.placed==entries.size()&&r.kept==0&&sameAsFixture(d)&&listTree(d)==fixtureTree()&&noLeftovers(d)&&sameWriteTime(source+L"\\flutter_windows.dll",d+L"\\flutter_windows.dll"),r.code);}
    // A late hash mismatch: the earlier files and the directories made for them go again.
    {const auto d=tree/L"d3";makeDirectory(d);auto list=entries;list.back().sha256[0]^=1;const auto r=Place(plan(d,list));
     report("hash-mismatch-rollback",!r.ok&&r.code=="payload_hash_mismatch"&&r.restored&&listTree(d).empty(),r.code);}
    // A size that differs from the listed one.
    {const auto d=tree/L"d4";makeDirectory(d);auto list=entries;list[1].size+=1;const auto r=Place(plan(d,list));
     report("size-mismatch-rollback",!r.ok&&r.code=="payload_size_mismatch"&&r.restored&&listTree(d).empty(),r.code);}
    // The same version again: an identical file stays, a changed one is
    // replaced; on a later failure the replaced original comes back.
    for(const bool failing:{false,true}){
      const auto d=tree/(failing?L"d6":L"d5");makeDirectory(d+L"\\driver");
      const auto host=source+L"\\MultitapHost.exe";const auto hostBytes=*readBytes(host);const auto oldDll=pattern(4096,9);
      writeBytes(d+L"\\MultitapHost.exe",hostBytes);writeBytes(d+L"\\flutter_windows.dll",oldDll);writeBytes(d+L"\\unrelated.txt",{'u'});
      const auto before=listTree(d);const auto keptId=identityAt(d+L"\\MultitapHost.exe"),oldId=identityAt(d+L"\\flutter_windows.dll");
      auto list=entries;if(failing)list.back().sha256[0]^=1;
      const auto r=Place(plan(d,list));
      const auto keptAfter=identityAt(d+L"\\MultitapHost.exe"),dllAfter=identityAt(d+L"\\flutter_windows.dll");const auto dllBytes=readBytes(d+L"\\flutter_windows.dll");
      const bool kept=keptId&&keptAfter&&sameIdentity(*keptId,*keptAfter)&&readBytes(d+L"\\unrelated.txt")==std::vector<char>{'u'};
      if(!failing)report("existing-keep-replace",r.ok&&r.kept==1&&r.placed==entries.size()-1&&kept&&dllAfter&&oldId&&!sameIdentity(*dllAfter,*oldId)&&sameAsFixture(d)&&noLeftovers(d),r.code);
      else report("existing-rollback-restores",!r.ok&&r.code=="payload_hash_mismatch"&&r.restored&&kept&&dllAfter&&oldId&&sameIdentity(*dllAfter,*oldId)&&dllBytes==oldDll&&listTree(d)==before,r.code);
    }
    // The destination itself is a junction.
    {const auto real=tree/L"real7",d=tree/L"d7";makeDirectory(real);const bool made=makeJunction(d,real);const auto r=Place(plan(d,entries));
     report("junction-destination",made&&!r.ok&&r.code=="payload_link_refused"&&listTree(real).empty(),r.code);}
    // A junction above the destination.
    {const auto real=tree/L"real8";makeDirectory(real+L"\\inner");const bool made=makeJunction(tree/L"j8",real);const auto r=Place(plan(tree/L"j8\\inner",entries));
     report("junction-ancestor",made&&!r.ok&&r.code=="payload_link_refused"&&listTree(real)==std::vector<std::wstring>{L"inner"},r.code);}
    // An existing directory inside the destination is a junction.
    {const auto d=tree/L"d9",elsewhere=tree/L"elsewhere9";makeDirectory(d);makeDirectory(elsewhere);const bool made=makeJunction(d+L"\\data",elsewhere);const auto r=Place(plan(d,entries));
     report("junction-inside-destination",made&&!r.ok&&r.code=="payload_link_refused"&&r.restored&&listTree(elsewhere).empty()&&listTree(d)==std::vector<std::wstring>{L"data@"},r.code);}
    // A file's name is taken by a junction.
    {const auto d=tree/L"d10",elsewhere=tree/L"elsewhere10";makeDirectory(d);makeDirectory(elsewhere);const bool made=makeJunction(d+L"\\MultitapHost.exe",elsewhere);const auto r=Place(plan(d,entries));
     report("junction-at-file-target",made&&!r.ok&&r.code=="payload_target_unsafe"&&r.restored&&listTree(elsewhere).empty()&&listTree(d)==std::vector<std::wstring>{L"MultitapHost.exe@"},r.code);}
    // A junction inside the source payload.
    {const auto linked=tree/L"src11",real=tree/L"driver-real11",d=tree/L"d11";makeDirectory(linked);makeDirectory(real);makeDirectory(d);
     for(const auto& file:fixtureFiles())if(file.relative.find(L'\\')==std::wstring::npos)writeBytes(linked+L"\\"+file.relative,file.bytes);
     writeBytes(real+L"\\MttVDD.sys",pattern(5000,3));const bool made=makeJunction(linked+L"\\driver",real);
     std::vector<Entry> list;for(const auto& entry:entries)if(entry.relative.find(L"data\\")!=0)list.push_back(entry);
     Plan p=plan(d,list);p.source=linked;const auto r=Place(p);
     report("junction-in-source",made&&!r.ok&&r.code=="payload_source_invalid"&&r.restored&&listTree(d).empty(),r.code);}
    // Relative names: plain names only.
    {bool ok=true;
     for(const wchar_t* good:{L"a.exe",L"data/flutter_assets/x y.ttf",L"driver\\MttVDD.sys",L"한글.txt"})ok&=safeRelative(good).has_value();
     for(const wchar_t* bad:{L"",L"..\\x",L"a/../b",L"./a",L"C:\\x",L"C:x",L"\\\\server\\share\\x",L"/abs",L"a:b",L"con",L"CON.txt",L"lpt1.log",L"x.",L"x ",L" x",L"a//b",L"a\\",L"nul\\x",L"a*b",L"a?b",L"a|b",L"a\"b",L"a<b",L"a\x01" L"b"})ok&=!safeRelative(bad).has_value();
     ok&=safeRelative(L"data/flutter_assets/x y.ttf")==std::optional<std::wstring>(L"data\\flutter_assets\\x y.ttf");
     report("relative-names",ok,"");}
    // The protected destination rule: a user-owned directory is refused, the
    // Windows directory (TrustedInstaller) passes.
    {bool refused=false,accepted=false;
     try{Handle mine=openDirectory(tree.root(),READ_CONTROL);protectedDirectory(mine.get());}catch(const Failure& f){refused=std::string(f.code)=="payload_destination_unprotected";}
     wchar_t windows[MAX_PATH+1]{};GetWindowsDirectoryW(windows,MAX_PATH+1);
     try{Handle system=openDirectory(windows,READ_CONTROL);protectedDirectory(system.get());accepted=true;}catch(const Failure&){}
     report("protected-destination-rule",refused&&accepted,"");}
    // The request channel with this product's roots under the tree.
    {Roots roots;const auto qa=tree/L"qa";roots.qa=true;roots.source=qa+L"\\payload\\bin";roots.programFiles=qa+L"\\ProgramFiles";roots.localAppData=qa+L"\\LocalAppData";
     roots.desktop=qa+L"\\Desktop";
     for(const auto* folder:{&roots.source,&roots.desktop})makeDirectory(*folder);
     writeFixture(roots.source);
     const std::string version="0.0.7",files=fileLines(entries);
     Server server(roots,version);
     std::string ignored;
     report("progress-lines-pass-through",!server.Receive("3|진행 중",ignored)&&!server.Receive("-5|preflight-complete-v1",ignored)&&ignored.empty(),"");
     report("file-before-place-refused",feed(server,"-6|file|1|"+std::string(64,'a')+"|x\n")==std::vector<std::string>{"payload-v1|error|payload_request_invalid\n"},"");
     {Server empty(Roots{},version);
      report("roots-missing-refused",feed(empty,"-6|place-v1|client|"+version+"|-\n"+files+"-6|commit\n")==std::vector<std::string>{"payload-v1|error|payload_unavailable\n"},"");}
     const std::string header="-6|place-v1|client|"+version+"|-\n";
     // The client goes to Program Files (administrator-only), not to the
     // per-user %LOCALAPPDATA%: one fixed
     // Client folder (not Client\<version>).
     const auto destination=roots.programFiles+L"\\Multitap\\Client";
     makeDirectory(destination);
     writeBytes(destination+L"\\multitap-client.json",{'{','}'});
     auto replies=feed(server,header+files+"-6|commit\n");
     report("channel-place",replies==std::vector<std::string>{"payload-v1|ok|"+std::to_string(entries.size())+"|0\n"}&&sameAsFixture(destination),replies.empty()?"":replies[0]);
     replies=feed(server,header+files+"-6|commit\n");
     report("channel-same-version-again",replies==std::vector<std::string>{"payload-v1|ok|0|"+std::to_string(entries.size())+"\n"}&&sameAsFixture(destination),replies.empty()?"":replies[0]);
     DeleteFileW((destination+L"\\multitap-client.json").c_str());
     replies=feed(server,header+files+"-6|commit\n");
     report("channel-unowned-destination",replies==std::vector<std::string>{"payload-v1|error|payload_destination_unowned\n"},replies.empty()?"":replies[0]);
     writeBytes(destination+L"\\multitap-client.json",{'{','}'});
     replies=feed(server,"-6|place-v1|client|0.0.8|-\n"+files+"-6|commit\n");
     report("channel-version-mismatch",replies==std::vector<std::string>{"payload-v1|error|payload_version_invalid\n"},replies.empty()?"":replies[0]);
     replies=feed(server,"-6|place-v1|other|0.0.7|-\n"+files+"-6|commit\n");
     report("channel-unknown-kind-refused",replies==std::vector<std::string>{"payload-v1|error|payload_request_invalid\n"},replies.empty()?"":replies[0]);
     replies=feed(server,header+"-6|file|5|XYZ|a.exe\n-6|commit\n");
     report("channel-bad-digest",replies==std::vector<std::string>{"payload-v1|error|payload_request_invalid\n"},replies.empty()?"":replies[0]);
     replies=feed(server,header+"-6|file|1|"+std::string(64,'a')+"|../evil.exe\n-6|commit\n");
     report("channel-unsafe-path",replies==std::vector<std::string>{"payload-v1|error|payload_request_invalid\n"}&&GetFileAttributesW((destination+L"\\..\\evil.exe").c_str())==INVALID_FILE_ATTRIBUTES,replies.empty()?"":replies[0]);
     replies=feed(server,header+files+"-6|shortcut-v1|client|"+version+"\n-6|commit\n");
     report("channel-interleaved-request-spoils",replies==std::vector<std::string>{"payload-v1|error|payload_request_invalid\n"}&&server.stats().shortcuts==0,replies.empty()?"":replies[0]);
     writeBytes(destination+L"\\Multitap.exe",pattern(100,11));
     const auto previousRoot=roots.localAppData+L"\\Multitap\\Client\\0.0.6";makeDirectory(previousRoot);writeBytes(previousRoot+L"\\Multitap.exe",pattern(100,8));
     {const std::string marker="{\"product\":\"Multitap\",\"version\":\"0.0.6\"}";writeBytes(previousRoot+L"\\multitap-client.json",{marker.begin(),marker.end()});}
     Shortcut previous;previous.path=roots.desktop+L"\\Multitap.lnk";previous.boundary=roots.localAppData+L"\\Multitap\\Client";previous.target=previousRoot+L"\\Multitap.exe";previous.directory=previousRoot;previous.icon=previous.target;previous.description=L"previous";previous.arguments=L"--kept";
     const bool planted=WriteShortcut(previous).ok&&plantRunAs(previous.path);const auto plantedBytes=readBytes(previous.path);
     replies=feed(server,"-6|shortcut-v1|client|0.0.7\n");
     const auto written=readLink(previous.path);
     report("channel-client-shortcut",planted&&replies==std::vector<std::string>{"payload-v1|ok\n"}&&written&&sameText(written->target,destination+L"\\Multitap.exe")&&sameText(written->directory,destination)&&written->arguments==L"--kept"&&!written->runAs&&noLeftovers(roots.desktop),replies.empty()?"":replies[0]);
     replies=feed(server,"-6|shortcut-restore-v1|client\n");
     report("channel-client-shortcut-restored",replies==std::vector<std::string>{"payload-v1|ok\n"}&&plantedBytes&&readBytes(previous.path)==plantedBytes,replies.empty()?"":replies[0]);
     const auto foreign=tree/L"foreign.exe";writeBytes(foreign,pattern(10,7));
     Shortcut other=previous;other.boundary=tree/L"foreign-root";other.target=foreign;other.directory=tree.root();other.icon=foreign;makeDirectory(other.boundary);
     DeleteFileW(other.path.c_str());const bool foreignPlanted=WriteShortcut(other).ok;const auto foreignBytes=readBytes(other.path);
     replies=feed(server,"-6|shortcut-v1|client|0.0.7\n");
     report("channel-foreign-shortcut-kept",foreignPlanted&&replies==std::vector<std::string>{"payload-v1|error|shortcut_foreign\n"}&&readBytes(other.path)==foreignBytes,replies.empty()?"":replies[0]);
    }
    // The per-user client of an earlier layout: once the new
    // installation is verified, exactly the marked version folders go, by
    // handle; the user's data and anything unmarked stay; a link is removed as
    // itself and never entered; a folder in use stays until a later run. The
    // extracted package under %TEMP% goes the same way.
    {
      Roots roots;const auto qa=tree/L"legacy";roots.qa=true;roots.source=qa+L"\\payload\\bin";roots.programFiles=qa+L"\\ProgramFiles";roots.localAppData=qa+L"\\LocalAppData";roots.desktop=qa+L"\\Desktop";
      makeDirectory(roots.source);writeFixture(roots.source);
      const auto installed=roots.programFiles+L"\\Multitap\\Client",legacy=roots.localAppData+L"\\Multitap\\Client";
      const auto marker=[](const std::string& version){const std::string text="{\r\n    \"mode\":  \"cloud-auth\",\r\n    \"product\":  \"Multitap\",\r\n    \"version\":  \""+version+"\"\r\n}";return std::vector<char>(text.begin(),text.end());};
      const auto exists=[](const std::wstring& path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;};
      const auto outside=tree/L"legacy-outside";makeDirectory(outside);writeBytes(outside+L"\\keep.txt",{'k'});
      for(const std::string version:{"0.0.5","0.0.6"}){
        const std::wstring folder=legacy+L"\\"+std::wstring(version.begin(),version.end());
        makeDirectory(folder+L"\\data\\flutter_assets");writeBytes(folder+L"\\multitap-client.json",marker(version));
        writeBytes(folder+L"\\Multitap.exe",pattern(500,20));writeBytes(folder+L"\\data\\flutter_assets\\a.bin",pattern(10,21));
      }
      SetFileAttributesW((legacy+L"\\0.0.6\\Multitap.exe").c_str(),FILE_ATTRIBUTE_READONLY);
      const bool linked=makeJunction(legacy+L"\\0.0.5\\data\\linked",outside);
      for(const wchar_t* name:{L"settings.json",L"login.json",L"launcher.json",L"session.json"})writeBytes(legacy+L"\\"+name,{'{','}'});
      // Not this product's folder of that version: a marker naming another
      // version, and a folder that is not a version.
      makeDirectory(legacy+L"\\0.0.4");writeBytes(legacy+L"\\0.0.4\\multitap-client.json",marker("0.0.3"));
      makeDirectory(legacy+L"\\notes");writeBytes(legacy+L"\\notes\\multitap-client.json",marker("0.0.2"));
      // A client still running from 0.0.3 (its EXE open without delete sharing).
      makeDirectory(legacy+L"\\0.0.3");writeBytes(legacy+L"\\0.0.3\\multitap-client.json",marker("0.0.3"));writeBytes(legacy+L"\\0.0.3\\Multitap.exe",pattern(100,22));
      Handle busy(CreateFileW((legacy+L"\\0.0.3\\Multitap.exe").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
      Server server(roots,"0.0.7");
      auto replies=feed(server,"-6|legacy-remove-v1|client|0.0.7\n");
      const bool unverified=replies==std::vector<std::string>{"payload-v1|error|legacy_new_install_unverified\n"}&&exists(legacy+L"\\0.0.5\\Multitap.exe");
      makeDirectory(installed);writeBytes(installed+L"\\multitap-client.json",marker("0.0.7"));writeBytes(installed+L"\\Multitap.exe",pattern(100,23));
      replies=feed(server,"-6|legacy-remove-v1|client|0.0.8\n");
      const bool otherVersion=replies==std::vector<std::string>{"payload-v1|error|payload_version_invalid\n"};
      replies=feed(server,"-6|legacy-remove-v1|client|0.0.7\n");
      const auto busyKept=[&]{for(const auto& name:listTree(legacy))if((name==L"0.0.3\\Multitap.exe"||(name.find(L"0.0.3.multitap-old-")==0&&name.ends_with(L"\\Multitap.exe")))&&readBytes(legacy+L"\\"+name)==pattern(100,22))return true;return false;}();
      const auto kept=[&]{return readBytes(outside+L"\\keep.txt")==std::vector<char>{'k'}&&readBytes(legacy+L"\\settings.json")==std::vector<char>{'{','}'}&&readBytes(legacy+L"\\login.json")==std::vector<char>{'{','}'}&&
          readBytes(legacy+L"\\launcher.json")==std::vector<char>{'{','}'}&&readBytes(legacy+L"\\session.json")==std::vector<char>{'{','}'}&&exists(legacy+L"\\0.0.4\\multitap-client.json")&&exists(legacy+L"\\notes\\multitap-client.json")&&exists(installed+L"\\Multitap.exe");};
      report("legacy-client-removed",linked&&unverified&&otherVersion&&replies==std::vector<std::string>{"payload-v1|ok|2|1\n"}&&!exists(legacy+L"\\0.0.5")&&!exists(legacy+L"\\0.0.6")&&busyKept&&kept(),replies.empty()?"":replies[0]);
      busy.reset();
      replies=feed(server,"-6|legacy-remove-v1|client|0.0.7\n");
      bool none=true;for(const auto& name:listTree(legacy))if(name.find(L"0.0.3")==0)none=false;
      report("legacy-client-in-use-removed-later",replies==std::vector<std::string>{"payload-v1|ok|1|0\n"}&&none&&kept()&&server.stats().legacyRemoved==3,replies.empty()?"":replies[0]);
      // The Client folder itself a junction: refused, its target untouched.
      {Roots linkedRoots=roots;linkedRoots.localAppData=qa+L"\\LinkedLocal";makeDirectory(linkedRoots.localAppData+L"\\Multitap");
       const auto real=tree/L"legacy-real";makeDirectory(real+L"\\0.0.5");writeBytes(real+L"\\0.0.5\\multitap-client.json",marker("0.0.5"));
       const bool made=makeJunction(linkedRoots.localAppData+L"\\Multitap\\Client",real);
       Server linkedServer(linkedRoots,"0.0.7");replies=feed(linkedServer,"-6|legacy-remove-v1|client|0.0.7\n");
       report("legacy-client-root-link-refused",made&&replies==std::vector<std::string>{"payload-v1|error|payload_link_refused\n"}&&exists(real+L"\\0.0.5\\multitap-client.json"),replies.empty()?"":replies[0]);}
      // Older versions under Program Files (Client\<version>, before the fixed
      // folder): every marked one goes, a running one stays for a
      // later run, anything unmarked stays. Real markers carry a BOM.
      {Roots oldRoots=roots;oldRoots.programFiles=qa+L"\\OldProgramFiles";
       const auto base=oldRoots.programFiles+L"\\Multitap\\Client";
       const auto real=[](const std::string& version){const std::string text="\xEF\xBB\xBF{\r\n    \"installed\":  \"2026-11-01T08:42:29+09:00\",\r\n    \"version\":  \""+version+"\",\r\n    \"mode\":  \"cloud-auth\",\r\n    \"product\":  \"Multitap\"\r\n}";return std::vector<char>(text.begin(),text.end());};
       for(const std::string version:{"0.0.3","0.0.4","0.0.5","0.0.6","0.0.7"}){
         const std::wstring folder=base+L"\\"+std::wstring(version.begin(),version.end());
         makeDirectory(folder);writeBytes(folder+L"\\multitap-client.json",real(version));writeBytes(folder+L"\\Multitap.exe",pattern(100,24));
       }
       makeDirectory(base+L"\\0.0.2");writeBytes(base+L"\\0.0.2\\keep.txt",{'k'});
       // The new installation: the fixed folder itself.
       writeBytes(base+L"\\multitap-client.json",real("0.0.7"));writeBytes(base+L"\\Multitap.exe",pattern(100,25));
       Handle running(CreateFileW((base+L"\\0.0.4\\Multitap.exe").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
       Server oldServer(oldRoots,"0.0.7");
       const bool otherTarget=feed(oldServer,"-6|old-remove-v1|client|0.0.8|0.0.6\n")==std::vector<std::string>{"payload-v1|error|payload_version_invalid\n"};
       const bool badReplaced=feed(oldServer,"-6|old-remove-v1|client|0.0.7|..\\x\n")==std::vector<std::string>{"payload-v1|error|payload_version_invalid\n"};
       replies=feed(oldServer,"-6|old-remove-v1|client|0.0.7|0.0.6\n");
       const bool held=[&]{for(const auto& name:listTree(base))if(name==L"0.0.4\\Multitap.exe"||(name.find(L"0.0.4.multitap-old-")==0&&name.ends_with(L"\\Multitap.exe")))return true;return false;}();
       report("old-client-versions-removed",otherTarget&&badReplaced&&replies==std::vector<std::string>{"payload-v1|ok|4|1\n"}&&!exists(base+L"\\0.0.3")&&!exists(base+L"\\0.0.5")&&!exists(base+L"\\0.0.6")&&!exists(base+L"\\0.0.7")&&held&&
           exists(base+L"\\Multitap.exe")&&exists(base+L"\\multitap-client.json")&&exists(base+L"\\0.0.2\\keep.txt"),replies.empty()?"":replies[0]);
      }
      // No per-user installation at all: nothing to do.
      {Roots fresh=roots;fresh.localAppData=qa+L"\\FreshLocal";makeDirectory(fresh.localAppData);
       Server freshServer(fresh,"0.0.7");replies=feed(freshServer,"-6|legacy-remove-v1|client|0.0.7\n");
       report("legacy-client-none",replies==std::vector<std::string>{"payload-v1|ok|0|0\n"},replies.empty()?"":replies[0]);}
      // The extracted package: removed by handle, a link inside never followed;
      // a source outside %TEMP% is refused before anything.
      {const auto outsideSource=tree/L"source-outside";makeDirectory(outsideSource);writeBytes(outsideSource+L"\\keep.txt",{'s'});
       const bool made=makeJunction(roots.source+L"\\linked",outsideSource);
       replies=feed(server,"-6|source-remove-v1|client\n");
       report("source-removed-by-handle",made&&replies==std::vector<std::string>{"payload-v1|ok\n"}&&!exists(roots.source)&&readBytes(outsideSource+L"\\keep.txt")==std::vector<char>{'s'},replies.empty()?"":replies[0]);
       wchar_t system[MAX_PATH+1]{};GetSystemDirectoryW(system,MAX_PATH+1);
       Roots foreign=roots;foreign.source=system;Server foreignServer(foreign,"0.0.7");
       replies=feed(foreignServer,"-6|source-remove-v1|client\n");
       report("source-outside-temp-refused",replies==std::vector<std::string>{"payload-v1|error|source_not_temporary\n"}&&exists(std::wstring(system)+L"\\kernel32.dll"),replies.empty()?"":replies[0]);}
    }
    // Shortcut paths: a junction under the shortcut's name, a missing target.
    {const auto folder=tree/L"links",real=tree/L"links-real";makeDirectory(folder);makeDirectory(real);const auto target=tree/L"target.exe";writeBytes(target,pattern(10,10));
     const bool made=makeJunction(folder+L"\\a.lnk",real);
     Shortcut link;link.path=folder+L"\\a.lnk";link.boundary=tree.root();link.target=target;link.directory=tree.root();link.icon=target;
     const auto linked=WriteShortcut(link);
     link.path=folder+L"\\b.lnk";link.target=tree/L"missing.exe";const auto missing=WriteShortcut(link);
     report("shortcut-link-and-missing-target",made&&!linked.ok&&linked.code=="shortcut_unsafe"&&listTree(real).empty()&&!missing.ok&&missing.code=="shortcut_target_missing"&&GetFileAttributesW((folder+L"\\b.lnk").c_str())==INVALID_FILE_ATTRIBUTES,linked.code+"/"+missing.code);}
    // No downgrade through an unattended update: the installed version is read
    // from the (administrator-only) Program Files tree, never from a record.
    {const auto programs=tree/L"programfiles";
     const auto product=multitap::relaunch::Product::Client;
     const std::wstring kind=L"Client";const std::string markerProduct="Multitap";
     const auto install=[&](const std::string& version,const std::string& declared){
       const auto folder=programs+L"\\Multitap\\"+kind+L"\\"+std::wstring(version.begin(),version.end());makeDirectory(folder);
       const std::string text="{\"product\":\""+markerProduct+"\",\"version\":\""+declared+"\"}";
       return writeBytes(folder+L"\\multitap-client.json",std::vector<char>(text.begin(),text.end()));};
     const auto refused=[&](const std::string& own){return multitap::relaunch::DowngradeRefused(product,own,programs);};
     bool ok=!refused("0.0.1")&&!multitap::relaunch::HighestInstalledVersion(product,programs);          // nothing installed
     makeDirectory(programs+L"\\Multitap\\"+kind);
     ok&=install("0.4.9","0.4.9")&&install("0.4.10","0.4.10");
     ok&=multitap::relaunch::HighestInstalledVersion(product,programs)==std::optional<std::string>("0.4.10"); // numeric, not textual, order
     ok&=refused("0.4.9")&&refused("0.3.99")&&refused("")&&refused("garbage");                           // older or unreadable
     ok&=!refused("0.4.10")&&!refused("0.4.11")&&!refused("1.0.0");                                      // same (reinstall) and newer
     // A folder whose marker names another version, a staging folder and a non-version name never count.
     ok&=install("0.9.0","0.8.0")&&makeDirectory(programs+L"\\Multitap\\"+kind+L"\\.stage-0.9.1-ab")&&makeDirectory(programs+L"\\Multitap\\"+kind+L"\\9.9");
     ok&=!refused("0.4.10")&&refused("0.4.9");
     ok&=multitap::relaunch::HighestInstalledVersion(product,tree/L"nowhere")==std::nullopt;
     report("downgrade-refused",ok,"");}
  }catch(const Failure& failure){report("unexpected-failure",false,failure.code);}
  catch(...){report("unexpected-exception",false,"");}
  const bool passed=failed==0&&cases>0;
  std::printf("{\"kind\":\"setup-payload-placement\",\"product\":\"client\",\"passed\":%s,\"cases\":%d,\"failed\":%d,\"isolatedTemporaryTree\":true,\"realInstallTouched\":false}\n",passed?"true":"false",cases,failed);
  return passed;
}

}  // namespace multitap::payload::qa
