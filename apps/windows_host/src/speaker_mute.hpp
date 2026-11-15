#pragma once
// speaker-mute-v1: a physical PC's own speakers are silent while it is streamed.
// The rules, without Windows audio objects (speaker_mute_native.hpp has those;
// speaker_mute_test.cpp checks these with substituted endpoints and records).
//  - Grant: the Host advertises "speakerMuteProtocol":"speaker-mute-v1" and
//    cannot tell a physical PC from a VM itself. The broker can (desktop kind
//    'pc'), so it adds the marker to that desktop's session.start. A VM's
//    session never carries it and nothing here runs for it.
//  - Capture first: such a stream's sound is captured with process loopback
//    (audio_capture_control.hpp), which does not need the endpoint audible.
//    The endpoint's own loopback can go silent with a muted endpoint on some
//    drivers, so the endpoint is muted only while the stream's running capture
//    is the process-loopback one. Where that does not start (before Windows 10
//    2004), nothing is muted, the capture is the endpoint's as before and the
//    journal says why (event `unavailable`).
//  - Mute: IAudioEndpointVolume::SetMute on the default render endpoint
//    (eRender, eConsole). Its volume level is never touched. An endpoint that
//    is already muted is the user's: left alone and not put in the record.
//    While the session lasts the worker looks every speakerMuteTick: an
//    endpoint the user unmuted is muted again, and after a default device
//    change the old endpoint gets its sound back and the new one is muted.
//    A stream that restarts inside its session (a resize) keeps the mute.
//  - Record: before an endpoint is muted, its id is written to the record
//    (speaker-mute.state in the signed-in user's local application data, the
//    link-hardened file of the display-scale record) with the worker that
//    holds it. The worker runs as the signed-in user, so it has nothing in
//    the Host's SYSTEM-only folder; a record there would need the service.
//  - Restore: the session's end (the worker's own tail, before it reports
//    the stop), a capture that became the endpoint's loopback, and every
//    later worker start (a stream or the service's display probe, which runs
//    after the service starts, after a sign-in or unlock, after every stream
//    and about every minute at rest): a record whose worker is gone gets its
//    endpoints unmuted. An endpoint that is not there then (unplugged, a
//    monitor's sound while that monitor is off) stays in the record until a
//    worker start finds it again. A record a running worker holds is left.
//  - Journal: fixed numbers only, no device name or id. The worker sends
//    stage 49 of its fixed record (worker.stream.failure); a probe worker
//    adds what its start restored to its reply. The service writes hostAction
//    298 (speakerMuteJournal below).
#include "support.hpp"
#include "audio_capture_control.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace host {
inline constexpr const char* speakerMuteProtocol = "speaker-mute-v1";
// A stream command (session.start, or a resize's restart, which keeps its fields) whose broker granted it.
inline bool speakerMuteSessionRequested(const Json& start) { return start.get("mode").string() == "stream" && start.get("speakerMuteProtocol").string() == speakerMuteProtocol; }

// Worker record stage (hostAction 200 stage 249 before the service turns it into 298).
inline constexpr uint32_t speakerMuteRecordStage = 49;
inline constexpr uint32_t speakerMuteAction = 298;
// How often the session looks at the default endpoint while it holds the mute.
inline constexpr std::chrono::milliseconds speakerMuteTick{500};

