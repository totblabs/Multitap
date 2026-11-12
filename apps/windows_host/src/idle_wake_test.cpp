// Idle wakeups of the boot service's always-on threads (backlog #70). Each part
// starts one real component in this process, lets it reach its idle wait and
// counts how often its threads were scheduled (context switches, wake_counter.hpp)
// over a window. It also checks that the component still reacts at once: a pipe
// client is served, a saved request is seen, a stop returns promptly.
// Only QA pipe names, a private temporary folder and substituted request passes
// are used; no service, registration, display or broker is touched.
#include "native_recovery_reset.hpp"
#include "native_display_files.hpp"
#include "pending_connection.hpp"
#include "resource_sampler.hpp"
#include "capability_inputs.hpp"
#include "display_cache.hpp"
#include "display_change_signals.hpp"
#include "print_redirection.hpp"
#include "drive_winfsp_service.hpp"
#include "wake_counter.hpp"
#include <iostream>
#include <sddl.h>
#include <aclapi.h>

using namespace host;
namespace {
int checks = 0;
void require(bool ok, const std::string& what) { ++checks; if (!ok) throw std::runtime_error(what); }
std::wstring ownSid() {
  HANDLE raw = nullptr; require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE, "QA token"); WinHandle token(raw);
  DWORD size = 0; GetTokenInformation(raw, TokenUser, nullptr, 0, &size); std::vector<uint8_t> bytes(size);
  require(GetTokenInformation(raw, TokenUser, bytes.data(), size, &size) != FALSE, "QA user");
  LPWSTR sid = nullptr; require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid) != FALSE, "QA SID");
  std::wstring result(sid); LocalFree(sid); return result;
}
std::wstring unique() { return std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()); }
// Wakeups of the threads started after `baseline` during `windowMs`.
uint64_t idleWakeups(const std::set<uint32_t>& baseline, DWORD windowMs) {
  const auto before = wake::sampleProcess(GetCurrentProcessId()); Sleep(windowMs);
  return wake::wakeups(before, wake::sampleProcess(GetCurrentProcessId()), baseline);
}
template<class F> uint64_t elapsedMs(F&& body) { const auto began = GetTickCount64(); body(); return GetTickCount64() - began; }
constexpr DWORD window = 3000;

void brokerStatusServer() {
  const auto endpoint = L"\\\\.\\pipe\\MultitapHost.BrokerStatus.QA." + unique();
  BrokerConnectionStatus status; const auto generation = status.begin(); status.upgraded(generation); status.acknowledged(generation, true);
  const auto baseline = wake::currentThreads();
  auto server = std::make_unique<BrokerStatusServer>(status, endpoint, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + ownSid() + L")");
  Sleep(300);
  const auto wakes = idleWakeups(baseline, window);
  std::cout << "BrokerStatusServer idle wakeups per minute: " << wakes * 60000 / window << '\n';
  require(wakes <= 2, "Idle broker status endpoint woke " + std::to_string(wakes) + " times in 3 s");
  BrokerEvidence read; const auto served = elapsedMs([&] { read = readBrokerStatusPipe(endpoint, GetCurrentProcessId(), brokerProcessCreation(GetCurrentProcess()), false); });
  require(read.phase == BrokerPhase::Online && served < 1000, "Broker status client was not served at once");
  require(elapsedMs([&] { server.reset(); }) < 500, "Broker status endpoint did not stop at once");
}

