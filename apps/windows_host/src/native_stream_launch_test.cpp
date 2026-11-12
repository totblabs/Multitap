#include "native_stream_launch.hpp"
#include <iostream>
using namespace host;
namespace {
int checks=0;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);++checks;}
template<class F>void rejected(F action,const char* message){bool threw=false;try{action();}catch(...){threw=true;}require(threw,message);}
NativeStreamLaunchReceipt fixture(){
  NativeStreamLaunchReceipt value;
  value.key={"stream-149",std::string(64,'a'),"ROOT\\DISPLAY\\00a1",2};
  value.service={4321,0x01dce33a11223344ull};value.worker={4322,0x01dce33a55667788ull};
  value.scope={2,std::string(64,'b'),1000,{}};
  value.entry={4321,7,{{"0",{33,-1},100,1920,1080},{"1",{33,-1},101,1280,720}}};
  value.lease=0xfedcba9876543210ull;value.outputs={{0xabcdef0102030405ull,1},{0xf0e0d0c0b0a09081ull,2}};return value;
}
NativeStreamProfileRecord linked(const NativeStreamLaunchReceipt& value){
  NativeStreamProfileRecord record;record.key=value.key;record.entry=record.current=value.entry;
  record.scope=value.scope;record.scope.tick++;record.launchSha256=nativeBaselineDigest(nativeStreamLaunchBytes(value));return record;
}
std::string replace(std::string value,const std::string& before,const std::string& after){
  const auto at=value.find(before);require(at!=std::string::npos,"Missing malformed-fixture field");value.replace(at,before.size(),after);return value;
}
void codec(){
  const auto value=fixture(),decoded=parseNativeStreamLaunch(nativeStreamLaunchBytes(fixture()));
  require(decoded.lease==value.lease&&decoded.worker==value.worker&&decoded.outputs[1].display==value.outputs[1].display,"64-bit private identities lost precision");
  require(nativeStreamLaunchBytes(value)==nativeStreamLaunchBytes(decoded),"Receipt did not round-trip canonically");
  for(const uint64_t number:{uint64_t(0),uint64_t(1),UINT64_MAX,uint64_t(9007199254740993ull)})require(parseNativeStreamU64(JsonParser::parse(jsonQuote(nativeStreamU64(number))))==number,"Lossless integer codec failed");
  const auto bytes=nativeStreamLaunchBytes(value);
  for(const auto& malformed:std::vector<std::string>{
      "", "{}",bytes+" ",replace(bytes,"\"schema\":1","\"schema\":2"),
      replace(bytes,"\"fedcba9876543210\"","\"FEDCBA9876543210\""),
      replace(bytes,"\"fedcba9876543210\"","18364758544493064720"),
      replace(bytes,"\"fedcba9876543210\"","\"edcba9876543210\""),
      replace(bytes,"\"session\":2","\"session\":2.5"),
      replace(bytes,"\"pid\":4322","\"pid\":4321"),
      replace(bytes,"\"connector\":2","\"connector\":1"),
      replace(bytes,"\"f0e0d0c0b0a09081\"","\"abcdef0102030405\""),
      replace(bytes,"\"kind\":","\"extra\":true,\"kind\":"),std::string(8193,'x')})
    rejected([&]{(void)parseNativeStreamLaunch(malformed);},"Malformed launch receipt accepted");
  for(const auto& change:std::vector<std::function<void(NativeStreamLaunchReceipt&)>>{
      [](auto& v){v.key.session=0;},[](auto& v){v.key.operation="invalid";},[](auto& v){v.service.created=0;},
      [](auto& v){v.scope.logon="unknown";},[](auto& v){v.scope.session=3;},[](auto& v){v.scope.tick=0;},
      [](auto& v){v.lease=1;},[](auto& v){v.entry.generation=0;},[](auto& v){v.outputs.clear();},
      [](auto& v){v.outputs[0].display=0;},[](auto& v){v.entry.targets[1].id="0";}}){
    auto changed=value;change(changed);rejected([&]{(void)nativeStreamLaunchBytes(changed);},"Invalid launch binding serialized");
  }
  const auto record=linked(value);require(nativeStreamLaunchMatches(value,record),"Matching immutable launch binding rejected");
  for(const auto& change:std::vector<std::function<void(NativeStreamProfileRecord&)>>{
      [](auto& r){r.launchSha256.clear();},[](auto& r){r.key.operation=std::string(64,'c');},[](auto& r){r.key.session++;},
      [](auto& r){r.entry.generation++;},[](auto& r){r.entry.targets[0].target++;},[](auto& r){r.scope.logon=std::string(64,'c');},
      [](auto& r){r.scope.tick=999;}}){auto changed=record;change(changed);require(!nativeStreamLaunchMatches(value,changed),"Different profile inherited launch authority");}
}
void persistence(){
  std::optional<std::string> bytes,profile;int saves=0;bool gone=false,loseAck=false,replaceAfterSave=false;
  NativeRuntimeJournalAccess store{[&]{return bytes;},[&](const auto& expected,const auto& desired){++saves;if(expected!=bytes)return false;bytes=replaceAfterSave?"{}":desired;return !loseAck;}};
  NativeRuntimeJournalAccess profiles{[&]{return profile;},{}};
  const auto exited=[&](const auto&){return gone;};auto value=fixture();
  require(saveNativeStreamLaunch(store,profiles,value,exited)==nativeBaselineDigest(nativeStreamLaunchBytes(value)),"Durable launch save failed");
  require(saveNativeStreamLaunch(store,profiles,value,exited).has_value()&&saves==1,"Identical acknowledged receipt rewritten");
  value.key.operation=std::string(64,'c');value.worker.created++;
  require(!saveNativeStreamLaunch(store,profiles,value,exited)&&saves==1,"Live original worker authority replaced");
  gone=true;require(saveNativeStreamLaunch(store,profiles,value,exited).has_value(),"Exited original worker prevented next launch");
  {auto raced=value;raced.key.operation=std::string(64,'d');const auto before=bytes;
    require(!saveNativeStreamLaunch(store,profiles,raced,[&](const auto&){profile="{}";return true;})&&bytes==before,"Late profile preparation was overwritten after worker exit");profile.reset();}
  value.worker.created++;require(!saveNativeStreamLaunch(store,profiles,value,exited),"Same operation rebound to another worker");
  value.key.operation=std::string(64,'e');profile="{}";require(!saveNativeStreamLaunch(store,profiles,value,exited),"Malformed prior profile discarded");profile.reset();
  loseAck=true;require(!saveNativeStreamLaunch(store,profiles,value,exited),"Lost save ACK authorized first start");loseAck=false;
  require(saveNativeStreamLaunch(store,profiles,value,exited).has_value(),"Exact durable retry could not recover lost save ACK");
  value.key.operation=std::string(64,'f');replaceAfterSave=true;require(!saveNativeStreamLaunch(store,profiles,value,exited),"Changed readback authorized start");
  const auto directory=std::filesystem::current_path()/(L"native-launch-qa-"+randomName());
  require(std::filesystem::create_directory(directory),"Cannot create isolated receipt fixture");
  const auto path=directory/L"native-stream-launch.json";
  struct Cleanup{std::filesystem::path file,dir;~Cleanup(){std::error_code ec;std::filesystem::remove(file,ec);std::filesystem::remove(dir,ec);}}cleanup{path,directory};
  const auto trusted=[&](const std::filesystem::path& p){return p==directory||p.parent_path()==directory;};
  auto disk=nativeStreamLaunchFileAccess(path,trusted);const auto serialized=nativeStreamLaunchBytes(fixture());
  require(!disk.load()&&disk.save({},serialized)&&disk.load()==serialized,"Actual file flush/readback failed");
  require(!disk.save({},serialized),"Stale CAS replaced existing receipt");
  require(!disk.save(serialized,"{}")&&disk.load()==serialized,"Invalid data replaced durable authority");
  auto denied=nativeStreamLaunchFileAccess(path,[](const auto&){return false;});rejected([&]{(void)denied.load();},"Untrusted file accepted");
  rejected([&]{(void)nativeStreamLaunchFileAccess(directory/L"native-stream-profile.json",trusted);},"Receipt could overwrite profile journal");
}
void quarantine(){
  std::optional<std::string> profile,launch;NativeRuntimeJournalAccess profiles{[&]{return profile;},{}},launches{[&]{return launch;},{}};
  const auto receipt=fixture();const NativeStreamProcessIdentity newService{9876,123456789};
  NativeStreamProcessState service=NativeStreamProcessState::gone,worker=NativeStreamProcessState::gone;
  const auto probe=[&](const NativeStreamProcessIdentity& process){return process==receipt.service?service:worker;};
  require(!nativeStreamLaunchQuarantined(profiles,launches,newService,probe),"Fresh install quarantined without old authority");
  launch=nativeStreamLaunchBytes(receipt);
  const auto allGone=[](const auto&){return NativeStreamProcessState::gone;};
  require(nativeStreamUnpreparedLaunchRetirable(profiles,launches,receipt.key,receipt.entry.generation,receipt.service,true,true,allGone),"Unprepared current launch could not retire after actual worker exit");
  require(!nativeStreamUnpreparedLaunchRetirable(profiles,launches,receipt.key,receipt.entry.generation,receipt.service,true,false,allGone),"Unobserved owned worker exit retired reservation");
  auto wrongKey=receipt.key;wrongKey.operation=std::string(64,'c');
  require(!nativeStreamUnpreparedLaunchRetirable(profiles,launches,wrongKey,receipt.entry.generation,receipt.service,true,true,allGone),"Dispatched command used another operation's receipt");
  require(!nativeStreamUnpreparedLaunchRetirable(profiles,launches,receipt.key,receipt.entry.generation+1,receipt.service,true,true,allGone),"Dispatched command used another generation's receipt");
  launch.reset();
  require(!nativeStreamUnpreparedLaunchRetirable(profiles,launches,receipt.key,receipt.entry.generation,receipt.service,true,true,allGone),"Dispatched command lost its durable launch receipt");
  require(nativeStreamUnpreparedLaunchRetirable(profiles,launches,receipt.key,receipt.entry.generation,receipt.service,false,true,allGone),"Undispatched command with quiescent worker could not retire");launch=nativeStreamLaunchBytes(receipt);
  service=NativeStreamProcessState::running;require(nativeStreamLaunchQuarantined(profiles,launches,newService,probe),"Live prior service admitted competing activation");
  service=NativeStreamProcessState::gone;worker=NativeStreamProcessState::running;require(nativeStreamLaunchQuarantined(profiles,launches,newService,probe),"Launch-before-prepare crash lost worker fence");
  worker=NativeStreamProcessState::unknown;require(nativeStreamLaunchQuarantined(profiles,launches,newService,probe),"Unqueryable old worker admitted activation");
  worker=NativeStreamProcessState::gone;service=NativeStreamProcessState::unknown;require(nativeStreamLaunchQuarantined(profiles,launches,newService,probe),"Unqueryable old service admitted activation");
  service=NativeStreamProcessState::gone;require(!nativeStreamLaunchQuarantined(profiles,launches,newService,probe),"Both exited original lifetimes with no prepared profile could not retire");
  service=NativeStreamProcessState::running;require(!nativeStreamLaunchQuarantined(profiles,launches,receipt.service,probe),"Same service could not retire its exited unprepared worker");
  service=NativeStreamProcessState::gone;
  require(nativeStreamLaunchQuarantined(profiles,launches,newService,[&](const auto& process){if(process==receipt.worker)profile="{}";return NativeStreamProcessState::gone;}),"Profile prepared during process-exit query was lost");profile.reset();
  require(nativeStreamLaunchQuarantined(profiles,launches,newService,[&](const auto& process){if(process==receipt.worker)launch="{}";return NativeStreamProcessState::gone;}),"Receipt changed during process-exit query was lost");
  require(nativeStreamLaunchQuarantined(profiles,launches,newService,probe),"Malformed launch did not quarantine");
}
struct Child {
  WinHandle release,job,process;
  Child(){
    const auto event=L"Local\\Multitap.LaunchQA."+randomName();release=WinHandle(CreateEventW(nullptr,TRUE,FALSE,event.c_str()));require(release.value!=nullptr,"Cannot create child release event");
    job=WinHandle(CreateJobObjectW(nullptr,nullptr));JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(job.value&&SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"Cannot fence QA child");
    auto command=L"\""+executablePath()+L"\" --process-child "+event;STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION created{};
    require(CreateProcessW(executablePath().c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&startup,&created)!=FALSE,"Cannot start owned QA child");
    process=WinHandle(created.hProcess);WinHandle thread(created.hThread);
    if(!AssignProcessToJobObject(job.value,process.value)){TerminateProcess(process.value,1);WaitForSingleObject(process.value,5000);throw std::runtime_error("Cannot fence owned QA process");}
    require(ResumeThread(thread.value)!=DWORD(-1),"Cannot resume owned QA child");
  }
  ~Child(){SetEvent(release.value);WaitForSingleObject(process.value,5000);}
  void finish(){require(SetEvent(release.value)&&WaitForSingleObject(process.value,5000)==WAIT_OBJECT_0,"Owned QA child did not exit");}
};
void processes(){
  const auto self=nativeStreamProcessIdentity(GetCurrentProcess());require(self.pid==GetCurrentProcessId()&&self.created,"Actual process creation identity missing");
  require(observeNativeStreamProcess(self).state==NativeStreamProcessState::running,"Live process incorrectly proved gone");
  auto reused=self;reused.created++;require(observeNativeStreamProcess(reused).state==NativeStreamProcessState::gone,"Reused PID did not prove old lifetime absent");
  require(WaitForSingleObject(GetCurrentProcess(),0)==WAIT_TIMEOUT,"PID reuse probe terminated an unrelated process");
  require(classifyNativeStreamProcess(self,{},WAIT_OBJECT_0)==NativeStreamProcessState::unknown&&classifyNativeStreamProcess(self,self,WAIT_FAILED)==NativeStreamProcessState::unknown,"Failed process query admitted recovery");
  require(observeNativeStreamProcess({0,1}).state==NativeStreamProcessState::unknown,"Invalid process identity admitted recovery");
  Child original;auto value=fixture();value.service=self;value.worker=nativeStreamProcessIdentity(original.process.value);value.entry.servicePid=self.pid;const auto profile=linked(value);
  require(nativeStreamLaunchWorkerProof(value,profile,self.pid,value.worker,false),"Original exact live worker proof failed");
  require(!nativeStreamLaunchWorkerProof(value,profile,self.pid,value.worker,true),"Original worker impersonated recovery worker");
  Child recovery;const auto other=nativeStreamProcessIdentity(recovery.process.value);
  require(!nativeStreamLaunchWorkerProof(value,profile,self.pid,other,true),"Live original worker allowed competing recovery");
  original.finish();DWORD exit=0;require(GetExitCodeProcess(original.process.value,&exit)&&exit==STILL_ACTIVE,"Exit-code 259 fixture not exercised");
  require(observeNativeStreamProcess(value.worker).state==NativeStreamProcessState::gone,"Signalled process with exit code 259 considered running");
  require(nativeStreamLaunchWorkerProof(value,profile,self.pid,other,true),"Verified original process exit rejected same-service cleanup");
  require(!nativeStreamLaunchWorkerProof(value,profile,self.pid+1,other,true),"New service inherited same-service cleanup authority");
  require(!nativeStreamLaunchWorkerProof(value,profile,self.pid,self,true),"Service process substituted for recovery worker");recovery.finish();
}
}
int wmain(int argc,wchar_t** argv){
  if(argc==3&&std::wstring(argv[1])==L"--process-child"){
    WinHandle release(OpenEventW(SYNCHRONIZE,FALSE,argv[2]));if(!release.value)return 2;
    return WaitForSingleObject(release.value,20000)==WAIT_OBJECT_0?int(STILL_ACTIVE):3;
  }
  try{codec();persistence();quarantine();processes();std::cout<<"Native stream launch: "<<checks<<" checks passed. Actual owned process/file lifecycle; no driver, service or display changes.\n";return 0;}
  catch(const std::exception& e){std::cerr<<"Native stream launch failed: "<<e.what()<<'\n';return 1;}
}
