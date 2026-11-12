// display-scale-v1 on the host: request validation, Windows' relative-step
// math and clamping, the keeper's apply/keep/restore/crash-recovery rules
// (record written before any change, foreign records kept, nothing changed
// for ungranted sessions or composite captures) with a substituted
// DisplayConfig and record store, the published result and the Session0
// Agent forwarding a request to its worker. No display setting is changed.
#include "agent.hpp"
#include "display_scale.hpp"
#include <winioctl.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
using namespace host;
namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
const std::string session = "00000000-0000-4000-8000-000000000072";
const std::string device = R"(\\\\.\\DISPLAY3)";
std::string start(const std::string& extra, const std::string& name = device) {
  return "{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"" + session + "\",\"displayDeviceName\":\"" + name + "\"" + extra + '}';
}
std::string granted(int percent) { return ",\"displayScaleProtocol\":\"display-scale-v1\",\"displayScale\":" + std::to_string(percent); }

// A fake Windows: one owned output (DISPLAY3 -> monitor MON-A), Windows'
// stored relative step and the recommended index of the current size.
struct FakeWindows {
  bool present = true, readable = true, writable = true, saveable = true;
  int recommended = 0, maximum = 3, relative = 0;
  std::string record;
  std::vector<std::string> log;  // "save", "write:<n>" in order
  std::vector<std::wstring> foundNames;
  unsigned reads = 0;
  DisplayScaleAccess access() {
    DisplayScaleAccess value;
    value.find = [this](const std::wstring& name) -> std::optional<DisplayScaleTarget> {
      foundNames.push_back(name);
      if (!present || name != L"\\\\.\\DISPLAY3") return std::nullopt;
      return DisplayScaleTarget{LUID{7, 0}, 1, L"MON-A"};
    };
    value.read = [this](const DisplayScaleTarget&) -> std::optional<DisplayScaleRange> {
      ++reads;
      if (!readable) return std::nullopt;
      return DisplayScaleRange{-recommended, std::clamp(relative, -recommended, maximum), maximum};
    };
    value.write = [this](const DisplayScaleTarget&, int next) { log.push_back("write:" + std::to_string(next)); if (!writable) return false; relative = next; return true; };
    value.load = [this]() -> std::optional<std::string> { return record; };
    value.save = [this](const std::string& text) { log.push_back("save"); if (!saveable) return false; record = text; return true; };
    return value;
  }
};

void parsing() {
  require(displayScaleSessionRequest(JsonParser::parse(start(granted(175)))) == 175, "A granted request was not read");
  require(!displayScaleSessionRequest(JsonParser::parse(start(""))), "An ungranted start asked for a scale");
  require(!displayScaleSessionRequest(JsonParser::parse(start(",\"displayScale\":175"))), "A scale without the marker was accepted");
  for (const auto* bad : {",\"displayScaleProtocol\":\"display-scale-v1\",\"displayScale\":160", ",\"displayScaleProtocol\":\"display-scale-v1\",\"displayScale\":175.5",
                          ",\"displayScaleProtocol\":\"display-scale-v1\",\"displayScale\":\"175\"", ",\"displayScaleProtocol\":\"display-scale-v2\",\"displayScale\":175"})
    require(!displayScaleSessionRequest(JsonParser::parse(start(bad))), "A malformed request was accepted");
  require(displayScaleMessage(JsonParser::parse(R"({"type":"display.scale","sessionId":"x","scale":250})")) == 250, "A runtime request was not read");
  require(!displayScaleMessage(JsonParser::parse(R"({"type":"display.scale","scale":90})")), "An out-of-table runtime request was accepted");
  require(!displayScaleMessage(JsonParser::parse(R"({"type":"display.scales","scale":200})")), "Another message type was read as a request");
}

void math() {
  // 1080-pixel output: recommended 100 % (index 0), Windows allows up to 175 %.
  const DisplayScaleRange compact{0, 0, 3};
  require(validDisplayScaleRange(compact), "A valid range was refused");
  require(displayScaleRelativeFor(compact, 175) == 3 && displayScaleRelativeFor(compact, 250) == 3, "175/250 % were not clamped to Windows' 175 %");
  require(displayScalePercentFor(compact, 3) == 175 && displayScaleRelativeFor(compact, 100) == 0, "Percent/step mapping differs");
  // Recommended 125 % (index 1): 100 % is one step below.
  const DisplayScaleRange tablet{-1, 0, 4};
  require(displayScaleRelativeFor(tablet, 100) == -1 && displayScaleRelativeFor(tablet, 175) == 2 && displayScalePercentFor(tablet, 4) == 225, "Steps around a 125 % recommendation differ");
  for (const auto& bad : {DisplayScaleRange{1, 0, 3}, DisplayScaleRange{0, 4, 3}, DisplayScaleRange{-12, 0, 0}, DisplayScaleRange{-6, 0, 6}})
    require(!validDisplayScaleRange(bad), "An impossible range was accepted");
}

