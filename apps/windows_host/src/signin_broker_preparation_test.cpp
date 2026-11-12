// Actual preparation parser and V2 transaction with existing injected CCD/store.
// No service, workerCommand, desktop, token, driver, GPU or Host process runs.
#include "signin_broker_preparation_authority.hpp"
#include "signin_preparation_test_support.hpp"
#include <iostream>

namespace {
void production(Parent& p){
  p.admission.purpose=Purpose::brokerStream;
  p.admission.operatorProcess=p.admission.service;
  p.admission.serviceInstance="0123456789abcdef0123456789abcdef";
  p.admission.grantRevision=7;p.admission.eligibilityRevision=11;
  p.admission.brokerDeadline=p.admission.issuedAt+1000;
  p.bound.operatorProcess=p.admission.service;
}
void brokerAdmissionCases(){
  Parent p;production(p);
  expect(validAdmission(p.admission,p.world.clock),"production broker purpose valid with exact service owner");
  expect(sameBinding(p.bound,p.admission,p.bound.worker),"broker child binds service/stream/nonce/console");
  expect(brokerLaunchDeadlineFresh(p.admission,p.admission.issuedAt),"one-shot grant fresh at local issue");
  expect(!brokerLaunchDeadlineFresh(p.admission,p.admission.brokerDeadline),"one-shot deadline is exclusive");
  expect(validAdmission(p.admission,p.admission.brokerDeadline+1),"consumed preparation remains transaction-bound after initial deadline");
  for(unsigned n=0;n<11;++n){auto bad=p.admission;
    switch(n){
      case 0:bad.operatorProcess={333,444};break;
      case 1:bad.serviceInstance.clear();break;
      case 2:bad.serviceInstance=std::string(32,'0');break;
      case 3:bad.serviceInstance[0]='G';break;
      case 4:bad.grantRevision=0;break;
      case 5:bad.eligibilityRevision=0;break;
      case 6:bad.brokerDeadline=0;break;
      case 7:bad.brokerDeadline=bad.issuedAt;break;
      case 8:bad.brokerDeadline=bad.issuedAt+1001;break;
      case 9:bad.purpose=Purpose::privateDiagnostic;break;
      default:bad.purpose=static_cast<Purpose>(99);break;
    }
    expect(!validAdmission(bad,p.world.clock),"missing/mixed/replayed broker correlation rejected");
  }
  Parent diagnostic;
  expect(validAdmission(diagnostic.admission,diagnostic.world.clock),"unchanged distinct-operator diagnostic admission remains valid");
  expect(!brokerLaunchDeadlineFresh(diagnostic.admission,diagnostic.world.clock),"diagnostic cannot consume production deadline");
  for(unsigned n=0;n<4;++n){auto mixed=diagnostic.admission;
    if(n==0)mixed.serviceInstance=p.admission.serviceInstance;
    else if(n==1)mixed.grantRevision=7;else if(n==2)mixed.eligibilityRevision=11;
    else mixed.brokerDeadline=mixed.issuedAt+100;
    expect(!validAdmission(mixed,diagnostic.world.clock),"diagnostic does not inherit broker grant fields");
  }
}
void brokerPacketAndTransaction(){
  Parent p;production(p);
  const auto packet=JsonParser::parse(initPacket(p.admission,p.bound,99));
  const auto parsed=parseInitPacket(packet,p.world.clock);
  expect(parsed.admission.purpose==Purpose::brokerStream&&parsed.binding==p.bound,
      "strict production private init roundtrip");
  expect(parsed.admission.serviceInstance==p.admission.serviceInstance&&
      parsed.admission.grantRevision==7&&parsed.admission.eligibilityRevision==11&&
      parsed.admission.brokerDeadline==p.admission.brokerDeadline,"all live grant correlation survives wire");
  expect(parsed.serviceQuery==99,"delegated retained service-query handle preserved");
  for(unsigned n=0;n<7;++n){auto malformed=packet;auto& object=std::get<Json::Object>(malformed.value);
    if(n==0)object.erase("serviceInstance");
    else if(n==1)object["grantRevision"].value=true;
    else if(n==2)object["eligibilityRevision"].value=0.0;
    else if(n==3)object["brokerDeadline"].value=std::string("01");
    else if(n==4)object["purpose"].value=std::string("private-diagnostic");
    else if(n==5)object["verified"].value=true;
    else object["serviceQuery"].value=std::string("0");
    bool refused=false;try{(void)parseInitPacket(malformed,p.world.clock);}catch(...){refused=true;}
    expect(refused,"private broker packet rejects missing/coerced/purpose-swapped or extra claims");
  }
  Parent diagnostic;
  const auto oldPacket=initPacket(diagnostic.admission,diagnostic.bound,99);
  expect(oldPacket.find("serviceInstance")==std::string::npos&&oldPacket.find("brokerDeadline")==std::string::npos,
      "diagnostic wire shape carries no production correlation");
  expect(parseInitPacket(JsonParser::parse(oldPacket),diagnostic.world.clock).admission.purpose==Purpose::privateDiagnostic,
      "old diagnostic strict shape remains valid");
  const auto receipt=p.run();
  expect(receipt.outcome==Outcome::prepared&&receipt.originalRetained&&!receipt.fullOriginalLayoutObserved,
      "actual V2 transaction supports production purpose without claiming restoration");
  expect(p.saves==1&&p.saveBeforeAnyChange,"production transaction saves durable original before mutation");
}
}
int main(){
  try{brokerAdmissionCases();brokerPacketAndTransaction();std::cout<<"broker preparation checks "<<checks<<" passed\n";return 0;}
  catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
