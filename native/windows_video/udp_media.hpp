#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

// UDP media core (docs/remote-session-protocol.md, "UDP media"), shared
// by the Host and the Windows client. Pure and clock-injected: no I/O, no threads,
// no clock, no crypto (udp_media_crypto.hpp seals what this produces). Not thread
// safe. Custom framing: it needs independent review before it carries real media.
namespace multitap::udp {
constexpr size_t maxDatagram = 1200, headerBytes = 17, tagBytes = 16, maxPlaintext = maxDatagram - headerBytes - tagBytes;
constexpr size_t fragmentHeaderBytes = 10, fragmentPayload = maxPlaintext - fragmentHeaderBytes;
constexpr size_t maxFragments = 2048, maxMessageBytes = maxFragments * fragmentPayload;
constexpr uint8_t kindFragment = 1, kindFeedback = 2, kindNack = 3, kindKeyframe = 4, kindTail = 5, kindLoss = 6;
// classRecovery: video that answers a LOSS, coded only from frames at or before its last good
// PTS (or an IDR). The Host sends it only to a client that sent LOSS, so released clients,
// which drop the class, never see it.
constexpr uint8_t classVideo = 0, classAudio = 1, classRecovery = 2;
// LOSS, client to Host: [1] 0; [2..3] epoch; [4..7] the message ID given up; [8..15] PTS of the
// newest video frame the decoder took. Every frame after that PTS is lost to the decoder. It
// goes before a KEYFRAME: released Hosts ignore it and answer that with an IDR, newer Hosts
// answer it with a recovery frame and merge the KEYFRAME.
constexpr size_t feedbackBytes = 34, keyframeBytes = 4, tailBytes = 26, lossBytes = 16, nackEntryBytes = 8, maxNackEntries = 64;
constexpr uint64_t feedbackIntervalUs = 100000, keyframeIntervalUs = 500000, assumedRttUs = 50000;
constexpr uint64_t minimumNackDelayUs = 10000, minimumNackRepeatUs = 20000;
constexpr unsigned maxNackRepeats = 3;
constexpr uint64_t giveUpSlackUs = 20000, minimumGiveUpUs = 60000, maximumGiveUpUs = 600000, resendKeepUs = 600000;
// A LOSS left without an answer is asked again this soon, of a Host that answers one with a
// recovery frame: three round trips and the Host's time to code a frame, never under the
// minimum. Then once more a quarter second on, then every keyframeIntervalUs as before.
constexpr uint64_t answerSlackUs = 60000, minimumAskAgainUs = 100000;
constexpr uint64_t tailFirstUs = 5000, tailSecondUs = 20000, feedbackSilenceUs = 1000000;
// A challenge is new at least every second: the next TAIL after this long replaces it.
constexpr uint64_t proofTailUs = 200000, challengeRotateUs = 800000;
constexpr uint64_t probeBackoffFirstUs = 30000000, probeBackoffMaximumUs = 300000000;
// The pacer spreads a frame over its send time, which input waits out: at 1.25 times
// the target a 40 KB frame at 20 Mbps took 13 ms (measured). The queue limit
// keeps the slower rate, so congestion does not build a longer queue. The burst lets one
// average frame at the target (rate x 3 x 6 ms = 18 ms of target, one frame at 60 fps)
// leave at once, as WSS sends it: with 2 ms, UDP input-to-video p50 was 15.7 ms against
// WSS 14.0 ms in the same load (measured); a key frame is still paced.
constexpr double pacerFactor = 3, queueFactor = 1.25, repairShare = 0.25;
// Memory bounds, the contract's own limits aside. Receiver: messages behind the
// next one to deliver, their bytes, recorded gaps, and NACK datagrams per tick.
constexpr size_t videoWindow = 64, maxVideoBytes = 16 * 1024 * 1024, maxGaps = 4096, maxNacksPerTick = 8;
// Audio: unfinished messages, and fragments of one (96 ms of PCM, real chunks take 2).
constexpr size_t audioPartials = 8, maxAudioFragments = 16;
// Sender: audio waiting, repairs waiting, sent video kept, and how much pacing
// time of P-frames may queue (a key frame replaces what waits, so one always fits).
constexpr size_t maxAudioQueue = 64, maxRepairs = 2048, maxKeptBytes = 16 * 1024 * 1024;
// Sound is not asked for again: the answer would come a round trip after the next packet
// showed the loss, later than the 40 ms a client plays from. Instead a sound message of one
// datagram that the caller marks goes out again, unchanged and under its own message ID,
// right before each of the next audioRepeats ones (so in order: a client that lost it gets
// it one packet late, before the packet that would have shown the loss), unless it is older
// than audioRepeatUs by then (the sound paused). Field 2026-11-24, a game over Wi-Fi: 0.3 to
// 1.6 % of the sound packets never came, as of the video datagrams, which NACK repaired.
// Only a message whose receiver plays a number once may be marked (Opus: every client drops
// a number it had; PCM has no numbers and would play it twice).
constexpr size_t audioRepeats = 2;
constexpr uint64_t audioRepeatUs = 100000;
constexpr uint64_t queuePacingUs = 250000, repairWindowUs = 100000, pacerBurstUs = 6000;
// The wire pace, under the pacer: however much credit there is, at most wireClump datagrams
// leave back to back, and the next ones wireClumpUs later (768 Mbps; `next` returns false in
// between, and the caller comes back that soon while `waiting`). A PC sends a burst as fast as
// its network card goes, and where the path steps down to a slower link the burst waits in
// that device's queue. Field 2026-11-26, a game at 20 to 43 Mbps, three 45 s captures where
// the stream reached the relay: of the pictures of up to 100 datagrams nothing was lost; of
// those of 100 to 119, 1.1 to 1.4 %; 120 to 139, 6 to 7 %; 140 to 159, 10 to 14 %; over 160,
// 17 to 19 % (2.2 to 3.9 % of all). The first loss came 102 to 106 datagrams into a burst,
// then two of five arrived, 12 us apart: a 2.5 Gbps sender into a 1 Gbps link behind a queue
// of about 60 packets. The IP IDs counted on through the gaps, so the PC had sent them. A
// clump of 40 fills 24 places of such a queue, and it has emptied before the next one; it
// is also one average frame at 20 Mbps and 60 fps, which still leaves at once.
constexpr size_t wireClump = 40;
constexpr uint64_t wireClumpUs = 500;
constexpr double wireClumpBytes = double(wireClump * maxDatagram), wireBytesPerUs = wireClumpBytes / double(wireClumpUs);
// parity-v1. A lost video fragment is asked for again (NACK), and its frame then reaches the
// decoder a NACK delay and a round trip late: about 20 ms on a LAN, two or three pictures at
// 120 fps. Field 2026-11-25, a game over Wi-Fi: 2.7 such repairs a second with 0.04 % of the
// datagrams lost. So a video message may be followed by parity datagrams, from which the
// client rebuilds a lost fragment at once.
//  * A message of `count` fragments has parityCount(count) parities. Parity `slot` is the XOR
//    of the fragments whose index is `slot` modulo that number, each padded with zeros to a
//    full payload: any one lost fragment of a class comes back, so a run of up to that many
//    lost in a row does.
//  * PARITY, Host to client (kindParity): [1] the message's class in the low four bits and
//    the number of parities less one in the high four; [2..5] the message ID; [6..7] the slot
//    in bits 11-14 and the size of the message's last fragment in bits 0-10; [8..9] the
//    fragment count; then fragmentPayload bytes.
//  * ABILITIES, client to Host (kindAbilities): [1] 0; [2..3] bits, abilityParity = it takes
//    PARITY; [4..7] the lost fragments parity has repaired for it so far: rebuilt ones whose
//    own datagram did not come within repairedAfterUs either (a parity that overtakes its
//    fragment on the way rebuilds it too, and that is no loss). It goes once a second.
//    A Host that does not know it ignores it, and a Host sends PARITY only to a client that
//    sent it, so released clients, which drop the kind, never see one.
//  * Parity costs about a sixteenth of the video rate, and it repairs single losses only: on
//    a path that drops runs of datagrams (a full queue, a radio pause) it is load and nothing
//    else (simulation, a link just above the video rate: late time 33 s -> 82 s with parity
//    always on; closed loop, a 25 Mbps link with an 8 ms queue: 97.8 % -> 93.9 % of the
//    frames delivered). So the Host sends it only where it works. A NACK starts a trial of
//    parityTrialUs, at most one in parityHoldUs; each ABILITIES whose count rose keeps parity
//    on for parityHoldUs from then. A KEYFRAME or a LOSS (the client gave a frame up: what
//    neither NACK nor parity brought back is a full queue, a policer or a blackout) ends
//    parity at once, and none starts for parityBackoffUs. So does a target the rate
//    controller lowers by more than parityCutShare (the path carries less), and so does a
//    second in which more than parityLossShare of the datagrams sent did not arrive (single
//    losses are rarer than that; a full queue is not).
constexpr uint8_t kindParity = 7, kindAbilities = 8;
constexpr size_t abilitiesBytes = 8, maxParities = 16;
constexpr uint16_t abilityParity = 1;
constexpr uint64_t abilitiesIntervalUs = 1000000, parityHoldUs = 60000000, parityTrialUs = 10000000, parityBackoffUs = 15000000, repairedAfterUs = 50000;
constexpr double parityCutShare = 0.03, parityLossShare = 0.04;
constexpr uint64_t parityLossWindowUs = 1000000, parityLossDatagrams = 200;
constexpr size_t maxRebuiltWatched = 256;
// quick-nack-v1. A NACK waited minimumNackDelayUs (10 ms) for a datagram that might only be out
// of order, so a repaired frame reached the decoder that wait and a round trip late: 18 ms at a
// 7 ms round trip, three or four pictures at 180 fps. Field 2026-11-25, a game over Wi-Fi with
// parity on: in two minutes 44 NACKs went and parity rebuilt two fragments; what that path
// loses comes in runs longer than a message's parities, which only a repair brings back.
// Each datagram carries the sealer's sequence, one per datagram and a new one for a repair, so
// the client sees exactly what came out of order and how late (SequenceTrack). Once that is
// known the NACK waits half again the longest lateness of the last windows and a millisecond,
// at least quickNackDelayUs and never longer than before. A path that reorders nothing asks
// after 2 ms; one that does keeps its wait.
constexpr uint64_t quickNackDelayUs = 2000, sequenceWindowUs = 15000000, sequenceLateUnknownUs = 100000;
constexpr uint32_t sequenceKnownAfter = 500;
constexpr size_t sequenceJumps = 32, sequenceRunBuckets = 6;
// FEEDBACK that shows none of this many sent datagrams arrived proves nothing.
constexpr uint64_t unconfirmedLimit = 16;

inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) << 32 | get32(p + 4); }
inline void put16(uint8_t* p, uint16_t value) { p[0] = uint8_t(value >> 8); p[1] = uint8_t(value); }
inline void put32(uint8_t* p, uint32_t value) { put16(p, uint16_t(value >> 16)); put16(p + 2, uint16_t(value)); }
inline void put64(uint8_t* p, uint64_t value) { put32(p, uint32_t(value >> 32)); put32(p + 4, uint32_t(value)); }

