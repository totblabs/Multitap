#pragma once
// Role::secure helper: its command line and the reader of the service's input
// pipe. Kept apart from the entry (which needs host::RemoteInput) so the tests
// exercise both against the service's real pipe writer (secure_desktop_launch.hpp).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "json_object.hpp"
#include "pipe_packet.hpp"
#include "secure_desktop_helper.hpp"
#include "secure_desktop_ipc.hpp"

namespace host::secure::entry {
struct Command {
  uint32_t session = 0;
  OwnedOutput output;
  std::string sid;
  HANDLE section = nullptr, event = nullptr, input = nullptr, worker = nullptr;
};

inline std::optional<Command> parseCommand(int argc, const wchar_t* const* argv) {
  Command c;
  bool haveSession = false, haveSid = false, haveAdapter = false, haveRect = false;
  const auto handle = [](const wchar_t* value) -> HANDLE {
    wchar_t* end = nullptr;
    const unsigned long long number = _wcstoui64(value, &end, 10);
    if (!end || *end || end == value || number < 4 || number > 0xffffffffull) return nullptr;
    return reinterpret_cast<HANDLE>(uintptr_t(number));
  };
  for (int i = 1; i < argc; ++i) {
    const std::wstring key = argv[i];
    if (i == 1 && key == L"--secure-desktop-helper-v1") continue;
    if (i + 1 >= argc) return {};
    const wchar_t* value = argv[++i];
    if (key == L"--session") {
      c.session = uint32_t(_wtoi64(value));
      haveSession = !!c.session && c.session != 0xffffffffu;
    } else if (key == L"--sid") {
      const std::wstring text = value;
      if (text.size() < 8 || text.size() > 184 || text.rfind(L"S-1-", 0) != 0 || text.find_first_not_of(L"S-0123456789") != std::wstring::npos) return {};
      c.sid.clear();
      for (const wchar_t ch : text) c.sid.push_back(char(ch));  // S, - and digits only (checked above)
      haveSid = true;
    } else if (key == L"--adapter") {
      const std::wstring text = value;
      const auto colon = text.find(L':');
      if (colon == std::wstring::npos) return {};
      c.output.adapterLow = uint32_t(wcstoul(text.substr(0, colon).c_str(), nullptr, 10));
      c.output.adapterHigh = int32_t(wcstol(text.substr(colon + 1).c_str(), nullptr, 10));
      haveAdapter = true;
    } else if (key == L"--rect") {
      int l = 0, t = 0, w = 0, h = 0;
      if (swscanf_s(value, L"%d,%d,%d,%d", &l, &t, &w, &h) != 4 || w <= 0 || h <= 0 || !ipc::sizeValid(uint32_t(w), uint32_t(h))) return {};
      c.output.left = l; c.output.top = t; c.output.width = uint32_t(w); c.output.height = uint32_t(h);
      haveRect = true;
    } else if (key == L"--section") { c.section = handle(value);
    } else if (key == L"--event") { c.event = handle(value);
    } else if (key == L"--input") { c.input = handle(value);
    } else if (key == L"--worker") { c.worker = handle(value);
    } else {
      return {};
    }
  }
  if (!haveSession || !haveSid || !haveAdapter || !haveRect || !c.section || !c.event || !c.input || !c.worker) return {};
  return c;
}

// Reads the service's framed input messages into a bounded queue the capture
// thread drains (injection happens on that thread, which is the one attached to
// the Winlogon desktop). The end of the pipe, or anything that is not a small
// text packet, closes it: the helper then exits.
class InputReader {
 public:
  explicit InputReader(HANDLE pipe) : pipe_(pipe) {
    thread_ = std::thread([this] {
      bool text = false;
      std::vector<uint8_t> bytes;
      while (!stop_ && readPipePacket(pipe_, text, bytes, stop_)) {
        if (!text || bytes.empty() || bytes.size() > 65536) break;
        try {
          auto value = JsonParser::parse(std::string(bytes.begin(), bytes.end()));
          std::lock_guard<std::mutex> lock(mutex_);
          // A stalled helper drops pointer moves first; presses, releases and resets are kept.
          const bool move = value.get("type").string() == "input" && value.get("event").string() == "move";
          if (queue_.size() < (move ? softLimit : hardLimit)) queue_.push_back(std::move(value));
          ready_.notify_one();
        } catch (...) {
          break;
        }
      }
      closed_ = true;
      ready_.notify_all();
    });
  }
  ~InputReader() {
    stop_ = true;
    CancelIoEx(pipe_, nullptr);
    if (thread_.joinable()) thread_.join();
  }
  InputReader(const InputReader&) = delete;
  InputReader& operator=(const InputReader&) = delete;
  std::optional<Json> pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return {};
    auto value = std::move(queue_.front());
    queue_.pop_front();
    return value;
  }
  // One message, waiting up to milliseconds for it (the system injector's loop; the
  // secure-desktop helper polls with pop() between captures).
  std::optional<Json> wait(uint32_t milliseconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return !queue_.empty() || closed_.load(); });
    if (queue_.empty()) return {};
    auto value = std::move(queue_.front());
    queue_.pop_front();
    return value;
  }
  bool closed() const { return closed_; }

 private:
  static constexpr size_t softLimit = 256, hardLimit = 2048;
  HANDLE pipe_;
  std::atomic<bool> stop_{false}, closed_{false};
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Json> queue_;
  std::thread thread_;
};

