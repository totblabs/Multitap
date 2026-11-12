#include "worker_exit_fence.hpp"
#include "windows_error.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
namespace {
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
struct Child {
  HANDLE process=nullptr,job=nullptr;
  Child(bool linger,bool isolated){
    wchar_t executable[32768]{};const auto size=GetModuleFileNameW(nullptr,executable,32768);
    require(size&&size<32768,"Own QA executable path unavailable");
    std::wstring command=L"\""+std::wstring(executable)+L"\" --child "+(linger?L"linger":L"exit");
    if(isolated){job=CreateJobObjectW(nullptr,nullptr);require(job!=nullptr,"QA job creation failed");JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;require(SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"QA job limit failed");}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
    require(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child),"Own QA child creation failed");
    process=child.hProcess;
    if(job&&!AssignProcessToJobObject(job,process)){TerminateProcess(process,1);WaitForSingleObject(process,2000);CloseHandle(child.hThread);throw std::runtime_error("Own QA child job assignment failed");}
    const auto resumed=ResumeThread(child.hThread);CloseHandle(child.hThread);require(resumed!=DWORD(-1),"Own QA child resume failed");
  }
  ~Child(){if(job)CloseHandle(job);if(process){if(WaitForSingleObject(process,0)!=WAIT_OBJECT_0){TerminateProcess(process,1);WaitForSingleObject(process,2000);}CloseHandle(process);}}
};
struct FailingConstruction {
  host::OwnedWorkerLifetime lifetime;
  FailingConstruction(HANDLE& pipe,Child& child,bool& observed):lifetime(pipe,child.process,child.job,&observed,20,30000){throw std::runtime_error("Intentional owned-worker constructor failure");}
};
}
int wmain(int argc,wchar_t** argv){
  if(argc==3&&std::wstring(argv[1])==L"--child"){
    if(std::wstring(argv[2])==L"linger"){Sleep(15000);return 0;}
    return std::wstring(argv[2])==L"exit"?0:ERROR_BAD_ARGUMENTS;
  }
  try{
    int passed=0;
    {DWORD flags=0;const BOOL success=GetHandleInformation(nullptr,&flags);const DWORD expected=GetLastError();require(!success&&expected!=ERROR_SUCCESS,"Expected real invalid-handle error was not produced");
      struct Cleanup{~Cleanup(){SetLastError(ERROR_CANCELLED);}};
      bool caught=false;try{Cleanup cleanup;host::winCheck(success,"Own QA operation");}catch(const host::WindowsOperationError& error){caught=true;require(error.systemError()==expected&&GetLastError()==ERROR_CANCELLED,"Cleanup replaced the original Windows operation error");}
      require(caught,"Windows failure did not retain its typed numeric code");++passed;}
    {SetLastError(ERROR_PRIVILEGE_NOT_HELD);host::winCheck(TRUE,"Own QA successful operation");require(GetLastError()==ERROR_PRIVILEGE_NOT_HELD,"Successful checks changed the thread's existing error");++passed;}
    {Child child(false,true);require(WaitForSingleObject(child.process,30000)==WAIT_OBJECT_0,"Graceful own child failed to exit");require(host::finishOwnedWorker(child.process,child.job,20,2000)&&!child.job,"Signalled graceful worker did not release its isolated job");++passed;}
    {Child child(true,true);require(WaitForSingleObject(child.process,0)==WAIT_TIMEOUT,"Linger QA child already exited");require(host::finishOwnedWorker(child.process,child.job,20,30000)&&!child.job&&WaitForSingleObject(child.process,0)==WAIT_OBJECT_0,"Closing the owned job was substituted for actual process exit");++passed;}
    {Child child(true,false);require(!host::finishOwnedWorker(child.process,child.job,10,10)&&WaitForSingleObject(child.process,0)==WAIT_TIMEOUT,"A live worker without a completed fence was reported quiescent");++passed;}
    for(HANDLE invalid:{HANDLE(nullptr),INVALID_HANDLE_VALUE}){HANDLE job=nullptr;require(!host::finishOwnedWorker(invalid,job,0,0),"Missing process proof became a successful worker fence");++passed;}
    {Child foreign(true,true),child(true,true);HANDLE reader=nullptr,writer=nullptr;require(CreatePipe(&reader,&writer,nullptr,0),"Own lifetime fixture pipe failed");bool observed=false,thrown=false;
      try{FailingConstruction failed(writer,child,observed);}catch(const std::runtime_error&){thrown=true;}
      require(thrown&&observed&&!writer&&!child.job&&WaitForSingleObject(child.process,0)==WAIT_OBJECT_0,"A partial constructor left its owned child/job alive or lost exit proof");
      DWORD bytes=0;const BOOL readable=PeekNamedPipe(reader,nullptr,0,nullptr,&bytes,nullptr);const DWORD pipeError=GetLastError();CloseHandle(reader);
      require(!readable&&pipeError==ERROR_BROKEN_PIPE&&WaitForSingleObject(foreign.process,0)==WAIT_TIMEOUT,"Cleanup leaked its pipe or stopped another job's child");++passed;}
    {Child child(false,true);HANDLE pipe=nullptr;bool observed=false;
      {host::OwnedWorkerLifetime lifetime(pipe,child.process,child.job,&observed,30000,5000);lifetime.connected();}
      require(observed&&!child.job&&WaitForSingleObject(child.process,0)==WAIT_OBJECT_0,"Connected worker did not retain graceful exit and held-handle verification");++passed;}
    {HANDLE process=nullptr,pipe=nullptr,job=CreateJobObjectW(nullptr,nullptr);require(job!=nullptr,"Prelaunch job fixture failed");bool observed=true;
      {host::OwnedWorkerLifetime lifetime(pipe,process,job,&observed,0,0);require(!observed,"A new launch retained a previous exit observation");}
      require(!job&&!observed,"A failed prelaunch leaked its job or invented process exit");++passed;}
    {Child child(true,true);HANDLE pipe=nullptr,duplicate=nullptr;require(DuplicateHandle(GetCurrentProcess(),child.job,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS),"Duplicate own QA job failed");bool observed=true;
      {host::OwnedWorkerLifetime lifetime(pipe,child.process,child.job,&observed,0,10);}
      require(!observed&&!child.job&&WaitForSingleObject(child.process,0)==WAIT_TIMEOUT,"Closing one job handle was mistaken for the live process exiting");CloseHandle(duplicate);
      require(WaitForSingleObject(child.process,30000)==WAIT_OBJECT_0,"Final owned QA job closure failed to stop its child");++passed;}
    // A worker that outlives the bounds (here: another job handle keeps it
    // alive) hands its owner a wait handle to that exact process, so a late
    // exit can still be observed instead of latching "not quiescent".
    {Child child(true,true);HANDLE pipe=nullptr,duplicate=nullptr,unexited=INVALID_HANDLE_VALUE;require(DuplicateHandle(GetCurrentProcess(),child.job,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS),"Duplicate own QA job failed");bool observed=true;
      {host::OwnedWorkerLifetime lifetime(pipe,child.process,child.job,&observed,0,10,&unexited);}
      require(!observed&&unexited&&unexited!=INVALID_HANDLE_VALUE&&WaitForSingleObject(unexited,0)==WAIT_TIMEOUT&&GetProcessId(unexited)==GetProcessId(child.process),"An unobserved worker exit left no way to observe it later");
      CloseHandle(duplicate);
      require(WaitForSingleObject(unexited,30000)==WAIT_OBJECT_0,"The retained handle did not observe the late worker exit");CloseHandle(unexited);++passed;}
    {Child child(false,true);HANDLE pipe=nullptr,unexited=INVALID_HANDLE_VALUE;bool observed=false;
      {host::OwnedWorkerLifetime lifetime(pipe,child.process,child.job,&observed,30000,5000,&unexited);lifetime.connected();}
      require(observed&&unexited==nullptr,"An observed worker exit retained a process handle");++passed;}
    std::cout<<"{\"cases\":"<<passed<<",\"actualOwnedChildProcesses\":9,\"unexitedWorkerLaterObserved\":true,\"constructorFailureCleanup\":true,\"privatePipeClosed\":true,\"originalWindowsErrorPreserved\":true,\"actualKillOnJobClose\":true,\"unrelatedJobPreserved\":true,\"externalProcessesChanged\":0,\"displayChanges\":0}\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