struct Plain { std::array<uint8_t, maxPlaintext> bytes; size_t size = 0; };

// The 12-byte header of a single-output WSS media message (mediaPacket).
struct Envelope { bool key = false; uint16_t epoch = 0; uint64_t pts = 0; };
inline bool parseEnvelope(const uint8_t* message, size_t size, uint8_t cls, Envelope& result) {
  if (size < 13 || message[0] != (cls == classVideo ? 1 : 2) || (message[1] & (cls == classVideo ? ~1 : ~0))) return false;
  result.key = (message[1] & 1) != 0; result.epoch = uint16_t(message[2] | message[3] << 8); result.pts = 0;
  for (unsigned n = 0; n < 8; ++n) result.pts |= uint64_t(message[4 + n]) << (8 * n);
  return true;
}

inline size_t fragmentCount(size_t messageBytes) {
  return messageBytes && messageBytes <= maxMessageBytes ? (messageBytes + fragmentPayload - 1) / fragmentPayload : 0;
}
inline void writeFragment(Plain& out, uint8_t cls, uint32_t id, const std::vector<uint8_t>& message, uint16_t index) {
  const size_t count = fragmentCount(message.size()), at = size_t(index) * fragmentPayload;
  const size_t size = std::min(fragmentPayload, message.size() - at);
  out.bytes[0] = kindFragment; out.bytes[1] = cls; put32(&out.bytes[2], id); put16(&out.bytes[6], index); put16(&out.bytes[8], uint16_t(count));
  std::memcpy(&out.bytes[fragmentHeaderBytes], message.data() + at, size); out.size = fragmentHeaderBytes + size;
}
inline unsigned parityCount(size_t fragments) { return fragments ? unsigned(std::min<size_t>((fragments + 15) / 16, maxParities)) : 0; }
inline void writeParity(Plain& out, uint8_t cls, uint32_t id, const std::vector<uint8_t>& message, unsigned slot, unsigned parities) {
  const size_t count = fragmentCount(message.size()), lastSize = message.size() - (count - 1) * fragmentPayload;
  out.bytes[0] = kindParity; out.bytes[1] = uint8_t(cls | (parities - 1) << 4); put32(&out.bytes[2], id);
  put16(&out.bytes[6], uint16_t(slot << 11 | lastSize)); put16(&out.bytes[8], uint16_t(count));
  uint8_t* mixed = &out.bytes[fragmentHeaderBytes]; std::memset(mixed, 0, fragmentPayload);
  for (size_t index = slot; index < count; index += parities) {
    const size_t at = index * fragmentPayload, size = std::min(fragmentPayload, message.size() - at);
    for (size_t n = 0; n < size; ++n) mixed[n] ^= message[at + n];
  }
  out.size = fragmentHeaderBytes + fragmentPayload;
}
struct Parity { uint8_t cls = 0; uint32_t id = 0; uint16_t count = 0, slot = 0, parities = 0, lastSize = 0; const uint8_t* data = nullptr; };
inline bool parseParity(const uint8_t* plain, size_t size, Parity& p) {
  if (size != fragmentHeaderBytes + fragmentPayload || plain[0] != kindParity) return false;
  const uint16_t packed = get16(plain + 6);
  p = {uint8_t(plain[1] & 15), get32(plain + 2), get16(plain + 8), uint16_t(packed >> 11), uint16_t((plain[1] >> 4) + 1), uint16_t(packed & 2047), plain + fragmentHeaderBytes};
  return (p.cls == classVideo || p.cls == classRecovery) && p.id && p.count && p.count <= maxFragments && p.parities == parityCount(p.count) && p.slot < p.parities
      && p.lastSize && p.lastSize <= fragmentPayload;
}
struct Fragment { uint8_t cls = 0; uint32_t id = 0; uint16_t index = 0, count = 0; const uint8_t* data = nullptr; size_t size = 0; };
// Every fragment but the last carries exactly fragmentPayload bytes: a slice's
// place follows from its index. Message IDs start at 1.
inline bool parseFragment(const uint8_t* plain, size_t size, Fragment& f) {
  if (size <= fragmentHeaderBytes || size > maxPlaintext || plain[0] != kindFragment || plain[1] > classRecovery) return false;
  f = {plain[1], get32(plain + 2), get16(plain + 6), get16(plain + 8), plain + fragmentHeaderBytes, size - fragmentHeaderBytes};
  return f.id && f.count && f.count <= maxFragments && f.index < f.count && (f.index + 1u == f.count || f.size == fragmentPayload);
}

// A message being rebuilt; its bytes end up identical to the input.
class Partial {
 public:
  bool started() const { return count_ != 0; }
  bool whole() const { return count_ && have_ == count_; }
  uint16_t count() const { return count_; }
  size_t missing() const { return size_t(count_) - have_; }
  bool has(uint16_t index) const { return got_[index]; }
  size_t reserved() const { return size_t(count_) * fragmentPayload; }
  const std::vector<uint8_t>& bytes() const { return bytes_; }
  void start(uint16_t count) { count_ = count; have_ = 0; got_.assign(count, false); bytes_.resize(reserved()); }
  bool add(const Fragment& f) {
    if (got_[f.index]) return false;
    got_[f.index] = true; ++have_;
    std::memcpy(bytes_.data() + size_t(f.index) * fragmentPayload, f.data, f.size);
    if (f.index + 1u == count_) size_ = size_t(f.index) * fragmentPayload + f.size;
    return true;
  }
  // XORs fragment `index` as the Host mixed it (padded with zeros to a full payload) into `to`.
  void mix(uint16_t index, uint8_t* to) const { const uint8_t* from = bytes_.data() + size_t(index) * fragmentPayload; for (size_t n = 0; n < fragmentPayload; ++n) to[n] ^= from[n]; }
  std::vector<uint8_t> take() { bytes_.resize(size_); return std::move(bytes_); }
  void release() { count_ = have_ = 0; size_ = 0; got_ = {}; bytes_ = {}; }
 private:
  uint16_t count_ = 0, have_ = 0;
  size_t size_ = 0;
  std::vector<bool> got_;
  std::vector<uint8_t> bytes_;
};