void nativeResetServer() {
  const auto endpoint = L"\\\\.\\pipe\\MultitapHost.NativeReset.QA." + unique();
  const auto baseline = wake::currentThreads();
  std::atomic<int> verified{0};
  auto server = std::make_unique<NativeRecoveryResetServer>([](HANDLE, const Json&) { return std::string(); }, [&](HANDLE) { ++verified; return false; }, endpoint, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + ownSid() + L")");
  Sleep(300);
  const auto wakes = idleWakeups(baseline, window);
  std::cout << "NativeRecoveryResetServer idle wakeups per minute: " << wakes * 60000 / window << '\n';
  require(wakes <= 2, "Idle native reset endpoint woke " + std::to_string(wakes) + " times in 3 s");
  std::optional<Json> reply; const auto served = elapsedMs([&] {
    require(WaitNamedPipeW(endpoint.c_str(), 1000) != FALSE, "Native reset endpoint missing");
    WinHandle pipe(CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    require(pipe.value != INVALID_HANDLE_VALUE, "Native reset endpoint refused the QA client");
    reply = nativeResetReceive(pipe.value, 2000); });
  require(reply && reply->get("type").string() == "native.reset.result" && !reply->get("ok").boolean() && verified == 1 && served < 1000, "Native reset client was not answered at once");
  require(elapsedMs([&] { server.reset(); }) < 500, "Native reset endpoint did not stop at once");
}

void saveRequest(const std::filesystem::path& path) {
  // The same atomic save as saveConnectionRequest: a new file, then a rename.
  const auto temporary = path.wstring() + L".tmp";
  { std::ofstream file(temporary, std::ios::binary); file << "request"; }
  require(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE, "QA request save failed");
}
template<class F> uint64_t untilTrue(F&& done, DWORD limitMs) { const auto began = GetTickCount64(); while (!done() && GetTickCount64() - began < limitMs) Sleep(10); return GetTickCount64() - began; }

void pendingConnectionWorker() {
  const auto folder = std::filesystem::temp_directory_path() / (L"MultitapHost.IdleQA." + unique());
  std::filesystem::create_directories(folder);
  struct Remove { std::filesystem::path path; ~Remove() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } remove{folder};
  const auto request = folder / L"connection-request.dpapi";
  std::atomic<int> ticks{0}; std::atomic<bool> active{true}; std::atomic<int> changes{0};
  PendingConnectionWatch watch; watch.request = request; watch.tick = [&] { ++ticks; return PendingConnectionTick{false, active.load()}; };
  const auto baseline = wake::currentThreads();
  auto worker = std::make_unique<PendingConnectionWorker>([&] { ++changes; }, watch);
  Sleep(300);
  // A registered Host: no request file. Nothing to do, so nothing wakes.
  const auto idle = idleWakeups(baseline, window);
  std::cout << "PendingConnectionWorker idle wakeups per minute (no request): " << idle * 60000 / window << '\n';
  require(idle <= 2 && ticks == 0, "Idle request worker woke " + std::to_string(idle) + " times in 3 s");
  // Host settings saves a request: the worker sees it at once.
  saveRequest(request);
  const auto seen = untilTrue([&] { return ticks > 0; }, 3000);
  std::cout << "PendingConnectionWorker new request seen after ms: " << seen << '\n';
  require(ticks > 0 && seen < 500, "A saved request was not seen at once");
  // An active request keeps its one-second pass (broker retries, approval).
  const int before = ticks; Sleep(3100);
  require(ticks - before >= 2 && ticks - before <= 4, "An active request lost its one-second pass: " + std::to_string(ticks - before));
  // A finished request (committed, rejected, expired): no more passes until the file changes.
  active = false; Sleep(1200); const int settled = ticks;
  const auto finished = idleWakeups(baseline, window);
  require(ticks == settled && finished <= 2, "A finished request kept the worker waking: " + std::to_string(finished));
  active = true; saveRequest(request);
  require(untilTrue([&] { return ticks > settled; }, 3000) < 500, "A changed request was not seen at once");
  require(elapsedMs([&] { worker.reset(); }) < 500, "Request worker did not stop at once");
  // Without a folder to watch it keeps the one-second check and still finds a later request.
  const auto missing = folder / L"later" / L"connection-request.dpapi";
  PendingConnectionWatch unwatched; unwatched.request = missing; unwatched.tick = [&] { ++ticks; return PendingConnectionTick{false, false}; };
  const int prior = ticks;
  auto fallback = std::make_unique<PendingConnectionWorker>([] {}, unwatched);
  Sleep(500); std::filesystem::create_directories(missing.parent_path()); saveRequest(missing);
  require(untilTrue([&] { return ticks > prior; }, 3000) < 1600, "A request in a folder created later was not found");
  require(elapsedMs([&] { fallback.reset(); }) < 500, "Unwatched request worker did not stop at once");
  require(changes == 0, "A substituted pass reported a registration change");
}

uint64_t ownSwitches() {
  const auto tid = GetCurrentThreadId();
  for (const auto& thread : wake::sampleProcess(GetCurrentProcessId()).threads) if (thread.tid == tid) return thread.switches;
  throw std::runtime_error("Own thread not sampled");
}
struct TempFolder {
  std::filesystem::path path = std::filesystem::temp_directory_path() / (L"MultitapHost.IdleQA." + unique());
  TempFolder() { std::filesystem::create_directories(path); }
  ~TempFolder() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

// The service loop's native display snapshot (native_display_service.hpp
// snapshot()): one simulated minute of passes 1.000-1.031 s apart with nothing
// changing. Each pass used to write and flush it. Then the real write, flushed
// and not, in a temporary folder: what the loop's minute costs in wakeups.
void nativeSnapshotWrites() {
  const auto state = [](uint64_t tick, bool pending) { return "{\"schema\":1,\"servicePid\":4,\"generation\":7,\"tick\":" + std::to_string(tick) + ",\"leaseActive\":true,\"configurationPending\":" + (pending ? "true" : "false") + ",\"targets\":[]}"; };
  NativeSnapshotPublication publication; uint64_t now = 100000, writtenAt = 0, oldest = 0; int changes = 0, refreshes = 0; uint32_t jitter = 777;
  for (int pass = 0; pass < 60; ++pass) {
    const auto bytes = state(now, false); const auto write = publication.plan(bytes, now);
    if (write != NativeSnapshotWrite::none) { (write == NativeSnapshotWrite::change ? changes : refreshes)++; publication.written(bytes, now); writtenAt = now; }
    jitter = jitter * 1103515245 + 12345; const uint64_t next = now + 1000 + (jitter >> 16) % 32;
    oldest = std::max(oldest, next - writtenAt);  // what a reader sees just before the next pass
    now = next;
  }
  std::cout << "Native display snapshot writes per minute, nothing changing: " << changes << " flushed + " << refreshes << " unflushed (was 60 flushed); oldest tick a reader saw " << oldest << " ms\n";
  require(changes == 1 && refreshes >= 25 && refreshes <= 31, "Unchanged snapshot writes: " + std::to_string(changes) + " flushed, " + std::to_string(refreshes) + " unflushed");
  require(oldest + 2000 < nativeDisplaySnapshotMaxAgeMs, "A reader could see a stale snapshot tick: " + std::to_string(oldest) + " ms");
  require(publication.plan(state(now, true), now) == NativeSnapshotWrite::change, "A changed snapshot waited for its refresh");
  publication.failed();
  require(publication.plan(state(now, false), now) == NativeSnapshotWrite::change, "A failed write was not written (and flushed) again at once");
  TempFolder folder; const auto path = folder.path / L"native-display-state.json"; const auto bytes = state(GetTickCount64(), false);
  const auto cost = [&](bool durable) {
    constexpr int writes = 20; const auto before = ownSwitches();
    for (int i = 0; i < writes; ++i) require(writeNativeDisplaySnapshot(path, bytes, durable), "Snapshot write failed");
    return double(ownSwitches() - before) / writes;
  };
  const double flushed = cost(true), unflushed = cost(false);
  std::ifstream file(path, std::ios::binary); require(std::string((std::istreambuf_iterator<char>(file)), {}) == bytes, "An unflushed write did not replace the snapshot");
  std::cout << "Snapshot write wakeups: flushed " << flushed << ", unflushed " << unflushed << "; a minute of the loop's writes " << 60 * flushed << " before, " << changes * flushed + refreshes * unflushed << " after\n";
}

// The provider marker read (native_display_status.hpp nativeDisplayConfig): its
// full trusted read (owner/ACL check, read, parse) ran twice on every service
// loop pass. It runs again only when the file changed: content, ACL, replaced.
void trustedFileCache() {
  TempFolder folder; const auto path = folder.path / L"native-display.json";
  const auto save = [&](const std::string& text) { std::ofstream(path, std::ios::binary | std::ios::trunc) << text; };
  save(R"({"provider":"a"})");
  TrustedFileCache<std::string> cache; int reads = 0;
  const auto read = [&] { ++reads; std::ifstream file(path, std::ios::binary); return std::string((std::istreambuf_iterator<char>(file)), {}); };
  for (int pass = 0; pass < 60; ++pass) require(cache.read(path, read) == R"({"provider":"a"})", "The cached marker changed");
  std::cout << "Provider marker trusted reads in 60 passes: " << reads << " (was 120)\n";
  require(reads == 1, "An unchanged marker was read and checked again: " + std::to_string(reads));
  Sleep(50); save(R"({"provider":"b"})");
  require(cache.read(path, read) == R"({"provider":"b"})" && reads == 2, "A marker rewritten in place kept its old value");
  // Owner/ACL: the change time moves, so the trust check runs again.
  Sleep(50); PSECURITY_DESCRIPTOR descriptor = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW((L"D:P(A;;FA;;;" + ownSid() + L")(A;;FR;;;BU)").c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE, "QA ACL");
  PACL dacl = nullptr; BOOL present = FALSE, defaulted = FALSE; GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
  const DWORD changed = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr); LocalFree(descriptor);
  require(changed == ERROR_SUCCESS, "QA ACL change failed");
  require(cache.read(path, read) == R"({"provider":"b"})" && reads == 3, "An ACL change did not make the marker checked again");
  // Host setup replaces it atomically: a new file.
  const auto temporary = path.wstring() + L".new"; std::ofstream(temporary, std::ios::binary) << R"({"provider":"c"})";
  require(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE, "QA replace failed");
  require(cache.read(path, read) == R"({"provider":"c"})" && reads == 4, "A replaced marker kept its old value");
  // A refused read (untrusted, unreadable) is never kept.
  Sleep(50); save(R"({"provider":"d"})"); int refused = 0;
  for (int attempt = 0; attempt < 2; ++attempt) { bool threw = false; try { (void)cache.read(path, [&]() -> std::string { ++refused; throw std::runtime_error("native_display_configuration_untrusted"); }); } catch (const std::runtime_error&) { threw = true; } require(threw, "A refused read returned a value"); }
  require(refused == 2 && cache.read(path, read) == R"({"provider":"d"})" && reads == 5, "A refused read was kept");
  // A missing marker is not cached.
  std::filesystem::remove(path); bool missing = false; try { (void)cache.read(path, [&]() -> std::string { missing = true; throw std::runtime_error("missing"); }); } catch (...) {}
  require(missing, "A missing marker returned a cached value");
}

// The broker channel's sender with a CPU reading that moves 20 points at every
// sample (a desktop in use), on a substituted clock: ten simulated minutes in
// sendLoop's order. An early heartbeat must not read the capabilities again:
// a read costs the service 70-96 wakeups (measured).
void senderHeartbeatCapabilities() {
  using namespace std::chrono;
  const auto start = steady_clock::time_point{} + hours(1);
  auto now = start, sampledAt = now - hostResourceSampleInterval, heartbeatAt = now;
  ResourceReportPolicy policy; std::optional<ResourceReading> latest; HostHeartbeatCapabilities reported;
  int reads = 0, heartbeats = 0, sample = 0; milliseconds oldest{0}; steady_clock::time_point readAt{};
  while (now < start + minutes(10)) {
    if (hostResourceSampleDue(now, sampledAt)) { sampledAt = now; ResourceReading reading; reading.cpuCount = 4; reading.memoryTotal = 100; reading.memoryUsed = 50; reading.cpuPercent = double(sample++ * 20 % 100); latest = reading; }
    const bool early = latest && now - heartbeatAt >= ResourceReportPolicy::kMinInterval && policy.EarlyDue(*latest, now), due = hostHeartbeatDue(now, heartbeatAt);
    if (early || due) {
      (void)hostHeartbeatCapabilities(reported, now, [&] { ++reads; readAt = now; return std::string("{\"service\":true}"); });
      oldest = std::max(oldest, duration_cast<milliseconds>(now - readAt)); ++heartbeats;
      if (latest) policy.Sent(*latest, now); heartbeatAt = now;
    }
    now += hostSenderIdleWait(now, sampledAt, heartbeatAt) + milliseconds(15);
  }
  std::cout << "Sender capability reads per minute, load moving: " << reads / 10.0 << " (one per heartbeat: " << heartbeats / 10.0 << "); oldest report a heartbeat repeated " << oldest.count() << " ms\n";
  require(heartbeats >= 120, "Early resource heartbeats stopped: " + std::to_string(heartbeats));
  require(reads <= 70, "Early heartbeats read the capabilities again: " + std::to_string(reads) + " reads for " + std::to_string(heartbeats) + " heartbeats");
  require(oldest < hostHeartbeatInterval, "A heartbeat repeated a report older than a heartbeat interval: " + std::to_string(oldest.count()) + " ms");
}

// The sender with nothing to send, on a substituted clock: ten simulated minutes,
// each wake 0-31 ms late as Windows timers are (15.6 ms ticks).
void senderIdleWait() {
  using namespace std::chrono;
  const auto start = steady_clock::time_point{} + hours(1);
  auto now = start, sampledAt = now - hostResourceSampleInterval, heartbeatAt = now;
  int wakes = 0, samples = 0, beats = 0; milliseconds widestSample{0}, shortestBeat{hours(1)}, longestBeat{0}; uint32_t jitter = 12345;
  while (now < start + minutes(10)) {
    ++wakes;
    // The sendLoop's order: wait, then a due sample, then a due heartbeat.
    if (hostResourceSampleDue(now, sampledAt)) { if (samples) widestSample = std::max(widestSample, duration_cast<milliseconds>(now - sampledAt)); ++samples; sampledAt = now; }
    if (hostHeartbeatDue(now, heartbeatAt)) { const auto gap = duration_cast<milliseconds>(now - heartbeatAt); shortestBeat = std::min(shortestBeat, gap); longestBeat = std::max(longestBeat, gap); ++beats; heartbeatAt = now; }
    const auto wait = hostSenderIdleWait(now, sampledAt, heartbeatAt);
    require(wait > milliseconds(0), "The sender would spin with nothing due");
    jitter = jitter * 1103515245 + 12345;
    now += wait + milliseconds((jitter >> 16) % 32);
  }
  std::cout << "Sender idle wakeups per minute: " << wakes / 10.0 << " (fixed one-second wait: 60); samples " << samples << ", heartbeats " << beats << ", heartbeat gap " << shortestBeat.count() << "-" << longestBeat.count() << " ms, widest sample gap " << widestSample.count() << " ms\n";
  require(wakes <= 310, "Idle sender woke " + std::to_string(wakes) + " times in ten minutes");
  require(samples >= 290 && widestSample <= milliseconds(2050), "Resource samples slowed down");
  require(beats >= 59 && shortestBeat >= milliseconds(9750) && longestBeat <= milliseconds(10050), "Heartbeats moved");
  require(hostSenderIdleWait(start, start - seconds(5), start) == milliseconds(0), "An overdue sample waited");
}
}

