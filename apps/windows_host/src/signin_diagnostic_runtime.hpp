#pragma once
#include "signin_diagnostic_coordinator.hpp"
#include "signin_worker_native.hpp"
#include "support.hpp"
#include <thread>

namespace host::signin::diagnostic {
// Owned by serviceMain, outside any replaceable Bridge. The callback takes
// fresh lexical Bridge locks on each call; no such locks enter retained state.
class ServiceRuntime {
 public:
  using Handler=std::function<Result(Coordinator&,VerifiedOperator&&,const Request&)>;
  ServiceRuntime(NativeDisplayService& display,HANDLE serviceStop,Process identity,ToolPin pin,Handler handler)
    :inletStop_(CreateEventW(nullptr,TRUE,FALSE,nullptr)),coordinator_(display,serviceStop),server_(identity,std::move(pin)){
    if(!inletStop_.value||!handler)throw std::runtime_error("Private diagnostic runtime is unavailable");
    worker_=std::thread([this,handler=std::move(handler)]{
      try{
        (void)server_.run(inletStop_.value,[this,&handler](VerifiedOperator&& peer,const Request& request){
          return handler(coordinator_,std::move(peer),request);
        });
      }catch(...){coordinator_.cancelForward();}
    });
  }
  ~ServiceRuntime(){(void)close();}
  ServiceRuntime(const ServiceRuntime&)=delete;ServiceRuntime& operator=(const ServiceRuntime&)=delete;
  Coordinator& coordinator(){return coordinator_;}
  std::shared_ptr<std::atomic<bool>> busyFlag()const{return coordinator_.busyFlag();}
  // Call without Bridge locks: an in-flight inlet callback may own them.
  bool closeInlet()noexcept{
    try{
      coordinator_.cancelForward();if(inletStop_.value)SetEvent(inletStop_.value);
      if(worker_.joinable())worker_.join();return true;
    }catch(...){return false;}
  }
  Result close()noexcept{
    if(closed_)return closedResult_;
    try{
      if(!closeInlet())return {false,Code::restorePending};
      closedResult_=coordinator_.shutdown();
      // A joined inlet does not prove display cleanup. Keep subsequent close
      // calls able to advance an owned pending restoration.
      closed_=!coordinator_.busy();return closedResult_;
    }catch(...){return {false,Code::restorePending};}
  }
 private:
  WinHandle inletStop_;Coordinator coordinator_;Server server_;std::thread worker_;
  bool closed_=false;Result closedResult_;
};
}
