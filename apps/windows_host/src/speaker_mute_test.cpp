// speaker-mute-v1 (speaker_mute.hpp): a physical PC's speakers while it is
// streamed. The grant, the fixed journal record and its hostAction 298 reading,
// the record file's text, and the rules with substituted endpoints, record and
// processes: mute only after the record, leave the user's own mute alone, mute
// again after the user unmutes, follow a default device change, give
// everything back at the session's end, and recover what a crashed worker
// left. Then the session object on a real thread with the same substitutes,
// the parts of the native side that need no audio object (who a process is,
// the lock), the worker's call sites, and one look at this machine's default
// render endpoint (read only: its mute is asked, never set). No endpoint of
// this machine is muted or changed, and the real record file is not written.
#include "speaker_mute_native.hpp"
#include "stream_prepare_diagnostics.hpp"
#include <fstream>
#include <iostream>
#include <set>

namespace {
using namespace host;
using namespace std::chrono_literals;
unsigned checks = 0;
void require(bool condition, const std::string& reason) { ++checks; if (!condition) throw std::runtime_error(reason); }

using E = SpeakerMuteEvent; using C = SpeakerMuteCause; using S = SpeakerMuteStep;
uint32_t ev(E event, C cause) { return speakerMuteValue(event, cause); }
const std::wstring speakers = L"{0.0.0.00000000}.{b3f8fa53-0004-438e-9003-51a46e139bfc}";
const std::wstring headset = L"{0.0.0.00000000}.{11111111-2222-3333-4444-555555555555}";
const std::wstring monitor = L"{0.0.0.00000000}.{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}";
const SpeakerMuteOwner first{4100, 133000000000000001ull}, second{4200, 133000000000000777ull};

// One PC: its render endpoints (present ones and whether each is muted), the default one, the record file
// and which worker processes run. Every setMute is noted with what the record held at that moment.
struct Pc {
  std::mutex mutex;
  std::map<std::wstring, bool> endpoints{{speakers, false}};
  std::optional<std::wstring> def = speakers;
  std::string record;
  std::vector<SpeakerMuteOwner> running;
  bool loadFails = false, saveFails = false, muteFails = false, restoreFails = false;
  int32_t error = 0;
  struct Call { std::wstring id; bool mute; std::string recordThen; };
  std::vector<Call> calls;
  unsigned saves = 0;

