// udp-media-v1 on the client. Pure, with injected time: the probe schedule (the Dart
// socket code this replaces, case by case) and the path rule. Then the leg over real
// loopback sockets against a fake relay and Host built on the core Sender and crypto,
// through to a fake engine. Loopback only: it says nothing about a real network.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "udp_media_leg.h"
#include "../../../../native/windows_video/udp_media_crypto.hpp"

using namespace multitap;
namespace {
std::atomic<int> checks{0};
void require(bool value, const std::string& message) { ++checks; if (!value) throw std::runtime_error(message); }

std::vector<uint8_t> hex(const char* text) {
  std::vector<uint8_t> bytes;
  for (; text[0] && text[1]; text += 2) bytes.push_back(uint8_t(std::stoi(std::string(text, 2), nullptr, 16)));
  return bytes;
}
// Printed by the relay's server/src/media-udp.mjs under Node for this ticket and ID.
const auto ticketBytes = hex("030a11181f262d343b424950575e656c737a81888f969da4abb2b9c0c7ced5dc");
const auto idBytes = hex("a1b2c3d4e5f60718");
const auto bindBytes = hex("01a1b2c3d4e5f60718ab888d6d37401b1e1a08612ffb35634f57bae04104bffdc1a5428a597a1645cf");
const auto ackBytes = hex("02a1b2c3d4e5f6071819ea576916fe53cab5b247fcd507e39b51db70c504a8dae16905443ead3d41ad");
const auto pingBytes = hex("03a1b2c3d4e5f6071801000000000000050000000000003039");

udp::Probe::Proof proof(const std::vector<uint8_t>& bytes) { udp::Probe::Proof out{}; std::copy(bytes.begin(), bytes.end(), out.begin()); return out; }
std::array<uint8_t, 8> legId() { std::array<uint8_t, 8> out{}; std::copy(idBytes.begin(), idBytes.end(), out.begin()); return out; }

// ---- Probe schedule, 1 ms steps ----------------------------------------------------
struct ProbeSim {
  struct Sent { uint64_t ms; size_t relay; std::vector<uint8_t> bytes; bool ping() const { return bytes[0] == 3; } uint32_t sequence() const { return udp::get32(&bytes[13]); } };
  explicit ProbeSim(size_t relays = 1, uint64_t base = 0) : base_(base), now(base), probe(relays, legId(), proof(bindBytes), proof(ackBytes), base, [this](size_t relay, const uint8_t* data, size_t size) {
    if (failing.count(relay)) return false;
    sent.push_back({(now - base_) / 1000, relay, std::vector<uint8_t>(data, data + size)});
    if (onSend) onSend(sent.back());
    return true;
  }) { for (size_t relay = 0; relay < relays; ++relay) ready.insert(relay); }
  void start() { for (const size_t relay : ready) probe.resolved(relay); }
  void until(uint64_t ms) {
    for (;; now += 1000) {
      while (!later.empty() && later.begin()->first <= now) { auto action = later.begin()->second; later.erase(later.begin()); action(); }
      probe.tick(now);
      if (now >= base_ + ms * 1000) break;
    }
  }
  void after(uint64_t ms, std::function<void()> action) { later.emplace(now + ms * 1000, std::move(action)); }
  void ack(size_t relay = 0) { probe.received(relay, ackBytes.data(), ackBytes.size(), now); }
  std::vector<uint8_t> pong(const Sent& ping) { auto bytes = ping.bytes; std::fill(bytes.begin() + 1, bytes.begin() + 9, uint8_t(0x99)); bytes[9] = 2; return bytes; }
  void deliverPong(const Sent& ping) { const auto bytes = pong(ping); probe.received(ping.relay, bytes.data(), bytes.size(), now); }
  void answer(std::function<int(uint32_t)> delayMs) {  // negative: never
    onSend = [this, delayMs](const Sent& s) {
      if (!s.ping()) return;
      const int delay = delayMs(s.sequence());
      if (delay < 0) return;
      const Sent ping = s;
      after(uint64_t(delay), [this, ping] { deliverPong(ping); });
    };
  }
  std::vector<Sent> pings() const { std::vector<Sent> out; for (const auto& s : sent) if (s.ping()) out.push_back(s); return out; }
  std::vector<Sent> binds() const { std::vector<Sent> out; for (const auto& s : sent) if (!s.ping()) out.push_back(s); return out; }

