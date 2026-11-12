#include "remote_audio.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
using Microsoft::WRL::ComPtr;
namespace multitap {
uint64_t RemoteAudio::NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
void RemoteAudio::Start(bool opus) {
  Stop(); error_ = S_OK; played_frames_ = 0; received_frames_ = 0;
  { std::lock_guard<std::mutex> lock(mutex_); buffer_.ResetCounters(); av_total_ms_ = 0; av_samples_ = 0; stream_latency_ms_ = 0; pushed_ = false; }
  {
    std::lock_guard<std::mutex> decoding(decode_mutex_); opus_ = opus;
    // Without a decoder the video goes on silent, as without an audio endpoint.
    if (opus_ && !decoder_.Open()) { error_ = E_FAIL; return; }
  }
  if (UsesTimerWait(opus)) {
    // Kept until destruction: a receive thread may still be signalling it.
    if (!opus_wake_) opus_wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!opus_wake_) { error_ = E_OUTOFMEMORY; return; }
    ResetEvent(opus_wake_);
  }
  running_ = true; thread_ = std::thread([this] { Run(); });
}
void RemoteAudio::Stop() {
  running_ = false; wake_.notify_all(); if (opus_wake_) SetEvent(opus_wake_); if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lock(mutex_); buffer_.Clear();
}
bool RemoteAudio::Push(uint64_t pts_us, const uint8_t* bytes, size_t size) {
  std::lock_guard<std::mutex> decoding(decode_mutex_);
  if (opus_) {
    if (!decoder_.ready()) return true;  // Start reported the failure; video goes on.
    const auto before = decoder_.counters();
    const bool ok = decoder_.Decode(pts_us, bytes, size, [this](uint64_t pts, const uint8_t* pcm, size_t pcm_size) { Accept(pts, pcm, pcm_size); });
    // A concealed number came: late (its time was played) or after a pause.
    const auto& after = decoder_.counters();
    if (running_ && (after.late_concealed != before.late_concealed || after.pauses != before.pauses)) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (after.late_concealed != before.late_concealed) buffer_.NoteLate(NowUs());
      if (after.pauses != before.pauses) buffer_.NotePause();
    }
    return ok;
  }
  if (!AudioJitterBuffer::Valid(size)) return false;
  Accept(pts_us, bytes, size); return true;
}
void RemoteAudio::Accept(uint64_t pts_us, const uint8_t* bytes, size_t size) {
  received_frames_ += size / AudioJitterBuffer::kFrameBytes;
  if (!running_ || FAILED(error_)) return;  // Video can continue without an audio endpoint.
  std::lock_guard<std::mutex> lock(mutex_);
  buffer_.Push(pts_us, bytes, size, NowUs());
  // Push holds decode_mutex_, which guards opus_.
  if (UsesTimerWait(opus_)) SetEvent(opus_wake_); else { pushed_ = true; wake_.notify_one(); }
}
void RemoteAudio::NoteVideo(uint64_t pts_us) {
  if (!running_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto heard = buffer_.PlayingPtsUs(NowUs());
  if (!heard) return;
  const double offset_ms = double(int64_t(pts_us) - *heard) / 1000.0;
  // Both PTS count from the same Host capture start; anything this far apart is no estimate.
  if (offset_ms < -5000 || offset_ms > 5000) return;
  av_total_ms_ += offset_ms; ++av_samples_;
}
void RemoteAudio::AppendStats(flutter::EncodableMap& stats) {
  using V = flutter::EncodableValue;
  {
    // audio-opus-v1 only: 10 ms frames made up by Opus PLC, before a packet
    // that showed a gap and ahead of a device about to run dry.
    std::lock_guard<std::mutex> decoding(decode_mutex_);
    if (opus_) stats[V("audioConcealed")] = V(int64_t(decoder_.counters().concealed + decoder_.counters().ahead));
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto& c = buffer_.counters();
  const double depth = buffer_.level() / 48.0;
  // Depth and latency mean something only while sound plays (not while the VM is silent).
  stats[V("audioPlaying")] = V(buffer_.playing());
  stats[V("audioDepthMs")] = V(depth);
  stats[V("audioTargetMs")] = V(buffer_.target() / 48.0);
  // Arrival here -> heard: what waits in the buffer and the device, plus the
  // shared-mode engine's own stream latency (GetStreamLatency).
  stats[V("audioLatencyMs")] = V(depth + stream_latency_ms_);
  stats[V("audioUnderruns")] = V(int64_t(c.underruns));
  stats[V("audioOverruns")] = V(int64_t(c.overruns));
  stats[V("audioInsertedFrames")] = V(int64_t(c.inserted_frames));
  stats[V("audioTrimmedFrames")] = V(int64_t(c.trimmed_frames));
  // Video PTS handed to the texture minus the audio PTS being heard at that moment (ms, summed).
  stats[V("audioAvOffsetTotalMs")] = V(av_total_ms_);
  stats[V("audioAvOffsetSamples")] = V(int64_t(av_samples_));
}
namespace {
// Windows' default playback device changed (headphones plugged in, a Bluetooth
// headset connected, the user picked another output): the render thread
// reopens on the new default (otherwise sound stops for the rest of the
// session). Registered for the thread's lifetime only; no reference count.
class DefaultOutputWatch final : public IMMNotificationClient {
 public:
  std::atomic<bool> changed{false};
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient)) { *out = static_cast<IMMNotificationClient*>(this); return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 1; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && role == eConsole) changed = true;
    return S_OK;
  }
  STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
  STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};
}  // namespace

