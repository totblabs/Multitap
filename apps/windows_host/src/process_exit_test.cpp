// hostAction 275 (backlog #69): the boot service's run marker, the exit of an
// owned worker and the record's place in the diagnostics report. The marker
// lives in a private temporary folder with a substituted clock; the workers are
// this executable's own children (exit codes set with ExitProcess, no real crash,
// so no error report appears); the crash filter is called with a made-up record.
#include "host_diagnostics.hpp"
#include "process_exit_record.hpp"
#include <iostream>

using namespace host;
namespace {
int checks = 0;
void require(bool ok, const std::string& what) { ++checks; if (!ok) throw std::runtime_error(what); }
constexpr uint64_t fileSecond = 10000000ull;  // FILETIME units
uint64_t fakeUtc = 134000000000000000ull, fakeTick = 3600000;  // 1 h after a boot
ServiceRunMarker::Clock fakeClock() { return {[] { return fakeUtc; }, [] { return fakeTick; }}; }
void advance(uint64_t ms) { fakeUtc += ms * 10000; fakeTick += ms; }

ServiceRunState state(uint32_t phase, uint64_t runSeconds, std::optional<uint32_t> code = {}) {
  ServiceRunState prior; prior.magic = serviceRunMagic; prior.version = 1; prior.state = phase;
  prior.startUtc = fakeUtc - runSeconds * fileSecond; prior.aliveUtc = fakeUtc; prior.aliveTick = fakeTick; prior.bootUtc = fakeUtc - fakeTick * 10000;
  if (code) { prior.codeKnown = 1; prior.code = *code; }
  return prior;
}
void previousRunDecisions() {
  const auto now = [&](uint64_t addMs) { return std::pair<uint64_t, uint64_t>{fakeUtc + addMs * 10000, fakeTick + addMs}; };
  auto [utc, tick] = now(5000);
  require(!previousServiceRun(state(serviceRunStopped, 600), utc, tick), "A clean stop was reported");
  require(!previousServiceRun(state(serviceRunStopping, 600), utc, tick), "A requested stop was reported");
  auto crashedStopping = previousServiceRun(state(serviceRunStopping, 600, 0xC0000005u), utc, tick);
  require(crashedStopping && crashedStopping->cause == ProcessExitCause::exited && crashedStopping->code == 0xC0000005u, "A crash during a stop was lost");
  auto killed = previousServiceRun(state(serviceRunRunning, 3725), utc, tick);
  require(killed && killed->process == ExitedProcess::service && killed->cause == ProcessExitCause::noCode && !killed->code && killed->runSeconds == 3725, "A killed run was not reported with its run time");
  auto failed = previousServiceRun(state(serviceRunFailed, 12, ERROR_SERVICE_SPECIFIC_ERROR), utc, tick);
  require(failed && failed->cause == ProcessExitCause::exited && failed->code == uint32_t(ERROR_SERVICE_SPECIFIC_ERROR), "An error stop lost its code");
  auto crash = previousServiceRun(state(serviceRunRunning, 40, 0xC0000409u), utc, tick);
  require(crash && crash->cause == ProcessExitCause::exited && crash->code == 0xC0000409u && crash->runSeconds == 40, "A crash lost its NTSTATUS");
  // Windows restarted: the tick count began again, or the implied boot time moved.
  auto restarted = previousServiceRun(state(serviceRunRunning, 86400), fakeUtc + 600 * fileSecond, 90000);
  require(restarted && restarted->cause == ProcessExitCause::windowsRestarted && restarted->runSeconds == 86400, "A Windows restart was not told apart");
  auto laterBoot = previousServiceRun(state(serviceRunRunning, 60), fakeUtc + 7200 * fileSecond, fakeTick + 100000);
  require(laterBoot && laterBoot->cause == ProcessExitCause::windowsRestarted, "A later boot with a larger tick count passed as the same boot");
  auto clockStep = previousServiceRun(state(serviceRunRunning, 60), fakeUtc + 30 * fileSecond + 40 * fileSecond, fakeTick + 30000);
  require(clockStep && clockStep->cause == ProcessExitCause::noCode, "A 40 s clock correction was taken for a restart");
  auto garbage = state(serviceRunRunning, 60); garbage.magic = 0;
  require(!previousServiceRun(garbage, utc, tick), "An unknown marker was reported");
}

void markerLifecycle(const std::filesystem::path& folder) {
  const auto path = folder / L"service-run.bin";
  { ServiceRunMarker marker(path, fakeClock()); require(marker.open() && !marker.previous(), "A first run found a previous one"); advance(20000); marker.alive(); advance(20000); }
  // The process ended without a word (killed, as by an antivirus): found next time.
  advance(5000);
  { ServiceRunMarker marker(path, fakeClock()); require(marker.previous() && marker.previous()->cause == ProcessExitCause::noCode && marker.previous()->runSeconds == 20, "A killed run was not found with its last alive mark");
    advance(3000); marker.alive(); advance(14000); marker.alive(); advance(1000); marker.stopping(); }
  { ServiceRunMarker marker(path, fakeClock()); require(!marker.previous(), "A requested stop was reported"); marker.stopped(); }
  { ServiceRunMarker marker(path, fakeClock()); require(!marker.previous(), "A clean stop was reported"); advance(60000); marker.alive(); marker.failed(ERROR_SERVICE_SPECIFIC_ERROR); }
  { ServiceRunMarker marker(path, fakeClock()); require(marker.previous() && marker.previous()->code == uint32_t(ERROR_SERVICE_SPECIFIC_ERROR) && marker.previous()->runSeconds == 60, "An error stop lost its code");
    // An unhandled exception: the process filter notes the code and the time.
    serviceRunMarker = &marker; advance(7000);
    EXCEPTION_RECORD exception{}; exception.ExceptionCode = DWORD(0xC0000005); CONTEXT context{}; EXCEPTION_POINTERS pointers{&exception, &context};
    require(serviceRunCrashFilter(&pointers) == EXCEPTION_CONTINUE_SEARCH, "The crash filter handled the exception itself");
    serviceRunMarker = nullptr; }
  { ServiceRunMarker marker(path, fakeClock()); require(marker.previous() && marker.previous()->cause == ProcessExitCause::exited && marker.previous()->code == 0xC0000005u && marker.previous()->runSeconds == 7, "A crash lost its code or time"); }
  // Windows restarted meanwhile: the tick count starts again.
  fakeUtc += 300 * fileSecond; fakeTick = 40000;
  { ServiceRunMarker marker(path, fakeClock()); require(marker.previous() && marker.previous()->cause == ProcessExitCause::windowsRestarted, "A restart of Windows was not told apart"); marker.stopped(); }
  // A marker that is a second link to another file is never written.
  const auto linked = folder / L"linked.bin";
  require(CreateHardLinkW(linked.c_str(), path.c_str(), nullptr) != FALSE, "QA hard link");
  { ServiceRunMarker marker(linked, fakeClock()); require(!marker.open(), "A hard-linked marker was written"); }
}

std::wstring executable() { wchar_t path[32768]{}; const DWORD count = GetModuleFileNameW(nullptr, path, 32768); require(count && count < 32768, "QA executable path"); return {path, count}; }
struct Child {
  HANDLE process = nullptr, job = nullptr;
  explicit Child(const std::wstring& mode) {
    auto command = L"\"" + executable() + L"\" --child " + mode;
    job = CreateJobObjectW(nullptr, nullptr); require(job != nullptr, "QA job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE, "QA job limit");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
    require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE, "QA child");
    process = child.hProcess;
    if (!AssignProcessToJobObject(job, process)) { TerminateProcess(process, 1); CloseHandle(child.hThread); throw std::runtime_error("QA child job"); }
    ResumeThread(child.hThread); CloseHandle(child.hThread);
  }
  ~Child() { if (job) CloseHandle(job); if (process) { if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) { TerminateProcess(process, 1); WaitForSingleObject(process, 2000); } CloseHandle(process); } }
};
OwnedWorkerExit runChild(const std::wstring& mode, DWORD gracefulMs) {
  Child child(mode); HANDLE pipe = nullptr; bool observed = false; OwnedWorkerExit exit;
  { OwnedWorkerLifetime lifetime(pipe, child.process, child.job, &observed, gracefulMs, 5000, nullptr, [&](const OwnedWorkerExit& value) { exit = value; }); lifetime.connected(); }
  require(observed, "QA child exit not observed");
  return exit;
}
void workerExits() {
  // A session worker that crashed by itself 1.2 s after its start.
  const auto crashed = runChild(L"exit 1200 C0000005", 30000);
  require(crashed.observed && crashed.ownExit && crashed.code == 0xC0000005u && crashed.runMs >= 1100 && crashed.runMs < 10000, "A crashed worker's code or run time was lost");
  const auto crashRecord = sessionWorkerExit(crashed, false);
  require(crashRecord && crashRecord->process == ExitedProcess::sessionWorker && crashRecord->code == 0xC0000005u && crashRecord->runSeconds == crashed.runMs / 1000, "A crashed session worker was not recorded");
  require(sessionWorkerExit(crashed, true).has_value(), "A crash during a requested stop was not recorded");
  require(workerCrashExit(crashed) && workerCrashExit(crashed)->process == ExitedProcess::worker, "A crashed probe worker was not recorded");
  // A worker that returned normally after a stop: expected. Without a stop: unexpected.
  const auto returned = runChild(L"exit 200 0", 30000);
  require(returned.ownExit && returned.code == 0, "A returning worker was misread");
  require(!sessionWorkerExit(returned, true) && sessionWorkerExit(returned, false) && !workerCrashExit(returned), "A normal worker exit was judged wrongly");
  // A worker the Host ended (job closed after the grace period) is not unexpected.
  const auto ended = runChild(L"linger", 100);
  std::cout << "A worker ended by closing its job exits with code 0x" << std::hex << ended.code << std::dec << '\n';
  require(ended.observed && !ended.ownExit && !sessionWorkerExit(ended, false) && !workerCrashExit(ended), "A worker the Host ended was recorded as unexpected");
}

