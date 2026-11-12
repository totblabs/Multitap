#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include "signin_diagnostic_handoff.hpp"
#include "json_object.hpp"
#include <algorithm>
#include <charconv>
#include <mutex>

namespace host::signin::handoff {
namespace {
constexpr size_t maximumRecord=16384;
[[noreturn]]void invalid(){throw std::runtime_error("Cleanup handoff is unavailable");}
template<size_t N>std::string hex(const std::array<uint8_t,N>& value){
  constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(N*2);
  for(auto byte:value){out+=digits[byte>>4];out+=digits[byte&15];}return out;
}
template<size_t N>std::array<uint8_t,N> unhex(const Json& value){
  const auto* text=std::get_if<std::string>(&value.value);if(!text||text->size()!=N*2)invalid();
  const auto digit=[](char c)->uint8_t{if(c>='0'&&c<='9')return uint8_t(c-'0');if(c>='a'&&c<='f')return uint8_t(c-'a'+10);invalid();};
  std::array<uint8_t,N> out{};for(size_t i=0;i<N;++i)out[i]=uint8_t(digit((*text)[2*i])*16+digit((*text)[2*i+1]));return out;
}
void fields(const Json& value,std::initializer_list<const char*> names){
  const auto* object=std::get_if<Json::Object>(&value.value);if(!object||object->size()!=names.size())invalid();
  for(const auto* name:names)if(!object->count(name))invalid();
}
uint64_t integer(const Json& value){
  const auto* text=std::get_if<std::string>(&value.value);uint64_t result=0;
  if(!text||text->empty()||text->size()>20||(text->size()>1&&text->front()=='0'))invalid();
  const auto parsed=std::from_chars(text->data(),text->data()+text->size(),result);
  if(parsed.ec!=std::errc{}||parsed.ptr!=text->data()+text->size())invalid();return result;
}
uint32_t number(const Json& value,uint32_t maximum=UINT32_MAX-1){
  const auto* item=std::get_if<double>(&value.value);
  if(!item||!std::isfinite(*item)||*item<1||*item>maximum||std::floor(*item)!=*item)invalid();return uint32_t(*item);
}
bool processValid(Process value){return signin::valid(value)&&value.pid<UINT32_MAX;}
bool identityValid(const FileIdentity& value,size_t maximum){
  return value.volume&&value.created&&value.bytes&&value.bytes<=maximum&&nonzero(value.fileId)&&nonzero(value.sha256);
}
bool instanceValid(const std::string& value){
  const std::string prefix="ROOT\\DISPLAY\\";
  return value.starts_with(prefix)&&value.size()>=prefix.size()+4&&value.size()<=prefix.size()+16&&
    std::all_of(value.begin()+prefix.size(),value.end(),[](char c){return(c>='0'&&c<='9')||(c>='A'&&c<='F');});
}
void sortChildren(std::vector<Child>& children){
  std::sort(children.begin(),children.end(),[](const Child& a,const Child& b){
    return a.process.pid!=b.process.pid?a.process.pid<b.process.pid:a.process.created<b.process.created;
  });
}
std::string processBytes(Process value){return "{\"pid\":"+std::to_string(value.pid)+",\"created\":"+jsonQuote(std::to_string(value.created))+'}';}
Process readProcess(const Json& value){fields(value,{"pid","created"});return{number(value.get("pid")),integer(value.get("created"))};}
std::string identityBytes(const FileIdentity& value){
  return "{\"volume\":"+jsonQuote(std::to_string(value.volume))+",\"fileId\":"+jsonQuote(hex(value.fileId))+
    ",\"created\":"+jsonQuote(std::to_string(value.created))+",\"bytes\":"+jsonQuote(std::to_string(value.bytes))+
    ",\"sha256\":"+jsonQuote(hex(value.sha256))+'}';
}
FileIdentity readIdentity(const Json& value){
  fields(value,{"volume","fileId","created","bytes","sha256"});
  return{integer(value.get("volume")),integer(value.get("created")),integer(value.get("bytes")),
    unhex<16>(value.get("fileId")),unhex<32>(value.get("sha256"))};
}
Nonce digest(const std::string& bytes){
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
  struct Close{BCRYPT_ALG_HANDLE& a;BCRYPT_HASH_HANDLE& h;~Close(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}}close{algorithm,hash};
  if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)invalid();
  DWORD size=0,returned=0;if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<BYTE*>(&size),sizeof(size),&returned,0)<0||!size||size>65536)invalid();
  std::vector<uint8_t> storage(size);Nonce result{};
  if(BCryptCreateHash(algorithm,&hash,storage.data(),size,nullptr,0,0)<0||
      BCryptHashData(hash,reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),ULONG(bytes.size()),0)<0||
      BCryptFinishHash(hash,result.data(),DWORD(result.size()),0)<0)invalid();return result;
}
struct FilePin final:Retention {
  HANDLE file=INVALID_HANDLE_VALUE;std::filesystem::path path;FileIdentity identity;std::string bytes;
  ~FilePin(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
};
struct DirectoryPin {
  HANDLE value=INVALID_HANDLE_VALUE;
  ~DirectoryPin(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
void plainAncestors(std::filesystem::path path){
  for(;;){const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))invalid();
    const auto parent=path.parent_path();if(parent==path||parent.empty())break;path=parent;}
}
std::shared_ptr<FilePin> readFile(const std::filesystem::path& path,const TrustedPath& trusted,size_t maximum){
  if(!trusted||!trusted(path.parent_path())||!trusted(path))return {};
  plainAncestors(path.parent_path());
  auto pin=std::make_shared<FilePin>();pin->path=path;
  pin->file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  if(pin->file==INVALID_HANDLE_VALUE)return {};
  BY_HANDLE_FILE_INFORMATION basic{};FILE_ID_INFO id{};LARGE_INTEGER size{};
  if(GetFileType(pin->file)!=FILE_TYPE_DISK||!GetFileInformationByHandle(pin->file,&basic)||basic.nNumberOfLinks!=1||
      (basic.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
      !GetFileInformationByHandleEx(pin->file,FileIdInfo,&id,sizeof(id))||!GetFileSizeEx(pin->file,&size)||size.QuadPart<=0||uint64_t(size.QuadPart)>maximum)return {};
  std::wstring actual(32768,L'\0');const DWORD count=GetFinalPathNameByHandleW(pin->file,actual.data(),DWORD(actual.size()),FILE_NAME_NORMALIZED);
  if(!count||count>=actual.size())return {};actual.resize(count);
  if(actual.starts_with(L"\\\\?\\"))actual.erase(0,4);
  if(CompareStringOrdinal(actual.c_str(),int(actual.size()),path.c_str(),int(path.native().size()),TRUE)!=CSTR_EQUAL)return {};
  pin->bytes.resize(size_t(size.QuadPart));DWORD read=0;
  if(!ReadFile(pin->file,pin->bytes.data(),DWORD(pin->bytes.size()),&read,nullptr)||read!=pin->bytes.size())return {};
  pin->identity.volume=id.VolumeSerialNumber;pin->identity.bytes=uint64_t(size.QuadPart);
  pin->identity.created=(uint64_t(basic.ftCreationTime.dwHighDateTime)<<32)|basic.ftCreationTime.dwLowDateTime;
  std::copy(std::begin(id.FileId.Identifier),std::end(id.FileId.Identifier),pin->identity.fileId.begin());
  pin->identity.sha256=digest(pin->bytes);
  if(!identityValid(pin->identity,maximum)||!trusted(path)||!trusted(path.parent_path()))return {};return pin;
}
bool snapshotValid(const Snapshot& value){
  return value.retained&&identityValid(value.identity,maximumRecord)&&value.identity.bytes==value.bytes.size()&&digest(value.bytes)==value.identity.sha256;
}
std::optional<OriginalObservation> originalFor(const Record& record,Authority& authority){
  const auto original=authority.original(record);
  if(!original||!original->retained||original->identity!=record.request.original)return {};return original;
}
struct ProducerProof {OriginalObservation original;std::shared_ptr<const Retention> gate;};
std::optional<ProducerProof> producer(const Record& record,Authority& authority){
  if(!authority.currentProducer(record)||authority.process(record,record.request.oldService)!=ProcessState::alive)return {};
  auto inventory=authority.ownedChildren(record);if(!inventory||!inventory->closedLaunchGate)return {};sortChildren(inventory->children);
  if(inventory->children!=record.request.children)return {};
  for(const auto& child:record.request.children)if(authority.process(record,child.process)!=ProcessState::exited)return {};
  const auto original=originalFor(record,authority);if(!original)return {};
  return ProducerProof{*original,inventory->closedLaunchGate};
}
std::optional<std::pair<Process,OriginalObservation>> consumer(const Record& record,Authority& authority){
  const auto current=authority.currentConsumer(record);
  if(!current||!processValid(*current)||*current==record.request.oldService||
      authority.process(record,*current)!=ProcessState::alive||authority.process(record,record.request.oldService)!=ProcessState::exited)return {};
  for(const auto& child:record.request.children)if(authority.process(record,child.process)!=ProcessState::exited)return {};
  const auto original=originalFor(record,authority);if(!original)return {};return std::pair{*current,*original};
}
} // namespace

bool valid(const Request& request){
  if(!processValid(request.oldService)||!nonzero(request.oldImageSha256)||!nonzero(request.operation)||!nonzero(request.stream)||
      !instanceValid(request.instance)||request.provider!="libvirtualdisplay-1.6.4"||!request.generation||
      !identityValid(request.original,65536)||request.children.empty()||request.children.size()>8)return false;
  unsigned preparers=0;
  for(size_t i=0;i<request.children.size();++i){const auto& child=request.children[i];
    if(!processValid(child.process)||child.process==request.oldService)return false;
    if(child.role==ChildRole::preparer)++preparers;else if(child.role!=ChildRole::restorer)return false;
    for(size_t j=0;j<i;++j)if(request.children[j].process.pid==child.process.pid)return false;
  }
  return preparers==1;
}
std::string canonical(const Record& record){
  if(!valid(record.request)||(record.phase!=Phase::intent&&record.phase!=Phase::handoffCommitted)||
      (record.phase==Phase::intent&&record.intentFile)||(record.phase==Phase::handoffCommitted&&
        (!record.intentFile||!identityValid(*record.intentFile,maximumRecord))))invalid();
  const auto& r=record.request;auto children=r.children;sortChildren(children);std::string list="[";
  for(const auto& child:children){if(list.size()>1)list+=',';list+="{\"process\":"+processBytes(child.process)+",\"role\":"+
    jsonQuote(child.role==ChildRole::preparer?"preparer":"restorer")+'}';}list+=']';
  auto result="{\"schema\":1,\"purpose\":\"sealed-original-cleanup-v1\",\"phase\":"+
    jsonQuote(record.phase==Phase::intent?"intent":"handoffCommitted")+",\"oldService\":"+processBytes(r.oldService)+
    ",\"oldImageSha256\":"+jsonQuote(hex(r.oldImageSha256))+",\"operation\":"+jsonQuote(hex(r.operation))+
    ",\"stream\":"+jsonQuote(hex(r.stream))+",\"instance\":"+jsonQuote(r.instance)+",\"provider\":"+jsonQuote(r.provider)+
    ",\"generation\":"+std::to_string(r.generation)+",\"original\":"+identityBytes(r.original)+",\"children\":"+list;
  if(record.intentFile)result+=",\"intentFile\":"+identityBytes(*record.intentFile);result+='}';
  if(result.size()>maximumRecord)invalid();return result;
}
std::optional<Record> parse(const std::string& bytes){
  try{
    if(bytes.empty()||bytes.size()>maximumRecord)return {};const auto root=JsonParser::parse(bytes);Record record;
    const auto phase=root.get("phase").string();
    if(phase=="intent")fields(root,{"schema","purpose","phase","oldService","oldImageSha256","operation","stream","instance","provider","generation","original","children"});
    else if(phase=="handoffCommitted"){record.phase=Phase::handoffCommitted;fields(root,{"schema","purpose","phase","oldService","oldImageSha256","operation","stream","instance","provider","generation","original","children","intentFile"});record.intentFile=readIdentity(root.get("intentFile"));}
    else return {};
    if(root.get("schema").number()!=1||root.get("purpose").string()!="sealed-original-cleanup-v1")return {};
    auto& r=record.request;r.oldService=readProcess(root.get("oldService"));r.oldImageSha256=unhex<32>(root.get("oldImageSha256"));
    r.operation=unhex<32>(root.get("operation"));r.stream=unhex<16>(root.get("stream"));r.instance=root.get("instance").string();
    r.provider=root.get("provider").string();r.generation=number(root.get("generation"),UINT32_MAX);r.original=readIdentity(root.get("original"));
    const auto* children=std::get_if<Json::Array>(&root.get("children").value);if(!children||children->empty()||children->size()>8)return {};
    for(const auto& value:*children){fields(value,{"process","role"});const auto role=value.get("role").string();
      if(role!="preparer"&&role!="restorer")return {};r.children.push_back({readProcess(value.get("process")),role=="preparer"?ChildRole::preparer:ChildRole::restorer});}
    if(canonical(record)!=bytes)return {};return record;
  }catch(...){return {};}
}
ProcessState observeKernelProcess(Process expected)noexcept{
  try{
    if(!processValid(expected))return ProcessState::unknown;
    HANDLE handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,expected.pid);
    if(!handle)return GetLastError()==ERROR_INVALID_PARAMETER?ProcessState::exited:ProcessState::unknown;
    struct Close{HANDLE value;~Close(){CloseHandle(value);}}close{handle};
    FILETIME created{},exited{},kernel{},user{};if(!GetProcessTimes(handle,&created,&exited,&kernel,&user))return ProcessState::unknown;
    const uint64_t actual=(uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    if(actual!=expected.created)return ProcessState::exited;
    const auto wait=WaitForSingleObject(handle,0);return wait==WAIT_OBJECT_0?ProcessState::exited:wait==WAIT_TIMEOUT?ProcessState::alive:ProcessState::unknown;
  }catch(...){return ProcessState::unknown;}
}
std::optional<OriginalObservation> inspectProtectedOriginal(const std::filesystem::path& path,TrustedPath trusted){
  try{if(!path.is_absolute()||path.lexically_normal()!=path)return {};const auto pin=readFile(path,trusted,65536);
    if(!pin)return {};return OriginalObservation{pin->identity,pin};}catch(...){return {};}
}

struct FileJournal::Impl {
  std::filesystem::path parent,directory;TrustedPath trusted;std::mutex mutex;
  DirectoryPin parentPin,directoryPin;
  Impl(std::filesystem::path value,TrustedPath check):parent(std::move(value)),directory(parent/L"signin-cleanup-handoff-v1"),trusted(std::move(check)){
    if(!parent.is_absolute()||parent.lexically_normal()!=parent||!trusted||!trusted(parent))invalid();plainAncestors(parent);
    parentPin.value=pinDirectory(parent);
    if(!CreateDirectoryW(directory.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)invalid();
    if(!trusted(directory))invalid();plainAncestors(directory);directoryPin.value=pinDirectory(directory);
    if(!membership())invalid();
  }
  static HANDLE pinDirectory(const std::filesystem::path& path){
    const auto value=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(value==INVALID_HANDLE_VALUE)invalid();return value;
  }
  std::filesystem::path path(Phase phase)const{return directory/(phase==Phase::intent?L"intent.json":L"committed.json");}
  bool membership()const{
    try{
      if(!trusted(parent)||!trusted(directory))return false;
      for(const auto& file:std::filesystem::directory_iterator(directory)){
        const auto name=file.path().filename().wstring();const auto attributes=GetFileAttributesW(file.path().c_str());
        if((name!=L"intent.json"&&name!=L"committed.json")||attributes==INVALID_FILE_ATTRIBUTES||
            (attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return false;
      }
      return true;
    }catch(...){return false;}
  }
  std::optional<Snapshot> read(Phase phase){
    if(!membership())return {};const auto pin=readFile(path(phase),trusted,maximumRecord);
    if(!pin)return {};const auto parsed=parse(pin->bytes);if(!parsed||parsed->phase!=phase)return {};
    return Snapshot{pin->bytes,pin->identity,pin};
  }
  bool absent(Phase phase)const{
    if(GetFileAttributesW(path(phase).c_str())!=INVALID_FILE_ATTRIBUTES)return false;
    return GetLastError()==ERROR_FILE_NOT_FOUND;
  }
  bool unchanged(Phase phase,const Snapshot& expected){
    const auto pin=std::dynamic_pointer_cast<const FilePin>(expected.retained);
    if(!pin||pin->path!=path(phase)||pin->identity!=expected.identity||pin->bytes!=expected.bytes)return false;
    const auto actual=read(phase);return actual&&actual->identity==expected.identity&&actual->bytes==expected.bytes;
  }
  std::optional<Snapshot> create(Phase phase,const std::string& bytes,const std::function<bool()>& finalCheck={}){
    if(!membership()||!absent(phase))return {};
    const auto value=parse(bytes);if(!value||value->phase!=phase)return {};
    Nonce suffix{};if(BCryptGenRandom(nullptr,suffix.data(),DWORD(suffix.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return {};
    const auto suffixText=hex(suffix);
    const auto temp=directory/(L"new-"+std::wstring(suffixText.begin(),suffixText.end())+L".tmp");
    HANDLE output=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(output==INVALID_HANDLE_VALUE)return {};
    struct Close{HANDLE value;~Close(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}}close{output};
    BY_HANDLE_FILE_INFORMATION basic{};FILE_ID_INFO id{};DWORD written=0;
    if(!GetFileInformationByHandle(output,&basic)||basic.nNumberOfLinks!=1||!GetFileInformationByHandleEx(output,FileIdInfo,&id,sizeof(id))||
        !WriteFile(output,bytes.data(),DWORD(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(output))return {};
    CloseHandle(output);close.value=INVALID_HANDLE_VALUE;
    if(!trusted(temp)||!trusted(directory)||(finalCheck&&!finalCheck())||
        !MoveFileExW(temp.c_str(),path(phase).c_str(),MOVEFILE_WRITE_THROUGH))return {};
    const auto result=read(phase);if(!result||result->bytes!=bytes||result->identity.volume!=id.VolumeSerialNumber||
        !std::equal(result->identity.fileId.begin(),result->identity.fileId.end(),std::begin(id.FileId.Identifier)))return {};
    return result;
  }
};
FileJournal::FileJournal(std::filesystem::path parent,TrustedPath trusted):impl_(std::make_unique<Impl>(std::move(parent),std::move(trusted))){}
FileJournal::~FileJournal()=default;
std::optional<Snapshot> FileJournal::read(Phase phase){std::lock_guard lock(impl_->mutex);return impl_->read(phase);}
std::optional<Snapshot> FileJournal::createIntent(const std::string& bytes){
  std::lock_guard lock(impl_->mutex);if(!impl_->absent(Phase::intent)||!impl_->absent(Phase::handoffCommitted))return {};
  return impl_->create(Phase::intent,bytes);
}
std::optional<Snapshot> FileJournal::commit(const Snapshot& expected,const std::string& bytes,const std::function<bool()>& finalCheck){
  std::lock_guard lock(impl_->mutex);
  const auto parsed=parse(bytes);
  if(!finalCheck||!parsed||parsed->phase!=Phase::handoffCommitted||parsed->intentFile!=std::optional(expected.identity)||
      !impl_->unchanged(Phase::intent,expected)||!impl_->absent(Phase::handoffCommitted))return {};
  const auto result=impl_->create(Phase::handoffCommitted,bytes,finalCheck);
  if(!result||!impl_->unchanged(Phase::intent,expected))return {};return result;
}
bool FileJournal::unchanged(Phase phase,const Snapshot& expected){std::lock_guard lock(impl_->mutex);return impl_->unchanged(phase,expected);}

std::optional<Intent> begin(const Request& requested,Authority& authority,Journal& journal){
  try{
    Record record;record.request=requested;sortChildren(record.request.children);
    const auto bytes=canonical(record);const auto before=producer(record,authority);if(!before)return {};
    const auto snapshot=journal.createIntent(bytes);
    const auto after=producer(record,authority);
    if(!snapshot||!snapshotValid(*snapshot)||snapshot->bytes!=bytes||!after||after->gate!=before->gate||
        !journal.unchanged(Phase::intent,*snapshot))return {};return Intent(record,*snapshot,before->original,before->gate);
  }catch(...){return {};}
}
std::optional<CommitReceipt> commit(const Intent& intent,Authority& authority,Journal& journal){
  try{
    if(intent.record_.phase!=Phase::intent||!intent.original_.retained||!intent.gate_||!snapshotValid(intent.snapshot_)||
        canonical(intent.record_)!=intent.snapshot_.bytes||!journal.unchanged(Phase::intent,intent.snapshot_))return {};
    Record record=intent.record_;record.phase=Phase::handoffCommitted;record.intentFile=intent.snapshot_.identity;
    auto proof=producer(record,authority);if(!proof||proof->gate!=intent.gate_)return {};
    const auto bytes=canonical(record);bool finalCalled=false,finalAccepted=false,duplicateFinal=false;
    const auto completed=journal.commit(intent.snapshot_,bytes,[&]{
      if(finalCalled){duplicateFinal=true;return false;}finalCalled=true;proof=producer(record,authority);
      finalAccepted=proof&&proof->gate==intent.gate_;return finalAccepted;
    });
    const auto after=producer(record,authority);
    if(!completed||!finalCalled||!finalAccepted||duplicateFinal||!after||after->gate!=intent.gate_||!snapshotValid(*completed)||completed->bytes!=bytes||
        !journal.unchanged(Phase::intent,intent.snapshot_)||!journal.unchanged(Phase::handoffCommitted,*completed))return {};
    return CommitReceipt(record,intent.snapshot_,*completed,intent.original_,intent.gate_);
  }catch(...){return {};}
}
std::optional<RecoveryObligation> recover(Authority& authority,Journal& journal){
  try{
    const auto intent=journal.read(Phase::intent),completed=journal.read(Phase::handoffCommitted);
    if(!intent||!completed||!snapshotValid(*intent)||!snapshotValid(*completed))return {};
    const auto first=parse(intent->bytes),last=parse(completed->bytes);
    if(!first||!last||first->phase!=Phase::intent||last->phase!=Phase::handoffCommitted||
        first->request!=last->request||last->intentFile!=std::optional(intent->identity))return {};
    auto proof=consumer(*last,authority);if(!proof)return {};
    if(!journal.unchanged(Phase::intent,*intent)||!journal.unchanged(Phase::handoffCommitted,*completed))return {};
    const auto again=consumer(*last,authority);if(!again||again->first!=proof->first||again->second.identity!=proof->second.identity||
        !journal.unchanged(Phase::intent,*intent)||!journal.unchanged(Phase::handoffCommitted,*completed))return {};
    return RecoveryObligation(*last,again->first,*intent,*completed,again->second);
  }catch(...){return {};}
}
} // namespace host::signin::handoff