void applyKeepRestore() {
  FakeWindows windows; DisplayScaleKeeper keeper(windows.access());
  const auto first = keeper.stream(JsonParser::parse(start(granted(250))));
  require(first && first->ok && first->scale == 175 && first->maximum == 175 && first->code == DisplayScaleCode::applied, "250 % was not applied as Windows' 175 %");
  require(windows.log.size() == 2 && windows.log[0] == "save" && windows.log[1] == "write:3", "The record was not written before the change");
  require(windows.record == "0\tMON-A\n", "The previous step was not recorded");
  // Runtime requests change it; an unchanged value writes nothing.
  windows.log.clear();
  const auto same = keeper.request(200);
  require(same && same->ok && same->scale == 175 && windows.log.empty(), "An unchanged (clamped) value was written again");
  const auto smaller = keeper.request(125);
  require(smaller && smaller->ok && smaller->scale == 125 && windows.log == std::vector<std::string>{"write:1"}, "A runtime request was not applied once");
  // A resize: Windows now recommends 125 % for the new size and keeps the
  // relative step, so the absolute percent must be applied again.
  windows.recommended = 1; windows.maximum = 4; windows.log.clear();
  const auto resized = keeper.stream(JsonParser::parse(start(granted(250))));
  require(resized && resized->ok && resized->scale == 125 && windows.relative == 0, "The kept percent was not re-applied after a resize");
  require(windows.log == std::vector<std::string>{"write:0"}, "A resize re-recorded or wrote twice");
  // The end puts the recorded step back and forgets the record.
  windows.log.clear();
  require(keeper.restore() && windows.relative == 0 && windows.record.empty(), "The previous step was not restored");
  require(windows.log == std::vector<std::string>{"write:0", "save"}, "Restore order differs");
  require(keeper.restore(), "A second restore was not idempotent");
}

void ungrantedAndComposite() {
  FakeWindows windows; DisplayScaleKeeper keeper(windows.access());
  require(!keeper.stream(JsonParser::parse(start(""))), "An ungranted session published a result");
  require(!keeper.request(200), "An ungranted session accepted a runtime request");
  require(windows.log.empty() && keeper.restore(), "An ungranted session touched the display");
  // Granted, but the capture is a composite (no single owned output).
  FakeWindows composite; DisplayScaleKeeper wide(composite.access());
  const auto result = wide.stream(JsonParser::parse(start(granted(150), "")));
  require(result && !result->ok && result->code == DisplayScaleCode::unsupported && composite.log.empty(), "A composite capture was scaled");
  // Granted without a value: observe current DPI without changing it.
  FakeWindows idle; DisplayScaleKeeper waiting(idle.access());
  const auto observed = waiting.stream(JsonParser::parse(start(",\"displayScaleProtocol\":\"display-scale-v1\"")));
  require(observed && observed->ok && observed->scale == 100 && idle.log.empty(), "A granted start without a value did not observe the unchanged scale");
  require(waiting.request(150) && idle.relative == 2, "A later request of a granted session was not applied");
}

void observeCurrentWithoutWrite() {
  const auto marker = ",\"displayScaleProtocol\":\"display-scale-v1\"";
  FakeWindows windows; windows.recommended = 1;
  DisplayScaleKeeper keeper(windows.access());
  const auto first = keeper.stream(JsonParser::parse(start(marker)));
  require(first && first->ok && first->scale == 125 && first->maximum == 200,
          "An automatic start did not report actual current DPI and maximum");
  require(windows.log.empty() && windows.record.empty() && !keeper.changed(),
          "A read-only initial observation saved or wrote DPI state");
  require(windows.reads == 1 && windows.foundNames == std::vector<std::wstring>{L"\\\\.\\DISPLAY3"},
          "Observation was not bound to the capture's single owned source");
  // With no explicit selection, a resize observes Windows' current result.
  // It must not latch the first observed125% as a requested fixed percentage.
  windows.recommended = 2;
  const auto resized = keeper.stream(JsonParser::parse(start(marker)));
  require(resized && resized->ok && resized->scale == 150 && windows.log.empty(),
          "An observed value was incorrectly applied as a fixed request");
  const auto selected = keeper.request(175);
  require(selected && selected->ok && selected->scale == 175 && keeper.changed(),
          "An explicit selection after automatic observation did not apply");
  require(keeper.restore() && windows.relative == 0 && windows.record.empty(),
          "An explicit selection did not restore its actual previous DPI");
  FakeWindows unreadable; unreadable.readable = false;
  DisplayScaleKeeper unknown(unreadable.access());
  const auto failed = unknown.stream(JsonParser::parse(start(marker)));
  require(failed && !failed->ok && failed->scale == 0 &&
              failed->code == DisplayScaleCode::unreadable && unreadable.log.empty(),
          "An unreadable DPI was invented or changed");
  FakeWindows missing; missing.present = false;
  DisplayScaleKeeper absent(missing.access());
  const auto noOutput = absent.stream(JsonParser::parse(start(marker)));
  require(noOutput && !noOutput->ok && noOutput->code == DisplayScaleCode::outputMissing &&
              missing.reads == 0 && missing.log.empty(),
          "A missing owned output was read or changed");
  FakeWindows other;
  DisplayScaleKeeper composite(other.access());
  const auto unsupported = composite.stream(JsonParser::parse(start(marker, "")));
  require(unsupported && !unsupported->ok && unsupported->code == DisplayScaleCode::unsupported &&
              other.reads == 0 && other.foundNames.empty() && other.log.empty(),
          "A composite source was observed as a single owned output");
  FakeWindows denied;
  DisplayScaleKeeper ungranted(denied.access());
  require(!ungranted.stream(JsonParser::parse(start(""))) && denied.reads == 0 && denied.log.empty(),
          "An ungranted clean session observed or wrote DPI");
}