// What the path did to the datagrams, from their sequences (quick-nack-v1). Pure; the caller
// passes each authenticated datagram's sequence as it arrives, repairs and all.
class SequenceTrack {
 public:
  // runs: stretches of sequences that were skipped, by length: 1, 2, 3-4, 5-16, 17-64, more.
  // late: datagrams that came after a later one (each was counted in a run before), and the
  // longest any came after the datagram that skipped it.
  struct Counts { uint32_t runs[sequenceRunBuckets] = {}; uint32_t late = 0; uint64_t lateMaxUs = 0; };
  static size_t bucket(uint64_t length) { return length <= 1 ? 0 : length == 2 ? 1 : length <= 4 ? 2 : length <= 16 ? 3 : length <= 64 ? 4 : 5; }
  // Returns how late this datagram came after one sent later (0: in order).
  uint64_t arrived(uint64_t sequence, uint64_t nowUs) {
    roll(nowUs);
    if (!highest_) { highest_ = sequence; return 0; }
    if (sequence > highest_) {
      if (const uint64_t missing = sequence - highest_ - 1) { ++counts_.runs[bucket(missing)]; jumps_[jumpAt_++ % jumps_.size()] = {highest_ + 1, sequence - 1, nowUs}; }
      highest_ = sequence; if (inOrder_ < UINT32_MAX) ++inOrder_;
      return 0;
    }
    // The jump that skipped it is forgotten: later than anything a NACK should wait for.
    uint64_t late = sequenceLateUnknownUs;
    for (const auto& jump : jumps_) if (sequence >= jump.first && sequence <= jump.last) { late = std::max<uint64_t>(nowUs - jump.at, 1); break; }
    ++counts_.late; counts_.lateMaxUs = std::max(counts_.lateMaxUs, late); lateNow_ = std::max(lateNow_, late);
    return late;
  }
  // Enough came in order to say what this path reorders.
  bool known() const { return inOrder_ >= sequenceKnownAfter; }
  // The longest lateness of this window and the one before.
  uint64_t lateUs(uint64_t nowUs) { roll(nowUs); return std::max(lateNow_, lateBefore_); }
  const Counts& counts() const { return counts_; }
 private:
  struct Jump { uint64_t first = 1, last = 0, at = 0; };
  void roll(uint64_t nowUs) {
    if (nowUs - windowAt_ < sequenceWindowUs) return;
    lateBefore_ = nowUs - windowAt_ < 2 * sequenceWindowUs ? lateNow_ : 0; lateNow_ = 0; windowAt_ = nowUs;
  }
  uint64_t highest_ = 0, windowAt_ = 0, lateNow_ = 0, lateBefore_ = 0; uint32_t inOrder_ = 0;
  std::array<Jump, sequenceJumps> jumps_{}; size_t jumpAt_ = 0;
  Counts counts_;
};

// Client side. The caller opens each datagram and passes the plaintext; `tick`
// runs the timers, every few milliseconds whether or not media arrives. It gets
// messages to hand to the decoder and control plaintexts to seal and send. Its RTT
// is the Host's, carried by TAIL. Malformed input only ever drops data.
class Receiver {
 public:
  struct Message { bool video = false, key = false; uint16_t epoch = 0; uint64_t pts = 0; std::vector<uint8_t> bytes; };
  struct Output { std::vector<Message> messages; std::vector<std::vector<uint8_t>> control; };

  // `parity`: it tells the Host that it takes parity (tests of a client that does not).
  Receiver(uint16_t epoch, uint64_t nowUs, bool parity = true) : epoch_(epoch), feedbackAt_(nowUs + feedbackIntervalUs), abilitiesAt_(parity ? nowUs : UINT64_MAX) {}
  // Fragments rebuilt from parity, those of them that were lost (see ABILITIES), and parity datagrams that came.
  uint32_t rebuilt() const { return rebuilt_; }
  uint32_t repaired() const { return repaired_; }
  uint32_t parities() const { return parities_; }
  uint64_t rttUs() const { return rtt_; }
  // quick-nack-v1: the longest this path delivered a datagram out of order lately
  // (SequenceTrack::lateUs), once the caller knows it. Until then a NACK waits as before.
  void setReorder(uint64_t lateUs) { reorderKnown_ = true; reorderUs_ = lateUs; }
  uint64_t nackDelayUs() const { return nackDelay(); }
  // Announced on WSS. What carries another epoch is dropped, and no frame of it is known good.
  void setEpoch(uint16_t epoch) { if (epoch != epoch_) { haveLast_ = lossWait_ = false; } epoch_ = epoch; }

  void datagram(const uint8_t* plain, size_t size, uint64_t nowUs, Output& out) {
    Fragment f; Parity p;
    if (parseFragment(plain, size, f)) { ++received_; if (f.cls == classAudio) audio(f, out); else video(f, nowUs, out); }
    else if (parseParity(plain, size, p)) { ++received_; ++parities_; parity(p, nowUs, out); }
    else if (size == tailBytes && plain[0] == kindTail && !plain[1]) {
      ++received_; challenge_ = get64(plain + 14);
      if (const uint32_t rtt = get32(plain + 22)) rtt_ = rtt;
      tail(get32(plain + 2), get64(plain + 6), nowUs, out);
    }
  }

  void tick(uint64_t nowUs, Output& out) {
    const uint64_t limit = giveUpDelay();
    while (!gaps_.empty()) {
      const Gap gap = gaps_.front();
      if (filled(gap)) { gaps_.pop_front(); continue; }
      if (nowUs - gap.at < limit) break;
      giveUp(gap.id, nowUs);
    }
    drain(out);
    nack(nowUs, out);
    // The interval keeps a second request from killing an answer still on its way. Once that
    // answer is given up, nothing is on its way: field 2026-11-20 (a game, RTT 7 ms) froze
    // 0.6 s on each lost recovery frame, a give-up plus the rest of the interval.
    if (std::exchange(answerLost_, false) && awaitKey_) keyAt_ = std::min(keyAt_, nowUs);
    if (awaitKey_ && nowUs >= keyAt_ && !keyArriving()) {
      if (lossWait_) {
        std::vector<uint8_t> loss(lossBytes); loss[0] = kindLoss; put16(&loss[2], epoch_); put32(&loss[4], lossId_); put64(&loss[8], lastPts_);
        out.control.push_back(std::move(loss));
      }
      std::vector<uint8_t> plain(keyframeBytes); plain[0] = kindKeyframe; put16(&plain[2], epoch_);
      out.control.push_back(std::move(plain));
      // The request itself can be lost, and then nothing is on its way. Field 2026-11-24 (a
      // game through a relay that dropped datagrams in clumps): of 87 requests the Host acted
      // on 80, and each of the others froze the picture 0.58 s, the whole interval, until the
      // next one. So the first repeat of a loss wait need only outlast an answer's way here.
      // The Host merges a request that comes while its answer is due or on its way
      // (Sender::keyInFlight), whether that answer is a recovery frame or an IDR, and an
      // answer that shows here holds the repeat (keyArriving). Until 2026-11-25 only a Host
      // that had already sent a recovery frame was asked again sooner; but the first LOSS an
      // encoder gets is always answered with an IDR, so a session with few losses never
      // got there (field: one lost request in a game, 0.6 s). A plain key wait keeps the
      // interval.
      fullAt_ = nowUs + keyframeIntervalUs;
      keyAt_ = nowUs + (lossWait_ ? askAgainDelay() : keyframeIntervalUs);
      if (asked_ < 2) ++asked_;
    }
    if (nowUs >= feedbackAt_) { feedback(nowUs, out); feedbackAt_ = nowUs + feedbackIntervalUs; }
    while (!made_.empty() && nowUs - made_.front().at >= repairedAfterUs) { made_.pop_front(); ++repaired_; }
    if (nowUs >= abilitiesAt_) {
      std::vector<uint8_t> plain(abilitiesBytes); plain[0] = kindAbilities; put16(&plain[2], abilityParity); put32(&plain[4], repaired_);
      out.control.push_back(std::move(plain)); abilitiesAt_ = nowUs + abilitiesIntervalUs;
    }
  }

