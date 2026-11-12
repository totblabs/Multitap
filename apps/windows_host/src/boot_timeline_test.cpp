// boot-ready-v1 (boot_timeline.hpp): stage marks, the one record per service
// run, its log fields, the service loop's first wait and its wake flag. No
// service, display, network or input is touched.
#include "boot_timeline.hpp"
#include "service_loop_guard.hpp"
#include <iostream>

using namespace host;
namespace {
int checks = 0;
void require(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
void timelineCases() {
  BootTimeline boot(100000);
  require(!boot.reached(BootStage::config) && boot.window(100000) && !boot.take(100500), "A fresh timeline reported a stage or a record");
  boot.mark(BootStage::config, 100012); boot.mark(BootStage::config, 100900);
  boot.mark(BootStage::display, 99000);  // a clock before the start counts as 0
  boot.mark(BootStage::broker, 100640); boot.mark(BootStage::logon, 121000);
  require(boot.reached(BootStage::config) && boot.reached(BootStage::display) && !boot.reached(BootStage::ready), "Marks were lost or invented");
  require(boot.window(150000) && !boot.take(150000), "A record was taken before ready inside the window");
  boot.mark(BootStage::ready, 126100);
  require(!boot.window(126200), "The boot window stayed open after the first ready report");
  const auto record = boot.take(126200);
  require(record && record->ready && record->uptimeMs == 100000, "The ready record was not taken");
  require(record->at[size_t(BootStage::config)] == 12 && record->at[size_t(BootStage::display)] == 0 && record->at[size_t(BootStage::broker)] == 640 &&
    record->at[size_t(BootStage::logon)] == 21000 && record->at[size_t(BootStage::ready)] == 26100 && record->at[size_t(BootStage::worker)] == -1, "Stage times are not the first marks in ms after the start, or an unreached stage was invented");
  require(!boot.take(126300) && !boot.take(900000), "A second record was taken in the same run");

  BootTimeline stalled(5000);
  stalled.mark(BootStage::config, 5010);
  require(stalled.window(5000 + BootTimeline::windowMs - 1) && !stalled.take(5000 + BootTimeline::windowMs - 1), "The deadline record came early");
  const auto late = stalled.take(5000 + BootTimeline::windowMs);
  require(late && !late->ready && late->at[size_t(BootStage::config)] == 10 && late->at[size_t(BootStage::ready)] == -1 && !stalled.window(5000 + BootTimeline::windowMs), "A VM never ready got no ten-minute record");
  stalled.mark(BootStage::ready, 5000 + BootTimeline::windowMs + 1);
  require(!stalled.take(5000 + BootTimeline::windowMs + 2), "A late ready added a second record");
}
// Every field of the event survives the log's own allow-list as a number
// (recordHostEvent and the diagnostics report drop unknown names silently).
void fieldCases() {
  std::string values = "{\"action\":274,\"stage\":1,\"ok\":true,\"" + std::string(bootUptimeField) + "\":4294967295";
  for (size_t i = 0; i < bootStageFields.size(); ++i) values += ",\"" + std::string(bootStageFields[i]) + "\":" + std::to_string(1000 + i);
  const auto row = JsonParser::parse("{\"time\":\"2026-11-03T00:00:00.000Z\",\"event\":\"hostAction\",\"values\":" + values + "}}");
  const auto safe = JsonParser::parse(safeHostLogRecord(row));
  require(safe.get("values").get(bootUptimeField).number() == 4294967295.0, "The uptime field was dropped by the log allow-list");
  for (size_t i = 0; i < bootStageFields.size(); ++i) require(hostLogField(bootStageFields[i]) && !hostLogBoolean(bootStageFields[i]) && safe.get("values").get(bootStageFields[i]).number() == double(1000 + i), "A boot stage field was dropped or logged as a boolean");
}
void loopCases() {
  require(serviceLoopWaitMs(true) == 0 && serviceLoopWaitMs(false) == 1000, "The service loop's first pass still waits a full second");
  WinHandle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
  require(event.value != nullptr, "Own test event unavailable");
  LoopWakeFlag flag;
  flag = true;
  require(flag.exchange(false) && WaitForSingleObject(event.value, 0) == WAIT_TIMEOUT, "A flag without a wake event signalled something");
  flag.wake(event.value);
  flag = false;
  require(WaitForSingleObject(event.value, 0) == WAIT_TIMEOUT && !flag.exchange(false), "Clearing the flag woke the loop");
  flag = true;
  require(WaitForSingleObject(event.value, 0) == WAIT_OBJECT_0 && flag.exchange(false) && !flag.exchange(false), "A readiness change did not wake the loop at once");
}
}
int main() {
  try {
    timelineCases(); fieldCases(); loopCases();
    std::cout << "{\"kind\":\"boot-timeline\",\"checks\":" << checks << ",\"serviceStarted\":false,\"networkUsed\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "Boot timeline: " << error.what() << '\n'; return 1; }
}
