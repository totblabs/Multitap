#include "signin_preparation.hpp"
#include "../tests/display-v2-world.hpp"
#include <cstdio>
#include <cstdlib>

using namespace host;
using namespace host::signin;
using namespace host::signin::preparation;
namespace {
unsigned checks=0;
void expect(bool v,const char* message){++checks;if(!v){std::fprintf(stderr,"FAILED: %s\n",message);std::exit(1);}}
template<class F>void rejects(F fn,const char* message){bool caught=false;try{fn();}catch(...){caught=true;}expect(caught,message);}
struct Parent final:Authority {
  v2world::World world{true};v2world::Memory unused;
  PreparationAdmission admission;PreparationBinding bound;
  std::optional<BaselineProof> saved;
  uint32_t generation=7;uint64_t serial=0;
  unsigned saves=0,sizes=0,retains=0,cleanupReads=0;
  bool forward=true,cleanup=true,denySave=false,denyRetain=false,denyOriginal=false,revokeAfterSave=false,tamperAck=false;
  bool saveBeforeAnyChange=false;
  Parent(){
    admission.service={101,10001};admission.operatorProcess={202,20002};admission.lease={"ROOT\\DISPLAY\\0000",generation};
    admission.stream.fill(0x31);admission.operation.fill(0x42);admission.installedImageSha256.fill(0x53);
    admission.consoleSession=3;admission.width=1280;admission.height=720;admission.issuedAt=world.clock;admission.expiresAt=world.clock+60000;
    bound={admission.service,{303,30003},admission.operatorProcess,admission.operation,admission.stream,admission.consoleSession};
  }
  std::optional<LeaseProof> current(const PreparationAdmission&,const PreparationBinding&,Operation mode)const override{
    if((mode==Operation::prepare&&!forward)||(mode==Operation::cleanup&&!cleanup))return {};
    return LeaseProof{{admission.lease.instance,generation},world.targets()};
  }
  std::optional<BaselineProof> original(const PreparationAdmission&,const PreparationBinding&,Operation)const override{
    if(denyOriginal)return {};
    return saved?*saved:BaselineProof{};
  }
  std::optional<BaselineProof> saveOriginal(const PreparationAdmission&,const PreparationBinding&,const std::string& bytes,const std::string& expected)override{
    ++saves;saveBeforeAnyChange=world.applies==0&&world.resizes==0;
    if(denySave||!expected.empty()||saved)return {};
    saved=BaselineProof{bytes,nativeBaselineDigest(bytes)};
    if(revokeAfterSave)forward=false;
    if(tamperAck)return BaselineProof{bytes,saved->sha256.substr(1)+"0"};
    return saved;
  }
  std::optional<LeaseProof> size(const PreparationAdmission&,const PreparationBinding&,uint32_t w,uint32_t h,Operation)override{
    ++sizes;
    if(!saved)throw std::runtime_error("No durable parent baseline");
    if(!world.resize("0",w,h))return {};++generation;
    return LeaseProof{{admission.lease.instance,generation},world.targets()};
  }
  bool retain(const PreparationAdmission&,const PreparationBinding&,const Receipt&)override{++retains;return !denyRetain;}
  Receipt run(){
    auto local=v2world::worldAccess(world,unused);local.identifyConsole={};
    return prepareDisplay(admission,bound,std::move(local),[&](const Json& packet){
      if(packet.get("mode").string()=="cleanup")++cleanupReads;
      return JsonParser::parse(answer(packet,admission,bound,*this,serial,world.clock));
    },[&]{return world.clock;});
  }
};
std::string bindingBytes(const Parent& p){return jsonObject(JsonParser::parse(initPacket(p.admission,p.bound,99)).get("binding"));}
Json request(const Parent& p,const std::string& op,const std::string& payload="{}",uint64_t serial=1,const char* mode="prepare"){
  return JsonParser::parse("{\"type\":\"signin.prepare.request-v1\",\"protocol\":\"signin-private-preparation-v1\",\"binding\":"+bindingBytes(p)+
    ",\"serial\":"+jsonQuote(std::to_string(serial))+",\"operation\":"+jsonQuote(op)+",\"mode\":"+jsonQuote(mode)+",\"payload\":"+payload+'}');
}
void admissionAndParser(){
  Parent p;expect(validAdmission(p.admission,p.world.clock),"distinct diagnostic admission valid without capture Context flags");
  expect(sameBinding(p.bound,p.admission,p.bound.worker),"exact operator/service/child/nonce/reservation binding");
  for(unsigned i=0;i<14;++i){
    auto a=p.admission;
    switch(i){
      case 0:a.service.created=0;break;case 1:a.operatorProcess=a.service;break;
      case 2:a.stream={};break;case 3:a.operation={};break;case 4:a.installedImageSha256={};break;
      case 5:a.consoleSession=0;break;case 6:a.consoleSession=UINT32_MAX;break;
      case 7:a.lease.generation=0;break;case 8:a.width=0;break;case 9:a.height=9000;break;
      case 10:a.issuedAt=p.world.clock+1;break;case 11:a.expiresAt=a.issuedAt+90001;break;
      case 12:a.expiresAt=a.issuedAt;break;case 13:a.expectedOriginalSha256="not-a-sha";break;
    }
    expect(!validAdmission(a,p.world.clock),"malformed/expired/unbound admission rejected");
  }
  for(unsigned i=0;i<6;++i){auto b=p.bound;
    if(i==0)++b.worker.created;else if(i==1)++b.service.pid;else if(i==2)++b.operatorProcess.created;
    else if(i==3)b.operation[0]^=1;else if(i==4)b.stream[0]^=1;else ++b.consoleSession;
    expect(!sameBinding(b,p.admission,p.bound.worker),"every exact lifetime/nonce/stream/session binding enforced");
  }
  const std::wstring pipe=L"\\\\.\\pipe\\MultitapHost.SigninPrepare.101."+std::wstring(64,L'a');
  const wchar_t* argv[]{L"Host",L"--signin-preparer-v1",L"--pipe",pipe.c_str(),L"--service-pid",L"101"};
  expect(parseCommand(6,argv).has_value(),"fixed preparation-only CLI accepted");
  expect(!parseCommand(5,argv),"truncated CLI refused");
  const wchar_t* bad[]{L"Host",L"--signin-worker-v1",L"--pipe",pipe.c_str(),L"--service-pid",L"101"};
  expect(!parseCommand(6,bad),"normal capture command cannot become a preparer");
  bad[1]=L"--signin-preparer-v1";bad[5]=L"0101";expect(!parseCommand(6,bad),"PID grammar has no aliases");
  bad[5]=L"102";expect(!parseCommand(6,bad),"pipe PID must match exact parent argument");
  bad[5]=L"101";const std::wstring remote=L"\\\\remote\\pipe\\MultitapHost.SigninPrepare.101."+std::wstring(64,L'a');bad[3]=remote.c_str();
  expect(!parseCommand(6,bad),"remote pipe cannot be used");
}
void actualV2SuccessfulPreparation(){
  Parent p;const auto before=p.world.query();const auto receipt=p.run();
  expect(receipt.outcome==Outcome::prepared,"actual V2 transaction reaches prepared through parent proxy");
  expect(receipt.originalRetained&&!receipt.originalSha256.empty()&&!receipt.fullOriginalLayoutObserved,"prepared is not restored/runtime-qualified");
  expect(p.saves==1&&p.saveBeforeAnyChange,"parent durable original ACK precedes every SDK/CCD mutation");
  expect(p.saved&&p.saved->sha256==receipt.originalSha256,"parent retains exact original bytes and SHA");
  expect(parseDisplayV2State(p.saved->bytes).ccd.paths.size()==before.paths.size(),"original includes all pre-stream foreign heads");
  expect(p.world.applies>0&&p.world.resizes==1&&p.retains==1,"real V2 planner/configurer and one owned size RPC exercised");
  expect(!p.world.console().active&&!p.world.extra().active&&p.world.owned().active,"stream topology hides every foreign head");
  expect(receipt.target.targetId==p.world.ownedTarget&&receipt.target.adapterLow==v2world::ownedAdapter.LowPart&&
    receipt.target.sourceId==p.world.owned().source&&receipt.target.width==1280&&receipt.target.height==720,"actual final CCD tuple bound to fresh service-owned SDK target");
  expect(receipt.target.left==0&&receipt.target.top==0&&receipt.target.mask==1,"exact one primary owned output");
  const auto round=parseReceipt(JsonParser::parse(receiptPacket(receipt)));
  expect(round.binding==p.bound&&round.originalSha256==receipt.originalSha256&&round.outcome==Outcome::prepared,"typed receipt codec retains exact provenance");
  expect(p.unused.saves==0&&p.unused.removes==0&&p.unused.discards==0,"local store cannot replace parent protected-store authority");
}
void saveFenceAndRevocation(){
  for(unsigned failure=0;failure<4;++failure){
    Parent p;
    if(failure==0)p.denySave=true;else if(failure==1)p.revokeAfterSave=true;else if(failure==2)p.tamperAck=true;else p.denyOriginal=true;
    const auto receipt=p.run();
    expect(receipt.outcome!=Outcome::prepared,"denied/lost/tampered original ACK cannot prepare");
    expect(p.world.applies==0&&p.world.resizes==0,"no SDK or CCD change before valid durable save ACK");
    expect(p.unused.removes==0&&p.unused.discards==0,"failure never deletes original through a local store");
    if(failure==1||failure==2)expect(p.saved.has_value(),"durably saved original kept even when ACK/forward authority fails");
  }
  Parent noAuthority;noAuthority.forward=false;const auto denied=noAuthority.run();
  expect(denied.outcome!=Outcome::prepared&&noAuthority.saves==0&&noAuthority.world.applies==0,"no successful callback fallback for absent authority");
  Parent two;expect(two.world.setCount(2).has_value(),"two-target negative fixture constructed");
  expect(two.run().outcome!=Outcome::prepared&&two.world.applies==0&&two.saves==0,"second owned target refuses before original save or forward changes");
}
void retainedFailureRestoresButNeverDeletes(){
  Parent p;p.denyRetain=true;
  const auto receipt=p.run();
  expect(receipt.outcome==Outcome::restorePending,"retention refusal requires parent cleanup even after local restore");
  expect(receipt.originalRetained&&p.saved.has_value(),"child cannot remove durable original after any restore/fallback");
  expect(receipt.fullOriginalLayoutObserved&&p.cleanupReads>0,"actual V2 restore and fresh full original-layout observation recorded");
  v2world::requireOriginal(p.world,true,"failed preparation must restore complete original foreign topology");++checks;
  Parent corrupt;corrupt.denyRetain=true;corrupt.cleanup=false;
  const auto pending=corrupt.run();expect(pending.originalRetained&&!pending.fullOriginalLayoutObserved,"failed cleanup remains explicit and original preserved");
}
void protocolBounds(){
  Parent p;uint64_t sequence=0;
  auto response=JsonParser::parse(answer(request(p,"original.read"),p.admission,p.bound,p,sequence,p.world.clock));
  expect(response.get("ok").boolean()&&!response.get("payload").get("present").boolean(),"verified absent is distinct successful response");
  rejects([&]{(void)answer(request(p,"original.read"),p.admission,p.bound,p,sequence,p.world.clock);},"replayed serial cannot repeat a command");
  for(const auto* op:{"original.remove","original.discard","input","publish","encoder","count"}){
    uint64_t serial=0;const auto denied=JsonParser::parse(answer(request(p,op),p.admission,p.bound,p,serial,p.world.clock));
    expect(!denied.get("ok").boolean(),"unsupported cleanup/input/media/count operation denied");
  }
  {uint64_t serial=0;const auto denied=JsonParser::parse(answer(request(p,"size","{\"width\":1280,\"height\":720}"),p.admission,p.bound,p,serial,p.world.clock));
    expect(!denied.get("ok").boolean()&&p.sizes==0,"size denied while original is absent");}
  {auto altered=request(p,"lease.read");std::get<Json::Object>(altered.value)["path"]=Json{std::string("C:\\arbitrary")};uint64_t serial=0;
    rejects([&]{(void)answer(altered,p.admission,p.bound,p,serial,p.world.clock);},"arbitrary path field not accepted");}
  {auto wrong=p.bound;wrong.operation[0]^=1;uint64_t serial=0;
    rejects([&]{(void)answer(request(p,"lease.read"),p.admission,wrong,p,serial,p.world.clock);},"wrong operation binding rejected before live callback");}
  {uint64_t serial=0;const auto denied=JsonParser::parse(answer(request(p,"lease.read"),p.admission,p.bound,p,serial,p.admission.expiresAt));
    expect(!denied.get("ok").boolean(),"expiry cannot be extended by an otherwise valid packet");}
  {uint64_t serial=0;const auto denied=JsonParser::parse(answer(request(p,"lease.read","{}",1,"cleanup"),p.admission,p.bound,p,serial,p.admission.issuedAt+90001));
    expect(!denied.get("ok").boolean(),"cleanup has a finite issued-time bound");}
  expect(p.saves==0&&p.sizes==0&&p.retains==0,"protocol denials never execute mutation callbacks");
}
}
