// Several open remote windows (backlog #62): the real RemoteMediaSession,
// RemoteEngine and Windows CPU H.264 decoder, one owner per simulated session
// window, each fed its own copy of a stream from the Microsoft encoder MFT at 60
// packets per second. Every window has its own registrar fixture with one
// "raster thread" that takes each marked frame once and copies its pixels (a
// model of Flutter's pixel-buffer upload: Flutter, its engine and the GPU are
// not run). The picture's top-left blocks carry the frame number, so a take
// says how old the shown picture is.
//
//   (default)  checks at 640x360: a hidden window hands Flutter nothing, keeps
//              decoding, keeps a recent picture for the thumbnail, and shows the
//              newest picture at once when it is shown again.
//   bench      cost per window at 1920x1080: process CPU, private bytes and
//              threads for 1, 2 and 3 windows, all shown and all but one hidden.
//              `seconds=N` per reading (default 6), `repeats=N` (default 3),
//              `windows=N` only that many windows.
//
// Built against an engine without hidden windows (RemoteMediaSession::SetHidden
// absent) the bench measures "hidden" as before: the window keeps everything,
// once with Flutter taking every frame and once without (Flutter's own
// behaviour for a minimized window is not measured here).
// No VM, relay, display, audio device or input is used.
#include "remote_media_session.h"
#include "../../../../native/windows_video/color.hpp"
#include <codecapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <wmcodecdsp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using V = flutter::EncodableValue;
using Clock = std::chrono::steady_clock;
namespace {
void require(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error(reason); }
void check(HRESULT hr, const std::string& reason) { require(SUCCEEDED(hr), reason + " (" + std::to_string(uint32_t(hr)) + ")"); }

// Hidden windows exist only in an engine that has them; the same source builds
// against the engine before the change to measure it.
template <class T> auto setHidden(T& owner, bool hidden, int) -> decltype(owner.SetHidden(hidden), bool()) { owner.SetHidden(hidden); return true; }
template <class T> bool setHidden(T&, bool, long) { return false; }
template <class T> bool hiddenApi(T& owner) { return setHidden(owner, false, 0); }

// The frame number, in kBits blocks of kBlock x kBlock luma on the first rows.
constexpr uint32_t kBlock = 32, kBits = 8;
struct Unit { std::vector<uint8_t> bytes; bool key = false; };
std::vector<Unit> encode(uint32_t width, uint32_t height, unsigned count, uint32_t bitrate) {
  ComPtr<IMFTransform> mft;
  check(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft)), "H.264 encoder MFT");
  ComPtr<ICodecAPI> codec; mft.As(&codec);
  const auto set = [&](const GUID& key, ULONG value, bool boolean = false) {
    if (!codec) return; VARIANT v; VariantInit(&v); v.vt = VARTYPE(boolean ? VT_BOOL : VT_UI4);
    if (boolean) v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE; else v.ulVal = value;
    codec->SetValue(&key, &v);
  };
  set(CODECAPI_AVLowLatencyMode, 1, true); set(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
  set(CODECAPI_AVEncMPVDefaultBPictureCount, 0); set(CODECAPI_AVEncMPVGOPSize, 3600);
  ComPtr<IMFMediaType> output; check(MFCreateMediaType(&output), "Output type");
  output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  output->SetUINT32(MF_MT_AVG_BITRATE, bitrate); MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height);
  MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, 60, 1); MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
  check(mft->SetOutputType(0, output.Get(), 0), "Encoder output type");
  ComPtr<IMFMediaType> input; check(MFCreateMediaType(&input), "Input type");
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height); MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 60, 1);
  MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1); input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  check(mft->SetInputType(0, input.Get(), 0), "Encoder input type");
  check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Encoder begin");
  check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Encoder start");
  const auto nal = [](const std::vector<uint8_t>& b, uint8_t type) { for (size_t i = 0; i + 3 < b.size(); ++i) if (!b[i] && !b[i + 1] && b[i + 2] == 1 && (b[i + 3] & 31) == type) return true; return false; };
  std::vector<uint8_t> headers;
  std::vector<Unit> units;
  for (unsigned n = 0; n < count; ++n) {
    // A moving desktop-like picture: soft gradients that scroll, plus the number.
    std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
      nv12[size_t(y) * width + x] = uint8_t(60 + ((x + n * 6) / 4 + (y + n * 2) / 6) % 120 + (((x ^ y) + n) & 7));
    for (uint32_t b = 0; b < kBits; ++b) for (uint32_t y = 0; y < kBlock; ++y) for (uint32_t x = 0; x < kBlock; ++x)
      nv12[size_t(y) * width + b * kBlock + x] = (n >> b) & 1 ? 235 : 16;
    for (size_t i = size_t(width) * height; i < nv12.size(); ++i) nv12[i] = uint8_t(118 + (i / 64 + n) % 20);
    for (uint32_t y = 0; y < kBlock / 2; ++y) for (uint32_t x = 0; x < kBits * kBlock; ++x) nv12[size_t(width) * height + size_t(y) * width + x] = 128;
    if (!n) set(CODECAPI_AVEncVideoForceKeyFrame, 1);
    ComPtr<IMFMediaBuffer> buffer; check(MFCreateMemoryBuffer(DWORD(nv12.size()), &buffer), "Raw buffer");
    BYTE* target = nullptr; check(buffer->Lock(&target, nullptr, nullptr), "Lock raw"); std::memcpy(target, nv12.data(), nv12.size()); buffer->Unlock(); buffer->SetCurrentLength(DWORD(nv12.size()));
    ComPtr<IMFSample> sample; check(MFCreateSample(&sample), "Raw sample"); sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(LONGLONG(n) * 166667); sample->SetSampleDuration(166667);
    check(mft->ProcessInput(0, sample.Get(), 0), "Encode input");
    Unit unit;
    for (;;) {
      MFT_OUTPUT_STREAM_INFO info{}; check(mft->GetOutputStreamInfo(0, &info), "Encoder output info");
      ComPtr<IMFSample> out; ComPtr<IMFMediaBuffer> memory;
      check(MFCreateSample(&out), "Encoded sample"); check(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 4 * 1024 * 1024), &memory), "Encoded buffer"); out->AddBuffer(memory.Get());
      MFT_OUTPUT_DATA_BUFFER data{}; data.pSample = out.Get(); DWORD status = 0;
      const HRESULT hr = mft->ProcessOutput(0, 1, &data, &status);
      if (data.pEvents) data.pEvents->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
      check(hr, "Encode output");
      ComPtr<IMFMediaBuffer> bytes; check(out->ConvertToContiguousBuffer(&bytes), "Encoded bytes");
      BYTE* p = nullptr; DWORD length = 0; check(bytes->Lock(&p, nullptr, &length), "Lock encoded"); unit.bytes.insert(unit.bytes.end(), p, p + length); bytes->Unlock();
    }
    require(!unit.bytes.empty(), "The low-latency encoder held a picture back");
    unit.key = nal(unit.bytes, 5);
    if (unit.key && !(nal(unit.bytes, 7) && nal(unit.bytes, 8))) {
      if (headers.empty()) {
        ComPtr<IMFMediaType> type; check(mft->GetOutputCurrentType(0, &type), "Encoder current type");
        UINT32 length = 0; check(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &length), "Sequence header size");
        headers.resize(length); check(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, headers.data(), length, &length), "Sequence header");
      }
      unit.bytes.insert(unit.bytes.begin(), headers.begin(), headers.end());
    }
    units.push_back(std::move(unit));
  }
  mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
  require(units.front().key, "The stream does not start with an IDR");
  return units;
}
// The legacy single-output envelope; the PTS counts every packet ever sent.
std::vector<uint8_t> legacy(const Unit& unit, uint64_t pts) {
  std::vector<uint8_t> media(12 + unit.bytes.size());
  media[0] = 1; media[1] = unit.key ? 1 : 0; std::memcpy(media.data() + 4, &pts, 8);
  std::copy(unit.bytes.begin(), unit.bytes.end(), media.begin() + 12);
  return media;
}
int number(const uint8_t* rgba, uint32_t width) {
  int value = 0;
  for (uint32_t b = 0; b < kBits; ++b) if (rgba[(size_t(kBlock / 2) * width + b * kBlock + kBlock / 2) * 4] > 128) value |= 1 << b;
  return value;
}

