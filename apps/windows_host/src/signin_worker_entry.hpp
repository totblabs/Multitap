#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "signin_worker_channel.hpp"
#include "signin_capture_dxgi.hpp"
#include "media.hpp"
#include <deque>

namespace host::signin::entry {
using Id = std::array<uint8_t,16>;
enum class Cause : uint32_t { running, stopped, handoff, unsupported, refused, protocol, deadline, transport, queue, capture, encoder, input };
enum class Stage : uint32_t { none, bootstrap, authority, start, capture, encode, publish, decoded, input, cleanup };
struct Exit { Cause cause=Cause::running;Stage stage=Stage::none;uint32_t nativeError=0;size_t releasePending=0; };
struct Start {
  Id transition{},firstFrame{};uint64_t scopeGeneration=0;uint32_t fps=0,bitrate=0;
};
struct FirstFrame {
  Id transition{},firstFrame{};uint64_t sequence=0,ptsUs=0;Nonce payloadSha256{};
  uint32_t width=0,height=0;bool operator==(const FirstFrame&)const=default;
};
enum class Grant { publish, decoded };
std::string startPacket(const channel::Init&,const Start&);
std::string grantPacket(const channel::Init&,const FirstFrame&,Grant);
std::string inputPacket(const channel::Init&,const Json& unchangedPublicPhase);
std::string stopPacket(const channel::Init&);
// Exact private child receipt after release() walked its own injected records.
std::string cleanupPacket(const channel::Init&,const Exit&);
bool cleanupComplete(const Json&,const channel::Init&);
FirstFrame parseFirstFrame(const Json&,const channel::Init&);
Nonce payloadDigest(std::span<const uint8_t>);

// Only WorkerLease::Defer pushes. No callbacks, RPC, injection or dispatch occur
// here. The main loop is the sole consumer after a lease query has unwound.
class Deferred {
 public:
  bool push(const Json&)noexcept;
  bool pop(Json&);
  bool pendingFence()const;
  bool poisoned()const{return poisoned_;}
 private:
  std::deque<std::pair<Json,size_t>> queue_;size_t bytes_=0;bool poisoned_=false;
};
// Shared actual orchestration seam. Production uses the authenticated Permit,
// DXGI, Encoder and standalone scoped RemoteInput adapter. Tests inject facts;
// they never construct those adapters or imply native runtime qualification.
class Pipeline {
 public:
  using Gate=std::function<bool()>;
  virtual ~Pipeline()=default;
  virtual native::Status current()=0;
  virtual native::Status open(const Start&)=0;
  virtual native::Status capture(native::BgraFrame&,uint32_t timeoutMs)=0;
  virtual bool encode(const native::BgraFrame&,uint64_t ptsUs,std::vector<Frame>&)=0;
  virtual bool input(const Json&,const Gate&)=0;
  virtual size_t release(const Gate&)=0;
  virtual void close()noexcept=0;
};
class Output {
 public:
  virtual ~Output()=default;
  virtual bool text(const std::string&)=0;
  virtual bool media(Frame,uint16_t epoch)=0;
};
class Loop {
 public:
  using Now=std::function<uint64_t()>; // trusted monotonic microseconds
  Loop(channel::Init,Deferred&,Pipeline&,Output&,Now,uint64_t begunUs);
  ~Loop();
  Exit tick();
  Exit stop();
  bool active()const;
  std::optional<FirstFrame> firstFrame()const;
 private:
  struct Impl;std::unique_ptr<Impl> impl_;
};
// Future main.cpp integration only. Linking does not call this. The caller's
// output was already prepared by the service with its sealed original baseline.
Exit run(HANDLE privatePipe,uint32_t expectedServicePid,const std::atomic<bool>& externalStop);
}