  uint64_t base_, now;
  std::set<size_t> ready, failing;
  std::vector<Sent> sent;
  std::function<void(const Sent&)> onSend;
  std::multimap<uint64_t, std::function<void()>> later;
  udp::Probe probe;
};

void probeBytesMatchTheRelay() {
  std::vector<std::vector<uint8_t>> sent;
  udp::Probe probe(1, legId(), proof(bindBytes), proof(ackBytes), 0, [&](size_t, const uint8_t* data, size_t size) { sent.emplace_back(data, data + size); return true; });
  probe.resolved(0); probe.tick(0);
  require(sent.back() == bindBytes, "BIND differs from the relay's reference");
  // The reference is PING 5 stamped 12345 us. Here the stamp moves with the clock, so
  // PING 0 carries it and PING 5 the rest.
  probe.received(0, ackBytes.data(), ackBytes.size(), 12345);
  require(std::equal(sent.back().begin() + 17, sent.back().end(), pingBytes.begin() + 17), "PING stamp differs from the relay's reference");
  for (int n = 1; n <= 5; ++n) probe.tick(12345 + n * 200000);
  auto ping = sent.back();
  std::copy(pingBytes.begin() + 17, pingBytes.end(), ping.begin() + 17);
  require(ping == pingBytes, "PING differs from the relay's reference");
}

void bindSchedule() {
  { ProbeSim sim; sim.start(); sim.until(4999);
    require(sim.sent.size() == 25, "One relay: one BIND every 200 ms");
    for (size_t n = 0; n < sim.sent.size(); ++n) require(sim.sent[n].ms == n * 200, "BIND time");
    require(sim.probe.report().code == 0, "No outcome before the budget ends");
    sim.until(5000); require(sim.probe.report().code == udp::probeBindTimeout && sim.probe.over(), "BIND timeout"); }
  { ProbeSim sim(2); sim.start(); sim.until(2500);
    const size_t expected[] = {0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0};
    require(sim.sent.size() == 13, "Two relays: thirteen BINDs");
    for (size_t n = 0; n < 13; ++n) require(sim.sent[n].relay == expected[n] && sim.sent[n].ms == n * 200, "Relays in order, three each, then again"); }
  { ProbeSim sim; sim.start(); sim.until(0);
    auto bad = ackBytes;
    const auto bounce = [&](std::vector<uint8_t> bytes) { sim.probe.received(0, bytes.data(), bytes.size(), sim.now); };
    bounce(std::vector<uint8_t>(ackBytes.begin(), ackBytes.end() - 1));
    { auto longer = ackBytes; longer.push_back(0); bounce(longer); }
    for (const size_t at : {size_t(0), size_t(1), size_t(9), size_t(40)}) { bad = ackBytes; bad[at] ^= 1; bounce(bad); }
    bounce(bindBytes); bounce(pingBytes); bounce({});
    sim.until(1000); require(sim.pings().empty(), "A bad ACK started the burst");
    sim.ack(); require(sim.pings().size() == 1, "The real ACK did not bind"); }
  { ProbeSim sim(2); sim.start(); sim.until(700); sim.ack(1); sim.until(6000);
    for (const auto& ping : sim.pings()) require(ping.relay == 1, "PING went to the wrong relay");
    require(sim.binds().back().ms == 600 && sim.probe.report().relay == 1 && sim.probe.report().code == udp::probeNoPong, "No BIND after the ACK; the relay that answered is kept"); }
  { ProbeSim sim(2); sim.start(); sim.until(700); sim.ack(0); sim.until(6000); require(sim.probe.report().relay == 0, "A late ACK from an earlier relay counts"); }
  { ProbeSim sim(2); sim.ready.erase(0); sim.start(); sim.until(700);
    require(sim.sent.size() == 1 && sim.sent[0].relay == 1, "An open name holds its turn, then the next relay goes");
    sim.probe.resolved(0); sim.until(1300); require(sim.sent.back().relay == 0, "A name that resolves late joins in"); }
  { ProbeSim sim; sim.until(100); require(sim.sent.empty(), "BIND before the name resolved");
    sim.probe.resolved(0); require(sim.sent.size() == 1 && sim.sent[0].ms == 100, "BIND at once, on its own turn"); }
}

void failureCodes() {
  { ProbeSim sim(2); sim.ready.clear(); sim.start(); sim.until(5000); require(sim.probe.report().code == udp::probeLookupFailed && sim.sent.empty(), "3 when no name resolves"); }
  { ProbeSim sim; sim.failing.insert(0); sim.start(); sim.until(5000); require(sim.probe.report().code == udp::probeSocketError, "2 when every send fails"); }
  { ProbeSim sim(2); sim.ready.erase(0); sim.start(); sim.until(5000); require(sim.probe.report().code == udp::probeBindTimeout, "4 when relays stay silent, even if another name failed"); }
  { ProbeSim sim; sim.start(); sim.until(0); sim.ack(); sim.until(2999);
    require(sim.probe.report().code == 0, "No outcome before 3 s");
    sim.until(3000);
    const auto r = sim.probe.report();
    require(r.code == udp::probeNoPong && r.relay == 0 && r.lossPct == 100 && r.keepSent == 0 && r.rttP50Ms < 0, "5 when the ACK came and no PONG did"); }
}

void pingSchedule() {
  { ProbeSim sim; sim.start(); sim.until(0); sim.answer([](uint32_t) { return 10; }); sim.ack(); sim.until(90000);
    const auto pings = sim.pings();
    require(pings.size() == 29, "25 and 4 keepalives");
    for (size_t n = 0; n < 25; ++n) require(pings[n].ms == n * 200, "Burst time");
    const uint64_t later[] = {24800, 44800, 64800, 84800};
    for (size_t n = 0; n < 4; ++n) require(pings[25 + n].ms == later[n], "Keepalive time");
    for (size_t n = 0; n < 29; ++n) require(pings[n].sequence() == n, "Sequence");
    require(sim.probe.report().keepSent == 4, "Keepalives sent"); }
  { ProbeSim sim; sim.start(); sim.until(0);
    sim.answer([](uint32_t sequence) { return sequence < 25 ? int(sequence) + 1 : 900; });
    sim.ack(); sim.until(30000);
    const auto r = sim.probe.report();
    require(r.code == udp::probeReachable && r.rttP50Ms == 13 && r.rttP95Ms == 24 && r.lossPct == 0 && r.keepSent == 1 && r.keepLost == 0, "Percentiles come from the burst only"); }
  { ProbeSim sim; sim.start(); sim.until(0);
    sim.answer([](uint32_t sequence) { return sequence == 3 ? 1001 : sequence == 4 ? 1000 : sequence == 5 ? -1 : 10; });
    sim.ack(); sim.until(6000);
    const auto r = sim.probe.report();
    require(r.lossPct == 8 && r.code == udp::probeReachable && r.rttP95Ms == 10, "Loss counts PINGs without a PONG within 1 s"); }
  { ProbeSim sim; sim.start(); sim.until(0);
    sim.onSend = [&](const ProbeSim::Sent& s) { if (s.ping() && !s.sequence()) { const auto ping = s; sim.after(5, [&sim, ping] { for (int n = 0; n < 30; ++n) sim.deliverPong(ping); }); } };
    sim.ack(); sim.until(6000); require(sim.probe.report().lossPct == 96, "A duplicate PONG hid a lost PING"); }
  { ProbeSim sim; sim.start(); sim.until(0); sim.ack();
    const auto pong = sim.pong(sim.pings().front());
    const auto bounce = [&](std::vector<uint8_t> bytes) { sim.probe.received(0, bytes.data(), bytes.size(), sim.now); };
    bounce(std::vector<uint8_t>(pong.begin(), pong.end() - 1));
    { auto longer = pong; longer.push_back(0); bounce(longer); }
    for (const auto& [at, value] : std::vector<std::pair<size_t, int>>{{0, 2}, {9, 1}, {10, 1}, {12, 1}, {16, 7}, {24, pong[24] ^ 1}}) { auto bad = pong; bad[at] = uint8_t(value); bounce(bad); }
    sim.until(6000);
    require(sim.probe.report().code == udp::probeNoPong && sim.probe.report().lossPct == 100, "A PONG must match its PING exactly"); }
  { ProbeSim sim; sim.start(); sim.until(0);
    sim.answer([](uint32_t sequence) { return sequence == 26 ? -1 : sequence == 27 ? 1500 : 100; });
    sim.ack(); sim.until(70000);
    const auto r = sim.probe.report();
    require(r.keepSent == 3 && r.keepLost == 2 && r.code == udp::probeReachable, "Keepalives count sent and unanswered"); }
}

void waitingForTheHost() {
  { ProbeSim sim; sim.start(); sim.until(0); sim.answer([](uint32_t) { return 450; }); sim.ack(); sim.until(7000);
    const auto pings = sim.pings();
    require(pings.size() == 25, "A first PONG for sequence 0 loses nothing");
    require(sim.probe.report().code == udp::probeReachable && sim.probe.report().lossPct == 0 && sim.probe.report().rttP50Ms == 450, "A slow path is not a late Host"); }
  { ProbeSim sim; sim.start(); sim.until(0);
    sim.onSend = [&](const ProbeSim::Sent& s) { if (!s.ping() || s.ms < 650) return; const auto ping = s; sim.after(20, [&sim, ping] { sim.deliverPong(ping); }); };
    sim.ack(); sim.until(7000);
    const auto pings = sim.pings();
    require(pings.size() == 30 && pings[4].sequence() == 4 && pings[5].sequence() == 0 && pings[5].ms == 820 && pings.back().ms == 820 + 24 * 200, "A first PONG for a later sequence restarts from 0");
    const auto r = sim.probe.report();
    require(r.code == udp::probeReachable && r.rttP50Ms == 20 && r.rttP95Ms == 20 && r.lossPct == 0, "Nothing is lost to a Host that binds late"); }
  { ProbeSim sim; sim.start(); sim.until(0);
    std::map<uint32_t, ProbeSim::Sent> old;
    sim.onSend = [&](const ProbeSim::Sent& s) {
      if (!s.ping()) return;
      if (s.ms < 650) { old.emplace(s.sequence(), s); return; }
      if (s.ms < 820 || s.sequence() != 2) { const auto ping = s; sim.after(20, [&sim, ping] { sim.deliverPong(ping); }); }
    };
    sim.ack(); sim.until(900);
    for (const uint32_t sequence : {1u, 2u, 3u}) sim.deliverPong(old.at(sequence));
    sim.until(1300); sim.deliverPong(old.at(2)); sim.until(7000);
    require(sim.probe.report().lossPct == 4 && sim.probe.report().rttP95Ms == 20, "A late PONG of the old numbering counts for nothing"); }
  { ProbeSim sim; sim.start(); sim.until(0);
    sim.onSend = [&](const ProbeSim::Sent& s) { if (!s.ping() || s.ms < 2800) return; const auto ping = s; sim.after(150, [&sim, ping] { sim.deliverPong(ping); }); };
    sim.ack(); sim.until(9000);
    require(sim.pings().size() == 15 + 25 && sim.probe.report().code == udp::probeReachable && sim.probe.report().lossPct == 0, "The last waiting PING can still start the burst"); }
  { ProbeSim sim; sim.start(); sim.until(0); sim.ack(); sim.until(2999);
    require(sim.pings().size() == 15 && sim.probe.report().code == 0, "Fifteen waiting PINGs");
    sim.until(3000);
    const auto last = sim.pings().back();
    sim.deliverPong(last); sim.until(48000);
    const auto pings = sim.pings();
    require(pings.size() == 17 && pings[15].ms == 22800 && pings[15].sequence() == 25 && pings[16].ms == 42800 && pings[16].sequence() == 26, "No PONG in 3 s: keepalives go on, numbered after a burst");
    require(sim.probe.report().code == udp::probeNoPong && sim.probe.report().keepSent == 2 && sim.probe.report().keepLost == 2, "A late PONG restarts nothing"); }
  { ProbeSim sim; sim.start(); sim.until(0); sim.answer([](uint32_t sequence) { return sequence < 25 ? -1 : 40; }); sim.ack(); sim.until(30000);
    require(sim.pings().size() == 16 && sim.probe.report().code == udp::probeNoPong && sim.probe.report().lossPct == 100 && sim.probe.report().keepSent == 1 && sim.probe.report().keepLost == 0, "A keepalive answered later leaves the outcome alone"); }
  { ProbeSim sim; sim.start(); sim.until(0); sim.ack(); sim.until(1000); sim.probe.socketLost();
    require(sim.probe.over() && sim.probe.report().code == udp::probeSocketError && sim.probe.report().relay == 0 && sim.probe.report().lossPct < 0, "A socket lost during the burst"); }
  { ProbeSim sim; sim.start(); sim.until(0); sim.answer([](uint32_t sequence) { return sequence < 25 ? 10 : -1; }); sim.ack(); sim.until(24900); sim.probe.socketLost();
    const auto r = sim.probe.report();
    require(r.code == udp::probeReachable && r.keepSent == 1 && r.keepLost == 1, "A socket lost later keeps what was measured"); }
}

// ---- Path rule -----------------------------------------------------------------------
std::vector<uint8_t> media(uint8_t kind, bool key, uint16_t epoch, uint64_t pts, size_t payload = 32, uint8_t fill = 0x41) {
  std::vector<uint8_t> packet(12 + payload, fill);
  packet[0] = kind; packet[1] = key ? 1 : 0; packet[2] = uint8_t(epoch); packet[3] = uint8_t(epoch >> 8);
  for (unsigned n = 0; n < 8; ++n) packet[4 + n] = uint8_t(pts >> (8 * n));
  return packet;
}
std::vector<uint8_t> tile(uint16_t epoch, uint64_t pts) { auto packet = media(1, false, epoch, pts); packet[12] = 'T'; packet[13] = 'S'; packet[14] = 'L'; packet[15] = '1'; return packet; }
std::vector<uint8_t> wire(std::vector<uint8_t> packet) { packet[2] = packet[3] = 0; return packet; }

struct FakeEngine {
  UdpPathGate::Offer offer() {
    return [this](const std::vector<uint8_t>& packet, bool watch) {
      std::lock_guard<std::mutex> lock(mutex);
      if (refuse > 0) { --refuse; ++refused; return MediaPushResult::full; }
      if (stopped) { ++afterStop; return MediaPushResult::stopped; }
      taken.push_back(packet); watches += watch ? 1 : 0;
      return MediaPushResult::accepted;
    };
  }
  size_t count() { std::lock_guard<std::mutex> lock(mutex); return taken.size(); }
  std::vector<std::vector<uint8_t>> copy() { std::lock_guard<std::mutex> lock(mutex); return taken; }
  void refuseNext(int n) { std::lock_guard<std::mutex> lock(mutex); refuse = n; }
  std::mutex mutex;
  std::vector<std::vector<uint8_t>> taken;
  int refuse = 0, refused = 0, watches = 0, afterStop = 0;
  bool stopped = false;
};

using Delivery = UdpPathGate::Delivery;
Delivery udpOffer(UdpPathGate& gate, std::vector<uint8_t> packet) {
  udp::Envelope envelope;
  const bool video = packet[0] == 1;
  udp::parseEnvelope(packet.data(), packet.size(), video ? udp::classVideo : udp::classAudio, envelope);
  return gate.OfferUdp(packet, video, envelope.key, envelope.epoch, envelope.pts);
}
struct Rig {
  explicit Rig(bool keyed = true, uint16_t epoch = 1) { gate.Attach(engine.offer()); gate.Stream(epoch, 1); gate.Engage(keyed); }
  MediaPushResult wss(std::vector<uint8_t> packet) { return gate.OfferWss(wire(std::move(packet)), false); }
  Delivery over(std::vector<uint8_t> packet) { return udpOffer(gate, std::move(packet)); }
  FakeEngine engine;
  UdpPathGate gate;
};

void pathRule() {
  // The contract's rule, in both directions.
  { Rig r; require(r.over(media(1, false, 1, 10)) == Delivery::dropped, "A P-frame counted from a path that is not active");
    require(r.wss(media(1, false, 1, 10)) == MediaPushResult::accepted && r.engine.count() == 1, "A P-frame from the active path was dropped");
    require(r.over(media(1, true, 1, 20)) == Delivery::accepted && r.gate.view().active == 1, "A UDP key frame did not make UDP active");
    require(r.wss(media(1, false, 1, 30)) == MediaPushResult::accepted && r.engine.count() == 2, "A WSS P-frame passed while UDP is active");
    require(r.over(media(1, false, 1, 30)) == Delivery::accepted && r.engine.count() == 3, "A UDP P-frame after its key frame was dropped");
    require(r.wss(media(1, true, 1, 40)) == MediaPushResult::accepted && r.engine.count() == 4 && r.gate.view().active == 0, "A WSS key frame did not make WSS active");
    require(r.gate.view().fallbacks == 1, "The return to WSS was not counted");
    require(r.over(media(1, false, 1, 50)) == Delivery::dropped && r.engine.count() == 4, "A UDP P-frame passed after WSS took over");
    require(r.wss(media(1, false, 1, 50)) == MediaPushResult::accepted && r.engine.count() == 5, "WSS stopped after its key frame"); }
  // PTS must be newer than the last delivered, on either path, key frames included.
  { Rig r; require(r.over(media(1, true, 1, 100)) == Delivery::accepted, "First key");
    require(r.over(media(1, true, 1, 100)) == Delivery::dropped, "A repeat of the same PTS passed");
    require(r.over(media(1, true, 1, 99)) == Delivery::dropped, "An older key frame passed on UDP");
    r.wss(media(1, true, 1, 99));
    require(r.engine.count() == 1 && r.gate.view().active == 1 && r.gate.wssKeys() == 0, "An older WSS key frame took over"); }
  // A full queue leaves the rule alone: WSS offers the same packet again.
  { Rig r; r.engine.refuseNext(1);
    require(r.wss(media(1, true, 1, 10)) == MediaPushResult::full, "A full queue did not reach WSS");
    require(r.wss(media(1, true, 1, 10)) == MediaPushResult::accepted && r.engine.count() == 1, "The same packet could not be offered again");
    r.engine.refuseNext(1);
    require(r.over(media(1, true, 1, 20)) == Delivery::full && r.gate.view().active == 0, "A refused UDP key frame changed the active path"); }
  // Tiles are stamped from the tick: they follow the path and never move "newer".
  { Rig r; r.wss(media(1, true, 1, 100));
    require(r.wss(tile(1, 1000)) == MediaPushResult::accepted && r.engine.count() == 2, "A tile on the active path was dropped");
    require(r.wss(media(1, false, 1, 200)) == MediaPushResult::accepted && r.engine.count() == 3, "A tile ahead of the encoder's PTS swallowed the next frame");
    require(r.over(tile(1, 201)) == Delivery::dropped && r.engine.count() == 3, "A tile passed from the wrong path"); }
  // The epoch gate.
  { Rig r; r.over(media(1, true, 1, 10));
    require(r.over(media(1, true, 0, 20)) == Delivery::dropped && r.over(media(2, false, 0, 20)) == Delivery::dropped, "A stale epoch passed");
    require(r.over(media(1, true, 2, 20)) == Delivery::dropped && r.over(media(2, false, 2, 20)) == Delivery::dropped, "A future epoch passed");
    r.gate.Stream(2, 2);
    require(r.gate.epoch() == 2, "Announced epoch");
    require(r.over(media(1, true, 1, 30)) == Delivery::dropped, "The old epoch passed after the announcement");
    require(r.over(media(1, false, 2, 5)) == Delivery::dropped, "A P-frame opened a new epoch");
    require(r.over(media(1, true, 2, 5)) == Delivery::accepted, "PTS restarts with the epoch");
    require(r.gate.view().keyedStream == 2, "A key frame over UDP did not name its stream"); }
  // The key gate, and a stream run that restarts PTS without a new epoch.
  { Rig r(false);
    require(r.over(media(1, false, 1, 10)) == Delivery::dropped && r.over(media(2, false, 1, 10)) == Delivery::dropped, "Media passed before the first key frame");
    require(r.wss(media(1, true, 1, 20)) == MediaPushResult::accepted, "WSS key");
    require(r.over(media(2, false, 1, 30)) == Delivery::accepted, "Audio stopped after the key frame");
    r.gate.Attach(r.engine.offer());
    require(r.over(media(1, false, 1, 40)) == Delivery::dropped && r.over(media(2, false, 1, 40)) == Delivery::dropped, "A new decoder took media before a key frame");
    require(r.over(media(1, true, 1, 40)) == Delivery::accepted, "A new decoder refused a key frame");
    r.gate.Stream(1, 2);
    require(r.over(media(1, false, 1, 7)) == Delivery::accepted, "A restarted run's PTS stayed behind the old one"); }
  // Audio on WSS has no path; a WSS key frame cues the leg.
  { Rig r; r.over(media(1, true, 1, 10));
    const auto before = r.engine.count();
    require(r.wss(media(2, false, 1, 11)) == MediaPushResult::accepted && r.engine.count() == before + 1, "WSS audio was dropped in flight");
    require(r.gate.wssKeys() == 0, "Audio read as a key frame");
    r.wss(media(1, true, 1, 12)); require(r.gate.wssKeys() == 1, "The leg's cue did not move"); }
  // Nothing reaches a detached engine, and a WSS push never fails for it.
  { Rig r; r.gate.Attach({});
    require(r.over(media(1, true, 1, 10)) == Delivery::dropped && !r.gate.attached(), "Detached");
    require(r.wss(media(1, true, 1, 10)) == MediaPushResult::accepted && r.engine.count() == 0, "A WSS push reached a detached engine"); }
  // Only video is watched on UDP, and the epoch never reaches the decoder.
  { Rig r; r.over(media(1, true, 1, 10, 32, 0x51)); r.over(media(2, false, 1, 11));
    require(r.engine.watches == 1 && r.engine.copy()[0] == wire(media(1, true, 1, 10, 32, 0x51)), "The decoder's input differs from the WSS path's"); }
  // A decoder refusal other than full is a drop.
  { UdpPathGate gate; gate.Attach([](const std::vector<uint8_t>&, bool) { return MediaPushResult::invalid; }); gate.Stream(1, 1); gate.Engage(true);
    require(udpOffer(gate, media(1, true, 1, 10)) == Delivery::dropped, "An invalid message was not a drop");
    require(gate.view().active == 0, "A refused key frame changed the active path"); }
  // Until the Host's key arrives WSS is a straight pass: no rule, no parsing, same bytes.
  { FakeEngine engine; UdpPathGate gate; gate.Attach(engine.offer()); gate.Stream(1, 1);
    const std::vector<std::vector<uint8_t>> packets = {wire(media(1, false, 1, 50)), wire(media(1, false, 1, 50)), wire(media(1, false, 1, 10)), wire(media(2, false, 1, 5)), {1, 2, 3}};
    for (const auto& packet : packets) require(gate.OfferWss(packet, true) == MediaPushResult::accepted, "A disengaged gate refused a packet");
    require(engine.copy() == packets && engine.watches == 5, "A disengaged gate changed the WSS path"); }
  // Off again, the rule is gone.
  { Rig r; r.over(media(1, true, 1, 10)); r.gate.Disengage();
    require(r.wss(media(1, false, 1, 1)) == MediaPushResult::accepted && r.engine.count() == 2 && r.gate.view().active == 0 && !r.gate.view().fallbacks, "Disengage left the rule on"); }
  // Attach cannot return while an offer is inside the engine.
  { UdpPathGate gate; std::atomic<bool> entered{false}, release{false}, detached{false};
    gate.Attach([&](const std::vector<uint8_t>&, bool) { entered = true; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); return MediaPushResult::accepted; });
    gate.Stream(1, 1); gate.Engage(false);
    std::thread sender([&] { udpOffer(gate, media(1, true, 1, 10)); });
    while (!entered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::thread stopper([&] { gate.Attach({}); detached = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool early = detached;
    release = true; sender.join(); stopper.join();
    require(!early && detached, "Attach returned while an offer was inside the engine"); }
}

// ---- Fake relay and Host on a real socket ------------------------------------------------
uint64_t nowUs() { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
void waitFor(const std::function<bool()>& done, int ms, const std::string& what) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!done()) { if (std::chrono::steady_clock::now() > end) throw std::runtime_error("Timed out: " + what); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}

// Plays the relay and the Host behind it, on one thread that owns the core Sender.
class FakeHost {
 public:
  enum class Mode { silent, ackOnly, full };
  explicit FakeHost(Mode mode = Mode::full) : mode_(mode), sock_(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) {
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock_, reinterpret_cast<sockaddr*>(&local), sizeof(local))) throw std::runtime_error("bind");
    int size = sizeof(local); getsockname(sock_, reinterpret_cast<sockaddr*>(&local), &size); port = ntohs(local.sin_port);
    u_long nonblocking = 1; ioctlsocket(sock_, FIONBIO, &nonblocking);
    int buffer = 4 * 1024 * 1024; setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    worker_ = std::thread([this] { Run(); });
  }
  ~FakeHost() { stop_ = true; worker_.join(); closesocket(sock_); }
  // Hands the Host's half of the keys to its thread, and returns its public key.
  udp::PublicKey Keys(const udp::PublicKey& client, const std::string& sessionId, int bps = 60000000) {
    udp::X25519 own; udp::Secret<udp::keyBytes> shared; auto keys = std::make_shared<udp::SessionKeys>();
    require(own.valid() && own.agree(client, shared) && udp::deriveKeys(shared, client, own.publicKey(), reinterpret_cast<const uint8_t*>(sessionId.data()), sessionId.size(), udp::Role::host, *keys), "Host keys");
    const auto hostId = std::array<uint8_t, 8>{0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99};
    Post([this, keys, hostId, bps] { sealer_.emplace(keys->send, hostId); opener_.emplace(keys->receive); sender_.emplace(bps, nowUs(), udp::randomChallenge); });
    return own.publicKey();
  }
  void Post(std::function<void()> action) { std::lock_guard<std::mutex> lock(mutex_); actions_.push_back(std::move(action)); }
  void Video(std::vector<uint8_t> message, bool key, bool recovery = false) { Post([this, message = std::move(message), key, recovery]() mutable { sender_->video(std::move(message), key, recovery); }); }
  // On this thread and at once: the first datagram can beat the leg's next pass.
  void VideoRightAfter(std::function<void()> first, std::vector<uint8_t> message, bool key) { Post([this, first = std::move(first), message = std::move(message), key]() mutable { first(); sender_->video(std::move(message), key); Send(); }); }
  void Audio(std::vector<uint8_t> message) { Post([this, message = std::move(message)]() mutable { sender_->audio(std::move(message)); }); }
  void Raw(std::vector<uint8_t> datagram) { Post([this, datagram = std::move(datagram)] { sendto(sock_, reinterpret_cast<const char*>(datagram.data()), int(datagram.size()), 0, reinterpret_cast<const sockaddr*>(&client_), sizeof(client_)); }); }
  void Replay() { Post([this] { if (!last_.empty()) sendto(sock_, reinterpret_cast<const char*>(last_.data()), int(last_.size()), 0, reinterpret_cast<const sockaddr*>(&client_), sizeof(client_)); }); }
  uint16_t port = 0;
  std::atomic<int> binds{0}, pings{0}, feedbacks{0}, keyRequests{0}, lastKeyEpoch{-1}, losses{0};
  // A Host that answers LOSS (as UdpMediaPath does); off, it is a released Host that ignores it.
  std::atomic<bool> answersLoss{false};
  std::atomic<uint64_t> lastGood{0};
  std::atomic<uint64_t> repairs{0}, sent{0}, dropped{0}, garbled{0};
  std::atomic<bool> proven{false};
  std::atomic<int> dropDownPerMille{0}, dropUpPerMille{0};
  std::atomic<int> firstPingsDropped{0};
  std::atomic<bool> pinFirstBind{true};
  std::vector<uint8_t> firstBind() { std::lock_guard<std::mutex> lock(mutex_); return bind_; }

 private:
  bool Lose(std::atomic<int>& rate) { state_ = state_ * 6364136223846793005ull + 1442695040888963407ull; return int((state_ >> 33) % 1000) < rate.load(); }
  void Run() {
    std::array<uint8_t, 2048> buffer;
    while (!stop_) {
      std::deque<std::function<void()>> actions;
      { std::lock_guard<std::mutex> lock(mutex_); actions.swap(actions_); }
      for (auto& action : actions) action();
      fd_set readable; FD_ZERO(&readable); FD_SET(sock_, &readable);
      timeval wait{0, 500};
      select(0, &readable, nullptr, nullptr, &wait);
      for (;;) {
        sockaddr_in from{}; int size = sizeof(from);
        const int got = recvfrom(sock_, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from), &size);
        if (got == SOCKET_ERROR) break;
        Datagram(buffer.data(), size_t(got), from);
      }
      if (sender_) Send();
    }
  }
  void Datagram(uint8_t* data, size_t size, const sockaddr_in& from) {
    if (mode_ == Mode::silent) return;
    client_ = from;
    if (size == udp::proofBytes && data[0] == 1) {
      ++binds; { std::lock_guard<std::mutex> lock(mutex_); if (bind_.empty()) bind_.assign(data, data + size); }
      sendto(sock_, reinterpret_cast<const char*>(ackBytes.data()), int(ackBytes.size()), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
      return;
    }
    if (mode_ == Mode::ackOnly) return;
    if (size == udp::probeBytes && data[0] == 3 && data[9] == 1) {
      ++pings;
      if (firstPingsDropped > 0) { --firstPingsDropped; return; }
      std::fill(data + 1, data + 9, uint8_t(0x99)); data[9] = 2;
      sendto(sock_, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
      return;
    }
    if (!opener_ || Lose(dropUpPerMille)) return;
    udp::Plain plain;
    const size_t opened = opener_->open(data, size, plain.bytes.data());
    if (!opened) { ++garbled; return; }
    const auto control = sender_->control(plain.bytes.data(), opened, nowUs());
    if (control.feedback) ++feedbacks;
    if (control.keyFrame) { ++keyRequests; lastKeyEpoch = control.epoch; }
    if (control.loss && answersLoss) { sender_->recovering(nowUs()); lastGood = control.lastGood; ++losses; }
    proven = sender_->proven();
  }
  void Send() {
    const uint64_t now = nowUs();
    udp::Plain plain;
    std::array<uint8_t, udp::maxDatagram> sealed;
    while (sender_->next(now, plain)) {
      const size_t size = sealer_->seal(plain.bytes.data(), plain.size, sealed.data());
      last_.assign(sealed.data(), sealed.data() + size);
      if (plain.bytes[0] == udp::kindFragment && Lose(dropDownPerMille)) { ++dropped; continue; }
      sendto(sock_, reinterpret_cast<const char*>(sealed.data()), int(size), 0, reinterpret_cast<const sockaddr*>(&client_), sizeof(client_));
      ++sent;
    }
    repairs = sender_->stats().repairs;
  }
  Mode mode_;
  SOCKET sock_;
  sockaddr_in client_{};
  std::atomic<bool> stop_{false};
  std::thread worker_;
  std::mutex mutex_;
  std::deque<std::function<void()>> actions_;
  std::vector<uint8_t> bind_, last_;
  uint64_t state_ = 42;
  std::optional<udp::Sealer> sealer_;
  std::optional<udp::Opener> opener_;
  std::optional<udp::Sender> sender_;
};

UdpMediaLeg::Grant grantFor(std::vector<uint16_t> ports) {
  UdpMediaLeg::Grant grant;
  for (const auto port : ports) grant.relays.push_back({"127.0.0.1", port});
  grant.id = legId(); std::copy(ticketBytes.begin(), ticketBytes.end(), grant.ticket.begin());
  grant.sessionId = "loopback-session";
  return grant;
}

// A leg through the probe and the keys, on a Host that may now send media.
struct Session {
  explicit Session(FakeHost& fake, uint16_t epoch = 1, bool keyed = true) : host(fake), gate(std::make_shared<UdpPathGate>()) {
    gate->Attach(engine.offer()); gate->Stream(epoch, 1);
    leg.emplace(grantFor({host.port}), gate, 5000000);
    waitFor([&] { return Latest().publicKey.has_value(); }, 9000, "the probe passed");
    gate->Engage(keyed);
    leg->HostKey(host.Keys(*reading.publicKey, "loopback-session"));
    waitFor([&] { return host.proven.load(); }, 3000, "FEEDBACK proved the address");
  }
  ~Session() { if (leg) leg->Stop(); }
  const UdpMediaLeg::Reading& Latest() {
    auto next = leg->Read();
    next.audioBytes += reading.audioBytes;
    next.arrivals.insert(next.arrivals.begin(), reading.arrivals.begin(), reading.arrivals.end());
    reading = std::move(next);
    return reading;
  }
  size_t videos() { size_t count = 0; for (const auto& packet : engine.copy()) count += packet[0] == 1 ? 1 : 0; return count; }
  FakeHost& host;
  FakeEngine engine;
  std::shared_ptr<UdpPathGate> gate;
  std::optional<UdpMediaLeg> leg;
  UdpMediaLeg::Reading reading;
};

std::vector<uint8_t> frame(bool key, uint16_t epoch, uint64_t pts, size_t size) {
  auto packet = media(1, key, epoch, pts, size);
  for (size_t n = 12; n < packet.size(); ++n) packet[n] = uint8_t(pts * 31 + n * 7);
  return packet;
}

std::string lossSummary;
void mediaWithLoss() {
  FakeHost host;
  Session session(host, 1, false);
  host.dropDownPerMille = 30; host.dropUpPerMille = 10;
  std::map<uint64_t, std::vector<uint8_t>> sentFrames;
  std::atomic<bool> keyWanted{false};
  uint64_t pts = 1000;
  const auto send = [&](bool key, size_t size) { auto message = frame(key, 1, pts, size); sentFrames[pts] = message; host.Video(std::move(message), key); pts += 16667; };
  send(true, 120000);
  int requests = 0;
  for (int n = 0; n < 150; ++n) {
    if (host.keyRequests.load() > requests) { requests = host.keyRequests.load(); keyWanted = true; }
    send(keyWanted.exchange(false), 4000 + size_t(n % 7) * 3000);
    if (n % 2 == 0) { auto audio = media(2, false, 1, pts, 1920, uint8_t(n)); host.Audio(std::move(audio)); }
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  host.dropDownPerMille = 0; host.dropUpPerMille = 0;
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const uint64_t last = pts;
  send(true, 60000);
  waitFor([&] { const auto taken = session.engine.copy(); for (const auto& packet : taken) if (packet[0] == 1) { uint64_t at = 0; for (unsigned n = 0; n < 8; ++n) at |= uint64_t(packet[4 + n]) << (8 * n); if (at == last) return true; } return false; }, 3000, "the last key frame");
  uint64_t previous = 0; size_t videos = 0, audios = 0;
  for (const auto& packet : session.engine.copy()) {
    require(!packet[2] && !packet[3], "The epoch reached the decoder");
    if (packet[0] == 2) { ++audios; require(packet.size() == 1932, "Audio changed size"); continue; }
    uint64_t at = 0; for (unsigned n = 0; n < 8; ++n) at |= uint64_t(packet[4 + n]) << (8 * n);
    require(at > previous, "Video left its order"); previous = at;
    auto expected = sentFrames.at(at); expected[2] = expected[3] = 0;
    require(packet == expected, "A reassembled message differs from what the Host sent");
    ++videos;
  }
  require(videos >= 40 && audios > 0, "Too little came through at 3% loss: " + std::to_string(videos));
  require(host.repairs.load() > 0 && host.dropped.load() > 0, "No loss was repaired");
  lossSummary = "\"lossFrames\":" + std::to_string(sentFrames.size()) + ",\"lossFramesDelivered\":" + std::to_string(videos) + ",\"lossDatagramsDropped\":" + std::to_string(host.dropped.load()) + ",\"lossRepairs\":" + std::to_string(host.repairs.load()) + ",\"lossKeyRequests\":" + std::to_string(host.keyRequests.load()) + ",";
  const auto reading = session.Latest();
  require(reading.path.active == 1 && reading.media, "The active path is not UDP");
  require(reading.arrivals.size() == videos * 3, "Arrivals differ from what the decoder took");
  require(reading.arrivals[1] >= 5000000 && reading.arrivals[1] < 65000000, "Arrivals are not on the session clock");
  require(reading.path.keyedStream == 1, "The key frame over UDP was not reported");
  require(reading.report.code == udp::probeReachable && reading.report.relay == 0 && reading.report.lossPct == 0, "The probe's numbers went missing");
  session.leg->Stop();
  require(session.engine.watches == int(videos), "Video on UDP was not watched");
}

void idleFeedbackAndBind() {
  FakeHost host;
  Session session(host);
  const auto before = host.feedbacks.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  const int count = host.feedbacks.load() - before;
  require(count >= 7 && count <= 13, "FEEDBACK did not keep its timer on an idle screen: " + std::to_string(count));
  require(host.firstBind() == bindBytes, "The real BIND differs from the relay's reference");
  // One socket from BIND to media: the relay pinned it.
  require(session.engine.count() == 0, "Media from nowhere");
}

void pathsOverSockets() {
  FakeHost host;
  Session session(host);
  host.Video(frame(true, 1, 100, 30000), true);
  waitFor([&] { return session.gate->view().active == 1; }, 2000, "UDP active");
  require(session.gate->OfferWss(wire(frame(false, 1, 150, 100)), false) == MediaPushResult::accepted && session.engine.count() == 1, "WSS passed while UDP is active");
  host.Video(frame(false, 1, 200, 30000), false);
  waitFor([&] { return session.engine.count() == 2; }, 2000, "UDP P-frame");
  // Back to WSS at a forced IDR: in-flight UDP frames die, and the Receiver is told.
  require(session.gate->OfferWss(wire(frame(true, 1, 300, 100)), false) == MediaPushResult::accepted && session.engine.count() == 3, "The WSS key frame");
  require(session.gate->view().active == 0 && session.gate->view().fallbacks == 1, "Fallback");
  host.Video(frame(false, 1, 400, 30000), false);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  require(session.engine.count() == 3, "A UDP P-frame passed after the fallback");
  // The Host switches back with a key frame.
  host.Video(frame(true, 1, 500, 30000), true);
  waitFor([&] { return session.engine.count() == 4; }, 2000, "The switch back");
  require(session.gate->view().active == 1, "The switch back did not make UDP active");
  const auto reading = session.Latest();
  require(reading.report.code == udp::probeReachable, "A path change touched the probe");
  // Three came over UDP; the frames the rule dropped arrived, but reach no count.
  require(reading.arrivals.size() == 3 * 3, "Arrivals count what the decoder never took");
}

void hostileDatagrams() {
  FakeHost host;
  Session session(host);
  host.Video(frame(true, 1, 100, 3000), true);
  waitFor([&] { return session.engine.count() == 1; }, 2000, "The first key frame");
  std::vector<uint8_t> junk(100);
  for (size_t size : {size_t(1), size_t(16), size_t(17), size_t(33), size_t(34), size_t(35), size_t(41), size_t(100), size_t(1199), size_t(1200), size_t(1201), size_t(1500)}) {
    junk.assign(size, uint8_t(size));
    host.Raw(junk);
    junk[0] = 3; host.Raw(junk);
    if (size > 17) { for (size_t n = 9; n < 16; ++n) junk[n] = 0; junk[16] = 1; host.Raw(junk); }
  }
  // A PONG for no PING, a replay of a real datagram, and the keys' forgeries.
  { auto pong = pingBytes; pong[9] = 2; host.Raw(pong); }
  for (int n = 0; n < 5; ++n) host.Replay();
  // Epochs the announcement did not name: dropped, and the session goes on.
  host.Video(frame(true, 0, 200, 3000), true);
  host.Video(frame(true, 2, 300, 3000), true);
  host.Audio(media(2, false, 2, 300, 1920));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  require(session.engine.count() == 1, "Hostile data reached the decoder");
  // The Receiver asks for a key frame after a future epoch; its number is the current one.
  session.gate->Stream(2, 2);
  session.gate->Attach(session.engine.offer());
  host.Video(frame(true, 2, 400, 3000), true);
  waitFor([&] { return session.engine.count() == 2; }, 2000, "The next epoch's key frame");
  host.Audio(media(2, false, 2, 410, 1920));
  host.Audio(media(2, false, 1, 420, 1920));
  waitFor([&] { return session.engine.count() >= 3; }, 2000, "This epoch's audio");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  require(session.engine.count() == 3, "A stale epoch's audio passed");
  require(session.Latest().running, "A hostile datagram stopped the leg");
}

void announcedEpochs() {
  FakeHost host;
  Session session(host);
  for (uint16_t epoch = 2; epoch < 12; ++epoch) {
    const size_t before = session.engine.count();
    host.VideoRightAfter([&session, epoch] { session.gate->Stream(epoch, epoch); session.gate->Attach(session.engine.offer()); }, frame(true, epoch, 100, 2000), true);
    waitFor([&] { return session.engine.count() == before + 1; }, 1000, "A key frame right after its announcement");
  }
}

void fullQueue() {
  FakeHost host;
  Session session(host);
  host.Video(frame(true, 1, 100, 3000), true);
  waitFor([&] { return session.engine.count() == 1; }, 2000, "The first key frame");
  // A queue that is full for a moment (a repaired hole releases every frame behind it at
  // once) costs nothing: the frames wait and go in order, and no key frame is asked for.
  session.engine.refuseNext(3);
  for (uint64_t n = 0; n < 5; ++n) { host.Video(frame(false, 1, 200 + n, 3000), false); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
  waitFor([&] { return session.engine.count() == 6; }, 2000, "The frames that waited");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  require(host.keyRequests.load() == 0, "A momentarily full queue asked for a key frame");
  { uint64_t previous = 0; for (const auto& packet : session.engine.copy()) { uint64_t at = 0; for (unsigned n = 0; n < 8; ++n) at |= uint64_t(packet[4 + n]) << (8 * n); require(at > previous, "Waiting frames left their order"); previous = at; } }
  { const auto reading = session.Latest(); require(reading.queueWaits >= 1 && reading.queueSkips == 0 && reading.keyRequests == 0, "The wait was not counted as a wait"); }
  // A queue that stays full is a decoder behind: after the hold, skip to the next key frame.
  session.engine.refuseNext(1 << 30);
  for (uint64_t n = 0; n < 3; ++n) { host.Video(frame(false, 1, 220 + n, 3000), false); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
  waitFor([&] { return host.keyRequests.load() > 0; }, 3000, "A key frame request");
  session.engine.refuseNext(0);
  host.Video(frame(false, 1, 240, 3000), false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  require(session.engine.count() == 6, "P-frames passed the hole a full queue left");
  require(host.lastKeyEpoch.load() == 1, "The request named another epoch");
  { const auto reading = session.Latest(); require(reading.queueSkips >= 1 && reading.keyRequests >= 1, "The skip was not counted"); }
  host.Video(frame(true, 1, 300, 3000), true);
  waitFor([&] { return session.engine.count() == 7; }, 2000, "The next key frame");
  host.Video(frame(false, 1, 310, 3000), false);
  waitFor([&] { return session.engine.count() == 8; }, 2000, "P-frames after the key frame");
  require(session.Latest().running, "A full queue stopped the leg");
  // Waiting again, a key frame on the other path answers the request.
  session.engine.refuseNext(1 << 30);
  host.Video(frame(false, 1, 320, 3000), false);
  const int asked = host.keyRequests.load();
  waitFor([&] { return host.keyRequests.load() > asked; }, 3000, "Another key frame request");
  session.engine.refuseNext(0);
  require(session.gate->OfferWss(wire(frame(true, 1, 330, 100)), false) == MediaPushResult::accepted, "The WSS key frame");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const int settled = host.keyRequests.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  require(host.keyRequests.load() == settled, "The leg kept asking after WSS brought the key frame");
}

// A resize with UDP active: the second key frame lands while the decoder restarts, and
// the first one, on WSS, makes WSS active. Every later UDP P-frame is refused, and only
// a new key frame from the Host can end that.
void refusedSwitchKey() {
  FakeHost host;
  Session session(host);
  host.Video(frame(true, 1, 1000, 3000), true);
  waitFor([&] { return session.gate->view().active == 1; }, 2000, "UDP active");
  session.gate->Stream(2, 2);
  session.gate->Attach({});
  host.Video(frame(true, 2, 40000, 3000), true);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  session.gate->Attach(session.engine.offer());
  require(session.gate->OfferWss(wire(frame(true, 2, 10000, 100)), false) == MediaPushResult::accepted, "The WSS key frame");
  require(session.gate->view().active == 0, "WSS did not take over");
  const int asked = host.keyRequests.load();
  for (uint64_t n = 0; n < 8; ++n) { host.Video(frame(false, 2, 60000 + n * 16667, 3000), false); std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
  waitFor([&] { return host.keyRequests.load() > asked; }, 2000, "A key frame request after refused frames");
  require(host.lastKeyEpoch.load() == 2, "The request named another epoch");
  const size_t before = session.engine.count();
  host.Video(frame(true, 2, 400000, 3000), true);
  waitFor([&] { return session.engine.count() == before + 1; }, 2000, "The key frame that ends it");
  require(session.gate->view().active == 1, "UDP did not take the path back");
}

void givenUp() {
  FakeHost host;
  Session session(host);
  host.Video(frame(true, 1, 100, 3000), true);
  waitFor([&] { return session.engine.count() == 1; }, 2000, "The first key frame");
  // A frame and its repairs all lost: NACK runs out, then the key frame request.
  host.dropDownPerMille = 1000;
  host.Video(frame(false, 1, 200, 3000), false);
  waitFor([&] { return host.keyRequests.load() > 0; }, 3000, "The give-up's key frame request");
  host.dropDownPerMille = 0;
  host.Video(frame(false, 1, 300, 3000), false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  require(session.engine.count() == 1, "A P-frame passed a frame given up");
  waitFor([&] { return session.Latest().gaps > 0; }, 1000, "The gap in the statistics");
  host.Video(frame(true, 1, 400, 3000), true);
  waitFor([&] { return session.engine.count() == 2; }, 2000, "The key frame after the gap");
  require(session.Latest().running, "A gap stopped the leg");
}

// The same loss on a Host that answers LOSS: it learns the decoder's last frame, the
// KEYFRAME that came along is merged, and its recovery frame (a P-frame) restarts the
// picture through the real socket, crypto, leg and gate.
void givenUpRecovered() {
  FakeHost host; host.answersLoss = true;
  Session session(host);
  host.Video(frame(true, 1, 100, 3000), true); host.Video(frame(false, 1, 150, 3000), false);
  waitFor([&] { return session.engine.count() == 2; }, 2000, "The first frames");
  host.dropDownPerMille = 1000;
  host.Video(frame(false, 1, 200, 3000), false);
  waitFor([&] { return host.losses.load() > 0; }, 3000, "The give-up's LOSS");
  require(host.lastGood.load() == 150, "LOSS did not name the decoder's last frame");
  host.dropDownPerMille = 0;
  host.Video(frame(false, 1, 300, 3000), false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  require(session.engine.count() == 2, "A plain P-frame ended a loss wait");
  host.Video(frame(false, 1, 400, 3000), false, true);
  waitFor([&] { return session.engine.count() == 3; }, 2000, "The recovery frame");
  host.Video(frame(false, 1, 500, 3000), false);
  waitFor([&] { return session.engine.count() == 4; }, 2000, "P-frames after the recovery frame");
  require(host.keyRequests.load() == 0, "The KEYFRAME sent with the LOSS was not merged");
  uint64_t previous = 0; std::vector<uint64_t> shown;
  for (const auto& packet : session.engine.copy()) { uint64_t at = 0; for (unsigned n = 0; n < 8; ++n) at |= uint64_t(packet[4 + n]) << (8 * n); require(at > previous, "Frames left their order"); previous = at; shown.push_back(at); }
  require(shown == std::vector<uint64_t>({100, 150, 400, 500}), "The decoder did not get exactly the frames before the loss and from the recovery frame on");
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  require(host.keyRequests.load() == 0 && host.losses.load() == 1, "Requests went on after the recovery frame");
}

void phase2Only() {
  // A Broker that never forwards udp.key: the probe runs, no media path changes.
  FakeHost host;
  FakeEngine engine;
  auto gate = std::make_shared<UdpPathGate>();
  gate->Attach(engine.offer()); gate->Stream(1, 1);
  UdpMediaLeg leg(grantFor({host.port}), gate, 0);
  waitFor([&] { return leg.Read().publicKey.has_value(); }, 9000, "The probe passed");
  const auto reading = leg.Read();
  require(reading.report.lossPct == 0 && reading.report.rttP50Ms >= 0 && !reading.media && reading.publicKey.has_value(), "Phase 2 numbers");
  const auto packet = wire(frame(false, 1, 5, 200));
  require(gate->OfferWss(packet, false) == MediaPushResult::accepted && engine.count() == 1 && engine.copy()[0] == packet, "The WSS path changed without keys");
  require(gate->view().active == 0 && !gate->view().fallbacks, "A path changed without keys");
}

void firstRelayClosed() {
  FakeHost host;
  SOCKET closed = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind(closed, reinterpret_cast<sockaddr*>(&local), sizeof(local));
  int size = sizeof(local); getsockname(closed, reinterpret_cast<sockaddr*>(&local), &size);
  const uint16_t port = ntohs(local.sin_port);
  closesocket(closed);
  auto gate = std::make_shared<UdpPathGate>();
  UdpMediaLeg leg(grantFor({port, host.port}), gate, 0);
  waitFor([&] { return leg.Read().report.code != 0; }, 11000, "The next relay answered");
  const auto reading = leg.Read();
  require(reading.report.code == udp::probeReachable && reading.report.relay == 1 && reading.running, "An ICMP reset from a closed port broke the socket");
}

void lateHost() {
  FakeHost host;
  host.firstPingsDropped = 3;
  UdpMediaLeg leg(grantFor({host.port}), std::make_shared<UdpPathGate>(), 0);
  waitFor([&] { return leg.Read().report.code != 0; }, 9000, "The burst");
  const auto reading = leg.Read();
  require(reading.report.code == udp::probeReachable && reading.report.lossPct == 0 && host.pings.load() == 3 + 1 + 25, "A Host that binds late cost loss");
}

void failures() {
  { FakeHost host(FakeHost::Mode::silent);
    UdpMediaLeg leg(grantFor({host.port}), std::make_shared<UdpPathGate>(), 0);
    waitFor([&] { return !leg.Read().running; }, 7000, "The BIND budget");
    require(leg.Read().report.code == udp::probeBindTimeout, "4 from a silent relay"); }
  { UdpMediaLeg leg(grantFor({9}), std::make_shared<UdpPathGate>(), 0, [](const std::string&, uint32_t&) { return false; });
    waitFor([&] { return !leg.Read().running; }, 7000, "The BIND budget");
    require(leg.Read().report.code == udp::probeLookupFailed, "3 from a failed name"); }
  { FakeHost host(FakeHost::Mode::ackOnly);
    UdpMediaLeg leg(grantFor({host.port}), std::make_shared<UdpPathGate>(), 0);
    waitFor([&] { return leg.Read().report.code != 0; }, 5000, "No PONG");
    const auto reading = leg.Read();
    require(reading.report.code == udp::probeNoPong && reading.report.lossPct == 100 && reading.running, "5, and keepalives go on"); }
}

void teardown() {
  const auto timed = [](const std::function<void()>& action, const char* what) {
    const auto began = std::chrono::steady_clock::now();
    action();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    require(ms < 500, std::string(what) + " took " + std::to_string(ms) + " ms");
  };
  // A name lookup that never returns stays behind.
  { auto release = std::make_shared<std::atomic<bool>>(false);
    auto leg = std::make_unique<UdpMediaLeg>(grantFor({9}), std::make_shared<UdpPathGate>(), 0, [release](const std::string&, uint32_t&) { while (!*release) std::this_thread::sleep_for(std::chrono::milliseconds(5)); return false; });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    timed([&] { leg.reset(); }, "Teardown over a stuck lookup");
    *release = true; }
  // During BIND, the burst, and a repeat stop.
  { FakeHost host(FakeHost::Mode::silent);
    UdpMediaLeg leg(grantFor({host.port}), std::make_shared<UdpPathGate>(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    timed([&] { leg.Stop(); leg.Stop(); }, "Stop while binding"); }
  { FakeHost host;
    UdpMediaLeg leg(grantFor({host.port}), std::make_shared<UdpPathGate>(), 0);
    waitFor([&] { return host.pings.load() > 2; }, 3000, "The burst");
    timed([&] { leg.Stop(); }, "Stop in the burst");
    require(!leg.Read().running, "A stopped leg reads as running"); }
  // During media: detach, then stop the engine, with frames still arriving.
  { FakeHost host;
    Session session(host, 1, false);
    std::atomic<bool> go{true};
    std::thread feed([&] { uint64_t pts = 1; while (go) { host.Video(frame(pts % 30 == 1, 1, pts, 3000), pts % 30 == 1); host.Audio(media(2, false, 1, pts, 1920)); ++pts; std::this_thread::sleep_for(std::chrono::milliseconds(1)); } });
    waitFor([&] { return session.engine.count() > 20; }, 3000, "Media flowing");
    session.gate->Attach({});
    { std::lock_guard<std::mutex> lock(session.engine.mutex); session.engine.stopped = true; }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(session.engine.afterStop == 0, "An offer reached a stopped engine");
    session.gate->Attach(session.engine.offer());  // the leg outlives a decoder restart
    { std::lock_guard<std::mutex> lock(session.engine.mutex); session.engine.stopped = false; }
    timed([&] { session.leg->Stop(); }, "Stop during media");
    go = false; feed.join();
    session.leg.reset(); }
  // A Host silent from the start of media: the leg just keeps its timer.
  { FakeHost host;
    Session session(host);
    host.Post([&host] { host.dropUpPerMille = 1000; });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require(session.Latest().running, "Silence stopped the leg");
    timed([&] { session.leg->Stop(); }, "Stop with a silent Host"); }
}

using Case = std::pair<const char*, void (*)()>;
int run(const std::vector<Case>& cases) {
  std::mutex mutex;
  int failed = 0;
  std::vector<std::thread> threads;
  for (const auto& entry : cases) threads.emplace_back([&, entry] {
    try { entry.second(); } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(mutex);
      ++failed; std::cerr << "FAILED " << entry.first << ": " << error.what() << std::endl;
    }
  });
  for (auto& thread : threads) thread.join();
  return failed;
}
}

int main() {
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
  int failed = 0;
  // Pure first, one after the other; the sockets run side by side, each burst takes 6 s.
  for (const auto& entry : std::vector<Case>{{"probe bytes", probeBytesMatchTheRelay}, {"bind", bindSchedule}, {"failure codes", failureCodes}, {"ping", pingSchedule}, {"waiting", waitingForTheHost}, {"path rule", pathRule}}) failed += run({entry});
  const int pure = checks;
  failed += run({{"media with loss", mediaWithLoss}, {"idle feedback", idleFeedbackAndBind}, {"paths", pathsOverSockets}, {"hostile datagrams", hostileDatagrams}, {"announced epochs", announcedEpochs}, {"full queue", fullQueue}, {"refused switch key", refusedSwitchKey}, {"given up", givenUp}, {"given up, recovered", givenUpRecovered}, {"phase 2 only", phase2Only}, {"closed relay", firstRelayClosed}, {"late Host", lateHost}, {"failures", failures}, {"teardown", teardown}});
  WSACleanup();
  std::cout << "{\"kind\":\"udp-media-leg-test\",\"pureChecks\":" << pure << ",\"loopbackChecks\":" << checks - pure << ",\"failed\":" << failed << "," << lossSummary << "\"realNetwork\":false,\"realHost\":false,\"realRelay\":false}" << std::endl;
  return failed ? 1 : 0;
}