// hostAction 298 `stage`: what happened.
//  muted        Multitap muted the default render endpoint.
//  restored     an endpoint Multitap had muted has its sound back.
//  failed       a step failed (ok false): `cause` is the SpeakerMuteStep, `error` its HRESULT.
//  unavailable  process loopback did not open, so nothing is muted (ok false): `cause` is the
//               AudioCaptureStep, `error` its HRESULT.
//  kept         the endpoint was already muted (the user's): left alone.
//  pending      endpoints Multitap muted are not there now; `items` of them stay in the record.
enum class SpeakerMuteEvent : uint32_t { muted = 1, restored = 2, failed = 3, unavailable = 4, kept = 5, pending = 6 };
// hostAction 298 `cause` of muted / restored / kept / pending: why.
enum class SpeakerMuteCause : uint32_t {
  none = 0,
  streamStarted = 1,   // the session's process-loopback capture runs
  userUnmuted = 2,     // the endpoint was unmuted during the session: muted again
  deviceChanged = 3,   // the default endpoint changed: the old one restored, the new one muted
  sessionEnded = 4,    // the worker's session ended
  captureChanged = 5,  // the session's capture is the endpoint's loopback now
  leftBehind = 6,      // an earlier worker left it (crash, power loss, kill): restored at a worker start
};
// hostAction 298 `cause` of failed: the step.
enum class SpeakerMuteStep : uint32_t {
  none = 0, recordUnreadable = 1, recordNotWritten = 2, recordFull = 3, endpoint = 4, state = 5, mute = 6, restore = 7, unexpected = 8,
};

// The 32-bit worker record value (error of stage 49). Bits 0-2: the event, bits 3-6: cause or step.
//  failed, unavailable: bit 7 the HRESULT's X bit (bit 27), bits 8-15 its facility (low 8 bits), bits 16-31
//                       its code; a failure that is no HRESULT carries 0.
//  pending:             bits 8-15 how many endpoints stay in the record.
inline uint32_t speakerMuteValue(SpeakerMuteEvent event, uint32_t cause, int32_t hresult = 0) {
  const uint32_t value = uint32_t(hresult);
  const uint32_t failure = value & 0x80000000u ? value : 0u;
  return uint32_t(event) | (cause & 15u) << 3 | ((failure >> 27) & 1u) << 7 | ((failure >> 16) & 0xffu) << 8 | (failure & 0xffffu) << 16;
}
inline uint32_t speakerMuteValue(SpeakerMuteEvent event, SpeakerMuteCause cause) { return speakerMuteValue(event, uint32_t(cause)); }
inline uint32_t speakerMuteFailed(SpeakerMuteStep step, int32_t hresult = 0) { return speakerMuteValue(SpeakerMuteEvent::failed, uint32_t(step), hresult); }
inline uint32_t speakerMuteUnavailable(AudioCaptureStep step, int32_t hresult) { return speakerMuteValue(SpeakerMuteEvent::unavailable, uint32_t(step), hresult); }
inline uint32_t speakerMutePending(SpeakerMuteCause cause, size_t count) {
  return uint32_t(SpeakerMuteEvent::pending) | (uint32_t(cause) & 15u) << 3 | uint32_t(std::min<size_t>(count, 255)) << 8;
}