  // A key frame took the other path: that path is active, and what was in flight
  // here is dead. Only the newest message may still finish, restarted empty,
  // because a switch back opens with a key frame that may just be arriving.
  void keyFrameOtherPath() {
    // The decoder now has WSS frames this path never saw: none of its PTS is the decoder's last.
    awaitKey_ = lossWait_ = haveLast_ = false; gaps_.clear();
    if (window_.empty()) return;
    abandonBefore(next_ + window_.size() - 1);
    held_ -= window_.front().reserved(); window_.front() = Entry{}; closed_ = next_ - 1;
  }
  // The decoder queue refused a message: skip to the next key frame. A decoder that
  // refuses the key frame it asked for gets the next one a key interval later, not at once.
  // What was delivered may never reach the decoder, so only a key frame ends this wait.
  void queueFull(uint64_t nowUs) { if (!awaitKey_) { awaitKey_ = true; asked_ = 0; keyAt_ = std::max(fullAt_, nowUs); } lossWait_ = false; }

  bool awaitingKey() const { return awaitKey_; }
  // The wait a recovery frame may end (a LOSS goes with each KEYFRAME).
  bool awaitingRecovery() const { return awaitKey_ && lossWait_; }
  size_t heldBytes() const { size_t bytes = held_; for (const auto& a : audio_) bytes += a.reserved(); return bytes; }
  size_t gaps() const { return gaps_.size(); }
  size_t windowMessages() const { return window_.size(); }

 private:
  // indices below frontier are passed. parity: the parities that came and are not used up, by slot.
  struct Entry : Partial { uint16_t frontier = 0; bool known = false, skip = false, recovery = false; Envelope envelope; uint16_t lastSize = 0; std::vector<std::vector<uint8_t>> parity; };
  // Oldest first. `whole`: no fragment of that message arrived, so its size is unknown.
  struct Gap { uint32_t id = 0; uint16_t index = 0; uint8_t nacks = 0; bool whole = false; uint64_t at = 0, due = 0; };
  struct Audio : Partial { uint32_t id = 0; };

