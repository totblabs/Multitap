#pragma once
// System input (docs/system-input.md).
//
// Windows drops input a process injects into a window whose process is above it
// (UIPI), without an error. A standard user's worker (medium integrity) can
// therefore operate neither a UAC prompt shown on the user's own desktop
// (PromptOnSecureDesktop = 0: consent.exe is an ordinary window at System
// integrity) nor any elevated window; an administrator's elevated worker meets
// the same wall at consent.exe. For such a stream the SERVICE writes the
// client's input to a SYSTEM injector it started in the stream's own console
// session, for the whole stream (no switching per window: nothing to race when a
// prompt appears, no key held across a hand-over).
//
// The boundary:
//   * input comes only from the service's control path, which carries the
//     authenticated client's input of this desktop's own stream. The user worker
//     never carries input to the injector; it is only told that an input was
//     injected elsewhere, so its bookkeeping (cursor mark, capture wake, latency
//     sample) goes on;
//   * the injector is the service's own image started by the service on
//     winsta0\Default (secure_desktop_launch.hpp): no named object, inherited
//     handles only, in the service's worker registry, ended with the relay;
//   * it injects only while its session is the active, unlocked console session
//     signed in as the stream's user, and only while the input desktop is the
//     user's ("Default"). On the secure desktop (UAC, Ctrl+Alt+Del) it injects
//     nothing: there the secure-desktop helper and its rules apply unchanged. A
//     locked, signed-out or switched session ends it;
//   * it has no capture, no encoder, no network and touches no policy value;
//   * counts only are recorded (hostAction 200 stage 246), never an input.
//
// Everything here is pure (no Windows calls): the decision, the injector's loop
// and the service's routing are tested with fakes.
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <utility>

#include "json_object.hpp"
#include "secure_desktop_ipc.hpp"
#include "secure_desktop_scope.hpp"
#include "secure_desktop_service.hpp"  // ServiceHelper