void RemoteAudio::Run() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com)) { error_ = com; running_ = false; return; }
  {
    ComPtr<IMMDeviceEnumerator> devices;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices));
    if (FAILED(hr)) { error_ = hr; running_ = false; CoUninitialize(); return; }
    DefaultOutputWatch watch;
    const bool watching = SUCCEEDED(devices->RegisterEndpointNotificationCallback(&watch));
    // Opus (UsesTimerWait): every packet wakes this thread and between packets
    // a high-resolution timer looks at the device every 5 ms on time; a plain
    // wait may stretch to ~16 ms, longer than a device about to run dry holds.
    const bool timer_wait = UsesTimerWait(opus_);
    HANDLE timer = nullptr;
    if (timer_wait) {
      timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);  // before Windows 10 1803
    }
    // One pass per output device. A failed open (no device yet) or a device
    // that goes away waits a second and tries the current default again, so
    // sound comes back by itself; nothing is kept for the gap.
    while (running_) {
      ComPtr<IMMDevice> device; ComPtr<IAudioClient> client; ComPtr<IAudioRenderClient> render;
      watch.changed = false;
      hr = devices->GetDefaultAudioEndpoint(eRender, eConsole, &device);
      if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
      WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2;
      format.nSamplesPerSec = 48000; format.wBitsPerSample = 16; format.nBlockAlign = 4; format.nAvgBytesPerSec = 192000;
      if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
          AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
          1000000, 0, &format, nullptr);
      UINT32 capacity = 0;
      if (SUCCEEDED(hr)) hr = client->GetBufferSize(&capacity);
      if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
      REFERENCE_TIME latency = 0;
      if (SUCCEEDED(hr) && SUCCEEDED(client->GetStreamLatency(&latency))) { std::lock_guard<std::mutex> lock(mutex_); stream_latency_ms_ = double(latency) / 10000.0; }
      if (SUCCEEDED(hr)) hr = client->Start();
      // Received audio is kept only while a device plays it (Accept drops it on a failed error_).
      error_ = hr;
      if (SUCCEEDED(hr)) {
        std::vector<uint8_t> scratch(size_t(capacity) * AudioJitterBuffer::kFrameBytes);
        while (SUCCEEDED(hr) && running_ && !watch.changed) {
          std::unique_lock<std::mutex> decoding(decode_mutex_, std::defer_lock);
          std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
          if (!timer_wait) {
            lock.lock();
            // Every packet wakes this thread; between packets the device is looked at
            // every 5 ms (Windows' timer may stretch that to ~16 ms; the device holds
            // the target, 30 ms or more, while playing).
            wake_.wait_for(lock, std::chrono::milliseconds(5), [this] { return !running_ || pushed_; });
            pushed_ = false;
            if (!running_) break;
          } else {
            LARGE_INTEGER due; due.QuadPart = -50000;
            if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
              const HANDLE wakes[2] = {opus_wake_, timer};
              WaitForMultipleObjects(2, wakes, FALSE, INFINITE);
            } else WaitForSingleObject(opus_wake_, 5);
            if (!running_) break;
            // Concealment decodes: the decoder first, as Push takes them.
            decoding.lock(); lock.lock();
          }
          UINT32 padding = 0; hr = client->GetCurrentPadding(&padding);
          if (FAILED(hr)) break;
          // Opus packets late or lost and the device about to run dry: Opus PLC
          // keeps it fed (bounded and fading, OpusAudioDecoder::Conceal). PCM has none.
          if (opus_) {
            while (buffer_.Starving(padding) && decoder_.Conceal([this](uint64_t pts, const uint8_t* pcm, size_t pcm_size) { buffer_.PushConcealed(pts, pcm, pcm_size); })) {}
          }
          const UINT32 frames = buffer_.Pull(scratch.data(), capacity - std::min(capacity, padding), padding, NowUs());
          lock.unlock();
          if (decoding.owns_lock()) decoding.unlock();
          if (!frames) continue;
          BYTE* output = nullptr; hr = render->GetBuffer(frames, &output);
          if (FAILED(hr)) break;
          std::memcpy(output, scratch.data(), size_t(frames) * AudioJitterBuffer::kFrameBytes);
          hr = render->ReleaseBuffer(frames, 0);
          if (SUCCEEDED(hr)) played_frames_ += frames;
        }
        client->Stop();
      }
      if (!running_) break;
      // The device went away or another became the default: what was buffered
      // belonged to the old one; start the new one from what arrives next.
      error_ = FAILED(hr) ? hr : AUDCLNT_E_DEVICE_INVALIDATED;
      { std::lock_guard<std::mutex> lock(mutex_); buffer_.Clear(); }
      if (watch.changed) continue;  // a new default is there now
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock, std::chrono::seconds(1), [this, &watch] { return !running_ || watch.changed.load(); });
    }
    if (watching) devices->UnregisterEndpointNotificationCallback(&watch);
    if (timer) CloseHandle(timer);
  }
  running_ = false; CoUninitialize();
}
}
