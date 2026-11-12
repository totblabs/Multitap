#include "signin_eligibility_history.hpp"
#include <iostream>
using namespace host::signin;
using namespace host::signin::broker;
namespace {
unsigned checks=0;void expect(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
EligibilityScope scope(uint64_t now){return {{{100,101},3,10,now,WindowsState::locked,"S-1-5-21-1001",true,true,true},"native-lease-instance/generation/config","protected-config-digest",7};}
void refreshKeepsOnlyPublishedCurrentScope(){
  EligibilityHistory h;expect(h.published(9,scope(1000),1000),"native published revision");expect(h.published(10,scope(1500),1500),"same realm refresh");expect(h.accepts(9,scope(1800),1800),"in-flight actually published revision survives same-scope refresh");expect(!h.accepts(11,scope(1800),1800),"future scalar revision is not an offer");expect(!h.published(10,scope(2000),2000),"replayed publication cannot renew lifetime");expect(!h.accepts(9,scope(6000),6000),"five seconds is an immutable per-publication deadline");expect(h.accepts(10,scope(6000),6000),"newer still-unexpired revision remains usable");
}
void scopeChangesNeverAdoptOldHistory(){
  for(unsigned field=0;field<9;field++){
    EligibilityHistory h;expect(h.published(9,scope(1000),1000),"record exact old scope");auto next=scope(1200);
    switch(field){case 0:next.native.sid="S-1-5-21-9999";break;case 1:++next.native.console;break;case 2:++next.native.generation;break;case 3:++next.native.service.created;break;case 4:next.native.windows=WindowsState::noUser;next.native.sid.clear();break;case 5:next.lease="other-lease";break;case 6:next.configuration="other-config";break;case 7:++next.connectionGeneration;break;default:next.native.ownedSingleOutput=false;break;}
    expect(!h.accepts(9,next,1200),"SID/console/native/service/state/lease/config/WSS change revokes old history");expect(!h.accepts(9,scope(1300),1300),"return to old scope cannot resurrect retired history");
  }
  EligibilityHistory h;expect(h.published(9,scope(1000),1000),"eligible old socket");h.clear();expect(!h.accepts(9,scope(1200),1200),"withdraw/disconnect clears history");auto next=scope(1300);++next.connectionGeneration;expect(h.published(10,next,1300)&&h.accepts(10,next,1300),"new actual connection can publish a new revision");
}
void boundedAndNoClockRevival(){
  EligibilityHistory h;for(uint32_t i=1;i<=40;i++)expect(h.published(i,scope(1000+i),1000+i),"bounded native publisher records monotonic revision");expect(!h.accepts(1,scope(1041),1041),"history is bounded even before expiry");expect(h.accepts(40,scope(1041),1041),"latest exact revision retained");expect(!h.accepts(40,scope(1000),1000),"backwards clock invalidates");expect(!h.published(41,scope(2000),2000),"clock rollback cannot silently revive authority");
}
void staleDesktopCallbackDoesNotMatchSuccessor(){
  Admission a;a.workerGeneration=7;a.transition[0]=1;DesktopPacketSource old{7,a.transition};expect(old.current(a),"exact current worker callback");expect(!DesktopPacketSource{}.current(a),"untagged callback has no desktop source authority");++a.workerGeneration;a.transition[0]=2;expect(!old.current(a),"queued predecessor media/info cannot satisfy successor");DesktopPacketSource next{a.workerGeneration,a.transition};expect(next.current(a),"fresh source key matches");next.transition[1]=1;expect(!next.current(a),"same generation with different transition is stale");
}
}
int main(){try{refreshKeepsOnlyPublishedCurrentScope();scopeChangesNeverAdoptOldHistory();boundedAndNoClockRevival();staleDesktopCallbackDoesNotMatchSuccessor();std::cout<<"{\"kind\":\"signin-published-scope-history-pure-tests\",\"checks\":"<<checks<<",\"nativeAdaptersInvoked\":false,\"runtimeQualified\":false}\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