// signal_cache.hpp on a substituted clock: a kept value is returned until its
// maximum age, a key change or invalidate(); a read that raced an invalidate(),
// threw or must not be kept is never kept.
void signalCache() {
  using namespace std::chrono;
  const auto start = steady_clock::time_point{} + hours(1);
  SignalCache<int, int> cache(seconds(30)); int reads = 0;
  const auto read = [&] { return ++reads; };
  for (int i = 0; i < 60; ++i) require(cache.get(start + milliseconds(i * 490), 1, read) == 1, "A kept value changed");
  require(reads == 1, "A kept value was read again: " + std::to_string(reads) + " reads");
  require(cache.get(start + seconds(30), 1, read) == 2, "A value 30 s old was still kept");
  require(cache.get(start + seconds(31), 2, read) == 3, "A new key reused the old value");
  cache.invalidate(); require(cache.get(start + seconds(31), 2, read) == 4, "An invalidated value was kept");
  require(cache.get(start + seconds(20), 2, read) == 5, "A clock behind the read reused its value");
  cache.invalidate(); require(cache.get(start + seconds(40), 2, [&] { cache.invalidate(); return 100; }) == 100 && cache.get(start + seconds(40), 2, read) == 6, "A value read across an invalidation was kept");
  bool threw = false; cache.invalidate();
  try { (void)cache.get(start + seconds(41), 2, []() -> int { throw std::runtime_error("injected"); }); } catch (const std::runtime_error&) { threw = true; }
  require(threw && cache.get(start + seconds(41), 2, read) == 7, "A failed read kept a value");
  cache.invalidate();
  require(cache.get(start + seconds(42), 2, [] { return 50; }, [](int) { return false; }) == 50 && cache.get(start + seconds(42), 2, read) == 8, "A value refused by `keep` was kept");
}