// ---- system injector (system_input.hpp) ---------------------------------------
// MultitapHost.exe --system-input-v1 --session N --sid S --grants B --input H --worker H --status H
// grants: bit 0 relative mouse, bit 1 text-input-v1, bit 2 touch-input-v1 (the
// session's own grants, as the worker applies them). The three handles are
// inherited: the read end of the service's input pipe, a wait handle of the
// stream's user worker, and a 64-byte section for the counts.
struct InjectorCommand {
  uint32_t session = 0, grants = 0;
  std::string sid;
  HANDLE input = nullptr, worker = nullptr, status = nullptr;
};
inline std::optional<InjectorCommand> parseInjectorCommand(int argc, const wchar_t* const* argv) {
  InjectorCommand c;
  bool haveSession = false, haveSid = false, haveGrants = false;
  const auto handle = [](const wchar_t* value) -> HANDLE {
    wchar_t* end = nullptr;
    const unsigned long long number = _wcstoui64(value, &end, 10);
    if (!end || *end || end == value || number < 4 || number > 0xffffffffull) return nullptr;
    return reinterpret_cast<HANDLE>(uintptr_t(number));
  };
  for (int i = 1; i < argc; ++i) {
    const std::wstring key = argv[i];
    if (i == 1 && key == L"--system-input-v1") continue;
    if (i + 1 >= argc) return {};
    const wchar_t* value = argv[++i];
    if (key == L"--session") {
      c.session = uint32_t(_wtoi64(value));
      haveSession = !!c.session && c.session != 0xffffffffu;
    } else if (key == L"--sid") {
      const std::wstring text = value;
      if (text.size() < 8 || text.size() > 184 || text.rfind(L"S-1-", 0) != 0 || text.find_first_not_of(L"S-0123456789") != std::wstring::npos) return {};
      c.sid.clear();
      for (const wchar_t ch : text) c.sid.push_back(char(ch));
      haveSid = true;
    } else if (key == L"--grants") {
      wchar_t* end = nullptr;
      const unsigned long number = wcstoul(value, &end, 10);
      if (!end || *end || end == value || number > 7) return {};
      c.grants = uint32_t(number);
      haveGrants = true;
    } else if (key == L"--input") { c.input = handle(value);
    } else if (key == L"--worker") { c.worker = handle(value);
    } else if (key == L"--status") { c.status = handle(value);
    } else {
      return {};
    }
  }
  if (!haveSession || !haveSid || !haveGrants || !c.input || !c.worker || !c.status) return {};
  return c;
}

}  // namespace host::secure::entry