void failures() {
  { FakeWindows windows; windows.saveable = false; DisplayScaleKeeper keeper(windows.access());
    const auto result = keeper.stream(JsonParser::parse(start(granted(150))));
    require(result && !result->ok && result->code == DisplayScaleCode::recordUnavailable && windows.relative == 0, "A change was made without a recovery record"); }
  { FakeWindows windows; windows.writable = false; DisplayScaleKeeper keeper(windows.access());
    const auto result = keeper.stream(JsonParser::parse(start(granted(150))));
    require(result && !result->ok && result->code == DisplayScaleCode::refused && result->scale == 100, "A refused change was reported as applied"); }
  { FakeWindows windows; windows.present = false; DisplayScaleKeeper keeper(windows.access());
    const auto result = keeper.stream(JsonParser::parse(start(granted(150))));
    require(result && !result->ok && result->code == DisplayScaleCode::outputMissing && windows.log.empty(), "A missing output was changed"); }
  { FakeWindows windows; windows.readable = false; DisplayScaleKeeper keeper(windows.access());
    const auto result = keeper.stream(JsonParser::parse(start(granted(150))));
    require(result && !result->ok && result->code == DisplayScaleCode::unreadable && windows.log.empty(), "An unreadable output was changed"); }
  { FakeWindows windows; DisplayScaleKeeper keeper(windows.access());
    (void)keeper.stream(JsonParser::parse(start(granted(150))));
    windows.present = false;
    require(!keeper.restore() && windows.record == "0\tMON-A\n", "A restore without the output dropped the recovery record"); }
}

void crashRecovery() {
  // A worker that ended without restoring left its record; the next session
  // on that monitor without a request puts the step back.
  { FakeWindows windows; windows.relative = 3; windows.record = "0\tMON-A\n-1\tMON-B\n"; DisplayScaleKeeper keeper(windows.access());
    require(!keeper.stream(JsonParser::parse(start(""))), "Recovery published a result");
    require(windows.relative == 0 && windows.record == "-1\tMON-B\n", "The leftover step was not restored, or a foreign record was dropped"); }
  // A granted session adopts the leftover record as its original.
  { FakeWindows windows; windows.recommended = 1; windows.maximum = 4; windows.relative = 2; windows.record = "-1\tMON-A\n"; DisplayScaleKeeper keeper(windows.access());
    const auto result = keeper.stream(JsonParser::parse(start(granted(200))));
    require(result && result->ok && result->scale == 200 && windows.relative == 3, "A recovered session did not apply its request");
    require(windows.record == "-1\tMON-A\n", "The adopted record was replaced by the leftover step");
    require(keeper.restore() && windows.relative == -1 && windows.record.empty(), "The adopted original was not restored"); }
  // A record for another monitor is never applied here and survives new records.
  { FakeWindows windows; windows.record = "2\tMON-B\n"; DisplayScaleKeeper keeper(windows.access());
    (void)keeper.stream(JsonParser::parse(start(granted(150))));
    require(windows.record == "0\tMON-A\n2\tMON-B\n", "A foreign record was lost when this output was recorded");
    require(keeper.restore() && windows.record == "2\tMON-B\n", "Restoring this output removed a foreign record"); }
  // A malformed record is ignored, never applied.
  { FakeWindows windows; windows.relative = 1; windows.record = "x\tMON-A\n99\tMON-A\n\tMON-A\n"; DisplayScaleKeeper keeper(windows.access());
    (void)keeper.stream(JsonParser::parse(start("")));
    require(windows.relative == 1 && windows.log.empty(), "A malformed record changed the display"); }
  // The output could not be identified at the first stream command (Windows
  // still re-laying out after the start): the leftover record must neither be
  // replaced by the changed step nor skipped once the output is found.
  { FakeWindows windows; windows.relative = 3; windows.record = "0\tMON-A\n"; windows.present = false; DisplayScaleKeeper keeper(windows.access());
    const auto missing = keeper.stream(JsonParser::parse(start(granted(150))));
    require(missing && missing->code == DisplayScaleCode::outputMissing && windows.record == "0\tMON-A\n", "A missing output touched the record");
    windows.present = true;
    const auto resized = keeper.stream(JsonParser::parse(start(granted(150))));
    require(resized && resized->ok && windows.relative == 2, "The request was not applied once the output was found");
    require(windows.record == "0\tMON-A\n", "The leftover record was replaced by the changed step");
    require(keeper.restore() && windows.relative == 0 && windows.record.empty(), "The leftover original was not restored"); }
  { FakeWindows windows; windows.relative = 3; windows.record = "0\tMON-A\n"; windows.present = false; DisplayScaleKeeper keeper(windows.access());
    require(!keeper.stream(JsonParser::parse(start(""))), "An ungranted missing output published a result");
    windows.present = true;
    require(!keeper.stream(JsonParser::parse(start(""))), "An ungranted resize published a result");
    require(windows.relative == 0 && windows.record.empty(), "Recovery was not tried again once the output was found"); }
}