// One window's Flutter engine as the RemoteEngine sees it: a registrar and a
// raster thread that takes each marked texture once and copies its pixels.
struct Window final : flutter::TextureRegistrar {
  std::mutex mutex;
  std::condition_variable wake;
  std::map<int64_t, flutter::TextureVariant*> textures;
  std::set<int64_t> marked;
  int64_t next = 0;
  bool stop = false;
  std::atomic<bool> draws{true};  // false: a Flutter that draws nothing for a minimized window
  std::atomic<uint64_t> marks{0}, taken{0};
  std::atomic<DWORD> worker{0};  // the engine thread that marks frames (its decode/convert thread)
  std::atomic<int> shown{-1};      // frame number of the last picture taken
  std::atomic<int64_t> shownAt{0};  // when (steady clock, microseconds)
  int64_t drawing = 0;  // the texture the raster thread uses outside the lock
  std::vector<uint8_t> upload;
  std::thread raster;
  Window() : raster([this] { Run(); }) {}
  ~Window() { { std::lock_guard<std::mutex> lock(mutex); stop = true; } wake.notify_all(); raster.join(); }
  int64_t RegisterTexture(flutter::TextureVariant* texture) override { std::lock_guard<std::mutex> lock(mutex); textures.emplace(++next, texture); return next; }
  // Like Flutter: only queues the request, never waits for a draw.
  bool MarkTextureFrameAvailable(int64_t id) override {
    ++marks; if (!worker) worker = GetCurrentThreadId();
    std::lock_guard<std::mutex> lock(mutex);
    if (!textures.count(id)) return false;
    marked.insert(id); wake.notify_all(); return true;
  }
  // Like Flutter: the texture goes once the raster thread no longer uses it.
  void UnregisterTexture(int64_t id, std::function<void()> callback) override {
    { std::unique_lock<std::mutex> lock(mutex); textures.erase(id); marked.erase(id); wake.wait(lock, [&] { return drawing != id; }); }
    if (callback) callback();
  }
  bool UnregisterTexture(int64_t id) override { UnregisterTexture(id, {}); return true; }
  void Run() {
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
      wake.wait(lock, [&] { return stop || !marked.empty(); });
      if (stop) return;
      const int64_t id = *marked.begin(); marked.erase(marked.begin());
      const auto found = textures.find(id);
      auto* pixels = draws && found != textures.end() ? std::get_if<flutter::PixelBufferTexture>(found->second) : nullptr;
      if (!pixels) continue;
      drawing = id; lock.unlock();
      if (const auto* buffer = pixels->CopyPixelBuffer(0, 0)) {
        const size_t bytes = buffer->width * buffer->height * 4;
        if (upload.size() != bytes) upload.resize(bytes);
        std::memcpy(upload.data(), buffer->buffer, bytes);
        shown = number(buffer->buffer, uint32_t(buffer->width));
        shownAt = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        ++taken;
        if (buffer->release_callback) buffer->release_callback(buffer->release_context);
      }
      lock.lock(); drawing = 0; wake.notify_all();
    }
  }
};