// What the service writes for a stage 49 value (hostAction 298): stage = event, cause, and
//  failed / unavailable: error = the HRESULT (0 for none), ok false; pending: items = endpoints kept in the record.
struct SpeakerMuteJournal {
  uint32_t stage = 0, cause = 0, items = 0;
  int64_t error = 0;
  bool ok = true, valid = false;
};
inline SpeakerMuteJournal speakerMuteJournal(uint32_t value) {
  SpeakerMuteJournal row; row.stage = value & 7u; row.cause = (value >> 3) & 15u;
  switch (SpeakerMuteEvent(row.stage)) {
    case SpeakerMuteEvent::muted: case SpeakerMuteEvent::restored: case SpeakerMuteEvent::kept: row.valid = row.cause != 0 && !(value >> 7); break;
    case SpeakerMuteEvent::pending: row.items = (value >> 8) & 0xffu; row.valid = row.cause != 0 && row.items != 0 && !((value >> 7) & 1u) && !(value >> 16); break;
    case SpeakerMuteEvent::failed: case SpeakerMuteEvent::unavailable: {
      const uint32_t facility = (value >> 8) & 0xffu, code = value >> 16, x = (value >> 7) & 1u;
      row.error = facility || code || x ? int64_t(0x80000000u | (x << 27) | (facility << 16) | code) : 0;
      row.ok = false; row.valid = row.cause != 0;
      break;
    }
    default: break;
  }
  return row;
}
// A stage 49 record is a failure when a step failed or nothing could be muted; the rest are observations.
inline bool speakerMuteRecordOk(uint32_t value) { const auto event = SpeakerMuteEvent(value & 7u); return event != SpeakerMuteEvent::failed && event != SpeakerMuteEvent::unavailable; }
// A probe worker has no session to report under: what its start restored rides on its reply as
// `,"speakerMute":[value,...]` (nothing when there is none), and the service journals each valid value.
inline constexpr size_t speakerMuteProbeMax = 16;
inline std::string speakerMuteProbeField(const std::vector<uint32_t>& values) {
  if (values.empty()) return {};
  std::string text = ",\"speakerMute\":[";
  for (size_t i = 0; i < values.size() && i < speakerMuteProbeMax; ++i) text += (i ? "," : "") + std::to_string(values[i]);
  return text + ']';
}
inline std::vector<uint32_t> speakerMuteProbeRecords(const Json& reply) {
  std::vector<uint32_t> values;
  const auto* list = std::get_if<Json::Array>(&reply.get("speakerMute").value);
  if (!list || list->size() > speakerMuteProbeMax) return values;
  for (const auto& item : *list) {
    const auto* number = std::get_if<double>(&item.value);
    if (!number || !(*number >= 1 && *number <= 4294967295.0) || *number != std::floor(*number) || !speakerMuteJournal(uint32_t(*number)).valid) return {};
    values.push_back(uint32_t(*number));
  }
  return values;
}

// --- The record ------------------------------------------------------------------
// Who holds the mute: a worker process, by its id and creation time (a FILETIME, so a reused id is another
// process). None: nobody holds it, the endpoints are leftovers that were not there to be restored.
struct SpeakerMuteOwner {
  uint32_t pid = 0; uint64_t started = 0;
  bool none() const { return !pid; }
  friend bool operator==(const SpeakerMuteOwner& a, const SpeakerMuteOwner& b) { return a.pid == b.pid && a.started == b.started; }
};
// The endpoints (MMDevice endpoint ids) Multitap muted and has not given their sound back.
struct SpeakerMuteRecord { SpeakerMuteOwner owner; std::vector<std::wstring> endpoints; };
inline constexpr size_t speakerMuteMaxEndpoints = 8;
inline constexpr size_t speakerMuteMaxId = 256;
inline bool speakerMuteEndpointId(const std::wstring& id) {
  return !id.empty() && id.size() <= speakerMuteMaxId && std::none_of(id.begin(), id.end(), [](wchar_t c) { return c < 0x20; });
}
// Endpoint ids are compared without case ({0.0.0.00000000}.{guid}: ASCII).
inline bool sameSpeakerEndpoint(const std::wstring& a, const std::wstring& b) {
  const auto fold = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c; };
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](wchar_t x, wchar_t y) { return fold(x) == fold(y); });
}
inline std::string speakerMuteRecordText(const SpeakerMuteRecord& record) {
  std::string text = "{\"v\":1,\"pid\":" + std::to_string(record.owner.pid) + ",\"started\":\"" + std::to_string(record.owner.started) + "\",\"endpoints\":[";
  for (size_t i = 0; i < record.endpoints.size(); ++i) text += (i ? "," : "") + jsonQuote(utf8(record.endpoints[i]));
  return text + "]}";
}
inline std::optional<SpeakerMuteRecord> parseSpeakerMuteRecord(const std::string& text) {
  try {
    if (text.empty() || text.size() > 8192) return {};
    const auto json = JsonParser::parse(text);
    const auto* fields = std::get_if<Json::Object>(&json.value);
    if (!fields || fields->size() != 4 || json.get("v").number(0) != 1) return {};
    const double pid = json.get("pid").number(-1);
    if (!(pid >= 0 && pid <= 4294967295.0) || pid != std::floor(pid)) return {};
    const auto started = json.get("started").string();
    if (started.empty() || started.size() > 20 || started.find_first_not_of("0123456789") != std::string::npos) return {};
    SpeakerMuteRecord record; record.owner.pid = uint32_t(pid);
    for (const char digit : started) {
      const uint64_t next = uint64_t(digit - '0');
      if (record.owner.started > (UINT64_MAX - next) / 10) return {};
      record.owner.started = record.owner.started * 10 + next;
    }
    if (record.owner.none() != (record.owner.started == 0)) return {};
    const auto* list = std::get_if<Json::Array>(&json.get("endpoints").value);
    if (!list || list->size() > speakerMuteMaxEndpoints) return {};
    for (const auto& item : *list) {
      const auto* id = std::get_if<std::string>(&item.value);
      if (!id) return {};
      auto endpoint = wide(*id);
      if (!speakerMuteEndpointId(endpoint) || std::any_of(record.endpoints.begin(), record.endpoints.end(), [&](const auto& known) { return sameSpeakerEndpoint(known, endpoint); })) return {};
      record.endpoints.push_back(std::move(endpoint));
    }
    return record;
  } catch (...) { return {}; }
}

