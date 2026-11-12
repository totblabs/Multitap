#pragma once
// udp-media-v1, the Host's path (docs/remote-session-protocol.md, "UDP
// media"). udp_media.hpp decides what goes on the wire; this decides whether media
// goes there at all. No socket, thread or clock of its own: udp_probe_leg.hpp runs it.
// Any doubt leaves media on WebSocket, and nothing here can end a session. Custom
// crypto framing: it needs independent review before it carries real media.
//
// Three threads. The broker channel's receive thread exchanges keys, the worker pipe
// reader routes media, and the leg's thread owns everything below "Leg thread". They
// share only what `mutex_` guards, held for a few assignments, so the pipe reader
// never waits for pacing or sealing.
#include "udp_probe.hpp"
#include "../../../native/windows_video/udp_media_crypto.hpp"
#include "../../../native/windows_video/udp_direct.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>

namespace host {
namespace udpcore=multitap::udp;
// hostAction 271, each path change: `stage` the path now, `error` why (fixed codes).
inline constexpr uint32_t udpPathAction=271,udpPathOnUdp=1,udpPathOnWss=2,udpPathHeld=3;  // 3: stayed on UDP through a dark network
enum UdpPathReason:uint32_t{udpPathProven=1,udpPathNewStream=2,udpPathSilent=3,udpPathTooLarge=4,udpPathStalled=5,udpPathExhausted=6,udpPathSealFailed=7,udpPathSocketFailed=8};
// What the path may ask of its session. They only store (AgentBridge::keyFrame) or
// take a short lock, and none is called once `detach` returns: the leg's thread can
// outlive the Agent.
struct UdpMediaHooks {
  std::function<void()> keyFrame;
  // FEEDBACK while on UDP: round trip (0 none), datagrams received and lost since the
  // last one. Returns the pacing target.
  std::function<int(uint64_t,uint32_t,uint32_t)> feedback;
  std::function<void()> backlog;
  std::function<int(bool)> path;  // media is now on UDP or not; the pacing target
  // A LOSS: every frame after this PTS is lost to the client's decoder. The next frame
  // must reference only that one or older, or be an IDR; either way it is marked.
  std::function<void(uint64_t)> recover;
  std::function<void(const std::string&,uint16_t)> local;
  std::function<void(const std::string&,uint16_t)> reflexive;
  std::function<void(uint32_t,uint16_t,const std::string&)> input;
};
class UdpMediaPath {
 public:
  using Record=std::function<void(uint32_t,uint32_t,uint32_t)>;  // stage, reason, silence cause (0 for other reasons)
  UdpMediaPath(const UdpLegId& id,UdpMediaHooks hooks,Record record,bool direct=false):id_(id),hooks_(std::move(hooks)),record_(std::move(record)),directEnabled_(direct){}
  void peer(std::vector<udpcore::DirectEndpoint> candidates){if(directEnabled_){std::lock_guard lock(mutex_);candidates_=std::move(candidates);}}
  void local(const std::string& address,uint16_t port){if(directEnabled_){std::lock_guard lock(hookMutex_);if(hooks_.local)hooks_.local(address,port);}}
  void reflexive(const std::string& address,uint16_t port){if(directEnabled_){std::lock_guard lock(hookMutex_);if(hooks_.reflexive)hooks_.reflexive(address,port);}}
  template<class Send> bool directReceived(std::span<const uint8_t> bytes,udpcore::DirectEndpoint from,uint64_t now,Send&& send){return directEnabled_&&direct_.received(bytes.data(),bytes.size(),from,now,std::forward<Send>(send));}
  bool directAccepts(udpcore::DirectEndpoint from,uint64_t now)const{return directEnabled_&&direct_.accepts(from,now);}
  template<class Send> void directTick(uint64_t now,Send&& send){if(directEnabled_)direct_.tick(now,std::forward<Send>(send));}
  std::optional<udpcore::DirectEndpoint> directTarget(uint64_t now)const{
    // Direct media still depends on current broker control/authorization. A
    // dark control path returns to the revocable relay, even if P2P is alive.
    const auto heard=wssHeard_.load();return directEnabled_&&heard&&now>=heard&&now-heard<2000000?direct_.target(now):std::nullopt;
  }
  void directRelay(udpcore::DirectEndpoint endpoint){if(directEnabled_)direct_.relay(endpoint);}
  // Any thread: this leg is done. Media on UDP comes back at a forced IDR first, because
  // a replaced leg's session goes on and its client would wait for one. No record: the
  // caller may be the receive thread, and a journal write can wait.
  void detach()noexcept{
    bool was=false;{std::lock_guard lock(mutex_);was=route_==Route::udp;route_=Route::wss;ended_=true;}
    std::lock_guard lock(hookMutex_);
    if(was&&hooks_.path)hooks_.path(false);
    if(was&&hooks_.keyFrame)hooks_.keyFrame();
    hooks_={};
  }
  // A PING was answered, so this leg's relay forwards this client. Without a precise
  // timer the pacer could not hold the target, so the leg never says so.
  void probePassed()noexcept{probePassed_=true;}

