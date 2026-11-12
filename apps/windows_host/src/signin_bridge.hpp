#pragma once
#include "agent.hpp"
#include "signin_bridge_policy.hpp"
#include "signin_worker_native.hpp"
#include <memory>

namespace host {class NativeDisplayService;}
namespace host::signin::broker {
// Service-owned, outliving replaceable authenticated ServiceBridge objects.
// The desktop factory is populated by the real existing ordinary-user bridge;
// it is never a peer-selected callback or a default-success adapter.
class SigninBridge {
 public:
  struct DesktopLane {
    std::unique_ptr<AgentBridge> bridge;
    std::function<native::Handle()> workerQuery;
    std::function<bool()> fullOriginalRestored;
  };
  using DesktopFactory=std::function<DesktopLane()>;
  SigninBridge(NativeDisplayService&,HANDLE serviceStop);
  ~SigninBridge();
  // Permanent service shutdown: join actual cleanup before releasing its pre-admitted owner.
  void closeSchedulingAndJoin();
  SigninBridge(const SigninBridge&)=delete;SigninBridge& operator=(const SigninBridge&)=delete;
  static bool requested(const Json&);
  bool start(const Json&,AgentBridge::Send,DesktopFactory);
  bool control(const Json&);
  void stop(const std::string& session={})noexcept;
  bool busy()const noexcept;
  bool owns(const std::string&)const noexcept;
  SessionStopEvidence stoppedSession(const std::string&)const;
  std::string capabilityFields();
  std::string eligibilityMessage();
  void connectionOpened(HINTERNET);
  void connectionClosed(HINTERNET);
  void protocolNegotiated(HINTERNET,bool);
 private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