// capability_inputs.hpp with substituted reads: each change signal makes the
// next report read again, the maximum ages refresh by themselves, and ten
// minutes of reports with nothing changing read the inputs 2 and 1 times a
// minute instead of once a report.
void capabilityInputs() {
  using namespace std::chrono;
  const auto start = steady_clock::time_point{} + hours(2);
  int sessionReads = 0, identityReads = 0; std::optional<MachineFileStamp> account; bool changeDuringRead = false;
  CapabilityInputAccess access;
  access.session = [&] { ++sessionReads; if (std::exchange(changeDuringRead, false)) noteHostSessionChange(); return CapabilitySessionView{1, true}; };
  access.identity = [&](DWORD console) { ++identityReads; StreamConsoleIdentity value; value.complete = true; value.consoleIdentityKnown = console == 1; return value; };
  access.account = [&] { return account; };
  CapabilityInputs inputs(access);
  const auto both = [&](seconds at, DWORD console = 1) { (void)inputs.session(start + at); (void)inputs.identity(console, start + at); };
  const auto reads = [&](int session, int identity) { return sessionReads == session && identityReads == identity; };
  for (int i = 0; i < 20; ++i) both(seconds(i / 4));
  require(reads(1, 1), "Unchanged inputs were read again: " + std::to_string(sessionReads) + " session, " + std::to_string(identityReads) + " identity reads");
  noteHostSessionChange(); both(seconds(6)); require(reads(2, 2), "A WTS session notification kept the inputs");
  inputs.invalidate(); both(seconds(7)); require(reads(3, 3), "invalidate() kept the inputs");
  both(seconds(8), 2); require(reads(3, 4), "A new console session kept its identities");
  account = MachineFileStamp{}; account->size = 42; both(seconds(9), 2); require(reads(3, 5), "A saved Windows account kept the old identities");
  account->written = 1; both(seconds(9), 2); require(reads(3, 6), "A rewritten Windows account kept the old identities");
  // Maximum ages from each read: the session view 30 s (read at 7 s), the identities 60 s (read at 9 s).
  both(seconds(36), 2); require(reads(3, 6), "Inputs were read again before their maximum age");
  both(seconds(37), 2); require(reads(4, 6), "The session view outlived 30 s");
  both(seconds(68), 2); require(reads(5, 6), "The identities were read again before 60 s");
  both(seconds(69), 2); require(reads(5, 7), "The identities outlived 60 s");
  // A session change during the read: its value is used once, never kept.
  inputs.invalidate(); changeDuringRead = true; (void)inputs.session(start + seconds(70)); (void)inputs.session(start + seconds(70));
  require(sessionReads == 7, "A session view read across a session change was kept");
  // Ten simulated minutes of reports (heartbeats every 10 s plus early ones), nothing changing.
  inputs.invalidate(); const int sessionsBefore = sessionReads, identitiesBefore = identityReads; int reports = 0;
  for (auto at = seconds(100); at < seconds(700); at += seconds(5)) { both(at); ++reports; }
  const double sessionPerMinute = (sessionReads - sessionsBefore) / 10.0, identityPerMinute = (identityReads - identitiesBefore) / 10.0;
  std::cout << "Capability report inputs read per minute, nothing changing, " << reports / 10.0 << " reports a minute: session view " << sessionPerMinute << ", console identities " << identityPerMinute << " (was " << reports / 10.0 << " each)\n";
  require(sessionPerMinute <= 2.1 && identityPerMinute <= 1.1, "Unchanged inputs were read more than their maximum ages need");
  // The real session view (WTS session list and lock state) in this process:
  // what a report paid for it before, and what a report with nothing changed pays now.
  unsigned wtsReads=0;
  CapabilityInputAccess real; real.session = [&] { ++wtsReads; CapabilitySessionView view; view.session = interactiveSession(); view.unlocked = streamConsoleUnlocked(view.session); return view; };
  real.identity = [](DWORD) { return StreamConsoleIdentity{}; }; real.account = [] { return std::optional<MachineFileStamp>{}; };
  CapabilityInputs live(real); (void)live.session();
  constexpr int calls = 20; auto before = ownSwitches();
  for (int i = 0; i < calls; ++i) { live.invalidate(); (void)live.session(); }
  const double cold = double(ownSwitches() - before) / calls;
  const auto readsBeforeKept=wtsReads;
  before = ownSwitches(); for (int i = 0; i < 60; ++i) (void)live.session();
  const auto kept = ownSwitches() - before;
  const auto keptReads=wtsReads-readsBeforeKept;
  std::cout << "Session view wakeups (WTS session list + lock state, this process): read " << cold << ", kept " << kept << " in 60 reports; WTS reads " << keptReads << '\n';
  // Measure the actual expensive callback; raw scheduler switches remain an
  // observation and may include preemption unrelated to a WTS read.
  require(keptReads == 0, "A kept session view called WTS " + std::to_string(keptReads) + " times in 60 reports");
}