  // Receive thread: the reply to send, once per grant. A failure uses up the grant
  // and the session stays on WebSocket. The Broker's substitute would pass here: not
  // end-to-end.
  std::optional<std::string> exchange(const Json& message,const std::string& session,int target){
    udpcore::PublicKey client;
    if(!probePassed_||target<=0||message.get("protocol").string()!=udpMediaProtocol||!udpBase64(message.get("publicKey").string(),client))return {};
    if(exchanged_.exchange(true))return {};
    udpcore::X25519 pair;udpcore::Secret<udpcore::keyBytes> shared;auto keys=std::make_unique<udpcore::SessionKeys>();
    if(!pair.valid()||!pair.agree(client,shared)||!udpcore::deriveKeys(shared,client,pair.publicKey(),reinterpret_cast<const uint8_t*>(session.data()),session.size(),udpcore::Role::host,*keys))return {};
    const auto reply=udpMediaKeyMessage(pair.publicKey(),session);if(reply.empty())return {};
    std::lock_guard lock(mutex_);keys_=std::move(keys);target_=target;return reply;
  }

  // Pipe reader. True: this message left WebSocket for good, with exactly its bytes.
  // `recovery`: video that answers a LOSS (the worker's mark, already taken off the bytes).
  bool outgoing(bool text,std::vector<uint8_t>& bytes,bool recovery=false){
    if(text){observe(bytes);return false;}
    const uint8_t kind=!bytes.empty()&&bytes[0]==2?udpcore::classAudio:udpcore::classVideo;udpcore::Envelope envelope;
    if(!udpcore::parseEnvelope(bytes.data(),bytes.size(),kind,envelope))return false;
    const bool video=kind==udpcore::classVideo;const size_t size=bytes.size();
    std::lock_guard lock(mutex_);
    if(video)epoch_=envelope.epoch;
    // The first IDR after stream.info opens the relay's gate and must follow that
    // text in order, so it stays on WebSocket whatever the path.
    if(envelope.key&&!firstKey_){firstKey_=true;return false;}
    if(route_==Route::armed&&envelope.key){
      if(!udpcore::fragmentCount(size)){route_=Route::wss;changes_.push_back({false,udpPathTooLarge,false});return false;}
      route_=Route::udp;changes_.push_back({true,udpPathProven,false});
    }
    if(route_!=Route::udp)return false;
    // A key frame too large for UDP makes WebSocket active by itself; a P-frame needs one.
    if(video&&!udpcore::fragmentCount(size)){route_=Route::wss;changes_.push_back({false,udpPathTooLarge,!envelope.key});return false;}
    if(!video&&udpcore::fragmentCount(size)>udpcore::maxAudioFragments)return false;
    if(inboxBytes_+size>udpcore::maxKeptBytes){
      // The leg stopped draining, so it would not act on a change: ask from here.
      route_=Route::wss;inbox_.clear();inboxBytes_=0;changes_.push_back({false,udpPathStalled,false});askKeyFrame();return false;
    }
    inboxBytes_+=size;inbox_.push_back({std::move(bytes),envelope.key,video,video&&recovery});return true;
  }

  // Channel's receive thread: the client's stream.feedback arrived on WSS (udpProbeNowUs clock).
  void wssHeard(uint64_t nowUs)noexcept{wssHeard_=nowUs;}
  void pauseInputs(){std::lock_guard lock(mutex_);inputMinimumEpoch_=uint32_t(epoch_)+1;}
  // audio-stats-v1, any thread: audio messages the sender took and refused since the last call.
  std::pair<uint64_t,uint64_t> takeAudio()noexcept{return {audioSent_.exchange(0),audioDropped_.exchange(0)};}