// Independent outputs (tracks-v1) are never scaled, but a leftover record of
// a monitor they stream is put back like at an ordinary start.
void independentOutputs() {
  FakeWindows windows; windows.relative = 3; windows.record = "0\tMON-A\n-1\tMON-B\n"; DisplayScaleKeeper keeper(windows.access());
  keeper.streamOutputs({L"\\\\.\\DISPLAY3", L"\\\\.\\DISPLAY9"});
  require(windows.relative == 0 && windows.record == "-1\tMON-B\n", "A leftover record of a streamed output was not restored");
  require(!keeper.changed() && keeper.restore(), "Independent outputs changed a scale");
  // Nothing recorded: nothing written.
  FakeWindows clean; DisplayScaleKeeper quiet(clean.access());
  quiet.streamOutputs({L"\\\\.\\DISPLAY3"});
  require(clean.log.empty(), "An output without a record was written");
}

// display-scale-outputs-v1: two owned outputs (DISPLAY3 -> MON-A allowing
// 175 %, DISPLAY4 -> MON-B allowing 200 %) with their own Windows steps.
struct FakeOutputs {
  std::map<std::wstring, std::wstring> monitorOf{{L"\\\\.\\DISPLAY3", L"MON-A"}, {L"\\\\.\\DISPLAY4", L"MON-B"}};
  std::map<std::wstring, int> relative{{L"MON-A", 0}, {L"MON-B", 0}}, recommended{{L"MON-A", 0}, {L"MON-B", 0}}, maximum{{L"MON-A", 3}, {L"MON-B", 4}};
  std::vector<std::wstring> missing;
  std::string record;
  std::vector<std::string> log;  // "save", "write:<monitor>:<n>" in order
  DisplayScaleAccess access() {
    DisplayScaleAccess value;
    value.find = [this](const std::wstring& name) -> std::optional<DisplayScaleTarget> {
      const auto found = monitorOf.find(name);
      if (found == monitorOf.end() || std::find(missing.begin(), missing.end(), name) != missing.end()) return std::nullopt;
      return DisplayScaleTarget{LUID{7, 0}, found->second == L"MON-A" ? 1u : 2u, found->second};
    };
    value.read = [this](const DisplayScaleTarget& target) -> std::optional<DisplayScaleRange> {
      const int low = -recommended.at(target.monitor), high = maximum.at(target.monitor);
      return DisplayScaleRange{low, std::clamp(relative.at(target.monitor), low, high), high};
    };
    value.write = [this](const DisplayScaleTarget& target, int next) { log.push_back("write:" + utf8(target.monitor) + ':' + std::to_string(next)); relative[target.monitor] = next; return true; };
    value.load = [this]() -> std::optional<std::string> { return record; };
    value.save = [this](const std::string& text) { log.push_back("save"); record = text; return true; };
    return value;
  }
};
std::string trackStart(const std::string& extra) {
  return "{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"" + session + "\",\"mediaProtocol\":\"tracks-v1\"" + extra + '}';
}
const auto outputsMarker = ",\"displayScaleProtocol\":\"display-scale-v1\",\"displayScaleOutputsProtocol\":\"display-scale-outputs-v1\"";
std::string outputsGranted(int percent) { return outputsMarker + std::string(",\"displayScale\":") + std::to_string(percent); }
const std::vector<DisplayScaleOutput> twoOutputs{{"0", L"\\\\.\\DISPLAY3"}, {"1", L"\\\\.\\DISPLAY4"}};
bool outputIs(const DisplayScaleOutputResult& value, const char* id, bool ok, int scale, int maximum, DisplayScaleCode code) {
  return value.id == id && value.result.ok == ok && value.result.scale == scale && value.result.maximum == maximum && value.result.code == code;
}

