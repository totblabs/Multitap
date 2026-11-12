// Resize stage fields (resize_stages.hpp): the worker's stages travel in the
// restart command and come back, with the capture thread's, as fixed numbers.
// Pure checks; nothing here captures, encodes or changes a display.
#include "resize_stages.hpp"
#include <iostream>
using namespace host;
namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
}
int main() {
  try {
    int passed = 0;
    ResizeStages worker; worker.suspendMs = 12; worker.displayMs = 640; worker.sdkMs = 310; worker.settleMs = 280; worker.confirmMs = 45;
    auto command = JsonParser::parse(R"({"type":"session.start","sessionId":"s","resizeId":"r","epoch":2})");
    const auto restart = withResizeStages(command, worker);
    require(command.get("resizeStages").get("displayMs").number(-1) == -1, "The stored stream command kept the stages");
    const auto read = resizeStagesFrom(restart);
    require(read.suspendMs == 12 && read.displayMs == 640 && read.sdkMs == 310 && read.settleMs == 280 && read.confirmMs == 45 && !read.startMs && !read.captureMs && !read.encoderMs && !read.audioMs, "Worker stages did not survive the restart command");
    ++passed;
    // The capture thread's report: every field, fixed names, integers.
    auto all = read; all.startMs = 95; all.captureMs = 30; all.encoderMs = 22; all.audioMs = 4;
    const auto json = resizeStagesJson(all);
    require(json == R"({"suspendMs":12,"displayMs":640,"sdkMs":310,"settleMs":280,"confirmMs":45,"startMs":95,"captureMs":30,"encoderMs":22,"audioMs":4})", "Stage report changed shape");
    const auto info = JsonParser::parse(R"({"type":"stream.info","resizeStages":)" + json + "}");
    const auto back = resizeStagesFrom(info);
    require(back.startMs == 95 && back.audioMs == 4 && back.displayMs == 640, "Stage report did not parse back");
    ++passed;
    // Missing, negative, fractional, huge or textual values read as 0; a
    // command without stages (initial start, older worker) reads all zeros.
    const auto odd = resizeStagesFrom(JsonParser::parse(R"({"resizeStages":{"suspendMs":-1,"displayMs":1.5,"sdkMs":700000,"settleMs":"9","confirmMs":null,"startMs":600000}})"));
    require(!odd.suspendMs && !odd.displayMs && !odd.sdkMs && !odd.settleMs && !odd.confirmMs && odd.startMs == 600000, "Invalid stage values were accepted");
    const auto none = resizeStagesFrom(JsonParser::parse(R"({"type":"session.start"})"));
    require(resizeStagesJson(none) == R"({"suspendMs":0,"displayMs":0,"sdkMs":0,"settleMs":0,"confirmMs":0,"startMs":0,"captureMs":0,"encoderMs":0,"audioMs":0})", "A start without stages reported some");
    ++passed;
    // Durations are bounded and never negative.
    const auto now = std::chrono::steady_clock::now();
    require(resizeStageMs(now, now - std::chrono::seconds(1)) == 0 && resizeStageMs(now, now + std::chrono::hours(1)) == resizeStageMaxMs && resizeStageMs(now, now + std::chrono::milliseconds(781)) == 781 && resizeStageMs(uint64_t(1) << 40) == resizeStageMaxMs, "Stage durations were not bounded");
    ++passed;
    std::cout << "Resize stage checks passed: " << passed << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Resize stage check failed: " << error.what() << "\n";
    return 1;
  }
}