  // Leg thread: a datagram from the bound relay. Every client datagram is hostile until
  // its tag verifies: nothing below `open` runs for anything else.
  void received(std::span<const uint8_t> datagram,uint64_t nowUs,bool direct=false){
    if(!opener_)return;
    std::array<uint8_t,udpcore::maxPlaintext> plain;
    const size_t size=opener_->open(datagram.data(),datagram.size(),plain.data());if(!size)return;
    if(plain[0]==udpcore::kindDirectInput){
      const auto heard=wssHeard_.load();
      if(!direct||!onUdp_||!heard||nowUs<heard||nowUs-heard>=2000000||size<udpcore::directInputHeader)return;
      const uint32_t sequence=udpcore::get32(plain.data()+1);const uint16_t epoch=udpcore::get16(plain.data()+5),length=udpcore::get16(plain.data()+7);
      if(!sequence||epoch<inputMinimumEpoch_||epoch!=this->epoch()||size!=udpcore::directInputHeader+length)return;
      std::lock_guard lock(hookMutex_);if(hooks_.input)hooks_.input(sequence,epoch,std::string(reinterpret_cast<const char*>(plain.data()+udpcore::directInputHeader),length));return;
    }
    const auto control=sender_->control(plain.data(),size,nowUs);
    if(control.feedback){
      feedbackAt_=nowUs;
      const uint32_t arrived=control.received-received_,lost=control.lost-lost_;received_=control.received;lost_=control.lost;
      if(onUdp_){std::lock_guard lock(hookMutex_);if(hooks_.feedback)if(const int target=hooks_.feedback(control.rttUs,arrived,lost);target>0)sender_->setTarget(target);}
    }
    // The Sender merges requests only once a key frame reaches it, so the encoder's gap
    // before that needs this limit: a client holding the keys must not turn this into
    // IDRs at packet rate. It repeats every 500 ms, so half that never starves it.
    if(control.keyFrame&&onUdp_&&control.epoch==epoch()&&nowUs-keyFrameAt_>=udpcore::keyframeIntervalUs/2){keyFrameAt_=nowUs;++clientKeys_;askKeyFrame();}
    // The same bound for LOSS, per frame time: each one costs the encoder a forced frame.
    if(control.loss&&control.lastGood&&onUdp_&&control.epoch==epoch()&&nowUs-lossAt_>=lossIntervalUs){
      lossAt_=nowUs;sender_->recovering(nowUs);
      std::lock_guard lock(hookMutex_);if(hooks_.recover)hooks_.recover(control.lastGood);else if(hooks_.keyFrame)hooks_.keyFrame();
    }
  }
  // Leg thread, every millisecond or so while it has a sender.
  template<class Send> void pump(uint64_t nowUs,Send&& send){
    std::deque<Item> items;std::vector<Change> changes;std::unique_ptr<udpcore::SessionKeys> keys;int target=0;
    {std::lock_guard lock(mutex_);keys=std::move(keys_);target=target_;items.swap(inbox_);inboxBytes_=0;changes.swap(changes_);for(const auto& candidate:candidates_)direct_.candidate(candidate);candidates_.clear();}
    if(keys){
      if(directEnabled_)direct_.configure(keys->send,keys->receive);
      sealer_=std::make_unique<udpcore::Sealer>(keys->send,id_);opener_=std::make_unique<udpcore::Opener>(keys->receive);
      sender_=std::make_unique<udpcore::Sender>(target,nowUs,udpcore::randomChallenge);keyed_=true;
    }
    if(!sender_)return;
    for(const auto& change:changes)apply(change,nowUs);
    for(auto& item:items)feed(item,nowUs);
    decide(nowUs);
    if(over_)return;
    udpcore::Plain plain;
    while(sender_->next(nowUs,plain)){
      const size_t size=sealer_->seal(plain.bytes.data(),plain.size,datagram_.data());
      if(!size){leave(udpPathSealFailed,nowUs,true);break;}
      send(std::span<const uint8_t>(datagram_.data(),size));
    }
    datagrams_=sender_->stats().datagrams;repairs_=sender_->stats().repairs;recoveries_=sender_->stats().recoveries;refused_=sender_->stats().refused;
  }
  // Leg thread: its sender's timer, in milliseconds; 0 with nothing to send. 200 ms
  // TAILs and what is left in the queue need no pacing precision off UDP.
  DWORD paceMs()const{return !sender_||over_?0:onUdp_?1:5;}
  // Leg thread, on its way out for any reason. A closed leg has already detached, so
  // media still on UDP here means this leg failed under a working session.
  void ended()noexcept{
    try{
      bool was=false;{std::lock_guard lock(mutex_);was=route_==Route::udp;route_=Route::wss;ended_=true;inbox_.clear();inboxBytes_=0;}
      if(!was)return;
      {std::lock_guard lock(hookMutex_);if(hooks_.path)hooks_.path(false);}
      askKeyFrame();++fallbacks_;if(record_)record_(udpPathOnWss,udpPathSocketFailed,0);
    }catch(...){ }
  }
 private:
  friend struct UdpMediaPathTest;
  enum class Route:uint8_t{wss,armed,udp};
  struct Item {std::vector<uint8_t> bytes;bool key=false,video=false,recovery=false;};
  static constexpr uint64_t lossIntervalUs=30000;
  struct Change {bool onUdp=false;uint32_t reason=0;bool needsKey=false;uint32_t cause=0;};
  static std::string udpMediaKeyMessage(const udpcore::PublicKey& key,const std::string& session){
    char text[64]{};DWORD length=DWORD(sizeof(text));
    if(!CryptBinaryToStringA(key.data(),DWORD(key.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,text,&length))return {};
    return std::string("{\"type\":\"udp.key\",\"protocol\":\"")+udpMediaProtocol+"\",\"publicKey\":\""+std::string(text,length)+"\",\"sessionId\":"+jsonQuote(session)+'}';
  }
  // stream.info opens a new picture: its first IDR is the WebSocket's (see outgoing).
  // Keys done, stream.stats carries the counters; before that not a byte changes.
  void observe(std::vector<uint8_t>& text){
    const std::string_view view(reinterpret_cast<const char*>(text.data()),text.size());
    if(view.starts_with(R"({"type":"stream.info")")){
      std::lock_guard lock(mutex_);firstKey_=firstKeyAsked_=false;
      if(route_==Route::udp)changes_.push_back({false,udpPathNewStream,false});
      route_=Route::wss;
    }else if(keyed_&&view.starts_with(R"({"type":"stream.stats")")&&view.back()=='}'){
      bool onUdp=false;{std::lock_guard lock(mutex_);onUdp=route_==Route::udp;}
      const auto extra=",\"udpPath\":"+std::to_string(onUdp?1:0)+",\"udpDatagrams\":"+std::to_string(datagrams_.load())+",\"udpRetransmits\":"+std::to_string(repairs_.load())+",\"udpFallbacks\":"+std::to_string(fallbacks_.load())+",\"udpRecoveries\":"+std::to_string(recoveries_.load())
        // Stall forensics: IDRs the pacer's backlog asked for, client KEYFRAME requests honoured, repair datagrams refused.
        +",\"udpBacklogKeys\":"+std::to_string(backlogKeys_.load())+",\"udpClientKeys\":"+std::to_string(clientKeys_.load())+",\"udpRefused\":"+std::to_string(refused_.load());
      text.insert(text.end()-1,extra.begin(),extra.end());
    }
  }
  uint16_t epoch(){std::lock_guard lock(mutex_);return epoch_;}
  void askKeyFrame(){std::lock_guard lock(hookMutex_);if(hooks_.keyFrame)hooks_.keyFrame();}
  void apply(const Change& change,uint64_t nowUs){
    onUdp_=change.onUdp;
    if(change.onUdp)switchedAt_=nowUs;
    else if(change.reason!=udpPathNewStream){++fallbacks_;backoff_.failed(nowUs);}
    if(change.needsKey)askKeyFrame();
    {std::lock_guard lock(hookMutex_);if(hooks_.path)if(const int target=hooks_.path(change.onUdp);target>0)sender_->setTarget(target);}
    if(record_)record_(change.onUdp?udpPathOnUdp:udpPathOnWss,change.reason,change.cause);
  }
  void leave(uint32_t reason,uint64_t nowUs,bool forever=false,uint32_t cause=0){
    if(directEnabled_&&direct_.target(nowUs))direct_.backoff(nowUs+5000000);
    {std::lock_guard lock(mutex_);route_=Route::wss;inbox_.clear();inboxBytes_=0;}
    over_=over_||forever;apply({false,reason,true,cause},nowUs);
  }
  void feed(Item& item,uint64_t nowUs){
    const auto admission=item.video?sender_->video(std::move(item.bytes),item.key,item.recovery):sender_->audio(std::move(item.bytes));
    if(!item.video)++(admission.queued?audioSent_:audioDropped_);
    if(!admission.requestKey||nowUs-keyFrameAt_<udpcore::keyframeIntervalUs/2)return;
    keyFrameAt_=nowUs;++backlogKeys_;askKeyFrame();
    std::lock_guard lock(hookMutex_);if(hooks_.backlog)hooks_.backlog();
  }
  void decide(uint64_t nowUs){
    if(onUdp_){
      if(sender_->exhausted()||sealer_->exhausted())leave(udpPathExhausted,nowUs,true);
      // After a retry the Sender's clock is as old as the outage: the key frame gets the
      // same second to arrive that silence is allowed.
      else if(nowUs-switchedAt_>=udpcore::feedbackSilenceUs&&sender_->silent(nowUs)){
        const auto brokerHeard=wssHeard_.load();
        if(directTarget(nowUs)&&direct_.relayReady(nowUs)&&brokerHeard&&nowUs-brokerHeard<udpcore::feedbackSilenceUs){
          direct_.backoff(nowUs+30000000);switchedAt_=nowUs;sender_->recovering(nowUs);askKeyFrame();return;
        }
        // Nothing heard on UDP nor on WSS, whose stream.feedback runs every 100 ms: the
        // client's network is dark, not this path, and WSS would only add an IDR and a
        // backoff to come back (field case, a phone: WSS RTT 2.7 s at that moment).
        const uint32_t cause=sender_->silenceCause(nowUs);const uint64_t wss=wssHeard_.load();
        const bool dark=cause==1&&wss&&nowUs>wss&&nowUs-wss>=udpcore::feedbackSilenceUs;
        if(dark){if(!holding_){holding_=true;if(record_)record_(udpPathHeld,udpPathSilent,cause);}}
        // Heard again: video gets the second a switch gets for its key frame.
        else if(holding_){holding_=false;switchedAt_=nowUs;}
        else leave(udpPathSilent,nowUs,false,cause);
      }
      else holding_=false;
      return;
    }
    const bool fresh=feedbackAt_&&nowUs-feedbackAt_<udpcore::feedbackSilenceUs;bool ask=false;
    {std::lock_guard lock(mutex_);
      if(route_==Route::armed){if(!fresh)route_=Route::wss;}
      else if(route_==Route::wss&&fresh&&!over_&&!ended_&&sender_->proven()&&backoff_.mayProbe(nowUs)){
        // The leg can start after the stream's first IDR went by unseen: that needs
        // a key frame of its own on WebSocket before one may go on UDP.
        if(firstKey_){route_=Route::armed;ask=true;}
        else if(!firstKeyAsked_){firstKeyAsked_=true;ask=true;}
      }
    }
    if(ask)askKeyFrame();
  }

  const UdpLegId id_;
  std::mutex hookMutex_;UdpMediaHooks hooks_;
  const Record record_;
  const bool directEnabled_;udpcore::DirectPath direct_;std::vector<udpcore::DirectEndpoint> candidates_;
  std::atomic<bool> probePassed_{false},exchanged_{false},keyed_{false};
  std::atomic<uint32_t> inputMinimumEpoch_{0};
  std::atomic<uint64_t> datagrams_{0},repairs_{0},fallbacks_{0},wssHeard_{0},recoveries_{0},audioSent_{0},audioDropped_{0},backlogKeys_{0},clientKeys_{0},refused_{0};
  // Guarded by mutex_.
  std::mutex mutex_;Route route_=Route::wss;bool firstKey_=false,firstKeyAsked_=false,ended_=false;uint16_t epoch_=0;int target_=0;
  std::unique_ptr<udpcore::SessionKeys> keys_;std::deque<Item> inbox_;size_t inboxBytes_=0;std::vector<Change> changes_;
  // Leg thread. The Sender keeps running after a return to WebSocket: its TAILs are how
  // this grant's binding shows it works again, and message IDs and sequences go on.
  std::unique_ptr<udpcore::Sender> sender_;std::unique_ptr<udpcore::Sealer> sealer_;std::unique_ptr<udpcore::Opener> opener_;
  udpcore::ProbeBackoff backoff_;std::array<uint8_t,udpcore::maxDatagram> datagram_{};
  bool onUdp_=false,over_=false,holding_=false;uint64_t switchedAt_=0,feedbackAt_=0,keyFrameAt_=0,lossAt_=0;uint32_t received_=0,lost_=0;
};
}