void outputsParsing() {
  require(displayScaleOutputsGranted(JsonParser::parse(trackStart(outputsMarker))), "Both markers did not grant independent outputs");
  require(!displayScaleOutputsGranted(JsonParser::parse(trackStart(",\"displayScaleProtocol\":\"display-scale-v1\""))), "display-scale-v1 alone granted independent outputs");
  require(!displayScaleOutputsGranted(JsonParser::parse(trackStart(",\"displayScaleOutputsProtocol\":\"display-scale-outputs-v1\""))), "The outputs marker alone granted independent outputs");
  require(!displayScaleOutputsGranted(JsonParser::parse(trackStart(",\"displayScaleProtocol\":\"display-scale-v1\",\"displayScaleOutputsProtocol\":\"display-scale-outputs-v2\""))), "Another outputs marker was accepted");
}

// One chosen scale on both outputs, each clamped by Windows, each recorded
// before its change, kept across a restart and both put back at the end.
void outputsApplyRestore() {
  FakeOutputs windows; DisplayScaleOutputs outputs(windows.access());
  const auto first = outputs.stream(JsonParser::parse(trackStart(outputsGranted(200))), twoOutputs);
  require(first && first->size() == 2 && outputIs((*first)[0], "0", true, 175, 175, DisplayScaleCode::applied) && outputIs((*first)[1], "1", true, 200, 200, DisplayScaleCode::applied),
          "200 % was not applied per output (175 % where Windows allows only that)");
  require(windows.log == std::vector<std::string>{"save", "write:MON-A:3", "save", "write:MON-B:4"}, "An output was changed before its previous value was recorded");
  require(windows.record == "0\tMON-B\n0\tMON-A\n", "Both previous values were not recorded");
  require(outputs.granted(), "A granted session did not say so");
  windows.log.clear();
  const auto smaller = outputs.request(125);
  require(smaller && smaller->size() == 2 && outputIs((*smaller)[0], "0", true, 125, 175, DisplayScaleCode::applied) && outputIs((*smaller)[1], "1", true, 125, 200, DisplayScaleCode::applied) &&
              windows.log == std::vector<std::string>{"write:MON-A:1", "write:MON-B:1"},
          "A runtime request did not change both outputs once");
  // Restart at a new size: Windows recommends 125 % for B and keeps its relative step.
  windows.recommended[L"MON-B"] = 1; windows.log.clear();
  const auto resized = outputs.stream(JsonParser::parse(trackStart(outputsGranted(200))), twoOutputs);
  require(resized && outputIs((*resized)[0], "0", true, 125, 175, DisplayScaleCode::applied) && outputIs((*resized)[1], "1", true, 125, 225, DisplayScaleCode::applied) &&
              windows.log == std::vector<std::string>{"write:MON-B:0"},
          "The kept percent was not re-applied to the output Windows re-laid");
  windows.log.clear();
  require(outputs.restore() && windows.relative[L"MON-A"] == 0 && windows.relative[L"MON-B"] == 0 && windows.record.empty(), "Both previous values were not put back");
  require(outputs.restore(), "A second restore was not idempotent");
}

// display-scale-v1 alone (a released broker never sends the outputs marker):
// exactly the released behaviour, nothing scaled, a leftover record put back once.
void outputsUngranted() {
  FakeOutputs windows; windows.relative[L"MON-A"] = 3; windows.record = "0\tMON-A\n"; DisplayScaleOutputs outputs(windows.access());
  require(!outputs.stream(JsonParser::parse(trackStart(",\"displayScaleProtocol\":\"display-scale-v1\",\"displayScale\":200")), twoOutputs), "An ungranted track session published a result");
  require(windows.relative[L"MON-A"] == 0 && windows.relative[L"MON-B"] == 0 && windows.record.empty() && windows.log == std::vector<std::string>{"write:MON-A:0", "save"},
          "The leftover record was not put back, or an output was scaled");
  windows.log.clear(); windows.record = "2\tMON-B\n";
  require(!outputs.stream(JsonParser::parse(trackStart(outputsGranted(200))), twoOutputs) && !outputs.request(150) && !outputs.granted(), "A restart or request turned an ungranted session into a granted one");
  require(windows.log.empty() && outputs.restore() && windows.log.empty(), "An ungranted session touched an output after its start");
}

