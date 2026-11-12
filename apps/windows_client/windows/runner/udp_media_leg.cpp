#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <chrono>
#include <mutex>
#include <thread>
#include "udp_media_leg.h"
#include "../../../../native/windows_video/udp_media_crypto.hpp"
#include "../../../../native/windows_video/udp_stun.hpp"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace multitap {
namespace {
constexpr DWORD kWaitMs = 2;  // Input/keys wake the socket; this only bounds receiver timers.
constexpr int kReceiveBuffer = 2 * 1024 * 1024;
constexpr unsigned kReceiveBurst = 256;  // datagrams per pass, so timers keep running
constexpr size_t kMaxArrivals = 256 * 3;
// A whole video message the decode queue refused waits this long before the leg gives up
// and skips to the next key frame, as WSS waits instead of dropping. The queue holds 8
// packets, a tick with tiles makes 2, and a repaired hole releases every frame behind it
// at once: dropping there cost a key frame round trip, and a refused key frame the
// Receiver's 500 ms repeat. A decoder that took nothing for this long is behind.
constexpr uint64_t kHoldUs = udp::keyframeIntervalUs;
constexpr size_t kMaxHeld = 256;

uint64_t SteadyUs() { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

struct Winsock {
  Winsock() { WSADATA data; ok = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
  ~Winsock() { if (ok) WSACleanup(); }
  bool ok = false;
};

bool ResolveIPv4(const std::string& host, uint32_t& ipv4) {
  Winsock winsock;
  addrinfo hints{}, *found = nullptr;
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
  if (!winsock.ok || getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) return false;
  ipv4 = reinterpret_cast<const sockaddr_in*>(found->ai_addr)->sin_addr.s_addr;
  freeaddrinfo(found);
  return true;
}

// Outlives the leg: a lookup that never returns must not hold up teardown.
struct Lookups {
  struct Entry { std::atomic<int> state{0}; uint32_t ipv4 = 0; };  // 0 open, 1 resolved, 2 failed
  explicit Lookups(size_t count) : entries(count) {}
  std::vector<Entry> entries;
};

udp::Probe::Proof ProofOf(uint8_t type, const char* label, size_t labelSize, const UdpMediaLeg::Grant& grant) {
  std::vector<uint8_t> message(label, label + labelSize);
  message.insert(message.end(), grant.id.begin(), grant.id.end());
  udp::Probe::Proof proof{};
  proof[0] = type; std::memcpy(&proof[1], grant.id.data(), grant.id.size());
  udp::hmacSha256(grant.ticket.data(), grant.ticket.size(), message.data(), message.size(), &proof[9]);
  return proof;
}
}

struct UdpMediaLeg::Impl {
  ~Impl(){if(wake)CloseHandle(wake);if(readable!=WSA_INVALID_EVENT)WSACloseEvent(readable);}
  Impl(Grant&& granted, std::shared_ptr<UdpPathGate> path, int64_t clockUs, Lookup resolver)
      : grant(std::move(granted)), gate(std::move(path)), lookup(std::move(resolver)), clock_base(int64_t(SteadyUs()) - clockUs) {
    SecureZeroMemory(granted.ticket.data(), granted.ticket.size());
    if (!lookup) lookup = ResolveIPv4;
  }

  void Run() {
    Winsock winsock;
    try { if (winsock.ok) Loop(); else Finish(udp::probeSocketError); } catch (...) { Finish(udp::probeSocketError); }
    if (sock != INVALID_SOCKET) closesocket(sock);
    if(readable!=WSA_INVALID_EVENT){WSACloseEvent(readable);readable=WSA_INVALID_EVENT;}
    std::lock_guard<std::mutex> lock(mutex);
    shared.running = false;
  }

  void Finish(int code) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!shared.report.code) shared.report.code = code;
  }

  void StartLookups(const std::shared_ptr<Lookups>& lookups) {
    for (size_t relay = 0; relay < lookups->entries.size(); ++relay) {
      try {
        const auto& target=relay<grant.relays.size()?grant.relays[relay]:grant.stun[relay-grant.relays.size()];
        std::thread([lookups, relay, host = target.first, resolver = lookup] {
          auto& entry = lookups->entries[relay];
          bool found = false;
          try { found = resolver(host, entry.ipv4); } catch (...) {}
          entry.state = found ? 1 : 2;
        }).detach();
      } catch (...) { lookups->entries[relay].state = 2; }
    }
  }

