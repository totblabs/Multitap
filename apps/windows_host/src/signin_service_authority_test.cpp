// The controller/CCD fixtures below are memory-only. The one additional child
// is this unelevated test EXE waiting on its own event, not a SYSTEM worker.
#define main inheritedDisplayV2ServiceMain
#include "display_v2_service_test.cpp"
#undef main
#include "signin_service_authority.hpp"

#ifdef MULTITAP_NATIVE_DISPLAY
namespace {
class OwnWaitChild {
 public:
  host::WinHandle event,process,job;
  OwnWaitChild(){
    const auto name=L"Local\\Multitap.Signin.Authority.Test."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
    event=host::WinHandle(CreateEventW(nullptr,TRUE,FALSE,name.c_str()));
    require(event.value&&GetLastError()!=ERROR_ALREADY_EXISTS,"Fresh own child event required");
    job=host::WinHandle(CreateJobObjectW(nullptr,nullptr));require(job.value!=nullptr,"Own test job unavailable");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=FALSE,"Own test job bounds failed");
    const auto image=host::executablePath();auto command=L"\""+image+L"\" --authority-child "+name;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
    require(CreateProcessW(image.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup,&child)!=FALSE,"Own ordinary child creation failed");
    process=host::WinHandle(child.hProcess);host::WinHandle thread(child.hThread);
    if(!AssignProcessToJobObject(job.value,process.value)){
      (void)TerminateProcess(process.value,1);(void)WaitForSingleObject(process.value,2000);throw std::runtime_error("Own test child job assignment failed");
    }
    require(ResumeThread(thread.value)!=DWORD(-1),"Own test child resume failed");
  }
  ~OwnWaitChild(){
    if(event.value)SetEvent(event.value);
    if(process.value)(void)WaitForSingleObject(process.value,2000);
    job=host::WinHandle{};
    if(process.value)(void)WaitForSingleObject(process.value,1000);
  }
  void exit(){require(SetEvent(event.value)!=FALSE&&WaitForSingleObject(process.value,3000)==WAIT_OBJECT_0,"Own test child exit was not observed");}
};
host::signin::Nonce digest(const std::string& bytes){
  const auto value=host::nativeBaselineDigest(bytes);host::signin::Nonce result{};
  const auto digit=[](char c){return c<='9'?unsigned(c-'0'):unsigned(c-'a'+10);};
  require(value.size()==64,"Actual baseline digest size changed");
  for(size_t i=0;i<result.size();++i)result[i]=uint8_t((digit(value[i*2])<<4)|digit(value[i*2+1]));
  return result;
}
int authorityCases(){
  using namespace host;using namespace host::signin;
  Fixture fixture(true,{},true,"ROOT\\DISPLAY\\00A1");OwnWaitChild child;
  const std::string stream="00000000-0000-0000-0000-000000000001";
  const auto snapshot=fixture.state();const auto outputs=nativeDisplayTargets(snapshot);
  require(fixture.service->reserveStreamProfile(stream,nativeDisplayLeaseKey(snapshot)),"Exact stream reservation unavailable");
  v2world::World world;std::optional<std::string> baseline=displayV2StateBytes({stream,world.query(),outputs,{}});
  Binding bound;bound.stream.back()=1;bound.service=native::processIdentity(GetCurrentProcess());bound.worker=native::processIdentity(child.process.value);
  bound.lease={snapshot.get("deviceInstanceId").string(),uint32_t(snapshot.get("generation").number())};bound.baseline=digest(*baseline);
  bound.nonce[0]=3;bound.consoleSession=1;bound.generation=2;bound.mediaEpoch=3;bound.role=Role::signin;
  const native::OwnedTarget output{outputs[0].adapter.LowPart,outputs[0].adapter.HighPart,0,outputs[0].target,outputs[0].width,outputs[0].height,1024,0,1};
  DisplayV2Store store;store.load=[&]{return baseline;};bool assigned=true;
  PreparedServiceAuthority authority(*fixture.service,stream,bound,output,child.process.value,store,[&](const Binding&){return assigned;});
  int passed=0;
  require(authority.current(bound,{&output,1}),"Fresh exact controller/baseline/ordinary child fixture was refused");++passed;
  auto other=bound;++other.mediaEpoch;require(!authority.current(other,{&output,1}),"Old media scope borrowed refreshed authority");++passed;
  auto foreign=output;++foreign.targetId;require(!authority.current(bound,{&foreign,1}),"Another target borrowed authority");++passed;
  assigned=false;require(!authority.current(bound,{&output,1}),"Revoked assignment retained authority");assigned=true;++passed;
  const auto original=baseline;baseline=*original+" ";require(!authority.current(bound,{&output,1}),"Changed original baseline retained authority");baseline=original;++passed;
  require(authority.current(bound,{&output,1}),"Restored exact test fixture lost its valid proof");++passed;
  auto corrupt=bound;corrupt.baseline[0]^=1;bool refused=false;
  try{PreparedServiceAuthority invalid(*fixture.service,stream,corrupt,output,child.process.value,store,[&](const Binding&){return true;});}catch(...){refused=true;}
  require(refused,"Hash-only mismatch baseline was admitted");++passed;
  corrupt=bound;corrupt.worker=corrupt.service;refused=false;
  try{PreparedServiceAuthority invalid(*fixture.service,stream,corrupt,output,GetCurrentProcess(),store,[&](const Binding&){return true;});}catch(...){refused=true;}
  require(refused,"Service process was accepted as its own worker");++passed;
  // A second owned but currently inactive output is still outside this first
  // composite-one-output contract. Do not rely on a later CCD capture check.
  {
    Fixture two(true,{},true,"ROOT\\DISPLAY\\00A2");
    require(two.service->setOwnedCountV2(2).has_value(),"Two-output refusal fixture was not prepared");
    const auto snapshot2=two.state();const auto targets2=nativeDisplayTargets(snapshot2);
    require(targets2.size()==2&&two.service->reserveStreamProfile(stream,nativeDisplayLeaseKey(snapshot2)),"Two-output exact reservation unavailable");
    auto binding2=bound;binding2.lease={snapshot2.get("deviceInstanceId").string(),uint32_t(snapshot2.get("generation").number())};
    const native::OwnedTarget output2{targets2[0].adapter.LowPart,targets2[0].adapter.HighPart,0,targets2[0].target,targets2[0].width,targets2[0].height,0,0,1};
    require(two.service->proveOwnedStreamLease(stream,binding2.lease.instance,binding2.lease.generation).has_value(),"Two-output actual held-controller fixture lacked a lease proof");
    const auto creates=two.transport.creates,removes=two.transport.removes;
    refused=false;
    try{PreparedServiceAuthority invalid(*two.service,stream,binding2,output2,child.process.value,store,[&](const Binding&){return true;});}catch(...){refused=true;}
    require(refused,"Composite sign-in authority admitted a second owned output");++passed;
    require(two.transport.creates==creates&&two.transport.removes==removes,"Two-output authority refusal changed display ownership");two.transport.checkForeign();++passed;
  }
  const auto creates=fixture.transport.creates,removes=fixture.transport.removes;
  child.exit();require(!authority.current(bound,{&output,1}),"Exited exact child retained authority");++passed;
  require(fixture.transport.creates==creates&&fixture.transport.removes==removes,"Lease checks changed display ownership");fixture.transport.checkForeign();++passed;
  return passed;
}
}
int main(int argc,char** argv){
  if(argc==3&&std::string(argv[1])=="--authority-child"){
    host::WinHandle event(OpenEventW(SYNCHRONIZE,FALSE,host::wide(argv[2]).c_str()));
    return event.value&&WaitForSingleObject(event.value,10000)==WAIT_OBJECT_0?0:2;
  }
  try{const auto cases=authorityCases();std::cout<<"{\"kind\":\"signin-prepared-service-authority-tests\",\"cases\":"<<cases<<",\"ordinaryOwnChild\":1,\"systemWorkers\":0,\"actualDriver\":false,\"actualDesktopCapture\":false,\"inputCalls\":0,\"runtimeQualified\":false}\n";return 0;}
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#else
int main(){std::cerr<<"Native SDK memory transport build is required\n";return 1;}
#endif