// --- The rules -------------------------------------------------------------------
struct SpeakerMuteAccess {
  // The default render endpoint (eRender, eConsole): its id, "" when Windows has none, nullopt when it
  // could not be asked.
  std::function<std::optional<std::wstring>()> defaultEndpoint;
  // Whether that endpoint is muted; nullopt when it is not there now (unplugged, disabled, removed) or
  // could not be asked.
  std::function<std::optional<bool>(const std::wstring&)> muted;
  std::function<bool(const std::wstring&, bool)> setMute;
  std::function<int32_t()> lastError;                 // the HRESULT of the last call above that failed (optional)
  std::function<std::optional<std::string>()> load;   // "" no record; nullopt unreadable
  std::function<bool(const std::string&)> save;       // "" removes the record
  std::function<SpeakerMuteOwner()> self;             // this process
  std::function<bool(const SpeakerMuteOwner&)> alive; // that process still runs (true when it cannot be told)
};

// One worker's hold on the mute. Not thread-safe, and its callers take the lock every worker of the Windows
// session shares (speaker_mute_native.hpp) around each call, so two workers never act on one record at once.
class SpeakerMuteKeeper {
 public:
  using Events = std::vector<uint32_t>;
  explicit SpeakerMuteKeeper(SpeakerMuteAccess access) : access_(std::move(access)) {}

  // A worker start, before it holds anything: endpoints an earlier worker left muted get their sound back.
  // A record a running worker holds is left alone. Every worker start looks, so only the first look after
  // that worker went says what could not be done; leftovers nobody holds say only what was restored, and
  // leftovers that are still not there change nothing.
  Events recover() noexcept {
    Events events;
    try {
      if (owning_) return events;
      const auto text = access_.load();
      if (!text || text->empty()) return events;
      const auto record = parseSpeakerMuteRecord(*text);
      if (!record) { (void)access_.save(""); events.push_back(speakerMuteFailed(SpeakerMuteStep::recordUnreadable)); return events; }  // unusable: dropped
      if (held(record->owner)) return events;
      entries_ = record->endpoints;
      const bool leftovers = record->owner.none();
      const bool present = std::any_of(entries_.begin(), entries_.end(), [&](const auto& id) { return access_.muted(id).has_value(); });
      if (present || !leftovers) settle(events, SpeakerMuteCause::leftBehind);
      if (leftovers) std::erase_if(events, [](uint32_t value) { return SpeakerMuteEvent(value & 7u) != SpeakerMuteEvent::restored; });
      entries_.clear();
    } catch (...) { entries_.clear(); }
    return events;
  }

