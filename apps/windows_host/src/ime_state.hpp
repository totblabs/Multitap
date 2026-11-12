#pragma once
// ime-state-v1. The browser Console types 한글 as 두벌식 keys over VNC and
// switches the VM's IME with its 한/영 key (Right Alt), so it must know the
// VM's real mode; VNC does not carry it. The Broker asks (host.ime.query) and
// this Host answers (host.ime.state) with the Korean IME state of the window
// that has the keyboard in the signed-in, unlocked console session:
// imeKorean true (한글), false (Latin letters), or no field when it cannot
// tell. Nothing else of the window (title, text, process) is read or sent.
//
// The reading is done by `MultitapHost.exe --ime-state`, started with the
// session user's own token on winsta0\default (ime_state_native.hpp); this
// service (session 0) cannot reach that desktop's windows. The helper answers
// one sample per question ('?' on stdin, one of k/e/u on stdout), so every
// answer is read at the moment of the question, never a remembered value.
// It is started on the first question and ended after a quiet minute, when
// the session changes or locks, and with the service.
#include "support.hpp"
#include <imm.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace host::ime {
inline constexpr char protocol[] = "ime-state-v1";
enum class Mode : uint8_t { unknown, korean, latin };

// What the helper read of the window that has the keyboard (pure, tested).
struct Reading {
  bool window = false;        // a focus window on the input desktop of this session
  bool reachable = false;     // not of a higher integrity level than the helper (UIPI)
  LANGID language = 0;        // its thread's keyboard layout language
  bool openKnown = false, open = false;
  bool conversionKnown = false; DWORD conversion = 0;
};
// Korean layout: 한글 only when the IME is open and in Hangul (native) mode;
// either answer missing is unknown. Another layout types Latin letters when
// its IME (if any) is closed; an open Japanese/Chinese IME is unknown.
inline Mode decide(const Reading& r) {
  if (!r.window || !r.reachable) return Mode::unknown;
  if (PRIMARYLANGID(r.language) == LANG_KOREAN) {
    if (!r.openKnown || !r.conversionKnown) return Mode::unknown;
    return r.open && (r.conversion & IME_CMODE_HANGUL) ? Mode::korean : Mode::latin;
  }
  if (!r.openKnown) return Mode::unknown;
  return r.open ? Mode::unknown : Mode::latin;
}
inline char modeLetter(Mode mode) { return mode == Mode::korean ? 'k' : mode == Mode::latin ? 'e' : 'u'; }
inline std::optional<Mode> modeFromLetter(char letter) {
  if (letter == 'k') return Mode::korean;
  if (letter == 'e') return Mode::latin;
  if (letter == 'u') return Mode::unknown;
  return std::nullopt;
}

// {"type":"host.ime.query","protocol":"ime-state-v1","id":<uuid>} exactly.
inline std::optional<std::string> queryId(const Json& message) {
  const auto* fields = std::get_if<Json::Object>(&message.value);
  if (!fields || fields->size() != 3 || message.get("type").string() != "host.ime.query" || message.get("protocol").string() != protocol) return std::nullopt;
  const auto id = message.get("id").string();
  if (id.size() != 36 || id.find_first_not_of("0123456789abcdef-") != std::string::npos) return std::nullopt;
  return id;
}
inline std::string statePacket(const std::string& id, Mode mode) {
  return "{\"type\":\"host.ime.state\",\"protocol\":\"" + std::string(protocol) + "\",\"id\":" + jsonQuote(id) +
    (mode == Mode::unknown ? std::string() : std::string(",\"imeKorean\":") + (mode == Mode::korean ? "true" : "false")) + '}';
}