  bool OpenSocket() {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return false;
    readable=WSACreateEvent();
    u_long nonblocking = 1;
    DWORD reset = FALSE, returned = 0;
    sockaddr_in any{}; any.sin_family = AF_INET;
    // An ICMP answer to a closed relay port must not read as a broken socket.
    WSAIoctl(sock, SIO_UDP_CONNRESET, &reset, sizeof(reset), nullptr, 0, &returned, nullptr, nullptr);
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&kReceiveBuffer), sizeof(kReceiveBuffer));
    return wake&&readable!=WSA_INVALID_EVENT&&ioctlsocket(sock, FIONBIO, &nonblocking) == 0 && bind(sock, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) == 0&&WSAEventSelect(sock,readable,FD_READ)==0;
  }

  bool Send(size_t relay, const uint8_t* data, size_t size) {
    return sendto(sock, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&relays[relay]), sizeof(sockaddr_in)) == int(size);
  }
  static udp::DirectEndpoint Endpoint(const sockaddr_in& address){return {address.sin_addr.s_addr,ntohs(address.sin_port)};}
  bool DirectSend(udp::DirectEndpoint endpoint,const uint8_t* data,size_t size){
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=endpoint.address;address.sin_port=htons(endpoint.port);
    std::array<uint8_t,9+udp::DirectPath::packetBytes> wrapped{};
    if(size==udp::DirectPath::packetBytes && data[0]=='T' && data[1]=='V' && data[2]=='D' && RelayOf(address)>=0){wrapped[0]=3;std::copy(grant.id.begin(),grant.id.end(),wrapped.begin()+1);std::copy_n(data,size,wrapped.begin()+9);data=wrapped.data();size=wrapped.size();}
    return sendto(sock,reinterpret_cast<const char*>(data),int(size),0,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==int(size);
  }
  void DirectTick(uint64_t now){
    if(!grant.direct || !receiver)return;
    // The relay first: a loopback candidate is probed only beside a loopback relay.
    direct.relay(Endpoint(relays[chosen]));
    {std::lock_guard<std::mutex> lock(mutex);for(const auto& candidate:candidates)direct.candidate(candidate);candidates.clear();}
    direct.tick(now,[this](udp::DirectEndpoint endpoint,const udp::DirectPath::Packet& packet){DirectSend(endpoint,packet.data(),packet.size());});
    const auto selected=direct.target(now);
    std::lock_guard<std::mutex> lock(mutex);if(!shared.directPath)shared.directPath=1;
    shared.directRttMs=selected?int((direct.rtt(*selected)+999)/1000):-1;
    shared.relayRttMs=direct.relayRtt(now)?int((direct.relayRtt(now)+999)/1000):-1;
  }
  void Local(const sockaddr_in& relay){
    if(!grant.direct || localReported)return;localReported=true;
    sockaddr_in own{},route{};int size=sizeof(own);if(getsockname(sock,reinterpret_cast<sockaddr*>(&own),&size))return;
    const SOCKET query=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(query==INVALID_SOCKET)return;
    size=sizeof(route);const bool ok=connect(query,reinterpret_cast<const sockaddr*>(&relay),sizeof(relay))==0&&getsockname(query,reinterpret_cast<sockaddr*>(&route),&size)==0;closesocket(query);
    char text[INET_ADDRSTRLEN]{};if(ok&&inet_ntop(AF_INET,&route.sin_addr,text,sizeof(text))){std::lock_guard<std::mutex> lock(mutex);shared.local={{text,ntohs(own.sin_port)}};}
  }

  int RelayOf(const sockaddr_in& from) const {
    for (size_t n = 0; n < relays.size(); ++n)
      if (relays[n].sin_port && relays[n].sin_port == from.sin_port && relays[n].sin_addr.s_addr == from.sin_addr.s_addr) return int(n);
    return -1;
  }

  void Loop() {
    const auto bindProof = ProofOf(1, "multitap-udp-bind-v1", 19, grant), ackProof = ProofOf(2, "multitap-udp-ack-v1", 18, grant);
    SecureZeroMemory(grant.ticket.data(), grant.ticket.size());
    if (!OpenSocket()) return Finish(udp::probeSocketError);
    relays.assign(grant.relays.size(), sockaddr_in{});
    auto lookups = std::make_shared<Lookups>(grant.relays.size()+grant.stun.size());
    StartLookups(lookups);
    std::vector<bool> looked(lookups->entries.size(), false);
    udp::Probe probe(grant.relays.size(), grant.id, bindProof, ackProof, SteadyUs(), [this](size_t relay, const uint8_t* data, size_t size) { return Send(relay, data, size); });
    std::array<uint8_t, udp::maxDatagram + 1> datagram;
    while (!stop) {
      for (size_t relay = 0; relay < looked.size(); ++relay) {
        const int state = lookups->entries[relay].state;
        if (looked[relay] || !state) continue;
        looked[relay] = true;
        if (state != 1) continue;
        if(relay>=grant.relays.size()){if(grant.direct)stun.server({lookups->entries[relay].ipv4,grant.stun[relay-grant.relays.size()].second});continue;}
        relays[relay].sin_family = AF_INET; relays[relay].sin_port = htons(grant.relays[relay].second); relays[relay].sin_addr.s_addr = lookups->entries[relay].ipv4;
        probe.resolved(relay);
      }
      const HANDLE events[]{readable,wake};const DWORD signaled=WaitForMultipleObjects(2,events,FALSE,kWaitMs);
      if(signaled==WAIT_FAILED){probe.socketLost();Publish(probe);return;}
      if(stop)return;
      if(signaled==WAIT_OBJECT_0)WSAResetEvent(readable);
      if(signaled==WAIT_OBJECT_0+1)Inputs(SteadyUs());
      for (unsigned n = 0; n < kReceiveBurst; ++n) {
        sockaddr_in from{}; int fromSize = sizeof(from);
        const int size = recvfrom(sock, reinterpret_cast<char*>(datagram.data()), int(datagram.size()), 0, reinterpret_cast<sockaddr*>(&from), &fromSize);
        if (size == SOCKET_ERROR) {
          const int error = WSAGetLastError();
          if (error == WSAEWOULDBLOCK) break;
          if (error == WSAECONNRESET || error == WSAEMSGSIZE || error == WSAENETRESET) continue;
          probe.socketLost(); Publish(probe); return;
        }
        Datagram(probe, from, datagram.data(), size_t(size));
      }
      const uint64_t now = SteadyUs();
      probe.tick(now);
      Publish(probe);
      if (probe.over()) return;
      Keys(probe, now);
      if(probe.bound())Local(relays[probe.chosen()]);
      DirectTick(now);
      Inputs(now);
      if(grant.direct)stun.tick(now,[this](auto endpoint,const auto& bytes){DirectSend(endpoint,bytes.data(),bytes.size());});
      if (receiver) Media(now);
    }
  }

  void Datagram(udp::Probe& probe, const sockaddr_in& from, const uint8_t* data, size_t size) {
    if(grant.direct)if(const auto mapped=stun.received(data,size,Endpoint(from),SteadyUs())){in_addr address{};address.s_addr=mapped->address;char name[INET_ADDRSTRLEN]{};if(inet_ntop(AF_INET,&address,name,sizeof(name))){std::lock_guard<std::mutex> lock(mutex);shared.reflexive={{name,mapped->port}};}return;}
    const int relay = RelayOf(from);
    const uint64_t now = SteadyUs();
    if(grant.direct && receiver){
      const auto send=[this](udp::DirectEndpoint endpoint,const udp::DirectPath::Packet& packet){DirectSend(endpoint,packet.data(),packet.size());};
      if(direct.received(data,size,Endpoint(from),now,send))return;
      if(relay>=0 && size==9+udp::DirectPath::packetBytes && data[0]==3 && direct.received(data+9,size-9,Endpoint(from),now,send))return;
    }
    if(relay<0 && (!grant.direct || !direct.accepts(Endpoint(from),now)))return;
    if (!probe.bound() || size == udp::probeBytes) return probe.received(size_t(relay), data, size, now);
    if ((relay>=0 && size_t(relay) != probe.chosen()) || !opener) return;
    if (const size_t plain = opener->open(data, size, opened.bytes.data())) { if(grant.direct){std::lock_guard<std::mutex> lock(mutex);shared.directPath=relay<0?2:1;}Sync(); receiver->datagram(opened.bytes.data(), plain, now, output); Deliver(now); }
  }

  void Keys(const udp::Probe& probe, uint64_t now) {
    if (key_failed || receiver || probe.report().code != udp::probeReachable) return;
    if (!own_key) {
      own_key.emplace();
      if (!own_key->valid()) { key_failed = true; return; }
      std::lock_guard<std::mutex> lock(mutex);
      shared.publicKey = own_key->publicKey();
    }
    udp::PublicKey host;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!host_key) return;
      host = *host_key;
    }
    udp::Secret<udp::keyBytes> shared_secret;
    udp::SessionKeys keys;
    if (!own_key->agree(host, shared_secret) ||
        !udp::deriveKeys(shared_secret, own_key->publicKey(), host, reinterpret_cast<const uint8_t*>(grant.sessionId.data()), grant.sessionId.size(), udp::Role::client, keys)) { key_failed = true; return; }
    if(grant.direct)direct.configure(keys.send,keys.receive);
    sealer.emplace(keys.send, grant.id);
    opener.emplace(keys.receive);
    epoch = gate->epoch();
    wss_keys = gate->wssKeys();
    receiver.emplace(epoch, now);
    std::lock_guard<std::mutex> lock(mutex);
    shared.media = true;
  }
  void Inputs(uint64_t now){
    std::deque<InputItem> pending;{std::lock_guard<std::mutex> lock(mutex);pending.swap(inputs);}
    const auto target=direct.target(now);if(!grant.direct||!sealer||!target)return;
    std::array<uint8_t,udp::maxPlaintext> plain{};std::array<uint8_t,udp::maxDatagram> sealed{};
    for(const auto& item:pending){
      plain[0]=udp::kindDirectInput;udp::put32(plain.data()+1,item.sequence);udp::put16(plain.data()+5,item.epoch);udp::put16(plain.data()+7,uint16_t(item.bytes.size()));
      std::copy(item.bytes.begin(),item.bytes.end(),plain.begin()+udp::directInputHeader);
      if(const auto size=sealer->seal(plain.data(),udp::directInputHeader+item.bytes.size(),sealed.data()))DirectSend(*target,sealed.data(),size);
    }
  }

  // Before any datagram: what WSS announced may be what this one carries. What waits
  // for the decoder is dead either way: another epoch, or WSS took the path.
  void Sync() {
    const uint16_t announced = gate->epoch();
    if (announced != epoch) { receiver->setEpoch(epoch = announced); held.clear(); }
    const uint64_t keys = gate->wssKeys();
    if (keys != wss_keys) { wss_keys = keys; receiver->keyFrameOtherPath(); held.clear(); }
  }

  void Media(uint64_t now) {
    Sync();
    receiver->tick(now, output);
    Deliver(now);
  }

  // Video waits in order behind a full decode queue (kHoldUs); audio has its own queue
  // and never waits for it.
  void Deliver(uint64_t now) {
    for (auto& message : output.messages) {
      if (message.video) { held.push_back({std::move(message), now}); continue; }
      const int64_t size = int64_t(message.bytes.size());
      if (gate->OfferUdp(message.bytes, false, false, message.epoch, message.pts) != UdpPathGate::Delivery::accepted) continue;
      std::lock_guard<std::mutex> lock(mutex);
      shared.audioBytes += size;
    }
    output.messages.clear();
    Release(now);
    uint32_t keys = 0, nacks = 0;
    for (const auto& plain : output.control) {
      std::array<uint8_t, udp::maxDatagram> sealed;
      if (plain.size() == udp::feedbackBytes && plain[0] == udp::kindFeedback) { std::lock_guard<std::mutex> lock(mutex); shared.gaps = udp::get32(&plain[10]); }
      keys += plain[0] == udp::kindKeyframe ? 1 : 0; nacks += plain[0] == udp::kindNack ? 1 : 0;
      if (const size_t size = sealer->seal(plain.data(), plain.size(), sealed.data())) {if(const auto endpoint=direct.target(now);grant.direct&&endpoint)DirectSend(*endpoint,sealed.data(),size);else Send(chosen,sealed.data(),size);}
    }
    output.control.clear();
    if (keys || nacks) { std::lock_guard<std::mutex> lock(mutex); shared.keyRequests += keys; shared.nacks += nacks; }
  }

  void Release(uint64_t now) {
    uint32_t waits = 0, skips = 0;
    while (!held.empty()) {
      Held& next = held.front();
      const int64_t size = int64_t(next.message.bytes.size());
      const auto result = gate->OfferUdp(next.message.bytes, true, next.message.key, next.message.epoch, next.message.pts);
      if (result == UdpPathGate::Delivery::full && now - next.since < kHoldUs && held.size() <= kMaxHeld) {
        if (!next.waited) { next.waited = true; ++waits; }
        break;
      }
      if (result == UdpPathGate::Delivery::accepted) {
        // Arrival is when the message was whole, as before the wait existed.
        std::lock_guard<std::mutex> lock(mutex);
        if (shared.arrivals.size() >= kMaxArrivals) shared.arrivals.erase(shared.arrivals.begin(), shared.arrivals.begin() + 3);
        shared.arrivals.insert(shared.arrivals.end(), {int64_t(next.message.pts), int64_t(next.since) - clock_base, size});
        held.pop_front();
        continue;
      }
      // Refused by the rule, or the queue stayed full: what waits behind it cannot be
      // decoded, so skip to the next key frame. One already waiting needs no request; a
      // refused switch key frame would otherwise end the picture for good.
      ++skips;
      held.pop_front();
      while (!held.empty() && !held.front().message.key) held.pop_front();
      if (held.empty()) receiver->queueFull(now);
    }
    if (waits || skips) { std::lock_guard<std::mutex> lock(mutex); shared.queueWaits += waits; shared.queueSkips += skips; }
  }

  void Publish(const udp::Probe& probe) {
    chosen = probe.bound() ? probe.chosen() : 0;
    std::lock_guard<std::mutex> lock(mutex);
    shared.report = probe.report();
  }

  Grant grant;
  std::shared_ptr<UdpPathGate> gate;
  Lookup lookup;
  const int64_t clock_base;
  std::thread worker;
  std::atomic<bool> stop{false};
  SOCKET sock = INVALID_SOCKET;
  std::vector<sockaddr_in> relays;
  size_t chosen = 0;

  // Leg thread only.
  std::optional<udp::X25519> own_key;
  std::optional<udp::Sealer> sealer;
  std::optional<udp::Opener> opener;
  std::optional<udp::Receiver> receiver;
  udp::Receiver::Output output;
  struct Held { udp::Receiver::Message message; uint64_t since = 0; bool waited = false; };
  std::deque<Held> held;  // whole video in order, behind a full decode queue
  udp::Plain opened;
  uint16_t epoch = 0;
  uint64_t wss_keys = 0;
  bool key_failed = false;

  std::mutex mutex;  // guards the two below
  Reading shared;
  HANDLE wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);WSAEVENT readable=WSA_INVALID_EVENT;
  struct InputItem{uint32_t sequence;uint16_t epoch;std::vector<uint8_t> bytes;};std::deque<InputItem> inputs;
  udp::DirectPath direct;udp::StunBinding stun;std::vector<udp::DirectEndpoint> candidates;bool localReported=false;
  std::optional<udp::PublicKey> host_key;
};

