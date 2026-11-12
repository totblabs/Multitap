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
constexpr uint64_t queuePacingUs = 250000, repairWindowUs = 100000, pacerBurstUs = 6000;
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
  std::vector<uint8_t> take() { bytes_.resize(size_); return std::move(bytes_); }
  void release() { count_ = have_ = 0; size_ = 0; got_ = {}; bytes_ = {}; }
 private:
  uint16_t count_ = 0, have_ = 0;
  size_t size_ = 0;
  std::vector<bool> got_;
  std::vector<uint8_t> bytes_;
};

// Client side. The caller opens each datagram and passes the plaintext; `tick`
// runs the timers, every few milliseconds whether or not media arrives. It gets
// messages to hand to the decoder and control plaintexts to seal and send. Its RTT
// is the Host's, carried by TAIL. Malformed input only ever drops data.
class Receiver {
 public:
  struct Message { bool video = false, key = false; uint16_t epoch = 0; uint64_t pts = 0; std::vector<uint8_t> bytes; };
  struct Output { std::vector<Message> messages; std::vector<std::vector<uint8_t>> control; };

  Receiver(uint16_t epoch, uint64_t nowUs) : epoch_(epoch), feedbackAt_(nowUs + feedbackIntervalUs) {}
  uint64_t rttUs() const { return rtt_; }
  // Announced on WSS. What carries another epoch is dropped, and no frame of it is known good.
  void setEpoch(uint16_t epoch) { if (epoch != epoch_) { haveLast_ = lossWait_ = false; } epoch_ = epoch; }

