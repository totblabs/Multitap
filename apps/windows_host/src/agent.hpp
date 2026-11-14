#pragma once
#include "support.hpp"
#include "audio_stats.hpp"
#include "opus_audio.hpp"
#include "capture_selection.hpp"
#include "stream_metrics.hpp"
#include "frame_encode_policy.hpp"
#include "broker_status.hpp"
#include "host_diagnostic_schedule.hpp"
#include "stream_prepare_diagnostics.hpp"
#include "encoder_diagnostics.hpp"
#include "track_input.hpp"
#include "session_stop.hpp"
#include "remote_pointer.hpp"
#include "lossless_tile_source.hpp"
#include "bitrate_session.hpp"
#include "host_update.hpp"
#include "host_power.hpp"
#include "input_latency.hpp"
#include "direct_input.hpp"
#include "direct_input_validation.hpp"
#include "json_object.hpp"
#include "cursor_channel.hpp"
#include "background_video.hpp"
#include "text_input.hpp"
#include "touch_input.hpp"
#include "caret_follow.hpp"
#include "frame_pacer.hpp"
#include "audio_capture_control.hpp"
#include "capture_topology_watch.hpp"
#include "capture_follow.hpp"
#include "stream_priority.hpp"
#include "stream_gpu_priority.hpp"
#include "gpu_capture.hpp"
#include "stream_encode_size.hpp"
#include "stream_size.hpp"
#include "send_queue.hpp"
#include "websocket_send.hpp"
#include "operational_log.hpp"
#include "boot_timeline.hpp"
#include "resource_sampler.hpp"
#include "stream_observability.hpp"
#include "host_observation_log.hpp"
#include "udp_probe_leg.hpp"
#include "broker_route.hpp"
#include "resize_stages.hpp"
#include "thumbnail_capture.hpp"
#include "host_resume.hpp"
#include "broker_reconnect.hpp"
#include "media_warmup.hpp"
#include "secure_desktop_notice.hpp"
#include "secure_desktop_scope.hpp"
#include "secure_desktop_worker.hpp"
#include "secure_desktop_tracks.hpp"
#include "system_input.hpp"
#include "system_input_tracks.hpp"
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <random>
#include <thread>

