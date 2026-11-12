#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <optional>
#include <utility>

namespace host {
// Private observation only. Neither the relay nor worker lifetime decisions
// consume these values. Keep the numeric vocabulary in the accompanying doc.
enum class WorkerRelayCause : unsigned {
  unknown=0,stopRequested=1,pipeReadFailed=2,resizeExpired=3,controlTooLarge=4,
  controlDispatchFailed=5,workerStopAck=6,trackPacketInvalid=7,
  initialMediaPending=8,videoWatchdog=9,trackSendFailed=10,
  commandWriteFailed=11,exception=12,scopeExit=13
};
enum class WorkerRelayChild : unsigned {
  unobserved=0,running=1,exited=2,waitFailed=3,exitCodeUnavailable=4,
  observationUnavailable=5
};
struct WorkerRelayFacts {
  WorkerRelayCause cause=WorkerRelayCause::unknown;
  WorkerRelayChild child=WorkerRelayChild::unobserved;
  bool captured=false,requested=false;
  std::optional<DWORD> exitCode,win32;
  void ended(WorkerRelayCause value)noexcept{if(cause==WorkerRelayCause::unknown)cause=value;}
  void readEnded(bool requestedStop,bool lastRead)noexcept{
    ended(requestedStop?WorkerRelayCause::stopRequested:!lastRead?WorkerRelayCause::pipeReadFailed:WorkerRelayCause::scopeExit);
  }
};
struct WorkerRelayProbeAccess {
  void* context=nullptr;
  DWORD (*wait)(void*,HANDLE,DWORD)=[](void*,HANDLE process,DWORD timeout)->DWORD{return WaitForSingleObject(process,timeout);};
  BOOL (*exit)(void*,HANDLE,DWORD*)=[](void*,HANDLE process,DWORD* code)->BOOL{return GetExitCodeProcess(process,code);};
  DWORD (*error)(void*)=[](void*)->DWORD{return GetLastError();};
};
// Observe this exact held process before pipe/job/feature teardown. No process
// is reopened by PID, no timed wait is added, and exit code 259 is not special
// after a signalled wait: it can be an actual exit code.
class WorkerRelayProbe {
 public:
  WorkerRelayProbe(WorkerRelayFacts& facts,HANDLE process,const std::atomic<bool>& stopped,
      const std::atomic<bool>& stopping,WorkerRelayProbeAccess access={})noexcept
      :facts_(facts),process_(process),stopped_(stopped),stopping_(stopping),access_(access),uncaught_(std::uncaught_exceptions()){}
  WorkerRelayProbe(const WorkerRelayProbe&)=delete;
  WorkerRelayProbe& operator=(const WorkerRelayProbe&)=delete;
  ~WorkerRelayProbe()noexcept{
    if(std::uncaught_exceptions()>uncaught_)facts_.ended(WorkerRelayCause::exception);
    capture();
  }
  void capture()noexcept{
    if(facts_.captured)return;
    facts_.captured=true;facts_.requested=stopped_.load()||stopping_.load();
    if(facts_.cause==WorkerRelayCause::unknown)facts_.ended(facts_.requested?WorkerRelayCause::stopRequested:WorkerRelayCause::scopeExit);
    if(!process_||process_==INVALID_HANDLE_VALUE)return;
    try{
      const auto waited=access_.wait(access_.context,process_,0);
      if(waited==WAIT_TIMEOUT){facts_.child=WorkerRelayChild::running;return;}
      if(waited!=WAIT_OBJECT_0){facts_.child=WorkerRelayChild::waitFailed;if(waited==WAIT_FAILED)facts_.win32=access_.error(access_.context);return;}
      DWORD code=0;
      if(!access_.exit(access_.context,process_,&code)){facts_.child=WorkerRelayChild::exitCodeUnavailable;facts_.win32=access_.error(access_.context);return;}
      facts_.child=WorkerRelayChild::exited;facts_.exitCode=code;
    }catch(...){facts_.child=WorkerRelayChild::observationUnavailable;facts_.exitCode.reset();facts_.win32.reset();}
  }
 private:
  WorkerRelayFacts& facts_;HANDLE process_;const std::atomic<bool>& stopped_;const std::atomic<bool>& stopping_;
  WorkerRelayProbeAccess access_;int uncaught_;
};
// The exact same emission policy is exercised with a memory sink in QA.
// No arbitrary string, PID, URL, pipe name, exception or content is accepted.
template<class Sink> void emitWorkerRelayDiagnostic(const WorkerRelayFacts& facts,Sink&& sink)noexcept{
  using Values=std::initializer_list<std::pair<const char*,int64_t>>;
  const auto cause=static_cast<unsigned>(facts.cause),stage=static_cast<unsigned>(facts.child);
  try{
    if(facts.exitCode){sink(Values{{"action",260},{"stage",stage},{"cause",cause},{"ok",facts.requested},{"running",false},{"error",*facts.exitCode}});}
    else if(facts.win32&&facts.child==WorkerRelayChild::exitCodeUnavailable){sink(Values{{"action",260},{"stage",stage},{"cause",cause},{"ok",facts.requested},{"running",false},{"win32",*facts.win32}});}
    else if(facts.win32){sink(Values{{"action",260},{"stage",stage},{"cause",cause},{"ok",facts.requested},{"win32",*facts.win32}});}
    else if(facts.child==WorkerRelayChild::running){sink(Values{{"action",260},{"stage",stage},{"cause",cause},{"ok",facts.requested},{"running",true}});}
    else{sink(Values{{"action",260},{"stage",stage},{"cause",cause},{"ok",facts.requested}});}
  }catch(...){ }
}
}