  // One look while the session's running capture is the process-loopback one. The first look takes the
  // record over (whatever an earlier worker left in it is this session's to give back from here).
  Events keep() noexcept {
    Events events;
    try {
      if (!owning_ && !claim(events)) return events;
      const auto current = access_.defaultEndpoint();
      if (!current) { failure(events, SpeakerMuteStep::endpoint, error()); return events; }
      bool clean = true;
      // Endpoints that are not the default any more get their sound back: the old default after a change,
      // or one that was away and came back. One that is not there now stays in the record.
      bool changed = false;
      for (size_t i = 0; i < entries_.size();) {
        if (sameSpeakerEndpoint(entries_[i], *current)) { ++i; continue; }
        const auto state = access_.muted(entries_[i]);
        if (!state) { ++i; continue; }
        if (*state && !access_.setMute(entries_[i], false)) { failure(events, SpeakerMuteStep::restore, error()); clean = false; ++i; continue; }
        if (*state) events.push_back(speakerMuteValue(SpeakerMuteEvent::restored, SpeakerMuteCause::deviceChanged));
        entries_.erase(entries_.begin() + ptrdiff_t(i)); changed = true;
      }
      if (changed && !save(access_.self())) { failure(events, SpeakerMuteStep::recordNotWritten, 0); clean = false; }
      if (current->empty()) { if (clean) lastFailure_ = 0; return events; }  // Windows has no render endpoint now
      const auto state = access_.muted(*current);
      if (!state) { failure(events, SpeakerMuteStep::state, error()); return events; }
      const bool ours = contains(*current);
      const auto cause = first_ ? SpeakerMuteCause::streamStarted : sameSpeakerEndpoint(seen_, *current) ? SpeakerMuteCause::userUnmuted : SpeakerMuteCause::deviceChanged;
      if (*state) {
        // Muted already. Ours: nothing to do. Not ours: the user's, left alone and said once.
        if (!ours && !sameSpeakerEndpoint(noted_, *current)) { noted_ = *current; events.push_back(speakerMuteValue(SpeakerMuteEvent::kept, cause == SpeakerMuteCause::userUnmuted ? SpeakerMuteCause::deviceChanged : cause)); }
        first_ = false; seen_ = *current;
        if (clean) lastFailure_ = 0;
        return events;
      }
      // Audible: into the record first, then muted.
      if (!ours) {
        if (entries_.size() >= speakerMuteMaxEndpoints) {
          // Full of endpoints that never came back: the oldest gives way (its mute stays as it is) and the journal says so.
          entries_.erase(entries_.begin()); failure(events, SpeakerMuteStep::recordFull, 0); clean = false;
        }
        entries_.push_back(*current);
        if (!save(access_.self())) { entries_.pop_back(); failure(events, SpeakerMuteStep::recordNotWritten, 0); return events; }
      }
      if (!access_.setMute(*current, true)) {
        const auto refused = error();
        if (!ours) { entries_.pop_back(); (void)save(access_.self()); }
        failure(events, SpeakerMuteStep::mute, refused); return events;
      }
      noted_.clear(); first_ = false; seen_ = *current;
      events.push_back(speakerMuteValue(SpeakerMuteEvent::muted, cause));
      if (clean) lastFailure_ = 0;
    } catch (...) { failure(events, SpeakerMuteStep::unexpected, 0); }
    return events;
  }

  // The session ended, or its capture is the endpoint's loopback now: what this worker holds gets its sound
  // back. When another running worker took the record over meanwhile, the endpoints are that worker's.
  Events release(SpeakerMuteCause cause) noexcept {
    Events events;
    try {
      if (!owning_) return events;
      owning_ = false; lastFailure_ = 0; noted_.clear(); seen_.clear();
      const auto text = access_.load();
      if (text && !text->empty()) if (const auto record = parseSpeakerMuteRecord(*text)) {
        if (held(record->owner)) { entries_.clear(); return events; }
        for (const auto& id : record->endpoints) if (!contains(id) && entries_.size() < speakerMuteMaxEndpoints) entries_.push_back(id);
      }
      settle(events, cause);
      entries_.clear();
    } catch (...) { entries_.clear(); }
    return events;
  }

