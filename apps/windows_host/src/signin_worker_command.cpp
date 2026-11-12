#include "signin_worker_command.hpp"
#include "signin_worker_entry.hpp"
#include "signin_owned_release.hpp"

namespace host::signin::entry {
int workerCommand(int argc,const wchar_t* const* argv) noexcept {
  try {
    const auto command=parseWorkerCommand(argc,argv);
    if(!command||command->servicePid==GetCurrentProcessId())return ERROR_INVALID_PARAMETER;
    // The service created this exact pipe before launching the suspended child.
    // A bounded wait never searches another pipe or accepts a remote endpoint.
    if(!WaitNamedPipeW(command->pipe.c_str(),5000))return ERROR_PIPE_NOT_CONNECTED;
    native::Handle pipe(CreateFileW(command->pipe.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
    if(!pipe)return ERROR_PIPE_NOT_CONNECTED;
    ULONG server=0;
    if(!GetNamedPipeServerProcessId(pipe.get(),&server)||server!=command->servicePid)return ERROR_ACCESS_DENIED;
    // This branch deliberately precedes main's ordinary Runtime/DPI setup.
    // Match physical CCD/DXGI pixels in this process only; never change the
    // user's Windows scale to discover or compensate for it.
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))return ERROR_INVALID_STATE;
    // run() owns Runtime only AFTER authenticated, current native authority.
    // Its return contains no credentials, SID, key data, or desktop pixels.
    std::atomic<bool> stopped=false;
    const auto result=run(pipe.get(),command->servicePid,stopped);
    if(result.releasePending)return int(releasePendingExitCode);
    return result.cause==Cause::stopped?0:64+int(result.cause);
  }catch(...){return ERROR_INVALID_DATA;}
}
}
