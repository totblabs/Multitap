#include <stream_size_query_owner.h>
#include <cstdio>
int main(){
  using namespace multitap;int checks=0,failures=0;
  const auto check=[&](bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}};
  StreamSizeActualSlot actual;StreamSizeQueryObserver oldWindow,newWindow;
  const auto first=actual.Begin();check(first.has_value(),"one actual metadata query admitted");
  if(!first)return 2;
  const auto old=oldWindow.Begin(*first);
  check(oldWindow.Current(old),"native-issued query stamp belongs to observer");
  check(actual.Busy(),"submitted callback owns actual slot before result");
  check(!actual.Begin().has_value(),"queue0 refuses a second actual query");
  oldWindow.Retire();
  check(!oldWindow.Current(old),"observer timeout rejects late success");
  check(actual.Busy(),"timeout does not fabricate native completion");
  check(!actual.Begin().has_value(),"retry after UI timeout cannot grow native workers");
  check(!actual.ActualReturned({first->serial+1})&&actual.Busy(),"foreign completion cannot release actual owner");
  oldWindow.Close();
  check(actual.Busy(),"window close does not wait for or free held vendor call");
  check(!oldWindow.Begin(*first).serial,"closed endpoint cannot be reattached");
  check(actual.ActualReturned(*first)&&!actual.Busy(),"actual worker return releases exactly its slot");
  check(!actual.ActualReturned(*first),"duplicate actual completion is ignored");
  const auto second=actual.Begin();check(second&&second->serial>first->serial,"successor uses a fresh process-wide serial");
  if(!second)return 2;
  const auto current=newWindow.Begin(*second);
  check(newWindow.Current(current),"new window observer admitted after old actual return");
  check(!newWindow.Current(old),"old HWND message cannot select a successor result");
  check(!actual.ActualReturned(*first)&&actual.Busy(),"late old completion does not clear successor busy");
  check(!oldWindow.Current(current),"retired window cannot receive new success");
  const auto beforeReplacement=current;newWindow.Retire();const auto resumed=newWindow.Begin(*second);
  check(!newWindow.Current(beforeReplacement)&&newWindow.Current(resumed),"observer epoch fences same-ticket callback after replacement");
  check(actual.Busy(),"observer replacement still leaves actual query held");
  int replies=0;if(newWindow.Current(beforeReplacement))++replies;if(newWindow.Current(resumed))++replies;newWindow.Retire();
  if(newWindow.Current(resumed))++replies;
  check(replies==1,"only current observer can produce one terminal reply");
  check(actual.ActualReturned(*second)&&!actual.Busy(),"closed or retired observers do not strand actual return");
  const auto third=actual.Begin();check(third.has_value(),"later query recovers after held old worker really completed");
  if(third)check(actual.ActualReturned(*third),"test cleanup releases only its actual query");
  std::printf("stream_size_query_owner checks=%d failures=%d windowsApi=0 nativeThreads=0\n",checks,failures);return failures?1:0;
}
