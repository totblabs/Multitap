#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <utility>
namespace host {
// Only handles created for this owned worker may be passed here. Closing its
// KILL_ON_JOB_CLOSE job is not proof of exit; the process must be signalled.
// `ownExit` (optional): the worker ended within the graceful wait, by itself,
// before the job was closed.
inline bool finishOwnedWorker(HANDLE process,HANDLE& job,DWORD gracefulMs=8000,DWORD terminationMs=5000,bool* ownExit=nullptr)noexcept{
  const bool valid=process&&process!=INVALID_HANDLE_VALUE;
  const DWORD first=valid?WaitForSingleObject(process,gracefulMs):WAIT_FAILED;
  if(ownExit)*ownExit=first==WAIT_OBJECT_0;
  if(job&&job!=INVALID_HANDLE_VALUE){if(CloseHandle(job))job=nullptr;}
  if(!valid)return false;
  return first==WAIT_OBJECT_0||WaitForSingleObject(process,terminationMs)==WAIT_OBJECT_0;
}
// A worker that ended, read from its held handle (process_exit_record.hpp uses
// it for hostAction 275): its exit code or NTSTATUS and how long it ran.
struct OwnedWorkerExit { bool observed=false,ownExit=false; uint32_t code=0; uint64_t runMs=0; };
inline OwnedWorkerExit readOwnedWorkerExit(HANDLE process,bool ownExit)noexcept{
  OwnedWorkerExit out;DWORD code=0;
  if(!process||process==INVALID_HANDLE_VALUE||WaitForSingleObject(process,0)!=WAIT_OBJECT_0||!GetExitCodeProcess(process,&code))return out;
  out.observed=true;out.ownExit=ownExit;out.code=code;
  FILETIME created{},exited{},kernel{},user{};
  if(GetProcessTimes(process,&created,&exited,&kernel,&user)){
    const auto value=[](const FILETIME& time){return (uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;};
    if(value(exited)>=value(created))out.runMs=(value(exited)-value(created))/10000;
  }
  return out;
}
// A worker whose exit was not observed inside the bounds: a wait-only handle to
// that exact process lets its owner observe the exit later instead of keeping
// the "not quiescent" verdict for the rest of the service's lifetime.
inline HANDLE retainUnexitedWorker(HANDLE process)noexcept{
  if(!process||process==INVALID_HANDLE_VALUE)return nullptr;
  HANDLE retained=nullptr;
  return DuplicateHandle(GetCurrentProcess(),process,GetCurrentProcess(),&retained,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,0)?retained:nullptr;
}
// Construct before launching the child. A containing constructor can throw
// after CreateProcess; its own destructor will then never run. This member's
// destructor still closes the private pipe/job and observes the held process.
// `unexited` (optional, caller closes) receives a wait handle to the process
// when its exit was not observed; it stays null otherwise. `exited` (optional)
// hears how an observed exit ended, while the process handle is still held.
class OwnedWorkerLifetime {
 public:
  OwnedWorkerLifetime(HANDLE& pipe,HANDLE& process,HANDLE& job,bool* observed=nullptr,DWORD gracefulMs=8000,DWORD terminationMs=5000,HANDLE* unexited=nullptr,std::function<void(const OwnedWorkerExit&)> exited={})noexcept
      :pipe_(pipe),process_(process),job_(job),observed_(observed),gracefulMs_(gracefulMs),terminationMs_(terminationMs),unexited_(unexited),exited_(std::move(exited)){if(observed_)*observed_=false;if(unexited_)*unexited_=nullptr;}
  OwnedWorkerLifetime(const OwnedWorkerLifetime&)=delete;
  OwnedWorkerLifetime& operator=(const OwnedWorkerLifetime&)=delete;
  void connected()noexcept{connected_=true;}
  ~OwnedWorkerLifetime(){
    const auto pipe=std::exchange(pipe_,nullptr);
    if(pipe&&pipe!=INVALID_HANDLE_VALUE){CancelIoEx(pipe,nullptr);CloseHandle(pipe);}
    bool ownExit=false;
    const bool exited=finishOwnedWorker(process_,job_,connected_?gracefulMs_:0,terminationMs_,&ownExit);
    if(observed_)*observed_=exited;
    if(unexited_&&!exited)*unexited_=retainUnexitedWorker(process_);
    if(exited&&exited_)try{exited_(readOwnedWorkerExit(process_,ownExit));}catch(...){ }
  }
 private:
  HANDLE &pipe_,&process_,&job_;bool* observed_;DWORD gracefulMs_,terminationMs_;HANDLE* unexited_;std::function<void(const OwnedWorkerExit&)> exited_;bool connected_=false;
};
}
