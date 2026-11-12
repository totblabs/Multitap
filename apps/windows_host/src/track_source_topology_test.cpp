// Pure topology-watch fixtures. No media runtime, encoder, desktop enumeration,
// driver, network, capture or input API is used by this test.
#include "track_source.hpp"
#include <future>
#include <iostream>
#include <source_location>
using namespace host;
namespace {
unsigned checks=0;
void require(bool value,const char* message,const std::source_location where=std::source_location::current()){
  ++checks;if(!value)throw std::runtime_error(std::string(message)+" line "+std::to_string(where.line()));
}
TrackSourcePlan plan(){TrackSourcePlan value;OwnedTrackSource out;out.capture.bounds={0,0,640,360};out.capture.monitors.push_back({L"\\\\.\\DISPLAY2",L"owned0",{0,0,640,360},true});value.outputs.push_back(std::move(out));return value;}
std::vector<CaptureMonitor> monitors(){return {{L"\\\\.\\DISPLAY2",L"owned0",{0,0,640,360},true},{L"\\\\.\\DISPLAY1",L"foreign",{1000,0,2280,800},false}};}
struct Observed {
  std::mutex mutex;std::condition_variable seen;std::vector<CaptureMonitor> value=monitors();
  const std::thread::id owner=std::this_thread::get_id();unsigned calls=0,mainCalls=0,revision=0,lastSeen=0;bool fail=false;
  std::vector<CaptureMonitor> read(){std::lock_guard lock(mutex);++calls;if(std::this_thread::get_id()==owner)++mainCalls;lastSeen=revision;seen.notify_all();if(fail)throw std::runtime_error("enumeration_fixture_failed");return value;}
  template<class Change>void change(Change fn){std::lock_guard lock(mutex);fn(*this);++revision;}
  void awaitChange(){std::unique_lock lock(mutex);require(seen.wait_for(lock,std::chrono::seconds(2),[&]{return lastSeen==revision;}),"Async watch did not enumerate changed fixture");}
};
std::string failure(CaptureTopologyWatch& watch,Observed& observed){const auto until=Clock::now()+std::chrono::seconds(2);for(;;){try{watch.verify();}catch(const std::exception& e){return e.what();}if(Clock::now()>=until)return {};std::unique_lock lock(observed.mutex);observed.seen.wait_for(lock,std::chrono::milliseconds(1));}}
void hotPath(){
  Observed state;auto selected=plan();auto watch=watchTrackSourceTopology(selected,[&]{return state.read();});
  // The actual TrackSourcePipeline::step calls precisely this verify method.
  for(int i=0;i<10000;++i)watch->verify();
  {std::lock_guard lock(state.mutex);require(state.mainCalls==1,"Hot capture ticks synchronously enumerated monitors");}
  selected.outputs[0].capture.monitors[0].identity=L"changed-caller-copy";
  state.change([](Observed&){});state.awaitChange();watch->verify();require(true,"Immutable selection copy was not retained");
}
void initialValidation(){
  for(int kind=0;kind<3;++kind){Observed state;if(kind==0)state.value[0].identity=L"wrong";if(kind==1)state.value.erase(state.value.begin());if(kind==2)state.value[0].bounds.right++;
    bool rejected=false;try{auto watch=watchTrackSourceTopology(plan(),[&]{return state.read();});}catch(const std::runtime_error& e){rejected=std::string_view(e.what())=="Selected virtual monitor changed; reconnect the session";}
    require(rejected,"Invalid initial ownership/geometry reached capture construction");require(state.calls==1&&state.mainCalls==1,"Invalid initial state started an async watch");
  }
}
void asyncFailures(){
  for(int kind=0;kind<4;++kind){Observed state;auto watch=watchTrackSourceTopology(plan(),[&]{return state.read();});
    state.change([&](Observed& s){if(kind==0)s.value[0].identity=L"replacement";if(kind==1)s.value.erase(s.value.begin());if(kind==2)s.value[0].bounds.bottom++;if(kind==3)s.fail=true;});state.awaitChange();
    const auto error=failure(*watch,state);require(error==(kind==3?"enumeration_fixture_failed":"Selected virtual monitor changed; reconnect the session"),"Async identity/loss/geometry/query failure did not propagate unchanged");
  }
}
void destructionJoins(){
  const auto owner=std::this_thread::get_id();std::promise<void> entered,release;auto allowed=release.get_future().share();std::atomic<bool> announced=false;
  auto watch=watchTrackSourceTopology(plan(),[&]{if(std::this_thread::get_id()!=owner){if(!announced.exchange(true))entered.set_value();if(allowed.wait_for(std::chrono::seconds(2))!=std::future_status::ready)throw std::runtime_error("bounded_fixture_wait");}return monitors();});
  const auto active=entered.get_future().wait_for(std::chrono::seconds(2));
  if(active!=std::future_status::ready){release.set_value();require(false,"Watch callback never entered");}
  std::promise<void> deleting;auto deletionStarted=deleting.get_future();
  auto joined=std::async(std::launch::async,[&]{deleting.set_value();watch.reset();});
  if(deletionStarted.wait_for(std::chrono::seconds(2))!=std::future_status::ready){release.set_value();require(false,"Destructor fixture did not begin");}
  const bool waited=joined.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout;release.set_value();require(waited,"Destructor returned while callback still owned its captures");require(joined.wait_for(std::chrono::seconds(2))==std::future_status::ready,"Destructor failed to join a released callback");joined.get();
}
}
int main(){try{initialValidation();hotPath();asyncFailures();destructionJoins();std::cout<<"{\"pureChecks\":"<<checks<<",\"hotVerifications\":10000,\"synchronousQueries\":1,\"asyncFailureCases\":4,\"joinVerified\":true,\"desktopQueries\":0,\"productExecutions\":0,\"network\":0}\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