  SpeakerMuteAccess access(SpeakerMuteOwner self) {
    SpeakerMuteAccess a;
    a.defaultEndpoint = [this] { std::lock_guard lock(mutex); return def; };
    a.muted = [this](const std::wstring& id) -> std::optional<bool> { std::lock_guard lock(mutex); const auto found = endpoints.find(id); if (found == endpoints.end()) return std::nullopt; return found->second; };
    a.setMute = [this](const std::wstring& id, bool mute) {
      std::lock_guard lock(mutex); calls.push_back({id, mute, record});
      const auto found = endpoints.find(id);
      if (found == endpoints.end() || (mute ? muteFails : restoreFails)) return false;
      found->second = mute; return true;
    };
    a.lastError = [this] { std::lock_guard lock(mutex); return error; };
    a.load = [this]() -> std::optional<std::string> { std::lock_guard lock(mutex); if (loadFails) return std::nullopt; return record; };
    a.save = [this](const std::string& text) { std::lock_guard lock(mutex); if (saveFails) return false; record = text; ++saves; return true; };
    a.self = [self] { return self; };
    a.alive = [this](const SpeakerMuteOwner& owner) { std::lock_guard lock(mutex); return std::find(running.begin(), running.end(), owner) != running.end(); };
    return a;
  }
  bool muted(const std::wstring& id) { std::lock_guard lock(mutex); return endpoints.at(id); }
  std::optional<SpeakerMuteRecord> saved() { std::lock_guard lock(mutex); return record.empty() ? std::nullopt : parseSpeakerMuteRecord(record); }
  bool recordEmpty() { std::lock_guard lock(mutex); return record.empty(); }
  size_t callCount() { std::lock_guard lock(mutex); return calls.size(); }
};
bool lists(const std::optional<SpeakerMuteRecord>& record, std::initializer_list<std::wstring> ids) {
  if (!record || record->endpoints.size() != ids.size()) return false;
  size_t i = 0; for (const auto& id : ids) if (!sameSpeakerEndpoint(record->endpoints[i++], id)) return false;
  return true;
}
using Events = SpeakerMuteKeeper::Events;

void grantAndRecords() {
  // Only a stream command that carries exactly the broker's marker.
  const auto command = [](const std::string& text) { return JsonParser::parse(text); };
  require(speakerMuteSessionRequested(command("{\"type\":\"session.start\",\"mode\":\"stream\",\"speakerMuteProtocol\":\"speaker-mute-v1\"}")), "a granted session.start");
  require(speakerMuteSessionRequested(command("{\"type\":\"session.start\",\"mode\":\"stream\",\"mediaProtocol\":\"tracks-v1\",\"speakerMuteProtocol\":\"speaker-mute-v1\"}")), "independent outputs too");
  for (const auto* text : {"{\"type\":\"session.start\",\"mode\":\"stream\"}", "{\"type\":\"session.start\",\"mode\":\"stream\",\"speakerMuteProtocol\":\"speaker-mute-v2\"}",
                           "{\"type\":\"session.start\",\"mode\":\"stream\",\"speakerMuteProtocol\":true}", "{\"type\":\"session.start\",\"mode\":\"rdp\",\"speakerMuteProtocol\":\"speaker-mute-v1\"}",
                           "{\"type\":\"session.start\",\"speakerMuteProtocol\":\"speaker-mute-v1\"}"})
    require(!speakerMuteSessionRequested(command(text)), std::string("not granted: ") + text);
  // The record value and the service's reading of it.
  for (const auto cause : {C::streamStarted, C::userUnmuted, C::deviceChanged}) {
    const auto row = speakerMuteJournal(ev(E::muted, cause));
    require(row.valid && row.ok && row.stage == 1 && row.cause == uint32_t(cause) && row.error == 0 && row.items == 0, "muted row");
  }
  for (const auto cause : {C::deviceChanged, C::sessionEnded, C::captureChanged, C::leftBehind}) {
    const auto row = speakerMuteJournal(ev(E::restored, cause));
    require(row.valid && row.ok && row.stage == 2 && row.cause == uint32_t(cause), "restored row");
  }
  auto row = speakerMuteJournal(ev(E::kept, C::streamStarted));
  require(row.valid && row.ok && row.stage == 5 && row.cause == 1, "kept row");
  row = speakerMuteJournal(speakerMutePending(C::sessionEnded, 3));
  require(row.valid && row.ok && row.stage == 6 && row.cause == 4 && row.items == 3, "pending row");
  require(speakerMuteJournal(speakerMutePending(C::leftBehind, 999)).items == 255 && !speakerMuteJournal(speakerMutePending(C::leftBehind, 0)).valid, "pending count bounds");
  for (const uint32_t hr : {0x88890004u /* AUDCLNT_E_DEVICE_INVALIDATED */, 0x80070005u /* E_ACCESSDENIED */, 0x80070490u /* E_NOTFOUND */, 0x800705B4u /* timeout */, 0x80004002u /* E_NOINTERFACE */}) {
    for (const auto step : {S::recordNotWritten, S::endpoint, S::state, S::mute, S::restore}) {
      row = speakerMuteJournal(speakerMuteFailed(step, int32_t(hr)));
      require(row.valid && !row.ok && row.stage == 3 && row.cause == uint32_t(step) && uint32_t(row.error) == hr, "failed HRESULT round trip");
    }
    row = speakerMuteJournal(speakerMuteUnavailable(AudioCaptureStep::activate, int32_t(hr)));
    require(row.valid && !row.ok && row.stage == 4 && row.cause == uint32_t(AudioCaptureStep::activate) && uint32_t(row.error) == hr, "unavailable HRESULT round trip");
  }
  row = speakerMuteJournal(speakerMuteFailed(S::recordUnreadable));
  require(row.valid && !row.ok && row.cause == 1 && row.error == 0, "a failure without an HRESULT");
  require(speakerMuteJournal(speakerMuteUnavailable(AudioCaptureStep::event, int32_t(0x80070006u))).cause == uint32_t(AudioCaptureStep::event), "every capture step fits the cause");
  require(!speakerMuteJournal(0).valid && !speakerMuteJournal(7).valid && !speakerMuteJournal(uint32_t(E::muted)).valid, "unknown events and a cause of none are no record");
  // The worker's packet carries it; only a failed step and unavailable process loopback are failures.
  const std::string session = "00000000-0000-0000-0000-000000000049";
  const uint32_t failure = speakerMuteFailed(S::mute, int32_t(0x80070005u));
  const auto parsed = streamPreparationObservation(JsonParser::parse(streamPreparationPacket(session, speakerMuteRecordStage, failure)), session);
  require(parsed && parsed->first == 49 && parsed->second == failure, "stage 49 packet");
  require(!streamObservationOk(49, failure) && !streamObservationOk(49, speakerMuteUnavailable(AudioCaptureStep::activate, int32_t(0x80070490u))) &&
          streamObservationOk(49, ev(E::muted, C::streamStarted)) && streamObservationOk(49, ev(E::restored, C::sessionEnded)) && streamObservationOk(49, ev(E::kept, C::streamStarted)) &&
          streamObservationOk(49, speakerMutePending(C::sessionEnded, 1)), "stage 49 ok");
  require(!streamObservationNoHardwareCapture(49, ev(E::muted, C::streamStarted)), "no render re-arm for the speakers");
  require(speakerMuteRecordStage != audioCaptureRecordStage && speakerMuteAction == 298 && audioCaptureAction == 297, "its own stage and action");
  // A probe worker's reply: what its start restored, and nothing malformed.
  require(speakerMuteProbeField({}).empty(), "no field without a record");
  const std::vector<uint32_t> restored{ev(E::restored, C::leftBehind), speakerMutePending(C::leftBehind, 1)};
  const auto reply = JsonParser::parse("{\"type\":\"worker.display\"" + speakerMuteProbeField(restored) + "}");
  require(speakerMuteProbeRecords(reply) == restored, "probe field round trip");
  require(speakerMuteProbeRecords(JsonParser::parse("{\"type\":\"worker.display\"}")).empty(), "a reply without the field");
  for (const auto* text : {"{\"speakerMute\":[0]}", "{\"speakerMute\":[7]}", "{\"speakerMute\":[1.5]}", "{\"speakerMute\":[\"18\"]}", "{\"speakerMute\":18}", "{\"speakerMute\":[18,-1]}", "{\"speakerMute\":[18,4294967296]}"})
    require(speakerMuteProbeRecords(JsonParser::parse(text)).empty(), std::string("a malformed probe field: ") + text);
  std::string many = "{\"speakerMute\":["; for (int i = 0; i < 17; ++i) many += std::string(i ? "," : "") + std::to_string(ev(E::restored, C::leftBehind));
  require(speakerMuteProbeRecords(JsonParser::parse(many + "]}")).empty(), "more values than a start can produce");
}

void recordText() {
  SpeakerMuteRecord record{first, {speakers, headset}};
  const auto text = speakerMuteRecordText(record);
  const auto parsed = parseSpeakerMuteRecord(text);
  require(parsed && parsed->owner == first && lists(parsed, {speakers, headset}), "record round trip (a FILETIME keeps every digit)");
  const auto leftovers = parseSpeakerMuteRecord(speakerMuteRecordText({SpeakerMuteOwner{}, {monitor}}));
  require(leftovers && leftovers->owner.none() && lists(leftovers, {monitor}), "a record nobody holds");
  require(text.find("volume") == std::string::npos && text.find("name") == std::string::npos, "ids only");
  const std::string id = jsonQuote(utf8(speakers));
  for (const std::string& bad : std::vector<std::string>{
         std::string(), std::string("{}"), std::string("not json"),
         "{\"v\":2,\"pid\":1,\"started\":\"5\",\"endpoints\":[" + id + "]}",
         "{\"v\":1,\"pid\":1,\"started\":\"5\",\"endpoints\":[" + id + "],\"extra\":1}",
         std::string("{\"v\":1,\"pid\":-1,\"started\":\"5\",\"endpoints\":[]}"), "{\"v\":1,\"pid\":1.5,\"started\":\"5\",\"endpoints\":[]}", "{\"v\":1,\"pid\":4294967296,\"started\":\"5\",\"endpoints\":[]}",
         "{\"v\":1,\"pid\":1,\"started\":5,\"endpoints\":[]}", "{\"v\":1,\"pid\":1,\"started\":\"\",\"endpoints\":[]}", "{\"v\":1,\"pid\":1,\"started\":\"5x\",\"endpoints\":[]}",
         "{\"v\":1,\"pid\":1,\"started\":\"18446744073709551616\",\"endpoints\":[]}", "{\"v\":1,\"pid\":1,\"started\":\"0\",\"endpoints\":[]}", "{\"v\":1,\"pid\":0,\"started\":\"5\",\"endpoints\":[]}",
         "{\"v\":1,\"pid\":1,\"started\":\"5\",\"endpoints\":{}}", "{\"v\":1,\"pid\":1,\"started\":\"5\",\"endpoints\":[1]}", "{\"v\":1,\"pid\":1,\"started\":\"5\",\"endpoints\":[\"\"]}",
         "{\"v\":1,\"pid\":1,\"started\":\"5\",\"endpoints\":[" + id + "," + id + "]}", "{\"v\":1,\"pid\":1,\"started\":\"5\",\"endpoints\":[\"a\\u0001b\"]}",
         "{\"v\":1,\"pid\":1,\"started\":\"5\",\"endpoints\":[" + jsonQuote(std::string(257, 'x')) + "]}"})
    require(!parseSpeakerMuteRecord(bad), "an unusable record: " + bad.substr(0, 60));
  require(parseSpeakerMuteRecord("{\"v\":1,\"pid\":1,\"started\":\"18446744073709551615\",\"endpoints\":[]}").has_value(), "the largest start time");
  std::string nine = "{\"v\":1,\"pid\":1,\"started\":\"5\",\"endpoints\":["; for (int i = 0; i < 9; ++i) nine += std::string(i ? "," : "") + "\"e" + std::to_string(i) + "\"";
  require(!parseSpeakerMuteRecord(nine + "]}"), "more endpoints than the record holds");
  require(sameSpeakerEndpoint(L"{0.0.0.00000000}.{ABCDEF}", L"{0.0.0.00000000}.{abcdef}") && !sameSpeakerEndpoint(speakers, headset), "ids without case");
}

void muteAndRestore() {
  Pc pc; pc.running = {first}; SpeakerMuteKeeper keeper(pc.access(first));
  require(keeper.recover().empty() && pc.callCount() == 0, "nothing to recover on a PC that was never muted");
  // The first look: into the record, then muted.
  require(keeper.keep() == Events{ev(E::muted, C::streamStarted)} && pc.muted(speakers) && keeper.holding(), "the default endpoint was not muted");
  require(pc.calls.size() == 1 && pc.calls[0].mute && pc.calls[0].id == speakers, "exactly one SetMute");
  const auto atMute = parseSpeakerMuteRecord(pc.calls[0].recordThen);
  require(atMute && atMute->owner == first && lists(atMute, {speakers}), "the record did not name the endpoint and this worker before the mute");
  // Looks that find it muted do nothing.
  for (int i = 0; i < 20; ++i) require(keeper.keep().empty(), "a quiet look");
  require(pc.calls.size() == 1 && pc.saves == 1, "quiet looks touch nothing");
  // The user unmutes: muted again, the record unchanged.
  pc.endpoints[speakers] = false;
  require(keeper.keep() == Events{ev(E::muted, C::userUnmuted)} && pc.muted(speakers) && pc.saves == 1, "an unmuted endpoint was not muted again");
  // The session ends: unmuted, the record gone.
  require(keeper.release(C::sessionEnded) == Events{ev(E::restored, C::sessionEnded)} && !pc.muted(speakers) && pc.record.empty() && !keeper.holding(), "the endpoint did not get its sound back");
  require(keeper.release(C::sessionEnded).empty() && pc.calls.size() == 3, "a second release changed something");
  // The next stream of another worker starts clean.
  SpeakerMuteKeeper next(pc.access(second)); pc.running = {second};
  require(next.recover().empty() && next.keep() == Events{ev(E::muted, C::streamStarted)} && pc.saved()->owner == second, "the next session");
  // The capture became the endpoint's loopback: given back at once; process loopback again: muted again.
  require(next.release(C::captureChanged) == Events{ev(E::restored, C::captureChanged)} && !pc.muted(speakers) && pc.record.empty(), "capture changed");
  require(next.keep() == Events{ev(E::muted, C::streamStarted)} && pc.muted(speakers), "held again");
  require(next.release(C::sessionEnded) == Events{ev(E::restored, C::sessionEnded)}, "released again");
}

void usersOwnMute() {
  // Muted before the stream: the user's. Not recorded, not touched, said once, still muted afterwards.
  Pc pc; pc.endpoints[speakers] = true; pc.running = {first}; SpeakerMuteKeeper keeper(pc.access(first));
  require(keeper.keep() == Events{ev(E::kept, C::streamStarted)} && pc.record.empty() && pc.calls.empty(), "the user's own mute was touched or recorded");
  for (int i = 0; i < 5; ++i) require(keeper.keep().empty(), "said once");
  require(keeper.release(C::sessionEnded).empty() && pc.muted(speakers) && pc.calls.empty() && pc.record.empty(), "the user's own mute was undone");
  // The user unmutes it during the stream: from then it is muted and recorded like any other, and comes back unmuted.
  SpeakerMuteKeeper again(pc.access(first));
  require(again.keep() == Events{ev(E::kept, C::streamStarted)}, "kept");
  pc.endpoints[speakers] = false;
  require(again.keep() == Events{ev(E::muted, C::userUnmuted)} && pc.muted(speakers) && lists(pc.saved(), {speakers}), "unmuted during the stream");
  require(again.release(C::sessionEnded) == Events{ev(E::restored, C::sessionEnded)} && !pc.muted(speakers), "back as the user last had it");
  // The user unmutes it right at the end: nothing left to restore, and no call.
  SpeakerMuteKeeper last(pc.access(first));
  require(last.keep() == Events{ev(E::muted, C::streamStarted)}, "muted");
  pc.endpoints[speakers] = false; const auto calls = pc.calls.size();
  require(last.release(C::sessionEnded).empty() && pc.calls.size() == calls && pc.record.empty(), "an endpoint that is audible already");
}

void deviceChange() {
  Pc pc; pc.endpoints[headset] = false; pc.running = {first}; SpeakerMuteKeeper keeper(pc.access(first));
  require(keeper.keep() == Events{ev(E::muted, C::streamStarted)}, "muted");
  // The default changes: the old endpoint back, the new one muted, the record names only the new one.
  pc.def = headset;
  require((keeper.keep() == Events{ev(E::restored, C::deviceChanged), ev(E::muted, C::deviceChanged)}) && !pc.muted(speakers) && pc.muted(headset) && lists(pc.saved(), {headset}), "a default device change");
  // And back.
  pc.def = speakers;
  require((keeper.keep() == Events{ev(E::restored, C::deviceChanged), ev(E::muted, C::deviceChanged)}) && pc.muted(speakers) && !pc.muted(headset) && lists(pc.saved(), {speakers}), "changed back");
  // A new default that is muted already is the user's.
  pc.endpoints[headset] = true; pc.def = headset;
  require((keeper.keep() == Events{ev(E::restored, C::deviceChanged), ev(E::kept, C::deviceChanged)}) && pc.record.empty() && pc.muted(headset), "a muted new default");
  require(keeper.keep().empty(), "said once");
  require(keeper.release(C::sessionEnded).empty() && pc.muted(headset) && !pc.muted(speakers), "nothing of ours left");

  // The muted default goes away (a headset switched off, a monitor's sound while that monitor is off).
  Pc away; away.endpoints[headset] = false; away.def = headset; away.running = {first}; SpeakerMuteKeeper holder(away.access(first));
  require(holder.keep() == Events{ev(E::muted, C::streamStarted)}, "headset muted");
  away.endpoints.erase(headset); away.def = speakers;
  require(holder.keep() == Events{ev(E::muted, C::deviceChanged)} && away.muted(speakers) && lists(away.saved(), {headset, speakers}), "the absent endpoint stays in the record");
  // It comes back during the stream as a non-default endpoint: restored then.
  away.endpoints[headset] = true;
  require(holder.keep() == Events{ev(E::restored, C::deviceChanged)} && !away.muted(headset) && lists(away.saved(), {speakers}), "restored when it came back");
  // Away again at the session's end: it waits in the record, held by nobody.
  away.def = headset; (void)holder.keep(); away.endpoints.erase(headset); away.def = speakers; (void)holder.keep();
  require((holder.release(C::sessionEnded) == Events{ev(E::restored, C::sessionEnded), speakerMutePending(C::sessionEnded, 1)}) && !away.muted(speakers), "pending at the end");
  require(away.saved() && away.saved()->owner.none() && lists(away.saved(), {headset}), "the leftover record");
  // Worker starts while it is still away change nothing and say nothing; the one that finds it restores it.
  const auto saves = away.saves;
  for (int i = 0; i < 5; ++i) { SpeakerMuteKeeper probe(away.access(second)); require(probe.recover().empty() && away.saves == saves, "a leftover that is not there"); }
  away.endpoints[headset] = true;
  { SpeakerMuteKeeper probe(away.access(second)); require(probe.recover() == Events{ev(E::restored, C::leftBehind)} && !away.muted(headset) && away.record.empty(), "the leftover found again"); }

  // Windows has no render endpoint, then gets one.
  Pc none; none.endpoints.clear(); none.def = std::wstring(); SpeakerMuteKeeper nothing(none.access(first));
  require(nothing.keep().empty() && nothing.keep().empty() && none.record.empty() && none.calls.empty(), "no endpoint: nothing to do");
  none.endpoints[speakers] = false; none.def = speakers;
  require(nothing.keep() == Events{ev(E::muted, C::streamStarted)}, "the first endpoint of the session");
  // The default cannot be asked: said once, asked again every look.
  none.def = std::nullopt; none.error = int32_t(0x800706BAu);
  require(nothing.keep() == Events{speakerMuteFailed(S::endpoint, int32_t(0x800706BAu))} && nothing.keep().empty() && none.muted(speakers), "an enumerator that fails");
  none.def = speakers;
  require(nothing.keep().empty() && nothing.release(C::sessionEnded) == Events{ev(E::restored, C::sessionEnded)}, "asked again");
}

void crashRecovery() {
  // A worker muted the speakers and is gone (crash, kill, power loss): the next worker start restores them.
  Pc pc; pc.running = {first};
  { SpeakerMuteKeeper crashed(pc.access(first)); require(crashed.keep() == Events{ev(E::muted, C::streamStarted)}, "muted"); }
  require(pc.muted(speakers) && lists(pc.saved(), {speakers}), "the mute and its record outlive the worker");
  // While that worker still runs, another worker's start leaves it alone.
  { SpeakerMuteKeeper probe(pc.access(second)); require(probe.recover().empty() && pc.muted(speakers) && !pc.record.empty(), "a running worker's mute was taken away"); }
  pc.running.clear();
  { SpeakerMuteKeeper probe(pc.access(second)); require(probe.recover() == Events{ev(E::restored, C::leftBehind)} && !pc.muted(speakers) && pc.record.empty(), "the leftover mute was not restored"); }
  { SpeakerMuteKeeper probe(pc.access(second)); require(probe.recover().empty(), "restored once"); }
  // The process id was given to another process meanwhile: its start time differs, so the worker is gone.
  { SpeakerMuteKeeper crashed(pc.access(first)); (void)crashed.keep(); }
  pc.running = {SpeakerMuteOwner{first.pid, first.started + 1}};
  { SpeakerMuteKeeper probe(pc.access(second)); require(probe.recover() == Events{ev(E::restored, C::leftBehind)} && !pc.muted(speakers), "a reused process id kept the mute"); }
  // The user unmuted it after the crash: nothing to do but drop the record.
  pc.running = {first};
  { SpeakerMuteKeeper crashed(pc.access(first)); (void)crashed.keep(); }
  pc.running.clear(); pc.endpoints[speakers] = false; const auto calls = pc.calls.size();
  { SpeakerMuteKeeper probe(pc.access(second)); require(probe.recover().empty() && pc.calls.size() == calls && pc.record.empty(), "an endpoint the user already unmuted"); }
  // The endpoint is away at the first start after the crash: said once, kept, restored when found.
  pc.running = {first};
  { SpeakerMuteKeeper crashed(pc.access(first)); (void)crashed.keep(); }
  pc.running.clear(); pc.endpoints.erase(speakers); pc.def = std::wstring();
  { SpeakerMuteKeeper probe(pc.access(second)); require(probe.recover() == Events{speakerMutePending(C::leftBehind, 1)} && pc.saved() && pc.saved()->owner.none(), "pending after a crash"); }
  { SpeakerMuteKeeper probe(pc.access(second)); require(probe.recover().empty(), "said once"); }
  pc.endpoints[speakers] = true; pc.def = speakers;
  { SpeakerMuteKeeper probe(pc.access(second)); require(probe.recover() == Events{ev(E::restored, C::leftBehind)} && !pc.muted(speakers) && pc.record.empty(), "found after a crash"); }
  // A restore Windows refuses: said at the first start after the crash, the record kept, tried at every start, silent until it works.
  pc.running = {first};
  { SpeakerMuteKeeper crashed(pc.access(first)); (void)crashed.keep(); }
  pc.running.clear(); pc.restoreFails = true; pc.error = int32_t(0x80070005u);
  { SpeakerMuteKeeper probe(pc.access(second)); require((probe.recover() == Events{speakerMuteFailed(S::restore, int32_t(0x80070005u)), speakerMutePending(C::leftBehind, 1)}) && pc.muted(speakers) && lists(pc.saved(), {speakers}), "a refused restore"); }
  { SpeakerMuteKeeper probe(pc.access(second)); require(probe.recover().empty() && pc.muted(speakers), "a refused restore is said once"); }
  pc.restoreFails = false;
  { SpeakerMuteKeeper probe(pc.access(second)); require(probe.recover() == Events{ev(E::restored, C::leftBehind)} && !pc.muted(speakers) && pc.record.empty(), "restored at a later start"); }
  // An unusable record is dropped; an unreadable one is left and nothing is touched.
  pc.record = "{\"v\":9}";
  { SpeakerMuteKeeper probe(pc.access(second)); require(probe.recover() == Events{speakerMuteFailed(S::recordUnreadable)} && pc.record.empty(), "an unusable record"); }
  pc.record = speakerMuteRecordText({SpeakerMuteOwner{}, {speakers}}); pc.endpoints[speakers] = true; pc.loadFails = true;
  { SpeakerMuteKeeper probe(pc.access(second)); require(probe.recover().empty() && pc.muted(speakers), "an unreadable record"); }
  pc.loadFails = false;
  // The next stream's first look inherits what a crashed worker left: a leftover that is not the default is restored,
  // the default it left muted is this session's from here.
  Pc inherit; inherit.endpoints[headset] = true; inherit.endpoints[speakers] = true; inherit.def = speakers;
  inherit.record = speakerMuteRecordText({first, {headset, speakers}});
  SpeakerMuteKeeper stream(inherit.access(second)); inherit.running = {second};
  require(stream.keep() == Events{ev(E::restored, C::deviceChanged)} && !inherit.muted(headset) && inherit.muted(speakers) && inherit.saved()->owner == second && lists(inherit.saved(), {speakers}), "inherited at the first look");
  require(stream.release(C::sessionEnded) == Events{ev(E::restored, C::sessionEnded)} && !inherit.muted(speakers) && inherit.record.empty(), "and given back");
}

void handover() {
  // A session's worker is still ending while the next session's worker starts on the same PC.
  Pc pc; pc.running = {first, second};
  SpeakerMuteKeeper ending(pc.access(first)), starting(pc.access(second));
  require(ending.keep() == Events{ev(E::muted, C::streamStarted)}, "first session");
  require(starting.keep().empty() && pc.saved()->owner == second && pc.muted(speakers), "the next session took the record over without a sound");
  // The ending worker leaves everything to the one that holds the record now.
  const auto calls = pc.calls.size();
  require(ending.release(C::sessionEnded).empty() && pc.calls.size() == calls && pc.muted(speakers) && pc.saved()->owner == second, "the ending worker unmuted the next session");
  require(starting.release(C::sessionEnded) == Events{ev(E::restored, C::sessionEnded)} && !pc.muted(speakers) && pc.record.empty(), "the last session gave it back");
  // The worker that took it over died before the first one ended: the first one restores.
  SpeakerMuteKeeper a(pc.access(first)), b(pc.access(second));
  (void)a.keep(); (void)b.keep(); pc.running = {first};
  require(a.release(C::sessionEnded) == Events{ev(E::restored, C::sessionEnded)} && !pc.muted(speakers) && pc.record.empty(), "nobody restored");
}

void failures() {
  // The record cannot be written: nothing is muted. Said once; muted as soon as it can be written.
  Pc pc; pc.running = {first}; pc.saveFails = true; SpeakerMuteKeeper keeper(pc.access(first));
  require(keeper.keep() == Events{speakerMuteFailed(S::recordNotWritten)} && !pc.muted(speakers) && pc.calls.empty(), "muted without a record");
  for (int i = 0; i < 5; ++i) require(keeper.keep().empty() && pc.calls.empty(), "said once");
  pc.saveFails = false;
  require(keeper.keep() == Events{ev(E::muted, C::streamStarted)} && pc.muted(speakers), "muted once the record can be written");
  require(keeper.release(C::sessionEnded) == Events{ev(E::restored, C::sessionEnded)}, "released");
  // The record cannot be read: nothing is muted.
  Pc blind; blind.loadFails = true; SpeakerMuteKeeper unread(blind.access(first));
  require(unread.keep() == Events{speakerMuteFailed(S::recordUnreadable)} && unread.keep().empty() && !blind.muted(speakers) && blind.calls.empty() && !unread.holding(), "muted without reading the record");
  // Windows refuses the mute: the endpoint leaves the record again.
  Pc refused; refused.muteFails = true; refused.error = int32_t(0x80070005u); SpeakerMuteKeeper denied(refused.access(first));
  require(denied.keep() == Events{speakerMuteFailed(S::mute, int32_t(0x80070005u))} && refused.record.empty() && !refused.muted(speakers), "a refused mute left a record");
  require(denied.keep().empty() && refused.calls.size() == 2, "said once, tried again");
  require(denied.release(C::sessionEnded).empty() && refused.record.empty(), "nothing to restore");
  // The endpoint's state cannot be asked (it went between the two questions).
  Pc gone; SpeakerMuteKeeper asked(gone.access(first)); gone.def = headset; gone.error = int32_t(0x88890004u);
  require(asked.keep() == Events{speakerMuteFailed(S::state, int32_t(0x88890004u))} && gone.calls.empty() && gone.record.empty(), "a default endpoint that is not there");
  // Windows refuses the restore at the session's end: said, and the record keeps it for the next worker start.
  Pc stuck; stuck.running = {first}; SpeakerMuteKeeper holder(stuck.access(first));
  (void)holder.keep(); stuck.restoreFails = true; stuck.error = int32_t(0x80070005u);
  require((holder.release(C::sessionEnded) == Events{speakerMuteFailed(S::restore, int32_t(0x80070005u)), speakerMutePending(C::sessionEnded, 1)}) && stuck.saved() && stuck.saved()->owner.none() && lists(stuck.saved(), {speakers}), "a refused restore lost the record");
  stuck.restoreFails = false; stuck.running.clear();
  { SpeakerMuteKeeper probe(stuck.access(second)); require(probe.recover() == Events{ev(E::restored, C::leftBehind)} && !stuck.muted(speakers), "restored by the next start"); }
  // The record is full of endpoints that never came back: the oldest gives way so the speakers can still be muted.
  Pc full; full.running = {first}; std::vector<std::wstring> old; for (int i = 0; i < int(speakerMuteMaxEndpoints); ++i) old.push_back(L"gone-" + std::to_wstring(i));
  full.record = speakerMuteRecordText({SpeakerMuteOwner{}, old}); SpeakerMuteKeeper crowded(full.access(first));
  require((crowded.keep() == Events{speakerMuteFailed(S::recordFull), ev(E::muted, C::streamStarted)}) && full.muted(speakers) && full.saved()->endpoints.size() == speakerMuteMaxEndpoints &&
          sameSpeakerEndpoint(full.saved()->endpoints.back(), speakers) && sameSpeakerEndpoint(full.saved()->endpoints.front(), L"gone-1"), "a full record");
  // The journal's limit: a burst, then one a minute.
  SpeakerMuteJournalLimit limit; SpeakerMuteJournalLimit::Clock::time_point at{}; uint32_t written = 0;
  for (int n = 0; n < 3600; ++n, at += 1s) if (limit.pass(at)) ++written;
  require(written == SpeakerMuteJournalLimit::burst + 59, "journal limit: " + std::to_string(written));
}

// The session object: its own thread, the capture's hooks, the end.
template<class Predicate> bool soon(Predicate&& predicate) {
  for (int i = 0; i < 2000; ++i) { if (predicate()) return true; std::this_thread::sleep_for(1ms); }
  return false;
}
void session() {
  Pc pc; pc.running = {first};
  std::mutex mutex; std::vector<uint32_t> journal; std::atomic<int> locks{0}, accesses{0};
  const auto seen = [&](uint32_t value) { std::lock_guard lock(mutex); return std::count(journal.begin(), journal.end(), value); };
  {
    SpeakerMuteSession muted([&] { ++accesses; return pc.access(first); }, [&](uint32_t value) { std::lock_guard lock(mutex); journal.push_back(value); },
                             [&] { ++locks; return std::shared_ptr<void>(); }, 2ms);
    const AudioProcessCapture hooks = muted.hooks();
    // Nothing happens before a capture starts, nor for the endpoint's loopback.
    std::this_thread::sleep_for(20ms); hooks.started(false); std::this_thread::sleep_for(20ms);
    require(pc.callCount() == 0 && accesses == 0 && !pc.muted(speakers), "muted without a process-loopback capture");
    hooks.unavailable(AudioCaptureStep::activate, int32_t(0x80070490u));
    require(seen(speakerMuteUnavailable(AudioCaptureStep::activate, int32_t(0x80070490u))) == 1, "why nothing is muted");
    // The process-loopback capture runs: muted.
    hooks.started(true);
    require(soon([&] { return pc.muted(speakers); }) && soon([&] { return seen(ev(E::muted, C::streamStarted)) == 1; }), "the session did not mute");
    // The user unmutes: muted again within a look.
    { std::lock_guard lock(pc.mutex); pc.endpoints[speakers] = false; }
    require(soon([&] { return pc.muted(speakers) && seen(ev(E::muted, C::userUnmuted)) == 1; }), "not muted again");
    // The stream restarts inside the session (a resize): the capture starts again, the mute never leaves.
    const auto calls = pc.callCount();
    hooks.started(true); std::this_thread::sleep_for(30ms);
    require(pc.muted(speakers) && pc.callCount() == calls && seen(ev(E::restored, C::captureChanged)) == 0, "a restart let the speakers through");
    // The capture is the endpoint's loopback now: given back at once. Process loopback again: muted again.
    hooks.started(false);
    require(soon([&] { return !pc.muted(speakers) && seen(ev(E::restored, C::captureChanged)) == 1; }) && pc.recordEmpty(), "the endpoint's loopback kept the mute");
    hooks.started(true);
    require(soon([&] { return pc.muted(speakers) && seen(ev(E::muted, C::streamStarted)) == 2; }), "not muted again for process loopback");
    // The session ends: restored before end() returns, and nothing is muted afterwards.
    muted.end();
    require(!pc.muted(speakers) && pc.recordEmpty() && seen(ev(E::restored, C::sessionEnded)) == 1, "end() returned with the speakers muted");
    const auto after = pc.callCount();
    hooks.started(true); std::this_thread::sleep_for(30ms); muted.end();
    require(pc.callCount() == after && !pc.muted(speakers), "muted after the session's end");
    require(accesses == 1 && locks > 0, "one access for the session, every look under the lock");
  }
  // A session destroyed while it holds the mute gives it back too.
  {
    SpeakerMuteSession muted([&] { return pc.access(first); }, {}, {}, 2ms);
    muted.captureStarted(true);
    require(soon([&] { return pc.muted(speakers); }), "muted");
  }
  require(!pc.muted(speakers) && pc.recordEmpty(), "a destroyed session left the speakers muted");
  // A session that never had a process-loopback capture ends without touching anything.
  { const auto calls = pc.callCount(); SpeakerMuteSession idle([&] { return pc.access(first); }, {}); idle.captureStarted(false); idle.end(); require(pc.callCount() == calls, "an idle session"); }
  // An endpoint that keeps being unmuted is muted every time, and fills the journal only up to its limit.
  {
    std::vector<uint32_t> noisy; std::mutex noisyMutex;
    SpeakerMuteSession muted([&] { return pc.access(first); }, [&](uint32_t value) { std::lock_guard lock(noisyMutex); noisy.push_back(value); }, {}, 1ms);
    muted.captureStarted(true);
    for (int i = 0; i < 40; ++i) { require(soon([&] { return pc.muted(speakers); }), "muted again"); std::lock_guard lock(pc.mutex); pc.endpoints[speakers] = false; }
    require(soon([&] { return pc.muted(speakers); }), "muted at last");
    muted.end();
    std::lock_guard lock(noisyMutex);
    require(noisy.size() == SpeakerMuteJournalLimit::burst + 1 && noisy.back() == ev(E::restored, C::sessionEnded), "journal bounded, the end always said: " + std::to_string(noisy.size()));
  }
  require(!pc.muted(speakers), "restored");
}

// The native side without an audio object: who a process is, and the lock.
void native() {
  const auto self = speakerMuteSelf();
  require(self.pid == GetCurrentProcessId() && self.started > 1 && speakerMuteOwnerAlive(self), "this process");
  require(!speakerMuteOwnerAlive({self.pid, self.started + 1}) && !speakerMuteOwnerAlive({self.pid, self.started - 1}), "this id at another start time is another process");
  require(!speakerMuteOwnerAlive(SpeakerMuteOwner{}), "nobody");
  // A process that has ended while its handle is still held (as the service holds a worker's).
  STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{}; wchar_t command[] = L"cmd.exe /c exit 0";
  require(CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE, "start a child");
  FILETIME created{}, exited{}, kernel{}, user{};
  require(GetProcessTimes(child.hProcess, &created, &exited, &kernel, &user) != FALSE, "child times");
  const SpeakerMuteOwner owner{child.dwProcessId, speakerMuteFileTime(created)};
  WaitForSingleObject(child.hProcess, 10000);
  require(!speakerMuteOwnerAlive(owner), "an ended process whose handle is held counts as gone");
  CloseHandle(child.hThread); CloseHandle(child.hProcess);
  // The lock: one holder at a time, taken again once it is let go.
  auto held = speakerMuteLock();
  require(held != nullptr, "the lock");
  std::atomic<int> stage{0};
  std::thread other([&] { stage = 1; const auto taken = speakerMuteLock(); stage = taken ? 2 : 3; });
  require(soon([&] { return stage == 1; }), "the other thread started");
  std::this_thread::sleep_for(50ms);
  require(stage == 1, "two holders at once");
  held.reset(); other.join();
  require(stage == 2, "the lock was not taken after its release");
  require(speakerMuteRecordPath().filename() == L"speaker-mute.state" && speakerMuteRecordPath().parent_path().filename() == L"Host", "the record's place");
}

// This machine's default render endpoint through the real access, read only: its id and whether it is
// muted. Nothing is set (the access's setMute and save are taken away first) and nothing of it is printed
// but two booleans.
void nativeRead() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  struct Uninit { HRESULT com; ~Uninit() { if (SUCCEEDED(com)) CoUninitialize(); } } uninit{com};
  auto access = nativeSpeakerMuteAccess();
  access.setMute = [](const std::wstring&, bool) -> bool { throw std::runtime_error("a read-only check set a mute"); };
  access.save = [](const std::string&) -> bool { throw std::runtime_error("a read-only check wrote the record"); };
  const auto endpoint = access.defaultEndpoint();
  if (!endpoint || endpoint->empty()) {
    std::cout << "{\"kind\":\"speaker-mute-native\",\"endpoint\":false,\"asked\":" << (endpoint ? "true" : "false") << "}\n";
    return;
  }
  require(speakerMuteEndpointId(*endpoint), "the default endpoint's id fits the record");
  const auto muted = access.muted(*endpoint);
  require(muted.has_value(), "the default endpoint's mute could not be read");
  require(!access.muted(L"{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}").has_value() && access.lastError() < 0, "an endpoint that does not exist is not there, with Windows' answer");
  std::cout << "{\"kind\":\"speaker-mute-native\",\"endpoint\":true,\"muted\":" << (*muted ? "true" : "false") << "}\n";
}

