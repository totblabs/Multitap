#pragma once
// drive-v1 for one stream session (docs/drive-redirection.md): the relay
// channel, the loopback WebDAV endpoint, the request multiplexer and the
// session user's drive letters. On drive.ready the endpoint starts; every
// drive.shares list is applied (new share: fresh token + mapping; removed:
// token revoked at once, then unmapped; read-only changes take effect at
// once). When the channel ends for any reason (revocation, session end,
// violation, network) every token is revoked first and every drive unmapped.
#include "drive_requests.hpp"
#include "drive_channel.hpp"
#include "drive_mount.hpp"
#include "drive_winfsp_service.hpp"
#include "operational_log.hpp"

namespace host::drive {
struct MountResult { std::string token, letter, error; uint32_t win32 = 0; };  // win32: the Windows error behind a failed mapping (diagnostics only)
class Link {
 public:
  virtual ~Link() = default;
  virtual void start(const Credentials& credentials, const std::string& sessionId, const std::string& url, Channel::Text text, Channel::Binary binary, Channel::Closed closed) = 0;
  virtual bool sendText(std::string text) = 0;
  virtual bool sendBinary(std::vector<uint8_t> data) = 0;
  virtual void abort() = 0;  // ends the channel without waiting (callbacks may call it)
  virtual bool refused() const { return false; }  // channel-resume-v1: the Broker refused the last dial
  virtual void stop() = 0;   // ends it and waits for its thread
};
class Mounter {
 public:
  virtual ~Mounter() = default;
  virtual std::string prepare() = 0;
  virtual bool oldLimit() { return false; }  // after prepare(): WebClient runs with the old 50 MB file cap
  virtual void sweep(DWORD session) = 0;
  virtual std::vector<MountResult> map(DWORD session, uint16_t port, const std::vector<std::pair<std::string, std::string>>& shares) = 0;
  virtual std::string openRoot(DWORD, uint16_t, const std::string&) { return "drive_open_unavailable"; }
  virtual void unmap(DWORD session, const std::vector<std::string>& tokens) = 0;
  // Default hooks own no capability; only WindowsMounter holds a native permit.
  virtual void closeForwardAdmission() {}
  virtual void releaseCleanupOwner() {}
};
class ChannelLink final : public Link {
 public:
  void start(const Credentials& credentials, const std::string& sessionId, const std::string& url, Channel::Text text, Channel::Binary binary, Channel::Closed closed) override { channel_.start(credentials, sessionId, url, std::move(text), std::move(binary), std::move(closed)); }
  bool sendText(std::string text) override { return channel_.send(std::move(text)); }
  bool sendBinary(std::vector<uint8_t> data) override { return channel_.sendBinary(std::move(data)); }
  void abort() override { channel_.abort(); }
  bool refused() const override { return channel_.refused(); }
  void stop() override { channel_.stop(); }
 private:
  Channel channel_;
};
class WindowsMounter final : public Mounter {
 public:
  explicit WindowsMounter(std::unique_ptr<WebClientSystem> system = std::make_unique<WindowsWebClient>()) : system_(std::move(system)),cleanupOwner_(service_workers::admitCurrentDriveCleanupOwner()) {}
  void closeForwardAdmission() override {forwardClosed_=true;}
  void releaseCleanupOwner() override {cleanupClosed_=true;cleanupOwner_.reset();}
  std::string prepare() override {
    if(forwardClosed_)return "drive_helper_failed";
    const auto outcome = prepareWebClient(*system_);
    oldLimit_ = outcome.oldLimit;
    recordHostEvent("hostAction", {{"action", diagnosticAction}, {"stage", outcome.stage}, {"ok", outcome.error.empty() && !outcome.oldLimit}});
    return outcome.error;
  }
  bool oldLimit() override { return oldLimit_; }
  void sweep(DWORD session) override { if(forwardClosed_)return;try { (void)runDriveMountHelper(session, "{\"op\":\"sweep\"}", 10000); } catch (...) { } }
  std::vector<MountResult> map(DWORD session, uint16_t port, const std::vector<std::pair<std::string, std::string>>& shares) override {
    std::vector<MountResult> out;
    if(forwardClosed_){for(const auto& item:shares)out.push_back({item.first,{},"drive_map_failed"});return out;}
    std::string list;
    for (const auto& [token, label] : shares) { if (!list.empty()) list += ','; list += "{\"token\":" + jsonQuote(token) + ",\"label\":" + jsonQuote(label) + '}'; }
    try {
      const auto answer = runDriveMountHelper(session, "{\"op\":\"map\",\"port\":" + std::to_string(port) + ",\"shares\":[" + list + "]}", 20000);
      const auto* results = std::get_if<Json::Array>(&answer.get("results").value);
      for (const auto& [token, label] : shares) {
        (void)label; MountResult result; result.token = token; result.error = "drive_map_failed";
        if (results) for (const auto& item : *results) if (item.get("token").string() == token) {
          const auto letter = item.get("letter").string(), error = item.get("error").string();
          if (letter.size() == 1 && letter[0] >= 'D' && letter[0] <= 'Z') { result.letter = letter; result.error.clear(); }
          else if (error == "drive_letter_unavailable") result.error = error;
          const double win32 = item.get("win32").number(0);
          if (std::isfinite(win32) && win32 > 0 && win32 < 4294967295.0) result.win32 = uint32_t(win32);
        }
        out.push_back(std::move(result));
      }
    } catch (const std::exception& failure) {
      const std::string code = failure.what() == std::string("no_interactive_session") ? "no_interactive_session" : "drive_map_failed";
      for (const auto& [token, label] : shares) { (void)label; out.push_back({token, {}, code}); }
    }
    return out;
  }
  std::string openRoot(DWORD session, uint16_t port, const std::string& token) override {
    if(forwardClosed_)return "drive_open_unavailable";
    if (!shareToken(token) || port < 1024) return "drive_share_unknown";
    try {
      const auto answer = runDriveMountHelper(session, "{\"op\":\"open\",\"port\":" + std::to_string(port) + ",\"token\":" + jsonQuote(token) + "}", 20000);
      const auto* object = std::get_if<Json::Object>(&answer.value);
      if (!object || object->size() != 1 || !std::holds_alternative<std::string>(answer.get("error").value)) return "drive_open_unknown";
      const auto error = answer.get("error").string();
      return error.empty() || openErrorCode(error) ? error : "drive_open_unknown";
    } catch (const std::exception& failure) {
      return failure.what() == std::string("no_interactive_session") ? "no_interactive_session" : "drive_open_unknown";
    }
  }
  void unmap(DWORD session, const std::vector<std::string>& tokens) override {
    if(cleanupClosed_)return;
    if(cleanupOwner_){try{(void)runDriveUnmountHelper(session,tokens,*cleanupOwner_);}catch(...){}return;}
    std::string list; for (const auto& token : tokens) { if (!list.empty()) list += ','; list += jsonQuote(token); }
    try { (void)runDriveMountHelper(session, "{\"op\":\"unmap\",\"tokens\":[" + list + "]}", 10000); } catch (...) { }
  }
 private:
  std::unique_ptr<WebClientSystem> system_; bool oldLimit_ = false;
  std::optional<service_workers::DriveCleanupPermit> cleanupOwner_;
  std::atomic<bool> forwardClosed_{false},cleanupClosed_{false};
};

class Redirection {
 public:
  // `fsMounter`: the WinFsp backend (null = WebDAV only). A session uses it only when the broker granted driveFs.
  explicit Redirection(std::unique_ptr<Link> link = std::make_unique<ChannelLink>(), std::unique_ptr<Mounter> mounter = std::make_unique<WindowsMounter>(), std::chrono::milliseconds timeout = std::chrono::seconds(65), std::unique_ptr<fs::FsMounter> fsMounter = nullptr)
      : link_(std::move(link)), mounter_(std::move(mounter)), timeout_(timeout), fs_(std::move(fsMounter)) { if (fs_) fs_->onLost([this] { record(16, false); violation("drive_helper_failed"); }); }
  ~Redirection() { closeSchedulingAndJoin(); }
  Redirection(const Redirection&) = delete; Redirection& operator=(const Redirection&) = delete;
  void start(const Credentials& credentials, const std::string& sessionId, const std::string& url, DWORD session, bool fs = false) {
    std::lock_guard lifecycle(lifecycle_);
    if(permanentlyClosing_)throw std::runtime_error("drive_helper_failed");
    stop();
    credentials_ = credentials; url_ = url; fsWanted_ = fs;
    { std::lock_guard lock(mutex_); closeReason_.clear(); }
    { std::lock_guard lock(mutex_); sessionId_ = sessionId; session_ = session; fsRequested_ = fs && fs_ != nullptr; shares_.clear(); removed_.clear(); pendingOpen_.reset(); lastOpenId_ = 0; openEnabled_ = false; ready_ = closed_ = stopping_ = prepared_ = notices_ = false; noticed_.clear(); prepareError_.clear(); dirty_ = false; }
    requests_ = std::make_unique<Requests>([this](std::string text) { return link_->sendText(std::move(text)); }, [this](std::vector<uint8_t> data) { return link_->sendBinary(std::move(data)); }, timeout_);
    dav_ = std::make_unique<DavServer>(*requests_, timeout_);
    dav_->onTooLarge([this](const std::string&) { notice("drive_file_too_large", 12); });
    worker_ = std::thread([this] { work(); });
    running_ = true;
    try { link_->start(credentials, sessionId, url, [this](std::string text) { receive(text); }, [this](std::vector<uint8_t> data) { if (!requests_->receiveBinary(data)) violation("drive_invalid_message"); }, [this] { closed(); }); }
    catch (...) { closed(); }
  }
  // channel-resume-v1 (channel_redial.hpp): a fresh pair for the same session after the channel ended for a
  // transport reason. Nothing of the ended pair is kept (its tokens were revoked and its drives unmapped when
  // it ended); the client offers its folders again under the same ids and names and they are mapped as new.
  void redial() {
    std::lock_guard lifecycle(lifecycle_);
    if (url_.empty()) throw std::runtime_error("drive_not_started");
    std::string sessionId; DWORD session;
    { std::lock_guard lock(mutex_); sessionId = sessionId_; session = session_; }
    const auto credentials = credentials_; const auto url = url_;
    start(credentials, sessionId, url, session, fsWanted_);
  }
  // Told when the channel ended by itself (never by stop()). `transient`: the same session may pair again.
  // Called on the channel's thread (or inside start() when the dial could not begin); it must not block.
  void onEnded(std::function<void(bool transient)> hook) { std::lock_guard lock(mutex_); ended_ = std::move(hook); }
  // Ends the channel, revokes every token, unmaps every drive, then returns.
  void stop() {
    std::lock_guard lifecycle(lifecycle_);
    if (!running_) return;
    { std::lock_guard lock(mutex_); stopping_ = true; }
    wake_.notify_all();
    link_->stop();
    closed();
    if (worker_.joinable()) worker_.join();
    if (dav_) dav_->stop();
    dav_.reset(); requests_.reset(); running_ = false;
  }
  void closeSchedulingAndJoin(){
    std::lock_guard lifecycle(lifecycle_);permanentlyClosing_=true;mounter_->closeForwardAdmission();
    stop(); // actual drive worker joins before its native unmap owner is released
    mounter_->releaseCleanupOwner();
  }
  uint16_t port() { std::lock_guard lock(mutex_); return port_; }
  // Test observation: share id -> letter (mapped) or error.
  std::map<std::string, std::string> state() { std::lock_guard lock(mutex_); std::map<std::string, std::string> out; for (const auto& [id, share] : shares_) out[id] = share.state == State::mapped ? share.letter : share.state == State::failed ? share.error : "pending"; return out; }
  std::string token(const std::string& id) { std::lock_guard lock(mutex_); const auto found = shares_.find(id); return found == shares_.end() ? std::string() : found->second.token; }
 private:
  enum class State { pending, mapping, mapped, failed };
  struct OpenJob { uint32_t id = 0; std::string share, token; bool started = false; };
  std::optional<OpenJob> pendingOpen_; uint32_t lastOpenId_ = 0; bool openEnabled_ = false;
  struct Share { std::string id, label, token, letter, error; bool readOnly = false, viaFs = false, roApplied = false; State state = State::pending; };
  std::unique_ptr<Link> link_; std::unique_ptr<Mounter> mounter_; std::chrono::milliseconds timeout_; std::unique_ptr<fs::FsMounter> fs_; bool fsRequested_ = false;
  std::recursive_mutex lifecycle_; std::mutex mutex_; std::condition_variable wake_; std::thread worker_; bool running_ = false,permanentlyClosing_=false;
  std::unique_ptr<Requests> requests_; std::unique_ptr<DavServer> dav_;
  std::string sessionId_; DWORD session_ = 0xffffffff; uint16_t port_ = 0;
  Credentials credentials_; std::string url_; bool fsWanted_ = false; std::function<void(bool)> ended_;  // channel-resume-v1: what redial() dials again, and who hears of an end
  std::map<std::string, Share> shares_; std::vector<std::string> removed_; // tokens to unmap
  bool ready_ = false, closed_ = false, stopping_ = false, prepared_ = false, dirty_ = false, notices_ = false; std::set<std::string> noticed_; std::string prepareError_, closeReason_;
  // `cause`: the Windows error behind a failed mapping (WNetAddConnection2), when known.
  static void record(uint32_t stage, bool ok, int64_t expected = -1, int64_t missing = -1, const std::string& code = {}, uint32_t cause = 0) {
    if (expected >= 0 && missing >= 0 && cause) recordHostEvent("hostAction", {{"action", diagnosticAction}, {"stage", stage}, {"ok", ok}, {"expected", expected}, {"missing", missing}, {"error", code.empty() ? 0 : diagnosticCode(code)}, {"cause", int64_t(cause)}});
    else if (expected >= 0 && missing >= 0) recordHostEvent("hostAction", {{"action", diagnosticAction}, {"stage", stage}, {"ok", ok}, {"expected", expected}, {"missing", missing}, {"error", code.empty() ? 0 : diagnosticCode(code)}});
    else recordHostEvent("hostAction", {{"action", diagnosticAction}, {"stage", stage}, {"ok", ok}, {"error", code.empty() ? 0 : diagnosticCode(code)}});
  }
  // A one-time hint for the person at the PC (once per session and code). The
  // broker announces `notices` in drive.ready when it can pass it on; an older
  // broker would close the pair on an unknown message, so nothing is sent then.
  void notice(const std::string& code, uint32_t stage) {
    bool fresh = false, send = false;
    { std::lock_guard lock(mutex_); if (!closed_ && !stopping_ && noticed_.insert(code).second) { fresh = true; send = notices_; } }
    if (fresh && stage == 12) record(stage, false);
    if (send) link_->sendText("{\"type\":\"drive.notice\",\"code\":" + jsonQuote(code) + "}");
  }
  void violation(const std::string& code) { { std::lock_guard lock(mutex_); if (closeReason_.empty()) closeReason_ = code; } link_->abort(); }
  void receive(const std::string& text) {
    Json value;
    try { value = JsonParser::parse(text); } catch (...) { violation("drive_invalid_message"); return; }
    const auto type = value.get("type").string();
    if (type == "drive.response") { if (!requests_->receive(value)) violation("drive_invalid_message"); return; }
    if (type == "drive.error") { const auto reason = value.get("reason").string(); { std::lock_guard lock(mutex_); if (closeReason_.empty() && reason.size() <= 64) closeReason_ = reason; } link_->abort(); return; }
    if (type == "drive.ready") {
      const auto& limits = value.get("limits");
      if (value.get("protocol").string() != protocol || limits.get("chunkBytes").number() != double(chunkBytes) || limits.get("maxInFlight").number() != double(maxInFlight)) { violation("drive_invalid_message"); return; }
      bool valid = false;
      { std::lock_guard lock(mutex_); valid = !ready_ && value.get("sessionId").string() == sessionId_; ready_ = true; openEnabled_ = value.get("openProtocol").string() == openProtocol; if (const auto* flag = std::get_if<bool>(&value.get("notices").value)) notices_ = *flag; }
      if (!valid) { violation("drive_invalid_message"); return; }
      try { const auto port = dav_->start(); std::lock_guard lock(mutex_); port_ = port; }
      catch (...) { violation("drive_server_failed"); return; }
      record(1, true);
      { std::lock_guard lock(mutex_); dirty_ = true; }
      wake_.notify_all(); return;
    }
    if (type == "drive.open") {
      uint32_t id = 0; std::string share, error;
      if (!openRequest(value, id, share)) { violation("drive_invalid_message"); return; }
      bool valid = false;
      {
        std::lock_guard lock(mutex_);
        valid = openEnabled_ && ready_ && !closed_ && !stopping_ && id > lastOpenId_;
        if (valid) {
          lastOpenId_ = id;
          const auto found = shares_.find(share);
          if (pendingOpen_) error = "drive_busy";
          else if (found == shares_.end() || found->second.state != State::mapped) error = "drive_share_unknown";
          else { pendingOpen_ = OpenJob{id, share, found->second.token, false}; dirty_ = true; }
        }
      }
      if (!valid) { violation("drive_invalid_message"); return; }
      if (!error.empty()) link_->sendText(openResponse(id, share, error));
      else wake_.notify_all();
      return;
    }
    if (type == "drive.shares") {
      const auto* list = std::get_if<Json::Array>(&value.get("shares").value);
      std::map<std::string, std::pair<std::string, bool>> wanted;
      if (!list || list->size() > maxShares) { violation("drive_invalid_message"); return; }
      for (const auto& item : *list) {
        const auto id = item.get("id").string(), name = item.get("name").string(); const auto* readOnly = std::get_if<bool>(&item.get("readOnly").value);
        if (!shareId(id) || !safeName(name, maxLabelUnits) || !readOnly || wanted.count(id)) { violation("drive_invalid_message"); return; }
        wanted[id] = {name, *readOnly};
      }
      bool accepted = false;
      {
        std::lock_guard lock(mutex_);
        accepted = ready_ && !closed_;
        if (accepted) {
        for (auto it = shares_.begin(); it != shares_.end();) {
          if (wanted.count(it->first)) { ++it; continue; }
          // Access ends now; the letter goes with the next worker pass.
          dav_->removeShare(it->second.token); removed_.push_back(it->second.token); it = shares_.erase(it);
        }
        for (const auto& [id, item] : wanted) {
          auto found = shares_.find(id);
          if (found == shares_.end()) { Share share; share.id = id; share.label = item.first; share.readOnly = item.second; share.token = std::string(sharePrefix) + randomToken(); found = shares_.emplace(id, std::move(share)).first; }
          found->second.readOnly = item.second;
          dav_->setShare(found->second.token, {id, item.second});
        }
        dirty_ = true;
        }
      }
      if (!accepted) { violation("drive_invalid_message"); return; }
      wake_.notify_all(); return;
    }
    violation("drive_invalid_message");
  }
  // The channel ended (or never started): revoke every token at once.
  void closed() {
    std::string reason; std::function<void(bool)> hook;
    { std::lock_guard lock(mutex_); if (closed_) return; closed_ = true; reason = closeReason_.empty() ? (stopping_ ? "session_ended" : "drive_transport_closed") : closeReason_; if (!stopping_) hook = ended_; }
    if (fs_) fs_->cancelOpen();
    if (requests_) requests_->close();
    if (dav_) dav_->clearShares();
    // A session that ended is the ordinary end, not a failure (otherwise every stream end reads as a failed drive share).
    record(5, reason == "session_ended", -1, -1, reason);
    wake_.notify_all();
    // channel-resume-v1: an end of its own (not stop()) is reported; a refused dial or a reason of the pair's own is final.
    if (hook) hook(channelEndTransient("drive", reason) && !link_->refused());
  }
  std::string mountedJson() {
    std::string out = "{\"type\":\"drive.mounted\",\"shares\":[";
    bool first = true;
    for (const auto& [id, share] : shares_) {
      if (!first) out += ','; first = false;
      out += "{\"id\":" + jsonQuote(id);
      if (share.state == State::mapped) out += ",\"state\":\"mapped\",\"letter\":\"" + share.letter + "\"}";
      else if (share.state == State::failed) out += ",\"state\":\"failed\",\"error\":" + jsonQuote(share.error) + '}';
      else out += ",\"state\":\"pending\"}";
    }
    return out + "]}";
  }
  void work() {
    std::vector<std::string> mapped; // tokens with a WebDAV drive letter
    std::set<std::string> fsMapped;  // tokens with a WinFsp drive letter
    const auto unmapDav = [&](DWORD session, const std::vector<std::string>& tokens) { if (tokens.empty()) return; mounter_->unmap(session, tokens); for (const auto& token : tokens) { const auto at = std::find(mapped.begin(), mapped.end(), token); if (at != mapped.end()) mapped.erase(at); } };
    const auto unmapFs = [&](const std::vector<std::string>& tokens) { if (tokens.empty()) return; fs_->unmap(tokens); for (const auto& token : tokens) fsMapped.erase(token); };
    for (;;) {
      std::vector<std::string> unmap; std::vector<std::pair<std::string, std::string>> map; std::vector<fs::FsShare> fsShares; std::vector<std::pair<std::string, bool>> readOnlyUpdates; DWORD session = 0; uint16_t port = 0; std::string announce; bool useFs = false;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return dirty_ || closed_ || stopping_; });
        if (closed_ || stopping_) break;
        dirty_ = false; session = session_; port = port_; useFs = fsRequested_;
        unmap.swap(removed_);
        for (auto& [id, share] : shares_) {
          if (share.state == State::pending) { share.state = State::mapping; map.emplace_back(share.token, share.label); fsShares.push_back({share.token, id, share.label, share.readOnly}); }
          else if (share.viaFs && share.state == State::mapped && share.roApplied != share.readOnly) { share.roApplied = share.readOnly; readOnlyUpdates.emplace_back(share.token, share.readOnly); }
        }
        if (!map.empty()) announce = mountedJson();
      }
      // One current mapping action, outside every state/map mutex. The native
      // operation owns this slot until actual reply; a client deadline cannot replay it.
      std::optional<OpenJob> opening; bool openFs = false;
      {
        std::lock_guard lock(mutex_);
        if (pendingOpen_ && !pendingOpen_->started) {
          pendingOpen_->started = true; opening = *pendingOpen_;
          const auto found = shares_.find(opening->share);
          if (found != shares_.end()) openFs = found->second.viaFs;
        }
      }
      if (opening) {
        bool current = false;
        {
          std::lock_guard lock(mutex_);
          const auto found = shares_.find(opening->share);
          current = !closed_ && !stopping_ && found != shares_.end() &&
            found->second.state == State::mapped && found->second.token == opening->token;
        }
        std::string error = "drive_share_unknown";
        if (current) {
          try { error = openFs ? fs_->openRoot(session, opening->token) : mounter_->openRoot(session, port, opening->token); }
          catch (...) { error = "drive_open_unknown"; }
        }
        bool reply = false;
        {
          std::lock_guard lock(mutex_);
          const auto found = shares_.find(opening->share);
          reply = !closed_ && !stopping_ && pendingOpen_ && pendingOpen_->id == opening->id;
          if (reply && (found == shares_.end() || found->second.token != opening->token || found->second.state != State::mapped)) error = "drive_share_unknown";
          pendingOpen_.reset(); // this native helper invocation actually returned
        }
        if (reply) link_->sendText(openResponse(opening->id, opening->share, error));
      }
      // WebDAV-only sessions prepare the WebClient service up front as before; a WinFsp session
      // touches it only when a share falls back to WebDAV.
      if (!useFs && !prepared_) {
        prepareError_ = mounter_->prepare(); prepared_ = true;
        if (mounter_->oldLimit()) notice("drive_webclient_old_limit", 10);
        if (prepareError_.empty()) mounter_->sweep(session);
      }
      for (const auto& [token, value] : readOnlyUpdates) fs_->setReadOnly(token, value);
      // Only tokens this worker mapped are unmapped (a removed pending share has none).
      std::vector<std::string> removeDav, removeFs;
      for (const auto& token : unmap) { if (fsMapped.count(token)) removeFs.push_back(token); else if (std::find(mapped.begin(), mapped.end(), token) != mapped.end()) removeDav.push_back(token); }
      if (!removeDav.empty()) { unmapDav(session, removeDav); record(4, true, int64_t(removeDav.size()), 0); }
      if (!removeFs.empty()) { unmapFs(removeFs); record(4, true, int64_t(removeFs.size()), 0); }
      if (!announce.empty()) link_->sendText(announce);
      if (map.empty()) continue;
      std::vector<MountResult> results; std::set<std::string> viaFs; std::map<std::string, bool> mountedReadOnly;
      auto davMap = map;
      if (useFs) {
        if (fs_->available()) {
          const auto outcomes = fs_->map(session, *requests_, fsShares);
          size_t failed = 0; std::string firstError; uint32_t firstCause = 0; std::set<std::string> fell;
          for (const auto& outcome : outcomes) {
            if (!outcome.letter.empty()) { MountResult result; result.token = outcome.token; result.letter = outcome.letter; results.push_back(std::move(result)); viaFs.insert(outcome.token); fsMapped.insert(outcome.token); }
            else { ++failed; fell.insert(outcome.token); if (firstError.empty()) { firstError = outcome.error; firstCause = outcome.status; } }
          }
          for (const auto& share : fsShares) mountedReadOnly[share.token] = share.readOnly;
          record(13, failed == 0, int64_t(outcomes.size()), int64_t(failed), firstError, firstCause);
          davMap.clear();
          for (const auto& item : map) if (!viaFs.count(item.first)) davMap.push_back(item);
          // The WinFsp mount failed: this share is served by WebDAV instead (stage 10).
          if (failed) record(14, false, int64_t(failed), int64_t(failed), firstError, firstCause);
        } else record(15, false, int64_t(map.size()), int64_t(map.size()));
      }
      if (!davMap.empty()) {
        if (!prepared_) {
          prepareError_ = mounter_->prepare(); prepared_ = true;
          if (prepareError_.empty()) mounter_->sweep(session);
        }
        if (!prepareError_.empty()) for (const auto& [token, label] : davMap) { (void)label; results.push_back({token, {}, prepareError_}); }
        else { auto dav = mounter_->map(session, port, davMap); results.insert(results.end(), dav.begin(), dav.end()); }
      }
      size_t failed = 0; std::vector<std::string> orphanedDav, orphanedFs; std::string mounted; std::string firstError; uint32_t firstCause = 0;
      {
        std::lock_guard lock(mutex_);
        for (const auto& result : results) {
          const bool onFs = viaFs.count(result.token) != 0;
          if (!result.letter.empty() && !onFs) mapped.push_back(result.token);
          auto found = std::find_if(shares_.begin(), shares_.end(), [&](const auto& item) { return item.second.token == result.token; });
          if (found == shares_.end() || closed_ || stopping_) { if (!result.letter.empty()) (onFs ? orphanedFs : orphanedDav).push_back(result.token); continue; }
          if (!result.letter.empty()) { found->second.state = State::mapped; found->second.letter = result.letter; found->second.viaFs = onFs; found->second.roApplied = onFs ? mountedReadOnly[result.token] : found->second.readOnly; if (onFs) dirty_ = dirty_ || found->second.roApplied != found->second.readOnly; }
          else { found->second.state = State::failed; found->second.error = result.error.empty() ? "drive_map_failed" : result.error; ++failed; if (firstError.empty()) { firstError = found->second.error; firstCause = result.win32; } }
        }
        // A share that never got an answer is a failure, not a forever-pending one.
        for (auto& [id, share] : shares_) if (share.state == State::mapping) { share.state = State::failed; share.error = "drive_map_failed"; ++failed; }
        if (!closed_ && !stopping_) mounted = mountedJson();
      }
      // Removed while it was being mapped: unmapped right away.
      unmapDav(session, orphanedDav); unmapFs(orphanedFs);
      record(failed ? 3 : 2, !failed, int64_t(results.size()), int64_t(failed), firstError, firstCause);
      if (!mounted.empty()) link_->sendText(mounted);
    }
    // Every drive this session mapped goes with it.
    DWORD session; { std::lock_guard lock(mutex_); session = session_; }
    if (!mapped.empty()) { const auto count = mapped.size(); mounter_->unmap(session, mapped); record(4, true, int64_t(count), 0); }
    if (!fsMapped.empty()) { std::vector<std::string> all(fsMapped.begin(), fsMapped.end()); fs_->unmap(all); record(4, true, int64_t(all.size()), 0); }
    if (fs_) fs_->stop();  // ends the helper process
  }};
}
