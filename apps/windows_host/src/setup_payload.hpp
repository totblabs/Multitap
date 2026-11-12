#pragma once
// payload-v1: the elevated setup (MultitapClientSetup.exe) writes every
// installed program file and shortcut itself. Install-Client.ps1 only lists,
// requests (over its progress channel) and verifies. A behaviour
// detection (AhnLab Safe Transaction, "Persistence/MDP.Event.M4290 · 실행 파일
// 생성") ends powershell.exe when it creates executables; an installer
// process creating them is ordinary.
//
// Placement: every listed file has an exact size and SHA-256, checked on the
// bytes read and again on the bytes written. Links are refused in the source,
// in the destination and its ancestors, and every entry is checked by its
// final path. A failure undoes what the request changed: new files and
// directories are removed and replaced files come back. An existing shortcut
// is changed only when it points into the product.
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <objbase.h>
#include <objidl.h>
#include <sddl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "setup_text.hpp"
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"shlwapi.lib")
#pragma comment(lib,"advapi32.lib")

namespace multitap::payload {

constexpr size_t MaxEntries=100000;
constexpr unsigned long long MaxFileSize=4ULL<<30;
constexpr size_t MaxRelative=1024;

// A fixed, data-free reason; the script shows it as a 확인 코드.
struct Failure { const char* code; };

class Handle {
 public:
  Handle()=default;
  explicit Handle(HANDLE value):value_(value){}
  Handle(const Handle&)=delete;
  Handle& operator=(const Handle&)=delete;
  Handle(Handle&& other)noexcept:value_(other.release()){}
  Handle& operator=(Handle&& other)noexcept{if(this!=&other){reset();value_=other.release();}return *this;}
  ~Handle(){reset();}
  HANDLE get()const{return value_;}
  explicit operator bool()const{return value_!=INVALID_HANDLE_VALUE&&value_!=nullptr;}
  HANDLE release(){const HANDLE value=value_;value_=INVALID_HANDLE_VALUE;return value;}
  void reset(){if(*this)CloseHandle(value_);value_=INVALID_HANDLE_VALUE;}
 private:
  HANDLE value_=INVALID_HANDLE_VALUE;
};

using Digest=std::array<unsigned char,32>;

inline BCRYPT_ALG_HANDLE sha256Provider(){
  static const BCRYPT_ALG_HANDLE provider=[]{BCRYPT_ALG_HANDLE value=nullptr;return BCryptOpenAlgorithmProvider(&value,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0?value:nullptr;}();
  return provider;
}
class Sha256 {
 public:
  Sha256(){if(!sha256Provider()||BCryptCreateHash(sha256Provider(),&hash_,nullptr,0,nullptr,0,0)<0)throw Failure{"payload_hash_unavailable"};}
  Sha256(const Sha256&)=delete;
  Sha256& operator=(const Sha256&)=delete;
  ~Sha256(){if(hash_)BCryptDestroyHash(hash_);}
  void add(const void* bytes,DWORD count){if(count&&BCryptHashData(hash_,static_cast<PUCHAR>(const_cast<void*>(bytes)),count,0)<0)throw Failure{"payload_hash_unavailable"};}
  Digest finish(){Digest out{};if(BCryptFinishHash(hash_,out.data(),DWORD(out.size()),0)<0)throw Failure{"payload_hash_unavailable"};return out;}
 private:
  BCRYPT_HASH_HANDLE hash_=nullptr;
};
inline std::optional<Digest> parseDigest(const std::string& text){
  if(text.size()!=64)return std::nullopt;
  Digest out{};
  for(size_t i=0;i<64;++i){
    const char ch=text[i];const int value=ch>='0'&&ch<='9'?ch-'0':ch>='a'&&ch<='f'?ch-'a'+10:-1;
    if(value<0)return std::nullopt;
    out[i/2]=static_cast<unsigned char>(i%2?out[i/2]|value:value<<4);
  }
  return out;
}

// --- Text and path helpers ---------------------------------------------------
inline bool sameText(const std::wstring& a,const std::wstring& b){
  return a.size()==b.size()&&(a.empty()||CompareStringOrdinal(a.data(),int(a.size()),b.data(),int(b.size()),TRUE)==CSTR_EQUAL);
}
inline bool startsWithText(const std::wstring& value,const std::wstring& prefix){
  return value.size()>=prefix.size()&&(prefix.empty()||CompareStringOrdinal(value.data(),int(prefix.size()),prefix.data(),int(prefix.size()),TRUE)==CSTR_EQUAL);
}
struct NoCase {bool operator()(const std::wstring& a,const std::wstring& b)const{return CompareStringOrdinal(a.data(),int(a.size()),b.data(),int(b.size()),TRUE)==CSTR_LESS_THAN;}};
inline std::optional<std::wstring> widen(const std::string& value){
  if(value.empty())return std::wstring();
  if(value.size()>65536)return std::nullopt;
  const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
  if(length<=0)return std::nullopt;
  std::wstring out(size_t(length),L'\0');
  MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),out.data(),length);
  return out;
}
inline std::string utf8(const std::wstring& value){
  if(value.empty())return {};
  const int length=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
  if(length<=0)return {};
  std::string out(size_t(length),'\0');
  WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),out.data(),length,nullptr,nullptr);
  return out;
}
// An absolute, canonical drive path without a trailing separator ("C:\a\b").
inline bool canonicalPath(const std::wstring& path){
  if(path.size()<3||path.size()>32000||path[1]!=L':'||path[2]!=L'\\'||!((path[0]>=L'A'&&path[0]<=L'Z')||(path[0]>=L'a'&&path[0]<=L'z')))return false;
  if(path.size()>3&&path.back()==L'\\')return false;
  if(path.find(L'/')!=std::wstring::npos||path.find(L':',2)!=std::wstring::npos||path.find(L'\0')!=std::wstring::npos)return false;
  std::wstring full(path.size()+16,L'\0');
  const DWORD length=GetFullPathNameW(path.c_str(),DWORD(full.size()),full.data(),nullptr);
  if(!length||length>=full.size())return false;
  full.resize(length);
  return full==path;
}
// The final path Windows resolves for an open handle (\\?\C:\...).
inline std::wstring finalPath(HANDLE handle){
  std::wstring buffer(512,L'\0');
  for(int attempt=0;attempt<4;++attempt){
    const DWORD length=GetFinalPathNameByHandleW(handle,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!length)break;
    if(length<buffer.size()){buffer.resize(length);return buffer;}
    buffer.resize(size_t(length)+1);
  }
  throw Failure{"payload_path_unverified"};
}
// \\?\ form of an existing canonical path, with 8.3 names expanded, for an
// exact comparison with finalPath.
inline std::wstring expectedFinal(const std::wstring& path){
  std::wstring buffer(path.size()+64,L'\0');
  for(int attempt=0;attempt<4;++attempt){
    const DWORD length=GetLongPathNameW(path.c_str(),buffer.data(),DWORD(buffer.size()));
    if(!length)break;
    if(length<buffer.size()){buffer.resize(length);return L"\\\\?\\"+buffer;}
    buffer.resize(size_t(length)+1);
  }
  throw Failure{"payload_path_unverified"};
}
inline bool reservedName(const std::wstring& segment){
  std::wstring stem=segment.substr(0,segment.find(L'.'));
  for(auto& ch:stem)if(ch>=L'a'&&ch<=L'z')ch=wchar_t(ch-32);
  for(const wchar_t* name:{L"CON",L"PRN",L"AUX",L"NUL",L"CONIN$",L"CONOUT$"})if(stem==name)return true;
  return stem.size()==4&&(stem.compare(0,3,L"COM")==0||stem.compare(0,3,L"LPT")==0)&&stem[3]>=L'0'&&stem[3]<=L'9';
}
// A payload-relative path: plain names separated by / or \, never rooted,
// never ".." or "." and nothing Windows would alias (trailing dot/space,
// device names, streams). Returned with backslashes.
inline std::optional<std::wstring> safeRelative(const std::wstring& value){
  if(value.empty()||value.size()>MaxRelative)return std::nullopt;
  std::wstring out;size_t start=0;
  for(;;){
    size_t end=value.find_first_of(L"/\\",start);if(end==std::wstring::npos)end=value.size();
    const std::wstring segment=value.substr(start,end-start);
    if(segment.empty()||segment==L"."||segment==L".."||segment.front()==L' '||segment.back()==L' '||segment.back()==L'.'||segment.size()>255)return std::nullopt;
    for(const wchar_t ch:segment)if(ch<32||ch==L'<'||ch==L'>'||ch==L':'||ch==L'"'||ch==L'|'||ch==L'?'||ch==L'*')return std::nullopt;
    if(reservedName(segment))return std::nullopt;
    if(!out.empty())out+=L'\\';
    out+=segment;
    if(end==value.size())break;
    start=end+1;
  }
  return out;
}