namespace host::sysinput {

// hostAction 200 stage 246, kinds of the system injector (the helper's kinds 1..10
// are in secure_desktop_scope.hpp). error = kind | value << 8.
//  11 received by the injector      12 accepted by Windows
//  13 dropped (not an input, or a kind this session was not granted)
//  14 refused before Windows was asked (malformed, outside the output, no area yet)
//  15 SendInput failed (count)      16 last SendInput error (Win32 code)
//  17 not injected because the input desktop was not the user's (secure desktop)
//  18 the injector was started; value = worker level | reason << 4
//     (reason 1 the worker is not elevated, 2 an elevated worker and UAC prompts
//      on the user's desktop)
//  19 the injector could not be started (the worker injects as before)
//  20 written to the injector by the service    21 the service's write failed
//  22 input the service gave the worker because no injector was alive
//  23 independent outputs: inputs NOT injected because the worker did not place them (an output that is gone
//     or changed, a plan the client has not acknowledged, a malformed event), gave no position for a positional
//     event, or never answered. Dropped, never guessed.
//  24 independent outputs: inputs the service asked the worker to place
//  25 independent outputs under a secure desktop: placed inputs the service wrote to a secure-desktop helper
inline constexpr uint32_t record246SecureRouted = 25;
inline constexpr uint32_t record246Received = 11, record246Injected = 12, record246Dropped = 13, record246Refused = 14,
    record246SendFailed = 15, record246LastError = 16, record246OffDesktop = 17, record246Started = 18,
    record246StartFailed = 19, record246Routed = 20, record246WriteFailed = 21, record246FellBack = 22,
    record246TrackDropped = 23, record246TrackAsked = 24;

// Why a stream gets the injector. Decided once, when the stream's relay starts.
//   worker below High (a standard user): always; no elevated window and no UAC
//     prompt on the user's desktop can be operated by it;
//   worker at High (an administrator's elevated token): only when Windows shows
//     UAC prompts on the user's desktop (PromptOnSecureDesktop = 0) and prompts
//     an administrator at all (ConsentPromptBehaviorAdmin != 0). consent.exe runs
//     at System integrity, above High. With the secure desktop on, the
//     secure-desktop helper answers prompts; with prompts off none appears;
//   independent outputs (tracks) are decided the same way: the worker's track plan still places
//     every position, the client's event itself goes from the service to the injector (ServiceSystemInput).
enum class Need { none, workerNotElevated, promptOnUserDesktop };
inline constexpr Need systemInputNeed(uint32_t workerLevel, bool promptOnSecureDesktop, uint32_t consentAdmin) noexcept {
  if (workerLevel == secure::integrityUnknown) return Need::none;
  if (workerLevel < 3) return Need::workerNotElevated;
  if (workerLevel == 3 && !promptOnSecureDesktop && consentAdmin != 0) return Need::promptOnUserDesktop;
  return Need::none;
}

// The marker the service sets on an input it wrote to the injector, for the
// worker. A client's own field of that name never reaches a worker.
inline constexpr char workerMarker[] = "systemInput";
// The worker tells the service which rectangle of the desktop the stream's
// picture is (it follows mode changes); the service hands it to the injector.
inline constexpr char controlArea[] = "worker.input.area";
inline constexpr char controlPrefix[] = "worker.input.";
inline constexpr char injectorArea[] = "system.area";
// Independent outputs. The client names an output and a position on its picture; only the worker's track plan
// knows where that output is on the desktop right now (and whether the client's plan is still the current one).
// The service therefore asks the worker to PLACE each input (the worker's copy carries workerSeq) and the worker
// answers with controlTrack: the sequence number, ok, and for a positional event the desktop pixel. The service
// then writes the CLIENT's event, with that pixel, to the injector. The worker supplies a position and nothing
// else: it cannot choose an event, a key or a button, and an answer that is not the next one expected is dropped.
inline constexpr char workerSeq[] = "systemSeq";
inline constexpr char controlTrack[] = "worker.input.track";
inline constexpr char placedX[] = "px";
inline constexpr char placedY[] = "py";

struct Area { int32_t left = 0, top = 0, right = 0, bottom = 0; };
// system.area from the service: whole numbers, a rectangle of at least 2x2.
inline std::optional<Area> parseArea(const Json& message) {
  double edge[4] = {};
  const char* names[4] = {"left", "top", "right", "bottom"};
  for (int i = 0; i < 4; ++i) if (!secure::detail::wholeNumber(message.get(names[i]), -65536, 65536, edge[i])) return {};
  if (edge[2] - edge[0] < 2 || edge[3] - edge[1] < 2) return {};
  return Area{int32_t(edge[0]), int32_t(edge[1]), int32_t(edge[2]), int32_t(edge[3])};
}

// The worker's answer to one input the service asked it to place.
inline std::string trackAnswerJson(const std::string& session, uint32_t seq, bool ok, const std::optional<std::pair<int32_t, int32_t>>& desktop) {
  std::string text = std::string("{\"type\":\"") + controlTrack + "\",\"sessionId\":" + jsonQuote(session) + ",\"seq\":" + std::to_string(seq) + ",\"ok\":" + (ok ? "true" : "false");
  if (ok && desktop) text += std::string(",\"") + placedX + "\":" + std::to_string(desktop->first) + ",\"" + placedY + "\":" + std::to_string(desktop->second);
  return text + '}';
}
// What the service writes to the injector for a placed input of independent outputs: the client's own event
// and its own content, and for a positional event the pixel the worker answered. nullopt: not an event of
// independent outputs, or a positional event without a position.
inline std::optional<std::string> trackInjectorMessage(const Json& client, const std::optional<std::pair<int32_t, int32_t>>& desktop) {
  const auto* fields = std::get_if<Json::Object>(&client.value);
  if (!fields || client.get("type").string() != "input") return {};
  const auto event = client.get("event").string();
  Json out; Json::Object object;
  const auto copy = [&](const char* name) { if (const auto found = fields->find(name); found != fields->end()) object[name] = found->second; };
  Json type; type.value = std::string("input"); object["type"] = type;
  Json kind; kind.value = event; object["event"] = kind;
  if (event == "key") { for (const char* name : {"vk", "scan", "down", "extended"}) copy(name); }
  else if (event == "moveRelative") { copy("dx"); copy("dy"); }
  else if (event == "button") { copy("button"); copy("down"); }
  else if (event == "wheel") { copy("delta"); }
  else if (event != "move") return {};
  const bool positional = event == "move" || fields->contains("track");
  if (positional) {
    if (!desktop) return {};
    Json x; x.value = double(desktop->first); object[placedX] = x;
    Json y; y.value = double(desktop->second); object[placedY] = y;
  }
  out.value = std::move(object);
  return jsonObject(out);
}
// The injector's side of a pixel position: the same event with x and y as fractions of the physical desktop
// (what RemoteInput takes when its area is that desktop). nullopt: not whole numbers, or not on the desktop.
inline std::optional<Json> placeOnDesktop(const Json& message, const Area& desktop) {
  double px = 0, py = 0;
  if (!secure::detail::wholeNumber(message.get(placedX), -131072, 131072, px) || !secure::detail::wholeNumber(message.get(placedY), -131072, 131072, py)) return {};
  const double width = double(desktop.right) - desktop.left, height = double(desktop.bottom) - desktop.top;
  if (width < 2 || height < 2 || px < desktop.left || px >= desktop.right || py < desktop.top || py >= desktop.bottom) return {};
  auto placed = message;
  auto* fields = std::get_if<Json::Object>(&placed.value);
  if (!fields) return {};
  fields->erase(placedX); fields->erase(placedY);
  Json x; x.value = (px - desktop.left) / (width - 1); (*fields)["x"] = x;
  Json y; y.value = (py - desktop.top) / (height - 1); (*fields)["y"] = y;
  return placed;
}

struct Grants { bool relative = false, text = false, touch = false; };

struct InjectorConfig {
  uint32_t session = 0;
  std::string userSid;  // the stream's signed-in user (never a service SID)
  Grants grants;
};
struct InjectorView {
  bool isSystem = false;
  uint32_t session = 0;
  bool winSta0 = false;
  bool inputDefault = false;  // the desktop that takes input now is the user's
};
enum class Note { received, injected, dropped, refused, offDesktop };
enum class InjectorExit { transportClosed, scopeLost, badConfig };

// Injected Windows parts. Every callback but note is required.
struct InjectorIo {
  std::function<secure::ConsoleView()> console;  // fresh WTS observation of the session
  std::function<InjectorView()> self;            // fresh observation of this process and the input desktop
  std::function<uint64_t()> nowMs;
  std::function<std::optional<Json>(uint32_t)> next;  // one message from the service, waiting up to N ms
  std::function<bool()> attach;                  // this thread joins the user's desktop; false: it is not the input desktop
  std::function<bool(const Json&)> inject;       // key, move, moveRelative, button, wheel
  std::function<bool(const Json&)> text;         // text-input-v1
  std::function<bool(const Json&)> touch;        // touch-input-v1
  std::function<bool(const Json&)> area;         // the stream's rectangle changed
  std::function<void()> release;                 // let go of every key, button and contact this injector holds
  std::function<bool()> stopped;                 // the service closed the pipe or the worker exited
  std::function<void(Note)> note;                // counts only
};

// The console facts are observed again at most every consoleCacheMs; the input
// desktop is looked at for every event (it is the first thing that changes on a
// lock or a prompt).
inline constexpr uint64_t consoleCacheMs = 50;
inline constexpr uint32_t idleWaitMs = 100;

inline InjectorExit runSystemInput(const InjectorConfig& config, const InjectorIo& io) {
  if (!config.session || config.session == 0xffffffffu || config.userSid.empty() || !io.console || !io.self || !io.nowMs ||
      !io.next || !io.attach || !io.inject || !io.text || !io.touch || !io.area || !io.release || !io.stopped)
    return InjectorExit::badConfig;
  const auto note = [&](Note what) { if (io.note) io.note(what); };
  bool consoleOk = false, observed = false, attached = false, away = false;
  uint64_t observedAt = 0;
  for (;;) {
    if (io.stopped()) { if (attached && !away) io.release(); return InjectorExit::transportClosed; }
    const auto message = io.next(idleWaitMs);
    // Scope first, for every message and every idle wake.
    if (const uint64_t before = io.nowMs(); !observed || before < observedAt || before - observedAt >= consoleCacheMs) {
      const secure::ConsoleView console = io.console();
      observedAt = io.nowMs();
      observed = true;
      consoleOk = secure::consoleAllowed(config.session, config.userSid, console, observedAt) == secure::Denial::none;
    }
    const InjectorView self = io.self();
    if (!consoleOk || !self.isSystem || self.session != config.session || !self.winSta0) {
      // Locked, signed out, another user, another session: nothing more is injected.
      if (attached && !away && self.inputDefault) io.release();
      return InjectorExit::scopeLost;
    }
    if (!self.inputDefault) {
      // The secure desktop (or another one) takes input: not this injector's.
      away = true;
      if (message && message->get("type").string() != injectorArea) note(Note::offDesktop);
      else if (message) (void)io.area(*message);
      continue;
    }
    if (away || !attached) {
      if (!io.attach()) { if (message) note(Note::offDesktop); continue; }
      // Back on the user's desktop: what was held before the switch is let go
      // (its release went to the other desktop, or never came).
      if (away) io.release();
      attached = true; away = false;
    }
    if (!message) continue;
    const auto type = message->get("type").string();
    if (type == injectorArea) { (void)io.area(*message); continue; }
    note(Note::received);
    if (!secure::inputTypeAllowed(type)) { note(Note::dropped); continue; }
    if (type == "input.reset") { io.release(); continue; }
    const auto event = message->get("event").string();
    bool granted = true, done = false;
    if (event == "text") { granted = config.grants.text; if (granted) done = io.text(*message); }
    else if (event == "touch") { granted = config.grants.touch; if (granted) done = io.touch(*message); }
    else if (event == "moveRelative") { granted = config.grants.relative; if (granted) done = io.inject(*message); }
    else if (event == "key" || event == "move" || event == "button" || event == "wheel") done = io.inject(*message);
    else granted = false;
    note(!granted ? Note::dropped : done ? Note::injected : Note::refused);
  }
}

// ---- service side -------------------------------------------------------------

struct ServiceCounts { uint32_t routed = 0, writeFailed = 0, fellBack = 0, trackAsked = 0, trackDropped = 0, secureRouted = 0; };

// One per relay (one stream). The relay thread attaches the injector it launched
// and closes it; the control path routes input through it.
class ServiceSystemInput {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr size_t pendingLimit = 512;                       // inputs waiting for the worker's placement
  static constexpr auto pendingAge = std::chrono::milliseconds(2000);  // a placement that takes longer is not waited for

