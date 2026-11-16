// capture_loop_stats.hpp: the capture loop's window of skipped ticks by reason,
// pacing lateness, the unmeasured tail and the worker's longest pipe write, as
// the fixed members of stream.stats "stall". Pure; no capture, pipe or network.
#include "capture_loop_stats.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
unsigned checks = 0;
void require(bool condition, const char* reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
using namespace std::chrono_literals;
using Skip = host::CaptureLoopStats::Skip;

void emptyWindow() {
  host::CaptureLoopStats stats;
  require(stats.takeJson() == ",\"backgroundTicks\":0,\"backgroundMs\":0,\"secureHoldTicks\":0,\"secureHoldMs\":0,\"secureNoticeTicks\":0,\"secureNoticeMs\":0,"
                              "\"secureHelperTicks\":0,\"secureHelperMs\":0,\"follows\":0,\"followMs\":0,\"overshootMaxUs\":0,\"tailMaxUs\":0",
          "An empty window's members differ");
  require(stats.takeJson(0).ends_with(",\"tailMaxUs\":0,\"pipeWriteMaxUs\":0"), "A worker's pipe write maximum is missing");
}

void reasonsAndReset() {
  host::CaptureLoopStats stats;
  stats.skipped(Skip::background, 50ms); stats.skipped(Skip::background, 51400us);
  stats.skipped(Skip::secureHold, 16ms); stats.skipped(Skip::secureNotice, 200ms); stats.skipped(Skip::secureHelper, 17ms);
  stats.skipped(Skip::follow, 1500ms);
  stats.skipped(Skip::secureHold, -5ms);  // a clock that went back counts the tick, not negative time
  stats.overshoot(900us); stats.overshoot(4200us); stats.overshoot(-3us);
  stats.tail(300us); stats.tail(12ms);
  const auto json = stats.takeJson(2500);
  require(json == ",\"backgroundTicks\":2,\"backgroundMs\":101,\"secureHoldTicks\":2,\"secureHoldMs\":16,\"secureNoticeTicks\":1,\"secureNoticeMs\":200,"
                  "\"secureHelperTicks\":1,\"secureHelperMs\":17,\"follows\":1,\"followMs\":1500,\"overshootMaxUs\":4200,\"tailMaxUs\":12000,\"pipeWriteMaxUs\":2500",
          "Window members differ");
  require(stats.takeJson().find("\"backgroundTicks\":0,\"backgroundMs\":0") != std::string::npos && stats.takeJson().find("\"tailMaxUs\":0") != std::string::npos,
          "A report did not reset the window");
}

void bounds() {
  host::CaptureLoopStats stats;
  stats.skipped(Skip::follow, std::chrono::hours(1)); stats.overshoot(std::chrono::hours(1)); stats.tail(std::chrono::hours(1));
  const auto json = stats.takeJson(uint64_t(1) << 60);
  require(json.find("\"followMs\":600000,") != std::string::npos && json.find("\"overshootMaxUs\":600000000,") != std::string::npos &&
              json.find("\"tailMaxUs\":600000000,") != std::string::npos && json.ends_with("\"pipeWriteMaxUs\":600000000"),
          "Values are not bounded");
}

void pipeWriteMax() {
  host::PipeWriteMax longest;
  require(longest.take() == 0, "A fresh maximum is not 0");
  longest.observe(250us); longest.observe(90us); longest.observe(-1us);
  require(longest.take() == 250 && longest.take() == 0, "The maximum or its reset is wrong");
  // Concurrent writers (capture, audio and control threads): the largest survives.
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t) writers.emplace_back([&longest, t] { for (int n = 0; n < 20000; ++n) longest.observe(std::chrono::microseconds(n % 1000 + t * 1000)); });
  for (auto& writer : writers) writer.join();
  require(longest.take() == 3999, "A concurrent maximum was lost");
}
}  // namespace

int main() {
  const struct { const char* name; void (*run)(); } cases[] = {{"empty-window", emptyWindow}, {"reasons-and-reset", reasonsAndReset}, {"bounds", bounds}, {"pipe-write-max", pipeWriteMax}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
  std::cout << "{\"kind\":\"capture-loop-stats\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << "}\n";
  return failed ? 1 : 0;
}