// --- Handle helpers ----------------------------------------------------------
inline DWORD attributesOf(HANDLE handle){
  FILE_ATTRIBUTE_TAG_INFO info{};
  if(!GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&info,sizeof(info)))throw Failure{"payload_identity_unavailable"};
  return info.FileAttributes;
}
struct Identity {unsigned long long volume=0;std::array<unsigned char,16> id{};};
inline bool sameIdentity(const Identity& a,const Identity& b){return a.volume==b.volume&&a.id==b.id;}
inline Identity identityOf(HANDLE handle){
  Identity out;FILE_ID_INFO info{};
  if(GetFileInformationByHandleEx(handle,FileIdInfo,&info,sizeof(info))){out.volume=info.VolumeSerialNumber;std::memcpy(out.id.data(),&info.FileId,sizeof(info.FileId));return out;}
  BY_HANDLE_FILE_INFORMATION legacy{};
  if(!GetFileInformationByHandle(handle,&legacy))throw Failure{"payload_identity_unavailable"};
  out.volume=legacy.dwVolumeSerialNumber;std::memcpy(out.id.data(),&legacy.nFileIndexLow,4);std::memcpy(out.id.data()+4,&legacy.nFileIndexHigh,4);
  return out;
}
// A directory itself, never what a link points to.
inline Handle openDirectory(const std::wstring& path,DWORD access){
  Handle directory(CreateFileW(path.c_str(),access|FILE_READ_ATTRIBUTES|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(!directory)throw Failure{"payload_destination_missing"};
  const DWORD attributes=attributesOf(directory.get());
  if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)throw Failure{"payload_link_refused"};
  if(!(attributes&FILE_ATTRIBUTE_DIRECTORY))throw Failure{"payload_destination_missing"};
  return directory;
}
// [path] and every ancestor up to [top] (the drive root when empty) are plain
// directories: a junction anywhere above also redirects an ordinary child.
inline void plainChain(std::wstring path,const std::wstring& top){
  for(;;){
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY))throw Failure{"payload_destination_missing"};
    if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)throw Failure{"payload_link_refused"};
    if(path.size()<=3||(!top.empty()&&sameText(path,top)))return;
    const size_t slash=path.find_last_of(L'\\');
    if(slash==std::wstring::npos)return;
    path.resize(slash<3?3:slash);
  }
}
// POSIX semantics (FileDispositionInfoEx, Windows 10 1607+) free the name at
// once even while a scanner still holds the file open; otherwise the classic
// delete-on-close.
inline bool deleteByHandle(HANDLE handle){
  struct {DWORD Flags;} posix{0x1|0x2};  // FILE_DISPOSITION_FLAG_DELETE | _POSIX_SEMANTICS
  if(SetFileInformationByHandle(handle,static_cast<FILE_INFO_BY_HANDLE_CLASS>(21),&posix,sizeof(posix)))return true;  // FileDispositionInfoEx
  FILE_DISPOSITION_INFO info{};info.DeleteFile=TRUE;
  return SetFileInformationByHandle(handle,FileDispositionInfo,&info,sizeof(info))!=FALSE;
}
// Renames exactly the open file to [path] (a full \\?\ path in its own
// directory) and confirms where it now is; never replaces an existing name.
// A bare name would be resolved against the current directory, not the
// file's own directory, so only full paths are passed.
inline bool renameByHandle(HANDLE handle,const std::wstring& path){
  if(path.compare(0,4,L"\\\\?\\")!=0)return false;
  std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO)+path.size()*sizeof(wchar_t),0);
  auto* info=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  info->ReplaceIfExists=FALSE;info->RootDirectory=nullptr;info->FileNameLength=DWORD(path.size()*sizeof(wchar_t));
  std::memcpy(info->FileName,path.data(),path.size()*sizeof(wchar_t));
  if(!SetFileInformationByHandle(handle,FileRenameInfo,info,DWORD(buffer.size())))return false;
  if(!sameText(finalPath(handle),path))throw Failure{"payload_link_refused"};
  return true;
}
inline std::wstring randomTag(){
  unsigned char bytes[6]{};
  if(BCryptGenRandom(nullptr,bytes,sizeof(bytes),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw Failure{"payload_hash_unavailable"};
  constexpr wchar_t hex[]=L"0123456789abcdef";std::wstring out;
  for(const unsigned char b:bytes){out+=hex[b>>4];out+=hex[b&15];}
  return out;
}
inline Digest hashOf(HANDLE handle,unsigned long long expected){
  LARGE_INTEGER zero{};
  if(!SetFilePointerEx(handle,zero,nullptr,FILE_BEGIN))throw Failure{"payload_verify_failed"};
  std::vector<unsigned char> buffer(1<<20);Sha256 hash;unsigned long long total=0;
  for(;;){
    DWORD read=0;
    if(!ReadFile(handle,buffer.data(),DWORD(buffer.size()),&read,nullptr))throw Failure{"payload_verify_failed"};
    if(!read)break;
    total+=read;if(total>expected)break;
    hash.add(buffer.data(),read);
  }
  if(total!=expected)return Digest{};
  return hash.finish();
}

// Administrators/SYSTEM (or TrustedInstaller) own it and nobody else may write.
inline bool sidText(PSID sid,const wchar_t* text){
  PSID expected=nullptr;
  if(!ConvertStringSidToSidW(text,&expected))return false;
  const bool same=EqualSid(sid,expected)!=FALSE;LocalFree(expected);return same;
}
constexpr const wchar_t* TrustedInstaller=L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";
inline void protectedDirectory(HANDLE handle){
  PSID owner=nullptr;PACL dacl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
  if(GetSecurityInfo(handle,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&dacl,nullptr,&descriptor)!=ERROR_SUCCESS)throw Failure{"payload_destination_unprotected"};
  struct Free {PSECURITY_DESCRIPTOR value;~Free(){LocalFree(value);}} release{descriptor};
  if(!owner||!(sidText(owner,L"S-1-5-18")||sidText(owner,L"S-1-5-32-544")||sidText(owner,TrustedInstaller))||!dacl)throw Failure{"payload_destination_unprotected"};
  const ACCESS_MASK write=FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES|DELETE|FILE_DELETE_CHILD|WRITE_DAC|WRITE_OWNER|GENERIC_WRITE|GENERIC_ALL;
  for(DWORD index=0;index<dacl->AceCount;++index){
    void* raw=nullptr;
    if(!GetAce(dacl,index,&raw))throw Failure{"payload_destination_unprotected"};
    const auto* header=static_cast<ACE_HEADER*>(raw);
    if(header->AceFlags&INHERIT_ONLY_ACE)continue;
    if(header->AceType==ACCESS_DENIED_ACE_TYPE)continue;
    if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE)throw Failure{"payload_destination_unprotected"};
    const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
    const PSID sid=const_cast<DWORD*>(&ace->SidStart);
    if((ace->Mask&write)&&!(sidText(sid,L"S-1-5-18")||sidText(sid,L"S-1-5-32-544")||sidText(sid,L"S-1-3-0")||sidText(sid,TrustedInstaller)))throw Failure{"payload_destination_unprotected"};
  }
}
inline void protectedChain(std::wstring path,const std::wstring& top){
  if(!canonicalPath(path)||!canonicalPath(top)||(!sameText(path,top)&&!startsWithText(path,top+L"\\")))throw Failure{"payload_path_unverified"};
  for(;;){auto directory=openDirectory(path,READ_CONTROL);protectedDirectory(directory.get());if(sameText(path,top))break;const auto slash=path.find_last_of(L'\\');if(slash==std::wstring::npos)throw Failure{"payload_path_unverified"};path.resize(slash);}
}

// --- Placement ---------------------------------------------------------------
struct Entry {std::wstring relative;unsigned long long size=0;Digest sha256{};};
struct Plan {
  std::wstring source;          // the setup's own payload directory (bin)
  std::wstring destination;     // where the files go
  std::wstring chainTop;        // highest ancestor that must not be a link ("" = the drive root)
  std::wstring requiredMarker;  // a file the script already wrote into the destination (ownership)
  bool protectedDestination=false; // destination and its parent: Administrators/SYSTEM-only write
  std::vector<Entry> entries;
};
struct Result {bool ok=false;std::string code;size_t placed=0,kept=0;bool restored=true;};

// What this request changed, undone in reverse order on failure.
class Journal {
 public:
  void directory(const std::wstring& path,const Identity& id){changes_.push_back({Kind::Directory,path,{},id,{},false});}
  void temporary(const std::wstring& path,const Identity& id){temporary_=Change{Kind::NewFile,path,{},id,{},false};}
  void dropTemporary(){temporary_.reset();}
  void created(const std::wstring& path){if(!temporary_)return;auto change=*temporary_;change.path=path;temporary_.reset();changes_.push_back(change);}
  void replaced(const std::wstring& path,const std::wstring& backup,const Identity& old){changes_.push_back({Kind::Backup,path,backup,{},old,false});}
  void replacedWith(){if(!temporary_||changes_.empty())return;changes_.back().id=temporary_->id;changes_.back().hasNew=true;temporary_.reset();}
  // The replaced originals are no longer needed once every file is in place.
  void commit(){
    for(const auto& change:changes_)if(change.kind==Kind::Backup){Handle backup=open(change.backup);if(backup&&sameIdentity(identityOf(backup.get()),change.old))deleteByHandle(backup.get());}
    changes_.clear();temporary_.reset();
  }
  bool rollback()noexcept{
    bool restored=true;
    try{
      if(temporary_){restored&=remove(temporary_->path,temporary_->id);temporary_.reset();}
      for(auto at=changes_.rbegin();at!=changes_.rend();++at){
        if(at->kind==Kind::Directory||at->kind==Kind::NewFile){restored&=remove(at->path,at->id);continue;}
        if(at->hasNew)restored&=remove(at->path,at->id);
        Handle backup=open(at->backup);
        if(!backup||!sameIdentity(identityOf(backup.get()),at->old)||!renameByHandle(backup.get(),at->path))restored=false;
      }
    }catch(...){restored=false;}
    changes_.clear();
    return restored;
  }
 private:
  enum class Kind {Directory,NewFile,Backup};
  struct Change {Kind kind;std::wstring path,backup;Identity id,old;bool hasNew;};
  std::vector<Change> changes_;std::optional<Change> temporary_;
  static Handle open(const std::wstring& path){return Handle(CreateFileW(path.c_str(),DELETE|FILE_READ_ATTRIBUTES|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));}
  // Only the exact entry this request made; a directory only when empty.
  static bool remove(const std::wstring& path,const Identity& id){
    Handle entry=open(path);
    if(!entry)return GetLastError()==ERROR_FILE_NOT_FOUND;
    if(!sameIdentity(identityOf(entry.get()),id))return false;
    return deleteByHandle(entry.get());
  }
};

class Placement {
 public:
  explicit Placement(const Plan& plan):plan_(plan){}
  Result run(){
    Result result;
    try{
      if(plan_.entries.empty()||plan_.entries.size()>MaxEntries)throw Failure{"payload_request_invalid"};
      std::set<std::wstring,NoCase> names;
      for(const auto& entry:plan_.entries)if(entry.size>MaxFileSize||!names.insert(entry.relative).second)throw Failure{"payload_request_invalid"};
      if(!canonicalPath(plan_.source)||!canonicalPath(plan_.destination)||(!plan_.chainTop.empty()&&!canonicalPath(plan_.chainTop)))throw Failure{"payload_unavailable"};
      Handle sourceRoot=openDirectory(plan_.source,FILE_LIST_DIRECTORY);
      sourceBase_=finalPath(sourceRoot.get());
      plainChain(plan_.destination,plan_.chainTop);
      Handle destination=openDirectory(plan_.destination,FILE_LIST_DIRECTORY|READ_CONTROL);
      base_=finalPath(destination.get());
      if(!sameText(base_,expectedFinal(plan_.destination)))throw Failure{"payload_link_refused"};
      if(plan_.protectedDestination){
        protectedDirectory(destination.get());
        const size_t slash=plan_.destination.find_last_of(L'\\');
        Handle parent=openDirectory(plan_.destination.substr(0,slash<3?3:slash),FILE_LIST_DIRECTORY|READ_CONTROL);
        protectedDirectory(parent.get());
      }
      if(!plan_.requiredMarker.empty()){
        Handle marker(CreateFileW((base_+L"\\"+plan_.requiredMarker).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!marker||(attributesOf(marker.get())&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))throw Failure{"payload_destination_unowned"};
      }
      tag_=randomTag();
      for(const auto& entry:plan_.entries)place(entry,result);
      journal_.commit();
      result.ok=true;
    }catch(const Failure& failure){result.code=failure.code;result.restored=journal_.rollback();result.placed=result.kept=0;}
    catch(...){result.code="payload_failed";result.restored=journal_.rollback();result.placed=result.kept=0;}
    return result;
  }
 private:
  const Plan& plan_;Journal journal_;std::wstring sourceBase_,base_,tag_;std::set<std::wstring,NoCase> directories_;
  // Existing directories must be plain; missing ones are made (and removed on rollback).
  void directory(const std::wstring& path){
    if(directories_.count(path))return;
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){
      const DWORD error=GetLastError();
      if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)throw Failure{"payload_directory_failed"};
      if(!CreateDirectoryW(path.c_str(),nullptr))throw Failure{"payload_directory_failed"};
      Handle made=openDirectory(path,DELETE);
      journal_.directory(path,identityOf(made.get()));
      if(!sameText(finalPath(made.get()),path))throw Failure{"payload_link_refused"};
    }else{
      if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)throw Failure{"payload_link_refused"};
      if(!(attributes&FILE_ATTRIBUTE_DIRECTORY))throw Failure{"payload_target_unsafe"};
      Handle existing=openDirectory(path,0);
      if(!sameText(finalPath(existing.get()),path))throw Failure{"payload_link_refused"};
    }
    directories_.insert(path);
  }
  void place(const Entry& entry,Result& result){
    size_t start=0;std::wstring parent=base_;
    for(;;){const size_t slash=entry.relative.find(L'\\',start);if(slash==std::wstring::npos)break;parent=base_+L"\\"+entry.relative.substr(0,slash);directory(parent);start=slash+1;}
    const std::wstring leaf=entry.relative.substr(start),sourcePath=sourceBase_+L"\\"+entry.relative,targetPath=base_+L"\\"+entry.relative,temporaryPath=parent+L"\\"+leaf+L"."+tag_+L".multitap-new";
    // The source: the exact listed file, held against writers while it is read.
    Handle source(CreateFileW(sourcePath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(!source)throw Failure{GetLastError()==ERROR_SHARING_VIOLATION?"payload_source_busy":"payload_source_missing"};
    if((attributesOf(source.get())&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!sameText(finalPath(source.get()),sourcePath))throw Failure{"payload_source_invalid"};
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(source.get(),&size)||static_cast<unsigned long long>(size.QuadPart)!=entry.size)throw Failure{"payload_size_mismatch"};
    FILETIME written{};const bool timed=GetFileTime(source.get(),nullptr,nullptr,&written)!=FALSE;
    // A new file beside the target: the target name changes only by rename.
    Handle temporary(CreateFileW(temporaryPath.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!temporary)throw Failure{"payload_write_failed"};
    journal_.temporary(temporaryPath,identityOf(temporary.get()));
    if(!sameText(finalPath(temporary.get()),temporaryPath))throw Failure{"payload_link_refused"};
    {
      std::vector<unsigned char> buffer(1<<20);Sha256 hash;unsigned long long total=0;
      for(;;){
        DWORD read=0;
        if(!ReadFile(source.get(),buffer.data(),DWORD(buffer.size()),&read,nullptr))throw Failure{"payload_source_unreadable"};
        if(!read)break;
        total+=read;if(total>entry.size)throw Failure{"payload_size_mismatch"};
        hash.add(buffer.data(),read);
        DWORD copied=0;
        if(!WriteFile(temporary.get(),buffer.data(),read,&copied,nullptr)||copied!=read)throw Failure{"payload_write_failed"};
      }
      if(total!=entry.size)throw Failure{"payload_size_mismatch"};
      if(hash.finish()!=entry.sha256)throw Failure{"payload_hash_mismatch"};
    }
    if(timed)SetFileTime(temporary.get(),nullptr,nullptr,&written);
    if(!FlushFileBuffers(temporary.get()))throw Failure{"payload_write_failed"};
    // What is on disk now, not what was meant to be written.
    if(hashOf(temporary.get(),entry.size)!=entry.sha256)throw Failure{"payload_verify_failed"};
    Handle existing(CreateFileW(targetPath.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(!existing){
      const DWORD error=GetLastError();
      if(error!=ERROR_FILE_NOT_FOUND)throw Failure{error==ERROR_SHARING_VIOLATION?"payload_target_busy":"payload_target_unsafe"};
      if(!renameByHandle(temporary.get(),targetPath))throw Failure{"payload_write_failed"};
      journal_.created(targetPath);
      ++result.placed;return;
    }
    if((attributesOf(existing.get())&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!sameText(finalPath(existing.get()),targetPath))throw Failure{"payload_target_unsafe"};
    LARGE_INTEGER present{};
    if(GetFileSizeEx(existing.get(),&present)&&static_cast<unsigned long long>(present.QuadPart)==entry.size&&hashOf(existing.get(),entry.size)==entry.sha256){
      // The same version's identical file stays as it is.
      if(!deleteByHandle(temporary.get()))throw Failure{"payload_write_failed"};
      temporary.reset();journal_.dropTemporary();
      ++result.kept;return;
    }
    const std::wstring backupLeaf=leaf+L"."+tag_+L".multitap-old";
    const Identity old=identityOf(existing.get());
    if(!renameByHandle(existing.get(),parent+L"\\"+backupLeaf))throw Failure{"payload_target_busy"};
    journal_.replaced(targetPath,parent+L"\\"+backupLeaf,old);
    if(!renameByHandle(temporary.get(),targetPath))throw Failure{"payload_write_failed"};
    journal_.replacedWith();
    ++result.placed;
  }
};
inline Result Place(const Plan& plan){return Placement(plan).run();}

// --- Shortcuts ---------------------------------------------------------------
struct ClientShortcutRoot {std::wstring path,executable,marker;std::string product;bool protectedFiles=true;};
inline bool ownedClientTarget(const std::wstring&,const std::vector<ClientShortcutRoot>&);
struct Shortcut {
  std::wstring path,boundary,target,arguments,directory,description,icon;
  bool setArguments=true; // the client keeps an existing shortcut's arguments, like WScript.Shell did
  // The client's installations: an existing shortcut is this product's own
  // only for an exact marked folder, executable name and verified plain
  // filesystem chain (ownedClientTarget). Empty: the boundary above decides.
  std::vector<ClientShortcutRoot> clientRoots;
};
struct ShortcutResult {bool ok=false;std::string code;bool existed=false;std::vector<char> previous;};
template<class T> class Com {
 public:
  Com()=default;
  Com(const Com&)=delete;
  Com& operator=(const Com&)=delete;
  ~Com(){if(value_)value_->Release();}
  T** put(){return &value_;}
  T* get()const{return value_;}
  T* operator->()const{return value_;}
  explicit operator bool()const{return value_!=nullptr;}
 private:
  T* value_=nullptr;
};
class Apartment {
 public:
  Apartment():result_(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE)){}
  Apartment(const Apartment&)=delete;
  Apartment& operator=(const Apartment&)=delete;
  ~Apartment(){if(SUCCEEDED(result_))CoUninitialize();}
 private:
  HRESULT result_;
};
inline bool plainFile(const std::wstring& path){
  Handle file(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
  return file&&!(attributesOf(file.get())&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
}
// Writes [bytes] beside [path] and renames it over the name: an existing link
// at [path] is replaced, never followed.
inline void replaceFile(const std::wstring& path,const std::vector<char>& bytes){
  const std::wstring temporary=path+L"."+randomTag()+L".multitap-new";
  {
    Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file)throw Failure{"shortcut_write_failed"};
    DWORD written=0;
    if(!WriteFile(file.get(),bytes.data(),DWORD(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(file.get())){deleteByHandle(file.get());throw Failure{"shortcut_write_failed"};}
  }
  if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temporary.c_str());throw Failure{"shortcut_write_failed"};}
}
// The current bytes of an existing shortcut (a plain file), or nullopt when absent.
inline std::optional<std::vector<char>> shortcutBytes(const std::wstring& path){
  Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
  if(!file){if(GetLastError()==ERROR_FILE_NOT_FOUND)return std::nullopt;throw Failure{"shortcut_unsafe"};}
  if(attributesOf(file.get())&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))throw Failure{"shortcut_unsafe"};
  BY_HANDLE_FILE_INFORMATION info{};
  if(!GetFileInformationByHandle(file.get(),&info)||info.nNumberOfLinks!=1||!sameText(finalPath(file.get()),expectedFinal(path)))throw Failure{"shortcut_unsafe"};
  LARGE_INTEGER size{};
  if(!GetFileSizeEx(file.get(),&size)||size.QuadPart>(1<<20))throw Failure{"shortcut_unreadable"};
  std::vector<char> bytes(size_t(size.QuadPart));DWORD read=0;
  if(!bytes.empty()&&(!ReadFile(file.get(),bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size()))throw Failure{"shortcut_unreadable"};
  return bytes;
}
inline void loadShortcut(IShellLinkW* link,const std::vector<char>& bytes){
  Com<IPersistStream> persist;Com<IStream> stream;
  if(FAILED(link->QueryInterface(IID_PPV_ARGS(persist.put()))))throw Failure{"shortcut_unavailable"};
  *stream.put()=SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()),UINT(bytes.size()));
  if(!stream||FAILED(persist->Load(stream.get())))throw Failure{"shortcut_unreadable"};
}
// The target an existing shortcut names, as WScript.Shell's TargetPath read it.
inline std::wstring shortcutTarget(const std::vector<char>& bytes){
  Apartment apartment;Com<IShellLinkW> link;
  if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put()))))throw Failure{"shortcut_unavailable"};
  loadShortcut(link.get(),bytes);
  std::wstring target(32768,L'\0');
  if(FAILED(link->GetPath(target.data(),int(target.size()),nullptr,0)))return {};
  target.resize(std::wcslen(target.c_str()));
  return target;
}
inline bool insideBoundary(const std::wstring& target,const std::wstring& boundary){
  std::wstring full(32768,L'\0');
  const DWORD length=GetFullPathNameW(target.c_str(),DWORD(full.size()),full.data(),nullptr);
  if(!length||length>=full.size())return false;
  full.resize(length);
  return startsWithText(full,boundary+L"\\");
}
inline ShortcutResult WriteShortcut(const Shortcut& shortcut){
  ShortcutResult result;
  try{
    Apartment apartment;
    const size_t slash=shortcut.path.find_last_of(L'\\');
    if(!canonicalPath(shortcut.path)||!canonicalPath(shortcut.boundary)||slash==std::wstring::npos)throw Failure{"shortcut_unavailable"};
    const DWORD folder=GetFileAttributesW(shortcut.path.substr(0,slash<3?3:slash).c_str());
    if(folder==INVALID_FILE_ATTRIBUTES||!(folder&FILE_ATTRIBUTE_DIRECTORY))throw Failure{"shortcut_folder_missing"};
    if(folder&FILE_ATTRIBUTE_REPARSE_POINT)throw Failure{"shortcut_unsafe"};
    if(!plainFile(shortcut.target)||!plainFile(shortcut.icon))throw Failure{"shortcut_target_missing"};
    Com<IShellLinkW> link;
    if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put()))))throw Failure{"shortcut_unavailable"};
    if(const auto previous=shortcutBytes(shortcut.path)){
      // An existing shortcut is updated (its other settings stay) only when it
      // already points into this product.
      result.existed=true;result.previous=*previous;
      loadShortcut(link.get(),*previous);
      std::wstring current(32768,L'\0');
      if(SUCCEEDED(link->GetPath(current.data(),int(current.size()),nullptr,0))){
        current.resize(std::wcslen(current.c_str()));
        if(!shortcut.clientRoots.empty()){
          if(!ownedClientTarget(current,shortcut.clientRoots))throw Failure{"shortcut_foreign"};
        }else if(!current.empty()&&!insideBoundary(current,shortcut.boundary))throw Failure{"shortcut_foreign"};
      }
    }
    if(FAILED(link->SetPath(shortcut.target.c_str()))||(shortcut.setArguments&&FAILED(link->SetArguments(shortcut.arguments.c_str())))||
       FAILED(link->SetWorkingDirectory(shortcut.directory.c_str()))||FAILED(link->SetDescription(shortcut.description.c_str()))||
       FAILED(link->SetIconLocation(shortcut.icon.c_str(),0)))throw Failure{"shortcut_write_failed"};
    // Elevation comes from the program's own manifest (the client runs
    // unelevated): a shortcut never forces "Run as
    // administrator", and a flag an earlier shortcut carried is cleared.
    {Com<IShellLinkDataList> data;DWORD flags=0;
     if(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(data.put())))&&SUCCEEDED(data->GetFlags(&flags))&&(flags&SLDF_RUNAS_USER)&&FAILED(data->SetFlags(flags&~DWORD(SLDF_RUNAS_USER))))throw Failure{"shortcut_write_failed"};}
    Com<IPersistStream> persist;Com<IStream> stream;
    if(FAILED(link->QueryInterface(IID_PPV_ARGS(persist.put())))||FAILED(CreateStreamOnHGlobal(nullptr,TRUE,stream.put()))||FAILED(persist->Save(stream.get(),TRUE)))throw Failure{"shortcut_write_failed"};
    STATSTG stat{};LARGE_INTEGER zero{};
    if(FAILED(stream->Stat(&stat,STATFLAG_NONAME))||stat.cbSize.QuadPart<=0||stat.cbSize.QuadPart>(1<<20)||FAILED(stream->Seek(zero,STREAM_SEEK_SET,nullptr)))throw Failure{"shortcut_write_failed"};
    std::vector<char> bytes(size_t(stat.cbSize.QuadPart));ULONG read=0;
    if(FAILED(stream->Read(bytes.data(),ULONG(bytes.size()),&read))||read!=bytes.size())throw Failure{"shortcut_write_failed"};
    replaceFile(shortcut.path,bytes);
    SHChangeNotify(result.existed?SHCNE_UPDATEITEM:SHCNE_CREATE,SHCNF_PATHW,shortcut.path.c_str(),nullptr);
    result.ok=true;
  }catch(const Failure& failure){result.code=failure.code;}
  catch(...){result.code="shortcut_write_failed";}
  return result;
}
// Puts the exact earlier shortcut back (the client keeps starting its
// previous version when the new one could not be completed).
inline std::string RestoreShortcut(const std::wstring& path,const std::vector<char>& bytes){
  try{
    if(!canonicalPath(path))return "shortcut_unavailable";
    (void)shortcutBytes(path);
    replaceFile(path,bytes);
    SHChangeNotify(SHCNE_UPDATEITEM,SHCNF_PATHW,path.c_str(),nullptr);
    return {};
  }catch(const Failure& failure){return failure.code;}
  catch(...){return "shortcut_write_failed";}
}