  ServiceSystemInput() = default;
  ~ServiceSystemInput() { (void)close(); }
  ServiceSystemInput(const ServiceSystemInput&) = delete;
  ServiceSystemInput& operator=(const ServiceSystemInput&) = delete;

  // Relay thread, before any input: this stream is one of independent outputs.
  // Its input comes through routeTrack(); route() then leaves everything to the worker.
  void tracks(bool on) { std::lock_guard<std::mutex> lock(mutex_); tracks_ = on; }

  // Independent outputs under a secure desktop (secure_desktop_tracks.hpp). While a secure-desktop
  // helper of this stream shows the prompt, the input goes to that helper, never to the injector (which injects
  // nothing off the user's desktop) and never to the worker (which cannot reach the secure desktop): the worker
  // places it exactly as for the injector and the service writes the client's event to the helper of the
  // output the pixel is on. This also holds for a stream that has no injector (an elevated worker).
  // routing(): a helper shows the prompt now. send(message, x, y, positional): written, or not. reset(message):
  // an input.reset for every helper that shows the prompt.
  struct SecureSink {
    std::function<bool()> routing;
    std::function<bool(const std::string& message, int32_t x, int32_t y, bool positional)> send;
    std::function<bool(const std::string& message)> reset;
  };
  void secureSink(SecureSink sink) { std::lock_guard<std::mutex> lock(mutex_); secure_ = std::move(sink); }

