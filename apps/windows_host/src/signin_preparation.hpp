#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "signin_worker_native.hpp"
#include "display_v2.hpp"
#include "json_object.hpp"
#include <atomic>

namespace host::signin::preparation {
inline constexpr const char* protocol = "signin-private-preparation-v1";
enum class Purpose { privateDiagnostic, brokerStream };
enum class Operation { prepare, cleanup };
enum class Outcome { refused, failed, prepared, restorePending };
// A distinct local-operator purpose, never convertible to capture Context.
// These fields are correlation, not authority. Launch/each RPC require the
// parent's live Authority plus native service/child/desktop verification.
struct PreparationAdmission {
  Purpose purpose = Purpose::privateDiagnostic;
  Process service, operatorProcess;
  StreamId stream{};
  Lease lease;
  Nonce operation{}, installedImageSha256{};
  uint32_t consoleSession = 0, width = 0, height = 0;
  uint64_t issuedAt = 0, expiresAt = 0;
  // Empty means absent. Existing state is allowed only with independently
  // approved exact original bytes; the preparer cannot discover/adopt it.
  std::string expectedOriginalSha256;
  // Production correlation copied from the authenticated current service
  // session's one-shot broker admission. These fields are NOT authority;
  // BrokerAuthority requires its live service-owned grant callback throughout.
  // Diagnostic admissions must leave all four fields empty/zero.
  std::string serviceInstance;
  uint32_t grantRevision = 0, eligibilityRevision = 0;
  uint64_t brokerDeadline = 0;
};
inline bool brokerLaunchDeadlineFresh(const PreparationAdmission& a,uint64_t now){
  return a.purpose==Purpose::brokerStream&&a.issuedAt&&now>=a.issuedAt&&now<a.brokerDeadline;
}
struct PreparationBinding {
  Process service, worker, operatorProcess;
  Nonce operation{};
  StreamId stream{};
  uint32_t consoleSession = 0;
  bool operator==(const PreparationBinding&) const = default;
};
struct LeaseProof {
  Lease lease;
  std::vector<NativeDisplayTarget> targets;
};
struct BaselineProof { std::string bytes, sha256; };
struct Receipt {
  PreparationBinding binding;
  Outcome outcome = Outcome::refused;
  uint32_t stage = 0, error = 0;
  std::string originalSha256;
  native::OwnedTarget target;
  bool originalRetained = false, fullOriginalLayoutObserved = false;
};
// Root supplies the actual service controller/reservation and protected-store
// closures. No default-success functions, peer verified flags or arbitrary path.
// cleanup may retain the service's original restore obligation after local
// observer loss; it must never authorize a new forward preparation.
class Authority {
 public:
  virtual ~Authority() = default;
  virtual std::optional<LeaseProof> current(const PreparationAdmission&,
      const PreparationBinding&, Operation) const = 0;
  // nullopt means unavailable/denied; BaselineProof{} means verified absent.
  virtual std::optional<BaselineProof> original(const PreparationAdmission&,
      const PreparationBinding&, Operation) const = 0;
  virtual std::optional<BaselineProof> saveOriginal(const PreparationAdmission&,
      const PreparationBinding&, const std::string& canonicalBytes,
      const std::string& expectedOriginalSha256) = 0;
  virtual std::optional<LeaseProof> size(const PreparationAdmission&,
      const PreparationBinding&, uint32_t width, uint32_t height,
      Operation) = 0;
  virtual bool retain(const PreparationAdmission&, const PreparationBinding&,
      const Receipt&) = 0;
};
struct Command { std::wstring pipe; uint32_t servicePid = 0; };
std::optional<Command> parseCommand(int argc, const wchar_t* const* argv);
bool validAdmission(const PreparationAdmission&, uint64_t now);
bool sameBinding(const PreparationBinding&, const PreparationAdmission&,
    const Process& actualChild);
std::string initPacket(const PreparationAdmission&, const PreparationBinding&,
    uint64_t delegatedServiceQuery);
struct ParsedInit {
  PreparationAdmission admission;
  PreparationBinding binding;
  uint64_t serviceQuery = 0;
};
// Strict private-pipe parser only. The native entry separately verifies actual
// pipe PID, retained service identity/image, own child identity and desktop.
ParsedInit parseInitPacket(const Json&, uint64_t now);
// Parent's one-reader dispatcher handles only these bound preparation RPCs.
// It must not route arbitrary ordinary worker/control packets through here.
std::string answer(const Json&, const PreparationAdmission&,
    const PreparationBinding&, Authority&, uint64_t& lastSerial,
    uint64_t now = GetTickCount64());
std::string receiptPacket(const Receipt&);
Receipt parseReceipt(const Json&);
using Exchange = std::function<Json(const Json&)>;
// Actual V2 transaction, with injected local CCD access only in fixtures.
// This function replaces store/owned/size with the bound parent RPC proxy.
Receipt prepareDisplay(const PreparationAdmission&, const PreparationBinding&,
    DisplayV2Access localCcd, Exchange, std::function<uint64_t()> now);

class OwnedPreparationChild {
 public:
  OwnedPreparationChild() = default;
  OwnedPreparationChild(const OwnedPreparationChild&) = delete;
  OwnedPreparationChild& operator=(const OwnedPreparationChild&) = delete;
  ~OwnedPreparationChild();
  HANDLE process() const { return process_?process_.get():retainedProcess_.get(); }
  HANDLE pipe() const { return pipe_.get(); }
  const PreparationBinding& binding() const { return binding_; }
  uint64_t delegatedServiceQuery() const { return delegatedServiceQuery_; }
  bool connect(uint32_t milliseconds);
  bool stopAndWait(uint32_t milliseconds);
  native::Handle takeUnexited();
 private:
  friend native::Status launch(const PreparationAdmission&, Authority&,
      OwnedPreparationChild&);
  native::Handle pipe_, process_, job_, imageLock_;
  service_workers::ProcessObserver retainedProcess_;
  PreparationBinding binding_;
  uint64_t delegatedServiceQuery_ = 0;
  std::wstring pipeName_;
  bool jobAssigned_ = false;
};
native::Status launch(const PreparationAdmission&, Authority&,
    OwnedPreparationChild&);
// Distinct preparation-only CLI: no Encoder, input, publish, decoder or Agent.
// Actual entry is never invoked by the injected fixture.
int workerCommand(int argc, const wchar_t* const* argv) noexcept;
} // namespace host::signin::preparation