  Entry* entry(uint64_t id) { return id >= next_ && id - next_ < window_.size() ? &window_[size_t(id - next_)] : nullptr; }
  bool filled(const Gap& gap) {
    const Entry* m = entry(gap.id);
    return !m || m->skip || m->whole() || (!gap.whole && m->has(gap.index));
  }
  // Counted from gap detection: a paced IDR can take longer than this with no loss at all.
  uint64_t giveUpDelay() const { return std::clamp<uint64_t>(rtt_ * 7 / 2 + giveUpSlackUs, minimumGiveUpUs, maximumGiveUpUs); }
  uint64_t nackDelay() const {
    const uint64_t slow = std::max(rtt_ / 4, minimumNackDelayUs);
    return reorderKnown_ ? std::clamp<uint64_t>(reorderUs_ ? reorderUs_ * 3 / 2 + 1000 : 0, quickNackDelayUs, slow) : slow;
  }
  // How long after a LOSS the next one goes while no answer shows: see tick.
  uint64_t askAgainDelay() const {
    const uint64_t first = std::clamp<uint64_t>(rtt_ * 3 + answerSlackUs, minimumAskAgainUs, keyframeIntervalUs);
    return asked_ == 0 ? first : asked_ == 1 ? std::max(first, keyframeIntervalUs / 2) : keyframeIntervalUs;
  }
  uint64_t nackRepeat() const { return std::max(rtt_ * 5 / 4, minimumNackRepeatUs); }
  void addGap(uint32_t id, uint16_t index, bool whole, uint64_t nowUs) { gaps_.push_back({id, index, 0, whole, nowUs, nowUs + nackDelay()}); }
  void dropUntilKey(uint64_t nowUs) { if (!awaitKey_) { awaitKey_ = true; asked_ = 0; keyAt_ = nowUs; } lossWait_ = false; }
  // Message `id` is lost to the decoder. With a frame delivered on this path this epoch, a
  // recovery frame may end the wait as well; a wait already running keeps its kind.
  void dropForLoss(uint64_t id, uint64_t nowUs) {
    if (awaitKey_) return;
    awaitKey_ = true; asked_ = 0; keyAt_ = nowUs; lossWait_ = haveLast_; lossId_ = uint32_t(id);
  }
  // Ends a wait: a key frame always, a recovery frame only the wait it may end.
  bool ends(const Entry& e) const { return e.known && !e.skip && (e.envelope.key || (lossWait_ && e.recovery)); }
  // What ends this wait once whole: a key frame, or a recovery frame the wait may take.
  bool answers(const Entry& e) const { return !e.skip && ((e.known && e.envelope.key) || (lossWait_ && e.recovery)); }
  void abandonBefore(uint64_t id) {
    while (next_ < id && !window_.empty()) {
      const Entry& m = window_.front();
      // The answer itself is lost: no other is coming, so the repeat need not wait out its interval.
      if (awaitKey_ && answers(m)) answerLost_ = true;
      if (!m.skip) lost_ += m.started() ? uint32_t(m.missing()) : 1;
      held_ -= m.reserved(); window_.pop_front(); ++next_;
    }
    next_ = std::max(next_, id); closed_ = std::max(closed_, next_ - 1);
  }
  void giveUp(uint64_t id, uint64_t nowUs) { dropForLoss(id, nowUs); abandonBefore(id + 1); }
  void deliver(Entry& m, Output& out) {
    if (m.envelope.epoch != epoch_) return;
    // drain lets a recovery frame through a wait only when it may end it.
    if (m.envelope.key || m.recovery) awaitKey_ = lossWait_ = false;
    // Whole and handed on: a whole frame that is dropped must not read as progress.
    highestWhole_ = uint32_t(next_);
    auto bytes = m.take();
    // Tiles (TSL1 after the 12-byte header) are no encoder frame: the Host knows no such PTS.
    if (!(bytes.size() >= 16 && bytes[12] == 'T' && bytes[13] == 'S' && bytes[14] == 'L' && bytes[15] == '1')) { lastPts_ = m.envelope.pts; haveLast_ = true; }
    out.messages.push_back({true, m.envelope.key, m.envelope.epoch, m.envelope.pts, std::move(bytes)});
  }
  // Video is delivered in order only.
  void drain(Output& out) {
    while (!window_.empty()) {
      Entry& m = window_.front();
      const bool useless = m.skip || (awaitKey_ && m.known && !ends(m));
      if (!useless && !m.whole()) return;
      if (!useless) deliver(m, out);
      held_ -= m.reserved(); window_.pop_front(); ++next_;
    }
  }
  // Everything through `last` went out in full: what is missing there is lost.
  void close(uint64_t last, uint64_t nowUs) {
    if (last < next_ || last <= closed_) return;
    if (last - next_ >= videoWindow) { dropForLoss(next_, nowUs); abandonBefore(last - videoWindow + 1); }
    while (next_ + window_.size() <= last) window_.emplace_back();
    for (uint64_t id = std::max(closed_ + 1, next_); id <= last; ++id) {
      Entry& m = window_[size_t(id - next_)];
      if (m.skip) continue;
      if (!m.started()) { addGap(uint32_t(id), 0, true, nowUs); continue; }
      for (unsigned i = m.frontier; i < m.count(); ++i) if (!m.has(uint16_t(i))) addGap(uint32_t(id), uint16_t(i), false, nowUs);
      m.frontier = m.count();
    }
    closed_ = last;
  }
  // What a key frame at `id` makes pointless: everything older, back to an older key
  // frame. That one may be nearly repaired, and dropping it for one just starting is
  // how IDRs cascade: every new request would kill the last answer.
  uint64_t oldestNeeded(uint64_t id) const {
    for (uint64_t at = id; at-- > next_;) { const Entry& e = window_[size_t(at - next_)]; if (ends(e) || (e.known && !e.skip && e.envelope.key)) return at; }
    return id;
  }
  // Another request now would only send a second IDR after this one. If this one
  // stalls, TAIL closes it, it is given up, and the repeat that was held goes at once.
  // A recovery frame shows by its class in any fragment, before its envelope.
  bool keyArriving() const {
    for (const auto& e : window_) if (answers(e)) return true;
    return false;
  }
  // Fragment 0 names the message. A message of no use stops costing memory and NACKs.
  // A P-frame stays even while waiting for a key: an older message may be that key,
  // and drain sorts it out.
  void inspect(uint32_t id, Entry& m, const Fragment& f, uint64_t nowUs) {
    const bool valid = parseEnvelope(f.data, f.size, classVideo, m.envelope);
    m.known = true;
    if (valid && m.envelope.epoch == epoch_) { if (ends(m)) abandonBefore(oldestNeeded(id)); return; }
    if (!valid || m.envelope.epoch > epoch_) dropUntilKey(nowUs);
    m.skip = true; held_ -= m.reserved(); m.release(); m.parity.clear();
  }
  // parity-v1: the one fragment still missing of a parity's class is that parity XOR the
  // others of the class. It is taken as if it had just arrived.
  void rebuild(uint32_t id, Entry& m, unsigned slot, uint64_t nowUs) {
    if (slot >= m.parity.size() || m.parity[slot].empty()) return;
    const unsigned step = unsigned(m.parity.size()); int missing = -1;
    for (unsigned index = slot; index < m.count(); index += step) if (!m.has(uint16_t(index))) { if (missing >= 0) return; missing = int(index); }
    std::vector<uint8_t> mixed = std::move(m.parity[slot]); m.parity[slot].clear();
    if (missing < 0) return;
    for (unsigned index = slot; index < m.count(); index += step) if (int(index) != missing) m.mix(uint16_t(index), mixed.data());
    Fragment f; f.cls = m.recovery ? classRecovery : classVideo; f.id = id; f.index = uint16_t(missing); f.count = m.count(); f.data = mixed.data();
    f.size = missing + 1 == int(m.count()) ? m.lastSize : fragmentPayload;
    if (!m.add(f)) return;
    ++rebuilt_;
    // Already asked for: it was lost, and the repair now on its way must not read as its own
    // datagram coming after all (a NACK may go before a long message's parity arrives).
    bool asked = false;
    for (const auto& gap : gaps_) if (gap.id == id && gap.nacks && (gap.whole ? gap.nacks <= maxNackRepeats : gap.index == f.index)) { asked = true; break; }
    if (asked) ++repaired_;
    else { made_.push_back({id, f.index, nowUs}); if (made_.size() > maxRebuiltWatched) { made_.pop_front(); ++repaired_; } }
    if (f.index == 0) inspect(id, m, f, nowUs);
  }
  // A parity datagram. Its message's fragments have all left the Host by now.
  void parity(const Parity& p, uint64_t nowUs, Output& out) {
    if (p.id < next_) return;
    if (p.id - next_ >= videoWindow) { dropForLoss(next_, nowUs); abandonBefore(p.id - videoWindow + 1); }
    close(uint64_t(p.id) - 1, nowUs);
    while (next_ + window_.size() <= p.id) window_.emplace_back();
    Entry& m = window_[size_t(p.id - next_)];
    if (m.skip) return;
    if (!m.started()) {
      if (held_ + size_t(p.count) * fragmentPayload > maxVideoBytes) return;
      m.start(p.count); held_ += m.reserved();
    }
    if (m.count() != p.count) return;
    m.recovery = m.recovery || p.cls == classRecovery;
    if (m.parity.empty()) m.parity.resize(p.parities);
    m.lastSize = p.lastSize;
    if (!m.whole() && m.parity[p.slot].empty()) m.parity[p.slot].assign(p.data, p.data + fragmentPayload);
    rebuild(p.id, m, p.slot, nowUs);
    if (m.skip) return drain(out);
    // What is still missing is lost; a later parity or a repair may bring it.
    for (unsigned i = m.frontier; i < m.count(); ++i) if (!m.has(uint16_t(i))) addGap(p.id, uint16_t(i), false, nowUs);
    m.frontier = m.count();
    limitGaps(nowUs); drain(out);
  }
  void video(const Fragment& f, uint64_t nowUs, Output& out) {
    // The fragment itself, after parity rebuilt it: it was not lost.
    for (auto it = made_.begin(); it != made_.end(); ++it) if (it->id == f.id && it->index == f.index) { made_.erase(it); break; }
    if (f.id < next_) return;
    if (f.id - next_ >= videoWindow) { dropForLoss(next_, nowUs); abandonBefore(f.id - videoWindow + 1); }
    close(uint64_t(f.id) - 1, nowUs);
    while (next_ + window_.size() <= f.id) window_.emplace_back();
    Entry& m = window_[size_t(f.id - next_)];
    if (m.skip) return;
    if (!m.started()) {
      if (held_ + size_t(f.count) * fragmentPayload > maxVideoBytes) return;  // stays a gap: deadline, then key frame
      m.start(f.count); held_ += m.reserved();
    }
    if (m.count() != f.count || !m.add(f)) return;
    m.recovery = m.recovery || f.cls == classRecovery;
    if (f.index == 0) { inspect(f.id, m, f, nowUs); if (m.skip) return drain(out); }
    // A fragment that came late may be the last one a waiting parity needed.
    if (!m.parity.empty()) { rebuild(f.id, m, unsigned(f.index % m.parity.size()), nowUs); if (m.skip) return drain(out); }
    const unsigned end = f.id <= closed_ ? m.count() : f.index;
    for (unsigned i = m.frontier; i < end; ++i) if (!m.has(uint16_t(i))) addGap(f.id, uint16_t(i), false, nowUs);
    m.frontier = uint16_t(std::max<unsigned>(m.frontier, end));
    limitGaps(nowUs); drain(out);
  }
  void limitGaps(uint64_t nowUs) {
    if (gaps_.size() <= maxGaps) return;
    gaps_.erase(std::remove_if(gaps_.begin(), gaps_.end(), [&](const Gap& gap) { return filled(gap); }), gaps_.end());
    while (gaps_.size() > maxGaps) { giveUp(gaps_.front().id, nowUs); gaps_.pop_front(); }
  }
  void tail(uint32_t last, uint64_t hostUs, uint64_t nowUs, Output& out) {
    echoUs_ = hostUs; echoAt_ = nowUs;
    close(last, nowUs); limitGaps(nowUs); drain(out);
  }
  // Audio is delivered as soon as it is whole and has no gaps: an unfinished
  // message just makes room for a newer one.
  void audio(const Fragment& f, Output& out) {
    if (f.count > maxAudioFragments) return;
    Audio* slot = nullptr;
    for (auto& a : audio_) if (a.started() && a.id == f.id) slot = &a;
    if (!slot) {
      slot = &audio_[0];
      for (auto& a : audio_) if (!a.started() || a.id < slot->id) { slot = &a; if (!a.started()) break; }
      if (slot->started()) lost_ += uint32_t(slot->missing());
      slot->release(); slot->start(f.count); slot->id = f.id;
    }
    if (slot->count() != f.count || !slot->add(f) || !slot->whole()) return;
    auto bytes = slot->take(); slot->release();
    Envelope e;
    if (parseEnvelope(bytes.data(), bytes.size(), classAudio, e) && e.epoch == epoch_) out.messages.push_back({false, false, e.epoch, e.pts, std::move(bytes)});
  }
  void nack(uint64_t nowUs, Output& out) {
    std::vector<uint8_t> plain(2); plain[0] = kindNack;
    uint32_t lastId = 0; unsigned lastEnd = 0, runAt = 0; size_t sent = 0;
    const auto flush = [&] { if (plain.size() > 2) { plain[1] = uint8_t((plain.size() - 2) / nackEntryBytes); out.control.push_back(plain); plain.resize(2); ++sent; } };
    for (auto& gap : gaps_) {
      if (gap.nacks > maxNackRepeats || gap.due > nowUs || filled(gap)) continue;
      if (gap.whole && entry(gap.id)->started()) { gap.nacks = maxNackRepeats + 1; continue; }  // its fragments have gaps of their own
      if (plain.size() > 2 && !gap.whole && gap.id == lastId && gap.index == lastEnd) put16(&plain[runAt], uint16_t(++lastEnd - get16(&plain[runAt - 2])));
      else {
        if (plain.size() == 2 + maxNackEntries * nackEntryBytes) { flush(); if (sent == maxNacksPerTick) return; }
        const size_t at = plain.size(); plain.resize(at + nackEntryBytes);
        // A wholly lost message: its size is unknown, the Host clamps the run.
        put32(&plain[at], gap.id); put16(&plain[at + 4], gap.index); put16(&plain[at + 6], gap.whole ? uint16_t(maxFragments) : 1);
        lastId = gap.id; lastEnd = gap.index + 1u; runAt = unsigned(at + 6);
      }
      ++gap.nacks; gap.due = nowUs + nackRepeat();
    }
    flush();
  }
  void feedback(uint64_t nowUs, Output& out) {
    std::vector<uint8_t> plain(feedbackBytes); plain[0] = kindFeedback;
    put32(&plain[2], highestWhole_); put32(&plain[6], received_); put32(&plain[10], lost_);
    if (echoUs_) { put64(&plain[14], echoUs_); put32(&plain[22], uint32_t(nowUs - echoAt_)); echoUs_ = 0; }
    put64(&plain[26], challenge_);
    out.control.push_back(std::move(plain));
  }

