#pragma once
// The user-session side of the WinFsp backend (`MultitapHost.exe --drive-fs`,
// started by the service with the session user's token). It owns one WinFsp
// file system per share and forwards every file operation to the service over
// stdin/stdout (drive_winfsp_wire.hpp). When the pipe closes - the service
// stopped, the session ended, or the service was killed - every file system is
// unmounted; if this process is killed instead, Windows closes the volume
// handles and the temporary drive-letter links go with them.
#include "drive_winfsp_fs.hpp"
#include "drive_mount.hpp"
#include <condition_variable>
#include <deque>
#include <functional>

namespace host::drive::fs {
// Backend over the pipe: one frame per call, answers matched by id.
class PipeBackend final : public Backend {
 public:
  using Write = std::function<bool(const std::vector<uint8_t>&)>;
  explicit PipeBackend(Write write) : write_(std::move(write)) {}
  ~PipeBackend() override { close(); }
  std::future<Result> submit(Call call) override {
    std::promise<Result> promise; auto future = promise.get_future();
    uint32_t id;
    { std::lock_guard lock(mutex_); if (closed_ || next_ == 0xffffffffu || pending_.size() >= wire::maxQueuedCalls) { Result r; r.error = closed_ || next_ == 0xffffffffu ? "drive_unavailable" : "drive_busy"; promise.set_value(std::move(r)); return future; } id = ++next_; pending_.emplace(id, std::move(promise)); }
    const auto bytes = wire::encode(wire::callFrame(id, call));
    bool sent;
    { std::lock_guard lock(writeMutex_); sent = write_(bytes); }
    if (!sent) fail(id);
    return future;
  }
  // A result frame from the service.
  void deliver(const wire::Frame& frame) {
    std::promise<Result> promise;
    { std::lock_guard lock(mutex_); const auto found = pending_.find(frame.id); if (found == pending_.end()) return; promise = std::move(found->second); pending_.erase(found); }
    Result result;
    if (!wire::resultParse(frame, result)) { result = Result{}; result.error = "drive_io_failed"; }
    promise.set_value(std::move(result));
  }
  void close() {
    std::map<uint32_t, std::promise<Result>> orphans;
    { std::lock_guard lock(mutex_); closed_ = true; orphans.swap(pending_); }
    for (auto& [id, promise] : orphans) { (void)id; Result r; r.error = "drive_unavailable"; promise.set_value(std::move(r)); }
  }
  size_t pending() { std::lock_guard lock(mutex_); return pending_.size(); }
 private:
  void fail(uint32_t id) {
    std::promise<Result> promise;
    { std::lock_guard lock(mutex_); const auto found = pending_.find(id); if (found == pending_.end()) return; promise = std::move(found->second); pending_.erase(found); }
    Result r; r.error = "drive_unavailable"; promise.set_value(std::move(r));
  }
  Write write_; std::mutex mutex_, writeMutex_; bool closed_ = false; uint32_t next_ = 0; std::map<uint32_t, std::promise<Result>> pending_;
};

// Explorer's label of a network drive comes from HKCU\...\MountPoints2\##<server>#<share>\_LabelFromReg
// (the same mechanism drive_mount.hpp uses for the WebDAV drive). Best effort, written before the
// letter appears; the helper runs as the user, so HKCU is the right hive.
inline std::wstring volumeLabelKey(const std::string& token) { return std::wstring(mountPointsKey) + L"\\" + labelKey(L"\\\\multitap\\" + wide(token)); }
inline void writeVolumeLabel(const std::string& token, const std::wstring& label) { RegSetKeyValueW(HKEY_CURRENT_USER, volumeLabelKey(token).c_str(), L"_LabelFromReg", REG_SZ, label.c_str(), DWORD((label.size() + 1) * sizeof(wchar_t))); }
inline void clearVolumeLabel(const std::string& token) { RegDeleteKeyW(HKEY_CURRENT_USER, volumeLabelKey(token).c_str()); }
// Label keys left by an earlier helper that was killed.
inline void sweepVolumeLabels() {
  HKEY key = nullptr; if (RegOpenKeyExW(HKEY_CURRENT_USER, mountPointsKey, 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) return;
  std::vector<std::wstring> stale; const std::wstring head = L"##multitap#" + wide(sharePrefix);
  for (DWORD index = 0; index < 4096; ++index) { wchar_t name[512]{}; DWORD size = 512; if (RegEnumKeyExW(key, index, name, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break; if (_wcsnicmp(name, head.c_str(), head.size()) == 0 && std::wstring(name).size() == head.size() + 43) stale.push_back(name); }
  for (const auto& name : stale) RegDeleteKeyW(key, name.c_str());
  RegCloseKey(key);
}

inline std::wstring currentUserSid() {
  HANDLE raw = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return {};
  WinHandle token(raw); DWORD size = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
  if (!size) return {};
  std::vector<BYTE> buffer(size);
  if (!GetTokenInformation(token.value, TokenUser, buffer.data(), size, &size)) return {};
  LPWSTR text = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) return {};
  std::wstring out = text; LocalFree(text); return out;
}

// The helper's command loop; transports are injected so the tests run it
// in-process against a fake WinFsp table.
class Helper {
 public:
  struct Transport { std::function<size_t(uint8_t*, size_t)> read; std::function<bool(const std::vector<uint8_t>&)> write; };
  struct Labels { std::function<void()> sweep; std::function<void(const std::string&, const std::wstring&)> set; std::function<void(const std::string&)> clear; };
  using Open = std::function<std::string(const std::wstring&)>;
  using CurrentMount = std::function<bool(const std::wstring&, const std::wstring&)>;
  Helper(const Api& api, Transport transport, std::wstring ownerSid, Labels labels = {}, Open open = openCurrentUserRoot, CurrentMount currentMount = currentOwnedDrive) : api_(api), transport_(std::move(transport)), ownerSid_(std::move(ownerSid)), backend_([this](const std::vector<uint8_t>& bytes) { return write(bytes); }), labels_(std::move(labels)), open_(std::move(open)), currentMount_(std::move(currentMount)) {}
  // Runs until the service closes the pipe or sends "stop". 0 = clean end.
  int run() {
    if (!api_.complete()) { send("{\"type\":\"unavailable\",\"error\":\"winfsp_missing\"}"); return 3; }
    if (labels_.sweep) labels_.sweep();
    send("{\"type\":\"ready\"}");
    std::thread reader([this] {
      wire::Frame frame;
      while (wire::readFrame(transport_.read, frame)) {
        if (frame.kind == wire::Kind::result) backend_.deliver(frame);
        else if (frame.kind == wire::Kind::control) { std::lock_guard lock(queueMutex_); controls_.push_back(frame.header); queue_.notify_all(); }
        else break;
      }
      backend_.close();
      std::lock_guard lock(queueMutex_); ended_ = true; queue_.notify_all();
    });
    for (;;) {
      std::string control;
      { std::unique_lock lock(queueMutex_); queue_.wait(lock, [this] { return ended_ || !controls_.empty(); }); if (controls_.empty()) break; control = std::move(controls_.front()); controls_.pop_front(); }
      if (!handle(control)) break;
    }
    // Revoke everything first, then let the reader finish.
    backend_.close();
    unmountAll();
    if (reader.joinable()) reader.join();
    return 0;
  }
  size_t mounted() { std::lock_guard lock(sharesMutex_); return shares_.size(); }
 private:
  const Api& api_; Transport transport_; std::wstring ownerSid_; PipeBackend backend_; Labels labels_;
  std::mutex queueMutex_, sharesMutex_, sendMutex_; std::condition_variable queue_; std::deque<std::string> controls_; bool ended_ = false;
  std::map<std::string, std::unique_ptr<ShareFs>> shares_;  // token -> volume
  std::map<std::string, std::wstring> letters_; Open open_; CurrentMount currentMount_;

  // Control and operation frames share one byte stream. Their entire frames
  // must use the same lock, including transports that write in small pieces.
  bool write(const std::vector<uint8_t>& bytes) { std::lock_guard lock(sendMutex_); return transport_.write(bytes); }
  void send(const std::string& json) { write(wire::encode(wire::controlFrame(json))); }
  bool handle(const std::string& text) {
    Json json; try { json = JsonParser::parse(text); } catch (...) { return true; }
    const auto type = json.get("type").string(), token = json.get("token").string();
    if (type == "stop") return false;
    if (type == "mount") {
      const auto id = json.get("id").string(), label = json.get("label").string(); const auto* readOnly = std::get_if<bool>(&json.get("readOnly").value);
      if (!shareToken(token) || !shareId(id) || !safeName(label, maxLabelUnits + 8) || !readOnly) { send("{\"type\":\"failed\",\"token\":" + jsonQuote(token) + ",\"error\":\"drive_map_failed\",\"status\":0}"); return true; }
      Options options; options.shareId = id; options.token = token; options.label = wide(label); options.readOnly = *readOnly; options.ownerSid = ownerSid_;
      auto volume = std::make_unique<ShareFs>(api_, backend_, std::move(options));
      if (labels_.set) labels_.set(token, volume->options().label);
      std::wstring letter; const NTSTATUS status = volume->mount(letter);
      if (NT_SUCCESS(status) && !letter.empty()) {
        { std::lock_guard lock(sharesMutex_); shares_[token] = std::move(volume); letters_[token] = letter; }
        send("{\"type\":\"mounted\",\"token\":" + jsonQuote(token) + ",\"letter\":\"" + utf8(letter) + "\"}");
      } else {
        volume.reset(); if (labels_.clear) labels_.clear(token);
        // No free letter -> the user can fix it; anything else is a mount failure (WebDAV takes over).
        const char* error = status == STATUS_NO_SUCH_DEVICE ? "drive_letter_unavailable" : "drive_map_failed";
        send("{\"type\":\"failed\",\"token\":" + jsonQuote(token) + ",\"error\":\"" + error + "\",\"status\":" + std::to_string(uint32_t(status)) + "}");
      }
      return true;
    }
    if (type == "open") {
      const auto* fields = std::get_if<Json::Object>(&json.value);
      const double id = json.get("id").number(0);
      if (!fields || fields->size() != 3 || !shareToken(token) || !std::isfinite(id) ||
          id < 1 || id > 4294967295.0 || std::floor(id) != id) return true;
      std::wstring root, volume;
      { std::lock_guard lock(sharesMutex_); if (shares_.count(token) && letters_.count(token)) {
          root = letters_.at(token) + L":\\"; volume = shares_.at(token)->ownedVolumeName();
        } }
      // Controls are serial: the exact volume remains owned during this call.
      // No mapping lock is held across COM. The helper's reader still serves file replies.
      std::string error = "drive_share_unknown";
      if (!root.empty()) {
        try { error = currentMount_(root, volume) ? open_(root) : "drive_open_unavailable"; }
        catch (...) { error = "drive_open_unknown"; }
      }
      send("{\"type\":\"opened\",\"id\":" + std::to_string(uint32_t(id)) + ",\"token\":" + jsonQuote(token) + ",\"error\":" + jsonQuote(error) + "}");
      return true;
    }
    if (type == "readOnly") { std::lock_guard lock(sharesMutex_); const auto found = shares_.find(token); if (found != shares_.end()) found->second->setReadOnly(json.get("value").boolean()); return true; }
    if (type == "unmount") {
      std::unique_ptr<ShareFs> volume;
      { std::lock_guard lock(sharesMutex_); const auto found = shares_.find(token); if (found != shares_.end()) { volume = std::move(found->second); shares_.erase(found); letters_.erase(token); } }
      if (volume) volume->unmount();
      volume.reset(); if (labels_.clear) labels_.clear(token);
      send("{\"type\":\"unmounted\",\"token\":" + jsonQuote(token) + "}");
    }
    return true;
  }
  void unmountAll() {
    std::map<std::string, std::unique_ptr<ShareFs>> all; { std::lock_guard lock(sharesMutex_); all.swap(shares_); letters_.clear(); }
    for (auto& [token, volume] : all) { volume->unmount(); if (labels_.clear) labels_.clear(token); }
  }
};

// Loads WinFsp the way FspLoad does, but from the registered install folder only
// (no PATH / current directory search) and returns the resolved table.
inline std::wstring winfspInstallDirectory() {
  wchar_t path[MAX_PATH]{}; DWORD size = sizeof(path);
  // winfsp.h: FSP_FSCTL_PRODUCT_FULL_REGKEY is the WOW6432Node key for x64 builds.
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"" FSP_FSCTL_PRODUCT_FULL_REGKEY, L"InstallDir", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) return {};
  return path;
}
inline std::wstring winfspLibraryPath() {
  auto directory = winfspInstallDirectory(); if (directory.empty()) return {};
  if (directory.back() != L'\\') directory += L'\\';
  return directory + L"bin\\" L"" FSP_FSCTL_PRODUCT_FILE_NAME L"-" L"" FSP_FSCTL_PRODUCT_FILE_ARCH L".dll";
}

#ifndef MULTITAP_WINFSP_NO_MAIN
// `MultitapHost.exe --drive-fs`: stdin/stdout are the pipes to the service.
inline int driveFsHelper() {
  try {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE || !output || output == INVALID_HANDLE_VALUE) return 2;
    const auto library = winfspLibraryPath();
    HMODULE module = library.empty() ? nullptr : LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    static Api api; api = resolveApi(module);
    Helper::Transport transport;
    transport.read = [input](uint8_t* buffer, size_t size) { DWORD count = 0; return ReadFile(input, buffer, DWORD(std::min<size_t>(size, 1 << 20)), &count, nullptr) ? size_t(count) : size_t(0); };
    transport.write = [output](const std::vector<uint8_t>& bytes) { size_t done = 0; while (done < bytes.size()) { DWORD count = 0; if (!WriteFile(output, bytes.data() + done, DWORD(std::min<size_t>(bytes.size() - done, 1 << 20)), &count, nullptr) || !count) return false; done += count; } return true; };
    Helper helper(api, std::move(transport), currentUserSid(), {sweepVolumeLabels, writeVolumeLabel, clearVolumeLabel});
    return helper.run();
  } catch (...) { return 1; }
}
#endif
}