// native_display_status.hpp: the lease snapshot's SCM status is kept only while
// SCM says this very process is the running Host service; its own next status
// report (or 60 s) makes it read again; any other answer is read every time.
void leaseServiceStatus() {
  using namespace std::chrono;
  const auto start = steady_clock::time_point{} + hours(3);
  LeaseServiceStatus status; int queries = 0; SERVICE_STATUS_PROCESS answer{}; answer.dwCurrentState = SERVICE_RUNNING; answer.dwProcessId = 4242;
  const auto query = [&] { ++queries; return answer; };
  for (int i = 0; i < 30; ++i) require(status.read(start + seconds(i), 4242, query).dwProcessId == 4242, "Own running status changed");
  require(queries == 1, "Own running status was queried again: " + std::to_string(queries));
  noteHostServiceStatusReported(); (void)status.read(start + seconds(31), 4242, query); require(queries == 2, "A status report kept the old SCM answer");
  noteHostServiceStatusReported(); answer.dwCurrentState = SERVICE_STOP_PENDING;
  for (int i = 0; i < 5; ++i) require(status.read(start + seconds(32), 4242, query).dwCurrentState == SERVICE_STOP_PENDING, "A stop pending was not seen");
  require(queries == 7, "A stop pending answer was kept");
  answer.dwCurrentState = SERVICE_RUNNING; answer.dwProcessId = 77;
  for (int i = 0; i < 5; ++i) (void)status.read(start + seconds(33), 4242, query);
  require(queries == 12, "Another process's running answer was kept");
  answer.dwProcessId = 4242; (void)status.read(start + seconds(40), 4242, query); (void)status.read(start + seconds(99), 4242, query);
  require(queries == 13, "Own running status was read again before 60 s");
  (void)status.read(start + seconds(100), 4242, query); require(queries == 14, "Own running status outlived 60 s");
  // The real SCM status read (another service standing in for this process).
  unsigned scmQueries=0;
  const auto real = [&] {
    ++scmQueries;
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)); ServiceHandle service(OpenServiceW(manager.value, L"EventLog", SERVICE_QUERY_STATUS));
    SERVICE_STATUS_PROCESS value{}; DWORD bytes = 0; require(QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&value), sizeof(value), &bytes) != FALSE, "QA SCM read"); return value; };
  const DWORD stand = real().dwProcessId; LeaseServiceStatus measured; constexpr int calls = 20;
  auto before = ownSwitches(); for (int i = 0; i < calls; ++i) { noteHostServiceStatusReported(); (void)measured.read(steady_clock::now(), stand, real); }
  const double cold = double(ownSwitches() - before) / calls;
  const auto queriesBeforeKept=scmQueries;
  before = ownSwitches(); for (int i = 0; i < 60; ++i) (void)measured.read(steady_clock::now(), stand, real);
  const auto kept = ownSwitches() - before;
  const auto keptQueries=scmQueries-queriesBeforeKept;
  std::cout << "Lease SCM status wakeups (open, open, query, close, close): read " << cold << ", kept " << kept << " in 60 reads; SCM queries " << keptQueries << '\n';
  // Scheduler preemption is not evidence of an SCM request. Keep that raw
  // observation, while directly checking that the same cached loop made none.
  require(keptQueries == 0, "A kept SCM status called the SCM " + std::to_string(keptQueries) + " times in 60 reads");
}