// A crashed worker left both records: a granted session adopts them as its
// originals; a marker-only one observes and puts the leftover value back.
void outputsRecovery() {
  { FakeOutputs windows; windows.relative = {{L"MON-A", 3}, {L"MON-B", 4}}; windows.record = "1\tMON-A\n2\tMON-B\n"; DisplayScaleOutputs outputs(windows.access());
    const auto result = outputs.stream(JsonParser::parse(trackStart(outputsGranted(150))), twoOutputs);
    require(result && outputIs((*result)[0], "0", true, 150, 175, DisplayScaleCode::applied) && outputIs((*result)[1], "1", true, 150, 200, DisplayScaleCode::applied), "A recovered session did not apply its request");
    require(windows.record == "1\tMON-A\n2\tMON-B\n", "An adopted record was replaced by the leftover step");
    require(outputs.restore() && windows.relative[L"MON-A"] == 1 && windows.relative[L"MON-B"] == 2 && windows.record.empty(), "The adopted originals were not put back"); }
  { FakeOutputs windows; windows.relative[L"MON-B"] = 4; windows.record = "2\tMON-B\n"; DisplayScaleOutputs outputs(windows.access());
    const auto observed = outputs.stream(JsonParser::parse(trackStart(outputsMarker)), twoOutputs);
    require(observed && outputIs((*observed)[0], "0", true, 100, 175, DisplayScaleCode::applied) && outputIs((*observed)[1], "1", true, 150, 200, DisplayScaleCode::applied),
            "A marker-only start did not report each output's actual scale");
    require(windows.log == std::vector<std::string>{"write:MON-B:2", "save"} && windows.record.empty(), "A marker-only start changed an output or kept the leftover record"); }
}

// One output cannot be found: the other is still scaled, the failure is
// reported for that output only and the summary is not ok.
void outputsPartial() {
  FakeOutputs windows; windows.missing = {L"\\\\.\\DISPLAY4"}; DisplayScaleOutputs outputs(windows.access());
  const auto result = outputs.stream(JsonParser::parse(trackStart(outputsGranted(150))), {{"0", L"\\\\.\\DISPLAY3"}, {"1", L"\\\\.\\DISPLAY4"}});
  require(result && outputIs((*result)[0], "0", true, 150, 175, DisplayScaleCode::applied) && outputIs((*result)[1], "1", false, 0, 0, DisplayScaleCode::outputMissing) &&
              windows.relative[L"MON-B"] == 0,
          "A missing output was not reported on its own");
  const auto composite = outputs.stream(JsonParser::parse(trackStart(outputsGranted(150))), {{"0", L"\\\\.\\DISPLAY3"}, {"1", L""}});
  require(composite && outputIs((*composite)[1], "1", false, 0, 0, DisplayScaleCode::unsupported), "An output without one owned monitor was scaled");
  const auto packet = JsonParser::parse(displayScaleOutputsAppliedPacket(session, *result));
  require(!packet.get("ok").boolean(true) && packet.get("code").number() == 1 && packet.get("scale").number() == 150 && packet.get("maximum").number() == 175, "A partial summary differs");
  require(outputs.restore() && windows.relative[L"MON-A"] == 0, "The scaled output was not put back");
}

void outputsPacket() {
  const DisplayScaleOutputResults both{{"0", {true, 175, 175, DisplayScaleCode::applied}}, {"1", {true, 200, 200, DisplayScaleCode::applied}}};
  const auto text = displayScaleOutputsAppliedPacket(session, both);
  require(text == "{\"type\":\"display.scale.applied\",\"sessionId\":\"" + session + "\",\"protocol\":\"display-scale-v1\",\"ok\":true,\"scale\":175,\"maximum\":200,\"code\":0,"
                  "\"outputs\":[{\"id\":\"0\",\"ok\":true,\"scale\":175,\"maximum\":175,\"code\":0},{\"id\":\"1\",\"ok\":true,\"scale\":200,\"maximum\":200,\"code\":0}]}",
          "The per-output result differs");
  const auto refused = JsonParser::parse(displayScaleOutputsAppliedPacket(session, {{"0", {true, 125, 200, DisplayScaleCode::applied}}, {"1", {false, 100, 200, DisplayScaleCode::refused}}}));
  require(!refused.get("ok").boolean(true) && refused.get("code").number() == 3 && refused.get("scale").number() == 125, "A refused output did not make the summary fail");
}

