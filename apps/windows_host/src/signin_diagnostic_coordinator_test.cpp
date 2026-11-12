// Test TU only: own unelevated process/pipe supplies a revocable inlet handle.
// The control-flow adapter below performs no SYSTEM launch, SDK, CCD or input.
#include "signin_diagnostic_inlet.cpp"
#include "signin_diagnostic_coordinator.hpp"
#include <iostream>
#include <thread>

namespace host::signin::diagnostic {
struct TestAccess {
  static VerifiedOperator own(HANDLE pipe,HANDLE stop,const std::shared_ptr<std::atomic<bool>>& alive){
    auto state=std::make_shared<VerifiedOperator::State>();
    state->process=WinHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,GetCurrentProcessId()));
    state->operatorIdentity=processIdentity(state->process.value);state->serviceIdentity=state->operatorIdentity;
    state->pipe=pipe;state->stop=stop;state->begun=GetTickCount64();state->deadline=state->begun+30000;
    if(BCryptGenRandom(nullptr,state->nonce.data(),DWORD(state->nonce.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
      throw std::runtime_error("Own challenge unavailable");
    const std::weak_ptr<VerifiedOperator::State> weak=state;
    state->check=[weak,alive]{const auto held=weak.lock();return held&&alive->load()&&
      processIdentity(held->process.value)==held->operatorIdentity&&WaitForSingleObject(held->process.value,0)==WAIT_TIMEOUT;};
    return VerifiedOperator(state);
  }
};
struct CoordinatorTestAccess {
  static std::unique_ptr<Coordinator> create(std::unique_ptr<coordinator_detail::Operations> operations,HANDLE stop){
    return std::unique_ptr<Coordinator>(new Coordinator(std::move(operations),stop));
  }
};
}
namespace {
using namespace host;
namespace d=host::signin::diagnostic;
using namespace d::coordinator_detail;
size_t checks=0;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
struct OwnPipe {
  WinHandle server,client,event,stop;
  std::shared_ptr<std::atomic<bool>> alive=std::make_shared<std::atomic<bool>>(true);
  OwnPipe(){
    const auto name=L"\\\\.\\pipe\\Multitap.Test.DiagnosticCoordinator."+std::to_wstring(GetCurrentProcessId())+L'.'+randomName();
    // Process-private random local endpoint. No real service or admin identity.
    server=WinHandle(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,8192,8192,0,nullptr));
    event=WinHandle(CreateEventW(nullptr,TRUE,FALSE,nullptr));stop=WinHandle(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    require(server.value!=INVALID_HANDLE_VALUE&&event.value&&stop.value,"Own pipe/events unavailable");
    OVERLAPPED operation{};operation.hEvent=event.value;
    const BOOL connected=ConnectNamedPipe(server.value,&operation);const DWORD error=GetLastError();
    require(connected||error==ERROR_IO_PENDING,"Own connection unavailable");
    client=WinHandle(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
    require(client.value!=INVALID_HANDLE_VALUE,"Own client unavailable");
    if(!connected){DWORD bytes=0;require(WaitForSingleObject(event.value,2000)==WAIT_OBJECT_0&&
      GetOverlappedResult(server.value,&operation,&bytes,FALSE),"Own connection incomplete");}
  }
  d::VerifiedOperator peer(){return d::TestAccess::own(server.value,stop.value,alive);}
};
struct Fake final:Operations {
  Start start=Start::started;bool claimOnBegin=true,claimed=false,current=true,prepared=true,finish=true,released=true;
  Poll prepPoll=Poll::finished,restorePoll=Poll::finished;
  Exit prepExit=Exit::exited,restoreExit=Exit::none,launchedRestoreExit=Exit::exited;
  Original originalState=Original::owned;
  bool restoreStarts=true,stopMakesExit=false,realPause=false,throwAfterClaim=false;
  bool sameOriginal=true,privateLeaseCurrent=true;
  Exit olderRestorerExit=Exit::exited;
  uint32_t beginCount=0,prepStops=0,restoreStartsCount=0,restoreStops=0,finishes=0,releases=0;
  uint32_t rearms=0;
  std::atomic<uint32_t> heartbeats=0;
  uint64_t time=GetTickCount64();std::function<void()> onPrep,onFinish,onRestore;
  Start begin(d::VerifiedOperator&&,const d::Request&)override{
    ++beginCount;claimed=claimOnBegin;if(throwAfterClaim)throw std::runtime_error("fixed");return start;
  }
  bool engaged()const noexcept override{return claimed;}
  bool forwardCurrent()const noexcept override{return current&&!forwardCancelled();}
  Poll preparation()override{if(onPrep)onPrep();return prepPoll;}
  bool preparationSucceeded()const noexcept override{return prepared;}
  Exit preparationExit()override{return prepExit;}
  void stopPreparation()noexcept override{++prepStops;if(stopMakesExit)prepExit=Exit::exited;}
  Original original()override{return originalState;}
  bool startRestoration()override{
    ++restoreStartsCount;
    if(restoreStarts){restoreExit=launchedRestoreExit;return true;}return false;
  }
  bool rearmRestoration()override{
    ++rearms;return claimed&&originalState==Original::owned&&sameOriginal&&privateLeaseCurrent&&
      prepExit==Exit::exited&&restoreExit==Exit::exited&&olderRestorerExit==Exit::exited;
  }
  Poll restoration()override{if(onRestore)onRestore();return restorePoll;}
  Exit restorationExit()override{return restoreExit;}
  void stopRestoration()noexcept override{++restoreStops;if(stopMakesExit)restoreExit=Exit::exited;}
  bool finishRestoration()override{++finishes;if(onFinish)onFinish();return finish;}
  bool releaseReservation()override{++releases;if(released){claimed=false;return true;}return false;}
  bool heartbeat()noexcept override{++heartbeats;return true;}
  uint64_t now()const noexcept override{return time;}
  void pause(uint32_t milliseconds)noexcept override{time+=milliseconds;if(realPause)Sleep(milliseconds);}
};
struct Case {
  OwnPipe pipe;Fake* fake=nullptr;std::unique_ptr<d::Coordinator> coordinator;
  Case(){auto value=std::make_unique<Fake>();fake=value.get();coordinator=d::CoordinatorTestAccess::create(std::move(value),pipe.stop.value);}
  d::Result run(const d::Coordinator::FinalAdmission& check=[] {return true;}){
    return coordinator->prepare(pipe.peer(),{1280,800},check);
  }
  void tick(unsigned count=12){for(unsigned i=0;i<count;++i){fake->time+=1000;(void)coordinator->tickCleanup();}}
};
void admission(){
  {Case c;unsigned calls=0;const auto result=c.run([&]{++calls;require(c.coordinator->busy(),"Final admission preceded the shared busy fence");return false;});
    require(!result.ok&&result.code==d::Code::refused&&calls==1,"Denied final admission result");
    require(c.fake->beginCount==0&&!c.coordinator->busy(),"Denied admission acquired resources");}
  {Case c;const auto result=c.run([]()->bool{throw std::runtime_error("fixed");});
    require(!result.ok&&!c.coordinator->busy()&&c.fake->beginCount==0,"Throwing final admission acquired resources");}
  {Case c;c.pipe.alive->store(false);const auto result=c.run();
    require(!result.ok&&!c.coordinator->busy()&&c.fake->beginCount==0,"Retired inlet began preparation");}
  {Case c;const auto result=c.coordinator->prepare(c.pipe.peer(),{1280,800},{});
    require(!result.ok&&!c.coordinator->busy()&&c.fake->beginCount==0,"Missing final admission accepted");}
  {Case c;unsigned calls=0;const auto result=c.run([&]{++calls;return true;});
    require(result.ok&&result.code==d::Code::prepared&&!c.coordinator->busy(),"Complete roundtrip not prepared");
    require(calls==1&&c.fake->beginCount==1&&c.fake->restoreStartsCount==1&&c.fake->finishes==1&&c.fake->releases==1,
      "Roundtrip repeated admission or skipped cleanup");}
}
void physicalExits(){
  {Case c;c.fake->prepPoll=Poll::failed;c.fake->prepExit=Exit::alive;
    const auto result=c.run();require(!result.ok&&c.coordinator->busy(),"EOF released a live predecessor");
    require(c.fake->restoreStartsCount==0&&c.fake->releases==0,"Restorer/release preceded physical exit");
    const auto again=c.run();require(again.code==d::Code::busy&&c.fake->beginCount==1,"Busy scope began a new forward operation");
    c.fake->prepExit=Exit::exited;c.tick();require(!c.coordinator->busy()&&c.fake->restoreStartsCount==1&&c.fake->releases==1,
      "Cleanup did not resume after held predecessor exit");}
  {Case c;c.fake->prepPoll=Poll::failed;c.fake->prepExit=Exit::unknown;
    (void)c.run();c.tick();const auto result=c.coordinator->shutdown();
    require(!result.ok&&c.coordinator->busy()&&c.fake->restoreStartsCount==0&&c.fake->releases==0,
      "Unknown predecessor/shutdown freed the obligation");}
  {Case c;c.fake->restorePoll=Poll::waiting;
    // Request observer expiry leaves the SAME restorer and original owned.
    (void)c.run();require(c.coordinator->busy()&&c.fake->restoreStartsCount==1&&c.fake->releases==0,
      "Pending restorer released original/reservation");
    c.fake->restorePoll=Poll::finished;c.pipe.alive->store(false);c.tick();
    require(!c.coordinator->busy()&&c.fake->finishes==1&&c.fake->releases==1,
      "Operator loss improperly revoked cleanup-only restoration");}
  {Case c;c.fake->launchedRestoreExit=Exit::alive;
    (void)c.run();require(c.coordinator->busy()&&c.fake->finishes==0&&c.fake->releases==0,
      "Finished receipt substituted for physical restorer exit");
    c.fake->restoreExit=Exit::exited;c.tick();
    require(!c.coordinator->busy()&&c.fake->finishes==1&&c.fake->releases==1,
      "Held restorer exit did not permit final retirement");}
  {Case c;c.fake->prepPoll=Poll::failed;c.fake->prepExit=Exit::none;c.fake->originalState=Original::absent;
    const auto result=c.run();require(!result.ok&&!c.coordinator->busy()&&c.fake->restoreStartsCount==0&&c.fake->releases==1,
      "Failed pre-create operation with positively absent original was not released safely");}
}
void persistence(){
  {Case c;c.fake->finish=false;(void)c.run();
    require(c.coordinator->busy()&&c.fake->releases==0,"Unverified topology receipt retired the scope");
    c.fake->originalState=Original::absent;c.tick();
    require(c.coordinator->busy()&&c.fake->releases==0,"Path absence substituted for completed restoration");}
  {Case c;c.fake->finish=false;c.fake->onFinish=[&]{if(c.fake->finishes==2)c.fake->finish=true;};
    const auto result=c.run();require(result.ok&&c.fake->finishes>=2&&c.fake->restoreStartsCount==1&&c.fake->releases==1,
      "Same-fresh-receipt sharing retry did not retain exact restoration");}
  {Case c;c.fake->released=false;(void)c.run();require(c.coordinator->busy()&&c.fake->finishes==1,
      "Failed reservation publication was reported complete");
    const auto begins=c.fake->beginCount,restores=c.fake->restoreStartsCount;
    c.fake->released=true;c.tick();require(!c.coordinator->busy()&&c.fake->beginCount==begins&&c.fake->restoreStartsCount==restores,
      "Reservation retry repeated forward/restore work");}
  {Case c;c.fake->restoreStarts=false;(void)c.run();c.tick();
    require(c.coordinator->busy()&&c.fake->releases==0&&c.fake->beginCount==1,
      "Restorer launch refusal forgot original obligation");
    c.fake->restoreStarts=true;c.tick();require(!c.coordinator->busy(),"Deferred cleanup could not resume");}
  {Case c;c.fake->start=Start::failed;c.fake->claimed=true;c.fake->prepExit=Exit::alive;
    (void)c.run();require(c.coordinator->busy()&&c.fake->restoreStartsCount==0&&c.fake->releases==0,
      "Failed launch status hid an already created process");}
  {Case c;c.fake->start=Start::refused;c.fake->claimOnBegin=false;
    const auto result=c.run();require(result.code==d::Code::refused&&!c.coordinator->busy()&&c.fake->releases==0,
      "Refused before reservation fabricated release");}
  {Case c;c.fake->throwAfterClaim=true;c.fake->prepExit=Exit::alive;
    const auto result=c.run();require(!result.ok&&c.coordinator->busy()&&c.fake->releases==0,
      "Exception after reservation acquisition forgot the obligation");
    c.fake->prepExit=Exit::exited;c.tick();require(!c.coordinator->busy()&&c.fake->beginCount==1,
      "Post-exception cleanup retried forward preparation");}
  {Case c;auto marker=std::make_shared<int>(1);const std::weak_ptr<int> weak=marker;
    c.fake->prepExit=Exit::alive;c.fake->prepPoll=Poll::failed;
    {auto callback=[marker]{return *marker==1;};marker.reset();(void)c.run(callback);}
    require(weak.expired()&&c.coordinator->busy(),"FinalAdmission closure escaped synchronous call scope");}
  {Case c;const auto flag=c.coordinator->busyFlag();
    require(c.run().ok&&c.run().ok&&c.fake->beginCount==2&&c.fake->restoreStartsCount==2&&
      c.coordinator->busyFlag()==flag&&!flag->load(),"Completed operation corrupted next operator scope/shared flag");}
}
void concurrency(){
  {Case c;c.fake->prepExit=Exit::alive;c.fake->prepPoll=Poll::failed;
    std::thread first([&]{(void)c.run();});first.join();
    c.fake->prepExit=Exit::exited;
    // prepare's lexical guard has ended on another thread. No retained guard
    // is ever unlocked here; persistent resources live independently of it.
    c.tick();require(!c.coordinator->busy(),"Cross-thread cleanup retained a foreign mutex guard");}
  {Case c;c.fake->realPause=true;bool observed=false;
    c.fake->onPrep=[&]{
      if(observed)return;observed=true;const auto before=c.fake->heartbeats.load();Sleep(650);
      require(c.fake->heartbeats.load()>before,"Lease heartbeat blocked behind forward lexical lock");
    };
    const auto result=c.run();require(result.ok&&observed,"Heartbeat control case did not complete");}
  {Case c;c.fake->prepPoll=Poll::waiting;
    c.fake->onPrep=[&]{c.coordinator->cancelForward();};
    const auto result=c.run();require(!result.ok&&c.fake->prepStops>0&&c.fake->restoreStartsCount==1&&!c.coordinator->busy(),
      "Forward cancellation skipped cleanup or reported prepared");}
}
void exhaustTwo(Case& c){
  c.fake->restorePoll=Poll::failed;bool observerExpired=false;
  c.fake->onRestore=[&]{if(c.fake->restoreStartsCount==2&&!observerExpired){
    observerExpired=true;c.fake->time+=30000;
  }};
  const auto result=c.run();c.fake->onRestore={};
  require(!result.ok&&c.coordinator->busy()&&c.fake->restoreStartsCount==2&&c.fake->releases==0,
    "two physically-exited failed restorers were not retained");
  (void)c.coordinator->tickCleanup(); // observes exact second exit, schedules 5s
}
void cleanupRearm(){
  {Case c;exhaustTwo(c);c.fake->restorePoll=Poll::finished;
    const auto before=c.fake->time;c.fake->time=before+4999;
    (void)c.coordinator->tickCleanup();
    require(c.fake->restoreStartsCount==2&&c.fake->rearms==0&&c.coordinator->busy(),"cleanup retry ignored five-second first backoff");
    ++c.fake->time;(void)c.coordinator->tickCleanup();
    require(c.fake->restoreStartsCount==3&&c.fake->rearms==1&&c.fake->beginCount==1&&c.coordinator->busy(),
      "healed original cleanup could not rearm, or began a new forward scope");
    c.tick(6);require(!c.coordinator->busy()&&c.fake->finishes==1&&c.fake->releases==1,
      "healed two-failure scope did not complete actual retirement/release exactly once");}
  for(unsigned field=0;field<6;++field){Case c;exhaustTwo(c);c.fake->restorePoll=Poll::finished;
    switch(field){case 0:c.fake->prepExit=Exit::alive;break;case 1:c.fake->olderRestorerExit=Exit::unknown;break;
      case 2:c.fake->olderRestorerExit=Exit::alive;break;case 3:c.fake->sameOriginal=false;break;
      case 4:c.fake->originalState=Original::unknown;break;default:c.fake->privateLeaseCurrent=false;break;}
    c.fake->time+=5000;(void)c.coordinator->tickCleanup();
    require(c.coordinator->busy()&&c.fake->rearms==1&&c.fake->restoreStartsCount==2&&c.fake->releases==0,
      "live/unknown predecessor, changed original or changed lease rearmed cleanup");
    for(unsigned i=0;i<100;++i)(void)c.coordinator->tickCleanup();
    require(c.fake->rearms==1&&c.fake->restoreStartsCount==2,"refused rearm entered fast retry loop");
    c.fake->time+=29999;(void)c.coordinator->tickCleanup();require(c.fake->rearms==1,"second backoff shorter than thirty seconds");
    ++c.fake->time;(void)c.coordinator->tickCleanup();require(c.fake->rearms==2&&c.coordinator->busy(),"thirty-second recovery check missing");
    c.fake->time+=59999;(void)c.coordinator->tickCleanup();require(c.fake->rearms==2,"later backoff shorter than sixty seconds");
    ++c.fake->time;(void)c.coordinator->tickCleanup();require(c.fake->rearms==3&&c.fake->restoreStartsCount==2&&
      c.fake->beginCount==1&&c.fake->releases==0,"bounded cleanup-only check reset scope or released by timeout");
  }
}
}
int main(){try{admission();physicalExits();persistence();concurrency();cleanupRearm();
  std::cout<<"PASS "<<checks<<" coordinator checks (injected resources; no SYSTEM/SDK/capture/input)\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