// display_cache.hpp on a substituted clock: thirty simulated minutes of capability
// reports (a heartbeat every 10 s) with a ready display. Each probe the reports
// start is a worker process in the user's session; each changed result is an
// announcement (one more report and broker message). Then a display change at a
// random moment: when the next probe starts.
void displayProbeCadence() {
  using namespace std::chrono;
  const auto start = steady_clock::time_point{} + hours(4);
  DisplayState ready; ready.installed = ready.deviceReady = ready.active = ready.owned = true; ready.foreign = 0; ready.width = 1920; ready.height = 1080; ready.name = L"\\\\.\\DISPLAY2"; ready.monitors.push_back({"0", ready.name, 0, 0, 1920, 1080, true});
  require(ready.ready(), "QA display not ready");
  DisplayReadinessCache cache; DisplayProbeKey key{1, 1}; int probes = 0, announcements = 0;
  const auto report = [&](steady_clock::time_point now) {
    if (!cache.due(3, key, now)) return false;
    ++probes; bool changed = false; require(cache.complete(cache.begin(3, key), ready, now, true, false, &changed), "Probe result refused"); announcements += changed; return true; };
  for (auto now = start; now < start + minutes(30); now += seconds(10)) (void)report(now);
  // The rule it replaced: due every 30 s, every result announced.
  int was = 0; for (auto now = start, probedAt = steady_clock::time_point{}; now < start + minutes(30); now += seconds(10)) if (probedAt == steady_clock::time_point{} || now - probedAt >= seconds(30)) { ++was; probedAt = now; }
  std::cout << "Display probes (worker processes) per minute, ready and nothing changing: " << probes / 30.0 << " (was " << was / 30.0 << "); announcements in 30 minutes " << announcements << " (was " << was << ")\n";
  require(probes <= 31 && announcements == 1, "An unchanged ready display was probed " + std::to_string(probes) + " times and announced " + std::to_string(announcements) + " times in 30 minutes");
  // A display change 0-9.9 s after a report: the next report probes (it was up to 30 s).
  int64_t slowest = 0; uint32_t jitter = 4242;
  for (int round = 0; round < 50; ++round) {
    auto now = start + minutes(40 + round);
    cache.complete(cache.begin(3, key), ready, now);
    jitter = jitter * 1103515245 + 12345; const auto changedAt = now + milliseconds((jitter >> 16) % 10000);
    now += seconds(10); key.displays++;
    require(report(now), "A display change did not make the next report probe");
    slowest = std::max<int64_t>(slowest, duration_cast<milliseconds>(now - changedAt).count());
  }
  std::cout << "Display change to the next probe, slowest of 50: " << slowest << " ms (a report every 10 s; the 30 s probe saw an unsignalled change up to 30000 ms later)\n";
  require(slowest <= 10000, "A display change waited for more than one report");
}