namespace host {
// Video, audio and cursor-v1 records (kind 3) carry the same media epoch.
inline void stampMediaEpoch(std::vector<uint8_t>& bytes,uint16_t epoch) {if(bytes.size()>=12&&(bytes[0]==1||bytes[0]==2||bytes[0]==cursor::mediaKind)){bytes[2]=uint8_t(epoch);bytes[3]=uint8_t(epoch>>8);}}
// Keep only an active streaming thread out of automatic display/system idle.
// This does not change a power plan, lock policy or the user's shutdown choice.
class StreamPowerActivity {
 public:
  using Setter = std::function<EXECUTION_STATE(EXECUTION_STATE)>;
  explicit StreamPowerActivity(Setter setter = [](EXECUTION_STATE state) { return SetThreadExecutionState(state); })
      : setter_(std::move(setter)), previous_(setter_(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)) {}
  ~StreamPowerActivity() { if (previous_) setter_(previous_); }
  StreamPowerActivity(const StreamPowerActivity&) = delete;
  bool active() const { return previous_ != 0; }
 private:
  Setter setter_; EXECUTION_STATE previous_ = 0;
};
class AgentBridge {
 public:
  using Send = std::function<void(bool, std::vector<uint8_t>)>;
  virtual ~AgentBridge() = default;
  virtual void start(const Json&, Send) = 0;
  virtual void control(const Json&) = 0;
  virtual void stop() = 0;
  virtual SessionStopEvidence stopSession(const std::string&) {stop();return {};}
  virtual SessionStopEvidence stoppedSession(const std::string&) {return {};}
  virtual std::string capabilities() = 0;
  virtual std::string signinEligibility() {return {};}
  // These are called only from actual authenticated main-WSS lifecycle sites.
  virtual void mainConnected(HINTERNET,std::function<bool(const std::string&)>,std::function<bool(HINTERNET)>) {}
  virtual void mainDisconnected(HINTERNET) {}
  virtual bool mainControl(HINTERNET,const Json&,uint64_t) {return false;}
  virtual void stopMainAdapters() {}
  virtual std::string diagnostics() {return "{}";}
  // The broker's answer to the last report; true while newer rows wait to be sent.
  virtual bool diagnosticsAcknowledged(bool) {return false;}
  virtual void displayControl(const Json&, Send) = 0;
  virtual void consoleControl(const Json&, Send) {}
  virtual void qemuConsoleControl(const Json&, Send) {}
  // ime-state-v1 (ime_state.hpp): one host.ime.query; the answer is sent later,
  // from another thread. Called on the receiving thread; never blocks.
  virtual void imeQuery(const Json&, Send) {}
  // Adaptive bitrate: a new target for the encoder of `session`. Called from
  // any Agent thread; an implementation may only store it (never block).
  virtual void bitrate(const std::string&, int) {}
  // The broker channel's send queue dropped video of the current stream: its
  // encoder must start a new IDR. Any Agent thread; only store it.
  virtual void keyFrame() {}
  // host-resume-v1: the broker channel came back. Cursor records the worker
  // sent meanwhile were dropped with the closed send queue, so the relay and
  // the client no longer hold what the worker's channel believes it sent: the
  // worker sends its shapes and state again (with the IDR asked for next).
  // Any Agent thread; only store it.
  virtual void cursorResend() {}
  // host-resume-v1: the stream is held through a lost broker channel, and it
  // continues. The session's clipboard channel is lost with the broker channel:
  // its failed dials cost no attempt while held, and the resumed stream starts
  // it again (clipboard_reconnect.hpp). Any Agent thread; neither blocks.
  virtual void clipboardAway() {}
  virtual void clipboardResumed() {}
  // channel-resume-v1 (channel_redial.hpp): the Broker says the session's client left (its socket is being
  // replaced) or is back. The side channels wait for it as they wait for this Host's own channel, and start
  // again when both are there. Any Agent thread; neither blocks.
  virtual void peerAway() {}
  virtual void peerBack() {}
  // thumbnail-capture-v1 (thumbnail_capture.hpp): one launcher picture on the
  // broker's request. Blocking; the Agent calls it on its own thumbnail thread.
  virtual ThumbnailShot thumbnail() { return {}; }
  // udp-media-v1 LOSS: frames after `lastGoodPts` never reached the client's decoder. The
  // next frame references only that frame or older, or is an IDR. Any Agent thread; only store it.
  virtual void recover(uint64_t) { keyFrame(); }
};
// hostAction 241 stage: why the Host closed an established broker channel
// (labels: server/admin/js/labels.js hostChannelCloseCodes). Index 0 unused.
inline constexpr const char* hostChannelCloseCodes[] = {"", "transport_closed", "send_failed", "heartbeat_timeout", "protocol_rejected", "send_queue_stalled", "service_stopping"};
inline uint32_t hostChannelCloseCode(const char* reason) {
  for (uint32_t index = 1; index < std::size(hostChannelCloseCodes); ++index) if (std::string_view(reason) == hostChannelCloseCodes[index]) return index;
  return 0;
}
// Keep only the numeric peer status; a close reason may contain private text.
// host-machine-v1: the named reason of a refused channel upgrade (relay.mjs
// reject: X-Multitap-Host-Error), or empty. Only lowercase codes are read.
inline std::string hostChannelRefusal(HINTERNET request) {
  wchar_t value[64]{}; DWORD size = sizeof(value) - sizeof(wchar_t);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"X-Multitap-Host-Error", value, &size, WINHTTP_NO_HEADER_INDEX)) return {};
  std::string code; for (const wchar_t* at = value; *at; at++) { if (!((*at >= L'a' && *at <= L'z') || *at == L'_') || code.size() >= 48) return {}; code += char(*at); }
  return code;
}
template<class Query> inline uint32_t hostChannelRemoteCloseCode(Query&& query) {
  USHORT code=0;DWORD used=0;std::array<char,128> ignored{};
  const auto result=query(&code,ignored.data(),DWORD(ignored.size()),&used);
  SecureZeroMemory(ignored.data(),ignored.size());
  return result==NO_ERROR&&code>=1000&&code<=4999?uint32_t(code):0;
}
// host-closing-v1: a broker whose capabilities ACK names this protocol accepts
// one {"type":"host.closing","reason"} before a deliberate close; its sessions
// then end with that reason as their detail instead of a bare disconnect.
inline constexpr char hostClosingProtocol[] = "host-closing-v1";
// keyframe-v1 (capability keyFrameProtocol): the Host codes its next frame as
// an IDR on {"type":"stream.keyframe","sessionId"}, so a relay may skip video
// for a client that falls behind and resume it at that IDR.
inline constexpr char keyFrameProtocol[] = "keyframe-v1";
class RemoteInput {
 public:
  using Injector = std::function<UINT(INPUT&)>;
  struct EnterBatchResult { UINT accepted=0; bool scopeCurrent=false; };
  using SigninEnterBatch = std::function<EnterBatchResult(std::array<INPUT,2>&)>;
  // touch-input-v1 contacts use their own backend (touch_input.hpp); an empty
  // one refuses every touch frame, so a substituted injector never reaches
  // real touch injection by default.
  explicit RemoteInput(Injector injector = [](INPUT& input) { return SendInput(1, &input, sizeof(input)); }, bool automaticReleaseRetry = false,RemotePointerBounds bounds=remotePointerDesktopBounds,TouchInjectionBackend touch={},SigninEnterBatch signinEnter={}) : inject_(std::move(injector)),signinEnter_(std::move(signinEnter)),bounds_(std::move(bounds)),touch_(std::move(touch),bounds_,automaticReleaseRetry) {
    if (automaticReleaseRetry) releaseWorker_ = std::thread([this] { retryReleases(); });
  }
  explicit RemoteInput(bool automaticReleaseRetry) : RemoteInput([](INPUT& input) { return SendInput(1, &input, sizeof(input)); }, automaticReleaseRetry, remotePointerDesktopBounds, nativeTouchInjection()) {}
  ~RemoteInput() {
    { std::lock_guard lock(mutex_); releaseClosing_ = true; }
    releaseWake_.notify_one();
    if (releaseWorker_.joinable()) releaseWorker_.join();
  }
  // Releases keys and buttons, and cancels every held touch contact.
  void reset() {
    { std::lock_guard lock(mutex_); resetLocked(); }
    touch_.release();
  }
  void retry() { std::lock_guard lock(mutex_); if (releasePending_) resetLocked(); }
  size_t pending() { std::lock_guard lock(mutex_); return keys_.size() + buttons_.size() + size_t(textUnit_.has_value()); }
  // A pointer button this injector pressed is still down (a drag in progress).
  bool buttonHeld() { std::lock_guard lock(mutex_); return !buttons_.empty(); }
  void area(RECT value) { std::lock_guard lock(mutex_); area_ = value; }
  bool apply(const Json& message) {
    std::lock_guard lock(mutex_);
    if (releasePending_) resetLocked();
    if (releasePending_) return false;
    const auto kind = message.get("event").string();
    INPUT input{};
    if (kind == "key") {
      const double virtualKey = message.get("vk").number(-1), scanCode = message.get("scan").number(0);
      if (virtualKey < 1 || virtualKey > 255 || std::floor(virtualKey) != virtualKey || scanCode < 0 || scanCode > 65535 || std::floor(scanCode) != scanCode) return false;
      const WORD vk = WORD(virtualKey); const bool down = message.get("down").boolean();
      // Hangul/Hanja are semantic IME actions. A client keyboard may report
      // E0 F1/F2 or Right Alt here; SCANCODE would ignore this virtual key and
      // reinterpret the physical key using the host keyboard's layout.
      const bool imeAction = vk == VK_HANGUL || vk == VK_HANJA;
      Key key{imeAction ? WORD(0) : WORD(scanCode), !imeAction && message.get("extended").boolean()};
      // A key sent without a scan code (Android's hardware keyboard sends letters
      // with 0): keep the virtual key that was sent and add this layout's scan code,
      // which Windows then reports with it. A scan code of 0 may keep the Korean IME
      // from composing. E1 keys (Pause) have no single scan code and stay as they were.
      // The navigation keys get the keypad's code without E0 (Left: 0x4B on the Korean
      // layout, measured); on a keyboard they are the extended ones.
      if (!imeAction && !key.scan) if (const UINT mapped = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX); (mapped & 0xff) && ((mapped >> 8) == 0 || (mapped >> 8) == 0xe0))
        key = Key{WORD(mapped & 0xff), key.extended || (mapped >> 8) == 0xe0 || vk == VK_INSERT || vk == VK_DELETE || (vk >= VK_PRIOR && vk <= VK_DOWN), true};
      // Release exactly the input we successfully pressed, even if the sender's
      // key-up carries no scan or a different layout has become active.
      if (const auto held = keys_.find(vk); held != keys_.end()) key = held->second;
      // Secure attention is not simulated by ordinary SendInput.
      if (vk == VK_DELETE && down && (keys_.contains(VK_CONTROL) || keys_.contains(VK_LCONTROL) || keys_.contains(VK_RCONTROL)) && (keys_.contains(VK_MENU) || keys_.contains(VK_LMENU) || keys_.contains(VK_RMENU))) return false;
      // Opt-in exists only in the authenticated Winlogon pipeline. Normal
      // desktop input retains its exact hold/repeat behavior and injector.
      if(signinEnter_&&vk==VK_RETURN&&enterTapAwaitingUp_){if(!down)enterTapAwaitingUp_=false;return true;}
      if(signinEnter_&&vk==VK_RETURN&&down&&key.scan==0x1c&&keys_.empty()&&buttons_.empty()&&!textUnit_){
        std::array<INPUT,2> batch{keyInput(vk,key,false),keyInput(vk,key,true)};
        const auto result=signinEnter_(batch);
        if(result.accepted==2){enterTapAwaitingUp_=true;return result.scopeCurrent;}
        if(result.accepted){keys_[vk]=key;releaseFailed();}
        return false;
      }
      input = keyInput(vk, key, !down);
      if (inject_(input) == 1) { if (down) keys_[vk] = key; else keys_.erase(vk);return true; } else if (!down && keys_.contains(vk)) releaseFailed();
      return false;
    }
    if (kind == "move" || kind == "moveRelative" || kind == "button" || kind == "wheel") {
      auto planned=planRemotePointerInput(message,area_,bounds_);
      if(!planned){
        // A topology change must not strand a button we already pressed.
        // Only releases may retry, without reusing the rejected coordinates.
        const auto button=message.get("button").string();
        if(kind=="button"&&remotePointerButton(button)&&std::holds_alternative<bool>(message.get("down").value)&&!message.get("down").boolean()&&buttons_.contains(button))releaseFailed();
        return false;
      }
      if(inject_(planned->input)==1){if(kind=="button"){if(planned->down)buttons_.insert(planned->button);else buttons_.erase(planned->button);}return true;}
      if(kind=="button"&&!planned->down&&buttons_.contains(planned->button))releaseFailed();
    }
    return false;
  }
  // text-input-v1: one committed text as Unicode key events, all or nothing
  // validated first. A failed Unicode key-up follows the same release retry.
  bool text(const Json& message) {
    const auto units = textInputUnits(message);
    if (!units) return false;
    std::lock_guard lock(mutex_);
    if (releasePending_) resetLocked();
    if (releasePending_) return false;
    for (auto input : textInputEvents(*units)) {
      const bool up = (input.ki.dwFlags & KEYEVENTF_KEYUP) != 0;
      if (inject_(input) != 1) { if (up) releaseFailed(); return false; }
      if (up) textUnit_.reset(); else textUnit_ = input.ki.wScan;
    }
    return true;
  }
  // touch-input-v1: one touch frame on the streamed source rectangle,
  // validated as a whole first (touch_input.hpp).
  bool touch(const Json& message) {
    const auto frame = touchInputFrame(message);
    if (!frame) return false;
    RECT target{};
    {
      std::lock_guard lock(mutex_);
      if (releasePending_) resetLocked();
      if (releasePending_) return false;
      target = area_;
    }
    return touch_.apply(*frame, target, std::chrono::steady_clock::now());
  }
  size_t touchHeld() { return touch_.held(); }
  RemoteTouch::Stats touchStats() { return touch_.stats(); }
 private:
  // virtualKey: the scan code was filled in here; Windows keeps the sent virtual key.
  struct Key { WORD scan; bool extended; bool virtualKey = false; };
  static INPUT keyInput(WORD vk, const Key& key, bool up) {
    const bool byScan = key.scan && !key.virtualKey;
    INPUT input{}; input.type = INPUT_KEYBOARD; input.ki.wVk = byScan ? 0 : vk; input.ki.wScan = key.scan;
    input.ki.dwFlags = (byScan ? KEYEVENTF_SCANCODE : 0) | (key.extended ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
    return input;
  }
  // Checking pending and releasing must share the input lock. Otherwise an
  // old retry could observe true, wait, then release a newly accepted press.
  void resetLocked() {
    for (auto it = keys_.begin(); it != keys_.end();) { const auto [vk, key] = *it; INPUT input = keyInput(vk, key, true); if (inject_(input) == 1) it = keys_.erase(it); else ++it; }
    for (auto it = buttons_.begin(); it != buttons_.end();) { const auto& button = *it; INPUT input{}; input.type = INPUT_MOUSE; remotePointerButtonInput(input.mi, button, false); if (inject_(input) == 1) it = buttons_.erase(it); else ++it; }
    if (textUnit_) { INPUT input{}; input.type = INPUT_KEYBOARD; input.ki.wScan = *textUnit_; input.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP; if (inject_(input) == 1) textUnit_.reset(); }
    releasePending_ = !keys_.empty() || !buttons_.empty() || textUnit_.has_value();
    releaseWake_.notify_one();
  }
  // The interactive worker can be blocked waiting for its next pipe command.
  // A failed key/button release must not depend on that command arriving. This
  // helper sleeps indefinitely when there is no failed release, and never
  // retries presses or switches the worker's Windows desktop.
  void releaseFailed() { releasePending_ = true; releaseWake_.notify_one(); }
  void retryReleases() {
    std::unique_lock lock(mutex_);
    while (!releaseClosing_) {
      releaseWake_.wait(lock, [this] { return releaseClosing_ || releasePending_; });
      if (releaseClosing_ || releaseWake_.wait_for(lock, std::chrono::milliseconds(25), [this] { return releaseClosing_; })) break;
      lock.unlock(); retry(); lock.lock();
    }
  }
  Injector inject_;SigninEnterBatch signinEnter_;bool enterTapAwaitingUp_=false;RemotePointerBounds bounds_; RemoteTouch touch_; bool releasePending_ = false; std::mutex mutex_; std::map<WORD, Key> keys_; std::set<std::string> buttons_; std::optional<WORD> textUnit_; RECT area_{};
  std::condition_variable releaseWake_; bool releaseClosing_ = false; std::thread releaseWorker_;
};
class Agent {
 public:
  using Status = std::function<void(const std::wstring&)>;
  explicit Agent(Status status, AgentBridge* bridge = nullptr, AgentBridge::Send local = {}, BrokerConnectionStatus* brokerStatus = nullptr) : status_(std::move(status)), bridge_(bridge), local_(std::move(local)), brokerStatus_(brokerStatus), input_(bool(local_)) {}
#ifdef MULTITAP_TRACK_SOURCE_TEST
  Agent(Status status,AgentBridge::Send local,RemoteInput::Injector injector,TrackSourcePipeline::Monitors monitors,TrackSourcePipeline::CaptureFactory capture,RemotePointerBounds bounds)
      :status_(std::move(status)),local_(std::move(local)),input_(std::move(injector),true,std::move(bounds)),trackMonitors_(std::move(monitors)),trackCapture_(std::move(capture)){}
#endif
#ifdef MULTITAP_INPUT_LATENCY_TEST
  // The actual worker control/injection path with a substituted injector and
  // no capture: binds the session exactly as stream() does before capture.
  Agent(Status status,AgentBridge::Send local,RemoteInput::Injector injector,RemotePointerBounds bounds,TouchInjectionBackend touch={})
      :status_(std::move(status)),local_(std::move(local)),input_(std::move(injector),true,std::move(bounds),std::move(touch)){}
  void inputLatencyTestStream(const Json& start,RECT area){stopStream();sessionId_=start.get("sessionId").string();inputLatency_.configure(start);configureCursor(start,area);configureCursorEcho(start);configureBackground(start);textInput_=textInputSessionRequested(start);touchInput_=touchInputSessionRequested(start);relativeMouse_=relativeMouseSessionRequested(start);input_.area(area);streamStop_=false;active_=true;stopped_=false;}
  // background-video-v1: whether the capture runs at the hidden pace, and whether the next picture is an IDR.
  bool backgroundTestHidden()const{return streamBackground_.load();}
  bool backgroundTestGranted()const{return backgroundGranted_.load();}
  // cursor-echo-v1: the request the capture's poller follows (0 none).
  uint32_t cursorTestEchoRequest()const{return cursorEchoRequest_.load();}
  bool cursorTestEchoGranted()const{return cursorEchoGranted_.load();}
  const cursor::ComposeRequest& cursorTestCompose()const{return cursorCompose_;}
  bool textInputTestEnabled()const{return textInput_;}
  bool touchInputTestEnabled()const{return touchInput_;}
  size_t touchTestHeld(){return input_.touchHeld();}
  InputLatencySession& inputLatencyTestState(){return inputLatency_;}
  cursor::InjectedPointer::Mark cursorTestMark()const{return cursorInput_.read();}
  bool cursorTestResend()const{return cursorResend_;}
  bool cursorTestSeparated()const{return cursorSeparated_;}
#endif
  ~Agent() { stop(); }
  // Set before starting this agent; called only on a failed capture path.
  void captureDiagnostic(std::function<void(uint32_t,uint32_t)> sink){captureDiagnostic_=std::move(sink);}
  void start(Credentials credentials) { stop(); credentials_ = std::move(credentials); updater_.reset(); if (hostUpdateEnvironment_) updater_ = std::make_unique<HostUpdater>(hostUpdateEnvironment_(credentials_.origin), [this](const std::string& message) { text(message); }); power_.reset(); if (hostPowerEnvironment_) power_ = std::make_unique<HostPower>(hostPowerEnvironment_(), [this](const std::string& message) { text(message); }); stopped_ = false; worker_ = std::thread([this] { run(); }); }
  void stop() { closingNotice("service_stopping");if(bridge_)bridge_->stopMainAdapters(); caretEnd(); cursorEchoEnd(); stopped_ = true; if(brokerStatus_)brokerStatus_->stop();streamStop_ = true; input_.reset(); closeTransport(); if (worker_.joinable()) worker_.join(); joinDiagnostics(); stopStream(); if (updater_) updater_->stop(); for (int i = 0; i < 10 && input_.pending(); i++) { input_.retry(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); } }
  // Service mode only (set before start): remote Host program update over
  // this authenticated channel, downloading from the configured origin.
  void enableHostUpdate(std::function<HostUpdateEnvironment(const std::wstring& origin)> environment) { hostUpdateEnvironment_ = std::move(environment); }
  // Service mode only (set before start): PC power over this channel (pc-power-v1).
  void enableHostPower(std::function<HostPowerEnvironment()> environment) { hostPowerEnvironment_ = std::move(environment); }
  // host-machine-v1: called once from the channel thread when the server refuses this machine.
  void onMachineChanged(std::function<void()> changed) { machineChanged_ = std::move(changed); }
  bool connected() const { return socket_.load() != nullptr; }
  void announceCapabilities() { if(bridge_)enqueue({true,{},true}); }
#ifdef MULTITAP_AGENT_PUBLICATION_TEST
  void publicationTestOpen(){std::lock_guard lock(queueMutex_);queueClosed_=false;stopped_=false;}
  // A worker's tiles opt-in state for a session (what its capture loop reads).
  bool losslessTestRequested(const std::string& session)const{return losslessRequested(session);}
  // The send loop's diagnostics step, and the wait the connection end does.
  void diagnosticsTestTick(){collectDiagnostics();}
  void diagnosticsTestJoin(){joinDiagnostics();}
  void publicationTestBoot(BootTimeline& timeline){boot_=&timeline;}
  bool publicationTestDrain(const std::function<bool(bool,const std::vector<uint8_t>&)>& sink){
    for(;;){Outgoing packet;{std::lock_guard lock(queueMutex_);if(!queue_.pop(packet))return true;}if(!publishOutgoing(packet,sink))return false;}
  }
  // The actual WebSocket write path (fragmenting) with a substituted
  // WinHttpWebSocketSend: (type, bytes, count) -> Win32 error.
  using RawSend=std::function<DWORD(WINHTTP_WEB_SOCKET_BUFFER_TYPE,const uint8_t*,DWORD)>;
  bool publicationTestWrite(const RawSend& raw){
    for(;;){Outgoing packet;{std::lock_guard lock(queueMutex_);if(!queue_.pop(packet))return true;}
      if(!publishOutgoing(packet,[&](bool text,const std::vector<uint8_t>& bytes){return sendWebSocketMessage(raw,text,bytes.data(),bytes.size())==NO_ERROR;}))return false;}
  }
  bool publicationTestClosed(){std::lock_guard lock(queueMutex_);return queueClosed_;}
  std::string publicationTestHeartbeatCapabilities(){return heartbeatCapabilities();}
  bool publicationTestAwaitingKeyFrame(){std::lock_guard lock(queueMutex_);return queue_.awaitingKeyFrame();}
  // Queues the host-closing-v1 notice as a negotiated close would (no socket here).
  bool publicationTestClosingNotice(const char* reason){if(!closingNegotiated_)return false;queueClosingNotice(reason,false);return true;}
  bool publicationTestKeyFrameRequested(){return keyFrameRequested_.load();}
  uint64_t publicationTestRecoverRequested(){return recoverRequested_.load();}
  bool publicationTestResourcesUptime(){return resourcesUptime_.load();}
  std::shared_ptr<UdpProbeLeg> publicationTestUdpLeg(){return udpProbe_.load();}
  // host-resume-v1: run()'s end of a channel and the start of the next one, without WinHTTP.
  void publicationTestChannelLost(const char* failure){closeTransport(failure);channelEnded(failure);}
  void publicationTestNewChannel(){{std::lock_guard lock(queueMutex_);queueClosed_=false;}resume_.channel();}
  // Restart recovery: run()'s wait after a channel end, and a dial that got through.
  uint32_t publicationTestChannelEndWait(uint32_t remoteCloseCode){return channelEndWait(remoteCloseCode);}
  void publicationTestDialed(){reconnectWait_.connected();}
#endif
  void localControl(const std::string& bytes) { control(bytes); }
  void localTrackStart(const Json& command,TrackSourcePlan plan){
    if(!local_||bridge_||plan.sessionId!=command.get("sessionId").string()||plan.topology!=command.get("topologyEpoch").number())
      throw std::runtime_error("Invalid bound track session");
    stopStream();sessionId_=plan.sessionId;observability_.configure(command);input_.area(plan.inputArea);
    // relative-mouse-tracks-v1 / cursor-compose-v1: 게임 마우스 on independent outputs.
    auto input=std::make_shared<TrackSourceInput>(plan,relativeMouseTrackSessionRequested(command));trackInput_.store(input);
    cursorCompose_.configure(cursor::composeTrackSessionRequested(command),plan.sessionId);
    streamStop_=false;active_=true;stopped_=false;
    const bool sizesV2=streamSizeV2(command);
    // cursor-tracks-v1: only a session.start carrying both granted markers.
    const bool cursorSeparated=cursor::trackSessionRequested(command);
    capture_=std::thread([this,plan=std::move(plan),input,sizesV2,cursorSeparated]{
      std::thread audio;std::atomic<bool> audioGo{false},audioEnabled{false};std::shared_ptr<TrackSourcePipeline> pipeline;
      try{
        StreamPowerActivity power;StreamPriority priority;Runtime runtime;
        struct CaptureCleanup{
          std::atomic<bool>& stop;std::atomic<bool>& go;std::thread& audio;std::shared_ptr<TrackSourcePipeline>& pipeline;
          ~CaptureCleanup(){stop=true;go=true;if(audio.joinable())audio.join();pipeline.reset();}
        }cleanup{streamStop_,audioGo,audio,pipeline};
        const auto beginning=Clock::now();
        const auto micros=[beginning]{return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-beginning).count());};
        // audio-opus-tracks-v1 (opus_audio.hpp): Opus where the broker granted it
        // and the encoder starts; otherwise PCM, and the metadata says which.
        std::optional<OpusAudioEncoder> opusEncoder;if(plan.audio&&plan.audioOpus)opusEncoder=OpusAudioEncoder::create();
        auto sent=plan;sent.audioOpus=opusEncoder.has_value();
        std::promise<bool> audioReady;auto ready=audioReady.get_future();
        if(plan.audio)audio=std::thread([this,beginning,&audioGo,&audioEnabled,&pipeline,promise=std::move(audioReady),opus=std::move(opusEncoder)]()mutable{
          const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);bool announced=false;
          try{check(com,"Track audio COM");Loopback loopback;const bool available=loopback.available();promise.set_value(available);announced=true;
            if(!available){audioCaptureRecord(loopback.failureRecord());}
            if(available){while(!audioGo&&!streamStop_)std::this_thread::sleep_for(std::chrono::milliseconds(2));
              if(!streamStop_&&audioEnabled)loopback.run(streamStop_,beginning,[this,&pipeline](std::vector<uint8_t> bytes){if(auto packet=pipeline->audio(bytes))enqueue({false,std::move(*packet)});},nullptr,opus?&*opus:nullptr,[this](uint32_t value){audioCaptureRecord(value);});
            }
          }catch(...){if(!announced)promise.set_value(false);report(L"소리 캡처를 사용할 수 없습니다.");}
          if(SUCCEEDED(com))CoUninitialize();
        });
        const bool available=plan.audio&&ready.wait_for(std::chrono::seconds(3))==std::future_status::ready&&ready.get();
#ifdef MULTITAP_TRACK_SOURCE_TEST
        if(trackMonitors_&&trackCapture_)pipeline=std::make_shared<TrackSourcePipeline>(sent,available,micros,trackMonitors_,trackCapture_);else
#endif
        pipeline=std::make_shared<TrackSourcePipeline>(sent,available,micros);
        if(local_)try{pipeline->enableOutputUsage();}catch(...){ }
        if(observability_.enabled())pipeline->enableObservability();
        if(cursorSeparated)pipeline->enableCursor([this,session=plan.sessionId]{return cursorIds_.next(session);});
        // capture_follow.hpp: an output followed an application's mode change;
        // input maps onto its new rectangle. Fixed record as for one output
        // (hostAction 200 stage 241, error = width<<16|height), at most every 10 s.
        pipeline->stopWhen(streamStop_);
        pipeline->onFollow([this,input,followRecordedAt=Clock::time_point{}](uint8_t id,RECT rect)mutable{
          input->follow(id,rect,[this](RECT area){input_.area(area);});
          const auto now=Clock::now();
          if(captureDiagnostic_&&(followRecordedAt==Clock::time_point{}||now-followRecordedAt>=std::chrono::seconds(10))){followRecordedAt=now;try{captureDiagnostic_(41,(uint32_t(std::min<LONG>(rect.right-rect.left,0xffff))<<16)|uint32_t(std::min<LONG>(rect.bottom-rect.top,0xffff)));}catch(...){ }}
        });
        // Secure desktop (track_source.hpp): a UAC prompt of this unlocked session no longer ends a stream of
        // independent outputs. Stage 244 records as for one output (1 notice, 2 returned, 3 locked, 4 limit).
#ifdef MULTITAP_TRACK_SOURCE_TEST
        if(trackDesktop_)pipeline->secureDesktop(*trackDesktop_);
#endif
        // Each output shows its own SYSTEM helper's frames of the prompt, or the notice (secure_desktop_tracks.hpp).
        // Only the service's own user worker asks for helpers; the service routes the client's input to them.
        const bool secureCapable=bool(local_)&&!bridge_&&!plan.sessionId.empty();
        const auto secureRecord=[this](uint32_t error){if(captureDiagnostic_)try{captureDiagnostic_(secureDesktopNoticeStage,error);}catch(...){ }};
        std::array<std::shared_ptr<secure::worker::Mapping>,secure::trackOutputs> secureMappings;std::array<RECT,secure::trackOutputs> secureRects{};
        secure::TrackSwitches::Env secureEnv;
        secureEnv.send=[this,secureCapable](const std::string& packet){if(secureCapable)text(packet);};
        secureEnv.owned=[&secureRects](size_t n){return n<secure::trackOutputs?secure::worker::ownedOutputForRect(secureRects[n]):std::optional<secure::OwnedOutput>{};};
        secureEnv.record=secureRecord;
        // hostAction 200 stage 246: what a helper that ended counted of its input. Counts only.
        secureEnv.counts=[this](const secure::ipc::InputCounts& counts){
          if(!captureDiagnostic_)return;
          const auto put=[&](uint32_t kind,uint32_t value,bool always=false){if(value||always)try{captureDiagnostic_(secure::inputRecordStage,secure::input246(kind,value));}catch(...){ }};
          put(secure::input246Received,counts.received,true);put(secure::input246Injected,counts.injected);put(secure::input246Dropped,counts.dropped);
          put(secure::input246Refused,counts.refused);put(secure::input246SendFailed,counts.sendFailed);if(counts.sendFailed)put(secure::input246LastError,counts.lastError);
        };
        secureEnv.toNv12=[](const uint8_t* bgra,uint8_t* nv12,int width,int height){bgraToNv12(bgra,nv12,width,height);};
        secure::TrackSwitches secureSwitches(plan.sessionId,{trackSecureSeq_[0].load(),trackSecureSeq_[1].load()},std::move(secureEnv));
        const auto secureEnded=[&]{
          secureSwitches.ended();
          for(size_t n=0;n<secure::trackOutputs;++n){secureMappings[n].reset();(void)trackSecureTakeGrant(n);trackSecureDenied_[n]=0;trackSecureSeq_[n].store(secureSwitches.seq(n));}
        };
        // Declared after everything the hooks use and destroyed before it: the helpers are stopped here, and the
        // pipeline (which outlives this scope's locals) no longer calls back.
        struct SecureGuard{std::function<void()> ended;std::shared_ptr<TrackSourcePipeline>& pipeline;~SecureGuard(){try{ended();}catch(...){ }try{if(pipeline)pipeline->onSecure({});}catch(...){ }}} secureGuard{secureEnded,pipeline};
        {
          TrackSecureHooks hooks;hooks.record=secureRecord;hooks.ended=secureEnded;
          hooks.still=[&](size_t n,const OwnedTrackSource& output,SecureDesktopNotice::Clock::time_point now)->TrackSecureStill{
            if(n>=secure::trackOutputs)return {};
            secureRects[n]=output.capture.bounds;
            if(auto grant=trackSecureTakeGrant(n)){
              const uint32_t seq=grant->seq;
              if(auto mapping=secure::worker::mapGrant(std::move(*grant))){if(secureSwitches.attach(n,std::shared_ptr<const uint8_t>(mapping,mapping->view),mapping->width,mapping->height,seq))secureMappings[n]=std::move(mapping);}
              else secureSwitches.denied(n,seq);
            }
            if(const uint32_t denied=trackSecureDenied_[n].exchange(0))secureSwitches.denied(n,denied);
            const auto still=secureSwitches.tick(n,output.media.width,output.media.height,now,secureCapable);
            return TrackSecureStill{still.nv12,still.hold,still.real,still.key};
          };
          pipeline->onSecure(std::move(hooks));
        }
        text(trackSourceInfo(sent,available));audioEnabled=available;audioGo=true;
        auto measured=Clock::now(),next=measured;FramePacer pacer;const auto period=std::chrono::microseconds(1000000/plan.fps);
        Clock::time_point lastWork{};  // last tick that emitted media
        cursor::PointerAsk pointerAsk;  // worker.pointer.absent (pointer_device.hpp), as for one output
        while(!streamStop_&&!stopped_){
          const auto began=Clock::now();
          if(local_&&pointerAsk.due(hostMonotonicUs()))text("{\"type\":\"worker.pointer.absent\",\"sessionId\":"+jsonQuote(plan.sessionId)+'}');
          // tracks-resume-v1: the relay restarts every output at an IDR after a resume.
          if(keyFrameRequested_.exchange(false))pipeline->keyFrame();
          pipeline->lossless(losslessRequested(plan.sessionId));pipeline->composeRequested(cursorCompose_.wanted());
          auto packets=pipeline->step();if(pipeline->keysReady())input->encodedReady();
          if(!packets.empty())lastWork=began;
          for(auto& packet:packets){if(streamStop_||stopped_)break;enqueue({false,std::move(packet)});}
          if(local_&&!streamStop_&&!stopped_)try{for(const auto& usage:pipeline->takeOutputUsage()){const auto packet=encoderUsagePacket({plan.sessionId,plan.topology,usage});if(!packet.empty())text(packet);}}catch(...){ }
          const auto now=Clock::now();const auto milliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(now-measured).count();
          if(milliseconds>10000)throw std::runtime_error("Track capture stalled");
          if(milliseconds>=1000){text(observedWorkerStats(pipeline->stats(uint32_t(milliseconds)),pipeline->takeReadbackObservation()));measured=now;}
          // The secure desktop shows: stills at their own pace, not the captures' frame events.
          if(pipeline->secureActive()){
            if(const auto wait=pipeline->secureWait();wait.count()>0)std::this_thread::sleep_for(wait);
            else{
              // A helper streams: the next tick starts with its next frame (or after 16 ms on a still prompt).
              HANDLE events[secure::trackOutputs]{};DWORD count=0;for(const auto& mapping:secureMappings)if(mapping&&mapping->event.value)events[count++]=mapping->event.value;
              if(count)WaitForMultipleObjects(count,events,FALSE,16);else std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            next=Clock::now();continue;
          }
          // A newly composed frame of any output starts the next tick at once
          // (re-phasing the schedule), never sooner than the frame-rate cap
          // after the last tick that emitted media.
          next+=period;if(next<now)next=now;
          auto wakeEvents=pipeline->frameEvents();if(inputWake_.handle)wakeEvents.push_back(inputWake_.handle);
          if(pacer.wait(next,wakeEvents,lastWork+captureMinimumGap(period)))next=Clock::now();
        }
      }catch(const std::exception& error){
        // A locked session (and a notice at its limit) ends as for one output: no_interactive_session, which the
        // client explains, not capture_failed ("Host가 화면을 준비하지 못했어요").
        const std::string reason=std::string_view(error.what())==secureDesktopUnavailable?"no_interactive_session":streamCapturePublicFailure(error.what(),"capture_failed",sizesV2);
        try{if(active_&&!stopped_)text("{\"type\":\"stream.unavailable\",\"sessionId\":"+jsonQuote(plan.sessionId)+",\"reason\":"+jsonQuote(reason)+'}');}catch(...){ }}
      catch(...){try{if(active_&&!stopped_)text("{\"type\":\"stream.unavailable\",\"sessionId\":"+jsonQuote(plan.sessionId)+",\"reason\":\"capture_failed\"}");}catch(...){ }}
      streamStop_=true;audioGo=true;if(audio.joinable())audio.join();input->close();input_.reset();active_=false;
    });
  }
  void suspendStreaming() { stopStream(); }
  // media_warmup.hpp: the session worker calls this before it prepares the
  // display of a single-output start; the stream's capture takes the device.
  void warmMedia(bool audio) { captureDevices_.warm(audio); }
 private:
  // media_warmup.hpp: the device the next capture of this worker starts with.
  CaptureDevices captureDevices_{windowsCaptureDeviceAccess()};
  using Outgoing = OutgoingPacket;
  // Adaptive bitrate (docs/adaptive-bitrate.md): the socket owner runs the
  // controller; the encoder owner applies a requested target of its session.
  AdaptiveBitrateSession adaptive_;
  std::atomic<int> requestedBitrate_{0}, autoBitrate_{0}; std::string autoSession_;
  // stream-size-v1 (stream_size.hpp): the client's latest decoder report (and
  // when it arrived), and what this session learned about load across resizes.
  std::atomic<uint64_t> clientDecoder_{0}, clientDecoderAt_{0};
  std::mutex scaleMemoryMutex_; std::string scaleMemorySession_; EncodeScaleMemory scaleMemory_;
  std::optional<ClientDecodeReport> clientDecodeReport() const {
    const auto packed = clientDecoder_.load();
    if (!packed || GetTickCount64() - clientDecoderAt_.load() > 3000) return {};
    return unpackDecoderReport(packed);
  }
  EncodeScaleMemory recallScale(const std::string& session) { std::lock_guard lock(scaleMemoryMutex_); return scaleMemorySession_ == session ? scaleMemory_ : EncodeScaleMemory{}; }
  void rememberScale(const std::string& session, const EncodeScaleMemory& memory) { std::lock_guard lock(scaleMemoryMutex_); scaleMemorySession_ = session; scaleMemory_ = memory; }
  // input-latency-v1 (input_latency.hpp): service stamps receipt, worker
  // reports injection and tags frames, service acknowledges at its send.
  InputLatencySession inputLatency_;
  // Set after every successful injection: an idle capture loop starts its next tick at once
  // instead of at the next fixed tick (a capture without a new-frame event). The frame-rate cap still applies.
  struct WakeEvent { HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr); ~WakeEvent() { if (handle) CloseHandle(handle); } } inputWake_;
  void wakeCapture() { if (inputWake_.handle) SetEvent(inputWake_.handle); }
  // cursor-v1 (cursor_channel.hpp): the capture owner leaves the cursor out of
  // the video of a granted session and describes it in kind-3 records. Shape
  // ids increase across the resize restarts of one session; the control thread
  // marks where each injected pointer event put the cursor.
  cursor::SessionShapeIds cursorIds_; cursor::InjectedPointer cursorInput_;
  // The service asked for the cursor again (AgentBridge::cursorResend); taken by the capture loop before its next key frame.
  std::atomic<bool> cursorResend_{false};
  // cursorArea_: the output the injected positions map onto; the capture
  // thread moves it when it follows an application's mode change.
  std::atomic<bool> cursorSeparated_{false}; std::mutex cursorAreaMutex_; RECT cursorArea_{};
  void cursorArea(RECT area){std::lock_guard lock(cursorAreaMutex_);cursorArea_=area;}
  void configureCursor(const Json& start,RECT area){cursorResend_=false;cursorSeparated_=cursor::sessionRequested(start);cursorArea(area);cursorInput_.reset();}
  // cursor-echo-v1 (cursor_channel.hpp): granted for the session; the client's
  // current request (0 none, else its number; kept across the stream restarts
  // of a resize of the same session like caret-v1, never into another
  // session); the event that wakes an idle poller when it changes.
  std::atomic<bool> cursorEchoGranted_{false}; std::atomic<uint32_t> cursorEchoRequest_{0},cursorEchoNumber_{0}; std::string cursorEchoSession_;
  WakeEvent cursorEchoWake_;
  void setCursorEcho(bool on){
    if(on){uint32_t number=cursorEchoNumber_.fetch_add(1)+1;if(!number)number=cursorEchoNumber_.fetch_add(1)+1;cursorEchoRequest_=number;}else cursorEchoRequest_=0;
    if(cursorEchoWake_.handle)SetEvent(cursorEchoWake_.handle);
  }
  void configureCursorEcho(const Json& start){
    cursorEchoGranted_=cursor::echoSessionRequested(start);if(bridge_)return;
    if(!cursorEchoGranted_||cursorEchoSession_!=sessionId_)setCursorEcho(false);
    cursorEchoSession_=cursorEchoGranted_?sessionId_:std::string();
    cursorCompose_.configure(cursor::composeSessionRequested(start),sessionId_);
  }
  // cursor-compose-v1: the same session rules; the capture reads it every tick.
  cursor::ComposeRequest cursorCompose_;
  void cursorEchoEnd(){setCursorEcho(false);cursorEchoSession_.clear();cursorCompose_.end();}
  // text-input-v1: only a session.start carrying the granted marker accepts
  // `text` input; any other session ignores it (and keeps keys).
  std::atomic<bool> textInput_{false};
  // touch-input-v1: likewise for `touch` frames (touch_input.hpp).
  std::atomic<bool> touchInput_{false};
  // relative-mouse-v1: likewise for `moveRelative` (remote_pointer.hpp).
  std::atomic<bool> relativeMouse_{false};
  // caret-v1 (caret_follow.hpp): granted for the session, the client's current
  // watch request (kept across the stream restarts of a resize) and the user-
  // session tracker (worker only). stop() ends it before the queue goes away.
  std::atomic<bool> caretGranted_{false}, caretWanted_{false}; std::string caretSession_;
  CaretTracker caret_{[this](const std::string& message) { text(message); }};
  // Called after a successful injection: the hotspot position its normalized
  // coordinate maps to (the same mapping as planRemotePointerInput).
  void markCursorInput(const Json& message){
    if(!cursorSeparated_)return;
    const auto event=message.get("event").string();
    if(event!="move"&&event!="button"&&event!="wheel")return;
    const auto* x=std::get_if<double>(&message.get("x").value);const auto* y=std::get_if<double>(&message.get("y").value);
    if(!x||!y||!std::isfinite(*x)||!std::isfinite(*y)||*x<0||*x>1||*y<0||*y>1)return;
    RECT area{};{std::lock_guard lock(cursorAreaMutex_);area=cursorArea_;}const double width=double(area.right-area.left),height=double(area.bottom-area.top);if(width<2||height<2)return;
    cursorInput_.injected(POINT{LONG(std::lround(area.left+*x*(width-1))),LONG(std::lround(area.top+*y*(height-1)))},hostMonotonicUs());
  }
  Credentials credentials_; Status status_; AgentBridge* bridge_ = nullptr; AgentBridge::Send local_; std::atomic<bool> stopped_{true}, streamStop_{true}, active_{false};
  BrokerConnectionStatus* brokerStatus_ = nullptr; std::atomic<uint64_t> brokerAttempt_{0};
  // boot-ready-v1: the service's timeline (tests substitute their own).
  BootTimeline* boot_ = &bootTimeline();
  // The first written report the broker shows as 연결 준비됨 (parsed only until then).
  void bootReady(const std::string& capabilities) noexcept { if(!boot_->reached(BootStage::ready))try{if(JsonParser::parse(capabilities).get("streaming").boolean())boot_->mark(BootStage::ready);}catch(...){ } }
  HostDiagnosticSchedule diagnosticSchedule_;
  std::function<HostUpdateEnvironment(const std::wstring&)> hostUpdateEnvironment_; std::unique_ptr<HostUpdater> updater_;
  std::function<HostPowerEnvironment()> hostPowerEnvironment_; std::unique_ptr<HostPower> power_; std::function<void()> machineChanged_;
  std::function<void(uint32_t,uint32_t)> captureDiagnostic_;
  // audio_capture_control.hpp: one sound capture record (stage 47 of the worker's fixed record; 0 is none).
  void audioCaptureRecord(uint32_t value) noexcept { if(value&&captureDiagnostic_)try{captureDiagnostic_(audioCaptureRecordStage,value);}catch(...){ } }
  std::atomic<HINTERNET> socket_{nullptr}; std::thread worker_, capture_;
  // queueMutex_ guards the queue, queueClosed_ and sending_ (the sender has
  // taken a packet and is writing it).
  std::mutex queueMutex_; std::condition_variable queueCondition_; HostSendQueue queue_; bool queueClosed_ = true, sending_ = false;
  // audio-stats-v1 (audio_stats.hpp): the worker's capture window, the service's send window.
  AudioCaptureWindow audioCapture_; AudioSendWindow audioSend_;
  // A video drop asks the encoder owner for an IDR (bridge, or this Agent's
  // own capture loop), at most every 500 ms; drops are recorded at most every 30 s
  // (the broker keeps only the newest eight Host events).
  // resourcesUptime_: the broker's ACK named host-resources-v2 (uptime in the heartbeat's resources).
  std::atomic<bool> keyFrameRequested_{false}, closingNegotiated_{false}, resourcesUptime_{false}, signinNegotiated_{false};
  // udp-media-v1 LOSS for the capture loop: the oldest last-good PTS asked for, plus one (0: none).
  std::atomic<uint64_t> recoverRequested_{0};
  void requestRecover(uint64_t lastGood) {
    const uint64_t want = lastGood + 1; uint64_t seen = recoverRequested_.load();
    while ((!seen || want < seen) && !recoverRequested_.compare_exchange_weak(seen, want)) {}
  }
  StreamObservability observability_;
  std::atomic<uint64_t> observedWriteUs_{0};
  std::atomic<bool> observedWriteKnown_{false};
  std::mutex dropMutex_; Clock::time_point lastKeyFrameRequest_{}, lastDropRecord_{}, lastCloseRecord_{}; size_t droppedVideo_ = 0, droppedOther_ = 0; bool dropReported_ = false;
  std::string sessionId_; SessionStopBinding stopBinding_; RemoteInput input_; std::atomic<uint16_t> mediaEpoch_{0};
  std::atomic<std::shared_ptr<TrackSourceInput>> trackInput_;
  // Role::secure (worker only): the service's answer to this worker's helper
  // request, taken by the capture loop. worker.secure.ready carries two handles
  // the service duplicated into this process; they are owned from the moment the
  // message is read, so an answer nobody takes closes them. Input never passes
  // through here: the service routes it to the helper itself.
  std::mutex secureGrantMutex_; std::optional<secure::worker::Grant> secureGrant_; std::atomic<uint32_t> secureDenied_{0}, secureSeq_{0};
  // Independent outputs (secure_desktop_tracks.hpp): the same answers, one helper per output ("output": 0 or 1).
  std::array<std::optional<secure::worker::Grant>, secure::trackOutputs> trackSecureGrant_; std::array<std::atomic<uint32_t>, secure::trackOutputs> trackSecureDenied_{}, trackSecureSeq_{};
  void secureAnswer(const std::string& type, const Json& message, bool currentSession) {
    const auto output = secure::trackOutput(message);
    if (type == secure::controlFailed) { if (currentSession) if (const auto seq = secure::worker::controlSeq(message)) (output ? trackSecureDenied_[*output] : secureDenied_).store(*seq); return; }
    auto grant = secure::worker::adoptGrant(message);  // owned (and closed) even when it is not for this session
    if (!currentSession) return;
    std::lock_guard<std::mutex> lock(secureGrantMutex_);
    if (output) trackSecureGrant_[*output] = std::move(grant); else secureGrant_ = std::move(grant);
  }
  std::optional<secure::worker::Grant> trackSecureTakeGrant(size_t output) { std::lock_guard<std::mutex> lock(secureGrantMutex_); return output < secure::trackOutputs ? std::exchange(trackSecureGrant_[output], std::nullopt) : std::nullopt; }
  std::optional<secure::worker::Grant> secureTakeGrant() { std::lock_guard<std::mutex> lock(secureGrantMutex_); return std::exchange(secureGrant_, std::nullopt); }
  // System input (system_input.hpp): for a stream whose worker cannot reach elevated windows the service writes
  // the client's input to its SYSTEM injector and marks the copy it hands this worker, which then injects nothing
  // itself and only keeps its bookkeeping (cursor mark, capture wake, latency sample). The marker comes from the
  // service's pipe only; the service removes a client's own field of that name. systemButtons_: the pointer
  // buttons such inputs hold (a drag, for the latency sample); control thread only.
  std::atomic<bool> systemInputSeen_{false}; std::set<std::string> systemButtons_;
  // Independent outputs: the sequence number of an input the service asks this worker to place (its marker and
  // "systemSeq"). The answer is system_input_tracks.hpp's: ok and, for a positional event, the desktop pixel.
  std::optional<uint32_t> systemInputAsked(const Json& message) const {
    const auto* marker = std::get_if<bool>(&message.get(sysinput::workerMarker).value);
    const auto* seq = std::get_if<double>(&message.get(sysinput::workerSeq).value);
    if (!local_ || bridge_ || !marker || !*marker || !seq || !std::isfinite(*seq) || *seq < 1 || *seq > 2147483647.0 || std::floor(*seq) != *seq) return {};
    return uint32_t(*seq);
  }
  // The rectangle the stream's picture is on the desktop, for the injector's pointer positions.
  void systemInputArea(const std::string& session, const RECT& area) {
    if (!local_ || bridge_ || session.empty()) return;
    try { text(std::string("{\"type\":\"") + sysinput::controlArea + "\",\"sessionId\":" + jsonQuote(session) + ",\"left\":" + std::to_string(area.left) + ",\"top\":" + std::to_string(area.top) + ",\"right\":" + std::to_string(area.right) + ",\"bottom\":" + std::to_string(area.bottom) + '}'); } catch (...) {}
  }
  // Lossless tiles are sent only for the session whose client opted in (ping
  // nonce prefix "lt1-") and did not pause them since ("lt0-": 게임 마우스
  // holds the mouse). Both survive the legacy resize restart of the same session.
  mutable std::mutex losslessMutex_; std::string losslessSession_; bool losslessOn_ = false;
  bool losslessRequested(const std::string& session) const { std::lock_guard lock(losslessMutex_); return !session.empty() && session == losslessSession_ && losslessOn_; }
  void requestLossless(bool on = true) {
    // Session0 answers pings itself; the worker owning the encoder needs the opt-in.
    if (bridge_) { bridge_->control(JsonParser::parse(on ? R"({"type":"stream.lossless","protocol":"tiles-v1"})" : R"({"type":"stream.lossless","protocol":"tiles-v1","on":false})")); return; }
    std::lock_guard lock(losslessMutex_); losslessSession_ = sessionId_; losslessOn_ = on;
  }