  // Relay thread: the injector the service launched for this stream.
  void attach(std::shared_ptr<secure::ServiceHelper> helper) {
    std::shared_ptr<secure::ServiceHelper> old;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) { old = std::move(helper); }
      else { old = std::move(helper_); helper_ = std::move(helper); broken_ = false; wanted_ = true; if (helper_ && !area_.empty()) (void)helper_->send(area_); }
    }
    if (old) old->close();
  }

  // Relay thread. true: one of this feature's worker messages (consumed, never
  // forwarded to the broker), whether or not an injector exists.
  bool consume(const Json& value, const std::string& session) {
    const auto type = value.get("type").string();
    if (type.rfind(controlPrefix, 0) != 0) return false;
    if (session.empty() || value.get("sessionId").string() != session) return true;
    if (type == controlTrack) { placed(value); return true; }
    if (type != controlArea) return true;
    double edge[4] = {};
    const char* names[4] = {"left", "top", "right", "bottom"};
    for (int i = 0; i < 4; ++i) if (!secure::detail::wholeNumber(value.get(names[i]), -65536, 65536, edge[i])) return true;
    if (edge[2] - edge[0] < 2 || edge[3] - edge[1] < 2) return true;
    std::string area = std::string("{\"type\":\"") + injectorArea + "\",\"left\":" + std::to_string(int(edge[0])) + ",\"top\":" + std::to_string(int(edge[1])) +
                       ",\"right\":" + std::to_string(int(edge[2])) + ",\"bottom\":" + std::to_string(int(edge[3])) + '}';
    std::lock_guard<std::mutex> lock(mutex_);
    area_ = std::move(area);
    if (helper_ && !broken_ && !helper_->send(area_)) broken_ = true;
    return true;
  }

  // Control path (any thread). `command` is an input or input.reset on its way to
  // the worker. true: it was written to the injector, and `command` now carries
  // the marker so the worker does not inject it again. false: the worker injects
  // it, as it always did (no injector, or it is gone). A marker the client put
  // there itself is removed either way.
  bool route(Json& command) {
    auto* fields = std::get_if<Json::Object>(&command.value);
    if (!fields) return false;
    fields->erase(workerMarker); fields->erase(workerSeq);
    const auto type = command.get("type").string();
    if (!secure::inputTypeAllowed(type)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wanted_ || tracks_) return false;
    if (!helper_ || broken_ || !helper_->alive()) { ++counts_.fellBack; return false; }
    const auto text = jsonObject(command);
    if (text.size() > secure::ServiceSecure::inputLimit) { ++counts_.fellBack; return false; }
    if (!helper_->send(text)) { broken_ = true; ++counts_.writeFailed; return false; }
    ++counts_.routed;
    Json marker; marker.value = true;
    (*fields)[workerMarker] = std::move(marker);
    return true;
  }

  // The tunnel of independent outputs (any thread). `command` is an input or
  // input.reset on its way to the worker. true for an input: the service keeps
  // the client's event, and `command` now asks the worker to place it (marker
  // and sequence number); the worker answers with controlTrack and injects
  // nothing. true for input.reset: it was written to the injector. false: the
  // worker injects it, as before. The client's own marker fields never pass.
  bool routeTrack(Json& command, Clock::time_point now = Clock::now()) {
    auto* fields = std::get_if<Json::Object>(&command.value);
    if (!fields) return false;
    fields->erase(workerMarker); fields->erase(workerSeq);
    const auto type = command.get("type").string();
    if (!secure::inputTypeAllowed(type)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tracks_) return false;
    const bool secure = secure_.routing && secure_.send && secure_.routing();
    const bool injector = wanted_ && helper_ && !broken_ && helper_->alive();
    if (!secure && !injector) { if (wanted_) ++counts_.fellBack; return false; }
    Json marker; marker.value = true;
    if (type == "input.reset") {
      // Every side lets go of what it holds: the helper, the injector, and (unmarked) a worker that injects itself.
      const auto text = jsonObject(command);
      if (secure && secure_.reset && secure_.reset(text)) ++counts_.secureRouted;
      if (!injector) return false;
      if (!helper_->send(text)) { broken_ = true; ++counts_.writeFailed; return false; }
      ++counts_.routed;
      (*fields)[workerMarker] = std::move(marker);
      return true;
    }
    // Placements the worker never answered are not waited for.
    while (!pending_.empty() && (pending_.size() >= pendingLimit || now - pending_.front().at >= pendingAge)) { pending_.pop_front(); ++counts_.trackDropped; }
    if (++seq_ == 0 || seq_ > 0x7fffffffu) seq_ = 1;
    pending_.push_back({seq_, command, now});
    ++counts_.trackAsked;
    Json seq; seq.value = double(seq_);
    (*fields)[workerMarker] = std::move(marker);
    (*fields)[workerSeq] = std::move(seq);
    return true;
  }

  // Relay thread, at the end of the relay: ends the injector (it releases what
  // it holds first) and returns the service's counts.
  ServiceCounts close() {
    std::shared_ptr<secure::ServiceHelper> old;
    ServiceCounts counts;
    { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; old = std::move(helper_); helper_.reset();
      counts_.trackDropped += uint32_t(pending_.size()); pending_.clear(); counts = counts_; counts_ = {}; }
    if (old) old->close();
    return counts;
  }

  bool held() { std::lock_guard<std::mutex> lock(mutex_); return bool(helper_); }
  // A stream that never had an injector can still have counts: its inputs went to secure-desktop helpers.
  bool counted() { std::lock_guard<std::mutex> lock(mutex_); return counts_.trackAsked || counts_.secureRouted || counts_.trackDropped; }
  bool routing() { std::lock_guard<std::mutex> lock(mutex_); return helper_ && !broken_ && helper_->alive(); }
  size_t waiting() { std::lock_guard<std::mutex> lock(mutex_); return pending_.size(); }

 private:
  struct Pending { uint32_t seq; Json command; Clock::time_point at; };
  // The worker placed (or refused) the input with this sequence number. Only the
  // next one expected counts: older waiting inputs were skipped by the worker and
  // are dropped; an answer for none that waits is ignored.
  void placed(const Json& value) {
    double seq = 0;
    if (!secure::detail::wholeNumber(value.get("seq"), 1, 2147483647.0, seq)) return;
    const auto* ok = std::get_if<bool>(&value.get("ok").value);
    std::optional<std::pair<int32_t, int32_t>> desktop;
    double px = 0, py = 0;
    if (secure::detail::wholeNumber(value.get(placedX), -131072, 131072, px) && secure::detail::wholeNumber(value.get(placedY), -131072, 131072, py)) desktop = std::pair<int32_t, int32_t>{int32_t(px), int32_t(py)};
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pending_.empty() && pending_.front().seq < uint32_t(seq)) { pending_.pop_front(); ++counts_.trackDropped; }
    if (pending_.empty() || pending_.front().seq != uint32_t(seq)) return;
    const Json client = std::move(pending_.front().command);
    pending_.pop_front();
    const auto message = ok && *ok ? trackInjectorMessage(client, desktop) : std::nullopt;
    if (!message || message->size() > secure::ServiceSecure::inputLimit) { ++counts_.trackDropped; return; }
    // The secure desktop shows: the helper of the output the pixel is on, or nothing.
    if (secure_.routing && secure_.send && secure_.routing()) {
      if (secure_.send(*message, desktop ? desktop->first : 0, desktop ? desktop->second : 0, desktop.has_value())) ++counts_.secureRouted;
      else ++counts_.trackDropped;
      return;
    }
    if (!helper_ || broken_ || !helper_->alive()) { ++counts_.trackDropped; return; }
    if (!helper_->send(*message)) { broken_ = true; ++counts_.writeFailed; ++counts_.trackDropped; return; }
    ++counts_.routed;
  }

  std::mutex mutex_;
  std::shared_ptr<secure::ServiceHelper> helper_;
  std::string area_;
  std::deque<Pending> pending_;
  SecureSink secure_;
  ServiceCounts counts_;
  uint32_t seq_ = 0;
  bool broken_ = false, closed_ = false, wanted_ = false, tracks_ = false;
};
}  // namespace host::sysinput