// display_change_signals.hpp with a QA key under HKCU standing in for the
// SYSTEM-only GraphicsDrivers keys: idle it does not wake; each change (values
// and subkeys, again and again) counts the display change epoch at once.
void displayChangeSignals() {
  const auto path = L"Software\\Multitap-QA\\DisplaySignals." + unique();
  HKEY created = nullptr; require(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &created, nullptr) == ERROR_SUCCESS, "QA key");
  struct Remove { std::wstring path; HKEY key; ~Remove() { RegCloseKey(key); RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); } } remove{path, created};
  const auto baseline = wake::currentThreads();
  auto signals = std::make_unique<DisplayChangeSignals>(std::vector<RegistryWatchKey>{{HKEY_CURRENT_USER, path}});
  require(signals->registryWatches() == 1 && signals->deviceWatches() == 2, "The display change watches were not registered");
  Sleep(300);
  const auto idle = idleWakeups(baseline, window);
  std::cout << "DisplayChangeSignals idle wakeups per minute: " << idle * 60000 / window << '\n';
  require(idle <= 2, "Idle display change watches woke " + std::to_string(idle) + " times in 3 s");
  const auto counted = [](uint64_t before) { return untilTrue([&] { return hostDisplayChangeEpoch().load() != before; }, 3000); };
  for (int change = 0; change < 3; ++change) {
    const auto before = hostDisplayChangeEpoch().load(); const DWORD value = change;
    require(RegSetValueExW(created, L"Topology", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS, "QA write");
    const auto seen = counted(before);
    require(seen < 500, "A saved display configuration was not counted at once (change " + std::to_string(change) + ")");
  }
  HKEY child = nullptr; const auto before = hostDisplayChangeEpoch().load();
  require(RegCreateKeyExW(created, L"00\\01", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &child, nullptr) == ERROR_SUCCESS, "QA subkey"); RegCloseKey(child);
  require(counted(before) < 500, "A new configuration subkey was not counted");
  require(elapsedMs([&] { signals.reset(); }) < 500, "Display change watches did not stop at once");
  // What the service itself can open (it runs as SYSTEM; this test does not).
  DisplayChangeSignals real;
  std::cout << "GraphicsDrivers keys this process can watch: " << real.registryWatches() << " of 2 (the service: SYSTEM); device interface watches " << real.deviceWatches() << '\n';
}

