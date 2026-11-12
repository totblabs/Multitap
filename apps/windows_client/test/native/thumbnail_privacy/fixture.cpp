#include <thumbnail_generation.h>
#include <cstdio>
int main(){
  using G=multitap::ThumbnailGeneration;int checks=0,failures=0;
  const auto check=[&](bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}};
  auto normal=G::Own();bool committed=false;
  check(G::Publish(normal,[&]{committed=true;return true;},[] {})&&committed,"ordinary user disconnect may retain current last frame");
  const auto pending=normal;auto revoked=G::Change(normal,false);
  check(G::Current(revoked,false),"privacy revoke linearizes one atomic generation");
  check(!G::Current(pending),"queued old writer is stale before encode/rename");
  auto racing=G::Own();bool file=false,discarded=false;
  const bool published=G::Publish(racing,[&]{file=true;(void)G::Change(racing,false);return true;},[&]{file=false;discarded=true;});
  check(!published&&!file&&discarded,"actual production publish seam discards exact result when revoke races rename");
  bool entered=false;check(!G::Publish(pending,[&]{entered=true;return true;},[] {})&&!entered,"revoked generation never enters rename");
  check(!G::Change(pending,true).state,"late old callback cannot resume");
  check(!G::Change(pending,false).state,"late old callback cannot replace revoke");
  auto fresh=G::Change(revoked,true);check(G::Current(fresh),"fresh ordinary stream may resume");
  check(!G::Current(revoked,false),"old queued purge must skip fresh file");
  check(!G::Change(revoked,false).state,"late old purge/revoke cannot affect fresh generation");
  G::Retire(fresh.state);check(!G::Current(fresh),"replacement native window retires old owner");
  check(!G::Change(fresh,true).state,"retired window cannot revive after replacement");
  auto replacement=G::Own();check(G::Current(replacement),"replacement owner works normally");
  check(replacement.stamp>fresh.stamp,"same-process recreated target never repeats notice generation");
  auto other=G::Own();check(G::Current(other),"different desktop stays usable");
  check(!G::Current(G::Ticket{}),"missing identity cannot publish");
  auto signin=G::Own();const auto queuedSignin=signin;
  check(G::Lock(signin)&&!G::Current(queuedSignin),"sign-in lifetime lock blocks already queued publication immediately");
  signin=G::Change(signin,false);signin.state->durablyRevoked=signin.stamp;
  const auto acknowledged=signin;
  for(int restart=0;restart<4;++restart)signin=G::Change(signin,true);
  check(!G::Current(signin)&&G::Current(signin,false),"decoder restarts and role handoff cannot unlock sign-in window");
  check(signin.stamp==acknowledged.stamp&&G::DurablyRevoked(signin),"restart preserves exact durable revoke acknowledgement");
  entered=false;check(!G::Publish(signin,[&]{entered=true;return true;},[] {})&&!entered,"locked window never enters image commit");
  auto signRace=G::Own();file=false;discarded=false;
  const bool signPublished=G::Publish(signRace,[&]{file=true;(void)G::Lock(signRace);return true;},[&]{file=false;discarded=true;});
  check(!signPublished&&!file&&discarded,"lifetime lock racing rename discards exact owned result");
  auto afterSignin=G::Own();check(G::Current(afterSignin)&&G::Current(other),"new window and unrelated desktop retain ordinary publication");
  std::printf("thumbnail_privacy checks=%d failures=%d windowsApi=0 durablePrivacy=0\n",checks,failures);
  return failures?1:0;
}
