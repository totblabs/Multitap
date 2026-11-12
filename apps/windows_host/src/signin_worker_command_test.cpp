#include "signin_worker_command.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
}
int main(){
  using host::signin::entry::parseWorkerCommand;
  try{
    const std::wstring nonce(64,L'a');
    const std::wstring pipe=L"\\\\.\\pipe\\MultitapHost.Signin.1234."+nonce;
    std::array<const wchar_t*,6> args={L"ignored-image",L"--signin-worker-v1",L"--pipe",pipe.c_str(),L"--service-pid",L"1234"};
    int cases=0;
    const auto parsed=parseWorkerCommand(int(args.size()),args.data());
    require(parsed&&parsed->servicePid==1234&&parsed->pipe==pipe,"Fixed service command refused");++cases;
    for(const auto pid:{L"",L"0",L"01234",L"-1",L"+1234",L"1234x",L"1234 ",L" 1234",L"4294967295",L"4294967296",L"99999999999"}){
      auto bad=args;bad[5]=pid;require(!parseWorkerCommand(6,bad.data()),"Malformed service PID admitted");++cases;
    }
    for(const auto& name:std::array<std::wstring,8>{
        L"\\\\remote\\pipe\\MultitapHost.Signin.1234."+nonce,
        L"\\\\.\\pipe\\MultitapHost.Signin.1235."+nonce,
        L"\\\\.\\pipe\\MultitapHost.Worker.1234."+nonce,
        pipe+L"\\other",pipe+L" ",pipe.substr(0,pipe.size()-1),
        L"\\\\.\\pipe\\MultitapHost.Signin.1234."+std::wstring(64,L'A'),
        L"\\\\.\\pipe\\MultitapHost.Signin.1234."+std::wstring(64,L'0')}){
      auto bad=args;bad[3]=name.c_str();require(!parseWorkerCommand(6,bad.data()),"Foreign or malformed pipe admitted");++cases;
    }
    for(int count:{0,1,2,3,4,5,7}){require(!parseWorkerCommand(count,args.data()),"Extra or missing arguments admitted");++cases;}
    for(size_t i=0;i<args.size();++i){auto bad=args;bad[i]=nullptr;require(!parseWorkerCommand(6,bad.data()),"Null argument admitted");++cases;}
    for(const size_t index:{size_t(1),size_t(2),size_t(4)}){auto bad=args;bad[index]=L"--other";require(!parseWorkerCommand(6,bad.data()),"Wrong argument flag admitted");++cases;}
    require(!parseWorkerCommand(6,nullptr),"Null argv admitted");++cases;
    std::cout<<"{\"kind\":\"signin-worker-command-parser\",\"cases\":"<<cases<<",\"systemWorkers\":0,\"pipeConnections\":0,\"captureCalls\":0,\"inputCalls\":0,\"runtimeQualified\":false}\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
