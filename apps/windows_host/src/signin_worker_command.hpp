#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace host::signin::entry {
struct WorkerCommand { std::wstring pipe; uint32_t servicePid=0; };
// Only the service's fixed local pipe grammar is accepted. No remote named
// pipes, aliases, extra arguments, or permissively parsed process IDs.
inline std::optional<WorkerCommand> parseWorkerCommand(int argc,const wchar_t* const* argv) {
  if(argc!=6||!argv)return {};
  for(int i=0;i<argc;++i)if(!argv[i])return {};
  if(std::wstring_view(argv[1])!=L"--signin-worker-v1"||std::wstring_view(argv[2])!=L"--pipe"||
      std::wstring_view(argv[4])!=L"--service-pid")return {};
  const std::wstring_view pid(argv[5]);
  if(pid.empty()||pid.size()>10||pid.front()==L'0')return {};
  uint64_t value=0;
  for(const auto digit:pid){if(digit<L'0'||digit>L'9')return {};value=value*10+uint32_t(digit-L'0');}
  if(!value||value>=UINT32_MAX)return {};
  const auto prefix=L"\\\\.\\pipe\\MultitapHost.Signin."+std::to_wstring(value)+L".";
  const std::wstring_view pipe(argv[3]);
  if(pipe.size()!=prefix.size()+64||pipe.substr(0,prefix.size())!=prefix)return {};
  bool nonzero=false;
  for(const auto digit:pipe.substr(prefix.size())){
    if(!((digit>=L'0'&&digit<=L'9')||(digit>=L'a'&&digit<=L'f')))return {};
    nonzero|=digit!=L'0';
  }
  if(!nonzero)return {};
  return WorkerCommand{std::wstring(pipe),uint32_t(value)};
}
// No interactive UI, token mutation, or readiness advertisement. The native
// worker independently verifies the actual SCM, service, pipe and its desktop.
int workerCommand(int argc,const wchar_t* const* argv) noexcept;
}