#ifdef MULTITAP_TRACK_SOURCE_TEST
  TrackSourcePipeline::Monitors trackMonitors_;TrackSourcePipeline::CaptureFactory trackCapture_;std::optional<TrackDesktop> trackDesktop_;
 public:
  // Before localTrackStart: the desktop observations the pipeline reads (secure desktop tests).
  void trackTestDesktop(TrackDesktop desktop){trackDesktop_=std::move(desktop);}
 private:
#endif
  void report(const std::wstring& text) { if (status_) status_(text); }
  // udp-media-v1 (udp_probe_leg.hpp): the current stream's leg, in the service only.
  // Runs on the channel's receive thread, so it may neither wait nor throw: a throw
  // here would end the channel, and with it the stream.
  std::atomic<std::shared_ptr<UdpProbeLeg>> udpProbe_;
  std::atomic<std::shared_ptr<DirectInputDispatcher>> directInput_;
  uint64_t directSampleAt_=0;uint32_t directSampleCount_=0;
  void directInput(const DirectInputItem& item){
    if(stopped_||!bridge_)return;auto message=JsonParser::parse(item.body);
    if(message.get("type").string()=="input.fence")return;
    if(message.get("type").string()=="input"&&!std::holds_alternative<std::nullptr_t>(message.get("sample").value)){
      if(item.receivedUs>=directSampleAt_&&item.receivedUs-directSampleAt_>=1000000){directSampleAt_=item.receivedUs;directSampleCount_=0;}
      if(++directSampleCount_>20)if(auto* object=std::get_if<Json::Object>(&message.value))object->erase("sample");
    }
    if(const auto stamped=inputLatency_.stamp(message,item.receivedUs))bridge_->control(*stamped);else bridge_->control(message);
  }
  std::atomic<bool> routed_{false};  // this channel reached the Broker through host-route-v1
  HostResumeHold resume_;  // host-resume-v1: a stream kept through a lost channel (host_resume.hpp)
  // Restart recovery (broker_reconnect.hpp): the wait before the next dial, its
  // jitter, and when the broker last ended a stream as server_stopping (0: never).
  BrokerReconnectWait reconnectWait_; std::minstd_rand reconnectJitter_{std::random_device{}()}; std::atomic<uint64_t> brokerStoppingAt_{0};
  // run() only: the wait after a channel (or a dial) ended with this peer close status (0: none).
  uint32_t channelEndWait(uint32_t remoteCloseCode){const auto now=GetTickCount64();return reconnectWait_.next(brokerRestartClose(remoteCloseCode,brokerStoppingAt_.exchange(0),now),now,uint32_t(reconnectJitter_()));}
  // A channel ended: a granted stream outlives a lost (not refused) one for the
  // next channel; anything else stops it. A stopping service stops it right after.
  void channelEnded(const char* failure){if(auto input=directInput_.load())input->pause();if(!resume_.lost(bridge_&&active_&&!sessionId_.empty(),failure,GetTickCount64()))stopStream();else if(bridge_)bridge_->clipboardAway();}
  bool udpEligible_=false,udpRelativeAllowed_=false;  // auto bitrate, one output, no tracks: all else stays on the channel
  void udpGrant(const Json& message,size_t bytes) noexcept {
    try {
      auto grant=parseUdpGrant(message,bytes);if(!grant)return;
      udpProbeEnd();directSampleAt_=0;directSampleCount_=0;
      const auto input=grant->direct?std::make_shared<DirectInputDispatcher>([this](const DirectInputItem& item){directInput(item);}):nullptr;
      directInput_.store(input);
      auto leg=UdpProbeLeg::start(std::move(*grant),{[](uint32_t stage,uint32_t relay){recordHostEvent("hostAction",{{"action",udpProbeAction},{"stage",stage},{"error",relay},{"ok",stage==udpStageBound}});},udpResolveRelay,
        {[this]{bridge_->keyFrame();},[this](uint64_t rtt,uint32_t received,uint32_t lost){return adaptive_.udpFeedback(rtt,received,lost);},[this]{adaptive_.udpBacklog();},[this](bool onUdp){adaptive_.udpPath(onUdp);return adaptive_.target();},[this](uint64_t lastGood){bridge_->recover(lastGood);},[this,session=sessionId_](const std::string& address,uint16_t port){text("{\"type\":\"udp.local\",\"protocol\":\"udp-direct-v1\",\"sessionId\":"+jsonQuote(session)+",\"address\":"+jsonQuote(address)+",\"port\":"+std::to_string(port)+"}");},[this,session=sessionId_](const std::string& address,uint16_t port){text("{\"type\":\"udp.local\",\"protocol\":\"udp-direct-v1\",\"candidateType\":\"srflx\",\"sessionId\":"+jsonQuote(session)+",\"address\":"+jsonQuote(address)+",\"port\":"+std::to_string(port)+"}");},[input,relative=udpRelativeAllowed_](uint32_t sequence,uint16_t,const std::string& body){try{if(input&&validDirectInput(JsonParser::parse(body),relative))input->submit(false,{sequence,body,hostMonotonicUs()});}catch(...){}}},
        [](uint32_t stage,uint32_t reason,uint32_t cause){recordHostEvent("hostAction",{{"action",udpPathAction},{"stage",stage},{"error",reason},{"cause",cause},{"ok",stage!=udpPathOnWss}});}});
      if(auto old=udpProbe_.exchange(std::move(leg)))old->close();
    } catch (...) { }
  }
  void udpKey(const Json& message) noexcept {
    try {
      const auto leg=udpProbe_.load();if(!leg||!udpEligible_||message.get("sessionId").string()!=sessionId_)return;
      if(const auto reply=leg->exchange(message,sessionId_,adaptive_.target()))text(*reply);
    } catch (...) { }
  }
  void udpProbeEnd() noexcept {if(auto leg=udpProbe_.exchange({}))leg->close();if(auto input=directInput_.exchange({}))input->stop();}
  // Ends the broker channel. The close of an established service channel is
  // recorded with its fixed reason and Win32 error (hostAction 241) so the
  // broker's Host diagnostics say why after the reconnect.
  void closeTransport(const char* reason="transport_closed",DWORD error=0,uint32_t remoteCode=0) {
    // The status pipe keeps its released vocabulary.
    if(brokerStatus_&&!stopped_)brokerStatus_->disconnected(brokerAttempt_.load(),std::string_view(reason)=="send_queue_stalled"?"send_failed":reason);
    const auto socket=socket_.exchange(nullptr);signinNegotiated_=false;
    if(socket){if(bridge_)try{bridge_->mainDisconnected(socket);}catch(...){}WinHttpCloseHandle(socket);if(brokerStatus_&&!stopped_)recordChannelClose(reason,error,remoteCode);}
    { std::lock_guard lock(queueMutex_); queueClosed_ = true; queue_.clear(); }
    queueCondition_.notify_all();
  }
  void recordChannelClose(const char* reason,DWORD error,uint32_t remoteCode){
    { std::lock_guard lock(dropMutex_); const auto now=Clock::now(); if(lastCloseRecord_!=Clock::time_point{}&&now-lastCloseRecord_<std::chrono::seconds(10))return; lastCloseRecord_=now; }
    if(remoteCode>=1000&&remoteCode<=4999)recordHostEvent("hostAction",{{"action",241},{"stage",hostChannelCloseCode(reason)},{"error",int64_t(error)},{"cause",int64_t(remoteCode)},{"ok",false}});
    else recordHostEvent("hostAction",{{"action",241},{"stage",hostChannelCloseCode(reason)},{"error",int64_t(error)},{"ok",false}});
  }
  // host-closing-v1: a deliberate close first tells a broker that accepts it,
  // so its sessions end with this reason. Queued media is dropped (the stream
  // ends anyway); waits at most 300 ms for the sender to write the notice.
  void closingNotice(const char* reason){if(closingNegotiated_&&socket_.load())queueClosingNotice(reason,true);}
  void queueClosingNotice(const char* reason,bool wait){
    const std::string message=std::string("{\"type\":\"host.closing\",\"reason\":")+jsonQuote(reason)+'}';
    std::unique_lock lock(queueMutex_);if(queueClosed_)return;
    queue_.dropMedia();if(!queue_.admit({true,{message.begin(),message.end()},false,Clock::now()},0).queued)return;
    queueCondition_.notify_all();
    if(wait)queueCondition_.wait_for(lock,std::chrono::milliseconds(300),[this]{return queueClosed_||(queue_.empty()&&!sending_);});
  }
  bool admitMainText(HINTERNET expected,const std::string& value){
    if(!expected||value.empty()||value.size()>65536)return false;
    std::lock_guard lock(queueMutex_);
    if(stopped_||socket_.load()!=expected||queueClosed_)return false;
    const auto admitted=queue_.admit({true,{value.begin(),value.end()},false,Clock::now()},0);
    if(!admitted.queued)return false;queueCondition_.notify_all();return true;
  }
  void enqueue(Outgoing packet) {
    if (!bridge_ && !packet.text) stampMediaEpoch(packet.bytes,mediaEpoch_.load());
    if (local_) { if(!bridge_&&!packet.capabilities)local_(packet.text,std::move(packet.bytes));else (void)publishOutgoing(packet,[this](bool text,const std::vector<uint8_t>& bytes){local_(text,bytes);return true;});return; }
    // An adaptive session observes what enters the send queue and its depth.
    const bool adaptive = adaptive_.active();
    // The worker's mark on video that answers a LOSS (mediaPacket) is for the UDP leg only.
    bool recovery = false;
    if (!packet.text && packet.bytes.size() > 1 && packet.bytes[0] == 1 && (packet.bytes[1] & 2)) { recovery = true; packet.bytes[1] &= uint8_t(~2); }
    if (adaptive) { if (packet.text) adaptive_.annotate(packet.bytes); adaptive_.outgoing(packet.text, packet.bytes); }
    if (bridge_) if (const auto leg = udpProbe_.load(); leg && leg->outgoing(packet.text, packet.bytes, recovery)) return;
    packet.queuedAt = Clock::now();
    SendQueueAdmission admission; uint64_t oldestUs = 0; size_t depthBytes = 0, depthPackets = 0;
    { std::lock_guard lock(queueMutex_); if (queueClosed_) return;
      // A media backlog drops video until the next IDR (send_queue.hpp); it
      // used to close this whole channel and with it every session of the Host.
      admission = queue_.admit(std::move(packet), adaptive ? adaptiveHostQueuePackets : 32);
      if (admission.queued) { oldestUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - queue_.oldest()).count()); depthBytes = queue_.bytes(); depthPackets = queue_.packets(); } }
    if (admission.stalled) { report(L"서버 전송이 멈췄습니다 · 연결을 다시 설정합니다."); closeTransport("send_queue_stalled"); return; }
    if (admission.droppedVideo || admission.droppedOther) mediaDropped(admission);
    if (admission.droppedAudio) audioSend_.dropped(admission.droppedAudio);
    if (admission.requestKeyFrame) requestKeyFrame();
    if (admission.queued) { queueCondition_.notify_one(); if (adaptive) adaptive_.queue(oldestUs, depthBytes, depthPackets); }
  }
  // hostAction 240: media the send queue skipped (stage 1: video, the stream
  // waits for an IDR; stage 2: audio/tiles only), counted per record.
  void mediaDropped(const SendQueueAdmission& admission) {
    bool first = false, record = false; size_t video = 0, other = 0;
    { std::lock_guard lock(dropMutex_); droppedVideo_ += admission.droppedVideo; droppedOther_ += admission.droppedOther;
      const auto now = Clock::now(); first = !std::exchange(dropReported_, true);
      if (lastDropRecord_ == Clock::time_point{} || now - lastDropRecord_ >= std::chrono::seconds(30)) { record = true; lastDropRecord_ = now; video = std::exchange(droppedVideo_, 0); other = std::exchange(droppedOther_, 0); } }
    if (first) report(L"서버 전송이 밀려 영상 프레임을 건너뜁니다.");
    if (record) recordHostEvent("hostAction", {{"action", 240}, {"stage", video ? 1 : 2}, {"dropped", int64_t(std::min<size_t>(video + other, 4294967295u))}});
  }
  void requestKeyFrame() {
    { std::lock_guard lock(dropMutex_); const auto now = Clock::now(); if (lastKeyFrameRequest_ != Clock::time_point{} && now - lastKeyFrameRequest_ < std::chrono::milliseconds(500)) return; lastKeyFrameRequest_ = now; }
    if (bridge_) bridge_->keyFrame(); else keyFrameRequested_ = true;
  }
  void text(const std::string& message) { enqueue({true, {message.begin(), message.end()}}); }
  std::string observedWorkerStats(const std::string& raw,std::optional<std::pair<uint64_t,uint64_t>> readback={}) noexcept {
    return observability_.augment<Json>(raw,false,{},readback,JsonParser::parse,streamObservationJson);
  }
  ObservedSendQueue observedQueue() {
    ObservedSendQueue snapshot;
    {std::lock_guard lock(queueMutex_);snapshot.packets=queue_.packets();snapshot.bytes=queue_.bytes();
      if(!queue_.empty())snapshot.oldestUs=uint64_t(std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-queue_.oldest()).count()));}
    if(observedWriteKnown_)snapshot.writeUs=observedWriteUs_.load();
    return snapshot;
  }
  template<class Sink> bool publishOutgoing(const Outgoing& packet,Sink&& sink){
    // Build readiness on the sole sending thread, never when an announcement
    // is queued. Otherwise a slow old producer can overwrite a newer profile.
    // Verified initial/resize metadata must follow its fresh capabilities on
    // the SAME channel, before the client can receive the first new IDR.
    const auto type=bridge_&&packet.text&&!packet.capabilities?JsonParser::parse(std::string(packet.bytes.begin(),packet.bytes.end())):Json{};
    // A result with its own capability report (host.display.result) is newer
    // than the heartbeat's: the next heartbeat reads a fresh one.
    if(std::holds_alternative<Json::Object>(type.get("capabilities").value))heartbeatCapabilities_={};
    // input-latency-v1: the worker's private injection report becomes the
    // public acknowledgement here, at the actual write, so its hold time also
    // covers this send queue. Reports/tags of an unnegotiated or retired
    // session are never written: a released broker would reject the channel.
    if(type.get("type").string()=="worker.input.latency"||type.get("type").string()=="input.frame"){
      const auto value=type.get("type").string()=="input.frame"?inputLatency_.frame(type,hostMonotonicUs()):inputLatency_.acknowledge(type,hostMonotonicUs());
      if(!value)return true;
      // The tag's video packet follows: its write is timed against the
      // worker's encoder output (the private field is not in `value`).
      if(type.get("type").string()=="input.frame")inputLatency_.expectVideo(type,hostMonotonicUs());
      const std::vector<uint8_t> bytes(value->begin(),value->end());
      return sink(true,bytes);
    }
    const bool metadata=type.get("type").string()=="stream.info";
    if(packet.capabilities||metadata){
      if(!bridge_)return false;
      const auto capabilities=bridge_->capabilities();
      const auto message="{\"type\":\"host.capabilities\",\"capabilities\":"+capabilities+'}';
      const std::vector<uint8_t> bytes(message.begin(),message.end());
      if(!sink(true,bytes))return false;
      heartbeatCapabilities_={capabilities,Clock::now()};
      bootReady(capabilities);
      if(packet.capabilities)return true;
    }
    // input-latency-v1 sessions: the statistics written now carry this send
    // path's share for recently tagged packets (observational member).
    if(bridge_&&type.get("type").string()=="stream.stats"&&packet.bytes.size()>1&&packet.bytes.back()=='}'){
      // audio-stats-v1: this window's audio on WebSocket and on the UDP path.
      if(const auto leg=udpProbe_.load()){const auto [sent,dropped]=leg->takeAudio();if(sent)audioSend_.sent(sent);if(dropped)audioSend_.dropped(dropped);}
      const auto extra=inputLatency_.sendTimingJson(hostMonotonicUs())+audioSend_.takeJson();
      if(!extra.empty()||observability_.enabled()){
        auto raw=std::string(packet.bytes.begin(),packet.bytes.end());raw.insert(raw.size()-1,extra);
        if(observability_.enabled()){raw=observability_.augment<Json>(raw,true,observedQueue(),{},JsonParser::parse,streamObservationJson);queueHostObservationLog(raw);}
        return sink(true,std::vector<uint8_t>(raw.begin(),raw.end()));
      }
    }
    if(bridge_&&!packet.text&&packet.bytes.size()>=12&&packet.bytes[0]==1){
      uint64_t pts=0;for(int n=0;n<8;++n)pts|=uint64_t(packet.bytes[4+n])<<(8*n);
      const auto started=hostMonotonicUs();const bool written=sink(false,packet.bytes);
      if(written){const auto finished=hostMonotonicUs();inputLatency_.videoWritten(pts,started,finished);if(observability_.enabled()){observedWriteUs_=finished-started;observedWriteKnown_=true;}}
      return written;
    }
    // audio-stats-v1: an audio packet's wait in the send queue (queued -> its write starts).
    const bool audio=bridge_&&!packet.text&&packet.bytes.size()>12&&packet.bytes[0]==2&&packet.queuedAt!=Clock::time_point{};
    const uint64_t waited=audio?uint64_t(std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-packet.queuedAt).count())):0;
    if(bridge_&&!packet.text&&observability_.enabled()){
      const auto began=Clock::now();const bool written=sink(false,packet.bytes);
      if(written){observedWriteUs_=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-began).count());observedWriteKnown_=true;}
      if(written&&audio)audioSend_.written(waited);
      return written;
    }
    const bool written=sink(packet.text,packet.bytes);
    if(written&&audio)audioSend_.written(waited);
    return written;
  }
  // Worker only: caret-v1 for this start. The watch request survives a restart
  // of the same session (resize); any other session starts unwatched.
  void configureCaret(const Json& start,const RECT& source){
    caretGranted_=caretSessionRequested(start);if(bridge_)return;
    const bool same=caretGranted_&&caretSession_==sessionId_;
    if(!same){caret_.watch(false);caretWanted_=false;}
    caretSession_=caretGranted_?sessionId_:std::string();
    if(caretGranted_){caret_.configure(sessionId_,source);if(caretWanted_)caret_.watch(true);}
  }
  void caretEnd(){caret_.watch(false);caretWanted_=false;caretSession_.clear();}
  // background-video-v1 (background_video.hpp): granted per session.start; the client's latest request survives a
  // restart of the same session (a resize), any other session starts shown.
  std::atomic<bool> backgroundGranted_{false}, streamBackground_{false}; std::string backgroundSession_;
  void configureBackground(const Json& start){
    const bool granted=backgroundVideoGranted(start);
    if(!granted||backgroundSession_!=sessionId_)streamBackground_=false;
    backgroundGranted_=granted;backgroundSession_=granted?sessionId_:std::string();
  }
  void backgroundEnd(){streamBackground_=false;backgroundGranted_=false;backgroundSession_.clear();}
  void stopStream(bool stopBridge=true) {
    resume_.clear();udpProbeEnd();active_ = false; streamStop_ = true;if(auto input=trackInput_.exchange({}))input->close();input_.reset();adaptive_.stop();inputLatency_.stop();observability_.reset();observedWriteKnown_=false;cursorSeparated_=false;cursorInput_.reset();
    if (bridge_&&stopBridge) bridge_->stop();
    if (capture_.joinable()) capture_.join();
    finishHostObservationLog(sessionId_);
    { std::lock_guard lock(queueMutex_); queue_.clear(); }
    { std::lock_guard lock(dropMutex_); dropReported_ = false; droppedVideo_ = droppedOther_ = 0; }
    audioSend_.clear();
    keyFrameRequested_ = false; recoverRequested_ = 0;
    sessionId_.clear();
  }
  void stream(const Json& control) {
    const auto startBegan = std::chrono::steady_clock::now();
    stopStream(); sessionId_ = control.get("sessionId").string();
    const bool epochProtocol=control.get("resizeProtocol").string()=="epoch-v1";const auto requestedEpoch=control.get("epoch").number(0);if(requestedEpoch<0||requestedEpoch>65535||std::floor(requestedEpoch)!=requestedEpoch)throw std::runtime_error("Invalid stream epoch");mediaEpoch_=epochProtocol?uint16_t(requestedEpoch):0;
    std::string epochFields=epochProtocol?",\"sessionId\":"+jsonQuote(sessionId_)+",\"epoch\":"+std::to_string(mediaEpoch_.load()):"";
    const bool resized=epochProtocol&&!control.get("resizeId").string().empty();if(resized)epochFields+=",\"requestId\":"+jsonQuote(control.get("requestId").string())+",\"resizeId\":"+jsonQuote(control.get("resizeId").string());
    // A resize restart reports where its time went (resize_stages.hpp).
    const auto resizeStages=resized?std::optional<ResizeStages>(resizeStagesFrom(control)):std::nullopt;
    if (sessionId_.empty() || sessionId_.size() > 80 || sessionId_.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) throw std::runtime_error("Invalid paired session id");
    // Only the broker that granted input-latency-v1 sends its marker here; a
    // resize restart of the same session carries it again (fresh tagger epoch).
    inputLatency_.configure(control);
    observability_.configure(control);
    // Fixed 4-100 Mbps, or auto (bitrate-adaptive-v1) with a start and ceiling.
    // The socket owner (service, or a direct host) runs the controller.
    const auto rate = adaptiveBitrateRequest(control);
    if (bridge_ || !local_) adaptive_.start(sessionId_, rate, int(std::clamp(control.get("fps").number(60), 1.0, 60.0)), [this](const std::string& session, int value) { if (bridge_) bridge_->bitrate(session, value); else requestedBitrate_.store(value); });
    udpEligible_ = rate.adaptive && control.get("monitor").string() != "all";udpRelativeAllowed_=relativeMouseSessionRequested(control);
    if (bridge_) { active_ = true; bridge_->start(control, [this](bool isText, std::vector<uint8_t> bytes) { enqueue({isText, std::move(bytes)}); }); return; }
    // A resize restarts the encoder of the same session at its last auto rate.
    if (autoSession_ != sessionId_) { autoSession_ = sessionId_; autoBitrate_ = 0; requestedBitrate_ = 0; }
    const int resumeBitrate = rate.adaptive ? autoBitrate_.load() : 0;
    // Viewport streams encode the source at its own size within the pixel
    // budget (administrator setting "videoPixelBudget"; stream_encode_size.hpp).
    const bool viewportLimits = control.get("viewportEncoderLimitsProtocol").string() == "separate-v1";
    // stream-size-v1 (stream_size.hpp): no fixed budget; the client decoder's
    // limits and measured load, this Host's measured load and an optional
    // administrator cap. Its box may be portrait (two per-side ceilings).
    std::optional<DecoderLimits> sizeLimits; std::optional<uint64_t> pixelCap;
    if (viewportLimits) sizeLimits=streamSizeRequest(control);
    if (sizeLimits) { try { pixelCap = streamPixelCap(readPublicConfig()); } catch (...) { pixelCap.reset(); } }
    clientDecoder_ = 0;
    const bool sizesV2=streamSizeV2(control);
    const auto limit=[&](const char* key,double fallback,double maximum){const auto n=control.get(key).number(fallback);if(!std::isfinite(n)||n!=std::floor(n)||n<(sizesV2?16:128)||n>maximum)throw std::runtime_error("encoder_format_unsupported");return int(n);};
    const int maximumWidth=limit("width",1920,sizesV2?double(ownedOutputMaxSide):3840.0),maximumHeight=limit("height",1080,sizesV2?double(ownedOutputMaxSide):(sizeLimits?3840.0:2160.0));
    const int fps = int(std::clamp(control.get("fps").number(60), 1.0, 60.0)), bitrate = rate.bitrate;
    uint64_t pixelBudget = streamPixelBudgetDefault;
    if (viewportLimits && !sizeLimits) { try { pixelBudget = streamPixelBudget(readPublicConfig()); } catch (...) { pixelBudget = streamPixelBudgetDefault; } }
    const auto sourceNumber=[&](const char* key,double minimum,double maximum){const auto n=control.get(key).number(minimum-1);if(!std::isfinite(n)||n!=std::floor(n)||n<minimum||n>maximum)throw std::runtime_error("Validated virtual display geometry required");return int(n);};
    const LONG sourceLeft=LONG(sourceNumber("captureLeft",INT32_MIN,INT32_MAX)),sourceTop=LONG(sourceNumber("captureTop",INT32_MIN,INT32_MAX));const int sourceWidth=sourceNumber("captureWidth",16,INT32_MAX),sourceHeight=sourceNumber("captureHeight",16,INT32_MAX);
    if(!checkedFrameLayout(uint64_t(sourceWidth),uint64_t(sourceHeight),false)||!displayRectangleFits(sourceLeft,sourceTop,uint64_t(sourceWidth),uint64_t(sourceHeight)))throw std::runtime_error("Validated virtual display geometry required");
    const RECT source{sourceLeft,sourceTop,sourceLeft+sourceWidth,sourceTop+sourceHeight};const auto selectedCapture=parseCaptureSelection(control,source);validateCaptureSelection(selectedCapture,currentCaptureMonitors());input_.area(source);systemInputSeen_=false;systemButtons_.clear();systemInputArea(control.get("sessionId").string(),source);const std::wstring device=wide(control.get("displayDeviceName").string());const bool audioRequested=control.get("audio").boolean(true),audioOpusRequested=opusAudioRequested(control);
    // cursor-v1: only a session.start carrying the granted marker separates it.
    configureCursor(control,source);textInput_=textInputSessionRequested(control);touchInput_=touchInputSessionRequested(control);relativeMouse_=relativeMouseSessionRequested(control);configureCaret(control,source);configureCursorEcho(control);configureBackground(control);const bool cursorSeparated=cursorSeparated_,cursorEchoAllowed=cursorSeparated&&cursorEchoGranted_;
    streamStop_ = false; active_ = true; if (local_) stopped_ = false;
    capture_ = std::thread([this, maximumWidth, maximumHeight, fps, bitrate,source,sourceWidth,sourceHeight,device,audioRequested,audioOpusRequested,selectedCapture,epochFields,resized,session=sessionId_,rate,resumeBitrate,cursorSeparated,cursorEchoAllowed,viewportLimits,sizesV2,pixelBudget,sizeLimits,pixelCap,resizeStages,startBegan] {
      std::thread audio; std::atomic<bool> audioGo{false};
      const char* failureReason="encoder_failed";uint32_t captureStage=1;
      // The first stream.info completes a resize restart (the service then
      // clears it): a later failure ends the stream, it is no resize failure.
      bool announced=false;
      // Reports a failure once: from the capture loop before its capture,
      // encoder, cursor and watch threads are torn down (any of them may wait
      // on Windows while a display mode changes), else after an early failure.
      bool notified=false;
      const auto failed=[&](const char* what){
        if(notified)return;notified=true;
        streamStop_ = true; active_ = false; input_.reset();try{if(captureDiagnostic_)captureDiagnostic_(captureStage,streamCaptureFailureCode(what));}catch(...){ }report(L"화면 전송 중단: " + wide(what));failureReason=streamCapturePublicFailure(what,failureReason,sizesV2);if(!interactiveDesktopAvailable())failureReason="no_interactive_session";
        if (local_) text(captureFailureNotice(resized,announced,epochFields,session,failureReason)); else closeTransport();
      };
      try {
        StreamPowerActivity powerActivity;
        if (!powerActivity.active()) report(L"화면 절전 방지 요청을 적용하지 못했습니다. Windows 전원 설정을 확인하세요.");
        // Capture/encode (and x264's slice threads, WARP and this worker's
        // input injection) preempt normal-priority applications while streaming.
        StreamPriority priority;
        Runtime runtime;
        // A viewport stream (separate-v1 encoder limits) is encoded at its own
        // size up to the Host's pixel budget; the box is only a ceiling
        // (stream_encode_size.hpp). Every other stream keeps the released box
        // fit, whose relays and clients require that exact size.
        const double boxScale = std::min({1.0, double(maximumWidth) / sourceWidth, double(maximumHeight) / sourceHeight});
        // stream-size-v1: the scale controller owns the picture size (and may
        // restart the encoder at another scale; see the statistics below).
        std::optional<EncodeScaleController> scaler;
        if (sizeLimits) scaler.emplace(StreamSizeBounds{sourceWidth, sourceHeight, fps, maximumWidth, maximumHeight, *sizeLimits, pixelCap}, recallScale(session));
        const std::pair<int, int> encodeSize = scaler ? scaler->size() : viewportLimits
            ? streamEncodeSize(sourceWidth, sourceHeight, maximumWidth, maximumHeight, fps, pixelBudget)
            : std::pair<int, int>{std::max(16, int(sourceWidth * boxScale) & ~1), std::max(16, int(sourceHeight * boxScale) & ~1)};
        int width = encodeSize.first, height = encodeSize.second;
        // Auto never goes below the measured MFT floor of this geometry.
        int floorBitrate = std::min(adaptiveBitrateFloor(width, height, fps), rate.maximum);
        int applied = rate.adaptive ? std::clamp(resumeBitrate ? resumeBitrate : bitrate, floorBitrate, rate.maximum) : bitrate;
        // Lossless tiles of an auto session use the part of the target that
        // video leaves unused (one shared rate); updated by this thread.
        std::atomic<size_t> tileShare{size_t(applied) / 8}; std::deque<std::pair<Clock::time_point, size_t>> recentVideo; size_t recentVideoBytes = 0;
        // In place (optional) so that a stream-size-v1 rescale can rebuild them
        // at a new size behind the same references.
        std::optional<Capture> captureSlot; std::optional<Encoder> encoderSlot;
        // media_warmup.hpp: when this capture ends, its device goes to the next
        // capture of this worker (a resize restart) instead of being released.
        struct KeepCaptureDevice{CaptureDevices& devices;std::optional<Capture>& capture;~KeepCaptureDevice(){try{if(capture)devices.keep(capture->sharedDevice());}catch(...){ }}}keepCaptureDevice{captureDevices_,captureSlot};
        const StreamMemoryShape initialMemory{uint32_t(sourceWidth),uint32_t(sourceHeight),uint32_t(width),uint32_t(height),true};
        validateStreamMemory({&initialMemory,1});
        // Audio starts first: its loopback device opens while the capture and
        // the encoder are created instead of after them (stream.info waits
        // for it). Its packets and the video share this time base.
        const auto beginning = Clock::now();
        std::promise<bool> audioReady; auto ready = audioReady.get_future();
        (void)audioCapture_.takeJson();  // an earlier stream's leftovers
        // audio-opus-v1 (opus_audio.hpp): Opus where the broker granted it and
        // the encoder starts; stream.info states the format either way.
        std::optional<OpusAudioEncoder> opusEncoder; if (audioRequested && audioOpusRequested) opusEncoder = OpusAudioEncoder::create();
        const bool audioOpus = opusEncoder.has_value();
        if(audioRequested)audio = std::thread([this, beginning, &audioGo, promise = std::move(audioReady), opus = std::move(opusEncoder)]() mutable {
          const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); bool announced = false;
          // audio_capture_control.hpp: the capture opens again after any error or default device change until the
          // stream stops; each start, failure, restart and change is a fixed record (hostAction 297 in the service).
          try { check(com, "Audio COM startup"); Loopback loopback; const bool available = loopback.available(); promise.set_value(available); announced = true; if (!available) audioCaptureRecord(loopback.failureRecord()); if (available) { while (!audioGo && !streamStop_) std::this_thread::sleep_for(std::chrono::milliseconds(2)); if (!streamStop_) loopback.run(streamStop_, beginning, [this](std::vector<uint8_t> bytes) { enqueue({false, std::move(bytes)}); }, &audioCapture_, opus ? &*opus : nullptr, [this](uint32_t value) { audioCaptureRecord(value); }); } }
          catch (...) { if (!announced) promise.set_value(false); report(L"소리 캡처를 사용할 수 없습니다. 화면 연결은 유지합니다."); }
          if (SUCCEEDED(com)) CoUninitialize();
        });
        // capture_follow.hpp: the output this capture reads now. An application
        // may change its mode under the stream; the announced source stays
        // sourceWidth x sourceHeight and the picture keeps its size.
        RECT current=source;CaptureSelection selection=selectedCapture;
        auto stages=resizeStages.value_or(ResizeStages{});auto stageBegan=std::chrono::steady_clock::now();
        failureReason="capture_failed";captureStage=2;validateCaptureSelection(selectedCapture,currentCaptureMonitors());captureStage=3;captureSlot.emplace(width, height, source, captureDevices_.take());Capture& capture=*captureSlot;
        stages.captureMs=resizeStageMs(stageBegan,std::chrono::steady_clock::now());stageBegan=std::chrono::steady_clock::now();
        failureReason="encoder_failed";captureStage=4;encoderSlot.emplace(width, height, fps, applied,EncoderTuning{},EncoderBackend::automatic,capture.adapterLuid(),capture.gpuDevice());Encoder& encoder=*encoderSlot;
        stages.encoderMs=resizeStageMs(stageBegan,std::chrono::steady_clock::now());
        const auto observeEncoderOutput=[this,session](const EncoderOutputObservation& output){const auto packet=encoderUsagePacket({session,0,{"0",mediaEpoch_.load(),encoderDiagnostic(output.backend,output.hardware,output.softwareFallbackCode)}});if(!packet.empty())text(packet);};
        // encoder_gpu_retry.hpp: each step of a way back to the GPU encoder, as a fixed
        // record (hostAction 200 stage 245, error = kind | attempt << 8 | GPU code << 16).
        const auto observeGpuRetry=[this](uint32_t kind,uint32_t attempt,uint32_t code){try{if(captureDiagnostic_)captureDiagnostic_(45,gpuRetryRecordValue(kind,attempt,code));}catch(...){ }};
        if(local_)try{encoder.setOutputObserver(observeEncoderOutput);encoder.setGpuRetryObserver(observeGpuRetry);}catch(...){ }
        // gpu-capture-v1 (gpu_capture.hpp): with NVENC on the capture's own device
        // a tick's picture stays on the GPU whenever nothing reads its pixels.
        // Armed again after every capture/encoder rebuild. Fixed record:
        // hostAction 200 stage 243 (the arm result, and why a GPU path ended).
        gpucapture::GpuPathSession gpuPath([this](uint32_t record){if(captureDiagnostic_)captureDiagnostic_(gpucapture::gpuPathStage,record);});
        const auto armGpu=[&]{gpuPath.armed(capture.armGpuOutput(encoder.textureDevice(),hardwareEncoderBackend(encoder.backend()),hardwareEncoderBackend(encoder.backend())));};
        armGpu();
        // stream_gpu_priority.hpp: the worker's GPU scheduling while it streams
        // (a hardware capture only; every capture rebuild raises its device again).
        // Fixed record: hostAction 200 stage 242 (what was applied).
        std::optional<StreamGpuPriority> gpuPriority;bool gpuThread=false;
        const auto raiseCaptureGpu=[&]{if(gpuPriority&&gpuPriority->attempted())gpuThread=capture.raiseGpuThreadPriority();};
        if(local_){gpuPriority.emplace(capture.adapterLuid());raiseCaptureGpu();try{if(captureDiagnostic_&&gpuPriority->attempted())captureDiagnostic_(42,gpuPriority->diagnostic(gpuThread));}catch(...){ }}
        // cursor-v1: no cursor in the video; its records follow the first IDR.
        // Shared with the cursor-echo-v1 poller below.
        std::optional<cursor::SharedChannel> cursorChannel;
        // worker.pointer.absent (pointer_device.hpp): with no mouse on this PC Windows hides the pointer, in
        // the video and as a record alike; the service then plugs in the Host's own mouse (once per epoch).
        cursor::PointerAsk pointerAsk;
        if (cursorSeparated) { capture.composeCursor(false); cursorChannel.emplace(source, cursor::Channel::Allocate([this, session] { return cursorIds_.next(session); })); }
        // hostAction 200 stage 248 when this epoch ends: why the pointer was shown or hidden (cursor_channel.hpp
        // CursorDiagnosis, counts only). Destroyed before the channel it reads.
        std::function<void(uint32_t,uint32_t)> cursorRecordSink=[this](uint32_t stage,uint32_t value){if(this->captureDiagnostic_)this->captureDiagnostic_(stage,value);};
        struct CursorEpochRecord {
          std::optional<cursor::SharedChannel>& channel; std::function<void(uint32_t,uint32_t)> record;
          ~CursorEpochRecord(){ if(!channel||!record)return; try{ for(const auto value:channel->diagnosis().records()) record(cursor::cursorRecordStage,value); }catch(...){ } }
        };
        CursorEpochRecord cursorEpochRecord{cursorChannel,std::move(cursorRecordSink)};
        capture.observeReadback(observability_.enabled());ReadbackObservation readback;
        // cursor-echo-v1: while the client holds the mouse, the VM's real cursor
        // position as it changes (its own thread; ends with this capture).
        std::optional<cursor::EchoPoller> cursorEcho;
        if (cursorChannel && cursorEchoAllowed)
          cursorEcho.emplace(cursorEchoRequest_, cursorEchoWake_.handle, cursor::readObservation, [this, &cursorChannel, beginning](const cursor::Observation& seen, bool fresh) {
            const auto pts = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - beginning).count());
            cursorChannel->echo(seen, fresh, hostMonotonicUs(), [&](const std::vector<uint8_t>& record) { enqueue({false, mediaPacket(cursor::mediaKind, pts, record)}); });
          });
        stageBegan=std::chrono::steady_clock::now();
        captureStage=5;const bool audioAvailable = audioRequested&&ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready && ready.get();
        stages.audioMs=resizeStageMs(stageBegan,std::chrono::steady_clock::now());
        // stream-size-v1 pictures also state their scale of the source and why.
        // Only the first one of a resize restart carries its stages.
        const auto announce = [&] { std::ostringstream info; info << "{\"type\":\"stream.info\",\"codec\":\"h264\",\"width\":" << width << ",\"height\":" << height << ",\"sourceWidth\":" << sourceWidth << ",\"sourceHeight\":" << sourceHeight << ",\"captureBackend\":"<<jsonQuote(capture.backend())<<",\"fpsTarget\":" << fps << ",\"bitrate\":" << applied << (rate.adaptive ? ",\"bitrateMode\":\"auto\",\"bitrateMax\":" + std::to_string(rate.maximum) : std::string()) << ",\"audio\":" << streamAudioInfoJson(audioAvailable, audioOpus) << (scaler ? ",\"encodeScale\":" + std::to_string(scaler->scalePermille()) + ",\"encodeLimit\":\"" + encodeLimitName(scaler->reason()) + '"' : std::string()) <<epochFields;
          if(resizeStages&&!announced){stages.startMs=resizeStageMs(startBegan,std::chrono::steady_clock::now());info<<",\"resizeStages\":"<<resizeStagesJson(stages);}
          info<<'}'; text(info.str()); };
        announce(); announced = true; audioGo = audioAvailable;
        uint64_t captured = 0, encoded = 0, bytes = 0; auto measured = Clock::now(), next = measured;
        StreamMetricsWindow metrics; EncodeHostWindow scaleWindow;
        std::optional<FrameEncodePolicy> policySlot(std::in_place, size_t(width)*height*3/2,FrameRefinement::forCbr(applied,fps)); FrameEncodePolicy& encodePolicy=*policySlot;
        // A refinement output (the unchanged picture re-sent for detail) does
        // not reduce the share: the tiles make that picture exact anyway, and
        // refinement of photo-like content kept the share near zero for up to
        // 1.5 s, so the screen took 1.9 s instead of 1.0 s to become lossless
        // (encoder_benchmark "photo" 1264x760).
        const auto shareTiles = [&](size_t frameBytes) {
          const auto now = Clock::now(); recentVideo.push_back({now, frameBytes}); recentVideoBytes += frameBytes;
          while (now - recentVideo.front().first > std::chrono::milliseconds(500)) { recentVideoBytes -= recentVideo.front().second; recentVideo.pop_front(); }
          const size_t used = recentVideoBytes * 2, target = size_t(applied) / 8;  // bytes per second over 0.5 s
          tileShare = target > used ? target - used : 0;
        };
        bool answerLoss = false;  // the next video answers a udp-media-v1 LOSS (marked for the service's UDP leg)
        // stream.stats "stall" (stall forensics, fixed numbers per ~1 s window): the longest gap between two encoder outputs and between two captured frames, the wait/capture/convert/encode (ms) of the tick whose output ended that encode gap, IDRs out, key frame requests taken, bitrate changes applied and the lowest target (kbps).
        // changeLate: the longest time from the start of the tick before a changed picture to its encode (its pts), the
        // part of a client's display gap the Host owes while the picture changed (about one period normally; an idle
        // screen's 1 s refreshes never count). The client's own gap past the frames' pts spacing is the rest.
        struct { Clock::time_point lastOutput{},lastFresh{},waitBegan{},lastTick{}; double encodeGap=0,captureGap=0,wait=0,capture=0,convert=0,encode=0,tickWait=0,changeLate=0; uint32_t keyFrames=0,keyRequests=0,steps=0; int targetMinKbps=0; bool gapTick=false; } stall;
        const auto emit = [&](Frame frame) { const auto outputUs=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-beginning).count());if(!encodePolicy.output(frame.ptsUs,frame.key,outputUs,frame.bytes.size()))throw std::runtime_error("Encoder output timestamp did not match a submitted frame");encoded++; bytes += frame.bytes.size(); if(rate.adaptive)shareTiles(encodePolicy.lastOutputRefinement()?0:frame.bytes.size());
          {const auto at=Clock::now();if(stall.lastOutput!=Clock::time_point{}){const double gap=std::chrono::duration<double,std::milli>(at-stall.lastOutput).count();if(gap>stall.encodeGap){stall.encodeGap=gap;stall.gapTick=true;}}stall.lastOutput=at;if(frame.key)++stall.keyFrames;}
          // input-latency-v1: the tag precedes exactly the video packet it names.
          // A worker's tag also carries the encoder output time for its service.
          if(const auto tag=inputLatency_.output(frame.ptsUs,mediaEpoch_.load(),bool(local_)))text(*tag);
          const bool key = frame.key;
          enqueue({false, mediaPacket(1, frame.ptsUs, frame.bytes, frame.key, std::exchange(answerLoss, false))});
          if (key && cursorChannel) cursorChannel->keyFrame(); };
        // Lossless tiles start only after this session's client opted in.
        // losslessRetired: the client was told to drop every tile (losslessStreamTick).
        std::unique_ptr<LosslessTileSource> lossless;std::unique_ptr<ParallelWork> losslessHelper;CapturedComposedFrame<Capture> composed(capture);bool losslessRetired=false;
        uint64_t losslessBytes=0,losslessPackets=0;double losslessMs=0;
        // A software encoder's overload may pace capture below the requested rate (EncodeScaleController::fps).
        auto period = std::chrono::microseconds(1000000 / fps); FramePacer pacer;
        report(audioAvailable ? L"원격 세션 연결됨 · CPU H.264 · 시스템 소리" : L"원격 세션 연결됨 · CPU H.264 · 오디오 장치 없음");
        // The owned selection and the device's geometry, re-checked every
        // 100 ms off this thread (it used to run on every tick; see
        // CaptureTopologyWatch). One synchronous check precedes the first tick.
        const auto topologyCheck = [&device](CaptureSelection expectedSelection, RECT expected) {
          return [&device, expectedSelection = std::move(expectedSelection), expected] {
            validateCaptureSelection(expectedSelection,currentCaptureMonitors());
            if(!device.empty()){DEVMODEW mode{};mode.dmSize=sizeof(mode);if(!EnumDisplaySettingsExW(device.c_str(),ENUM_CURRENT_SETTINGS,&mode,0)||mode.dmPosition.x!=expected.left||mode.dmPosition.y!=expected.top||int(mode.dmPelsWidth)!=expected.right-expected.left||int(mode.dmPelsHeight)!=expected.bottom-expected.top)throw std::runtime_error("Virtual display geometry changed; reconnect the session");}
          };
        };
        std::optional<CaptureTopologyWatch> topology;
        // capture_follow.hpp: an application changed the owned output's mode (a
        // game's exclusive fullscreen at another resolution, or back). Wait until
        // the output settles, rebuild the capture on its new rectangle (the
        // encoder, picture and announced source stay) and move input, cursor and
        // caret onto it. false: the stream is stopping. Throws `reason`, as the
        // stream failed before, when the output does not settle as the same
        // owned single output within captureFollowSettle. A topology check
        // that failed while the output kept its rectangle (Windows briefly
        // unreadable) only re-arms the check; a capture that saw another frame
        // size (`captureChanged`) is always rebuilt.
        const bool followable=selection.monitors.size()==1&&!device.empty();
        Clock::time_point followedAt{},followRecordedAt{};bool tilesAllowed=true;
        const auto follow=[&](const std::string& reason,bool captureChanged)->bool{
          if(!followable)throw std::runtime_error(reason);
          topology.reset();
          const bool wgc=std::string_view(capture.backend())!="gdi";
          // gpu-capture-v1: the rebuilt capture keeps this device (an encoder may take its textures).
          const auto keepDevice=captureSlot?captureSlot->sharedDevice():std::shared_ptr<ID3D11Device>{};
          const auto deadline=Clock::now()+captureFollowSettle;
          for(;;){
            const auto settled=settledOwnedOutput([&]()->std::optional<RECT>{try{return followedOwnedOutput(selection.monitors.front(),currentCaptureMonitors(),currentDisplayModeRect(device));}catch(...){return std::nullopt;}},
              [](std::chrono::milliseconds step){std::this_thread::sleep_for(step);},[]{return Clock::now();},deadline,streamStop_);
            if(streamStop_||stopped_)return false;
            if(!settled)throw std::runtime_error(reason);
            if(!captureChanged&&captureSlot&&sameRect(*settled,current)){topology.emplace(topologyCheck(selection,current),std::chrono::milliseconds(100));return true;}
            // At most one rebuild per captureFollowGap (a game may flip modes); the output is read again after it.
            if(followedAt!=Clock::time_point{}&&Clock::now()<followedAt+captureFollowGap){std::this_thread::sleep_until(std::min(followedAt+captureFollowGap,deadline));continue;}
            bool rebuilt=false;
            try{
              captureSlot.reset();captureSlot.emplace(width,height,*settled,keepDevice);
              if(cursorSeparated)capture.composeCursor(false);
              capture.observeReadback(observability_.enabled());raiseCaptureGpu();
              // WGC created while Windows still changes the mode can fall back to GDI.
              if(wgc&&std::string_view(capture.backend())=="gdi"&&Clock::now()+captureFollowStep<deadline)throw std::runtime_error("capture_backend_changed");
              (void)capture.frame();  // the new mode's first frame; throws while it still changes
              rebuilt=true;
            }catch(const std::exception&){if(Clock::now()+captureFollowStep>=deadline)throw std::runtime_error(reason);}
            if(!rebuilt){std::this_thread::sleep_for(captureFollowStep);continue;}
            current=*settled;selection.bounds=current;selection.monitors.front().bounds=current;
            armGpu();
            input_.area(current);cursorArea(current);systemInputArea(session,current);
            if(cursorChannel)cursorChannel->rebase(current,sourceWidth,sourceHeight);
            if(caretGranted_)caret_.configure(session,current);
            // Tiles describe the announced source at its own pixels; none after a
            // follow. The client's decoder keeps running, so the next tick drops
            // every tile it still shows (losslessStreamTick) with an IDR of the new capture.
            tilesAllowed=false;keyFrameRequested_=true;
            topology.emplace(topologyCheck(selection,current),std::chrono::milliseconds(100));
            followedAt=Clock::now();
            // Fixed record (hostAction 200 stage 241, error = new width<<16|height), at most every 10 s.
            if(captureDiagnostic_&&(followRecordedAt==Clock::time_point{}||followedAt-followRecordedAt>=std::chrono::seconds(10))){followRecordedAt=followedAt;try{captureDiagnostic_(41,(uint32_t(std::min<LONG>(current.right-current.left,0xffff))<<16)|uint32_t(std::min<LONG>(current.bottom-current.top,0xffff)));}catch(...){ }}
            return true;
          }
        };
        try{
        failureReason="capture_failed";captureStage=6;
        // false: stopping, and the loop below does not start.
        try{topologyCheck(selection,current)();topology.emplace(topologyCheck(selection,current),std::chrono::milliseconds(100));}
        catch(const std::exception& error){(void)follow(error.what(),false);}
        // Start of the last tick that captured a new frame or submitted one.
        Clock::time_point lastWork{};
        BackgroundPace backgroundPace;
        // secure_desktop_notice.hpp: a UAC prompt of this unlocked session. The notice (NV12 at the encoded size,
        // made on first use and after a rescale) stands in for the capture; no statistics while it shows.
        SecureDesktopNotice secureDesktop;std::vector<uint8_t> noticeNv12;
        constexpr auto noticePeriod=std::chrono::milliseconds(200);
        const auto secureRecord=[&](uint32_t error){if(captureDiagnostic_)try{captureDiagnostic_(secureDesktopNoticeStage,error);}catch(...){ }};
        // One tick of a picture that stands in for the capture (the notice, or the helper's prompt; NV12 at the
        // encoded size), encoded like an idle screen (the policy's refreshes, a key frame on request or loss); the
        // first one drops every tile of the source with its video.
        const auto stillTick=[&](const std::vector<uint8_t>& nv12){
          const auto pts=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-beginning).count();failureReason="encoder_failed";captureStage=8;
          const bool keyFrame=keyFrameRequested_.exchange(false);if(keyFrame){encoder.requestKeyFrame();++stall.keyRequests;}
          if(const uint64_t lost=recoverRequested_.exchange(0)){answerLoss=true;if(!keyFrame&&!encoder.recover(lost-1))encoder.requestKeyFrame();}
          const bool force=keyFrame||answerLoss;
          const EncoderPicture picture{nv12.data(),nullptr,{}};
          std::vector<Frame> outputs;std::optional<LosslessTick> tiled;
          const auto collect=[&]{encoder.encode(picture,uint64_t(pts),[&](Frame output){outputs.push_back(std::move(output));});};
          if(lossless)tiled=losslessRetireTick(*lossless,losslessRetired,[&](bool forced){return encodePolicy.prepare(nv12,uint64_t(pts),forced);},uint64_t(pts),collect,force);
          if(tiled){if(tiled->payload)enqueue({false,mediaPacket(1,uint64_t(pts)-1,*tiled->payload,false)});for(auto& output:outputs)emit(std::move(output));}
          else{const auto decision=encodePolicy.prepare(nv12,uint64_t(pts),force);if(decision.submit)encoder.encode(picture,uint64_t(pts),emit);}
        };
        const auto noticeTick=[&]{
          const size_t noticeBytes=size_t(width)*size_t(height)*3/2;
          if(noticeNv12.size()!=noticeBytes){const auto bgra=secureDesktopNoticePicture(width,height);noticeNv12.assign(noticeBytes,0);bgraToNv12(bgra.data(),noticeNv12.data(),width,height);}
          stillTick(noticeNv12);
        };
        // Role::secure (secure_desktop_switch.hpp): while the secure desktop shows, the service's SYSTEM helper
        // captures the real prompt on the Winlogon desktop and this worker feeds its frames to the same encoder,
        // same session and epoch. Only the service's own user worker with one owned output asks for it; the service
        // routes the client's input to the helper itself. Every helper failure is the notice above, never the end
        // of the stream.
        secure::WorkerSwitch secureSwitch(secureSeq_.load());std::shared_ptr<secure::worker::Mapping> secureMapping;
        std::vector<uint8_t> helperScaled,helperNv12;bool secureOff=false;
        const uint32_t workerLevel=secure::worker::processLevel(GetCurrentProcess());Clock::time_point reachRecordedAt{};bool reachBlocked=false;
        const bool secureCapable=bool(local_)&&!bridge_&&followable&&!session.empty();
        const auto secureSend=[&](const std::string& packet){if(secureCapable)try{text(packet);}catch(...){ }};
        // The desktop is back, or this capture loop ends: no helper is wanted any more.
        // hostAction 200 stage 246 (secure_desktop_scope.hpp): what the helper that just ended counted of its
        // input. Counts only. "received" is written even when it is 0: no input reached the helper.
        const auto secureInputRecords=[&]{
          const auto counts=secureSwitch.takeInputCounts();if(!counts||!captureDiagnostic_)return;
          const auto put=[&](uint32_t kind,uint32_t value,bool always=false){if(value||always)try{captureDiagnostic_(secure::inputRecordStage,secure::input246(kind,value));}catch(...){ }};
          put(secure::input246Received,counts->received,true);put(secure::input246Injected,counts->injected);put(secure::input246Dropped,counts->dropped);
          put(secure::input246Refused,counts->refused);put(secure::input246SendFailed,counts->sendFailed);if(counts->sendFailed)put(secure::input246LastError,counts->lastError);
        };
        const auto secureTeardown=[&]{secureInputRecords();secureOff=false;secureMapping.reset();(void)secureTakeGrant();secureDenied_=0;helperNv12.clear();secureSeq_.store(secureSwitch.seq());if(secureSwitch.reset())secureSend(secure::worker::controlJson(secure::controlStop,session));};
        struct SecureGuard{std::function<void()> f;~SecureGuard(){try{f();}catch(...){ }}} secureGuard{secureTeardown};
        // One tick while the secure desktop shows: the helper's frame when it streams, otherwise the notice.
        // Returns how long this tick may wait for the next one. Nothing the helper side does can end the stream:
        // an error there shows the notice for the rest of this secure desktop.
        const auto secureFill=[&](const Clock::time_point& now)->std::chrono::milliseconds{
          bool notice=true,hold=false;auto wait=noticePeriod;
          if(!secureOff)try{
            if(auto grant=secureTakeGrant()){const uint32_t seq=grant->seq;if(auto mapping=secure::worker::mapGrant(std::move(*grant))){
              if(secureSwitch.attach(std::shared_ptr<const uint8_t>(mapping,mapping->view),mapping->width,mapping->height,seq))secureMapping=std::move(mapping);}else secureSwitch.denied(seq);}
            if(const uint32_t denied=secureDenied_.exchange(0))secureSwitch.denied(denied);
            const auto tick=secureSwitch.tick(now,secureCapable);
            for(unsigned i=0;i<tick.recordCount;++i)secureRecord(tick.records[i]);
            if(tick.request){if(const auto owned=secure::worker::ownedOutputForRect(current))secureSend(secure::worker::requestJson(session,tick.seq,*owned));else secureSwitch.denied(tick.seq);}
            if(tick.activated)secureSend(secure::worker::controlJson(secure::controlActive,session));
            if(tick.stop){secureInputRecords();secureMapping.reset();secureSend(secure::worker::controlJson(secure::controlStop,session));}
            // The picture changes between the notice and the prompt: an IDR, so no client blends the two.
            if(tick.sourceChanged)keyFrameRequested_=true;
            // The helper was asked for and has not answered yet (at most 2 s): nothing is encoded, so the client
            // keeps the picture it has instead of flashing the notice before every prompt.
            if(tick.hold){hold=true;wait=std::chrono::milliseconds(16);}
            else if(!tick.notice){
              const size_t nvBytes=size_t(width)*size_t(height)*3/2;const auto& bgra=secureSwitch.frame();const uint32_t ow=secureSwitch.frameWidth(),oh=secureSwitch.frameHeight();
              if(bgra.size()!=size_t(ow)*oh*4)throw std::runtime_error("secure_frame_size");
              if(tick.fresh||helperNv12.size()!=nvBytes){
                const uint8_t* picture=bgra.data();
                if(ow!=uint32_t(width)||oh!=uint32_t(height)){helperScaled.resize(size_t(width)*size_t(height)*4);secure::scaleBgra(bgra.data(),ow,oh,helperScaled.data(),uint32_t(width),uint32_t(height));picture=helperScaled.data();}
                if(helperNv12.size()!=nvBytes)helperNv12.assign(nvBytes,0);
                bgraToNv12(picture,helperNv12.data(),width,height);
              }
              notice=false;wait=std::chrono::milliseconds(0);
            }
          }catch(...){
            const bool streamed=secureSwitch.phase()==secure::HelperPhase::active;
            notice=true;hold=false;wait=noticePeriod;secureOff=true;secureMapping.reset();helperNv12.clear();keyFrameRequested_=true;
            try{if(secureSwitch.reset())secureSend(secure::worker::controlJson(secure::controlStop,session));secureRecord(streamed?secure::record244HelperLost:secure::record244HelperLaunchFail);}catch(...){ }
          }
          if(hold)return wait;
          if(notice)noticeTick();else{
            secureDesktop.answerable(now);stillTick(helperNv12);
            // cursor-v1: the client draws the pointer itself and would draw it a second time over the helper's
            // frame, which has the secure desktop's pointer composed in. It is told hidden while the prompt shows;
            // the capture's first tick after the return reports the real cursor again.
            if(cursorChannel)try{const auto at=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-beginning).count());
              cursorChannel->hide(hostMonotonicUs(),[&](const std::vector<uint8_t>& record){enqueue({false,mediaPacket(cursor::mediaKind,at,record)});});}catch(...){ }
          }
          // While the helper streams, the next tick starts with its next frame (or after 16 ms on a still prompt).
          if(!notice){if(secureMapping&&secureMapping->event.value)WaitForSingleObject(secureMapping->event.value,16);else wait=std::chrono::milliseconds(16);}
          return wait;
        };
        // The desktop is back: a new capture on the same output and device (WGC may have lost its item while
        // Winlogon showed), armed again, its first video an IDR. false: it could not be made yet.
        const auto resumeCapture=[&]()->bool{
          const auto keep=captureSlot?captureSlot->sharedDevice():std::shared_ptr<ID3D11Device>{};
          failureReason="capture_failed";captureStage=3;
          try{captureSlot.reset();captureSlot.emplace(width,height,current,keep);}
          catch(const std::exception&){if(!interactiveDesktopAvailable()&&ownSessionUnlocked())return false;throw;}
          if(cursorSeparated)capture.composeCursor(false);
          capture.observeReadback(observability_.enabled());raiseCaptureGpu();armGpu();
          topology.emplace(topologyCheck(selection,current),std::chrono::milliseconds(100));
          keyFrameRequested_=true;captured=encoded=bytes=0;metrics.reset();measured=Clock::now();scaleWindow={};
          return true;
        };
        while (!streamStop_ && !stopped_) {
          const auto iteration = Clock::now();
          stall.tickWait=stall.waitBegan==Clock::time_point{}?0:std::chrono::duration<double,std::milli>(iteration-stall.waitBegan).count();
          const auto priorTick=std::exchange(stall.lastTick,iteration);
          failureReason="capture_failed";captureStage=6;
          if(secureDesktop.active()||!captureSlot){
            const auto step=secureDesktop.step(interactiveDesktopAvailable(),ownSessionUnlocked(),iteration);
            if(step==SecureDesktopStep::end){secureRecord(secureDesktop.endedRecord());throw std::runtime_error(secureDesktopUnavailable);}
            if(step==SecureDesktopStep::resume){secureRecord(secureDesktop.resumedRecord());secureTeardown();}
            // A capture that could not be made yet (the desktop flipped again) leaves the notice showing.
            if(step!=SecureDesktopStep::notice&&!resumeCapture())(void)secureDesktop.step(false,true,iteration);
            if(secureDesktop.active()){if(const auto wait=secureFill(iteration);wait.count()>0)std::this_thread::sleep_for(wait);continue;}
          }
          try{topology->verify();}catch(const std::exception& error){if(!follow(error.what(),false))break;continue;}
          // background-video-v1 (background_video.hpp): while the client's view is hidden, one tick a second
          // captures and encodes, at an idle screen's pace; the others only wait, so a return is seen at once.
          if(!backgroundPace.due(streamBackground_.load(),Clock::now())){std::this_thread::sleep_for(BackgroundPace::idleWait);continue;}
          const auto captureBegan = Clock::now();
          // The client wants tiles now (opted in, not paused by 게임 마우스) and the
          // capture still describes the announced source (no follow since).
          // Decided before frame(): tiles read the CPU picture (gpu-capture-v1).
          const bool tilesWanted=tilesAllowed&&losslessRequested(session);
          capture.gpuOutput(gpuPath.tick(tilesWanted,encoder.textureDevice()&&encoder.textureDevice()==capture.gpuDevice()));
          // Injections reported before this point precede this frame's capture.
          inputLatency_.beginCapture(hostMonotonicUs());
          captureStage=7;const std::vector<uint8_t>* read=nullptr;
          // A frame of another size, or a WGC error (its item may close while the mode changes): rebuild on the output as it is now.
          try{read=&capture.frame();}
          catch(const CaptureGeometryChanged& error){if(!follow(error.what(),true))break;continue;}
          catch(const std::runtime_error& error){
            // A secure desktop of this unlocked session (a UAC prompt; a WGC error of the same switch counts):
            // the notice instead of the end of the stream (secure_desktop_notice.hpp).
            if((std::string_view(error.what())==secureDesktopUnavailable||!interactiveDesktopAvailable())&&ownSessionUnlocked()&&secureDesktop.step(false,true,Clock::now())==SecureDesktopStep::notice){
              if(const auto wait=secureFill(Clock::now());wait.count()>0)std::this_thread::sleep_for(wait);continue;}
            if(!followable||std::string_view(error.what()).rfind("Windows capture failed (0x",0)!=0)throw;if(!follow(error.what(),true))break;continue;}
          if(const auto gpuFailure=capture.takeGpuFailure())gpuPath.failed(*gpuFailure);
          // On a GPU tick the picture is the capture's texture and `frame` is stale.
          const auto* gpuPicture=capture.gpuPicture();const EncoderPicture encoderInput=capture.encoderPicture();
          const auto& frame=*read; if(capture.frameUpdated()){captured++;if(stall.lastFresh!=Clock::time_point{})stall.captureGap=std::max(stall.captureGap,std::chrono::duration<double,std::milli>(iteration-stall.lastFresh).count());stall.lastFresh=iteration;}
          const auto encodeBegan = Clock::now();
          const auto pts = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - beginning).count();failureReason="encoder_failed";captureStage=8;
          // Auto: a live CBR target change on this encoder (no IDR); refinement
          // budget and the tile share follow it.
          if(rate.adaptive){
            if(const int wanted=requestedBitrate_.exchange(0)){
              const int value=std::clamp(wanted,floorBitrate,rate.maximum);
              if(value!=applied&&encoder.setBitrate(value)){applied=value;autoBitrate_=value;encodePolicy.retarget(FrameRefinement::forCbr(value,fps));++stall.steps;stall.targetMinKbps=stall.targetMinKbps?std::min(stall.targetMinKbps,value/1000):value/1000;}
            }
            shareTiles(0);  // a still screen frees the share again
          }
          // host-resume-v1: records wait for the IDR asked for with it, then leave again.
          if(cursorResend_.exchange(false)&&cursorChannel)cursorChannel->pictureRestart();
          // The broker channel's send queue dropped video (stream.keyframe):
          // this tick submits a frame and the encoder codes it as an IDR.
          const bool keyFrame=keyFrameRequested_.exchange(false);if(keyFrame){encoder.requestKeyFrame();++stall.keyRequests;}
          // udp-media-v1 LOSS: frames after the client's last good one never reached its
          // decoder. This tick's frame references only that one or older (reference
          // invalidation) or is an IDR, and is marked as the answer.
          if(const uint64_t lost=recoverRequested_.exchange(0)){answerLoss=true;if(!keyFrame&&!encoder.recover(lost-1))encoder.requestKeyFrame();}
          const bool force=keyFrame||answerLoss;
          // Tiles always describe the unscaled source (also when the encoded
          // video is downscaled); a capture without that view (scaled GDI) or a
          // source beyond the tile limits keeps plain video.
          if(!lossless&&tilesWanted&&capture.composedWidth()==sourceWidth&&capture.composedHeight()==sourceHeight&&multitap::lossless::validFrame(uint32_t(sourceWidth),uint32_t(sourceHeight))){
            auto settings=LosslessSettings::forBitrate(applied);
            // Auto: tiles and video share the adaptive target (read every tick).
            if(rate.adaptive){settings=LosslessSettings::forTargetRate([&tileShare]{return tileShare.load();});settings.minimumRateBytesPerSecond=62500;}
            // The encoder's picture (width x height) anchors the tiles (lossless_tile_source.hpp).
            lossless=std::make_unique<LosslessTileSource>(uint32_t(sourceWidth),uint32_t(sourceHeight),settings,uint32_t(width),uint32_t(height));losslessHelper=std::make_unique<ParallelWork>();losslessRetired=false;}
          bool submitted=false,changedTick=false;
          // Tile packet (pts T-1) strictly before this tick's video (pts T). No
          // longer wanted: one packet drops every tile with this tick's video.
          std::vector<Frame> outputs;std::optional<LosslessTick> tiled;
          const auto collect=[&]{encoder.encode(encoderInput,uint64_t(pts),[&](Frame output){outputs.push_back(std::move(output));});};
          // A GPU tick never has tiles wanted; it can only retire them.
          if(lossless&&!gpuPicture)tiled=losslessStreamTick(*lossless,losslessRetired,tilesWanted,*losslessHelper,composed,encodePolicy,frame,uint64_t(pts),collect,force);
          else if(lossless)tiled=losslessRetireTick(*lossless,losslessRetired,[&](bool forced){return encodePolicy.prepareChanged(capture.gpuPictureChanged(),uint64_t(pts),forced);},uint64_t(pts),collect,force);
          if(tiled){
            losslessMs+=tiled->tileMs;submitted=tiled->decision.submit;changedTick=tiled->decision.contentChanged;
            inputLatency_.submitted(uint64_t(pts),tiled->decision.contentChanged);
            if(tiled->payload){losslessBytes+=tiled->payload->size();++losslessPackets;enqueue({false,mediaPacket(1,uint64_t(pts)-1,*tiled->payload,false)});}
            for(auto& output:outputs)emit(std::move(output));
          }else{const auto decision=gpuPicture?encodePolicy.prepareChanged(capture.gpuPictureChanged(),uint64_t(pts),force):encodePolicy.prepare(frame,uint64_t(pts),force);submitted=decision.submit;changedTick=decision.contentChanged;inputLatency_.submitted(uint64_t(pts),decision.contentChanged);if(decision.submit)encoder.encode(encoderInput, uint64_t(pts), emit);}
          if(submitted||capture.frameUpdated())lastWork=iteration;
          if(changedTick&&priorTick!=Clock::time_point{})stall.changeLate=std::max(stall.changeLate,std::chrono::duration<double,std::milli>(encodeBegan-priorTick).count());
          // cursor-v1: the cursor this frame() read, after this tick's video.
          // An unrepresentable cursor is composed from the next frame on.
          if (cursorChannel) {
            const auto& seen = capture.cursorSample();
            cursorChannel->composeRequested(cursorCompose_.wanted());
            cursorChannel->observe({seen.known, seen.flags, seen.handle, seen.position}, cursorInput_.read(), hostMonotonicUs(),
                                   [&](const std::vector<uint8_t>& record) { enqueue({false, mediaPacket(cursor::mediaKind, uint64_t(pts), record)}); });
            capture.composeCursor(cursorChannel->composeFallback());
          }
          if (local_ && pointerAsk.due(hostMonotonicUs())) text("{\"type\":\"worker.pointer.absent\",\"sessionId\":" + jsonQuote(session) + '}');
          const auto now = Clock::now(); const double seconds = std::chrono::duration<double>(now - measured).count();
          const double encodeMs = std::chrono::duration<double,std::milli>(now-encodeBegan).count();
          if(observability_.enabled())readback.observe(capture.lastReadbackMs());
          metrics.observe({std::chrono::duration<double,std::milli>(captureBegan-iteration).count(),capture.lastCaptureMs(),capture.lastConvertMs(),encodeMs});
          if(stall.gapTick){stall.gapTick=false;stall.wait=stall.tickWait;stall.capture=capture.lastCaptureMs();stall.convert=capture.lastConvertMs();stall.encode=encodeMs;}
          // stream-size-v1: this tick's work; capture is source-sized, convert +
          // encode follow the picture's pixels.
          if (scaler) { scaleWindow.busyMs += capture.lastCaptureMs() + capture.lastConvertMs() + encodeMs; if (submitted) { ++scaleWindow.frames; scaleWindow.fixedMs += capture.lastCaptureMs(); scaleWindow.scalableMs += capture.lastConvertMs() + encodeMs; } }
          if (seconds >= 1.0) {
            // hostAction 200 stage 246 kind 10: this worker injects the stream's input itself (no system injector)
            // and the window in front belongs to a process above it, so Windows drops that input without an
            // error (UIPI): a UAC prompt on the user's desktop, an elevated window of a standard user. Levels
            // only, once when it comes to the front and then at most every 10 s.
            if(local_&&!bridge_&&captureDiagnostic_&&!systemInputSeen_.load()){
              const uint32_t front=secure::worker::foregroundLevel();const bool blocked=secure::inputUnreachable(workerLevel,front);
              if(blocked&&(!reachBlocked||iteration-reachRecordedAt>=std::chrono::seconds(10))){reachRecordedAt=iteration;try{captureDiagnostic_(secure::inputRecordStage,secure::input246Reach(workerLevel,front));}catch(...){ }}
              reachBlocked=blocked;
            }
            std::ostringstream stats;
            stats << std::fixed << std::setprecision(2) << "{\"type\":\"stream.stats\",\"capturedFps\":" << double(captured) / seconds << ",\"encodedFps\":" << double(encoded) / seconds << ",\"bitrateKbps\":" << double(bytes) * 8 / seconds / 1000 << ",\"measuredMs\":" << seconds*1000 << epochFields;
            // The encoder's current CBR target (bitrateKbps above is its measured output).
            stats << ",\"bitrateTarget\":" << applied << ",\"bitrateMode\":" << (rate.adaptive ? "\"auto\",\"bitrateMax\":" + std::to_string(rate.maximum) : std::string("\"fixed\""));
            // Active H.264 backend ("x264" | "mft"; it changes on an x264 fallback). Relay/client pass or ignore it.
            stats << ",\"encoder\":\"" << encoder.backendName() << '"';
            stats << ",\"encoderInfo\":" << encoderDiagnosticJson(encoderDiagnostic(encoder.backendName(),encoder.hardwareInfo(),encoder.fallbackCode()));
            metrics.writeJson(stats);
            // audio-stats-v1: this window's capture -> handed-on time per audio packet.
            stats << audioCapture_.takeJson();
            // input-latency-v1 sessions: host share of recently tagged inputs
            // (injection -> capture tick, capture tick -> encoder output).
            stats << inputLatency_.frameTimingJson(hostMonotonicUs());
            // Only with tiles active (an opted-in client): observational, outside bitrateKbps.
            // mismatches: tiles whose capture BGRA was not the encoder's picture since the source began (0 when consistent).
            if(lossless&&!losslessRetired)stats<<",\"lossless\":{\"protocol\":\"tiles-v1\",\"presentTiles\":"<<lossless->presentTiles()<<",\"packets\":"<<losslessPackets<<",\"kbps\":"<<double(losslessBytes)*8/seconds/1000<<",\"tileMs\":"<<losslessMs<<",\"mismatches\":"<<lossless->stats().mismatchTiles<<'}';
            // Only in a separated session: records sent and ticks composed as
            // fallback; with cursor-echo-v1 also the echoed positions.
            if(cursorChannel){const auto counted=cursorChannel->takeCounters();stats<<",\"cursor\":{\"protocol\":\"cursor-v1\",\"shapes\":"<<counted.shapes<<",\"states\":"<<counted.states<<",\"warps\":"<<counted.warps<<",\"composed\":"<<counted.composed;if(cursorEcho)stats<<",\"echoes\":"<<counted.echoes;stats<<'}';}
            // stream-size-v1: the picture's scale of the source (per mille) and why.
            if(scaler)stats<<",\"encodeScale\":"<<scaler->scalePermille()<<",\"encodeLimit\":\""<<encodeLimitName(scaler->reason())<<'"';
            // The rate capture paces at now (below fpsTarget of stream.info only for a software encoder's overload).
            stats<<",\"fpsTarget\":"<<1000000/std::max<int64_t>(1,period.count());
            {const auto ms=[](double value){return int64_t(std::lround(std::clamp(value,0.0,600000.0)));};
              stats<<",\"stall\":{\"encodeGapMs\":"<<ms(stall.encodeGap)<<",\"captureGapMs\":"<<ms(stall.captureGap)<<",\"waitMs\":"<<ms(stall.wait)<<",\"captureMs\":"<<ms(stall.capture)<<",\"convertMs\":"<<ms(stall.convert)<<",\"encodeMs\":"<<ms(stall.encode)
                <<",\"keyFrames\":"<<stall.keyFrames<<",\"keyRequests\":"<<stall.keyRequests<<",\"bitrateSteps\":"<<stall.steps<<",\"changeLateMs\":"<<ms(stall.changeLate);
              if(stall.targetMinKbps)stats<<",\"targetMinKbps\":"<<stall.targetMinKbps;
              stats<<'}';
              stall.encodeGap=stall.captureGap=stall.wait=stall.capture=stall.convert=stall.encode=stall.changeLate=0;stall.keyFrames=stall.keyRequests=stall.steps=0;stall.targetMinKbps=0;}
            stats << '}'; text(observedWorkerStats(stats.str(),readback.take()));
            captured = encoded = bytes = 0; losslessBytes = losslessPackets = 0; losslessMs = 0; metrics.reset(); measured = now;
            // stream-size-v1: a measured overload (or headroom) restarts the
            // encoder at another scale of the same source within this epoch: a
            // new stream.info, then an IDR. At most every few seconds.
            if(scaler){scaleWindow.seconds=seconds;auto candidate=*scaler;const bool rescale=candidate.observe(scaleWindow,clientDecodeReport(),now,!hardwareEncoderBackend(encoder.backend()));scaleWindow={};
              if(rescale){
                const auto picture=candidate.size();
                const StreamMemoryShape currentMemory{uint32_t(sourceWidth),uint32_t(sourceHeight),uint32_t(width),uint32_t(height),true},nextMemory{uint32_t(sourceWidth),uint32_t(sourceHeight),uint32_t(picture.first),uint32_t(picture.second),true};
                const auto growth=streamMemoryGrowthRequiresAdmission(currentMemory,nextMemory);bool memoryAdmitted=growth.has_value();
                if(memoryAdmitted&&*growth)try{validateStreamMemory({&nextMemory,1});}
                catch(const std::exception& error){const std::string_view reason(error.what());if(reason=="frame_memory_insufficient"||reason=="frame_memory_unavailable"||reason=="frame_layout_unsupported")memoryAdmitted=false;else throw;}
                // An unadmitted candidate changes neither controller, metadata,
                // workspace nor the current live Capture/Encoder owners.
                // A checked nonincreasing forecast can downscale under memory
                // pressure. Existing allocation checks still run after teardown;
                // no guessed reclaimed bytes are added to the system budget.
                if(memoryAdmitted){
                encoderSlot.reset();captureDevices_.keep(captureSlot->sharedDevice());captureSlot.reset();width=picture.first;height=picture.second;
                failureReason="capture_failed";captureStage=3;captureSlot.emplace(width,height,current,captureDevices_.take());if(cursorSeparated)capture.composeCursor(false);capture.observeReadback(observability_.enabled());raiseCaptureGpu();
                floorBitrate=std::min(adaptiveBitrateFloor(width,height,fps),rate.maximum);if(rate.adaptive){applied=std::clamp(applied,floorBitrate,rate.maximum);autoBitrate_=applied;}
                failureReason="encoder_failed";captureStage=4;encoderSlot.emplace(width,height,fps,applied,EncoderTuning{},EncoderBackend::automatic,capture.adapterLuid(),capture.gpuDevice());
                if(local_)try{encoder.setOutputObserver(observeEncoderOutput);encoder.setGpuRetryObserver(observeGpuRetry);}catch(...){ }
                armGpu();
                policySlot.emplace(size_t(width)*height*3/2,FrameRefinement::forCbr(applied,fps));
                // Tiles describe the source; the client's new decoder starts without them.
                lossless.reset();losslessHelper.reset();
                *scaler=std::move(candidate);rememberScale(session,scaler->memory());
                // cursor-v1: the client's new decoder knows no shape or state (cursor_channel.hpp).
                if(cursorChannel)cursorChannel->pictureRestart();
                announce();
                }
              }else *scaler=std::move(candidate);
              // A software encoder's rate step (no new encoder): the pacing follows it.
              if(const auto paced=std::chrono::microseconds(1000000/std::clamp(scaler->fps(),1,60));paced!=period){period=paced;rememberScale(session,scaler->memory());}}
          }
          // A newly composed WGC frame starts the next tick at once (and the
          // schedule continues one period after it) instead of waiting for the
          // fixed tick; never sooner than the frame-rate cap after the last
          // tick that captured or submitted a frame (idle ticks do not count).
          next += period; if (next < now) next = now;
          stall.waitBegan = Clock::now();
          const HANDLE frameEvent = capture.frameEvent();
          HANDLE wakeEvents[2]; DWORD wakeCount = 0; if (frameEvent) wakeEvents[wakeCount++] = frameEvent; if (inputWake_.handle) wakeEvents[wakeCount++] = inputWake_.handle;
          if (pacer.wait(next, std::span<const HANDLE>(wakeEvents, wakeCount), lastWork + captureMinimumGap(period))) next = Clock::now();
        }
        // The notice leaves before this scope's capture, encoder and threads are torn down.
        }catch(const std::exception& error){failed(error.what());}
      } catch (const std::exception& error) { failed(error.what()); }
      streamStop_ = true; audioGo = true; if (audio.joinable()) audio.join(); active_ = false;
    });
  }
  void control(const std::string& bytes,HINTERNET actualMain=nullptr) {
    const Json message = JsonParser::parse(bytes); const auto type = message.get("type").string();
    // Role::secure: the service's answer to this worker's helper request (secure_desktop_service.hpp). Only the
    // service's own user worker reads it, and only its service pipe carries its control messages; an Agent that
    // talks to the broker (the service's, or a standalone one) ignores these types whoever sends them.
    if(type==secure::controlReady||type==secure::controlFailed){if(local_&&!bridge_&&!actualMain)secureAnswer(type,message,!sessionId_.empty()&&message.get("sessionId").string()==sessionId_);return;}
    if(actualMain&&bridge_){const bool consumed=bridge_->mainControl(actualMain,message,GetTickCount64());if(consumed&&type!="host.capabilities.ack")return;}
    // A broker that is restarting ends each stream so first (relay.mjs close()).
    if(type=="session.stop"&&message.get("reason").string()=="server_stopping")brokerStoppingAt_=GetTickCount64();
    if(type=="host.session.stop.status"||(type=="session.stop"&&!std::holds_alternative<std::nullptr_t>(message.get("sessionStopProtocol").value))){
      const auto request=parseSessionStopRequest(message);if(!request||!bridge_)return;
      if(request->status){if(stopBinding_.query(*request)){text(sessionStopPacket(*request,bridge_->stoppedSession(request->session)));announceCapabilities();}return;}
      if(!active_||!stopBinding_.retire(*request,sessionId_))return;
      if(auto input=directInput_.load())input->pause();
      const auto evidence=bridge_->stopSession(request->session);
      stopStream(false); // Clear stale media first; the receipt survives cleanup.
      text(sessionStopPacket(*request,evidence));
      announceCapabilities();return;
    }
    if(type=="host.qemu-console.prepare"||type=="host.qemu-console.cancel"||type=="host.qemu-console.status"||type=="host.qemu-console.renew"){
      if(bridge_)bridge_->qemuConsoleControl(message,[this](bool isText,std::vector<uint8_t> data){enqueue({isText,std::move(data)});});
      if(type=="host.qemu-console.cancel"||type=="host.qemu-console.status"||type=="host.qemu-console.renew")announceCapabilities();
      return;
    }
    if(type=="host.ime.query"){
      if(bridge_)bridge_->imeQuery(message,[this](bool isText,std::vector<uint8_t> data){enqueue({isText,std::move(data)});});
      return;
    }
    if(type=="host.console.prepare"||type=="host.console.cancel"){
      if(bridge_)bridge_->consoleControl(message,[this](bool isText,std::vector<uint8_t> data){enqueue({isText,std::move(data)});});
      return;
    }
    if (type == "host.display.configure" || type == "host.display.cancel") {
      if (bridge_) {
        bridge_->displayControl(message, [this](bool isText, std::vector<uint8_t> data) { enqueue({isText, std::move(data)}); });
      }
      return;
    }
    if (type == "session.start") { stream(message); return; }
    if (type == "host.heartbeat.ack") {if(brokerStatus_)brokerStatus_->acknowledged(brokerAttempt_.load(),true);return;}
    if (type == "host.capabilities.ack") {signinNegotiated_=message.get("accepted").boolean()&&message.get("signinServiceProtocol").string()=="signin-service-v1";/* Whether streamed sign-in was negotiated (active); the record itself is information, not a failure (the broker does not offer it, so ok:false would mark every connection as failed). */recordHostEvent("hostAction",{{"action",291},{"ok",true},{"active",signinNegotiated_},{"changed",bridge_!=nullptr}});diagnosticSchedule_.Negotiate(message.get("accepted").boolean()&&message.get("diagnosticsProtocol").string()=="host-diagnostics-v1");closingNegotiated_=message.get("accepted").boolean()&&message.get("closingProtocol").string()==hostClosingProtocol;resourcesUptime_=message.get("accepted").boolean()&&message.get("resourcesProtocol").string()==hostResourcesProtocol;if(brokerStatus_){if(message.get("accepted").boolean())brokerStatus_->acknowledged(brokerAttempt_.load(),false);else closeTransport("protocol_rejected");}
      // host-resume-v1: this channel's first ACK offers the stream kept through
      // the last one (or none); a Broker without the protocol ends it.
      if(bridge_){bool stopHeld=false;if(const auto offer=resume_.acknowledged(message.get("accepted").boolean()&&message.get("hostResumeProtocol").string()==hostResumeProtocol,sessionId_,stopHeld))text(*offer);if(stopHeld)stopStream();}
      // host-route-v1: an idle channel that reached the Broker by DNS reconnects through the route.
      if(brokerRoute().offer(message.get("hostRoute").string())&&!routed_&&!active_&&!brokerRoute().current(GetTickCount64()).empty())closeTransport();
      return;}
    // host-resume-v1: the Broker took the kept stream back; it restarts at an IDR.
    if(type=="host.session.resumed"){if(resume_.resumed(message.get("sessionId").string(),sessionId_)&&bridge_){bridge_->cursorResend();bridge_->keyFrame();bridge_->clipboardResumed();}return;}
    // channel-resume-v1: whether the current session's client is there (only sent for a session that negotiated it).
    if(type=="session.channels"){if(bridge_&&!sessionId_.empty()&&message.get("sessionId").string()==sessionId_){const auto state=message.get("state").string();if(state=="away")bridge_->peerAway();else if(state=="back")bridge_->peerBack();}return;}
    if(type=="host.diagnostics.request"){diagnosticSchedule_.Request();return;}
    // Rows an accepted report carried count as delivered; rows still waiting ask for the next report after 5 s instead of 30 s.
    if(type=="host.diagnostics.ack"){if(bridge_&&bridge_->diagnosticsAcknowledged(message.get("accepted").boolean()))diagnosticSchedule_.Request();return;}
    if(type=="host.thumbnail.request"){thumbnailRequest(message);return;}
    if(type=="host.update.start"){if(updater_)updater_->start(message);return;}
    if(type=="host.update.hello.ack"||type=="host.update.status.ack"){if(updater_)updater_->acknowledged(message);return;}
    if(type=="host.power.request"){if(power_)power_->request(message);return;}
    if (type == "stream.resize" && bridge_) {
      if (!active_) return;
      if(auto input=directInput_.load())input->pause();
      if(auto leg=udpProbe_.load())leg->pauseInputs();
      { std::lock_guard lock(queueMutex_); queue_.dropMedia(); }
      bridge_->control(message);return;
    }
    if(bridge_&&active_&&(type=="host.signin.authority.query"||type=="stream.phase.decoded"||type=="input.phase")){if(message.get("sessionId").string()==sessionId_)bridge_->control(message);return;}
    if (!active_) return;
    const auto id = message.get("sessionId").string(); if (!id.empty() && id != sessionId_) return;
    if(type=="input.direct"){
      const auto sequence=inputLatencyInteger(message.get("sequence"),1,UINT32_MAX);const auto& value=message.get("input");const auto inputType=value.get("type").string();
      if(!bridge_||id!=sessionId_||message.get("protocol").string()!="udp-direct-v1"||!sequence||(inputType!="input"&&inputType!="input.reset"&&inputType!="input.fence"))return;
      const auto body=jsonObject(value);if(body.size()>8192)return;const DirectInputItem item{uint32_t(*sequence),body,hostMonotonicUs()};
      if(const auto input=directInput_.load())input->submit(true,item);else directInput(item);return;
    }
    if(type=="input"||type=="input.reset"||type=="cursor.compose"||type=="cursor.echo"||type=="caret.watch"||type=="display.scale"||type=="stream.resize"||type=="session.stop"||type=="session.ended")if(auto input=directInput_.load())input->pause();
    if(type=="stream.tracks.ready"){if(auto input=trackInput_.load())input->acknowledge(message);return;}
    if (type == "input.reset") { if (bridge_) bridge_->control(message); else { input_.reset(); systemButtons_.clear(); } }
    else if (type == "session.stop" || type == "session.ended") { caretEnd(); cursorEchoEnd(); backgroundEnd(); stopStream(); report(L"서버 연결됨 · 원격 접속 대기"); }
    else if (type == "input") {
      // input-latency-v1 adds nothing to the injected event: the same message is
      // applied in the same order. Service: a sampled input carries its receipt
      // time to the worker. Worker: the report follows the injection call.
      if (bridge_) { if (const auto stamped = inputLatency_.stamp(message, hostMonotonicUs())) bridge_->control(*stamped); else bridge_->control(message); }
      else if(auto input=trackInput_.load()){
        // System input on independent outputs (system_input.hpp): the service keeps the client's event and asks
        // this worker only to PLACE it. The track plan admits it exactly as it would before injecting (session,
        // topology, the output and its epoch, the client's acknowledgement) and answers with the desktop pixel of
        // its position, or with a refusal. Nothing is injected here, and the answer is written under the plan's
        // lock, so it is ordered with every change of the outputs.
        if(const auto asked=systemInputAsked(message)){
          systemInputSeen_=true;
          if(sysinput::answerTrackInput(input.get(),message,*asked,sessionId_,[this](const std::string& answer){try{text(answer);}catch(...){ }}))wakeCapture();
        }else input->apply(message,[this](const Json& converted){if(input_.apply(converted))wakeCapture();});
      }
      // Asked to place an input while no track plan runs (it just ended): refused, so the service drops it.
      else if(const auto asked=systemInputAsked(message)){systemInputSeen_=true;(void)sysinput::answerTrackInput(nullptr,message,*asked,sessionId_,[this](const std::string& answer){try{text(answer);}catch(...){ }});}
      else {
        // text-input-v1 text never moves the pointer and is never sampled;
        // touch-input-v1 frames neither (Windows draws no cursor for touch).
        const auto event = message.get("event").string();
        const bool textEvent = event == "text", touchEvent = event == "touch";
        // System input: the service already wrote this input to its SYSTEM injector (the marker); nothing is
        // injected here, the same grants decide whether it counts as injected.
        const auto* marker = std::get_if<bool>(&message.get(sysinput::workerMarker).value);
        const bool viaSystem = local_ && marker && *marker;
        if (viaSystem) {
          systemInputSeen_ = true;
          if (event == "button" && std::holds_alternative<bool>(message.get("down").value)) { const auto button = message.get("button").string(); if (message.get("down").boolean()) systemButtons_.insert(button); else systemButtons_.erase(button); }
        }
        const bool injected = viaSystem ? (textEvent ? textInput_.load() : touchEvent ? touchInput_.load() : (event != "moveRelative" || relativeMouse_))
          : textEvent ? textInput_ && input_.text(message) : touchEvent ? touchInput_ && input_.touch(message) : (event != "moveRelative" || relativeMouse_) && input_.apply(message);
        if (injected) wakeCapture();
        if (injected && !textEvent) markCursorInput(message);
        // input-latency-v1: only an input that can change the picture by itself
        // names a frame (no key release or modifier; under cursor-v1 a move only
        // while a button is held, i.e. a drag).
        const bool frameCandidate = inputFrameCandidate(message, cursorSeparated_, viaSystem ? !systemButtons_.empty() : input_.buttonHeld());
        if (local_) if (const auto report = inputLatency_.injected(message, injected, hostMonotonicUs(), frameCandidate)) text(*report);
      }
    }
    else if (type == "ping") { const auto nonce = message.get("id").string(); if (nonce.size() <= 128) { if (losslessTilesOptIn(nonce)) requestLossless(); else if (losslessTilesPause(nonce)) requestLossless(false); text("{\"type\":\"pong\",\"id\":" + jsonQuote(nonce) + '}'); } }
    else if (type == "stream.lossless") { if (!bridge_ && message.get("protocol").string() == "tiles-v1") { const auto* on = std::get_if<bool>(&message.get("on").value); requestLossless(!on || *on); } }
    // display-scale-v1: the worker owning the output applies it (service.hpp).
    else if (type == "display.scale") { if (bridge_) bridge_->control(message); }
    // caret-v1: the service hands it to the worker, which owns the user session.
    else if (type == "caret.watch") {
      if (bridge_) bridge_->control(message);
      else if (caretGranted_) if (const auto on = caretWatchRequest(message)) { caretWanted_ = *on; caret_.watch(*on); }
    }
    // cursor-echo-v1: likewise; the worker's capture polls while asked.
    else if (type == "cursor.echo") {
      if (bridge_) bridge_->control(message);
      else if (cursorEchoGranted_) if (const auto on = cursor::echoRequest(message)) setCursorEcho(*on);
    }
    // cursor-compose-v1: likewise; the worker's capture composes while asked.
    else if (type == "cursor.compose") {
      if (bridge_) bridge_->control(message);
      else if (const auto on = cursor::composeRequest(message)) cursorCompose_.set(*on);
    }
    // background-video-v1: likewise; the worker's capture takes one picture a second while hidden, and the first
    // picture after it is an IDR (the client may have dropped its decoder's surface meanwhile).
    // gamepad-v1: the service's own virtual controllers (gamepad_device.hpp); a worker has nothing to do with them.
    else if (type == "gamepad.state" || type == "gamepad.remove") { if (bridge_) bridge_->control(message); }
    else if (type == "stream.background") {
      if (bridge_) bridge_->control(message);
      else if (backgroundGranted_ && (message.get("sessionId").string().empty() || message.get("sessionId").string() == sessionId_))
        if (const auto on = backgroundRequest(message)) { if (streamBackground_.exchange(*on) && !*on) keyFrameRequested_ = true; }
    }
    // stream-size-v1: the client's decoder load; the service hands it to the
    // worker, whose capture thread reads the latest report.
    else if (type == "stream.decoder") {
      if (bridge_) bridge_->control(message);
      else if (const auto report = streamDecoderReport(message)) { clientDecoderAt_ = GetTickCount64(); clientDecoder_ = *report; }
    }
    // Adaptive bitrate: client arrivals (via the relay) feed the socket owner's
    // controller; its target reaches the encoder owner as stream.bitrate.
    else if (type == "stream.feedback") { if (bridge_ || !local_) adaptive_.feedback(message); if (const auto leg = udpProbe_.load()) leg->wssHeard(); }
    else if (type == "stream.bitrate") {
      const double value = message.get("bitrate").number(-1);
      if (!bridge_ && value >= encoderMinimumBitrate && value <= encoderMaximumBitrate && std::floor(value) == value) requestedBitrate_ = int(value);
    }
    // The service's send queue dropped video of this session (service.hpp
    // forwards it to the worker): the capture loop codes its next frame as an IDR.
    else if (type == "stream.keyframe") { if (bridge_) bridge_->keyFrame(); else { if (const auto* resend = std::get_if<bool>(&message.get("cursor").value); resend && *resend) cursorResend_ = true; keyFrameRequested_ = true; } }
    // udp-media-v1 LOSS, from the service (service.hpp) to the worker's capture loop only.
    else if (type == "stream.recover") {
      const double pts = message.get("pts").number(-1);
      if (!bridge_ && pts >= 0 && pts < 9007199254740991.0 && std::floor(pts) == pts) requestRecover(uint64_t(pts));
    }
    // udp-media-v1: for the current stream, on the same active and session checks.
    else if (type == "udp.grant") { if (bridge_) udpGrant(message, bytes.size()); }
    else if (type == "udp.key") { if (bridge_) udpKey(message); }
    else if (type == "udp.peer") { if (message.get("sessionId").string()==sessionId_) if(const auto leg=udpProbe_.load())leg->peer(message); }
  }
  // The Session0 diagnostics snapshot (measured 3.5 ms p50, 76 ms max on a
  // GPU-less VM) used to run on this send thread, holding every queued video
  // and audio packet meanwhile. It now runs on its own thread, one at a time;
  // its report is queued like any text. Send thread only; joinDiagnostics()
  // once the send thread has ended.
  std::thread diagnosticsCollector_; std::atomic<bool> diagnosticsCollecting_{false};
  void collectDiagnostics() {
    if (!bridge_ || diagnosticsCollecting_) return;
    const bool report=diagnosticSchedule_.Claim(int64_t(GetTickCount64()));
    if(!report&&!hostObservationLogsPending())return;
    if (diagnosticsCollector_.joinable()) diagnosticsCollector_.join();
    diagnosticsCollecting_ = true;
    try {
      diagnosticsCollector_ = std::thread([this,report] {
        flushHostObservationLogs();
        if(report)try { const auto value = bridge_->diagnostics(); if (value.size() <= 6144) text("{\"type\":\"host.diagnostics\",\"report\":" + value + '}'); } catch (...) { }
        diagnosticsCollecting_ = false;
      });
    } catch (...) { diagnosticsCollecting_ = false; }
  }
  void joinDiagnostics() { if (diagnosticsCollector_.joinable()) diagnosticsCollector_.join(); }
  // The only writer of this channel: every message leaves in fragments of at
  // most webSocketFragmentBytes (websocket_send.hpp: WinHTTP's PONG would
  // otherwise land inside a large frame and the relay drops the channel).
  DWORD writeMessage(bool text,const uint8_t* data,size_t size){return sendWebSocketMessageOn(socket_.load(),text,data,size);}
  // The newest capability report this channel wrote (resource_sampler.hpp). The
  // sending thread's alone, like publishOutgoing; each channel starts empty.
  HostHeartbeatCapabilities heartbeatCapabilities_;
  std::string heartbeatCapabilities() { return bridge_ ? hostHeartbeatCapabilities(heartbeatCapabilities_, Clock::now(), [this] { return bridge_->capabilities(); }) : std::string(); }
  void sendLoop() {
    auto heartbeat = Clock::now();auto signinEligibilityAt=Clock::time_point{};
    ResourceSampler resources = ResourceSampler::System(); ResourceReportPolicy resourcePolicy; std::optional<ResourceReading> latestResources; auto resourceSampledAt = Clock::now() - std::chrono::seconds(2);
    heartbeatCapabilities_ = {};
    const auto finishSending=[this]{{std::lock_guard lock(queueMutex_);sending_=false;}queueCondition_.notify_all();};
    while (!stopped_) {
      input_.retry();
      Outgoing packet; bool found = false;
      // Idle: wake when the next sample or heartbeat is due (about every 2 s, was every 1 s).
      { std::unique_lock lock(queueMutex_); queueCondition_.wait_for(lock, hostSenderIdleWait(Clock::now(), resourceSampledAt, heartbeat), [this] { return queueClosed_ || !queue_.empty(); }); if (queueClosed_) return; found = queue_.pop(packet); sending_ = found; }
      if(brokerStatus_&&brokerStatus_->expired(brokerAttempt_.load())){closeTransport("heartbeat_timeout");return;}
      collectDiagnostics();
      if(updater_)try{updater_->poll();}catch(...){ }
      // host-resources-v1: sampled about every 2 s (two cheap system calls);
      // goes out with each heartbeat, and sooner after a >= 5 point change.
      if (hostResourceSampleDue(Clock::now(), resourceSampledAt)) { resourceSampledAt = Clock::now(); try { latestResources = resources.Sample(); } catch (...) { latestResources.reset(); } }
      const bool resourcesEarly = latestResources && Clock::now() - heartbeat >= ResourceReportPolicy::kMinInterval && resourcePolicy.EarlyDue(*latestResources, Clock::now());
      const bool heartbeatDue = hostHeartbeatDue(Clock::now(), heartbeat);
      if(bridge_&&signinNegotiated_&&Clock::now()-signinEligibilityAt>=std::chrono::seconds(1)){signinEligibilityAt=Clock::now();const auto eligibility=bridge_->signinEligibility();if(!eligibility.empty()){if(const DWORD error=writeMessage(true,reinterpret_cast<const uint8_t*>(eligibility.data()),eligibility.size());error!=NO_ERROR){finishSending();closeTransport("send_failed",error);return;}}}
      if (!found && !resourcesEarly && !heartbeatDue) continue;
      if (resourcesEarly || heartbeatDue) { const std::string capabilities = heartbeatCapabilities(); const std::string ping = "{\"type\":\"host.heartbeat\",\"version\":\"" MULTITAP_HOST_VERSION_UTF8 "\"" + (bridge_ ? ",\"capabilities\":" + capabilities : "") + (latestResources ? ",\"resources\":" + ResourcesJson(*latestResources, resourcesUptime_) : std::string()) + '}'; if (latestResources) resourcePolicy.Sent(*latestResources, Clock::now()); if (const DWORD error = writeMessage(true, reinterpret_cast<const uint8_t*>(ping.data()), ping.size()); error != NO_ERROR) { finishSending(); closeTransport("send_failed", error); return; } heartbeat = Clock::now(); if (bridge_) bootReady(capabilities); }
      DWORD sendError = NO_ERROR;
      if (found && !publishOutgoing(packet,[&](bool text,const std::vector<uint8_t>& bytes){sendError=writeMessage(text,bytes.data(),bytes.size());return sendError==NO_ERROR;})) { finishSending(); streamStop_ = true; active_ = false; input_.reset(); closeTransport("send_failed", sendError); return; }
      if (found) finishSending();
      // Adaptive: a completed write (host uplink rate) and the remaining depth.
      if (found && !packet.text && adaptive_.active()) {
        adaptive_.written(packet.bytes.size());
        uint64_t oldestUs = 0; size_t depthBytes = 0, depthPackets = 0;
        { std::lock_guard lock(queueMutex_); if (!queue_.empty()) oldestUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - queue_.oldest()).count()); depthBytes = queue_.bytes(); depthPackets = queue_.packets(); }
        adaptive_.queue(oldestUs, depthBytes, depthPackets);
      }
    }
  }
  void run() {
    while (!stopped_) {
      std::thread sender;
      const auto attempt=brokerStatus_?brokerStatus_->begin():0;brokerAttempt_=attempt;const char* failure="network_unavailable";DWORD receiveError=0;uint32_t remoteCloseCode=0;bool machineRefused=false;
      try {
        const auto origin=verifiedCredentialOrigin(credentials_); report(L"클라우드 중계 서버에 연결 중…");
        Internet session(httpSession()), connection(WinHttpConnect(session.handle, origin.hostname.c_str(), origin.port, 0));
        Internet request(WinHttpOpenRequest(connection.handle, L"GET", L"/api/v1/hosts/channel", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, origin.flags()));
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; winCheck(WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)), "Disable channel redirects");
        winCheck(WinHttpSetOption(request.handle, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0), "Request secure WebSocket");
        // host-route-v1: the request keeps the public name; only the address it resolves changes.
        routed_=false;
        if(const auto route=brokerRoute().current(GetTickCount64());!route.empty())routed_=WinHttpSetOption(request.handle,WINHTTP_OPTION_RESOLUTION_HOSTNAME,const_cast<wchar_t*>(route.c_str()),DWORD(route.size()*sizeof(wchar_t)))!=FALSE;
        // host-machine-v1 (credential_binding.hpp): the SMBIOS UUID of this Windows, when one could be read.
        const std::wstring authorization = L"Authorization: Host " + wide(credentials_.token) + L"\r\n" + (bridge_ ? L"X-Multitap-Host-Protocol: capabilities-v1\r\n" : L"") + (credentialMachineUuid(credentials_.machineUuid) ? L"X-Multitap-Host-Machine: " + wide(credentials_.machineUuid) + L"\r\n" : L"");
        const auto channelCheck=[&](BOOL ok,const char* operation){if(!ok){const auto error=GetLastError();if(error==ERROR_WINHTTP_SECURE_FAILURE||error==ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED)failure="tls_failed";SetLastError(error);winCheck(FALSE,operation);}};
        channelCheck(WinHttpSendRequest(request.handle, authorization.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0), "Connect host channel"); channelCheck(WinHttpReceiveResponse(request.handle, nullptr), "Receive host channel handshake");
        DWORD code = 0, length = sizeof(code); winCheck(WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &length, WINHTTP_NO_HEADER_INDEX), "Read host channel status"); if (code != 101) {failure=code==401||code==403?"authorization_failed":"server_unavailable";if(code==403&&hostChannelRefusal(request.handle)=="host_machine_changed")machineRefused=true;throw std::runtime_error("Host channel HTTP " + std::to_string(code));}
        HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request.handle, 0); if (!socket) winCheck(FALSE, "Upgrade host channel"); socket_ = socket;
        reconnectWait_.connected();
        if(brokerStatus_)brokerStatus_->upgraded(attempt);failure="transport_closed";
        if(bridge_)boot_->mark(BootStage::broker);
        if(routed_)recordHostEvent("hostAction",{{"action",brokerRouteAction},{"stage",uint32_t(1)},{"ok",true}});
        { std::lock_guard lock(queueMutex_); queueClosed_ = false; sending_ = false; queue_.clear(); }
        diagnosticSchedule_.Reset();closingNegotiated_=false;resourcesUptime_=false;signinNegotiated_=false;resume_.channel();
        if(bridge_)bridge_->mainConnected(socket,[this,socket](const std::string& value){try{return admitMainText(socket,value);}catch(...){return false;}},[this](HINTERNET expected){return expected&&socket_.load()==expected&&!stopped_;});
        {std::lock_guard lock(queueMutex_);queue_.clear();} // Old callbacks drained by lifecycle hooks; publish only this connection's fresh capabilities.
        sender = std::thread([this] { try{sendLoop();}catch(...){input_.reset();closeTransport("send_failed");} }); report(L"서버 응답 확인 중…");
        // Before capabilities: no session can bind to this channel yet, so a
        // released broker simply ignores this unknown message.
        if(updater_)try{text(updater_->hello());}catch(...){ }
        if(power_)try{text(power_->hello());}catch(...){ }
        announceCapabilities();
        std::array<uint8_t, 16384> buffer{}; std::string message;
        while (!stopped_ && socket_.load()) {
          DWORD received = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
          const DWORD result = WinHttpWebSocketReceive(socket, buffer.data(), DWORD(buffer.size()), &received, &type);
          if(result!=NO_ERROR){receiveError=result;break;}
          if(type==WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE){remoteCloseCode=hostChannelRemoteCloseCode([&](USHORT* status,void* reasonBuffer,DWORD capacity,DWORD* bytesUsed){return WinHttpWebSocketQueryCloseStatus(socket,status,reasonBuffer,capacity,bytesUsed);});break;}
          if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE && type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {failure="protocol_rejected";throw std::runtime_error("Unexpected host control payload");}
          if (message.size() + received > 65536) {failure="protocol_rejected";throw std::runtime_error("Host control exceeds bounds");}message.append(reinterpret_cast<const char*>(buffer.data()), received);
          if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) { control(message,socket); message.clear(); }
        }
      } catch (const std::exception& error) { if (!stopped_) report(L"연결 대기: " + wide(error.what())); }
      // Anything else a control handler throws also ends only this channel and
      // reconnects; escaping run() would terminate the boot service.
      catch (...) { if (!stopped_) report(L"연결 대기"); }
      // A control message the Host refused: tell a broker that accepts it why.
      if (!stopped_ && std::string_view(failure) == "protocol_rejected") closingNotice("protocol_rejected");
      // A routed attempt that could not reach or trust the route: DNS for the next ones.
      if (routed_ && !stopped_ && (std::string_view(failure) == "network_unavailable" || std::string_view(failure) == "tls_failed")) {
        brokerRoute().failed(GetTickCount64());routed_=false;
        recordHostEvent("hostAction",{{"action",brokerRouteAction},{"stage",uint32_t(2)},{"ok",false}});
      }
      input_.reset(); closeTransport(failure, receiveError, remoteCloseCode); channelEnded(failure);
      if (sender.joinable()) sender.join(); joinDiagnostics();
      // host-machine-v1: the server says this registration belongs to another
      // machine (a copy of a registered desktop). Dialling again cannot succeed
      // and only fills its log: hand it to the service and stop.
      if (machineRefused && !stopped_) { if (machineChanged_) try { machineChanged_(); } catch (...) {} break; }
      // Released: 2 s after any end. After a broker restart (broker_reconnect.hpp)
      // 250 ms, 500 ms, then 1 s for a few dials.
      const auto ended = GetTickCount64(), until = ended + channelEndWait(remoteCloseCode);
      for (auto now = ended; !stopped_ && now < until; now = GetTickCount64()) { input_.retry(); std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(100, until - now))); }
      if (resume_.expired(GetTickCount64())) stopStream();
    }
    report(L"연결 중지됨");
  }
  // thumbnail-capture-v1: the picture (binary, "TVP1") right before its result
  // on this ordered queue. A second request while one runs answers busy.
  void thumbnailRequest(const Json& message){
    const auto id=message.get("requestId").string();
    if(!bridge_||!validThumbnailRequestId(id))return;
    if(!thumbnails_.start([this,id]{
         ThumbnailShot shot;try{shot=bridge_->thumbnail();}catch(...){shot={};}
         if(shot.state==ThumbnailState::captured)enqueue({false,thumbnailPacket(id,shot.jpeg)});
         text(thumbnailResultJson(id,shot));}))
      text(thumbnailResultJson(id,ThumbnailShot{ThumbnailState::busy}));
  }
  // Declared last: destroyed (joined) first, while the queue it writes to and
  // the bridge (released after this Agent) still exist.
  ThumbnailRunner thumbnails_;
};
}