  uint64_t rtt_ = assumedRttUs, reorderUs_ = 0; bool reorderKnown_ = false;
  uint16_t epoch_;
  uint64_t feedbackAt_, abilitiesAt_, keyAt_ = 0, echoUs_ = 0, echoAt_ = 0, challenge_ = 0;
  uint32_t rebuilt_ = 0, repaired_ = 0, parities_ = 0;
  struct Made { uint32_t id = 0; uint16_t index = 0; uint64_t at = 0; };
  std::deque<Made> made_;  // rebuilt fragments whose own datagram may still come
  // fullAt_: a full decoder queue asks no sooner than this, across waits (queueFull).
  // asked_: requests sent in this wait, counted to two.
  uint64_t fullAt_ = 0; int asked_ = 0;
  uint64_t next_ = 1, closed_ = 0;  // next ID to deliver, and the last known sent in full
  // lossWait_: the wait a recovery frame may end; lastPts_ the newest video this path delivered.
  // answerLost_: an answer to this wait was given up since the last tick.
  bool awaitKey_ = false, lossWait_ = false, haveLast_ = false, answerLost_ = false;
  uint64_t lastPts_ = 0; uint32_t lossId_ = 0;
  size_t held_ = 0;
  uint32_t received_ = 0, lost_ = 0, highestWhole_ = 0;
  std::deque<Entry> window_;  // window_[0] is message next_
  std::deque<Gap> gaps_;
  std::array<Audio, audioPartials> audio_;
};

// Host side. It never handles a sealed datagram: `next` yields plaintext, and the
// sealer gives each one a new sequence, a retransmission included. Call `next`
// until it returns false, every millisecond or so; a return of false is the pacer.
//
// The relay checks no return route, so a ticket holder can spoof its source and
// aim this stream at someone else; holding the keys does not help, because the
// challenge goes to the pinned address. Until FEEDBACK echoes a challenge the Host
// sent, nothing but TAIL leaves here and only that FEEDBACK counts. `random` must be
// unpredictable (randomChallenge in udp_media_crypto.hpp); 0 never proves anything.
class Sender {
 public:
  struct Admission { bool queued = false, requestKey = false, tooLarge = false; };
  // loss: a LOSS to act on, with the message ID given up and the decoder's newest frame.
  struct Control { bool feedback = false, keyFrame = false, loss = false; uint16_t epoch = 0; uint32_t highestWhole = 0, received = 0, lost = 0, lostId = 0; uint64_t rttUs = 0, lastGood = 0; };
  // wireBytes: every datagram emitted (media, repairs, TAIL), as it goes on the wire.
  // audioRepeats: sound datagrams sent again (see audioRepeats above).
  // parities: parity datagrams sent (parity-v1), parityBytes on the wire.
  struct Stats { uint64_t datagrams = 0, videoBytes = 0, repairs = 0, repairBytes = 0, refused = 0, tails = 0, mergedKeys = 0, recoveries = 0, wireBytes = 0, audioRepeats = 0, parities = 0, parityBytes = 0; };

  Sender(int targetBps, uint64_t nowUs, std::function<uint64_t()> random)
      : random_(std::move(random)), at_(nowUs), alive_(nowUs), proofAt_(nowUs), challengeAt_(nowUs), challenge_(random_()) { setTarget(targetBps); budget_ = budgetLimit(); credit_ = creditLimit(); }
  // `nowUs`: when the rate controller set it (0: no change of its own, the first target).
  void setTarget(int targetBps, uint64_t nowUs = 0) {
    const double next = double(std::max(targetBps, 1)) / 8e6;
    if (nowUs && next < bytesPerUs_ * (1 - parityCutShare)) gaveUp(nowUs);  // parity-v1
    bytesPerUs_ = next;
  }
  bool proven() const { return proven_; }
  uint64_t rttUs() const { return rtt_; }

  // tooLarge: the message cannot go on UDP. A key frame goes on WSS, which makes
  // WSS active; after a P-frame the Host returns to WSS at a forced IDR.
  // `recovery`: the answer to a LOSS (classRecovery), a P-frame the frames waiting here are
  // useless to, like a key frame's; the backlog still needs a key frame.
  Admission video(std::vector<uint8_t> message, bool key, bool recovery = false) {
    Admission result;
    if (!proven_) return result;
    if (!fragmentCount(message.size())) { result.tooLarge = true; if (!key) { awaitingKey_ = true; result.requestKey = true; } return result; }
    if (!key && (awaitingKey_ || double(queuedBytes_ - queuedKeyBytes_) > bytesPerUs_ * queueFactor * double(queuePacingUs))) {
      awaitingKey_ = true; result.requestKey = true; return result;
    }
    if (key) awaitingKey_ = false;
    if (key || recovery) {
      recoverAt_ = 0;
      while (queue_.size() > (!queue_.empty() && queue_.front().sent ? 1u : 0u)) { account(queue_.back(), false); queue_.pop_back(); }
    }
    Item item; item.count = uint16_t(fragmentCount(message.size())); item.key = key; item.recovery = recovery && !key; item.bytes = std::move(message);
    account(item, true); queue_.push_back(std::move(item)); result.queued = true; return result;
  }
  // The caller acts on a LOSS: until its answer is queued, a KEYFRAME that came with it is merged.
  void recovering(uint64_t nowUs) { recoverAt_ = std::max<uint64_t>(nowUs, 1); ++stats_.recoveries; }
  // Never retransmitted on request. `again`: a receiver plays this message's number once, so
  // it may go out again with the next ones (audioRepeats); only one datagram's worth does.
  Admission audio(std::vector<uint8_t> message, bool again = false) {
    Admission result; const size_t count = fragmentCount(message.size());
    if (!proven_) return result;
    if (!count || count > maxAudioFragments) { result.tooLarge = true; return result; }
    if (audio_.size() >= maxAudioQueue) return result;
    Item item; item.count = uint16_t(count); item.again = again && count == 1; item.bytes = std::move(message); audio_.push_back(std::move(item)); result.queued = true; return result;
  }

  // An authenticated plaintext from the client. The caller forces an IDR for a
  // key frame request of the current epoch. A request made while one is in flight
  // does not come back here as a request: that IDR is its answer.
  Control control(const uint8_t* plain, size_t size, uint64_t nowUs) {
    Control result;
    if (size == feedbackBytes && plain[0] == kindFeedback && !plain[1]) feedback(plain, nowUs, result);
    else if (!proven_) return result;
    else if (size >= 2 && plain[0] == kindNack) nack(plain, size, nowUs);
    else if (size == keyframeBytes && plain[0] == kindKeyframe && !plain[1]) {
      result.keyFrame = !keyInFlight(nowUs); result.epoch = get16(plain + 2);
      if (!result.keyFrame) ++stats_.mergedKeys;
      gaveUp(nowUs);
    }
    // A key frame in flight answers a LOSS too: the client takes one in any wait.
    else if (size == lossBytes && plain[0] == kindLoss && !plain[1]) {
      result.loss = !keyInFlight(nowUs); result.epoch = get16(plain + 2); result.lostId = get32(plain + 4); result.lastGood = get64(plain + 8);
      gaveUp(nowUs);
    }
    // parity-v1: what the client takes, and what parity has repaired for it so far.
    else if (size == abilitiesBytes && plain[0] == kindAbilities && !plain[1]) {
      parityAsked_ = (get16(plain + 2) & abilityParity) != 0;
      const uint32_t repaired = get32(plain + 4);
      if (int32_t(repaired - repaired_) > 0) { repaired_ = repaired; if (parityAsked_ && nowUs >= parityBlockedUntil_) parityUntil_ = std::max(parityUntil_, nowUs + parityHoldUs); }
    }
    return result;
  }