int64_t integer(const flutter::EncodableMap& map, const char* key) {
  const auto it = map.find(V(key)); require(it != map.end(), std::string("Missing statistic ") + key);
  if (const auto* value = std::get_if<int64_t>(&it->second)) return *value;
  if (const auto* value = std::get_if<int32_t>(&it->second)) return *value;
  throw std::runtime_error("Statistic is not an integer");
}
double real(const flutter::EncodableMap& map, const char* key) {
  const auto it = map.find(V(key)); require(it != map.end(), std::string("Missing statistic ") + key);
  if (const auto* value = std::get_if<double>(&it->second)) return *value;
  return double(integer(map, key));
}

// A session window: its owner, its window fixture and a feeder at 60 packets/s.
struct Session {
  Window window;
  multitap::RemoteMediaSession owner{&window};
  int64_t lease = 0, texture = -1;
  std::atomic<bool> feeding{false};
  std::atomic<int> sent{-1};       // frame number of the newest packet the engine accepted
  std::atomic<uint64_t> refused{0};
  std::atomic<bool> rejected{false};
  std::thread feeder;
  std::mutex ui;  // the dispatcher is a UI-thread object: one caller at a time
  ~Session() { Stop(); }
  void Start(int64_t id, uint32_t width, uint32_t height, const std::vector<Unit>& units) {
    lease = id;
    std::lock_guard<std::mutex> lock(ui);
    check(owner.Start(lease, width, height, false, texture), "Start");
    feeding = true;
    feeder = std::thread([this, &units] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
      auto due = Clock::now(); uint64_t packet = 0;
      while (feeding) {
        const auto& unit = units[packet % units.size()];
        const auto bytes = legacy(unit, 1000 + packet * 16667);
        for (;;) {
          HRESULT hr; { std::lock_guard<std::mutex> lock(ui); hr = owner.Push(lease, bytes); }
          if (hr == S_OK) break;
          if (hr != multitap::kMediaQueueFull) { rejected = true; return; }
          ++refused; if (!feeding) return;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        sent = int(packet % units.size());
        ++packet; due += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(due);
      }
    });
  }
  void Stop() {
    feeding = false; if (feeder.joinable()) feeder.join();
    std::lock_guard<std::mutex> lock(ui); owner.Close();
  }
  flutter::EncodableMap Stats() {
    require(!rejected, "Packet rejected");
    std::lock_guard<std::mutex> lock(ui); flutter::EncodableMap value; check(owner.Stats(lease, value), "Stats"); return value;
  }
  bool Hide(bool hidden) { std::lock_guard<std::mutex> lock(ui); return setHidden(owner, hidden, 0); }
  std::shared_ptr<const multitap::DecodedFrame> Latest() { std::lock_guard<std::mutex> lock(ui); int64_t at = 0; uint64_t serial = 0; return owner.Latest(at, serial); }
};