UdpMediaLeg::UdpMediaLeg(Grant&& grant, std::shared_ptr<UdpPathGate> gate, int64_t clockUs, Lookup lookup) : impl_(std::make_unique<Impl>(std::move(grant), std::move(gate), clockUs, std::move(lookup))) {
  impl_->worker = std::thread([impl = impl_.get()] { impl->Run(); });
}
UdpMediaLeg::~UdpMediaLeg() { Stop(); }

void UdpMediaLeg::HostKey(const std::array<uint8_t, 32>& key) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->host_key = key;
  SetEvent(impl_->wake);
}

void UdpMediaLeg::Peer(const std::vector<std::pair<std::string,uint16_t>>& candidates){
  if(candidates.size()>udp::DirectPath::maxCandidates)return;std::vector<udp::DirectEndpoint> parsed;
  for(const auto& [name,port]:candidates){in_addr address{};if(!port||inet_pton(AF_INET,name.c_str(),&address)!=1)return;parsed.push_back({address.s_addr,port});}
  std::lock_guard<std::mutex> lock(impl_->mutex);impl_->candidates=std::move(parsed);SetEvent(impl_->wake);
}
void UdpMediaLeg::Input(uint32_t sequence,uint16_t epoch,std::vector<uint8_t> bytes){
  if(!sequence||bytes.empty()||bytes.size()>udp::maxDirectInput)return;
  std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->grant.direct||!impl_->shared.media||impl_->shared.directPath!=2||impl_->inputs.size()>=64)return;
  impl_->inputs.push_back({sequence,epoch,std::move(bytes)});
  SetEvent(impl_->wake);
}

UdpMediaLeg::Reading UdpMediaLeg::Read() {
  Reading reading;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    reading = impl_->shared;
    impl_->shared.arrivals.clear(); impl_->shared.audioBytes = 0;
  }
  reading.path = impl_->gate->view();
  return reading;
}

void UdpMediaLeg::Stop() {
  impl_->stop = true;
  SetEvent(impl_->wake);
  if (impl_->worker.joinable()) impl_->worker.join();
}
}
