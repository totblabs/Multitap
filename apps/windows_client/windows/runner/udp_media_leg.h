#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "udp_path_gate.h"
#include "udp_probe_core.h"
#include "../../../../native/windows_video/udp_direct.hpp"

namespace multitap {
// udp-media-v1 on the client: one thread owns the socket from the first BIND, because
// the relay pins that socket's address. It runs the probe, the key agreement and the
// core Receiver, and hands whole messages to the gate. Nothing here can end a session:
// when this leg stops or fails, the Host hears silence and goes back to WSS.
class UdpMediaLeg {
 public:
  struct Grant {
    std::vector<std::pair<std::string, uint16_t>> relays;
    std::array<uint8_t, 8> id{};
    std::array<uint8_t, 32> ticket{};  // a secret: never logged
    std::string sessionId;
    bool direct = false;
    std::vector<std::pair<std::string,uint16_t>> stun;
  };
  struct Reading {
    udp::Probe::Report report;
    bool running = true, media = false;
    std::optional<std::array<uint8_t, 32>> publicKey;
    // Since the last reading, delivered to the decoder: pts, arrival us, bytes.
    std::vector<int64_t> arrivals;
    int64_t audioBytes = 0;
    uint32_t gaps = 0;  // datagrams given up as lost, this leg's lifetime
    // This leg's lifetime: video messages that found the decode queue full and waited,
    // skips to the next key frame (refused, or the queue stayed full), and KEYFRAME and
    // NACK datagrams sent.
    uint32_t queueWaits = 0, queueSkips = 0, keyRequests = 0, nacks = 0;
    UdpPathGate::View path;
    std::optional<std::pair<std::string,uint16_t>> local;
    std::optional<std::pair<std::string,uint16_t>> reflexive;
    int directPath = 0, directRttMs = -1, relayRttMs = -1;
  };
  // Resolves an IPv4 name; it may never return, so it runs off the leg's thread. Tests only.
  using Lookup = std::function<bool(const std::string& host, uint32_t& ipv4)>;

  // `clockUs`: the session clock now, so arrivals read on the WSS path's time base.
  // `grant`'s ticket is wiped here: the leg holds the only copy, until the proofs exist.
  UdpMediaLeg(Grant&& grant, std::shared_ptr<UdpPathGate> gate, int64_t clockUs, Lookup lookup = {});
  ~UdpMediaLeg();
  UdpMediaLeg(const UdpMediaLeg&) = delete;
  UdpMediaLeg& operator=(const UdpMediaLeg&) = delete;

  void HostKey(const std::array<uint8_t, 32>& key);
  void Peer(const std::vector<std::pair<std::string,uint16_t>>& candidates);
  void Input(uint32_t sequence,uint16_t epoch,std::vector<uint8_t> bytes);
  Reading Read();
  // Repeatable, and bounded by one pass of the loop: a name lookup is left behind.
  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
