#include "service.hpp"
#include <iostream>
using namespace host;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
StreamConsoleBinding binding(){StreamConsoleBinding value;value.session=2;value.logon={99,-1};PSID raw=nullptr;require(ConvertStringSidToSidW(L"S-1-5-21-100-200-300-1001",&raw)!=FALSE,"QA SID creation failed");const auto length=GetLengthSid(raw);value.sid.resize(length);CopySid(length,value.sid.data(),raw);LocalFree(raw);return value;}
void run(unsigned scenario){
 const DisplayOperationKey key{"00000000-1234-1234-1234-000000000001",std::string(43,'a')};
 const auto name=L"\\\\.\\pipe\\MultitapHost.QA.Console."+randomName();
 WinHandle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));require(server.value!=INVALID_HANDLE_VALUE,"QA pipe creation failed");
 WinHandle client(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));require(client.value!=INVALID_HANDLE_VALUE,"QA pipe open failed");ConnectNamedPipe(server.value,nullptr);
 std::atomic<bool> stopped=false,cancelled=false,moved=false,changed=false,foreign=false;std::atomic<int64_t> now{1000};std::atomic<int> calls{0};
 std::mutex serverWrite,clientWrite;std::exception_ptr failure;
 std::thread child([&]{try{
   bool text=false;std::vector<uint8_t> bytes;require(readPipePacket(client.value,text,bytes,stopped,std::chrono::seconds(1))&&text,"QA initial request failed");const auto request=JsonParser::parse(std::string(bytes.begin(),bytes.end()));
   StreamConsoleWorkerAccess access;
   access.write=[&](const auto& value){return pipePacket(client.value,true,{value.begin(),value.end()},stopped,clientWrite);};
   access.read=[&](DWORD timeout)->std::optional<Json>{bool text=false;std::vector<uint8_t> data;if(!readPipePacket(client.value,text,data,stopped,std::chrono::milliseconds(timeout))||!text)return {};return JsonParser::parse(std::string(data.begin(),data.end()));};
   access.binding=[&]{auto value=binding();if(changed)value.logon.LowPart++;return value;};
   access.observe=[&]{ConsoleTransferObservation s;s.source=2;s.console=moved?2:1;s.administrator=s.native=s.installed=s.started=s.sourceIdentity=s.consoleUserKnown=true;s.protocol=moved?0:2;s.sourceActive=moved||scenario==1;s.sourceDisconnected=!s.sourceActive;s.consoleOccupied=foreign;s.consoleSameUser=false;return s;};
   access.connect=[&](DWORD source,DWORD console){require(source==2&&console==1,"Private worker transfer escaped confirmed scope");++calls;if(scenario==6)return false;if(scenario==7)throw std::runtime_error("UNTRUSTED_RAW_ERROR");if(scenario>=11)throw WindowsOperationError("PRIVATE_RAW_WINDOWS_TEXT",scenario==11?ERROR_ACCESS_DENIED:UINT32_MAX);if(scenario!=8)moved=true;return true;};
   access.now=[&]{return now.load();};access.pause=[&]{now+=100;};access.cancelled=[&]{return cancelled.load();};prepareStreamConsoleWorker(request,std::move(access));
 }catch(...){failure=std::current_exception();}});
 struct Join{std::thread& thread;~Join(){if(thread.joinable())thread.join();}}join{child};
 const auto request="{\"type\":\"worker.console.prepare\","+displayOperationFields(key)+",\"sourceBinding\":"+streamConsoleBindingJson(binding())+",\"consoleSessionId\":1,\"workerDeadlineMs\":"+std::to_string(scenario==9?500:11000)+'}';
 require(pipePacket(server.value,true,{request.begin(),request.end()},stopped,serverWrite),"QA prepare send failed");
 const auto read=[&](DWORD timeout)->std::optional<Json>{bool text=false;std::vector<uint8_t> bytes;if(!readPipePacket(server.value,text,bytes,stopped,std::chrono::milliseconds(timeout))||!text)return {};return JsonParser::parse(std::string(bytes.begin(),bytes.end()));};
 const auto prepared=read(1500);require(prepared&&prepared->get("type").string()=="worker.console.prepared"&&key.matches(*prepared),"Worker prepare correlation lost");
 if(scenario==9){require(!prepared->get("ok").boolean()&&prepared->get("reason").string()=="console_timeout"&&!calls,"Expired worker command reached transfer");child.join();if(failure)std::rethrow_exception(failure);return;}
 require(prepared->get("ok").boolean()&&!prepared->get("attempted").boolean(),"Preparing a transfer already changed Windows");
 if(scenario==3)cancelled=true;if(scenario==4)changed=true;if(scenario==5)foreign=true;if(scenario==10)now=12000;
 const auto commit="{\"type\":\"worker.console.apply\","+displayOperationFields(scenario==2?DisplayOperationKey{key.id,std::string(43,'b')}:key)+'}';
 require(pipePacket(server.value,true,{commit.begin(),commit.end()},stopped,serverWrite),"QA commit send failed");
 const auto result=read(400);child.join();if(failure)std::rethrow_exception(failure);
 if(scenario==2||scenario==3||scenario==10){require(!result&&!calls,"Wrong nonce/cancel/expired commit mutated Windows or forged a result");return;}
 require(result&&result->get("type").string()=="worker.console.result"&&key.matches(*result),"Worker result correlation lost");
 if(scenario==0||scenario==1)require(result->get("ok").boolean()&&result->get("attempted").boolean()&&calls==1,"A confirmed one-shot handoff failed exact post-transfer verification");
 else if(scenario==4||scenario==5)require(!result->get("ok").boolean()&&!result->get("attempted").boolean()&&!calls,"Changed identity or foreign console reached Windows call");
 else require(!result->get("ok").boolean()&&result->get("attempted").boolean()&&calls==1,"Uncertain/failed Windows call was retried or advertised successful");
 require(result->get("systemError").number()==(scenario==11?double(ERROR_ACCESS_DENIED):scenario==12?double(UINT32_MAX):0),"Original unsigned Windows error was lost or invented");
 require(jsonObject(*result).find("UNTRUSTED_RAW_ERROR")==std::string::npos&&jsonObject(*result).find("PRIVATE_RAW_WINDOWS_TEXT")==std::string::npos,"Raw error text escaped worker IPC");
}
}
int main(){try{for(unsigned scenario=0;scenario<13;++scenario)run(scenario);std::cout<<"{\"kind\":\"stream-console-private-worker\",\"cases\":13,\"actualPrivatePipeTransactions\":13,\"productionHandshakeAndBinding\":true,\"singleWindowsCallBoundary\":true,\"originalWindowsErrorPreserved\":true,\"windowsTransfer\":\"injected\",\"actualWtsTransfers\":0,\"installedHostTouched\":false}\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
