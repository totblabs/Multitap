#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace host {
struct DirectInputItem {uint32_t sequence=0;std::string body;uint64_t receivedUs=0;};
// WSS is the reliable ordered copy. UDP can arrive first, but never skips a
// missing input. WSS can advance past a message the broker deliberately denied.
class DirectInputOrder {
 public:
  template<class Emit,class Current> void accept(bool wss,DirectInputItem item,Emit&& emit,Current&& current){
    std::lock_guard lock(mutex_);if(!current()||!item.sequence||item.sequence<=last_)return;
    if(wss){
      if(const auto found=pending_.find(item.sequence);found!=pending_.end())item.receivedUs=std::min(item.receivedUs,found->second.receivedUs);
      while(!pending_.empty()&&pending_.begin()->first<=item.sequence)pending_.erase(pending_.begin());
      paused_=false;last_=item.sequence;emit(item);
    }else{
      if(paused_||uint64_t(item.sequence)>uint64_t(last_)+64||pending_.size()>=64)return;
      pending_.try_emplace(item.sequence,std::move(item));
    }
    while(last_!=UINT32_MAX){auto found=pending_.find(last_+1);if(found==pending_.end())break;auto next=std::move(found->second);pending_.erase(found);last_=next.sequence;emit(next);}
  }
  void pause(){std::lock_guard lock(mutex_);paused_=true;pending_.clear();}
 private:
  std::mutex mutex_;uint32_t last_=0;bool paused_=true;std::map<uint32_t,DirectInputItem> pending_;
};

// Pipe writes may wait for a busy worker. They must never block the UDP media
// socket. One bounded dispatcher owns injection, with WSS backpressure intact.
class DirectInputDispatcher {
 public:
  using Emit=std::function<void(const DirectInputItem&)>;
  explicit DirectInputDispatcher(Emit emit):emit_(std::move(emit)),thread_([this]{run();}){}
  ~DirectInputDispatcher(){stop();}
  void submit(bool wss,DirectInputItem item){
    std::unique_lock lock(mutex_);
    if(!wss){if(item.receivedUs>=windowAt_&&item.receivedUs-windowAt_>=1000000){windowAt_=item.receivedUs;windowCount_=0;}if(++windowCount_>2000||queue_.size()>=128)return;}
    else space_.wait(lock,[&]{return stopped_||queue_.size()<128;});
    if(stopped_)return;queue_.push_back({wss,std::move(item),generation_.load()});changed_.notify_one();
  }
  void pause(){
    {std::lock_guard lock(mutex_);++generation_;queue_.clear();space_.notify_all();}
    order_.pause();
  }
  void stop(){
    {std::lock_guard lock(mutex_);stopped_=true;++generation_;queue_.clear();changed_.notify_all();space_.notify_all();}
    if(thread_.joinable())thread_.join();
  }
 private:
  struct Job{bool wss;DirectInputItem item;uint64_t generation;};
  void run(){
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    for(;;){Job job;
    {std::unique_lock lock(mutex_);changed_.wait(lock,[&]{return stopped_||!queue_.empty();});if(stopped_)return;job=std::move(queue_.front());queue_.pop_front();space_.notify_all();}
    try{order_.accept(job.wss,std::move(job.item),emit_,[&]{return generation_.load()==job.generation;});}catch(...){order_.pause();}
  }}
  Emit emit_;std::mutex mutex_;std::condition_variable changed_,space_;std::deque<Job> queue_;std::atomic<uint64_t> generation_{0};
  bool stopped_=false;uint64_t windowAt_=0;uint32_t windowCount_=0;DirectInputOrder order_;std::thread thread_;
};
}
