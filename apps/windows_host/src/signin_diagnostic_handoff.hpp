#pragma once
#include "signin_stream_state.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace host::signin::handoff {
enum class Phase { intent,handoffCommitted };
enum class ChildRole { preparer,restorer };
enum class ProcessState { unknown,alive,exited };
struct FileIdentity {
  uint64_t volume=0,created=0,bytes=0;
  std::array<uint8_t,16> fileId{};
  Nonce sha256{};
  bool operator==(const FileIdentity&)const=default;
};
struct Child {
  Process process;ChildRole role=ChildRole::preparer;
  bool operator==(const Child&)const=default;
};
struct Request {
  Process oldService;Nonce oldImageSha256{},operation{};StreamId stream{};
  std::string instance,provider="libvirtualdisplay-1.6.4";
  uint32_t generation=0;
  FileIdentity original;
  // Complete currently-owned inventory, not a peer-selected subset. Previously
  // retired/closed children may only have been pruned after their actual exit.
  std::vector<Child> children;
  bool operator==(const Request&)const=default;
};
struct Record {
  Request request;Phase phase=Phase::intent;
  std::optional<FileIdentity> intentFile;
  bool operator==(const Record&)const=default;
};
bool valid(const Request&);
std::string canonical(const Record&);
std::optional<Record> parse(const std::string&);

class Retention {public:virtual ~Retention()=default;};
struct OriginalObservation {FileIdentity identity;std::shared_ptr<const Retention> retained;};
struct OwnedInventory {
  std::vector<Child> children;
  // Actual service-owned logical no-new-child gate, stable for this operation.
  // Must NOT be a thread-owned unique_lock transferred between service threads.
  std::shared_ptr<const Retention> closedLaunchGate;
};
struct Snapshot {std::string bytes;FileIdentity identity;std::shared_ptr<const Retention> retained;};
// These are mandatory native/service closures, never parsed JSON verified
// booleans. Each receives the complete record INCLUDING its bound phase.
class Authority {
 public:
  virtual ~Authority()=default;
  virtual bool currentProducer(const Record&)=0; // exact own SCM/SYSTEM/image
  virtual std::optional<Process> currentConsumer(const Record&)=0;
  virtual std::optional<OwnedInventory> ownedChildren(const Record&)=0;
  virtual ProcessState process(const Record&,Process)=0;
  virtual std::optional<OriginalObservation> original(const Record&)=0;
};
class Journal {
 public:
  virtual ~Journal()=default;
  virtual std::optional<Snapshot> read(Phase)=0;
  virtual std::optional<Snapshot> createIntent(const std::string&)=0;
  // Create a distinct committed leaf only while the exact intent is pinned.
  // No overwrite of either fixed leaf or an unknown record is permitted.
  // finalCheck is synchronous and must run after candidate bytes are durable,
  // before committed publication. It must not reenter this Journal.
  virtual std::optional<Snapshot> commit(const Snapshot& exactIntent,const std::string&,
      const std::function<bool()>& finalCheck)=0;
  virtual bool unchanged(Phase,const Snapshot&)=0;
};
using TrustedPath=std::function<bool(const std::filesystem::path&)>;
// Fixed privateParent/signin-cleanup-handoff-v1/{intent,committed}.json only.
// Parent/path trust is supplied by the actual service, never by journal bytes.
// Unknown extra leaves, links and stale phases fail closed. The two immutable
// leaves avoid replacing an expected inode through a racy path-based rename.
class FileJournal final:public Journal {
 public:
  FileJournal(std::filesystem::path privateParent,TrustedPath);
  ~FileJournal();
  FileJournal(const FileJournal&)=delete;FileJournal& operator=(const FileJournal&)=delete;
  std::optional<Snapshot> read(Phase)override;
  std::optional<Snapshot> createIntent(const std::string&)override;
  std::optional<Snapshot> commit(const Snapshot&,const std::string&,const std::function<bool()>&)override;
  bool unchanged(Phase,const Snapshot&)override;
 private:struct Impl;std::unique_ptr<Impl> impl_;
};
// Fixed-path authority adapters may use these actual read-only primitives.
// AccessDenied/unknown metadata NEVER becomes an exited process. A reused PID
// with a different creation time proves the expected older process is gone.
ProcessState observeKernelProcess(Process)noexcept;
std::optional<OriginalObservation> inspectProtectedOriginal(const std::filesystem::path& fixedOriginal,TrustedPath);

class Intent {
 public:
  Intent()=delete;const Record& record()const{return record_;}
 private:
  friend std::optional<Intent> begin(const Request&,Authority&,Journal&);
  friend class CommitReceipt;
  friend std::optional<class CommitReceipt> commit(const Intent&,Authority&,Journal&);
  Intent(Record record,Snapshot snapshot,OriginalObservation original,std::shared_ptr<const Retention> gate)
    :record_(std::move(record)),snapshot_(std::move(snapshot)),original_(std::move(original)),gate_(std::move(gate)){}
  Record record_;Snapshot snapshot_;OriginalObservation original_;std::shared_ptr<const Retention> gate_;
};
class CommitReceipt {
 public:
  CommitReceipt()=delete;const Record& record()const{return record_;}
 private:
  friend std::optional<CommitReceipt> commit(const Intent&,Authority&,Journal&);
  CommitReceipt(Record record,Snapshot intent,Snapshot completed,OriginalObservation original,std::shared_ptr<const Retention> gate)
    :record_(std::move(record)),intent_(std::move(intent)),completed_(std::move(completed)),original_(std::move(original)),gate_(std::move(gate)){}
  Record record_;Snapshot intent_,completed_;OriginalObservation original_;std::shared_ptr<const Retention> gate_;
};
// This is a cleanup-only persisted-obligation fact, NOT an old-HANDLE authority,
// a new controller reservation, capture Context or runtimeQualified admission.
class RecoveryObligation {
 public:
  RecoveryObligation()=delete;
  const Request& request()const{return record_.request;}
  Process consumer()const{return consumer_;}
 private:
  friend std::optional<RecoveryObligation> recover(Authority&,Journal&);
  RecoveryObligation(Record record,Process consumer,Snapshot intent,Snapshot completed,OriginalObservation original)
    :record_(std::move(record)),consumer_(consumer),intent_(std::move(intent)),completed_(std::move(completed)),original_(std::move(original)){}
  Record record_;Process consumer_;Snapshot intent_,completed_;OriginalObservation original_;
};
std::optional<Intent> begin(const Request&,Authority&,Journal&);
std::optional<CommitReceipt> commit(const Intent&,Authority&,Journal&);
std::optional<RecoveryObligation> recover(Authority&,Journal&);
} // namespace host::signin::handoff
