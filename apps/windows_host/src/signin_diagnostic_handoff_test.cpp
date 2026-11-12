#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <aclapi.h>
#include "signin_diagnostic_handoff.hpp"
#include <fstream>
#include <iostream>
#include <type_traits>

namespace h=host::signin::handoff;
using host::signin::Nonce;using host::signin::Process;
namespace {
unsigned checks=0;
void expect(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);++checks;}
struct Dummy final:h::Retention {};
Nonce hash(const std::string& bytes){
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE value=nullptr;
  struct Close{BCRYPT_ALG_HANDLE& a;BCRYPT_HASH_HANDLE& h;~Close(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}}close{algorithm,value};
  if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("Fixture hash unavailable");
  DWORD size=0,done=0;expect(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<BYTE*>(&size),sizeof(size),&done,0)>=0,"Fixture hash property");
  std::vector<uint8_t> object(size);Nonce digest{};
  expect(BCryptCreateHash(algorithm,&value,object.data(),size,nullptr,0,0)>=0&&
    BCryptHashData(value,reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),ULONG(bytes.size()),0)>=0&&
    BCryptFinishHash(value,digest.data(),DWORD(digest.size()),0)>=0,"Fixture hash calculation");return digest;
}
h::FileIdentity identity(const std::string& bytes,unsigned index){
  h::FileIdentity value;value.volume=9;value.created=1000+index;value.bytes=bytes.size();value.fileId[0]=uint8_t(index);
  value.sha256=hash(bytes);return value;
}
h::Request request(){
  h::Request r;r.oldService={101,10001};r.oldImageSha256[0]=1;r.operation[0]=2;r.stream[0]=3;
  r.instance="ROOT\\DISPLAY\\0000";r.generation=17;r.original=identity("numeric original fixture",8);
  r.children={{{201,20001},h::ChildRole::preparer},{{301,30001},h::ChildRole::restorer}};return r;
}
struct Memory final:h::Journal {
  std::optional<h::Snapshot> intent,completed;bool third=false,denyCreate=false,denyCommit=false;
  std::function<void()> onCommit;unsigned nextIdentity=20;
  std::optional<h::Snapshot> read(h::Phase phase)override{return third?std::nullopt:phase==h::Phase::intent?intent:completed;}
  std::optional<h::Snapshot> createIntent(const std::string& bytes)override{
    if(third||denyCreate||intent||completed)return {};intent=h::Snapshot{bytes,identity(bytes,++nextIdentity),std::make_shared<Dummy>()};return intent;
  }
  std::optional<h::Snapshot> commit(const h::Snapshot& before,const std::string& bytes,const std::function<bool()>& finalCheck)override{
    if(denyCommit||completed||!unchanged(h::Phase::intent,before))return {};
    if(onCommit)onCommit();if(!finalCheck||!finalCheck())return {};
    completed=h::Snapshot{bytes,identity(bytes,++nextIdentity),std::make_shared<Dummy>()};return completed;
  }
  bool unchanged(h::Phase phase,const h::Snapshot& before)override{
    const auto now=read(phase);return now&&now->identity==before.identity&&now->bytes==before.bytes;
  }
};
struct Facts final:h::Authority {
  h::Request expected;Process next{401,40001};
  bool producer=true,oldExited=false,nextAlive=true,inventoryAvailable=true,originalAvailable=true,retainOriginal=true;
  bool wrongOriginal=false,unknownOld=false;unsigned originalReads=0;
  std::vector<h::Child> inventory;std::optional<Process> badProcess;
  std::shared_ptr<const h::Retention> gate=std::make_shared<Dummy>();
  h::ProcessState badState=h::ProcessState::unknown;
  std::vector<h::Phase> seen;
  std::function<std::optional<h::OriginalObservation>(const h::Record&)> originalReader;
  explicit Facts(h::Request r=request()):expected(std::move(r)),inventory(expected.children){}
  bool currentProducer(const h::Record& record)override{seen.push_back(record.phase);return producer&&record.request==expected;}
  std::optional<Process> currentConsumer(const h::Record& record)override{seen.push_back(record.phase);return record.request==expected?std::optional(next):std::nullopt;}
  std::optional<h::OwnedInventory> ownedChildren(const h::Record& record)override{
    seen.push_back(record.phase);return inventoryAvailable?std::optional(h::OwnedInventory{inventory,gate}):std::nullopt;
  }
  h::ProcessState process(const h::Record& record,Process value)override{
    seen.push_back(record.phase);
    if(badProcess&&*badProcess==value)return badState;
    if(value==expected.oldService)return unknownOld?h::ProcessState::unknown:oldExited?h::ProcessState::exited:h::ProcessState::alive;
    if(value==next)return nextAlive?h::ProcessState::alive:h::ProcessState::unknown;
    for(const auto& child:expected.children)if(child.process==value)return h::ProcessState::exited;
    return h::ProcessState::unknown;
  }
  std::optional<h::OriginalObservation> original(const h::Record& record)override{
    ++originalReads;seen.push_back(record.phase);if(originalReader)return originalReader(record);
    if(!originalAvailable)return {};auto value=expected.original;if(wrongOriginal)++value.fileId[0];
    return h::OriginalObservation{value,retainOriginal?std::make_shared<Dummy>():nullptr};
  }
};
void codec(){
  static_assert(!std::is_default_constructible_v<h::RecoveryObligation>);
  static_assert(!std::is_default_constructible_v<h::Intent>);
  static_assert(!std::is_default_constructible_v<h::CommitReceipt>);
  static_assert(!std::is_convertible_v<h::RecoveryObligation,host::signin::Context>);
  const h::Record record{request()};const auto bytes=h::canonical(record);
  expect(h::parse(bytes)==record,"canonical intent roundtrip");
  auto maximumGeneration=record;maximumGeneration.request.generation=UINT32_MAX;
  expect(h::parse(h::canonical(maximumGeneration))==maximumGeneration,"full uint32 generation remains exact for cleanup");
  expect(!h::parse(" "+bytes)&&!h::parse(bytes+"\n"),"noncanonical whitespace rejected");
  for(const auto& field:{std::string("\"schema\":1"),std::string("\"generation\":17")}){
    auto bad=bytes;const auto at=bad.find(field);expect(at!=std::string::npos,"codec mutation boundary");
    bad.replace(at,field.size(),field+",\"verified\":true");expect(!h::parse(bad),"JSON verified boolean cannot confer authority");
  }
  for(unsigned part=0;part<13;++part){auto r=request();
    switch(part){case 0:r.oldService.created=0;break;case 1:r.oldImageSha256={};break;case 2:r.operation={};break;
      case 3:r.stream={};break;case 4:r.instance="ROOT\\DISPLAY\\FOREIGN";break;case 5:r.provider="untrusted";break;
      case 6:r.generation=0;break;case 7:r.original.fileId={};break;case 8:r.original.bytes=65537;break;
      case 9:r.children.clear();break;case 10:r.children.push_back(r.children[0]);break;
      case 11:r.children[0].process=r.oldService;break;default:r.children[0].role=h::ChildRole::restorer;break;}
    expect(!h::valid(r),"bounded request cannot carry malformed identity/inventory");
  }
  auto overflow=bytes;const auto number=overflow.find("\"created\":\"10001\"");
  overflow.replace(number,std::string("\"created\":\"10001\"").size(),"\"created\":\"18446744073709551616\"");
  expect(!h::parse(overflow),"u64 overflow rejected");
  expect(!h::parse(std::string(16385,'x')),"record byte bound");
}
void factory(){
  {Facts facts;Memory io;auto begun=h::begin(facts.expected,facts,io);expect(begun.has_value(),"producer observed actual complete exited children");
    expect(!h::recover(facts,io),"uncommitted intent is not recoverable authority");
    expect(!h::begin(facts.expected,facts,io),"stale existing intent cannot be overwritten or newly adopted");
    auto done=h::commit(*begun,facts,io);expect(done.has_value(),"exact intent CAS commit");
    expect(!h::recover(facts,io),"live old service cannot be adopted");facts.oldExited=true;
    auto recovered=h::recover(facts,io);expect(recovered&&recovered->request()==facts.expected&&recovered->consumer()==facts.next,"new exact service consumes committed cleanup-only fact");
    expect(std::find(facts.seen.begin(),facts.seen.end(),h::Phase::intent)!=facts.seen.end()&&
      std::find(facts.seen.begin(),facts.seen.end(),h::Phase::handoffCommitted)!=facts.seen.end(),"every callback receives bound phase");}
  for(unsigned field=0;field<10;++field){Facts facts;Memory io;
    switch(field){case 0:facts.producer=false;break;case 1:facts.inventoryAvailable=false;break;case 2:facts.inventory.pop_back();break;
      case 3:facts.inventory.push_back({{501,50001},h::ChildRole::restorer});break;case 4:facts.badProcess=facts.expected.children[0].process;facts.badState=h::ProcessState::alive;break;
      case 5:facts.badProcess=facts.expected.children[1].process;break;case 6:facts.originalAvailable=false;break;case 7:facts.wrongOriginal=true;break;
      case 8:facts.retainOriginal=false;break;default:facts.gate.reset();break;}
    expect(!h::begin(facts.expected,facts,io)&&!io.intent,"producer cannot omit unknown/live child or changed/unpinned original");
  }
  {Facts facts;Memory io;auto begun=h::begin(facts.expected,facts,io);expect(begun.has_value(),"stale CAS fixture intent");
    ++io.intent->identity.fileId[0];expect(!h::commit(*begun,facts,io)&&!io.completed,"same-byte replaced intent rejects CAS");}
  {Facts facts;Memory io;auto begun=h::begin(facts.expected,facts,io);expect(begun.has_value(),"commit race fixture intent");
    io.onCommit=[&]{facts.wrongOriginal=true;};expect(!h::commit(*begun,facts,io)&&!io.completed,"post-I/O changed original never publishes committed authority");
    facts.oldExited=true;expect(!h::recover(facts,io),"changed original rejects reader even if write completed");}
  for(unsigned field=0;field<8;++field){Facts facts;Memory io;auto begun=h::begin(facts.expected,facts,io);expect(begun.has_value(),"consumer fixture intent");
    expect(h::commit(*begun,facts,io).has_value(),"consumer fixture committed");facts.oldExited=true;
    switch(field){case 0:facts.unknownOld=true;break;case 1:facts.next=facts.expected.oldService;break;
      case 2:facts.badProcess=facts.expected.children[0].process;facts.badState=h::ProcessState::alive;break;
      case 3:facts.badProcess=facts.expected.children[1].process;break;case 4:facts.wrongOriginal=true;break;
      case 5:io.completed.reset();break;case 6:++io.intent->identity.fileId[0];break;default:io.third=true;break;}
    expect(!h::recover(facts,io),"consumer fails closed on unknown/live/changed/missing/third-leaf state");
  }
  {Facts facts;Memory io;auto begun=h::begin(facts.expected,facts,io);expect(begun.has_value(),"launch gate race fixture intent");
    io.onCommit=[&]{facts.gate=std::make_shared<Dummy>();};
    expect(!h::commit(*begun,facts,io)&&!io.completed,"changed no-new-child gate cannot publish committed record");}
  {Facts facts;Memory io;auto begun=h::begin(facts.expected,facts,io);expect(begun&&h::commit(*begun,facts,io),"PID reuse fixture");
    facts.oldExited=true;facts.next={facts.expected.oldService.pid,facts.expected.oldService.created+1};
    expect(h::recover(facts,io).has_value(),"different creation time is a distinct new service, not the old live process");}
  {Facts facts;Memory io;auto begun=h::begin(facts.expected,facts,io);expect(begun&&h::commit(*begun,facts,io),"orphan committed fixture");
    io.intent.reset();facts.oldExited=true;expect(!h::recover(facts,io),"committed leaf without exact intent cannot mint authority");}
  {Facts facts;Memory io;auto begun=h::begin(facts.expected,facts,io);expect(begun&&h::commit(*begun,facts,io),"late record replacement fixture");
    facts.oldExited=true;facts.originalReads=0;
    facts.originalReader=[&](const auto&)->std::optional<h::OriginalObservation>{
      if(facts.originalReads==2)++io.completed->identity.fileId[0];
      return h::OriginalObservation{facts.expected.original,std::make_shared<Dummy>()};
    };
    expect(!h::recover(facts,io),"record replacement during final native callback cannot mint stale recovery authority");}
}

