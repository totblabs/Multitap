// Private typed release authority, using the original channel's exact fixtures.
// No service, token, desktop, capture, input injection or actual pipe is invoked.
#define main inheritedChannelTests
#include "signin_worker_channel_test.cpp"
#undef main

namespace {
struct ReleaseAuthority final : native::LeaseAuthority {
  bool forward = false, release = true, raises = false;
  mutable unsigned forwardCalls = 0, releaseCalls = 0;
  bool current(const Binding&, std::span<const native::OwnedTarget>) const override {
    ++forwardCalls; return forward;
  }
  bool currentForRelease(const Binding&, std::span<const native::OwnedTarget>) const override {
    ++releaseCalls; if (raises) throw std::runtime_error("Synthetic release authority failure");
    return release;
  }
};
void releaseAuthorityCases() {
  const auto init = fixture();
  auto request = JsonParser::parse(leaseRequest(init));
  std::get<Json::Object>(request.value).at("type").value = std::string("signin.worker.release-lease-v1");
  Authority oldAuthority; // Old forward-only adapters inherit default DENY.
  expect(!JsonParser::parse(answerReleaseLease(request, init.binding, {&init.target,1}, oldAuthority)).get("current").boolean(), "Forward-only authority cannot release");
  expect(oldAuthority.calls == 0, "Release must never borrow old forward authority");
  ReleaseAuthority authority;
  const auto reply = JsonParser::parse(answerReleaseLease(request, init.binding, {&init.target,1}, authority));
  expect(reply.get("type").string() == "signin.worker.release-lease.result-v1", "Release reply phase remains distinct");
  expect(reply.get("current").boolean() && authority.releaseCalls == 1 && authority.forwardCalls == 0, "Revoked forward admission can only consult release authority");
  expect(sameBinding(parseInit(JsonParser::parse(initPacket(init))).binding, init.binding), "Existing exact binding remains intact");
  const auto forward = JsonParser::parse(answerLease(JsonParser::parse(leaseRequest(init)),init.binding,{&init.target,1},authority));
  expect(!forward.get("current").boolean() && authority.forwardCalls == 1, "Release permission cannot grant forward work");
  for (unsigned variant=0; variant<6; ++variant) {
    auto changed=request; auto expected=init.binding; auto target=init.target;
    if (variant==0) std::get<Json::Object>(changed.value).at("type").value=std::string("signin.worker.lease-v1");
    else if (variant==1) std::get<Json::Object>(changed.value).emplace("owned",Json{true});
    else if (variant==2) std::get<Json::Object>(changed.value).at("serial").value=0.0;
    else if (variant==3) ++expected.generation;
    else if (variant==4) ++target.targetId;
    else ++expected.worker.created;
    const auto previous=authority.releaseCalls;
    bool denied=false;try{(void)answerReleaseLease(changed,expected,{&target,1},authority);}catch(...){denied=true;}
    expect(denied && authority.releaseCalls == previous, "Wrong phase, peer fields, type, target or worker identity cannot reach release authority");
  }
  authority.release=false;
  expect(!JsonParser::parse(answerReleaseLease(request,init.binding,{&init.target,1},authority)).get("current").boolean(), "Release revocation remains denied");
  authority.release=true; authority.raises=true;
  expect(!JsonParser::parse(answerReleaseLease(request,init.binding,{&init.target,1},authority)).get("current").boolean(), "Release observation failure remains denied");
  bool denied=false;try{(void)answerLease(request,init.binding,{&init.target,1},authority);}catch(...){denied=true;}
  expect(denied, "Forward dispatcher never accepts release packet");
}
}
int main(){try{releaseAuthorityCases();std::cout<<"{\"kind\":\"signin-release-lease-pure-tests\",\"checks\":"<<checks<<",\"runtimeQualified\":false}\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