  // keep() ran since the last release: this worker answers for the record.
  bool holding() const { return owning_; }
  // Endpoints in the record now (muted by Multitap, or waiting to be found again).
  size_t recorded() const { return entries_.size(); }

 private:
  SpeakerMuteAccess access_;
  std::vector<std::wstring> entries_;
  bool owning_ = false, first_ = true;
  std::wstring seen_, noted_;  // the default endpoint of the last look; the user-muted one already said
  uint32_t lastFailure_ = 0;   // a failing step is said once until a look goes through

  int32_t error() const { return access_.lastError ? access_.lastError() : 0; }
  bool contains(const std::wstring& id) const { return std::any_of(entries_.begin(), entries_.end(), [&](const auto& known) { return sameSpeakerEndpoint(known, id); }); }
  // Another worker that still runs holds the record.
  bool held(const SpeakerMuteOwner& owner) const { return !owner.none() && !(owner == access_.self()) && access_.alive(owner); }
  bool save(const SpeakerMuteOwner& owner) { return access_.save(entries_.empty() ? std::string() : speakerMuteRecordText({owner, entries_})); }
  void failure(Events& events, SpeakerMuteStep step, int32_t hresult) {
    const auto value = speakerMuteFailed(step, hresult);
    if (value != lastFailure_) { lastFailure_ = value; events.push_back(value); }
  }
  bool claim(Events& events) {
    const auto text = access_.load();
    if (!text) { failure(events, SpeakerMuteStep::recordUnreadable, 0); return false; }  // no record, so nothing is muted
    entries_.clear();
    if (!text->empty()) {
      if (const auto record = parseSpeakerMuteRecord(*text)) entries_ = record->endpoints;
      if (!save(access_.self())) { entries_.clear(); failure(events, SpeakerMuteStep::recordNotWritten, 0); return false; }
    }
    owning_ = true; first_ = true; seen_.clear(); noted_.clear();
    return true;
  }
  // Every recorded endpoint that is there gets its sound back and leaves the record; the rest stay in it
  // with nobody holding them.
  void settle(Events& events, SpeakerMuteCause cause) {
    for (size_t i = 0; i < entries_.size();) {
      const auto state = access_.muted(entries_[i]);
      if (!state) { ++i; continue; }
      if (*state) {
        if (!access_.setMute(entries_[i], false)) { events.push_back(speakerMuteFailed(SpeakerMuteStep::restore, error())); ++i; continue; }
        events.push_back(speakerMuteValue(SpeakerMuteEvent::restored, cause));
      }
      entries_.erase(entries_.begin() + ptrdiff_t(i));
    }
    if (!entries_.empty()) events.push_back(speakerMutePending(cause, entries_.size()));
    if (!save(SpeakerMuteOwner{})) events.push_back(speakerMuteFailed(SpeakerMuteStep::recordNotWritten));
  }
};

// The journal: the first `burst` events of a session, then one per `spacing` (an endpoint that keeps being
// unmuted, or a step that keeps failing differently, must not fill it). What a release restores always goes.
class SpeakerMuteJournalLimit {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr uint32_t burst = 16;
  static constexpr std::chrono::seconds spacing{60};
  bool pass(Clock::time_point now) {
    if (passed_ < burst) { ++passed_; last_ = now; return true; }
    if (now - last_ >= spacing) { last_ = now; return true; }
    return false;
  }
 private:
  Clock::time_point last_{};
  uint32_t passed_ = 0;
};