// The worker's call sites (a stream of a physical PC cannot be made on this machine).
std::string code(const char* path) {
  std::ifstream file(path); std::string line, text;
  while (std::getline(file, line)) { const auto at = line.find("//"); text += (at == std::string::npos ? line : line.substr(0, at)) + '\n'; }
  return text;
}
size_t count(const std::string& text, const std::string& needle) { size_t n = 0; for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n; return n; }
void callSites() {
  const auto agent = code(MULTITAP_AGENT_SOURCE), service = code(MULTITAP_SERVICE_SOURCE), media = code(MULTITAP_MEDIA_SOURCE), native = code(MULTITAP_SPEAKER_NATIVE_SOURCE);
  const auto none = std::string::npos;
  require(agent.find("class Agent") != none && service.find("interactiveWorker") != none && media.find("struct Loopback::Impl") != none && native.find("nativeSpeakerMuteAccess") != none, "the sources were read");
  // Both sound threads of the worker give the capture the session's hooks for a granted command only, and no
  // stream capture is made without that choice.
  require(count(agent, "const bool speakerMute=speakerMuteSessionRequested(command);") == 1 && count(agent, "const bool speakerMute=local_&&speakerMuteSessionRequested(control);") == 1, "a stream no longer reads the broker's marker");
  require(count(agent, "Loopback loopback(speakerMute?&speaker:nullptr)") == 1 && count(agent, "Loopback loopback(speakerMute ? &speaker : nullptr)") == 1 && count(agent, "Loopback loopback;") == 0, "a stream's capture is made without the session's hooks");
  // The capture says which loopback started, and only an opened process-loopback client is one.
  require(count(media, "impl.hooks->started(impl.process)") == 1 && count(media, "process = true; ready = true; return true;") == 1 && count(media, "PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE") == 1, "the capture no longer says which loopback runs");
  // The endpoint is only ever muted or unmuted: no volume level call exists.
  require(count(native, "->SetMute(") == 1 && native.find("SetMasterVolume") == none && native.find("SetChannelVolume") == none && native.find("VolumeStep") == none, "the volume level is touched");
  // The Host advertises it; the service journals stage 49 and a probe's restored leftovers.
  require(count(service, "\\\"speakerMuteProtocol\\\":\\\"\" + std::string(speakerMuteProtocol)") == 1, "the Host no longer advertises speaker-mute-v1");
  require(count(service, "if(stage==200+speakerMuteRecordStage){recordSpeakerMuteEvent(error);return;}") == 1 && count(service, "for(const auto value:result.speakerMute)recordSpeakerMuteEvent(value);") == 1, "the service no longer journals the records");
  // The worker: an earlier worker's leftovers are restored at its start, before any command; its session's
  // end restores before the stop is reported.
  const auto worker = service.find("inline int interactiveWorker("), recover = service.find("speakerRecovered=speakerMuteRecover();", worker), loop = service.find("\"worker.probe\"", recover);
  const auto tail = service.find("worker.suspendStreaming();{std::atomic<bool> speakerStopped{false};diagnosticStop=&speakerStopped;worker.speakerMuteEnd();", loop), reported = service.find("nativeStreamStoppedPacket(streamSession", tail);
  require(worker != none && recover != none && loop != none && tail != none && reported != none, "the worker no longer restores at its start and at its session's end, before it reports the stop");
  require(count(agent, "~Agent() { stop(); speakerMute_.end(); }") == 1, "a worker that ends without its tail no longer restores");
}
}

int main() {
  try {
    grantAndRecords(); recordText(); muteAndRestore(); usersOwnMute(); deviceChange(); crashRecovery(); handover(); failures(); session(); native(); callSites(); nativeRead();
    std::cout << "{\"kind\":\"speaker-mute\",\"passed\":" << checks << "}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "speaker mute check failed: " << error.what() << "\n"; return 1; }
}