// --- The per-user client of an earlier layout ---------------------------------
// Early releases installed the (administrator-required) client under
// %LOCALAPPDATA%\Multitap\Client\<version>, where any program of the user can
// replace it. Once the new installation under Program Files is verified, the
// setup removes exactly those version folders: a plain folder named x.y.z
// holding this product's own marker. The user's data in the same folder
// (settings.json, login.json, launcher.json, session.json), the other
// %LOCALAPPDATA%\Multitap folders and Credential Manager stay as they are.
//
// Everything is removed by handle: each entry is opened without following a
// link and must resolve to the exact final path expected below the verified
// folder; a link is removed as the link itself and never entered. A folder in
// use (a running client) is not touched: it is first renamed as a whole,
// which Windows refuses while a file inside is open, and kept otherwise.
inline bool jsonFieldIs(const std::string& text,const std::string& field,const std::string& value){
  const std::string key="\""+field+"\"";
  for(size_t at=text.find(key);at!=std::string::npos;at=text.find(key,at+1)){
    size_t i=at+key.size();
    while(i<text.size()&&(text[i]==' '||text[i]=='\t'||text[i]=='\r'||text[i]=='\n'))++i;
    if(i>=text.size()||text[i]!=':')continue;
    ++i;while(i<text.size()&&(text[i]==' '||text[i]=='\t'||text[i]=='\r'||text[i]=='\n'))++i;
    if(text.compare(i,value.size()+2,"\""+value+"\"")==0)return true;
  }
  return false;
}
// The client's ownership marker (written by Install-Client.ps1): a plain file
// of at most 4 KiB naming this product and exactly [version].
inline bool clientMarker(const std::wstring& directory,const std::string& version){
  Handle file(CreateFileW((directory+L"\\multitap-client.json").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(!file||(attributesOf(file.get())&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return false;
  LARGE_INTEGER size{};if(!GetFileSizeEx(file.get(),&size)||size.QuadPart<2||size.QuadPart>4096)return false;
  std::string text(size_t(size.QuadPart),'\0');DWORD read=0;
  if(!ReadFile(file.get(),text.data(),DWORD(text.size()),&read,nullptr)||read!=text.size())return false;
  return jsonFieldIs(text,"product","Multitap")&&jsonFieldIs(text,"version",version);
}
inline bool versionName(const std::wstring& name){
  int parts=0,digits=0;
  for(const wchar_t ch:name){if(ch==L'.'){if(!digits||++parts>2)return false;digits=0;}else if(ch>=L'0'&&ch<=L'9'){if(++digits>6)return false;}else return false;}
  return parts==2&&digits>0;
}
// The ownership marker is a flat JSON object of strings. Parse the complete
// bounded document; substrings and duplicate fields must not establish
// ownership of another program.
inline bool clientOwnershipText(const std::string& text,const std::string& product,const std::string& version){
  size_t at=text.compare(0,3,"\xef\xbb\xbf")==0?3:0;
  const auto space=[&]{while(at<text.size()&&(text[at]==' '||text[at]=='\t'||text[at]=='\r'||text[at]=='\n'))++at;};
  const auto take=[&](char c){space();if(at>=text.size()||text[at]!=c)return false;++at;return true;};
  const auto string=[&](std::string& out){
    if(!take('"'))return false;
    while(at<text.size()){
      char c=text[at++];if(c=='"')return true;if(static_cast<unsigned char>(c)<32)return false;
      if(c=='\\'){if(at>=text.size())return false;c=text[at++];switch(c){case '"':case '\\':case '/':break;case 'b':c='\b';break;case 'f':c='\f';break;case 'n':c='\n';break;case 'r':c='\r';break;case 't':c='\t';break;default:return false;}}
      out+=c;
    }
    return false;
  };
  std::map<std::string,std::string> values;
  if(!take('{'))return false;
  do{std::string key,value;if(!string(key)||!take(':')||!string(value)||!values.emplace(key,value).second)return false;}while(take(','));
  if(!take('}'))return false;space();
  if(at!=text.size()||values["product"]!=product)return false;
  if(!version.empty())return values["version"]==version;
  // Empty [version]: any x.y.z (the fixed install folder's marker names the installed version).
  int parts=0,digits=0;
  for(const char ch:values["version"]){if(ch=='.'){if(!digits||++parts>2)return false;digits=0;}else if(ch>='0'&&ch<='9'){if(++digits>6)return false;}else return false;}
  return parts==2&&digits>0;
}
inline bool ownedClientTarget(const std::wstring& target,const std::vector<ClientShortcutRoot>& roots){
  try{
    if(!canonicalPath(target))return false;
    for(const auto& root:roots){
      if(!canonicalPath(root.path)||!insideBoundary(target,root.path))continue;
      const auto relative=target.substr(root.path.size()+1);const auto slash=relative.find(L'\\');
      // <root>\<executable>: the one fixed install folder, marker in the root.
      const bool fixed=slash==std::wstring::npos&&sameText(relative,root.executable);
      if(!fixed&&(slash==std::wstring::npos||!versionName(relative.substr(0,slash))||!sameText(relative.substr(slash+1),root.executable)))return false;
      const auto version=fixed?std::string():utf8(relative.substr(0,slash));const auto directory=fixed?root.path:root.path+L"\\"+relative.substr(0,slash);
      plainChain(directory,{});
      Handle folder=openDirectory(directory,READ_CONTROL),parent=openDirectory(root.path,READ_CONTROL);
      if(!sameText(finalPath(folder.get()),expectedFinal(directory))||!sameText(finalPath(parent.get()),expectedFinal(root.path)))return false;
      Handle marker(CreateFileW((directory+L"\\"+root.marker).c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
      Handle executable(CreateFileW(target.c_str(),FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
      if(!marker||!executable||((attributesOf(marker.get())|attributesOf(executable.get()))&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return false;
      BY_HANDLE_FILE_INFORMATION mi{},ei{};
      if(!GetFileInformationByHandle(marker.get(),&mi)||!GetFileInformationByHandle(executable.get(),&ei)||mi.nNumberOfLinks!=1||ei.nNumberOfLinks!=1||
         !sameText(finalPath(marker.get()),expectedFinal(directory+L"\\"+root.marker))||!sameText(finalPath(executable.get()),expectedFinal(target)))return false;
      if(root.protectedFiles){const auto at=root.path.find(L"\\Multitap\\Client");if(at==std::wstring::npos)return false;protectedChain(directory,root.path.substr(0,at));protectedDirectory(marker.get());protectedDirectory(executable.get());}
      LARGE_INTEGER size{};if(!GetFileSizeEx(marker.get(),&size)||size.QuadPart<2||size.QuadPart>4096)return false;
      std::string text(size_t(size.QuadPart),'\0');DWORD read=0;
      if(!ReadFile(marker.get(),text.data(),DWORD(text.size()),&read,nullptr)||read!=text.size())return false;
      return clientOwnershipText(text,root.product,version);
    }
  }catch(...){ }
  return false;
}
// "<x.y.z>.multitap-old-<12 hex>": a version folder an earlier run renamed but
// could not finish removing.
inline bool tombstoneName(const std::wstring& name){
  const size_t at=name.find(L".multitap-old-");
  if(at==std::wstring::npos||!versionName(name.substr(0,at))||name.size()!=at+13+12)return false;
  for(size_t i=at+13;i<name.size();++i){const wchar_t ch=name[i];if(!((ch>=L'0'&&ch<=L'9')||(ch>=L'a'&&ch<=L'f')))return false;}
  return true;
}
struct DirectoryEntry {std::wstring name;DWORD attributes=0;};
// The names in an open directory, read through its handle (never a path).
inline std::vector<DirectoryEntry> listByHandle(HANDLE directory){
  std::vector<DirectoryEntry> out;std::vector<unsigned char> buffer(64*1024);
  for(bool first=true;;first=false){
    if(!GetFileInformationByHandleEx(directory,first?FileFullDirectoryRestartInfo:FileFullDirectoryInfo,buffer.data(),DWORD(buffer.size()))){
      if(GetLastError()==ERROR_NO_MORE_FILES)break;
      throw Failure{"legacy_list_failed"};
    }
    for(size_t offset=0;;){
      const auto* info=reinterpret_cast<const FILE_FULL_DIR_INFO*>(buffer.data()+offset);
      const std::wstring name(info->FileName,info->FileNameLength/sizeof(wchar_t));
      if(name!=L"."&&name!=L"..")out.push_back({name,info->FileAttributes});
      if(out.size()>100000)throw Failure{"legacy_list_failed"};
      if(!info->NextEntryOffset)break;
      offset+=info->NextEntryOffset;
    }
  }
  return out;
}
// Removes the entry at [expected] (a \\?\ final path) and, for a plain
// folder, everything below it. False when anything stayed.
inline bool removeOwnedTree(const std::wstring& expected,int depth=0){
  if(depth>64)return false;
  try{
    Handle entry(CreateFileW(expected.c_str(),DELETE|FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!entry)return false;
    if(!sameText(finalPath(entry.get()),expected))return false;  // redirected: leave it
    const DWORD attributes=attributesOf(entry.get());
    if((attributes&FILE_ATTRIBUTE_DIRECTORY)&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT))
      for(const auto& child:listByHandle(entry.get()))if(!removeOwnedTree(expected+L"\\"+child.name,depth+1))return false;
    if(attributes&FILE_ATTRIBUTE_READONLY){
      FILE_BASIC_INFO basic{};basic.FileAttributes=(attributes&~FILE_ATTRIBUTE_READONLY)|((attributes&~FILE_ATTRIBUTE_READONLY)?0:FILE_ATTRIBUTE_NORMAL);
      SetFileInformationByHandle(entry.get(),FileBasicInfo,&basic,sizeof(basic));
    }
    return deleteByHandle(entry.get());
  }catch(...){return false;}
}
// The package the installer extracted under this user's %TEMP% (the setup's
// own payload folder), removed by handle once the installation is verified.
inline std::string RemoveExtractedSource(const std::wstring& source){
  try{
    if(!canonicalPath(source))return "payload_unavailable";
    wchar_t temp[MAX_PATH+1]{};const DWORD length=GetTempPathW(MAX_PATH+1,temp);
    if(!length||length>MAX_PATH)return "payload_unavailable";
    std::wstring tempRoot(temp,length);while(tempRoot.size()>3&&tempRoot.back()==L'\\')tempRoot.pop_back();
    const std::wstring expected=expectedFinal(source);
    if(!startsWithText(expected,expectedFinal(tempRoot)+L"\\"))return "source_not_temporary";
    return removeOwnedTree(expected)?std::string():std::string("source_remove_incomplete");
  }catch(const Failure& failure){return failure.code;}
  catch(...){return "source_remove_failed";}
}
struct LegacyResult {bool ok=false;std::string code;size_t removed=0,kept=0;};
// [installed]: the new installation's folder, which must hold this product's
// marker and a plain Multitap.exe (administrator-only unless [qa]) before
// anything of the old one is removed.
inline void verifyNewClient(const std::wstring& installed,const std::string& version,bool qa){
  Handle folder;
  try{folder=openDirectory(installed,READ_CONTROL);}catch(const Failure&){throw Failure{"legacy_new_install_unverified"};}
  if(!sameText(finalPath(folder.get()),expectedFinal(installed))||!clientMarker(installed,version)||!plainFile(installed+L"\\Multitap.exe"))throw Failure{"legacy_new_install_unverified"};
  if(!qa){
    protectedDirectory(folder.get());
    const size_t slash=installed.find_last_of(L'\\');
    Handle parent=openDirectory(installed.substr(0,slash),READ_CONTROL);protectedDirectory(parent.get());
  }
}
// Removes the marked version folders of [boundary]\Multitap\Client, except [keep].
inline void removeClientVersions(const std::wstring& boundary,const std::vector<std::wstring>& keep,LegacyResult& result){
  const std::wstring vendor=boundary+L"\\Multitap",root=vendor+L"\\Client";
  if(GetFileAttributesW(root.c_str())==INVALID_FILE_ATTRIBUTES){
    const DWORD error=GetLastError();
    if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return;
    throw Failure{"legacy_unavailable"};
  }
  plainChain(root,boundary);
  // Held without delete sharing: neither folder can be renamed or replaced
  // by a link while its version folders are removed.
  Handle vendorHandle(CreateFileW(vendor.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  Handle rootHandle(CreateFileW(root.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(!vendorHandle||!rootHandle)throw Failure{"legacy_unavailable"};
  if((attributesOf(vendorHandle.get())|attributesOf(rootHandle.get()))&FILE_ATTRIBUTE_REPARSE_POINT)throw Failure{"payload_link_refused"};
  const std::wstring base=finalPath(rootHandle.get());
  if(!sameText(base,expectedFinal(root))||!sameText(finalPath(vendorHandle.get()),expectedFinal(vendor)))throw Failure{"payload_link_refused"};
  for(const auto& child:listByHandle(rootHandle.get())){
    if(!(child.attributes&FILE_ATTRIBUTE_DIRECTORY)||(child.attributes&FILE_ATTRIBUTE_REPARSE_POINT))continue;
    const std::wstring path=base+L"\\"+child.name;
    if(tombstoneName(child.name)){if(removeOwnedTree(path))++result.removed;else ++result.kept;continue;}
    const auto narrowName=utf8(child.name);
    if(!versionName(child.name)||!clientMarker(path,narrowName))continue;
    if(std::any_of(keep.begin(),keep.end(),[&](const auto& name){return sameText(name,child.name);}))continue;
    bool removed=false;
    try{
      Handle folder(CreateFileW(path.c_str(),DELETE|FILE_READ_ATTRIBUTES|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
      if(folder&&!(attributesOf(folder.get())&FILE_ATTRIBUTE_REPARSE_POINT)&&sameText(finalPath(folder.get()),path)){
        const std::wstring tombstone=base+L"\\"+child.name+L".multitap-old-"+randomTag();
        if(renameByHandle(folder.get(),tombstone)){folder.reset();removed=removeOwnedTree(tombstone);}
      }
    }catch(...){removed=false;}
    if(removed)++result.removed;else ++result.kept;
  }
}
inline LegacyResult RemoveLegacyClient(const std::wstring& localAppData,const std::wstring& installed,const std::string& version,bool qa){
  LegacyResult result;
  try{
    if(!canonicalPath(localAppData)||!canonicalPath(installed))throw Failure{"payload_unavailable"};
    verifyNewClient(installed,version,qa);
    removeClientVersions(localAppData,{},result);
    result.ok=true;
  }catch(const Failure& failure){result.code=failure.code;}
  catch(...){result.code="legacy_failed";}
  return result;
}
// Older versions under Program Files: the client lives in the fixed
// Client folder itself, so every marked Client\<x.y.z> folder goes ([replaced]
// is kept in the request format only). A folder still in use stays for the next run.
inline LegacyResult RemoveOldClientVersions(const std::wstring& programFiles,const std::wstring& installed,const std::string& version,const std::string& replaced,bool qa){
  LegacyResult result;(void)replaced;
  try{
    if(!canonicalPath(programFiles)||!canonicalPath(installed))throw Failure{"payload_unavailable"};
    verifyNewClient(installed,version,qa);
    removeClientVersions(programFiles,{},result);
    result.ok=true;
  }catch(const Failure& failure){result.code=failure.code;}
  catch(...){result.code="legacy_failed";}
  return result;
}

// --- payload-v1 request channel ----------------------------------------------
// Lines from the script (all start with "-6|", which progress parsing refuses):
//   -6|place-v1|client|<version>|-
//   -6|file|<size>|<sha256 hex>|<relative path>          (repeated)
//   -6|commit
//   -6|shortcut-preflight-v1|client|<version>   (the desktop shortcut is this product's or absent)
//   -6|shortcut-v1|client|<version>
//   -6|shortcut-restore-v1|client
//   -6|legacy-remove-v1|client|<version>   (after the new client is verified)
//   -6|old-remove-v1|client|<version>|<replaced version>   (same, Program Files)
//   -6|source-remove-v1|client             (the extracted package under %TEMP%)
// One reply line per commit/shortcut request: "payload-v1|ok|<placed>|<kept>"
// (legacy-remove: "payload-v1|ok|<removed>|<kept>") or "payload-v1|error|<code>".
// Every destination is computed here from the roots and this setup's own
// version; the script names none of them. The client lives in
// %ProgramFiles%\Multitap\Client; %LOCALAPPDATA% only for the old per-user
// installation it replaces.
struct Roots {std::wstring source,programFiles,localAppData,desktop;bool qa=false;};
inline std::vector<ClientShortcutRoot> clientShortcutRoots(const Roots& roots){
  std::vector<ClientShortcutRoot> out;
  for(const auto& base:std::vector<std::pair<std::wstring,bool>>{{roots.programFiles,!roots.qa},{roots.localAppData,false}}){
    if(!canonicalPath(base.first))continue;
    out.push_back({base.first+L"\\Multitap\\Client",L"Multitap.exe",L"multitap-client.json","Multitap",base.second});
  }
  return out;
}
inline std::string clientShortcutPreflight(const Roots& roots){
  try{
    if(!canonicalPath(roots.desktop))return "shortcut_unavailable";
    plainChain(roots.desktop,{});
    if(const auto bytes=shortcutBytes(roots.desktop+L"\\Multitap.lnk"))
      if(!ownedClientTarget(shortcutTarget(*bytes),clientShortcutRoots(roots)))return "shortcut_foreign";
    return {};
  }catch(const Failure& failure){return failure.code;}catch(...){return "shortcut_unavailable";}
}
struct ServerStats {size_t requests=0,placed=0,kept=0,shortcuts=0,restores=0,failures=0,legacyRemoved=0,legacyKept=0;std::string lastCode;};
inline bool validVersion(const std::string& value){
  int parts=0,digits=0;
  for(const char ch:value){if(ch=='.'){if(!digits||++parts>2)return false;digits=0;}else if(ch>='0'&&ch<='9'){if(++digits>6)return false;}else return false;}
  return parts==2&&digits>0;
}
class Server {
 public:
  Server(Roots roots,std::string version):roots_(std::move(roots)),version_(std::move(version)){}
  const ServerStats& stats()const{return stats_;}
  // True when [line] belongs to payload-v1; [reply] is then empty or one line to send.
  bool Receive(std::string line,std::string& reply){
    reply.clear();
    if(line.size()<3||line.compare(0,3,"-6|")!=0)return false;
    if(line.back()=='\r')line.pop_back();
    std::vector<std::string> fields;size_t start=0;
    for(;;){const size_t bar=line.find('|',start);fields.push_back(line.substr(start,bar==std::string::npos?std::string::npos:bar-start));if(bar==std::string::npos||fields.size()>6)break;start=bar+1;}
    const std::string op=fields.size()>1?fields[1]:std::string();
    if(op=="file"){
      if(!open_){reply=error("payload_request_invalid");return true;}
      if(failure_.empty())failure_=addFile(fields);
      return true;
    }
    if(op=="commit"){
      if(!open_||fields.size()!=2){reply=error("payload_request_invalid");return true;}
      open_=false;++stats_.requests;
      if(!failure_.empty()){reply=error(failure_);failure_.clear();plan_=Plan{};return true;}
      const Result result=Place(plan_);plan_=Plan{};
      if(!result.ok){reply=error(result.code);return true;}
      stats_.placed+=result.placed;stats_.kept+=result.kept;
      reply="payload-v1|ok|"+std::to_string(result.placed)+"|"+std::to_string(result.kept)+"\n";
      return true;
    }
    // Anything else inside an open request spoils it; its commit answers once.
    if(open_){if(failure_.empty())failure_="payload_request_invalid";return true;}
    if(op=="place-v1"){open_=true;failure_=begin(fields);return true;}
    ++stats_.requests;
    if(op=="shortcut-preflight-v1"&&fields.size()==4&&fields[2]=="client"){
      if(fields[3]!=version_){reply=error("payload_version_invalid");return true;}
      const auto code=clientShortcutPreflight(roots_);reply=code.empty()?"payload-v1|ok\n":error(code);return true;
    }
    if(op=="shortcut-v1"&&fields.size()==4){reply=shortcut(fields[2],fields[3]);return true;}
    if(op=="legacy-remove-v1"&&fields.size()==4&&fields[2]=="client"){
      if(!validVersion(fields[3])||fields[3]!=version_){reply=error("payload_version_invalid");return true;}
      if(!canonicalPath(roots_.programFiles)||!canonicalPath(roots_.localAppData)){reply=error("payload_unavailable");return true;}
      const auto result=RemoveLegacyClient(roots_.localAppData,clientDirectory(std::wstring(version_.begin(),version_.end())),version_,roots_.qa);
      if(!result.ok){reply=error(result.code);return true;}
      stats_.legacyRemoved+=result.removed;stats_.legacyKept+=result.kept;
      reply="payload-v1|ok|"+std::to_string(result.removed)+"|"+std::to_string(result.kept)+"\n";return true;
    }
    if(op=="old-remove-v1"&&fields.size()==5&&fields[2]=="client"){
      if(!validVersion(fields[3])||fields[3]!=version_||!validVersion(fields[4])){reply=error("payload_version_invalid");return true;}
      const auto result=RemoveOldClientVersions(roots_.programFiles,clientDirectory(std::wstring(version_.begin(),version_.end())),version_,fields[4],roots_.qa);
      if(!result.ok){reply=error(result.code);return true;}
      stats_.legacyRemoved+=result.removed;stats_.legacyKept+=result.kept;
      reply="payload-v1|ok|"+std::to_string(result.removed)+"|"+std::to_string(result.kept)+"\n";return true;
    }
    if(op=="source-remove-v1"&&fields.size()==3&&fields[2]=="client"){
      const auto code=RemoveExtractedSource(roots_.source);
      reply=code.empty()?std::string("payload-v1|ok\n"):error(code);return true;
    }
    if(op=="shortcut-restore-v1"&&fields.size()==3&&fields[2]=="client"){
      if(!clientPrevious_){reply=error("shortcut_restore_unavailable");return true;}
      const auto code=RestoreShortcut(roots_.desktop+L"\\Multitap.lnk",*clientPrevious_);
      if(!code.empty()){reply=error(code);return true;}
      ++stats_.restores;reply="payload-v1|ok\n";return true;
    }
    // Further fixed requests the setup itself answers (setup_main.cpp SetupChannel).
    if(extension&&extension(fields,reply))return true;
    reply=error("payload_request_invalid");return true;
  }
  std::function<bool(const std::vector<std::string>&,std::string&)> extension;
 private:
  Roots roots_;std::string version_;ServerStats stats_;
  bool open_=false;std::string failure_;Plan plan_;std::set<std::wstring,NoCase> names_;
  std::optional<std::vector<char>> clientPrevious_;bool clientCaptured_=false;
  std::string error(const std::string& code){++stats_.failures;stats_.lastCode=code;return "payload-v1|error|"+code+"\n";}
  // One fixed folder (not Client\<version>); [version] stays in the signature.
  std::wstring clientDirectory(const std::wstring&)const{return roots_.programFiles+L"\\Multitap\\Client";}
  bool rootsReady()const{return canonicalPath(roots_.source)&&canonicalPath(roots_.programFiles);}
  std::string begin(const std::vector<std::string>& fields){
    plan_=Plan{};names_.clear();
    if(fields.size()!=5)return "payload_request_invalid";
    const std::string& kind=fields[2];
    if(!validVersion(fields[3])||fields[3]!=version_)return "payload_version_invalid";
    if(!rootsReady())return "payload_unavailable";
    const std::wstring version(fields[3].begin(),fields[3].end());
    plan_.source=roots_.source;
    if(kind=="client"&&fields[4]=="-"){
      // Administrator-only (never the
      // user-writable %LOCALAPPDATA%\Multitap\Client\<version>).
      plan_.destination=clientDirectory(version);
      plan_.chainTop=roots_.programFiles;
      plan_.requiredMarker=L"multitap-client.json";
      // The QA tree cannot be Administrators-owned; the unit cases cover the check.
      plan_.protectedDestination=!roots_.qa;
      return {};
    }
    return "payload_request_invalid";
  }
  std::string addFile(const std::vector<std::string>& fields){
    if(fields.size()!=5||plan_.entries.size()>=MaxEntries)return "payload_request_invalid";
    const std::string& size=fields[2];
    if(size.empty()||size.size()>12||size.find_first_not_of("0123456789")!=std::string::npos)return "payload_request_invalid";
    Entry entry;entry.size=std::stoull(size);
    const auto digest=parseDigest(fields[3]);const auto wide=widen(fields[4]);
    if(entry.size>MaxFileSize||!digest||!wide)return "payload_request_invalid";
    const auto relative=safeRelative(*wide);
    if(!relative||!names_.insert(*relative).second)return "payload_request_invalid";
    entry.sha256=*digest;entry.relative=*relative;plan_.entries.push_back(std::move(entry));
    return {};
  }
  std::string shortcut(const std::string& kind,const std::string& version){
    if(!validVersion(version)||version!=version_)return error("payload_version_invalid");
    const std::wstring wide(version.begin(),version.end());
    if(kind!="client")return error("payload_request_invalid");
    if(!canonicalPath(roots_.programFiles))return error("payload_unavailable");
    Shortcut link;const std::wstring directory=clientDirectory(wide);
    link.path=roots_.desktop+L"\\Multitap.lnk";link.boundary=roots_.programFiles+L"\\Multitap\\Client";link.target=directory+L"\\Multitap.exe";
    link.directory=directory;link.icon=link.target;link.description=multitap::setup_text::text(multitap::setup_text::Id::ClientShortcutDescription);link.setArguments=false;
    // The roots include the per-user installation (an earlier layout): its shortcut moves here.
    link.clientRoots=clientShortcutRoots(roots_);
    const auto result=WriteShortcut(link);
    if(!result.ok)return error(result.code);
    // The first change of this run keeps what the desktop had before it.
    if(!clientCaptured_){clientCaptured_=true;if(result.existed)clientPrevious_=result.previous;}
    ++stats_.shortcuts;
    return "payload-v1|ok\n";
  }
};

}  // namespace multitap::payload