// print_redirection.hpp PrintAvailability on a substituted clock: an hour of
// capability reports (every 10 s) with printing offered, the spooler process
// stood in for by an event. Each check is an SCM query and two spooler
// enumerations. Then the spooler ends: the next report checks again.
void printCheckCadence() {
  int queried = 0; DWORD pid = 1234; WinHandle spooler(CreateEventW(nullptr, TRUE, FALSE, nullptr)); require(spooler.value != nullptr, "QA event");
  PrintCheckAccess access;
  access.spooler = [&] { ++queried; return pid; };
  access.watch = [&](DWORD) { HANDLE copy = nullptr; DuplicateHandle(GetCurrentProcess(), spooler.value, GetCurrentProcess(), &copy, SYNCHRONIZE, FALSE, 0); return copy; };
  access.driver = access.monitor = [] { return true; };
  PrintAvailability availability; uint64_t now = 1000000;
  for (int report = 0; report < 360; ++report, now += 10000) require(!*availability.reason(now, access, false), "Printing was not offered");
  std::cout << "Print checks per hour, printing offered and the spooler running: " << queried << " (was 60)\n";
  require(queried <= 7, "An unchanged print state was checked " + std::to_string(queried) + " times in an hour");
  // The spooler stops 10 s after a check: the next report checks again and finds it gone.
  (void)availability.reason(now, access, false); now += 10000;
  SetEvent(spooler.value); pid = 0; const int before = queried;
  require(std::string(availability.reason(now, access, false)) == "print_spooler_unavailable" && queried == before + 1, "A spooler that ended was not seen at the next report");
  // Not offered: the minute check stays (the spooler may start).
  for (int report = 0; report < 6; ++report) { now += 10000; (void)availability.reason(now, access, false); }
  require(queried == before + 2, "A stopped spooler was not checked again each minute");
  // A spooler that cannot be watched keeps the minute check too.
  pid = 99; access.watch = [](DWORD) -> HANDLE { return nullptr; }; now += 60000; (void)availability.reason(now, access, false); const int unwatched = queried;
  now += 60000; require(!*availability.reason(now, access, false) && queried == unwatched + 1, "An unwatched spooler was kept for more than a minute");
  require(!*availability.reason(now + 1, access, true) && queried == unwatched + 2, "Host settings' recheck answered from the cache");
}

// The other inputs of an idle capability report, as this process pays for them
// (wakeups of its own thread per call): the drive check (every 30 s), the print
// check (each one an SCM query and two spooler enumerations) and the settings
// file's trust check (each report).
void reportInputCosts() {
  const auto cost = [](auto&& call) { constexpr int calls = 20; const auto before = ownSwitches(); for (int i = 0; i < calls; ++i) call(); return double(ownSwitches() - before) / calls; };
  const double winfsp = cost([] { (void)drive::fs::winfspInstalled(); }), webClient = cost([] { (void)drive::webClientUsable(); });
  std::cout << "Drive check wakeups per call: WinFsp registered " << winfsp << " (installed: " << drive::fs::winfspInstalled() << "), WebClient state " << webClient << " (asked only without WinFsp)\n";
  const double print = cost([] { const DWORD pid = printSpoolerProcessId(); (void)(pid && printDriverInstalled() && printMonitorInstalled()); });
  const DWORD spooler = printSpoolerProcessId(); const WinHandle watched(spooler ? OpenProcess(SYNCHRONIZE, FALSE, spooler) : nullptr);
  std::cout << "Print check wakeups per call: " << print << "; spooler running " << (spooler != 0) << ", its process can be watched from this process " << (watched.value != nullptr) << '\n';
  TempFolder folder; const auto path = folder.path / L"settings.json"; std::ofstream(path, std::ios::binary) << R"({"origin":"https://qa.invalid"})";
  const double trust = cost([&] { (void)trustedMachineFile(path); std::ifstream file(path, std::ios::binary); (void)JsonParser::parse(std::string((std::istreambuf_iterator<char>(file)), {})); });
  const double stamp = cost([&] { (void)machineFileStamp(path); });
  std::cout << "Settings file per report: trusted read " << trust << " wakeups, file stamp " << stamp << '\n';
}

int main() {
  int failures = 0;
  for (const auto& [name, part] : std::initializer_list<std::pair<const char*, void (*)()>>{{"display probe cadence", displayProbeCadence}, {"display change signals", displayChangeSignals}, {"print check cadence", printCheckCadence}, {"report input costs", reportInputCosts}, {"sender", senderIdleWait}, {"sender heartbeat capabilities", senderHeartbeatCapabilities}, {"signal cache", signalCache}, {"capability inputs", capabilityInputs}, {"lease service status", leaseServiceStatus}, {"native display snapshot", nativeSnapshotWrites}, {"provider marker", trustedFileCache}, {"broker status", brokerStatusServer}, {"native reset", nativeResetServer}, {"pending connection", pendingConnectionWorker}}) {
    try { part(); } catch (const std::exception& error) { ++failures; std::cout << "FAIL " << name << ": " << error.what() << '\n'; }
  }
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
