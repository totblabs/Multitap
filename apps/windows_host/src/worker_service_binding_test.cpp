#include "service.hpp"
#include <iostream>
using namespace host;
namespace {
int checks=0;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
struct Child {
  WinHandle process,job;std::wstring releaseName;WinHandle release;
  Child(const std::wstring& arguments,bool sealed=false){
    job=WinHandle(CreateJobObjectW(nullptr,nullptr));JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(job.value&&SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"Own child job unavailable");
    SECURITY_DESCRIPTOR descriptor{};ACL empty{};SECURITY_ATTRIBUTES attributes{sizeof(attributes),&descriptor,FALSE};
    if(sealed){require(InitializeSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION)&&InitializeAcl(&empty,sizeof(empty),ACL_REVISION)&&SetSecurityDescriptorDacl(&descriptor,TRUE,&empty,FALSE),"Own sealed child descriptor unavailable");}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};const auto executable=executablePath();
    std::wstring command=L"\""+executable+L"\" "+arguments;
    require(CreateProcessW(executable.c_str(),command.data(),sealed?&attributes:nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup,&child),"Own child creation failed");
    process=WinHandle(child.hProcess);WinHandle thread(child.hThread);
    if(!AssignProcessToJobObject(job.value,process.value)){TerminateProcess(process.value,1);throw std::runtime_error("Own child isolation failed");}
    require(ResumeThread(thread.value)!=DWORD(-1),"Own child resume failed");
  }
  ~Child(){job=WinHandle();if(process.value)WaitForSingleObject(process.value,3000);}
  void successfulExit(){require(WaitForSingleObject(process.value,5000)==WAIT_OBJECT_0,"Own child did not exit");DWORD code=99;require(GetExitCodeProcess(process.value,&code)&&!code,"Own child rejected delegated service proof");}
};
NativeStreamLaunchReceipt receipt(const NativeStreamProcessIdentity& service,const NativeStreamProcessIdentity& worker){
  NativeStreamLaunchReceipt r;r.key={"binding-test",std::string(64,'a'),"ROOT\\DISPLAY\\00a1",2};r.service=service;r.worker=worker;
  r.scope={2,std::string(64,'b'),1000,{}};r.entry={service.pid,7,{{"0",{33,-1},100,1920,1080}}};
  r.lease=0xfedcba9876543210ull;r.outputs={{0xabcdef0102030405ull,1}};return r;
}
NativeStreamProfileRecord profile(const NativeStreamLaunchReceipt& r){NativeStreamProfileRecord p;p.key=r.key;p.entry=p.current=r.entry;p.scope=r.scope;p.scope.tick++;p.launchSha256=nativeBaselineDigest(nativeStreamLaunchBytes(r));return p;}
void sealedProcessProof(){
  const auto eventName=L"Local\\Multitap.Binding.Release."+randomName();WinHandle release(CreateEventW(nullptr,TRUE,FALSE,eventName.c_str()));require(release.value!=nullptr,"Own release event missing");
  Child service(L"--idle "+eventName,true);const auto serviceIdentity=nativeStreamProcessIdentity(service.process.value),self=nativeStreamProcessIdentity(GetCurrentProcess());
  WinHandle reopened(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,serviceIdentity.pid));
  const auto openError=GetLastError();require(!reopened.value&&openError==ERROR_ACCESS_DENIED,"Sealed owned process did not reproduce denied process reopen");
  const auto launch=receipt(serviceIdentity,self);const auto held=profile(launch);
  require(!nativeStreamLaunchWorkerProof(launch,held,serviceIdentity.pid,self,false),"Legacy reopen unexpectedly verified the sealed process");
  WinHandle query=duplicateWorkerServiceQueryHandle(service.process.value);DWORD flags=0;
  require(GetHandleInformation(query.value,&flags)&&!(flags&HANDLE_FLAG_INHERIT),"Delegated query handle is inheritable");
  require(nativeStreamLaunchWorkerProof(launch,held,serviceIdentity.pid,self,false,query.value),"Exact delegated lifetime could not replace denied reopen");
  require(nativeStreamLaunchWorkerProofFailure(launch,held,serviceIdentity.pid,self,false)==9&&nativeStreamLaunchWorkerProofFailure(launch,held,serviceIdentity.pid,self,false,query.value)==0,"Bounded diagnostic did not distinguish denied reopen from verified delegation");
  auto changed=launch;changed.service.created++;auto mismatched=profile(changed);
  require(!nativeStreamLaunchWorkerProof(changed,mismatched,serviceIdentity.pid,self,false,query.value),"Delegated handle bypassed service creation-time binding");
  require(nativeStreamLaunchWorkerProofFailure(changed,mismatched,serviceIdentity.pid,self,false,query.value)==7,"Bounded diagnostic lost delegated lifetime mismatch");
  WorkerServiceBinding binding{serviceIdentity,self.pid,uint64_t(reinterpret_cast<uintptr_t>(query.value)),std::string(64,'c')};
  const auto original=JsonParser::parse(workerServiceBindingPacket(binding));
  for(int n=0;n<7;n++){
    auto bad=original;auto& fields=std::get<Json::Object>(bad.value);
    if(n==0)fields["servicePid"].value=double(self.pid);
    if(n==1)fields["serviceCreated"].value=nativeStreamU64(serviceIdentity.created+1);
    if(n==2)fields["workerPid"].value=double(self.pid+1);
    if(n==3)fields["handle"].value=std::string(16,'f');
    if(n==4)fields["nonce"].value=std::string("unknown");
    if(n==5)fields["unexpected"].value=true;
    if(n==6)fields["type"].value=std::string("worker.probe");
    bool refused=false;try{std::string nonce;auto value=acceptWorkerServiceBinding(bad,serviceIdentity.pid,nonce);}catch(...){refused=true;}
    require(refused&&nativeStreamProcessIdentity(query.value)==serviceIdentity,"Malformed binding adopted/closed an unrelated valid handle");
  }
  std::string nonce;auto accepted=acceptWorkerServiceBinding(original,serviceIdentity.pid,nonce);query.value=nullptr;
  require(nonce==binding.nonce&&nativeStreamProcessIdentity(accepted.value)==serviceIdentity,"Exact binding did not adopt the expected kernel object");
  auto ack=JsonParser::parse(workerServiceBindingAck(serviceIdentity,nonce));require(workerServiceBindingAckMatches(ack,binding),"Exact binding ACK rejected");
  std::get<Json::Object>(ack.value)["nonce"].value=std::string(64,'d');require(!workerServiceBindingAckMatches(ack,binding),"Uncorrelated binding ACK accepted");
  require(SetEvent(release.value),"Own child release failed");service.successfulExit();
  require(!nativeStreamLaunchWorkerProof(launch,held,serviceIdentity.pid,self,false,accepted.value),"Signalled delegated service handle remained a live proof");
}
int boundChild(const std::wstring& pipeName,DWORD parent){
  require(WaitNamedPipeW(pipeName.c_str(),5000),"Own binding pipe missing");
  WinHandle pipe(CreateFileW(pipeName.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
  require(pipe.value!=INVALID_HANDLE_VALUE,"Own binding pipe open failed");ULONG peer=0;require(GetNamedPipeServerProcessId(pipe.value,&peer)&&peer==parent,"Own binding peer differs");
  std::atomic<bool> stopped{false};std::mutex write;bool text=false;std::vector<uint8_t> data;
  require(readPipePacket(pipe.value,text,data,stopped,std::chrono::seconds(5))&&text&&data.size()<4096,"Own binding packet missing");
  std::string nonce;auto service=acceptWorkerServiceBinding(JsonParser::parse(std::string(data.begin(),data.end())),peer,nonce);
  const auto identity=nativeStreamProcessIdentity(service.value),self=nativeStreamProcessIdentity(GetCurrentProcess());const auto launch=receipt(identity,self);
  require(nativeStreamLaunchWorkerProof(launch,profile(launch),peer,self,false,service.value),"Cross-process delegated handle failed exact launch proof");
  require(!TerminateProcess(service.value,1)&&GetLastError()==ERROR_ACCESS_DENIED,"Query-only delegation included process termination rights");
  const auto ack=workerServiceBindingAck(identity,nonce);require(pipePacket(pipe.value,true,{ack.begin(),ack.end()},stopped,write),"Own binding ACK write failed");return 0;
}
void actualBindingPipe(){
  const auto name=L"\\\\.\\pipe\\Multitap.Binding.QA."+randomName();
  WinHandle pipe(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr));require(pipe.value!=INVALID_HANDLE_VALUE,"Own binding listener failed");
  Child child(L"--bound "+name+L" "+std::to_wstring(GetCurrentProcessId()));
  const auto binding=createWorkerServiceBinding(child.process.value);
  WinHandle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));OVERLAPPED operation{};operation.hEvent=event.value;
  BOOL connected=ConnectNamedPipe(pipe.value,&operation);const auto error=GetLastError();
  if(!connected&&error==ERROR_IO_PENDING){require(WaitForSingleObject(event.value,5000)==WAIT_OBJECT_0,"Own child pipe connection timed out");DWORD bytes=0;connected=GetOverlappedResult(pipe.value,&operation,&bytes,FALSE);}
  else if(!connected&&error==ERROR_PIPE_CONNECTED)connected=TRUE;
  require(connected,"Own child not connected");ULONG peer=0;require(GetNamedPipeClientProcessId(pipe.value,&peer)&&peer==binding.workerPid,"Wrong child on binding pipe");
  std::atomic<bool> stopped{false};std::mutex write;const auto packet=workerServiceBindingPacket(binding);
  require(pipePacket(pipe.value,true,{packet.begin(),packet.end()},stopped,write),"Own binding packet write failed");
  bool text=false;std::vector<uint8_t> data;require(readPipePacket(pipe.value,text,data,stopped,std::chrono::seconds(5))&&text&&data.size()<4096&&workerServiceBindingAckMatches(JsonParser::parse(std::string(data.begin(),data.end())),binding),"Actual child ACK differs");
  child.successfulExit();
}
}
int wmain(int argc,wchar_t** argv){
  try{
    if(argc==3&&std::wstring(argv[1])==L"--idle"){WinHandle release(OpenEventW(SYNCHRONIZE,FALSE,argv[2]));return release.value&&WaitForSingleObject(release.value,20000)==WAIT_OBJECT_0?0:2;}
    if(argc==4&&std::wstring(argv[1])==L"--bound")return boundChild(argv[2],DWORD(_wtol(argv[3])));
    sealedProcessProof();actualBindingPipe();
    std::cout<<"{\"kind\":\"owned-worker-service-binding\",\"checks\":"<<checks<<",\"actualDeniedProcessReopen\":true,\"actualDuplicateHandle\":true,\"actualPrivatePipeHandshake\":true,\"terminationRightDenied\":true,\"installedServicesChanged\":false,\"windowsPolicyChanged\":false,\"crossSessionRuntimeTested\":false}\n";return 0;
  }catch(const std::exception& e){std::cerr<<"Worker service binding failed: "<<e.what()<<'\n';return 1;}
}