struct Usage { double cpu = 0; int64_t privateBytes = 0, threads = 0; };
double seconds(FILETIME kernel, FILETIME user) {
  const auto value = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) / 1e7; };
  return value(kernel) + value(user);
}
double cpuSeconds() {
  FILETIME created{}, exited{}, kernel{}, user{};
  GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
  return seconds(kernel, user);
}
// One thread's CPU time: unlike the whole process (the CPU decoder's own pool
// threads spin more or less with this PC's other load), it repeats closely.
double threadSeconds(DWORD id) {
  const HANDLE thread = id ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id) : nullptr;
  if (!thread) return 0;
  FILETIME created{}, exited{}, kernel{}, user{};
  const double value = GetThreadTimes(thread, &created, &exited, &kernel, &user) ? seconds(kernel, user) : 0;
  CloseHandle(thread);
  return value;
}
Usage usage() {
  Usage value; value.cpu = cpuSeconds();
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
      if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++value.threads;
    CloseHandle(snapshot);
  }
  PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
  value.privateBytes = int64_t(memory.PrivateUsage);
  return value;
}
int64_t nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }
// Frames the shown picture is older than the newest sent packet (0: that one or a
// later one decoded meanwhile); -1 unknown.
int behind(int sent, int shown, size_t loop) {
  if (sent < 0 || shown < 0) return -1;
  const int difference = int((sent - shown + int(loop)) % int(loop));
  return difference > int(loop) / 2 ? 0 : difference;
}
void waitFor(const std::function<bool()>& done, int seconds, const std::string& reason) {
  const auto end = Clock::now() + std::chrono::seconds(seconds);
  while (!done()) { require(Clock::now() < end, reason); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}

// Checks (default): what a hidden window does and how it comes back.
void checks() {
  const auto units = encode(640, 360, 120, 1500000);
  Session session;
  session.Start(1, 640, 360, units);
  // Without hidden windows the hide below changes nothing and the checks fail.
  waitFor([&] { return session.window.taken >= 30; }, 30, "The shown window took no pictures");
  // Hidden: Flutter is asked for nothing, decoding goes on.
  session.Hide(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto marks = session.window.marks.load(), taken = session.window.taken.load();
  const auto before = session.Stats();
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  const auto after = session.Stats();
  require(session.window.marks == marks, "A hidden window asked Flutter for frames (" + std::to_string(session.window.marks - marks) + ")");
  require(session.window.taken == taken, "Flutter took frames of a hidden window");
  const int64_t decoded = integer(after, "decodedFrames") - integer(before, "decodedFrames");
  require(decoded >= 100, "A hidden window stopped decoding (" + std::to_string(decoded) + " in 2.5 s)");
  // Most pictures are decoded but not converted to RGBA while hidden.
  const int64_t deferred = integer(after, "hiddenDeferredFrames") - integer(before, "hiddenDeferredFrames");
  require(deferred >= decoded * 3 / 4, "A hidden window converted its pictures (" + std::to_string(deferred) + " of " + std::to_string(decoded) + " deferred)");
  // The thumbnail's picture stays recent (about one per second is converted).
  const auto latest = session.Latest();
  require(latest && !latest->rgba.empty(), "No recent picture while hidden");
  const int age = behind(session.sent, number(latest->rgba.data(), latest->width), units.size());
  require(age >= 0 && age <= 75, "The hidden window's picture is " + std::to_string(age) + " frames old");
  // Shown again: the newest picture at once (not the last one shown, not the next packet).
  const int sent = session.sent;
  const int64_t revealed = nowUs();
  require(session.Hide(false), "Show");
  waitFor([&] { return session.window.taken > taken; }, 5, "A shown window got no picture");
  const int64_t ms = (session.window.shownAt - revealed) / 1000;
  const int old = behind(sent, session.window.shown, units.size());
  std::cout << "{\"check\":\"reveal\",\"ms\":" << ms << ",\"framesBehind\":" << old << ",\"hiddenDecoded\":" << decoded << ",\"hiddenDeferred\":" << deferred << "}" << std::endl;
  require(old >= 0 && old <= 3, "The shown picture is " + std::to_string(old) + " frames old");
  require(ms <= 100, "The shown window waited " + std::to_string(ms) + " ms for its picture");
  // And it keeps going.
  const auto resumed = session.window.taken.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  require(session.window.taken >= resumed + 20, "A shown window stopped getting pictures");
  // A stream that starts while its window is hidden starts hidden.
  require(session.Hide(true), "Hide again");
  { std::lock_guard<std::mutex> lock(session.ui); session.feeding = false; }
  session.feeder.join();
  int64_t texture = -1;
  { std::lock_guard<std::mutex> lock(session.ui); check(session.owner.Start(2, 640, 360, false, texture), "Hidden start"); }
  session.lease = 2;
  const auto hiddenMarks = session.window.marks.load();
  for (size_t i = 0; i < 30; ++i) { std::lock_guard<std::mutex> lock(session.ui); while (session.owner.Push(2, legacy(units[i], 1000 + i * 16667)) == multitap::kMediaQueueFull) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  waitFor([&] { return integer(session.Stats(), "decodedFrames") >= 30; }, 30, "The hidden start decoded nothing");
  require(session.window.marks == hiddenMarks, "A stream started in a hidden window asked Flutter for frames");
  require(session.Hide(false), "Show the hidden start");
  waitFor([&] { return session.window.shown == 29; }, 5, "The hidden start did not show its newest picture");
  session.Stop();
  std::cout << "{\"kind\":\"multi-session-checks\",\"ok\":true,\"actualWindowsCpuDecoder\":true,\"flutterRegistrarFixture\":true,\"actualVm\":false}" << std::endl;
}

// Bench: cost per window, every window shown, then all but the first hidden,
// alternating `repeats` times (this PC's other load moves single readings).
// Per window it reads its engine thread (decode calls, conversion, publication)
// and its raster thread fixture; the CPU decoder's own pool threads only show
// in the process total.
void bench(int seconds, int repeats, int only) {
  const uint32_t width = 1920, height = 1080;
  const auto units = encode(width, height, 120, 12000000);
  size_t bytes = 0; for (const auto& unit : units) bytes += unit.bytes.size();
  std::cout << "{\"stream\":\"" << width << "x" << height << "@60\",\"packets\":" << units.size() << ",\"kbps\":" << bytes * 8 / 2 / 1000 << "}" << std::endl;
  const auto idle = usage();
  std::cout << "{\"windows\":0,\"privateMB\":" << idle.privateBytes / 1048576.0 << ",\"threads\":" << idle.threads << "}" << std::endl;
  {
    // The per-picture work a hidden window changes, on this thread's CPU time:
    // RGBA conversion and the modelled Flutter upload (both skipped), the NV12
    // copy (added).
    std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2, 90), rgba(size_t(width) * height * 4), copy(rgba.size()), kept(nv12.size());
    const auto time = [&](const std::function<void()>& work) {
      // At least a second of this thread's CPU: the clock counts in timer ticks.
      const DWORD self = GetCurrentThreadId(); const double began = threadSeconds(self);
      int count = 0; double spent = 0;
      while ((spent = threadSeconds(self) - began) < 1.0) { work(); ++count; }
      return 1000 * spent / count;
    };
    const double convert = time([&] { multitap::video::nv12ToRgba(nv12.data(), rgba.data(), width, height, width, height, false, false); });
    const double upload = time([&] { std::memcpy(copy.data(), rgba.data(), rgba.size()); });
    const double keep = time([&] { std::memcpy(kept.data(), nv12.data(), nv12.size()); });
    std::cout << std::fixed << std::setprecision(2) << "{\"perPictureCpuMs\":{\"rgbaConvert\":" << convert << ",\"uploadModel\":" << upload << ",\"nv12Copy\":" << keep
              << "},\"savedPerHiddenWindowPctOfOneCoreAt60\":" << 60 * (convert + upload - keep) / 10 << ",\"check\":" << int(copy[rgba.size() / 2] ^ kept[nv12.size() / 2]) << "}" << std::endl;
  }
  struct Mode { const char* name; bool hide, flutterDraws; };
  const auto median = [](std::vector<double> values) { std::sort(values.begin(), values.end()); return values.empty() ? 0.0 : values[values.size() / 2]; };
  for (int count = only ? only : 1; count <= (only ? only : 3); ++count) {
    std::vector<std::unique_ptr<Session>> sessions;
    for (int i = 0; i < count; ++i) { sessions.push_back(std::make_unique<Session>()); sessions.back()->Start(i + 1, width, height, units); }
    waitFor([&] { return std::all_of(sessions.begin(), sessions.end(), [](const auto& s) { return s->window.worker != 0; }); }, 30, "No window got a frame");
    const bool api = hiddenApi(sessions.front()->owner);
    std::vector<Mode> modes{{"shown", false, true}};
    if (count > 1) {
      if (api) modes.push_back({"hidden", true, true});
      else { modes.push_back({"hidden(flutter draws)", false, true}); modes.push_back({"hidden(flutter idle)", false, false}); }
    }
    // Per mode: process CPU, the shown window's threads, one hidden window's threads (% of one core).
    std::map<std::string, std::vector<double>> process, shownThreads, hiddenThreads, privateMb, threads;
    for (int repeat = 0; repeat < repeats; ++repeat) for (const auto& mode : modes) {
      for (int i = 1; i < count; ++i) { sessions[i]->window.draws = mode.flutterDraws; if (api) sessions[i]->Hide(mode.hide); }
      std::this_thread::sleep_for(std::chrono::seconds(2));
      std::vector<flutter::EncodableMap> first; for (auto& s : sessions) first.push_back(s->Stats());
      std::vector<uint64_t> takes; std::vector<double> worker, raster;
      for (auto& s : sessions) { takes.push_back(s->window.taken); worker.push_back(threadSeconds(s->window.worker)); raster.push_back(threadSeconds(GetThreadId(s->window.raster.native_handle()))); }
      const auto began = usage(); const auto start = Clock::now();
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      const auto ended = usage(); const double wall = std::chrono::duration<double>(Clock::now() - start).count();
      std::ostringstream row;
      row << std::fixed << std::setprecision(1) << "{\"windows\":" << count << ",\"mode\":\"" << mode.name << "\",\"processCpuPct\":" << 100 * (ended.cpu - began.cpu) / wall
          << ",\"privateMB\":" << ended.privateBytes / 1048576.0 << ",\"threads\":" << ended.threads << ",\"perWindow\":[";
      for (int i = 0; i < count; ++i) {
        const auto& s = *sessions[i];
        const auto last = sessions[i]->Stats();
        const double engine = 100 * (threadSeconds(s.window.worker) - worker[i]) / wall, draw = 100 * (threadSeconds(GetThreadId(sessions[i]->window.raster.native_handle())) - raster[i]) / wall;
        const double decodedFps = (integer(last, "decodedFrames") - integer(first[i], "decodedFrames")) / wall;
        row << (i ? "," : "") << "{\"engineThreadPct\":" << engine << ",\"rasterThreadPct\":" << draw << ",\"decodedFps\":" << decodedFps << ",\"flutterTakesFps\":" << (s.window.taken - takes[i]) / wall << "}";
        if (i == 0) shownThreads[mode.name].push_back(engine + draw);
        if (i == count - 1 && count > 1) hiddenThreads[mode.name].push_back(engine + draw);
      }
      row << "],\"hiddenApi\":" << (api ? "true" : "false") << "}";
      std::cout << row.str() << std::endl;
      process[mode.name].push_back(100 * (ended.cpu - began.cpu) / wall);
      privateMb[mode.name].push_back(ended.privateBytes / 1048576.0); threads[mode.name].push_back(double(ended.threads));
    }
    for (const auto& mode : modes) {
      std::cout << std::fixed << std::setprecision(1) << "{\"summary\":true,\"windows\":" << count << ",\"mode\":\"" << mode.name << "\",\"processCpuPctMedian\":" << median(process[mode.name])
                << ",\"processCpuPctMin\":" << *std::min_element(process[mode.name].begin(), process[mode.name].end()) << ",\"processCpuPctMax\":" << *std::max_element(process[mode.name].begin(), process[mode.name].end())
                << ",\"firstWindowThreadsPct\":" << median(shownThreads[mode.name]) << ",\"lastWindowThreadsPct\":" << (count > 1 ? median(hiddenThreads[mode.name]) : 0.0)
                << ",\"privateMB\":" << median(privateMb[mode.name]) << ",\"threads\":" << median(threads[mode.name]) << ",\"readings\":" << repeats << "}" << std::endl;
    }
    if (api && count > 1) {
      // Showing a hidden window again: how soon and how fresh its picture is.
      auto& s = *sessions.back();
      const auto taken = s.window.taken.load(); const int sent = s.sent; const int64_t revealed = nowUs();
      s.Hide(false);
      waitFor([&] { return s.window.taken > taken; }, 5, "A shown window got no picture");
      std::cout << "{\"windows\":" << count << ",\"reveal\":{\"ms\":" << (s.window.shownAt - revealed) / 1000.0 << ",\"framesBehind\":" << behind(sent, s.window.shown, units.size()) << "}}" << std::endl;
    }
    for (auto& s : sessions) s->Stop();
    uint64_t refused = 0; for (auto& s : sessions) refused += s->refused;
    if (refused) std::cout << "{\"windows\":" << count << ",\"fullQueueWaits\":" << refused << "}" << std::endl;
  }
  std::cout << "{\"kind\":\"multi-session-bench\",\"actualWindowsCpuDecoder\":true,\"flutterRasterModel\":\"one memcpy per taken frame\",\"flutterEngine\":false,\"gpu\":false,\"actualVm\":false}" << std::endl;
}
}  // namespace

int main(int argc, char** argv) {
  // This PC may be someone's desktop.
  SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
  bool measure = false; int seconds = 6, repeats = 3, only = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "bench") == 0) measure = true;
    if (std::strncmp(argv[i], "seconds=", 8) == 0) seconds = std::max(2, std::atoi(argv[i] + 8));
    if (std::strncmp(argv[i], "repeats=", 8) == 0) repeats = std::max(1, std::atoi(argv[i] + 8));
    if (std::strncmp(argv[i], "windows=", 8) == 0) only = std::clamp(std::atoi(argv[i] + 8), 1, 3);
  }
  int code = 0;
  try {
    check(MFStartup(MF_VERSION), "Media Foundation");
    if (measure) bench(seconds, repeats, only); else checks();
    MFShutdown();
  } catch (const std::exception& error) {
    std::cerr << "multi session: " << error.what() << std::endl;
    code = 2;
  }
  CoUninitialize();
  return code;
}