  bool next(uint64_t nowUs, Plain& out) {
    if (stopped_) return false;
    refill(nowUs); expire(nowUs);
    if (wire_ < double(maxDatagram)) return false;  // the wire pace: this clump is out
    if (tailAt_ && nowUs >= tailAt_ && !queue_.empty()) tailAt_ = 0;  // a newer message is starting
    if (nowUs >= proofAt_ || (tailAt_ && nowUs >= tailAt_)) { tail(nowUs, out); return emitted(out); }
    // The target is a video rate; audio stays outside it, as in BitrateController.
    if (!audio_.empty()) {
      Item& item = audio_.front();
      // Before a new one: the ones before it again, oldest first.
      if (!item.sent && item.again) while (repeatAt_ < sentAudio_.size()) {
        const Item& old = sentAudio_[repeatAt_++];
        if (nowUs - old.doneAt > audioRepeatUs) continue;
        writeFragment(out, classAudio, old.id, old.bytes, 0); ++stats_.audioRepeats;
        return emitted(out);
      }
      if (!item.sent) item.id = audioId_++;
      writeFragment(out, classAudio, item.id, item.bytes, item.sent);
      if (++item.sent == item.count) {
        repeatAt_ = 0;
        if (item.again) { item.doneAt = nowUs; sentAudio_.push_back(std::move(item)); if (sentAudio_.size() > audioRepeats) sentAudio_.pop_front(); }
        else sentAudio_.clear();
        audio_.pop_front();
      }
      return emitted(out);
    }
    while (!repairs_.empty()) {
      const Repair repair = repairs_.front();
      const Item* item = find(repair.id);
      if (!item) { repairs_.pop_front(); continue; }
      writeFragment(out, item->recovery ? classRecovery : classVideo, repair.id, item->bytes, repair.index);
      if (!charge(out.size)) return false;
      repairs_.pop_front(); ++stats_.repairs; stats_.repairBytes += out.size + headerBytes + tagBytes;
      return emitted(out);
    }
    if (queue_.empty()) return false;
    Item& item = queue_.front();
    const uint8_t cls = item.recovery ? classRecovery : classVideo;
    if (item.sent == item.count) {  // its parity, right behind it
      writeParity(out, cls, item.id, item.bytes, item.paritySent, item.parity);
      if (!charge(out.size)) return false;
      ++stats_.parities; stats_.parityBytes += out.size + headerBytes + tagBytes;
      if (++item.paritySent == item.parity) finish(nowUs);
      return emitted(out);
    }
    writeFragment(out, cls, item.sent ? item.id : videoId_, item.bytes, item.sent);
    if (!charge(out.size)) return false;
    if (!item.sent) { item.id = videoId_++; item.parity = uint8_t(parityOn(nowUs) ? parityCount(item.count) : 0); }
    stats_.videoBytes += out.size + headerBytes + tagBytes;
    if (++item.sent == item.count && !item.parity) finish(nowUs);
    return emitted(out);
  }
  // parity-v1: whether a message that starts now is followed by parity.
  bool parityOn(uint64_t nowUs) const { return parityAsked_ && nowUs < parityUntil_; }
  // The client gave a frame up, or the rate controller lowered the target: parity is load on this path now.
  void gaveUp(uint64_t nowUs) { parityUntil_ = 0; parityBlockedUntil_ = nowUs + parityBackoffUs; }
  // The client asked for a repair: parity gets a trial, unless one ran lately and repaired
  // nothing, or the path gave datagrams up lately.
  void lossShown(uint64_t nowUs) {
    if (!parityAsked_ || nowUs < parityUntil_ || nowUs < parityBlockedUntil_ || (trialAt_ && nowUs - trialAt_ < parityHoldUs)) return;
    trialAt_ = std::max<uint64_t>(nowUs, 1); parityUntil_ = nowUs + parityTrialUs;
  }

  // Back to WSS at a forced IDR. The contract's rule is FEEDBACK silence, but
  // FEEDBACK runs on the client's timer: a Host-to-client outage, or video that is
  // never made whole, would not go silent and would freeze on UDP for good. So
  // FEEDBACK that shows no progress counts as silence, but only while the Host has
  // sent something newer than it reports. An idle screen has nothing to report.
  bool silent(uint64_t nowUs) const { return nowUs - alive_ >= feedbackSilenceUs; }
  // Why it is silent, for the record only: 1 no FEEDBACK came, 2 FEEDBACK came but what was
  // sent did not arrive, 3 it arrived but no newer message was made whole and handed on.
  uint32_t silenceCause(uint64_t nowUs) const {
    return nowUs - heard_ >= feedbackSilenceUs ? 1 : staleEcho_ || !arriving_ ? 2 : 3;
  }
  bool exhausted() const { return videoId_ == UINT32_MAX || audioId_ == UINT32_MAX; }
  void stop() { stopped_ = true; queue_.clear(); kept_.clear(); audio_.clear(); sentAudio_.clear(); repairs_.clear(); queuedBytes_ = queuedKeyBytes_ = keptBytes_ = 0; }

  const Stats& stats() const { return stats_; }
  size_t heldBytes() const { return queuedBytes_ + keptBytes_; }
  size_t waitingRepairs() const { return repairs_.size(); }
  // Something is held here: the caller comes back a wireClumpUs later, not at its idle pace.
  bool waiting() const { return !queue_.empty() || !repairs_.empty() || !audio_.empty(); }

 private:
  // parity, paritySent: parity datagrams this message is followed by, and how many have gone.
  struct Item { std::vector<uint8_t> bytes; uint32_t id = 0; uint16_t count = 0, sent = 0; uint8_t parity = 0, paritySent = 0; bool key = false, recovery = false, again = false; uint64_t doneAt = 0; };
  struct Repair { uint32_t id = 0; uint16_t index = 0; };

