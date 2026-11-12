// Actual files only in a fresh explicitly supplied .artifacts workspace.
// No real display query, driver, service, account, pipe, or input operation.
#include "signin_original_file.hpp"
#include "../tests/display-v2-world.hpp"
#include <barrier>
#include <iostream>
#include <type_traits>

namespace {
using namespace host;
using host::signin::preparation::OriginalFile;
static_assert(std::is_same_v<decltype(std::declval<const OriginalFile&>().sha256()),std::string>);
size_t checks=0;
DWORD replacementRefusal=0;
void expect(bool value,const char* reason){
  if(!value)throw std::runtime_error(reason);
  ++checks;
}
std::filesystem::path root;
bool fixtureTrusted(const std::filesystem::path& path){
  const auto normalized=std::filesystem::absolute(path).lexically_normal();
  const auto relative=normalized.lexically_relative(root);
  if(relative.empty()||relative.is_absolute())return false;
  for(const auto& part:relative)if(part==L"..")return false;
  const DWORD attributes=GetFileAttributesW(normalized.c_str());
  return attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT);
}
std::filesystem::path folder(const wchar_t* name){
  const auto value=root/name;
  expect(CreateDirectoryW(value.c_str(),nullptr)!=FALSE,"Fresh case directory required");
  return value;
}
std::string baseline(const std::string& session="stream-a"){
  v2world::World world;
  return displayV2StateBytes({session,world.query(),world.targets(),{}});
}
void writeNew(const std::filesystem::path& path,const std::string& bytes){
  WinHandle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
    FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  expect(file.value!=INVALID_HANDLE_VALUE,"Fixture must not overwrite an existing file");
  DWORD written=0;
  expect(WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&
    written==bytes.size()&&FlushFileBuffers(file.value),"Fixture file write failed");
}
std::string read(const std::filesystem::path& path){
  std::ifstream file(path,std::ios::binary);
  expect(file.good(),"Fixture file unavailable");
  return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
void creation(){
  const auto bytes=baseline();const auto path=folder(L"canonical")/L"original.json";
  {
    OriginalFile file(path,fixtureTrusted);
    expect(file.absent(),"Fresh trusted destination was not absent");
    expect(file.create(bytes,"stream-a"),"Canonical original creation failed");
    expect(!file.absent(),"Held destination still reported absent");
    expect(file.current()==bytes,"Current bytes differ from the original");
    expect(file.sha256()==nativeBaselineDigest(bytes),"Original SHA is not the actual byte digest");
    expect(!file.create(baseline("stream-b"),"stream-b"),"Held object replaced original");
    expect(!file.adopt(file.sha256()),"Held object adopted another owner");
    expect(read(path)==bytes,"Creation changed canonical bytes");
  }
  expect(read(path)==bytes,"Destructor removed the retained original");
  OriginalFile reopened(path,fixtureTrusted);
  expect(reopened.adopt(nativeBaselineDigest(bytes)),"Exact original reopen/adoption failed");
  expect(reopened.current()==bytes,"Adopted bytes changed");
}
void refuseExisting(){
  const auto bytes=baseline();const auto path=folder(L"existing")/L"original.json";
  writeNew(path,bytes);
  OriginalFile file(path,fixtureTrusted);
  expect(!file.absent(),"Existing destination reported absent");
  expect(!file.create(baseline("stream-b"),"stream-b"),"New session overwrote existing original");
  expect(!file.current(),"Absent-only refusal acquired existing bytes without expected SHA");
  expect(read(path)==bytes,"Existing original was modified on refusal");
  OriginalFile wrong(path,fixtureTrusted);
  expect(!wrong.adopt(nativeBaselineDigest(baseline("stream-b"))),"Wrong expected digest adopted old session");
  expect(read(path)==bytes,"Wrong adoption modified existing original");
  expect(wrong.adopt(nativeBaselineDigest(bytes)),"Failed adoption poisoned a later exact adoption");
}
void malformed(){
  const auto directory=folder(L"invalid");
  for(const auto& item:std::vector<std::pair<std::string,std::string>>{
      {baseline("stream-a"),"stream-b"},{baseline(),""},{"{broken", "stream-a"},
      {baseline()+" ","stream-a"},{"", "stream-a"},{std::string(65537,'x'),"stream-a"}}){
    const auto path=directory/(L"new-"+std::to_wstring(checks)+L".json");
    OriginalFile file(path,fixtureTrusted);
    expect(!file.create(item.first,item.second),"Invalid/mismatched original was created");
    expect(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,
      "Rejected input published a destination");
  }
  const auto corrupt=directory/L"corrupt.json";
  writeNew(corrupt,"{broken");
  OriginalFile corruptFile(corrupt,fixtureTrusted);
  expect(!corruptFile.adopt(nativeBaselineDigest("{broken")),"Exact hash bypassed canonical state validation");
  expect(read(corrupt)=="{broken","Corrupt original was silently replaced");
  const auto canonical=directory/L"canonical.json";
  writeNew(canonical,baseline());
  for(const auto& digest:std::vector<std::string>{"",std::string(63,'a'),std::string(64,'A'),
      std::string(64,'g'),std::string(64,'0')}){
    OriginalFile file(canonical,fixtureTrusted);
    expect(!file.adopt(digest),"Invalid/wrong digest adopted the original");
  }
  OriginalFile absent(directory/L"absent.json",fixtureTrusted);
  expect(!absent.adopt(nativeBaselineDigest(baseline())),"Adoption created an absent original");
  OriginalFile untrusted(directory/L"untrusted.json",[](const auto&){return false;});
  expect(!untrusted.absent(),"Untrusted parent reported admissibly absent");
  expect(!untrusted.create(baseline(),"stream-a"),"Untrusted parent admitted creation");
  OriginalFile missing(directory/L"not-created"/L"original.json",fixtureTrusted);
  expect(!missing.absent(),"Missing parent reported admissibly absent");
}
void heldFileAndRestorer(){
  const auto directory=folder(L"held"),path=directory/L"original.json";
  const auto bytes=baseline();const auto replacement=directory/L"replacement.json";
  writeNew(replacement,baseline("stream-b"));
  OriginalFile file(path,fixtureTrusted);
  expect(file.create(bytes,"stream-a"),"Held original creation failed");
  WinHandle writer(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
    nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
  expect(writer.value==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,
    "Held original admitted a writer");
  expect(!DeleteFileW(path.c_str())&&GetLastError()==ERROR_SHARING_VIOLATION,"Held original admitted deletion");
  const BOOL replaced=MoveFileExW(replacement.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
  replacementRefusal=GetLastError();
  expect(!replaced,"Held original admitted atomic replacement");
  expect(replacementRefusal==ERROR_SHARING_VIOLATION||replacementRefusal==ERROR_ACCESS_DENIED,
    "Held replacement refusal had an unexpected Win32 error");
  expect(read(path)==bytes&&read(replacement)==baseline("stream-b"),"Refused replacement changed either file");
  const auto restorer=displayV2FileStore(path,fixtureTrusted);
  expect(!restorer.save(baseline("stream-b")),"Normal V2 store replaced a held original");
  expect(!restorer.remove(),"Normal V2 store deleted a held original");
  expect(file.current()==bytes&&read(path)==bytes,"Refused mutations changed held original");
  file.releaseForRestore();
  expect(!file.current(),"Released object retained current authority");
  expect(restorer.load()==bytes,"Ordinary restorer cannot read released original");
  expect(restorer.save(baseline("stream-b")),"Ordinary restorer cannot save after release");
  expect(restorer.remove(),"Ordinary restorer cannot remove after release");
  expect(file.absent(),"Released and restored destination did not report absent");
  expect(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES,"Restored original was retained");
  expect(read(replacement)==baseline("stream-b"),"Failed replace consumed the other fixture");
}
void linkedFile(){
  const auto directory=folder(L"hardlink"),path=directory/L"original.json",alias=directory/L"alias.json";
  const auto bytes=baseline();writeNew(path,bytes);
  expect(CreateHardLinkW(alias.c_str(),path.c_str(),nullptr)!=FALSE,"Own hardlink fixture unavailable");
  OriginalFile file(path,fixtureTrusted);
  expect(!file.adopt(nativeBaselineDigest(bytes)),"Multi-link original was adopted");
  expect(read(path)==bytes&&read(alias)==bytes,"Rejected hardlink changed bytes");
}
void noChildPath(){
  const auto directory=folder(L"parent-only"),path=directory/L"original.json",child=directory/L"child.json";
  const auto bytes=baseline();
  const auto injected=bytes.substr(0,bytes.size()-1)+",\"path\":\"child.json\"}";
  OriginalFile file(path,fixtureTrusted);
  expect(!file.create(injected,"stream-a"),"Noncanonical path-bearing state was accepted");
  expect(!file.create(bytes,"../../child.json"),"Caller session mismatch changed the fixed destination");
  expect(GetFileAttributesW(child.c_str())==INVALID_FILE_ATTRIBUTES,"State bytes selected a child path");
  expect(file.create(bytes,"stream-a")&&file.current()==bytes,"Fixed parent destination failed");
  expect(GetFileAttributesW(child.c_str())==INVALID_FILE_ATTRIBUTES,"Canonical state created a child file");
}
void competingCreators(){
  const auto directory=folder(L"race");
  for(unsigned round=0;round<16;++round){
    const auto path=directory/(L"original-"+std::to_wstring(round)+L".json");
    const auto a=baseline("stream-a"),b=baseline("stream-b");
    std::barrier started(3);bool wonA=false,wonB=false;
    OriginalFile ownerA(path,fixtureTrusted),ownerB(path,fixtureTrusted);
    std::thread first([&]{started.arrive_and_wait();wonA=ownerA.create(a,"stream-a");});
    std::thread second([&]{started.arrive_and_wait();wonB=ownerB.create(b,"stream-b");});
    started.arrive_and_wait();first.join();second.join();
    expect(wonA!=wonB,"Independent creators did not yield exactly one winner");
    expect(read(path)==(wonA?a:b),"Race replaced the winning original");
    expect((wonA?ownerA.current():ownerB.current())==(wonA?a:b),"Winning owner lost exact byte authority");
    expect(!(wonA?ownerB.current():ownerA.current()),"Losing creator adopted another session");
  }
}
void restoredRetirement(){
  const auto directory=folder(L"retirement"),path=directory/L"original.json";
  const auto bytes=baseline();const auto sha=nativeBaselineDigest(bytes);
  OriginalFile file(path,fixtureTrusted);expect(file.create(bytes,"stream-a"),"Retirement original fixture failed");
  unsigned proofs=0;
  expect(!file.retireRestored(sha,{})&&file.current()==bytes,"Missing restore proof retired original");
  expect(!file.retireRestored(std::string(64,'0'),[&]{++proofs;return true;})&&proofs==0&&file.current()==bytes,
    "Wrong expected hash invoked proof or retired original");
  expect(!file.retireRestored(sha,[&]{++proofs;return false;})&&proofs==1&&file.current()==bytes,
    "Unproven child exit/restoration released original");
  proofs=0;
  expect(!file.retireRestored(sha,[&]{return ++proofs==1;})&&proofs==2&&read(path)==bytes,
    "Revoked post-open proof deleted original");
  expect(!file.current()&&file.adopt(sha)&&file.current()==bytes,"Refused final proof prevented explicit original re-adoption");
  {
    WinHandle other(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    expect(other.value!=INVALID_HANDLE_VALUE,"Independent own reader fixture failed");
    expect(!file.retireRestored(sha,[]{return true;})&&read(path)==bytes,"Another reader was ignored during exact-handle retirement");
  }
  expect(file.adopt(sha),"Original could not be re-adopted after sharing refusal");
  proofs=0;
  expect(file.retireRestored(sha,[&]{++proofs;return true;})&&proofs==2&&!file.current()&&file.absent(),
    "Exact proved original was not retired by its locked file handle");
  expect(!file.retireRestored(sha,[]{return true;}),"Absent file was reported as a newly proved retirement");

  const auto otherPath=directory/L"replacement-original.json",aside=directory/L"retained-old-instance.json";
  OriginalFile replaced(otherPath,fixtureTrusted);
  expect(replaced.create(bytes,"stream-a"),"Replacement-identity original fixture failed");
  {
    WinHandle reader(CreateFileW(otherPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    expect(reader.value!=INVALID_HANDLE_VALUE&&!replaced.retireRestored(sha,[]{return true;}),"Sharing refusal fixture did not retain original");
  }
  // Retain the old instance under another name so the filesystem cannot reuse
  // its ID for the replacement. Both files have identical canonical bytes.
  expect(MoveFileExW(otherPath.c_str(),aside.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"Own old-instance preservation failed");
  writeNew(otherPath,bytes);
  expect(!replaced.adopt(sha)&&!replaced.retireRestored(sha,[]{return true;}),
    "An identical-byte replacement reused the previous file's restore receipt");
  expect(read(otherPath)==bytes&&read(aside)==bytes,"Rejected replacement changed either exact file instance");
}
}
int wmain(int argc,wchar_t** argv){
  try{
    if(argc!=2)throw std::runtime_error("One fresh own-workspace path is required");
    root=std::filesystem::absolute(argv[1]).lexically_normal();
    const auto parent=std::filesystem::current_path()/L".artifacts";
    if(root.parent_path()!=parent||!root.filename().wstring().starts_with(L"signin-original-file-"))
      throw std::runtime_error("Workspace must be a new direct .artifacts/signin-original-file-* child");
    expect(CreateDirectoryW(root.c_str(),nullptr)!=FALSE,"Existing workspace must never be reused");
    creation();refuseExisting();malformed();heldFileAndRestorer();linkedFile();noChildPath();competingCreators();restoredRetirement();
    std::cout<<"{\"kind\":\"signin-original-file-own-workspace\",\"passed\":true,\"checks\":"<<checks
      <<",\"replacementRefusalWin32\":"<<replacementRefusal
      <<",\"raceRounds\":16,\"displayCalls\":0,\"driverCalls\":0,\"serviceCalls\":0,\"inputEvents\":0,\"runtimeQualified\":false}\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<"FAILED after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}
}
