#ifndef MULTITAP_REMOTE_AUDIO_H_
#define MULTITAP_REMOTE_AUDIO_H_
#include <windows.h>
#include <flutter/encodable_value.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include "audio_jitter.h"
#include "opus_audio_decoder.h"
namespace multitap {
class RemoteAudio {
 public:
  ~RemoteAudio() { Stop(); if (opus_wake_) CloseHandle(opus_wake_); }
  // `opus`: the stream's packets are audio-opus-v1 (stream.info format
  // "opus"), decoded here to the PCM the jitter buffer holds.
  void Start(bool opus = false);
  // The render thread's wait between packets. PCM keeps its condition
  // variable (5 ms, which Windows may stretch to ~16 ms). Opus waits on an
  // event and a 5 ms high-resolution timer, so concealment comes before the
  // device runs dry. Not granting Opus brings a client back to the PCM wait.
  static bool UsesTimerWait(bool opus) { return opus; }
  void Stop();
  // False only for a malformed packet. A missing endpoint or a backlog
  // (oldest samples dropped, see AudioJitterBuffer) never stops the video
  // stream. `pts_us`: the packet's Host capture-clock PTS.
  bool Push(uint64_t pts_us, const uint8_t* bytes, size_t size);
  // A video frame of Host capture-clock PTS `pts_us` was handed to its
  // texture now: its distance to the audio being heard (A/V offset estimate).
  void NoteVideo(uint64_t pts_us);
  uint64_t played_frames() const { return played_frames_; }
  uint64_t received_frames() const { return received_frames_; }
  // Dropped to bound the latency (overruns); drift trimming is counted apart.
  uint64_t dropped_frames() { std::lock_guard<std::mutex> lock(mutex_); return buffer_.counters().overrun_frames; }
  HRESULT error() const { return error_; }
  // audio-stats-v1 members of an engine's statistics (cumulative counters;
  // depth, target and latency as of now).
  void AppendStats(flutter::EncodableMap& stats);
 private:
  void Run();
  void Accept(uint64_t pts_us, const uint8_t* bytes, size_t size);
  static uint64_t NowUs();
  // Push may come from the WebSocket and the UDP receive threads; the render
  // thread conceals with the decoder too. Always taken before mutex_.
  std::mutex decode_mutex_;
  bool opus_ = false;
  OpusAudioDecoder decoder_;
  std::atomic<bool> running_{false};
  std::atomic<HRESULT> error_{S_OK};
  std::atomic<uint64_t> played_frames_{0};
  std::atomic<uint64_t> received_frames_{0};
  std::mutex mutex_;
  std::condition_variable wake_;
  bool pushed_ = false;
  HANDLE opus_wake_ = nullptr;  // UsesTimerWait: auto-reset, a packet came or Stop; made at the first such Start
  AudioJitterBuffer buffer_;
  double stream_latency_ms_ = 0, av_total_ms_ = 0;
  uint64_t av_samples_ = 0;
  std::thread thread_;
};
}
#endif