  // From `video` taking a key frame until a round trip after its last fragment. It
  // starts there and not at the request, which the caller may rightly ignore. A
  // recovery frame counts the same, and so does the wait for one the caller asked for.
  bool keyInFlight(uint64_t nowUs) const {
    return queuedKeyBytes_ || (keySentAt_ && nowUs < keySentAt_ + (rtt_ ? rtt_ : assumedRttUs)) || (recoverAt_ && nowUs < recoverAt_ + keyframeIntervalUs / 2);
  }
  // The message at the front has left in full, its parity included.
  void finish(uint64_t nowUs) {
    Item& item = queue_.front();
    if (item.key || item.recovery) keySentAt_ = nowUs;
    account(item, false); keptBytes_ += item.bytes.size(); item.doneAt = nowUs; finished_ = item.id;
    kept_.push_back(std::move(item)); queue_.pop_front();
    tailAt_ = nowUs + tailFirstUs; tailsLeft_ = 2;
  }
  void account(const Item& item, bool added) {
    const size_t size = item.bytes.size();
    if (added) queuedBytes_ += size; else queuedBytes_ -= size;
    if (item.key || item.recovery) { if (added) queuedKeyBytes_ += size; else queuedKeyBytes_ -= size; }
  }
  double creditLimit() const { return std::max(double(maxDatagram) * 2, bytesPerUs_ * pacerFactor * double(pacerBurstUs)); }
  double budgetLimit() const { return bytesPerUs_ * repairShare * double(repairWindowUs); }
  void refill(uint64_t nowUs) {
    const double elapsed = double(nowUs - at_); at_ = nowUs;
    credit_ = std::min(credit_ + elapsed * bytesPerUs_ * pacerFactor, creditLimit());
    wire_ = std::min(wire_ + elapsed * wireBytesPerUs, wireClumpBytes);
    budget_ = std::min(budget_ + elapsed * bytesPerUs_ * repairShare, budgetLimit());
  }
  bool charge(size_t plainBytes) {
    const double wire = double(plainBytes + headerBytes + tagBytes);
    if (credit_ < wire) return false;
    credit_ -= wire; return true;
  }
  void expire(uint64_t nowUs) {
    while (!kept_.empty() && (nowUs - kept_.front().doneAt > resendKeepUs || keptBytes_ > maxKeptBytes)) { keptBytes_ -= kept_.front().bytes.size(); kept_.pop_front(); }
  }
  // The contract's 200 ms timer is for before the switch, but frames that leave under
  // 5 ms apart send no end-of-frame TAIL, and FEEDBACK would go stale and read as
  // silence. So it keeps running. It may fire mid-message: it names the last finished.
  void tail(uint64_t nowUs, Plain& out) {
    if (nowUs - challengeAt_ >= challengeRotateUs) { previous_ = challenge_; challenge_ = random_(); challengeAt_ = nowUs; }
    out.bytes[0] = kindTail; out.bytes[1] = 0; put32(&out.bytes[2], finished_); put64(&out.bytes[6], std::max<uint64_t>(nowUs, 1)); put64(&out.bytes[14], challenge_);
    put32(&out.bytes[22], uint32_t(std::min<uint64_t>(rtt_, UINT32_MAX))); out.size = tailBytes;
    if (tailAt_ && nowUs >= tailAt_) tailAt_ = tailsLeft_-- > 1 ? nowUs + tailSecondUs : 0;
    proofAt_ = nowUs + proofTailUs; ++stats_.tails;
  }
  bool emitted(const Plain& out) { ++sent_; ++stats_.datagrams; stats_.wireBytes += out.size + headerBytes + tagBytes; wire_ -= double(out.size + headerBytes + tagBytes); return true; }
  const Item* find(uint32_t id) const {
    for (const auto& item : kept_) if (item.id == id) return &item;
    return !queue_.empty() && queue_.front().sent && queue_.front().id == id ? &queue_.front() : nullptr;
  }
  void feedback(const uint8_t* plain, uint64_t nowUs, Control& result) {
    // Authenticated, so the client is heard even when its echo is stale: the echo is
    // stale when no TAIL reaches it, which only the cause below may use.
    heard_ = nowUs;
    const uint64_t challenge = get64(plain + 26);
    if (!challenge || (challenge != challenge_ && challenge != previous_)) { staleEcho_ = true; return; }
    proven_ = true; staleEcho_ = false;
    // The network can reorder reports. Both counters only grow, so a lower one is old news.
    const uint32_t received = get32(plain + 6), lost = get32(plain + 10);
    if (int32_t(received - confirmed_) < 0 || int32_t(lost - confirmedLost_) < 0) return;
    confirmedLost_ = lost;
    result.feedback = true; result.highestWhole = get32(plain + 2); result.received = received; result.lost = lost;
    const uint64_t echo = get64(plain + 14), hold = get32(plain + 22);
    if (echo && echo <= nowUs && hold <= nowUs - echo) {
      result.rttUs = std::max<uint64_t>(nowUs - echo - hold, 1);
      rtt_ = rtt_ ? (7 * rtt_ + result.rttUs) / 8 : result.rttUs;
    }
    if (result.received != confirmed_) { confirmed_ = result.received; sentAtConfirm_ = sent_; }
    // parity-v1: what did not arrive of what was sent, over about a second (the datagrams on their way at both ends cancel).
    if (nowUs - shareAt_ >= parityLossWindowUs) {
      const uint64_t sent = sent_ - shareSent_; const uint32_t arrived = received - shareReceived_;
      if (shareAt_ && sent >= parityLossDatagrams && double(arrived) < double(sent) * (1 - parityLossShare)) gaveUp(nowUs);
      shareAt_ = std::max<uint64_t>(nowUs, 1); shareSent_ = sent_; shareReceived_ = received;
    }
    arriving_ = sent_ - sentAtConfirm_ < unconfirmedLimit;
    const bool videoMoves = result.highestWhole != whole_ || finished_ <= result.highestWhole;
    whole_ = result.highestWhole;
    if (videoMoves && sent_ - sentAtConfirm_ < unconfirmedLimit) alive_ = nowUs;
  }
  void nack(const uint8_t* plain, size_t size, uint64_t nowUs) {
    const size_t entries = plain[1];
    if (!entries || entries > maxNackEntries || size != 2 + entries * nackEntryBytes) return;
    for (size_t n = 0; n < entries; ++n) if (!get16(plain + 2 + n * nackEntryBytes + 6)) return;
    lossShown(nowUs);
    refill(nowUs); expire(nowUs);
    for (size_t n = 0; n < entries; ++n) {
      const uint8_t* entry = plain + 2 + n * nackEntryBytes;
      const Item* item = find(get32(entry));
      if (!item) continue;
      const unsigned first = get16(entry + 4), end = std::min<unsigned>(first + get16(entry + 6), item->sent);
      // A key or recovery frame ends the client's wait; without it the client asks again and
      // waits for another. Its repairs may borrow one window's budget, paid back before any
      // other repair goes. Field 2026-11-20: a repair was refused in nearly every 0.6 s freeze.
      const double floor = item->key || item->recovery ? -budgetLimit() : 0;
      for (unsigned index = first; index < end; ++index) {
        // The budget, like the pacer, only ever counts what goes on the wire.
        const double wire = double(std::min(fragmentPayload, item->bytes.size() - size_t(index) * fragmentPayload) + fragmentHeaderBytes + headerBytes + tagBytes);
        if (budget_ - wire < floor || repairs_.size() >= maxRepairs) { stats_.refused += end - index; break; }
        budget_ -= wire; repairs_.push_back({item->id, uint16_t(index)});
      }
    }
  }

  std::function<uint64_t()> random_;
  uint64_t at_, alive_, proofAt_, challengeAt_, challenge_, previous_ = 0, rtt_ = 0;
  double bytesPerUs_ = 0, credit_ = 0, budget_ = 0, wire_ = wireClumpBytes;
  bool proven_ = false, awaitingKey_ = false, stopped_ = false, arriving_ = true, staleEcho_ = false;  // as the last FEEDBACK showed
  uint32_t videoId_ = 1, audioId_ = 1;  // given when a message starts, so a dropped one leaves no gap
  size_t queuedBytes_ = 0, queuedKeyBytes_ = 0, keptBytes_ = 0;  // a key frame alone must not look like a backlog
  uint64_t tailAt_ = 0, keySentAt_ = 0, recoverAt_ = 0; int tailsLeft_ = 0;
  // parity-v1: the client asked for parity; until when it goes; when its last trial began; the client's count of repaired fragments.
  // parityBlockedUntil_: no parity before then (the client gave a frame up).
  bool parityAsked_ = false; uint64_t parityUntil_ = 0, trialAt_ = 0, parityBlockedUntil_ = 0; uint32_t repaired_ = 0;
  // The start of the second now measured for parityLossShare, and the two counts then.
  uint64_t shareAt_ = 0, shareSent_ = 0; uint32_t shareReceived_ = 0;
  uint64_t sent_ = 0, sentAtConfirm_ = 0, heard_ = 0; uint32_t confirmed_ = 0, confirmedLost_ = 0, whole_ = 0, finished_ = 0;
  std::deque<Item> queue_, kept_, audio_;
  std::deque<Item> sentAudio_;  // the last audioRepeats sound messages sent that may go again
  size_t repeatAt_ = 0;         // which of them goes next, before the sound message now waiting
  std::deque<Repair> repairs_;
  Stats stats_;
};

// "Probe again no sooner than 30 s later, doubling to 5 minutes."
class ProbeBackoff {
 public:
  void failed(uint64_t nowUs) { notBefore_ = nowUs + delay_; delay_ = std::min(delay_ * 2, probeBackoffMaximumUs); }
  bool mayProbe(uint64_t nowUs) const { return nowUs >= notBefore_; }
  void reset() { delay_ = probeBackoffFirstUs; notBefore_ = 0; }
 private:
  uint64_t delay_ = probeBackoffFirstUs, notBefore_ = 0;
};

// The path rule: a key frame from either path makes that path active, P-frames
// count only from the active path, and the PTS must be newer than the last
// delivered. PTS restarts with the encoder, so "newer" holds within an epoch.
enum class Path : uint8_t { wss, udp };
struct PathState { Path active = Path::wss; uint16_t epoch = 0; uint64_t lastPts = 0; bool delivered = false; };
inline bool acceptVideo(PathState& state, Path from, uint16_t epoch, bool key, uint64_t pts) {
  const bool current = state.delivered && epoch == state.epoch;
  if (state.delivered && epoch < state.epoch) return false;
  if (current ? pts <= state.lastPts || (!key && from != state.active) : !key) return false;
  state = {key ? from : state.active, epoch, pts, true};
  return true;
}
}