// One running helper. ask: one fresh sample, or nullopt when the helper did
// not answer in time or ended (the channel is then discarded).
struct Channel {
  virtual ~Channel() = default;
  virtual std::optional<Mode> ask(uint32_t timeoutMs) = 0;
};
struct WatchAccess {
  // The signed-in, unlocked console session, or nullopt.
  std::function<std::optional<DWORD>()> session;
  std::function<std::unique_ptr<Channel>(DWORD)> start;
  std::function<uint64_t()> now = [] { return GetTickCount64(); };
  // hostAction 296 (it shows why when a Console types only Latin
  // letters): stage 1 no signed-in unlocked console session, 2 the
  // helper could not start (error = Win32 error), 3 a helper gave no answer,
  // 4 answered (format 0 unknown, 1 한글, 2 Latin). Fixed numbers only.
  std::function<void(uint32_t stage, uint32_t error, uint32_t format)> record;
};
inline constexpr uint32_t firstAnswerMs = 1000, answerMs = 300;
inline constexpr uint64_t idleMs = 60000;

class Watch {
 public:
  explicit Watch(WatchAccess access) : access_(std::move(access)) {}
  Mode query() {
    const auto session = access_.session();
    if (!session) { reset(); note(1, 0, 0); return Mode::unknown; }
    lastUse_ = access_.now();
    if (channel_ && session_ != *session) reset();
    // An existing helper that no longer answers (it ended with a sign-out, say)
    // is replaced once; a new one that does not answer is unknown.
    for (int attempt = 0; attempt < 2; ++attempt) {
      bool started = false;
      if (!channel_) {
        DWORD startError = 0;
        try { channel_ = access_.start(*session); } catch (...) { startError = GetLastError(); channel_.reset(); }
        if (!channel_) { note(2, startError, 0); return Mode::unknown; }
        session_ = *session; started = true;
      }
      std::optional<Mode> answer;
      try { answer = channel_->ask(started ? firstAnswerMs : answerMs); } catch (...) { answer.reset(); }
      if (answer) {
        // A sample taken while the session locked or changed is not this session's.
        const auto again = access_.session();
        if (!again || *again != session_) { reset(); note(1, 0, 0); return Mode::unknown; }
        note(4, 0, *answer == Mode::korean ? 1u : *answer == Mode::latin ? 2u : 0u);
        return *answer;
      }
      reset();
      note(3, 0, 0);
      if (started) return Mode::unknown;
    }
    return Mode::unknown;
  }
  // Ends the helper after a quiet minute or when its session is gone.
  void idle() {
    if (!channel_) return;
    const auto session = access_.session();
    if (!session || *session != session_ || access_.now() - lastUse_ >= idleMs) reset();
  }
  void reset() { channel_.reset(); session_ = 0; }
  bool running() const { return channel_ != nullptr; }
 private:
  void note(uint32_t stage, uint32_t error, uint32_t format) { if (access_.record) try { access_.record(stage, error, format); } catch (...) {} }
  WatchAccess access_;
  std::unique_ptr<Channel> channel_;
  DWORD session_ = 0;
  uint64_t lastUse_ = 0;
};

// The questions of the Broker, answered one at a time on this thread (never on
// the channel's receiving thread). The Broker asks one at a time per Host, so
// more than a few waiting are refused (that question times out as unknown).
class Service {
 public:
  using Send = std::function<void(const std::string&)>;
  explicit Service(WatchAccess access) : watch_(std::move(access)) {}
  ~Service() { stop(); }
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;
  bool post(std::string id, Send send) {
    std::lock_guard lock(mutex_);
    if (stopping_ || queue_.size() >= 4) return false;
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    queue_.push_back({std::move(id), std::move(send)}); wake_.notify_one(); return true;
  }
  void stop() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    wake_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
  }
 private:
  struct Item { std::string id; Send send; };
  void run() {
    std::unique_lock lock(mutex_);
    for (;;) {
      wake_.wait_for(lock, std::chrono::seconds(5), [&] { return stopping_ || !queue_.empty(); });
      if (stopping_) break;
      if (queue_.empty()) { lock.unlock(); try { watch_.idle(); } catch (...) {} lock.lock(); continue; }
      auto item = std::move(queue_.front()); queue_.pop_front(); lock.unlock();
      Mode mode = Mode::unknown;
      try { mode = watch_.query(); } catch (...) { mode = Mode::unknown; }
      try { item.send(statePacket(item.id, mode)); } catch (...) {}
      lock.lock();
    }
    lock.unlock(); watch_.reset();
  }
  Watch watch_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Item> queue_;
  bool stopping_ = false;
  std::thread thread_;
};
}  // namespace host::ime
