#pragma once
#include "udp_media_crypto.hpp"
#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace multitap::udp {
constexpr uint8_t kindDirectInput=0x30;
constexpr size_t directInputHeader=9,maxDirectInput=maxPlaintext-directInputHeader;
// udp-direct-v1: a return-routability check on the media socket. The broker
// supplies candidates; only a fresh, authenticated round trip can select one.
struct DirectEndpoint {
  uint32_t address = 0; uint16_t port = 0;
  bool operator==(const DirectEndpoint& other) const {return address==other.address&&port==other.port;}
};
// Freshness (no part of the wire format): an
// address learned from a ping occupies at most maxLearned of the slots, is
// dropped when it does not answer our challenge within learnedTrialUs (or
// goes silent for learnedIdleUs), and a broker candidate displaces it. A ping
// whose nonce was already seen from another address is a replay: neither
// answered nor learned. No path ever probes 0.0.0.0/8, multicast, reserved
// or broadcast addresses, nor loopback unless the relay itself is loopback.
class DirectPath {
 public:
  static constexpr size_t packetBytes = 53, maxCandidates = 5, maxLearned = 2, replayRing = 64;
  static constexpr uint64_t probeEveryUs = 250000, aliveUs = 1500000;
  static constexpr uint64_t learnedTrialUs = 3000000, learnedIdleUs = 10000000, replayWindowUs = 10000000;
  using Packet = std::array<uint8_t, packetBytes>;
  // An address this path may probe (network byte order, as in sockaddr_in).
  bool routable(DirectEndpoint endpoint) const {
    const auto first = firstOctet(endpoint.address);
    if (!endpoint.port || first == 0 || first >= 224) return false;
    return first != 127 || firstOctet(relay_.endpoint.address) == 127;
  }
  bool configure(const DirectionKey& send, const DirectionKey& receive) {
    constexpr char label[] = "multitap-udp-direct-v1";
    ready_ = send.ready && receive.ready &&
      hmacSha256(send.key.data(), keyBytes, reinterpret_cast<const uint8_t*>(label), sizeof(label)-1, send_.data()) &&
      hmacSha256(receive.key.data(), keyBytes, reinterpret_cast<const uint8_t*>(label), sizeof(label)-1, receive_.data());
    return ready_;
  }
  // A candidate the broker supplied.
  void candidate(DirectEndpoint endpoint) { add(endpoint, false, 0); }
  void relay(DirectEndpoint endpoint){relay_.endpoint=endpoint;}
  bool relayReady(uint64_t now)const{return relay_.seen&&now-relay_.seen<aliveUs;}
  uint64_t relayRtt(uint64_t now=0)const{return now&&!relayReady(now)?0:relay_.rtt;}
  void backoff(uint64_t until){backoffUntil_=until;}
  template<class Send> void tick(uint64_t now, Send&& send) {
    if (!ready_) return;
    // Learned addresses that never answered our challenge, or stopped.
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [&](const Peer& peer) {
      return peer.learned && (peer.seen ? now - peer.seen >= learnedIdleUs : now - peer.added >= learnedTrialUs);
    }), peers_.end());
    const auto probe=[&](Peer& peer){
      if (!peer.endpoint.port || now < peer.next) return;
      peer.next = now + probeEveryUs;
      if (!peer.waiting || now - peer.sent >= aliveUs) {
        if (BCryptGenRandom(nullptr, peer.nonce.data(), ULONG(peer.nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return;
        peer.sent = now; peer.waiting = true;
      }
      Packet packet{}; if (seal(1, peer.nonce.data(), packet)) send(peer.endpoint, packet);
    };
    probe(relay_);for(auto& peer:peers_)probe(peer);
  }
  template<class Send> bool received(const uint8_t* bytes, size_t size, DirectEndpoint from, uint64_t now, Send&& send) {
    if (!ready_ || size != packetBytes || bytes[0] != 'T' || bytes[1] != 'V' || bytes[2] != 'D' || bytes[3] != 1 || (bytes[4] != 1 && bytes[4] != 2)) return false;
    std::array<uint8_t,32> tag{};
    if (!hmacSha256(receive_.data(), keyBytes, bytes, 21, tag.data())) return false;
    uint8_t difference=0; for(size_t n=0;n<tag.size();++n) difference |= tag[n]^bytes[21+n];
    if (difference) return false;
    if (bytes[4] == 1) {
      // A valid peer can reveal a NAT's peer-reflexive address, but cannot make
      // it active without answering our own unpredictable challenge there.
      if (now - repliesAt_ >= 1000000) { repliesAt_=now; replies_=0; }
      if (++replies_ > 32) return true;
      if (!freshPing(bytes + 5, from, now)) return true;  // a replay from elsewhere
      add(from, true, now);
      Packet pong{}; if(seal(2,bytes+5,pong))send(from,pong);
    } else if(auto* peer=find(from);peer && peer->waiting && now-peer->sent<aliveUs) {
      uint8_t different=0;for(size_t n=0;n<peer->nonce.size();++n)different|=peer->nonce[n]^bytes[5+n];
      if(!different){peer->rtt=std::max<uint64_t>(1,now-peer->sent);peer->seen=now;peer->waiting=false;}
    }
    return true;
  }
  std::optional<DirectEndpoint> target(uint64_t now, uint64_t relayRttUs=0) const {
    if(now<backoffUntil_)return {};
    if(!relayRttUs)relayRttUs=relayRtt(now);
    const Peer* best=nullptr;
    for(const auto& peer:peers_)if(peer.seen && now-peer.seen<aliveUs && (!relayRttUs || peer.rtt<=relayRttUs+2000) && (!best || peer.rtt<best->rtt))best=&peer;
    return best ? std::optional(best->endpoint) : std::nullopt;
  }
  bool accepts(DirectEndpoint from,uint64_t now) const {
    for(const auto& peer:peers_)if(peer.endpoint==from && peer.seen && now-peer.seen<aliveUs)return true;
    return false;
  }
  uint64_t rtt(DirectEndpoint endpoint) const {for(const auto& peer:peers_)if(peer.endpoint==endpoint)return peer.rtt;return 0;}
  size_t learned() const {size_t count=0;for(const auto& peer:peers_)count+=peer.learned?1:0;return count;}
  size_t candidates() const {return peers_.size();}
 private:
  struct Peer {DirectEndpoint endpoint;std::array<uint8_t,16> nonce{};uint64_t next=0,sent=0,seen=0,rtt=0,added=0;bool waiting=false,learned=false;};
  struct SeenPing {std::array<uint8_t,16> nonce{};DirectEndpoint from{};uint64_t at=0;bool used=false;};
  static uint8_t firstOctet(uint32_t address){uint8_t bytes[4];std::memcpy(bytes,&address,4);return bytes[0];}
  bool add(DirectEndpoint endpoint, bool fromPing, uint64_t now) {
    if (!routable(endpoint) || endpoint==relay_.endpoint || find(endpoint)) return false;
    if (fromPing && learned() >= maxLearned) return false;
    if (peers_.size() >= maxCandidates) {
      if (fromPing) return false;
      // A broker candidate displaces a learned address, an unconfirmed one first.
      auto victim = std::find_if(peers_.begin(), peers_.end(), [](const Peer& peer){return peer.learned && !peer.seen;});
      if (victim == peers_.end()) victim = std::find_if(peers_.begin(), peers_.end(), [](const Peer& peer){return peer.learned;});
      if (victim == peers_.end()) return false;
      peers_.erase(victim);
    }
    Peer peer{endpoint}; peer.learned = fromPing; peer.added = now;
    peers_.push_back(peer);
    return true;
  }
  // The peer repeats one nonce until answered, always from the same address;
  // the same nonce from another address is a captured ping played back.
  bool freshPing(const uint8_t* nonce, DirectEndpoint from, uint64_t now) {
    for (const auto& seen : seen_) {
      if (seen.used && now - seen.at < replayWindowUs && std::memcmp(seen.nonce.data(), nonce, seen.nonce.size()) == 0) return seen.from == from;
    }
    auto& slot = seen_[seenNext_++ % seen_.size()];
    std::memcpy(slot.nonce.data(), nonce, slot.nonce.size()); slot.from = from; slot.at = now; slot.used = true;
    return true;
  }
  Peer* find(DirectEndpoint endpoint){if(relay_.endpoint==endpoint)return &relay_;for(auto& peer:peers_)if(peer.endpoint==endpoint)return &peer;return nullptr;}
  bool seal(uint8_t kind,const uint8_t* nonce,Packet& packet){packet[0]='T';packet[1]='V';packet[2]='D';packet[3]=1;packet[4]=kind;std::memcpy(packet.data()+5,nonce,16);return hmacSha256(send_.data(),keyBytes,packet.data(),21,packet.data()+21);}
  Secret<32> send_,receive_;bool ready_=false;Peer relay_{};std::vector<Peer> peers_;uint64_t repliesAt_=0,backoffUntil_=0;uint32_t replies_=0;
  std::array<SeenPing,replayRing> seen_{};size_t seenNext_=0;
};
}
