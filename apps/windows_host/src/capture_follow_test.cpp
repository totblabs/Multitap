// capture_follow.hpp: what the capture thread follows when an application
// changes the stream's own output mode (a game's exclusive fullscreen at
// another resolution, and back), what stays fatal, the settle/pacing rule, and
// the worker's notice when a capture still fails after its stream started.
// Pure: no display, capture, input, encoder or network is touched.
#include "capture_follow.hpp"
#include "media.hpp"
#include "stream_prepare_diagnostics.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace host;
namespace {
unsigned checks = 0;
void require(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
const std::wstring name = L"\\\\.\\DISPLAY3", identity = L"MONITOR\\TOTB0001|{4d36e96e}\\0003";
CaptureMonitor monitor(RECT bounds, const std::wstring& device = name, const std::wstring& id = identity) { return {device, id, bounds, true}; }

void followsTheOwnedOutput() {
  const auto selected = monitor({0, 0, 2560, 1440});
  const RECT game{0, 0, 1920, 1080};
  const auto followed = followedOwnedOutput(selected, {monitor(game)}, game);
  require(followed && sameRect(*followed, game), "The owned output at a game's resolution was not followed");
  const auto back = followedOwnedOutput(selected, {monitor({0, 0, 2560, 1440})}, RECT{0, 0, 2560, 1440});
  require(back && sameRect(*back, RECT{0, 0, 2560, 1440}), "The output back at its stream size was not followed");
  // Larger than the stream (a 1280x720 window, a 1920x1080 game) is followed too.
  const auto larger = followedOwnedOutput(monitor({0, 0, 1280, 720}), {monitor(game)}, game);
  require(larger && sameRect(*larger, game), "A larger mode was not followed");
  // A neighbour that does not overlap the output does not stop it.
  require(followedOwnedOutput(selected, {monitor(game), monitor({1920, 0, 3200, 1024}, L"\\\\.\\DISPLAY1", L"QEMU|0000")}, game).has_value(), "A non-overlapping display stopped the follow");
}

void refusesWhatIsNotTheSameOutput() {
  const auto selected = monitor({0, 0, 2560, 1440});
  const RECT game{0, 0, 1920, 1080};
  require(!followedOwnedOutput(selected, {monitor(game)}, std::nullopt), "Followed without a current display mode");
  require(!followedOwnedOutput(selected, {monitor(game)}, RECT{0, 0, 2560, 1440}), "Followed while the mode and the monitor disagree (between modes)");
  require(!followedOwnedOutput(selected, {monitor(game, name, L"OTHER|KEY")}, game), "Followed another device on the same GDI name");
  require(!followedOwnedOutput(selected, {monitor(game, L"\\\\.\\DISPLAY4")}, game), "Followed a monitor with another name");
  require(!followedOwnedOutput(selected, {}, game), "Followed a missing output");
  require(!followedOwnedOutput(selected, {monitor(game), monitor(game)}, game), "Followed an ambiguous output");
  require(!followedOwnedOutput(selected, {monitor(game), monitor({1000, 0, 2024, 768}, L"\\\\.\\DISPLAY1", L"QEMU|0000")}, game), "Followed an output another display overlaps");
  require(!followedOwnedOutput(monitor({0, 0, 2560, 1440}, name, L""), {monitor(game, name, L"")}, game), "Followed without a device identity");
}

void settlesBeforeFollowing() {
  using Clock = std::chrono::steady_clock;
  std::atomic<bool> stop{false};
  auto now = Clock::time_point{} + std::chrono::hours(1);
  const auto deadline = now + captureFollowSettle;
  unsigned waits = 0;
  const auto wait = [&](std::chrono::milliseconds step) { ++waits; now += step; };
  const auto clock = [&] { return now; };
  // Windows lands a mode change in steps: nothing, an intermediate size, then the final one twice.
  std::vector<std::optional<RECT>> seen{std::nullopt, RECT{0, 0, 2560, 1440}, RECT{0, 0, 1920, 1080}, RECT{0, 0, 1920, 1080}};
  size_t at = 0;
  const auto settled = settledOwnedOutput([&] { return seen[std::min(at++, seen.size() - 1)]; }, wait, clock, deadline, stop);
  require(settled && sameRect(*settled, RECT{0, 0, 1920, 1080}) && waits == 3, "The output was not followed once two observations agreed");
  // An output that keeps changing (or stays unreadable) gives up at the deadline, never later.
  now = Clock::time_point{} + std::chrono::hours(2); waits = 0; at = 0;
  const auto start = now;
  const auto flapping = settledOwnedOutput([&] { const LONG side = (at++ % 2) ? 1920 : 2560; return std::optional<RECT>(RECT{0, 0, side, 1080}); }, wait, clock, start + captureFollowSettle, stop);
  require(!flapping && now <= start + captureFollowSettle && waits == unsigned(captureFollowSettle / captureFollowStep),"A flapping output was followed or waited past the deadline");
  now = start; waits = 0;
  require(!settledOwnedOutput([] { return std::optional<RECT>(); }, wait, clock, start + captureFollowSettle, stop), "An unreadable output was followed");
  stop = true; waits = 0;
  require(!settledOwnedOutput([] { return std::optional<RECT>(RECT{0, 0, 1920, 1080}); }, wait, clock, start + captureFollowSettle, stop) && waits == 0, "A stopping stream kept waiting");
  require(captureFollowGap >= std::chrono::milliseconds(500) && captureFollowSettle < std::chrono::seconds(15), "Pacing must stay under the 15 s relay/client timeouts");
}

// Independent outputs settle together: Windows moves the second output in a
// later step than the first one's mode change.
void settlesEveryOutput() {
  using Clock = std::chrono::steady_clock;
  std::atomic<bool> stop{false};
  auto now = Clock::time_point{} + std::chrono::hours(1);
  unsigned waits = 0;
  const auto wait = [&](std::chrono::milliseconds step) { ++waits; now += step; };
  const RECT first{0, 0, 2560, 1440}, second{2560, 0, 4480, 1080}, game{0, 0, 1920, 1080}, moved{1920, 0, 3840, 1080};
  std::vector<std::optional<std::vector<RECT>>> seen{std::vector<RECT>{game, second}, std::vector<RECT>{game, moved}, std::vector<RECT>{game, moved}};
  size_t at = 0;
  const auto settled = settledOwnedOutputs([&] { return seen[std::min(at++, seen.size() - 1)]; }, wait, [&] { return now; }, now + captureFollowSettle, stop);
  require(settled && settled->size() == 2 && sameRect((*settled)[0], game) && sameRect((*settled)[1], moved) && waits == 2, "The outputs were followed before every one of them settled");
  at = 0; waits = 0; seen = {std::vector<RECT>{first, second}, std::nullopt, std::vector<RECT>{first, second}, std::vector<RECT>{first, second}};
  const auto unreadable = settledOwnedOutputs([&] { return seen[std::min(at++, seen.size() - 1)]; }, wait, [&] { return now; }, now + captureFollowSettle, stop);
  require(unreadable && waits == 3, "An output unreadable between two observations counted as settled");
}

void failureNoticeAfterTheStreamStarted() {
  const std::string fields = ",\"sessionId\":\"s-1\",\"epoch\":3,\"requestId\":\"r\",\"resizeId\":\"z\"";
  // Before its stream.info, a resize restart's failure is the resize's.
  require(captureFailureNotice(true, false, fields, "s-1", "capture_failed") == "{\"type\":\"stream.resize.failed\",\"reason\":\"resize_failed\"" + fields + "}", "A failing resize restart did not report the resize");
  // After it the service has completed the resize and would drop a resize
  // failure (the 15 s freeze): the stream itself is unavailable.
  require(captureFailureNotice(true, true, fields, "s-1", "capture_failed") == "{\"type\":\"stream.unavailable\",\"sessionId\":\"s-1\",\"reason\":\"capture_failed\"}", "A failure after the resized stream started was reported as a resize failure");
  require(captureFailureNotice(false, true, "", "s-2", "encoder_failed") == "{\"type\":\"stream.unavailable\",\"sessionId\":\"s-2\",\"reason\":\"encoder_failed\"}", "A plain stream's failure changed");
  require(captureFailureNotice(false, false, "", "s-3", "no_interactive_session") == "{\"type\":\"stream.unavailable\",\"sessionId\":\"s-3\",\"reason\":\"no_interactive_session\"}", "An early plain failure changed");
  // The followed WGC size change keeps its fixed diagnostic code.
  require(streamCaptureFailureCode(CaptureGeometryChanged().what()) == 2, "The geometry change lost its diagnostic code");
}
}

int main() {
  const struct { const char* name; void (*run)(); } cases[] = {{"follows-owned-output", followsTheOwnedOutput}, {"refuses-other-output", refusesWhatIsNotTheSameOutput},
    {"settles-before-following", settlesBeforeFollowing}, {"settles-every-output", settlesEveryOutput}, {"failure-notice-after-start", failureNoticeAfterTheStreamStarted}};
  unsigned passed = 0, failed = 0;
  for (const auto& value : cases) try { value.run(); ++passed; } catch (const std::exception& error) { ++failed; std::cerr << value.name << ": " << error.what() << '\n'; }
  std::cout << "{\"kind\":\"capture-follow\",\"passed\":" << passed << ",\"failed\":" << failed << ",\"checks\":" << checks << ",\"displayChanged\":false,\"captureStarted\":false}\n";
  return failed ? 1 : 0;
}