// The capture's monitor changes during the session (A, then B, then A again):
// each monitor's recorded original survives, and the end restores the current
// one with its own original, never with a value this session set.
void monitorChange() {
  struct Monitors {
    std::wstring current = L"MON-A";
    std::map<std::wstring, int> relative{{L"MON-A", 0}, {L"MON-B", 1}};
    std::string record;
    DisplayScaleAccess access() {
      DisplayScaleAccess value;
      value.find = [this](const std::wstring&) -> std::optional<DisplayScaleTarget> { return DisplayScaleTarget{LUID{7, 0}, 1, current}; };
      value.read = [this](const DisplayScaleTarget& target) -> std::optional<DisplayScaleRange> { return DisplayScaleRange{0, relative.at(target.monitor), 4}; };
      value.write = [this](const DisplayScaleTarget& target, int next) { relative[target.monitor] = next; return true; };
      value.load = [this]() -> std::optional<std::string> { return record; };
      value.save = [this](const std::string& text) { record = text; return true; };
      return value;
    }
  } windows;
  DisplayScaleKeeper keeper(windows.access());
  require(keeper.stream(JsonParser::parse(start(granted(200))))->ok && windows.relative[L"MON-A"] == 4, "A was not scaled");
  windows.current = L"MON-B";
  require(keeper.stream(JsonParser::parse(start(granted(200))))->ok && windows.relative[L"MON-B"] == 4, "B was not scaled");
  windows.current = L"MON-A";
  require(keeper.stream(JsonParser::parse(start(granted(200))))->ok && windows.relative[L"MON-A"] == 4, "A lost its scale");
  require(keeper.request(150)->ok && windows.relative[L"MON-A"] == 2, "A was not scaled again");
  require(keeper.restore() && windows.relative[L"MON-A"] == 0, "A was restored with a value this session set");
  require(windows.record == "1\tMON-B\n", "B's original was lost");
}

void packet() {
  const auto value = JsonParser::parse(displayScaleAppliedPacket(session, {true, 175, 175, DisplayScaleCode::applied}));
  require(value.get("type").string() == "display.scale.applied" && value.get("sessionId").string() == session && value.get("protocol").string() == "display-scale-v1" &&
          value.get("ok").boolean() && value.get("scale").number() == 175 && value.get("maximum").number() == 175 && value.get("code").number(-1) == 0, "The applied result differs");
  const auto failed = JsonParser::parse(displayScaleAppliedPacket(session, {false, 0, 0, DisplayScaleCode::outputMissing}));
  require(!failed.get("ok").boolean(true) && failed.get("scale").number(-1) == 0 && failed.get("code").number() == 1, "A failure result differs");
}

// A directory junction (mount point) at |link| resolving to |target|; no
// privilege is needed (thumbnail_writer_smoke.cpp), so it stands in for every
// link a same-user program could plant in the user's own profile.
bool junction(const std::filesystem::path& link, const std::filesystem::path& target) {
  if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
  const HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (directory == INVALID_HANDLE_VALUE) return false;
  const std::wstring substitute = L"\\??\\" + target.wstring(), print = target.wstring();
  const size_t names = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
  std::vector<unsigned char> buffer(16 + names, 0);
  const auto put16 = [&](size_t at, size_t value) { buffer[at] = BYTE(value & 0xff); buffer[at + 1] = BYTE((value >> 8) & 0xff); };
  const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
  std::memcpy(buffer.data(), &tag, 4);
  put16(4, 8 + names); put16(8, 0); put16(10, substitute.size() * 2); put16(12, (substitute.size() + 1) * 2); put16(14, print.size() * 2);
  std::memcpy(buffer.data() + 16, substitute.c_str(), (substitute.size() + 1) * 2);
  std::memcpy(buffer.data() + 16 + (substitute.size() + 1) * 2, print.c_str(), (print.size() + 1) * 2);
  DWORD returned = 0;
  const bool made = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, buffer.data(), DWORD(buffer.size()), nullptr, 0, &returned, nullptr) != FALSE;
  CloseHandle(directory);
  return made;
}
bool emptyFolder(const std::filesystem::path& folder) { return std::filesystem::is_directory(folder) && std::filesystem::directory_iterator(folder) == std::filesystem::directory_iterator(); }
bool isLink(const std::filesystem::path& path) { const DWORD attributes = GetFileAttributesW(path.c_str()); return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT); }

