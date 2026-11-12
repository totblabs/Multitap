#pragma once
// audio-stats-v1: the Host's share of the sound path, as fixed numbers in the
// stream.stats it writes (client lib/data/audio_stats.dart reads them).
//  - Worker, `audioCapture`: WASAPI loopback capture -> packet handed to the
//    send path, per packet (the device position's QPC time of the packet's
//    first frame to the moment the packet leaves the capture loop).
//  - Service, `audioSend`: packets handed on (written to the broker channel or
//    taken by the UDP path) and dropped (send queue or UDP sender full), and
//    the send-queue wait (queued -> its WebSocket write starts) of each audio
//    packet written there.
// Observational only; nothing here changes what is sent.
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>

namespace host {
// QPC ticks (`counter`, `frequency` per second) and a device position time in
// 100 ns units, as IAudioCaptureClient::GetBuffer gives it: microseconds since
// that position, or -1 when the position is missing or in the future.
inline int64_t audioCaptureLatencyUs(int64_t counter, int64_t frequency, uint64_t position100ns) {
  if (counter <= 0 || frequency <= 0 || !position100ns) return -1;
  const uint64_t now100ns = uint64_t(counter / frequency) * 10000000ull + uint64_t(counter % frequency) * 10000000ull / uint64_t(frequency);
  return now100ns < position100ns ? -1 : int64_t((now100ns - position100ns) / 10);
}

class AudioCaptureWindow {
 public:
  void add(uint64_t latencyUs) { std::lock_guard lock(mutex_); ++packets_; total_ += latencyUs; maximum_ = std::max(maximum_, latencyUs); }
  // `,"audioCapture":{...}` for the packets since the last call, or "" without any.
  std::string takeJson() {
    std::lock_guard lock(mutex_);
    if (!packets_) return {};
    const std::string json = ",\"audioCapture\":{\"packets\":" + std::to_string(packets_) + ",\"avgUs\":" + std::to_string(total_ / packets_) + ",\"maxUs\":" + std::to_string(maximum_) + '}';
    packets_ = total_ = maximum_ = 0;
    return json;
  }
 private:
  std::mutex mutex_;
  uint64_t packets_ = 0, total_ = 0, maximum_ = 0;
};

class AudioSendWindow {
 public:
  void sent(uint64_t packets = 1) { std::lock_guard lock(mutex_); sent_ += packets; }
  void dropped(uint64_t packets) { std::lock_guard lock(mutex_); dropped_ += packets; }
  // A packet written to the broker channel after `waitUs` in the send queue.
  void written(uint64_t waitUs) { std::lock_guard lock(mutex_); ++sent_; ++queued_; total_ += waitUs; maximum_ = std::max(maximum_, waitUs); }
  // `,"audioSend":{...}` since the last call, or "" when no audio came by.
  std::string takeJson() {
    std::lock_guard lock(mutex_);
    if (!sent_ && !dropped_) return {};
    std::string json = ",\"audioSend\":{\"sent\":" + std::to_string(sent_) + ",\"dropped\":" + std::to_string(dropped_);
    if (queued_) json += ",\"queuePackets\":" + std::to_string(queued_) + ",\"queueAvgUs\":" + std::to_string(total_ / queued_) + ",\"queueMaxUs\":" + std::to_string(maximum_);
    sent_ = dropped_ = queued_ = total_ = maximum_ = 0;
    return json + '}';
  }
  void clear() { std::lock_guard lock(mutex_); sent_ = dropped_ = queued_ = total_ = maximum_ = 0; }
 private:
  std::mutex mutex_;
  uint64_t sent_ = 0, dropped_ = 0, queued_ = 0, total_ = 0, maximum_ = 0;
};
}
