// Latency and throughput of the WinFsp drive path on loopback fakes (docs/drive-redirection.md, "WinFsp backend"): the real ShareFs
// callbacks through the real interface table, the fake WinFsp and in-memory PC of drive_winfsp_test.cpp, and a link that answers
// after a fixed round trip, carries at most eight requests at once (the relay's bound) and shares one bandwidth. Nothing is mounted.
//   MultitapHostDriveFsBench.exe        prints one JSON line per scenario and round-trip time
#define main driveFsBenchFixtureMainNotInvoked
#include "drive_winfsp_test.cpp"
#undef main
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

namespace {
// The relay and the PC as the share sees them: answers arrive rtt after the request, data also waits for the shared bandwidth.
class Link final : public Backend {
 public:
  Link(Mem& pc, std::chrono::microseconds rtt, double bytesPerSecond) : pc_(pc), rtt_(rtt), bytesPerSecond_(bytesPerSecond) {
    for (size_t i = 0; i < maxInFlight; ++i) workers_.emplace_back([this] { run(); });
  }
  ~Link() override { { std::lock_guard lock(mutex_); stop_ = true; } wake_.notify_all(); for (auto& t : workers_) t.join(); }
  std::future<Result> submit(Call call) override {
    auto promise = std::make_shared<std::promise<Result>>(); auto future = promise->get_future();
    { std::lock_guard lock(mutex_); ++counts[int(call.op)]; queue_.push_back({std::move(call), promise, Clock::now()}); }
    wake_.notify_one(); return future;
  }
  size_t count(Op op) { std::lock_guard lock(mutex_); return counts[int(op)]; }
  size_t total() { std::lock_guard lock(mutex_); size_t n = 0; for (auto c : counts) n += c; return n; }
 private:
  struct Job { Call call; std::shared_ptr<std::promise<Result>> promise; Clock::time_point sent; };
  void run() {
    for (;;) {
      Job job; { std::unique_lock lock(mutex_); wake_.wait(lock, [this] { return stop_ || !queue_.empty(); }); if (stop_) return; job = std::move(queue_.front()); queue_.pop_front(); }
      Result result = pc_.submit(job.call).get();
      const size_t bytes = result.data.size() + job.call.data.size();
      Clock::time_point arrives;
      { std::lock_guard lock(mutex_);
        const auto start = std::max(std::max(job.sent, Clock::now()) + rtt_ / 2, linkFree_);
        linkFree_ = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(bytes) / bytesPerSecond_));
        arrives = linkFree_ + rtt_ / 2; }
      std::this_thread::sleep_until(arrives);
      job.promise->set_value(std::move(result));
    }
  }
  Mem& pc_; std::chrono::microseconds rtt_; double bytesPerSecond_;
  std::mutex mutex_; std::condition_variable wake_; std::deque<Job> queue_; bool stop_ = false; Clock::time_point linkFree_{};
  std::vector<std::thread> workers_; size_t counts[10]{};
};
double msSince(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
void line(const char* scenario, int rttMs, double ms, size_t calls, double mbps = 0) {
  std::cout << "{\"scenario\":\"" << scenario << "\",\"rttMs\":" << rttMs << ",\"ms\":" << ms << ",\"backendCalls\":" << calls;
  if (mbps > 0) std::cout << ",\"MBps\":" << mbps;
  std::cout << "}\n";
}
void scenarios(int rttMs) {
  Mem pc; pc.page = 256; pc.dir("folder"); for (int i = 0; i < 500; ++i) pc.file("folder/f" + std::to_string(i) + ".txt", "x"); pc.sparse("movie.bin", 64ULL << 20);
  const auto fresh = [&](auto&& body) {
    Link link(pc, std::chrono::milliseconds(rttMs), 100e6); FakeFsp fsp; const auto api = fsp.api(); auto o = options(); o.callTimeout = std::chrono::seconds(60);
    fs::ShareFs share(api, link, o); std::wstring letter; if (share.mount(letter)) throw std::runtime_error("mount");
    Vol v(fsp); body(v, link); share.unmount();
  };
  // Opening one file: the lookup (GetSecurityByName), the open and the close, as CreateFile does.
  fresh([&](Vol& v, Link& link) {
    const auto start = Clock::now(); UINT32 attr = 0; PVOID c = nullptr; FSP_FSCTL_FILE_INFO info{};
    v.sec(L"\\folder\\f1.txt", &attr); v.open(L"\\folder\\f1.txt", &c, &info); v.cleanup(c, L"\\folder\\f1.txt", 0); v.close(c);
    line("open", rttMs, msSince(start), link.total());
  });
  // An Explorer folder view: list 500 files, then look up and open each one (icons, properties).
  fresh([&](Vol& v, Link& link) {
    const auto start = Clock::now(); PVOID d = nullptr; FSP_FSCTL_FILE_INFO info{};
    v.open(L"\\folder", &d, &info); const auto names = v.list(d); v.close(d);
    for (const auto& name : names) {
      if (name == L"." || name == L"..") continue;
      const auto path = L"\\folder\\" + name; UINT32 attr = 0; PVOID c = nullptr;
      v.sec(path.c_str(), &attr); v.open(path.c_str(), &c, &info); v.close(c);
    }
    line("folder-500", rttMs, msSince(start), link.total());
    // The same folder listed again at once (another window, a refresh).
    const size_t before = link.total(); const auto again = Clock::now();
    v.open(L"\\folder", &d, &info); (void)v.list(d); v.close(d);
    line("relist-500", rttMs, msSince(again), link.total() - before);
  });
  // Sequential reads of a 64 MiB file, as an application (64 KiB) and as a copy (1 MiB) reads it.
  for (const ULONG size : {ULONG(64 << 10), ULONG(1 << 20)}) fresh([&](Vol& v, Link& link) {
    PVOID c = nullptr; FSP_FSCTL_FILE_INFO info{}; v.open(L"\\movie.bin", &c, &info); std::string out;
    const auto start = Clock::now(); uint64_t total = 0;
    for (uint64_t at = 0; at < (64ULL << 20); at += size) { if (v.read(c, out, at, size)) throw std::runtime_error("read"); total += out.size(); }
    const double ms = msSince(start); v.close(c);
    line(size == (64 << 10) ? "read-64k" : "read-1m", rttMs, ms, link.count(Op::read), double(total) / 1e3 / ms);
  });
}
}

int wmain() {
  // 1 ms sleeps for the link's round trips (this process only, Windows 10 2004 and later), not the default 15.6 ms tick.
  timeBeginPeriod(1);
  try { for (const int rtt : {1, 10, 30}) scenarios(rtt); timeEndPeriod(1); return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; timeEndPeriod(1); return 1; }
}
