// Memory over days (backlog #71): state the service keeps for a whole run or a
// whole stream, driven through many sessions and requests with fakes, must stay
// bounded. Covered: the WinFsp bridge's unmount answers (one per share removal
// were kept for the helper's life: a days-long stream grew without bound), the
// diagnostics delivery ring, and the stream observation log buffer.
// No helper process, WinFsp, broker or file is used: a fake helper answers on
// in-memory pipes.
#include "drive_winfsp_service.hpp"
#include "host_diagnostics.hpp"
#include "host_observation_log.hpp"
#include <iostream>

using namespace host;
namespace {
int checks = 0;
void require(bool ok, const std::string& what) { ++checks; if (!ok) throw std::runtime_error(what); }
class Pipe {
 public:
  void write(const uint8_t* data, size_t size) { std::lock_guard lock(mutex_); if (closed_) return; bytes_.insert(bytes_.end(), data, data + size); changed_.notify_all(); }
  size_t read(uint8_t* out, size_t size) { std::unique_lock lock(mutex_); changed_.wait(lock, [&] { return closed_ || !bytes_.empty(); }); if (bytes_.empty()) return 0; const size_t n = std::min(size, bytes_.size()); for (size_t i = 0; i < n; ++i) { out[i] = bytes_.front(); bytes_.pop_front(); } return n; }
  void close() { std::lock_guard lock(mutex_); closed_ = true; changed_.notify_all(); }
 private:
  std::mutex mutex_; std::condition_variable changed_; std::deque<uint8_t> bytes_; bool closed_ = false;
};
struct NoFiles : drive::Backend { std::future<drive::Result> submit(drive::Call) override { std::promise<drive::Result> done; drive::Result result; result.error = "drive_unavailable"; done.set_value(result); return done.get_future(); } };

// A helper that mounts every share at Z: and answers each unmount; every tenth
// answer comes after the service stopped waiting, and every seventh unmount
// also brings an answer for a token nobody asked about.
void driveUnmountAnswers() {
  Pipe toHelper, toService; NoFiles files;
  drive::fs::FsBridge bridge(files, [&](uint8_t* b, size_t n) { return toService.read(b, n); }, [&](const std::vector<uint8_t>& bytes) { toHelper.write(bytes.data(), bytes.size()); return true; }, [&] { toHelper.close(); });
  const auto answer = [&](const std::string& json) { const auto bytes = drive::wire::encode(drive::wire::controlFrame(json)); toService.write(bytes.data(), bytes.size()); };
  std::atomic<int> unmounts{0};
  std::thread helper([&] {
    drive::wire::Frame frame;
    const auto read = [&](uint8_t* b, size_t n) { return toHelper.read(b, n); };
    while (drive::wire::readFrame(read, frame)) {
      const auto value = JsonParser::parse(frame.header); const auto type = value.get("type").string(), token = value.get("token").string();
      if (type == "mount") answer("{\"type\":\"mounted\",\"token\":" + jsonQuote(token) + ",\"letter\":\"Z\"}");
      else if (type == "unmount") {
        const int n = ++unmounts;
        if (n % 10 == 0) Sleep(40);  // the service waited 20 ms: a late answer
        answer("{\"type\":\"unmounted\",\"token\":" + jsonQuote(token) + "}");
        if (n % 7 == 0) answer("{\"type\":\"unmounted\",\"token\":\"stray-" + std::to_string(n) + "\"}");
      } else if (type == "stop") break;
    }
  });
  answer("{\"type\":\"ready\"}");
  // The helper thread is joined on every path, also when a check fails.
  struct End { drive::fs::FsBridge& bridge; std::thread& helper; Pipe& toService; ~End() { bridge.signalStop(); if (helper.joinable()) helper.join(); toService.close(); bridge.stop(); } } end{bridge, helper, toService};
  require(bridge.waitReady(std::chrono::seconds(2)).empty(), "Fake helper not ready");
  size_t widest = 0; int late = 0;
  for (int i = 1; i <= 2000; ++i) {
    const std::string token = "share-token-" + std::to_string(i);
    const auto mounted = bridge.mount({token, "docs", "Documents", false}, std::chrono::seconds(5));
    require(mounted.letter == "Z", "Fake mount failed at " + std::to_string(i));
    if (!bridge.unmount(token, i % 10 == 0 ? std::chrono::milliseconds(20) : std::chrono::seconds(5))) ++late;
    widest = std::max(widest, bridge.unmountRecords());
  }
  Sleep(100);  // the last late answers arrive
  const auto held = bridge.unmountRecords();
  std::cout << "WinFsp bridge unmount records after 2000 share removals (" << late << " answered late): " << held << ", widest " << widest << '\n';
  require(late >= 150, "The fixture's late answers did not happen");
  require(held == 0 && widest <= 2, "Unmount answers were kept: " + std::to_string(held) + " now, " + std::to_string(widest) + " at most");
}

// 2000 reports with fresh rows, each acknowledged: the delivered-row memory stays at its ring.
void diagnosticsDelivery() {
  HostDiagnosticDelivery delivery;
  for (int i = 0; i < 2000; ++i) { delivery.sent({"row-" + std::to_string(i * 2), "row-" + std::to_string(i * 2 + 1)}, false); delivery.acknowledged(true); }
  require(delivery.delivered().size() <= 256, "Delivered diagnostics rows grew to " + std::to_string(delivery.delivered().size()));
}

// 1000 streams, each with a few stats windows and a finish: the buffered log stays at 16 records.
void observationLog() {
  operationalLogEnabled = false;  // never writes; the buffer itself is exercised
  HostObservationLogBuffer buffer; uint64_t now = 1000;
  for (int session = 0; session < 1000; ++session) {
    char id[40]; std::snprintf(id, sizeof(id), "00000000-0000-4000-8000-%012d", session);
    for (int window = 0; window < 3; ++window) {
      now += 31000;
      buffer.observe(JsonParser::parse(std::string("{\"type\":\"stream.stats\",\"sessionId\":\"") + id + "\",\"epoch\":1,\"observability\":{\"protocol\":\"" + streamObservabilityProtocol + "\",\"metrics\":{\"hostQueuePackets\":3}}}"), now);
    }
    buffer.finish(id, now);
  }
  size_t drained = 0; while (buffer.pending()) buffer.drain([&](const std::string&) { ++drained; });
  std::cout << "Observation log records held after 1000 streams: " << drained << " (dropped " << buffer.dropped() << ")\n";
  require(drained <= 16 && buffer.dropped() > 0, "The observation log buffer grew to " + std::to_string(drained));
}
}

int main() {
  int failures = 0;
  for (const auto& [name, part] : std::initializer_list<std::pair<const char*, void (*)()>>{{"drive unmount answers", driveUnmountAnswers}, {"diagnostics delivery", diagnosticsDelivery}, {"observation log", observationLog}}) {
    try { part(); } catch (const std::exception& error) { ++failures; std::cout << "FAIL " << name << ": " << error.what() << '\n'; }
  }
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
