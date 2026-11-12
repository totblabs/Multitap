#pragma once
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include "service_worker_drain_policy.hpp"

namespace host::service_workers {
enum class Role { userWorker, autologonRecovery, driveMount, driveFileSystem, displayHelper, signinRestoration, driveUnmount, secureDesktopHelper, systemInput, imeState };
class Registry;
class Creation;
class CleanupPermit;
class DriveCleanupPermit;
class ProcessObserver;
// The only injection is the duplicate operation, used by the isolated fixture.
// Production uses DuplicateHandle on the exact handle returned by CreateProcess.
using Duplicate = BOOL (WINAPI*)(HANDLE,HANDLE,HANDLE,LPHANDLE,DWORD,BOOL,DWORD);
struct Entry {
  explicit Entry(Role value) noexcept : role(value) {}
  ~Entry() { if (process) CloseHandle(process); if (job) CloseHandle(job); }
  Entry(const Entry&) = delete;
  Entry& operator=(const Entry&) = delete;
  Role role;
  std::atomic<HANDLE> process{nullptr};
  HANDLE job = nullptr;
  std::atomic<DWORD> pid{0};
  std::atomic<uint64_t> birth{0};
  std::atomic<bool> identityKnown{false};
  bool adoptedAfterDuplicateFailure = false, creationSettled = false;
  bool containmentVerified = false;
  bool cleanupAdmission = false;
};
// Copies share the preallocated entry, not another kernel duplicate. The raw
// handle is borrowed: callers never close it. A token survives registry-row
// retirement, so a native owner can still observe a fallback-adopted child.
class ProcessObserver {
 public:
  struct Identity {DWORD pid=0;uint64_t birth=0;bool known=false;};
  ProcessObserver() noexcept = default;
  HANDLE get() const noexcept { return entry_ ? entry_->process.load() : nullptr; }
  Identity identity() const noexcept {
    if(!entry_||!entry_->identityKnown.load())return {};
    return {entry_->pid.load(),entry_->birth.load(),true};
  }
 private:
  friend class Creation;
  explicit ProcessObserver(std::shared_ptr<Entry> entry) noexcept : entry_(std::move(entry)) {}
  std::shared_ptr<Entry> entry_;
};
class Registry : public std::enable_shared_from_this<Registry> {
 public:
  explicit Registry(Duplicate duplicate = DuplicateHandle) noexcept : duplicate_(duplicate) {}
  ~Registry() { drain([](const DrainFacts&){}); }
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;
  Creation reserve(Role role);
  CleanupPermit admitCleanupOwner();
  DriveCleanupPermit admitDriveCleanupOwner();
  // Nonblocking on the SCM handler: pre-admitted calls remain counted until
  // they either register the returned handle or finish without a child.
  void closeAdmission() noexcept { closed_.store(true, std::memory_order_seq_cst); }
  bool closed() const noexcept { return closed_.load(std::memory_order_seq_cst); }
  DrainFacts poll() {
    std::lock_guard lock(mutex_);
    return pollLocked();
  }
  template<class Pending> void drain(Pending&& pending) noexcept {
    closeAdmission();
    for (;;) {
      DrainFacts facts;
      try { facts = poll(); }
      catch (...) { facts.admissionClosed = true; facts.unknownWaits = 1; }
      if (mayReportStopped(facts)) return;
      // Notification errors do not free handles or authorize SERVICE_STOPPED.
      try { pending(facts); } catch (...) {}
      Sleep(100);
    }
  }
 private:
  friend class Creation;
  friend class CleanupPermit;
  friend class DriveCleanupPermit;
  Creation reserveImpl(Role role, const CleanupPermit* permit, const DriveCleanupPermit* drivePermit=nullptr);
  DrainFacts pollLocked() noexcept {
    std::size_t unknown = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
      const auto& entry = *it;
      bool rootExited = !entry->process;
      if (entry->process) {
        const DWORD wait = WaitForSingleObject(entry->process, 0);
        const auto observation = wait == WAIT_OBJECT_0 ? ProcessObservation::exited :
            wait == WAIT_TIMEOUT ? ProcessObservation::live : ProcessObservation::unknown;
        rootExited = physicalExit(observation);
        if (observation == ProcessObservation::unknown) ++unknown;
      }
      bool jobEmpty = !entry->job;
      if (entry->job) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (QueryInformationJobObject(entry->job, JobObjectBasicAccountingInformation,
              &accounting, sizeof(accounting), nullptr)) jobEmpty = accounting.ActiveProcesses == 0;
        else ++unknown;
        if (entry->process && !entry->containmentVerified) { jobEmpty = false; ++unknown; }
      }
      if (entry->creationSettled && rootExited && jobEmpty) it = entries_.erase(it);
      else ++it;
    }
    std::size_t retained = 0, jobs = 0;
    for (const auto& entry : entries_) { if (entry->process) ++retained; if (entry->job) ++jobs; }
    return {closed(), creators_, retained, unknown, jobs, cleanupOwners_};
  }
  std::atomic<bool> closed_{false};
  std::mutex mutex_;
  std::vector<std::shared_ptr<Entry>> entries_;
  std::size_t creators_ = 0;
  std::size_t cleanupOwners_ = 0;
  Duplicate duplicate_;
};
class Creation {
 public:
  Creation() noexcept = default; // explicitly outside a service lifetime
  Creation(const Creation&) = delete;
  Creation& operator=(const Creation&) = delete;
  Creation(Creation&& other) noexcept
      : registry_(std::move(other.registry_)), entry_(std::move(other.entry_)) {}
  Creation& operator=(Creation&&) = delete;
  ~Creation() { finish(); }
  ProcessObserver observer() const noexcept { return ProcessObserver(entry_); }
  bool active() const noexcept { return registry_ != nullptr; }
  void requireLaunchAllowed() const {
    if (registry_ && registry_->closed() && !entry_->cleanupAdmission)
      throw std::runtime_error("service_worker_creation_closed");
  }
  // For a dedicated non-killing tracking job, BEFORE creating its child.
  // Retaining a KILL_ON_JOB_CLOSE job changes cancellation, so reject it rather
  // than quietly claiming the existing last-close behavior is preserved.
  // No BREAKAWAY or SILENT_BREAKAWAY is admitted. The caller must assign the
  // suspended root before resume and not grant breakaway to descendants.
  void retainNonKillingJob(HANDLE job) {
    if (!registry_) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!job || job == INVALID_HANDLE_VALUE ||
        !QueryInformationJobObject(job, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits), nullptr) ||
        (limits.BasicLimitInformation.LimitFlags & (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)))
      throw std::runtime_error("service_worker_job_tracking_unsupported");
    std::lock_guard lock(registry_->mutex_);
    if (entry_->job || entry_->process) throw std::runtime_error("service_worker_job_registration_order");
    HANDLE duplicate = nullptr;
    if (!registry_->duplicate_(GetCurrentProcess(), job, GetCurrentProcess(), &duplicate,
          JOB_OBJECT_QUERY, FALSE, 0)) throw std::runtime_error("service_worker_job_duplicate_failed");
    entry_->job = duplicate;
  }
  HANDLE createTrackingJob() {
    if (!registry_) return nullptr;
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) throw std::runtime_error("service_worker_tracking_job_create_failed");
    try { retainNonKillingJob(job); }
    catch (...) { CloseHandle(job); throw; }
    return job; // caller owns original; registry holds only a query duplicate
  }
  // Call before returning from a successful CreateProcess path, before any
  // ResumeThread, and before fallible identity/protocol work. Slots/allocations
  // already exist. If duplication fails, ownership of the ORIGINAL is moved to
  // the slot before throwing, so an exception cannot lose the kernel object.
  // Job-using production callers use JobListStartup at CreateProcess so the
  // kernel assigns outer tracking + existing inner cancellation before success.
  void retain(HANDLE& original) {
    if (!registry_) return;
    if (!original || original == INVALID_HANDLE_VALUE)
      throw std::runtime_error("service_worker_process_missing");
    std::lock_guard lock(registry_->mutex_);
    if (entry_->process) throw std::runtime_error("service_worker_registered_twice");
    HANDLE duplicate = nullptr;
    const bool copied = registry_->duplicate_(GetCurrentProcess(), original,
        GetCurrentProcess(), &duplicate, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, 0) != FALSE;
    entry_->process = copied ? duplicate : std::exchange(original, nullptr);
    entry_->adoptedAfterDuplicateFailure = !copied;
    entry_->pid = GetProcessId(entry_->process);
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(entry_->process, &created, &exited, &kernel, &user))
      entry_->birth = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    entry_->identityKnown = entry_->pid != 0 && entry_->birth != 0;
    BOOL member = FALSE;
    entry_->containmentVerified = !entry_->job ||
        (IsProcessInJob(entry_->process, entry_->job, &member) && member);
    // Handle identity remains exact even if optional metadata is unavailable.
    if (!copied) throw std::runtime_error("service_worker_duplicate_failed_owned");
    if (!entry_->containmentVerified) throw std::runtime_error("service_worker_job_membership_unverified_owned");
  }
 private:
  friend class Registry;
  Creation(std::shared_ptr<Registry> registry, std::shared_ptr<Entry> entry) noexcept
      : registry_(std::move(registry)), entry_(std::move(entry)) {}
  void finish() noexcept {
    if (!registry_) return;
    {
      std::lock_guard lock(registry_->mutex_);
      --registry_->creators_;
      entry_->creationSettled = true;
      if (!entry_->process && !entry_->job) {
        for (auto it = registry_->entries_.begin(); it != registry_->entries_.end(); ++it)
          if (it->get() == entry_.get()) { registry_->entries_.erase(it); break; }
      }
    }
    entry_.reset(); registry_.reset();
  }
  std::shared_ptr<Registry> registry_;
  std::shared_ptr<Entry> entry_;
};
// A permit is issued before normal admission closes and belongs to one already
// existing sign-in cleanup owner. It cannot be copied, fabricated from a flag,
// issued late, or used to reserve any role except the fixed restoration child.
class CleanupPermit {
 public:
  CleanupPermit() noexcept = default;
  CleanupPermit(const CleanupPermit&) = delete;
  CleanupPermit& operator=(const CleanupPermit&) = delete;
  CleanupPermit(CleanupPermit&& other) noexcept : registry_(std::move(other.registry_)) {}
  CleanupPermit& operator=(CleanupPermit&&) = delete;
  ~CleanupPermit() {
    if (!registry_) return;
    { std::lock_guard lock(registry_->mutex_); --registry_->cleanupOwners_; }
    registry_.reset();
  }
  Creation reserveRestoration() const {
    if (!registry_) throw std::runtime_error("service_cleanup_owner_missing");
    return registry_->reserveImpl(Role::signinRestoration, this);
  }
 private:
  friend class Registry;
  explicit CleanupPermit(std::shared_ptr<Registry> registry) noexcept : registry_(std::move(registry)) {}
  std::shared_ptr<Registry> registry_;
};
// Separate typed capability: it cannot reserve a sign-in restorer or any
// map/open/sweep worker. Only its pre-admitted owner may reserve unmap cleanup.
class DriveCleanupPermit {
 public:
  DriveCleanupPermit(const DriveCleanupPermit&)=delete;
  DriveCleanupPermit& operator=(const DriveCleanupPermit&)=delete;
  DriveCleanupPermit(DriveCleanupPermit&& other)noexcept:registry_(std::move(other.registry_)){}
  DriveCleanupPermit& operator=(DriveCleanupPermit&&)=delete;
  ~DriveCleanupPermit(){if(registry_){{std::lock_guard lock(registry_->mutex_);--registry_->cleanupOwners_;}registry_.reset();}}
  Creation reserveUnmount()const{if(!registry_)throw std::runtime_error("service_drive_cleanup_owner_missing");return registry_->reserveImpl(Role::driveUnmount,nullptr,this);}
 private:
  friend class Registry;
  explicit DriveCleanupPermit(std::shared_ptr<Registry> registry)noexcept:registry_(std::move(registry)){}
  std::shared_ptr<Registry> registry_;
};
inline DriveCleanupPermit Registry::admitDriveCleanupOwner(){
  auto self=shared_from_this();std::lock_guard lock(mutex_);
  if(closed())throw std::runtime_error("service_drive_cleanup_admission_closed");
  ++cleanupOwners_;return DriveCleanupPermit(std::move(self));
}
inline CleanupPermit Registry::admitCleanupOwner() {
  auto self = shared_from_this();
  std::lock_guard lock(mutex_);
  if (closed()) throw std::runtime_error("service_cleanup_admission_closed");
  ++cleanupOwners_;
  return CleanupPermit(std::move(self));
}
inline Creation Registry::reserveImpl(Role role, const CleanupPermit* permit, const DriveCleanupPermit* drivePermit) {
  auto self = shared_from_this();
  auto entry = std::make_shared<Entry>(role); // before child creation
  std::lock_guard lock(mutex_);
  const bool cleanup = (permit && permit->registry_.get() == this && role == Role::signinRestoration)||
      (drivePermit && drivePermit->registry_.get()==this && role==Role::driveUnmount);
  if (closed() && !cleanup) throw std::runtime_error("service_worker_creation_closed");
  entry->cleanupAdmission = cleanup;
  (void)pollLocked();
  entries_.push_back(entry); // allocation before child creation
  ++creators_;
  return Creation(std::move(self), std::move(entry));
}
inline Creation Registry::reserve(Role role) { return reserveImpl(role, nullptr); }
inline std::mutex currentMutex;
inline std::shared_ptr<Registry> currentRegistry;
inline std::shared_ptr<Registry> current() {
  std::lock_guard lock(currentMutex); return currentRegistry;
}
inline Creation reserveCurrent(Role role, bool required = false) {
  if (auto registry = current()) return registry->reserve(role);
  if (required) throw std::runtime_error("service_worker_registry_missing");
  return Creation{};
}
inline CleanupPermit admitCurrentCleanupOwner() {
  if (auto registry = current()) return registry->admitCleanupOwner();
  throw std::runtime_error("service_worker_registry_missing");
}
inline std::optional<DriveCleanupPermit> admitCurrentDriveCleanupOwner(){
  if(auto registry=current())return registry->admitDriveCleanupOwner();
  return std::nullopt; // no service context, no fabricated permit
}
inline void closeCurrentAdmission() noexcept {
  if (auto registry = current()) registry->closeAdmission();
}
// Use for the autologon launcher, which was not suspended. The Windows 10/11
// job-list attribute assigns the job before the first instruction, eliminating
// the create-then-assign descendant escape window. An unsupported assignment
// makes CreateProcess fail; it never falls back to an untracked launch.
class JobListStartup {
 public:
  explicit JobListStartup(HANDLE job, HANDLE inner = nullptr) : jobs_{job, inner} {
    startup.StartupInfo.cb = sizeof(startup);
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    storage_.resize(bytes);
    startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if (!jobs_[0] || !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes))
      throw std::runtime_error("service_worker_job_attribute_create_failed");
    initialized_ = true;
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
          jobs_, inner ? sizeof(jobs_) : sizeof(jobs_[0]), nullptr, nullptr)) {
      DeleteProcThreadAttributeList(startup.lpAttributeList); initialized_ = false;
      throw std::runtime_error("service_worker_job_attribute_set_failed");
    }
  }
  ~JobListStartup() { if (initialized_) DeleteProcThreadAttributeList(startup.lpAttributeList); }
  JobListStartup(const JobListStartup&) = delete;
  JobListStartup& operator=(const JobListStartup&) = delete;
  STARTUPINFOEXW startup{};
 private:
  HANDLE jobs_[2];
  std::vector<BYTE> storage_;
  bool initialized_ = false;
};
class ServiceLifetime {
 public:
  ServiceLifetime() : registry_(std::make_shared<Registry>()) {
    std::lock_guard lock(currentMutex);
    if (currentRegistry) throw std::runtime_error("service_worker_registry_already_active");
    currentRegistry = registry_;
  }
  ServiceLifetime(const ServiceLifetime&) = delete;
  ServiceLifetime& operator=(const ServiceLifetime&) = delete;
  ~ServiceLifetime() {
    registry_->drain([](const DrainFacts&){}); // never release live handles
    std::lock_guard lock(currentMutex);
    if (currentRegistry == registry_) currentRegistry.reset();
  }
  void closeAdmission() noexcept { registry_->closeAdmission(); }
  template<class Pending> void drain(Pending&& pending) noexcept {
    registry_->drain(std::forward<Pending>(pending));
  }
 private:
  std::shared_ptr<Registry> registry_;
};
// Construct before any service launcher; a thread-creation failure is then a
// startup failure, not a reason to report STOPPED while a shutdown join blocks.
// Every callback must use the service's serialized status-report function.
class StopPendingHeartbeat {
 public:
  explicit StopPendingHeartbeat(std::function<void()> report)
      : report_(std::move(report)), thread_([this] {
          uint64_t last = 0;
          while (!ending_.load()) {
            const auto registry = current();
            const auto now = GetTickCount64();
            if (registry && registry->closed() && now - last >= 1000) {
              try { report_(); } catch (...) {}
              last = now;
            }
            Sleep(100);
          }
        }) {}
  StopPendingHeartbeat(const StopPendingHeartbeat&) = delete;
  StopPendingHeartbeat& operator=(const StopPendingHeartbeat&) = delete;
  ~StopPendingHeartbeat() { stop(); }
  void stop() noexcept { ending_.store(true); if (thread_.joinable()) thread_.join(); }
 private:
  std::function<void()> report_;
  std::atomic<bool> ending_{false};
  std::thread thread_;
};
} // namespace host::service_workers