// The real record file (the worker writes it with the user's elevated token
// into that user's own profile): a plain path round-trips; a junction on the
// way, under the record's name or under its temporary name never redirects
// the elevated create, replace or delete. An unsafe record changes no scale.
void recordFile() {
  const auto root = std::filesystem::temp_directory_path() / (L"multitap-display-scale-" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ignored; std::filesystem::remove_all(root, ignored); std::filesystem::create_directories(root);
  struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code e; for (const auto* link : {L"linked", L"dest\\display-scale.state"}) RemoveDirectoryW((root / link).c_str()); std::filesystem::remove_all(root, e); } } cleanup{root};
  // Plain profile folders: created on the first write, read back, replaced, removed.
  const auto path = root / L"Multitap" / L"Host" / L"display-scale.state";
  const DisplayScaleRecordFile record(path);
  require(record.load() == std::string() && !std::filesystem::exists(root / L"Multitap"), "A missing record was not empty, or reading created folders");
  require(record.save("0\tMON-A\n") && record.load() == std::string("0\tMON-A\n"), "A safe record did not round-trip");
  require(record.save("2\tMON-B\n") && record.load() == std::string("2\tMON-B\n") && !std::filesystem::exists(path.wstring() + L".new"), "A safe record was not replaced cleanly");
  require(record.save("") && !std::filesystem::exists(path) && record.save(""), "A safe record was not removed");
  // A junctioned folder on the way: refused, its target untouched.
  const auto elsewhere = root / L"elsewhere"; std::filesystem::create_directories(elsewhere);
  require(junction(root / L"linked", elsewhere), "Junction fixture");
  const DisplayScaleRecordFile linked(root / L"linked" / L"display-scale.state");
  require(!linked.save("0\tMON-A\n") && emptyFolder(elsewhere), "The record was written through a junctioned folder");
  std::filesystem::path planted = elsewhere / L"display-scale.state";
  { std::ofstream file(planted, std::ios::binary); file << "0\tMON-A\n"; }
  require(!linked.load() && !linked.save("") && std::filesystem::exists(planted), "A record behind a junctioned folder was read or deleted");
  std::filesystem::remove(planted);
  // The keeper with that record: no recovery record, no change (code 4).
  { FakeWindows windows; auto access = windows.access();
    access.load = [&] { return linked.load(); }; access.save = [&](const std::string& text) { return linked.save(text); };
    DisplayScaleKeeper keeper(access);
    const auto result = keeper.stream(JsonParser::parse(start(granted(150))));
    require(result && !result->ok && result->code == DisplayScaleCode::recordUnavailable && windows.relative == 0 && emptyFolder(elsewhere), "An unsafe record still allowed a change"); }
  // A junction planted under the temporary name goes as itself.
  const auto temporaryTarget = root / L"temporary-target"; std::filesystem::create_directories(temporaryTarget);
  require(junction(path.wstring() + L".new", temporaryTarget), "Temporary-name junction fixture");
  require(record.save("0\tMON-A\n") && emptyFolder(temporaryTarget) && !std::filesystem::exists(path.wstring() + L".new") && std::filesystem::is_regular_file(path) && !isLink(path),
          "A junction under the temporary name was followed or left behind");
  // A junction under the record's own name: never written through, read or removed.
  const auto destinationTarget = root / L"destination-target"; std::filesystem::create_directories(root / L"dest"); std::filesystem::create_directories(destinationTarget);
  require(junction(root / L"dest" / L"display-scale.state", destinationTarget), "Record-name junction fixture");
  const DisplayScaleRecordFile named(root / L"dest" / L"display-scale.state");
  require(!named.save("0\tMON-A\n") && !named.load() && !named.save("") && emptyFolder(destinationTarget) && isLink(root / L"dest" / L"display-scale.state"), "A junction under the record's name was followed or replaced");
}

struct Bridge final : AgentBridge {
  std::vector<Json> controls;
  void start(const Json&, Send) override {}
  void control(const Json& value) override { controls.push_back(value); }
  void stop() override {}
  std::string capabilities() override { return "{\"service\":true}"; }
  void displayControl(const Json&, Send) override {}
};
void serviceForwarding() {
  Bridge bridge; Agent agent([](const std::wstring&) {}, &bridge); agent.publicationTestOpen();
  agent.localControl(start(granted(175)));
  agent.localControl("{\"type\":\"display.scale\",\"sessionId\":\"" + session + "\",\"scale\":200}");
  agent.localControl("{\"type\":\"display.scale\",\"sessionId\":\"another\",\"scale\":125}");
  require(bridge.controls.size() == 1 && bridge.controls[0].get("type").string() == "display.scale" && bridge.controls[0].get("scale").number() == 200, "Session0 did not forward exactly this session's request");
}
}  // namespace

int main() {
  const struct { const char* name; void (*run)(); } cases[] = {{"parsing", parsing}, {"math", math}, {"apply-keep-restore", applyKeepRestore}, {"ungranted-composite", ungrantedAndComposite},
                                                                {"observe-read-only", observeCurrentWithoutWrite}, {"failures", failures}, {"crash-recovery", crashRecovery}, {"independent-outputs", independentOutputs},
                                                                {"outputs-parsing", outputsParsing}, {"outputs-apply-restore", outputsApplyRestore}, {"outputs-ungranted", outputsUngranted}, {"outputs-recovery", outputsRecovery},
                                                                {"outputs-partial", outputsPartial}, {"outputs-packet", outputsPacket}, {"monitor-change", monitorChange}, {"record-file", recordFile}, {"packet", packet}, {"service-forwarding", serviceForwarding}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
  std::cout << "{\"kind\":\"display-scale-v1\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"displayConfig\":\"substituted\",\"displayChanged\":false}\n";
  return failed ? 1 : 0;
}
