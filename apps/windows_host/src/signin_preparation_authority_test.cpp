// The controller/CCD are injected. The only processes started by this test
// are two copies of this ordinary test EXE waiting on their own events.
#define main inheritedDisplayV2ServiceMain
#include "display_v2_service_test.cpp"
#undef main
#include "signin_preparation_authority.hpp"
#include "signin_restoration_authority.hpp"

#ifdef MULTITAP_NATIVE_DISPLAY
namespace {
class WaitChild {
 public:
  host::WinHandle event,process,job;
  WaitChild(){
    const auto name=L"Local\\Multitap.Preparation.Authority.Test."+host::randomName();
    event=host::WinHandle(CreateEventW(nullptr,TRUE,FALSE,name.c_str()));
    require(event.value&&GetLastError()!=ERROR_ALREADY_EXISTS,"Fresh ordinary-child event required");
    job=host::WinHandle(CreateJobObjectW(nullptr,nullptr));require(job.value,"Own test job unavailable");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"Own test job bounds failed");
    const auto image=host::executablePath();auto command=L"\""+image+L"\" --preparation-authority-child "+name;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
    require(CreateProcessW(image.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup,&child),"Own ordinary child creation failed");
    process=host::WinHandle(child.hProcess);host::WinHandle thread(child.hThread);
    if(!AssignProcessToJobObject(job.value,process.value)){
      (void)TerminateProcess(process.value,1);(void)WaitForSingleObject(process.value,2000);
      throw std::runtime_error("Own ordinary child job assignment failed");
    }
    require(ResumeThread(thread.value)!=DWORD(-1),"Own ordinary child resume failed");
  }
  ~WaitChild(){if(event.value)SetEvent(event.value);if(process.value)(void)WaitForSingleObject(process.value,2000);job=host::WinHandle{};}
  void exit(){require(SetEvent(event.value)&&WaitForSingleObject(process.value,3000)==WAIT_OBJECT_0,"Exact ordinary child exit was not observed");}
};
int preparationAuthorityCases(){
  using namespace host;using namespace host::signin;using namespace host::signin::preparation;
  Fixture fixture(true,{},true,"ROOT\\DISPLAY\\00A1");WaitChild observer,child;
  PreparationAdmission admission;admission.service=native::processIdentity(GetCurrentProcess());
  admission.operatorProcess=native::processIdentity(observer.process.value);admission.stream.back()=1;
  const auto snapshot=fixture.state();const auto targets=nativeDisplayTargets(snapshot);
  admission.lease={snapshot.get("deviceInstanceId").string(),uint32_t(snapshot.get("generation").number())};
  admission.operation[0]=7;admission.installedImageSha256[0]=9;
  DWORD ownSession=0;require(ProcessIdToSessionId(GetCurrentProcessId(),&ownSession)&&ownSession&&ownSession!=UINT32_MAX,"Ordinary console test session required");
  admission.consoleSession=ownSession;admission.width=1280;admission.height=720;
  admission.issuedAt=GetTickCount64();admission.expiresAt=admission.issuedAt+30000;
  const auto stream=PreparedServiceAuthority::streamText(admission.stream);
  require(fixture.service->reservePreparation(stream,nativeDisplayLeaseKey(snapshot)),"Exact memory-controller preparation reservation unavailable");
  const auto root=std::filesystem::current_path()/L".artifacts"/(L"signin-preparation-authority-"+randomName());
  std::filesystem::create_directories(root);
  const auto trusted=[root](const std::filesystem::path& path){
    const auto value=std::filesystem::absolute(path).lexically_normal();const auto relative=value.lexically_relative(root);
    const auto attributes=GetFileAttributesW(value.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT)&&
      (value==root||(!relative.empty()&&*relative.begin()!=L".."));
  };
  v2world::World world;
  // Windows' active mode can differ from the SDK's nominal original mode.
  // Restoration must use the actual saved CCD size, not that nominal size.
  world.owned().width=1600;world.owned().height=900;
  const auto bytes=displayV2StateBytes({stream,world.query(),targets,{}});
  int passed=0;
  {
    const auto path=root/L"adoption.state";auto store=displayV2FileStore(path,trusted);
    require(store.save(bytes),"Constructor-failure original fixture was not saved");
    auto adoptedAdmission=admission;adoptedAdmission.expectedOriginalSha256=nativeBaselineDigest(bytes);
    OriginalFile adopted(path,trusted);unsigned calls=0;bool refused=false;
    try{ServiceAuthority rejected(*fixture.service,adoptedAdmission,observer.process.value,adopted,
      [&](const auto&){return ++calls==1;});}catch(...){refused=true;}
    require(refused&&calls==2&&!adopted.current()&&store.load()==std::optional(bytes)&&store.remove(),
      "Consent loss during construction stranded the newly adopted original pin");++passed;
    auto bad=parseDisplayV2State(bytes);++bad.owned.front().target;
    const auto wrong=displayV2StateBytes(bad);require(store.save(wrong),"Mismatched-baseline fixture was not saved");
    adoptedAdmission.expectedOriginalSha256=nativeBaselineDigest(wrong);refused=false;
    try{ServiceAuthority rejected(*fixture.service,adoptedAdmission,observer.process.value,adopted,[](const auto&){return true;});}catch(...){refused=true;}
    require(refused&&!adopted.current()&&store.load()==std::optional(wrong)&&store.remove(),
      "Baseline mismatch during construction stranded the newly adopted original pin");++passed;
    require(store.save(bytes),"Earlier-owner fixture was not saved");
    adoptedAdmission.expectedOriginalSha256=nativeBaselineDigest(bytes);
    require(adopted.adopt(adoptedAdmission.expectedOriginalSha256),"Earlier original owner could not adopt its own bytes");
    refused=false;
    try{ServiceAuthority rejected(*fixture.service,adoptedAdmission,observer.process.value,adopted,[](const auto&){return true;});}catch(...){refused=true;}
    require(refused&&adopted.current()==std::optional(bytes)&&!store.remove(),"Constructor failure released an earlier owner's pin");
    adopted.releaseForRestore();require(store.remove(),"Earlier owner could not perform its own cleanup");++passed;
  }
  OriginalFile original(root/L"display-v2.state",trusted);bool consent=true;
  ServiceAuthority authority(*fixture.service,admission,observer.process.value,original,[&](const auto&){return consent;});
  PreparationBinding binding;binding.service=admission.service;binding.operatorProcess=admission.operatorProcess;
  binding.operation=admission.operation;binding.stream=admission.stream;binding.consoleSession=admission.consoleSession;
  require(authority.current(admission,binding,Operation::prepare).has_value(),"Exact prelaunch authority was refused");++passed;
  const auto creates=fixture.transport.creates,removes=fixture.transport.removes;
  require(!authority.saveOriginal(admission,binding,bytes,{})&&!authority.size(admission,binding,1280,720,Operation::prepare)&&
    original.absent()&&fixture.transport.creates==creates&&fixture.transport.removes==removes,"Prelaunch binding performed a child operation");++passed;
  require(authority.bindChild(child.process.value)&&!authority.bindChild(child.process.value),"Child ownership did not bind exactly once");
  binding.worker=native::processIdentity(child.process.value);++passed;
  const auto missing=authority.original(admission,binding,Operation::prepare);
  require(missing&&missing->bytes.empty()&&missing->sha256.empty(),"Confirmed absent original was indistinguishable from denied read");++passed;
  require(!authority.size(admission,binding,1280,720,Operation::prepare)&&fixture.transport.creates==creates,"SDK size preceded durable original");++passed;
  auto changed=admission;changed.width+=2;
  require(!authority.current(changed,binding,Operation::prepare)&&!authority.saveOriginal(changed,binding,bytes,{}),"Changed admission borrowed operator consent");++passed;
  const auto saved=authority.saveOriginal(admission,binding,bytes,{});
  require(saved&&saved->bytes==bytes&&saved->sha256==nativeBaselineDigest(bytes)&&original.current()==std::optional(bytes),"Actual canonical original was not sealed");++passed;
  require(authority.saveOriginal(admission,binding,bytes,{}).has_value()&&
    !authority.saveOriginal(admission,binding,bytes+" ",{}),"Lost-reply retry replaced original bytes");++passed;
  require(!authority.size(admission,binding,1600,900,Operation::prepare)&&fixture.transport.creates==creates,"Unrequested size reached the SDK");++passed;
  const auto resized=authority.size(admission,binding,1280,720,Operation::prepare);
  require(resized&&resized->targets.size()==1&&resized->targets[0].width==1280&&resized->targets[0].height==720&&
    original.current()==std::optional(bytes),"Bound size did not reuse exact sealed original/controller");++passed;
  Receipt receipt;receipt.binding=binding;receipt.outcome=Outcome::prepared;receipt.originalSha256=saved->sha256;receipt.originalRetained=true;
  const auto& target=resized->targets[0];receipt.target={target.adapter.LowPart,target.adapter.HighPart,0,target.target,target.width,target.height,0,0,1};
  auto wrong=receipt;++wrong.target.targetId;
  require(!authority.retain(admission,binding,wrong)&&authority.retain(admission,binding,receipt)&&authority.retained().has_value(),"Wrong target retained original or exact target failed");++passed;
  unsigned privateFailures=0;
  fixture.service->testSnapshotPublisher([&](const std::string&){++privateFailures;throw std::runtime_error("Injected parent private snapshot failure");},true);
  require(!authority.size(admission,binding,1280,720,Operation::prepare)&&privateFailures==1&&
    !authority.current(admission,binding,Operation::prepare)&&authority.current(admission,binding,Operation::cleanup).has_value()&&
    original.current()==std::optional(bytes),"Forward failure lost parent original-bound cleanup authority");++passed;
  fixture.service->testSnapshotPublisher([](const std::string&){},true);
  consent=false;
  require(!authority.current(admission,binding,Operation::prepare)&&!authority.original(admission,binding,Operation::prepare)&&
    authority.original(admission,binding,Operation::cleanup).has_value(),"Revoked forward consent prevented original cleanup or permitted preparation");++passed;
  require(!authority.size(admission,binding,1024,768,Operation::cleanup)&&
    !authority.size(admission,binding,targets[0].width,targets[0].height,Operation::cleanup)&&
    authority.size(admission,binding,1600,900,Operation::cleanup).has_value(),"Cleanup allowed an arbitrary/nominal mode or failed saved Windows mode restoration");++passed;
  require(!authority.releaseOriginalForRestore()&&original.current()==std::optional(bytes),"Living child released held original");++passed;
  observer.exit();require(!authority.current(admission,binding,Operation::prepare)&&
    authority.current(admission,binding,Operation::cleanup).has_value(),"Expired operator transferred forward authority or lost cleanup obligation");++passed;
  child.exit();require(!authority.current(admission,binding,Operation::cleanup)&&authority.releaseOriginalForRestore(),"Exact exited child still held active authority or blocked final restore");++passed;
  const auto store=displayV2FileStore(root/L"display-v2.state",trusted);
  require(store.load()==std::optional(bytes)&&store.remove()&&!store.load(),"Final normal restorer could not use/remove retained original");++passed;
  fixture.transport.checkForeign();
  return passed;
}
int restorationAuthorityCases(){
  using namespace host;using namespace host::signin;
  namespace restore=host::signin::restoration;
  Fixture fixture(true,{},true,"ROOT\\DISPLAY\\00A2");WaitChild predecessor,child;
  StreamId streamId{};streamId.back()=2;const auto stream=PreparedServiceAuthority::streamText(streamId);
  const auto initial=fixture.state();const auto targets=nativeDisplayTargets(initial);
  v2world::World world;world.owned().width=1600;world.owned().height=900;
  const DisplayV2Console console{v2world::consoleAdapter,v2world::consoleAdapter,0,v2world::consoleTarget};
  const auto bytes=displayV2StateBytes({stream,displayV2ActiveCcd(world.query()),targets,console});
  const auto root=std::filesystem::current_path()/L".artifacts"/(L"signin-restoration-authority-"+randomName());
  std::filesystem::create_directories(root);
  const auto trusted=[root](const std::filesystem::path& path){
    const auto value=std::filesystem::absolute(path).lexically_normal();const auto relative=value.lexically_relative(root);
    const auto attributes=GetFileAttributesW(value.c_str());return attributes!=INVALID_FILE_ATTRIBUTES&&
      !(attributes&FILE_ATTRIBUTE_REPARSE_POINT)&&(value==root||(!relative.empty()&&*relative.begin()!=L".."));
  };
  preparation::OriginalFile original(root/L"display-v2.state",trusted);
  require(original.create(bytes,stream),"Restore exact original fixture unavailable");
  require(fixture.service->setOwnedSizeV2("0",1280,720).has_value(),"Memory preparation mode unavailable");
  const auto snapshot=fixture.state();
  require(fixture.service->reservePreparation(stream,nativeDisplayLeaseKey(snapshot)),"Restore private reservation unavailable");
  restore::RestorationAdmission admission;
  admission.service=native::processIdentity(GetCurrentProcess());admission.stream=streamId;
  admission.lease={snapshot.get("deviceInstanceId").string(),uint32_t(snapshot.get("generation").number())};
  admission.operation[0]=23;admission.installedImageSha256[0]=24;
  admission.predecessors={native::processIdentity(predecessor.process.value)};admission.originalSha256=nativeBaselineDigest(bytes);
  DWORD consoleSession=0;require(ProcessIdToSessionId(GetCurrentProcessId(),&consoleSession)&&consoleSession,"Own restore test session unavailable");
  admission.consoleSession=consoleSession;admission.issuedAt=GetTickCount64();admission.expiresAt=admission.issuedAt+30000;
  const HANDLE predecessorHandle=predecessor.process.value;int passed=0;bool refused=false;
  try{restore::ServiceAuthority invalid(*fixture.service,admission,{&predecessorHandle,1},original);}catch(...){refused=true;}
  require(refused&&original.current()==std::optional(bytes),"Living predecessor admitted restoration or released original");++passed;
  predecessor.exit();
  auto wrongAdmission=admission;++wrongAdmission.predecessors.front().created;refused=false;
  try{restore::ServiceAuthority invalid(*fixture.service,wrongAdmission,{&predecessorHandle,1},original);}catch(...){refused=true;}
  require(refused,"A recycled/different predecessor lifetime authorized restoration");++passed;
  restore::ServiceAuthority authority(*fixture.service,admission,{&predecessorHandle,1},original);
  restore::RestorationBinding binding;binding.service=admission.service;binding.stream=streamId;
  binding.operation=admission.operation;binding.consoleSession=admission.consoleSession;
  require(authority.current(admission,binding).has_value()&&authority.original(admission,binding).has_value()&&!authority.sizeOriginal(admission,binding),
    "Restore prelaunch admitted child-only operations or lacked retained obligation");++passed;
  require(authority.bindChild(child.process.value)&&!authority.bindChild(child.process.value),"Restore child did not bind exactly once");
  binding.worker=native::processIdentity(child.process.value);++passed;
  const auto saved=authority.original(admission,binding);
  require(saved&&saved->bytes==bytes&&saved->sha256==admission.originalSha256,"Restore child could not read exact retained original");++passed;
  auto stale=binding;stale.operation[0]^=1;
  require(!authority.current(admission,stale)&&!authority.sizeOriginal(admission,stale),"Stale restore operation reached SDK");++passed;
  unsigned restoreFailures=0;
  fixture.service->testSnapshotPublisher([&](const std::string&){++restoreFailures;throw std::runtime_error("Injected restorer snapshot failure");},true);
  require(!authority.sizeOriginal(admission,binding)&&restoreFailures==1&&authority.current(admission,binding).has_value()&&
    original.current()==std::optional(bytes),"Restorer commit failure lost exact exited-predecessor/original authority");++passed;
  fixture.service->testSnapshotPublisher([](const std::string&){},true);
  const auto restored=authority.sizeOriginal(admission,binding);
  require(restored&&restored->targets.size()==1&&restored->targets.front().width==1600&&restored->targets.front().height==900&&
    original.current()==std::optional(bytes),"Parent did not derive sealed Windows mode independently of peer width/height");++passed;
  const auto& target=restored->targets.front();world.owned().target=target.target;world.ownedTarget=target.target;
  world.ownedWidth=target.width;world.ownedHeight=target.height;
  restore::Receipt receipt;receipt.binding=binding;receipt.outcome=restore::Outcome::topologyRestoredOriginalHeld;
  receipt.underlyingOutcome=DisplayV2Outcome::failed;receipt.stage=displayV2Stage::state;receipt.fallback=1;
  receipt.originalSha256=admission.originalSha256;receipt.observedLease=restored->lease;receipt.observedAt=GetTickCount64();
  receipt.observedState=displayV2StateBytes({stream,displayV2ActiveCcd(world.query()),restored->targets,console});
  require(!authority.finish(receipt)&&original.current()==std::optional(bytes),"A topology receipt retired original before actual restorer exit");++passed;
  child.exit();
  auto wrong=receipt;wrong.error=ERROR_INVALID_PARAMETER;wrong.stage=displayV2Stage::apply;
  require(!authority.finish(wrong)&&original.current()==std::optional(bytes),"Real apply failure was rewritten as full restoration");++passed;
  wrong=receipt;wrong.binding.operation[0]^=1;
  require(!authority.finish(wrong),"Another restore operation retired original");++passed;
  wrong=receipt;world.console().active=false;
  wrong.observedState=displayV2StateBytes({stream,displayV2ActiveCcd(world.query()),restored->targets,{}});world.console().active=true;
  require(!authority.finish(wrong)&&original.current()==std::optional(bytes),"Owned-only/console-missing observation retired full original");++passed;
  require(authority.finish(receipt)&&authority.completed()&&original.absent()&&!original.current(),
    "Fresh full topology/exact exit/exact file retirement did not complete");++passed;
  require(!authority.finish(receipt)&&!authority.current(admission,binding),"Completed restore receipt was replayed as a new completion");++passed;
  fixture.transport.checkForeign();return passed;
}
}
int main(int argc,char** argv){
  if(argc==3&&std::string(argv[1])=="--preparation-authority-child"){
    host::WinHandle event(OpenEventW(SYNCHRONIZE,FALSE,host::wide(argv[2]).c_str()));
    return event.value&&WaitForSingleObject(event.value,60000)==WAIT_OBJECT_0?0:2;
  }
  try{const auto prepare=preparationAuthorityCases(),restore=restorationAuthorityCases();std::cout<<"{\"kind\":\"signin-parent-authority\",\"preparationCases\":"<<prepare<<",\"restorationCases\":"<<restore<<",\"ordinaryOwnChildren\":4,\"actualSystemWorkers\":0,\"actualDriver\":false,\"actualDesktopCapture\":false,\"inputCalls\":0,\"runtimeQualified\":false}\n";return 0;}
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#else
int main(){std::cerr<<"Native SDK memory transport build is required\n";return 1;}
#endif