struct OwnWorkspace {
  std::filesystem::path root;std::wstring sid;
  explicit OwnWorkspace(const std::filesystem::path& parent){
    HANDLE raw=nullptr;expect(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)!=FALSE,"own token query");
    DWORD needed=0;GetTokenInformation(raw,TokenUser,nullptr,0,&needed);std::vector<uint8_t> data(needed);
    expect(needed&&GetTokenInformation(raw,TokenUser,data.data(),needed,&needed),"own SID query");CloseHandle(raw);
    LPWSTR text=nullptr;expect(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&text)!=FALSE,"own SID format");sid=text;LocalFree(text);
    Nonce nonce{};expect(BCryptGenRandom(nullptr,nonce.data(),DWORD(nonce.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"own workspace nonce");
    std::wstring suffix;for(unsigned i=0;i<16;++i){const wchar_t digits[]=L"0123456789abcdef";suffix+=digits[nonce[i]>>4];suffix+=digits[nonce[i]&15];}
    root=parent/(L"handoff-"+suffix);
    PSECURITY_DESCRIPTOR descriptor=nullptr;const auto sddl=L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;"+sid+L")";
    expect(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"own workspace descriptor");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
    const bool created=CreateDirectoryW(root.c_str(),&attributes)!=FALSE;LocalFree(descriptor);expect(created,"fresh own workspace");
  }
  bool trusted(const std::filesystem::path& path)const{
    const auto normalized=path.lexically_normal();
    if(normalized!=root&&!normalized.native().starts_with(root.native()+L"\\"))return false;
    const auto attributes=GetFileAttributesW(path.c_str());if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    PACL acl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&acl,nullptr,&descriptor)!=ERROR_SUCCESS||!acl){if(descriptor)LocalFree(descriptor);return false;}
    bool safe=true;for(DWORD i=0;i<acl->AceCount;++i){void* raw=nullptr;if(!GetAce(acl,i,&raw)){safe=false;break;}
      const auto* header=static_cast<ACE_HEADER*>(raw);if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE){safe=false;break;}
      const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);const auto user=const_cast<DWORD*>(&ace->SidStart);
      if(IsWellKnownSid(user,WinLocalSystemSid))continue;LPWSTR value=nullptr;
      if(!ConvertSidToStringSidW(user,&value)){safe=false;break;}const bool own=sid==value;LocalFree(value);if(!own){safe=false;break;}
    }
    LocalFree(descriptor);return safe;
  }
  void write(const std::filesystem::path& path,const std::string& bytes){
    HANDLE output=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    expect(output!=INVALID_HANDLE_VALUE,"own CreateNew file");DWORD written=0;
    const bool ok=WriteFile(output,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(output);CloseHandle(output);expect(ok,"own durable file write");
  }
};
void fileCases(const std::filesystem::path& parent){
  {OwnWorkspace work(parent);const auto original=work.root/L"display-v2.state";work.write(original,"synthetic numeric original");
    auto trusted=[&](const auto& path){return work.trusted(path);};const auto observed=h::inspectProtectedOriginal(original,trusted);
    expect(observed.has_value(),"real own original identity+read pin");Facts facts;facts.expected.original=observed->identity;
    facts.originalReader=[&](const auto&){return h::inspectProtectedOriginal(original,trusted);};
    h::FileJournal io(work.root,trusted);auto begun=h::begin(facts.expected,facts,io);expect(begun.has_value(),"real fixed intent durable create");
    const auto leaf=work.root/L"signin-cleanup-handoff-v1"/L"intent.json";
    expect(!MoveFileExW(leaf.c_str(),(work.root/L"replaced-intent.json").c_str(),MOVEFILE_WRITE_THROUGH),"held intent denies external rename");
    expect(!h::recover(facts,io),"real uncommitted intent fails reader");auto completed=h::commit(*begun,facts,io);expect(completed.has_value(),"real immutable committed leaf bound to exact intent");
    facts.oldExited=true;expect(h::recover(facts,io).has_value(),"real committed pair plus kernel adapters mints cleanup fact");
    work.write(work.root/L"signin-cleanup-handoff-v1"/L"unknown.json","{}");
    expect(!h::recover(facts,io),"real third leaf prevents completed claim");
  }
  {OwnWorkspace work(parent);const auto original=work.root/L"display-v2.state";work.write(original,"same bytes remain same content");
    auto trusted=[&](const auto& path){return work.trusted(path);};Facts facts;
    {auto observed=h::inspectProtectedOriginal(original,trusted);expect(observed.has_value(),"replacement fixture original");facts.expected.original=observed->identity;}
    facts.originalReader=[&](const auto&){return h::inspectProtectedOriginal(original,trusted);};
    h::FileJournal io(work.root,trusted);
    {auto begun=h::begin(facts.expected,facts,io);expect(begun&&h::commit(*begun,facts,io),"replacement fixture committed");}
    const auto leaf=work.root/L"signin-cleanup-handoff-v1"/L"intent.json";std::ifstream input(leaf,std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)),{});input.close();
    expect(MoveFileExW(leaf.c_str(),(work.root/L"retained-old-intent.json").c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"move exact old own intent aside");
    work.write(leaf,bytes);facts.oldExited=true;
    expect(!h::recover(facts,io),"identical canonical bytes with DIFFERENT real fileID rejects committed link");
  }
  {OwnWorkspace work(parent);const auto original=work.root/L"display-v2.state";work.write(original,"original identity fixture");
    auto trusted=[&](const auto& path){return work.trusted(path);};Facts facts;
    {auto value=h::inspectProtectedOriginal(original,trusted);expect(value.has_value(),"original replacement identity");facts.expected.original=value->identity;}
    facts.originalReader=[&](const auto&){return h::inspectProtectedOriginal(original,trusted);};h::FileJournal io(work.root,trusted);
    {auto begun=h::begin(facts.expected,facts,io);expect(begun&&h::commit(*begun,facts,io),"original replacement committed");}
    expect(MoveFileExW(original.c_str(),(work.root/L"retained-original.state").c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"retain old original object");
    work.write(original,"original identity fixture");facts.oldExited=true;
    expect(!h::recover(facts,io),"sameSHA original replacement different fileID rejects recovery");
  }
  {OwnWorkspace work(parent);const auto original=work.root/L"display-v2.state";work.write(original,"linked original fixture");
    expect(CreateHardLinkW((work.root/L"other-link.state").c_str(),original.c_str(),nullptr)!=FALSE,"own hardlink fixture");
    expect(!h::inspectProtectedOriginal(original,[&](const auto& path){return work.trusted(path);}),"hardlinked original rejects protected identity");
  }
}
}
int main(int argc,char** argv){try{
  expect(argc==2,"fresh own workspace parent argument required");const auto parent=std::filesystem::absolute(argv[1]);
  expect(std::filesystem::is_directory(parent),"own workspace parent must exist");codec();factory();fileCases(parent);
  FILETIME created{},exited{},kernel{},user{};expect(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)!=FALSE,"own process identity");
  Process own{GetCurrentProcessId(),(uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime};
  expect(h::observeKernelProcess(own)==h::ProcessState::alive,"actual own process remains alive");++own.created;
  expect(h::observeKernelProcess(own)==h::ProcessState::exited,"different creation time is not expected older process");
  expect(h::observeKernelProcess({})==h::ProcessState::unknown,"invalid process identity is not exited");
  std::cout<<"PASS "<<checks<<" cleanup handoff checks; own files/process query only; SCM/SYSTEM/driver/input0\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
