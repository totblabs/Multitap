#include "console_input_ownership.h"
#include <iostream>
int main(){
  using multitap::ConsoleInputOwnership;
  ConsoleInputOwnership policy;int checks=0,failures=0;
  const auto check=[&](bool ok,const char* label){++checks;if(!ok){++failures;std::cerr<<label<<'\n';}};
  const std::string a(32,'a'),b(32,'b');
  check(!policy.CanAcquire("bad",1,false),"opaque owner required");
  check(!policy.CanAcquire(a,0,false),"positive generation required");
  check(!policy.CanAcquire(a,1,true),"media owner blocks console");
  const auto first=policy.Acquire(a,1,false);
  check(first.lease>0&&policy.Current(first),"current first lease");
  check(!policy.captured(),"acquire never installs capture");
  check(!policy.CanAcquire(b,1,false),"unrelated owner cannot preempt");
  check(!policy.CanAcquire(a,1,false),"same generation cannot reacquire");
  check(!policy.CanCapture(first,true,false,false),"background cannot capture");
  check(!policy.CanCapture(first,true,true,true),"other engine cannot be stolen");
  check(policy.CanCapture(first,true,true,false),"foreground exact owner eligible");
  check(policy.Captured(first,true)&&policy.captured(),"actual capture recorded");
  policy.ResetCapture();check(!policy.captured()&&policy.Current(first),"focus reset preserves lease but drops capture");
  check(policy.Release(first)&&!policy.valid(),"retire current lease");
  check(policy.BlocksOtherStart(),"late media starts blocked while console view remains");
  const auto second=policy.Acquire(a,2,false);
  check(second.lease>first.lease&&policy.Current(second),"successor lease monotonic");
  check(!policy.Current(first),"old queued event invalid");
  check(!policy.Release(first)&&policy.Current(second),"late release cannot clear successor");
  check(!policy.Captured(first,true)&&!policy.captured(),"late capture cannot change successor");
  check(!policy.CanCapture(first,true,true,false),"retired generation cannot capture");
  check(!policy.RetireView(b,2)&&policy.BlocksOtherStart(),"foreign view retirement cannot enable old starts");
  check(!policy.RetireView(a,1)&&policy.Current(second),"old view generation cannot retire current");
  check(policy.RetireView(a,2)&&!policy.BlocksOtherStart(),"only own completed view retirement releases mode fence");
  const auto third=policy.Acquire(b,1,false);
  check(!policy.RetireView(a,2)&&policy.Current(third),"late old view cleanup cannot clear successor owner");
  policy.Close();check(!policy.valid()&&!policy.Current(second)&&!policy.Current(third),"window close retires everything");
  check(!policy.CanAcquire(b,3,false),"closed window cannot revive");
  std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