  void datagram(const uint8_t* plain, size_t size, uint64_t nowUs, Output& out) {
    Fragment f;
    if (parseFragment(plain, size, f)) { ++received_; if (f.cls == classAudio) audio(f, out); else video(f, nowUs, out); }
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
    if (awaitKey_ && nowUs >= keyAt_ && !keyArriving()) {
      if (lossWait_) {
        std::vector<uint8_t> loss(lossBytes); loss[0] = kindLoss; put16(&loss[2], epoch_); put32(&loss[4], lossId_); put64(&loss[8], lastPts_);
        out.control.push_back(std::move(loss));
      }
      std::vector<uint8_t> plain(keyframeBytes); plain[0] = kindKeyframe; put16(&plain[2], epoch_);
      out.control.push_back(std::move(plain)); keyAt_ = nowUs + keyframeIntervalUs;
    }
    if (nowUs >= feedbackAt_) { feedback(nowUs, out); feedbackAt_ = nowUs + feedbackIntervalUs; }
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
  void queueFull(uint64_t nowUs) { if (!awaitKey_) { awaitKey_ = true; keyAt_ = std::max(keyAt_, nowUs); } lossWait_ = false; }

  bool awaitingKey() const { return awaitKey_; }
  // The wait a recovery frame may end (a LOSS goes with each KEYFRAME).
  bool awaitingRecovery() const { return awaitKey_ && lossWait_; }
  size_t heldBytes() const { size_t bytes = held_; for (const auto& a : audio_) bytes += a.reserved(); return bytes; }
  size_t gaps() const { return gaps_.size(); }
  size_t windowMessages() const { return window_.size(); }

 private:
  struct Entry : Partial { uint16_t frontier = 0; bool known = false, skip = false, recovery = false; Envelope envelope; };  // indices below frontier are passed
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
  uint64_t nackDelay() const { return std::max(rtt_ / 4, minimumNackDelayUs); }
  uint64_t nackRepeat() const { return std::max(rtt_ * 5 / 4, minimumNackRepeatUs); }
  void addGap(uint32_t id, uint16_t index, bool whole, uint64_t nowUs) { gaps_.push_back({id, index, 0, whole, nowUs, nowUs + nackDelay()}); }
  void dropUntilKey(uint64_t nowUs) { if (!awaitKey_) { awaitKey_ = true; keyAt_ = nowUs; } lossWait_ = false; }
  // Message `id` is lost to the decoder. With a frame delivered on this path this epoch, a
  // recovery frame may end the wait as well; a wait already running keeps its kind.
  void dropForLoss(uint64_t id, uint64_t nowUs) {
    if (awaitKey_) return;
    awaitKey_ = true; keyAt_ = nowUs; lossWait_ = haveLast_; lossId_ = uint32_t(id);
  }
  // Ends a wait: a key frame always, a recovery frame only the wait it may end.
  bool ends(const Entry& e) const { return e.known && !e.skip && (e.envelope.key || (lossWait_ && e.recovery)); }
  void abandonBefore(uint64_t id) {
    while (next_ < id && !window_.empty()) {
      const Entry& m = window_.front();
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
    for (const auto& e : window_) if (!e.skip && ((e.known && e.envelope.key) || (lossWait_ && e.recovery))) return true;
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
    m.skip = true; held_ -= m.reserved(); m.release();
  }
  void video(const Fragment& f, uint64_t nowUs, Output& out) {
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

  uint64_t rtt_ = assumedRttUs;
  uint16_t epoch_;
  uint64_t feedbackAt_, keyAt_ = 0, echoUs_ = 0, echoAt_ = 0, challenge_ = 0;
  uint64_t next_ = 1, closed_ = 0;  // next ID to deliver, and the last known sent in full
  // lossWait_: the wait a recovery frame may end; lastPts_ the newest video this path delivered.
  bool awaitKey_ = false, lossWait_ = false, haveLast_ = false;
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
  struct Stats { uint64_t datagrams = 0, videoBytes = 0, repairs = 0, repairBytes = 0, refused = 0, tails = 0, mergedKeys = 0, recoveries = 0; };

  Sender(int targetBps, uint64_t nowUs, std::function<uint64_t()> random)
      : random_(std::move(random)), at_(nowUs), alive_(nowUs), proofAt_(nowUs), challengeAt_(nowUs), challenge_(random_()) { setTarget(targetBps); budget_ = budgetLimit(); credit_ = creditLimit(); }
  void setTarget(int targetBps) { bytesPerUs_ = double(std::max(targetBps, 1)) / 8e6; }
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
  // Never retransmitted, so it is never kept.
  Admission audio(std::vector<uint8_t> message) {
    Admission result; const size_t count = fragmentCount(message.size());
    if (!proven_) return result;
    if (!count || count > maxAudioFragments) { result.tooLarge = true; return result; }
    if (audio_.size() >= maxAudioQueue) return result;
    Item item; item.count = uint16_t(count); item.bytes = std::move(message); audio_.push_back(std::move(item)); result.queued = true; return result;
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
    }
    // A key frame in flight answers a LOSS too: the client takes one in any wait.
    else if (size == lossBytes && plain[0] == kindLoss && !plain[1]) {
      result.loss = !keyInFlight(nowUs); result.epoch = get16(plain + 2); result.lostId = get32(plain + 4); result.lastGood = get64(plain + 8);
    }
    return result;
  }

  bool next(uint64_t nowUs, Plain& out) {
    if (stopped_) return false;
    refill(nowUs); expire(nowUs);
    if (tailAt_ && nowUs >= tailAt_ && !queue_.empty()) tailAt_ = 0;  // a newer message is starting
    if (nowUs >= proofAt_ || (tailAt_ && nowUs >= tailAt_)) { tail(nowUs, out); return emitted(); }
    // The target is a video rate; audio stays outside it, as in BitrateController.
    if (!audio_.empty()) {
      Item& item = audio_.front();
      if (!item.sent) item.id = audioId_++;
      writeFragment(out, classAudio, item.id, item.bytes, item.sent);
      if (++item.sent == item.count) audio_.pop_front();
      return emitted();
    }
    while (!repairs_.empty()) {
      const Repair repair = repairs_.front();
      const Item* item = find(repair.id);
      if (!item) { repairs_.pop_front(); continue; }
      writeFragment(out, item->recovery ? classRecovery : classVideo, repair.id, item->bytes, repair.index);
      if (!charge(out.size)) return false;
      repairs_.pop_front(); ++stats_.repairs; stats_.repairBytes += out.size + headerBytes + tagBytes;
      return emitted();
    }
    if (queue_.empty()) return false;
    Item& item = queue_.front();
    writeFragment(out, item.recovery ? classRecovery : classVideo, item.sent ? item.id : videoId_, item.bytes, item.sent);
    if (!charge(out.size)) return false;
    if (!item.sent) item.id = videoId_++;
    stats_.videoBytes += out.size + headerBytes + tagBytes;
    if (++item.sent == item.count) {
      if (item.key || item.recovery) keySentAt_ = nowUs;
      account(item, false); keptBytes_ += item.bytes.size(); item.doneAt = nowUs; finished_ = item.id;
      kept_.push_back(std::move(item)); queue_.pop_front();
      tailAt_ = nowUs + tailFirstUs; tailsLeft_ = 2;
    }
    return emitted();
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
  void stop() { stopped_ = true; queue_.clear(); kept_.clear(); audio_.clear(); repairs_.clear(); queuedBytes_ = queuedKeyBytes_ = keptBytes_ = 0; }

  const Stats& stats() const { return stats_; }
  size_t heldBytes() const { return queuedBytes_ + keptBytes_; }
  size_t waitingRepairs() const { return repairs_.size(); }

 private:
  struct Item { std::vector<uint8_t> bytes; uint32_t id = 0; uint16_t count = 0, sent = 0; bool key = false, recovery = false; uint64_t doneAt = 0; };
  struct Repair { uint32_t id = 0; uint16_t index = 0; };

  // From `video` taking a key frame until a round trip after its last fragment. It
  // starts there and not at the request, which the caller may rightly ignore. A
  // recovery frame counts the same, and so does the wait for one the caller asked for.
  bool keyInFlight(uint64_t nowUs) const {
    return queuedKeyBytes_ || (keySentAt_ && nowUs < keySentAt_ + (rtt_ ? rtt_ : assumedRttUs)) || (recoverAt_ && nowUs < recoverAt_ + keyframeIntervalUs / 2);
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
  bool emitted() { ++sent_; ++stats_.datagrams; return true; }
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
    arriving_ = sent_ - sentAtConfirm_ < unconfirmedLimit;
    const bool videoMoves = result.highestWhole != whole_ || finished_ <= result.highestWhole;
    whole_ = result.highestWhole;
    if (videoMoves && sent_ - sentAtConfirm_ < unconfirmedLimit) alive_ = nowUs;
  }
  void nack(const uint8_t* plain, size_t size, uint64_t nowUs) {
    const size_t entries = plain[1];
    if (!entries || entries > maxNackEntries || size != 2 + entries * nackEntryBytes) return;
    for (size_t n = 0; n < entries; ++n) if (!get16(plain + 2 + n * nackEntryBytes + 6)) return;
    refill(nowUs); expire(nowUs);
    for (size_t n = 0; n < entries; ++n) {
      const uint8_t* entry = plain + 2 + n * nackEntryBytes;
      const Item* item = find(get32(entry));
      if (!item) continue;
      const unsigned first = get16(entry + 4), end = std::min<unsigned>(first + get16(entry + 6), item->sent);
      for (unsigned index = first; index < end; ++index) {
        // The budget, like the pacer, only ever counts what goes on the wire.
        const double wire = double(std::min(fragmentPayload, item->bytes.size() - size_t(index) * fragmentPayload) + fragmentHeaderBytes + headerBytes + tagBytes);
        if (budget_ < wire || repairs_.size() >= maxRepairs) { stats_.refused += end - index; return; }
        budget_ -= wire; repairs_.push_back({item->id, uint16_t(index)});
      }
    }
  }

  std::function<uint64_t()> random_;
  uint64_t at_, alive_, proofAt_, challengeAt_, challenge_, previous_ = 0, rtt_ = 0;
  double bytesPerUs_ = 0, credit_ = 0, budget_ = 0;
  bool proven_ = false, awaitingKey_ = false, stopped_ = false, arriving_ = true, staleEcho_ = false;  // as the last FEEDBACK showed
  uint32_t videoId_ = 1, audioId_ = 1;  // given when a message starts, so a dropped one leaves no gap
  size_t queuedBytes_ = 0, queuedKeyBytes_ = 0, keptBytes_ = 0;  // a key frame alone must not look like a backlog
  uint64_t tailAt_ = 0, keySentAt_ = 0, recoverAt_ = 0; int tailsLeft_ = 0;
  uint64_t sent_ = 0, sentAtConfirm_ = 0, heard_ = 0; uint32_t confirmed_ = 0, confirmedLost_ = 0, whole_ = 0, finished_ = 0;
  std::deque<Item> queue_, kept_, audio_;
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