// One worker's session: the capture says which loopback runs, a thread of its own keeps the mute while it is
// the process-loopback one, and end() gives everything back. Thread-safe.
class SpeakerMuteSession {
 public:
  using Record = std::function<void(uint32_t)>;
  using Lock = std::function<std::shared_ptr<void>()>;
  // `access` is called once on the session's thread (a COM thread of its own); `record` gets each stage 49
  // value; `lock` (optional) returns the hold on the lock every worker of the Windows session shares.
  SpeakerMuteSession(std::function<SpeakerMuteAccess()> access, Record record, Lock lock = {}, std::chrono::milliseconds tick = speakerMuteTick)
      : access_(std::move(access)), record_(std::move(record)), lock_(std::move(lock)), tick_(tick) {}
  ~SpeakerMuteSession() { end(); }
  SpeakerMuteSession(const SpeakerMuteSession&) = delete; SpeakerMuteSession& operator=(const SpeakerMuteSession&) = delete;

  // The stream's capture started; `process`: it is the process-loopback one. That one holds the mute until
  // the session ends, across the stream's own restarts; the endpoint's loopback gives it back at once.
  void captureStarted(bool process) {
    std::lock_guard lock(mutex_);
    if (ended_) return;
    hold_ = process; changed_ = true;
    if (process && !thread_.joinable()) thread_ = std::thread([this] { run(); });
    wake_.notify_all();
  }
  // Process loopback did not open: nothing is muted for this capture.
  void captureUnavailable(AudioCaptureStep step, int32_t hresult) { emit(speakerMuteUnavailable(step, hresult), true); }
  // The hooks a Loopback takes (they call this session, which must outlive that Loopback).
  AudioProcessCapture hooks() {
    return {[this](bool process) { captureStarted(process); }, [this](AudioCaptureStep step, int32_t hresult) { captureUnavailable(step, hresult); }};
  }
  // The session ended: the thread stops and what it holds is restored before this returns. Nothing is
  // muted after it.
  void end() noexcept {
    try {
      std::thread thread;
      { std::lock_guard lock(mutex_); if (ended_) return; ended_ = true; stop_ = true; hold_ = false; thread = std::move(thread_); }
      wake_.notify_all();
      if (thread.joinable()) thread.join();
      if (keeper_ && keeper_->holding()) pass([&] { return keeper_->release(SpeakerMuteCause::sessionEnded); }, false);
    } catch (...) {}
  }

 private:
  std::function<SpeakerMuteAccess()> access_; Record record_; Lock lock_; std::chrono::milliseconds tick_;
  std::mutex mutex_, journal_; std::condition_variable wake_; std::thread thread_;
  bool hold_ = false, changed_ = false, stop_ = false, ended_ = false;
  std::optional<SpeakerMuteKeeper> keeper_;  // the session's thread, then end() after that thread is gone
  SpeakerMuteJournalLimit limit_;

  void emit(uint32_t value, bool limited) noexcept {
    try {
      if (!record_) return;
      { std::lock_guard lock(journal_); if (limited && !limit_.pass(SpeakerMuteJournalLimit::Clock::now())) return; }
      record_(value);
    } catch (...) {}
  }
  template<class Operation> void pass(Operation&& operation, bool limited) {
    const auto held = lock_ ? lock_() : std::shared_ptr<void>();
    for (const auto value : operation()) emit(value, limited);
  }
  void run() noexcept {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_lock lock(mutex_);
    while (!stop_) {
      const bool hold = hold_; changed_ = false;
      lock.unlock();
      try {
        if (!keeper_) keeper_.emplace(access_());
        if (hold) pass([&] { return keeper_->keep(); }, true);
        else if (keeper_->holding()) pass([&] { return keeper_->release(SpeakerMuteCause::captureChanged); }, false);
      } catch (...) { emit(speakerMuteFailed(SpeakerMuteStep::unexpected), true); }
      lock.lock();
      if (stop_) break;
      if (hold) wake_.wait_for(lock, tick_, [&] { return stop_ || changed_; });
      else wake_.wait(lock, [&] { return stop_ || changed_; });
    }
    lock.unlock();
    if (SUCCEEDED(com)) CoUninitialize();
  }
};
}  // namespace host