void recordAndReport() {
  std::vector<std::string> rows;
  const auto sink = [&](auto values) {
    std::string fields = "{"; for (const auto& [key, value] : values) { if (fields.size() > 1) fields += ','; fields += jsonQuote(key) + ':' + (hostLogBoolean(key) ? std::string(value ? "true" : "false") : std::to_string(value)); }
    rows.push_back("{\"time\":\"2026-11-03T01:22:00.000Z\",\"event\":\"hostAction\",\"values\":" + fields + "}}");
  };
  emitProcessExit({ExitedProcess::service, ProcessExitCause::noCode, std::nullopt, 86000}, sink);
  emitProcessExit({ExitedProcess::sessionWorker, ProcessExitCause::exited, 0xC0000409u, 5000000000ull}, sink);
  require(rows.size() == 2, "Records were not emitted");
  const auto first = JsonParser::parse(safeHostLogRecord(JsonParser::parse(rows[0]))), second = JsonParser::parse(safeHostLogRecord(JsonParser::parse(rows[1])));
  require(first.get("values").get("action").number() == 275 && first.get("values").get("stage").number() == 1 && first.get("values").get("cause").number() == 2 &&
          first.get("values").get("runSeconds").number() == 86000 && std::holds_alternative<std::nullptr_t>(first.get("values").get("win32").value) && !first.get("values").get("ok").boolean(), "The service record lost a fixed field");
  require(second.get("values").get("win32").number() == double(0xC0000409u) && second.get("values").get("runSeconds").number() == 4294967295.0, "The worker record lost its code or its run-time bound");
  // The newest process end outlives a day of newer ordinary events in the report.
  DisplayState display; Json native, history; auto& events = history.value.emplace<Json::Array>();
  events.push_back(JsonParser::parse(rows[0])); events.push_back(JsonParser::parse(rows[1]));
  for (unsigned i = 0; i < 190; ++i) events.push_back(JsonParser::parse("{\"time\":\"2026-11-03T02:00:00.000Z\",\"event\":\"hostAction\",\"values\":{\"action\":218,\"ok\":true,\"changed\":true,\"stage\":" + std::to_string(i % 7) + "}}"));
  const auto report = JsonParser::parse(buildHostDiagnosticReport(display, false, false, false, "", native, history));
  unsigned exits = 0; Json kept;
  for (const auto& row : std::get<Json::Array>(report.get("events").value)) if (row.get("values").get("action").number() == 275) { ++exits; kept = row; }
  require(exits == 1 && kept.get("values").get("stage").number() == 2 && kept.get("values").get("win32").number() == double(0xC0000409u), "The newest process end was pushed out of the report");
}
}

int wmain(int argc, wchar_t** argv) {
  if (argc >= 3 && std::wstring(argv[1]) == L"--child") {
    if (std::wstring(argv[2]) == L"linger") { Sleep(15000); return 0; }
    if (argc == 5 && std::wstring(argv[2]) == L"exit") { Sleep(DWORD(_wtoi(argv[3]))); ExitProcess(UINT(wcstoul(argv[4], nullptr, 16))); }
    return 2;
  }
  const auto folder = std::filesystem::temp_directory_path() / (L"MultitapHost.ExitQA." + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(folder);
  struct Remove { std::filesystem::path path; ~Remove() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } remove{folder};
  int failures = 0;
  const std::pair<const char*, std::function<void()>> parts[] = {{"previous run", previousRunDecisions}, {"marker", [&] { markerLifecycle(folder); }}, {"worker exits", workerExits}, {"record and report", recordAndReport}};
  for (const auto& [name, part] : parts) { try { part(); } catch (const std::exception& error) { ++failures; std::cout << "FAIL " << name << ": " << error.what() << '\n'; } }
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
