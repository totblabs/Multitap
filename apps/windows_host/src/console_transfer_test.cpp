#include "console_transfer.hpp"
#include <iostream>
using namespace host;
namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
ConsoleTransferObservation fixture(){
  ConsoleTransferObservation s;s.source=2;s.console=1;s.administrator=s.native=s.installed=s.started=s.sourceActive=s.sourceIdentity=s.consoleUserKnown=true;s.protocol=2;return s;
}
}
int main(int argc,char** argv){try{
  if(argc==2&&std::string(argv[1])=="--observe"){
    const auto d=virtualDisplayStatus();const auto s=observeConsoleTransfer(d,nativeDisplaySelected());
    std::cout<<"{\"kind\":\"console-transfer-read-only-observation\",\"administrator\":"<<(s.administrator?"true":"false")<<",\"rdpSession\":"<<(s.protocol==2?"true":"false")<<",\"ownSessionIdentityVerified\":"<<(s.sourceIdentity?"true":"false")<<",\"consoleIdentityKnown\":"<<(s.consoleUserKnown?"true":"false")<<",\"consoleOccupied\":"<<(s.consoleOccupied?"true":"false")<<",\"consoleSameUser\":"<<(s.consoleSameUser?"true":"false")<<",\"rebootRequired\":"<<(s.restart?"true":"false")<<",\"reason\":"<<jsonQuote(consoleTransferReason(s))<<",\"sessionTransferExecuted\":false}\n";return 0;
  }
  unsigned cases=0;
  for(int scenario=0;scenario<16;++scenario){
    auto s=fixture();switch(scenario){
      case 0:s.administrator=false;break;case 1:s.native=false;break;
      case 2:s.source=0;break;case 3:s.source=0xffffffff;break;
      case 4:s.installed=false;break;case 5:s.started=false;break;case 6:s.restart=true;break;
      case 7:s.sourceActive=false;break;case 8:s.protocol=0;break;case 9:s.sourceIdentity=false;break;
      case 10:s.console=0;break;case 11:s.console=0xffffffff;break;case 12:s.console=s.source;break;
      case 13:s.consoleUserKnown=false;break;case 14:s.consoleOccupied=true;break;
      case 15:s.console=s.source;s.protocol=0;break;
    }
    int connects=0;bool rejected=false;
    try{transferOwnConsole({[&]{return s;},[&](DWORD,DWORD){++connects;return true;},[]{}},2,1);}catch(...){rejected=true;}
    require(rejected&&connects==0,"A disallowed identity/session/device reached the transfer API");++cases;
  }
  for(int scenario=0;scenario<4;++scenario){
    int observations=0,connects=0;bool rejected=false;
    try{transferOwnConsole({[&]{auto s=fixture();if(++observations==2){
      if(scenario==0)s.console=3;if(scenario==1)s.source=4;
      if(scenario==2){s.consoleOccupied=true;s.consoleSameUser=false;}
      if(scenario==3)s.sourceIdentity=false;
    }return s;},[&](DWORD,DWORD){++connects;return true;},[]{}},2,1);}catch(...){rejected=true;}
    require(rejected&&connects==0,"Changed confirmation scope reached the transfer API");++cases;
  }
  for(int scenario=0;scenario<4;++scenario){
    int observations=0,connects=0,pauses=0;bool rejected=false;ConsoleTransferResult result;
    try{result=transferOwnConsole({[&]{auto s=fixture();++observations;
      // An existing console login may belong only to the same verified user.
      s.consoleOccupied=true;s.consoleSameUser=true;
      if(observations>2){
        if(scenario==0){s.console=s.source;s.protocol=0;}
        if(scenario==2)throw std::runtime_error("fixture-observation-failed");
      }return s;
    },[&](DWORD source,DWORD console){require(source==2&&console==1,"Transfer target escaped the confirmed session");++connects;return scenario!=3;},[&]{++pauses;}},2,1);}catch(...){rejected=true;}
    require(connects==1,"A transfer was retried after an uncertain response");
    if(scenario==3)require(rejected&&!result.requested,"Rejected transfer reported success");
    else require(!rejected&&result.requested&&result.completed==(scenario==0),"Transfer acceptance was confused with verified console completion");
    require(pauses<=10,"Transfer completion polling exceeded its bound");++cases;
  }
  for(const auto expected: {std::pair<DWORD,DWORD>{3,1},{2,3},{0,1},{2,0xffffffff}}){
    int connects=0;bool rejected=false;try{transferOwnConsole({fixture,[&](DWORD,DWORD){++connects;return true;},[]{}},expected.first,expected.second);}catch(...){rejected=true;}
    require(rejected&&!connects,"Stale or arbitrary session parameters were accepted");++cases;
  }
  const auto json=JsonParser::parse(publicConsoleTransfer(fixture()));require(json.get("eligible").boolean()&&json.get("reason").string().empty()&&json.get("sourceSessionId").number()==2,"Public transfer status lost its binding");++cases;
  std::cout<<"{\"kind\":\"own-session-console-transfer\",\"cases\":"<<cases<<",\"passed\":true,\"windowsConnectCallMocked\":true,\"sessionTransferExecuted\":false,\"passwordOrIdentityLogged\":false}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
